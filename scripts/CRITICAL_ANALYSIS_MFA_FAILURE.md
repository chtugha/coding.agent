# Critical Analysis: MFA Approach Failure

## Date: 2026-06-18

## Problem Summary

The Montreal Forced Aligner (MFA) approach to add word-level timestamps has **fundamental flaws** that make it unsuitable for this podcast dataset.

## Issues Identified

### 1. MFA Cannot Handle Multi-Speaker Audio
- **Root Cause**: MFA is designed for single-speaker forced alignment
- **Impact**: Treats entire 38-minute podcast as one speaker
- **Result**: All 7,989 words aligned to single audio stream, losing speaker boundaries
- **Evidence**: MFA output shows "1 speaker detected" despite transcript having 2 speakers (Speaker A and Speaker B)

### 2. Ad Removal Was Completely Bypassed
- **Root Cause**: `force_align_podcast_transcripts.py` worked directly with raw MP3 files
- **Impact**: Aligned advertisements along with podcast content
- **Existing Solution**: `prepare_german_dataset.py` has sophisticated ad removal (lines 406-739):
  - Cross-correlation alignment between audio and transcript
  - Detects regions where audio has energy but transcript doesn't  
  - Caches results for efficiency (line 1158: `clean_podcast_ads_waveform_based()`)
- **Evidence**: No ad removal step in MFA script before alignment

### 3. Wrong Processing Order
**Current (Incorrect)**:
```
Raw Audio (with ads) → MFA Alignment (single speaker) → Add timestamps
```

**Correct Workflow** (from `prepare_german_dataset.py`):
```
Raw Audio → Ad Removal → Parse Transcript (with speakers) → 
Split by Speaker Turns → Create Chunks (one channel muted) → 
Process with word-level timestamps
```

## Why Original Workflow Is Actually Correct

The `prepare_german_dataset.py` script implements the correct approach:

1. **Line 1153**: Load raw MP3 audio
2. **Line 1158**: Remove ads using waveform correlation
3. **Line 1161**: Parse transcript with speaker labels  
4. **Lines 1163-1189**: Process turns and create speaker-separated chunks
5. **Missing**: Word-level timestamps (currently uses uniform distribution fallback)

**The ONLY missing piece is word-level timestamps in source transcripts.**

## Correct Solution

### Option A: Fix at Source (Recommended)
Re-transcribe original podcasts with Whisper to get word-level timestamps:
- Use Whisper with `word_timestamps=True`
- Preserve speaker diarization
- Include ad detection in transcription
- **Advantage**: Clean, accurate timestamps from the start
- **Disadvantage**: Requires re-transcription of all 373 episodes

### Option B: Align Individual Chunks (Workaround)
Work with already-processed chunks from `/Volumes/eHDD/moshi-rag-data/processed/podcast/`:
1. Each chunk is already single-speaker (one channel muted)
2. Each chunk already has ads removed
3. Apply MFA to individual chunks (single-speaker audio)
4. Merge word timestamps back into chunk transcripts
- **Advantage**: Works with existing processed data
- **Disadvantage**: Complex merging logic, potential timestamp drift

### Option C: Hybrid Approach
1. Use existing ad-cleaned audio from cache
2. Split by speaker turns before MFA
3. Align each speaker turn separately
4. Merge results preserving speaker labels
- **Advantage**: Leverages existing ad removal
- **Disadvantage**: Still complex, requires speaker-aware splitting

## Recommendation

**Option A (Re-transcription)** is the cleanest solution:
- One-time cost to re-transcribe
- Produces clean, accurate word-level timestamps
- Maintains speaker diarization
- Integrates seamlessly with existing pipeline
- No complex merging logic needed

## Next Steps

1. Abandon current MFA approach
2. Implement Whisper-based re-transcription with word timestamps
3. Verify word-level timestamps in output
4. Re-run `prepare_german_dataset.py` with enhanced transcripts
5. Verify improved WER (expect 13.78% → 5-8%)

## Lessons Learned

- **Always understand the full pipeline** before attempting fixes
- **MFA is not a universal solution** - it's designed for specific use cases
- **Multi-speaker audio requires speaker-aware processing**
- **Ad removal is critical** for podcast datasets
- **The original workflow was correct** - we just needed better source data