#!/usr/bin/env python3
"""
Fixed waveform alignment algorithm V4 - OPTIMIZED
Implements Upgrade 1: FFT-Based Correlation + Downsampling

Key optimizations:
1. Downsample audio to 8kHz (6x reduction from 48kHz)
2. Vectorized RMS calculation using sliding windows
3. FFT-based cross-correlation (O(n log n) vs O(n²))

Expected speedup: 10-20x faster than V3
Maintains accuracy: 8kHz preserves speech characteristics
"""

import numpy as np
from scipy.ndimage import gaussian_filter1d
from scipy import signal as scipy_signal


def generate_transcript_pattern(transcript_segments, duration, sr=2):
    """
    Generate smoothed pattern from transcript: speech regions = 1.0, silence = 0.0.
    Applies Gaussian smoothing to create energy envelope.
    
    Same as V3 - no changes needed here.
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


def generate_audio_pattern_fast(audio, sr, target_sr=2):
    """
    OPTIMIZED: Generate smoothed binary pattern from audio energy.
    
    Optimizations:
    1. Downsample to 8kHz first (6x reduction)
    2. Vectorized RMS calculation using sliding windows
    3. Maintains same output format as V3
    """
    # OPTIMIZATION 1: Downsample to 8kHz (6x reduction from 48kHz)
    # This preserves speech characteristics while reducing computation
    target_downsample_sr = 8000
    downsample_factor = sr // target_downsample_sr
    
    if downsample_factor > 1:
        # Use scipy.signal.decimate for high-quality downsampling
        audio = scipy_signal.decimate(audio, downsample_factor, ftype='fir', zero_phase=True)
        sr = sr // downsample_factor
        print(f"    Downsampled to {sr} Hz ({downsample_factor}x reduction)")
    
    # OPTIMIZATION 2: Vectorized RMS calculation
    # Use sliding window view instead of loop
    window_size = int(0.5 * sr)  # 500ms windows
    hop = window_size
    
    # Create sliding windows efficiently
    from numpy.lib.stride_tricks import sliding_window_view
    
    # Pad audio to ensure we get all windows
    n_windows = (len(audio) - window_size) // hop + 1
    
    if n_windows > 0:
        # Create windows using stride tricks (no data copying)
        windows = sliding_window_view(audio, window_size)[::hop]
        
        # Vectorized RMS calculation (much faster than loop)
        energy = np.sqrt(np.mean(windows**2, axis=1))
    else:
        # Fallback for very short audio
        energy = np.array([np.sqrt(np.mean(audio**2))])
    
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


def align_audio_to_transcript_fast(audio, sr, transcript_segments):
    """
    OPTIMIZED: Align audio to transcript using FFT-based correlation.
    
    Optimizations:
    1. Uses fast audio pattern generation (downsampled + vectorized)
    2. FFT-based cross-correlation (O(n log n) instead of O(n²))
    3. Maintains same output format as V3
    """
    print("  Creating transcript pattern...")
    transcript_duration = max(seg['end'] for seg in transcript_segments)
    pattern_sr = 2  # 2 Hz = 500ms resolution
    transcript_pattern = generate_transcript_pattern(transcript_segments, transcript_duration, pattern_sr)
    
    print(f"  Transcript pattern: {transcript_duration:.1f}s, {len(transcript_pattern)} samples")
    print(f"    Speech samples: {np.sum(transcript_pattern):.0f} ({np.sum(transcript_pattern)/len(transcript_pattern)*100:.1f}%)")
    
    print("  Creating audio pattern (optimized)...")
    audio_pattern = generate_audio_pattern_fast(audio, sr, pattern_sr)
    audio_duration = len(audio_pattern) / pattern_sr
    
    print(f"  Audio pattern: {audio_duration:.1f}s, {len(audio_pattern)} samples")
    print(f"    Speech samples: {np.sum(audio_pattern):.0f} ({np.sum(audio_pattern)/len(audio_pattern)*100:.1f}%)")
    
    print("  Finding initial alignment offset (FFT-based)...")
    
    # Use a larger window for initial alignment (30 seconds)
    window_size = int(30.0 * pattern_sr)
    pattern_window = transcript_pattern[:window_size]
    
    # Search for best match in first 2 minutes of audio
    max_search = min(int(120 * pattern_sr), len(audio_pattern) - window_size)
    
    if max_search <= 0:
        print("  WARNING: Audio too short for alignment!")
        return audio, [(0, len(audio)/sr)]
    
    # OPTIMIZATION 3: FFT-based cross-correlation
    # Extract search region from audio
    search_audio = audio_pattern[:max_search + window_size]
    
    # Normalize patterns for better correlation
    pattern_norm = (pattern_window - np.mean(pattern_window)) / (np.std(pattern_window) + 1e-8)
    
    # Use FFT-based correlation (much faster than direct method)
    correlation = scipy_signal.correlate(search_audio, pattern_norm, mode='valid', method='fft')
    
    # Find best offset
    best_offset = np.argmax(correlation)
    
    # Calculate normalized correlation coefficient at best offset
    audio_window = search_audio[best_offset:best_offset + window_size]
    audio_norm = (audio_window - np.mean(audio_window)) / (np.std(audio_window) + 1e-8)
    best_corr = np.corrcoef(pattern_norm, audio_norm)[0, 1]
    
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


# Backward compatibility: alias to match V3 function name
align_audio_to_transcript_continuous = align_audio_to_transcript_fast


if __name__ == "__main__":
    print("=" * 70)
    print("Waveform Alignment Algorithm V4 - OPTIMIZED")
    print("=" * 70)
    print()
    print("Optimizations:")
    print("  1. Downsample to 8kHz (6x reduction)")
    print("  2. Vectorized RMS calculation")
    print("  3. FFT-based cross-correlation")
    print()
    print("Expected speedup: 10-20x faster than V3")
    print("Maintains accuracy: 8kHz preserves speech")
    print()
    print("Usage:")
    print("  from fix_waveform_alignment_v4_optimized import align_audio_to_transcript_fast")
    print("  cleaned_audio, regions = align_audio_to_transcript_fast(audio, sr, segments)")
    print()
    print("Or use backward-compatible name:")
    print("  from fix_waveform_alignment_v4_optimized import align_audio_to_transcript_continuous")

# Made with Bob