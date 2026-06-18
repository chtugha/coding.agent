#!/usr/bin/env python3
"""
Validate MFA timestamps by comparing with Whisper word-level timestamps.
Extracts first 5 minutes and last 5 minutes of audio and re-transcribes with Whisper.
"""

import os
import sys
import json
import subprocess
import tempfile
import librosa
import soundfile as sf
from pathlib import Path

# Paths
TRANSCRIPT_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/transcripts_aligned_final"
MFA_TRANSCRIPT = os.path.join(TRANSCRIPT_DIR, "episode_150_gemischtes_hack.json")
CLEANED_AUDIO_PATH = os.path.join(TRANSCRIPT_DIR, "episode_150_gemischtes_hack_cleaned.wav")

def extract_audio_segment(audio_path, start_time, end_time, output_path):
    """Extract audio segment using ffmpeg."""
    duration = end_time - start_time
    cmd = [
        'ffmpeg', '-y',
        '-ss', str(start_time),
        '-t', str(duration),
        '-i', audio_path,
        '-ar', '16000',
        '-ac', '1',
        output_path
    ]
    subprocess.run(cmd, capture_output=True, check=True)
    print(f"  Extracted {start_time}s-{end_time}s to {output_path}")


def transcribe_with_whisper(audio_path):
    """Transcribe audio with Whisper using word timestamps."""
    print(f"  Transcribing with Whisper...")
    
    # Use whisper-cpp for transcription
    whisper_cli = "/Users/whisper/zenflow_projects/coding.agent/whisper-cpp/build/bin/whisper-cli"
    whisper_model = "/Users/whisper/zenflow_projects/coding.agent/bin/models/ggml-large-v3-turbo-q5_0.bin"
    
    cmd = [
        whisper_cli,
        '-m', whisper_model,
        '-l', 'de',
        '-f', audio_path,
        '--output-json',
        '--max-len', '1'  # Word-level timestamps
    ]
    
    result = subprocess.run(cmd, capture_output=True, text=True)
    print(f"  Whisper exit code: {result.returncode}")
    
    # Parse JSON output - whisper-cpp format
    # whisper-cpp creates filename.wav.json, not filename.json
    output_file = audio_path + '.json'
    print(f"  Looking for JSON at: {output_file}")
    print(f"  JSON exists: {os.path.exists(output_file)}")
    
    if os.path.exists(output_file):
        with open(output_file, 'r') as f:
            data = json.load(f)
        
        print(f"  Transcription segments: {len(data.get('transcription', []))}")
        
        # With --max-len 1, each segment in transcription array IS a word
        # No tokens array - the segment itself contains the word
        words = []
        for segment in data.get('transcription', []):
            text = segment.get('text', '').strip()
            # Skip empty segments and special tokens
            if text and not text.startswith('[') and not text.startswith('<'):
                offsets = segment.get('offsets', {})
                words.append({
                    'word': text.lower(),
                    'start': offsets.get('from', 0) / 1000.0,  # Convert ms to seconds
                    'end': offsets.get('to', 0) / 1000.0
                })
        
        print(f"  Extracted {len(words)} words")
        return words
    else:
        print(f"  ERROR: JSON file not found!")
        if result.stderr:
            print(f"  Whisper stderr: {result.stderr[:500]}")
    
    return []


def get_mfa_words_in_range(mfa_data, start_time, end_time):
    """Extract MFA words within time range."""
    words = []
    for seg in mfa_data['segments']:
        if 'words' not in seg:
            continue
        for word in seg['words']:
            if start_time <= word['start'] < end_time:
                words.append({
                    'word': word['word'].lower(),
                    'start': word['start'] - start_time,  # Relative to segment start
                    'end': word['end'] - start_time
                })
    return words


def compare_timestamps(mfa_words, whisper_words, segment_name):
    """Compare MFA and Whisper timestamps."""
    print(f"\n{'='*60}")
    print(f"{segment_name}")
    print(f"{'='*60}")
    
    print(f"\nMFA words: {len(mfa_words)}")
    print(f"Whisper words: {len(whisper_words)}")
    
    # Show first 10 words from each
    print(f"\nFirst 10 words comparison:")
    print(f"{'MFA':<40} {'Whisper':<40}")
    print(f"{'-'*40} {'-'*40}")
    
    for i in range(min(10, len(mfa_words), len(whisper_words))):
        mfa_w = mfa_words[i]
        whisper_w = whisper_words[i] if i < len(whisper_words) else {'word': 'N/A', 'start': 0, 'end': 0}
        
        mfa_str = f"{mfa_w['word']:<15} ({mfa_w['start']:.2f}-{mfa_w['end']:.2f})"
        whisper_str = f"{whisper_w.get('word', 'N/A'):<15} ({whisper_w.get('start', 0):.2f}-{whisper_w.get('end', 0):.2f})"
        
        print(f"{mfa_str:<40} {whisper_str:<40}")
    
    # Calculate average timestamp difference for matching words
    if len(mfa_words) > 0 and len(whisper_words) > 0:
        min_len = min(len(mfa_words), len(whisper_words))
        start_diffs = []
        end_diffs = []
        
        for i in range(min_len):
            if mfa_words[i]['word'] == whisper_words[i].get('word', '').lower():
                start_diffs.append(abs(mfa_words[i]['start'] - whisper_words[i].get('start', 0)))
                end_diffs.append(abs(mfa_words[i]['end'] - whisper_words[i].get('end', 0)))
        
        if start_diffs:
            avg_start_diff = sum(start_diffs) / len(start_diffs)
            avg_end_diff = sum(end_diffs) / len(end_diffs)
            print(f"\nAverage timestamp difference (matching words):")
            print(f"  Start times: {avg_start_diff:.3f}s")
            print(f"  End times: {avg_end_diff:.3f}s")


def main():
    print("=== MFA Timestamp Validation ===\n")
    print(f"Comparing MFA timestamps against Whisper re-transcription")
    print(f"Using CLEANED audio (ads removed): {os.path.basename(CLEANED_AUDIO_PATH)}\n")
    
    # Load MFA transcript
    with open(MFA_TRANSCRIPT, 'r') as f:
        mfa_data = json.load(f)
    
    # Get total duration from cleaned audio
    audio, sr = librosa.load(CLEANED_AUDIO_PATH, sr=None, duration=1)
    total_duration = librosa.get_duration(path=CLEANED_AUDIO_PATH)
    print(f"Cleaned audio duration: {total_duration:.1f}s ({total_duration/60:.1f} minutes)\n")
    
    # Use /tmp instead of tempfile.TemporaryDirectory to keep files for debugging
    temp_dir = "/tmp/mfa_validation"
    os.makedirs(temp_dir, exist_ok=True)
    
    # Test first 5 minutes
    print("Processing first 5 minutes...")
    first_5min_path = os.path.join(temp_dir, "first_5min.wav")
    extract_audio_segment(CLEANED_AUDIO_PATH, 0, 300, first_5min_path)
    
    whisper_first = transcribe_with_whisper(first_5min_path)
    mfa_first = get_mfa_words_in_range(mfa_data, 0, 300)
    
    compare_timestamps(mfa_first, whisper_first, "FIRST 5 MINUTES")
    
    # Test last 5 minutes
    print(f"\nProcessing last 5 minutes...")
    last_5min_start = total_duration - 300
    last_5min_path = os.path.join(temp_dir, "last_5min.wav")
    extract_audio_segment(CLEANED_AUDIO_PATH, last_5min_start, total_duration, last_5min_path)
    
    whisper_last = transcribe_with_whisper(last_5min_path)
    mfa_last = get_mfa_words_in_range(mfa_data, last_5min_start, total_duration)
    
    compare_timestamps(mfa_last, whisper_last, "LAST 5 MINUTES")
    
    print(f"\n{'='*60}")
    print("Validation complete")
    print(f"{'='*60}")
    print(f"\nDebug files saved in: {temp_dir}")


if __name__ == "__main__":
    main()

# Made with Bob
