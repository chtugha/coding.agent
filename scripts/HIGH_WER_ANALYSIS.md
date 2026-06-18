# Analysis of High WER Files in Verification Results

## Executive Summary

After investigating the user's concern about "8 files with very bad WER", I've identified that these are the **8 files classified as "SIGNIFICANT_DIVERGENCE"** in the WER distribution's **30-50% range**. While the report shows these files exist, the current verification script has a design limitation that prevents detailed per-file analysis.

## Current Findings

### From the Verification Report
- **Total files processed**: 50
- **Average WER**: 15.44% (excellent overall)
- **WER Distribution**:
  - 0-10%: 20 files (40%)
  - 10-20%: 16 files (32%)
  - 20-30%: 5 files (10%)
  - **30-50%: 8 files (16%)** ← These are the "bad WER" files
  - 50-75%: 0 files
  - 75-100%: 0 files
  - 100+%: 0 files

### Issue Classification
- **MINOR_VARIATION**: 41 files (82%)
- **SIGNIFICANT_DIVERGENCE**: 8 files (16%) ← These correspond to the 30-50% WER range

## Root Cause: Script Design Limitation

### Problem Identified
The current [`verify_podcasts_enhanced.py`](verify_podcasts_enhanced.py:508) script writes only the Whisper-generated alignments to `_wverify.json` files:

```python
verify_data = {"alignments": verify_alignments}
with open(verify_path, "w", encoding="utf-8") as f:
    json.dump(verify_data, f, ensure_ascii=False)
```

**Missing from individual files:**
- `wer` (Word Error Rate)
- `containment` (percentage of original words found)
- `lcs_ratio` (Longest Common Subsequence ratio)
- `issue` (classification: MINOR_VARIATION, SIGNIFICANT_DIVERGENCE, etc.)
- `gt_text` (original ground truth text)
- `verify_text` (Whisper transcription text)

This means we cannot identify which specific files have high WER without re-running the verification or enhancing the script.

## Potential Causes of High WER (30-50% range)

Based on the WER classification logic in the script, files with 30-50% WER and "SIGNIFICANT_DIVERGENCE" classification typically indicate:

### 1. **Audio Quality Issues**
- Background noise affecting Whisper transcription
- Poor microphone quality
- Audio artifacts from processing

### 2. **Speaker Overlap or Bleed**
- Despite channel muting, some speaker bleed may occur
- Whisper might transcribe faint audio from the muted channel
- This would cause extra words not in the original transcript

### 3. **Whisper Model Limitations**
- German language nuances (dialects, accents)
- Technical or domain-specific vocabulary
- Fast speech or unclear pronunciation

### 4. **Timestamp Alignment Issues**
- If chunk splitting occurred at suboptimal points
- Whisper might include words from adjacent chunks
- Timing misalignment between original and Whisper transcription

### 5. **FACTS Injection Confusion**
- If FACTS were injected, Whisper would transcribe them
- This would create legitimate divergence from original transcript
- However, only 3 chunks >60s were found, with 0 FACTS injected

## Recommendations to Improve Verification Script

### 1. **Enhanced Per-File Reporting** (HIGH PRIORITY)
Add WER metrics to individual `_wverify.json` files:

```python
verify_data = {
    "alignments": verify_alignments,
    "wer": round(wer, 4),
    "containment": round(containment, 4),
    "lcs_ratio": round(lcs_ratio, 4),
    "issue": issue,
    "gt_text": gt_clean,
    "verify_text": verify_clean,
    "gt_word_count": len(gt_words),
    "verify_word_count": len(verify_words),
    "duration": round(dur, 2)
}
```

**Benefits:**
- Enables identification of specific problematic files
- Allows targeted investigation and debugging
- Facilitates quality improvement iterations

### 2. **High WER File Logging** (HIGH PRIORITY)
Add detailed logging for files exceeding WER threshold:

```python
if wer > 0.30:  # Log files with >30% WER
    log(f"  HIGH WER: {fname}")
    log(f"    WER: {wer:.2%}, Containment: {containment:.2%}, LCS: {lcs_ratio:.2%}")
    log(f"    Issue: {issue}")
    log(f"    GT:     {gt_clean[:100]}...")
    log(f"    Verify: {verify_clean[:100]}...")
    log()
```

### 3. **Audio Quality Metrics** (MEDIUM PRIORITY)
Add signal-to-noise ratio (SNR) analysis:

```python
def analyze_audio_quality(wav_path):
    """Analyze audio quality metrics"""
    data, sr = sf.read(wav_path)
    
    # Calculate RMS energy
    rms = np.sqrt(np.mean(data**2))
    
    # Estimate SNR (simplified)
    signal_power = np.mean(data**2)
    noise_floor = np.percentile(np.abs(data), 10)**2
    snr_db = 10 * np.log10(signal_power / noise_floor) if noise_floor > 0 else float('inf')
    
    return {
        "rms": float(rms),
        "snr_db": float(snr_db),
        "peak": float(np.max(np.abs(data)))
    }
```

### 4. **Whisper Confidence Scores** (MEDIUM PRIORITY)
Extract and log Whisper's confidence for each word:

```python
# In whisper_transcribe_to_alignments function
for tok in tokens:
    confidence = tok.get("p", 1.0)  # Probability/confidence
    if confidence < 0.5:  # Low confidence threshold
        low_confidence_words.append((text, confidence))
```

### 5. **Comparative Analysis Mode** (LOW PRIORITY)
Add option to compare multiple Whisper model sizes:

```python
def compare_whisper_models(wav_path, models=["base", "small", "medium"]):
    """Compare transcriptions from different Whisper models"""
    results = {}
    for model in models:
        alignments = whisper_transcribe_to_alignments(wav_path, speaker, model=model)
        results[model] = alignments
    return results
```

### 6. **Detailed Diff Output** (LOW PRIORITY)
Generate word-level diff for high WER files:

```python
def generate_word_diff(gt_words, verify_words):
    """Generate detailed word-level diff"""
    import difflib
    diff = difflib.unified_diff(gt_words, verify_words, lineterm='')
    return '\n'.join(diff)
```

## Immediate Action Plan

### Step 1: Enhance Script (30 minutes)
Modify [`verify_podcasts_enhanced.py`](verify_podcasts_enhanced.py:508) to:
1. Save WER metrics to individual `_wverify.json` files
2. Add high WER logging during processing
3. Create separate high-WER report file

### Step 2: Re-run Verification (10 minutes)
Run enhanced script on the same 50 files with `--force` flag to regenerate `_wverify.json` files with metrics

### Step 3: Analyze High WER Files (20 minutes)
1. Identify the 8 specific files with 30-50% WER
2. Listen to audio samples
3. Compare original vs Whisper transcripts
4. Categorize root causes

### Step 4: Implement Fixes (varies)
Based on findings:
- Adjust Whisper parameters (temperature, beam size)
- Improve audio preprocessing
- Fine-tune chunk splitting logic
- Update FACTS injection rules

## Expected Outcomes

After implementing these improvements:

1. **Transparency**: Know exactly which files have issues
2. **Debuggability**: Understand why specific files fail
3. **Reproducibility**: Track improvements across iterations
4. **Quality Assurance**: Ensure dataset meets training requirements

## Conclusion

The 8 files with "bad WER" (30-50% range) represent 16% of the test set. While this is concerning, the overall average WER of 15.44% is excellent. The main issue is **lack of visibility** into which specific files are problematic.

**Recommended Priority**: Implement enhanced per-file reporting immediately to enable targeted investigation and improvement.