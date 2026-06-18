#!/usr/bin/env python3
"""
Test the fixed preparation script on episode 150 only.
"""

import sys
import os

# Add parent directory to path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Import the main processing function
from prepare_german_dataset import process_podcast

# Override the episode range for testing
import prepare_german_dataset as prep_module

# Temporarily modify the matched episodes logic
original_process = prep_module.process_podcast

def test_process_podcast():
    """Process only episode 150 for testing"""
    import glob
    import re
    import json
    
    RAW_DIR = "/Volumes/eHDD/moshi-rag-data/raw"
    PROCESSED_DIR = "/Volumes/eHDD/moshi-rag-data/processed"
    
    print("\n" + "=" * 60)
    print("Testing Episode 150 with Fixed Preparation Script")
    print("=" * 60)
    
    output_dir = os.path.join(PROCESSED_DIR, "podcast_test_fixed")
    os.makedirs(output_dir, exist_ok=True)
    
    trans_dir = os.path.join(RAW_DIR, "Gemischtes.Hack.Podcast", "transcripts")
    audio_dir = os.path.join(RAW_DIR, "Gemischtes.Hack.Podcast")
    
    if not os.path.exists(trans_dir):
        print(f"ERROR: Transcript directory not found: {trans_dir}")
        return
    
    # Find episode 150 files
    ep_jsons = sorted(glob.glob(os.path.join(trans_dir, "episode_*.json")))
    mp3_files = sorted(glob.glob(os.path.join(audio_dir, "*.mp3")))
    
    mp3_by_ep = {}
    for mp3 in mp3_files:
        m_mp3 = re.search(r"^#(\d+)\s", os.path.basename(mp3))
        if m_mp3:
            mp3_by_ep[int(m_mp3.group(1))] = mp3
    
    # Find episode 150
    json_p = None
    for jp in ep_jsons:
        m = re.search(r"episode_(\d+)_", os.path.basename(jp))
        if m and int(m.group(1)) == 150:
            json_p = jp
            break
    
    if not json_p or 150 not in mp3_by_ep:
        print("ERROR: Episode 150 not found")
        return
    
    mp3_path = mp3_by_ep[150]
    
    print(f"\nFound episode 150:")
    print(f"  Transcript: {json_p}")
    print(f"  Audio: {mp3_path}")
    
    # Process using the fixed script
    from prepare_german_dataset import (
        parse_podcast_turns,
        clean_podcast_ads_waveform_based,
        process_dialogue,
        log_error
    )
    import librosa
    import numpy as np
    
    try:
        print(f"\n  Parsing transcript...")
        turns = parse_podcast_turns(json_p)
        if not turns:
            print("ERROR: No speaker turns in transcript")
            return
        print(f"  Found {len(turns)} speaker turns")
        
        # Check if turns have word-level timestamps
        if turns and turns[0][3]:
            first_word = turns[0][3][0]
            print(f"  First word: '{first_word[0]}' at {first_word[1]:.3f}s - {first_word[2]:.3f}s")
        
        print(f"\n  Loading audio...")
        mono, sr = librosa.load(mp3_path, sr=48000, mono=True)
        mono = mono.astype(np.float32)
        print(f"  Audio loaded: {len(mono)/sr:.1f}s at {sr}Hz")
        
        print(f"\n  Cleaning ads with waveform alignment...")
        cleaned, offset, ad_breaks = clean_podcast_ads_waveform_based(
            mono, int(sr), json_p, ep_label="ep150"
        )
        print(f"  Cleaned audio: {len(cleaned)/sr:.1f}s (removed {len(mono)/sr - len(cleaned)/sr:.1f}s)")
        print(f"  Offset: {offset:.1f}s, Ad breaks: {len(ad_breaks)}")
        
        print(f"\n  Creating chunks...")
        n = process_dialogue(
            cleaned, int(sr), None, None, "podcast", 
            "ep150", output_dir, precomputed_turns=turns
        )
        print(f"\n  SUCCESS: Created {n} chunks in {output_dir}")
        
    except Exception as e:
        print(f"\nERROR: {e}")
        import traceback
        traceback.print_exc()

if __name__ == "__main__":
    test_process_podcast()

# Made with Bob
