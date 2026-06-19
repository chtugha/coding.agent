#!/usr/bin/env python3
"""Test MFA on episode 1 only"""

import json
import os
import subprocess
import shutil
import sys

# Force immediate output
sys.stdout = os.fdopen(sys.stdout.fileno(), 'w', buffering=1)

print("Testing MFA on Episode 1", flush=True)

# Paths
CLEAN_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/podcast_clean"
audio_path = os.path.join(CLEAN_DIR, "episode_001_cleaned.wav")
metadata_path = os.path.join(CLEAN_DIR, "episode_001_metadata.json")

# Load metadata
print("Loading metadata...", flush=True)
with open(metadata_path, 'r') as f:
    metadata = json.load(f)

transcript_path = metadata['transcript_path']
print(f"Transcript: {transcript_path}", flush=True)

# Load transcript
with open(transcript_path, 'r') as f:
    transcript_data = json.load(f)

segments = transcript_data['segments']
text_content = " ".join([seg['text'] for seg in segments])
print(f"Segments: {len(segments)}, Words: ~{len(text_content.split())}", flush=True)

# Create MFA corpus
corpus_dir = "/tmp/mfa_test_ep1"
output_dir = "/tmp/mfa_test_ep1_output"

os.makedirs(corpus_dir, exist_ok=True)
os.makedirs(output_dir, exist_ok=True)

# Copy audio
print("Copying audio...", flush=True)
mfa_audio = os.path.join(corpus_dir, "episode_001.wav")
shutil.copy(audio_path, mfa_audio)
print(f"Audio: {os.path.getsize(mfa_audio) / 1024 / 1024:.1f} MB", flush=True)

# Create text file
print("Creating text file...", flush=True)
mfa_text = os.path.join(corpus_dir, "episode_001.txt")
with open(mfa_text, 'w', encoding='utf-8') as f:
    f.write(text_content)
print(f"Text: {len(text_content)} chars", flush=True)

# Run MFA
mfa_cmd = [
    "mfa", "align",
    "--clean",
    "--single_speaker",
    "--beam", "100",
    "--retry_beam", "400",
    "--output_format", "json",
    corpus_dir,
    "german_mfa",
    "german_mfa",
    output_dir
]

print(f"\nMFA Command:", flush=True)
print(f"  {' '.join(mfa_cmd)}", flush=True)
print(f"\nRunning MFA...", flush=True)

result = subprocess.run(mfa_cmd, capture_output=True, text=True)

print(f"\nReturn code: {result.returncode}", flush=True)
print(f"\nSTDOUT:", flush=True)
print(result.stdout, flush=True)
print(f"\nSTDERR:", flush=True)
print(result.stderr, flush=True)

# Made with Bob
