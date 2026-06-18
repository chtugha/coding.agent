#!/usr/bin/env python3
"""
Add word-level timestamps to podcast transcripts using Montreal Forced Aligner (MFA).
This version properly handles:
1. Ad removal using existing clean_podcast_ads_waveform_based()
2. Speaker separation (align each speaker turn separately)
3. Merging results back into original transcript structure
"""

import os
import sys
import json
import subprocess
import tempfile
import shutil
import librosa
import soundfile as sf
import numpy as np
from pathlib import Path

# Import ad removal from prepare_german_dataset
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from prepare_german_dataset import clean_podcast_ads_waveform_based

# Paths
DATASET_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast"
TRANSCRIPT_DIR = os.path.join(DATASET_DIR, "Gemischtes.Hack.Podcast.Transcript/transcripts")
OUTPUT_DIR = os.path.join(DATASET_DIR, "transcripts_aligned_proper")
MFA_MODEL = "german_mfa"
MFA_DICT = "german_mfa"

def ensure_mfa_installed():
    """Check if MFA is installed and models are downloaded."""
    try:
        result = subprocess.run(["mfa", "version"], capture_output=True, text=True)
        print(f"MFA version: {result.stdout.strip()}")
        
        # Check if models exist
        model_check = subprocess.run(
            ["mfa", "model", "inspect", "acoustic", MFA_MODEL],
            capture_output=True,
            text=True
        )
        if model_check.returncode != 0:
            print(f"Downloading acoustic model: {MFA_MODEL}")
            subprocess.run(["mfa", "model", "download", "acoustic", MFA_MODEL], check=True)
        
        dict_check = subprocess.run(
            ["mfa", "model", "inspect", "dictionary", MFA_DICT],
            capture_output=True,
            text=True
        )
        if dict_check.returncode != 0:
            print(f"Downloading dictionary: {MFA_DICT}")
            subprocess.run(["mfa", "model", "download", "dictionary", MFA_DICT], check=True)
            
        return True
    except Exception as e:
        print(f"Error checking MFA installation: {e}")
        return False


def load_transcript(json_path):
    """Load transcript JSON."""
    with open(json_path, 'r', encoding='utf-8') as f:
        return json.load(f)


def extract_speaker_turns(segments):
    """
    Extract speaker turns from segments.
    Returns list of (speaker, start, end, text, segment_ids)
    """
    turns = []
    current_speaker = None
    current_start = None
    current_end = None
    current_text = []
    current_seg_ids = []
    
    for seg in segments:
        speaker = seg.get('speaker', '')
        start = float(seg['start'])
        end = float(seg['end'])
        text = seg.get('text', '').strip()
        seg_id = seg.get('id', 0)
        
        if not text or not speaker:
            continue
            
        if speaker != current_speaker:
            # Save previous turn
            if current_speaker and current_text and current_end is not None:
                turns.append({
                    'speaker': current_speaker,
                    'start': current_start,
                    'end': current_end,
                    'text': ' '.join(current_text),
                    'segment_ids': current_seg_ids
                })
            
            # Start new turn
            current_speaker = speaker
            current_start = start
            current_end = end
            current_text = [text]
            current_seg_ids = [seg_id]
        else:
            # Continue current turn
            current_text.append(text)
            current_seg_ids.append(seg_id)
            current_end = end
    
    # Save last turn
    if current_speaker and current_text and current_end is not None:
        turns.append({
            'speaker': current_speaker,
            'start': current_start,
            'end': current_end,
            'text': ' '.join(current_text),
            'segment_ids': current_seg_ids
        })
    
    return turns


def align_speaker_turn(audio_segment, text, temp_dir, turn_idx):
    """
    Align a single speaker turn using MFA.
    Returns list of (word, start, end) tuples with times relative to segment start.
    """
    # Create corpus directory structure for MFA
    corpus_dir = os.path.join(temp_dir, "corpus")
    os.makedirs(corpus_dir, exist_ok=True)
    
    # Save audio as 16kHz mono WAV (MFA requirement)
    audio_16k = librosa.resample(audio_segment, orig_sr=48000, target_sr=16000)
    wav_path = os.path.join(corpus_dir, f"turn_{turn_idx}.wav")
    sf.write(wav_path, audio_16k, 16000)
    
    # Save text file
    txt_path = os.path.join(corpus_dir, f"turn_{turn_idx}.txt")
    with open(txt_path, 'w', encoding='utf-8') as f:
        f.write(text)
    
    # Run MFA alignment
    output_dir = os.path.join(temp_dir, "output")
    os.makedirs(output_dir, exist_ok=True)
    
    cmd = [
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
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        if result.returncode != 0:
            print(f"    MFA alignment failed for turn {turn_idx}")
            print(f"    stderr: {result.stderr}")
            return []
        
        # Parse TextGrid output
        textgrid_path = os.path.join(output_dir, f"turn_{turn_idx}.TextGrid")
        if not os.path.exists(textgrid_path):
            print(f"    No TextGrid output for turn {turn_idx}")
            return []
        
        words = parse_textgrid(textgrid_path)
        return words
        
    except subprocess.TimeoutExpired:
        print(f"    MFA alignment timed out for turn {turn_idx}")
        return []
    except Exception as e:
        print(f"    Error aligning turn {turn_idx}: {e}")
        return []


def parse_textgrid(textgrid_path):
    """Parse MFA TextGrid output to extract word timings."""
    try:
        import textgrid
        tg = textgrid.TextGrid.fromFile(textgrid_path)
        
        words = []
        for tier in tg:
            if tier.name == "words":
                for interval in tier:
                    if interval.mark.strip():
                        words.append({
                            'word': interval.mark.strip(),
                            'start': float(interval.minTime),
                            'end': float(interval.maxTime)
                        })
        return words
    except Exception as e:
        print(f"    Error parsing TextGrid: {e}")
        return []


def merge_word_timestamps_into_segments(segments, turns_with_words, offset):
    """
    Merge word-level timestamps back into original segment structure.
    
    Args:
        segments: Original segments from transcript
        turns_with_words: List of turns with word timestamps
        offset: Time offset from ad removal (to adjust all timestamps)
    """
    # Create mapping from segment_id to turn
    seg_to_turn = {}
    for turn in turns_with_words:
        for seg_id in turn['segment_ids']:
            seg_to_turn[seg_id] = turn
    
    # Update segments with word timestamps
    for seg in segments:
        seg_id = seg.get('id', 0)
        if seg_id not in seg_to_turn:
            continue
        
        turn = seg_to_turn[seg_id]
        if 'words' not in turn or not turn['words']:
            continue
        
        # Find words that belong to this segment
        seg_start = float(seg['start'])
        seg_end = float(seg['end'])
        seg_text = seg.get('text', '').strip()
        
        # Get words for this segment from the turn
        # Words are relative to turn start, need to adjust to absolute time
        turn_start = turn['start']
        seg_words = []
        
        for word_info in turn['words']:
            # Convert word times from turn-relative to absolute
            word_abs_start = turn_start + word_info['start'] - offset
            word_abs_end = turn_start + word_info['end'] - offset
            
            # Check if word falls within this segment
            if word_abs_start >= seg_start and word_abs_end <= seg_end:
                seg_words.append({
                    'word': word_info['word'],
                    'start': round(word_abs_start, 2),
                    'end': round(word_abs_end, 2)
                })
        
        if seg_words:
            seg['words'] = seg_words
    
    return segments


def process_episode(episode_num):
    """Process a single episode."""
    print(f"\n{'='*60}")
    print(f"Processing Episode {episode_num:03d}")
    print(f"{'='*60}")
    
    # Find audio and transcript files
    audio_files = list(Path(DATASET_DIR).glob(f"#{episode_num} *.mp3"))
    if not audio_files:
        print(f"  No audio file found for episode {episode_num}")
        return False
    
    audio_path = str(audio_files[0])
    transcript_path = os.path.join(TRANSCRIPT_DIR, f"episode_{episode_num:03d}_gemischtes_hack.json")
    
    if not os.path.exists(transcript_path):
        print(f"  No transcript found: {transcript_path}")
        return False
    
    print(f"  Audio: {os.path.basename(audio_path)}")
    print(f"  Transcript: {os.path.basename(transcript_path)}")
    
    # Load transcript
    transcript = load_transcript(transcript_path)
    segments = transcript.get('segments', [])
    
    if not segments:
        print(f"  No segments in transcript")
        return False
    
    # Load and clean audio (remove ads)
    print(f"  Loading audio...")
    mono, sr = librosa.load(audio_path, sr=48000, mono=True)
    mono = mono.astype(np.float32)
    print(f"  Audio loaded: {len(mono)/sr:.1f}s at {sr}Hz")
    
    print(f"  Removing ads...")
    cleaned, offset, ad_breaks = clean_podcast_ads_waveform_based(
        mono, int(sr), transcript_path, ep_label=f"ep{episode_num:03d}"
    )
    print(f"  Cleaned: {len(cleaned)/sr:.1f}s (removed {len(mono)/sr - len(cleaned)/sr:.1f}s)")
    print(f"  Offset: {offset:.1f}s, Ad breaks: {len(ad_breaks)}")
    
    # Extract speaker turns
    print(f"  Extracting speaker turns...")
    turns = extract_speaker_turns(segments)
    print(f"  Found {len(turns)} speaker turns")
    
    # Align each turn with MFA
    print(f"  Aligning speaker turns with MFA...")
    with tempfile.TemporaryDirectory() as temp_dir:
        for i, turn in enumerate(turns):
            print(f"    Turn {i+1}/{len(turns)}: {turn['speaker']} ({turn['end']-turn['start']:.1f}s)")
            
            # Extract audio segment for this turn (accounting for offset)
            turn_start_samples = int((turn['start'] - offset) * sr)
            turn_end_samples = int((turn['end'] - offset) * sr)
            
            if turn_start_samples < 0 or turn_end_samples > len(cleaned):
                print(f"      Turn outside cleaned audio bounds, skipping")
                continue
            
            audio_segment = cleaned[turn_start_samples:turn_end_samples]
            
            # Align this turn
            words = align_speaker_turn(audio_segment, turn['text'], temp_dir, i)
            
            if words:
                turn['words'] = words
                print(f"      Aligned {len(words)} words")
            else:
                print(f"      No words aligned")
    
    # Merge word timestamps back into segments
    print(f"  Merging word timestamps into segments...")
    updated_segments = merge_word_timestamps_into_segments(segments, turns, offset)
    
    # Count segments with words
    segs_with_words = sum(1 for seg in updated_segments if 'words' in seg and seg['words'])
    print(f"  Segments with word timestamps: {segs_with_words}/{len(updated_segments)}")
    
    # Save enhanced transcript
    output_path = os.path.join(OUTPUT_DIR, f"episode_{episode_num:03d}_gemischtes_hack.json")
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    
    transcript['segments'] = updated_segments
    
    with open(output_path, 'w', encoding='utf-8') as f:
        json.dump(transcript, f, ensure_ascii=False, indent=2)
    
    print(f"  Saved: {output_path}")
    return True


def main():
    """Main entry point."""
    if not ensure_mfa_installed():
        print("ERROR: MFA not properly installed")
        return 1
    
    # Test on episode 1 first
    episode_num = 1
    
    success = process_episode(episode_num)
    
    if success:
        print(f"\n{'='*60}")
        print(f"SUCCESS: Episode {episode_num} processed")
        print(f"{'='*60}")
        return 0
    else:
        print(f"\n{'='*60}")
        print(f"FAILED: Episode {episode_num}")
        print(f"{'='*60}")
        return 1


if __name__ == "__main__":
    sys.exit(main())

# Made with Bob
