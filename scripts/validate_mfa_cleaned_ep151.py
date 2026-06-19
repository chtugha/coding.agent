#!/usr/bin/env python3
"""
Validate MFA timestamps on cleaned episode 151 using WhisperX.
Compare timestamp accuracy: expect <100ms error vs previous 2.4s error.
"""

import json
import os
import sys
import numpy as np
from pathlib import Path

def load_mfa_timestamps():
    """Load MFA word timestamps from JSON."""
    print("=== Loading MFA Timestamps ===\n")
    
    mfa_json = "/tmp/fixed_alignment_test/episode_151_mfa_aligned.json"
    
    with open(mfa_json, 'r', encoding='utf-8') as f:
        mfa_data = json.load(f)
    
    # Extract words from MFA format
    tiers = mfa_data.get('tiers', {})
    if 'words' not in tiers:
        print("✗ No 'words' tier found in MFA output")
        return None
    
    words_tier = tiers['words']
    entries = words_tier.get('entries', [])
    
    # Convert to list of dicts
    mfa_words = []
    for entry in entries:
        start, end, text = entry
        mfa_words.append({
            'word': text,
            'start': start,
            'end': end
        })
    
    print(f"✓ Loaded {len(mfa_words)} MFA word timestamps")
    print(f"  Duration: {mfa_data.get('end', 0):.1f}s")
    print(f"\nFirst 5 words:")
    for i, w in enumerate(mfa_words[:5]):
        print(f"  {i+1}. [{w['start']:.3f} - {w['end']:.3f}] {w['word']}")
    
    return mfa_words

def validate_with_whisperx():
    """Validate MFA timestamps using WhisperX."""
    print("\n=== Validating with WhisperX ===\n")
    
    cleaned_audio = "/tmp/fixed_alignment_test/episode_151_cleaned_fixed.wav"
    
    # Import WhisperX
    try:
        import whisperx
        import torch
    except ImportError:
        print("✗ WhisperX not installed. Install with: pip install whisperx")
        return None
    
    # Detect device
    device = "cpu"  # WhisperX doesn't support MPS
    compute_type = "int8"
    
    print(f"Using device: {device}")
    print("Loading WhisperX model (large-v2)...")
    
    # Load model
    model = whisperx.load_model("large-v2", device, compute_type=compute_type, language="de")
    
    print("Transcribing audio (this may take several minutes)...")
    audio = whisperx.load_audio(cleaned_audio)
    result = model.transcribe(audio, batch_size=16)
    
    print(f"✓ Transcribed {len(result['segments'])} segments")
    
    # Align whisper output
    print("Aligning with WhisperX...")
    model_a, metadata = whisperx.load_align_model(language_code="de", device=device)
    result = whisperx.align(result["segments"], model_a, metadata, audio, device, return_char_alignments=False)
    
    print(f"✓ Aligned {len(result['segments'])} segments")
    
    # Extract words
    whisperx_words = []
    for seg in result['segments']:
        for word in seg.get('words', []):
            whisperx_words.append({
                'word': word['word'],
                'start': word['start'],
                'end': word['end']
            })
    
    print(f"✓ Extracted {len(whisperx_words)} WhisperX word timestamps")
    
    # Save WhisperX results
    output_file = "/tmp/fixed_alignment_test/episode_151_whisperx_validation.json"
    with open(output_file, 'w', encoding='utf-8') as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
    
    print(f"✓ Saved WhisperX results: {output_file}")
    
    return whisperx_words

def compare_timestamps(mfa_words, whisperx_words):
    """Compare MFA and WhisperX timestamps to measure accuracy."""
    print("\n=== Comparing Timestamps ===\n")
    
    # Align words by matching text (case-insensitive, normalized)
    def normalize(text):
        return text.lower().strip()
    
    # Create lookup for WhisperX words
    whisperx_lookup = {normalize(w['word']): w for w in whisperx_words}
    
    # Compare timestamps
    errors = []
    matched = 0
    unmatched = 0
    
    for mfa_word in mfa_words:
        norm_word = normalize(mfa_word['word'])
        
        if norm_word in whisperx_lookup:
            wx_word = whisperx_lookup[norm_word]
            
            # Calculate error (in milliseconds)
            start_error = abs(mfa_word['start'] - wx_word['start']) * 1000
            end_error = abs(mfa_word['end'] - wx_word['end']) * 1000
            avg_error = (start_error + end_error) / 2
            
            errors.append(avg_error)
            matched += 1
        else:
            unmatched += 1
    
    if not errors:
        print("✗ No matching words found for comparison")
        return
    
    # Calculate statistics
    errors = np.array(errors)
    mean_error = np.mean(errors)
    median_error = np.median(errors)
    std_error = np.std(errors)
    max_error = np.max(errors)
    min_error = np.min(errors)
    
    # Count errors by threshold
    under_100ms = np.sum(errors < 100)
    under_500ms = np.sum(errors < 500)
    over_1s = np.sum(errors > 1000)
    
    print(f"Matched words: {matched}")
    print(f"Unmatched words: {unmatched}")
    print(f"\nTimestamp Error Statistics:")
    print(f"  Mean error:   {mean_error:.1f} ms")
    print(f"  Median error: {median_error:.1f} ms")
    print(f"  Std dev:      {std_error:.1f} ms")
    print(f"  Min error:    {min_error:.1f} ms")
    print(f"  Max error:    {max_error:.1f} ms")
    print(f"\nError Distribution:")
    print(f"  < 100ms:  {under_100ms} words ({under_100ms/len(errors)*100:.1f}%)")
    print(f"  < 500ms:  {under_500ms} words ({under_500ms/len(errors)*100:.1f}%)")
    print(f"  > 1000ms: {over_1s} words ({over_1s/len(errors)*100:.1f}%)")
    
    # Compare to previous results
    print(f"\n{'='*60}")
    print("COMPARISON TO PREVIOUS RESULTS")
    print(f"{'='*60}")
    print(f"Previous (broken alignment): 2400ms average error")
    print(f"Current (fixed alignment):   {mean_error:.1f}ms average error")
    print(f"Improvement:                 {2400 - mean_error:.1f}ms ({(2400-mean_error)/2400*100:.1f}%)")
    
    if mean_error < 100:
        print("\n✓ SUCCESS: Timestamp accuracy is excellent (<100ms)")
    elif mean_error < 500:
        print("\n✓ GOOD: Timestamp accuracy is acceptable (<500ms)")
    else:
        print("\n⚠ WARNING: Timestamp accuracy needs improvement (>500ms)")

def main():
    print("=" * 70)
    print("Validating MFA Timestamps on Cleaned Episode 151")
    print("=" * 70)
    print()
    
    # Step 1: Load MFA timestamps
    mfa_words = load_mfa_timestamps()
    if not mfa_words:
        print("\n✗ Failed to load MFA timestamps")
        return False
    
    # Step 2: Validate with WhisperX
    whisperx_words = validate_with_whisperx()
    if not whisperx_words:
        print("\n✗ WhisperX validation failed")
        return False
    
    # Step 3: Compare timestamps
    compare_timestamps(mfa_words, whisperx_words)
    
    print("\n" + "=" * 70)
    print("Validation Complete")
    print("=" * 70)
    
    return True

if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)

# Made with Bob