#!/usr/bin/env python3
"""
Run MFA alignment on 10 cleaned podcast episodes
Processes one episode at a time to avoid memory issues
"""

import json
import os
import glob
import subprocess
import shutil
import librosa
import soundfile as sf
import sys
from pathlib import Path

# Paths
CLEAN_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/podcast_clean"
OUTPUT_DIR = os.path.join(CLEAN_DIR, "mfa_output")

def process_episode(episode_num, audio_path, metadata_path):
    """Process a single episode through MFA"""
    print(f"\n{'='*70}")
    print(f"Processing Episode {episode_num}")
    print(f"{'='*70}")
    
    # Load metadata to get transcript path
    with open(metadata_path, 'r') as f:
        metadata = json.load(f)
    
    transcript_path = metadata.get('transcript_path')
    if not transcript_path or not os.path.exists(transcript_path):
        print(f"  ✗ No transcript found")
        return False
    
    # Load transcript
    print(f"  Loading transcript...")
    with open(transcript_path, 'r') as f:
        transcript_data = json.load(f)
    
    segments = transcript_data['segments']
    text_content = " ".join([seg['text'] for seg in segments])
    print(f"  Segments: {len(segments)}, Words: ~{len(text_content.split())}")
    
    # Create temporary MFA corpus directory for this episode
    corpus_dir = f"/tmp/mfa_ep{episode_num:03d}_corpus"
    output_dir = f"/tmp/mfa_ep{episode_num:03d}_output"
    
    os.makedirs(corpus_dir, exist_ok=True)
    os.makedirs(output_dir, exist_ok=True)
    
    # Copy audio file directly (MFA will handle conversion)
    print(f"  Copying audio file...")
    mfa_audio_path = os.path.join(corpus_dir, f"episode_{episode_num:03d}.wav")
    shutil.copy(audio_path, mfa_audio_path)
    print(f"  Audio copied")
    
    # Create text file
    mfa_text_path = os.path.join(corpus_dir, f"episode_{episode_num:03d}.txt")
    with open(mfa_text_path, 'w', encoding='utf-8') as f:
        f.write(text_content)
    
    # Run MFA alignment
    print(f"  Running MFA alignment (this may take 5-10 minutes)...")
    
    mfa_cmd = [
        "mfa", "align",
        "--clean",
        "--single_speaker",
        "--beam", "100",
        "--retry_beam", "400",
        "--output_format", "json",
        corpus_dir,
        "german_mfa",
        "german_mfa",
        output_dir
    ]
    
    try:
        result = subprocess.run(mfa_cmd, capture_output=True, text=True, timeout=1800)
        
        if result.returncode != 0:
            print(f"  ✗ MFA failed!")
            print(f"  Error: {result.stderr[:200]}")
            return False
        
        # Find output JSON
        json_files = list(Path(output_dir).glob("**/*.json"))
        if not json_files:
            print(f"  ✗ No JSON output found")
            return False
        
        # Load and save to final location
        with open(json_files[0], 'r') as f:
            mfa_result = json.load(f)
        
        # Count words
        word_count = 0
        if 'tiers' in mfa_result:
            tiers = mfa_result['tiers']
            if isinstance(tiers, dict) and 'words' in tiers:
                word_count = len(tiers['words'].get('entries', []))
            elif isinstance(tiers, list):
                for tier in tiers:
                    if tier.get('name') == 'words':
                        word_count = len(tier.get('entries', []))
        
        # Save to output directory
        os.makedirs(OUTPUT_DIR, exist_ok=True)
        output_json = os.path.join(OUTPUT_DIR, f"episode_{episode_num:03d}_mfa.json")
        with open(output_json, 'w', encoding='utf-8') as f:
            json.dump(mfa_result, f, indent=2, ensure_ascii=False)
        
        print(f"  ✓ Success! Aligned {word_count} words")
        print(f"  Saved to: {output_json}")
        
        # Cleanup temp directories
        shutil.rmtree(corpus_dir, ignore_errors=True)
        shutil.rmtree(output_dir, ignore_errors=True)
        
        return True
        
    except subprocess.TimeoutExpired:
        print(f"  ✗ MFA timed out (>30 minutes)")
        return False
    except Exception as e:
        print(f"  ✗ Error: {e}")
        return False

def main():
    print("="*70)
    print("MFA Alignment for 10 Cleaned Episodes")
    print("="*70)
    print(f"Output directory: {OUTPUT_DIR}")
    print()
    
    # Find all cleaned audio files
    audio_files = sorted(glob.glob(os.path.join(CLEAN_DIR, "episode_*_cleaned.wav")))
    print(f"Found {len(audio_files)} cleaned audio files")
    
    # Process each episode
    results = []
    for i, audio_path in enumerate(audio_files, 1):
        basename = os.path.basename(audio_path)
        episode_num = int(basename.split('_')[1])
        metadata_path = audio_path.replace('_cleaned.wav', '_metadata.json')
        
        if not os.path.exists(metadata_path):
            print(f"\nEpisode {episode_num}: No metadata found, skipping")
            results.append((episode_num, False))
            continue
        
        success = process_episode(episode_num, audio_path, metadata_path)
        results.append((episode_num, success))
        
        print(f"\nProgress: {i}/{len(audio_files)} episodes processed")
    
    # Summary
    print("\n" + "="*70)
    print("Summary")
    print("="*70)
    successful = sum(1 for _, success in results if success)
    print(f"Successful: {successful}/{len(results)}")
    print(f"Failed: {len(results) - successful}/{len(results)}")
    
    if successful > 0:
        print(f"\nMFA output files saved to: {OUTPUT_DIR}")
    
    return 0 if successful == len(results) else 1

if __name__ == "__main__":
    sys.exit(main())