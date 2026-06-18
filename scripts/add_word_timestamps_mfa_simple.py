#!/usr/bin/env python3
"""
Add word-level timestamps to podcast transcripts using Montreal Forced Aligner (MFA).

CORRECT APPROACH:
1. Keep transcript UNCHANGED (it's correct - already without ads!)
2. Remove ads/intros from AUDIO using waveform correlation (to match transcript)
3. Force align cleaned audio with FULL transcript text using MFA
4. Distribute MFA word timestamps sequentially across segments (no adjustment needed!)

The waveform method already aligns audio to transcript by removing non-matching sections.
MFA just adds precise word-level timestamps to the already-aligned audio+transcript.
"""

import os
import sys
import json
import subprocess
import tempfile
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
OUTPUT_DIR = os.path.join(DATASET_DIR, "transcripts_aligned_final")
MFA_MODEL = "german_mfa"
MFA_DICT = "german_mfa"


def load_transcript(json_path):
    """Load transcript JSON."""
    with open(json_path, 'r', encoding='utf-8') as f:
        return json.load(f)


def extract_full_text(segments):
    """Extract full text from all segments, preserving order."""
    texts = []
    for seg in segments:
        text = seg.get('text', '').strip()
        if text:
            texts.append(text)
    return ' '.join(texts)


def run_mfa_alignment(audio_path, text, temp_dir):
    """
    Run MFA alignment on cleaned audio with full transcript text.
    Returns list of (word, start, end) tuples.
    """
    # Create corpus directory structure for MFA
    corpus_dir = os.path.join(temp_dir, "corpus")
    os.makedirs(corpus_dir, exist_ok=True)
    
    # MFA requires 16kHz mono WAV
    print(f"    Converting audio to 16kHz mono for MFA...")
    audio, sr = librosa.load(audio_path, sr=16000, mono=True)
    wav_path = os.path.join(corpus_dir, "podcast.wav")
    sf.write(wav_path, audio, 16000)
    
    # Save text file
    txt_path = os.path.join(corpus_dir, "podcast.txt")
    with open(txt_path, 'w', encoding='utf-8') as f:
        f.write(text)
    
    print(f"    Running MFA alignment...")
    print(f"    Text length: {len(text)} chars, {len(text.split())} words")
    print(f"    Audio duration: {len(audio)/16000:.1f}s")
    
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
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        if result.returncode != 0:
            print(f"    MFA alignment failed!")
            print(f"    stderr: {result.stderr}")
            return []
        
        # Parse TextGrid output
        textgrid_path = os.path.join(output_dir, "podcast.TextGrid")
        if not os.path.exists(textgrid_path):
            print(f"    No TextGrid output found")
            return []
        
        words = parse_textgrid(textgrid_path)
        print(f"    MFA aligned {len(words)} words")
        return words
        
    except subprocess.TimeoutExpired:
        print(f"    MFA alignment timed out (>10 minutes)")
        return []
    except Exception as e:
        print(f"    Error running MFA: {e}")
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
                            'word': interval.mark.strip().lower(),
                            'start': float(interval.minTime),
                            'end': float(interval.maxTime)
                        })
        return words
    except Exception as e:
        print(f"    Error parsing TextGrid: {e}")
        return []


def add_words_to_segments(segments, aligned_words):
    """
    Add MFA word-level timestamps to segments.
    
    After waveform-based ad removal, the audio and transcript are already aligned.
    MFA provides precise word-level timestamps in sequential order.
    We just need to distribute these words across segments.
    
    Args:
        segments: Original transcript segments
        aligned_words: Words with timestamps from MFA (sequential, matching transcript)
    
    Returns:
        Updated segments with word-level timestamps
    """
    word_idx = 0
    
    for seg in segments:
        seg_text = seg.get('text', '').strip()
        if not seg_text:
            continue
        
        # Count words in this segment
        seg_word_count = len(seg_text.split())
        
        # Assign next N words from MFA output
        seg_words = []
        for _ in range(seg_word_count):
            if word_idx < len(aligned_words):
                word_info = aligned_words[word_idx]
                seg_words.append({
                    'word': word_info['word'],
                    'start': round(word_info['start'], 2),
                    'end': round(word_info['end'], 2)
                })
                word_idx += 1
        
        if seg_words:
            seg['words'] = seg_words
    
    print(f"    Added {word_idx} words to {len(segments)} segments")
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
    
    # Try to find transcript file - check multiple patterns
    transcript_patterns = [
        f"episode_{episode_num:03d}_gemischtes_hack.json",
        f"episode_{episode_num:03d}_*.json"
    ]
    
    transcript_path = None
    for pattern in transcript_patterns:
        matches = list(Path(TRANSCRIPT_DIR).glob(pattern))
        if matches:
            transcript_path = str(matches[0])
            break
    
    if not transcript_path:
        print(f"  No transcript found for episode {episode_num}")
        return False
    
    print(f"  Audio: {os.path.basename(audio_path)}")
    print(f"  Transcript: {os.path.basename(transcript_path)}")
    
    # Load transcript (KEEP IT UNCHANGED!)
    transcript = load_transcript(transcript_path)
    segments = transcript.get('segments', [])
    
    if not segments:
        print(f"  No segments in transcript")
        return False
    
    # Extract full text from transcript
    full_text = extract_full_text(segments)
    print(f"  Transcript: {len(full_text)} chars, {len(full_text.split())} words")
    
    # Load and clean audio (remove ads from AUDIO only)
    print(f"  Loading audio...")
    mono, sr = librosa.load(audio_path, sr=48000, mono=True)
    mono = mono.astype(np.float32)
    print(f"  Audio loaded: {len(mono)/sr:.1f}s at {sr}Hz")
    
    print(f"  Removing ads from audio...")
    cleaned, offset, ad_breaks = clean_podcast_ads_waveform_based(
        mono, int(sr), transcript_path, ep_label=f"ep{episode_num:03d}"
    )
    print(f"  Cleaned audio: {len(cleaned)/sr:.1f}s (removed {len(mono)/sr - len(cleaned)/sr:.1f}s)")
    print(f"  Offset: {offset:.1f}s, Ad breaks: {len(ad_breaks)}")
    
    # Save cleaned audio in same folder as aligned transcript for validation
    cleaned_audio_filename = f"episode_{episode_num:03d}_gemischtes_hack_cleaned.wav"
    cleaned_audio_path = os.path.join(OUTPUT_DIR, cleaned_audio_filename)
    sf.write(cleaned_audio_path, cleaned, int(sr))
    print(f"  Saved cleaned audio: {cleaned_audio_filename}")
    
    # Run MFA alignment on cleaned audio with FULL transcript text
    with tempfile.TemporaryDirectory() as temp_dir:
        aligned_words = run_mfa_alignment(cleaned_audio_path, full_text, temp_dir)
        
        if not aligned_words:
            print(f"  MFA alignment failed")
            return False
    
    # Add word timestamps to segments
    print(f"  Adding word timestamps to segments...")
    updated_segments = add_words_to_segments(segments, aligned_words)
    
    # Count segments with words
    segs_with_words = sum(1 for seg in updated_segments if 'words' in seg and seg['words'])
    total_words = sum(len(seg.get('words', [])) for seg in updated_segments)
    print(f"  Segments with words: {segs_with_words}/{len(updated_segments)}")
    print(f"  Total words added: {total_words}")
    
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
    # Check MFA installation
    try:
        result = subprocess.run(["mfa", "version"], capture_output=True, text=True)
        print(f"MFA version: {result.stdout.strip()}")
    except Exception as e:
        print(f"ERROR: MFA not installed: {e}")
        return 1
    
    # Test on episode 150
    episode_num = 150
    
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
