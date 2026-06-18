#!/usr/bin/env python3
"""
Diagnose waveform alignment issues for episode 150.
"""

import os
import json
import numpy as np
import librosa
import soundfile as sf
from pathlib import Path
import sys

# Add parent directory to path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prepare_german_dataset import (
    clean_podcast_ads_waveform_based,
    generate_transcript_waveform,
    extract_audio_envelope,
    find_alignment_with_cross_correlation
)

# Paths
DATASET_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast"
TRANSCRIPT_DIR = os.path.join(DATASET_DIR, "Gemischtes.Hack.Podcast.Transcript/transcripts")
AUDIO_FILE = os.path.join(DATASET_DIR, "#150 SEIL SEIL SEIL (mit Tommi Schmitt).mp3")
TRANSCRIPT_FILE = os.path.join(TRANSCRIPT_DIR, "episode_150_gemischtes_hack.json")

def main():
    print("=== Waveform Alignment Diagnostic ===\n")
    
    # Load transcript
    print("Loading transcript...")
    with open(TRANSCRIPT_FILE, 'r') as f:
        transcript = json.load(f)
    
    segments = transcript.get('segments', [])
    print(f"Transcript segments: {len(segments)}")
    
    # Show first few segments
    print("\nFirst 5 segments:")
    for i, seg in enumerate(segments[:5]):
        print(f"  {i}: {seg['start']:.2f}-{seg['end']:.2f}s: {seg['text'][:50]}...")
    
    # Load audio
    print(f"\nLoading audio...")
    mono, sr = librosa.load(AUDIO_FILE, sr=48000, mono=True)
    mono = mono.astype(np.float32)
    audio_duration = len(mono) / sr
    print(f"Audio: {audio_duration:.1f}s at {sr}Hz")
    
    # Generate transcript waveform pattern
    print(f"\nGenerating transcript waveform pattern...")
    transcript_wf = generate_transcript_waveform(segments, audio_duration, int(sr))
    print(f"Transcript waveform: {len(transcript_wf)} samples")
    
    # Calculate how much of transcript is "speech" vs "silence"
    speech_samples = np.sum(transcript_wf > 0.5)
    speech_duration = speech_samples / sr
    silence_duration = audio_duration - speech_duration
    print(f"Transcript pattern: {speech_duration:.1f}s speech, {silence_duration:.1f}s silence")
    
    # Extract audio envelope
    print(f"\nExtracting audio envelope...")
    audio_envelope = extract_audio_envelope(mono, sr, window_size=0.1)
    print(f"Audio envelope: {len(audio_envelope)} points")
    
    # Run cross-correlation
    print(f"\nRunning cross-correlation alignment...")
    offset, correlation, ad_breaks = find_alignment_with_cross_correlation(mono, sr, segments)
    
    print(f"\nAlignment results:")
    print(f"  Offset: {offset:.2f}s")
    print(f"  Correlation: {correlation:.3f}")
    print(f"  Ad breaks detected: {len(ad_breaks)}")
    
    if ad_breaks:
        print(f"\nAd breaks:")
        total_ad_duration = 0
        for i, (start, end) in enumerate(ad_breaks):
            duration = end - start
            total_ad_duration += duration
            print(f"  {i+1}: {start:.1f}s - {end:.1f}s ({duration:.1f}s)")
        print(f"  Total ad duration: {total_ad_duration:.1f}s")
    
    # Clean audio
    print(f"\nCleaning audio...")
    cleaned, offset_returned, ad_breaks_returned = clean_podcast_ads_waveform_based(
        mono, int(sr), TRANSCRIPT_FILE, ep_label="ep150"
    )
    cleaned_duration = len(cleaned) / sr
    removed_duration = audio_duration - cleaned_duration
    
    print(f"\nCleaning results:")
    print(f"  Original audio: {audio_duration:.1f}s")
    print(f"  Cleaned audio: {cleaned_duration:.1f}s")
    print(f"  Removed: {removed_duration:.1f}s")
    print(f"  Offset: {offset_returned:.2f}s")
    
    # Calculate expected cleaned duration from transcript
    transcript_duration = max(seg['end'] for seg in segments)
    print(f"\nExpected vs Actual:")
    print(f"  Transcript duration: {transcript_duration:.1f}s")
    print(f"  Cleaned audio duration: {cleaned_duration:.1f}s")
    print(f"  Difference: {abs(transcript_duration - cleaned_duration):.1f}s")
    
    if abs(transcript_duration - cleaned_duration) > 5:
        print(f"\n⚠️  WARNING: Cleaned audio duration doesn't match transcript!")
        print(f"  This explains the 2.4s average timestamp error.")
        print(f"  The waveform alignment is not cutting the audio correctly.")
    else:
        print(f"\n✓ Cleaned audio duration matches transcript (within 5s)")
    
    # Save cleaned audio for inspection
    output_path = "/tmp/ep150_cleaned_diagnostic.wav"
    sf.write(output_path, cleaned, int(sr))
    print(f"\nSaved cleaned audio to: {output_path}")
    print(f"You can compare this with the transcript to verify alignment.")

if __name__ == "__main__":
    main()

# Made with Bob
