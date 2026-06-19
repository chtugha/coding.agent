#!/usr/bin/env python3
"""
Diagnose why pattern matching has low correlation.
Visualize transcript and audio patterns to understand the mismatch.
"""

import json
import numpy as np
import soundfile as sf
import sys
import os

# Add current directory to path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fix_waveform_alignment_v3 import generate_transcript_pattern, generate_audio_pattern

def main():
    # Paths
    transcript_path = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/Gemischtes.Hack.Podcast.Transcript/transcripts/episode_150_seil_seil_seil.json"
    audio_path = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/#150 SEIL SEIL SEIL [07736554-72c1-11eb-8725-67d7ee38f508].mp3"
    
    print("=== Pattern Matching Diagnostic ===\n")
    
    # Load transcript
    print("Loading transcript...")
    with open(transcript_path, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    segments = data['segments']
    transcript_duration = max(seg['end'] for seg in segments)
    print(f"Transcript: {len(segments)} segments, {transcript_duration:.1f}s duration\n")
    
    # Load audio
    print("Loading audio...")
    audio, sr = sf.read(audio_path, dtype='float32')
    if len(audio.shape) > 1:
        audio = np.mean(audio, axis=1)
    print(f"Audio: {len(audio)/sr:.1f}s at {sr}Hz\n")
    
    # Generate patterns
    print("Generating patterns...")
    pattern_sr = 10
    transcript_pattern = generate_transcript_pattern(segments, transcript_duration, pattern_sr)
    audio_pattern = generate_audio_pattern(audio, sr, pattern_sr)
    
    print(f"Transcript pattern: {len(transcript_pattern)} samples")
    print(f"  Speech: {np.sum(transcript_pattern):.0f} ({np.sum(transcript_pattern)/len(transcript_pattern)*100:.1f}%)")
    print(f"  Silence: {len(transcript_pattern) - np.sum(transcript_pattern):.0f} ({(1-np.sum(transcript_pattern)/len(transcript_pattern))*100:.1f}%)")
    
    print(f"\nAudio pattern: {len(audio_pattern)} samples")
    print(f"  Speech: {np.sum(audio_pattern):.0f} ({np.sum(audio_pattern)/len(audio_pattern)*100:.1f}%)")
    print(f"  Silence: {len(audio_pattern) - np.sum(audio_pattern):.0f} ({(1-np.sum(audio_pattern)/len(audio_pattern))*100:.1f}%)")
    
    # Sample first 60 seconds
    print("\n=== First 60 seconds comparison ===")
    sample_len = 60 * pattern_sr
    t_sample = transcript_pattern[:sample_len]
    a_sample = audio_pattern[:sample_len]
    
    print(f"Transcript [0-60s]: {np.sum(t_sample):.0f}/{len(t_sample)} speech ({np.sum(t_sample)/len(t_sample)*100:.1f}%)")
    print(f"Audio [0-60s]: {np.sum(a_sample):.0f}/{len(a_sample)} speech ({np.sum(a_sample)/len(a_sample)*100:.1f}%)")
    
    # Try different offsets
    print("\n=== Testing different offsets ===")
    window_size = 30 * pattern_sr
    t_window = transcript_pattern[:window_size]
    
    for offset_sec in [0, 30, 60, 90, 120]:
        offset = offset_sec * pattern_sr
        if offset + window_size > len(audio_pattern):
            break
        
        a_window = audio_pattern[offset:offset + window_size]
        corr = np.corrcoef(t_window, a_window)[0, 1]
        
        print(f"Offset {offset_sec:3d}s: correlation = {corr:.3f}")
    
    # Show pattern samples
    print("\n=== Pattern samples (first 10 seconds) ===")
    print("Time | Transcript | Audio")
    print("-----|------------|------")
    for i in range(0, min(100, len(transcript_pattern)), 10):
        t_val = int(transcript_pattern[i])
        a_val = int(audio_pattern[i]) if i < len(audio_pattern) else 0
        print(f"{i/pattern_sr:4.1f}s |     {t_val}      |   {a_val}")

if __name__ == "__main__":
    main()

# Made with Bob
