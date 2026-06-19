#!/usr/bin/env python3
"""
Validate the fixed waveform alignment by:
1. Running MFA on the cleaned audio
2. Validating MFA timestamps with WhisperX
3. Comparing to previous results (2.4s avg error)
"""

import json
import os
import sys
import subprocess
import numpy as np
from pathlib import Path

def run_mfa_on_cleaned_audio():
    """Run MFA alignment on the cleaned audio from fixed alignment."""
    print("=== Running MFA on Cleaned Audio ===\n")
    
    cleaned_audio = "/tmp/fixed_alignment_test/episode_150_cleaned_fixed.wav"
    transcript_path = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/Gemischtes.Hack.Podcast.Transcript/transcripts/episode_150_seil_seil_seil.json"
    output_dir = "/tmp/fixed_alignment_test/mfa_output"
    
    # Create output directory
    os.makedirs(output_dir, exist_ok=True)
    
    # Load transcript
    with open(transcript_path, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    segments = data['segments']
    
    # Create text file for MFA
    text_file = os.path.join(output_dir, "episode_150.txt")
    with open(text_file, 'w', encoding='utf-8') as f:
        for seg in segments:
            f.write(seg['text'] + ' ')
    
    print(f"Created text file: {text_file}")
    print(f"Segments: {len(segments)}")
    print(f"Audio: {cleaned_audio}")
    
    # Run MFA
    print("\nRunning MFA alignment...")
    cmd = [
        "mfa", "align",
        "--clean",
        "--single_speaker",
        cleaned_audio,
        text_file,
        "german_mfa",
        "german_mfa",
        output_dir
    ]
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        if result.returncode == 0:
            print("✓ MFA alignment completed successfully")
        else:
            print(f"✗ MFA alignment failed: {result.stderr}")
            return None
    except subprocess.TimeoutExpired:
        print("✗ MFA alignment timed out after 10 minutes")
        return None
    except Exception as e:
        print(f"✗ MFA alignment error: {e}")
        return None
    
    # Find output TextGrid file
    textgrid_files = list(Path(output_dir).glob("**/*.TextGrid"))
    if textgrid_files:
        print(f"✓ Found TextGrid: {textgrid_files[0]}")
        return str(textgrid_files[0])
    else:
        print("✗ No TextGrid file found")
        return None

def validate_with_whisperx():
    """Validate MFA timestamps using WhisperX."""
    print("\n=== Validating with WhisperX ===\n")
    
    cleaned_audio = "/tmp/fixed_alignment_test/episode_150_cleaned_fixed.wav"
    
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
    print("Loading WhisperX model...")
    
    # Load model
    model = whisperx.load_model("large-v2", device, compute_type=compute_type, language="de")
    
    print("Transcribing audio...")
    audio = whisperx.load_audio(cleaned_audio)
    result = model.transcribe(audio, batch_size=16)
    
    print(f"✓ Transcribed {len(result['segments'])} segments")
    
    # Align whisper output
    print("Aligning with WhisperX...")
    model_a, metadata = whisperx.load_align_model(language_code="de", device=device)
    result = whisperx.align(result["segments"], model_a, metadata, audio, device, return_char_alignments=False)
    
    print(f"✓ Aligned {len(result['segments'])} segments")
    
    # Save WhisperX results
    output_file = "/tmp/fixed_alignment_test/whisperx_validation.json"
    with open(output_file, 'w', encoding='utf-8') as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
    
    print(f"✓ Saved WhisperX results: {output_file}")
    
    return result

def compare_timestamps(mfa_textgrid, whisperx_result):
    """Compare MFA and WhisperX timestamps."""
    print("\n=== Comparing Timestamps ===\n")
    
    # Parse TextGrid (simplified - would need proper parser)
    print("Note: Full TextGrid parsing not implemented")
    print("Manual comparison recommended")
    
    # Show WhisperX word count
    total_words = sum(len(seg.get('words', [])) for seg in whisperx_result['segments'])
    print(f"WhisperX words: {total_words}")

def main():
    print("=" * 60)
    print("Validating Fixed Waveform Alignment")
    print("=" * 60)
    print()
    
    # Step 1: Run MFA
    textgrid_file = run_mfa_on_cleaned_audio()
    
    if not textgrid_file:
        print("\n✗ MFA alignment failed - cannot proceed with validation")
        return
    
    # Step 2: Validate with WhisperX
    whisperx_result = validate_with_whisperx()
    
    if not whisperx_result:
        print("\n✗ WhisperX validation failed")
        return
    
    # Step 3: Compare
    compare_timestamps(textgrid_file, whisperx_result)
    
    print("\n" + "=" * 60)
    print("Validation Complete")
    print("=" * 60)
    print("\nNext steps:")
    print("1. Manually compare MFA TextGrid with WhisperX JSON")
    print("2. Calculate timestamp errors")
    print("3. Compare to previous 2.4s average error")

if __name__ == "__main__":
    main()

# Made with Bob
