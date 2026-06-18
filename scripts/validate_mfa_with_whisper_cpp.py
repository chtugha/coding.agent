#!/usr/bin/env python3
"""
Validate MFA timestamps using whisper-cpp with CoreML (fast on Apple Silicon).
"""

import os
import json
import subprocess
import librosa
import soundfile as sf
from pathlib import Path

# Paths
TRANSCRIPT_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/transcripts_aligned_final"
MFA_TRANSCRIPT = os.path.join(TRANSCRIPT_DIR, "episode_150_gemischtes_hack.json")
CLEANED_AUDIO_PATH = os.path.join(TRANSCRIPT_DIR, "episode_150_gemischtes_hack_cleaned.wav")
WHISPER_CLI = "/Users/whisper/zenflow_projects/coding.agent/whisper-cpp/build/bin/whisper-cli"
WHISPER_MODEL = "/Users/whisper/zenflow_projects/coding.agent/bin/models/ggml-large-v3-turbo-q5_0.bin"

def extract_audio_segment(audio_path, start_time, end_time, output_path):
    """Extract audio segment."""
    audio, sr = librosa.load(audio_path, sr=16000, offset=start_time, duration=end_time-start_time)
    sf.write(output_path, audio, 16000)
    return output_path

def transcribe_with_whisper_cpp(audio_path):
    """Transcribe with whisper-cpp (CoreML accelerated)."""
    print(f"  Transcribing with whisper-cpp (CoreML)...")
    
    cmd = [
        WHISPER_CLI,
        '-m', WHISPER_MODEL,
        '-l', 'de',
        '-f', audio_path,
        '--output-json',
        '--max-len', '1'  # Word-level output
    ]
    
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    # Parse JSON output
    output_file = audio_path + '.json'
    if os.path.exists(output_file):
        with open(output_file, 'r') as f:
            data = json.load(f)
        
        # Extract words from transcription segments
        words = []
        for segment in data.get('transcription', []):
            text = segment.get('text', '').strip()
            if text and not text.startswith('[') and not text.startswith('<'):
                offsets = segment.get('offsets', {})
                words.append({
                    'word': text.lower(),
                    'start': offsets.get('from', 0) / 1000.0,
                    'end': offsets.get('to', 0) / 1000.0
                })
        
        print(f"  Extracted {len(words)} words")
        return words
    
    print(f"  ERROR: JSON file not found!")
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
                    'start': word['start'] - start_time,
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
    
    # Show first 10 words
    print(f"\nFirst 10 words comparison:")
    print(f"{'MFA':<40} {'Whisper':<40}")
    print(f"{'-'*40} {'-'*40}")
    
    for i in range(min(10, len(mfa_words), len(whisper_words))):
        mfa_w = mfa_words[i]
        whisper_w = whisper_words[i] if i < len(whisper_words) else {'word': 'N/A', 'start': 0, 'end': 0}
        
        mfa_str = f"{mfa_w['word']:<15} ({mfa_w['start']:.2f}-{mfa_w['end']:.2f})"
        whisper_str = f"{whisper_w.get('word', 'N/A'):<15} ({whisper_w.get('start', 0):.2f}-{whisper_w.get('end', 0):.2f})"
        
        print(f"{mfa_str:<40} {whisper_str:<40}")

def main():
    print("=== MFA Validation with whisper-cpp (CoreML) ===\n")
    print(f"Using CLEANED audio: {os.path.basename(CLEANED_AUDIO_PATH)}\n")
    
    # Load MFA transcript
    with open(MFA_TRANSCRIPT, 'r') as f:
        mfa_data = json.load(f)
    
    # Get total duration
    total_duration = librosa.get_duration(path=CLEANED_AUDIO_PATH)
    print(f"Cleaned audio duration: {total_duration:.1f}s ({total_duration/60:.1f} minutes)\n")
    
    # Create temp directory
    temp_dir = "/tmp/mfa_validation_whisper_cpp"
    os.makedirs(temp_dir, exist_ok=True)
    
    # Test first 2 minutes
    print("Processing first 2 minutes...")
    first_2min_path = os.path.join(temp_dir, "first_2min.wav")
    extract_audio_segment(CLEANED_AUDIO_PATH, 0, 120, first_2min_path)
    
    whisper_first = transcribe_with_whisper_cpp(first_2min_path)
    mfa_first = get_mfa_words_in_range(mfa_data, 0, 120)
    
    compare_timestamps(mfa_first, whisper_first, "FIRST 2 MINUTES")
    
    print(f"\n{'='*60}")
    print("Validation complete")
    print(f"{'='*60}")
    print(f"\nNote: Whisper-cpp uses token-level output, not true word-level.")
    print(f"For production validation, MFA statistical analysis is sufficient.")

if __name__ == "__main__":
    main()

# Made with Bob
