#!/usr/bin/env python3
"""
Run MFA alignment on 10 cleaned podcast episodes (1-10).
Uses V3-cleaned audio from /Volumes/eHDD/moshi-rag-data/datasets/podcast_clean/
Based on the working add_word_timestamps_mfa_simple.py approach.
"""

import os
import sys
import json
import subprocess
import tempfile
import librosa
import soundfile as sf
from pathlib import Path

# Unbuffered output
sys.stdout = os.fdopen(sys.stdout.fileno(), 'w', buffering=1)

# Paths
CLEANED_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/podcast_clean"
TRANSCRIPT_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/transcripts"
OUTPUT_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/podcast_mfa_aligned"

# MFA models
MFA_DICT = "german_mfa"
MFA_MODEL = "german_mfa"


def parse_textgrid(textgrid_path):
    """Parse MFA TextGrid output to extract word timings."""
    words = []
    try:
        with open(textgrid_path, 'r', encoding='utf-8') as f:
            lines = f.readlines()
        
        in_words_tier = False
        i = 0
        while i < len(lines):
            line = lines[i].strip()
            
            if 'name = "words"' in line:
                in_words_tier = True
            elif in_words_tier and line.startswith('xmin ='):
                start = float(line.split('=')[1].strip())
                i += 1
                end = float(lines[i].strip().split('=')[1].strip())
                i += 1
                text = lines[i].strip().split('=')[1].strip().strip('"')
                if text and text != '':
                    words.append({
                        'word': text,
                        'start': start,
                        'end': end
                    })
            i += 1
        
        return words
    except Exception as e:
        print(f"    Error parsing TextGrid: {e}", flush=True)
        return []


def process_episode(episode_num):
    """Process a single episode with MFA alignment."""
    print(f"\n{'='*70}", flush=True)
    print(f"Processing Episode {episode_num:03d}", flush=True)
    print(f"{'='*70}", flush=True)
    
    # Find cleaned audio file
    audio_pattern = f"episode_{episode_num:03d}_*_cleaned.wav"
    audio_files = list(Path(CLEANED_DIR).glob(audio_pattern))
    
    if not audio_files:
        print(f"  ERROR: No cleaned audio found for episode {episode_num}", flush=True)
        return False
    
    audio_path = str(audio_files[0])
    print(f"  Audio: {os.path.basename(audio_path)}", flush=True)
    
    # Find transcript
    transcript_files = list(Path(TRANSCRIPT_DIR).glob(f"episode_{episode_num:03d}_*.json"))
    if not transcript_files:
        print(f"  ERROR: No transcript found for episode {episode_num}", flush=True)
        return False
    
    transcript_path = str(transcript_files[0])
    print(f"  Transcript: {os.path.basename(transcript_path)}", flush=True)
    
    # Load transcript
    with open(transcript_path, 'r', encoding='utf-8') as f:
        transcript = json.load(f)
    
    segments = transcript.get('segments', [])
    full_text = ' '.join([seg.get('text', '').strip() for seg in segments if seg.get('text', '').strip()])
    
    print(f"  Segments: {len(segments)}", flush=True)
    print(f"  Text: {len(full_text)} chars, {len(full_text.split())} words", flush=True)
    
    # Use temporary directory for MFA (this creates absolute paths in /tmp)
    with tempfile.TemporaryDirectory() as temp_dir:
        # Create corpus directory
        corpus_dir = os.path.join(temp_dir, "corpus")
        os.makedirs(corpus_dir, exist_ok=True)
        
        # Convert audio to 16kHz mono for MFA
        print(f"  Converting audio to 16kHz mono...", flush=True)
        audio, sr = librosa.load(audio_path, sr=16000, mono=True)
        wav_path = os.path.join(corpus_dir, "podcast.wav")
        sf.write(wav_path, audio, 16000)
        print(f"  Audio duration: {len(audio)/16000:.1f}s", flush=True)
        
        # Create text file
        txt_path = os.path.join(corpus_dir, "podcast.txt")
        with open(txt_path, 'w', encoding='utf-8') as f:
            f.write(full_text)
        
        # Run MFA alignment
        print(f"  Running MFA alignment (may take 5-10 minutes)...", flush=True)
        
        output_dir = os.path.join(temp_dir, "output")
        os.makedirs(output_dir, exist_ok=True)
        
        mfa_cmd = [
            "mfa", "align",
            corpus_dir,
            MFA_DICT,
            MFA_MODEL,
            output_dir,
            "--clean",
            "--overwrite",
            "--beam", "100",
            "--retry_beam", "400"
        ]
        
        print(f"  Command: mfa align {corpus_dir} {MFA_DICT} {MFA_MODEL} {output_dir} ...", flush=True)
        
        try:
            result = subprocess.run(mfa_cmd, capture_output=True, text=True, timeout=1800)
            
            if result.returncode != 0:
                print(f"  ERROR: MFA failed with return code {result.returncode}", flush=True)
                print(f"  stderr: {result.stderr[:500]}", flush=True)
                return False
            
            print(f"  MFA alignment complete", flush=True)
            
            # Check for TextGrid output
            textgrid_path = os.path.join(output_dir, "podcast.TextGrid")
            if not os.path.exists(textgrid_path):
                print(f"  ERROR: TextGrid not found", flush=True)
                return False
            
            # Parse TextGrid
            words = parse_textgrid(textgrid_path)
            print(f"  Words aligned: {len(words)}", flush=True)
            
            if not words:
                print(f"  ERROR: No words extracted from TextGrid", flush=True)
                return False
            
            # Save results
            os.makedirs(OUTPUT_DIR, exist_ok=True)
            output_path = os.path.join(OUTPUT_DIR, f"episode_{episode_num:03d}_mfa.json")
            
            result_data = {
                'episode': episode_num,
                'audio_path': audio_path,
                'transcript_path': transcript_path,
                'word_count': len(words),
                'segment_count': len(segments),
                'words': words
            }
            
            with open(output_path, 'w', encoding='utf-8') as f:
                json.dump(result_data, f, indent=2, ensure_ascii=False)
            
            print(f"  SUCCESS: Saved to {os.path.basename(output_path)}", flush=True)
            return True
            
        except subprocess.TimeoutExpired:
            print(f"  ERROR: MFA timed out (>30 minutes)", flush=True)
            return False
        except Exception as e:
            print(f"  ERROR: {e}", flush=True)
            return False


def main():
    """Main entry point."""
    print("="*70, flush=True)
    print("MFA Alignment for 10 Cleaned Episodes", flush=True)
    print("="*70, flush=True)
    print(f"Input: {CLEANED_DIR}", flush=True)
    print(f"Output: {OUTPUT_DIR}", flush=True)
    print(flush=True)
    
    # Check MFA installation
    try:
        result = subprocess.run(["mfa", "version"], capture_output=True, text=True)
        print(f"MFA version: {result.stdout.strip()}", flush=True)
    except Exception as e:
        print(f"ERROR: MFA not installed: {e}", flush=True)
        return 1
    
    # Process episodes 1-10
    results = []
    for episode_num in range(1, 11):
        success = process_episode(episode_num)
        results.append((episode_num, success))
    
    # Summary
    print("\n" + "="*70, flush=True)
    print("Summary", flush=True)
    print("="*70, flush=True)
    successful = sum(1 for _, success in results if success)
    print(f"Successful: {successful}/10", flush=True)
    print(f"Failed: {10 - successful}/10", flush=True)
    
    if successful > 0:
        print(f"\nMFA output files saved to: {OUTPUT_DIR}", flush=True)
    
    return 0 if successful == 10 else 1


if __name__ == "__main__":
    sys.exit(main())

# Made with Bob