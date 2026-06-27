#!/usr/bin/env python3
"""
podcast_to_moshi_dataset.py
────────────────────────────────────────────────────────────────
Convert Gemischtes Hack Podcast episodes into the moshi-finetune
dataset format:

  {out_dir}/ep{N}_{chunk:04d}_{main|other}.wav   48kHz stereo PCM-16
  {out_dir}/ep{N}_{chunk:04d}_{main|other}.json  {"alignments": [[word,[s,e],spk], ...]}

Audio layout (double-mono):
  SPEAKER_MAIN  → channel-0 = speech, channel-1 = zeros
  SPEAKER_OTHER → channel-0 = zeros,  channel-1 = speech

Word-level timestamps come from WhisperX forced-alignment (wav2vec2 + MPS)
applied to each segment of the already-transcribed podcast JSONs.

The waveform-alignment cache (podcast_alignment_cache.json) holds the
intro-offset + mid-roll ad-break positions for every episode, computed
previously by cross-correlation.  We consume that cache directly —
no re-correlation needed.

Memory design
─────────────
Long episodes (up to 2 h) at 48 kHz float32 take > 1.5 GB — exceeding
macOS OOM thresholds when the whisperx model is also loaded.

Solution: all alignment work runs at 16 kHz (4× smaller).  Audio is never
held at 48 kHz as a full array.  When writing output chunks we decode only
the required time-slice directly from the MP3 via ffmpeg subprocess (no
full-file re-read), then resample that small slice to 48 kHz.

Usage (all episodes):
    python3 scripts/podcast_to_moshi_dataset.py

Usage (subset):
    python3 scripts/podcast_to_moshi_dataset.py --episodes 150-159
    python3 scripts/podcast_to_moshi_dataset.py --episodes 150,151,200
"""

import argparse
import gc
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

import numpy as np
import soundfile as sf
import librosa

os.environ.setdefault("KMP_DUPLICATE_LIB_OK", "TRUE")

# ── paths ─────────────────────────────────────────────────────────────────────
RAW_DIR       = "/Volumes/eHDD/moshi-rag-data/datasets"
CACHE_PATH    = "/Volumes/eHDD/moshi-rag-data/datasets/podcast_alignment_cache.json"
TRANSCRIPT_DIR= os.path.join(RAW_DIR, "Gemischtes.Hack.Podcast", "transcripts")
AUDIO_DIR     = os.path.join(RAW_DIR, "Gemischtes.Hack.Podcast")
OUTPUT_DIR    = "/Volumes/eHDD/moshi-rag-data/processed/podcast"

# ── audio constants ────────────────────────────────────────────────────────────
TARGET_SR    = 48000   # moshi-finetune requirement
WHISPERX_SR  = 16000   # whisperx alignment native sample-rate
MIN_CHUNK_DUR = 0.5    # discard chunks shorter than this

# ── lazy-load whisperx ────────────────────────────────────────────────────────
_align_model  = None
_align_meta   = None

def get_align_model():
    global _align_model, _align_meta
    if _align_model is None:
        import whisperx, torch
        device = "mps" if torch.backends.mps.is_available() else "cpu"
        print(f"[whisperx] loading German alignment model on {device} …", flush=True)
        _align_model, _align_meta = whisperx.load_align_model(
            language_code="de", device=device
        )
        print("[whisperx] model ready", flush=True)
    return _align_model, _align_meta


# ── audio helpers ─────────────────────────────────────────────────────────────

def load_cache():
    if os.path.exists(CACHE_PATH):
        with open(CACHE_PATH, encoding="utf-8") as f:
            return json.load(f)
    return {}


def build_clean_regions(offset: float, ad_breaks: list, total_dur: float):
    """Return list of (start_sec, end_sec) content regions in the raw audio."""
    regions = []
    cursor  = max(0.0, offset)
    for ad_s, ad_e in sorted(ad_breaks):
        if cursor < ad_s:
            regions.append((cursor, ad_s))
        cursor = ad_e
    if cursor < total_dur:
        regions.append((cursor, total_dur))
    return regions


def get_audio_duration(mp3_path: str) -> float:
    """Return duration in seconds via ffprobe (no audio decode)."""
    r = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "default=noprint_wrappers=1:nokey=1", mp3_path],
        capture_output=True, text=True
    )
    return float(r.stdout.strip())


def load_audio_segment_16k(mp3_path: str, start_sec: float, end_sec: float) -> np.ndarray:
    """
    Decode a time-range of an MP3 directly to 16 kHz mono float32 via ffmpeg.
    Uses -ss/-to for fast seeking — no full-file decode.
    Returns numpy array at 16 kHz.
    """
    duration = end_sec - start_sec
    cmd = [
        "ffmpeg", "-v", "error",
        "-ss", str(start_sec),
        "-t",  str(duration),
        "-i",  mp3_path,
        "-ar", str(WHISPERX_SR),
        "-ac", "1",
        "-f",  "f32le",        # raw 32-bit float little-endian PCM
        "pipe:1",
    ]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError(f"ffmpeg failed: {r.stderr.decode()[:300]}")
    audio = np.frombuffer(r.stdout, dtype=np.float32).copy()
    return audio


def load_audio_segment_48k(mp3_path: str, start_sec: float, end_sec: float) -> np.ndarray:
    """
    Decode a time-range of an MP3 to 48 kHz mono float32 via ffmpeg.
    Used only when writing small output chunks — never the full file.
    """
    duration = end_sec - start_sec
    cmd = [
        "ffmpeg", "-v", "error",
        "-ss", str(start_sec),
        "-t",  str(duration),
        "-i",  mp3_path,
        "-ar", str(TARGET_SR),
        "-ac", "1",
        "-f",  "f32le",
        "pipe:1",
    ]
    r = subprocess.run(cmd, capture_output=True)
    if r.returncode != 0:
        raise RuntimeError(f"ffmpeg failed: {r.stderr.decode()[:300]}")
    return np.frombuffer(r.stdout, dtype=np.float32).copy()


def transcript_time_to_clean_time(t: float, ad_breaks: list) -> float:
    """
    Map content-relative time (t = raw_time − offset) to position in the
    concatenated clean audio (after ad-breaks are removed).
    """
    removed = 0.0
    for ad_s, ad_e in sorted(ad_breaks):
        if ad_e <= t:
            removed += ad_e - ad_s
        elif ad_s < t:
            removed += t - ad_s
            break
    return max(0.0, t - removed)


def clean_time_to_raw_time(clean_t: float, offset: float, ad_breaks: list) -> float:
    """
    Inverse of transcript_time_to_clean_time: given a position in the
    concatenated clean audio, return the corresponding time in the original
    raw MP3.  Used to seek to the right position when decoding output chunks.
    """
    raw_t = clean_t + offset
    for ad_s, ad_e in sorted(ad_breaks):
        ad_dur = ad_e - ad_s
        if raw_t >= ad_s:
            raw_t += ad_dur
        else:
            break
    return raw_t


# ── whisperx alignment ────────────────────────────────────────────────────────

def align_segments_whisperx(segments: list, audio_16k: np.ndarray) -> dict:
    """
    Run WhisperX forced alignment on `segments` against `audio_16k`.

    `segments` is a list of dicts: {text, start, end}  (seconds, relative to
    the start of audio_16k).

    Returns a dict mapping segment index → list of word dicts:
        {"word": str, "start": float, "end": float, "score": float}

    NOTE: whisperx may SPLIT one input segment into multiple output segments
    when backtrack fails.  We therefore use the flat `word_segments` list and
    assign each word to the input segment whose time window contains it.
    """
    import whisperx

    model_a, metadata = get_align_model()
    device = next(iter(model_a.parameters())).device.type \
        if hasattr(model_a, "parameters") else "cpu"

    result = whisperx.align(segments, model_a, metadata, audio_16k, device,
                            return_char_alignments=False)

    all_words = result.get("word_segments", [])
    word_map: dict = {i: [] for i in range(len(segments))}

    if all_words:
        for wd in all_words:
            ws = wd.get("start")
            we = wd.get("end")
            if ws is None or we is None:
                continue
            mid = (float(ws) + float(we)) / 2.0
            for idx, seg in enumerate(segments):
                if seg["start"] - 0.5 <= mid <= seg["end"] + 0.5:
                    word_map[idx].append(wd)
                    break
    else:
        out_segs = result.get("segments", [])
        for i, out_seg in enumerate(out_segs):
            if i >= len(segments):
                break
            word_map[i] = out_seg.get("words", [])

    return word_map


# ── output helpers ────────────────────────────────────────────────────────────

def clean_word(text: str) -> str:
    """Strip punctuation except hyphens and German characters."""
    return re.sub(r"[^\w\däöüßÄÖÜ\s-]", "", text).strip()


def write_chunk(stereo: np.ndarray, sr: int, alignments: list,
                out_dir: str, fname: str):
    os.makedirs(out_dir, exist_ok=True)
    wav_path  = os.path.join(out_dir, f"{fname}.wav")
    json_path = os.path.join(out_dir, f"{fname}.json")
    sf.write(wav_path, stereo.T, sr, subtype="PCM_16")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump({"alignments": alignments}, f, ensure_ascii=False)


# ── core per-episode logic ─────────────────────────────────────────────────────

def process_episode(ep_num: int, transcript_path: str, mp3_path: str,
                    cache: dict, out_dir: str) -> int:
    ep_label = f"ep{ep_num}"
    print(f"\n{'─'*60}", flush=True)
    print(f"  Episode {ep_num}", flush=True)

    # ── 1. load cache entry ────────────────────────────────────────────────────
    entry     = cache.get(ep_label, {})
    offset    = float(entry.get("offset", 34.2))
    ad_breaks = [(float(a), float(b)) for a, b in entry.get("ad_breaks", [])]
    print(f"  cache: offset={offset:.1f}s, {len(ad_breaks)} ad-break(s)", flush=True)

    # ── 2. determine total duration (ffprobe — no audio decode) ───────────────
    total_dur_orig = get_audio_duration(mp3_path)
    print(f"  duration: {total_dur_orig:.1f}s ({total_dur_orig/3600:.2f}h)", flush=True)
    mem_estimate_gb = total_dur_orig * TARGET_SR * 4 / 1e9
    print(f"  (48k array would be {mem_estimate_gb:.2f} GB — loading at 16k instead)",
          flush=True)

    # ── 3. build clean regions (content only, no intro/ads) ───────────────────
    clean_regions = build_clean_regions(offset, ad_breaks, total_dur_orig)
    clean_dur = sum(e - s for s, e in clean_regions)
    print(f"  clean duration: {clean_dur:.1f}s (removed {total_dur_orig - clean_dur:.1f}s)",
          flush=True)

    # ── 4. load transcript, map speakers ──────────────────────────────────────
    with open(transcript_path, encoding="utf-8") as f:
        data = json.load(f)

    segments_raw = [s for s in data.get("segments", []) if s.get("text", "").strip()]
    if not segments_raw:
        print(f"  ✗ no segments, skipping", flush=True)
        return 0

    speaker_map: dict = {}
    for seg in segments_raw:
        spk = seg.get("speaker", "")
        if spk and spk not in speaker_map:
            speaker_map[spk] = "SPEAKER_MAIN" if len(speaker_map) == 0 else "SPEAKER_OTHER"
    print(f"  speakers: {speaker_map}", flush=True)

    # ── 5. re-time segments to clean-audio space ───────────────────────────────
    segments_clean = []
    for seg in segments_raw:
        raw_start = float(seg["start"])
        raw_end   = float(seg["end"])
        if raw_end <= offset:
            continue
        clean_start = transcript_time_to_clean_time(raw_start - offset, ad_breaks)
        clean_end   = transcript_time_to_clean_time(raw_end   - offset, ad_breaks)
        clean_start = max(0.0, min(clean_start, clean_dur))
        clean_end   = max(0.0, min(clean_end,   clean_dur))
        if clean_end <= clean_start:
            continue
        spk_raw   = seg.get("speaker", "")
        spk_label = speaker_map.get(spk_raw, "SPEAKER_MAIN")
        segments_clean.append({
            "text":    seg["text"].strip(),
            "start":   clean_start,
            "end":     clean_end,
            "speaker": spk_label,
        })

    if not segments_clean:
        print(f"  ✗ all segments before offset, skipping", flush=True)
        return 0
    print(f"  {len(segments_clean)} content segments (of {len(segments_raw)} total)",
          flush=True)

    # ── 6. whisperx alignment in batches — all at 16 kHz ──────────────────────
    # Each batch decodes only BATCH_DUR seconds from the MP3 at 16k,
    # which is tiny compared to loading the full file at 48k.
    BATCH_DUR = 300.0   # seconds

    word_results: list = []

    batch_start_idx   = 0
    batch_clean_start = 0.0   # position in concatenated clean audio

    while batch_start_idx < len(segments_clean):
        # Collect segments for this batch
        batch_segs = []
        batch_clean_end = batch_clean_start + BATCH_DUR
        i = batch_start_idx
        while i < len(segments_clean) and segments_clean[i]["start"] < batch_clean_end:
            batch_segs.append(segments_clean[i])
            i += 1
        if not batch_segs:
            i = batch_start_idx + 1

        # ── decode this batch's audio at 16 kHz from the MP3 ─────────────────
        # We need to reassemble clean audio from possibly multiple raw regions.
        # Find which raw-time ranges correspond to [batch_clean_start, batch_clean_end].
        raw_start_of_batch = clean_time_to_raw_time(batch_clean_start, offset, ad_breaks)
        raw_end_of_batch   = clean_time_to_raw_time(
            min(batch_clean_end + 10.0, clean_dur), offset, ad_breaks
        )

        # Collect all clean sub-regions that overlap this batch window
        # (handles ad breaks that fall inside the batch)
        batch_audio_parts = []
        for rs, re_ in clean_regions:
            # Does this clean region overlap [raw_start_of_batch, raw_end_of_batch]?
            seg_raw_s = max(rs, raw_start_of_batch)
            seg_raw_e = min(re_, raw_end_of_batch)
            if seg_raw_e > seg_raw_s + 0.01:
                part = load_audio_segment_16k(mp3_path, seg_raw_s, seg_raw_e)
                batch_audio_parts.append(part)

        if not batch_audio_parts:
            # Nothing decoded — skip (shouldn't happen)
            word_results.extend([[] for _ in batch_segs])
            batch_start_idx  = i
            batch_clean_start = batch_clean_end
            continue

        batch_audio_16k = np.concatenate(batch_audio_parts)
        del batch_audio_parts

        # whisperx wants times relative to the start of batch_audio_16k
        wx_segs = [{
            "text":  s["text"],
            "start": s["start"] - batch_clean_start,
            "end":   s["end"]   - batch_clean_start,
        } for s in batch_segs]

        try:
            word_map = align_segments_whisperx(wx_segs, batch_audio_16k)
        except Exception as exc:
            print(f"  ⚠ whisperx error in batch [{batch_clean_start:.0f}s]: {exc}",
                  flush=True)
            word_map = {}

        del batch_audio_16k
        gc.collect()

        # Shift word times back to absolute clean-audio space
        for j, seg in enumerate(batch_segs):
            words_raw = word_map.get(j, [])
            words_abs = []
            for w in words_raw:
                ws = w.get("start")
                we = w.get("end")
                wt = clean_word(w.get("word", ""))
                if ws is None or we is None or not wt:
                    continue
                words_abs.append({
                    "word":  wt,
                    "start": float(ws) + batch_clean_start,
                    "end":   float(we) + batch_clean_start,
                })
            word_results.append(words_abs)

        batch_start_idx   = i
        batch_clean_start = batch_segs[-1]["end"] if batch_segs else batch_clean_end

    assert len(word_results) == len(segments_clean), \
        f"word_results mismatch: {len(word_results)} vs {len(segments_clean)}"

    # ── 7. fill in fallback timestamps for unaligned segments ─────────────────
    for seg, words in zip(segments_clean, word_results):
        if words:
            seg["words"] = words
        else:
            raw_words = [clean_word(w) for w in seg["text"].split() if clean_word(w)]
            if not raw_words:
                seg["words"] = []
                continue
            dur  = seg["end"] - seg["start"]
            wdur = dur / len(raw_words)
            seg["words"] = [
                {"word": w, "start": seg["start"] + idx * wdur,
                 "end": seg["start"] + (idx + 1) * wdur}
                for idx, w in enumerate(raw_words)
            ]

    # ── 8. group consecutive same-speaker segments into turns ──────────────────
    turns = []
    for seg in segments_clean:
        spk = seg["speaker"]
        if turns and turns[-1][0] == spk:
            turns[-1][1].extend(seg["words"])
            turns[-1][3] = seg["end"]
        else:
            turns.append([spk, list(seg["words"]), seg["start"], seg["end"]])

    # ── 9. split boundaries ────────────────────────────────────────────────────
    split_points = [0.0]
    for k in range(len(turns) - 1):
        mid = (turns[k][3] + turns[k + 1][2]) / 2.0
        mid = max(split_points[-1], min(mid, clean_dur))
        split_points.append(mid)
    split_points.append(clean_dur)

    # ── 10. write output chunks — decode 48k audio only for each small chunk ──
    chunk_count = 0
    for k, (speaker, words, _ts, _te) in enumerate(turns):
        chunk_clean_s = split_points[k]
        chunk_clean_e = split_points[k + 1]
        chunk_dur     = chunk_clean_e - chunk_clean_s
        if chunk_dur < MIN_CHUNK_DUR:
            continue

        # Build alignment list first (cheap)
        aligns = []
        for wd in words:
            adj_s = round(max(0.0,       wd["start"] - chunk_clean_s), 6)
            adj_e = round(min(chunk_dur, wd["end"]   - chunk_clean_s), 6)
            if adj_e > adj_s and wd["word"]:
                aligns.append([wd["word"], [adj_s, adj_e], speaker])
        if not aligns:
            continue

        # Decode only this chunk's audio at 48 kHz from the MP3
        raw_s = clean_time_to_raw_time(chunk_clean_s, offset, ad_breaks)
        raw_e = clean_time_to_raw_time(chunk_clean_e, offset, ad_breaks)

        # A chunk may span an ad-break boundary — decode each sub-region
        chunk_parts = []
        for rs, re_ in clean_regions:
            seg_raw_s = max(rs, raw_s)
            seg_raw_e = min(re_, raw_e)
            if seg_raw_e > seg_raw_s + 0.001:
                part = load_audio_segment_48k(mp3_path, seg_raw_s, seg_raw_e)
                chunk_parts.append(part)

        if not chunk_parts:
            continue

        chunk_mono = np.concatenate(chunk_parts)
        del chunk_parts

        # Pad/trim to expected length (ffmpeg seek can be slightly off)
        expected_samples = int(round(chunk_dur * TARGET_SR))
        if len(chunk_mono) < expected_samples:
            chunk_mono = np.pad(chunk_mono, (0, expected_samples - len(chunk_mono)))
        else:
            chunk_mono = chunk_mono[:expected_samples]

        # Double-mono stereo
        stereo = np.stack([chunk_mono, chunk_mono.copy()])
        del chunk_mono
        if speaker == "SPEAKER_MAIN":
            suffix = "main"
            stereo[1] = 0.0
        else:
            suffix = "other"
            stereo[0] = 0.0

        fname = f"ep{ep_num}_{chunk_count:04d}_{suffix}"
        write_chunk(stereo, TARGET_SR, aligns, out_dir, fname)
        del stereo
        chunk_count += 1

    print(f"  ✓ {chunk_count} chunks written to {out_dir}", flush=True)
    return chunk_count


# ── episode discovery ─────────────────────────────────────────────────────────

def find_episodes(episode_filter=None):
    trans_files = sorted(glob.glob(os.path.join(TRANSCRIPT_DIR, "episode_*.json")))
    mp3_files   = sorted(glob.glob(os.path.join(AUDIO_DIR, "*.mp3")))

    mp3_by_ep = {}
    for mp3 in mp3_files:
        m = re.search(r"^#(\d+)\s", os.path.basename(mp3))
        if m:
            mp3_by_ep[int(m.group(1))] = mp3

    episodes = []
    for tp in trans_files:
        m = re.search(r"episode_(\d+)_", os.path.basename(tp))
        if not m or "GLT" in os.path.basename(tp):
            continue
        ep_num = int(m.group(1))
        if episode_filter is not None and ep_num not in episode_filter:
            continue
        if ep_num not in mp3_by_ep:
            continue
        episodes.append((ep_num, tp, mp3_by_ep[ep_num]))

    episodes.sort()
    return episodes


def parse_episode_filter(spec: str):
    result = set()
    for part in spec.split(","):
        part = part.strip()
        if "-" in part:
            lo, hi = part.split("-", 1)
            result.update(range(int(lo), int(hi) + 1))
        else:
            result.add(int(part))
    return result


# ── main ──────────────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    parser.add_argument("--episodes", default=None,
                        help='Episode filter, e.g. "150-159" or "150,151,200"')
    parser.add_argument("--out-dir", default=OUTPUT_DIR,
                        help=f"Output directory (default: {OUTPUT_DIR})")
    parser.add_argument("--skip-existing", action="store_true",
                        help="Skip episodes that already have output chunks")
    args = parser.parse_args()

    out_dir   = args.out_dir
    os.makedirs(out_dir, exist_ok=True)

    ep_filter = parse_episode_filter(args.episodes) if args.episodes else None
    cache     = load_cache()

    episodes = find_episodes(ep_filter)
    if not episodes:
        print("No matching episodes found. Check paths.", file=sys.stderr)
        sys.exit(1)

    print(f"Found {len(episodes)} episode(s) to process → {out_dir}")

    total_chunks = 0
    total_errors = 0

    for ep_num, transcript_path, mp3_path in episodes:
        ep_label = f"ep{ep_num}"

        if args.skip_existing:
            existing = glob.glob(os.path.join(out_dir, f"ep{ep_num}_*.wav"))
            if existing:
                print(f"  skipping ep{ep_num} ({len(existing)} chunks already present)")
                continue

        if ep_label not in cache:
            print(f"  ⚠ {ep_label} not in alignment cache — using default offset=34.2s")

        try:
            n = process_episode(ep_num, transcript_path, mp3_path, cache, out_dir)
            total_chunks += n
        except Exception as exc:
            import traceback
            print(f"  ✗ {ep_label} FAILED: {exc}", flush=True)
            traceback.print_exc()
            total_errors += 1

    print(f"\n{'='*60}")
    print(f"Done.  {total_chunks} chunks written,  {total_errors} episode errors.")
    print(f"Output: {out_dir}")


if __name__ == "__main__":
    main()
