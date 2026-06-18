# Final Root Cause Analysis and Fix

## The Problem

Chunk transcript timestamps are inaccurate by ~0.5s. Audio analysis proves:
- Chunk transcript: "Unterfickt" at 0.600s
- Actual audio: Speech starts at 0.100s
- Discrepancy: 0.500s

## Root Cause: Uniform Word Distribution

**Location**: `prepare_german_dataset.py` lines 234-238 in `parse_podcast_turns()`

```python
seg_dur = end_t - start_t
word_dur = seg_dur / len(raw_words)  # ← UNIFORM DISTRIBUTION
for idx, w in enumerate(raw_words):
    w_start = start_t + idx * word_dur  # ← EVENLY SPACED
    w_end = start_t + (idx + 1) * word_dur
```

### What Happens:
1. Original podcast JSON has **accurate segment-level timestamps** (e.g., "segment from 10.0s to 15.0s")
2. Script artificially distributes words **evenly** across segment duration
3. Example: 5 words in 5 seconds → 1.0s per word (10.0-11.0, 11.0-12.0, etc.)
4. Reality: Words might be spoken as "Hello...........world" (actual timing: 10.0-10.5s, 14.5-15.0s)

### Why This Causes 0.5s Offset:
- Uniform distribution assumes words are evenly spaced
- Real speech has pauses, varying word lengths, speaking rate changes
- Average error accumulates to ~0.5s for typical speech patterns

## The Padding is Correct

The midpoint-based chunking (lines 854-865) is working as designed:
```python
midpoint = (prev_end + next_start) / 2.0  # Cut at midpoint between turns
chunk_start = split_points[i]
chunk_end = split_points[i + 1]
```

This creates chunks with natural silence padding at boundaries, which is intentional and correct.

## The Fix: Use Whisper for Word-Level Timestamps

Replace uniform distribution with actual Whisper word-level alignment:

### Step 1: Modify `parse_podcast_turns()` to use Whisper

```python
def parse_podcast_turns_with_whisper(json_path, audio_path):
    """
    Parse podcast turns using Whisper for accurate word-level timestamps.
    """
    import whisper
    
    # Load original segment data
    with open(json_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    segments = data.get("segments", [])
    
    # Load audio
    audio = whisper.load_audio(audio_path)
    
    # Run Whisper with word timestamps
    model = whisper.load_model("large-v2")  # or appropriate model
    result = model.transcribe(
        audio,
        language="de",
        word_timestamps=True,
        initial_prompt="German podcast conversation"
    )
    
    # Map speakers from original segments
    speaker_map = {}
    turns = []
    
    for seg in result['segments']:
        # Find corresponding original segment to get speaker
        seg_start = seg['start']
        seg_end = seg['end']
        
        # Match to original segment by timestamp overlap
        speaker = None
        for orig_seg in segments:
            if abs(orig_seg['start'] - seg_start) < 2.0:  # Within 2s
                orig_speaker = orig_seg.get('speaker', '')
                if orig_speaker:
                    if orig_speaker not in speaker_map:
                        speaker_map[orig_speaker] = len(speaker_map)
                    speaker = "SPEAKER_MAIN" if speaker_map[orig_speaker] == 0 else "SPEAKER_OTHER"
                    break
        
        if not speaker:
            continue
        
        # Extract words with ACTUAL timestamps from Whisper
        words = []
        for word_info in seg.get('words', []):
            word = word_info['word'].strip()
            w_start = word_info['start']
            w_end = word_info['end']
            
            # Clean word
            w_clean = re.sub(r"[^\w\däöüßÄÖÜ\s-]", "", word)
            if w_clean:
                words.append((w_clean, w_start, w_end))
        
        if not words:
            continue
        
        # Merge consecutive turns from same speaker
        if turns and turns[-1][0] == speaker:
            prev = turns[-1]
            turns[-1] = (prev[0], prev[1], seg_end, prev[3] + words)
        else:
            turns.append((speaker, seg_start, seg_end, words))
    
    return turns
```

### Step 2: Update `process_podcast()` to use new function

```python
def process_podcast():
    # ... existing code ...
    
    for ei, (ep_num, json_p, mp3_path) in enumerate(matched):
        try:
            # Use Whisper for accurate word timestamps
            turns = parse_podcast_turns_with_whisper(json_p, mp3_path)
            
            if not turns:
                log_error("podcast", f"ep{ep_num}", "No speaker turns in transcript")
                total_errors += 1
                continue
            
            # Load and clean audio
            mono, sr = librosa.load(mp3_path, sr=TARGET_SR, mono=True)
            mono = mono.astype(np.float32)
            
            # Clean ads using waveform matching
            cleaned, offset, ad_breaks = clean_podcast_ads_waveform_based(
                mono, int(sr), json_p, ep_label=f"ep{ep_num}"
            )
            
            # Adjust timestamps for removed regions
            turns_adjusted = adjust_timestamps_for_removed_regions(
                turns, offset, ad_breaks
            )
            
            # Process with adjusted timestamps
            n = process_dialogue(
                cleaned, int(sr), None, None, "podcast", 
                f"ep{ep_num}", output_dir, precomputed_turns=turns_adjusted
            )
            total_chunks += n
            
        except Exception as e:
            log_error("podcast", f"ep{ep_num}", str(e))
            total_errors += 1
```

### Step 3: Add timestamp adjustment function

```python
def adjust_timestamps_for_removed_regions(turns, offset, ad_breaks):
    """
    Adjust all timestamps to account for removed audio regions.
    
    Args:
        turns: List of (speaker, start, end, words)
        offset: Initial offset (intro removal)
        ad_breaks: List of (start, end) tuples for removed ad regions
    
    Returns:
        Adjusted turns with corrected timestamps
    """
    adjusted_turns = []
    
    for speaker, turn_start, turn_end, words in turns:
        # Calculate cumulative time removed before this turn
        time_removed_at_turn = offset
        for ad_start, ad_end in sorted(ad_breaks):
            if ad_end <= turn_start:
                time_removed_at_turn += (ad_end - ad_start)
        
        new_turn_start = turn_start - time_removed_at_turn
        
        # Adjust each word timestamp
        new_words = []
        for word, ws, we in words:
            # Calculate time removed before this word
            time_removed_at_word = offset
            for ad_start, ad_end in sorted(ad_breaks):
                if ad_end <= ws:
                    time_removed_at_word += (ad_end - ad_start)
                elif ad_start < ws < ad_end:
                    # Word starts in removed region - skip it
                    break
            else:
                # Word is in kept region
                new_ws = ws - time_removed_at_word
                new_we = we - time_removed_at_word
                
                # Ensure timestamps are valid
                if new_ws >= 0 and new_we > new_ws:
                    new_words.append((word, new_ws, new_we))
        
        if new_words:
            new_turn_end = new_words[-1][2]  # End of last word
            adjusted_turns.append((speaker, new_turn_start, new_turn_end, new_words))
    
    return adjusted_turns
```

## Expected Results After Fix

1. **Accurate word-level timestamps**: Whisper detects actual speech timing
2. **Proper offset adjustment**: Timestamps account for intro/ad removal
3. **WER ≈ 0%**: Verification should show near-perfect alignment
4. **Chunk timestamps match audio**: No more 0.5s discrepancies

## Verification Script Status

The `verify_podcasts.py` script is **working correctly**. It:
- Re-transcribes with Whisper (gets accurate timestamps)
- Compares to chunk transcripts
- Reveals the inaccuracies in chunk transcripts

**No changes needed to verification script.**

## Implementation Priority

1. Implement `parse_podcast_turns_with_whisper()` 
2. Add `adjust_timestamps_for_removed_regions()`
3. Update `process_podcast()` to use both functions
4. Re-run preparation on all episodes
5. Verify with `verify_podcasts.py` - should achieve WER ≈ 0%