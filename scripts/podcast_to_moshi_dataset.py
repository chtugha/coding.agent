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

    We detect δ automatically, assign speaker labels, and flag intro/ad/outro
    words as out_of_transcript=True.

    Saved:  aligned_cache/ep{N}_aligned.json

Step 3 — Cut stripped MP3 + recalculate timestamps
    Build ep{N}_stripped.mp3 by concatenating only the keep-regions
    (raw MP3 minus all detected gaps).  Recalculate every in-transcript
    word's timestamp to stripped-MP3 time (raw_t − cumulative_gap_before_t).
    Saved:  ep{N}_stripped.mp3  +  whisper_cache/ep{N}_w2.json

Step 4 — Rerun whisper on stripped audio + compare timestamps
    Transcribe the stripped MP3 with whisper-cpp → w3.json.
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
    json.dump(_to_moshi_format(words), open(out_path, "w", encoding="utf-8"),
              ensure_ascii=False)
    print(f"  step1: saved → {out_path}", flush=True)
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
    os.makedirs(ALIGNED_CACHE, exist_ok=True)
    out_path = os.path.join(ALIGNED_CACHE, f"ep{ep_num}_aligned.json")

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
    cursor        = 0
    consec_misses = 0   # consecutive misses on ≥MIN_WORDS segments

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
                print(f"  step2: recal at t={t_start:.0f}s  "
                      f"Δ={jump:+.1f}s  raw={whisper_words[abs_start]['start']:.1f}s",
                      flush=True)
                offset = new_offset
                new_cursor_t = whisper_words[abs_end - 1]["end"] - OVERLAP_SEC
                cursor = max(cursor, bisect.bisect_left(w_starts, new_cursor_t))
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
        offset = whisper_words[abs_start]["start"] - t_start
        consec_misses = 0

        seg_raw_start[si] = whisper_words[abs_start]["start"]
        seg_raw_end[si]   = whisper_words[abs_end - 1]["end"]
        n_matched += 1

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
        # Walk in reverse order (closest to anchor first).
        # Each pending seg gets a ±TIGHT_WIN window anchored by current offset,
        # capped at abs_start so segment order is preserved.
        for psi in reversed(pending_miss):
            pseg   = transcript_segs[psi]
            pt_n   = norm_words(pseg["text"])
            pt_s   = float(pseg["start"])
            pt_len = len(pt_n)
            if pt_len < MIN_WORDS:
                continue
            p_expected = pt_s + offset
            p_lo = bisect.bisect_left(w_starts,  p_expected - TIGHT_WIN)
            p_hi = min(abs_start,
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
            if pa_s is not None and pa_e <= abs_start:
                seg_raw_start[psi] = whisper_words[pa_s]["start"]
                seg_raw_end[psi]   = whisper_words[pa_e - 1]["end"]
                n_matched += 1
                n_missed  -= 1
                if DEBUG:
                    print(f"  BACKFILL MATCH psi={psi:4d} "
                          f"t={pt_s:.1f}s raw={seg_raw_start[psi]:.1f}s "
                          f"score={p_score:.2f}", flush=True)
            else:
                if DEBUG:
                    print(f"  BACKFILL MISS psi={psi:4d} t={pt_s:.1f}s",
                          flush=True)
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
    seg_windows = []
    for si, seg in enumerate(transcript_segs):
        rs = seg_raw_start[si] or 0.0
        re = seg_raw_end[si]   or 0.0
        seg_windows.append((rs, re, seg.get("speaker", ""), seg.get("text", "").strip()))

    # ── Close cracks between adjacent windows ────────────────────────────────
    # After interpolation, consecutive segment windows may not be contiguous:
    # the end of window[i] < start of window[i+1].  Any whisper word that
    # falls in this crack gets misclassified as out_of_transcript even though
    # it belongs to real content.  Extend each window's end to the next
    # window's start so cracks are closed.  The gap-detection below then only
    # sees spans that are genuinely not covered by any window.
    closed = []
    for i, (rs, re, spk, txt) in enumerate(seg_windows):
        if i + 1 < len(seg_windows):
            re = max(re, seg_windows[i + 1][0])
        closed.append((rs, re, spk, txt))
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
    sw_starts = [s[0] for s in seg_windows]
    sw_ends   = [s[1] for s in seg_windows]

    result = []
    n_in = n_out = 0
    for w in whisper_words:
        t = w["start"]
        if in_gap(t):
            result.append({"word": w["word"], "start": w["start"], "end": w["end"],
                           "p": w["p"], "speaker": None, "seg_text": None,
                           "out_of_transcript": True})
            n_out += 1
        else:
            idx = bisect.bisect_right(sw_starts, t) - 1
            if idx >= 0 and t <= sw_ends[idx]:
                result.append({"word": w["word"], "start": w["start"], "end": w["end"],
                               "p": w["p"], "speaker": seg_windows[idx][2],
                               "seg_text": seg_windows[idx][3],
                               "out_of_transcript": False})
                n_in += 1
            else:
                result.append({"word": w["word"], "start": w["start"], "end": w["end"],
                               "p": w["p"], "speaker": None, "seg_text": None,
                               "out_of_transcript": True})
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
# Shared audio helper
# ══════════════════════════════════════════════════════════════════════════════

def _build_stripped_mp3(mp3_path: str, keep_regions: list, out_mp3: str):
    """Concatenate keep_regions from raw MP3 into a single stripped MP3."""
    inputs, labels = [], []
    for idx, (rs, re_) in enumerate(keep_regions):
        inputs += ["-ss", str(rs), "-t", str(re_ - rs), "-i", mp3_path]
        labels.append(f"[{idx}:a]")
    n   = len(keep_regions)
    flt = "".join(labels) + f"concat=n={n}:v=0:a=1[out]"
    subprocess.run(["ffmpeg", "-v", "error"] + inputs +
                   ["-filter_complex", flt, "-map", "[out]", out_mp3, "-y"],
                   check=True)


# ══════════════════════════════════════════════════════════════════════════════
# Step 3 — Cut stripped MP3 + recalculate timestamps to stripped-audio time
# ══════════════════════════════════════════════════════════════════════════════

def step3_cut(ep_num: int, mp3_path: str, aligned: list) -> None:
    """
    Build ep{N}_stripped.mp3 by cutting all gap regions out of the raw MP3,
    then recalculate every in-transcript word's timestamp to stripped-MP3 time.

    Gap regions are re-derived here from the aligned word list (consecutive
    out_of_transcript spans ≥ MIN_AD_GAP).  This keeps step 2 clean and makes
    step 3 independently reproducible from the aligned.json cache.

    Saves:
      ep{N}_stripped.mp3  — raw MP3 with intro/ads removed
      whisper_cache/ep{N}_w2.json — in-transcript words at stripped-MP3 timestamps
    """
    MIN_AD_GAP = 8.0  # seconds — same threshold as step 2

    # ── Re-derive gap regions from the aligned word list ──────────────────────
    # Collect contiguous runs of out_of_transcript=True words.
    gap_regions: list = []
    run_start: float | None = None
    run_end:   float | None = None
    for w in aligned:
        if w["out_of_transcript"]:
            if run_start is None:
                run_start = w["start"]
            run_end = w["end"]
        else:
            if run_start is not None and (run_end - run_start) >= MIN_AD_GAP:
                gap_regions.append((run_start, run_end))
            run_start = run_end = None
    if run_start is not None and (run_end - run_start) >= MIN_AD_GAP:
        gap_regions.append((run_start, run_end))

    print(f"  step3: {len(gap_regions)} gap region(s) → cutting stripped MP3",
          flush=True)
    for gs, ge in gap_regions:
        print(f"    gap  {gs:.1f}s – {ge:.1f}s  ({ge-gs:.1f}s)", flush=True)

    # ── Build keep-regions (inverse of gaps) ──────────────────────────────────
    raw_end = aligned[-1]["end"]
    keep_regions: list = []
    cursor_t = 0.0
    for gs, ge in gap_regions:
        if gs > cursor_t:
            keep_regions.append((cursor_t, gs))
        cursor_t = ge
    if cursor_t < raw_end:
        keep_regions.append((cursor_t, raw_end))

    total_keep = sum(e - s for s, e in keep_regions)
    print(f"  step3: {len(keep_regions)} keep-region(s)  "
          f"{total_keep:.0f}s total content", flush=True)

    # ── Build stripped MP3 ────────────────────────────────────────────────────
    stripped_path = os.path.join(AUDIO_DIR, f"ep{ep_num}_stripped.mp3")
    _build_stripped_mp3(mp3_path, keep_regions, stripped_path)
    print(f"  step3: stripped MP3 saved → {stripped_path}", flush=True)

    # ── Compute w2: adjust timestamps to stripped-MP3 time ───────────────────
    # Process gaps one at a time in order.  After each removal the coordinate
    # system shifts, so every subsequent gap's boundaries are expressed in the
    # already-adjusted time.  Concretely:
    #
    #   gap_regions are in raw MP3 time.
    #   carried_offset accumulates how much has been subtracted so far.
    #   gap_start_adj = gap_start_raw − carried_offset  (adjusted coordinate)
    #   gap_dur       = gap_end_raw − gap_start_raw     (duration never changes)
    #
    # For each gap we subtract gap_dur from every word whose current (adjusted)
    # start time is ≥ gap_start_adj.  Then carried_offset += gap_dur so the next
    # gap's boundary is computed in the new coordinate system.
    #
    # Working on a mutable list of floats avoids re-reading the aligned list N times.

    # Collect only in-transcript words, sorted by start time.
    # Whisper segment boundaries can produce slightly non-monotone timestamps
    # (adjacent segments overlap by a few ms).  bisect requires a sorted list,
    # so we sort here.  The output w2 is then in strict chronological order.
    w2_proto = sorted(
        (w for w in aligned if not w["out_of_transcript"]),
        key=lambda w: w["start"],
    )
    starts = [w["start"] for w in w2_proto]
    ends   = [w["end"]   for w in w2_proto]

    carried_offset = 0.0
    for gap_num, (gs_raw, ge_raw) in enumerate(gap_regions):
        gap_dur       = ge_raw - gs_raw
        gap_start_adj = gs_raw - carried_offset   # where this gap starts in current coords

        # All words at or after gap_start_adj get shifted left by gap_dur.
        cut_idx = bisect.bisect_left(starts, gap_start_adj)
        for i in range(cut_idx, len(starts)):
            starts[i] -= gap_dur
            ends[i]   -= gap_dur

        carried_offset += gap_dur
        print(f"    gap {gap_num+1}: raw [{gs_raw:.1f}–{ge_raw:.1f}s]  "
              f"adj_start={gap_start_adj:.1f}s  dur={gap_dur:.1f}s  "
              f"→ shifted {len(starts) - cut_idx} words", flush=True)

    # ── Write w2 in moshi-finetune alignment format ───────────────────────────
    # Format matches annotate.py output exactly:
    #   {"alignments": [[word_text, [start_sec, end_sec], speaker], ...]}
    # One file per episode (full stripped audio). Chunking into per-turn clips
    # is Phase 2.
    w2_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w2.json")
    alignments = []
    for i, w in enumerate(w2_proto):
        alignments.append([
            w["word"],
            [round(starts[i], 4), round(ends[i], 4)],
            w["speaker"] or "SPEAKER_MAIN",
        ])
    w2_out = {"alignments": alignments}
    json.dump(w2_out, open(w2_path, "w", encoding="utf-8"), ensure_ascii=False)
    print(f"  step3: w2 ({len(alignments)} words, moshi-finetune format) saved → {w2_path}",
          flush=True)


# ══════════════════════════════════════════════════════════════════════════════
# Step 4 — Verify: transcribe stripped MP3 → w3, compare timestamps with w2
# ══════════════════════════════════════════════════════════════════════════════

def step4_verify(ep_num: int) -> None:
    """
    Transcribe ep{N}_stripped.mp3 with whisper → w3.json.
    Compare w3 timestamps against w2 timestamps word by word.

    w2 = step3 output: in-transcript words with timestamps in stripped-MP3 time.
    w3 = fresh whisper transcription of the same stripped-MP3.

    Since both reference the same audio file starting at t=0, the timestamps
    must agree within whisper's natural jitter (target: ≥98% of words within
    300 ms, mean < 50 ms).  Any systematic deviation points to a bug in the
    gap-cut boundaries or the timestamp-adjustment logic in step 3.
    """
    stripped_path = os.path.join(AUDIO_DIR, f"ep{ep_num}_stripped.mp3")
    w2_path       = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w2.json")
    w3_path       = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w3.json")

    if not os.path.isfile(stripped_path):
        raise FileNotFoundError(f"Stripped MP3 not found: {stripped_path}")
    if not os.path.isfile(w2_path):
        raise FileNotFoundError(f"w2 not found: {w2_path}")

    # ── Transcribe stripped MP3 → w3 ─────────────────────────────────────────
    print(f"  step4: transcribing stripped MP3 → w3…", flush=True)
    w3_words = run_whisper_words(stripped_path)
    json.dump(_to_moshi_format(w3_words), open(w3_path, "w", encoding="utf-8"),
              ensure_ascii=False)
    print(f"  step4: {len(w3_words)} words saved → {w3_path}", flush=True)

    # ── Compare w2 vs w3 ──────────────────────────────────────────────────────
    # Both in moshi-finetune format: {"alignments": [[text, [start, end], ...], ...]}
    w2_alignments = json.load(open(w2_path, encoding="utf-8"))["alignments"]

    w3_norm  = [((norm_words(w["word"]) or [""])[0]) for w in w3_words]
    w3_start = [w["start"] for w in w3_words]

    MATCH_WIN = 5.0   # ±s around w2 time to search for w3 counterpart
    n_match = n_miss = 0
    deltas: list = []
    large:  list = []

    for entry in w2_alignments:
        text, (t2, _t2e) = entry[0], entry[1]
        key = (norm_words(text) or [""])[0]
        if not key:
            continue
        lo = bisect.bisect_left(w3_start,  t2 - MATCH_WIN)
        hi = bisect.bisect_right(w3_start, t2 + MATCH_WIN)
        best_d, best_t3 = float("inf"), None
        for i in range(lo, hi):
            if w3_norm[i] == key:
                d = abs(w3_start[i] - t2)
                if d < best_d:
                    best_d  = d
                    best_t3 = w3_start[i]
        if best_t3 is None:
            n_miss += 1
        else:
            n_match += 1
            deltas.append(best_d)
            if best_d > 0.300:
                large.append((text, t2, best_t3))

    total    = n_match + n_miss
    pct      = 100.0 * n_match / total if total else 0
    mean_d   = sum(deltas) / len(deltas) if deltas else 0
    max_d    = max(deltas)               if deltas else 0
    over_300 = len(large)

    print(f"\n  Step 4 — w2 vs w3 timestamp comparison:", flush=True)
    print(f"  w2 words        : {len(w2_alignments)}", flush=True)
    print(f"  w3 words        : {len(w3_words)}", flush=True)
    print(f"  Matched         : {n_match}/{total}  ({pct:.1f}%)", flush=True)
    print(f"  Mean  |Δ|       : {mean_d*1000:.0f} ms", flush=True)
    print(f"  Max   |Δ|       : {max_d*1000:.0f} ms", flush=True)
    print(f"  Words > 300 ms  : {over_300}", flush=True)

    if over_300:
        print(f"\n  Large deltas (first 10):", flush=True)
        for word, t2, t3 in large[:10]:
            print(f"    {word!r:20s}  w2={t2:.3f}s  w3={t3:.3f}s  "
                  f"Δ={abs(t2-t3)*1000:.0f}ms", flush=True)

    if pct < 98.0 or max_d > 0.300:
        print(f"\n  ✗ Quality gate FAILED  "
              f"(need ≥98% match and max Δ ≤ 300ms)", flush=True)
    else:
        print(f"\n  ✓ Quality gate PASSED", flush=True)


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

    tdata = json.load(open(transcript_path, encoding="utf-8"))
    transcript_segs = [s for s in tdata.get("segments", [])
                       if s.get("text", "").strip()]
    print(f"  transcript: {len(transcript_segs)} segments  "
          f"[{transcript_segs[0]['start']:.1f}s – "
          f"{transcript_segs[-1]['end']:.1f}s]", flush=True)

    # ── Step 1 ────────────────────────────────────────────────────────────────
    w1_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w1.json")
    if os.path.exists(w1_path):
        # w1 on disk is in moshi format; convert back to internal dict list
        raw = json.load(open(w1_path, encoding="utf-8"))
        w1 = [{"word": e[0], "start": e[1][0], "end": e[1][1], "p": 1.0}
              for e in raw["alignments"]]
        print(f"\n  [Step 1] cached — {len(w1)} words from {w1_path}", flush=True)
    else:
        print(f"\n  [Step 1] Transcribe full MP3", flush=True)
        w1 = step1_transcribe(ep_num, mp3_path)

    # ── Step 2 ────────────────────────────────────────────────────────────────
    print(f"\n  [Step 2] Auto-detect offsets + align words", flush=True)
    aligned = step2_align(ep_num, w1, transcript_segs)
    print_alignment_report(aligned, transcript_segs)

    # ── Step 3 ────────────────────────────────────────────────────────────────
    print(f"\n  [Step 3] Cut stripped MP3 + recalculate timestamps", flush=True)
    step3_cut(ep_num, mp3_path, aligned)

    # ── Step 4 ────────────────────────────────────────────────────────────────
    if skip_step3:
        print(f"\n  [Step 4] skipped (--skip-step3)", flush=True)
    else:
        print(f"\n  [Step 4] Verify: transcribe stripped MP3 + compare timestamps",
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
