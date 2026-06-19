#!/usr/bin/env python3
"""
Test fixed waveform alignment on Episode 151.
"""

import json
import numpy as np
import soundfile as sf
import os
import sys

# Add current directory to path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from fix_waveform_alignment_v3 import align_audio_to_transcript_continuous

def main():
    # Paths for episode 151
    base_dir = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast"
    
    # Find episode 151 files
    import glob
    audio_files = glob.glob(os.path.join(base_dir, "*151*.mp3"))
    transcript_files = glob.glob(os.path.join(base_dir, "Gemischtes.Hack.Podcast.Transcript/transcripts/*151*.json"))
    
    if not audio_files:
        print("ERROR: Could not find episode 151 audio file")
        return
    
    if not transcript_files:
        print("ERROR: Could not find episode 151 transcript file")
        return
    
    audio_path = audio_files[0]
    transcript_path = transcript_files[0]
    
    print("=== Testing Fixed Waveform Alignment on Episode 151 ===\n")
    print(f"Audio: {os.path.basename(audio_path)}")
    print(f"Transcript: {os.path.basename(transcript_path)}\n")
    
    # Load transcript
    print("Loading transcript...")
    with open(transcript_path, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    segments = data['segments']
    transcript_duration = max(seg['end'] for seg in segments)
    
    print(f"Transcript: {len(segments)} segments, {transcript_duration:.1f}s duration\n")
    
    # Check for large gaps (potential ads)
    print("Checking for large gaps in transcript (potential ads)...")
    gaps = []
    for i in range(len(segments) - 1):
        gap = segments[i+1]['start'] - segments[i]['end']
        if gap > 30:  # Gaps larger than 30 seconds
            gaps.append({
                'position': segments[i]['end'],
                'duration': gap,
                'before': segments[i]['text'][-50:],
                'after': segments[i+1]['text'][:50]
            })
    
    if gaps:
        print(f"Found {len(gaps)} large gaps (>30s):")
        for gap in gaps:
            print(f"  At {gap['position']:.1f}s: {gap['duration']:.1f}s gap")
            print(f"    Before: ...{gap['before']}")
            print(f"    After: {gap['after']}...")
    else:
        print("No large gaps found (episode likely has no mid-content ads)")
    print()
    
    # Load audio
    print("Loading audio...")
    audio, sr = sf.read(audio_path, dtype='float32')
    
    # Convert stereo to mono if needed
    if len(audio.shape) > 1:
        audio = np.mean(audio, axis=1)
    
    print(f"Audio: {len(audio)/sr:.1f}s at {sr}Hz\n")
    
    # Run alignment
    print("=" * 60)
    print("Running FIXED waveform alignment...")
    print("=" * 60)
    print()
    
    cleaned_audio, kept_regions = align_audio_to_transcript_continuous(
        audio, sr, segments
    )
    
    # Results
    print()
    print("=" * 60)
    print("Alignment Results")
    print("=" * 60)
    
    original_duration = len(audio) / sr
    cleaned_duration = len(cleaned_audio) / sr
    difference = abs(cleaned_duration - transcript_duration)
    
    print(f"Original audio: {original_duration:.1f}s")
    print(f"Cleaned audio: {cleaned_duration:.1f}s")
    print(f"Transcript duration: {transcript_duration:.1f}s")
    print(f"Difference: {difference:.1f}s")
    print(f"Kept regions: {len(kept_regions)}")
    print()
    
    removed_duration = original_duration - cleaned_duration
    removed_pct = (removed_duration / original_duration) * 100
    print(f"Removed: {removed_duration:.1f}s ({removed_pct:.1f}%)")
    print()
    
    # Save results
    output_dir = "/tmp/fixed_alignment_test"
    os.makedirs(output_dir, exist_ok=True)
    
    output_audio = os.path.join(output_dir, "episode_151_cleaned_fixed.wav")
    sf.write(output_audio, cleaned_audio, sr)
    print(f"Saved cleaned audio: {output_audio}")
    
    output_regions = os.path.join(output_dir, "kept_regions_ep151.json")
    with open(output_regions, 'w') as f:
        json.dump(kept_regions, f, indent=2)
    print(f"Saved regions info: {output_regions}")
    
    # Assessment
    print()
    print("=" * 60)
    print("Assessment")
    print("=" * 60)
    
    if difference < 5:
        print("✓ EXCELLENT: Duration matches transcript within 5s")
        print("  This should result in accurate MFA timestamps (<100ms error)")
    elif difference < 30:
        print("⚠ ACCEPTABLE: Duration within 30s of transcript")
        print("  MFA timestamps may have some error (200-500ms)")
    else:
        print("❌ POOR: Duration differs by {:.1f}s".format(difference))
        print("  Alignment algorithm needs tuning")
    
    print()
    if gaps:
        print(f"Note: Episode has {len(gaps)} large gaps in transcript")
        print("If cleaned duration doesn't match, gaps may contain ads that need removal")
    
    print("\nNext step: Run MFA alignment on cleaned audio and validate with WhisperX")

if __name__ == "__main__":
    main()

# Made with Bob
