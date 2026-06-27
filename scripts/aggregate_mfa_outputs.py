#!/usr/bin/env python3
"""
Aggregate all MFA segment outputs into a single comprehensive JSON file
"""

import json
import os
from pathlib import Path

def aggregate_mfa_segments(mfa_output_dir, transcript_path, output_path):
    """Aggregate all MFA segment JSON files into one comprehensive output"""
    
    print("Loading original transcript for segment timing...")
    with open(transcript_path, 'r', encoding='utf-8') as f:
        transcript_data = json.load(f)
    
    segments = transcript_data['segments']
    max_duration = 300  # 5 minutes
    filtered_segments = [seg for seg in segments if seg['start'] < max_duration]
    
    print(f"Found {len(filtered_segments)} segments in first 5 minutes")
    
    # Find all MFA output JSON files
    json_files = sorted(Path(mfa_output_dir).glob("seg_*.json"))
    print(f"Found {len(json_files)} MFA output files")
    
    # Aggregate all words with adjusted timestamps
    all_words = []
    segments_processed = 0
    
    for json_file in json_files:
        # Extract segment index from filename (e.g., seg_0001.json -> 1)
        seg_idx = int(json_file.stem.split('_')[1])
        
        if seg_idx >= len(filtered_segments):
            print(f"Warning: Segment index {seg_idx} out of range, skipping")
            continue
        
        # Get the original segment timing
        original_segment = filtered_segments[seg_idx]
        segment_start = original_segment['start']
        
        # Load MFA output for this segment
        with open(json_file, 'r', encoding='utf-8') as f:
            mfa_data = json.load(f)
        
        # Extract words and adjust timestamps
        if 'tiers' in mfa_data:
            tiers = mfa_data['tiers']
            if isinstance(tiers, dict) and 'words' in tiers:
                entries = tiers['words'].get('entries', [])
                for entry in entries:
                    start, end, text = entry
                    if text:  # Skip empty entries
                        # Adjust timestamps relative to full audio
                        adjusted_start = segment_start + start
                        adjusted_end = segment_start + end
                        all_words.append({
                            'text': text,
                            'start': adjusted_start,
                            'end': adjusted_end,
                            'duration': end - start,
                            'segment_idx': seg_idx
                        })
        
        segments_processed += 1
    
    print(f"\nProcessed {segments_processed} segments")
    print(f"Total words extracted: {len(all_words)}")
    
    # Sort by start time
    all_words.sort(key=lambda x: x['start'])
    
    # Create output structure
    output_data = {
        'source': 'MFA alignment',
        'audio_duration': max_duration,
        'segments_processed': segments_processed,
        'total_words': len(all_words),
        'words': all_words
    }
    
    # Save aggregated output
    with open(output_path, 'w', encoding='utf-8') as f:
        json.dump(output_data, f, indent=2, ensure_ascii=False)
    
    print(f"\nAggregated output saved to: {output_path}")
    
    # Show first 20 words
    print("\nFirst 20 words:")
    for i, word in enumerate(all_words[:20]):
        print(f"  {i+1}. [{word['start']:.3f}s - {word['end']:.3f}s] {word['text']}")
    
    return output_data

def main():
    mfa_output_dir = "/tmp/mfa_output_ep150_5min"
    transcript_path = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/transcripts_aligned_final/episode_150_gemischtes_hack.json"
    output_path = "/Volumes/eHDD/moshi-rag-data/datasets/whisper_cpp_test/episode_150_5min_mfa_aligned_full.json"
    
    print("="*70)
    print("Aggregating MFA Segment Outputs")
    print("="*70)
    print()
    
    result = aggregate_mfa_segments(mfa_output_dir, transcript_path, output_path)
    
    print("\n" + "="*70)
    print("Aggregation Complete")
    print("="*70)
    print(f"\nTotal words: {result['total_words']}")
    print(f"Segments processed: {result['segments_processed']}")
    print(f"Duration: {result['audio_duration']}s")

if __name__ == "__main__":
    main()

# Made with Bob