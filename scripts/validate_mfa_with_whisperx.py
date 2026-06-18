#!/usr/bin/env python3
"""
Validate MFA timestamps using WhisperX (fast word-level timestamps with Apple Silicon).
WhisperX: https://github.com/m-bain/whisperX
"""

import os
import json
import torch
import whisperx
import librosa
import soundfile as sf
from pathlib import Path

# Paths
TRANSCRIPT_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/transcripts_aligned_final"
MFA_TRANSCRIPT = os.path.join(TRANSCRIPT_DIR, "episode_150_gemischtes_hack.json")
CLEANED_AUDIO_PATH = os.path.join(TRANSCRIPT_DIR, "episode_150_gemischtes_hack_cleaned.wav")

def extract_audio_segment(audio_path, start_time, end_time, output_path):
    """Extract audio segment."""
    audio, sr = librosa.load(audio_path, sr=16000, offset=start_time, duration=end_time-start_time)
    sf.write(output_path, audio, 16000)
    return output_path

def transcribe_with_whisperx(audio_path, device="cpu"):
    """Transcribe with WhisperX (word-level timestamps)."""
    print(f"  Loading WhisperX model on {device}...")
    
    # Load model - use int8 for CPU
    compute_type = "int8" if device == "cpu" else "float16"
    model = whisperx.load_model("large-v3", device, compute_type=compute_type, language="de")
    
    # Load audio
    audio = whisperx.load_audio(audio_path)
    
    print(f"  Transcribing...")
    result = model.transcribe(audio, batch_size=16)
    
    print(f"  Aligning for word-level timestamps...")
    # Load alignment model
    model_a, metadata = whisperx.load_align_model(language_code="de", device=device)
    
    # Align whisper output
    result = whisperx.align(result["segments"], model_a, metadata, audio, device, return_char_alignments=False)
    
    # Extract words
    words = []
    for segment in result["segments"]:
        if "words" in segment:
            for word in segment["words"]:
                words.append({
                    'word': word['word'].strip().lower(),
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
                    'start': word['start'] - start_time,
                    'end': word['end'] - start_time
                })
    return words

def calculate_timestamp_differences(mfa_words, whisperx_words):
    """Calculate timestamp differences between MFA and WhisperX."""
    differences = []
    
    # Match words by position (assuming same word order)
    min_len = min(len(mfa_words), len(whisperx_words))
    
    for i in range(min_len):
        mfa_w = mfa_words[i]
        wx_w = whisperx_words[i]
        
        # Calculate differences
        start_diff = abs(mfa_w['start'] - wx_w['start'])
        end_diff = abs(mfa_w['end'] - wx_w['end'])
        
        differences.append({
            'index': i,
            'mfa_word': mfa_w['word'],
            'whisperx_word': wx_w['word'],
            'start_diff': start_diff,
            'end_diff': end_diff,
            'mfa_start': mfa_w['start'],
            'whisperx_start': wx_w['start'],
            'mfa_end': mfa_w['end'],
            'whisperx_end': wx_w['end']
        })
    
    return differences

def compare_timestamps(mfa_words, whisperx_words, segment_name):
    """Compare MFA and WhisperX timestamps."""
    print(f"\n{'='*80}")
    print(f"{segment_name}")
    print(f"{'='*80}")
    
    print(f"\nMFA words: {len(mfa_words)}")
    print(f"WhisperX words: {len(whisperx_words)}")
    
    # Calculate differences
    differences = calculate_timestamp_differences(mfa_words, whisperx_words)
    
    if differences:
        # Statistics
        start_diffs = [d['start_diff'] for d in differences]
        end_diffs = [d['end_diff'] for d in differences]
        
        avg_start_diff = sum(start_diffs) / len(start_diffs)
        avg_end_diff = sum(end_diffs) / len(end_diffs)
        max_start_diff = max(start_diffs)
        max_end_diff = max(end_diffs)
        
        print(f"\nTimestamp Accuracy:")
        print(f"  Average start difference: {avg_start_diff*1000:.1f}ms")
        print(f"  Average end difference: {avg_end_diff*1000:.1f}ms")
        print(f"  Max start difference: {max_start_diff*1000:.1f}ms")
        print(f"  Max end difference: {max_end_diff*1000:.1f}ms")
        
        # Show first 15 words with differences
        print(f"\nFirst 15 words comparison:")
        print(f"{'#':<4} {'MFA Word':<15} {'WX Word':<15} {'MFA Time':<20} {'WX Time':<20} {'Δ Start':<10} {'Δ End':<10}")
        print(f"{'-'*4} {'-'*15} {'-'*15} {'-'*20} {'-'*20} {'-'*10} {'-'*10}")
        
        for i, diff in enumerate(differences[:15]):
            mfa_time = f"{diff['mfa_start']:.2f}-{diff['mfa_end']:.2f}"
            wx_time = f"{diff['whisperx_start']:.2f}-{diff['whisperx_end']:.2f}"
            start_diff_ms = f"{diff['start_diff']*1000:.0f}ms"
            end_diff_ms = f"{diff['end_diff']*1000:.0f}ms"
            
            print(f"{i:<4} {diff['mfa_word']:<15} {diff['whisperx_word']:<15} {mfa_time:<20} {wx_time:<20} {start_diff_ms:<10} {end_diff_ms:<10}")
        
        # Show worst cases
        print(f"\nWorst 5 timestamp differences:")
        worst = sorted(differences, key=lambda x: x['start_diff'] + x['end_diff'], reverse=True)[:5]
        print(f"{'#':<4} {'MFA Word':<15} {'WX Word':<15} {'MFA Time':<20} {'WX Time':<20} {'Δ Start':<10} {'Δ End':<10}")
        print(f"{'-'*4} {'-'*15} {'-'*15} {'-'*20} {'-'*20} {'-'*10} {'-'*10}")
        
        for i, diff in enumerate(worst):
            mfa_time = f"{diff['mfa_start']:.2f}-{diff['mfa_end']:.2f}"
            wx_time = f"{diff['whisperx_start']:.2f}-{diff['whisperx_end']:.2f}"
            start_diff_ms = f"{diff['start_diff']*1000:.0f}ms"
            end_diff_ms = f"{diff['end_diff']*1000:.0f}ms"
            
            print(f"{diff['index']:<4} {diff['mfa_word']:<15} {diff['whisperx_word']:<15} {mfa_time:<20} {wx_time:<20} {start_diff_ms:<10} {end_diff_ms:<10}")

def main():
    print("=== MFA Validation with WhisperX (Apple Silicon MPS) ===\n")
    
    # WhisperX uses faster-whisper which only supports CPU and CUDA
    if torch.cuda.is_available():
        device = "cuda"
        print("✓ Using CUDA acceleration\n")
    else:
        device = "cpu"
        print("✓ Using CPU with int8 quantization (optimized for Apple Silicon)\n")
    
    print(f"Using CLEANED audio: {os.path.basename(CLEANED_AUDIO_PATH)}\n")
    
    # Load MFA transcript
    with open(MFA_TRANSCRIPT, 'r') as f:
        mfa_data = json.load(f)
    
    # Get total duration
    total_duration = librosa.get_duration(path=CLEANED_AUDIO_PATH)
    print(f"Cleaned audio duration: {total_duration:.1f}s ({total_duration/60:.1f} minutes)\n")
    
    # Create temp directory
    temp_dir = "/tmp/mfa_validation_whisperx"
    os.makedirs(temp_dir, exist_ok=True)
    
    # Test first 2 minutes
    print("Processing first 2 minutes...")
    first_2min_path = os.path.join(temp_dir, "first_2min.wav")
    extract_audio_segment(CLEANED_AUDIO_PATH, 0, 120, first_2min_path)
    
    whisperx_first = transcribe_with_whisperx(first_2min_path, device)
    mfa_first = get_mfa_words_in_range(mfa_data, 0, 120)
    
    compare_timestamps(mfa_first, whisperx_first, "FIRST 2 MINUTES")
    
    print(f"\n{'='*80}")
    print("Validation complete")
    print(f"{'='*80}")
    print(f"\nInterpretation:")
    print(f"  • <50ms difference: Excellent alignment")
    print(f"  • 50-100ms difference: Good alignment")
    print(f"  • 100-200ms difference: Acceptable alignment")
    print(f"  • >200ms difference: Poor alignment (investigate)")

if __name__ == "__main__":
    main()

# Made with Bob
