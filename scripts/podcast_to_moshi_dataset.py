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
    Estimate the initial raw_time − transcript_time offset by finding the
    first 4-word exact run shared between the transcript (first max_segs
    segments) and the whisper output (first max_raw_t seconds only).

    Constraining to the first max_raw_t seconds of whisper prevents a rare
    false match deep in the audio from producing a wildly wrong initial offset.

    Returns offset (float) or None if nothing found.
    """
    w_norm = [((norm_words(w["word"]) or [""])[0]) for w in whisper_words]
    w_starts = [w["start"] for w in whisper_words]

    # Only search within the first max_raw_t seconds of whisper output
    wi_limit = bisect.bisect_right(w_starts, max_raw_t)

    for seg in transcript_segs[:max_segs]:
        tn = norm_words(seg["text"])
        t_start = float(seg["start"])
        seg_dur = max(float(seg["end"]) - t_start, 0.001)
        if len(tn) < 4:
            continue
        for gi in range(len(tn) - 3):
            gram = tn[gi:gi + 4]
            for wi in range(min(wi_limit, len(w_norm) - 3)):
                if w_norm[wi:wi + 4] == gram:
                    word_frac = gi / len(tn)
                    approx_gram_t = t_start + word_frac * seg_dur
                    offset = w_starts[wi] - approx_gram_t
                    return offset
    return None


def step2_align(ep_num: int, whisper_words: list,
                transcript_segs: list) -> list:
    """
    Segment-by-segment stream alignment with offset tracking.

    For each transcript segment:
    • Expected raw position = seg.start + current_offset
    • Search ±TIGHT_WIN seconds around expected position (covers normal jitter)
    • Cursor starts 2s before end of previous match (boundary overlap)
    • If not found in tight window → ad break → search ±WIDE_WIN, update offset
    • Unmatched segments (short / mistranscribed) get timestamps interpolated
      from surrounding matches using the current offset.

    Real gaps ≥ MIN_AD_GAP seconds in raw audio = intro/ad/outro.
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
        print(f"  step2: bootstrap offset={offset:+.2f}s (4-gram scan)", flush=True)
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
    if skip_step3:
        print(f"\n  [Step 3] skipped (--skip-step3)", flush=True)
    else:
        print(f"\n  [Step 3] Rerun whisper on stripped audio + compare", flush=True)
        step3_rerun_compare(ep_num, mp3_path, aligned)


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
