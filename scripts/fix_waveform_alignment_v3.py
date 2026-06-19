#!/usr/bin/env python3
"""
Fixed waveform alignment algorithm V3.
Uses smoothed energy patterns for both transcript and audio (like original).
"""

import numpy as np
from scipy.ndimage import gaussian_filter1d


def generate_transcript_pattern(transcript_segments, duration, sr=10):
    """
    Generate smoothed pattern from transcript: speech regions = 1.0, silence = 0.0.
    Applies Gaussian smoothing to create energy envelope.
    """
    samples = int(duration * sr)
    pattern = np.zeros(samples, dtype=np.float32)
    
    for seg in transcript_segments:
        start_sample = int(seg['start'] * sr)
        end_sample = int(seg['end'] * sr)
        end_sample = min(end_sample, samples)
        if end_sample > start_sample:
            pattern[start_sample:end_sample] = 1.0
    
    # Smooth with gaussian filter to simulate energy envelope
    pattern = gaussian_filter1d(pattern, sigma=sr*0.05)  # 50ms smoothing
    
    return pattern


def generate_audio_pattern(audio, sr, target_sr=2):
    """
    Generate smoothed binary pattern from audio energy.
    Uses RMS energy with thresholding and Gaussian smoothing to match transcript pattern format.
    """
    # Calculate RMS energy on original audio at target resolution
    # Window size in original sample rate (500ms windows)
    window_size = int(0.5 * sr)
    hop = window_size
    
    energy = []
    for i in range(0, len(audio) - window_size, hop):
        window = audio[i:i+window_size]
        rms = np.sqrt(np.mean(window**2))
        energy.append(rms)
    
    energy = np.array(energy)
    
    # Normalize to 0-1 range
    if len(energy) > 0 and energy.max() > 0:
        energy = energy / energy.max()
    
    # Threshold to create binary pattern (speech=1, silence=0)
    # Use 0.3 threshold which gives ~88% speech, close to typical podcast density
    binary_pattern = (energy > 0.3).astype(np.float32)
    
    # Smooth with gaussian filter to match transcript pattern
    # Sigma in samples at target_sr (50ms = 0.05 * 2 = 0.1 samples, use at least 0.2)
    smoothed = gaussian_filter1d(binary_pattern, sigma=max(0.2, target_sr*0.05))
    
    return smoothed


def align_audio_to_transcript_continuous(audio, sr, transcript_segments):
    """
    Align audio to transcript using smoothed energy patterns.
    
    Simplified algorithm:
    1. Create smoothed pattern from transcript (speech regions smoothed)
    2. Create smoothed energy pattern from audio
    3. Find initial offset using cross-correlation
    4. Keep entire audio from that offset, assuming continuous alignment
    """
    print("  Creating transcript pattern...")
    transcript_duration = max(seg['end'] for seg in transcript_segments)
    pattern_sr = 2  # 2 Hz = 500ms resolution (much faster for long files)
    transcript_pattern = generate_transcript_pattern(transcript_segments, transcript_duration, pattern_sr)
    
    print(f"  Transcript pattern: {transcript_duration:.1f}s, {len(transcript_pattern)} samples")
    print(f"    Speech samples: {np.sum(transcript_pattern):.0f} ({np.sum(transcript_pattern)/len(transcript_pattern)*100:.1f}%)")
    
    print("  Creating audio pattern...")
    audio_pattern = generate_audio_pattern(audio, sr, pattern_sr)
    audio_duration = len(audio_pattern) / pattern_sr
    
    print(f"  Audio pattern: {audio_duration:.1f}s, {len(audio_pattern)} samples")
    print(f"    Speech samples: {np.sum(audio_pattern):.0f} ({np.sum(audio_pattern)/len(audio_pattern)*100:.1f}%)")
    
    print("  Finding initial alignment offset...")
    
    # Use a larger window for initial alignment (30 seconds)
    window_size = int(30.0 * pattern_sr)
    pattern_window = transcript_pattern[:window_size]
    
    # Search for best match in first 2 minutes of audio
    best_corr = -1
    best_offset = 0
    max_search = min(int(120 * pattern_sr), len(audio_pattern) - window_size)
    
    for offset in range(0, max_search, int(1.0 * pattern_sr)):
        audio_window = audio_pattern[offset:offset + window_size]
        
        if len(audio_window) != len(pattern_window):
            continue
        
        # Calculate correlation between binary patterns
        corr = np.corrcoef(pattern_window, audio_window)[0, 1]
        
        if not np.isnan(corr) and corr > best_corr:
            best_corr = corr
            best_offset = offset
    
    print(f"  Best alignment: offset={best_offset/pattern_sr:.1f}s, correlation={best_corr:.3f}")
    
    if best_corr < 0.3:
        print("  WARNING: Low correlation - audio may not match transcript!")
        return audio, [(0, len(audio)/sr)]
    
    # Calculate how much audio to keep
    # Keep from offset to end of audio (transcript should fit within this)
    start_time = best_offset / pattern_sr
    audio_duration_sec = len(audio) / sr
    end_time = audio_duration_sec  # Keep to the end
    
    # Verify the kept duration is reasonable
    kept_duration = end_time - start_time
    if abs(kept_duration - transcript_duration) > 60:
        print(f"  WARNING: Kept duration ({kept_duration:.1f}s) differs significantly from transcript ({transcript_duration:.1f}s)")
    
    kept_regions = [(start_time, end_time)]
    print(f"  Keeping audio from {start_time:.1f}s to {end_time:.1f}s ({end_time-start_time:.1f}s)")
    
    print(f"  Found {len(kept_regions)} continuous region(s)")
    
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
    print("This module provides the fixed waveform alignment algorithm V3.")
    print("Import and use align_audio_to_transcript_continuous() in your scripts.")

# Made with Bob
