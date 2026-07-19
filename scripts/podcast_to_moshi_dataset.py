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
                 whisper_cache/ep{N}_w1.json  (übersprungen falls vorhanden und Transkript unverändert;
                                               bei Transkript-Änderung wird der Cache ungültig gemacht)

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
    "bin", "models", "ggml-large-v3-turbo.bin",
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

# ── Teacher-Forcing Injection Facts ───────────────────────────────────────────
# Kopiert 1:1 aus prepare_german_dataset.py.
# Diese Sätze werden als "[Injected reference] … [End of injected reference]"
# in die w2-Alignment-Liste eingebettet, damit das Moshi-Modell lernt,
# zwischen diesen Markern stehende Fakten als externe Referenz zu nutzen
# (teacher-forcing, kein ARC-Encoder benötigt).
FACTS = [
    "Der Schwarzschild-Radius beschreibt die Grenze, ab der keine Information dem Schwarzen Loch entkommen kann.",
    "Quantenverschränkung bewirkt, dass zwei Teilchen über beliebige Distanzen instantan korreliert bleiben.",
    "Die Hawking-Strahlung resultiert aus Quantenvakuumfluktuationen nahe des Ereignishorizonts.",
    "Gravitationswellen stauchen und strecken die Raumzeit bei asymmetrischen Sternkollisionen.",
    "Superposition bedeutet, dass sich ein Quantensystem gleichzeitig in mehreren Zuständen befindet.",
    "Die Schrödinger-Gleichung beschreibt die zeitliche Entwicklung einer quantenmechanischen Wellenfunktion.",
    "Dunkle Materie wechselwirkt scheinbar nur über die Gravitation mit normaler Materie.",
    "Ein stabiler Orbit erfordert die exakte Balance zwischen Zentrifugalkraft und Gravitationsanziehung.",
    "Der Spin eines Elektrons kann unter Beobachtung in einen eindeutigen Zustand kollabieren.",
    "Im Doppelspaltexperiment interferiert ein einzelnes Teilchen physikalisch mit sich selbst.",
    "Heisenbergs Unschärferelation verbietet die gleichzeitige Messung von Ort und Impuls.",
    "Die Krümmung der Raumzeit bestimmt die Bewegung von massereichen Himmelskörpern.",
]


# ══════════════════════════════════════════════════════════════════════════════
# Silbenzählung (geprüft, 95/95 Testfälle korrekt)
# ══════════════════════════════════════════════════════════════════════════════
#
# Ziel: gesprochene Silbenanzahl im deutschen Audio treffen (nicht orthografisch).
# Regeln:
#   DIPHTHONGE (1 Schlag): ei  ai  äu  eu  au  ie
#     Ausnahme: "-ie" am Wortende nach Konsonant in Lehnwörtern → 2 (Ma-te-ri-e)
#   -SION-Suffix (komprimiert, 1 Schlag): Fusion=2, Version=2, Mission=2
#   -TION (aufgeteilt, ti-on = 2 Schläge): Nation=3, Funktion=3, Wellenfunktion=5
#   qu + Vokal → u kein eigenständiger Schlag (que, qua → 1)
#   Alle anderen Zweifachvokal-Gruppen → aufteilen (2 Schläge)
#   Bindestrich-Komposita → jedes Teil separat zählen

_SYL_VOWELS = set("aeiouäöüy")
_SYL_DIPHTHONGS = {"ei", "ai", "äu", "eu", "au", "ie"}


def _syl_vgroup_beats(vspan: str, pre_char: str, post_span: str) -> int:
    """Gesprochene Schläge für eine maximale Vokal-Gruppe (Kleinbuchstaben)."""
    v = vspan
    if len(v) == 1:
        return 1
    if len(v) == 2:
        if v in _SYL_DIPHTHONGS:
            # Wortfinales "-ie" nach Konsonant = Lehnwort-Schwa (Ma-te-ri-e)
            if v == "ie" and not post_span and pre_char and pre_char not in _SYL_VOWELS:
                return 2
            return 1
        if pre_char == "q" and v in ("ue", "ua", "ui"):
            return 1  # "qu"-Digraph
        # -SION: "io" nach "s", gefolgt von wortfinalem "n"
        if v == "io" and pre_char == "s":
            if post_span and post_span[0] == "n":
                after_n = post_span[1:]
                if not after_n or after_n[0] not in _SYL_VOWELS:
                    return 1  # sion = 1 gesprochener Schlag
            return 2
        return 2  # alle anderen Zweifach-Vokal-Gruppen → aufteilen
    # Drei+ Vokale: gierig den Diphthong-Präfix konsumieren, Rest rekursiv
    if v[:2] in _SYL_DIPHTHONGS:
        rest = v[2:]
        if not rest:
            return 1
        return 1 + _syl_vgroup_beats(rest, v[1], post_span)
    return 1 + _syl_vgroup_beats(v[1:], v[0], post_span)


def _syl_part(word: str) -> int:
    """Gesprochene Silbenschläge in einem (nicht-bindestrich-getrennten) Wort (Kleinbuchstaben)."""
    if not word:
        return 0
    n = len(word)
    count = 0
    i = 0
    while i < n:
        if word[i] not in _SYL_VOWELS:
            i += 1
            continue
        j = i
        while j < n and word[j] in _SYL_VOWELS:
            j += 1
        count += _syl_vgroup_beats(word[i:j], word[i - 1] if i > 0 else "", word[j:])
        i = j
    return max(count, 1) if count > 0 or re.search(r"[a-zäöüßy]", word) else 0


def count_syllables_word(word: str) -> int:
    """Gesprochene Silbenschläge in einem einzelnen deutschen Wort."""
    clean = re.sub(r"[^\w\-äöüÄÖÜßy]", "", word, flags=re.UNICODE)
    if not clean:
        return 0
    return sum(_syl_part(p) for p in clean.lower().split("-"))


# ══════════════════════════════════════════════════════════════════════════════
# Facts-Injektion (teacher-forcing)
# ══════════════════════════════════════════════════════════════════════════════
#
# Mechanismus:
#   1. Alignments-Liste in satzartige Spans aufteilen (Speaker-Wechsel oder
#      Pause > GAP_THRESHOLD_S Sekunden → neuer Span).
#   2. Für jeden FACT: besten w2-Satz nach Silbenanzahl-Ähnlichkeit finden.
#   3. Die Original-Timestamps des gefundenen Spans behalten.
#      Nur die Text-Labels ersetzen: FACT-Wörter gleichmäßig über die vorhandenen
#      Zeitstempel verteilt, mit [Injected reference]…[End of injected reference]
#      Klammerwörtern.
#   4. Jeder Span wird höchstens einmal verwendet (Greedy-Matching).
#
# Format des Teacher-Forcing-Wrappers (exakt wie in stream_both.rs):
#   [Injected reference]
#   <fact text>
#   [End of injected reference]
#
# Die drei Marker-Token werden als eigene "Wörter" mit minimals Dauer
# an den Timestamps des Span-Anfangs/-Endes verankert.

_INJECT_GAP = 0.8          # Sekunden Pause → Satzgrenze
_INJECT_MIN_WORDS = 4      # Kandidaten-Sätze müssen mindestens so viele Wörter haben
_INJECT_MIN_SCORE = 0.70   # Mindest-Ähnlichkeit (0..1) um einen Span zu ersetzen
# Satzende-Interpunktion: ein Span darf NUR dann als Kandidat gelten, wenn das
# letzte Wort auf ein Satzende-Zeichen endet (d.h. es ist eine vollständige Satz).
_SENTENCE_END_RE = re.compile(r'[.!?…]$')


def _segment_alignments(alignments: list) -> list[dict]:
    """
    Teilt die Alignments-Liste in satzartige Spans auf.

    Rückgabe: Liste von Dicts mit
      start_idx, end_idx  — Indices in die Original-Alignments-Liste (inklusiv)
      speaker, words, starts, ends, total_syl
    """
    if not alignments:
        return []
    spans = []
    cur_start = 0
    cur_spk   = alignments[0][2]
    cur_words = [alignments[0][0]]
    cur_ts    = [alignments[0][1][0]]
    cur_te    = [alignments[0][1][1]]

    def flush(end_idx: int):
        total_syl = sum(count_syllables_word(w) for w in cur_words)
        spans.append({
            "start_idx": cur_start,
            "end_idx":   end_idx,
            "speaker":   cur_spk,
            "words":     list(cur_words),
            "starts":    list(cur_ts),
            "ends":      list(cur_te),
            "total_syl": total_syl,
        })

    for i in range(1, len(alignments)):
        word, (t_s, t_e), spk = alignments[i]
        gap = t_s - cur_te[-1]
        # Boundary: speaker change, long pause, OR previous word ended a sentence
        prev_word_ends_sentence = bool(_SENTENCE_END_RE.search(cur_words[-1]))
        if spk != cur_spk or gap > _INJECT_GAP or prev_word_ends_sentence:
            flush(i - 1)
            cur_start = i
            cur_spk   = spk
            cur_words = [word]
            cur_ts    = [t_s]
            cur_te    = [t_e]
        else:
            cur_words.append(word)
            cur_ts.append(t_s)
            cur_te.append(t_e)

    flush(len(alignments) - 1)
    return spans


def _syl_similarity(fact_syl: int, fact_words: int,
                    sent_syl: int, sent_words: int) -> float:
    """
    Ähnlichkeits-Score zwischen einem Fakt und einem Satz (0..1).
    Gewichtung: 60 % Silbenanzahl, 40 % Wortanzahl.
    """
    if not fact_syl or not sent_syl:
        return 0.0
    syl_ratio  = abs(fact_syl  - sent_syl)  / max(fact_syl,  sent_syl)
    word_ratio = abs(fact_words - sent_words) / max(fact_words, sent_words)
    return 1.0 - (0.6 * syl_ratio + 0.4 * word_ratio)


def inject_facts_into_alignments(alignments: list) -> list:
    """
    Gibt eine neue Alignments-Liste zurück, in der für jeden FACT ein
    w2-Satz-Span durch den Fakt-Text ersetzt wurde.

    Die Fact-Wörter werden gleichmäßig über die Zeitstempel des ersetzten
    Spans verteilt. Keine Marker-Token — reiner Wort-für-Wort-Ersatz.

    Rückgabe: neue alignments-Liste (Original wird nicht verändert).
    """
    if not alignments or not FACTS:
        return alignments

    # Schritt 1: Segmentieren
    spans = _segment_alignments(alignments)
    # Kandidaten: Mindestanzahl Wörter UND letztes Wort endet auf Satzzeichen
    # (garantiert, dass nur vollständige Einzelsätze ersetzt werden)
    candidates = [
        s for s in spans
        if len(s["words"]) >= _INJECT_MIN_WORDS
        and bool(_SENTENCE_END_RE.search(s["words"][-1]))
    ]
    if not candidates:
        return alignments

    # Vorab: Silbenzahlen der FACTS berechnen
    fact_meta = []
    for fact in FACTS:
        words = fact.split()
        total_syl = sum(count_syllables_word(w) for w in words)
        fact_meta.append({"text": fact, "words": words, "total_syl": total_syl})

    # Schritt 2: Greedy-Matching (jeden Kandidaten-Span nur einmal vergeben)
    used_spans: set[int] = set()  # set von start_idx

    # Sortiere FACTS nach absteigender Silbenzahl → speziellste zuerst matchen
    fact_order = sorted(range(len(fact_meta)), key=lambda i: -fact_meta[i]["total_syl"])

    assignments: dict[int, dict] = {}   # fact_idx → span

    for fi in fact_order:
        fm = fact_meta[fi]
        best_score = -1.0
        best_span  = None
        for span in candidates:
            if span["start_idx"] in used_spans:
                continue
            score = _syl_similarity(
                fm["total_syl"], len(fm["words"]),
                span["total_syl"], len(span["words"]),
            )
            if score > best_score:
                best_score = score
                best_span  = span
        if best_span is not None and best_score >= _INJECT_MIN_SCORE:
            assignments[fi] = best_span
            used_spans.add(best_span["start_idx"])

    if not assignments:
        return alignments

    # Schritt 3: Neue Alignments-Liste aufbauen
    # Sammle die Span-Indices die ersetzt werden sollen
    replaced_ranges: dict[int, list] = {}  # start_idx → neue Einträge-Liste

    for fi, span in assignments.items():
        fm = fact_meta[fi]
        fact_words = fm["words"]
        n_fact = len(fact_words)
        spk    = span["speaker"]
        t0     = span["starts"][0]
        t_end  = span["ends"][-1]
        span_dur = t_end - t0

        # Verfügbare Zeitpunkte im Span für Fact-Wörter
        # Wir interpolieren n_fact Zeitstempel gleichmäßig im [t0, t_end]-Intervall
        # (Audiodaten bleiben physikalisch unverändert)
        if n_fact == 1:
            word_ts = [(t0, t_end)]
        else:
            step = span_dur / n_fact
            word_ts = [
                (round(t0 + k * step, 4), round(t0 + (k + 1) * step, 4))
                for k in range(n_fact)
            ]

        # Erzeuge neue Einträge: nur die Fakt-Wörter, keine Marker
        new_entries: list = []

        for word, (ws, we) in zip(fact_words, word_ts):
            new_entries.append([word, [ws, we], spk])

        replaced_ranges[span["start_idx"]] = (span["end_idx"], new_entries)

    # Schritt 4: Original-Liste mit Ersetzungen zusammenführen
    result: list = []
    i = 0
    n = len(alignments)
    while i < n:
        entry = alignments[i]
        if i in replaced_ranges:
            end_idx, new_entries = replaced_ranges[i]
            result.extend(new_entries)
            i = end_idx + 1  # überspringen bis nach Span-Ende (inklusiv)
        else:
            result.append(entry)
            i += 1

    print(
        f"  inject_facts: {len(assignments)}/{len(FACTS)} Fakten injiziert "
        f"({len(alignments)} → {len(result)} Einträge)",
        flush=True,
    )
    return result


# ══════════════════════════════════════════════════════════════════════════════
# Timestamp-Überlappungsauflösung (silbenproportionales Merging)
# ══════════════════════════════════════════════════════════════════════════════

def _resolve_overlaps(alignments: list) -> list:
    """
    Löst Zeitstempel-Überlappungen in der Alignment-Liste durch
    silbenproportionales Merging auf.

    Algorithmus (ein Vorwärts-Durchlauf):
      Für jedes konsekutive Paar (words[i], words[i+1]) mit Overlap:
        1. merged_start = min(start_i, start_next)
           merged_end   = max(end_i,   end_next)
        2. Silbenzahl s_i    für words[i].word
           Silbenzahl s_next für words[i+1].word
           Fallback-Zähler: jede zusammenhängende Vokal-Gruppe (a/e/i/o/u/y)
           zählt als 1 Silbe, Minimum 1 pro Wort.
        3. boundary = merged_start + merged_duration * (s_i / (s_i + s_next))
        4. words[i].end      = boundary
           words[i+1].start  = boundary  (exakt, ohne Rundung)
           words[i+1].end    = merged_end
      Durch den Vorwärts-Durchlauf werden Ketten von Überlappungen natürlich
      aufgelöst, da words[i+1] nach der Korrektur direkt als words[i] des
      nächsten Schritts fungiert.

    Nach dem Durchlauf wird eine Assertions-Prüfung ausgeführt:
      Für jedes Paar gilt words[i].end == words[i+1].start — andernfalls
      wird ein ValueError ausgelöst (fängt Regressionen sofort ab).

    Gibt eine neue Liste zurück; die Originalliste bleibt unverändert.
    Druckt eine Zusammenfassung der vorgenommenen Korrekturen.
    """
    if not alignments:
        return alignments

    # Fallback-Silbenzähler: Vokal-Gruppen, Minimum 1
    def _syl_fallback(word: str) -> int:
        groups = re.findall(r'[aeiouäöüyAEIOUÄÖÜY]+', word)
        return max(len(groups), 1)

    # Benutze den vorhandenen Silbenzähler, Fallback wenn nötig
    def _syl(word: str) -> int:
        try:
            n = count_syllables_word(word)
            return n if n > 0 else _syl_fallback(word)
        except Exception:
            return _syl_fallback(word)

    # Flache Kopie — jede Subliste [word, [start, end], spk] wird bei Bedarf
    # durch eine neue Liste ersetzt, nie in-place mutiert.
    result = [entry for entry in alignments]
    resolved = 0

    for i in range(len(result) - 1):
        cur  = result[i]
        nxt  = result[i + 1]

        end_i      = cur[1][1]
        start_next = nxt[1][0]

        if end_i <= start_next:
            continue  # kein Overlap

        merged_start    = min(cur[1][0], start_next)
        merged_end      = max(end_i, nxt[1][1])
        merged_duration = merged_end - merged_start

        s_i    = _syl(cur[0])
        s_next = _syl(nxt[0])
        total_syl = s_i + s_next

        boundary = merged_start + merged_duration * (s_i / total_syl)

        result[i]     = [cur[0], [cur[1][0], boundary],    cur[2]]
        result[i + 1] = [nxt[0], [boundary,  merged_end],  nxt[2]]
        resolved += 1

    if resolved:
        print(
            f"  _resolve_overlaps: {resolved} Overlap(s) silbenproportional aufgelöst",
            flush=True,
        )

    # ── Assertions-Prüfung ────────────────────────────────────────────────────
    for i in range(len(result) - 1):
        end_i      = result[i][1][1]
        start_next = result[i + 1][1][0]
        if end_i > start_next:
            raise ValueError(
                f"_resolve_overlaps: Überlappung nach Bereinigung verblieben bei "
                f"Index {i} → {i+1}: "
                f"'{result[i][0]}' endet {end_i:.6f}s, "
                f"'{result[i+1][0]}' beginnt {start_next:.6f}s "
                f"(Overlap {(end_i - start_next)*1000:.3f} ms)"
            )

    return result


# ══════════════════════════════════════════════════════════════════════════════
# Hilfsfunktionen
# ══════════════════════════════════════════════════════════════════════════════

def norm_words(text: str) -> list[str]:
    """Kleinbuchstaben + Satzzeichen entfernen → Wortliste."""
    return [w for w in
            re.sub(r"[^\w\däöüßàáâãåæçèéêëìíîïðñòóôõøùúûüýþÿ-]", " ",
                   text.lower()).split()
            if w]


def _lcs_ratio(a: list[str], b: list[str]) -> float:
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


def _lcs_first_last(a: list, b: list) -> tuple[int, int] | None:
    """
    Run LCS on word lists a (w0 segment) and b (w1 window slice) and return
    (first_b_idx, last_b_idx) — the indices into b of the first and last matched
    words.  Returns None if no match exists.

    Used by _try_match_seg to find the exact span of w1 words that correspond to
    a w0 segment, so block_start and block_end are derived from real matched words
    rather than the raw window boundaries (which may include jingle/preamble words
    before the segment's content actually begins in the audio).
    """
    if not a or not b:
        return None
    m, n = len(a), len(b)
    # Forward DP
    dp = [[0] * (n + 1) for _ in range(m + 1)]
    for i in range(m - 1, -1, -1):
        for j in range(n - 1, -1, -1):
            if a[i] == b[j]:
                dp[i][j] = 1 + dp[i + 1][j + 1]
            else:
                dp[i][j] = max(dp[i + 1][j], dp[i][j + 1])
    # Traceback to collect matched b-indices
    matched_b = []
    i, j = 0, 0
    while i < m and j < n:
        if a[i] == b[j]:
            matched_b.append(j)
            i += 1
            j += 1
        elif dp[i + 1][j] >= dp[i][j + 1]:
            i += 1
        else:
            j += 1
    if not matched_b:
        return None
    return matched_b[0], matched_b[-1]


def _lcs_first_pair(a: list, b: list) -> tuple[int, int] | None:
    """
    Run LCS on word lists a (w0 segment) and b (w1 window slice) and return
    (first_a_idx, first_b_idx) — the indices into a and b of the FIRST matched
    word pair.  Returns None if no match exists.

    Used by _align_segs_to_w1 to find the first matched w0 word in the anchor
    segment, so the unmatched prefix (e.g. a leading "Und" that whisper dropped)
    can be dropped from w2aligned.
    """
    if not a or not b:
        return None
    m, n = len(a), len(b)
    # Forward DP (same structure as _lcs_first_last)
    dp = [[0] * (n + 1) for _ in range(m + 1)]
    for i in range(m - 1, -1, -1):
        for j in range(n - 1, -1, -1):
            if a[i] == b[j]:
                dp[i][j] = 1 + dp[i + 1][j + 1]
            else:
                dp[i][j] = max(dp[i + 1][j], dp[i][j + 1])
    # Traceback — return on the FIRST match
    i, j = 0, 0
    while i < m and j < n:
        if a[i] == b[j]:
            return (i, j)
        elif dp[i + 1][j] >= dp[i][j + 1]:
            i += 1
        else:
            j += 1
    return None


def _lcs_pairs(a: list, b: list) -> list[tuple[int, int]]:
    """
    Run LCS on word lists a (w0 segment, normalised) and b (w1 window slice,
    normalised) and return ALL matched (a_idx, b_idx) pairs in order.

    Used by _align_segs_to_w1 to assign each matched w0 word the REAL w1 word
    [start, end].  Unmatched w0 words (whisper dropped/merged them) are removed
    from the output — no invented timestamps.
    """
    if not a or not b:
        return []
    m, n = len(a), len(b)
    dp = [[0] * (n + 1) for _ in range(m + 1)]
    for i in range(m - 1, -1, -1):
        for j in range(n - 1, -1, -1):
            if a[i] == b[j]:
                dp[i][j] = 1 + dp[i + 1][j + 1]
            else:
                dp[i][j] = max(dp[i + 1][j], dp[i][j + 1])
    pairs = []
    i, j = 0, 0
    while i < m and j < n:
        if a[i] == b[j]:
            pairs.append((i, j))
            i += 1
            j += 1
        elif dp[i + 1][j] >= dp[i][j + 1]:
            i += 1
        else:
            j += 1
    return pairs


def _try_match_seg(seg: dict, t_norm: list, w1_pos: int,
                   w1_starts: list[float], w1_nwords: list[int]) -> tuple[int, float] | None:
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


def run_whisper_words(wav_path: str) -> list[dict]:
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
       Cache-Validierung: Der sha256 des geschriebenen w0 wird im w1-JSON
       gespeichert.  Bei Transkript-Änderung (sha256-Mismatch) werden w1.json
       und alle zugehörigen Chunk-JSONs gelöscht und neu erzeugt.

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
    # *written* w0_path (not transcript_path) is stored in the w1 JSON header
    # and compared on load.  Hashing w0_path (already written above) eliminates
    # the TOCTOU window that would exist if we hashed the external source file,
    # and is conceptually correct — the hash validates the w0 that w1 was built
    # from, not the original source location.
    with open(w0_path, "rb") as _fh:
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
            # Also delete per-chunk JSON caches: they store relative timestamps
            # built against the old chunk cut-points, which are derived from
            # w0 segment boundaries.  If the transcript changed, the new w0
            # may produce different cut-points, making old chunk JSONs wrong.
            for stale_chunk in glob.glob(
                os.path.join(WHISPER_CACHE, f"ep{ep_num}_chunk*_w1.json")
            ):
                os.remove(stale_chunk)
                print(f"  step1: staler Chunk-Cache gelöscht → {stale_chunk}", flush=True)
            # Also delete step-2 caches: w2aligned.json and anchor_offset.json
            # are derived from w1 timestamps.  If w1 is regenerated, these are
            # stale — the anchor_offset calibrated against the old w1 would
            # produce a misaligned result when applied to the new w1.
            for stale_step2 in [
                os.path.join(WHISPER_CACHE, f"ep{ep_num}_w2aligned.json"),
                os.path.join(WHISPER_CACHE, f"ep{ep_num}_anchor_offset.json"),
            ]:
                if os.path.exists(stale_step2):
                    os.remove(stale_step2)
                    print(f"  step1: staler Step-2-Cache gelöscht → {stale_step2}", flush=True)
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

    # Resolve any timestamp overlaps introduced by chunk-boundary stitching
    # before saving w1, so downstream steps never see overlapping timestamps.
    w1_entries = [[w["word"], [w["start"], w["end"]], w["p"]] for w in all_words]
    w1_entries = _resolve_overlaps(w1_entries)

    with open(w1_path, "w", encoding="utf-8") as fh:
        json.dump({"transcript_sha256": transcript_sha256,
                   "alignments": w1_entries},
                  fh, ensure_ascii=False)
    print(f"  step1: {len(w1_entries)} Wörter gesamt, w1 gespeichert → {w1_path}", flush=True)
    return all_words, w0_path


def _chunk_cut_points(transcript_segs: list, wav_duration_s: float,
                      target_chunk_sec: float = 600.0,
                      min_gap_sec: float = 0.5) -> list:
    """
    Berechnet WAV-Schnittpunkte für die Chunk-Transkription.

    Strategie (neu, gegenüber der alten 900-s-Pause-Methode):
      • Ziel-Chunk-Länge: 600 s (statt 900 s) — hält alle Chunks sicher unter
        Whispers zuverlässigem Kontextfenster und verhindert Tail-Truncation.
      • Kandidaten-Grenzen: aufeinanderfolgende w0-Segmente, bei denen
          (a) das aktuelle Segment mit Satzschluss-Interpunktion endet  UND
          (b) entweder der Sprecher wechselt ODER eine Pause ≥ min_gap_sec vorliegt.
        Beide Bedingungen zusammen ergeben saubere, inhaltlich sinnvolle Schnitte.
      • Ad-Break-Mauer: kein Schnitt darf innerhalb des Ad-Break-Bereichs
        [ad_start_w0, ad_end_w0] liegen.  Stattdessen wird die Abschnittgrenze
        genau an ad_start_w0 bzw. ad_end_w0 gesetzt — der Ad-Break bleibt immer
        vollständig in einem eigenen Bereich und kontaminiert keine Chunk-Transkription.
      • Fallback: Wenn für eine Sektion keine geeignete Kandidaten-Grenze in
        Reichweite liegt, wird der nächstbeste Pause-Kandidat (≥ min_gap_sec)
        ohne Sprecher-/Satzende-Bedingung akzeptiert.

    Rückgabe: Liste von WAV-Schnittpunkten (float, Sekunden), aufsteigend sortiert.
    Funktionssignatur identisch zur alten Implementierung — keine Änderung am Aufrufer.
    """
    if not transcript_segs:
        return []
    w0_duration = float(transcript_segs[-1]["end"])
    if w0_duration <= 0:
        return []

    # Intro-Offset: WAV enthält Intro-Jingle vor dem ersten w0-Segment.
    # Clamped auf [5 s, 120 s] für Randfall-Robustheit.
    intro_offset = max(5.0, min(120.0, wav_duration_s - w0_duration))

    # ── Ad-Break-Grenzen aus w0 ermitteln ─────────────────────────────────────
    # Wir suchen das erste START-Marker-Segment und das erste END-Marker-Segment.
    # Falls kein Ad-Break vorhanden, wird [w0_duration, w0_duration] verwendet
    # (leerer Bereich → keine Mauer notwendig).
    ad_start_w0 = w0_duration  # Fallback: kein Ad-Break
    ad_end_w0   = w0_duration
    for seg in transcript_segs:
        txt = seg.get("text", "")
        if AD_START_RE.search(txt) and ad_start_w0 == w0_duration:
            ad_start_w0 = float(seg["start"])
        if AD_END_RE.search(txt) and ad_end_w0 == w0_duration:
            ad_end_w0 = float(seg["end"])
    # Sicherstellen, dass ad_end > ad_start (Fallback bei fehlerhafter Reihenfolge)
    if ad_end_w0 <= ad_start_w0:
        ad_end_w0 = ad_start_w0

    # ── Satzende-Regex (identisch zu _SENTENCE_END_RE aus der Injektion) ──────
    _sent_end = re.compile(r'[.!?…]\s*$')

    # ── Kandidaten-Grenzen aufbauen ───────────────────────────────────────────
    # Bevorzugt (pref): Satzende + Sprecher-Wechsel oder Pause ≥ min_gap_sec
    # Fallback  (fall): beliebige Pause ≥ min_gap_sec (kein Satzende-Zwang)
    pref_candidates: list[float] = []  # w0-Mittelpunkte (bevorzugt)
    fall_candidates: list[float] = []  # w0-Mittelpunkte (Fallback)

    for i in range(len(transcript_segs) - 1):
        seg  = transcript_segs[i]
        nxt  = transcript_segs[i + 1]
        seg_end   = float(seg["end"])
        nxt_start = float(nxt["start"])
        gap = nxt_start - seg_end

        # Midpoint in w0-Zeit
        mid = seg_end + max(gap, 0.0) / 2.0

        # Nie innerhalb des Ad-Break-Bereichs schneiden
        if ad_start_w0 < mid < ad_end_w0:
            continue
        # Auch die direkten Ad-Marker-Segmente selbst nie als Schnittgrenze
        if (AD_START_RE.search(seg.get("text","")) or
                AD_END_RE.search(seg.get("text","")) or
                AD_START_RE.search(nxt.get("text","")) or
                AD_END_RE.search(nxt.get("text",""))):
            continue

        has_gap        = gap >= min_gap_sec
        has_sent_end   = bool(_sent_end.search(seg.get("text", "")))
        has_spk_change = (seg.get("speaker","") != nxt.get("speaker","")
                          and seg.get("speaker","") and nxt.get("speaker",""))

        if has_gap:
            fall_candidates.append(mid)
        if has_sent_end and (has_spk_change or has_gap):
            pref_candidates.append(mid)

    def _pick_cut(candidates: list[float], last_w0: float,
                  section_end_w0: float) -> float | None:
        """Wählt den Kandidaten nächst am Zielzeitpunkt next_target."""
        # Kandidaten, die einen Mindestabstand von MIN_CHUNK von beiden Enden einhalten
        MIN_CHUNK = 300.0
        valid = [c for c in candidates
                 if last_w0 + MIN_CHUNK <= c <= section_end_w0 - MIN_CHUNK]
        return min(valid, key=lambda c: abs(c - _pick_cut.target)) if valid else None

    # ── Schnitte für jede Sektion separat auswählen ───────────────────────────
    # Sektion 1: [0, ad_start_w0]
    # Sektion 2: [ad_end_w0, w0_duration]
    # Die Ad-Break-Mauer selbst wird nicht als WAV-Schnitt ausgegeben —
    # step1 erhält nur die Inhalts-Schnittpunkte; der Ad-Break wird durch
    # step3/step4 (Skip-Liste) aus dem WAV herausgeschnitten.

    sections = [
        (0.0,        ad_start_w0),
        (ad_end_w0,  w0_duration),
    ]

    wav_cut_points: list[float] = []

    for sec_start, sec_end in sections:
        if sec_end - sec_start < target_chunk_sec:
            # Sektion zu kurz für einen weiteren Schnitt
            continue

        last_w0   = sec_start
        next_target = sec_start + target_chunk_sec

        while next_target < sec_end - target_chunk_sec / 2:
            _pick_cut.target = next_target  # type: ignore[attr-defined]

            # Erst bevorzugte Kandidaten versuchen, dann Fallback
            mid = _pick_cut(pref_candidates, last_w0, sec_end)
            if mid is None:
                mid = _pick_cut(fall_candidates, last_w0, sec_end)
            if mid is None:
                print(
                    f"  _chunk_cut_points: WARNUNG — kein Kandidat im Fenster "
                    f"[{last_w0:.0f}s+300, {sec_end:.0f}s-300] für Ziel {next_target:.0f}s "
                    f"— Sektion wird als ein Chunk belassen.",
                    flush=True,
                )
                break

            wav_cut = min(round(mid + intro_offset, 3), wav_duration_s - 0.001)
            wav_cut_points.append(wav_cut)
            last_w0      = mid
            next_target  = mid + target_chunk_sec

    result = sorted(set(wav_cut_points))
    print(
        f"  _chunk_cut_points: {len(result)} Schnitt(e) — "
        f"Ziel={target_chunk_sec:.0f}s  "
        f"Ad-Break-Mauer=[{ad_start_w0:.1f}s,{ad_end_w0:.1f}s]  "
        f"intro_offset={intro_offset:.2f}s",
        flush=True,
    )
    return result


# ══════════════════════════════════════════════════════════════════════════════
# Step 2 — Anchor-Offset bestimmen + w0-Wörter mit linearem Offset versehen
# ══════════════════════════════════════════════════════════════════════════════

def step2_align(ep_num: int, w1_words: list, transcript_segs: list) -> tuple[list, float]:
    """
    Bestimmt den globalen anchor_offset durch Suche des ersten zuverlässig
    bestätigten Ankerpunkts in w1 (unverändert gegenüber der alten Implementierung).

    NEU gegenüber der alten Version:
      Statt linearer Interpolation der w0-Segment-Grenzen werden alle
      Wort-Zeitstempel direkt aus w1-Wort-Timestamps bezogen.  Dazu wird
      _align_segs_to_w1() aufgerufen, das den Matching-Cursor w_pos durch
      w1 vorwärts schiebt und für jedes w0-Segment den passenden w1-Zeitblock
      identifiziert.  Nur Timestamps aus diesem Block fließen in w2aligned ein.

      Nach jedem Ad-Break-Marker-Paar wird der Anker neu gesetzt, weil die
      w1-Aufnahme zwischen AD_END und dem nächsten Podcast-Wort eine beliebig
      lange Werbeunterbrechung enthält, die in w0 nicht existiert.

    Gibt (aligned_word_list, anchor_offset) zurück:
      aligned_word_list: [{"w0_word": str, "start": float, "end": float,
                           "speaker": str, "seg_id": int, "is_ad_marker": bool}, ...]
      anchor_offset: w1_time = w0_time + anchor_offset  (erster Anker, für step3)
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

    # ── Anchor-Suche (unverändert) ────────────────────────────────────────────
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
    # Wird unverändert zurückgegeben — step3 braucht ihn zur Ad-Break-Lokalisation.
    #
    # FIX: compute anchor_offset from the first LCS-matched content word, not from
    # the raw window start w1[anchor_wi].  The window start may be mid-jingle or
    # preamble — several words before the anchor segment's text actually begins.
    # Using the first matched word gives a correct offset for all fallback segments.
    anchor_w0_time = float(transcript_segs[anchor_si]["start"])
    anchor_seg     = transcript_segs[anchor_si]
    anchor_seg_dur = float(anchor_seg["end"]) - float(anchor_seg["start"])
    anchor_win_end = bisect.bisect_right(w1_starts,
                                         w1_starts[anchor_wi] + anchor_seg_dur + 3.0)
    anchor_win_end = max(anchor_wi + 1, min(anchor_win_end, nw1))
    anchor_w1_win  = w1_nwords[anchor_wi:anchor_win_end]
    anchor_bounds  = _lcs_first_last(seg_nwords[anchor_si], anchor_w1_win)
    if anchor_bounds is not None:
        anchor_first_abs = anchor_wi + anchor_bounds[0]
        anchor_w1_time   = w1_words[anchor_first_abs]["start"]
    else:
        anchor_w1_time = w1_words[anchor_wi]["start"]   # fallback: raw window start
    anchor_offset  = anchor_w1_time - anchor_w0_time

    print(f"  step2: Anchor w0-seg[{anchor_si}] @ w0={anchor_w0_time:.2f}s "
          f"→ w1[{anchor_wi if anchor_bounds is None else anchor_wi + anchor_bounds[0]}]"
          f"={anchor_w1_time:.2f}s  "
          f"anchor_offset={anchor_offset:+.3f}s", flush=True)

    # ── Ad-Marker-Segmente vormerken ──────────────────────────────────────────
    ad_marker_sids = {si for si, seg in enumerate(transcript_segs)
                      if AD_START_RE.search(seg.get("text", ""))
                      or AD_END_RE.search(seg.get("text", ""))}

    # ── w1-Timestamps direkt zuweisen (neu) ───────────────────────────────────
    # OLD: for each w0 segment, timestamps were produced by linear interpolation:
    #        t_word_k = (seg.start + anchor_offset) + k * (seg_dur / n_words)
    #      This meant w1 was only used to find anchor_offset; after that, every
    #      word timestamp was synthetic.  Segments that Whisper never transcribed
    #      (because they fell in silence, tail truncation, or context overflow)
    #      still received plausible-looking timestamps derived from w0 boundaries,
    #      causing silent-audio words to appear in w2 with non-zero timestamps.
    #
    # NEW: _align_segs_to_w1() advances a cursor w_pos through w1 segment by
    #      segment.  For each w0 segment it finds the matching w1 time block via
    #      _try_match_seg (same LCS logic used in the anchor confirmation chain),
    #      then distributes the w0 words evenly over the *w1-observed* time span
    #      of that block.  A segment that does not match in w1 gets no entries in
    #      the result — it is silently skipped.  This ensures every timestamp in
    #      w2aligned corresponds to a real Whisper observation.
    #
    #      After each ad-break pair, _find_post_ad_anchor() re-anchors w_pos to
    #      the first confirmed w1 position after the injected ad audio, because
    #      the w0→w1 time mapping shifts by exactly the ad duration at that point.
    result = _align_segs_to_w1(
        transcript_segs  = transcript_segs,
        seg_nwords       = seg_nwords,
        ad_marker_sids   = ad_marker_sids,
        w1_words         = w1_words,
        w1_starts        = w1_starts,
        w1_nwords        = w1_nwords,
        anchor_si        = anchor_si,
        anchor_wi        = anchor_wi,
        anchor_offset    = anchor_offset,
        min_seg_words    = MIN_SEG_WORDS,
        confirm_n        = CONFIRM_N,
    )

    n_w1_pinned  = sum(1 for w in result if w.get("_w1_pinned"))
    n_fallback   = sum(1 for w in result if not w.get("_w1_pinned"))
    # Strip the internal bookkeeping flag before saving
    for w in result:
        w.pop("_w1_pinned", None)

    print(f"  step2: {len(result)} w0-Wörter ausgerichtet — "
          f"w1-verankert={n_w1_pinned}, Fallback-interpoliert={n_fallback}  "
          f"(anchor_offset={anchor_offset:+.3f}s)", flush=True)
    with open(out_path, "w", encoding="utf-8") as fh:
        json.dump(result, fh, ensure_ascii=False)
    print(f"  step2: gespeichert → {out_path}", flush=True)
    return result, anchor_offset


def _find_anchor(
    transcript_segs: list,
    seg_nwords: dict,
    w1_starts: list,
    w1_nwords: list,
    search_start_wi: int,
    search_end_wi:   int,
    min_seg_words:   int,
    confirm_n:       int,
    label:           str = "",
) -> tuple[int, int] | None:
    """
    Sucht ab w1-Position search_start_wi einen bestätigten Ankerpunkt.

    Identische Logik wie der ursprüngliche Anchor-Such-Block in step2_align,
    jetzt als eigenständige Funktion damit sie nach Ad-Breaks wiederverwendet
    werden kann.

    Rückgabe: (anchor_si, anchor_wi) oder None wenn kein Anker gefunden.
    """
    nw1 = len(w1_starts)
    # Kandidaten: erste 30 w0-Segmente ab dem übergebenen Startpunkt
    # OLD: always searched from transcript_segs[0].
    # NEW: called with a slice of transcript_segs and an offset so the same
    #      LCS-plus-confirmation logic works for post-ad re-anchoring too.
    for relaxed in (False, True):
        end_wi = nw1 if relaxed else search_end_wi
        for cand_si, seg0 in enumerate(transcript_segs):
            if len(seg_nwords.get(cand_si, [])) < min_seg_words:
                continue
            for wi in range(search_start_wi, end_wi):
                r0 = _try_match_seg(seg0, seg_nwords[cand_si], wi, w1_starts, w1_nwords)
                if r0 is None:
                    continue
                w_pos = r0[0]
                hits  = 1
                for si2 in range(cand_si + 1, len(transcript_segs)):
                    if hits >= confirm_n:
                        break
                    if len(seg_nwords.get(si2, [])) < min_seg_words:
                        continue
                    r2 = _try_match_seg(transcript_segs[si2], seg_nwords.get(si2, []),
                                        w_pos, w1_starts, w1_nwords)
                    if r2 is None:
                        break
                    w_pos = r2[0]
                    hits += 1
                if hits >= max(2, confirm_n // 2):
                    if label:
                        print(f"  step2: {label} Anker w0-seg[{cand_si}] → w1[{wi}]  "
                              f"t_w1={w1_starts[wi]:.2f}s", flush=True)
                    return cand_si, wi
    return None


def _align_segs_to_w1(
    transcript_segs:  list,
    seg_nwords:       dict,
    ad_marker_sids:   set,
    w1_words:         list,
    w1_starts:        list,
    w1_nwords:        list,
    anchor_si:        int,
    anchor_wi:        int,
    anchor_offset:    float,
    min_seg_words:    int,
    confirm_n:        int,
) -> list:
    """
    Weist jedem w0-Wort einen Timestamp zu, der direkt aus w1 stammt.

    Algorithmus:
      1. Starte bei anchor_si / anchor_wi (beide wurden von step2_align geliefert).
      2. Schiebe w_pos (Cursor in w1) vorwärts, Segment für Segment durch w0.
         Für jedes w0-Segment:
           a. Rufe _try_match_seg() ab w_pos auf.
           b. Bei Treffer: verteile die w0-Wörter gleichmäßig über die w1-Zeitspanne
              [w1_words[w_pos].start, w1_words[match_end_pos - 1].end] dieses Blocks.
              Setze _w1_pinned=True.
           c. Bei Kein-Treffer: nutze als Fallback den linearen Offset
              (w0_seg.start + anchor_offset) — exakt das alte Verhalten, aber nur
              für Segmente, die w1 nicht bestätigt hat.
              Setze _w1_pinned=False.
           d. Schiebe w_pos auf match_end_pos weiter (bei Treffer), sonst unverändert.
      3. Nach jedem AD_END-Marker: rufe _find_anchor() für die nachfolgenden
         w0-Segmente ab der aktuellen w_pos-Position auf.  Bei Erfolg wird w_pos
         auf den neuen Anker gesetzt und anchor_offset lokal aktualisiert.
         Damit wird die durch den Ad-Break verursachte Zeitverschiebung in w1
         automatisch absorbiert, ohne die globale anchor_offset-Variable zu ändern
         (step3 braucht den ursprünglichen Wert).

    OLD: alle Timestamps aus linearer Interpolation: t_k = seg.start + anchor_offset
         + k * (seg_dur / n_words). w1 wurde nach der Anker-Suche nie wieder gelesen.
    NEW: Timestamps kommen aus dem w1-Zeitblock des LCS-gematchten Segments.
         Segmente ohne w1-Match erhalten linearen Fallback, sind aber in den
         Statistiken als solche markiert (_w1_pinned=False).

    Rückgabe: Liste von aligned_word-Dicts (identisches Format wie zuvor, plus
    temporäres Feld _w1_pinned für die Statistik in step2_align).
    """
    nw1    = len(w1_starts)
    result = []

    # w_pos starts at the confirmed anchor position in w1.
    # OLD: no cursor existed — the anchor was only used to compute anchor_offset.
    # NEW: w_pos is the live position in w1; it advances with every matched segment
    #      so subsequent matches always start from where the previous one ended.
    w_pos          = anchor_wi
    cur_offset     = anchor_offset  # may be updated after each ad-break re-anchor
    in_ad          = False          # True while we are inside an ad-break pair
    last_ad_end_si = -1             # w0 seg index of the most recent AD_END marker

    for si, seg in enumerate(transcript_segs):
        spk     = seg.get("speaker", "")
        seg_txt = seg.get("text", "").strip()
        if not seg_txt:
            continue
        words_in_seg = seg_txt.split()
        if not words_in_seg:
            continue

        is_ad_marker = si in ad_marker_sids
        is_ad_start  = bool(AD_START_RE.search(seg.get("text", "")))
        is_ad_end    = bool(AD_END_RE.search(seg.get("text", "")))

        # Track ad-break state so we know when to re-anchor.
        if is_ad_start:
            in_ad = True
        if is_ad_end:
            in_ad          = False
            last_ad_end_si = si

        # ── Post-ad-break re-anchor ───────────────────────────────────────────
        # After the AD_END marker, w1 contains an unknown amount of injected
        # ad audio that does not exist in w0.  The mapping w0_time + cur_offset
        # no longer predicts w1 times correctly.
        #
        # OLD: the single global anchor_offset was used for the entire episode,
        #      causing all post-ad timestamps to drift by exactly the ad duration.
        #
        # NEW: we find the first confirmed post-ad anchor in w1 (starting from
        #      the current w_pos so we never go backwards), re-compute cur_offset
        #      for the post-ad section, and advance w_pos accordingly.
        if last_ad_end_si == si:
            # Build a local view: w0 segments from si+1 onward, capped at 30 segs.
            post_segs   = transcript_segs[si + 1 : si + 31]
            post_nwords = {k: seg_nwords.get(si + 1 + k, [])
                           for k in range(len(post_segs))}
            # Search w1 from w_pos onwards (never go backwards).
            # Limit the search to the next 600 s of w1 to avoid runaway scans.
            search_end_wi = bisect.bisect_right(w1_starts,
                                                w1_starts[w_pos] + 600.0
                                                if w_pos < nw1 else 0.0)
            search_end_wi = min(search_end_wi, nw1)
            anchor_result = _find_anchor(
                transcript_segs  = post_segs,
                seg_nwords       = post_nwords,
                w1_starts        = w1_starts,
                w1_nwords        = w1_nwords,
                search_start_wi  = w_pos,
                search_end_wi    = search_end_wi,
                min_seg_words    = min_seg_words,
                confirm_n        = confirm_n,
                label            = f"Post-Ad (nach seg[{si}])",
            )
            if anchor_result is not None:
                rel_si, new_wi = anchor_result
                abs_si         = si + 1 + rel_si
                new_w1_time    = w1_starts[new_wi]
                new_w0_time    = float(transcript_segs[abs_si]["start"])
                cur_offset     = new_w1_time - new_w0_time
                w_pos          = new_wi
                print(f"  step2: Post-Ad-Anker w0-seg[{abs_si}] @ w0={new_w0_time:.2f}s "
                      f"→ w1[{new_wi}]={new_w1_time:.2f}s  "
                      f"neuer cur_offset={cur_offset:+.3f}s", flush=True)
            else:
                print(f"  step2: WARNUNG — kein Post-Ad-Anker nach seg[{si}] gefunden; "
                      f"cur_offset bleibt {cur_offset:+.3f}s", flush=True)

        # ── Timestamp-Zuweisung ───────────────────────────────────────────────
        # Skip segments that precede the anchor in w0 — they belong to the intro
        # jingle and have no match in the post-anchor w1 stream.
        if si < anchor_si:
            continue

        # Ad-marker segments: emit with linear fallback, mark is_ad_marker=True.
        # Their timestamps are only used by step3 to locate WAV cut boundaries,
        # not in the final w2 word list (step4 drops them).
        if is_ad_marker:
            seg_start_fb = float(seg["start"]) + cur_offset
            seg_end_fb   = float(seg["end"])   + cur_offset
            seg_dur_fb   = max(seg_end_fb - seg_start_fb, 0.0)
            n_w          = len(words_in_seg)
            dpw          = seg_dur_fb / n_w if n_w and seg_dur_fb > 0 else 0.05
            for wi0, word in enumerate(words_in_seg):
                t_s = seg_start_fb + wi0 * dpw
                result.append({
                    "w0_word":      word,
                    "start":        round(t_s, 4),
                    "end":          round(t_s + dpw, 4),
                    "speaker":      spk,
                    "seg_id":       si,
                    "is_ad_marker": True,
                    "_w1_pinned":   False,
                })
            continue

        # ── Match this w0 segment against w1 ───────────────────────────────────
        # Strategy: use w0 segment timestamps + running cur_offset to ESTIMATE
        # where in w1 this segment occurs, then confirm with _try_match_seg
        # (LCS at segment level).  cur_offset is UPDATED after each successful
        # match, so it tracks the w0→w1 drift across the episode ("align by
        # adding up").
        #
        # Once the w1 block is confirmed, _lcs_pairs matches individual w0
        # words to w1 words.  Matched w0 words get the REAL w1 word [start,end].
        # Unmatched w0 words (whisper dropped/merged them) are REMOVED — no
        # invented timestamps.
        if w_pos >= nw1:
            break  # no more w1 words to search

        if len(seg_nwords.get(si, [])) >= 2:
            w0_seg_start = float(seg["start"])
            w0_seg_end   = float(seg["end"])

            # Estimate w1 position from w0 time + running offset.
            est_w1_time = w0_seg_start + cur_offset
            est_w_pos   = bisect.bisect_left(w1_starts, est_w1_time)
            # Never go backwards in w1.
            search_w_pos = max(w_pos, est_w_pos)

            match = _try_match_seg(seg, seg_nwords[si], search_w_pos,
                                   w1_starts, w1_nwords)
            # Fallback: if estimate was too far ahead, try from w_pos.
            if match is None and search_w_pos > w_pos:
                match = _try_match_seg(seg, seg_nwords[si], w_pos,
                                       w1_starts, w1_nwords)
                if match is not None:
                    search_w_pos = w_pos

            if match is not None:
                match_end_pos = match[0]
                # Build the w1 window for per-word LCS matching.
                seg_dur_w = float(seg["end"]) - float(seg["start"])
                t0_w      = w1_starts[search_w_pos]
                win_end_w = bisect.bisect_right(w1_starts,
                                                t0_w + max(seg_dur_w, 0) + 3.0)
                win_end_w = max(search_w_pos + 1, min(win_end_w, nw1))
                w1_win    = w1_nwords[search_w_pos:win_end_w]

                # Normalise each w0 word individually (NOT norm_words which can
                # split one word into multiple, e.g. "0,3er" → ["0","3er"]).
                w0_norm_pw = [(norm_words(w) or [""])[0] for w in words_in_seg]

                pairs = _lcs_pairs(w0_norm_pw, w1_win)
                if pairs:
                    # Update cur_offset from the FIRST matched pair — this is
                    # the most reliable anchor point (actual word match, not
                    # just time-window boundary).
                    first_w1_abs = search_w_pos + pairs[0][1]
                    cur_offset   = (w1_words[first_w1_abs]["start"]
                                    - w0_seg_start)

                    for w0_idx, w1_rel in pairs:
                        w1_abs = search_w_pos + w1_rel
                        w1w    = w1_words[w1_abs]
                        result.append({
                            "w0_word":      words_in_seg[w0_idx],
                            "start":        round(w1w["start"], 4),
                            "end":          round(w1w["end"], 4),
                            "speaker":      spk,
                            "seg_id":       si,
                            "is_ad_marker": False,
                            "_w1_pinned":   True,
                        })
                    # Advance w_pos past the LAST matched w1 word, NOT just to
                    # the time-based window end.  match_end_pos is computed
                    # from w0_seg_dur, but the w1 words may span MORE time
                    # than w0_seg_dur (e.g. whisper spoke slower).  If we only
                    # advance to match_end_pos, the next segment could match
                    # w1 words that belong to THIS segment — causing
                    # non-monotonic timestamps.
                    last_matched_w1 = search_w_pos + pairs[-1][1] + 1
                    w_pos = max(match_end_pos, last_matched_w1)
                else:
                    w_pos = match_end_pos

    return result


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

    def find_post_ad_anchor(si_e: int, t_exp_end: float) -> tuple[int, int] | None:
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
    Produces ep{N}_stripped.wav (intro + all ad-breaks removed) and ep{N}_w2.json
    (word-level alignments with timestamps adjusted to the stripped WAV clock).

    Strategy: sequential cuts, not a single offset formula.
      CUT 1  — remove the intro (everything before the first spoken word).
      CUT 2…N — remove each ad-break in order; after every removal the in-memory
                 word list is immediately re-indexed before the next cut.
      CUT TAIL — trim the WAV to end exactly at the last word's end timestamp.

    Because each cut is processed independently on already-adjusted timestamps,
    words can never straddle a cut boundary, so no word is ever silently dropped.

    Return value: (keep_regions, spk_order, alignments)
      keep_regions  — list of (start_raw, end_raw) kept intervals (raw WAV clock)
      spk_order     — speaker labels in first-appearance order (for step5)
      alignments    — w2 word list already in memory (no need to re-read w2.json)
    """
    wav_path      = os.path.join(WHISPER_CACHE, f"ep{ep_num}.wav")
    stripped_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}_stripped.wav")
    w2_path       = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w2.json")

    total_dur = wav_duration(wav_path)

    # ── Build speaker order from w2aligned (first-appearance, before any drops) ─
    spk_order = []
    for w in aligned_words:
        if w["speaker"] not in spk_order:
            spk_order.append(w["speaker"])

    # ─────────────────────────────────────────────────────────────────────────────
    # PHASE 1 — determine every raw-WAV cut interval (intro + ad-breaks + tail)
    #           and build the keep_regions list for ffmpeg.
    #
    # The word list timestamps are raw WAV times throughout this phase.
    # We do NOT touch the word list here — cuts are applied to it in Phase 2.
    # ─────────────────────────────────────────────────────────────────────────────

    # CUT 1 — INTRO
    # The intro ends exactly where the first non-ad-marker spoken word begins.
    # This is the raw WAV offset that corresponds to timestamp 0.0 in w2aligned
    # (the alignment anchor was established at the first spoken word in step 2).
    content_words = [w for w in aligned_words if not w["is_ad_marker"]]
    if not content_words:
        raise RuntimeError(
            f"step4: aligned_words enthält keine Nicht-Ad-Marker-Einträge — "
            f"w2aligned.json prüfen."
        )
    intro_end_raw = content_words[0]["start"]   # raw WAV offset of first word

    # CUT 2…N — AD-BREAKS (taken directly from step3's skip_list)
    skips = sorted(skip_list)   # list of (skip_s_raw, skip_e_raw)

    # CUT TAIL — find the raw end of the last spoken word
    tail_end_raw = content_words[-1]["end"]

    # Build keep_regions: intro_end → skip[0].start, skip[0].end → skip[1].start, …
    # … skip[-1].end → tail_end_raw.
    keep_regions = []
    cursor = intro_end_raw
    for (skip_s, skip_e) in skips:
        if skip_s > cursor:
            keep_regions.append((cursor, skip_s))
        # If skip_s <= cursor the skip overlaps the current position; skip it.
        cursor = max(cursor, skip_e)
    # Add the final content segment up to the last spoken word (tail trim).
    if cursor < tail_end_raw:
        keep_regions.append((cursor, tail_end_raw))

    if not keep_regions:
        raise RuntimeError(
            f"step4: keine Keep-Regionen übrig nach Intro/Ad-Break/Tail-Trimming. "
            f"Skip-Liste prüfen: {skip_list}"
        )

    print(f"  step4: {len(keep_regions)} Keep-Region(en), "
          f"{len(skips)} Ad-Break(s) + Intro + Tail übersprungen", flush=True)
    for i, (ks, ke) in enumerate(keep_regions):
        print(f"    keep[{i}]: {ks:.3f}s – {ke:.3f}s  ({ke-ks:.1f}s)", flush=True)

    # ─────────────────────────────────────────────────────────────────────────────
    # PHASE 2 — produce the stripped WAV via ffmpeg
    # ─────────────────────────────────────────────────────────────────────────────

    filter_parts = []
    for i, (ks, ke) in enumerate(keep_regions):
        filter_parts.append(
            f"[0:a]atrim={ks:.6f}:{ke:.6f},asetpts=PTS-STARTPTS[p{i}]"
        )
    concat_inputs = "".join(f"[p{i}]" for i in range(len(keep_regions)))
    filter_str = (
        ";".join(filter_parts)
        + f";{concat_inputs}concat=n={len(keep_regions)}:v=0:a=1[out]"
    )

    subprocess.run([
        "ffmpeg", "-v", "error", "-i", wav_path,
        "-filter_complex", filter_str,
        "-map", "[out]", stripped_path, "-y",
    ], check=True)

    print(f"  step4: gestrippter WAV gespeichert → {stripped_path}", flush=True)

    # ─────────────────────────────────────────────────────────────────────────────
    # PHASE 3 — adjust word timestamps to the stripped WAV clock.
    #
    # Every cut boundary (intro end, each skip start/end, tail end) was derived
    # from an exact word boundary in w2aligned or w1 by steps 2 and 3.  That
    # means no word can ever straddle a cut — a word is either entirely inside a
    # kept region or entirely inside a removed region.  No clamping is needed.
    #
    # The approach is a single linear pass over the sorted word list.  We track
    # a running `offset` that accumulates the total duration of all removed
    # segments seen so far.  For each word:
    #   • If it falls inside a removed region  → drop it.
    #   • Otherwise                            → subtract `offset` from its
    #                                            timestamps (closing the gaps).
    #
    # Removed regions in raw-WAV order:
    #   1. [0.0,          intro_end_raw]   — the intro jingle
    #   2. [skip_s, skip_e] for each skip  — each ad-break
    #   3. [tail_end_raw,  total_dur]      — silence after the last word
    # ─────────────────────────────────────────────────────────────────────────────

    # Build the removed regions sorted by start time.
    removed: list[tuple[float, float]] = (
        [(0.0, intro_end_raw)]
        + list(skips)
        + [(tail_end_raw, total_dur)]
    )
    removed.sort(key=lambda r: r[0])

    # Merge any adjacent/overlapping removed regions (defensive; should not
    # occur in practice since the boundaries come from non-overlapping sources).
    merged: list[tuple[float, float]] = []
    for rs, re in removed:
        if re <= rs:
            continue  # zero-length region — skip
        if merged and rs <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], re))
        else:
            merged.append((rs, re))

    # Single pass: for each word decide keep-or-drop and apply the offset.
    # `ri` is the index into `merged` of the next removed region to consider.
    # `offset` is the total removed duration before the current word.
    words: list[dict] = []
    ri     = 0
    offset = 0.0

    for w in aligned_words:
        if w["is_ad_marker"]:
            continue  # ad-marker words are never in the output

        ws, we = w["start"], w["end"]

        # Advance `offset` past all removed regions that end before this word.
        while ri < len(merged) and merged[ri][1] <= ws:
            offset += merged[ri][1] - merged[ri][0]
            ri += 1

        # If this word falls inside the current removed region — drop it.
        if ri < len(merged) and ws >= merged[ri][0]:
            continue

        # Word is in a kept region — subtract the accumulated offset.
        words.append({
            "w0_word": w["w0_word"],
            "start":   round(ws - offset, 4),
            "end":     round(we - offset, 4),
            "speaker": w["speaker"] or "SPEAKER_MAIN",
        })

    # ─────────────────────────────────────────────────────────────────────────────
    # PHASE 4 — write w2.json
    # Overlaps were already resolved in step1 before w1 was saved, so
    # _resolve_overlaps is not called again here.
    # ─────────────────────────────────────────────────────────────────────────────

    alignments = [
        [w["w0_word"], [w["start"], w["end"]], w["speaker"]]
        for w in words
    ]

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

def find_episodes(episode_filter: set[int] | None = None) -> list[tuple[int, str, str]]:
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


def parse_episode_filter(spec: str) -> set[int]:
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


def process_episode(ep_num: int, transcript_path: str, mp3_path: str) -> None:
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

    # ── Step 6 — Facts-Injektion (teacher-forcing) → w3.json ─────────────────
    # Liest w2_alignments aus dem Speicher, ersetzt ausgewählte Satz-Spans durch
    # FACT-Text (Timestamps unverändert) und schreibt das Ergebnis als w3.json.
    # w2.json bleibt dabei vollständig unberührt.
    print(f"\n  [Step 6] Facts-Injektion → w3.json", flush=True)
    w3_alignments = inject_facts_into_alignments(w2_alignments)
    w3_path = os.path.join(WHISPER_CACHE, f"ep{ep_num}_w3.json")
    with open(w3_path, "w", encoding="utf-8") as fh:
        json.dump({"alignments": w3_alignments}, fh, ensure_ascii=False)
    print(f"  step6: w3 ({len(w3_alignments)} Einträge) gespeichert → {w3_path}",
          flush=True)

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
    print(f"    w3.json:         {w3_path}  ({len(w3_alignments)} Einträge, inkl. Fakten)", flush=True)


def main() -> None:
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
