#!/usr/bin/env python3
"""
podcast_to_moshi_dataset.py — Wort-genaue Alignment-Pipeline
═════════════════════════════════════════════════════════════

ARCHITEKTUR (Spezifikation)
────────────────────────────
Step 1 — MP3 → 16 kHz Mono-WAV; Transkript als w0 kopieren; WAV transkribieren (w1)
    a) ffmpeg konvertiert MP3 → 16 kHz Mono-WAV → ep{N}.wav
    b) Original-Transkript wird als w0.json gespeichert (Ground-Truth-Text)
    c) whisper-cpp transkribiert den WAV → flache Wortliste mit Zeitstempeln (w1)
    Gespeichert: whisper_cache/ep{N}.wav  (übersprungen falls vorhanden)
                 whisper_cache/ep{N}_w0.json  (immer aktualisiert)
                 whisper_cache/ep{N}_w1.json  (übersprungen falls vorhanden)

Step 2 — Alignment: w0-Wörter ↔ w1-Zeitstempel (1:1-Zuordnung)
    - w0-Ankerpunkt in w1 finden (Podcast-Intro in w1 überspringen)
    - Segmentweise Selbstprüfung: ∑w1-Dauer für w0-Segment ≈ w0-Segment-Dauer
    - Jedem w0-Wort wird der Zeitstempel des nächstgelegenen w1-Wortes zugewiesen
    - Kein w0-Wort bleibt ohne Zeitstempel
    Gespeichert: whisper_cache/ep{N}_w2aligned.json

Step 3 — Skip-Liste aus geradzahligen Ad-Break-Tags in w0 aufbauen
    - Erkenne Werbepausen-Marker (Ankündigungen der Hosts) in w0
    - Geradzahlige Marker [0,2,4,...] = Ad-Start-Grenzen
    - Ungeradzahlige Marker [1,3,5,...] = Ad-End-Grenzen
    - Nutze w1-Zeitstempel (über w2-Alignment) für exakte WAV-Positionen

Step 4 — Gestrippten WAV erzeugen + Zeitstempel neu berechnen
    - Nicht-Ad-Segmente des WAV verketten → ep{N}_stripped.wav
    - Wort-Zeitstempel neu berechnen: t_stripped = t_raw - kumulativer_offset_bis_t
    Gespeichert: whisper_cache/ep{N}_stripped.wav
                 whisper_cache/ep{N}_w2.json

Step 5 — Double-Mono-Format
    - Stereo-WAV: Kanal 0 = Speaker A, Kanal 1 = Speaker B
    - Wenn Speaker A nicht spricht: Kanal 0 wird stumm geschaltet (Nullen)
    - Wenn Speaker B nicht spricht: Kanal 1 wird stumm geschaltet
    Gespeichert: whisper_cache/ep{N}_double_mono.wav

Verwendung:
    python3 scripts/podcast_to_moshi_dataset.py --episodes 160
    python3 scripts/podcast_to_moshi_dataset.py --episodes 160-163
"""

import argparse
import array as _array
import bisect
import glob
import hashlib
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import traceback
import wave

# ── Pfade ─────────────────────────────────────────────────────────────────────
# BASE_DIR default points to the external drive mount.  Override via --data-dir.
_DEFAULT_BASE_DIR = "/Volumes/eHDD/moshi-rag-data"

BASE_DIR       = _DEFAULT_BASE_DIR
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

# Endian probe — constant for the lifetime of the process.
# WAV PCM is always little-endian on disk; array.array decodes using host byte
# order, so we must byteswap on big-endian hosts before interleaving.
# sys.byteorder == 'little' on all Apple Silicon / x86 machines.
_HOST_IS_BIG_ENDIAN: bool = sys.byteorder != "little"

# Ad-Break-Marker-Muster (Case-insensitive)
# START-Marker: Ankündigung, dass jetzt Werbung kommt
# END-Marker: Ankündigung, dass Werbung beendet ist
AD_START_RE = re.compile(
    r"(kurze?\s+werbung|ganz\s+kurz\s+werbung|jetzt\s+werbung|werbung[,\s]+jetzt)",
    re.IGNORECASE,
)
AD_END_RE = re.compile(
    r"(werbung\s+ende|werbung\s+vorbei|werbung\s+und\s+empfehlung)",
    re.IGNORECASE,
)


# ══════════════════════════════════════════════════════════════════════════════
# Hilfsfunktionen
# ══════════════════════════════════════════════════════════════════════════════

def norm_words(text: str) -> list:
    """Kleinbuchstaben + Satzzeichen entfernen → Wortliste."""
    return [w for w in
            re.sub(r"[^\w\däöüßàáâãåæçèéêëìíîïðñòóôõøùúûüýþÿ-]", " ",
                   text.lower()).split()
            if w]


def _lcs_ratio(a: list, b: list) -> float:
    """
    Longest-Common-Subsequence-Anteil: LCS(a, b) / len(a).
    Gibt 1.0 zurück wenn a leer (leere Sequenz gilt immer als Match).
    O(|a| × |b|) — nur für kurze Wortlisten (~10–30 Wörter) aufrufen.
    """
    if not a:
        return 1.0
    m, n = len(a), len(b)
    prev = [0] * (n + 1)
    for aw in a:
        curr = [0] * (n + 1)
        for bi, bw in enumerate(b):
            curr[bi + 1] = prev[bi] + 1 if aw == bw else max(curr[bi], prev[bi + 1])
        prev = curr
    return prev[n] / m


def _try_match_seg(seg: dict, t_norm: list, w1_pos: int,
                   w1_starts: list, w1_nwords: list) -> tuple | None:
    """
    Versucht ein w0-Segment ab w1-Position w1_pos zu matchen.

    Fenster = seg_dur + 3 s Slack (zeitbasiert via bisect).
    Gibt (end_exclusive_w1_pos, score) zurück, oder None wenn Score unter Schwelle.

    Args:
        seg:       w0-Segment-Dict mit "start", "end" (für seg_dur).
        t_norm:    Vorberechnete normalisierte Wortliste für seg (seg_nwords[si]).
                   Wird vom Aufrufer übergeben, damit norm_words() nicht bei jedem
                   wi-Schritt im engen Suchloop erneut aufgerufen wird.
        w1_pos:    Startindex in w1_starts / w1_nwords.
        w1_starts: Vorberechnete Start-Zeitstempel-Liste je w1-Wort.
        w1_nwords: Vorberechnete normalisierte Einzelwörter je w1-Wort.
                   Jeder Eintrag = (norm_words(w["word"]) or [""])[0].
                   Wird als Slice w1_nwords[w1_pos:win_end] genutzt.
    """
    nw1 = len(w1_starts)
    if not t_norm:
        return w1_pos, 1.0
    seg_dur = float(seg["end"]) - float(seg["start"])
    t0      = w1_starts[w1_pos]
    win_end = bisect.bisect_right(w1_starts, t0 + seg_dur + 3.0)
    win_end = max(w1_pos + 1, min(win_end, nw1))
    w_flat  = w1_nwords[w1_pos:win_end]
    score   = _lcs_ratio(t_norm, w_flat)
    thr     = 0.30 if len(t_norm) > 10 else 0.45 if len(t_norm) > 6 else 0.60
    if score < thr:
        return None
    end_pos = bisect.bisect_right(w1_starts, t0 + seg_dur)
    return max(w1_pos + 1, min(end_pos, nw1)), score


def wav_duration(path: str) -> float:
    """WAV-Datei-Dauer in Sekunden via ffprobe."""
    r = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries", "format=duration",
         "-of", "default=noprint_wrappers=1:nokey=1", path],
        capture_output=True, text=True, check=True,
    )
    raw = r.stdout.strip()
    try:
        return float(raw)
    except ValueError:
        raise RuntimeError(
            f"wav_duration: ffprobe gab keine numerische Ausgabe für {path!r}: {raw!r}\n"
            f"  (Datei beschädigt, leer oder ohne Dauer-Metadaten?)"
        ) from None


def run_whisper_words(wav_path: str) -> list:
    """
    Führt whisper-cpp auf einem 16 kHz Mono-WAV aus.
    Gibt eine flache Liste von {word, start, end, p}-Dicts zurück.
    BPE-Sub-Token werden zu Wörtern zusammengeführt (Leerzeichen-Präfix = Wortgrenze).
    """
    with tempfile.TemporaryDirectory() as td:
        out_base = os.path.join(td, "out")
        r = subprocess.run([
            WHISPER_CLI, "-m", WHISPER_MODEL, "-l", "de", "-f", wav_path,
            "--output-json-full", "--output-file", out_base,
            "--threads", str(WHISPER_THREADS), "--beam-size", "5",
        ], capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError(f"whisper-cpp fehlgeschlagen:\n{r.stderr[:600]}")
        # Read and close the file handle *inside* the TemporaryDirectory block so
        # the descriptor is released before the directory is deleted (P0 fix).
        # json.load() fully materialises `raw` into a Python dict before the
        # with-block exits — `raw` is safe to use after the TD is torn down.
        with open(out_base + ".json", encoding="utf-8") as fh:
            raw = json.load(fh)

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
                                  "p":     round(sum(cur_p) / len(cur_p), 4)})
                cur, cur_s, cur_e, cur_p = t, ts, te, [tp]
            else:
                cur += t
                if cur_s is None:
                    cur_s = ts
                cur_e = te
                cur_p.append(tp)
        if cur.strip() and cur_e is not None and cur_e > (cur_s or 0):
            w = cur.strip()
            words.append({"word": w,
                          "start": round(cur_s, 4),
                          "end":   round(cur_e, 4),
                          "p":     round(sum(cur_p) / len(cur_p), 4)})
    return words


# ══════════════════════════════════════════════════════════════════════════════
# Step 1 — MP3 → WAV; Transkript (w0) kopieren; WAV transkribieren (w1)
# ══════════════════════════════════════════════════════════════════════════════

def step1_transcribe(ep_num: int, mp3_path: str, transcript_path: str) -> tuple[list, str]:
    """
    a) MP3 → 16 kHz Mono-WAV (übersprungen falls vorhanden).
    b) Transkript als w0.json kopieren (immer aktualisiert).
    c) WAV in ~15-Minuten-Chunks transkribieren, zu w1 zusammenfügen.
       Chunks werden an natürlichen w0-Pausen geschnitten.

    Gibt (w1_words, w0_path) zurück.  w0_path wird auch von process_episode
    benötigt — hier als einzige Quelle der Wahrheit zurückgegeben, damit
    Umbenennungen nur an einer Stelle gepflegt werden müssen.
    """
    os.makedirs(WHISPER_CACHE, exist_ok=True)
    wav_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}.wav")
    w0_path  = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w0.json")
    w1_path  = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w1.json")

    # a) MP3 → 16 kHz Mono-WAV
    if os.path.exists(wav_path):
        print(f"  step1: WAV vorhanden — Konvertierung übersprungen ({wav_path})", flush=True)
    else:
        print("  step1: konvertiere MP3 → 16 kHz Mono-WAV…", flush=True)
        subprocess.run([
            "ffmpeg", "-v", "error", "-i", mp3_path,
            "-ar", "16000", "-ac", "1", wav_path, "-y",
        ], check=True)
        print(f"  step1: WAV gespeichert → {wav_path}", flush=True)

    # b) Transkript als w0 kopieren
    shutil.copy2(transcript_path, w0_path)
    print(f"  step1: Transkript kopiert → {w0_path}", flush=True)

    # c) w1 aus Cache laden oder neu transkribieren
    # The cache is invalidated when the transcript changes: the sha256 of the
    # transcript file is stored in the w1 JSON header and compared on load.
    # This prevents stale w1 timestamps from being used with a corrected w0.
    with open(transcript_path, "rb") as _fh:
        transcript_sha256 = hashlib.sha256(_fh.read()).hexdigest()

    if os.path.exists(w1_path):
        print(f"  step1: w1 vorhanden — prüfe Cache-Gültigkeit…", flush=True)
        with open(w1_path, encoding="utf-8") as fh:
            raw = json.load(fh)
        if not isinstance(raw.get("alignments"), list):
            raise ValueError(
                f"step1: w1-Cache hat unerwartetes Format (kein 'alignments'-Array): {w1_path}"
            )
        cached_sha = raw.get("transcript_sha256", "")
        if cached_sha != transcript_sha256:
            print(
                f"  step1: Transkript hat sich geändert (sha256 stimmt nicht überein) — "
                f"w1-Cache wird ungültig gemacht und neu transkribiert.",
                flush=True,
            )
            os.remove(w1_path)
            # Fall through to re-transcription below.
        else:
            print(f"  step1: w1 gültig — Transkription übersprungen ({w1_path})", flush=True)
            # Cache entry format: [word, [start, end]] (old) or [word, [start, end], p] (new).
            # Graceful fallback: old 2-element entries get p=1.0.
            return ([{"word": e[0], "start": e[1][0], "end": e[1][1],
                      "p": e[2] if len(e) > 2 else 1.0}
                     for e in raw["alignments"]], w0_path)

    total_dur = wav_duration(wav_path)

    # Natürliche Schnittzeiten aus w0-Segmenten ermitteln (alle ~15 Minuten)
    with open(w0_path, encoding="utf-8") as fh:
        transcript_segs = json.load(fh)["segments"]
    cuts = _chunk_cut_points(transcript_segs, total_dur)

    boundaries = [0.0] + cuts + [total_dur]
    chunks = [(boundaries[i], boundaries[i + 1]) for i in range(len(boundaries) - 1)]
    print(f"  step1: {len(chunks)} Chunk(s) — Schnitte bei {[f'{c:.1f}s' for c in cuts]}", flush=True)

    all_words = []
    for idx, (t_start, t_end) in enumerate(chunks):
        chunk_wav  = os.path.join(WHISPER_CACHE, f"ep{ep_num}_chunk{idx:02d}.wav")
        chunk_json = os.path.join(WHISPER_CACHE, f"ep{ep_num}_chunk{idx:02d}_w1.json")

        if os.path.exists(chunk_json):
            # JSON-Cache vorhanden: kein ffmpeg nötig — Transkription direkt laden.
            print(f"  step1: Chunk {idx:02d} [{t_start:.1f}s–{t_end:.1f}s] aus Cache geladen", flush=True)
            with open(chunk_json, encoding="utf-8") as fh:
                raw_chunk = json.load(fh)
            if not isinstance(raw_chunk.get("alignments"), list):
                raise ValueError(
                    f"step1: Chunk-Cache hat unerwartetes Format (kein 'alignments'-Array): "
                    f"{chunk_json}"
                )
            # Cache entry format: [word, [start, end]] (old) or [word, [start, end], p] (new).
            # Graceful fallback: old 2-element entries get p=1.0.
            chunk_words = [
                {"word": e[0], "start": e[1][0], "end": e[1][1],
                 "p": e[2] if len(e) > 2 else 1.0}
                for e in raw_chunk["alignments"]
            ]
        else:
            # Chunk-WAV extrahieren und transkribieren.
            # TODO(perf): chunk_wav accumulates on disk (~54 MB per chunk per episode).
            # Kept intentionally for debugging.  Once the pipeline is stable, consider
            # writing chunk WAVs to a TemporaryDirectory and only persisting chunk_json.
            subprocess.run([
                "ffmpeg", "-v", "error",
                "-ss", f"{t_start:.6f}", "-t", f"{t_end - t_start:.6f}",
                "-i", wav_path, "-ar", "16000", "-ac", "1", chunk_wav, "-y",
            ], check=True)
            print(f"  step1: transkribiere Chunk {idx:02d} [{t_start:.1f}s–{t_end:.1f}s]…", flush=True)
            chunk_words = run_whisper_words(chunk_wav)
            with open(chunk_json, "w", encoding="utf-8") as fh:
                json.dump({"alignments": [[w["word"], [w["start"], w["end"]], w["p"]]
                                          for w in chunk_words]},
                          fh, ensure_ascii=False)
            print(f"  step1: Chunk {idx:02d}: {len(chunk_words)} Wörter gespeichert → {chunk_json}", flush=True)

        # ORDERING INVARIANT: mutate timestamps in chunk_words BEFORE extending
        # all_words.  all_words holds the same dict objects by reference, so
        # mutating after extend() would shift already-committed entries a second
        # time.  Do not swap these two operations.
        for w in chunk_words:
            w["start"] = round(w["start"] + t_start, 4)
            w["end"]   = round(w["end"]   + t_start, 4)
        all_words.extend(chunk_words)

    with open(w1_path, "w", encoding="utf-8") as fh:
        json.dump({"transcript_sha256": transcript_sha256,
                   "alignments": [[w["word"], [w["start"], w["end"]], w["p"]]
                                  for w in all_words]},
                  fh, ensure_ascii=False)
    print(f"  step1: {len(all_words)} Wörter gesamt, w1 gespeichert → {w1_path}", flush=True)
    return all_words, w0_path


def _chunk_cut_points(transcript_segs: list, wav_duration_s: float,
                      target_chunk_sec: float = 900.0,
                      min_gap_sec: float = 1.0) -> list:
    """Schnittpunkte für WAV-Chunks anhand von w0-Segmentpausen berechnen."""
    if not transcript_segs:
        return []
    w0_duration = float(transcript_segs[-1]["end"])
    if w0_duration <= 0:
        return []

    gap_mids = []
    for i in range(len(transcript_segs) - 1):
        seg_end   = float(transcript_segs[i]["end"])
        seg_start = float(transcript_segs[i + 1]["start"])
        gap = seg_start - seg_end
        if gap >= min_gap_sec:
            gap_mids.append((seg_end + gap / 2.0, gap))

    if not gap_mids:
        return []

    # Intro-Offset: difference between WAV duration and transcript duration,
    # clamped to [5 s, 120 s] to handle edge cases.
    intro_offset = max(5.0, min(120.0, wav_duration_s - w0_duration))

    cut_points = []
    next_target = target_chunk_sec
    while next_target < w0_duration - target_chunk_sec / 2:
        best = min(gap_mids, key=lambda g: abs(g[0] - next_target))
        # WAV-Schnitt ≈ w0-Zeitpunkt + geschätzter Intro-Offset.
        # Clamped so the cut never exceeds the actual WAV duration (which would
        # produce a negative -t value in the subsequent ffmpeg call).
        wav_cut = min(best[0] + intro_offset, wav_duration_s - 0.001)
        cut_points.append(wav_cut)
        next_target = best[0] + target_chunk_sec

    deduped = sorted(set(round(c, 3) for c in cut_points))
    if len(deduped) < len(cut_points):
        # This happens when there are fewer natural pauses than needed chunks.
        # The episode will be processed in fewer (longer) chunks than intended,
        # which may exceed whisper-cpp's effective context window (~30 min).
        print(
            f"  _chunk_cut_points: WARNUNG — {len(cut_points)} Schnittpunkte berechnet, "
            f"aber nur {len(deduped)} eindeutig (zu wenig natürliche Pausen). "
            f"Episode wird in {len(deduped) + 1} Chunk(s) transkribiert — "
            f"Qualitätsverlust bei sehr langen Chunks möglich.",
            flush=True,
        )
    return deduped


# ══════════════════════════════════════════════════════════════════════════════
# Step 2 — Anchor-Offset bestimmen + w0-Wörter mit linearem Offset versehen
# ══════════════════════════════════════════════════════════════════════════════

def step2_align(ep_num: int, w1_words: list, transcript_segs: list) -> tuple:
    """
    Bestimmt den globalen anchor_offset (w1_time = w0_time + anchor_offset)
    durch Suche des ersten zuverlässig bestätigten Ankerpunkts in w1.

    Danach erhalten alle w0-Wörter ihren Zeitstempel durch lineare Anwendung
    des Offsets auf die w0-Segmentzeitstempel. Kein iteratives Sliding,
    kein Segment-Matching nach dem Anchor.

    Selbstprüfung pro Segment: erwartet w1-Wörter nahe dem linearen Offset-
    Zeitpunkt. Wenn der Drift zu groß ist, wird eine Warnung ausgegeben.
    w0 bleibt immer Ground Truth — der Drift zeigt einen Fehler in w1, nie in w0.

    Gibt (aligned_word_list, anchor_offset) zurück:
      aligned_word_list: [{"w0_word": str, "start": float, "end": float,
                           "speaker": str, "seg_id": int, "is_ad_marker": bool}, ...]
      anchor_offset: w1_time = w0_time + anchor_offset  (fest, linear)
    """
    out_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w2aligned.json")

    w1_starts  = [w["start"] for w in w1_words]
    # Pre-compute normalised single-word tokens for each w1 entry once per
    # episode.  _try_match_seg builds a window slice from this list instead of
    # calling norm_words(w["word"]) on every (wi, window_position) combination.
    w1_nwords  = [(norm_words(w["word"]) or [""])[0] for w in w1_words]
    nw1        = len(w1_starts)

    MIN_SEG_WORDS = 5    # Mindestanzahl Wörter im Ankersegment
    CONFIRM_N     = 6    # Anzahl nachfolgender Segmente zur Bestätigung
    SLIDE_LIMIT_S = 240.0  # w1-Suchfenster für den Anchor (Intro + evtl. Vorsatz)

    # Pre-compute normalised word lists for all transcript segments once.
    # The anchor search inner loop calls len(norm_words(seg["text"])) thousands of
    # times for the same segment; the cache eliminates redundant tokenisation.
    seg_nwords: dict[int, list] = {
        i: norm_words(s.get("text", ""))
        for i, s in enumerate(transcript_segs)
    }

    # ── Anchor-Suche ──────────────────────────────────────────────────────────
    # Kandidaten: erste 30 w0-Segmente mit ≥ MIN_SEG_WORDS Wörtern
    candidates = [i for i in range(min(30, len(transcript_segs)))
                  if len(seg_nwords[i]) >= MIN_SEG_WORDS]
    if not candidates:
        raise RuntimeError("step2: kein w0-Segment mit ≥ MIN_SEG_WORDS Wörtern gefunden")

    limit_wi  = bisect.bisect_right(w1_starts, SLIDE_LIMIT_S)
    anchor_wi = None
    anchor_si = candidates[0]

    # Performance note: the inner loop is O(end_wi × CONFIRM_N × LCS).
    # The normal pass (relaxed=False) is bounded by SLIDE_LIMIT_S (~240 s worth of
    # words, typically < 1000).  The relaxed fallback scans all nw1 words but only
    # triggers when the normal pass fails — which should be rare.  An early-break on
    # the first fully-confirmed hit keeps the average case fast.
    for relaxed in (False, True):
        end_wi = nw1 if relaxed else limit_wi
        for cand_si in candidates:
            seg0 = transcript_segs[cand_si]
            for wi in range(end_wi):
                r0 = _try_match_seg(seg0, seg_nwords[cand_si], wi, w1_starts, w1_nwords)
                if r0 is None:
                    continue
                # Bestätigung: CONFIRM_N nachfolgende lange Segmente müssen matchen
                w_pos = r0[0]
                hits  = 1
                for si2 in range(cand_si + 1, len(transcript_segs)):
                    if hits >= CONFIRM_N:
                        break
                    if len(seg_nwords[si2]) < MIN_SEG_WORDS:
                        continue
                    r2 = _try_match_seg(transcript_segs[si2], seg_nwords[si2], w_pos, w1_starts, w1_nwords)
                    if r2 is None:
                        break
                    w_pos = r2[0]
                    hits += 1
                if hits >= max(2, CONFIRM_N // 2):
                    anchor_wi = wi
                    anchor_si = cand_si
                    break  # confirmed — stop scanning wi positions
            if anchor_wi is not None:
                break
        if anchor_wi is not None:
            break

    if anchor_wi is None:
        raise RuntimeError("step2: FEHLER — kein bestätigter Ankerpunkt in w1 gefunden. "
                           "Bitte w1 prüfen oder SLIDE_LIMIT_S erhöhen.")

    # anchor_offset = w1_time_at_anchor - w0_time_at_anchor
    # Damit gilt global: w1_time ≈ w0_time + anchor_offset
    anchor_w1_time = w1_words[anchor_wi]["start"]
    anchor_w0_time = float(transcript_segs[anchor_si]["start"])
    anchor_offset  = anchor_w1_time - anchor_w0_time

    print(f"  step2: Anchor w0-seg[{anchor_si}] @ w0={anchor_w0_time:.2f}s "
          f"→ w1[{anchor_wi}]={anchor_w1_time:.2f}s  "
          f"anchor_offset={anchor_offset:+.3f}s", flush=True)

    # ── Segment-Dauer-Selbstprüfung (linearer Offset) ─────────────────────────
    # Nur Segmente NACH dem Anchor prüfen — Segmente davor liegen im Intro-Jingle,
    # wo Whisper erwartungsgemäß still ist. Das ist kein Drift.
    DRIFT_WARN_S = 5.0  # Warnschwelle für Timestamp-Drift
    n_warn = 0
    # Use anchor_si:anchor_si+50 (relative), not anchor_si:50 (absolute), so the
    # drift check always examines 50 segments after the anchor regardless of where
    # in the transcript the anchor was found.
    for si, seg in enumerate(transcript_segs[anchor_si:anchor_si + 50], start=anchor_si):
        if len(seg_nwords[si]) < MIN_SEG_WORDS:
            continue
        expected_w1 = float(seg["start"]) + anchor_offset
        # Prüfe ob in ±DRIFT_WARN_S ein w1-Wort liegt
        lo = bisect.bisect_left(w1_starts, expected_w1 - DRIFT_WARN_S)
        hi = bisect.bisect_right(w1_starts, expected_w1 + DRIFT_WARN_S)
        if hi <= lo:
            print(f"  step2: WARNUNG Segment {si} — "
                  f"erwartet w1-Zeit {expected_w1:.2f}s, "
                  f"aber kein w1-Wort in ±{DRIFT_WARN_S:.0f}s-Fenster", flush=True)
            n_warn += 1
    if n_warn:
        print(f"  step2: {n_warn} Drift-Warnungen nach dem Anchor (nächste 50 Segmente)", flush=True)

    # ── Linearer Offset → Zeitstempel für alle w0-Wörter ─────────────────────
    # Jedes w0-Wort erhält: start = w0_seg.start + anchor_offset (linear)
    # Die Dauer des Segments wird gleichmäßig auf die Wörter verteilt.
    # Ad-Marker-Segmente werden markiert (is_ad_marker=True), nicht gefiltert.
    ad_marker_sids = {si for si, seg in enumerate(transcript_segs)
                      if AD_START_RE.search(seg.get("text", ""))
                      or AD_END_RE.search(seg.get("text", ""))}

    result = []
    for si, seg in enumerate(transcript_segs):
        spk     = seg.get("speaker", "")
        seg_txt = seg.get("text", "").strip()
        if not seg_txt:
            continue
        words_in_seg = seg_txt.split()
        if not words_in_seg:
            continue

        is_ad_marker = si in ad_marker_sids
        seg_start_w1 = float(seg["start"]) + anchor_offset
        seg_end_w1   = float(seg["end"])   + anchor_offset
        seg_dur      = seg_end_w1 - seg_start_w1
        if seg_dur <= 0:
            # anchor_offset so negative that this segment predates t=0 in w1.
            # Clamp to avoid negative word timestamps flowing into downstream steps.
            print(f"  step2: WARNUNG Segment {si} — seg_dur={seg_dur:.4f}s ≤ 0 "
                  f"(anchor_offset={anchor_offset:+.3f}s); Zeitstempel auf 0 geklemmt",
                  flush=True)
            seg_start_w1 = max(0.0, seg_start_w1)
            seg_end_w1   = seg_start_w1
            seg_dur      = 0.0
        n_w          = len(words_in_seg)
        dur_per_word = seg_dur / n_w if n_w > 0 and seg_dur > 0 else 0.05

        for wi0, word in enumerate(words_in_seg):
            t_s = seg_start_w1 + wi0 * dur_per_word
            t_e = t_s + dur_per_word
            result.append({
                "w0_word":      word,
                "start":        round(t_s, 4),
                "end":          round(t_e, 4),
                "speaker":      spk,
                "seg_id":       si,
                "is_ad_marker": is_ad_marker,
            })

    print(f"  step2: {len(result)} w0-Wörter mit linearem Offset versehen "
          f"(anchor_offset={anchor_offset:+.3f}s)", flush=True)
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(result, fh, ensure_ascii=False)
    print(f"  step2: gespeichert → {out_path}", flush=True)
    return result, anchor_offset


# ══════════════════════════════════════════════════════════════════════════════
# Step 3 — Ad-Break-Grenzen direkt aus w0-Marker-Timestamps + w1
# ══════════════════════════════════════════════════════════════════════════════

def step3_build_skiplist(transcript_segs: list, w1_words: list,
                         anchor_offset: float) -> list:
    """
    Berechnet die exakten WAV-Schnittgrenzen für jeden Ad-Break.

    KERNPRINZIP:
    - w0-Marker liefern den erwarteten w1-Zeitpunkt: t_exp = w0_time + anchor_offset.
    - raw_ad_start  = Ende des letzten w1-Wortes vor t_exp(START) im Fenster ±NARROW_S.
    - raw_ad_end    = Rückwärts-aligned Timestamp des ersten w0-Worts nach dem END-Marker,
                      bestimmt über einen bestätigten Post-Ad-Anchor (identischer
                      Confirmation-Mechanismus wie der Intro-Anchor in step2).
      Begründung: Nach dem END-Marker folgt im WAV injizierte Werbeaudio (bis zu 90 s),
      die in w0 nicht existiert. Ein einzelnes Segment-Match ist nicht zuverlässig —
      wir brauchen N bestätigende Folge-Segmente, bevor wir den Anchor akzeptieren.
      Dann Rückwärts-Alignment: raw_ad_end = anchor_w1_time - (w0_anchor_time - w0_first_post_ad_time).

    Gibt eine Liste von (raw_ad_start_s, raw_ad_end_s) zurück.
    """
    # NOTE on AD_START_RE / AD_END_RE coverage: these patterns cover the known
    # Gemischtes Hack announcement phrases.  New phrasings (e.g. "Werbepause",
    # "Werbung beginnt") will be silently missed.  Add patterns to AD_START_RE /
    # AD_END_RE at the top of this file when new episode variants are encountered.

    # ── Ad-Marker typisiert erfassen ──────────────────────────────────────────
    typed_markers = []  # (seg_id, "START"|"END")
    for si, seg in enumerate(transcript_segs):
        txt = seg.get("text", "")
        # Use two independent if-checks (not if/elif) so a segment that matches
        # both patterns (e.g. announcing end of one ad and start of another in the
        # same sentence) emits both a START and an END marker.  The downstream
        # pairing loop handles zero-duration spans via the raw_ad_end <= raw_ad_start
        # guard (which emits a WARNUNG and skips the degenerate pair).
        if AD_START_RE.search(txt):
            typed_markers.append((si, "START"))
        if AD_END_RE.search(txt):
            typed_markers.append((si, "END"))

    if not typed_markers:
        print("  step3: keine Ad-Break-Marker in w0 — kein Ad-Break wird herausgeschnitten",
              flush=True)
        return []

    print(f"  step3: {len(typed_markers)} Ad-Marker in w0:", flush=True)
    for si, kind in typed_markers:
        t_w0 = float(transcript_segs[si]["start"])
        print(f"    seg[{si}] {kind}  w0={t_w0:.2f}s → erwartet_w1={t_w0 + anchor_offset:.2f}s  "
              f"{repr(transcript_segs[si].get('text','').strip()[:55])}", flush=True)

    w1_starts  = [w["start"] for w in w1_words]
    w1_ends    = [w["end"]   for w in w1_words]
    # Pre-compute normalised single-word tokens for each w1 entry once.
    # The find_post_ad_anchor inner loop passes this to _try_match_seg as a
    # slice instead of calling norm_words(w["word"]) on every iteration.
    w1_nwords  = [(norm_words(w["word"]) or [""])[0] for w in w1_words]

    # raw_ad_start: Timestamp-Lookup-Fenster um t_exp(START)
    NARROW_S     = 30.0
    # raw_ad_end:  maximale injizierte Werbedauer im WAV (Suchfenster-Obergrenze)
    MAX_AD_WAV_S = 120.0
    # Bestätigung: mindestens diese Anzahl Folgesegmente müssen beim Post-Ad-Anchor matchen
    CONFIRM_N    = 4
    MIN_SEG_WORDS = 5

    # Pre-compute normalised word lists for all transcript segments once.
    # find_post_ad_anchor calls norm_words() inside two nested loops — for every
    # candidate × every w1-position × every confirmation segment.  Without this
    # cache a single episode can trigger ~360 000 redundant norm_words() calls.
    # Same pattern as step2_align's seg_nwords (lines 397–400).
    seg_nwords: dict[int, list] = {
        i: norm_words(s.get("text", ""))
        for i, s in enumerate(transcript_segs)
    }

    def last_w1_end_before(t_exp: float) -> float | None:
        """Ende des letzten w1-Wortes mit Start ≤ t_exp im Fenster ±NARROW_S."""
        lo  = bisect.bisect_left(w1_starts,  t_exp - NARROW_S)
        hi  = bisect.bisect_right(w1_starts, t_exp + NARROW_S)
        idx = bisect.bisect_right(w1_starts, t_exp, lo, hi) - 1
        return w1_ends[idx] if idx >= lo else None

    def find_post_ad_anchor(si_e: int, t_exp_end: float) -> tuple | None:
        """
        Findet den ersten zuverlässig bestätigten Anchor-Punkt in w1 für den
        w0-Inhalt nach dem END-Marker si_e.

        Identischer Confirmation-Mechanismus wie der Intro-Anchor in step2:
        Ein Kandidaten-Segment muss durch CONFIRM_N nachfolgende lange Segmente
        bestätigt werden.

        Suchfenster in w1: [t_exp_end, t_exp_end + MAX_AD_WAV_S].

        Gibt (anchor_si, anchor_wi) zurück:
          anchor_si  = Index in transcript_segs des bestätigten Anchor-Segments
          anchor_wi  = Index in w1_words des bestätigten Anchor-Starts
        Oder None wenn kein bestätigter Anchor gefunden.
        """
        # Kandidaten-Segmente: erste substanzielle Inhalts-Segmente nach si_e
        candidates = []
        for si in range(si_e + 1, len(transcript_segs)):
            seg = transcript_segs[si]
            txt = seg.get("text", "")
            if AD_START_RE.search(txt) or AD_END_RE.search(txt):
                continue
            if len(seg_nwords[si]) >= MIN_SEG_WORDS:
                candidates.append(si)
            if len(candidates) >= 15:  # Erste 15 Kandidaten reichen
                break

        if not candidates:
            return None

        # w1-Suchbereich: [t_exp_end, t_exp_end + MAX_AD_WAV_S]
        lo_wi = bisect.bisect_left(w1_starts,  t_exp_end)
        hi_wi = bisect.bisect_right(w1_starts, t_exp_end + MAX_AD_WAV_S)

        for cand_si in candidates:
            seg0 = transcript_segs[cand_si]
            for wi in range(lo_wi, hi_wi):
                r0 = _try_match_seg(seg0, seg_nwords[cand_si], wi, w1_starts, w1_nwords)
                if r0 is None:
                    continue
                # Bestätigung: CONFIRM_N nachfolgende lange Segmente müssen ebenfalls matchen
                w_pos = r0[0]
                hits  = 1
                for si2 in range(cand_si + 1, len(transcript_segs)):
                    if hits >= CONFIRM_N:
                        break
                    if len(seg_nwords[si2]) < MIN_SEG_WORDS:
                        continue
                    r2 = _try_match_seg(transcript_segs[si2], seg_nwords[si2], w_pos, w1_starts, w1_nwords)
                    if r2 is None:
                        break
                    w_pos = r2[0]
                    hits += 1
                if hits >= max(2, CONFIRM_N // 2):
                    return cand_si, wi  # Bestätigter Anchor — early exit
        return None

    def post_ad_start_time(si_e: int, t_exp_end: float) -> float | None:
        """
        Berechnet raw_ad_end = den w1-Timestamp des ersten w0-Worts nach dem
        END-Marker, via Post-Ad-Anchor + Rückwärts-Alignment.

        Wenn der bestätigte Anchor bei w0-Segment cand_si liegt (statt beim ersten
        Segment si_e+1 nach END), werden die relativen w0-Timestamps genutzt, um
        rückwärts auf das erste w0-Wort nach END zu alignen:
          raw_ad_end = anchor_w1_time - (w0_anchor_seg_start - w0_first_post_ad_seg_start)
        """
        result = find_post_ad_anchor(si_e, t_exp_end)
        if result is None:
            return None

        cand_si, anchor_wi = result
        anchor_w1_time = w1_words[anchor_wi]["start"]

        # Erstes substanzielles Inhalts-Segment nach si_e bestimmen
        first_post_si = None
        for si in range(si_e + 1, len(transcript_segs)):
            seg = transcript_segs[si]
            txt = seg.get("text", "")
            if AD_START_RE.search(txt) or AD_END_RE.search(txt):
                continue
            if seg.get("text", "").strip():
                first_post_si = si
                break
        # Guard: if no non-empty, non-ad-marker segment was found after si_e,
        # or all remaining segments were ad-markers, we cannot compute a valid
        # raw_ad_end.  The old fallback `first_post_si = si_e + 1` was removed
        # because si_e+1 may itself be an ad-marker segment, causing
        # w0_first_post_time to be taken from an ad-marker timestamp and
        # silently producing a wrong raw_ad_end.  Returning None here lets the
        # pairing loop emit a WARNUNG and skip the ad-break cleanly.
        if first_post_si is None or first_post_si >= len(transcript_segs):
            print(f"  step3: WARNUNG — kein Inhalts-Segment nach END-Marker seg[{si_e}] "
                  f"vorhanden; Ad-Break-Ende kann nicht berechnet werden — übersprungen",
                  flush=True)
            return None

        # Rückwärts-Alignment:
        # anchor_w1_time entspricht w0-Zeit von transcript_segs[cand_si]["start"]
        # first_post_si beginnt bei transcript_segs[first_post_si]["start"] in w0
        w0_anchor_time     = float(transcript_segs[cand_si]["start"])
        w0_first_post_time = float(transcript_segs[first_post_si]["start"])
        raw_ad_end = anchor_w1_time - (w0_anchor_time - w0_first_post_time)

        print(f"  step3:   Post-Ad-Anchor: w0-seg[{cand_si}] @ w0={w0_anchor_time:.2f}s "
              f"→ w1[{anchor_wi}]={anchor_w1_time:.2f}s  "
              f"Rückwärts-Alignment: raw_ad_end={raw_ad_end:.3f}s "
              f"(Δ={w0_anchor_time - w0_first_post_time:.2f}s zurück zu seg[{first_post_si}])",
              flush=True)
        return raw_ad_end

    # ── Paare bilden, WAV-Grenzen berechnen ───────────────────────────────────
    skip_list = []
    i = 0
    while i < len(typed_markers):
        si_s, kind_s = typed_markers[i]
        if kind_s != "START":
            i += 1
            continue

        end_idx = next((j for j in range(i + 1, len(typed_markers))
                        if typed_markers[j][1] == "END"), None)
        if end_idx is None:
            print(f"  step3: WARNUNG — START-Marker seg[{si_s}] ohne END-Partner, übersprungen",
                  flush=True)
            i += 1
            continue
        si_e, _ = typed_markers[end_idx]

        # raw_ad_start — letztes w1-Wort-Ende vor t_exp(START)
        t_start_exp  = float(transcript_segs[si_s]["start"]) + anchor_offset
        raw_ad_start = last_w1_end_before(t_start_exp)
        if raw_ad_start is None:
            print(f"  step3: WARNUNG — kein w1-Wort im ±{NARROW_S:.0f}s-Fenster um "
                  f"{t_start_exp:.2f}s (START seg[{si_s}]) — übersprungen", flush=True)
            i = end_idx + 1
            continue

        # raw_ad_end — bestätigter Post-Ad-Anchor + Rückwärts-Alignment
        t_end_exp  = float(transcript_segs[si_e]["end"]) + anchor_offset
        raw_ad_end = post_ad_start_time(si_e, t_end_exp)
        if raw_ad_end is None:
            print(f"  step3: WARNUNG — kein bestätigter Post-Ad-Anchor im Fenster "
                  f"[{t_end_exp:.1f}s, {t_end_exp + MAX_AD_WAV_S:.1f}s] "
                  f"(END seg[{si_e}]) — übersprungen", flush=True)
            i = end_idx + 1
            continue

        if raw_ad_end <= raw_ad_start:
            print(f"  step3: WARNUNG — ungültige Ad-Break-Spanne "
                  f"{raw_ad_start:.3f}s–{raw_ad_end:.3f}s "
                  f"(seg[{si_s}]–seg[{si_e}]) — übersprungen", flush=True)
            i = end_idx + 1
            continue

        drift_start = raw_ad_start - t_start_exp
        drift_end   = raw_ad_end   - t_end_exp
        print(f"  step3: Ad-Break [{len(skip_list)}]  "
              f"seg[{si_s}]–seg[{si_e}]  "
              f"WAV {raw_ad_start:.3f}s–{raw_ad_end:.3f}s  "
              f"({raw_ad_end - raw_ad_start:.1f}s)  "
              f"drift_start={drift_start:+.2f}s  drift_end={drift_end:+.2f}s",
              flush=True)

        skip_list.append((raw_ad_start, raw_ad_end))
        i = end_idx + 1

    return skip_list


# ══════════════════════════════════════════════════════════════════════════════
# Step 4 — Gestrippten WAV erzeugen + Zeitstempel neu berechnen
# ══════════════════════════════════════════════════════════════════════════════

def step4_cut(ep_num: int, skip_list: list, aligned_words: list,
              transcript_segs: list) -> tuple[list, list, list]:
    """
    a) Nicht-Ad-WAV-Segmente verketten → ep{N}_stripped.wav
       Das erste Nicht-Ad-Segment beginnt bei t=0 in der Ausgabe.
    b) Wort-Zeitstempel neu berechnen: t_stripped = t_raw - kumulativer_cut_offset
       Wörter innerhalb von Skip-Regionen werden weggelassen.
    c) Ausgabe-JSON: ein Eintrag pro w0-Wort, Text aus w0, Zeitstempel aus w1
       (angepasst auf gestrippten WAV).

    Gespeichert:
      whisper_cache/ep{N}_stripped.wav
      whisper_cache/ep{N}_w2.json

    Gibt (keep_regions, spk_order, alignments) zurück.
    alignments wird bereits im Speicher gehalten — der Aufrufer kann es direkt
    an step5_double_mono weitergeben ohne w2.json erneut von Disk zu lesen.
    """
    wav_path      = os.path.join(WHISPER_CACHE, f"ep{ep_num}.wav")
    stripped_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}_stripped.wav")
    w2_path       = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w2.json")

    total_dur = wav_duration(wav_path)

    # ── a) Keep-Regionen bestimmen (alles außer Skip + Intro) ─────────────────
    # Intro = Zeit vor dem ersten w0-Wort in w1
    first_word_start = min((w["start"] for w in aligned_words), default=0.0)

    # Alle Skip-Grenzen sortiert
    skips = sorted(skip_list)

    # Keep-Regionen: von Intro-Ende bis erstem Skip, zwischen Skips, nach letztem Skip
    keep_regions = []
    cursor = first_word_start
    for (skip_s, skip_e) in skips:
        if skip_s > cursor:
            keep_regions.append((cursor, skip_s))
        cursor = skip_e
    if cursor < total_dur:
        keep_regions.append((cursor, total_dur))

    print(f"  step4: {len(keep_regions)} Keep-Region(en), "
          f"{len(skips)} Ad-Break(s) übersprungen", flush=True)
    for i, (ks, ke) in enumerate(keep_regions):
        print(f"    keep[{i}]: {ks:.3f}s – {ke:.3f}s  ({ke-ks:.1f}s)", flush=True)

    if not keep_regions:
        raise RuntimeError(
            f"step4: alle Inhalte liegen in Ad-Break-Regionen — "
            f"keine Keep-Regionen übrig. Skip-Liste prüfen: {skip_list}"
        )

    # ── b) WAV aus Keep-Regionen zusammensetzen ────────────────────────────────
    # ffmpeg filter_complex: N keep-Teile konkatenieren
    filter_parts = []
    for i, (ks, ke) in enumerate(keep_regions):
        filter_parts.append(
            f"[0:a]atrim={ks:.6f}:{ke:.6f},asetpts=PTS-STARTPTS[p{i}]"
        )
    concat_inputs = "".join(f"[p{i}]" for i in range(len(keep_regions)))
    filter_str = ";".join(filter_parts) + f";{concat_inputs}concat=n={len(keep_regions)}:v=0:a=1[out]"

    subprocess.run([
        "ffmpeg", "-v", "error", "-i", wav_path,
        "-filter_complex", filter_str,
        "-map", "[out]", stripped_path, "-y",
    ], check=True)

    print(f"  step4: gestrippter WAV gespeichert → {stripped_path}", flush=True)

    # ── c) Zeitstempel-Offset-Anpassung ───────────────────────────────────────
    # Für jedes Wort: t_stripped = t_raw - Σ(Dauer aller Skips + Intro vor t_raw)
    # Ein Wort, das in einer Skip-Region oder im Intro liegt, wird weggelassen.

    def raw_to_stripped(t_raw: float) -> float | None:
        """Wandelt rohen WAV-Zeitstempel in gestrippten-WAV-Zeitstempel um."""
        if t_raw < first_word_start:
            return None  # Im Intro
        offset = first_word_start  # Intro-Dauer wird abgezogen
        for (skip_s, skip_e) in skips:
            if t_raw < skip_s:
                break
            if t_raw < skip_e:
                return None  # Im Ad-Break
            offset += (skip_e - skip_s)
        return t_raw - offset

    # w0-Segmente nach Sprecher-Label für Double-Mono-Zuordnung
    # Speaker-Mapping: ersten auftretenden Sprecher → Kanal 0 (Speaker A)
    spk_order = []
    for w in aligned_words:
        if w["speaker"] not in spk_order:
            spk_order.append(w["speaker"])

    alignments = []
    for w in aligned_words:
        if w["is_ad_marker"]:
            continue  # Ad-Marker nicht in Ausgabe
        t_s = raw_to_stripped(w["start"])
        t_e = raw_to_stripped(w["end"])
        if t_s is None or t_e is None:
            continue  # Im Intro oder Ad-Break
        if t_e <= t_s:
            # Word straddles a cut boundary: start mapped to valid content but
            # end is in (or before) a stripped region.  Skip entirely — emitting
            # a phantom word with no real audio backing would corrupt timestamps
            # in the Moshi training set.
            continue
        spk = w["speaker"] or "SPEAKER_MAIN"
        alignments.append([
            w["w0_word"],
            [round(t_s, 4), round(t_e, 4)],
            spk,
        ])

    with open(w2_path, "w", encoding="utf-8") as fh:
        json.dump({"alignments": alignments}, fh, ensure_ascii=False)
    print(f"  step4: w2 ({len(alignments)} Wörter, w0-Text mit w1-Zeitstempeln) "
          f"gespeichert → {w2_path}", flush=True)

    return keep_regions, spk_order, alignments


# ══════════════════════════════════════════════════════════════════════════════
# Step 5 — Double-Mono-WAV erzeugen
# ══════════════════════════════════════════════════════════════════════════════

def step5_double_mono(ep_num: int, w2_alignments: list, spk_order: list) -> None:
    """
    Stereo-WAV: Kanal 0 = Speaker A (erster Sprecher), Kanal 1 = Speaker B.
    Wenn ein Sprecher NICHT spricht, wird seine Spur stummgeschaltet (Null-Samples).
    Kein Fading, kein Padding — nur exaktes Muting außerhalb der Wort-Zeitstempel.

    Sprecherzuordnung aus w0-Segment-Speaker-Labels (spk_order[0] = A, [1] = B).
    Gespeichert: whisper_cache/ep{N}_double_mono.wav

    Implementierung: bytearray-basiert, keine sample-by-sample-Schleife.
    1. Mono-WAV als rohe int16-Bytes einlesen
    2. Stereo-Array = ch0-Kopie + ch1-Kopie
    3. Mute-Regionen als Sample-Index-Ranges nullsetzen (slice-Assignment)
    """
    stripped_path    = os.path.join(WHISPER_CACHE, f"ep{ep_num}_stripped.wav")
    double_mono_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}_double_mono.wav")

    if not os.path.exists(stripped_path):
        raise FileNotFoundError(f"Gestrippter WAV nicht gefunden: {stripped_path}")

    # ── WAV lesen ─────────────────────────────────────────────────────────────
    with wave.open(stripped_path, "rb") as wf:
        if wf.getsampwidth() != 2:
            raise ValueError(
                f"Erwartet 16-bit PCM, gefunden: {wf.getsampwidth() * 8}-bit"
            )
        if wf.getnchannels() != 1:
            raise ValueError(
                f"Erwartet Mono-Eingang, gefunden: {wf.getnchannels()} Kanäle"
            )
        framerate = wf.getframerate()
        n_frames  = wf.getnframes()
        raw_mono  = wf.readframes(n_frames)  # bytes, 2 Bytes pro Sample

    # Normalise actual sample count from the raw bytes (wave.getnframes() can be
    # wrong for some encoders; using len(raw_mono) is always authoritative).
    actual_frames = len(raw_mono) // 2  # 2 bytes per int16 sample
    n_frames      = actual_frames

    # Zwei unabhängige bytearrays für die beiden Kanäle
    ch0 = bytearray(raw_mono)  # Speaker A
    ch1 = bytearray(raw_mono)  # Speaker B

    # ── Sprecher-Aktivitätszeiträume aus w2 bestimmen ─────────────────────────
    spk_a = spk_order[0] if len(spk_order) > 0 else ""
    spk_b = spk_order[1] if len(spk_order) > 1 else ""

    spk_a_intervals = []
    spk_b_intervals = []
    for idx, entry in enumerate(w2_alignments):
        # Structural check: each entry must be [word, [start, end], speaker].
        # This guards against stale w2.json files written by a different version.
        if (not isinstance(entry, (list, tuple)) or len(entry) < 3
                or not isinstance(entry[1], (list, tuple)) or len(entry[1]) < 2):
            raise ValueError(
                f"step5: w2_alignments[{idx}] hat unerwartetes Format: {entry!r}. "
                f"w2.json könnte von einer anderen Script-Version stammen."
            )
        ts, te, spk = entry[1][0], entry[1][1], entry[2]
        if spk == spk_a:
            spk_a_intervals.append((ts, te))
        elif spk == spk_b:
            spk_b_intervals.append((ts, te))

    # Intervalle zusammenführen (direkt aneinandergrenzende Wörter verbinden)
    def merge_intervals(ivs: list) -> list:
        if not ivs:
            return []
        out = [list(ivs[0])]
        for s, e in sorted(ivs)[1:]:
            if s <= out[-1][1]:
                out[-1][1] = max(out[-1][1], e)
            else:
                out.append([s, e])
        return out

    a_active = merge_intervals(spk_a_intervals)
    b_active = merge_intervals(spk_b_intervals)

    # ── Mute-Regionen berechnen: alles AUSSERHALB aktiver Intervalle ──────────
    # Mute für ch0 = alle Lücken zwischen a_active-Intervallen
    # Mute für ch1 = alle Lücken zwischen b_active-Intervallen

    def mute_outside(ch: bytearray, active: list, n_frames: int, framerate: int) -> None:
        """Setzt alle Samples in ch auf 0, die NICHT in einem aktiven Intervall liegen.

        active muss nach Startzeit sortiert sein (merge_intervals() garantiert das).
        Defensive sort hier stellt sicher, dass unsortierte Listen kein stilles
        Fehlverhalten erzeugen.
        """
        active = sorted(active)  # defensive: garantiert korrekte Lückenberechnung
        # Lücken zwischen aktiven Intervallen ermitteln
        mute_regions = []
        cursor = 0.0
        for s, e in active:
            if s > cursor:
                mute_regions.append((cursor, s))
            cursor = e
        if cursor * framerate < n_frames:
            mute_regions.append((cursor, n_frames / framerate))

        for ms, me in mute_regions:
            # Use round() for the mute-start and math.ceil() for the mute-end
            # so that the full mute region is covered to the nearest sample.
            # int() would truncate toward zero, leaving up to 1 sample of
            # un-muted audio at each boundary — audible as a boundary click.
            i0 = round(ms * framerate) * 2          # Byte-Offset (2 Bytes/Sample)
            i1 = min(math.ceil(me * framerate) * 2, len(ch))
            if i1 > i0:
                ch[i0:i1] = b"\x00" * (i1 - i0)

    mute_outside(ch0, a_active, n_frames, framerate)
    mute_outside(ch1, b_active, n_frames, framerate)

    # ── Stereo-WAV schreiben: Kanäle interleaven ──────────────────────────────
    # Interleaved stereo: [L0, R0, L1, R1, ...]  je 2 Bytes pro Sample.
    #
    # WAV PCM is always stored as little-endian int16.
    #
    # Implementation: use array.array('h') to decode both mono channels as signed
    # int16 values, then interleave them into a new array without creating any
    # intermediate Python-int lists.  For a 3-hour episode at 16 kHz this avoids
    # materialising ~346 million Python integer objects (~9.7 GB).
    #
    # array.array decodes from bytes using the *native* byte order of the host,
    # so we must byte-swap on big-endian hosts (WAV is always little-endian on
    # disk).  sys.byteorder is 'little' on all Apple Silicon / x86 machines.
    arr_a = _array.array("h", ch0)
    arr_b = _array.array("h", ch1)
    if _HOST_IS_BIG_ENDIAN:
        # Host is big-endian: swap to match little-endian WAV on disk
        arr_a.byteswap()
        arr_b.byteswap()

    # Interleave: [a0, b0, a1, b1, ...]
    stereo_arr = _array.array("h", [0]) * (n_frames * 2)
    stereo_arr[0::2] = arr_a
    stereo_arr[1::2] = arr_b

    # Convert back to little-endian bytes for writing
    if _HOST_IS_BIG_ENDIAN:
        stereo_arr.byteswap()
    stereo_bytes = stereo_arr.tobytes()

    with wave.open(double_mono_path, "wb") as wf_out:
        wf_out.setnchannels(2)
        wf_out.setsampwidth(2)
        wf_out.setframerate(framerate)
        wf_out.writeframes(stereo_bytes)

    duration_s = n_frames / framerate
    print(f"  step5: Double-Mono-WAV ({duration_s:.1f}s) gespeichert → {double_mono_path}",
          flush=True)
    print(f"    Speaker A ({spk_a}): {len(a_active)} aktive Intervalle", flush=True)
    print(f"    Speaker B ({spk_b}): {len(b_active)} aktive Intervalle", flush=True)


# ══════════════════════════════════════════════════════════════════════════════
# Episoden-Suche + Orchestrierung
# ══════════════════════════════════════════════════════════════════════════════

def find_episodes(episode_filter=None) -> list:
    trans_files = sorted(glob.glob(os.path.join(TRANSCRIPT_DIR, "episode_*.json")))
    mp3_files   = sorted(glob.glob(os.path.join(AUDIO_DIR, "*.mp3")))
    mp3_by_ep   = {}
    for mp3 in mp3_files:
        # No ^ anchor: filenames that have a leading space, BOM, or an informal
        # prefix (e.g. "GH #160 …") would be silently skipped with ^.  The
        # literal "#" followed by digits is already specific enough.
        m = re.search(r"#(\d+)\s", os.path.basename(mp3))
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
    """
    Parst einen Episodenfilter-String, z.B. "160", "160-163" oder "160,162".
    Löst ValueError mit lesbarer Meldung bei ungültigem Format aus.
    """
    result = set()
    for part in spec.split(","):
        part = part.strip()
        try:
            if "-" in part:
                lo, hi = part.split("-", 1)
                result.update(range(int(lo), int(hi) + 1))
            else:
                result.add(int(part))
        except ValueError:
            raise ValueError(
                f"Ungültiger --episodes-Wert: {part!r}. "
                f"Erwartet wird z.B. '160', '160-163' oder '160,162'."
            ) from None
    return result


def process_episode(ep_num: int, transcript_path: str, mp3_path: str):
    print(f"\n{'═' * 60}", flush=True)
    print(f"  Folge {ep_num}  —  {os.path.basename(mp3_path)}", flush=True)
    print(f"{'─' * 60}", flush=True)

    # ── Step 1 ────────────────────────────────────────────────────────────────
    print(f"\n  [Step 1] MP3 → WAV; Transkript (w0) kopieren; WAV transkribieren (w1)",
          flush=True)
    w1, w0_path = step1_transcribe(ep_num, mp3_path, transcript_path)

    # ── w0 laden ─────────────────────────────────────────────────────────────
    # w0_path is the single source of truth returned by step1_transcribe.
    with open(w0_path, encoding="utf-8") as fh:
        tdata = json.load(fh)
    transcript_segs = [s for s in tdata.get("segments", []) if s.get("text", "").strip()]
    if not transcript_segs:
        raise ValueError(
            f"ep{ep_num}: Transkript enthält keine nicht-leeren Segmente — "
            f"w0.json prüfen: {w0_path}"
        )
    print(f"  Transkript (w0): {len(transcript_segs)} Segmente  "
          f"[{transcript_segs[0]['start']:.1f}s – {transcript_segs[-1]['end']:.1f}s]",
          flush=True)

    # ── Step 2 ────────────────────────────────────────────────────────────────
    w2aligned_path    = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w2aligned.json")
    anchor_cache_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}_anchor_offset.json")

    if os.path.exists(w2aligned_path) and os.path.exists(anchor_cache_path):
        with open(w2aligned_path, encoding="utf-8") as fh:
            aligned = json.load(fh)
        if not isinstance(aligned, list):
            raise ValueError(
                f"step2: w2aligned-Cache hat unerwartetes Format (keine Liste): "
                f"{w2aligned_path}"
            )
        with open(anchor_cache_path, encoding="utf-8") as fh:
            _anchor_data  = json.load(fh)
        anchor_offset = _anchor_data.get("anchor_offset")
        if not isinstance(anchor_offset, (int, float)):
            raise ValueError(
                f"step2: anchor_offset-Cache hat unerwartetes Format "
                f"(kein numerischer 'anchor_offset'-Wert): {anchor_cache_path}"
            )
        anchor_offset = float(anchor_offset)
        print(f"\n  [Step 2] aus Cache geladen — {len(aligned)} w0-Wörter, "
              f"anchor_offset={anchor_offset:.2f}s", flush=True)
    else:
        print(f"\n  [Step 2] w0-Wörter mit w1-Zeitstempeln verknüpfen", flush=True)
        aligned, anchor_offset = step2_align(ep_num, w1, transcript_segs)
        with open(anchor_cache_path, "w", encoding="utf-8") as fh:
            json.dump({"anchor_offset": anchor_offset}, fh)

    # ── Step 3 ────────────────────────────────────────────────────────────────
    print(f"\n  [Step 3] Skip-Liste aus w0-Ad-Markern aufbauen", flush=True)
    skip_list = step3_build_skiplist(transcript_segs, w1, anchor_offset)

    # ── Step 4 ────────────────────────────────────────────────────────────────
    print(f"\n  [Step 4] Gestrippten WAV erzeugen + Zeitstempel neu berechnen", flush=True)
    keep_regions, spk_order, w2_alignments = step4_cut(
        ep_num, skip_list, aligned, transcript_segs
    )

    # ── Step 5 ────────────────────────────────────────────────────────────────
    # w2_alignments is already in memory from step4_cut — no disk re-read needed.
    print(f"\n  [Step 5] Double-Mono-WAV erzeugen", flush=True)
    step5_double_mono(ep_num, w2_alignments, spk_order)

    # ── Ergebnis-Zusammenfassung ──────────────────────────────────────────────
    stripped_path    = os.path.join(WHISPER_CACHE, f"ep{ep_num}_stripped.wav")
    double_mono_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}_double_mono.wav")
    w2_path          = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w2.json")
    dur_stripped     = wav_duration(stripped_path)
    n_words_out      = len(w2_alignments)

    print(f"\n  ✓ Folge {ep_num} abgeschlossen:", flush=True)
    print(f"    Gestrippter WAV:  {stripped_path}  ({dur_stripped:.1f}s)", flush=True)
    print(f"    Double-Mono WAV: {double_mono_path}", flush=True)
    print(f"    w2.json:         {w2_path}  ({n_words_out} Wörter, Text aus w0)", flush=True)


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--episodes", default=None,
                        help='z.B. "160" oder "160-163" oder "160,162"')
    parser.add_argument(
        "--data-dir", default=None, metavar="DIR",
        help=(
            f"Basisverzeichnis für Datensätze (Standard: {_DEFAULT_BASE_DIR}). "
            "Überschreibt BASE_DIR; nützlich wenn das Laufwerk unter einem anderen "
            "Mountpunkt eingehängt ist."
        ),
    )
    args = parser.parse_args()

    # ── Basispfad setzen (überschreibt Modulkonstanten falls --data-dir übergeben) ──
    global BASE_DIR, DATASETS_DIR, TRANSCRIPT_DIR, AUDIO_DIR, WHISPER_CACHE
    if args.data_dir:
        BASE_DIR = os.path.abspath(args.data_dir)
    DATASETS_DIR   = os.path.join(BASE_DIR, "datasets")
    TRANSCRIPT_DIR = os.path.join(DATASETS_DIR, "Gemischtes.Hack.Podcast", "transcripts")
    AUDIO_DIR      = os.path.join(DATASETS_DIR, "Gemischtes.Hack.Podcast")
    WHISPER_CACHE  = os.path.join(DATASETS_DIR, "whisper_cache")

    # ── Laufwerk-/Verzeichnis-Check ────────────────────────────────────────────
    if not os.path.isdir(BASE_DIR):
        sys.exit(
            f"FEHLER: Datenverzeichnis nicht gefunden: {BASE_DIR}\n"
            f"Ist das externe Laufwerk eingehängt? Oder --data-dir angeben."
        )

    if not os.path.isfile(WHISPER_CLI):
        sys.exit(f"whisper-cli nicht gefunden: {WHISPER_CLI}")
    if not os.path.isfile(WHISPER_MODEL):
        sys.exit(f"whisper-Modell nicht gefunden: {WHISPER_MODEL}")

    try:
        ep_filt = parse_episode_filter(args.episodes) if args.episodes else None
    except ValueError as exc:
        sys.exit(f"FEHLER: {exc}")

    episodes = find_episodes(ep_filt)

    if not episodes:
        sys.exit("Keine passenden Folgen gefunden.")

    print(f"{len(episodes)} Folge(n) gefunden.")

    errors = 0
    for ep_num, transcript_path, mp3_path in episodes:
        try:
            process_episode(ep_num, transcript_path, mp3_path)
        except Exception as exc:
            print(f"\n  ✗ ep{ep_num} FEHLER: {exc}", flush=True)
            traceback.print_exc()
            errors += 1

    print(f"\n{'═' * 60}")
    print(f"Fertig.  {errors} Fehler.")


if __name__ == "__main__":
    main()
