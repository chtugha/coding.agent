#!/usr/bin/env python3
"""
Process episodes 150-159 with V3 waveform alignment.
Reads original MP3 files, applies V3 alignment, and outputs to podcast_clean directory.
"""

import json
import time
import os
import glob
import librosa
import soundfile as sf
from pathlib import Path
import sys

# Import the V3 alignment function
sys.path.insert(0, '/Users/whisper/zenflow_projects/coding.agent/scripts')
from fix_waveform_alignment_v3 import align_audio_to_transcript_continuous

# Directories
AUDIO_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast"
TRANSCRIPT_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/transcripts"
OUTPUT_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/podcast_clean"

# Episode range
START_EPISODE = 150
END_EPISODE = 159

def find_audio_file(episode_num):
    """Find the audio file for a given episode number"""
    pattern = f"#{episode_num} *.mp3"
    matches = glob.glob(os.path.join(AUDIO_DIR, pattern))
    if matches:
        return matches[0]
    return None

def find_transcript_file(episode_num):
    """Find the transcript JSON file for a given episode number"""
    pattern = f"episode_{episode_num}_*.json"
    matches = glob.glob(os.path.join(TRANSCRIPT_DIR, pattern))
    if matches:
        return matches[0]
    return None

def process_episode(episode_num):
    """Process a single episode with V3 alignment"""
    print("\n" + "="*80)
    print(f"Processing Episode {episode_num}")
    print("="*80)
    
    # Find files
    audio_file = find_audio_file(episode_num)
    transcript_file = find_transcript_file(episode_num)
    
    if not audio_file:
        print(f"❌ Audio file not found for episode {episode_num}")
        return False
    
    if not transcript_file:
        print(f"❌ Transcript file not found for episode {episode_num}")
        return False
    
    print(f"\n📁 Files:")
    print(f"   Audio: {os.path.basename(audio_file)}")
    print(f"   Transcript: {os.path.basename(transcript_file)}")
    
    # Output paths
    output_audio = os.path.join(OUTPUT_DIR, f"episode_{episode_num:03d}_cleaned.wav")
    output_metadata = os.path.join(OUTPUT_DIR, f"episode_{episode_num:03d}_metadata.json")
    
    # Load transcript
    print(f"\n1️⃣  Loading transcript...")
    try:
        with open(transcript_file, 'r', encoding='utf-8') as f:
            transcript_data = json.load(f)
        
        # Handle different transcript formats
        if 'segments' in transcript_data:
            segments = transcript_data['segments']
        elif 'transcription' in transcript_data:
            segments = transcript_data['transcription']
        else:
            print(f"❌ Unknown transcript format")
            return False
        
        print(f"   ✓ Loaded {len(segments)} segments")
    except Exception as e:
        print(f"❌ Error loading transcript: {e}")
        return False
    
    # Load audio
    print(f"\n2️⃣  Loading audio...")
    try:
        start_time = time.time()
        audio, sr = librosa.load(audio_file, sr=None, mono=True)
        load_time = time.time() - start_time
        duration = len(audio) / sr
        print(f"   ✓ Sample rate: {sr} Hz")
        print(f"   ✓ Duration: {duration:.1f}s ({duration/60:.1f} min)")
        print(f"   ✓ Load time: {load_time:.2f}s")
    except Exception as e:
        print(f"❌ Error loading audio: {e}")
        return False
    
    # Run V3 alignment
    print(f"\n3️⃣  Running V3 waveform alignment...")
    try:
        start_time = time.time()
        cleaned_audio, kept_regions = align_audio_to_transcript_continuous(audio, sr, segments)
        align_time = time.time() - start_time
        
        print(f"   ✓ Alignment time: {align_time:.2f}s")
        print(f"   ✓ Kept regions: {len(kept_regions)}")
    except Exception as e:
        print(f"❌ Error during alignment: {e}")
        return False
    
    # Calculate statistics
    original_duration = len(audio) / sr
    cleaned_duration = len(cleaned_audio) / sr
    removed_duration = original_duration - cleaned_duration
    removal_pct = (removed_duration / original_duration) * 100
    
    print(f"\n4️⃣  Results:")
    print(f"   Original:  {original_duration:.1f}s ({original_duration/60:.1f} min)")
    print(f"   Cleaned:   {cleaned_duration:.1f}s ({cleaned_duration/60:.1f} min)")
    print(f"   Removed:   {removed_duration:.1f}s ({removed_duration/60:.1f} min)")
    print(f"   Removal:   {removal_pct:.2f}%")
    print(f"   Speed:     {duration/align_time:.1f}x realtime")
    
    # Save cleaned audio
    print(f"\n5️⃣  Saving outputs...")
    try:
        sf.write(output_audio, cleaned_audio, sr)
        print(f"   ✓ Audio: {output_audio}")
        
        # Save metadata
        metadata = {
            'episode': episode_num,
            'original_file': os.path.basename(audio_file),
            'transcript_file': os.path.basename(transcript_file),
            'original_duration': original_duration,
            'cleaned_duration': cleaned_duration,
            'removed_duration': removed_duration,
            'removal_percentage': removal_pct,
            'sample_rate': sr,
            'segments_count': len(segments),
            'kept_regions_count': len(kept_regions),
            'processing_time': align_time,
            'processing_speed': duration/align_time
        }
        
        with open(output_metadata, 'w', encoding='utf-8') as f:
            json.dump(metadata, f, indent=2)
        print(f"   ✓ Metadata: {output_metadata}")
        
    except Exception as e:
        print(f"❌ Error saving outputs: {e}")
        return False
    
    print(f"\n✅ Episode {episode_num} completed successfully!")
    return True

def main():
    print("="*80)
    print("V3 Waveform Alignment - Episodes 150-159")
    print("="*80)
    print(f"\nAudio directory: {AUDIO_DIR}")
    print(f"Transcript directory: {TRANSCRIPT_DIR}")
    print(f"Output directory: {OUTPUT_DIR}")
    print(f"\nProcessing episodes {START_EPISODE} to {END_EPISODE}")
    
    # Create output directory if needed
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    
    # Process each episode
    results = {}
    total_start = time.time()
    
    for episode_num in range(START_EPISODE, END_EPISODE + 1):
        success = process_episode(episode_num)
        results[episode_num] = success
    
    total_time = time.time() - total_start
    
    # Summary
    print("\n" + "="*80)
    print("PROCESSING SUMMARY")
    print("="*80)
    
    successful = sum(1 for v in results.values() if v)
    failed = sum(1 for v in results.values() if not v)
    
    print(f"\n✅ Successful: {successful}/{len(results)}")
    print(f"❌ Failed: {failed}/{len(results)}")
    print(f"⏱️  Total time: {total_time:.1f}s ({total_time/60:.1f} min)")
    
    if failed > 0:
        print(f"\nFailed episodes:")
        for ep, success in results.items():
            if not success:
                print(f"  - Episode {ep}")
    
    print("\n" + "="*80)

if __name__ == "__main__":
    main()

# Made with Bob
