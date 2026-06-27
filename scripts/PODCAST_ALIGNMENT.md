# Podcast → Moshi Dataset: Word-Level Timestamp Alignment

`scripts/podcast_to_moshi_dataset.py` — Phase 1 of the Moshi fine-tune pipeline.

Converts raw Gemischtes Hack Podcast MP3s into word-level aligned transcripts
by fusing whisper-cpp's audio-grounded timestamps with the existing diarized
transcript (which has correct speaker labels and clean text but timestamps
relative to ad-stripped audio).

---

## The problem

The diarized transcripts were produced on clean audio (intro jingle and ad
breaks already removed). The raw MP3 files contain the full broadcast including
intro (~34s for ep152) and one or more ad breaks. This means every transcript
timestamp is shifted by an unknown amount relative to the raw audio, and the
shift increases by the length of each ad break.

```
Raw MP3:   [jingle 34s][content...][StepStone ad 9s][more content...]
Transcript:             [content...t=0               ][more content...]
                        ↑ offset ≈ +34s               ↑ offset ≈ +43s
```

---

## Pipeline

### Step 1 — Full transcription (`step1_transcribe`)

Runs whisper-cpp on the entire raw MP3 (no stripping, no chunking). Produces a
flat word list with audio-grounded timestamps. Saved to
`whisper_cache/ep{N}_w1.json`. Cached — skipped on re-runs if the file exists.

**Model:** `ggml-large-v3.bin` with CoreML encoder
(`bin/models/ggml-large-v3-encoder.mlmodelc`) for Apple Neural Engine
acceleration. The binary at `whisper-cpp/build/bin/whisper-cli` was compiled
with `-DWHISPER_COREML=ON`; CoreML is activated automatically when the
`-encoder.mlmodelc` directory exists next to the model file.

**Token merging:** whisper-cpp emits sub-word BPE tokens. A space-prefix rule
merges them back into words: a new word starts whenever a token begins with a
space character.

**w1.json schema:**
```json
[{"word": "Gemischtes", "start": 0.12, "end": 0.51, "p": 0.97}, ...]
```

---

### Step 2 — Alignment (`step2_align`)

The core of the pipeline. Produces `aligned_cache/ep{N}_aligned.json`.

#### 2a. Bootstrap offset

Before scanning segments, we need an initial estimate of the
raw\_time − transcript\_time offset (δ).

**Primary strategy** — LCS first-segment alignment:
For each of the first 40 transcript segments with ≥4 words, run a true LCS
search within the first 180 s of whisper output. Collect all candidates and
return the one with the **earliest raw timestamp** — the first content to appear
in the whisper stream is the most reliable anchor because it is as close to
the jingle boundary as possible.

Searching only the first 180 s prevents a false match from a repeated phrase
deep in the episode (which previously produced an offset of +2034 s with the
turbo model).

**Fallback** — exact 4-gram scan: if LCS finds nothing (all early segments are
very short), scan for the first exact 4-word run shared between transcript and
whisper within the same 180 s window.

#### 2b. Forward scan with recalibration

Iterates transcript segments in order. For each segment with ≥4 words:

1. **Tight window search** (±12 s around `t_segment + offset`):
   Uses true DP LCS over all whisper words in the window. Returns the whisper
   span whose LCS score against the transcript segment exceeds a per-length
   threshold:

   | segment words | min score |
   |---|---|
   | ≤ 4 | 0.75 |
   | ≤ 6 | 0.67 |
   | ≤ 10 | 0.60 |
   | > 10 | 0.55 |

   On a match: update offset from `whisper_match_start − t_segment`, advance
   cursor to 2 s before match end (boundary overlap).

2. **Recalibration** (after ≥8 consecutive misses):
   The offset has likely drifted (gradual whisper timing drift) or an ad break
   has shifted it. Search ±60 s around the expected position, using the current
   cursor as a floor (never rewinds). Accept the match regardless of jump
   direction or magnitude and update offset + cursor. This fires ~once per real
   ad break and self-corrects without oscillation.

3. **Queue for back-fill**: if both tight and recal searches fail, append to
   `pending_miss`.

4. **Back-fill** on every successful match: walk `pending_miss` in reverse
   order (closest to the new anchor first), search each with a ±12 s window
   capped at `abs_start` of the current anchor to enforce segment ordering.

**Segments with <4 words** are never queued; they are handled entirely by the
interpolation pass (see below).

#### 2c. Interpolation

After the forward scan, any segment that still has no raw timestamp (missed by
both tight search and back-fill) gets its position linearly interpolated between
its nearest matched neighbours, using transcript time as the parameter.

#### 2d. Segment windows and gap closure

Each segment is assigned a raw time window `[raw_start, raw_end]`. After
building all windows, **cracks** between adjacent windows are closed by
extending each window's end to the next window's start. Without this step,
whisper words belonging to unmatched-but-real content would fall into cracks
and be misclassified as out-of-transcript, creating false gap regions.

Gap detection then runs on the closed windows: any span between consecutive
windows ≥8 s that is not covered by any window is a real gap (intro jingle or
ad break).

#### 2e. Word assignment

Each whisper word is assigned to a segment window by bisect lookup. Words in
detected gap regions (intro/ad/outro) get `out_of_transcript: True`; all others
get the speaker label and segment text of their containing window.

**aligned.json schema:**
```json
[
  {
    "word": "goes,",
    "start": 34.74,
    "end": 34.99,
    "p": 0.92,
    "speaker": "SPEAKER_00",
    "seg_text": "But so it goes, turning into some so-and-sos...",
    "out_of_transcript": false
  },
  ...
]
```

**ep152 results (large-v3 + CoreML):**

| Metric | Value |
|---|---|
| Words assigned | 11,996 / 12,021 (99.8%) |
| Words excluded | 25 (StepStone ad only) |
| Gap regions | 2 (36 s intro + 9 s ad) |
| Recalibrations | 1 (at known ad break t≈1403 s) |
| Bootstrap offset | +36.17 s |

---

### Step 3 — Stripped audio re-run (`step3_rerun_compare`)

Builds a stripped MP3 (ffmpeg concat of keep-regions from step 2), re-runs
whisper-cpp on it, then refines each assigned word's timestamp by matching
it to the closest w2 word within ±3 s. Words that can't be matched keep their
w1 timestamps.

w2 timestamps (relative to stripped audio start) are remapped to raw MP3 time
via the keep-region boundaries. The refined alignment overwrites
`aligned_cache/ep{N}_aligned.json`.

Skip with `--skip-step3` when iteration speed matters (step 3 takes ~70 min
for large-v3 on a 70-min episode).

---

## Usage

```bash
# Single episode — all 3 steps
python3 scripts/podcast_to_moshi_dataset.py --episodes 152

# Range
python3 scripts/podcast_to_moshi_dataset.py --episodes 150-152

# Comma list
python3 scripts/podcast_to_moshi_dataset.py --episodes 150,152

# Skip step 3 (fast iteration on alignment only)
python3 scripts/podcast_to_moshi_dataset.py --episodes 152 --skip-step3
```

Set `DEBUG = True` inside `step2_align` for per-segment `MATCH` / `MISS` /
`BACKFILL` output.

---

## File layout

```
bin/models/
  ggml-large-v3.bin                  # unquantized whisper large-v3 (2.9 GB)
  ggml-large-v3-encoder.mlmodelc/    # CoreML encoder compiled from mlpackage

whisper-cpp/
  build/bin/whisper-cli              # built with -DWHISPER_COREML=ON
  models/
    coreml-encoder-large-v3.mlpackage/  # source mlpackage (1.2 GB)

/Volumes/eHDD/moshi-rag-data/datasets/
  Gemischtes.Hack.Podcast/
    transcripts/episode_NNN_*.json   # diarized transcripts (read-only)
    #NNN *.mp3                       # raw episode MP3s (read-only)
  whisper_cache/
    ep{N}_w1.json                    # step 1 output (word list, cached)
    ep{N}_w2.json                    # step 3 output (stripped-audio words)
  aligned_cache/
    ep{N}_aligned.json               # final aligned word list
```

---

## Design decisions and audit history

**Why true DP LCS, not greedy subsequence matching?**
The original `lcs_score` used a greedy forward scan that required `t_norm[0]`
to appear in the whisper chunk. If whisper dropped the first word(s) of a
segment (common at intro/ad boundaries), the segment scored 0.0. True DP LCS
matches in any order-preserving subsequence, scoring correctly regardless of
which words whisper dropped.

**Why LCS bootstrap over 4-gram scan?**
The 4-gram scan was a workaround for broken LCS. With true LCS, first-segment
alignment is strictly better: it scores the best-matching position in the
window, tolerates dropped words, and is immune to accidental 4-gram repeats
mid-episode (which caused a +2034 s false offset with the large-v3 model).

**Why recalibration instead of a forward-only wide window?**
A forward-only wide window anchored at `cursor_time + WIDE_WIN` oscillated
wildly: each ad-break false-positive pushed cursor forward, making the next
search center too far ahead. Consecutive-miss recalibration fires exactly once
per real structural shift (ad break or accumulated drift) and self-corrects.

**Why crack-closing on segment windows?**
Without it, whisper words belonging to unmatched segments between two matched
anchors fell into cracks between windows and were mis-classified as
out-of-transcript. This inflated the excluded word count from ~25 (correct) to
~1,365 and produced 5 false gap regions. The fix: extend each window's end to
the next window's start before gap detection.
