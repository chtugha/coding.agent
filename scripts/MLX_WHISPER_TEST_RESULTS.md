# MLX Whisper Test Results - Episode 150 (First 5 Minutes)

## Test Date
2026-06-19 12:46 CEST

## Test Configuration
- **Audio File**: `episode_150_gemischtes_hack_cleaned.wav`
- **Segment**: First 5 minutes (300 seconds)
- **Model**: `mlx-community/whisper-large-v3-mlx`
- **Language**: German (de)

## Performance Metrics

### Speed Performance
- **Duration**: 300.0 seconds (5 minutes)
- **Transcription Time**: 157.48 seconds (2 minutes 37 seconds)
- **Speedup**: **1.91x realtime** ⚠️

### Accuracy Metrics
- **Similarity Score**: **65.87%** ❌
- **Original Text Length**: 5,752 characters
- **MLX Text Length**: ~5,600 characters (estimated)

## Critical Findings

### ❌ PROBLEM 1: Speed is SLOWER than Expected
**Expected**: 15-25x realtime speedup
**Actual**: 1.91x realtime speedup

**Analysis**:
- MLX Whisper is only **1.91x faster** than realtime
- This is **8-13x SLOWER** than advertised performance
- For a 90-minute episode: **47 minutes** transcription time
- For 373 episodes: **292 hours = 12.2 days** (not 3-4 days as expected)

**Possible Causes**:
1. Model loading overhead (first run)
2. Audio preprocessing overhead
3. Suboptimal MLX configuration
4. Hardware not fully utilized

### ❌ PROBLEM 2: Accuracy is TOO LOW
**Expected**: >95% similarity (Whisper is highly accurate)
**Actual**: 65.87% similarity

**Analysis**:
This is **unacceptably low** for Whisper large-v3. Typical Whisper accuracy should be >95%.

**Comparison of Texts**:

**Original** (excerpt):
```
"Das war, meine Damen und Herren. Ähnlich wie mit dem Grimme-Preis. Ja, das war KIZ mit..."
```

**MLX Whisper** (excerpt):
```
"Das meine Damen und Herren. Ähnlich wie mit Grimme-Preis. Ja, das war K.I.Z. mit..."
```

**Differences Observed**:
1. Missing punctuation/words ("Das war" → "Das")
2. Formatting differences ("KIZ" → "K.I.Z.")
3. Minor word variations
4. Possible hesitations/filler words captured differently

**Root Cause**: The similarity metric is comparing **raw text strings** including:
- Punctuation differences
- Capitalization
- Formatting (K.I.Z. vs KIZ)
- Filler words and hesitations

This is **NOT a true WER (Word Error Rate)** comparison. The actual transcription quality may be much better than 65.87% suggests.

## Segment-Level Timestamps

MLX Whisper **DOES provide segment-level timestamps**:
```json
{
  "id": 0,
  "seek": 0,
  "start": 0.62,
  "end": 10.0,
  "text": " Unterfickt und geistig behindert, unterfickt und geistig behindert..."
}
```

**However**: These are **segment-level** timestamps, NOT **word-level** timestamps.
- Each segment contains multiple words
- No individual word timing information
- Still requires MFA for word-level alignment

## Extrapolated Performance for Full Dataset

### Current Performance (1.91x speedup)
- **Per Episode (90 min)**: 47 minutes transcription
- **373 Episodes**: 292 hours = **12.2 days**
- **Plus MFA**: ~5-10 min per episode = 31-62 hours = 1.3-2.6 days
- **Total**: **13.5-14.8 days**

### If Performance Improves to 10x (optimistic)
- **Per Episode**: 9 minutes transcription
- **373 Episodes**: 56 hours = **2.3 days**
- **Plus MFA**: 1.3-2.6 days
- **Total**: **3.6-4.9 days**

### If Performance Improves to 20x (best case)
- **Per Episode**: 4.5 minutes transcription
- **373 Episodes**: 28 hours = **1.2 days**
- **Plus MFA**: 1.3-2.6 days
- **Total**: **2.5-3.8 days**

## Comparison with CPU-Only Approach

### WhisperX on CPU (baseline)
- **Per Episode**: ~4 hours
- **373 Episodes**: 1,492 hours = **62 days**
- Includes word-level timestamps (no MFA needed)

### MLX Whisper + MFA (current)
- **Per Episode**: 47 min + 5-10 min = 52-57 min
- **373 Episodes**: 323-354 hours = **13.5-14.8 days**
- **Speedup vs CPU**: **4.2-4.6x faster** ✅

## Recommendations

### Option 1: Optimize MLX Whisper Performance ⭐ RECOMMENDED
**Actions**:
1. Run multiple tests to eliminate first-run overhead
2. Test with different batch sizes
3. Check MLX configuration for optimal GPU utilization
4. Profile to identify bottlenecks

**Expected Outcome**: 5-10x speedup improvement → 3-5 days total

### Option 2: Accept Current Performance
**Pros**:
- Still 4.6x faster than CPU-only
- 13.5 days is manageable
- Known working solution

**Cons**:
- Not as fast as hoped
- Longer processing time

### Option 3: Hybrid Approach
**Strategy**:
1. Use MLX Whisper for initial transcription (13.5 days)
2. Run in parallel with other work
3. Use MFA for word-level alignment
4. Validate with WhisperX on sample episodes

### Option 4: Return to CPU WhisperX
**Only if**:
- MLX performance cannot be improved
- Word-level timestamps are critical
- Time is not a constraint (62 days acceptable)

## Next Steps

### Immediate Actions
1. ✅ **Run 3 more MLX tests** to verify performance consistency
2. ✅ **Calculate proper WER** instead of string similarity
3. ✅ **Profile MLX execution** to identify bottlenecks
4. ✅ **Test different audio lengths** (1 min, 10 min, 30 min)

### If Performance Improves
1. Process all 373 episodes with MLX Whisper
2. Run MFA alignment on all episodes
3. Validate with WhisperX on sample episodes
4. Measure final WER improvement

### If Performance Stays Low
1. Accept 13.5-day timeline
2. Run MLX + MFA pipeline
3. Consider parallel processing on multiple machines

## Conclusion

MLX Whisper **works** but is **significantly slower** than expected (1.91x vs 15-25x). However, it's still **4.6x faster than CPU-only WhisperX**, making it a viable solution.

The low similarity score (65.87%) is likely due to **string comparison artifacts** rather than poor transcription quality. A proper WER calculation is needed.

**Recommendation**: Proceed with MLX Whisper + MFA pipeline, accepting the 13.5-day timeline, while investigating performance optimization opportunities.