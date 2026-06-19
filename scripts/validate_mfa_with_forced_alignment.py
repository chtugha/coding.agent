#!/usr/bin/env python3
"""
Validate MFA timestamps by using WhisperX forced alignment on the SAME text.
This ensures we're comparing timestamps for the same words, not different transcriptions.
"""

import json
import sys
import torch
import whisperx
import numpy as np
from pathlib import Path

def load_mfa_text(mfa_json_path):
    """Extract the text that MFA aligned"""
    with open(mfa_json_path, 'r') as f:
        mfa_data = json.load(f)
    
    # Reconstruct text from MFA words
    words = [entry[2] for entry in mfa_data['tiers']['words']['entries']]
    return " ".join(words), words

def forced_align_with_whisperx(audio_path, text, device="cpu"):
    """Use WhisperX to force-align the given text to audio"""
    print(f"Loading audio: {audio_path}")
    
    # Load audio
    audio = whisperx.load_audio(audio_path)
    
    # Load alignment model
    print("Loading WhisperX alignment model...")
    model_a, metadata = whisperx.load_align_model(
        language_code="de",
        device=device
    )
    
    # Create segments from text (WhisperX expects this format)
    # We'll create one large segment and let it split by words
    segments = [{
        "start": 0.0,
        "end": len(audio) / 16000.0,  # Approximate duration
        "text": text
    }]
    
    print("Performing forced alignment...")
    result = whisperx.align(
        segments,
        model_a,
        metadata,
        audio,
        device,
        return_char_alignments=False
    )
    
    return result

def compare_timestamps(mfa_words, wx_result):
    """Compare MFA and WhisperX timestamps"""
    # Extract WhisperX words
    wx_words = []
    for seg in wx_result['segments']:
        for word in seg.get('words', []):
            wx_words.append({
                'word': word['word'].lower().strip(),
                'start': word['start'],
                'end': word['end']
            })
    
    print(f"\nMFA words: {len(mfa_words)}")
    print(f"WhisperX words: {len(wx_words)}")
    
    # Compare word by word
    errors = []
    min_len = min(len(mfa_words), len(wx_words))
    
    for i in range(min_len):
        mfa_w = mfa_words[i]
        wx_w = wx_words[i]
        
        start_error = abs(mfa_w['start'] - wx_w['start']) * 1000
        end_error = abs(mfa_w['end'] - wx_w['end']) * 1000
        avg_error = (start_error + end_error) / 2
        
        errors.append({
            'index': i,
            'mfa_word': mfa_w['word'],
            'wx_word': wx_w['word'],
            'mfa_start': mfa_w['start'],
            'wx_start': wx_w['start'],
            'error_ms': avg_error
        })
    
    return errors

def main():
    # Paths
    audio_path = "/tmp/fixed_alignment_test/episode_151_cleaned_fixed.wav"
    mfa_json_path = "/tmp/fixed_alignment_test/episode_151_mfa_aligned.json"
    output_path = "/tmp/fixed_alignment_test/episode_151_whisperx_forced_alignment.json"
    
    print("="*80)
    print("MFA TIMESTAMP VALIDATION - FORCED ALIGNMENT")
    print("="*80)
    
    # Load MFA text and words
    print("\n1. Loading MFA alignment...")
    mfa_text, mfa_word_list = load_mfa_text(mfa_json_path)
    print(f"   MFA text length: {len(mfa_text)} characters")
    print(f"   MFA word count: {len(mfa_word_list)} words")
    print(f"   First 200 chars: {mfa_text[:200]}")
    
    # Load MFA word timestamps
    with open(mfa_json_path, 'r') as f:
        mfa_data = json.load(f)
    
    mfa_words = []
    for entry in mfa_data['tiers']['words']['entries']:
        start, end, text = entry
        mfa_words.append({'word': text.lower().strip(), 'start': start, 'end': end})
    
    # Perform forced alignment with WhisperX
    print("\n2. Performing WhisperX forced alignment...")
    print("   (This will take several minutes on CPU)")
    
    device = "cuda" if torch.cuda.is_available() else "cpu"
    print(f"   Using device: {device}")
    
    wx_result = forced_align_with_whisperx(audio_path, mfa_text, device)
    
    # Save WhisperX result
    print(f"\n3. Saving WhisperX result to: {output_path}")
    with open(output_path, 'w') as f:
        json.dump(wx_result, f, indent=2, ensure_ascii=False)
    
    # Compare timestamps
    print("\n4. Comparing timestamps...")
    errors = compare_timestamps(mfa_words, wx_result)
    
    # Calculate statistics
    error_array = np.array([e['error_ms'] for e in errors])
    
    print("\n" + "="*80)
    print("VALIDATION RESULTS")
    print("="*80)
    print(f"\nWords compared: {len(errors)}")
    print(f"\nTimestamp Accuracy:")
    print(f"  Mean error:   {np.mean(error_array):.1f} ms")
    print(f"  Median error: {np.median(error_array):.1f} ms")
    print(f"  Std dev:      {np.std(error_array):.1f} ms")
    print(f"  Min error:    {np.min(error_array):.1f} ms")
    print(f"  Max error:    {np.max(error_array):.1f} ms")
    
    under_100ms = np.sum(error_array < 100)
    under_500ms = np.sum(error_array < 500)
    over_1s = np.sum(error_array > 1000)
    
    print(f"\nError Distribution:")
    print(f"  < 100ms:  {under_100ms:5d} ({under_100ms/len(error_array)*100:5.1f}%)")
    print(f"  < 500ms:  {under_500ms:5d} ({under_500ms/len(error_array)*100:5.1f}%)")
    print(f"  > 1000ms: {over_1s:5d} ({over_1s/len(error_array)*100:5.1f}%)")
    
    print(f"\nFirst 30 word comparisons:")
    for e in errors[:30]:
        print(f"  {e['index']:4d}: MFA[{e['mfa_start']:7.2f}s] '{e['mfa_word'][:15]:15s}' vs WX[{e['wx_start']:7.2f}s] '{e['wx_word'][:15]:15s}' = {e['error_ms']:6.1f}ms")
    
    print("\n" + "="*80)
    print("VALIDATION COMPLETE")
    print("="*80)

if __name__ == "__main__":
    main()

# Made with Bob
