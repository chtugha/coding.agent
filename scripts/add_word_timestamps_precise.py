#!/usr/bin/env python3
"""
Add precise word-level timestamps using Whisper's word-level transcription.

This re-transcribes with Whisper's word timestamps enabled, which is more
reliable than forced alignment for getting exact word boundaries.
"""

import json
import os
import sys
from pathlib import Path
import argparse
import whisper
import torch

def transcribe_with_word_timestamps(audio_path, language="de"):
    """
    Transcribe audio with word-level timestamps using Whisper.
    
    Returns segments with word-level timestamps.
    """
    print(f"Loading Whisper model...")
    model = whisper.load_model("large-v3")
    
    print(f"Transcribing with word timestamps...")
    result = model.transcribe(
        str(audio_path),
        language=language,
        word_timestamps=True,
        verbose=False
    )
    
    return result["segments"]

def match_segments_to_speakers(new_segments, original_segments):
    """
    Match new segments with word timestamps to original segments with speaker labels.
    
    Uses time overlap to assign speakers to new segments.
    """
    matched_segments = []
    
    for new_seg in new_segments:
        new_start = new_seg["start"]
        new_end = new_seg["end"]
        
        # Find original segment with most overlap
        best_match = None
        best_overlap = 0
        
        for orig_seg in original_segments:
            orig_start = orig_seg["start"]
            orig_end = orig_seg["end"]
            
            # Calculate overlap
            overlap_start = max(new_start, orig_start)
            overlap_end = min(new_end, orig_end)
            overlap = max(0, overlap_end - overlap_start)
            
            if overlap > best_overlap:
                best_overlap = overlap
                best_match = orig_seg
        
        # Create matched segment
        matched_seg = {
            "id": new_seg.get("id", 0),
            "start": new_seg["start"],
            "end": new_seg["end"],
            "text": new_seg["text"],
            "words": new_seg.get("words", []),
            "speaker": best_match["speaker"] if best_match else "Unknown"
        }
        
        # Copy other fields from original if available
        if best_match:
            for key in ["avg_logprob", "no_speech_prob"]:
                if key in best_match:
                    matched_seg[key] = best_match[key]
        
        matched_segments.append(matched_seg)
    
    return matched_segments

def process_dataset(dataset_path, output_path=None, max_episodes=None):
    """
    Process all episodes in a dataset, adding word-level timestamps.
    """
    dataset_path = Path(dataset_path)
    
    # Find transcript directory
    transcript_dir = dataset_path / "transcripts"
    
    if not transcript_dir.exists():
        print(f"Error: Could not find transcripts/ in {dataset_path}")
        return
    
    # Audio files are in the same directory as the dataset
    audio_dir = dataset_path
    
    # Get list of transcript files (exclude .chunks.json files)
    transcript_files = sorted([f for f in transcript_dir.glob("*.json") 
                              if not f.name.endswith('.chunks.json')])
    
    if max_episodes:
        transcript_files = transcript_files[:max_episodes]
    
    print(f"Found {len(transcript_files)} transcript files to process")
    
    # Process each episode
    for i, transcript_path in enumerate(transcript_files, 1):
        print(f"\n[{i}/{len(transcript_files)}] Processing: {transcript_path.name}")
        
        try:
            # Load transcript to get title and original segments
            with open(transcript_path, 'r', encoding='utf-8') as f:
                transcript_data = json.load(f)
            
            title = transcript_data.get('meta', {}).get('title', '')
            if not title:
                print(f"  Warning: No title in transcript metadata")
                continue
            
            original_segments = transcript_data.get('segments', [])
            if not original_segments:
                print(f"  Warning: No segments in transcript")
                continue
            
            # Check if already has word-level timestamps
            if original_segments and 'words' in original_segments[0] and original_segments[0]['words']:
                print(f"  Already has word-level timestamps, skipping")
                continue
            
            # Find audio file by title
            audio_path = None
            for audio_file in audio_dir.glob("*.mp3"):
                if title.lower() in audio_file.name.lower():
                    audio_path = audio_file
                    break
            
            if not audio_path:
                # Try .wav files
                for audio_file in audio_dir.glob("*.wav"):
                    if title.lower() in audio_file.name.lower():
                        audio_path = audio_file
                        break
            
            if not audio_path:
                print(f"  Warning: Audio file not found for title: {title}")
                continue
            
            print(f"  Audio: {audio_path.name}")
            
            # Transcribe with word timestamps
            new_segments = transcribe_with_word_timestamps(str(audio_path))
            
            print(f"  Got {len(new_segments)} segments with word timestamps")
            
            # Match to original segments to preserve speaker labels
            matched_segments = match_segments_to_speakers(new_segments, original_segments)
            
            # Update transcript data
            transcript_data['segments'] = matched_segments
            
            # Save updated transcript
            if output_path:
                output_dir = Path(output_path) / "transcripts"
                output_dir.mkdir(parents=True, exist_ok=True)
                output_file = output_dir / transcript_path.name
            else:
                # Create backup
                backup_file = transcript_path.with_suffix('.json.backup')
                if not backup_file.exists():
                    import shutil
                    shutil.copy2(transcript_path, backup_file)
                    print(f"  Created backup: {backup_file.name}")
                
                output_file = transcript_path
            
            with open(output_file, 'w', encoding='utf-8') as f:
                json.dump(transcript_data, f, ensure_ascii=False, indent=2)
            
            # Count words added
            total_words = sum(len(seg.get('words', [])) for seg in matched_segments)
            print(f"  ✓ Added {total_words} word timestamps across {len(matched_segments)} segments")
            print(f"  ✓ Saved to: {output_file}")
            
        except Exception as e:
            print(f"  Error processing {transcript_path.name}: {e}")
            import traceback
            traceback.print_exc()
            continue

def main():
    parser = argparse.ArgumentParser(
        description="Add precise word-level timestamps using Whisper re-transcription"
    )
    parser.add_argument(
        "dataset",
        help="Path to dataset directory (should contain audio/ and transcripts/ subdirs)"
    )
    parser.add_argument(
        "--output",
        help="Output directory (default: overwrite original transcripts with backup)"
    )
    parser.add_argument(
        "--max-episodes",
        type=int,
        help="Maximum number of episodes to process (for testing)"
    )
    
    args = parser.parse_args()
    
    # Check if whisper is installed
    try:
        import whisper
    except ImportError:
        print("Error: whisper not installed")
        print("Install with: pip install openai-whisper")
        sys.exit(1)
    
    process_dataset(
        args.dataset,
        output_path=args.output,
        max_episodes=args.max_episodes
    )
    
    print("\n✓ Processing complete!")

if __name__ == "__main__":
    main()

# Made with Bob
