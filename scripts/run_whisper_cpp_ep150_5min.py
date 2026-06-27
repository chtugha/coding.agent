#!/usr/bin/env python3
"""
Run Whisper.cpp on episode 150 first 5 minutes to generate word-level timestamps.
Automatically merges BPE tokens into complete words.
"""

import subprocess
import os
import json

def merge_bpe_tokens(transcription):
    """
    Merge BPE tokens into complete words.
    
    Whisper.cpp outputs individual BPE tokens as separate segments.
    Tokens that don't start with a space are continuations of the previous word.
    
    Args:
        transcription: List of segments from Whisper.cpp JSON output
        
    Returns:
        List of merged word dictionaries with 'text', 'start', 'end', 'start_ms', 'end_ms'
    """
    merged_words = []
    current_word = None
    
    for segment in transcription:
        text = segment['text']
        start_ms = segment['offsets']['from']
        end_ms = segment['offsets']['to']
        
        # Check if this token starts a new word (begins with space or is first token)
        if text.startswith(' ') or current_word is None:
            # Save previous word if exists
            if current_word is not None:
                merged_words.append(current_word)
            
            # Start new word
            current_word = {
                'text': text.strip(),
                'start': start_ms / 1000.0,  # Convert to seconds
                'end': end_ms / 1000.0,
                'start_ms': start_ms,
                'end_ms': end_ms
            }
        else:
            # This is a continuation of the current word (no leading space)
            if current_word is not None:
                current_word['text'] += text
                current_word['end'] = end_ms / 1000.0
                current_word['end_ms'] = end_ms
    
    # Don't forget the last word
    if current_word is not None:
        merged_words.append(current_word)
    
    return merged_words

def main():
    print("="*70)
    print("Running Whisper.cpp on Episode 150 (First 5 Minutes)")
    print("="*70)
    print()
    
    # Paths
    audio_path = "/Volumes/eHDD/moshi-rag-data/datasets/whisper_cpp_test/episode_150_cleaned_v3.wav"
    output_dir = "/Volumes/eHDD/moshi-rag-data/datasets/whisper_cpp_test"
    output_base = "episode_150_5min_whisper"
    
    # Whisper.cpp binary
    whisper_bin = "./whisper-cpp/build/bin/whisper-cli"
    model_path = "./bin/models/ggml-large-v3-turbo-q5_0.bin"
    
    # Check if binary exists
    if not os.path.exists(whisper_bin):
        print(f"ERROR: Whisper binary not found at {whisper_bin}")
        print("Please build whisper.cpp first")
        return False
    
    # Check if model exists
    if not os.path.exists(model_path):
        print(f"ERROR: Model not found at {model_path}")
        return False
    
    # Check if audio exists
    if not os.path.exists(audio_path):
        print(f"ERROR: Audio file not found at {audio_path}")
        return False
    
    print(f"Audio: {audio_path}")
    print(f"Model: {model_path}")
    print(f"Output: {output_dir}/{output_base}.json")
    print()
    
    # Build command
    cmd = [
        whisper_bin,
        "-m", model_path,
        "-f", audio_path,
        "-l", "de",
        "-oj",  # Output JSON with word timestamps
        "-of", os.path.join(output_dir, output_base),
        "-d", "300000",  # Duration: 5 minutes = 300,000 ms
        "-ml", "1",  # Max segment length
        "-t", "8",  # Threads
        "-pp",  # Print progress
    ]
    
    print("Running Whisper.cpp...")
    print(f"Command: {' '.join(cmd)}")
    print()
    
    try:
        result = subprocess.run(cmd, check=True, capture_output=True, text=True)
        print(result.stdout)
        if result.stderr:
            print("STDERR:", result.stderr)
    except subprocess.CalledProcessError as e:
        print(f"ERROR: Whisper failed with exit code {e.returncode}")
        print(f"STDOUT: {e.stdout}")
        print(f"STDERR: {e.stderr}")
        return False
    
    # Check output
    output_json = os.path.join(output_dir, f"{output_base}.json")
    if os.path.exists(output_json):
        print()
        print("="*70)
        print("Whisper.cpp Transcription Complete")
        print("="*70)
        print(f"\nRaw output: {output_json}")
        
        # Load and show stats
        with open(output_json, 'r') as f:
            data = json.load(f)
        
        if 'transcription' in data:
            num_segments = len(data['transcription'])
            print(f"Raw BPE segments: {num_segments}")
            
            # Merge BPE tokens into complete words
            print("\nMerging BPE tokens into complete words...")
            merged_words = merge_bpe_tokens(data['transcription'])
            print(f"Merged words: {len(merged_words)}")
            
            # Create merged output
            merged_output = {
                'source': 'whisper.cpp',
                'model': data.get('params', {}).get('model', 'unknown'),
                'language': data.get('result', {}).get('language', 'de'),
                'word_count': len(merged_words),
                'words': merged_words
            }
            
            # Save merged output
            merged_json = os.path.join(output_dir, f"{output_base}_merged.json")
            with open(merged_json, 'w', encoding='utf-8') as f:
                json.dump(merged_output, f, indent=2, ensure_ascii=False)
            
            print(f"\n✓ Saved merged words to: {merged_json}")
            
            # Show sample words
            print("\n" + "="*70)
            print("Sample merged words:")
            print("="*70)
            for i, word in enumerate(merged_words[:20]):
                print(f"[{word['start']:.3f}s - {word['end']:.3f}s] {word['text']}")
            
            if len(merged_words) > 20:
                print(f"... ({len(merged_words) - 20} more words)")
        
        return True
    else:
        print(f"ERROR: Output file not found at {output_json}")
        return False

if __name__ == "__main__":
    success = main()
    exit(0 if success else 1)

# Made with Bob