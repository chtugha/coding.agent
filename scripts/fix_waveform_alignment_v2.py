#!/usr/bin/env python3
"""
Fixed waveform alignment algorithm V2.
Matches from the start and tracks continuously.
"""

import numpy as np
from scipy.ndimage import gaussian_filter1d
from scipy.signal import correlate, resample


def generate_transcript_pattern(transcript_segments, duration, sr=10):
    """Generate binary pattern from transcript: speech=1, silence=0."""
    samples = int(duration * sr)
    pattern = np.zeros(samples, dtype=np.float32)
    
    for seg in transcript_segments:
        start_sample = int(seg['start'] * sr)
        end_sample = int(seg['end'] * sr)
        end_sample = min(end_sample, samples)
        if end_sample > start_sample:
            pattern[start_sample:end_sample] = 1.0
    
    # Smooth pattern
    pattern = gaussian_filter1d(pattern, sigma=sr*0.05)
    
    return pattern


def extract_audio_envelope(audio, sr, target_sr=10):
    """Extract energy envelope from audio."""
    downsample_factor = int(sr / target_sr)
    audio_down = audio[::downsample_factor]
    
    window_size = int(0.1 * target_sr)
    hop = window_size
    
    energy = []
    for i in range(0, len(audio_down) - window_size, hop):
        window = audio_down[i:i+window_size]
        rms = np.sqrt(np.mean(window**2))
        energy.append(rms)
    
    energy = np.array(energy)
    if energy.max() > 0:
        energy = energy / energy.max()
    
    return energy


def align_audio_to_transcript_continuous(audio, sr, transcript_segments):
    """
    Align audio to transcript by matching from the start and tracking continuously.
    
    Algorithm:
    1. Create pattern from transcript
    2. Extract audio envelope
    3. Start matching from beginning
    4. Track correlation continuously
    5. When correlation drops → CUT (ad/intro)
    6. Find where correlation resumes → RESUME
    7. Continue until end
    """
    print("  Creating transcript pattern...")
    transcript_duration = max(seg['end'] for seg in transcript_segments)
    pattern_sr = 10
    pattern = generate_transcript_pattern(transcript_segments, transcript_duration, pattern_sr)
    
    print(f"  Pattern: {transcript_duration:.1f}s, {len(pattern)} samples at {pattern_sr}Hz")
    
    print("  Extracting audio envelope...")
    audio_envelope = extract_audio_envelope(audio, sr, pattern_sr)
    audio_duration = len(audio_envelope) / pattern_sr
    
    print(f"  Audio envelope: {audio_duration:.1f}s, {len(audio_envelope)} samples")
    
    print("  Finding continuous alignment...")
    
    # Parameters
    window_size = int(10.0 * pattern_sr)  # 10-second window for correlation
    correlation_threshold = 0.3  # Lower threshold for continuous tracking
    min_gap = int(2.0 * pattern_sr)  # Minimum 2s gap to consider a cut
    
    # Track alignment
    pattern_pos = 0
    audio_pos = 0
    kept_regions = []
    current_region_start = None
    
    while pattern_pos < len(pattern) and audio_pos < len(audio_envelope):
        # Get pattern window
        pattern_end = min(pattern_pos + window_size, len(pattern))
        pattern_window = pattern[pattern_pos:pattern_end]
        
        if len(pattern_window) < window_size // 2:
            break
        
        # Try to find match in audio
        best_corr = -1
        best_audio_pos = audio_pos
        
        # Search forward in audio (up to 60 seconds ahead for intro/ads)
        max_search = min(audio_pos + int(60 * pattern_sr), len(audio_envelope) - len(pattern_window))
        
        for test_pos in range(audio_pos, max_search, int(0.5 * pattern_sr)):
            audio_window = audio_envelope[test_pos:test_pos + len(pattern_window)]
            
            if len(audio_window) != len(pattern_window):
                continue
            
            # Calculate correlation
            corr = np.corrcoef(pattern_window, audio_window)[0, 1]
            
            if corr > best_corr:
                best_corr = corr
                best_audio_pos = test_pos
        
        if best_corr > correlation_threshold:
            # Good match found
            if current_region_start is None:
                # Start new region
                current_region_start = best_audio_pos / pattern_sr
                print(f"    Starting region at audio {current_region_start:.1f}s (pattern {pattern_pos/pattern_sr:.1f}s, corr={best_corr:.3f})")
            
            # Advance both pattern and audio
            advance = len(pattern_window)
            pattern_pos += advance
            audio_pos = best_audio_pos + advance
            
        else:
            # No match - end current region if we have one
            if current_region_start is not None:
                region_end = audio_pos / pattern_sr
                kept_regions.append((current_region_start, region_end))
                print(f"    Ending region at audio {region_end:.1f}s (gap detected, corr={best_corr:.3f})")
                current_region_start = None
            
            # Skip ahead in pattern to find next content
            pattern_pos += int(1.0 * pattern_sr)
    
    # Close final region
    if current_region_start is not None:
        region_end = audio_pos / pattern_sr
        kept_regions.append((current_region_start, region_end))
        print(f"    Ending final region at audio {region_end:.1f}s")
    
    print(f"  Found {len(kept_regions)} continuous regions")
    
    if not kept_regions:
        print("  WARNING: No matching regions found!")
        return audio, [(0, len(audio)/sr)]
    
    # Extract audio chunks
    cleaned_chunks = []
    for start_time, end_time in kept_regions:
        start_sample = int(start_time * sr)
        end_sample = int(end_time * sr)
        end_sample = min(end_sample, len(audio))
        
        if end_sample > start_sample:
            cleaned_chunks.append(audio[start_sample:end_sample])
            print(f"    Keeping: {start_time:.1f}s - {end_time:.1f}s ({end_time-start_time:.1f}s)")
    
    if not cleaned_chunks:
        print("  WARNING: No audio chunks extracted!")
        return audio, [(0, len(audio)/sr)]
    
    cleaned_audio = np.concatenate(cleaned_chunks)
    
    print(f"  Cleaned: {len(audio)/sr:.1f}s → {len(cleaned_audio)/sr:.1f}s")
    print(f"  Expected: {transcript_duration:.1f}s")
    print(f"  Difference: {abs(len(cleaned_audio)/sr - transcript_duration):.1f}s")
    
    return cleaned_audio, kept_regions


if __name__ == "__main__":
    print("This module provides the fixed waveform alignment algorithm V2.")
    print("Import and use align_audio_to_transcript_continuous() in your scripts.")

# Made with Bob
