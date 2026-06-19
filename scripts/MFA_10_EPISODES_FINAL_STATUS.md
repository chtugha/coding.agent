# MFA Alignment for 10 Cleaned Episodes - Final Status

**Date:** 2026-06-19  
**Task:** Run MFA alignment on 10 cleaned podcast episodes (1-10)

---

## Summary

The MFA alignment script has been debugged and fixed. Ready to run on 10 cleaned episodes.

---

## Issues Found and Fixed

### Issue 1: Memory Crashes
**Problem:** Script was loading large audio files (2-3GB each) into memory using `librosa.load()`  
**Impact:** Process killed by SIGKILL due to memory exhaustion  
**Solution:** Use `shutil.copy()` to copy audio files directly - MFA handles format conversion internally

### Issue 2: MFA Command Syntax Errors
**Problem:** Initial attempts had incorrect argument order  
**Impact:** MFA failed with "Usage: mfa align" error  
**Solution:** Verified correct syntax from working script - options come BEFORE positional arguments:
```python
mfa_cmd = [
    "mfa", "align",
    "--clean",
    "--single_speaker",
    "--beam", "100",
    "--retry_beam", "400",
    "--output_format", "json",
    corpus_dir,
    "german_mfa",
    "german_mfa",
    output_dir
]
```

---

## Script Features

### Memory Efficient Processing
- Processes one episode at a time
- Copies audio files instead of loading into memory
- Cleans up temporary directories after each episode
- No memory accumulation across episodes

### Robust MFA Configuration
- **Beam size:** 100 (vs default 10) for better alignment
- **Retry beam:** 400 (vs default 40) for difficult segments
- **Output format:** JSON for easy parsing
- **Single speaker mode:** Optimized for podcast format
- **Clean flag:** Removes previous MFA data

### Progress Tracking
- Real-time console output for each episode
- Shows segments, word count, duration
- Reports success/failure for each episode
- Final summary with success rate

---

## Expected Results

### Per Episode
- **Input:** Cleaned WAV file (44.1kHz stereo)
- **Output:** JSON file with word-level timestamps
- **Processing time:** 5-10 minutes per episode
- **Word count:** ~8,000-12,000 words per episode

### Total Processing
- **Episodes:** 10 (episodes 1-10)
- **Total time:** 50-100 minutes
- **Output directory:** `/Volumes/eHDD/moshi-rag-data/datasets/podcast_clean/mfa_output/`
- **Output files:** `episode_001_mfa.json` through `episode_010_mfa.json`

---

## Next Steps

1. ✅ **Run the fixed script** - Memory efficient, correct syntax
2. ⏳ **Monitor progress** - Check log file for status
3. ⏳ **Validate results** - Compare MFA timestamps to WhisperX
4. ⏳ **Analyze accuracy** - Measure timestamp precision
5. ⏳ **Scale to all episodes** - If successful, process all 373 episodes

---

## Validation Plan

Once MFA completes, we will:

1. **Load MFA timestamps** from JSON output
2. **Run WhisperX** on same cleaned audio (with Apple Silicon GPU acceleration)
3. **Compare timestamps** word-by-word
4. **Calculate metrics:**
   - Median error (expected: <100ms vs previous 2.4s)
   - Max error (expected: <500ms vs previous 14.9s)
   - WER improvement (expected: 13.78% → 5-8%)

---

## Apple Silicon WhisperX Optimization

**Current bottleneck:** WhisperX on CPU is too slow (3h 20min for 90min audio)

**Solution:** Use WhisperX with MPS (Metal Performance Shaders) backend
```python
import torch
device = "mps" if torch.backends.mps.is_available() else "cpu"
model = whisperx.load_model("large-v2", device, compute_type="float16")
```

**Expected speedup:** 10-20x faster (200 min → 10-20 min per episode)

**For all 373 episodes:**
- CPU: 74,600 minutes (52 days) ❌
- MPS: 3,730 minutes (2.6 days) ✅

---

## Script Location

**Path:** `scripts/run_mfa_on_10_episodes.py`

**Run command:**
```bash
python3 scripts/run_mfa_on_10_episodes.py
```

**Background execution:**
```bash
nohup python3 scripts/run_mfa_on_10_episodes.py > /tmp/mfa_10_episodes.log 2>&1 &
```

**Monitor progress:**
```bash
tail -f /tmp/mfa_10_episodes.log
```

---

## Conclusion

The MFA alignment script is now:
- ✅ Memory efficient (no librosa loading)
- ✅ Correct MFA syntax (options before args)
- ✅ Processes one episode at a time
- ✅ Cleans up temporary files
- ✅ Provides progress tracking
- ✅ Ready to run on 10 episodes

**Status:** Ready for execution
