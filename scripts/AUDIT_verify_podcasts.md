# Audit Report: verify_podcasts.py Script

## Date: 2026-06-18
## Auditor: Bob (AI Code Assistant)

---

## Executive Summary

This audit evaluates whether `verify_podcasts.py` meets all requirements for verifying the processed podcast dataset chunks created by `prepare_german_dataset.py`.

---

## Requirements Checklist

### 1. Audio Format Verification (48 kHz / 16 Bit / Stereo PCM)

**Status: ❌ MISSING**

**Finding:**
- The script uses `sf.info(wav_path)` to get duration (line 249) but **does NOT verify**:
  - Sample rate is 48000 Hz
  - Bit depth is 16-bit
  - Format is PCM
  - Audio is stereo (2 channels)

**Impact:** Critical - Cannot confirm files meet the required audio specifications.

**Recommendation:** Add audio format validation before processing each file.

---

### 2. Double Mono Verification (Same Audio on Both Channels)

**Status: ❌ MISSING**

**Finding:**
- The script does NOT verify that both stereo channels contain identical audio (double mono)
- Lines 134-138 extract the active channel based on filename (_main or _other) but don't verify the inactive channel is muted

**Impact:** High - Cannot confirm proper channel muting as specified in requirements.

**Recommendation:** Add verification that:
- For `_main` files: left channel has audio, right channel is silent (all zeros)
- For `_other` files: right channel has audio, left channel is silent (all zeros)

---

### 3. Transcript JSON Format Compatibility

**Status: ⚠️ PARTIAL**

**Finding:**

**Chunk Transcript Format (from prepare_german_dataset.py line 559):**
```json
{
  "alignments": [
    ["word", [start_time, end_time], "SPEAKER_MAIN"],
    ...
  ]
}
```

**Verify Transcript Format (from verify_podcasts.py lines 287-290):**
```json
{
  "alignments": [
    ["word", [start_time, end_time], "SPEAKER_MAIN"],
    ...
  ]
}
```

**Analysis:**
- ✅ Both use the same top-level structure: `{"alignments": [...]}`
- ✅ Both use the same alignment format: `[word, [start, end], speaker]`
- ✅ Timestamps are rounded to 6 decimal places in both (prepare: line 624, verify: line 110)
- ⚠️ **ISSUE**: Verify script uses different word cleaning regex than prep script
  - Prep script (line 237): `r"[^\w\däöüßÄÖÜ\s-]"` - preserves German umlauts and hyphens
  - Verify script (line 106): `r"[^\w\däöüßÄÖÜ\s-]"` - **SAME** (Good!)
  - BUT verify also has `clean_for_wer()` (line 192) that uses `r"[^\w\s]"` which removes umlauts!

**Impact:** Medium - WER calculation may be inaccurate due to different text normalization.

**At WER 0%:** The verify JSON would match chunk JSON in structure, but word content might differ slightly due to:
1. Whisper transcription differences
2. Different cleaning in WER calculation (removes umlauts)

---

### 4. FACTS Handling

**Status: ⚠️ PARTIAL**

**Finding:**
- Prep script injects FACTS with markers: `[Injected reference] ... [End of injected reference]` (line 570)
- Verify script has `extract_gt_text()` function (lines 171-188) that attempts to skip FACTS
- **ISSUE**: The FACT detection logic is fragile:
  - Looks for `[Injected` followed later by `reference]` with `prev_word.lower() == "injected"`
  - This could fail if Whisper transcribes the markers differently or splits them

**Impact:** Medium - FACTS might not be properly excluded from WER calculation.

**Recommendation:** More robust FACT marker detection.

---

### 5. Whisper Transcription Method

**Status: ✅ GOOD**

**Finding:**
- Uses whisper-cli with large-v3-q5_0 model (lines 18-21)
- Correctly extracts active channel based on filename (lines 132-138)
- Resamples to 16kHz for Whisper (line 140)
- Uses beam-size=1, best-of=1 for consistency (line 150)
- Generates word-level alignments from token timestamps (lines 90-127)

**Note:** Line 203 says "beam=5, best-of=5" but code uses beam=1, best-of=1 (line 150) - documentation mismatch.

---

### 6. Metrics and Analysis

**Status: ✅ EXCELLENT**

**Finding:**
- Computes WER (Word Error Rate) - lines 27-43
- Computes containment score (fraction of GT words found in verify) - lines 46-51
- Computes LCS ratio (longest common subsequence) - lines 54-71
- Classifies issues into categories - lines 74-87
- Provides detailed per-episode statistics
- Generates comprehensive report

**Impact:** Positive - Very thorough analysis capabilities.

---

### 7. Verify JSON Output Format

**Status: ✅ CORRECT**

**Finding:**
- Verify JSON files use suffix `_wverify.json` (line 23)
- Format matches chunk transcript format exactly (lines 287-290)
- At WER 0%, the alignments array would be identical in structure

---

## Critical Issues Summary

### 🔴 Critical (Must Fix)
1. **No audio format verification** - Cannot confirm 48kHz/16bit/Stereo PCM
2. **No channel muting verification** - Cannot confirm proper double mono with one channel muted

### 🟡 Medium (Should Fix)
3. **WER text cleaning removes umlauts** - May cause artificially high WER for German text
4. **FACT detection is fragile** - May not properly exclude injected facts from WER

### 🟢 Minor (Nice to Have)
5. **Documentation mismatch** - Comments say beam=5 but code uses beam=1

---

## Recommendations

### Priority 1: Add Audio Format Verification
```python
def verify_audio_format(wav_path):
    """Verify audio is 48kHz, 16-bit, Stereo PCM"""
    info = sf.info(wav_path)
    issues = []
    
    if info.samplerate != 48000:
        issues.append(f"Sample rate is {info.samplerate}Hz, expected 48000Hz")
    
    if info.channels != 2:
        issues.append(f"Channels: {info.channels}, expected 2 (stereo)")
    
    if info.subtype != 'PCM_16':
        issues.append(f"Format is {info.subtype}, expected PCM_16")
    
    return issues
```

### Priority 2: Add Channel Muting Verification
```python
def verify_channel_muting(wav_path, is_main):
    """Verify correct channel is muted"""
    audio, sr = sf.read(wav_path)
    
    if audio.ndim != 2 or audio.shape[1] != 2:
        return ["Audio is not stereo"]
    
    left_channel = audio[:, 0]
    right_channel = audio[:, 1]
    
    # Check if channels are truly muted (all zeros or near-zero)
    left_silent = np.allclose(left_channel, 0, atol=1e-6)
    right_silent = np.allclose(right_channel, 0, atol=1e-6)
    
    issues = []
    if is_main and not right_silent:
        issues.append("Right channel should be muted for _main files")
    elif not is_main and not left_silent:
        issues.append("Left channel should be muted for _other files")
    
    # Verify active channel has audio
    if is_main and left_silent:
        issues.append("Left channel is silent but should have audio for _main files")
    elif not is_main and right_silent:
        issues.append("Right channel is silent but should have audio for _other files")
    
    return issues
```

### Priority 3: Fix WER Text Cleaning
```python
def clean_for_wer(text):
    """Clean text for WER but preserve German characters"""
    # Remove punctuation but keep German umlauts and ß
    return re.sub(r"[^\w\säöüßÄÖÜ]", "", text.lower()).strip()
```

---

## Conclusion

The `verify_podcasts.py` script provides **excellent WER analysis and metrics** but is **missing critical audio format verification**. It cannot currently confirm that:
1. Files are in the correct audio format (48kHz/16bit/Stereo PCM)
2. Channels are properly muted (double mono with one channel silent)

The transcript JSON format is compatible, but WER calculations may be slightly inaccurate due to text normalization differences.

**Overall Assessment: 60% Complete - Needs Critical Fixes Before Production Use**