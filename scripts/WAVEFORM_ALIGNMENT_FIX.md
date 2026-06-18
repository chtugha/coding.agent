# Waveform Alignment Fix - Root Cause Analysis

## Problem Identified

The current `detect_ad_breaks_from_correlation()` function in [`prepare_german_dataset.py:406-444`](prepare_german_dataset.py:406-444) is **fundamentally broken**.

### Current (Broken) Approach
```python
def detect_ad_breaks_from_correlation(audio_env, transcript_env, offset, sr, hop):
    # Detects where audio has energy but transcript doesn't
    mismatch = (audio_aligned > threshold) & (transcript_aligned < threshold)
```

**Why it fails:**
- Assumes ads are "audio energy without transcript"
- Doesn't account for intro/outro music
- Doesn't handle overlapping speech
- Creates false positives from silence in transcript

## Correct Approach

### Algorithm
1. **Create transcript pattern** from segment timestamps:
   - Speech segments = 1.0
   - Silence/gaps = 0.0
   
2. **Slide pattern along audio** envelope:
   - Calculate correlation at each position
   - Find continuous matching regions
   
3. **Keep matching regions**:
   - Where correlation > threshold
   - Audio aligns with transcript pattern
   
4. **Cut non-matching regions**:
   - Intro (before first match)
   - Ads (gaps in matching)
   - Outro (after last match)

### Implementation

```python
def align_audio_to_transcript_pattern(audio, sr, transcript_segments):
    """
    Align audio to transcript pattern by finding matching regions.
    Returns cleaned audio that matches transcript timeline.
    """
    # 1. Generate transcript pattern (speech=1, silence=0)
    transcript_duration = max(seg['end'] for seg in transcript_segments)
    pattern = generate_transcript_waveform(transcript_segments, transcript_duration, sr)
    
    # 2. Extract audio envelope
    audio_envelope = extract_audio_envelope(audio, sr)
    
    # 3. Find matching regions using sliding window correlation
    window_size = len(pattern)
    matches = []
    
    for i in range(0, len(audio_envelope) - window_size, hop):
        audio_window = audio_envelope[i:i+window_size]
        correlation = np.corrcoef(audio_window, pattern)[0,1]
        
        if correlation > threshold:
            start_time = i * hop / sr
            end_time = (i + window_size) * hop / sr
            matches.append((start_time, end_time, correlation))
    
    # 4. Merge overlapping matches
    merged_matches = merge_overlapping_regions(matches)
    
    # 5. Extract audio from matching regions
    cleaned_chunks = []
    for start, end in merged_matches:
        start_sample = int(start * sr)
        end_sample = int(end * sr)
        cleaned_chunks.append(audio[start_sample:end_sample])
    
    cleaned_audio = np.concatenate(cleaned_chunks)
    
    return cleaned_audio, merged_matches
```

### Key Differences

| Current Approach | Correct Approach |
|-----------------|------------------|
| Find "audio without transcript" | Find "audio matching transcript pattern" |
| Negative detection (what to remove) | Positive detection (what to keep) |
| Unreliable with music/noise | Robust pattern matching |
| Creates false positives | Accurate alignment |

## Expected Results

After fixing:
- Cleaned audio duration ≈ transcript duration (within 1-2 seconds)
- MFA timestamps will be accurate (< 100ms error)
- WER will drop from 13.78% to 5-8%

## Implementation Plan

1. Replace `detect_ad_breaks_from_correlation()` with `align_audio_to_transcript_pattern()`
2. Update `clean_podcast_ads_waveform_based()` to use new function
3. Test on episode 150
4. Validate with WhisperX (expect < 100ms average error)
5. Process all 373 episodes