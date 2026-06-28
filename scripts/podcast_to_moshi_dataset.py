#!/usr/bin/env python3
"""
podcast_to_moshi_dataset.py — Phase 1: word-level timestamp alignment
═══════════════════════════════════════════════════════════════════════

ARCHITECTURE
────────────
Step 1 — Convert MP3 → 16 kHz mono WAV; copy transcript as w0; transcribe WAV
    a) ffmpeg converts the raw MP3 to 16 kHz mono WAV → ep{N}.wav
    b) Original diarized transcript is copied as w0.json (ground truth)
    c) whisper-cpp transcribes the WAV → flat word list with audio timestamps.
    Saved:  whisper_cache/ep{N}.wav  (skipped if exists)
            whisper_cache/ep{N}_w0.json  (transcript copy, always refreshed)
            whisper_cache/ep{N}_w1.json  (whisper output, skipped if exists)

Step 2 — Auto-detect offset(s) + align words to transcript
    The transcript (w0) has correct text+speaker+relative-timestamps but was
    produced on the clean audio (intro/ads removed).  The WAV whisper
    timestamps are shifted by an unknown amount δ that may jump at ad breaks.

    We detect δ automatically, assign speaker labels, and flag intro/ad/outro
    words as out_of_transcript=True.

    Saved:  whisper_cache/ep{N}_w2uncut.json

Step 3 — Cut stripped WAV + recalculate timestamps
    Build ep{N}_stripped.wav by concatenating only the keep-regions
    (WAV minus all detected gaps).  Recalculate every in-transcript
    word's timestamp to stripped-WAV time (raw_t − cumulative_gap_before_t).
    Saved:  whisper_cache/ep{N}_stripped.wav  +  whisper_cache/ep{N}_w2.json

Step 4 — Rerun whisper on stripped WAV + compare timestamps
    Transcribe the stripped WAV with whisper-cpp → w3.json.
    Compare w2 vs w3 timestamps word by word.
    Saved:  whisper_cache/ep{N}_w3.json

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
import shutil
import subprocess
import sys
import tempfile

# ── paths ─────────────────────────────────────────────────────────────────────
BASE_DIR       = "/Volumes/eHDD/moshi-rag-data"
DATASETS_DIR   = os.path.join(BASE_DIR, "datasets")
TRANSCRIPT_DIR = os.path.join(DATASETS_DIR, "Gemischtes.Hack.Podcast", "transcripts")
AUDIO_DIR      = os.path.join(DATASETS_DIR, "Gemischtes.Hack.Podcast")
WHISPER_CACHE  = os.path.join(DATASETS_DIR, "whisper_cache")

WHISPER_CLI    = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "whisper-cpp", "build", "bin", "whisper-cli",
)
WHISPER_MODEL  = os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "bin", "models", "ggml-large-v3.bin",
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


def _to_moshi_format(words: list, speaker: str | None = None) -> dict:
    """
    Convert internal word-dict list to moshi-finetune alignment format:
      {"alignments": [[word_text, [start_sec, end_sec]], ...]}
    Speaker is omitted (no third element) when not provided — matches the
    format produced by annotate.py when speaker info is unavailable.
    """
    alignments = []
    for w in words:
        entry = [w["word"], [round(w["start"], 4), round(w["end"], 4)]]
        if speaker is not None:
            entry.append(speaker)
        alignments.append(entry)
    return {"alignments": alignments}


# ══════════════════════════════════════════════════════════════════════════════
# Step 1 — Convert MP3 → 16 kHz mono WAV; copy transcript (w0); transcribe WAV
# ══════════════════════════════════════════════════════════════════════════════

def step1_transcribe(ep_num: int, mp3_path: str, transcript_path: str) -> list:
    """
    a) Convert raw MP3 to 16 kHz mono WAV (skip if WAV already exists).
    b) Copy original transcript to whisper_cache/ep{N}_w0.json (always refreshed).
    c) Run whisper-cpp on the WAV → flat word list (skip if w1 already exists).
    """
    os.makedirs(WHISPER_CACHE, exist_ok=True)
    wav_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}.wav")
    w0_path  = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w0.json")
    w1_path  = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w1.json")

    # ── a) MP3 → 16 kHz mono WAV ─────────────────────────────────────────────
    if os.path.exists(wav_path):
        print(f"  step1: WAV already exists — skipping conversion ({wav_path})",
              flush=True)
    else:
        print(f"  step1: converting MP3 → 16 kHz mono WAV…", flush=True)
        subprocess.run([
            "ffmpeg", "-v", "error", "-i", mp3_path,
            "-ar", "16000", "-ac", "1", wav_path, "-y",
        ], check=True)
        print(f"  step1: WAV saved → {wav_path}", flush=True)

    # ── b) Copy transcript as w0 ──────────────────────────────────────────────
    shutil.copy2(transcript_path, w0_path)
    print(f"  step1: transcript copied → {w0_path}", flush=True)

    # ── c) Transcribe WAV → w1 ────────────────────────────────────────────────
    if os.path.exists(w1_path):
        print(f"  step1: w1 already exists — skipping transcription ({w1_path})",
              flush=True)
        raw = json.load(open(w1_path, encoding="utf-8"))
        return [{"word": e[0], "start": e[1][0], "end": e[1][1], "p": 1.0}
                for e in raw["alignments"]]

    print(f"  step1: running whisper-cpp on WAV…", flush=True)
    words = run_whisper_words(wav_path)

    print(f"  step1: {len(words)} words  "
          f"{words[0]['start']:.2f}s – {words[-1]['end']:.2f}s", flush=True)
    json.dump(_to_moshi_format(words), open(w1_path, "w", encoding="utf-8"),
              ensure_ascii=False)
    print(f"  step1: w1 saved → {w1_path}", flush=True)
    return words


# ══════════════════════════════════════════════════════════════════════════════
# Step 2 — Segment-by-segment alignment
# ══════════════════════════════════════════════════════════════════════════════

def _true_lcs_len(a: list, b: list) -> int:
    """
    Standard O(|a|·|b|) DP longest-common-subsequence count.
    Works regardless of where the match starts in either sequence.
    """
    n, m = len(a), len(b)
    if n == 0 or m == 0:
        return 0
    prev = [0] * (m + 1)
    for ai in range(n):
        curr = [0] * (m + 1)
        for bi in range(m):
            if a[ai] == b[bi]:
                curr[bi + 1] = prev[bi] + 1
            else:
                curr[bi + 1] = max(curr[bi], prev[bi + 1])
        prev = curr
    return prev[m]


def lcs_score(t_norm: list, w_flat: list) -> float:
    """True LCS match count / len(t_norm). Order-preserving, position-independent."""
    n = len(t_norm)
    return _true_lcs_len(t_norm, w_flat) / n if n else 0.0


def _find_lcs_span(t_norm: list, w_flat: list) -> tuple:
    """
    Return (start_idx, end_idx_exclusive) in w_flat that is the tightest span
    enclosing the full LCS match, plus the lcs length.

    Uses the DP table to recover which w_flat positions were matched.
    Returns (None, None, 0) if no match.
    """
    n, m = len(t_norm), len(w_flat)
    if n == 0 or m == 0:
        return None, None, 0

    # Full DP table to trace back
    dp = [[0] * (m + 1) for _ in range(n + 1)]
    for ai in range(n):
        for bi in range(m):
            if t_norm[ai] == w_flat[bi]:
                dp[ai + 1][bi + 1] = dp[ai][bi] + 1
            else:
                dp[ai + 1][bi + 1] = max(dp[ai][bi + 1], dp[ai + 1][bi])

    lcs_len = dp[n][m]
    if lcs_len == 0:
        return None, None, 0

    # Traceback to find first and last matched positions in w_flat
    ai, bi = n, m
    first_b = last_b = -1
    while ai > 0 and bi > 0:
        if t_norm[ai - 1] == w_flat[bi - 1] and dp[ai][bi] == dp[ai - 1][bi - 1] + 1:
            if last_b == -1:
                last_b = bi - 1
            first_b = bi - 1
            ai -= 1
            bi -= 1
        elif dp[ai - 1][bi] >= dp[ai][bi - 1]:
            ai -= 1
        else:
            bi -= 1

    return first_b, last_b + 1, lcs_len  # exclusive end


def search_segment(t_norm: list, whisper_words: list, w_starts: list,
                   cursor: int, center: float, half_win: float,
                   min_score: float) -> tuple:
    """
    Search for t_norm in whisper_words within [center-half_win, center+half_win],
    starting no earlier than cursor.

    Uses true LCS (DP) over the entire window — no sliding sub-window needed.
    The DP finds the best subsequence match regardless of where t_norm starts
    or ends relative to the window boundaries.

    Returns (abs_start_idx, abs_end_idx, score) or (None, None, 0.0).
    abs_end_idx is exclusive (one past last matched whisper word).
    """
    lo = max(cursor, bisect.bisect_left(w_starts,  center - half_win))
    hi = bisect.bisect_right(w_starts, center + half_win)
    if lo >= hi:
        return None, None, 0.0

    flat = []
    for j in range(lo, hi):
        k = norm_words(whisper_words[j]["word"])
        flat.append(k[0] if k else "")

    first_b, last_b, lcs_len = _find_lcs_span(t_norm, flat)
    if first_b is None:
        return None, None, 0.0

    score = lcs_len / len(t_norm)
    if score < min_score:
        return None, None, 0.0

    return lo + first_b, lo + last_b, score


def _bootstrap_offset(whisper_words: list, transcript_segs: list,
                       max_segs: int = 40,
                       max_raw_t: float = 180.0) -> float | None:
    """
    Estimate the initial raw_time − transcript_time offset.

    Strategy (primary): for each of the first max_segs transcript segments
    with ≥4 words, run a true LCS search within the first max_raw_t seconds
    of whisper output.  The first segment that scores above threshold gives
    the offset as:  raw_start_of_match − transcript_seg_start.

    This is better than a 4-gram exact scan because:
    - It tolerates whisper dropping the first 1-2 words of a segment (common
      at intro boundaries) — true LCS scores the remaining words correctly.
    - It scores the best-matching position in the window rather than the
      first exact 4-gram, so it's robust to accidental 4-gram repeats
      elsewhere in the audio.
    - Bounding to max_raw_t ensures we can never get a false offset from a
      repeated phrase deep in the episode.

    Fallback: if LCS finds nothing, try an exact 4-gram scan in the same
    window (handles cases where all early segments are very short and LCS
    thresholds are never met).

    Returns offset (float) or None if nothing found.
    """
    w_starts_all = [w["start"] for w in whisper_words]

    # Window: first max_raw_t seconds only
    hi_idx = bisect.bisect_right(w_starts_all, max_raw_t)
    if hi_idx == 0:
        return None

    w_window = whisper_words[:hi_idx]
    w_starts  = w_starts_all[:hi_idx]

    def _min_score(n: int) -> float:
        if n <= 4:  return 0.75
        if n <= 6:  return 0.67
        if n <= 10: return 0.60
        return 0.55

    # ── Primary: LCS search across all early segments, pick earliest raw hit ─
    # We collect all candidates and return the one with the smallest raw_time,
    # because intro jingles/ads always precede content — the first content
    # segment to appear in whisper output anchors the offset most accurately.
    best_raw_t = float("inf")
    best_offset = None
    mid = w_starts[-1] / 2.0
    for seg in transcript_segs[:max_segs]:
        tn = norm_words(seg["text"])
        n  = len(tn)
        t_start = float(seg["start"])
        if n < 4:
            continue
        abs_s, abs_e, score = search_segment(
            tn, w_window, w_starts, 0, mid, mid + 1.0, _min_score(n))
        if abs_s is not None and w_starts[abs_s] < best_raw_t:
            best_raw_t  = w_starts[abs_s]
            best_offset = w_starts[abs_s] - t_start
    if best_offset is not None:
        return best_offset

    # ── Fallback: exact 4-gram scan ───────────────────────────────────────────
    w_norm = [((norm_words(w["word"]) or [""])[0]) for w in w_window]
    for seg in transcript_segs[:max_segs]:
        tn = norm_words(seg["text"])
        t_start = float(seg["start"])
        seg_dur = max(float(seg["end"]) - t_start, 0.001)
        if len(tn) < 4:
            continue
        for gi in range(len(tn) - 3):
            gram = tn[gi:gi + 4]
            for wi in range(len(w_norm) - 3):
                if w_norm[wi:wi + 4] == gram:
                    word_frac = gi / len(tn)
                    approx_gram_t = t_start + word_frac * seg_dur
                    return w_starts[wi] - approx_gram_t

    return None


def step2_align(ep_num: int, whisper_words: list,
                transcript_segs: list) -> list:
    """
    Segment-by-segment stream alignment with offset tracking.

    For each transcript segment:
    • Expected raw position = seg.start + current_offset
    • Search ±TIGHT_WIN seconds around expected position (covers normal jitter)
    • Cursor starts 2s before end of previous match (boundary overlap)
    • If not found in tight window → recalibrate after N_MISS_AD misses
    • Unmatched segments get timestamps interpolated from surrounding matches.

    Real gaps ≥ MIN_AD_GAP seconds in raw audio = intro/ad/outro.

    Returns the flat aligned word list (same whisper_words with speaker/seg_text
    fields added and out_of_transcript flag).  Gap-cutting and w2 computation
    happen in step3_cut().
    """
    os.makedirs(WHISPER_CACHE, exist_ok=True)
    out_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w2uncut.json")

    TIGHT_WIN    = 12.0  # ±s normal search window
    # After N_MISS_AD consecutive misses we try a wider recalibration around
    # the expected raw position. This handles both gradual offset drift and
    # ad-break jumps without the oscillation of a forward-only wide scan.
    N_MISS_AD    = 8     # consecutive misses before recalibration attempt
    RECAL_WIN    = 60.0  # ±s recalibration window (centered on expected pos)
    MIN_WORDS    = 4     # min words to anchor a segment
    MIN_AD_GAP   = 8.0   # raw gap ≥ this → real ad/intro/outro
    OVERLAP_SEC  = 2.0   # rewind cursor this many seconds for boundary overlap
    DEBUG        = False # set True for per-segment detail

    def min_score(n: int) -> float:
        if n <= 4:  return 0.75
        if n <= 6:  return 0.67
        if n <= 10: return 0.60
        return 0.55

    w_starts = [w["start"] for w in whisper_words]

    # ── Bootstrap offset via 4-gram exact scan ────────────────────────────────
    offset = _bootstrap_offset(whisper_words, transcript_segs)
    if offset is not None:
        print(f"  step2: bootstrap offset={offset:+.2f}s", flush=True)
    else:
        offset = 0.0
        print(f"  step2: bootstrap failed, starting with offset=0.0", flush=True)

    # cursor: whisper-word lower-bound for the tight search.
    # Never goes backwards on normal matches; back-fill uses explicit lo/hi.
    cursor               = 0
    consec_misses        = 0  # consecutive misses on ≥MIN_WORDS segments
    last_matched_abs_end = 0  # abs index past end of last confirmed match

    seg_raw_start = [None] * len(transcript_segs)
    seg_raw_end   = [None] * len(transcript_segs)
    n_matched = n_missed = 0

    print(f"  step2: aligning {len(transcript_segs)} segments…", flush=True)

    # pending_miss: segment indices that failed tight search and are waiting
    # to be back-filled once the next successful match anchors them.
    pending_miss: list = []

    si = 0
    while si < len(transcript_segs):
        seg     = transcript_segs[si]
        t_norm  = norm_words(seg["text"])
        t_start = float(seg["start"])
        n       = len(t_norm)

        if n < MIN_WORDS:
            # Too short to anchor — never goes into pending_miss.
            # Interpolation handles these later.
            n_missed += 1
            si += 1
            continue

        # Expected raw position using current offset
        expected = t_start + offset

        # ── Tight-window search ───────────────────────────────────────────────
        abs_start, abs_end, score = search_segment(
            t_norm, whisper_words, w_starts,
            cursor, expected, TIGHT_WIN, min_score(n))

        # ── Recalibration after consecutive misses ────────────────────────────
        # After N_MISS_AD consecutive misses, the offset has likely drifted or
        # an ad break occurred. Search ±RECAL_WIN around the expected position,
        # using cursor as the floor (never rewinds past consumed audio).
        if abs_start is None and consec_misses >= N_MISS_AD:
            abs_start, abs_end, score = search_segment(
                t_norm, whisper_words, w_starts,
                cursor, expected, RECAL_WIN, min_score(n))
            if abs_start is not None:
                new_offset = whisper_words[abs_start]["start"] - t_start
                jump = new_offset - offset
                print(f"  step2: offset-recal at t={t_start:.0f}s  "
                      f"offset-jump={jump:+.1f}s  raw={whisper_words[abs_start]['start']:.1f}s"
                      f"  (whisper drift, not an ad break)",
                      flush=True)
                offset = new_offset
                new_cursor_t = whisper_words[abs_end - 1]["end"] - OVERLAP_SEC
                cursor = max(cursor, bisect.bisect_left(w_starts, new_cursor_t))
                last_matched_abs_end = max(last_matched_abs_end, abs_end)
                consec_misses = 0

        # ── Queue for back-fill ───────────────────────────────────────────────
        if abs_start is None:
            pending_miss.append(si)
            n_missed += 1
            consec_misses += 1
            if DEBUG:
                print(f"  MISS  si={si:4d} t={t_start:.1f}s "
                      f"T:{seg['text'].strip()[:60]!r}", flush=True)
            si += 1
            continue

        # ── Successful match ──────────────────────────────────────────────────
        # Enforce strict order: abs_start must be >= last_matched_abs_end.
        # The cursor already prevents the search window from rewinding, but
        # a repeated phrase can still be found at an earlier whisper position
        # than where the previous anchor ended (e.g. 'Ich glaube,' appears 50×).
        if abs_start < last_matched_abs_end:
            pending_miss.append(si)
            n_missed += 1
            consec_misses += 1
            si += 1
            continue

        offset = whisper_words[abs_start]["start"] - t_start
        consec_misses = 0

        seg_raw_start[si] = whisper_words[abs_start]["start"]
        seg_raw_end[si]   = whisper_words[abs_end - 1]["end"]
        n_matched += 1
        last_matched_abs_end = abs_end

        # Advance cursor to 2s before end of this match
        new_cursor_t = whisper_words[abs_end - 1]["end"] - OVERLAP_SEC
        cursor = max(cursor, bisect.bisect_left(w_starts, new_cursor_t))

        if DEBUG:
            wt = " ".join(whisper_words[j]["word"]
                          for j in range(abs_start, min(abs_end + 1, len(whisper_words))))
            print(f"  MATCH si={si:4d} t={t_start:.1f}s raw={seg_raw_start[si]:.1f}s "
                  f"score={score:.2f} off={offset:+.2f}s", flush=True)
            print(f"    T: {seg['text'].strip()[:70]!r}", flush=True)
            print(f"    W: {wt[:70]!r}", flush=True)

        # ── Back-fill pending missed segments using this anchor ───────────────
        # Walk REVERSED (closest to anchor → farthest from anchor).
        # upper_bound starts at abs_start and moves LEFT as we assign.
        # Each back-filled segment must end strictly before upper_bound,
        # and upper_bound is updated to pa_s so the next (earlier) segment
        # is constrained to be placed before the one just assigned.
        # This guarantees the back-filled assignments are in transcript order.
        upper_bound = abs_start
        bf_results: dict = {}  # psi → (pa_s, pa_e)

        for psi in reversed(pending_miss):
            pseg   = transcript_segs[psi]
            pt_n   = norm_words(pseg["text"])
            pt_s   = float(pseg["start"])
            pt_len = len(pt_n)
            if pt_len < MIN_WORDS:
                continue
            p_expected = pt_s + offset
            p_lo = bisect.bisect_left(w_starts,  p_expected - TIGHT_WIN)
            p_hi = min(upper_bound,
                       bisect.bisect_right(w_starts, p_expected + TIGHT_WIN))
            if p_lo >= p_hi:
                if DEBUG:
                    print(f"  BACKFILL MISS psi={psi:4d} no window", flush=True)
                continue
            p_center   = (w_starts[p_lo] + w_starts[p_hi - 1]) / 2.0
            p_half_win = (w_starts[p_hi - 1] - w_starts[p_lo]) / 2.0 + 0.01
            pa_s, pa_e, p_score = search_segment(
                pt_n, whisper_words, w_starts,
                p_lo, p_center, p_half_win, min_score(pt_len))
            if pa_s is not None and pa_e <= upper_bound:
                bf_results[psi] = (pa_s, pa_e)
                upper_bound = pa_s  # earlier segs must be placed before this
                if DEBUG:
                    print(f"  BACKFILL MATCH psi={psi:4d} "
                          f"t={pt_s:.1f}s raw={whisper_words[pa_s]['start']:.1f}s "
                          f"score={p_score:.2f}", flush=True)
            else:
                if DEBUG:
                    print(f"  BACKFILL MISS psi={psi:4d} t={pt_s:.1f}s",
                          flush=True)

        for psi, (pa_s, pa_e) in bf_results.items():
            seg_raw_start[psi] = whisper_words[pa_s]["start"]
            seg_raw_end[psi]   = whisper_words[pa_e - 1]["end"]
            n_matched += 1
            n_missed  -= 1
        pending_miss.clear()

        si += 1

    print(f"  step2: matched {n_matched}  missed {n_missed}  "
          f"total {len(transcript_segs)}", flush=True)

    # ── Interpolate timestamps for unmatched segments ─────────────────────────
    # Use offset from surrounding matched segments, linear in transcript time.
    t_starts_f = [float(s["start"]) for s in transcript_segs]
    t_ends_f   = [float(s["end"])   for s in transcript_segs]

    for si in range(len(transcript_segs)):
        if seg_raw_start[si] is not None:
            continue
        prev = next((j for j in range(si - 1, -1, -1) if seg_raw_start[j] is not None), None)
        nxt  = next((j for j in range(si + 1, len(transcript_segs)) if seg_raw_start[j] is not None), None)

        if prev is not None and nxt is not None:
            t0, r0 = t_starts_f[prev], seg_raw_start[prev]
            t1, r1 = t_starts_f[nxt],  seg_raw_start[nxt]
            a = (t_starts_f[si] - t0) / (t1 - t0) if t1 != t0 else 0.0
            seg_raw_start[si] = r0 + a * (r1 - r0)
        elif prev is not None:
            off = seg_raw_start[prev] - t_starts_f[prev]
            seg_raw_start[si] = t_starts_f[si] + off
        elif nxt is not None:
            off = seg_raw_start[nxt] - t_starts_f[nxt]
            seg_raw_start[si] = t_starts_f[si] + off

        if seg_raw_start[si] is not None:
            seg_raw_end[si] = seg_raw_start[si] + (t_ends_f[si] - t_starts_f[si])

    # ── Build segment windows ─────────────────────────────────────────────────
    # Each entry: (raw_start, raw_end, speaker, seg_text, segment_id)
    # segment_id is the index into transcript_segs (0-based), used in step 3
    # to validate that gap boundaries fall between consecutive segments.
    seg_windows = []
    for si, seg in enumerate(transcript_segs):
        rs = seg_raw_start[si] or 0.0
        re = seg_raw_end[si]   or 0.0
        seg_windows.append((rs, re, seg.get("speaker", ""), seg.get("text", "").strip(), si))

    # ── Close cracks between adjacent windows ────────────────────────────────
    # After interpolation, consecutive segment windows may not be contiguous:
    # the end of window[i] < start of window[i+1].  Any whisper word that
    # falls in this crack gets misclassified as out_of_transcript even though
    # it belongs to real content.  Extend each window's end to the next
    # window's start so cracks are closed.  The gap-detection below then only
    # sees spans that are genuinely not covered by any window.
    closed = []
    for i, (rs, re, spk, txt, sid) in enumerate(seg_windows):
        if i + 1 < len(seg_windows):
            re = max(re, seg_windows[i + 1][0])
        closed.append((rs, re, spk, txt, sid))
    seg_windows = closed

    # ── Detect real gaps ≥ MIN_AD_GAP ─────────────────────────────────────────
    gap_regions = []
    if seg_windows[0][0] > MIN_AD_GAP:
        gap_regions.append((0.0, seg_windows[0][0]))
        print(f"  step2: intro  0.0 – {seg_windows[0][0]:.1f}s", flush=True)

    for i in range(len(seg_windows) - 1):
        gs = seg_windows[i][1]
        ge = seg_windows[i + 1][0]
        if ge - gs >= MIN_AD_GAP:
            gap_regions.append((gs, ge))
            print(f"  step2: gap  {gs:.1f}s – {ge:.1f}s  ({ge-gs:.1f}s)", flush=True)

    print(f"  step2: {len(gap_regions)} gap region(s)", flush=True)

    g_starts = [g[0] for g in gap_regions]
    g_ends   = [g[1] for g in gap_regions]

    def in_gap(t: float) -> bool:
        i = bisect.bisect_right(g_starts, t) - 1
        return i >= 0 and t < g_ends[i]

    # ── Assign whisper words ──────────────────────────────────────────────────
    # Every word gets: speaker, seg_text, segment_id, out_of_transcript.
    # OOT words (intro/ad/outro) get speaker=None, seg_text=None, segment_id=None.
    sw_starts = [s[0] for s in seg_windows]
    sw_ends   = [s[1] for s in seg_windows]

    result = []
    n_in = n_out = 0
    for w in whisper_words:
        t = w["start"]
        if in_gap(t):
            result.append({"word": w["word"], "start": w["start"], "end": w["end"],
                           "p": w["p"], "speaker": None, "seg_text": None,
                           "segment_id": None, "out_of_transcript": True})
            n_out += 1
        else:
            idx = bisect.bisect_right(sw_starts, t) - 1
            if idx >= 0 and t <= sw_ends[idx]:
                result.append({"word": w["word"], "start": w["start"], "end": w["end"],
                               "p": w["p"], "speaker": seg_windows[idx][2],
                               "seg_text": seg_windows[idx][3],
                               "segment_id": seg_windows[idx][4],
                               "out_of_transcript": False})
                n_in += 1
            else:
                result.append({"word": w["word"], "start": w["start"], "end": w["end"],
                               "p": w["p"], "speaker": None, "seg_text": None,
                               "segment_id": None, "out_of_transcript": True})
                n_out += 1

    pct = 100.0 * n_in / len(result) if result else 0
    print(f"  step2: {n_in} words assigned ({pct:.1f}%)  "
          f"{n_out} outside (intro/ad/outro)", flush=True)

    json.dump(result, open(out_path, "w", encoding="utf-8"), ensure_ascii=False)
    print(f"  step2: saved → {out_path}", flush=True)

    return result, gap_regions


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
# Step 3 — Replace gaps with silence in WAV; adjust w2uncut timestamps → w2
# ══════════════════════════════════════════════════════════════════════════════

INTRO_OUTRO_FILLER_MS = 500   # ms of silence to keep for intro / outro gaps


def step3_cut(ep_num: int, gap_regions: list, transcript_segs: list,
              w2uncut: list) -> None:
    """
    Exact execution order per spec:

    a) For every gap from step2: classify intro/outro; for mid-episode gaps
       find the boundary words (before/after), validate consecutive segment_ids,
       compute gapcuttime_ms and gapfiller_ms.

    b) Copy ep{N}.wav → ep{N}_stripped.wav.
       For every gap in forward order: replace the gap region with silence of
       gapfiller_ms duration.

    c) Copy w2uncut → w2 (as working data).
       For every gap in forward order: adjust all timestamps that fall after
       the gap's cut point by delta = gapfiller_ms - gapcuttime_ms.

    Saves:
      whisper_cache/ep{N}_stripped.wav
      whisper_cache/ep{N}_w2.json
    """
    wav_path      = os.path.join(WHISPER_CACHE, f"ep{ep_num}.wav")
    stripped_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}_stripped.wav")
    w2_path       = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w2.json")

    # WAV duration — needed to classify outro gaps
    r = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "default=noprint_wrappers=1:nokey=1", wav_path],
        capture_output=True, text=True, check=True,
    )
    wav_duration_s = float(r.stdout.strip())
    INTRO_THRESH = 1.0   # gap starts within 1 s of t=0  → intro
    OUTRO_THRESH = 5.0   # gap ends   within 5 s of end  → outro

    print(f"  step3: {len(gap_regions)} gap(s)  wav_duration={wav_duration_s:.1f}s",
          flush=True)

    # ── a) Classify every gap; compute gapcuttime + gapfiller ────────────────
    # in-transcript words sorted by start time, used to find boundary words
    in_words  = sorted(
        (w for w in w2uncut if not w["out_of_transcript"]),
        key=lambda w: w["start"],
    )
    in_starts = [w["start"] for w in in_words]

    # gaps[i] matches gap_regions[i]; keys: gs, ge, is_intro, is_outro,
    #   gapcuttime_ms, gapfiller_ms, before_word, after_word
    gaps = []

    for gap_idx, (gs, ge) in enumerate(gap_regions):
        is_intro = gs <= INTRO_THRESH
        is_outro = ge >= wav_duration_s - OUTRO_THRESH
        gapcuttime_ms = round((ge - gs) * 1000)

        # ── a1) intro or outro: skip a2–a6, go straight to a7 ────────────────
        if is_intro or is_outro:
            # a7) filler = 500 ms; gapcuttime already set above
            gapfiller_ms = INTRO_OUTRO_FILLER_MS
            label = "intro" if is_intro else "outro"
            print(f"  step3: gap[{gap_idx}] {label}  "
                  f"{gs:.3f}s–{ge:.3f}s  "
                  f"gapcuttime={gapcuttime_ms}ms  gapfiller={gapfiller_ms}ms",
                  flush=True)
            gaps.append({
                "idx": gap_idx, "gs": gs, "ge": ge,
                "is_intro": is_intro, "is_outro": is_outro,
                "gapcuttime_ms": gapcuttime_ms,
                "gapfiller_ms":  gapfiller_ms,
                "before_word": None, "after_word": None,
            })
            continue

        # ── a2) word immediately before the gap ───────────────────────────────
        pos_before = bisect.bisect_left(in_starts, gs) - 1
        if pos_before < 0:
            print(f"  step3 ABORT: gap[{gap_idx}] {gs:.3f}s–{ge:.3f}s  "
                  f"no in-transcript word found before the gap.", flush=True)
            raise SystemExit(1)
        before_gap_w2a = in_words[pos_before]

        # ── a3) word immediately after the gap ────────────────────────────────
        pos_after = bisect.bisect_right(in_starts, ge)
        if pos_after >= len(in_words):
            print(f"  step3 ABORT: gap[{gap_idx}] {gs:.3f}s–{ge:.3f}s  "
                  f"no in-transcript word found after the gap.", flush=True)
            raise SystemExit(1)
        after_gap_w2a = in_words[pos_after]

        sid_before = before_gap_w2a["segment_id"]
        sid_after  = after_gap_w2a["segment_id"]

        # ── a4) same segment_id on both sides → false gap ─────────────────────
        if sid_before == sid_after:
            print(
                f"\n  step3 ABORT: gap[{gap_idx}] is a false gap — "
                f"both boundary words belong to segment_id={sid_before}\n"
                f"  $before_gap{gap_idx}_w2a : {json.dumps(before_gap_w2a)}\n"
                f"  $after_gap{gap_idx}_w2a  : {json.dumps(after_gap_w2a)}\n"
                f"  gap raw timestamps       : {gs:.3f}s – {ge:.3f}s",
                flush=True,
            )
            raise SystemExit(1)

        # ── a5) not consecutive → unexpected gap ──────────────────────────────
        if sid_after != sid_before + 1:
            print(
                f"\n  step3 ABORT: gap[{gap_idx}] spans non-consecutive segments "
                f"(sid_before={sid_before}, sid_after={sid_after})\n"
                f"  $before_gap{gap_idx}_w2a : {json.dumps(before_gap_w2a)}\n"
                f"  $after_gap{gap_idx}_w2a  : {json.dumps(after_gap_w2a)}\n"
                f"  gap raw timestamps       : {gs:.3f}s – {ge:.3f}s",
                flush=True,
            )
            raise SystemExit(1)

        # ── a6) consecutive segments → gapfiller = w0 pause between them ──────
        seg_before_end   = float(transcript_segs[sid_before]["end"])
        seg_after_start  = float(transcript_segs[sid_after]["start"])
        gapfiller_ms     = max(0, round((seg_after_start - seg_before_end) * 1000))

        print(f"  step3: gap[{gap_idx}] mid-episode  "
              f"{gs:.3f}s–{ge:.3f}s  "
              f"gapcuttime={gapcuttime_ms}ms  gapfiller={gapfiller_ms}ms  "
              f"(seg {sid_before} end={seg_before_end:.3f}s → "
              f"seg {sid_after} start={seg_after_start:.3f}s)",
              flush=True)
        print(f"    $before_gap{gap_idx}_w2a: {json.dumps(before_gap_w2a)}",
              flush=True)
        print(f"    $after_gap{gap_idx}_w2a:  {json.dumps(after_gap_w2a)}",
              flush=True)

        gaps.append({
            "idx": gap_idx, "gs": gs, "ge": ge,
            "is_intro": False, "is_outro": False,
            "gapcuttime_ms": gapcuttime_ms,
            "gapfiller_ms":  gapfiller_ms,
            "before_word":   before_gap_w2a,
            "after_word":    after_gap_w2a,
        })

    # ── b1) Copy source WAV → stripped WAV ───────────────────────────────────
    shutil.copy2(wav_path, stripped_path)
    print(f"  step3 b1: copied {wav_path} → {stripped_path}", flush=True)

    # ── b2) For every gap in forward order: replace gap region with silence ───
    # We process gaps left-to-right.  Each ffmpeg pass reads the current
    # stripped file and writes a tmp, then the tmp replaces the stripped file.
    # Because we process forward and rewrite each time, the timestamps inside
    # the file shift after every pass — but since we always re-read the full
    # file for the next gap we always use the *original* gs/ge timestamps
    # referenced in the file as it exists at that point.
    #
    # The key insight: after replacing gap[i], the audio timeline has shifted
    # by delta_i = gapfiller_i - gapcuttime_i for everything after gap[i].
    # So for gap[i+1] the correct position in the current file is:
    #   gs_adj = gs_original + sum(delta_j for j < i+1)
    # We track this cumulative_audio_delta_ms for WAV editing.

    cumulative_audio_delta_ms = 0

    for gap in gaps:
        gapcuttime_ms = gap["gapcuttime_ms"]
        gapfiller_ms  = gap["gapfiller_ms"]
        delta_ms      = gapfiller_ms - gapcuttime_ms

        # Adjusted timestamps in current file
        gs_adj = gap["gs"] + cumulative_audio_delta_ms / 1000.0
        ge_adj = gap["ge"] + cumulative_audio_delta_ms / 1000.0
        sil_s  = gapfiller_ms / 1000.0

        tmp = stripped_path + ".tmp.wav"

        if gap["is_outro"]:
            # Outro: keep content up to gs_adj, then append gapfiller_ms silence
            sil_src = (
                f"anullsrc=r=16000:cl=mono,"
                f"atrim=duration={sil_s:.6f},"
                f"asetpts=PTS-STARTPTS"
            )
            silence_filter = (
                f"[0:a]atrim=0:{gs_adj:.6f},asetpts=PTS-STARTPTS[before];"
                f"{sil_src}[sil];"
                f"[before][sil]concat=n=2:v=0:a=1[out]"
            )
            subprocess.run([
                "ffmpeg", "-v", "error", "-i", stripped_path,
                "-filter_complex", silence_filter,
                "-map", "[out]", tmp, "-y",
            ], check=True)
            os.replace(tmp, stripped_path)
            print(f"  step3 b2a: gap[{gap['idx']}] outro replaced "
                  f"→ kept up to {gs_adj:.3f}s + {sil_s:.3f}s silence",
                  flush=True)

        elif gap["is_intro"]:
            # Intro: starts at t=0 so no [before] segment
            # Build: silence(sil_s) + [ge_adj, end)
            sil_src = (
                f"anullsrc=r=16000:cl=mono,"
                f"atrim=duration={sil_s:.6f},"
                f"asetpts=PTS-STARTPTS"
            )
            silence_filter = (
                f"{sil_src}[sil];"
                f"[0:a]atrim={ge_adj:.6f},asetpts=PTS-STARTPTS[after];"
                f"[sil][after]concat=n=2:v=0:a=1[out]"
            )
            subprocess.run([
                "ffmpeg", "-v", "error", "-i", stripped_path,
                "-filter_complex", silence_filter,
                "-map", "[out]", tmp, "-y",
            ], check=True)
            os.replace(tmp, stripped_path)
            print(f"  step3 b2a: gap[{gap['idx']}] intro replaced "
                  f"[0–{ge_adj:.3f}s] → {sil_s:.3f}s silence",
                  flush=True)

        else:
            # Mid-episode: [0, gs_adj) + silence(sil_s) + [ge_adj, end)
            sil_src = (
                f"anullsrc=r=16000:cl=mono,"
                f"atrim=duration={sil_s:.6f},"
                f"asetpts=PTS-STARTPTS"
            )
            silence_filter = (
                f"[0:a]atrim=0:{gs_adj:.6f},asetpts=PTS-STARTPTS[before];"
                f"{sil_src}[sil];"
                f"[0:a]atrim={ge_adj:.6f},asetpts=PTS-STARTPTS[after];"
                f"[before][sil][after]concat=n=3:v=0:a=1[out]"
            )
            subprocess.run([
                "ffmpeg", "-v", "error", "-i", stripped_path,
                "-filter_complex", silence_filter,
                "-map", "[out]", tmp, "-y",
            ], check=True)
            os.replace(tmp, stripped_path)
            print(f"  step3 b2a: gap[{gap['idx']}] mid-episode replaced "
                  f"[{gs_adj:.3f}–{ge_adj:.3f}s] → {sil_s:.3f}s silence",
                  flush=True)

        cumulative_audio_delta_ms += delta_ms

    print(f"  step3 b: stripped WAV ready → {stripped_path}", flush=True)

    # ── c1) Copy w2uncut → working word list ─────────────────────────────────
    # We work on mutable start/end arrays in milliseconds (integer precision).
    # The source list is w2uncut (already in memory); all_words is a sorted copy.
    all_words = sorted(w2uncut, key=lambda w: w["start"])
    starts_ms = [round(w["start"] * 1000) for w in all_words]
    ends_ms   = [round(w["end"]   * 1000) for w in all_words]

    # ── c2) For every gap in forward order: adjust timestamps ─────────────────
    # cumulative_ts_delta_ms tracks how much the coordinate system has already
    # shifted due to previously processed gaps, so we can find the correct
    # cut-point in the already-adjusted array for each successive gap.
    cumulative_ts_delta_ms = 0

    for gap in gaps:
        gapcuttime_ms = gap["gapcuttime_ms"]
        gapfiller_ms  = gap["gapfiller_ms"]
        delta_ms      = gapfiller_ms - gapcuttime_ms

        # c2b) outro → nothing to do for timestamps (words get dropped at write)
        if gap["is_outro"]:
            continue

        # c2a) intro: cut-point is 0 (all timestamps shift)
        # c2c) mid-episode: cut-point is end of before_word in adjusted coords
        if gap["is_intro"]:
            cut_ms = 0
        else:
            # end of the word immediately before this gap, in adjusted coords
            cut_ms = round(gap["before_word"]["end"] * 1000) + cumulative_ts_delta_ms

        # Shift every timestamp at or after cut_ms
        cut_idx = bisect.bisect_left(starts_ms, cut_ms)
        for i in range(cut_idx, len(starts_ms)):
            starts_ms[i] += delta_ms
            ends_ms[i]   += delta_ms

        cumulative_ts_delta_ms += delta_ms
        print(f"  step3 c2: gap[{gap['idx']}] "
              f"cut_ms={cut_ms}  delta={delta_ms:+d}ms  "
              f"shifted {len(starts_ms) - cut_idx} words",
              flush=True)

    # Outro cutoff in adjusted ms — words at or after this are excluded from w2
    outro_cutoff_ms = None
    for gap in gaps:
        if gap["is_outro"]:
            outro_cutoff_ms = (
                round(gap["gs"] * 1000) + cumulative_ts_delta_ms + gap["gapfiller_ms"]
            )
            break

    # ── Write w2 in moshi-finetune format ─────────────────────────────────────
    alignments = []
    for i, w in enumerate(all_words):
        if w["out_of_transcript"]:
            continue
        s_ms = starts_ms[i]
        e_ms = ends_ms[i]
        if outro_cutoff_ms is not None and s_ms >= outro_cutoff_ms:
            continue
        alignments.append([
            w["word"],
            [round(s_ms / 1000, 4), round(e_ms / 1000, 4)],
            w.get("speaker") or "SPEAKER_MAIN",
        ])

    json.dump({"alignments": alignments},
              open(w2_path, "w", encoding="utf-8"), ensure_ascii=False)
    print(f"  step3 c: w2 ({len(alignments)} words, moshi-finetune format) "
          f"saved → {w2_path}", flush=True)


# ══════════════════════════════════════════════════════════════════════════════
# Step 4 — Verify: transcribe stripped MP3 → w3, compare timestamps with w2
# ══════════════════════════════════════════════════════════════════════════════

def step4_verify(ep_num: int) -> None:
    """
    Transcribe ep{N}_stripped.wav with whisper → w3.json.
    Compare w3 timestamps against w2 timestamps word by word.

    w2 = step3 output: in-transcript words with timestamps in stripped-WAV time.
    w3 = fresh whisper transcription of the same stripped-WAV.

    Since both reference the same audio file starting at t=0, the timestamps
    must agree within whisper's natural jitter (target: ≥98% of words within
    300 ms, mean < 50 ms).  Any systematic deviation points to a bug in the
    gap-cut boundaries or the timestamp-adjustment logic in step 3.
    """
    stripped_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}_stripped.wav")
    w2_path       = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w2.json")
    w3_path       = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w3.json")

    if not os.path.isfile(stripped_path):
        raise FileNotFoundError(f"Stripped WAV not found: {stripped_path}")
    if not os.path.isfile(w2_path):
        raise FileNotFoundError(f"w2 not found: {w2_path}")

    # ── Transcribe stripped WAV → w3 ─────────────────────────────────────────
    print(f"  step4: transcribing stripped WAV → w3…", flush=True)
    w3_words = run_whisper_words(stripped_path)
    json.dump(_to_moshi_format(w3_words), open(w3_path, "w", encoding="utf-8"),
              ensure_ascii=False)
    print(f"  step4: {len(w3_words)} words saved → {w3_path}", flush=True)

    # ── Compare w2 vs w3 timestamps ───────────────────────────────────────────
    # Both w2 and w3 share the same coordinate system (stripped WAV, t=0 =
    # first content word) so no offset bootstrap is needed.
    #
    # Strategy: forward-cursor walk through both streams in order.
    # For each w2 word advance the w3 cursor until we find the same normalised
    # text within a ±LOOK_AHEAD word window.  Only words that both runs agree on
    # (same text, same relative order) get their timestamps compared.
    # This avoids the "common short word matched at wrong instance" problem of
    # the previous bisect approach.
    w2_alignments = json.load(open(w2_path, encoding="utf-8"))["alignments"]

    w3_norm  = [((norm_words(w["word"]) or [""])[0]) for w in w3_words]
    w3_start = [w["start"] for w in w3_words]
    w2_norm  = [((norm_words(e[0]) or [""])[0]) for e in w2_alignments]
    w2_start = [e[1][0] for e in w2_alignments]

    LOOK_AHEAD = 30  # scan up to this many w3 words ahead before giving up
    # TIME_SYNC: re-anchor the w3 cursor by time when it drifts (e.g. whisper
    # hallucinated a run of words w3 has but w2 doesn't).  Both streams are in
    # stripped-WAV time so this should never absorb a large systematic offset —
    # a 3s threshold catches only local hallucination drift, not bugs.
    TIME_SYNC  = 3.0
    deltas: list = []
    large:  list = []
    n_matched = n_skipped = 0
    j = 0   # w3 cursor

    for i, key in enumerate(w2_norm):
        if not key:
            continue
        t2 = w2_start[i]
        # Re-anchor cursor when local drift exceeds TIME_SYNC.
        if j < len(w3_start) and abs(w3_start[j] - t2) > TIME_SYNC:
            j = bisect.bisect_left(w3_start, t2 - 1.0)
        # scan w3 from current cursor up to LOOK_AHEAD words ahead
        found = False
        for k in range(j, min(j + LOOK_AHEAD, len(w3_norm))):
            if w3_norm[k] == key:
                d = abs(w3_start[k] - t2)
                deltas.append(d)
                if d > 0.300:
                    large.append((w2_alignments[i][0], t2, w3_start[k]))
                j = k + 1   # advance cursor past this match
                n_matched += 1
                found = True
                break
        if not found:
            n_skipped += 1

    total    = n_matched + n_skipped
    pct      = 100.0 * n_matched / total if total else 0
    mean_d   = sum(deltas) / len(deltas) if deltas else 0
    max_d    = max(deltas)               if deltas else 0
    median_d = sorted(deltas)[len(deltas) // 2] if deltas else 0
    over_300 = len(large)

    print(f"\n  Step 4 — w2 vs w3 timestamp comparison:", flush=True)
    print(f"  w2 words        : {len(w2_alignments)}", flush=True)
    print(f"  w3 words        : {len(w3_words)}", flush=True)
    print(f"  Order-matched   : {n_matched}/{total}  ({pct:.1f}%)", flush=True)
    print(f"  Mean  |Δ|       : {mean_d*1000:.0f} ms", flush=True)
    print(f"  Median|Δ|       : {median_d*1000:.0f} ms", flush=True)
    print(f"  Max   |Δ|       : {max_d*1000:.0f} ms", flush=True)
    print(f"  Words > 300 ms  : {over_300}", flush=True)

    if over_300:
        print(f"\n  Large deltas (first 10):", flush=True)
        for word, t2, t3 in large[:10]:
            print(f"    {word!r:20s}  w2={t2:.3f}s  w3={t3:.3f}s  "
                  f"Δ={abs(t2-t3)*1000:.0f}ms", flush=True)

    # Quality gate: only words both runs agree on (same text, same order) are
    # compared.  Timestamp delta should be small — large median Δ means the
    # gap-cut boundaries or timestamp math is wrong.
    if median_d > 0.300:
        print(f"\n  ✗ Quality gate FAILED  "
              f"(median Δ={median_d*1000:.0f}ms, need ≤ 300ms)", flush=True)
    else:
        print(f"\n  ✓ Quality gate PASSED  "
              f"(median|Δ|={median_d*1000:.0f}ms)", flush=True)


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


def process_episode(ep_num: int, transcript_path: str, mp3_path: str,
                    skip_step3: bool = False):
    print(f"\n{'═'*60}", flush=True)
    print(f"  Episode {ep_num}  —  {os.path.basename(mp3_path)}", flush=True)
    print(f"{'─'*60}", flush=True)

    # ── Step 1 ────────────────────────────────────────────────────────────────
    print(f"\n  [Step 1] Convert MP3 → WAV; copy transcript (w0); transcribe WAV",
          flush=True)
    w1 = step1_transcribe(ep_num, mp3_path, transcript_path)

    # ── Step 2 ────────────────────────────────────────────────────────────────
    # Load transcript segments from w0 (the copy in the cache folder)
    w0_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w0.json")
    tdata   = json.load(open(w0_path, encoding="utf-8"))
    transcript_segs = [s for s in tdata.get("segments", [])
                       if s.get("text", "").strip()]
    print(f"  transcript (w0): {len(transcript_segs)} segments  "
          f"[{transcript_segs[0]['start']:.1f}s – "
          f"{transcript_segs[-1]['end']:.1f}s]", flush=True)

    w2uncut_path   = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w2uncut.json")
    gap_cache_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}_gaps.json")

    if os.path.exists(w2uncut_path) and os.path.exists(gap_cache_path):
        aligned     = json.load(open(w2uncut_path, encoding="utf-8"))
        gap_regions = [tuple(g) for g in
                       json.load(open(gap_cache_path, encoding="utf-8"))]
        print(f"\n  [Step 2] cached — {len(aligned)} words, "
              f"{len(gap_regions)} gap(s) from cache", flush=True)
    else:
        print(f"\n  [Step 2] Auto-detect offsets + align words", flush=True)
        aligned, gap_regions = step2_align(ep_num, w1, transcript_segs)
        json.dump(list(gap_regions),
                  open(gap_cache_path, "w", encoding="utf-8"))
        print_alignment_report(aligned, transcript_segs)

    # ── Step 3 ────────────────────────────────────────────────────────────────
    print(f"\n  [Step 3] Replace gaps with silence + adjust timestamps",
          flush=True)
    step3_cut(ep_num, gap_regions, transcript_segs, aligned)

    # ── Step 4 ────────────────────────────────────────────────────────────────
    if skip_step3:
        print(f"\n  [Step 4] skipped (--skip-step3)", flush=True)
    else:
        print(f"\n  [Step 4] Verify: transcribe stripped WAV + compare timestamps",
              flush=True)
        step4_verify(ep_num)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--episodes", default=None,
                        help='e.g. "152" or "150-152" or "150,152"')
    parser.add_argument("--skip-step3", action="store_true",
                        help="skip the stripped-audio re-transcription step")
    args = parser.parse_args()

    if not os.path.isfile(WHISPER_CLI):
        sys.exit(f"whisper-cli not found: {WHISPER_CLI}")
    if not os.path.isfile(WHISPER_MODEL):
        sys.exit(f"whisper model not found: {WHISPER_MODEL}")

    ep_filt  = parse_episode_filter(args.episodes) if args.episodes else None
    episodes = find_episodes(ep_filt)

    if not episodes:
        sys.exit("No matching episodes found.")

    steps = "steps 1+2" if args.skip_step3 else "all 3 steps"
    print(f"Found {len(episodes)} episode(s). Running {steps}.")

    errors = 0
    for ep_num, transcript_path, mp3_path in episodes:
        try:
            process_episode(ep_num, transcript_path, mp3_path,
                            skip_step3=args.skip_step3)
        except Exception as exc:
            import traceback
            print(f"\n  ✗ ep{ep_num} FAILED: {exc}", flush=True)
            traceback.print_exc()
            errors += 1

    print(f"\n{'═'*60}")
    print(f"Done.  {errors} error(s).")


if __name__ == "__main__":
    main()
