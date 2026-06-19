#!/usr/bin/env python3
"""
Verify which alignment (V3 or V4) is correct by extracting audio samples
and comparing them to the transcript timestamps.
"""

import json
import soundfile as sf
import librosa

# Load transcript
transcript_path = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/transcripts/episode_153_gedanken_zum_ausdrucken_bringen.json"
with open(transcript_path, 'r') as f:
    data = json.load(f)
segments = data['segments']

# Load both cleaned audio files
print("Loading V3 cleaned audio...")
v3_audio, v3_sr = librosa.load("/tmp/fixed_alignment_test/episode_153_cleaned_v3.wav", sr=None, mono=True)
print(f"V3: {len(v3_audio)/v3_sr:.1f}s at {v3_sr}Hz")

print("\nLoading V4 cleaned audio...")
v4_audio, v4_sr = librosa.load("/tmp/fixed_alignment_test/episode_153_cleaned_v4.wav", sr=None, mono=True)
print(f"V4: {len(v4_audio)/v4_sr:.1f}s at {v4_sr}Hz")

# Extract samples at specific transcript times
test_times = [
    (0.0, 6.6, "What happened to that boy? Ich bin in meiner Blüte wie eine"),
    (1440.3, 1440.9, "So ein Pinguin,"),
    (2880.0, 2885.0, "Around minute 48"),
]

print("\n" + "="*80)
print("EXTRACTING AUDIO SAMPLES FOR VERIFICATION")
print("="*80)

for transcript_start, transcript_end, expected_text in test_times:
    print(f"\nTranscript time: {transcript_start:.1f}s - {transcript_end:.1f}s")
    print(f"Expected text: {expected_text}")
    
    # V3: transcript time maps directly to cleaned audio time (intro was removed)
    v3_start_sample = int(transcript_start * v3_sr)
    v3_end_sample = int(transcript_end * v3_sr)
    if v3_end_sample <= len(v3_audio):
        v3_sample = v3_audio[v3_start_sample:v3_end_sample]
        v3_output = f"/tmp/fixed_alignment_test/v3_sample_{transcript_start:.0f}s.wav"
        sf.write(v3_output, v3_sample, v3_sr)
        print(f"  V3 sample saved: {v3_output}")
    else:
        print(f"  V3: Time {transcript_start:.1f}s exceeds audio length")
    
    # V4: transcript time maps directly to cleaned audio time (intro was removed)
    v4_start_sample = int(transcript_start * v4_sr)
    v4_end_sample = int(transcript_end * v4_sr)
    if v4_end_sample <= len(v4_audio):
        v4_sample = v4_audio[v4_start_sample:v4_end_sample]
        v4_output = f"/tmp/fixed_alignment_test/v4_sample_{transcript_start:.0f}s.wav"
        sf.write(v4_output, v4_sample, v4_sr)
        print(f"  V4 sample saved: {v4_output}")
    else:
        print(f"  V4: Time {transcript_start:.1f}s exceeds audio length")

print("\n" + "="*80)
print("VERIFICATION INSTRUCTIONS")
print("="*80)
print("""
To verify which alignment is correct:

1. Listen to the sample files in /tmp/fixed_alignment_test/
2. Compare what you hear to the expected text
3. The correct alignment will have audio matching the transcript text

Files to check:
- v3_sample_0s.wav vs v4_sample_0s.wav (should say "What happened to that boy?")
- v3_sample_1440s.wav vs v4_sample_1440s.wav (should say "So ein Pinguin,")

If V3 is correct: The audio will match the transcript text
If V4 is correct: The audio will match the transcript text
If neither matches: Both algorithms are wrong!
""")

# Made with Bob