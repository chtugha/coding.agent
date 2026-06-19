#!/usr/bin/env python3
"""
Process 10 podcast episodes with V3 alignment
Save cleaned audio and metadata to podcast_clean directory
"""

import json
import os
import glob
import re
import librosa
import soundfile as sf
from fix_waveform_alignment_v3 import align_audio_to_transcript_continuous

# Paths
PODCAST_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast"
TRANSCRIPT_DIR = os.path.join(PODCAST_DIR, "transcripts")
OUTPUT_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/podcast_clean"

# Create output directory
os.makedirs(OUTPUT_DIR, exist_ok=True)

print("="*80)
print("Processing 10 Podcast Episodes with V3 Alignment")
print("="*80)
print(f"Output directory: {OUTPUT_DIR}")

# Find all transcript files
transcript_files = sorted(glob.glob(os.path.join(TRANSCRIPT_DIR, "episode_*.json")))
print(f"\nFound {len(transcript_files)} transcript files")

# Find all audio files
audio_files = sorted(glob.glob(os.path.join(PODCAST_DIR, "*.mp3")))
print(f"Found {len(audio_files)} audio files")

# Match transcripts to audio files
matched_episodes = []
for transcript_path in transcript_files:
    # Extract episode number from transcript filename
    match = re.search(r"episode_(\d+)_", os.path.basename(transcript_path))
    if not match:
        continue
    
    episode_num = int(match.group(1))
    
    # Find matching audio file
    audio_path = None
    for audio_file in audio_files:
        if f"#{episode_num} " in os.path.basename(audio_file):
            audio_path = audio_file
            break
    
    if audio_path:
        matched_episodes.append({
            'episode_num': episode_num,
            'transcript_path': transcript_path,
            'audio_path': audio_path
        })

print(f"Matched {len(matched_episodes)} episodes with both transcript and audio")

# Sort by episode number and take first 10
matched_episodes.sort(key=lambda x: x['episode_num'])
episodes_to_process = matched_episodes[:10]

print(f"\nProcessing first 10 episodes:")
for ep in episodes_to_process:
    print(f"  Episode {ep['episode_num']}")

print("\n" + "="*80)

# Process each episode
results = []

for idx, episode in enumerate(episodes_to_process):
    ep_num = episode['episode_num']
    print(f"\n[{idx+1}/10] Processing Episode {ep_num}...")
    
    try:
        # Load transcript
        print(f"  Loading transcript...")
        with open(episode['transcript_path'], 'r') as f:
            transcript_data = json.load(f)
        
        segments = transcript_data['segments']
        print(f"  Loaded {len(segments)} segments")
        
        # Calculate expected duration from transcript
        expected_duration = segments[-1]['end'] - segments[0]['start']
        print(f"  Expected duration: {expected_duration:.1f}s ({expected_duration/60:.1f} min)")
        
        # Load audio
        print(f"  Loading audio...")
        audio, sr = librosa.load(episode['audio_path'], sr=None, mono=True)
        original_duration = len(audio) / sr
        print(f"  Original audio: {original_duration:.1f}s ({original_duration/60:.1f} min) at {sr}Hz")
        
        # Run V3 alignment
        print(f"  Running V3 alignment...")
        cleaned_audio, kept_regions = align_audio_to_transcript_continuous(audio, sr, segments)
        cleaned_duration = len(cleaned_audio) / sr
        removed_duration = original_duration - cleaned_duration
        removal_pct = (removed_duration / original_duration) * 100
        
        print(f"  Cleaned: {cleaned_duration:.1f}s ({cleaned_duration/60:.1f} min)")
        print(f"  Removed: {removed_duration:.1f}s ({removal_pct:.2f}%)")
        print(f"  Kept regions: {len(kept_regions)}")
        
        # Calculate accuracy
        duration_diff = abs(cleaned_duration - expected_duration)
        accuracy_pct = (duration_diff / expected_duration) * 100
        print(f"  Accuracy: {duration_diff:.1f}s difference ({accuracy_pct:.2f}%)")
        
        # Save cleaned audio
        audio_filename = f"episode_{ep_num:03d}_cleaned.wav"
        audio_output_path = os.path.join(OUTPUT_DIR, audio_filename)
        print(f"  Saving audio: {audio_filename}")
        sf.write(audio_output_path, cleaned_audio, int(sr))
        
        # Save metadata
        metadata = {
            'episode_num': ep_num,
            'original_audio_path': episode['audio_path'],
            'transcript_path': episode['transcript_path'],
            'original_duration': original_duration,
            'cleaned_duration': cleaned_duration,
            'removed_duration': removed_duration,
            'removal_percentage': removal_pct,
            'expected_duration': expected_duration,
            'duration_difference': duration_diff,
            'accuracy_percentage': accuracy_pct,
            'kept_regions': kept_regions,
            'sample_rate': int(sr),
            'num_segments': len(segments)
        }
        
        metadata_filename = f"episode_{ep_num:03d}_metadata.json"
        metadata_output_path = os.path.join(OUTPUT_DIR, metadata_filename)
        print(f"  Saving metadata: {metadata_filename}")
        with open(metadata_output_path, 'w') as f:
            json.dump(metadata, f, indent=2)
        
        # Store result
        results.append({
            'episode_num': ep_num,
            'success': True,
            'original_duration': original_duration,
            'cleaned_duration': cleaned_duration,
            'removed_duration': removed_duration,
            'removal_percentage': removal_pct,
            'accuracy_percentage': accuracy_pct,
            'num_regions': len(kept_regions)
        })
        
        print(f"  ✓ Episode {ep_num} complete")
        
    except Exception as e:
        print(f"  ✗ ERROR processing episode {ep_num}: {str(e)}")
        results.append({
            'episode_num': ep_num,
            'success': False,
            'error': str(e)
        })

# Save summary
print("\n" + "="*80)
print("PROCESSING COMPLETE")
print("="*80)

summary = {
    'total_episodes': len(episodes_to_process),
    'successful': sum(1 for r in results if r['success']),
    'failed': sum(1 for r in results if not r['success']),
    'results': results
}

summary_path = os.path.join(OUTPUT_DIR, "processing_summary.json")
with open(summary_path, 'w') as f:
    json.dump(summary, f, indent=2)

print(f"\nSummary saved to: {summary_path}")

# Print statistics
successful_results = [r for r in results if r['success']]
if successful_results:
    print(f"\nStatistics for {len(successful_results)} successful episodes:")
    
    avg_removed = sum(r['removed_duration'] for r in successful_results) / len(successful_results)
    avg_removal_pct = sum(r['removal_percentage'] for r in successful_results) / len(successful_results)
    avg_accuracy = sum(r['accuracy_percentage'] for r in successful_results) / len(successful_results)
    
    print(f"  Average removed: {avg_removed:.1f}s ({avg_removal_pct:.2f}%)")
    print(f"  Average accuracy: {avg_accuracy:.2f}%")
    
    # Check for ads
    multi_region_episodes = [r for r in successful_results if r['num_regions'] > 1]
    print(f"\n  Episodes with multiple regions (ads detected): {len(multi_region_episodes)}")
    if multi_region_episodes:
        print(f"  Episodes with ads:")
        for r in multi_region_episodes:
            print(f"    Episode {r['episode_num']}: {r['num_regions']} regions")

print(f"\nAll files saved to: {OUTPUT_DIR}")

# Made with Bob