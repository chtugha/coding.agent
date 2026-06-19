# whisper.cpp Investigation for Word-Level Timestamps

## Date
2026-06-19

## Objective
Determine if whisper.cpp can provide word-level timestamps directly, eliminating the need for Montreal Forced Aligner (MFA) and potentially offering ultra-fast transcription with CoreML acceleration on Apple Silicon.

## Background

### Current Situation
- **MLX Whisper**: 1.91x realtime (much slower than expected 15-25x)
- **Timeline with MLX + MFA**: 13.5-14.8 days for 373 episodes
- **CPU WhisperX**: 62 days for 373 episodes (baseline)

### User's Insight
> "whisper.cpp is ultra fast with the large coreml model"

This suggests whisper.cpp with CoreML acceleration may be significantly faster than MLX Whisper.

## whisper.cpp Installation

### Location
- **Binary**: `./whisper-cpp/build/bin/whisper-cli`
- **Models**: `./bin/models/`

### Available Models
1. **ggml-large-v3-q5_0.bin** (1.0GB) - Quantized large-v3
2. **ggml-large-v3-turbo-q5_0.bin** (547MB) - Quantized turbo (faster)
3. **ggml-large-v3-encoder.mlmodelc** - CoreML encoder (Apple Silicon optimized)
4. **ggml-large-v3-turbo-encoder.mlmodelc** - CoreML turbo encoder

## Key Features for Timestamps

### Command-Line Options
```bash
-oj,       --output-json          # Basic JSON output
-ojf,      --output-json-full     # Full JSON with more details
-osrt,     --output-srt           # SRT subtitle format (segment timestamps)
-owts,     --output-words         # Word-level output (karaoke format)
-ocsv,     --output-csv           # CSV format
-dtw MODEL --dtw MODEL            # Compute token-level timestamps
-wt N,     --word-thold N         # Word timestamp probability threshold
```

### Critical Options for Our Use Case

1. **`-ojf` (output-json-full)**
   - Includes more information than basic JSON
   - May include word-level timestamps

2. **`-owts` (output-words)**
   - Described as "output script for generating karaoke video"
   - Karaoke requires word-level timing
   - **This is likely our solution!**

3. **`-dtw MODEL` (DTW alignment)**
   - Computes token-level timestamps
   - May require additional model file
   - Could provide even more precise timing

## Test Strategy

### Current Test (Running)
Testing 6 different output formats on episode 150 (first 5 minutes):

1. **basic_json** (`-oj`)
2. **full_json** (`-ojf`)
3. **srt** (`-osrt`)
4. **words** (`-owts`) ⭐ Most promising
5. **csv** (`-ocsv`)
6. **full_json_words** (`-ojf -owts`) ⭐ Best combination

### What We're Looking For

#### Ideal Output Format
```json
{
  "transcription": [
    {
      "start": 0.62,
      "end": 1.24,
      "text": "Unterfickt",
      "words": [
        {
          "word": "Unterfickt",
          "start": 0.62,
          "end": 1.24,
          "probability": 0.95
        }
      ]
    }
  ]
}
```

#### Minimum Acceptable Format
- Segment-level timestamps (start/end for phrases)
- Can use MFA for word-level alignment if needed
- But still much faster than MLX Whisper

## Expected Performance

### CoreML Acceleration Benefits
- **Apple Silicon optimized**: Uses Neural Engine + GPU
- **Reported speedup**: 20-50x realtime (user claims "ultra fast")
- **Memory efficient**: CoreML models are optimized for mobile/edge devices

### Estimated Timeline (if 20x speedup)
- **Per episode (90 min)**: 4.5 minutes transcription
- **373 episodes**: 28 hours = **1.2 days**
- **Plus MFA (if needed)**: 1.3-2.6 days
- **Total**: **2.5-3.8 days** (vs 13.5 days with MLX)

### Estimated Timeline (if 50x speedup)
- **Per episode (90 min)**: 1.8 minutes transcription
- **373 episodes**: 11 hours = **0.5 days**
- **Plus MFA (if needed)**: 1.3-2.6 days
- **Total**: **1.8-3.1 days** (vs 13.5 days with MLX)

## Comparison Matrix

| Solution | Speed | Word Timestamps | Total Time (373 eps) | Complexity |
|----------|-------|-----------------|---------------------|------------|
| CPU WhisperX | 0.25x | ✅ Native | 62 days | Low |
| MLX Whisper + MFA | 1.91x | ❌ Need MFA | 13.5-14.8 days | Medium |
| whisper.cpp (quantized) | ~10x | ❓ Testing | ~6-8 days | Low |
| whisper.cpp (CoreML) | ~20-50x | ❓ Testing | **1.8-3.8 days** | Low |

## Decision Criteria

### If whisper.cpp provides word-level timestamps:
✅ **USE whisper.cpp with CoreML**
- Fastest solution (1.8-3.8 days)
- Native word timestamps
- No MFA needed
- Simple pipeline

### If whisper.cpp only provides segment timestamps:
✅ **USE whisper.cpp + MFA**
- Still fast (2.5-3.8 days)
- Better than MLX Whisper (13.5 days)
- Proven MFA pipeline

### If whisper.cpp is slower than expected:
⚠️ **Fallback to MLX Whisper + MFA**
- Known working solution
- 13.5 days acceptable
- 4.6x faster than CPU

## Next Steps

1. ✅ Run timestamp format tests (in progress)
2. ⏳ Analyze output formats
3. ⏳ Measure actual CoreML speed
4. ⏳ Test on full 90-minute episode
5. ⏳ Make final decision
6. ⏳ Process all 373 episodes

## Questions to Answer

1. **Does `-owts` provide word-level timestamps?**
   - If YES: Perfect solution, no MFA needed
   - If NO: Check if `-ojf` has word data

2. **What is the actual CoreML speed?**
   - Measure on 5-minute segment
   - Extrapolate to 90-minute episodes
   - Compare to MLX Whisper (1.91x)

3. **Is the output format compatible with our pipeline?**
   - Can we parse it easily?
   - Does it match our JSON schema?
   - Can we convert if needed?

## Potential Issues

### Issue 1: CoreML Model Not Found
- May need to download/convert CoreML model
- Check if `.mlmodelc` files are complete
- May need to build from source

### Issue 2: Word Timestamps Not Available
- Fallback: Use segment timestamps + MFA
- Still faster than MLX Whisper
- Proven alignment pipeline

### Issue 3: Speed Not as Fast as Expected
- Test with different thread counts
- Try turbo model vs full model
- Check CoreML acceleration is active

## Success Metrics

### Minimum Success
- ✅ Faster than MLX Whisper (>1.91x)
- ✅ Provides segment timestamps
- ✅ Can use MFA for word alignment
- **Result**: <10 days for 373 episodes

### Ideal Success
- ✅ 20-50x realtime speed
- ✅ Native word-level timestamps
- ✅ No MFA needed
- **Result**: <4 days for 373 episodes

## Conclusion

whisper.cpp with CoreML acceleration is a **highly promising solution** that could:
1. Provide 10-25x speedup over MLX Whisper
2. Potentially include native word-level timestamps
3. Reduce total processing time from 13.5 days to 2-4 days
4. Simplify the pipeline (no MFA if word timestamps available)

**Current Status**: Testing in progress to determine timestamp format and actual speed.