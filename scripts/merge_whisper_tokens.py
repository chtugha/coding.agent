#!/usr/bin/env python3
"""
Merge BPE tokens from Whisper.cpp output into complete words.

Whisper.cpp outputs individual BPE tokens as separate segments, which results in
fragmented words like " Unter", "f", "ickt" instead of "Unterfickt".

This script merges tokens that don't start with a space into the previous word,
creating proper word-level timestamps.
"""

import json
import sys
from pathlib import Path


def merge_bpe_tokens(input_file: str, output_file: str):
    """
    Merge BPE tokens into complete words.
    
    Args:
        input_file: Path to Whisper.cpp JSON output with fragmented tokens
        output_file: Path to save merged word-level JSON
    """
    print("=" * 70)
    print("Merging BPE Tokens into Complete Words")
    print("=" * 70)
    print(f"\nInput:  {input_file}")
    print(f"Output: {output_file}")
    
    # Load the Whisper.cpp output
    with open(input_file, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    transcription = data.get('transcription', [])
    print(f"\nOriginal segments: {len(transcription)}")
    
    # Merge tokens into words
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
    
    print(f"Merged words: {len(merged_words)}")
    
    # Create output structure
    output_data = {
        'source': 'whisper.cpp',
        'model': data.get('params', {}).get('model', 'unknown'),
        'language': data.get('result', {}).get('language', 'de'),
        'word_count': len(merged_words),
        'words': merged_words
    }
    
    # Save merged output
    with open(output_file, 'w', encoding='utf-8') as f:
        json.dump(output_data, f, indent=2, ensure_ascii=False)
    
    print(f"\n✓ Saved merged words to: {output_file}")
    
    # Show some examples
    print("\n" + "=" * 70)
    print("Sample merged words:")
    print("=" * 70)
    for i, word in enumerate(merged_words[:20]):
        print(f"[{word['start']:.3f}s - {word['end']:.3f}s] {word['text']}")
    
    if len(merged_words) > 20:
        print(f"... ({len(merged_words) - 20} more words)")
    
    return output_data


def main():
    input_file = "/Volumes/eHDD/moshi-rag-data/datasets/whisper_cpp_test/episode_150_5min_whisper.json"
    output_file = "/Volumes/eHDD/moshi-rag-data/datasets/whisper_cpp_test/episode_150_5min_whisper_merged.json"
    
    if not Path(input_file).exists():
        print(f"Error: Input file not found: {input_file}")
        sys.exit(1)
    
    merge_bpe_tokens(input_file, output_file)
    
    print("\n" + "=" * 70)
    print("Token Merging Complete")
    print("=" * 70)


if __name__ == "__main__":
    main()

# Made with Bob
