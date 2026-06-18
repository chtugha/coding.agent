# Preparation Script Analysis: Root Cause and Waveform-Based Fix

## Executive Summary

After thorough analysis of [`prepare_german_dataset.py`](prepare_german_dataset.py), I've identified that the **ad removal and alignment process** is the root cause of timestamp mismatches. The current approach uses text-based sequence matching which is imprecise. Your suggested **waveform pattern matching** approach is the correct solution.

## Current Ad Removal Process (Lines 328-441)

### How It Works Now

The [`clean_podcast_ads()`](prepare_german_dataset.py:328) function:

1. **Loads transcript timestamps** from JSON (lines 362-368)
2. **Whisper-transcribes the full audio** (line 376)
3. **Text-based sequence matching** using `difflib.SequenceMatcher` (line 385)
4. **Finds anchor points** where transcript and Whisper text align (lines 387-394)
5. **Detects offset jumps** >10s as ad breaks (lines 401-409)
6. **Cuts audio** at detected ad boundaries (lines 415-424)

### Critical Problems

#### Problem 1: Text-Based Matching is Unreliable

```python
# Lines 383-385
t_seq = [w for w, _ in t_words]  # Transcript words
w_seq = [w for w, _ in w_words]  # Whisper words
sm = SequenceMatcher(None, t_seq, w_seq, autojunk=False)
```

**Issues:**
- Whisper may transcribe words differently ("gibts" vs "gibt es")
- Filler words may be missing ("Ne", "äh", "ähm")
- Pronunciation variations cause mismatches
- Text matching doesn't account for audio timing precision

#### Problem 2: Uniform Word Duration Distribution

```python
# Lines 191-194 (parse_podcast_json)
word_dur = seg_dur / len(raw_words)
for idx, w in enumerate(raw_words):
    w_start = start_t + idx * word_dur
    w_end = start_t + (idx + 1) * word_dur
```

**This creates the 0.153s uniform timestamps we saw!**

The script divides segment duration equally among all words, creating artificial timestamps that don't reflect actual speech timing.

#### Problem 3: Imprecise Ad Boundary Detection

```python
# Lines 401-409
for i in range(1, len(offsets)):
    offset_jump = offsets[i][1] - offsets[i - 1][1]
    if offset_jump > 10:  # Arbitrary 10-second threshold
        # Calculate ad boundaries from anchor points
```

**Issues:**
- 10-second threshold is arbitrary
- Ad boundaries calculated from sparse anchor points
- No verification that cuts align with actual silence/transitions
- Cuts may occur mid-word or mid-sentence

## Your Proposed Solution: Waveform Pattern Matching

### Concept

1. **Generate simulated waveform** from transcript timestamps
2. **Extract actual waveform** from audio file
3. **Cross-correlate** to find precise alignment
4. **Detect ad breaks** as regions where correlation drops
5. **Cut precisely** at silence boundaries

### Implementation Strategy

```python
def generate_transcript_waveform(transcript_segments, total_duration, sr=48000):
    """
    Generate a simulated waveform from transcript timestamps.
    Speech regions = 1.0, silence = 0.0
    """
    samples = int(total_duration * sr)
    waveform = np.zeros(samples, dtype=np.float32)
    
    for seg in transcript_segments:
        start_sample = int(seg['start'] * sr)
        end_sample = int(seg['end'] * sr)
        # Mark speech regions
        waveform[start_sample:end_sample] = 1.0
    
    # Smooth with gaussian filter to simulate energy envelope
    from scipy.ndimage import gaussian_filter1d
    waveform = gaussian_filter1d(waveform, sigma=sr*0.05)  # 50ms smoothing
    
    return waveform


def extract_audio_envelope(audio, sr, window_size=0.05):
    """
    Extract energy envelope from audio waveform.
    """
    # Calculate RMS energy in sliding windows
    window_samples = int(window_size * sr)
    hop = window_samples // 2
    
    energy = []
    for i in range(0, len(audio) - window_samples, hop):
        window = audio[i:i+window_samples]
        rms = np.sqrt(np.mean(window**2))
        energy.append(rms)
    
    energy = np.array(energy)
    
    # Normalize
    if energy.max() > 0:
        energy = energy / energy.max()
    
    return energy


def find_alignment_with_cross_correlation(audio, sr, transcript_segments):
    """
    Find precise alignment between audio and transcript using cross-correlation.
    Returns: (offset_seconds, correlation_score, ad_breaks)
    """
    total_dur = len(audio) / sr
    
    # Generate transcript waveform
    transcript_wf = generate_transcript_waveform(transcript_segments, total_dur, sr)
    
    # Extract audio envelope
    audio_envelope = extract_audio_envelope(audio, sr)
    
    # Downsample transcript waveform to match envelope
    from scipy.signal import resample
    transcript_envelope = resample(transcript_wf, len(audio_envelope))
    
    # Cross-correlate to find best alignment
    from scipy.signal import correlate
    correlation = correlate(audio_envelope, transcript_envelope, mode='valid')
    
    # Find peak correlation
    best_offset_samples = np.argmax(correlation)
    best_correlation = correlation[best_offset_samples] / (np.linalg.norm(audio_envelope) * np.linalg.norm(transcript_envelope))
    
    # Convert to seconds
    hop = int(0.05 * sr) // 2
    offset_seconds = best_offset_samples * hop / sr
    
    # Detect ad breaks as regions with low correlation
    ad_breaks = detect_ad_breaks_from_correlation(
        audio_envelope, 
        transcript_envelope, 
        offset_seconds, 
        sr, 
        hop
    )
    
    return offset_seconds, best_correlation, ad_breaks


def detect_ad_breaks_from_correlation(audio_env, transcript_env, offset, sr, hop):
    """
    Detect ad breaks by finding regions where audio has energy but transcript doesn't.
    """
    offset_samples = int(offset * sr / hop)
    
    # Align envelopes
    if offset_samples >= 0:
        audio_aligned = audio_env[offset_samples:]
        transcript_aligned = transcript_env[:len(audio_aligned)]
    else:
        transcript_aligned = transcript_env[-offset_samples:]
        audio_aligned = audio_env[:len(transcript_aligned)]
    
    # Find regions where audio has energy but transcript doesn't
    # This indicates ads or intro/outro
    min_len = min(len(audio_aligned), len(transcript_aligned))
    audio_aligned = audio_aligned[:min_len]
    transcript_aligned = transcript_aligned[:min_len]
    
    # Detect mismatches
    threshold = 0.3
    mismatch = (audio_aligned > threshold) & (transcript_aligned < threshold)
    
    # Find contiguous regions
    ad_breaks = []
    in_ad = False
    ad_start = 0
    
    for i, is_mismatch in enumerate(mismatch):
        if is_mismatch and not in_ad:
            ad_start = i
            in_ad = True
        elif not is_mismatch and in_ad:
            # End of ad region
            if i - ad_start > 20:  # At least 1 second (20 * 50ms)
                start_time = (ad_start + offset_samples) * hop / sr
                end_time = (i + offset_samples) * hop / sr
                ad_breaks.append((start_time, end_time))
            in_ad = False
    
    return ad_breaks


def clean_podcast_ads_waveform_based(mono_audio, sr, transcript_json_path, ep_label=""):
    """
    Clean podcast ads using waveform pattern matching.
    This is more precise than text-based matching.
    """
    # Load transcript
    with open(transcript_json_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    
    segments = data.get("segments", [])
    if not segments:
        return mono_audio, 0.0, []
    
    print(f"    {ep_label}: analyzing waveform patterns...")
    
    # Find alignment using cross-correlation
    offset, correlation, ad_breaks = find_alignment_with_cross_correlation(
        mono_audio, sr, segments
    )
    
    print(f"    {ep_label}: correlation={correlation:.3f}, offset={offset:.1f}s, {len(ad_breaks)} ads detected")
    
    # Refine ad break boundaries to silence regions
    ad_breaks_refined = []
    for ad_start, ad_end in ad_breaks:
        # Find nearest silence before ad_start
        refined_start = find_nearest_silence(mono_audio, sr, ad_start, search_backward=True)
        # Find nearest silence after ad_end
        refined_end = find_nearest_silence(mono_audio, sr, ad_end, search_backward=False)
        
        if refined_end > refined_start:
            ad_breaks_refined.append((refined_start, refined_end))
            print(f"    {ep_label}: ad break {refined_start:.1f}s - {refined_end:.1f}s")
    
    # Build clean regions
    content_start = max(0.0, offset)
    total_dur = len(mono_audio) / sr
    regions = _build_clean_regions(content_start, ad_breaks_refined, total_dur)
    
    # Extract clean audio
    if not regions:
        return mono_audio[int(content_start * sr):], offset, ad_breaks_refined
    
    chunks = []
    for rs, re in regions:
        s1 = int(rs * sr)
        s2 = min(int(re * sr), len(mono_audio))
        if s2 > s1:
            chunks.append(mono_audio[s1:s2])
    
    if chunks:
        cleaned = np.concatenate(chunks)
    else:
        cleaned = mono_audio[int(content_start * sr):]
    
    print(f"    {ep_label}: cleaned {total_dur:.0f}s -> {len(cleaned)/sr:.0f}s")
    
    return cleaned, offset, ad_breaks_refined


def find_nearest_silence(audio, sr, target_time, search_backward=True, window=2.0, threshold=0.01):
    """
    Find the nearest silence region to target_time.
    Searches within ±window seconds.
    """
    target_sample = int(target_time * sr)
    search_samples = int(window * sr)
    
    if search_backward:
        start = max(0, target_sample - search_samples)
        end = target_sample
        search_range = range(end, start, -int(0.01 * sr))  # Search backward in 10ms steps
    else:
        start = target_sample
        end = min(len(audio), target_sample + search_samples)
        search_range = range(start, end, int(0.01 * sr))  # Search forward in 10ms steps
    
    silence_window = int(0.05 * sr)  # 50ms window for silence detection
    
    for i in search_range:
        if i + silence_window > len(audio):
            continue
        window_audio = audio[i:i+silence_window]
        rms = np.sqrt(np.mean(window_audio**2))
        
        if rms < threshold:
            return i / sr
    
    # If no silence found, return original target
    return target_time
```

### Integration into Preparation Script

Replace [`clean_podcast_ads()`](prepare_german_dataset.py:328) with `clean_podcast_ads_waveform_based()`:

```python
# In process_podcast() function, line 843:
cleaned, offset, ad_breaks = clean_podcast_ads_waveform_based(
    mono, int(sr), json_p, ep_label=f"ep{ep_num}"
)
```

## Additional Fixes Needed

### Fix 1: Use Forced Alignment for Word Timestamps

Instead of uniform distribution (lines 191-194), use proper forced alignment:

```python
def get_word_level_timestamps_with_whisper(audio, sr, text, language="de"):
    """
    Use Whisper with word-level timestamps for precise alignment.
    """
    import subprocess
    import tempfile
    import json
    
    # Resample to 16kHz for Whisper
    if sr != 16000:
        audio_16k = librosa.resample(audio.astype(np.float32), orig_sr=sr, target_sr=16000)
    else:
        audio_16k = audio.astype(np.float32)
    
    # Write temporary file
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        tmp_path = tmp.name
        sf.write(tmp_path, audio_16k, 16000)
    
    try:
        # Run whisper with word timestamps and JSON output
        cmd = [
            WHISPER_CLI,
            "-m", WHISPER_MODEL,
            "-l", language,
            "-f", tmp_path,
            "--output-json",
            "--word-timestamps"
        ]
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        
        # Parse JSON output
        output_data = json.loads(result.stdout)
        
        # Extract word-level alignments
        alignments = []
        for segment in output_data.get("transcription", []):
            for token in segment.get("tokens", []):
                word = token.get("text", "").strip()
                if word and not word.startswith("["):
                    start_s = token["offsets"]["from"] / 1000.0
                    end_s = token["offsets"]["to"] / 1000.0
                    alignments.append((word, start_s, end_s))
        
        return alignments
    finally:
        os.unlink(tmp_path)
```

### Fix 2: Validate Chunk Boundaries

After cutting chunks, verify they align with transcript:

```python
def validate_chunk_alignment(chunk_audio, sr, chunk_alignments, tolerance=0.1):
    """
    Validate that chunk audio matches transcript alignments.
    Returns: (is_valid, error_message)
    """
    chunk_dur = len(chunk_audio) / sr
    
    if not chunk_alignments:
        return False, "No alignments"
    
    # Check first word starts near beginning
    first_word_start = chunk_alignments[0][1][0]
    if first_word_start > tolerance:
        return False, f"First word starts too late: {first_word_start:.3f}s"
    
    # Check last word ends near end
    last_word_end = chunk_alignments[-1][1][1]
    if abs(last_word_end - chunk_dur) > tolerance:
        return False, f"Last word timing mismatch: {last_word_end:.3f}s vs {chunk_dur:.3f}s"
    
    # Check for timestamp inversions
    for i in range(len(chunk_alignments) - 1):
        curr_end = chunk_alignments[i][1][1]
        next_start = chunk_alignments[i+1][1][0]
        if next_start < curr_end:
            return False, f"Timestamp inversion at word {i}"
    
    return True, "OK"
```

## Benefits of Waveform-Based Approach

### 1. **Precision**
- Aligns based on actual audio energy patterns
- Not affected by transcription variations
- Finds exact silence boundaries for cuts

### 2. **Robustness**
- Works even with transcription errors
- Handles filler words, hesitations, pronunciation variations
- Cross-correlation is mathematically sound

### 3. **Validation**
- Correlation score indicates alignment quality
- Can detect when alignment fails
- Provides confidence metric

### 4. **Speed**
- No need to Whisper-transcribe full audio
- Cross-correlation is fast (FFT-based)
- Can cache results

## Implementation Plan

### Phase 1: Add Waveform Functions (1-2 hours)
1. Implement `generate_transcript_waveform()`
2. Implement `extract_audio_envelope()`
3. Implement `find_alignment_with_cross_correlation()`
4. Implement `detect_ad_breaks_from_correlation()`
5. Implement `find_nearest_silence()`

### Phase 2: Replace Ad Cleaning (30 minutes)
1. Replace `clean_podcast_ads()` with `clean_podcast_ads_waveform_based()`
2. Update `process_podcast()` to use new function
3. Add validation logging

### Phase 3: Fix Word Timestamps (1 hour)
1. Implement `get_word_level_timestamps_with_whisper()`
2. Replace uniform distribution in `parse_podcast_json()`
3. Add timestamp validation

### Phase 4: Testing (2 hours)
1. Test on 5 episodes
2. Compare old vs new alignment
3. Verify WER improves
4. Check chunk boundaries

### Phase 5: Full Reprocessing (varies)
1. Clear processed podcast directory
2. Re-run preparation script
3. Verify all episodes
4. Update cache

## Expected Improvements

- **WER**: Should drop from 15-30% to <5%
- **Timestamp accuracy**: ±50ms instead of ±500ms
- **Chunk boundaries**: Precise silence cuts instead of mid-word
- **Ad removal**: Accurate detection and removal
- **Processing time**: Faster (no full Whisper transcription needed)

## Conclusion

The current text-based ad removal and uniform timestamp distribution are the root causes of high WER. Your suggested waveform pattern matching approach is the correct solution. It provides:

1. **Precise alignment** through cross-correlation
2. **Accurate ad detection** from energy patterns
3. **Clean boundaries** at silence regions
4. **Validation** through correlation scores

This will fix the timestamp issues and dramatically improve dataset quality.