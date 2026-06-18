#!/usr/bin/env python3
"""
Validate MFA timestamps by comparing with whisper-timestamped word-level timestamps.
Uses the same method as Moshi finetune project.
"""

import os
import json
import librosa
import soundfile as sf
import whisper_timestamped as whisper
import torch
from pathlib import Path

# Paths
TRANSCRIPT_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/transcripts_aligned_final"
MFA_TRANSCRIPT = os.path.join(TRANSCRIPT_DIR, "episode_150_gemischtes_hack.json")
CLEANED_AUDIO_PATH = os.path.join(TRANSCRIPT_DIR, "episode_150_gemischtes_hack_cleaned.wav")

def extract_audio_segment(audio_path, start_time, end_time, output_path):
    """Extract audio segment."""
    audio, sr = librosa.load(audio_path, sr=16000, offset=start_time, duration=end_time-start_time)
    sf.write(output_path, audio, int(sr))
    return output_path

def transcribe_with_whisper_timestamped(audio_path):
    """Transcribe audio with whisper-timestamped for proper word-level timestamps."""
    print(f"  Loading audio...")
    audio, sr = librosa.load(audio_path, sr=16000)
    
    print(f"  Transcribing with whisper-timestamped...")
    device = "cuda" if torch.cuda.is_available() else "cpu"
    model = whisper.load_model("large-v3", device=device)
    print(f"  Using model: large-v3 on {device}")
    
    result = whisper.transcribe(
        model,
        audio,
        language="de",
        vad=True,
        best_of=5,
        beam_size=5,
        temperature=(0.0, 0.2, 0.4, 0.6, 0.8, 1.0),
        verbose=False,
    )
    
    # Extract words
    words = []
    for segment in result["segments"]:
        if "words" in segment:
            for word in segment["words"]:
                words.append({
                    'word': word['text'].strip().lower(),
                    'start': word['start'],
                    'end': word['end']
                })
    
    print(f"  Extracted {len(words)} words")
    return words

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
        word_matches = 0
        
        for i in range(min_len):
            # Normalize words for comparison (remove punctuation)
            mfa_word = mfa_words[i]['word'].strip('.,!?;:')
            whisper_word = whisper_words[i].get('word', '').strip('.,!?;')
            
            if mfa_word == whisper_word:
                word_matches += 1
                start_diffs.append(abs(mfa_words[i]['start'] - whisper_words[i].get('start', 0)))
                end_diffs.append(abs(mfa_words[i]['end'] - whisper_words[i].get('end', 0)))
        
        if start_diffs:
            avg_start_diff = sum(start_diffs) / len(start_diffs)
            avg_end_diff = sum(end_diffs) / len(end_diffs)
            match_rate = word_matches / min_len * 100
            
            print(f"\nWord match rate: {match_rate:.1f}% ({word_matches}/{min_len})")
            print(f"Average timestamp difference (matching words):")
            print(f"  Start times: {avg_start_diff:.3f}s")
            print(f"  End times: {avg_end_diff:.3f}s")
            
            # Quality assessment
            if avg_start_diff < 0.1 and avg_end_diff < 0.1:
                print(f"  ✅ EXCELLENT: Timestamps within 100ms")
            elif avg_start_diff < 0.2 and avg_end_diff < 0.2:
                print(f"  ✅ GOOD: Timestamps within 200ms")
            elif avg_start_diff < 0.5 and avg_end_diff < 0.5:
                print(f"  ⚠️  ACCEPTABLE: Timestamps within 500ms")
            else:
                print(f"  ❌ POOR: Timestamps differ by more than 500ms")

def main():
    print("=== MFA Timestamp Validation with whisper-timestamped ===\n")
    print(f"Using CLEANED audio: {os.path.basename(CLEANED_AUDIO_PATH)}\n")
    
    # Load MFA transcript
    with open(MFA_TRANSCRIPT, 'r') as f:
        mfa_data = json.load(f)
    
    # Get total duration
    total_duration = librosa.get_duration(path=CLEANED_AUDIO_PATH)
    print(f"Cleaned audio duration: {total_duration:.1f}s ({total_duration/60:.1f} minutes)\n")
    
    # Create temp directory for segments
    temp_dir = "/tmp/mfa_validation_whisper_timestamped"
    os.makedirs(temp_dir, exist_ok=True)
    
    # Test first 2 minutes only
    print("Processing first 2 minutes...")
    first_2min_path = os.path.join(temp_dir, "first_2min.wav")
    extract_audio_segment(CLEANED_AUDIO_PATH, 0, 120, first_2min_path)
    
    whisper_first = transcribe_with_whisper_timestamped(first_2min_path)
    mfa_first = get_mfa_words_in_range(mfa_data, 0, 120)
    
    compare_timestamps(mfa_first, whisper_first, "FIRST 2 MINUTES")
    
    print(f"\n{'='*60}")
    print("Validation complete")
    print(f"{'='*60}")
    print(f"\nDebug files saved in: {temp_dir}")

if __name__ == "__main__":
    main()

# Made with Bob
