#!/usr/bin/env python3
"""
Fixed waveform alignment algorithm.
Implements the correct pattern-matching approach.
"""

import numpy as np
from scipy.ndimage import gaussian_filter1d
from scipy.signal import correlate, resample


def generate_transcript_pattern(transcript_segments, duration, sr=10):
    """
    Generate binary pattern from transcript: speech=1, silence=0.
    Uses low sample rate (10Hz) for efficiency.
    """
    samples = int(duration * sr)
    pattern = np.zeros(samples, dtype=np.float32)
    
    for seg in transcript_segments:
        start_sample = int(seg['start'] * sr)
        end_sample = int(seg['end'] * sr)
        end_sample = min(end_sample, samples)
        if end_sample > start_sample:
            pattern[start_sample:end_sample] = 1.0
    
    # Smooth pattern to simulate energy envelope
    pattern = gaussian_filter1d(pattern, sigma=sr*0.05)
    
    return pattern


def extract_audio_envelope(audio, sr, target_sr=10):
    """
    Extract energy envelope from audio at target sample rate.
    """
    # Downsample for efficiency
    downsample_factor = int(sr / target_sr)
    audio_down = audio[::downsample_factor]
    
    # Extract RMS energy in windows
    window_size = int(0.1 * target_sr)  # 100ms windows
    hop = window_size
    
    energy = []
    for i in range(0, len(audio_down) - window_size, hop):
        window = audio_down[i:i+window_size]
        rms = np.sqrt(np.mean(window**2))
        energy.append(rms)
    
    energy = np.array(energy)
    
    # Normalize
    if energy.max() > 0:
        energy = energy / energy.max()
    
    return energy


def align_audio_to_transcript_pattern(audio, sr, transcript_segments, 
                                     correlation_threshold=0.6,
                                     min_match_duration=5.0):
    """
    Align audio to transcript pattern using iterative matching.
    
    Algorithm:
    1. Create pattern from transcript timestamps
    2. Start at beginning of audio
    3. Find where pattern matches (correlation > threshold)
    4. Keep matching audio
    5. When pattern stops matching → CUT (skip non-matching audio)
    6. SLIDE forward to find next match
    7. Repeat until end
    
    Returns:
        cleaned_audio: Audio segments that match transcript
        kept_regions: List of (start, end) times kept from original audio
    """
    print("  Creating transcript pattern...")
    transcript_duration = max(seg['end'] for seg in transcript_segments)
    pattern_sr = 10  # Hz - low rate for efficiency
    pattern = generate_transcript_pattern(transcript_segments, transcript_duration, pattern_sr)
    pattern_duration = len(pattern) / pattern_sr
    
    print(f"  Pattern: {pattern_duration:.1f}s, {len(pattern)} samples at {pattern_sr}Hz")
    
    print("  Extracting audio envelope...")
    audio_envelope = extract_audio_envelope(audio, sr, pattern_sr)
    audio_duration = len(audio_envelope) / pattern_sr
    
    print(f"  Audio envelope: {audio_duration:.1f}s, {len(audio_envelope)} samples")
    
    # Iterative matching
    print("  Finding matching regions...")
    kept_regions = []
    pattern_pos = 0  # Position in pattern
    audio_pos = 0    # Position in audio envelope
    
    window_size = int(2.0 * pattern_sr)  # 2-second correlation window
    slide_step = int(0.5 * pattern_sr)   # 0.5-second slide step
    
    while pattern_pos < len(pattern) and audio_pos < len(audio_envelope):
        # Get pattern window
        pattern_end = min(pattern_pos + window_size, len(pattern))
        pattern_window = pattern[pattern_pos:pattern_end]
        
        if len(pattern_window) < window_size // 2:
            break  # Pattern too short
        
        # Try to find match in audio starting from current position
        best_corr = -1
        best_audio_pos = audio_pos
        
        # Search forward in audio (up to 30 seconds ahead)
        max_search = min(audio_pos + int(30 * pattern_sr), len(audio_envelope) - len(pattern_window))
        
        for test_pos in range(audio_pos, max_search, slide_step):
            audio_window = audio_envelope[test_pos:test_pos + len(pattern_window)]
            
            if len(audio_window) != len(pattern_window):
                continue
            
            # Calculate correlation
            corr = np.corrcoef(pattern_window, audio_window)[0, 1]
            
            if corr > best_corr:
                best_corr = corr
                best_audio_pos = test_pos
            
            # If we found a good match, stop searching
            if corr > correlation_threshold:
                break
        
        if best_corr > correlation_threshold:
            # Found match! Keep this region
            match_start_time = best_audio_pos / pattern_sr
            match_end_time = (best_audio_pos + len(pattern_window)) / pattern_sr
            
            kept_regions.append((match_start_time, match_end_time))
            
            # Advance both pattern and audio
            pattern_pos += len(pattern_window)
            audio_pos = best_audio_pos + len(pattern_window)
            
            print(f"    Match: pattern {pattern_pos/pattern_sr:.1f}s, audio {match_start_time:.1f}-{match_end_time:.1f}s, corr={best_corr:.3f}")
        else:
            # No match found - slide pattern forward
            pattern_pos += slide_step
            print(f"    No match at pattern {pattern_pos/pattern_sr:.1f}s, sliding...")
    
    print(f"  Found {len(kept_regions)} matching regions")
    
    # Extract audio from kept regions
    if not kept_regions:
        print("  WARNING: No matching regions found!")
        return audio, [(0, len(audio)/sr)]
    
    # Merge overlapping regions
    kept_regions = merge_overlapping_regions(kept_regions)
    
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


def merge_overlapping_regions(regions, gap_threshold=1.0):
    """
    Merge overlapping or nearby regions.
    """
    if not regions:
        return []
    
    sorted_regions = sorted(regions, key=lambda x: x[0])
    merged = [sorted_regions[0]]
    
    for start, end in sorted_regions[1:]:
        last_start, last_end = merged[-1]
        
        # If regions overlap or are close (within gap_threshold)
        if start <= last_end + gap_threshold:
            # Merge by extending the last region
            merged[-1] = (last_start, max(last_end, end))
        else:
            # Add as new region
            merged.append((start, end))
    
    return merged


if __name__ == "__main__":
    print("This module provides the fixed waveform alignment algorithm.")
    print("Import and use align_audio_to_transcript_pattern() in your scripts.")

# Made with Bob
