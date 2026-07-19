#!/usr/bin/env python3
"""
convert_w2json_to_nu_format.py — Convert pipeline output to nu-dialogue training format
═══════════════════════════════════════════════════════════════════════════════════════

Runs on the LOCAL machine (no GPU needed).

Reads the w2.json files produced by podcast_to_moshi_dataset.py and the
matching double_mono.wav files, and writes:

  <out_dir>/audio/ep{N}.wav          — symlink or copy of ep{N}_double_mono.wav
  <out_dir>/text/ep{N}.json          — word transcript in nu-dialogue format
  <out_dir>/train.jsonl              — manifest of training files
  <out_dir>/eval.jsonl               — manifest of held-out eval files (last --eval-episodes)

nu-dialogue per-file text format (list of word dicts):
  [
    {"speaker": "A", "word": "Hallo",  "start": 1.230, "end": 1.580},
    {"speaker": "B", "word": "guten",  "start": 2.010, "end": 2.210},
    ...
  ]

Speaker mapping from w2.json:
  The w2.json entries carry the original diarisation labels (e.g. "SPEAKER_00",
  "SPEAKER_01").  The first label encountered is mapped to "A", the second to "B".
  Labels "SPEAKER_MAIN" and "SPEAKER_OTHER" (medical dataset) are mapped the same way.

Usage:
    python3 scripts/convert_w2json_to_nu_format.py \\
        --whisper-cache /Volumes/eHDD/moshi-rag-data/datasets/whisper_cache \\
        --output-dir /Volumes/eHDD/moshi-rag-data/nu_dataset \\
        --eval-episodes 5

    # Dry-run (print stats, write nothing):
    python3 scripts/convert_w2json_to_nu_format.py \\
        --whisper-cache /Volumes/eHDD/moshi-rag-data/datasets/whisper_cache \\
        --output-dir /Volumes/eHDD/moshi-rag-data/nu_dataset \\
        --dry-run
"""

import argparse
import json
import os
import shutil
import subprocess
import sys

# ── Default paths (override via CLI) ──────────────────────────────────────────
_DEFAULT_WHISPER_CACHE = "/Volumes/eHDD/moshi-rag-data/datasets/whisper_cache"
_DEFAULT_OUTPUT_DIR    = "/Volumes/eHDD/moshi-rag-data/nu_dataset"


def wav_duration_ffprobe(path: str) -> float:
    """Return WAV duration in seconds via ffprobe."""
    r = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "default=noprint_wrappers=1:nokey=1", path],
        capture_output=True, text=True, check=True,
    )
    return float(r.stdout.strip())


def convert_episode(ep_num: int, cache_dir: str, out_audio_dir: str,
                    out_text_dir: str, symlink: bool) -> dict | None:
    """
    Process one episode.  Returns a dict {"path": ..., "duration": ...} for the
    manifest on success, or None if required files are missing.
    """
    w2_path         = os.path.join(cache_dir, f"ep{ep_num}_w2.json")
    double_mono_src = os.path.join(cache_dir, f"ep{ep_num}_double_mono.wav")

    if not os.path.exists(w2_path):
        print(f"  ep{ep_num}: missing w2.json — skipped", flush=True)
        return None
    if not os.path.exists(double_mono_src):
        print(f"  ep{ep_num}: missing double_mono.wav — skipped", flush=True)
        return None

    # ── 1. Load w2 alignments ─────────────────────────────────────────────────
    with open(w2_path, encoding="utf-8") as fh:
        raw = json.load(fh)

    alignments = raw.get("alignments", [])
    if not alignments:
        print(f"  ep{ep_num}: empty alignments in w2.json — skipped", flush=True)
        return None

    # ── 2. Build speaker map (first seen → "A", second → "B") ────────────────
    spk_map: dict[str, str] = {}
    for entry in alignments:
        spk_raw = entry[2] if len(entry) > 2 else ""
        if spk_raw and spk_raw not in spk_map:
            spk_map[spk_raw] = "A" if len(spk_map) == 0 else "B"
            if len(spk_map) == 2:
                break

    if not spk_map:
        print(f"  ep{ep_num}: no speaker labels in w2.json — skipped", flush=True)
        return None

    # ── 3. Convert to nu-dialogue word-transcript format ──────────────────────
    transcript: list[dict] = []
    for entry in alignments:
        # entry format: [word, [start, end], speaker]
        if not isinstance(entry, (list, tuple)) or len(entry) < 3:
            continue
        word, ts, spk_raw = entry[0], entry[1], entry[2]
        if not isinstance(ts, (list, tuple)) or len(ts) < 2:
            continue
        spk_letter = spk_map.get(spk_raw)
        if spk_letter is None:
            # More than 2 speakers — assign to "B" (treat as second speaker)
            spk_letter = "B"
        transcript.append({
            "speaker": spk_letter,
            "word":    str(word),
            "start":   round(float(ts[0]), 4),
            "end":     round(float(ts[1]), 4),
        })

    if len(transcript) < 10:
        print(f"  ep{ep_num}: only {len(transcript)} words — skipped (too short)", flush=True)
        return None

    # ── 4. Write text JSON ────────────────────────────────────────────────────
    out_name    = f"ep{ep_num}"
    text_path   = os.path.join(out_text_dir, f"{out_name}.json")
    with open(text_path, "w", encoding="utf-8") as fh:
        json.dump(transcript, fh, ensure_ascii=False)

    # ── 5. Link or copy audio ─────────────────────────────────────────────────
    audio_path = os.path.join(out_audio_dir, f"{out_name}.wav")
    if os.path.exists(audio_path) or os.path.islink(audio_path):
        os.remove(audio_path)
    if symlink:
        os.symlink(os.path.abspath(double_mono_src), audio_path)
    else:
        shutil.copy2(double_mono_src, audio_path)

    # ── 6. Get duration ───────────────────────────────────────────────────────
    try:
        duration = wav_duration_ffprobe(double_mono_src)
    except Exception as e:
        print(f"  ep{ep_num}: ffprobe failed ({e}) — using 0.0", flush=True)
        duration = 0.0

    n_a = sum(1 for w in transcript if w["speaker"] == "A")
    n_b = sum(1 for w in transcript if w["speaker"] == "B")
    print(f"  ep{ep_num}: {len(transcript)} words  "
          f"(A={n_a}, B={n_b})  "
          f"spk_map={spk_map}  "
          f"dur={duration:.1f}s  → {out_name}", flush=True)

    return {"path": audio_path, "duration": round(duration, 6)}


def main():
    parser = argparse.ArgumentParser(
        description="Convert podcast_to_moshi_dataset.py output to nu-dialogue training format",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        "--whisper-cache", default=_DEFAULT_WHISPER_CACHE,
        help=f"Directory containing ep{{N}}_w2.json and ep{{N}}_double_mono.wav files "
             f"(default: {_DEFAULT_WHISPER_CACHE})",
    )
    parser.add_argument(
        "--output-dir", default=_DEFAULT_OUTPUT_DIR,
        help=f"Root output directory (default: {_DEFAULT_OUTPUT_DIR})",
    )
    parser.add_argument(
        "--eval-episodes", type=int, default=5,
        help="Number of episodes to hold out for eval.jsonl (default: 5)",
    )
    parser.add_argument(
        "--symlink", action="store_true", default=True,
        help="Symlink audio files instead of copying (default: True)",
    )
    parser.add_argument(
        "--no-symlink", action="store_false", dest="symlink",
        help="Copy audio files instead of symlinking",
    )
    parser.add_argument(
        "--dry-run", action="store_true",
        help="Only print what would be done; write nothing",
    )
    args = parser.parse_args()

    cache_dir   = args.whisper_cache
    out_dir     = args.output_dir
    out_audio   = os.path.join(out_dir, "audio")
    out_text    = os.path.join(out_dir, "text")

    if not os.path.isdir(cache_dir):
        print(f"ERROR: --whisper-cache does not exist: {cache_dir}", file=sys.stderr)
        sys.exit(1)

    # Discover episodes from w2.json files present in the cache
    import glob as _glob
    w2_files = sorted(_glob.glob(os.path.join(cache_dir, "ep*_w2.json")))
    import re
    ep_nums = sorted({int(m.group(1))
                      for f in w2_files
                      for m in [re.search(r"ep(\d+)_w2\.json$", os.path.basename(f))]
                      if m})

    if not ep_nums:
        print(f"No ep*_w2.json files found in {cache_dir}. "
              f"Run podcast_to_moshi_dataset.py first.", file=sys.stderr)
        sys.exit(1)

    print(f"Found {len(ep_nums)} episodes with w2.json: {ep_nums}", flush=True)

    if args.dry_run:
        print("[DRY RUN] Would write to:", out_dir)
        print(f"  train episodes: {ep_nums[:-args.eval_episodes] if args.eval_episodes else ep_nums}")
        print(f"  eval  episodes: {ep_nums[-args.eval_episodes:] if args.eval_episodes else []}")
        return

    os.makedirs(out_audio, exist_ok=True)
    os.makedirs(out_text,  exist_ok=True)

    train_manifest = []
    eval_manifest  = []
    eval_ep_set    = set(ep_nums[-args.eval_episodes:]) if args.eval_episodes else set()

    for ep_num in ep_nums:
        entry = convert_episode(ep_num, cache_dir, out_audio, out_text,
                                symlink=args.symlink)
        if entry is None:
            continue
        if ep_num in eval_ep_set:
            eval_manifest.append(entry)
        else:
            train_manifest.append(entry)

    # Write manifests
    train_jsonl = os.path.join(out_dir, "train.jsonl")
    eval_jsonl  = os.path.join(out_dir, "eval.jsonl")

    with open(train_jsonl, "w", encoding="utf-8") as fh:
        for item in train_manifest:
            fh.write(json.dumps(item) + "\n")
    print(f"\nWrote {len(train_manifest)} train entries → {train_jsonl}")

    if eval_manifest:
        with open(eval_jsonl, "w", encoding="utf-8") as fh:
            for item in eval_manifest:
                fh.write(json.dumps(item) + "\n")
        print(f"Wrote {len(eval_manifest)} eval  entries → {eval_jsonl}")

    print(f"\nDone. Output structure:")
    print(f"  {out_dir}/audio/ep{{N}}.wav       — {'symlinks to' if args.symlink else 'copies of'} double_mono.wav")
    print(f"  {out_dir}/text/ep{{N}}.json       — nu-dialogue word transcripts")
    print(f"  {out_dir}/train.jsonl")
    if eval_manifest:
        print(f"  {out_dir}/eval.jsonl")
    print(f"\nNext step: upload {out_dir} to the Modal volume, then run modal_tokenize_dataset.py")


if __name__ == "__main__":
    main()
