# Preparation Script Bugs - Root Cause Analysis

## Executive Summary

The chunk transcript timestamps are **INCORRECT**. Whisper verification timestamps are **CORRECT**. The preparation script has three critical bugs that create inaccurate timestamps.

## Audio Analysis Proof

Testing `ep150_0000_main.wav`:
- **Chunk transcript says**: "Unterfickt" starts at 0.600s
- **Actual audio analysis**: Speech detected at 0.100s (RMS energy spike)
- **Whisper verification**: "Unterfickt" at 0.130s ✓ CORRECT

```
Audio energy (RMS):
  0.00s: 0.000016 (silence)
  0.13s: 0.141561 (SPEECH DETECTED)
  0.60s: 0.096686 (speech continues)
```

## Bug #1: Uniform Timestamp Distribution

**Location**: `prepare_german_dataset.py` lines 234-238

```python
seg_dur = end_t - start_t
word_dur = seg_dur / len(raw_words)  # ← BUG: Uniform distribution
for idx, w in enumerate(raw_words):
    w_start = start_t + idx * word_dur  # ← Evenly spaced
    w_end = start_t + (idx + 1) * word_dur
```

**Problem**: Words are artificially distributed evenly across segment duration, ignoring actual speech timing.

**Example**:
- Segment: 10 words in 5 seconds
- Script assigns: 0.5s per word (0.0-0.5, 0.5-1.0, 1.0-1.5...)
- Reality: Words might be "Hello...........world" (0.0-0.5s, 4.5-5.0s)

## Bug #2: No Timestamp Adjustment After Ad Removal

**Location**: `prepare_german_dataset.py` line 1106

```python
cleaned, offset, ad_breaks = clean_podcast_ads_waveform_based(...)
# cleaned audio is SHORTER (ads removed)
# but turns still have timestamps from ORIGINAL audio!
n = process_dialogue(cleaned, int(sr), None, None, "podcast", 
                     f"ep{ep_num}", output_dir, precomputed_turns=turns)
```

**Problem**: 
- Original audio: 3600s with ads at 1200-1500s
- Cleaned audio: 3300s (300s removed)
- Timestamps still reference original positions
- Word at 1600s in original → should be 1300s in cleaned, but script uses 1600s

## Bug #3: Offset Not Applied to Timestamps

**Location**: `prepare_german_dataset.py` line 1105

```python
cleaned, offset, ad_breaks = clean_podcast_ads_waveform_based(...)
# offset calculated (e.g., 30s intro removed)
# but NEVER applied to turn timestamps!
```

**Problem**: If intro/outro removed, all timestamps shift but script doesn't adjust them.

## Impact on Chunks

When `process_dialogue()` creates chunks (lines 882-886):

```python
for word, ws, we in turn_words:
    adj_s = max(0.0, ws - chunk_start)
    adj_e = min(chunk_dur, we - chunk_start)
```

It tries to make timestamps relative to chunk start, but:
- `ws` and `we` are already wrong (bugs #1, #2, #3)
- Subtracting `chunk_start` doesn't fix the underlying inaccuracy
- Result: Chunk timestamps don't match actual audio

## Why Whisper is Correct

Whisper analyzes the **actual audio waveform** and detects:
- Voice activity (when speech actually occurs)
- Word boundaries (acoustic features)
- Silence vs. speech

It ignores any pre-existing timestamps and works from ground truth.

## Required Fixes

### Fix #1: Use Whisper for Word-Level Timestamps

Replace uniform distribution with actual Whisper word-level alignment:

```python
def parse_podcast_turns_with_whisper(json_path, audio_path):
    # Run Whisper with word-level timestamps
    result = whisper.transcribe(audio_path, word_timestamps=True)
    
    # Use actual word timings instead of uniform distribution
    for segment in result['segments']:
        for word_info in segment['words']:
            word = word_info['word']
            w_start = word_info['start']
            w_end = word_info['end']
            # Use ACTUAL timings
```

### Fix #2: Adjust Timestamps After Ad Removal

```python
def adjust_timestamps_for_removed_regions(turns, offset, ad_breaks):
    """Adjust all timestamps to account for removed audio regions"""
    adjusted_turns = []
    
    for speaker, turn_start, turn_end, words in turns:
        # Calculate cumulative time removed before this turn
        time_removed = offset  # Initial offset
        for ad_start, ad_end in ad_breaks:
            if ad_end <= turn_start:
                time_removed += (ad_end - ad_start)
        
        # Adjust turn and word timestamps
        new_turn_start = turn_start - time_removed
        new_words = []
        for word, ws, we in words:
            # Recalculate for each word
            word_time_removed = offset
            for ad_start, ad_end in ad_breaks:
                if ad_end <= ws:
                    word_time_removed += (ad_end - ad_start)
            
            new_ws = ws - word_time_removed
            new_we = we - word_time_removed
            new_words.append((word, new_ws, new_we))
        
        adjusted_turns.append((speaker, new_turn_start, ..., new_words))
    
    return adjusted_turns
```

### Fix #3: Apply in Correct Order

```python
# 1. Parse with Whisper word timestamps
turns = parse_podcast_turns_with_whisper(json_p, mp3_path)

# 2. Clean audio
cleaned, offset, ad_breaks = clean_podcast_ads_waveform_based(...)

# 3. Adjust timestamps to match cleaned audio
turns_adjusted = adjust_timestamps_for_removed_regions(turns, offset, ad_breaks)

# 4. Process with adjusted timestamps
n = process_dialogue(cleaned, int(sr), None, None, "podcast", 
                     f"ep{ep_num}", output_dir, precomputed_turns=turns_adjusted)
```

## Verification Script Status

The `verify_podcasts.py` script is **WORKING CORRECTLY**. It:
- Re-transcribes audio with Whisper (gets accurate timestamps)
- Compares to chunk transcripts
- Reveals the timestamp inaccuracies in chunk transcripts

**The verification script does NOT need fixing. The preparation script needs fixing.**

## Next Steps

1. Implement Whisper word-level timestamp extraction
2. Implement timestamp adjustment for ad removal
3. Re-run preparation script on all episodes
4. Verify with `verify_podcasts.py` - should achieve WER ≈ 0%