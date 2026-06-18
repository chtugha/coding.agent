#!/usr/bin/env python3
"""
Test the fixed waveform alignment on episode 150.
"""

import os
import json
import numpy as np
import librosa
import soundfile as sf
import sys

# Add current directory to path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fix_waveform_alignment import align_audio_to_transcript_pattern

# Paths
DATASET_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast"
TRANSCRIPT_DIR = os.path.join(DATASET_DIR, "Gemischtes.Hack.Podcast.Transcript/transcripts")
AUDIO_FILE = os.path.join(DATASET_DIR, "#150 SEIL SEIL SEIL (mit Tommi Schmitt).mp3")
TRANSCRIPT_FILE = os.path.join(TRANSCRIPT_DIR, "episode_150_gemischtes_hack.json")
OUTPUT_DIR = "/tmp/fixed_alignment_test"

def main():
    print("=== Testing Fixed Waveform Alignment on Episode 150 ===\n")
    
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    
    # Load transcript
    print("Loading transcript...")
    with open(TRANSCRIPT_FILE, 'r') as f:
        transcript = json.load(f)
    
    segments = transcript.get('segments', [])
    transcript_duration = max(seg['end'] for seg in segments)
    print(f"Transcript: {len(segments)} segments, {transcript_duration:.1f}s duration")
    
    # Load audio
    print(f"\nLoading audio...")
    audio, sr = librosa.load(AUDIO_FILE, sr=48000, mono=True)
    audio = audio.astype(np.float32)
    audio_duration = len(audio) / sr
    print(f"Audio: {audio_duration:.1f}s at {sr}Hz")
    
    # Run fixed alignment
    print(f"\n{'='*60}")
    print("Running FIXED waveform alignment...")
    print(f"{'='*60}\n")
    
    cleaned_audio, kept_regions = align_audio_to_transcript_pattern(
        audio, sr, segments,
        correlation_threshold=0.6,
        min_match_duration=5.0
    )
    
    cleaned_duration = len(cleaned_audio) / sr
    
    print(f"\n{'='*60}")
    print("Alignment Results")
    print(f"{'='*60}")
    print(f"Original audio: {audio_duration:.1f}s")
    print(f"Cleaned audio: {cleaned_duration:.1f}s")
    print(f"Transcript duration: {transcript_duration:.1f}s")
    print(f"Difference: {abs(cleaned_duration - transcript_duration):.1f}s")
    print(f"Kept regions: {len(kept_regions)}")
    
    # Calculate removed duration
    removed_duration = audio_duration - cleaned_duration
    print(f"\nRemoved: {removed_duration:.1f}s ({removed_duration/audio_duration*100:.1f}%)")
    
    # Save cleaned audio
    output_path = os.path.join(OUTPUT_DIR, "episode_150_cleaned_fixed.wav")
    sf.write(output_path, cleaned_audio, int(sr))
    print(f"\nSaved cleaned audio: {output_path}")
    
    # Save kept regions info
    regions_path = os.path.join(OUTPUT_DIR, "kept_regions.json")
    with open(regions_path, 'w') as f:
        json.dump({
            'original_duration': audio_duration,
            'cleaned_duration': cleaned_duration,
            'transcript_duration': transcript_duration,
            'kept_regions': kept_regions
        }, f, indent=2)
    print(f"Saved regions info: {regions_path}")
    
    # Assessment
    print(f"\n{'='*60}")
    print("Assessment")
    print(f"{'='*60}")
    
    duration_diff = abs(cleaned_duration - transcript_duration)
    
    if duration_diff < 5:
        print(f"✓ EXCELLENT: Duration matches transcript within 5s")
        print(f"  This should result in accurate MFA timestamps (<100ms error)")
    elif duration_diff < 10:
        print(f"✓ GOOD: Duration matches transcript within 10s")
        print(f"  MFA timestamps should be acceptable (<200ms error)")
    elif duration_diff < 30:
        print(f"⚠ ACCEPTABLE: Duration within 30s of transcript")
        print(f"  MFA timestamps may have some error (200-500ms)")
    else:
        print(f"❌ POOR: Duration differs by {duration_diff:.1f}s")
        print(f"  Alignment algorithm needs tuning")
    
    print(f"\nNext step: Run MFA alignment on cleaned audio and validate with WhisperX")

if __name__ == "__main__":
    main()

# Made with Bob
