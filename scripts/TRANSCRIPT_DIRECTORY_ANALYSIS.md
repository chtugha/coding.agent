# Transcript Directory Analysis

## Investigation Date
2026-06-18

## Directories Analyzed

1. `/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/transcripts/`
2. `/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/Gemischtes.Hack.Podcast.Transcript/transcripts/`

## Key Findings

### Both Directories Are IDENTICAL

After comparing `episode_001_gemischtes_hack.json` from both locations:

- **Same content**: Identical text, timestamps, and speaker labels
- **Same structure**: Both use segment-level organization
- **Same metadata**: Episode info, transcription details

### Critical Discovery: NO Word-Level Timestamps

**Both transcript directories lack word-level timestamps!**

#### Current Structure (Both Locations):
```json
{
  "segments": [
    {
      "id": 0,
      "start": 0.0,
      "end": 1.92,
      "text": " Ich habe dieses Gesicht, wo man immer lachen muss.",
      "speaker": "Speaker A"
      // NO "words" array!
    }
  ]
}
```

#### Required Structure (Missing):
```json
{
  "segments": [
    {
      "id": 0,
      "start": 0.0,
      "end": 1.92,
      "text": " Ich habe dieses Gesicht, wo man immer lachen muss.",
      "speaker": "Speaker A",
      "words": [
        {"word": "Ich", "start": 0.0, "end": 0.15},
        {"word": "habe", "start": 0.15, "end": 0.35},
        {"word": "dieses", "start": 0.35, "end": 0.65},
        // ... etc
      ]
    }
  ]
}
```

## Differences Between Directories

### Directory 1: `/transcripts/` (Root Level)
- Simpler structure
- No `meta` section at root level
- Only `text` and `segments` fields
- Appears to be a copy or simplified version

### Directory 2: `/Gemischtes.Hack.Podcast.Transcript/transcripts/`
- Has `meta` section with:
  - `episode_number`
  - `title` (e.g., "#1 GEMISCHTES HACK")
  - `pub_date`
  - `filename`
  - `model` used for transcription
  - `language`
  - `transcribed_at` timestamp
  - `duration_seconds`
  - `num_speakers`
  - `has_diarization`
- More organized structure
- **This is the ORIGINAL source**

## Root Cause Confirmation

This confirms our earlier finding that the high WER (13.78%) is caused by:

1. **Original transcripts lack word-level timestamps**
2. **`prepare_german_dataset.py` (lines 231-254)** detects missing `words` array
3. **Falls back to uniform word distribution** (inaccurate)
4. **Chunk splitting occurs at wrong boundaries** (mid-word/mid-sentence)
5. **Verification transcription differs from chunk transcripts** (high WER)

## Impact on Dataset Preparation

### Current Flow (Problematic):
```
Original Transcripts (segment-level only)
    ↓
prepare_german_dataset.py (uniform distribution fallback)
    ↓
Inaccurate chunk boundaries
    ↓
High WER (13.78%)
```

### Required Flow (Solution):
```
Original Transcripts (segment-level only)
    ↓
Montreal Forced Aligner (add word-level timestamps)
    ↓
Enhanced Transcripts (with word-level timestamps)
    ↓
prepare_german_dataset.py (accurate splitting)
    ↓
Accurate chunk boundaries
    ↓
Low WER (expected 5-8%)
```

## Recommendation

**Use Directory 2 as source**: `/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/Gemischtes.Hack.Podcast.Transcript/transcripts/`

**Reasons**:
1. Contains complete metadata
2. Has episode titles needed for audio file matching
3. More organized structure
4. Clearly the original source

## Next Steps

1. Run Montreal Forced Aligner on transcripts from Directory 2
2. Generate word-level timestamps for all episodes
3. Re-run `prepare_german_dataset.py` with enhanced transcripts
4. Verify improved WER (expect 50% reduction: 13.78% → 5-8%)

## Files Count

Both directories contain **373 transcript files** (episodes 001-373).