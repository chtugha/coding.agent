#!/usr/bin/env python3
"""
Add word-level timestamps to existing podcast transcripts using forced alignment.

This is much faster than re-transcribing because we already have the text,
we just need to align it to the audio to get precise word timings.

Uses WhisperX for forced alignment which is optimized for this task.
"""

import json
import os
import sys
from pathlib import Path
import argparse
import whisperx
import torch
import gc

def load_existing_transcript(transcript_path):
    """Load existing segment-level transcript."""
    with open(transcript_path, 'r', encoding='utf-8') as f:
        return json.load(f)

def align_transcript_with_audio(audio_path, transcript_data, device="cpu", compute_type="float32"):
    """
    Use forced alignment to add word-level timestamps to existing transcript.
    
    Args:
        audio_path: Path to audio file
        transcript_data: Existing transcript with segments
        device: "cuda" or "cpu"
        compute_type: "float16" or "float32"
    
    Returns:
        Updated transcript with word-level timestamps
    """
    print(f"Loading audio: {audio_path}")
    
    # Load audio
    audio = whisperx.load_audio(audio_path)
    
    # Load alignment model for German
    print("Loading alignment model...")
    model_a, metadata = whisperx.load_align_model(
        language_code="de", 
        device=device
    )
    
    # Prepare segments for alignment
    segments = transcript_data.get('segments', [])
    
    # WhisperX expects segments in this format
    whisperx_segments = []
    for seg in segments:
        whisperx_segments.append({
            "start": seg["start"],
            "end": seg["end"],
            "text": seg["text"]
        })
    
    # Perform alignment
    print(f"Aligning {len(segments)} segments...")
    result = whisperx.align(
        whisperx_segments,
        model_a,
        metadata,
        audio,
        device,
        return_char_alignments=False
    )
    
    # Update original transcript with word-level timestamps
    aligned_segments = result["segments"]
    
    for i, aligned_seg in enumerate(aligned_segments):
        if i < len(segments):
            # Add word-level timestamps to original segment
            segments[i]["words"] = aligned_seg.get("words", [])
    
    # Clean up
    del model_a
    gc.collect()
    if device == "cuda":
        torch.cuda.empty_cache()
    
    return transcript_data

def process_dataset(dataset_path, output_path=None, max_episodes=None):
    """
    Process all episodes in a dataset, adding word-level timestamps.
    
    Args:
        dataset_path: Path to dataset directory
        output_path: Optional output directory (default: overwrite originals)
        max_episodes: Optional limit on number of episodes to process
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
    transcript_files = sorted([f for f in transcript_dir.glob("*.json") if not f.name.endswith('.chunks.json')])
    
    if max_episodes:
        transcript_files = transcript_files[:max_episodes]
    
    print(f"Found {len(transcript_files)} transcript files to process")
    
    # Determine device
    device = "cuda" if torch.cuda.is_available() else "cpu"
    compute_type = "float16" if device == "cuda" else "float32"
    print(f"Using device: {device} with compute_type: {compute_type}")
    
    # Process each episode
    for i, transcript_path in enumerate(transcript_files, 1):
        print(f"\n[{i}/{len(transcript_files)}] Processing: {transcript_path.name}")
        
        # Load transcript to get title
        try:
            with open(transcript_path, 'r', encoding='utf-8') as f:
                transcript_data_temp = json.load(f)
            
            title = transcript_data_temp.get('meta', {}).get('title', '')
            if not title:
                print(f"  Warning: No title in transcript metadata")
                continue
            
            # Find audio file by title (search for files containing the title)
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
                
        except Exception as e:
            print(f"  Error reading transcript: {e}")
            continue
        
        try:
            # Use already loaded transcript data
            transcript_data = transcript_data_temp
            
            # Check if already has word-level timestamps
            segments = transcript_data.get('segments', [])
            if segments and 'words' in segments[0]:
                print(f"  Already has word-level timestamps, skipping")
                continue
            
            # Perform forced alignment
            aligned_transcript = align_transcript_with_audio(
                str(audio_path),
                transcript_data,
                device=device,
                compute_type=compute_type
            )
            
            # Save updated transcript
            if output_path:
                output_dir = Path(output_path) / "transcripts"
                output_dir.mkdir(parents=True, exist_ok=True)
                output_file = output_dir / transcript_path.name
            else:
                output_file = transcript_path
            
            with open(output_file, 'w', encoding='utf-8') as f:
                json.dump(aligned_transcript, f, ensure_ascii=False, indent=2)
            
            print(f"  ✓ Added word-level timestamps to {len(segments)} segments")
            
        except Exception as e:
            print(f"  Error processing {transcript_path.name}: {e}")
            continue

def main():
    parser = argparse.ArgumentParser(
        description="Add word-level timestamps to podcast transcripts using forced alignment"
    )
    parser.add_argument(
        "dataset",
        help="Path to dataset directory (should contain audio/ and transcripts/ subdirs)"
    )
    parser.add_argument(
        "--output",
        help="Output directory (default: overwrite original transcripts)"
    )
    parser.add_argument(
        "--max-episodes",
        type=int,
        help="Maximum number of episodes to process (for testing)"
    )
    
    args = parser.parse_args()
    
    # Check if whisperx is installed
    try:
        import whisperx
    except ImportError:
        print("Error: whisperx not installed")
        print("Install with: pip install whisperx")
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
