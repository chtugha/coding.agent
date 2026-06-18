#!/usr/bin/env python3
"""
Add precise word-level timestamps using Montreal Forced Aligner (MFA).

MFA is the industry standard for forced alignment and handles all segment lengths.
"""

import json
import os
import sys
from pathlib import Path
import argparse
import subprocess
import tempfile
import shutil

def prepare_mfa_input(transcript_data, audio_path, temp_dir):
    """
    Prepare input files for MFA.
    
    MFA expects:
    - Audio file: temp_dir/audio.wav
    - Text file: temp_dir/audio.txt (plain text transcript)
    """
    temp_dir = Path(temp_dir)
    
    # Copy audio file
    audio_dest = temp_dir / "audio.wav"
    
    # Convert MP3 to WAV if needed
    if audio_path.suffix.lower() == '.mp3':
        print(f"  Converting MP3 to WAV...")
        subprocess.run([
            'ffmpeg', '-i', str(audio_path),
            '-ar', '16000',  # MFA works best with 16kHz
            '-ac', '1',       # Mono
            '-y',             # Overwrite
            str(audio_dest)
        ], check=True, capture_output=True)
    else:
        shutil.copy2(audio_path, audio_dest)
    
    # Create text file with full transcript
    segments = transcript_data.get('segments', [])
    full_text = ' '.join(seg['text'].strip() for seg in segments if seg['text'].strip())
    
    text_file = temp_dir / "audio.txt"
    with open(text_file, 'w', encoding='utf-8') as f:
        f.write(full_text)
    
    return audio_dest, text_file

def run_mfa_alignment(audio_file, text_file, output_dir, language="german"):
    """
    Run MFA alignment.
    
    Returns path to TextGrid file with word alignments.
    """
    print(f"  Running MFA alignment...")
    
    # MFA command
    cmd = [
        'mfa', 'align',
        '--clean',
        '--single_speaker',
        str(audio_file.parent),  # Input directory
        language,                 # Language/acoustic model
        language,                 # Dictionary
        str(output_dir)          # Output directory
    ]
    
    try:
        result = subprocess.run(
            cmd,
            check=True,
            capture_output=True,
            text=True
        )
        print(f"  MFA output: {result.stdout}")
    except subprocess.CalledProcessError as e:
        print(f"  MFA error: {e.stderr}")
        raise
    
    # Find TextGrid file
    textgrid_file = output_dir / "audio.TextGrid"
    if not textgrid_file.exists():
        raise FileNotFoundError(f"MFA did not produce TextGrid file: {textgrid_file}")
    
    return textgrid_file

def parse_textgrid(textgrid_file):
    """
    Parse TextGrid file to extract word-level timestamps.
    
    Returns list of (word, start, end) tuples.
    """
    import textgrid
    
    tg = textgrid.TextGrid.fromFile(str(textgrid_file))
    
    words = []
    for tier in tg:
        if tier.name == 'words':
            for interval in tier:
                if interval.mark.strip():  # Skip empty intervals
                    words.append({
                        'word': interval.mark,
                        'start': interval.minTime,
                        'end': interval.maxTime
                    })
    
    return words

def assign_words_to_segments(words, segments):
    """
    Assign word timestamps to original segments.
    
    Uses text matching to distribute words across segments.
    """
    # Create a word queue
    word_queue = list(words)
    word_idx = 0
    
    for seg in segments:
        seg_text = seg['text'].strip()
        seg_words = seg_text.split()
        
        # Assign words to this segment
        seg['words'] = []
        
        for expected_word in seg_words:
            if word_idx < len(word_queue):
                # Take next word from queue
                word_data = word_queue[word_idx]
                seg['words'].append(word_data)
                word_idx += 1
        
        # If no words assigned, create placeholder
        if not seg['words'] and seg_text:
            # Use segment timestamps as fallback
            seg['words'] = [{
                'word': seg_text,
                'start': seg['start'],
                'end': seg['end']
            }]
    
    return segments

def process_dataset(dataset_path, output_path=None, max_episodes=None):
    """
    Process all episodes in a dataset, adding word-level timestamps with MFA.
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
        
        temp_dir = None
        try:
            # Load transcript
            with open(transcript_path, 'r', encoding='utf-8') as f:
                transcript_data = json.load(f)
            
            title = transcript_data.get('meta', {}).get('title', '')
            if not title:
                print(f"  Warning: No title in transcript metadata")
                continue
            
            segments = transcript_data.get('segments', [])
            if not segments:
                print(f"  Warning: No segments in transcript")
                continue
            
            # Check if already has word-level timestamps
            if segments and 'words' in segments[0] and segments[0]['words']:
                print(f"  Already has word-level timestamps, skipping")
                continue
            
            # Find audio file
            audio_path = None
            for audio_file in audio_dir.glob("*.mp3"):
                if title.lower() in audio_file.name.lower():
                    audio_path = audio_file
                    break
            
            if not audio_path:
                for audio_file in audio_dir.glob("*.wav"):
                    if title.lower() in audio_file.name.lower():
                        audio_path = audio_file
                        break
            
            if not audio_path:
                print(f"  Warning: Audio file not found for title: {title}")
                continue
            
            print(f"  Audio: {audio_path.name}")
            
            # Create temporary directory for MFA
            temp_dir = Path(tempfile.mkdtemp(prefix='mfa_'))
            output_dir = temp_dir / "output"
            output_dir.mkdir()
            
            # Prepare MFA input
            audio_file, text_file = prepare_mfa_input(
                transcript_data, audio_path, temp_dir
            )
            
            # Run MFA alignment
            textgrid_file = run_mfa_alignment(
                audio_file, text_file, output_dir
            )
            
            # Parse TextGrid
            words = parse_textgrid(textgrid_file)
            print(f"  Got {len(words)} word timestamps from MFA")
            
            # Assign words to segments
            segments = assign_words_to_segments(words, segments)
            transcript_data['segments'] = segments
            
            # Save updated transcript
            if output_path:
                output_dir_final = Path(output_path) / "transcripts"
                output_dir_final.mkdir(parents=True, exist_ok=True)
                output_file = output_dir_final / transcript_path.name
            else:
                # Create backup
                backup_file = transcript_path.with_suffix('.json.backup')
                if not backup_file.exists():
                    shutil.copy2(transcript_path, backup_file)
                    print(f"  Created backup: {backup_file.name}")
                
                output_file = transcript_path
            
            with open(output_file, 'w', encoding='utf-8') as f:
                json.dump(transcript_data, f, ensure_ascii=False, indent=2)
            
            total_words = sum(len(seg.get('words', [])) for seg in segments)
            print(f"  ✓ Added {total_words} word timestamps")
            print(f"  ✓ Saved to: {output_file}")
            
        except Exception as e:
            print(f"  Error processing {transcript_path.name}: {e}")
            import traceback
            traceback.print_exc()
            continue
        
        finally:
            # Cleanup temp directory
            if temp_dir and temp_dir.exists():
                shutil.rmtree(temp_dir)

def main():
    parser = argparse.ArgumentParser(
        description="Add precise word-level timestamps using Montreal Forced Aligner"
    )
    parser.add_argument(
        "dataset",
        help="Path to dataset directory"
    )
    parser.add_argument(
        "--output",
        help="Output directory (default: overwrite with backup)"
    )
    parser.add_argument(
        "--max-episodes",
        type=int,
        help="Maximum number of episodes to process"
    )
    
    args = parser.parse_args()
    
    # Check dependencies
    try:
        subprocess.run(['mfa', 'version'], check=True, capture_output=True)
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("Error: Montreal Forced Aligner (MFA) not installed")
        print("Install with: conda install -c conda-forge montreal-forced-aligner")
        sys.exit(1)
    
    try:
        import textgrid
    except ImportError:
        print("Error: textgrid library not installed")
        print("Install with: pip install textgrid")
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
