#!/usr/bin/env python3
"""
Test script for waveform-based alignment on a single podcast episode.
This validates the new alignment approach before full reprocessing.
"""
import os
import sys
import json
import numpy as np
import librosa
import soundfile as sf

# Add parent directory to path to import from prepare_german_dataset
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from prepare_german_dataset import (
    clean_podcast_ads_waveform_based,
    parse_podcast_turns,
    process_dialogue,
    TARGET_SR
)

RAW_DIR = "/Volumes/eHDD/moshi-rag-data/datasets"
PROCESSED_DIR = "/Volumes/eHDD/moshi-rag-data/processed"
TEST_OUTPUT_DIR = "/Volumes/eHDD/moshi-rag-data/processed/podcast_test"


def test_single_episode(ep_num=150):
    """Test waveform-based alignment on a single episode"""
    print(f"\n{'='*60}")
    print(f"Testing Waveform-Based Alignment on Episode {ep_num}")
    print(f"{'='*60}\n")
    
    # Find episode files
    trans_dir = os.path.join(RAW_DIR, "Gemischtes.Hack.Podcast", "transcripts")
    audio_dir = os.path.join(RAW_DIR, "Gemischtes.Hack.Podcast")
    
    # Find transcript file (may have episode title in filename)
    import glob
    json_files = glob.glob(os.path.join(trans_dir, f"episode_{ep_num}_*.json"))
    
    if not json_files:
        print(f"ERROR: Transcript not found for episode {ep_num}")
        print(f"  Searched in: {trans_dir}")
        return False
    
    json_path = json_files[0]
    mp3_files = [f for f in os.listdir(audio_dir) if f.startswith(f"#{ep_num} ") and f.endswith(".mp3")]
    
    if not mp3_files:
        print(f"ERROR: Audio file not found for episode {ep_num}")
        return False
    
    mp3_path = os.path.join(audio_dir, mp3_files[0])
    
    print(f"Transcript: {json_path}")
    print(f"Audio: {mp3_path}")
    print()
    
    try:
        # Load transcript
        with open(json_path, "r", encoding="utf-8") as f:
            data = json.load(f)
        segments = data.get("segments", [])
        print(f"Transcript segments: {len(segments)}")
        
        # Parse turns
        turns = parse_podcast_turns(json_path)
        print(f"Speaker turns: {len(turns)}")
        print()
        
        # Load audio
        print("Loading audio...")
        mono, sr = librosa.load(mp3_path, sr=TARGET_SR, mono=True)
        mono = mono.astype(np.float32)
        original_duration = len(mono) / sr
        print(f"Original audio: {original_duration:.1f}s ({sr}Hz)")
        print()
        
        # Apply waveform-based cleaning
        print("Applying waveform-based ad removal...")
        cleaned, offset, ad_breaks = clean_podcast_ads_waveform_based(
            mono, int(sr), json_path, ep_label=f"ep{ep_num}"
        )
        
        cleaned_duration = len(cleaned) / sr
        removed_duration = original_duration - cleaned_duration
        
        print()
        print(f"Results:")
        print(f"  Offset: {offset:.1f}s")
        print(f"  Ad breaks detected: {len(ad_breaks)}")
        for i, (start, end) in enumerate(ad_breaks, 1):
            print(f"    Ad {i}: {start:.1f}s - {end:.1f}s ({end-start:.1f}s)")
        print(f"  Original duration: {original_duration:.1f}s")
        print(f"  Cleaned duration: {cleaned_duration:.1f}s")
        print(f"  Removed: {removed_duration:.1f}s ({removed_duration/original_duration*100:.1f}%)")
        print()
        
        # Process dialogue to create chunks
        print("Creating chunks...")
        os.makedirs(TEST_OUTPUT_DIR, exist_ok=True)
        n_chunks = process_dialogue(
            cleaned, int(sr), None, None, 
            "podcast", f"ep{ep_num}", 
            TEST_OUTPUT_DIR, 
            precomputed_turns=turns
        )
        
        print(f"Created {n_chunks} chunks")
        print()
        
        # Verify chunks
        print("Verifying chunks...")
        chunk_files = sorted([f for f in os.listdir(TEST_OUTPUT_DIR) if f.startswith(f"ep{ep_num}_")])
        wav_files = [f for f in chunk_files if f.endswith(".wav")]
        json_files = [f for f in chunk_files if f.endswith(".json") and not f.endswith("_wverify.json")]
        
        print(f"  WAV files: {len(wav_files)}")
        print(f"  JSON files: {len(json_files)}")
        
        # Check first few chunks
        print("\nFirst 5 chunks:")
        for i, (wav_f, json_f) in enumerate(zip(wav_files[:5], json_files[:5])):
            wav_path = os.path.join(TEST_OUTPUT_DIR, wav_f)
            json_path = os.path.join(TEST_OUTPUT_DIR, json_f)
            
            info = sf.info(wav_path)
            with open(json_path, "r") as f:
                chunk_data = json.load(f)
            
            alignments = chunk_data.get("alignments", [])
            if alignments:
                first_word = alignments[0][0]
                last_word = alignments[-1][0]
                first_time = alignments[0][1][0]
                last_time = alignments[-1][1][1]
            else:
                first_word = last_word = "N/A"
                first_time = last_time = 0.0
            
            print(f"  {wav_f}:")
            print(f"    Duration: {info.duration:.2f}s")
            print(f"    Format: {info.samplerate}Hz, {info.channels}ch, {info.subtype}")
            print(f"    Words: {len(alignments)}")
            print(f"    Text: {first_word}...{last_word}")
            print(f"    Time: {first_time:.2f}s - {last_time:.2f}s")
        
        print()
        print(f"{'='*60}")
        print("Test completed successfully!")
        print(f"Test output directory: {TEST_OUTPUT_DIR}")
        print(f"{'='*60}")
        
        return True
        
    except Exception as e:
        print(f"\nERROR: {e}")
        import traceback
        traceback.print_exc()
        return False


if __name__ == "__main__":
    ep_num = int(sys.argv[1]) if len(sys.argv) > 1 else 150
    success = test_single_episode(ep_num)
    sys.exit(0 if success else 1)

# Made with Bob
