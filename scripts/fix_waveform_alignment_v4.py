#!/usr/bin/env python3
"""
Fixed waveform alignment algorithm V4.
Implements continuous tracking with ad detection:
1. Align at start
2. Follow matching pattern
3. When correlation breaks → detect ad, cut and slide
4. Resume when patterns match again

Uses word-level granularity for better pattern matching.
"""

import numpy as np
from scipy.ndimage import gaussian_filter1d


def generate_transcript_pattern_word_level(transcript_segments, duration, sr=2):
    """
    Generate smoothed pattern from transcript using word-level granularity.
    Distributes words evenly within each segment for finer-grained pattern.
    """
    samples = int(duration * sr)
    pattern = np.zeros(samples, dtype=np.float32)
    
    for seg in transcript_segments:
        # Get words in segment (split on whitespace)
        words = seg['text'].strip().split()
        if not words:
            continue
        
        seg_duration = seg['end'] - seg['start']
        if seg_duration <= 0:
            continue
        
        # Distribute words evenly across segment duration
        word_duration = seg_duration / len(words)
        
        for i, word in enumerate(words):
            word_start = seg['start'] + (i * word_duration)
            word_end = word_start + word_duration
            
            start_sample = int(word_start * sr)
            end_sample = int(word_end * sr)
            end_sample = min(end_sample, samples)
            
            if end_sample > start_sample:
                pattern[start_sample:end_sample] = 1.0
    
    # Smooth with gaussian filter to simulate energy envelope
    pattern = gaussian_filter1d(pattern, sigma=max(0.2, sr*0.05))
    
    return pattern


def generate_audio_pattern(audio, sr, target_sr=2):
    """
    Generate smoothed binary pattern from audio energy.
    Uses RMS energy with thresholding and Gaussian smoothing.
    """
    # Calculate RMS energy on original audio at target resolution
    window_size = int(0.5 * sr)  # 500ms windows
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
    
    # Threshold to create binary pattern
    binary_pattern = (energy > 0.3).astype(np.float32)
    
    # Smooth with gaussian filter
    smoothed = gaussian_filter1d(binary_pattern, sigma=max(0.2, target_sr*0.05))
    
    return smoothed


def align_audio_to_transcript_continuous_tracking(audio, sr, transcript_segments):
    """
    Align audio to transcript using continuous tracking with ad detection.
    
    Algorithm:
    1. Create word-level pattern from transcript
    2. Create energy pattern from audio
    3. Find initial alignment
    4. Track continuously:
       - While correlation is good → keep audio
       - When correlation drops → ad detected, slide forward in audio
       - Resume when correlation recovers
    """
    print("  Creating word-level transcript pattern...")
    transcript_duration = max(seg['end'] for seg in transcript_segments)
    pattern_sr = 2  # 2 Hz = 500ms resolution
    transcript_pattern = generate_transcript_pattern_word_level(transcript_segments, transcript_duration, pattern_sr)
    
    print(f"  Transcript pattern: {transcript_duration:.1f}s, {len(transcript_pattern)} samples")
    print(f"    Speech samples: {np.sum(transcript_pattern > 0.5):.0f} ({np.sum(transcript_pattern > 0.5)/len(transcript_pattern)*100:.1f}%)")
    
    print("  Creating audio pattern...")
    audio_pattern = generate_audio_pattern(audio, sr, pattern_sr)
    audio_duration = len(audio_pattern) / pattern_sr
    
    print(f"  Audio pattern: {audio_duration:.1f}s, {len(audio_pattern)} samples")
    print(f"    Speech samples: {np.sum(audio_pattern > 0.5):.0f} ({np.sum(audio_pattern > 0.5)/len(audio_pattern)*100:.1f}%)")
    
    print("  Continuous tracking with ad detection...")
    
    # Parameters
    window_size = int(30.0 * pattern_sr)  # 30-second tracking window
    step_size = int(1.0 * pattern_sr)  # 1-second step
    correlation_threshold = 0.4  # Threshold for good match
    max_ad_search = int(120 * pattern_sr)  # Search up to 2 minutes for content resume
    
    # Track alignment
    pattern_pos = 0
    audio_pos = 0
    kept_regions = []
    current_region_start = None
    
    # Find initial alignment
    print("  Finding initial alignment...")
    best_corr = -1
    best_offset = 0
    search_range = min(int(120 * pattern_sr), len(audio_pattern) - window_size)
    
    for offset in range(0, search_range, step_size):
        if pattern_pos + window_size > len(transcript_pattern):
            break
        
        pattern_window = transcript_pattern[pattern_pos:pattern_pos + window_size]
        audio_window = audio_pattern[offset:offset + window_size]
        
        if len(audio_window) != len(pattern_window):
            continue
        
        corr = np.corrcoef(pattern_window, audio_window)[0, 1]
        if not np.isnan(corr) and corr > best_corr:
            best_corr = corr
            best_offset = offset
    
    if best_corr < 0.3:
        print(f"  WARNING: Low initial correlation ({best_corr:.3f})")
        return audio, [(0, len(audio)/sr)]
    
    audio_pos = best_offset
    current_region_start = audio_pos / pattern_sr
    print(f"  Initial alignment: audio {current_region_start:.1f}s, correlation={best_corr:.3f}")
    
    # Start tracking from initial position
    # Advance past the initial window since we already verified it
    pattern_pos += window_size
    audio_pos += window_size
    
    # Continuous tracking
    while pattern_pos < len(transcript_pattern):
        # Get current window
        pattern_end = min(pattern_pos + window_size, len(transcript_pattern))
        pattern_window = transcript_pattern[pattern_pos:pattern_end]
        
        if len(pattern_window) < window_size // 2:
            break
        
        # Check correlation at current position
        audio_end = min(audio_pos + len(pattern_window), len(audio_pattern))
        audio_window = audio_pattern[audio_pos:audio_end]
        
        if len(audio_window) != len(pattern_window):
            break
        
        corr = np.corrcoef(pattern_window, audio_window)[0, 1]
        
        if not np.isnan(corr) and corr > correlation_threshold:
            # Good match - continue tracking
            advance = step_size
            pattern_pos += advance
            audio_pos += advance
            
        else:
            # Correlation dropped - ad detected
            if current_region_start is not None:
                region_end = audio_pos / pattern_sr
                kept_regions.append((current_region_start, region_end))
                print(f"    Ad detected at audio {region_end:.1f}s (corr={corr:.3f})")
                current_region_start = None
            
            # Slide forward in audio to find where content resumes
            print(f"    Searching for content resume...")
            found_resume = False
            
            for slide_offset in range(audio_pos, min(audio_pos + max_ad_search, len(audio_pattern) - len(pattern_window)), step_size):
                test_window = audio_pattern[slide_offset:slide_offset + len(pattern_window)]
                
                if len(test_window) != len(pattern_window):
                    continue
                
                test_corr = np.corrcoef(pattern_window, test_window)[0, 1]
                
                if not np.isnan(test_corr) and test_corr > correlation_threshold:
                    # Found resume point
                    audio_pos = slide_offset
                    current_region_start = audio_pos / pattern_sr
                    ad_duration = (audio_pos - (kept_regions[-1][1] * pattern_sr if kept_regions else 0)) / pattern_sr
                    print(f"    Content resumes at audio {current_region_start:.1f}s (ad duration: {ad_duration:.1f}s, corr={test_corr:.3f})")
                    found_resume = True
                    break
            
            if not found_resume:
                print(f"    WARNING: Could not find content resume point")
                break
    
    # Close final region
    if current_region_start is not None:
        region_end = audio_pos / pattern_sr
        kept_regions.append((current_region_start, region_end))
        print(f"    Final region ends at audio {region_end:.1f}s")
    
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
            chunk = audio[start_sample:end_sample]
            cleaned_chunks.append(chunk)
            print(f"    Keeping: {start_time:.1f}s - {end_time:.1f}s ({end_time-start_time:.1f}s)")
    
    if not cleaned_chunks:
        print("  WARNING: No audio chunks extracted!")
        return audio, [(0, len(audio)/sr)]
    
    # Concatenate chunks
    cleaned_audio = np.concatenate(cleaned_chunks)
    
    cleaned_duration = len(cleaned_audio) / sr
    print(f"  Cleaned: {len(audio)/sr:.1f}s → {cleaned_duration:.1f}s")
    print(f"  Expected: {transcript_duration:.1f}s")
    print(f"  Difference: {abs(cleaned_duration - transcript_duration):.1f}s")
    
    return cleaned_audio, kept_regions

# Made with Bob
