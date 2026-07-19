#!/usr/bin/env python3
"""
dryrun_facts_injection.py — Dry-run: syllable-based fact–sentence matching
═══════════════════════════════════════════════════════════════════════════

Reads an existing ep{N}_w2.json from the whisper_cache, counts syllables for
every sentence in the alignment AND for every FACTS sentence, then tries to
match each fact to the best-matching w2 sentence by syllable-count similarity.

NO files are written or modified.

Usage:
    python3 scripts/dryrun_facts_injection.py --episode 150
    python3 scripts/dryrun_facts_injection.py --episode 150 --cache-dir /Volumes/eHDD/moshi-rag-data/datasets/whisper_cache
"""

import argparse
import json
import os
import re

# ── FACTS (exact copy from podcast_to_moshi_dataset.py) ──────────────────────
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

# ── Syllable counting (accurate German spoken-beat counter) ──────────────────
# Rules:
#   DIPHTHONGS (1 beat):   ei  ai  äu  eu  au  ie
#     Exception: word-final "-ie" after consonant in loanwords → 2 (Ma-te-ri-e)
#   -SION suffix (compressed, 1 beat): Fusion=2, Version=2, Mission=2
#   -TION (splits, 2 beats for "ti-on"): Nation=3, Funktion=3, Wellenfunktion=5
#   qu + vowel → u not an independent beat (que, qua → 1)
#   All other two-vowel runs → split (2 beats)
#   Hyphen-joined compounds → count each part separately

_VOWELS = set("aeiouäöüy")
_DIPHTHONGS = {"ei", "ai", "äu", "eu", "au", "ie"}


def _vowel_group_beats(vspan: str, pre_char: str, post_span: str) -> int:
    """Return spoken syllable beats for one maximal vowel run (lowercase input)."""
    v = vspan
    if len(v) == 1:
        return 1
    if len(v) == 2:
        if v in _DIPHTHONGS:
            # word-final "-ie" after consonant = loanword schwa (Ma-te-ri-e)
            if v == "ie" and not post_span and pre_char and pre_char not in _VOWELS:
                return 2
            return 1
        if pre_char == "q" and v in ("ue", "ua", "ui"):
            return 1  # "qu" digraph
        # -SION: "io" after "s", followed by word-final "n"
        if v == "io" and pre_char == "s":
            if post_span and post_span[0] == "n":
                after_n = post_span[1:]
                if not after_n or after_n[0] not in _VOWELS:
                    return 1  # sion compressed to 1 spoken beat
            return 2
        return 2  # all other two-vowel runs split
    # Three+ vowels: greedily consume diphthong prefix, recurse on remainder
    if v[:2] in _DIPHTHONGS:
        rest = v[2:]
        if not rest:
            return 1
        return 1 + _vowel_group_beats(rest, v[1], post_span)
    return 1 + _vowel_group_beats(v[1:], v[0], post_span)


def _syllables_in_part(word: str) -> int:
    """Count spoken syllable beats in a single (non-hyphenated) lowercase part."""
    if not word:
        return 0
    n = len(word)
    count = 0
    i = 0
    while i < n:
        if word[i] not in _VOWELS:
            i += 1
            continue
        j = i
        while j < n and word[j] in _VOWELS:
            j += 1
        count += _vowel_group_beats(word[i:j], word[i - 1] if i > 0 else "", word[j:])
        i = j
    return max(count, 1) if count > 0 or re.search(r"[a-zäöüßy]", word) else 0


def count_syllables_word(word: str) -> int:
    """Count spoken syllable beats in a single German word."""
    clean = re.sub(r"[^\w\-äöüÄÖÜßy]", "", word, flags=re.UNICODE)
    if not clean:
        return 0
    clean = clean.lower()
    return sum(_syllables_in_part(p) for p in clean.split("-"))


def syllable_array(words: list[str]) -> list[int]:
    """Return per-word syllable counts for a list of word strings."""
    return [count_syllables_word(w) for w in words]


# ── Sentence segmentation from w2 alignments ─────────────────────────────────
# A "sentence" here = a contiguous run of words from the same speaker
# separated from the next run by either a gap > GAP_THRESHOLD seconds
# or a speaker change.

GAP_THRESHOLD_S = 0.8   # pause longer than this → sentence boundary


def segment_into_sentences(alignments: list) -> list[dict]:
    """
    Split the w2 alignment list into sentence-like spans.

    Returns a list of dicts:
      {
        "start_idx": int,      # first word index in alignments
        "end_idx":   int,      # last word index (inclusive)
        "speaker":   str,
        "words":     [str],
        "starts":    [float],  # per-word start times
        "ends":      [float],  # per-word end times
        "t_start":   float,    # sentence start time
        "t_end":     float,    # sentence end time
        "duration":  float,    # t_end - t_start
        "syllables": [int],    # per-word syllable counts
        "total_syl": int,      # sum
      }
    """
    sentences = []
    if not alignments:
        return sentences

    cur_start = 0
    cur_speaker = alignments[0][2]
    cur_words   = [alignments[0][0]]
    cur_starts  = [alignments[0][1][0]]
    cur_ends    = [alignments[0][1][1]]

    def flush(end_idx):
        syls = syllable_array(cur_words)
        sentences.append({
            "start_idx": cur_start,
            "end_idx":   end_idx,
            "speaker":   cur_speaker,
            "words":     list(cur_words),
            "starts":    list(cur_starts),
            "ends":      list(cur_ends),
            "t_start":   cur_starts[0],
            "t_end":     cur_ends[-1],
            "duration":  cur_ends[-1] - cur_starts[0],
            "syllables": syls,
            "total_syl": sum(syls),
        })

    for i in range(1, len(alignments)):
        word, (t_s, t_e), spk = alignments[i]
        prev_end = cur_ends[-1]
        gap = t_s - prev_end

        # Sentence boundary: speaker change OR long gap
        if spk != cur_speaker or gap > GAP_THRESHOLD_S:
            flush(i - 1)
            cur_start  = i
            cur_speaker = spk
            cur_words  = [word]
            cur_starts = [t_s]
            cur_ends   = [t_e]
        else:
            cur_words.append(word)
            cur_starts.append(t_s)
            cur_ends.append(t_e)

    flush(len(alignments) - 1)
    return sentences


# ── Syllable similarity ───────────────────────────────────────────────────────

def similarity_score(fact_syls: list[int], sent_syls: list[int]) -> float:
    """
    Score how well a fact's per-word syllable array matches a w2 sentence's.

    Strategy:
      1. Total-count ratio: abs(sum_fact - sum_sent) / max(sum_fact, sum_sent)
         → 0.0 = perfect total match, 1.0 = completely different totals
      2. Word-count ratio: abs(n_fact - n_sent) / max(n_fact, n_sent)
         → penalises very different word counts (different rhythm)
      3. Combined score = 1 - (0.6 * total_ratio + 0.4 * word_ratio)
         → 1.0 = perfect, 0.0 = worst

    We weight total syllable count more (0.6) than word count (0.4) because
    duration in the WAV is driven primarily by total syllables.
    """
    if not fact_syls or not sent_syls:
        return 0.0
    sum_f = sum(fact_syls)
    sum_s = sum(sent_syls)
    n_f   = len(fact_syls)
    n_s   = len(sent_syls)
    total_ratio = abs(sum_f - sum_s) / max(sum_f, sum_s)
    word_ratio  = abs(n_f - n_s)    / max(n_f, n_s)
    return 1.0 - (0.6 * total_ratio + 0.4 * word_ratio)


# ── Main dry-run ──────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--episode", type=int, required=True,
                        help="Episode number, e.g. 150")
    parser.add_argument("--cache-dir",
                        default="/Volumes/eHDD/moshi-rag-data/datasets/whisper_cache",
                        help="Path to whisper_cache directory")
    parser.add_argument("--top-n", type=int, default=3,
                        help="Show top N w2 sentence matches per fact (default 3)")
    parser.add_argument("--min-words", type=int, default=4,
                        help="Minimum words a w2 sentence must have to be a candidate (default 4)")
    args = parser.parse_args()

    w2_path = os.path.join(args.cache_dir, f"ep{args.episode}_w2.json")
    if not os.path.exists(w2_path):
        print(f"ERROR: {w2_path} not found")
        return

    with open(w2_path, encoding="utf-8") as f:
        data = json.load(f)
    alignments = data["alignments"]
    print(f"Loaded ep{args.episode}_w2.json — {len(alignments)} words")

    # ── Build w2 sentence index ───────────────────────────────────────────────
    sentences = segment_into_sentences(alignments)
    candidates = [s for s in sentences if len(s["words"]) >= args.min_words]
    print(f"Total sentence spans: {len(sentences)},  "
          f"candidates (≥{args.min_words} words): {len(candidates)}")
    print()

    # ── Build fact syllable arrays ────────────────────────────────────────────
    print("═" * 72)
    print("FACTS syllable arrays:")
    print("═" * 72)
    fact_data = []
    for fi, fact in enumerate(FACTS):
        words = fact.split()
        syls  = syllable_array(words)
        total = sum(syls)
        pairs = list(zip(words, syls))
        print(f"  [{fi:02d}] {fact[:60]}{'…' if len(fact)>60 else ''}")
        print(f"        words={len(words)}, total_syllables={total}")
        print(f"        per-word: {pairs}")
        fact_data.append({"text": fact, "words": words, "syllables": syls, "total": total})
    print()

    # ── Match each fact to the best w2 sentences ──────────────────────────────
    print("═" * 72)
    print(f"TOP-{args.top_n} MATCHES per fact:")
    print("═" * 72)

    for fi, fd in enumerate(fact_data):
        scored = []
        for si, sent in enumerate(candidates):
            score = similarity_score(fd["syllables"], sent["syllables"])
            scored.append((score, si, sent))
        scored.sort(key=lambda x: -x[0])
        top = scored[:args.top_n]

        print(f"\nFact [{fi:02d}]: \"{fd['text'][:70]}\"")
        print(f"  Fact syllables: {fd['total']} total, {len(fd['words'])} words")
        print(f"  syllable array: {list(zip(fd['words'], fd['syllables']))}")
        print(f"  Best {args.top_n} matches in ep{args.episode}:")
        for rank, (score, si, sent) in enumerate(top, 1):
            word_preview = " ".join(sent["words"][:10])
            if len(sent["words"]) > 10:
                word_preview += " …"
            print(f"    #{rank}  score={score:.3f}  syl={sent['total_syl']}  "
                  f"words={len(sent['words'])}  dur={sent['duration']:.1f}s  "
                  f"spk={sent['speaker']}")
            print(f"         t=[{sent['t_start']:.1f}s–{sent['t_end']:.1f}s]")
            print(f"         text: \"{word_preview}\"")
            print(f"         per-word syls: {list(zip(sent['words'][:10], sent['syllables'][:10]))}")

    # ── Summary stats ─────────────────────────────────────────────────────────
    print()
    print("═" * 72)
    print("SUMMARY — syllable distribution of w2 sentences:")
    print("═" * 72)
    totals = sorted(s["total_syl"] for s in candidates)
    fact_totals = [fd["total"] for fd in fact_data]
    print(f"  w2 candidate sentences: {len(candidates)}")
    print(f"  Syllable range in w2:  {totals[0]} – {totals[-1]}")
    print(f"  Median syllables in w2: {totals[len(totals)//2]}")
    print(f"  Fact syllable totals:  {sorted(fact_totals)}")
    print()
    print("  Coverage: for each fact, what fraction of w2 sentences are")
    print("  within ±20% of the fact's total syllable count?")
    for fd in fact_data:
        lo = fd["total"] * 0.8
        hi = fd["total"] * 1.2
        n_in_range = sum(1 for s in candidates if lo <= s["total_syl"] <= hi)
        pct = 100.0 * n_in_range / len(candidates) if candidates else 0
        print(f"    \"{fd['text'][:45]}…\"  "
              f"syl={fd['total']}  "
              f"matches_within_20pct: {n_in_range} ({pct:.1f}%)")


if __name__ == "__main__":
    main()
