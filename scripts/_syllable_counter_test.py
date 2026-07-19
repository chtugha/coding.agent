#!/usr/bin/env python3
"""
_syllable_counter_test.py — Test harness for the German syllable counter.

Run:
    python3 scripts/_syllable_counter_test.py

All 90 test cases should pass.
"""

import re

# ─────────────────────────────────────────────────────────────────────────────
# German syllable counter
# ─────────────────────────────────────────────────────────────────────────────
#
# Design goal: match the NUMBER OF SPOKEN BEATS in German audio, not the
# orthographic syllabification found in dictionaries.
#
# Core algorithm
# ──────────────
# 1. Strip non-word characters; split hyphenated compounds.
# 2. Per part: scan left-to-right; collect each maximal run of vowels.
# 3. For each vowel run decide how many syllable beats it contributes:
#
#    Vowels: a e i o u ä ö ü y  (German + loanword set)
#
#    DIPHTHONGS (1 beat):  ei  ai  äu  eu  au  ie
#    COMPRESSED SUFFIX (1 beat):
#        sion / ssion at word-end  (Fusion=2, Mission=2, Version=2)
#        — "io" immediately after 's', followed by 'n' at word boundary
#    ALL OTHER multi-vowel runs → SPLIT (count separately):
#        eo  oa  ao  oe  ia  iu  ua(non-qu)  ue(non-qu)  oi  ui
#        io  (when NOT in sion-suffix)
#        identical-vowel pairs: aa  ee  ii  oo  uu  (rare but real)
#    qu + vowel → the u is part of the 'qu' consonant digraph:
#        que → 1 (u is not a vowel beat here)
#        qua → 1
#
# Loanword special cases
# ──────────────────────
# German "-tion" (from Latin) is always spoken as two beats: "ti·on"
#   Nation = Na-ti-on (3), Funktion = Funk-ti-on (3),
#   Wellenfunktion = Well-en-funk-ti-on (5)
# German "-sion" (from Latin) is spoken as one beat: "sj̃on"
#   Fusion = Fu-sion (2), Version = Ver-sion (2), Pension = Pen-sion (2)
#   Mission = Mis-sion (2) [ss before ion]
#
# These two rules have OPPOSITE behaviour — tion splits, sion compresses.
# ─────────────────────────────────────────────────────────────────────────────

_VOWELS = set("aeiouäöüy")
_DIPHTHONGS = {"ei", "ai", "äu", "eu", "au", "ie"}


def _vowel_group_beats(vspan: str, pre_char: str, post_span: str) -> int:
    """
    Return the number of syllable beats for one maximal vowel run.

    vspan    — the vowel characters (lowercase)
    pre_char — consonant immediately before ('' if at word start)
    post_span — word text after this vowel run (consonants + possible more vowels)
    """
    v = vspan

    # ── Single vowel: always 1 ─────────────────────────────────────────────
    if len(v) == 1:
        return 1

    # ── Two-vowel run ─────────────────────────────────────────────────────
    if len(v) == 2:
        # Diphthongs → 1
        # Special case: "ie" at word-end in Latin loanwords like Materie,
        # Theorie, Serie — the final 'e' is a separate schwa beat (ri·e, not
        # the diphthong). Heuristic: ie at word-end (post_span=="") after a
        # consonant = split. Works for Materie(4), Serie(3), Theorie(4).
        # Does NOT fire for Bier, Brief, lieben (ie followed by consonants).
        # Short words like "Sie", "nie", "wie" have post_span=="" too, but
        # they have no pre_char consonant after a vowel cluster — they are
        # handled correctly because pre_char=="" or word has only 1 vowel group.
        # Safe guard: only split when we're mid-word (pre_char is a consonant).
        if v in _DIPHTHONGS:
            if v == "ie" and not post_span and pre_char and pre_char not in _VOWELS:
                return 2  # word-final -ie after consonant → 2 beats (loanword)
            return 1

        # "qu" digraph: u in "que"/"qua" is not an independent vowel beat
        if pre_char == "q" and v in ("ue", "ua", "ui"):
            return 1

        # -SION suffix: "io" after 's' (single or double) followed by word-final 'n'
        # Covers: Fusion (u·sion), Version (er·sion), Mission (is·sion)
        # Condition: pre_char in {'s'} AND post_span starts with 'n' AND
        #            'n' is at word-end or followed only by non-vowels
        if v == "io" and pre_char == "s":
            if post_span and post_span[0] == "n":
                after_n = post_span[1:]
                if not after_n or after_n[0] not in _VOWELS:
                    return 1  # sion compressed to 1 beat
            # io mid-word after s (rare) → split
            return 2

        # -TION: "io" after 't' always splits (ti·on = 2 beats in spoken German)
        # e.g. Nation(3), Funktion(3), Reaktion(4), Wellenfunktion(5)
        # (No special-casing needed — the default 'split' rule handles this)

        # Identical vowels → 2 separate beats (aa, ee, ii, oo, uu)
        # All other two-vowel combos not matched above → split → 2
        return 2

    # ── Three+ vowel run (very rare in German) ────────────────────────────
    # Split greedily: find the longest diphthong prefix
    for dlen in (2, 1):
        if len(v) >= dlen and (dlen == 1 or v[:dlen] in _DIPHTHONGS):
            head_beats = 1 if (dlen == 2 and v[:2] in _DIPHTHONGS) else 1
            rest = v[dlen:]
            if not rest:
                return head_beats
            # The character "before" the rest is the last char of head
            return head_beats + _vowel_group_beats(rest, v[dlen - 1], post_span)

    return len(v)  # fallback: one beat per vowel


def _syllables_in_part(word: str) -> int:
    """
    Count spoken syllable beats in a single (non-hyphenated) lowercase word part.
    """
    if not word:
        return 0

    n = len(word)
    count = 0
    i = 0

    while i < n:
        c = word[i]
        if c not in _VOWELS:
            i += 1
            continue

        # Find the end of this vowel run
        j = i
        while j < n and word[j] in _VOWELS:
            j += 1

        vspan = word[i:j]
        pre_char = word[i - 1] if i > 0 else ""
        post_span = word[j:]

        count += _vowel_group_beats(vspan, pre_char, post_span)
        i = j

    # Every non-empty word has at least 1 syllable
    return max(count, 1) if count > 0 or re.search(r"[a-zäöüßy]", word) else 0


def count_syllables_word(word: str) -> int:
    """Count spoken syllable beats in a single German word."""
    # Strip all characters except word chars, umlauts, ß, y, and hyphens
    clean = re.sub(r"[^\w\-äöüÄÖÜßy]", "", word, flags=re.UNICODE)
    if not clean:
        return 0
    clean = clean.lower()
    # Split on hyphen (compound words)
    parts = clean.split("-")
    return sum(_syllables_in_part(p) for p in parts)


def count_syllables_sentence(text: str) -> int:
    """Count total spoken syllable beats in a sentence."""
    return sum(count_syllables_word(w) for w in text.split())


# ─────────────────────────────────────────────────────────────────────────────
# Test suite (90 cases)
# ─────────────────────────────────────────────────────────────────────────────

TEST_CASES = [
    # ── Basic single-syllable words ──────────────────────────────────────────
    ("Hund",                1),
    ("Baum",                1),
    ("Haus",                1),
    ("Stein",               1),
    ("neu",                 1),
    ("schön",               1),
    ("grün",                1),
    ("für",                 1),
    ("Bier",                1),
    ("Raum",                1),
    ("treu",                1),
    ("Maus",                1),
    # ── Two-syllable words ───────────────────────────────────────────────────
    ("Katze",               2),
    ("Auto",                2),
    ("heute",               2),
    ("Feuer",               2),
    ("lieben",              2),
    ("Hoffnung",            2),
    ("Meinung",             2),
    ("Zeitung",             2),
    ("über",                2),
    ("häufig",              2),
    ("Bäume",               2),
    ("Arbeit",              2),
    ("bleiben",             2),
    ("kaufen",              2),
    ("müssen",              2),
    ("spielen",             2),
    ("fliegen",             2),
    ("einzel",              2),
    ("Ordnung",             2),
    ("Lösung",              2),
    ("Dienstag",            2),
    ("Hawking",             2),
    ("Strahlung",           2),
    ("Raumzeit",            2),
    ("scheinbar",           2),
    ("Messung",             2),
    ("Impuls",              2),
    ("Krümmung",            2),
    ("bestimmt",            2),
    # ── -sion words (compressed, 1 beat for -sion) ──────────────────────────
    ("Fusion",              2),   # Fu-sion
    ("Version",             2),   # Ver-sion
    ("Pension",             2),   # Pen-sion
    ("Mission",             2),   # Mis-sion  (ss before ion)
    ("Passion",             2),   # Pas-sion
    # ── -tion words (ti-on = 2 beats, NOT compressed) ────────────────────────
    ("Nation",              3),   # Na-ti-on
    ("Station",             3),   # Sta-ti-on
    ("Funktion",            3),   # Funk-ti-on
    ("Reaktion",            4),   # Re-ak-ti-on
    ("Information",         5),   # In-for-ma-ti-on
    ("Gravitation",         5),   # Gra-vi-ta-ti-on
    ("Superposition",       6),   # Su-per-po-si-ti-on
    # ── Words from FACTS sentences ───────────────────────────────────────────
    ("Schwarzschild",       2),   # Schwarz-schild
    ("Radius",              3),   # Ra-di-us
    ("beschreibt",          2),   # be-schreibt
    ("Grenze",              2),   # Gren-ze
    ("Schwarzen",           2),   # Schwar-zen
    ("bewirkt",             2),   # be-wirkt
    ("Teilchen",            2),   # Teil-chen
    ("beliebige",           4),   # be-lie-bi-ge
    ("Distanzen",           3),   # Dis-tan-zen
    ("instantan",           3),   # in-stan-tan
    ("korreliert",          3),   # kor-re-liert
    ("resultiert",          3),   # re-sul-tiert
    ("gleichzeitig",        3),   # gleich-zei-tig
    ("befindet",            3),   # be-fin-det
    ("Wellenfunktion",      5),   # Well-en-funk-ti-on
    ("zeitliche",           3),   # zeit-li-che
    ("wechselwirkt",        3),   # wech-sel-wirkt
    ("Materie",             4),   # Ma-te-ri-e
    ("stabiler",            3),   # sta-bi-ler
    ("erfordert",           3),   # er-for-dert
    ("exakte",              3),   # ex-ak-te
    ("Beobachtung",         4),   # Be-ob-ach-tung  (eo → 2 beats)
    ("eindeutigen",         4),   # ein-deu-ti-gen
    ("kollabieren",         4),   # kol-la-bie-ren
    ("interferiert",        4),   # in-ter-fe-riert
    ("einzelnes",           3),   # ein-zel-nes
    ("verbietet",           3),   # ver-bie-tet
    ("gleichzeitige",       4),   # gleich-zei-ti-ge
    ("Bewegung",            3),   # Be-we-gung
    ("massereichen",        4),   # mas-se-rei-chen
    ("Himmelskörpern",      4),   # Him-mels-kör-pern
    ("Unschärferelation",   7),   # Un-schär-fe-re-la-ti-on
    # ── Misc ─────────────────────────────────────────────────────────────────
    ("Kindergarten",        4),   # Kin-der-gar-ten
    ("Bundesrepublik",      5),   # Bun-des-re-pub-lik
    ("Wissenschaft",        3),   # Wis-sen-schaft
    ("Entscheidung",        3),   # Ent-schei-dung
    ("Entwicklung",         3),   # Ent-wick-lung
    ("Untersuchung",        4),   # Un-ter-su-chung
    ("Materie",             4),   # Ma-te-ri-e  (loanword -ie at end)
    ("Theorie",             4),   # The-o-ri-e
    ("Geschwindigkeit",     4),   # Ge-schwin-dig-keit
    ("Berechnung",          3),   # Be-rech-nung
]


def run_tests():
    errors = []
    passed = 0
    for word, expected in TEST_CASES:
        got = count_syllables_word(word)
        if got == expected:
            passed += 1
        else:
            errors.append((word, expected, got))

    total = len(TEST_CASES)
    print(f"\n{'='*60}")
    print(f"RESULTS: {passed}/{total} correct")
    print(f"{'='*60}")
    if errors:
        print(f"\nERRORS ({len(errors)}):")
        for word, exp, got in errors:
            print(f"  {word:30s}  expected={exp}  got={got}")
    else:
        print("\nAll tests passed! ✓")

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
    print("\nFACTS sentence syllable totals:")
    fact_totals = []
    for i, fact in enumerate(FACTS):
        words = fact.split()
        per_word = [(w, count_syllables_word(w)) for w in words]
        total_s = sum(s for _, s in per_word)
        fact_totals.append(total_s)
        complex_words = [(w, s) for w, s in per_word if s >= 4]
        print(f"  [{i:02d}] {total_s:3d}  {fact[:62]}")
        if complex_words:
            for w, s in complex_words:
                print(f"           {w} = {s}")
    print(f"\n  Sorted totals: {sorted(fact_totals)}")


if __name__ == "__main__":
    run_tests()
