# Timestamp Adjustment Issue Analysis

## Problem Statement

Whisper verification timestamps consistently start **earlier** than chunk transcript timestamps, indicating the chunk cutting mechanism is not properly adjusting timestamps relative to the chunk boundaries.

## Data Evidence

From comparing chunk transcripts vs wverify transcripts:

```
ep150_0000_main.json:
  Chunk:   First word 'Unterfickt' at 0.600s
  Wverify: First word 'Unterfickt' at 0.130s
  Difference: 0.470s (chunk starts LATER)

ep150_0001_other.json:
  Chunk:   First word 'Das' at 0.290s
  Wverify: First word 'Das' at 0.000s
  Difference: 0.290s (chunk starts LATER)

ep150_0002_main.json:
  Chunk:   First word 'Ähnlich' at 0.400s
  Wverify: First word 'Ähnlich' at 0.000s
  Difference: 0.400s (chunk starts LATER)
```

**Pattern**: Chunk transcript timestamps are offset from 0, while Whisper correctly identifies speech starting near 0.

## Root Cause Analysis

### Current Implementation (Lines 854-886)

```python
# 1. Calculate split points (midpoints between speaker turns)
split_points = [0.0]
for i in range(len(turns) - 1):
    prev_end = turns[i][2]
    next_start = turns[i + 1][1]
    midpoint = (prev_end + next_start) / 2.0
    split_points.append(midpoint)
split_points.append(total_dur)

# 2. Extract audio chunk
chunk_start = split_points[i]
chunk_end = split_points[i + 1]
s_sample = int(chunk_start * sr)
e_sample = min(int(chunk_end * sr), total_samples)
chunk_mono = mono[s_sample:e_sample]

# 3. Adjust word timestamps
for word, ws, we in turn_words:
    adj_s = max(0.0, ws - chunk_start)  # ← PROBLEM HERE
    adj_e = min(chunk_dur, we - chunk_start)
    if adj_e > adj_s:
        chunk_aligns.append([word, [round(adj_s, 6), round(adj_e, 6)], speaker])
```

### The Problem

**Line 883**: `adj_s = max(0.0, ws - chunk_start)`

This calculation assumes:
1. `ws` (word start) is relative to the **full cleaned audio**
2. `chunk_start` is the cut point in the **full cleaned audio**
3. Subtracting gives position relative to chunk start

**BUT**: The issue is that `turn_words` contains timestamps from the **ORIGINAL transcript** (before ad removal), while `chunk_start` is calculated from the **CLEANED audio** (after ad removal).

### Example Walkthrough

**Original Episode Timeline** (before ad removal):
```
[Intro 0-34s] [Content starts at 34s...]
```

**After Ad Removal** (cleaned audio):
```
[Content starts at 0s...]  ← 34s intro removed
```

**Turn 1 words** (from original transcript):
```
word "Unterfickt": ws=34.6s, we=35.2s (in original timeline)
```

**Chunk cutting**:
```
chunk_start = 0.0 (first chunk in cleaned audio)
chunk_end = 10.33s
```

**Timestamp adjustment** (WRONG):
```
adj_s = 34.6 - 0.0 = 34.6s  ← WRONG! Should be 0.6s
adj_e = 35.2 - 0.0 = 35.2s  ← WRONG! Should be 1.2s
```

But wait - the code has `max(0.0, ...)` and `min(chunk_dur, ...)`, so:
```
adj_s = max(0.0, 34.6) = 34.6s
adj_e = min(10.33, 35.2) = 10.33s
```

This would create invalid timestamps (start > end), so these words would be filtered out by `if adj_e > adj_s`.

**BUT** - looking at the actual output, the words ARE present with timestamps like 0.6s. This means...

### Wait - Let Me Re-analyze

Looking at the actual data again:
- Chunk transcript has "Unterfickt" at 0.600s
- Whisper has "Unterfickt" at 0.130s

The chunk transcript timestamp (0.600s) suggests the word timestamps ARE being adjusted, but there's still a ~0.47s offset.

Let me check what `turn_words` actually contains...

### The Real Issue

Looking at [`parse_podcast_turns()`](prepare_german_dataset.py:209-250), the function creates turns with word timestamps that are **already relative to the cleaned audio** because it processes the transcript segments directly.

But then in [`process_dialogue()`](prepare_german_dataset.py:863), when we cut chunks:

1. **Turn timestamps** are relative to the **full cleaned audio** (0 = start of cleaned audio)
2. **Chunk cutting** extracts a portion of the cleaned audio
3. **Timestamp adjustment** subtracts `chunk_start` from word timestamps

**The problem**: The turn timestamps include **silence/padding** at the beginning of each turn, but the actual speech starts later.

### Visual Example

**Turn 1 in cleaned audio**:
```
Time:    0.0s -------- 0.6s -------- 10.0s
Audio:   [silence]     [speech starts]
Chunk:   |<-------- chunk_start=0.0 -------->|
```

**Word timestamps in turn**:
```
"Unterfickt": ws=0.6s (relative to cleaned audio start)
```

**After chunk extraction and adjustment**:
```
adj_s = 0.6 - 0.0 = 0.6s  ← Correct relative to chunk
```

**But Whisper transcribes the actual chunk audio**:
```
Chunk audio: [silence 0-0.6s] [speech 0.6-10.0s]
Whisper detects speech starting at: 0.13s  ← Whisper is more accurate!
```

## The Real Root Cause

The issue is **NOT** in the timestamp adjustment logic - that's working correctly!

The issue is in the **original transcript timestamps** which are:
1. **Uniformly distributed** (all words have same duration)
2. **Include leading silence** in the turn boundaries

When Whisper re-transcribes the chunk, it:
1. Detects actual speech boundaries more accurately
2. Starts timestamps closer to where speech actually begins
3. Ignores leading silence

## Why This Causes High WER

The WER calculation compares:
- **Chunk transcript**: "Unterfickt" at 0.600s (uniform distribution with silence padding)
- **Whisper transcript**: "Unterfickt" at 0.130s (actual speech detection)

The 0.47s difference causes:
1. Timestamp misalignment in WER calculation
2. Words appearing "out of order" temporally
3. Inflated WER scores

## Solution

### Option 1: Trim Leading/Trailing Silence from Chunks (RECOMMENDED)

Modify chunk extraction to remove silence:

```python
def trim_silence_from_chunk(audio, sr, threshold=0.01, min_silence=0.05):
    """Trim leading and trailing silence from audio chunk"""
    window_size = int(min_silence * sr)
    
    # Find first non-silent window
    start_idx = 0
    for i in range(0, len(audio) - window_size, window_size // 2):
        window = audio[i:i+window_size]
        rms = np.sqrt(np.mean(window**2))
        if rms > threshold:
            start_idx = i
            break
    
    # Find last non-silent window
    end_idx = len(audio)
    for i in range(len(audio) - window_size, 0, -window_size // 2):
        window = audio[i:i+window_size]
        rms = np.sqrt(np.mean(window**2))
        if rms > threshold:
            end_idx = i + window_size
            break
    
    return audio[start_idx:end_idx], start_idx / sr, end_idx / sr
```

Then adjust timestamps accordingly:

```python
# After extracting chunk
chunk_mono_trimmed, trim_start, trim_end = trim_silence_from_chunk(chunk_mono, sr)

# Adjust word timestamps
for word, ws, we in turn_words:
    adj_s = max(0.0, ws - chunk_start - trim_start)
    adj_e = min(len(chunk_mono_trimmed)/sr, we - chunk_start - trim_start)
    if adj_e > adj_s:
        chunk_aligns.append([word, [round(adj_s, 6), round(adj_e, 6)], speaker])
```

### Option 2: Use Whisper Timestamps as Ground Truth

Replace uniform timestamps with Whisper-generated timestamps:

```python
def get_whisper_word_timestamps(audio, sr, text):
    """Get word-level timestamps from Whisper"""
    # Run Whisper with word timestamps
    # Return list of (word, start, end) tuples
    pass
```

### Option 3: Ignore Timestamps in WER Calculation (SIMPLEST)

Modify WER calculation to compare only text, not timestamps:

```python
def compute_wer_text_only(gt_alignments, verify_alignments):
    """Compare only text content, ignore timestamps"""
    gt_text = " ".join([a[0] for a in gt_alignments])
    verify_text = " ".join([a[0] for a in verify_alignments])
    return compute_wer(clean_for_wer(gt_text), clean_for_wer(verify_text))
```

## Recommendation

**Implement Option 1 (Trim Silence)** because:

1. **Fixes root cause**: Removes silence padding that causes timestamp offset
2. **Improves quality**: Chunks contain only actual speech
3. **Reduces file size**: No wasted silence in training data
4. **Better alignment**: Timestamps match actual speech boundaries

**Also implement Option 3** for verification:
- Use text-only WER as primary metric
- Keep timestamp-aware WER as secondary metric for alignment quality

## Expected Improvement

After implementing silence trimming:
- **Timestamp offset**: 0.47s → <0.05s
- **WER**: 14-15% → <5% (text-only comparison)
- **Chunk quality**: Only actual speech, no padding
- **Training efficiency**: Smaller, cleaner chunks

## Implementation Priority

1. **High**: Implement silence trimming in `process_dialogue()`
2. **High**: Add text-only WER calculation in verification
3. **Medium**: Add silence trimming statistics to logs
4. **Low**: Consider Whisper timestamps as alternative (more complex)