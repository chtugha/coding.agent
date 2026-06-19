#!/usr/bin/env python3
"""
Analyze why audio pattern shows 46% speech vs transcript 84% speech.
"""

import json
import numpy as np
import soundfile as sf
import sys
import os
from scipy.ndimage import gaussian_filter1d

# Add current directory to path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

def generate_transcript_pattern(transcript_segments, duration, sr=2):
    """Generate smoothed pattern from transcript."""
    samples = int(duration * sr)
    pattern = np.zeros(samples, dtype=np.float32)
    
    for seg in transcript_segments:
        start_sample = int(seg['start'] * sr)
        end_sample = int(seg['end'] * sr)
        end_sample = min(end_sample, samples)
        if end_sample > start_sample:
            pattern[start_sample:end_sample] = 1.0
    
    # Smooth with gaussian filter
    pattern = gaussian_filter1d(pattern, sigma=sr*0.05)
    return pattern

def generate_audio_pattern(audio, sr, target_sr=2):
    """Generate smoothed energy pattern from audio."""
    window_size = int(0.5 * sr)
    hop = window_size
    
    energy = []
    for i in range(0, len(audio) - window_size, hop):
        window = audio[i:i+window_size]
        rms = np.sqrt(np.mean(window**2))
        energy.append(rms)
    
    energy = np.array(energy)
    
    if len(energy) > 0 and energy.max() > 0:
        energy = energy / energy.max()
    
    energy = gaussian_filter1d(energy, sigma=max(0.2, target_sr*0.05))
    return energy

def main():
    # Paths
    transcript_path = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/Gemischtes.Hack.Podcast.Transcript/transcripts/episode_150_seil_seil_seil.json"
    audio_path = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/#150 SEIL SEIL SEIL [07736554-72c1-11eb-8725-67d7ee38f508].mp3"
    
    print("=== Pattern Correlation Analysis ===\n")
    
    # Load transcript
    with open(transcript_path, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    segments = data['segments']
    transcript_duration = max(seg['end'] for seg in segments)
    
    # Load audio
    audio, sr = sf.read(audio_path, dtype='float32')
    if len(audio.shape) > 1:
        audio = np.mean(audio, axis=1)
    
    # Generate patterns
    pattern_sr = 2
    t_pattern = generate_transcript_pattern(segments, transcript_duration, pattern_sr)
    a_pattern = generate_audio_pattern(audio, sr, pattern_sr)
    
    print(f"Transcript pattern: {len(t_pattern)} samples, {np.sum(t_pattern > 0.5):.0f} speech ({np.sum(t_pattern > 0.5)/len(t_pattern)*100:.1f}%)")
    print(f"Audio pattern: {len(a_pattern)} samples, {np.sum(a_pattern > 0.5):.0f} speech ({np.sum(a_pattern > 0.5)/len(a_pattern)*100:.1f}%)")
    
    # Try different thresholds for audio
    print("\n=== Audio pattern at different thresholds ===")
    for thresh in [0.1, 0.2, 0.3, 0.4, 0.5, 0.6]:
        speech_pct = np.sum(a_pattern > thresh) / len(a_pattern) * 100
        print(f"Threshold {thresh:.1f}: {speech_pct:.1f}% speech")
    
    # Check energy distribution
    print("\n=== Audio energy distribution ===")
    print(f"Min: {a_pattern.min():.3f}")
    print(f"Max: {a_pattern.max():.3f}")
    print(f"Mean: {a_pattern.mean():.3f}")
    print(f"Median: {np.median(a_pattern):.3f}")
    for percentile in [10, 25, 50, 75, 90]:
        val = np.percentile(a_pattern, percentile)
        print(f"{percentile}th percentile: {val:.3f}")
    
    # Sample comparison
    print("\n=== Sample comparison (first 20 seconds) ===")
    print("Time | Transcript | Audio")
    print("-----|------------|-------")
    for i in range(0, min(40, len(t_pattern)), 2):
        t_val = t_pattern[i]
        a_val = a_pattern[i] if i < len(a_pattern) else 0
        print(f"{i/pattern_sr:4.1f}s |   {t_val:.3f}   | {a_val:.3f}")

if __name__ == "__main__":
    main()

# Made with Bob
