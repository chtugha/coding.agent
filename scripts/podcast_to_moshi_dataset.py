#!/usr/bin/env python3
"""
podcast_to_moshi_dataset.py — Phase 1: word-level timestamp alignment
═══════════════════════════════════════════════════════════════════════

ARCHITECTURE
────────────
Step 1 — Transcribe full raw MP3 with whisper-cpp
    No intro-stripping, no chunking.  Whisper sees the whole file.
    Output: flat word list with audio-grounded timestamps.
    Saved:  whisper_cache/ep{N}_w1.json

Step 2 — Auto-detect offset(s) + align words to transcript
    The transcript has correct text+speaker+relative-timestamps but was
    produced on the clean audio (intro/ads removed).  The raw MP3 whisper
    timestamps are shifted by an unknown amount δ that may jump at ad breaks.

    We detect δ automatically:
      a) Sample ~40 anchor segments spread across the transcript.
      b) For each anchor, search the whisper word stream for matching text
         in a ±60s window around the current δ estimate.
      c) Build a piecewise δ timeline — sudden jumps reveal ad-break positions.

    For each whisper word, look up which transcript interval contains
    word.start − δ(t).  Assign that segment's speaker + text.
    Words with no match = intro / ad / outro → out_of_transcript=True.

    Saved:  aligned_cache/ep{N}_aligned.json

Step 3 — Rerun whisper on stripped audio + compare timestamps
    Keep only the assigned-word regions from the raw MP3.
    Run whisper-cpp on the stripped WAV.
    Compare word timestamps: w1-assigned vs w2-stripped.
    Saved:  whisper_cache/ep{N}_w2.json

Usage:
    python3 scripts/podcast_to_moshi_dataset.py --episodes 152
    python3 scripts/podcast_to_moshi_dataset.py --episodes 150-152
"""

import argparse
import bisect
import glob
import json
import os
import re
import subprocess
import sys
import tempfile

# ── paths ─────────────────────────────────────────────────────────────────────
BASE_DIR       = "/Volumes/eHDD/moshi-rag-data"
DATASETS_DIR   = os.path.join(BASE_DIR, "datasets")
TRANSCRIPT_DIR = os.path.join(DATASETS_DIR, "Gemischtes.Hack.Podcast", "transcripts")
AUDIO_DIR      = os.path.join(DATASETS_DIR, "Gemischtes.Hack.Podcast")
WHISPER_CACHE  = os.path.join(DATASETS_DIR, "whisper_cache")
ALIGNED_CACHE  = os.path.join(DATASETS_DIR, "aligned_cache")

WHISPER_CLI    = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "whisper-cpp", "build", "bin", "whisper-cli",
)
WHISPER_MODEL  = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "bin", "models", "ggml-large-v3-turbo-q5_0.bin",
)

WHISPER_THREADS = 8


# ══════════════════════════════════════════════════════════════════════════════
# Shared helper — whisper-cpp → flat word list
# ══════════════════════════════════════════════════════════════════════════════

def norm_words(text: str) -> list:
    """Lowercase + strip punctuation → list of word strings."""
    return [w for w in
            re.sub(r"[^\w\däöüßàáâãåæçèéêëìíîïðñòóôõøùúûüýþÿ-]", " ",
                   text.lower()).split()
            if w]


def run_whisper_words(wav_path: str) -> list:
    """
    Run whisper-cpp on a 16 kHz mono WAV.
    Returns a flat list of {word, start, end, p} dicts.
    Sub-word BPE tokens are merged into words (space prefix = word boundary).
    """
    with tempfile.TemporaryDirectory() as td:
        out_base = os.path.join(td, "out")
        r = subprocess.run([
            WHISPER_CLI, "-m", WHISPER_MODEL, "-l", "de", "-f", wav_path,
            "--output-json-full", "--output-file", out_base,
            "--threads", str(WHISPER_THREADS), "--beam-size", "5",
        ], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"whisper-cpp failed:\n{r.stderr[:600]}")
        raw = json.load(open(out_base + ".json", encoding="utf-8"))

    words = []
    for seg in raw.get("transcription", []):
        cur, cur_s, cur_e, cur_p = "", None, None, []
        for tok in seg.get("tokens", []):
            t = tok["text"]
            if t.startswith("[_") or t.startswith("<"):
                continue
            ts = tok["offsets"]["from"] / 1000.0
            te = tok["offsets"]["to"]   / 1000.0
            tp = tok.get("p", 1.0)
            if (t.startswith(" ") or cur == "") and cur:
                w = cur.strip()
                if w and cur_e is not None and cur_e > (cur_s or 0):
                    words.append({"word": w,
                                  "start": round(cur_s, 4),
                                  "end":   round(cur_e, 4),
                                  "p":     round(sum(cur_p)/len(cur_p), 4)})
                cur, cur_s, cur_e, cur_p = t, ts, te, [tp]
            else:
                cur += t
                if cur_s is None: cur_s = ts
                cur_e = te; cur_p.append(tp)
        if cur.strip() and cur_e is not None and cur_e > (cur_s or 0):
            w = cur.strip()
            words.append({"word": w,
                          "start": round(cur_s, 4),
                          "end":   round(cur_e, 4),
                          "p":     round(sum(cur_p)/len(cur_p), 4)})
    return words


# ══════════════════════════════════════════════════════════════════════════════
# Step 1 — Transcribe full raw MP3
# ══════════════════════════════════════════════════════════════════════════════

def step1_transcribe(ep_num: int, mp3_path: str) -> list:
    """Run whisper-cpp directly on the MP3 → flat word list."""
    os.makedirs(WHISPER_CACHE, exist_ok=True)
    out_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w1.json")

    print(f"  step1: running whisper-cpp on MP3…", flush=True)
    words = run_whisper_words(mp3_path)

    print(f"  step1: {len(words)} words  "
          f"{words[0]['start']:.2f}s – {words[-1]['end']:.2f}s", flush=True)
    json.dump(words, open(out_path, "w", encoding="utf-8"), ensure_ascii=False)
    print(f"  step1: saved → {out_path}", flush=True)
    return words


# ══════════════════════════════════════════════════════════════════════════════
# Step 2 — Segment-by-segment alignment
# ══════════════════════════════════════════════════════════════════════════════

def match_segment_in_whisper(t_norm: list, whisper_words: list,
                              w_starts: list, search_from: int,
                              expected_raw_t: float, window: float) -> tuple:
    """
    Find the best position in the whisper stream for a transcript segment.

    Uses a sliding window of (len(t_norm) + SLACK) whisper words scored by
    longest-common-subsequence overlap, tolerating whisper insertions/deletions
    at segment boundaries.

    Returns (best_whisper_index, score) or (None, 0.0) if no good match.
    """
    n     = len(t_norm)
    SLACK = min(4, n)   # allow up to SLACK extra whisper words per window
    WIN   = n + SLACK

    lo = bisect.bisect_left(w_starts,  expected_raw_t - window)
    hi = bisect.bisect_right(w_starts, expected_raw_t + window)
    lo = max(lo, search_from)
    if lo >= hi:
        return None, 0.0

    best_score, best_i = 0.0, None
    for i in range(lo, hi):
        end  = min(i + WIN, hi)
        flat = []
        for j in range(i, end):
            k = norm_words(whisper_words[j]["word"])
            flat.append(k[0] if k else "")
        # LCS: count t_norm words found in flat in order
        ti = fi = 0
        while ti < n and fi < len(flat):
            if t_norm[ti] == flat[fi]:
                ti += 1
            fi += 1
        score = ti / n
        if score > best_score:
            best_score = score
            best_i = i

    if best_score < 0.50 or best_i is None:
        return None, 0.0

    return best_i, best_score


def step2_align(ep_num: int, whisper_words: list,
                transcript_segs: list) -> list:
    """
    Aligns every transcript segment (ground truth) to raw-MP3 timestamps from
    whisper.  The transcript is the authority on text and order — no segment is
    ever skipped from the output.

    Pass 1 — build per-segment delta:
      Walk segments in order.  For each segment with ≥MIN_WORDS:
        • Search whisper near (seg.start + current_delta) ± SEARCH_WINDOW.
        • If found: update delta, advance whisper cursor past matched words.
        • If not found: widen to WIDE_WINDOW once (ad-break recovery).
        • If still not found: keep current delta (whisper mistranscription).
      Short segments always inherit the current delta.

    Pass 2 — build raw-time windows for every segment:
      raw_start = seg.start + delta_for_this_seg
      raw_end   = seg.end   + delta_for_this_seg
      Every segment gets a window — none are skipped.

    Pass 3 — mark ad-break gaps:
      Walk consecutive segment windows.  If the raw gap between window[i].end
      and window[i+1].start is ≥ MIN_AD_GAP seconds, the whisper words that
      fall in that gap are out_of_transcript (intro/ad/outro).

    Pass 4 — assign whisper words to segment windows.
    """
    os.makedirs(ALIGNED_CACHE, exist_ok=True)
    out_path = os.path.join(ALIGNED_CACHE, f"ep{ep_num}_aligned.json")

    w_starts = [w["start"] for w in whisper_words]

    SEARCH_WINDOW = 45.0  # normal per-segment search window (±s)
    WIDE_WINDOW   = 120.0 # bootstrap / ad-break recovery window (±s)
    MIN_WORDS     = 4     # minimum words to use a segment as an anchor
    MIN_AD_GAP    = 8.0   # raw-audio gap ≥ this → real ad/intro/outro break
    DEBUG         = False  # set True to print per-segment match details

    def min_score(n_words: int) -> float:
        """Minimum acceptable LCS score — higher threshold for short segments."""
        if n_words <= 4:  return 0.75
        if n_words <= 6:  return 0.67
        if n_words <= 10: return 0.60
        return 0.55

    delta    = None  # raw_mp3_time = transcript_time + delta
    w_cursor = 0     # whisper index — advance past last matched words
    n_matched = 0
    n_missed  = 0

    # Per-segment delta: full_deltas[i] = delta to apply to segment i
    full_deltas = [None] * len(transcript_segs)

    print(f"  step2: aligning {len(transcript_segs)} segments…", flush=True)

    # ── Pass 1: anchor matching ───────────────────────────────────────────────
    for si, seg in enumerate(transcript_segs):
        t_norm  = norm_words(seg["text"])
        t_start = float(seg["start"])

        if len(t_norm) >= MIN_WORDS:
            expected = t_start + (delta if delta is not None else 0.0)
            window   = WIDE_WINDOW if delta is None else SEARCH_WINDOW

            n = len(t_norm)
            best_i, score = match_segment_in_whisper(
                t_norm, whisper_words, w_starts, w_cursor, expected, window)

            if (best_i is None or score < min_score(n)) and delta is not None:
                best_i2, score2 = match_segment_in_whisper(
                    t_norm, whisper_words, w_starts, w_cursor, expected, WIDE_WINDOW)
                if best_i2 is not None and score2 >= min_score(n):
                    best_i, score = best_i2, score2
                elif best_i is not None and score < min_score(n):
                    best_i = None  # reject the low-confidence normal-window match

            if best_i is not None and score >= min_score(n):
                # Compute delta via LCS pairs: walk t_norm and the whisper
                # window in LCS order, recording (w.start - proportional_t)
                # for each matched pair, then take the median.
                seg_dur = float(seg["end"]) - t_start
                SLACK   = min(4, n)
                WIN     = n + SLACK
                end_wi  = min(best_i + WIN, len(whisper_words))

                flat = []
                for j in range(best_i, end_wi):
                    k = norm_words(whisper_words[j]["word"])
                    flat.append((k[0] if k else "", j))  # (norm, original_index)

                # Replay LCS to get matched pairs (t_norm_pos, whisper_idx)
                ti = fi = 0
                pairs = []
                while ti < n and fi < len(flat):
                    if t_norm[ti] == flat[fi][0]:
                        pairs.append((ti, flat[fi][1]))
                        ti += 1
                    fi += 1

                if pairs:
                    # Median delta across matched pairs
                    indiv = []
                    for (tk, wi) in pairs:
                        t_word = t_start + (tk / n) * seg_dur
                        indiv.append(whisper_words[wi]["start"] - t_word)
                    indiv.sort()
                    delta = indiv[len(indiv) // 2]
                else:
                    delta = whisper_words[best_i]["start"] - t_start

                n_matched += 1
                w_cursor = best_i + n    # advance only past matched segment length

                if DEBUG:
                    w_text = " ".join(whisper_words[j]["word"]
                                      for j in range(best_i, end_wi))
                    print(f"  MATCH si={si:4d} t={t_start:7.1f}s "
                          f"score={score:.2f} δ={delta:+.2f}s  "
                          f"T: {seg['text'].strip()[:50]!r}", flush=True)
                    print(f"        W: {w_text[:60]!r}", flush=True)
            else:
                n_missed += 1
                if DEBUG:
                    print(f"  MISS  si={si:4d} t={t_start:7.1f}s "
                          f"δ={delta if delta else 0:+.2f}s  "
                          f"T: {seg['text'].strip()[:60]!r}", flush=True)

        # Every segment gets a delta — unmatched inherit last known delta.
        if delta is not None:
            full_deltas[si] = delta

    if delta is None:
        raise RuntimeError("step2: could not align any segment")

    # Back-fill segments before the first anchor with the first known delta.
    first_delta = next(d for d in full_deltas if d is not None)
    full_deltas = [d if d is not None else first_delta for d in full_deltas]

    print(f"  step2: matched {n_matched}  missed {n_missed}  "
          f"total {len(transcript_segs)} segments", flush=True)

    # ── Pass 2: build raw-time windows for every segment ─────────────────────
    seg_windows = []  # (raw_start, raw_end, speaker, seg_text) — one per segment
    for si, seg in enumerate(transcript_segs):
        d   = full_deltas[si]
        rs  = float(seg["start"]) + d
        re  = float(seg["end"])   + d
        seg_windows.append((rs, re, seg.get("speaker", ""), seg.get("text", "").strip()))

    # ── Pass 3: identify genuine gap regions (ad breaks / intro / outro) ──────
    # A gap between consecutive segment windows that is ≥ MIN_AD_GAP seconds
    # in raw time is a real ad/intro/outro.  Whisper words in those gaps get
    # out_of_transcript = True.
    gap_regions = []  # list of (raw_gap_start, raw_gap_end)

    # Gap before the first segment (intro jingle)
    first_raw_start = seg_windows[0][0]
    if first_raw_start > MIN_AD_GAP:
        gap_regions.append((0.0, first_raw_start))
        print(f"  step2: intro gap  0.0s – {first_raw_start:.1f}s  "
              f"({first_raw_start:.1f}s)", flush=True)

    # Gaps between consecutive segments
    for i in range(len(seg_windows) - 1):
        gap_start = seg_windows[i][1]     # end of segment i
        gap_end   = seg_windows[i + 1][0] # start of segment i+1
        gap_dur   = gap_end - gap_start
        if gap_dur >= MIN_AD_GAP:
            gap_regions.append((gap_start, gap_end))
            print(f"  step2: gap  {gap_start:.1f}s – {gap_end:.1f}s  "
                  f"({gap_dur:.1f}s)", flush=True)

    print(f"  step2: {len(gap_regions)} gap region(s) detected", flush=True)

    # Build a fast lookup: is raw_t inside any gap?
    gap_starts = [g[0] for g in gap_regions]
    gap_ends   = [g[1] for g in gap_regions]

    def in_gap(raw_t: float) -> bool:
        idx = bisect.bisect_right(gap_starts, raw_t) - 1
        return idx >= 0 and raw_t < gap_ends[idx]

    # ── Pass 4: assign each whisper word to a segment window ─────────────────
    sw_starts = [s[0] for s in seg_windows]
    sw_ends   = [s[1] for s in seg_windows]

    result  = []
    n_in = n_out = 0
    for w in whisper_words:
        raw_t = w["start"]

        if in_gap(raw_t):
            result.append({
                "word": w["word"], "start": w["start"], "end": w["end"],
                "p": w["p"], "speaker": None, "seg_text": None,
                "out_of_transcript": True,
            })
            n_out += 1
            continue

        idx = bisect.bisect_right(sw_starts, raw_t) - 1
        if idx >= 0 and raw_t <= sw_ends[idx]:
            result.append({
                "word":              w["word"],
                "start":             w["start"],
                "end":               w["end"],
                "p":                 w["p"],
                "speaker":           seg_windows[idx][2],
                "seg_text":          seg_windows[idx][3],
                "out_of_transcript": False,
            })
            n_in += 1
        else:
            result.append({
                "word": w["word"], "start": w["start"], "end": w["end"],
                "p": w["p"], "speaker": None, "seg_text": None,
                "out_of_transcript": True,
            })
            n_out += 1

    pct = 100.0 * n_in / len(result) if result else 0
    print(f"  step2: {n_in} words assigned ({pct:.1f}%)  "
          f"{n_out} outside (intro/ad/outro)", flush=True)

    json.dump(result, open(out_path, "w", encoding="utf-8"), ensure_ascii=False)
    print(f"  step2: saved → {out_path}", flush=True)
    return result


def print_alignment_report(aligned_words: list, transcript_segs: list):
    """Print assignment rate + first 5 segment examples."""
    in_words  = [w for w in aligned_words if not w["out_of_transcript"]]
    out_words = [w for w in aligned_words if     w["out_of_transcript"]]
    pct = 100.0 * len(in_words) / len(aligned_words) if aligned_words else 0

    print(f"\n  Alignment QA:", flush=True)
    print(f"  Assigned  : {len(in_words)}  ({pct:.1f}%)", flush=True)
    print(f"  Excluded  : {len(out_words)}  (intro/ad/outro)", flush=True)

    # Group assigned words by seg_text so we can show per-segment coverage
    print(f"\n  First 5 transcript segments vs whisper words:", flush=True)
    for seg in transcript_segs[:5]:
        seg_txt = seg.get("text", "").strip()
        seg_words = [w for w in in_words if w["seg_text"] == seg_txt]
        whisper_text = " ".join(w["word"] for w in seg_words)
        t_norm = set(norm_words(seg_txt))
        w_norm = set(norm_words(whisper_text))
        conf = len(t_norm & w_norm) / len(t_norm) if t_norm else 1.0
        t_s, t_e = float(seg["start"]), float(seg["end"])
        print(f"  [{t_s:.2f}–{t_e:.2f}s] conf={conf:.2f} n={len(seg_words)}", flush=True)
        print(f"    T: '{seg_txt[:80]}'", flush=True)
        print(f"    W: '{whisper_text[:80]}'", flush=True)


# ══════════════════════════════════════════════════════════════════════════════
# Step 3 — Stripped audio rerun + timestamp comparison
# ══════════════════════════════════════════════════════════════════════════════

def derive_keep_regions(aligned_words: list) -> list:
    """
    From aligned words, derive contiguous keep-regions in raw MP3 time.
    A new region starts whenever there is a gap > 2s between consecutive
    assigned words (= intro, ad-break, or outro boundary).
    """
    in_words = [w for w in aligned_words if not w["out_of_transcript"]]
    if not in_words:
        raise ValueError("No assigned words")
    regions = []
    r_start = in_words[0]["start"]
    prev_e  = in_words[0]["end"]
    for w in in_words[1:]:
        if w["start"] - prev_e > 2.0:
            regions.append((r_start, prev_e))
            r_start = w["start"]
        prev_e = w["end"]
    regions.append((r_start, prev_e))
    return regions


def build_stripped_mp3(mp3_path: str, keep_regions: list, out_mp3: str):
    """Extract keep_regions from raw MP3 and concatenate into a single MP3."""
    inputs, labels = [], []
    for idx, (rs, re_) in enumerate(keep_regions):
        inputs += ["-ss", str(rs), "-t", str(re_ - rs), "-i", mp3_path]
        labels.append(f"[{idx}:a]")
    n   = len(keep_regions)
    flt = "".join(labels) + f"concat=n={n}:v=0:a=1[out]"
    subprocess.run(["ffmpeg", "-v", "error"] + inputs +
                   ["-filter_complex", flt, "-map", "[out]", out_mp3, "-y"],
                   check=True)


def step3_rerun_compare(ep_num: int, mp3_path: str, aligned_words: list) -> list:
    """
    Build stripped WAV, run whisper-cpp on it, then update each assigned word's
    timestamps to use the more accurate w2 timestamps (clean audio, no intro
    distraction).

    Matching is done positionally along the w2 word stream: we walk w1_in and w2
    in parallel using a small sliding window, matching on normalised text.  This
    is robust because both streams cover the same audio region in the same order.

    Returns updated aligned_words with 'start'/'end' replaced by w2-remapped
    timestamps for every word that could be matched; unmatched words keep their
    w1 timestamps.  The refined list is saved to aligned_cache/ep{N}_aligned.json.

    w2 timestamps are relative to the stripped WAV start.
    w1 timestamps are in raw MP3 time.
    We remap w2 timestamps back to raw MP3 time using the keep_region boundaries.
    """
    os.makedirs(WHISPER_CACHE, exist_ok=True)
    out_path    = os.path.join(WHISPER_CACHE,  f"ep{ep_num}_w2.json")
    refined_path = os.path.join(ALIGNED_CACHE, f"ep{ep_num}_aligned.json")

    keep_regions = derive_keep_regions(aligned_words)
    total_kept   = sum(e - s for s, e in keep_regions)
    print(f"  step3: {len(keep_regions)} keep-region(s)  {total_kept:.0f}s total",
          flush=True)

    with tempfile.NamedTemporaryFile(suffix=".mp3", delete=False) as f:
        tmp = f.name
    try:
        build_stripped_mp3(mp3_path, keep_regions, tmp)
        print(f"  step3: stripped MP3 built — running whisper-cpp…", flush=True)
        w2 = run_whisper_words(tmp)
    finally:
        os.unlink(tmp)

    json.dump(w2, open(out_path, "w", encoding="utf-8"), ensure_ascii=False)
    print(f"  step3: {len(w2)} words saved → {out_path}", flush=True)

    # ── Remap w2 timestamps from stripped-WAV time → raw MP3 time ────────────
    cum_offsets = []   # (stripped_start, raw_start) for each region
    stripped_t  = 0.0
    for rs, re_ in keep_regions:
        cum_offsets.append((stripped_t, rs))
        stripped_t += (re_ - rs)
    stripped_starts = [c[0] for c in cum_offsets]

    def to_raw(stripped: float) -> float:
        idx = bisect.bisect_right(stripped_starts, stripped) - 1
        if idx < 0: idx = 0
        offset_in_region = stripped - cum_offsets[idx][0]
        return cum_offsets[idx][1] + offset_in_region

    # ── Time-anchored text match: for each w1 word find the w2 word with the
    # same normalised text whose remapped timestamp is closest to w1's timestamp,
    # within a ±3s window.  This is robust to divergences between the two
    # whisper passes because it uses w1's time as an anchor, not a global cursor.
    # Build index: norm_text → sorted list of (raw_time, end_raw_time, w2_idx)
    w2_index: dict = {}
    for i, w in enumerate(w2):
        key = norm_words(w["word"])
        key = key[0] if key else ""
        if key:
            t_raw = to_raw(w["start"])
            e_raw = to_raw(w["end"])
            w2_index.setdefault(key, []).append((t_raw, e_raw))
    # Sort each list by time so we can bisect
    w2_starts: dict = {}
    for key, entries in w2_index.items():
        entries.sort()
        w2_starts[key] = [e[0] for e in entries]

    WINDOW = 3.0   # ±seconds around w1 timestamp to accept a w2 match

    w1_in  = [w for w in aligned_words if not w["out_of_transcript"]]
    n_updated = n_kept = 0
    deltas: list = []
    large:  list = []

    for w1w in w1_in:
        key = norm_words(w1w["word"])
        key = key[0] if key else ""
        if not key or key not in w2_index:
            n_kept += 1
            continue

        entries  = w2_index[key]
        starts   = w2_starts[key]
        t1       = w1w["start"]

        # Binary-search for candidates within ±WINDOW
        lo = bisect.bisect_left(starts,  t1 - WINDOW)
        hi = bisect.bisect_right(starts, t1 + WINDOW)
        if lo >= hi:
            n_kept += 1
            continue

        # Pick the closest one
        best_t, best_e = min(entries[lo:hi], key=lambda e: abs(e[0] - t1))
        d = abs(t1 - best_t)
        deltas.append(d)
        if d > 0.3:
            large.append((w1w["word"], t1, best_t))
        w1w["start"] = round(best_t, 4)
        w1w["end"]   = round(best_e, 4)
        n_updated += 1

    # ── Report ────────────────────────────────────────────────────────────────
    n      = len(deltas)
    mean_d = sum(deltas) / n if n else 0
    max_d  = max(deltas)     if n else 0

    print(f"\n  Timestamp refinement (w1 → w2, positional stream-match):",
          flush=True)
    print(f"  w1-assigned words  : {len(w1_in)}", flush=True)
    print(f"  Updated to w2      : {n_updated}  kept w1: {n_kept}", flush=True)
    print(f"  Mean |Δ| before    : {mean_d*1000:.0f} ms", flush=True)
    print(f"  Max  |Δ| before    : {max_d*1000:.0f} ms", flush=True)
    print(f"  Words Δ>300ms      : {len(large)}", flush=True)
    if large:
        print(f"\n  Large deltas (first 10):", flush=True)
        for word, t1, t2 in large[:10]:
            print(f"    '{word}'  w1={t1:.3f}s  w2={t2:.3f}s  "
                  f"Δ={abs(t1-t2)*1000:.0f}ms", flush=True)

    # ── Save refined alignment ─────────────────────────────────────────────
    json.dump(aligned_words, open(refined_path, "w", encoding="utf-8"),
              ensure_ascii=False)
    print(f"\n  step3: refined alignment saved → {refined_path}", flush=True)
    return aligned_words


# ══════════════════════════════════════════════════════════════════════════════
# Episode discovery + orchestration
# ══════════════════════════════════════════════════════════════════════════════

def find_episodes(episode_filter=None) -> list:
    trans_files = sorted(glob.glob(os.path.join(TRANSCRIPT_DIR, "episode_*.json")))
    mp3_files   = sorted(glob.glob(os.path.join(AUDIO_DIR, "*.mp3")))
    mp3_by_ep   = {}
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
        if ep_num in mp3_by_ep:
            episodes.append((ep_num, tp, mp3_by_ep[ep_num]))
    episodes.sort()
    return episodes


def parse_episode_filter(spec: str) -> set:
    result = set()
    for part in spec.split(","):
        part = part.strip()
        if "-" in part:
            lo, hi = part.split("-", 1)
            result.update(range(int(lo), int(hi) + 1))
        else:
            result.add(int(part))
    return result


def process_episode(ep_num: int, transcript_path: str, mp3_path: str):
    print(f"\n{'═'*60}", flush=True)
    print(f"  Episode {ep_num}  —  {os.path.basename(mp3_path)}", flush=True)
    print(f"{'─'*60}", flush=True)

    tdata = json.load(open(transcript_path, encoding="utf-8"))
    transcript_segs = [s for s in tdata.get("segments", [])
                       if s.get("text", "").strip()]
    print(f"  transcript: {len(transcript_segs)} segments  "
          f"[{transcript_segs[0]['start']:.1f}s – "
          f"{transcript_segs[-1]['end']:.1f}s]", flush=True)

    # ── Step 1 ────────────────────────────────────────────────────────────────
    w1_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w1.json")
    if os.path.exists(w1_path):
        w1 = json.load(open(w1_path, encoding="utf-8"))
        print(f"\n  [Step 1] cached — {len(w1)} words from {w1_path}", flush=True)
    else:
        print(f"\n  [Step 1] Transcribe full MP3", flush=True)
        w1 = step1_transcribe(ep_num, mp3_path)

    # ── Step 2 ────────────────────────────────────────────────────────────────
    print(f"\n  [Step 2] Auto-detect offsets + align words", flush=True)
    aligned = step2_align(ep_num, w1, transcript_segs)
    print_alignment_report(aligned, transcript_segs)

    # ── Step 3 ────────────────────────────────────────────────────────────────
    print(f"\n  [Step 3] Rerun whisper on stripped audio + compare", flush=True)
    step3_rerun_compare(ep_num, mp3_path, aligned)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--episodes", default=None,
                        help='e.g. "152" or "150-152" or "150,152"')
    args = parser.parse_args()

    if not os.path.isfile(WHISPER_CLI):
        sys.exit(f"whisper-cli not found: {WHISPER_CLI}")
    if not os.path.isfile(WHISPER_MODEL):
        sys.exit(f"whisper model not found: {WHISPER_MODEL}")

    ep_filt  = parse_episode_filter(args.episodes) if args.episodes else None
    episodes = find_episodes(ep_filt)

    if not episodes:
        sys.exit("No matching episodes found.")

    print(f"Found {len(episodes)} episode(s). Running all 3 steps.")

    errors = 0
    for ep_num, transcript_path, mp3_path in episodes:
        try:
            process_episode(ep_num, transcript_path, mp3_path)
        except Exception as exc:
            import traceback
            print(f"\n  ✗ ep{ep_num} FAILED: {exc}", flush=True)
            traceback.print_exc()
            errors += 1

    print(f"\n{'═'*60}")
    print(f"Done.  {errors} error(s).")


if __name__ == "__main__":
    main()
