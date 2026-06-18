# WhisperX Validation Results - Critical Analysis

## Executive Summary

**CRITICAL FINDING**: The MFA timestamps show an average difference of **2.4 seconds** compared to WhisperX re-transcription. This is **UNACCEPTABLE** for production use.

## Validation Results

### Overall Statistics
- **MFA words**: 341
- **WhisperX words**: 329
- **Word count difference**: 12 words (3.5%)
- **Average start difference**: 2,385.8ms (2.4 seconds)
- **Average end difference**: 2,411.3ms (2.4 seconds)
- **Max start difference**: 14,151ms (14.2 seconds)
- **Max end difference**: 14,891ms (14.9 seconds)

### Quality Assessment

| Metric | Threshold | Result | Status |
|--------|-----------|--------|--------|
| Average difference | <50ms (excellent) | 2,386ms | ❌ FAIL |
| Average difference | <100ms (good) | 2,386ms | ❌ FAIL |
| Average difference | <200ms (acceptable) | 2,386ms | ❌ FAIL |
| Max difference | <500ms | 14,891ms | ❌ FAIL |

## Root Cause Analysis

### Problem 1: Timestamp Drift Over Time

Looking at the "Worst 5" cases:
```
#    MFA Word    MFA Time        WX Time         Δ Start
186  hat         59.84-60.08     73.75-74.97     13,911ms
185  schwanz     59.48-59.84     73.63-73.73     14,151ms
184  kleinen     59.27-59.48     72.71-73.59     13,440ms
```

At the 60-second mark, MFA timestamps are **~14 seconds behind** WhisperX timestamps. This indicates:

1. **Cumulative drift**: The timestamps progressively diverge over time
2. **Systematic error**: Not random variation, but consistent offset

### Problem 2: Audio Preprocessing Mismatch

The MFA alignment was performed on **cleaned audio** (ads removed), but the timestamps may not have been properly adjusted after ad removal.

**Evidence**:
- First words show small differences (231ms, 32ms, 102ms)
- Later words show massive differences (13-14 seconds)
- This pattern suggests missing audio segments weren't accounted for

### Problem 3: Segment Boundary Issues

The MFA script processes segments independently and may not properly handle:
- Gaps between segments
- Overlapping speech
- Silence periods

## Critical Issues Identified

### Issue 1: Ad Removal Timestamp Adjustment
**Location**: [`add_word_timestamps_mfa_simple.py:243-250`](scripts/add_word_timestamps_mfa_simple.py:243-250)

The script saves cleaned audio but doesn't adjust timestamps to account for removed segments:

```python
# Save cleaned audio permanently
cleaned_audio_path = transcript_path.replace('.json', '_cleaned.wav')
sf.write(cleaned_audio_path, cleaned_audio, 16000)
print(f"  Saved cleaned audio: {os.path.basename(cleaned_audio_path)}")
```

**Problem**: MFA aligns to cleaned audio, but timestamps reference original audio timeline.

### Issue 2: Segment Time Offset Not Applied
**Location**: [`add_word_timestamps_mfa_simple.py:137-178`](scripts/add_word_timestamps_mfa_simple.py:137-178)

When distributing words across segments, the segment start time offset may not be correctly applied:

```python
def add_words_to_segments(segments, mfa_words):
    """Add word-level timestamps to segments."""
    # ... code distributes words but may not adjust for segment offsets
```

## Recommended Fix

### Option 1: Re-align Without Ad Removal (Recommended)
1. Keep ads in audio during MFA alignment
2. Remove ads AFTER alignment using original timestamps
3. Adjust word timestamps based on removed segments

### Option 2: Fix Timestamp Adjustment
1. Track all removed audio segments (start, end times)
2. After MFA alignment, adjust all word timestamps by subtracting removed durations
3. Validate adjusted timestamps against original audio

### Option 3: Use WhisperX Instead of MFA
1. WhisperX provides word-level timestamps directly
2. No separate alignment step needed
3. Timestamps are accurate to the source audio

## Impact on WER

The 2.4-second average timestamp error explains the high WER (13.78%):

1. **Chunk splitting occurs at wrong times**
   - Words are cut mid-sentence
   - Context is lost between chunks

2. **Speaker attribution errors**
   - Wrong speaker assigned due to timing mismatch
   - Affects left/right channel muting

3. **Transcript misalignment**
   - Text doesn't match audio timing
   - ASR model receives incorrect context

## Next Steps

1. ❌ **DO NOT** proceed with processing all 373 episodes using current MFA approach
2. ✅ **INVESTIGATE** the timestamp adjustment issue in MFA script
3. ✅ **CONSIDER** switching to WhisperX for word-level timestamps
4. ✅ **RE-VALIDATE** after fixing timestamp issues

## Conclusion

The MFA alignment produces timestamps that are **off by 2-14 seconds**, making them **unsuitable for production use**. The root cause appears to be improper handling of ad removal and segment boundaries.

**Recommendation**: Fix the timestamp adjustment logic or switch to WhisperX before processing the full dataset.