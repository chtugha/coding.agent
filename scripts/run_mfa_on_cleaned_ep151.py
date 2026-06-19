#!/usr/bin/env python3
"""
Run MFA alignment on cleaned episode 151 audio.
This should produce much better timestamps than the previous 2.4s error.
"""

import json
import os
import subprocess
import sys
from pathlib import Path

def main():
    print("=" * 70)
    print("Running MFA on Cleaned Episode 151")
    print("=" * 70)
    print()
    
    # Paths
    cleaned_audio = "/tmp/fixed_alignment_test/episode_151_cleaned_fixed.wav"
    transcript_path = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/Gemischtes.Hack.Podcast.Transcript/transcripts/episode_151_alle_heissen_tim_und_alles_muss_raus.json"
    
    # Create MFA corpus directory structure
    corpus_dir = "/tmp/mfa_corpus_ep151"
    output_dir = "/tmp/mfa_output_ep151"
    
    os.makedirs(corpus_dir, exist_ok=True)
    os.makedirs(output_dir, exist_ok=True)
    
    # Load transcript
    print("Loading transcript...")
    with open(transcript_path, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    segments = data['segments']
    print(f"Segments: {len(segments)}")
    
    # Copy audio to corpus directory
    import shutil
    corpus_audio = os.path.join(corpus_dir, "episode_151.wav")
    shutil.copy(cleaned_audio, corpus_audio)
    print(f"Copied audio to corpus: {corpus_audio}")
    
    # Create text file in corpus directory (same basename as audio)
    text_file = os.path.join(corpus_dir, "episode_151.txt")
    with open(text_file, 'w', encoding='utf-8') as f:
        for seg in segments:
            f.write(seg['text'] + ' ')
    
    print(f"Created text file: {text_file}")
    print()
    
    # Run MFA
    print("Running MFA alignment...")
    print("This may take several minutes...")
    print()
    
    cmd = [
        "mfa", "align",
        "--clean",
        "--single_speaker",
        "--output_format", "json",
        corpus_dir,
        "german_mfa",
        "german_mfa",
        output_dir
    ]
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
        
        if result.returncode == 0:
            print("✓ MFA alignment completed successfully")
        else:
            print(f"✗ MFA alignment failed with return code {result.returncode}")
            print(f"STDERR: {result.stderr}")
            return False
            
    except subprocess.TimeoutExpired:
        print("✗ MFA alignment timed out after 30 minutes")
        return False
    except Exception as e:
        print(f"✗ MFA alignment error: {e}")
        return False
    
    # Find output files
    print()
    print("Looking for MFA output files...")
    
    json_files = list(Path(output_dir).glob("**/*.json"))
    textgrid_files = list(Path(output_dir).glob("**/*.TextGrid"))
    
    if json_files:
        print(f"✓ Found JSON output: {json_files[0]}")
        
        # Load and analyze
        with open(json_files[0], 'r', encoding='utf-8') as f:
            mfa_result = json.load(f)
        
        # Count words
        word_count = 0
        if 'tiers' in mfa_result:
            tiers = mfa_result['tiers']
            if isinstance(tiers, dict) and 'words' in tiers:
                # New format: tiers is a dict with 'words' key
                word_count = len(tiers['words'].get('entries', []))
            elif isinstance(tiers, list):
                # Old format: tiers is a list
                for tier in tiers:
                    if tier.get('name') == 'words':
                        word_count = len(tier.get('entries', []))
        
        print(f"  Words aligned: {word_count}")
        
        # Show first 10 words
        if word_count > 0:
            print("\nFirst 10 word alignments:")
            tiers = mfa_result['tiers']
            if isinstance(tiers, dict) and 'words' in tiers:
                words = tiers['words'].get('entries', [])
                for i, word in enumerate(words[:10]):
                    start, end, text = word
                    print(f"  {i+1}. [{start:.3f} - {end:.3f}] {text}")
        
        # Save to better location
        output_json = "/tmp/fixed_alignment_test/episode_151_mfa_aligned.json"
        with open(output_json, 'w', encoding='utf-8') as f:
            json.dump(mfa_result, f, indent=2, ensure_ascii=False)
        print(f"  Saved to: {output_json}")
        
    elif textgrid_files:
        print(f"✓ Found TextGrid output: {textgrid_files[0]}")
    else:
        print("✗ No output files found")
        return False
    
    print()
    print("=" * 70)
    print("MFA Alignment Complete")
    print("=" * 70)
    print()
    print("Next step: Validate with WhisperX to measure timestamp accuracy")
    print("Expected: <100ms error (vs previous 2.4s error)")
    
    return True

if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)

# Made with Bob
