#!/usr/bin/env python3
"""
Retry failed episodes 152, 157, 159 with 60-minute timeout
"""

import json
import os
import subprocess
import sys
from pathlib import Path
import glob
import shutil

def process_episode(episode_num):
    """Process a single episode with MFA alignment"""
    print("=" * 70)
    print(f"Running MFA on Episode {episode_num}")
    print("=" * 70)
    print()
    
    # Paths
    cleaned_audio = f"/Volumes/eHDD/moshi-rag-data/datasets/podcast_clean/episode_{episode_num:03d}_cleaned.wav"
    transcript_pattern = f"/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/transcripts/episode_{episode_num}_*.json"
    
    # Find transcript file
    transcript_files = glob.glob(transcript_pattern)
    if not transcript_files:
        print(f"✗ No transcript found for episode {episode_num}")
        return False
    
    transcript_path = transcript_files[0]
    
    # Check if audio exists
    if not os.path.exists(cleaned_audio):
        print(f"✗ Audio file not found: {cleaned_audio}")
        return False
    
    # Create MFA corpus directory structure on external drive
    corpus_dir = f"/Volumes/eHDD/moshi-rag-data/datasets/podcast_clean/mfa_temp/corpus_ep{episode_num}"
    output_dir = f"/Volumes/eHDD/moshi-rag-data/datasets/podcast_clean/mfa_output/episode_{episode_num:03d}"
    
    os.makedirs(output_dir, exist_ok=True)
    
    # Load transcript
    print("Loading transcript...")
    with open(transcript_path, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    segments = data['segments']
    print(f"Segments: {len(segments)}")
    
    # Ensure corpus directory exists and copy audio
    os.makedirs(corpus_dir, exist_ok=True)
    corpus_audio = os.path.join(corpus_dir, f"episode_{episode_num}.wav")
    shutil.copy(cleaned_audio, corpus_audio)
    print(f"Copied audio to corpus: {corpus_audio}")
    
    # Create text file in corpus directory (same basename as audio)
    text_file = os.path.join(corpus_dir, f"episode_{episode_num}.txt")
    with open(text_file, 'w', encoding='utf-8') as f:
        for seg in segments:
            f.write(seg['text'] + ' ')
    
    print(f"Created text file: {text_file}")
    print()
    
    # Run MFA with 60-minute timeout
    print("Running MFA alignment...")
    print("This may take up to 60 minutes...")
    print()
    
    cmd = [
        "mfa", "align",
        corpus_dir,
        "german_mfa",
        "german_mfa",
        output_dir,
        "--clean",
        "--single_speaker",
        "--beam", "100",
        "--retry_beam", "400",
        "--output_format", "json"
    ]
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
        
        if result.returncode == 0:
            print("✓ MFA alignment completed successfully")
        else:
            print(f"✗ MFA alignment failed with return code {result.returncode}")
            print(f"STDERR: {result.stderr}")
            return False
            
    except subprocess.TimeoutExpired:
        print("✗ MFA alignment timed out after 60 minutes")
        return False
    except Exception as e:
        print(f"✗ MFA alignment error: {e}")
        return False
    
    # Find output files
    print()
    print("Looking for MFA output files...")
    
    json_files = list(Path(output_dir).glob("**/*.json"))
    
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
                word_count = len(tiers['words'].get('entries', []))
        
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
        
        # Save to output directory
        output_json = os.path.join(output_dir, f"episode_{episode_num:03d}_mfa_aligned.json")
        with open(output_json, 'w', encoding='utf-8') as f:
            json.dump(mfa_result, f, indent=2, ensure_ascii=False)
        print(f"  Saved to: {output_json}")
    else:
        print("✗ No output files found")
        return False
    
    # Cleanup temp directory
    shutil.rmtree(corpus_dir, ignore_errors=True)
    
    print()
    print("=" * 70)
    print(f"Episode {episode_num} Complete")
    print("=" * 70)
    print()
    
    return True

def main():
    """Process failed episodes 152, 157, 159"""
    failed_episodes = [152, 157, 159]
    
    print("=" * 70)
    print("Retrying Failed Episodes: 152, 157, 159")
    print("=" * 70)
    print()
    
    results = {}
    for episode_num in failed_episodes:
        success = process_episode(episode_num)
        results[episode_num] = success
    
    # Summary
    print()
    print("=" * 70)
    print("RETRY SUMMARY")
    print("=" * 70)
    successful = sum(1 for v in results.values() if v)
    failed = len(results) - successful
    
    print(f"Total episodes: {len(results)}")
    print(f"Successful: {successful}")
    print(f"Failed: {failed}")
    print()
    
    for ep, success in sorted(results.items()):
        status = "✓" if success else "✗"
        print(f"  {status} Episode {ep}")
    
    return failed == 0

if __name__ == "__main__":
    success = main()
    sys.exit(0 if success else 1)

# Made with Bob
