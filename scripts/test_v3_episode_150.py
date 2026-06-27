#!/usr/bin/env python3
"""Test V3 waveform alignment on episode 150 for comparison with V4"""

import json
import time
import librosa
import soundfile as sf
from fix_waveform_alignment_v3 import align_audio_to_transcript_continuous

# Paths
audio_path = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/transcripts_aligned_final/episode_150_gemischtes_hack_cleaned.wav"
transcript_path = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/transcripts_aligned_final/episode_150_gemischtes_hack.json"
output_path = "/Volumes/eHDD/moshi-rag-data/datasets/whisper_cpp_test/episode_150_cleaned_v3.wav"
regions_path = "/Volumes/eHDD/moshi-rag-data/datasets/whisper_cpp_test/episode_150_kept_regions_v3.json"

print("="*80)
print("Testing V3 Waveform Alignment - Episode 150")
print("="*80)

# Load transcript
print("\n1. Loading transcript...")
with open(transcript_path, 'r') as f:
    transcript_data = json.load(f)

segments = transcript_data['segments']
print(f"   Loaded {len(segments)} segments")

# Load audio
print("\n2. Loading audio...")
start_time = time.time()
audio, sr = librosa.load(audio_path, sr=None, mono=True)
load_time = time.time() - start_time
duration = len(audio) / sr
print(f"   Sample rate: {sr} Hz")
print(f"   Duration: {duration:.1f} seconds ({duration/60:.1f} minutes)")
print(f"   Load time: {load_time:.2f}s")

# Run V3 alignment
print("\n3. Running V3 alignment...")
start_time = time.time()
cleaned_audio, kept_regions = align_audio_to_transcript_continuous(audio, sr, segments)
align_time = time.time() - start_time

print(f"   Alignment time: {align_time:.2f}s")
print(f"   Kept regions: {len(kept_regions)}")

# Calculate statistics
original_duration = len(audio) / sr
cleaned_duration = len(cleaned_audio) / sr
removed_duration = original_duration - cleaned_duration
removal_pct = (removed_duration / original_duration) * 100

print(f"\n4. Results:")
print(f"   Original duration:  {original_duration:.1f}s ({original_duration/60:.1f} min)")
print(f"   Cleaned duration:   {cleaned_duration:.1f}s ({cleaned_duration/60:.1f} min)")
print(f"   Removed duration:   {removed_duration:.1f}s ({removed_duration/60:.1f} min)")
print(f"   Removal percentage: {removal_pct:.2f}%")

# Save cleaned audio
print(f"\n5. Saving cleaned audio to: {output_path}")
sf.write(output_path, cleaned_audio, sr)

# Save kept regions
print(f"   Saving kept regions to: {regions_path}")
with open(regions_path, 'w') as f:
    json.dump(kept_regions, f, indent=2)

print("\n" + "="*80)
print("V3 ALIGNMENT COMPLETE")
print("="*80)
print(f"\nProcessing speed: {duration/align_time:.1f}x realtime")
print(f"Total time: {align_time:.2f}s for {duration/60:.1f} minutes of audio")

# Made with Bob