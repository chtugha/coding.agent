# V4 Test Script Audit - Episode 151

**Script:** `test_v4_ep151_with_mfa.py`  
**Date:** 2026-06-19

---

## Critical Issues Found

### 1. ⚠️ V4 Alignment Produces Low Correlation (0.292)

**Evidence from test run:**
```
Best alignment: offset=19.5s, correlation=0.292
WARNING: Low correlation - audio may not match transcript!
```

**Problem:**
- V4 detected correlation of only **0.292** (very low)
- V3 on episode 153 had correlation of **0.353** (also low but better)
- V3 on episodes 150-152 had much better correlations
- This suggests V4's downsampling is **losing critical alignment information**

**Impact:**
- V4 may be misaligning the entire episode
- MFA timestamps based on V4-cleaned audio will be incorrect
- Cannot trust V4 results for validation

### 2. ❌ V4 Didn't Remove Any Audio

**Evidence:**
```
Original: 5439.1s → Cleaned: 5439.1s
```

**Problem:**
- V4 removed **0 seconds** of audio (no intro/outro detected)
- V3 on episode 153 removed 46s
- V4 on episode 153 removed 14s
- Episode 151 should have intro/outro like all podcast episodes

**Root Cause:**
- Low correlation (0.292) means V4 couldn't find a good alignment
- V4 defaulted to keeping all audio when alignment failed
- This is a **safety mechanism** but indicates alignment failure

### 3. ⚠️ WhisperX Offset May Be Wrong

**Issue in script (line 152):**
```python
whisperx_offset = 0.23
```

**Problem:**
- This offset was calculated for **V3-cleaned episode 151**
- We're now using **V4-cleaned episode 151** (different alignment!)
- The offset will be different because V4 removed 0s vs V3's removal
- Applying wrong offset will corrupt the comparison

### 4. ❌ Comparison Logic Assumes Same Audio

**Issue in script (lines 157-180):**
```python
# Align words by matching text
matched_pairs = []
mfa_idx = 0
wx_idx = 0
```

**Problem:**
- Script assumes MFA and WhisperX are aligning the **same audio**
- But WhisperX was run on **V3-cleaned audio** (with intro removed)
- MFA is being run on **V4-cleaned audio** (no intro removed)
- They're aligning **different audio files** with different offsets!

**Impact:**
- Word matching will fail or produce meaningless results
- Timestamps won't align because they're from different audio sources

---

## Fundamental Design Flaw

### The Core Problem

The script is trying to:
1. Clean audio with V4
2. Run MFA on V4-cleaned audio
3. Compare MFA timestamps to WhisperX timestamps

**But WhisperX was run on V3-cleaned audio, not V4-cleaned audio!**

This is like:
- Person A measures a 10-meter rope
- Person B cuts 2 meters off the rope
- Person C measures the 8-meter rope
- Then comparing Person A's and Person C's measurements

They're measuring **different things**!

---

## What Should Happen

### Correct Approach

To properly test V4, we need to:

1. **Run V4 alignment** on episode 151
2. **Run WhisperX** on V4-cleaned audio (not use old V3-based WhisperX results)
3. **Run MFA** on V4-cleaned audio
4. **Compare MFA to WhisperX** (both on same V4-cleaned audio)

OR

1. Use existing **V3-cleaned audio** from previous test
2. Use existing **WhisperX results** from V3-cleaned audio
3. Use existing **MFA results** from V3-cleaned audio
4. **Compare those** (all from same V3-cleaned audio)

---

## Specific Code Issues

### Issue 1: Wrong WhisperX File
**Line 23:**
```python
whisperx_ground_truth = "/tmp/fixed_alignment_test/episode_151_whisperx_validation.json"
```

**Problem:** This file contains WhisperX results from **V3-cleaned audio**, not V4-cleaned audio.

**Fix:** Either:
- Run WhisperX on V4-cleaned audio first
- Or use V3-cleaned audio for everything

### Issue 2: Wrong Offset
**Line 152:**
```python
whisperx_offset = 0.23
```

**Problem:** This offset is for V3-cleaned audio, not V4-cleaned audio.

**Fix:** Calculate new offset for V4-cleaned audio, or don't use offset if comparing same audio source.

### Issue 3: No Validation of V4 Success
**Lines 40-43:**
```python
cleaned_audio, kept_regions = align_audio_to_transcript_fast(audio, sr, segments)
align_time = time.time() - start_time
print(f"   Alignment complete in {align_time:.2f}s")
print(f"   Original: {len(audio)/sr:.1f}s → Cleaned: {len(cleaned_audio)/sr:.1f}s")
```

**Problem:** Script doesn't check if V4 alignment succeeded (correlation score, amount removed, etc.)

**Fix:** Add validation:
```python
if len(cleaned_audio) == len(audio):
    print("   WARNING: V4 removed no audio - alignment may have failed!")
    print("   Aborting test - V4 alignment unsuccessful")
    exit(1)
```

### Issue 4: MFA Will Fail on Misaligned Audio
**Lines 68-76:**
```python
mfa_cmd = [
    "mfa", "align",
    "--clean",
    "--single_speaker",
    mfa_input_dir,
    "german_mfa",
    "german_mfa",
    "/tmp/fixed_alignment_test/mfa_v4_output"
]
```

**Problem:** If V4 alignment failed (correlation 0.292, removed 0s), the audio doesn't match the transcript. MFA will either:
- Fail completely
- Produce garbage timestamps
- Take very long time trying to align mismatched audio/text

---

## Recommendations

### Option 1: Fix V4 First (Recommended)
1. **Don't run this script yet**
2. **Debug why V4 has low correlation** (0.292)
3. **Fix V4 algorithm** to work correctly on episode 151
4. **Re-test V4** on episode 151 until correlation > 0.35 and it removes intro
5. **Then** run this script with fixed V4

### Option 2: Test V3 Instead
1. **Use existing V3-cleaned audio** from `/tmp/fixed_alignment_test/episode_151_cleaned_v3.wav`
2. **Use existing WhisperX results** (already have them)
3. **Use existing MFA results** from previous V3 test
4. **Compare those** - they're all from same audio source

### Option 3: Run WhisperX on V4 Audio
1. **Keep V4 alignment as-is** (even though it failed)
2. **Run WhisperX on V4-cleaned audio** (will take 3+ hours)
3. **Run MFA on V4-cleaned audio**
4. **Compare MFA to WhisperX** (both on V4 audio)
5. **Document that V4 failed** to align properly

---

## Conclusion

### Script Status: ❌ NOT READY TO RUN

**Reasons:**
1. V4 alignment failed (correlation 0.292, removed 0s)
2. Comparing timestamps from different audio sources (V3 vs V4)
3. Using wrong offset (0.23s is for V3, not V4)
4. No validation of V4 success before proceeding

### Recommended Action

**STOP and choose:**

**A) Fix V4 algorithm** - Debug why correlation is so low on episode 151
- Check if downsampling is losing information
- Compare V3 vs V4 pattern generation
- Test on episode 151 specifically

**B) Use V3 for everything** - V3 is proven to work (0.05-0.5% accuracy)
- Already have V3-cleaned audio
- Already have WhisperX results on V3 audio
- Already have MFA results on V3 audio
- Just need to compare them properly

**C) Accept V4 failure** - Run WhisperX on V4 audio to document the failure
- Will take 3+ hours
- Will show V4 produces worse results than V3
- Confirms V3 is superior

---

**My Recommendation: Option B - Use V3**

V3 is already validated and working. We have all the data we need. Just need to properly compare MFA (V3) to WhisperX (V3) using the existing results.
