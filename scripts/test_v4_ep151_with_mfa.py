#!/usr/bin/env python3
"""
Test V4 alignment on episode 151, run MFA, and compare to WhisperX ground truth
"""

import json
import time
import librosa
import soundfile as sf
import subprocess
import os
from fix_waveform_alignment_v4_optimized import align_audio_to_transcript_fast

print("="*80)
print("V4 + MFA Test on Episode 151")
print("="*80)

# Paths
audio_path = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/#151 ALLE HEISSEN TIM UND ALLES MUSS RAUS [077773f6-72c1-11eb-8725-738ddadbf5bb].mp3"
transcript_path = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/transcripts/episode_151_alle_heissen_tim_und_alles_muss_raus.json"
v4_cleaned_path = "/tmp/fixed_alignment_test/episode_151_cleaned_v4.wav"
v4_mfa_output = "/tmp/fixed_alignment_test/episode_151_mfa_v4.json"
whisperx_ground_truth = "/tmp/fixed_alignment_test/episode_151_whisperx_validation.json"

# Step 1: Run V4 alignment
print("\n1. Running V4 alignment on episode 151...")
with open(transcript_path, 'r') as f:
    transcript_data = json.load(f)
segments = transcript_data['segments']
print(f"   Loaded {len(segments)} segments")

print("   Loading audio...")
start_time = time.time()
audio, sr = librosa.load(audio_path, sr=None, mono=True)
load_time = time.time() - start_time
print(f"   Audio: {len(audio)/sr:.1f}s at {sr}Hz (loaded in {load_time:.2f}s)")

print("   Running V4 alignment...")
start_time = time.time()
cleaned_audio, kept_regions = align_audio_to_transcript_fast(audio, sr, segments)
align_time = time.time() - start_time
print(f"   Alignment complete in {align_time:.2f}s")
print(f"   Original: {len(audio)/sr:.1f}s → Cleaned: {len(cleaned_audio)/sr:.1f}s")

print(f"   Saving cleaned audio to: {v4_cleaned_path}")
sf.write(v4_cleaned_path, cleaned_audio, int(sr))

# Step 2: Run MFA on V4-cleaned audio
print("\n2. Running MFA on V4-cleaned audio...")
print("   This will take several minutes...")

# Create directory structure for MFA
mfa_input_dir = "/tmp/fixed_alignment_test/mfa_v4_input"
os.makedirs(mfa_input_dir, exist_ok=True)

# Copy audio to MFA input directory
import shutil
mfa_audio_path = os.path.join(mfa_input_dir, "episode_151_v4.wav")
shutil.copy(v4_cleaned_path, mfa_audio_path)

# Create text file for MFA in the same directory
text_content = " ".join([seg['text'] for seg in segments])
text_path = os.path.join(mfa_input_dir, "episode_151_v4.txt")
with open(text_path, 'w', encoding='utf-8') as f:
    f.write(text_content)

# Run MFA
mfa_cmd = [
    "mfa", "align",
    "--clean",
    "--single_speaker",
    mfa_input_dir,
    "german_mfa",
    "german_mfa",
    "/tmp/fixed_alignment_test/mfa_v4_output"
]

print(f"   Running: {' '.join(mfa_cmd)}")
start_time = time.time()
result = subprocess.run(mfa_cmd, capture_output=True, text=True)
mfa_time = time.time() - start_time

if result.returncode != 0:
    print(f"   MFA failed with error:")
    print(result.stderr)
    exit(1)

print(f"   MFA complete in {mfa_time:.1f}s")

# Step 3: Parse MFA output
print("\n3. Parsing MFA output...")
textgrid_path = "/tmp/fixed_alignment_test/mfa_v4_output/episode_151_v4.TextGrid"

if not os.path.exists(textgrid_path):
    print(f"   ERROR: TextGrid not found at {textgrid_path}")
    exit(1)

# Parse TextGrid (simple parser)
mfa_words = []
with open(textgrid_path, 'r', encoding='utf-8') as f:
    lines = f.readlines()
    
in_words_tier = False
i = 0
while i < len(lines):
    line = lines[i].strip()
    
    if 'name = "words"' in line:
        in_words_tier = True
    elif in_words_tier and line.startswith('xmin ='):
        start = float(line.split('=')[1].strip())
        i += 1
        end = float(lines[i].strip().split('=')[1].strip())
        i += 1
        text = lines[i].strip().split('=')[1].strip().strip('"')
        if text and text != '':
            mfa_words.append({
                'word': text,
                'start': start,
                'end': end
            })
    i += 1

print(f"   Parsed {len(mfa_words)} words from MFA output")

# Save MFA results
with open(v4_mfa_output, 'w') as f:
    json.dump(mfa_words, f, indent=2, ensure_ascii=False)
print(f"   Saved to: {v4_mfa_output}")

# Step 4: Load WhisperX ground truth
print("\n4. Loading WhisperX ground truth...")
with open(whisperx_ground_truth, 'r') as f:
    whisperx_data = json.load(f)

whisperx_words = []
for seg in whisperx_data.get('segments', []):
    for word_info in seg.get('words', []):
        whisperx_words.append({
            'word': word_info['word'].strip(),
            'start': word_info['start'],
            'end': word_info['end']
        })

print(f"   Loaded {len(whisperx_words)} words from WhisperX")

# Step 5: Compare MFA (V4) to WhisperX
print("\n5. Comparing MFA (V4-cleaned) to WhisperX ground truth...")
print("   Applying +0.23s offset to WhisperX (from previous analysis)")

# Apply offset
whisperx_offset = 0.23
for w in whisperx_words:
    w['start'] += whisperx_offset
    w['end'] += whisperx_offset

# Align words by matching text
matched_pairs = []
mfa_idx = 0
wx_idx = 0

while mfa_idx < len(mfa_words) and wx_idx < len(whisperx_words):
    mfa_word = mfa_words[mfa_idx]['word'].lower()
    wx_word = whisperx_words[wx_idx]['word'].lower()
    
    if mfa_word == wx_word:
        matched_pairs.append((mfa_words[mfa_idx], whisperx_words[wx_idx]))
        mfa_idx += 1
        wx_idx += 1
    elif len(mfa_word) < len(wx_word) and wx_word.startswith(mfa_word):
        # MFA might have split a word
        mfa_idx += 1
    elif len(wx_word) < len(mfa_word) and mfa_word.startswith(wx_word):
        # WhisperX might have split a word
        wx_idx += 1
    else:
        # Skip mismatches
        mfa_idx += 1
        wx_idx += 1

print(f"   Matched {len(matched_pairs)} word pairs")

# Calculate errors
errors = []
for mfa_w, wx_w in matched_pairs:
    start_error = abs(mfa_w['start'] - wx_w['start'])
    end_error = abs(mfa_w['end'] - wx_w['end'])
    errors.append({
        'word': mfa_w['word'],
        'mfa_start': mfa_w['start'],
        'wx_start': wx_w['start'],
        'start_error': start_error,
        'end_error': end_error
    })

# Statistics
start_errors = [e['start_error'] for e in errors]
start_errors.sort()

print(f"\n6. Results:")
print(f"   Total matched words: {len(errors)}")
print(f"   Mean start error: {sum(start_errors)/len(start_errors):.3f}s")
print(f"   Median start error: {start_errors[len(start_errors)//2]:.3f}s")
print(f"   95th percentile: {start_errors[int(len(start_errors)*0.95)]:.3f}s")
print(f"   Max error: {max(start_errors):.3f}s")

# Error distribution
under_100ms = sum(1 for e in start_errors if e < 0.1)
under_500ms = sum(1 for e in start_errors if e < 0.5)
under_1s = sum(1 for e in start_errors if e < 1.0)

print(f"\n   Error distribution:")
print(f"   <100ms: {under_100ms} ({under_100ms/len(start_errors)*100:.1f}%)")
print(f"   <500ms: {under_500ms} ({under_500ms/len(start_errors)*100:.1f}%)")
print(f"   <1.0s:  {under_1s} ({under_1s/len(start_errors)*100:.1f}%)")

print("\n" + "="*80)
print("V4 + MFA TEST COMPLETE")
print("="*80)

# Made with Bob