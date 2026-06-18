#!/usr/bin/env python3
"""
Force align original podcast transcripts using Montreal Forced Aligner (MFA).

This script:
1. Reads original podcast transcripts (segment-level timestamps only)
2. Matches them to audio files by title
3. Uses MFA to generate word-level timestamps
4. Saves enhanced transcripts with word-level timestamps to output directory

Usage:
    python3 force_align_podcast_transcripts.py \\
        --audio-dir /path/to/audio \\
        --transcript-dir /path/to/transcripts \\
        --output-dir /path/to/output \\
        --episodes ep001,ep002,ep003
"""

import os
import sys
import json
import argparse
import subprocess
import tempfile
import shutil
import re
from pathlib import Path
from typing import List, Dict, Tuple, Optional

try:
    import soundfile as sf
    import librosa
    import numpy as np
except ImportError:
    print("ERROR: Required packages not installed. Run:")
    print("  pip install soundfile librosa numpy")
    sys.exit(1)

try:
    import textgrid
except ImportError:
    print("ERROR: textgrid package not installed. Run:")
    print("  pip install textgrid")
    sys.exit(1)


def log(msg=""):
    """Print log message with flush."""
    print(msg, flush=True)


def find_audio_for_transcript(transcript_path: Path, audio_dir: Path) -> Optional[Path]:
    """
    Find matching audio file for a transcript by comparing titles.
    
    Args:
        transcript_path: Path to transcript JSON file
        audio_dir: Directory containing audio files
        
    Returns:
        Path to matching audio file, or None if not found
    """
    try:
        with open(transcript_path, 'r', encoding='utf-8') as f:
            data = json.load(f)
        
        # Try to get title from meta section first, then root level
        title = data.get('meta', {}).get('title', '') or data.get('title', '')
        title = title.strip()
        
        if not title:
            log(f"  WARNING: No title in {transcript_path.name}")
            return None
        
        # Search for audio file with matching title
        for audio_file in audio_dir.glob("*.mp3"):
            if title.lower() in audio_file.name.lower():
                return audio_file
        
        log(f"  WARNING: No audio file found for title: {title}")
        return None
        
    except Exception as e:
        log(f"  ERROR reading {transcript_path.name}: {e}")
        return None


def prepare_mfa_input(audio_path: Path, transcript_data: dict, work_dir: Path) -> Tuple[Optional[Path], Optional[Path]]:
    """
    Prepare audio and text files for MFA alignment.
    
    MFA expects:
    - A corpus directory containing .wav and .txt files with matching basenames
    - Text files contain the transcript
    - Audio must be 16kHz mono WAV
    
    Args:
        audio_path: Path to source audio file
        transcript_data: Transcript JSON data
        work_dir: Working directory for MFA files
        
    Returns:
        Tuple of (wav_path, txt_path) for MFA input
    """
    log(f"    Preparing MFA input files...")
    
    # Create 16kHz mono WAV file (MFA requirement)
    log(f"    Loading audio from {audio_path.name}...")
    audio, sr = sf.read(str(audio_path))
    
    if audio.ndim == 2:
        log(f"    Converting stereo to mono...")
        audio = np.mean(audio, axis=1)
    
    if sr != 16000:
        log(f"    Resampling from {sr}Hz to 16000Hz...")
        audio = librosa.resample(audio, orig_sr=sr, target_sr=16000)
        sr = 16000
    
    # Use consistent basename for wav and txt
    basename = "episode"
    wav_path = work_dir / f"{basename}.wav"
    sf.write(str(wav_path), audio, sr, subtype='PCM_16')
    log(f"    Saved WAV: {wav_path} ({len(audio)/sr:.1f} seconds)")
    
    # Create text file with all transcript text
    segments = transcript_data.get('segments', [])
    text_lines = []
    for seg in segments:
        text = seg.get('text', '').strip()
        if text:
            # Remove leading/trailing whitespace and normalize spaces
            text = ' '.join(text.split())
            text_lines.append(text)
    
    full_text = ' '.join(text_lines)
    
    # Validate text
    if not full_text or len(full_text) < 10:
        log(f"    ERROR: Transcript text too short: {len(full_text)} chars")
        return None, None
    
    txt_path = work_dir / f"{basename}.txt"
    with open(txt_path, 'w', encoding='utf-8') as f:
        f.write(full_text)
    
    log(f"    Saved TXT: {txt_path} ({len(full_text)} chars, {len(text_lines)} segments)")
    
    return wav_path, txt_path


def run_mfa_alignment(wav_path: Path, txt_path: Path, output_dir: Path) -> Optional[Path]:
    """
    Run Montreal Forced Aligner on audio and text.
    
    MFA corpus format:
    - Input directory contains .wav and .txt files with matching basenames
    - Output directory will contain .TextGrid files
    
    Args:
        wav_path: Path to 16kHz mono WAV file
        txt_path: Path to text file (must have same basename as wav)
        output_dir: Directory for MFA output
        
    Returns:
        Path to generated TextGrid file, or None on failure
    """
    try:
        corpus_dir = wav_path.parent
        basename = wav_path.stem
        
        log(f"    Running MFA alignment...")
        log(f"    Corpus dir: {corpus_dir}")
        log(f"    Output dir: {output_dir}")
        
        # MFA command: align corpus directory
        # IMPORTANT: Positional args MUST come before options!
        cmd = [
            'mfa', 'align',
            str(corpus_dir),  # CORPUS_DIRECTORY (positional)
            'german_mfa',     # DICTIONARY_PATH (positional)
            'german_mfa',     # ACOUSTIC_MODEL_PATH (positional)
            str(output_dir),  # OUTPUT_DIRECTORY (positional)
            '--clean',        # Options come AFTER positional args
            '--overwrite',
            '--beam', '100',
            '--retry_beam', '400'
        ]
        
        log(f"    Command: {' '.join(cmd)}")
        
        result = subprocess.run(
            cmd,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            timeout=1200  # 20 minute timeout
        )
        
        # Log output for debugging
        if result.stdout:
            log(f"    MFA stdout: {result.stdout[:500]}")
        if result.stderr:
            log(f"    MFA stderr: {result.stderr[:500]}")
        
        if result.returncode != 0:
            log(f"    MFA returned non-zero exit code: {result.returncode}")
            return None
        
        # Find generated TextGrid file
        textgrid_path = output_dir / f"{basename}.TextGrid"
        if textgrid_path.exists():
            log(f"    SUCCESS: TextGrid generated at {textgrid_path}")
            return textgrid_path
        
        log(f"    ERROR: TextGrid file not found at {textgrid_path}")
        return None
        
    except subprocess.TimeoutExpired:
        log(f"    ERROR: MFA alignment timed out after 20 minutes")
        return None
    except Exception as e:
        log(f"    ERROR running MFA: {e}")
        import traceback
        log(f"    Traceback: {traceback.format_exc()}")
        return None


def parse_textgrid(textgrid_path: Path) -> List[Tuple[str, float, float]]:
    """
    Parse TextGrid file to extract word-level alignments.
    
    Args:
        textgrid_path: Path to TextGrid file
        
    Returns:
        List of (word, start_time, end_time) tuples
    """
    try:
        tg = textgrid.TextGrid.fromFile(str(textgrid_path))
        
        words = []
        for tier in tg:
            if tier.name.lower() == 'words':
                for interval in tier:
                    word = interval.mark.strip()
                    if word and word != '':
                        words.append((
                            word,
                            float(interval.minTime),
                            float(interval.maxTime)
                        ))
                break
        
        return words
        
    except Exception as e:
        log(f"    ERROR parsing TextGrid: {e}")
        return []


def assign_words_to_segments(
    segments: List[dict],
    aligned_words: List[Tuple[str, float, float]]
) -> List[dict]:
    """
    Assign aligned words back to original segments with speaker labels.
    
    Args:
        segments: Original transcript segments with speaker info
        aligned_words: Word alignments from MFA
        
    Returns:
        Updated segments with word-level timestamps
    """
    # Create word index
    word_idx = 0
    
    for seg in segments:
        seg_start = float(seg['start'])
        seg_end = float(seg['end'])
        seg_text = seg.get('text', '').strip()
        
        if not seg_text:
            seg['words'] = []
            continue
        
        # Find words that fall within this segment
        seg_words = []
        while word_idx < len(aligned_words):
            word, w_start, w_end = aligned_words[word_idx]
            
            # Check if word overlaps with segment
            if w_start >= seg_end:
                break  # Word is after this segment
            
            if w_end <= seg_start:
                word_idx += 1  # Word is before this segment
                continue
            
            # Word overlaps with segment - add it
            seg_words.append({
                'word': word,
                'start': w_start,
                'end': w_end
            })
            word_idx += 1
        
        seg['words'] = seg_words
    
    return segments


def process_transcript(
    transcript_path: Path,
    audio_dir: Path,
    output_dir: Path
) -> bool:
    """
    Process a single transcript file with MFA alignment.
    
    Args:
        transcript_path: Path to transcript JSON
        audio_dir: Directory containing audio files
        output_dir: Directory for output files
        
    Returns:
        True if successful, False otherwise
    """
    log(f"\nProcessing: {transcript_path.name}")
    
    # Find matching audio file
    audio_path = find_audio_for_transcript(transcript_path, audio_dir)
    if not audio_path:
        return False
    
    log(f"  Audio: {audio_path.name}")
    
    # Load transcript
    try:
        with open(transcript_path, 'r', encoding='utf-8') as f:
            transcript_data = json.load(f)
    except Exception as e:
        log(f"  ERROR loading transcript: {e}")
        return False
    
    # Check if already has word-level timestamps
    segments = transcript_data.get('segments', [])
    if segments and segments[0].get('words'):
        log(f"  SKIP: Already has word-level timestamps")
        return True
    
    # Create temporary working directory
    with tempfile.TemporaryDirectory() as tmpdir:
        work_dir = Path(tmpdir)
        
        # Prepare MFA input files
        try:
            wav_path, txt_path = prepare_mfa_input(audio_path, transcript_data, work_dir)
            
            # Check if preparation was successful
            if not wav_path or not txt_path:
                log(f"  ERROR: Failed to prepare MFA input files")
                return False
                
        except Exception as e:
            log(f"  ERROR preparing MFA input: {e}")
            import traceback
            log(f"  Traceback: {traceback.format_exc()}")
            return False
        
        # Run MFA alignment
        mfa_output_dir = work_dir / "output"
        mfa_output_dir.mkdir(exist_ok=True)
        
        textgrid_path = run_mfa_alignment(wav_path, txt_path, mfa_output_dir)
        if not textgrid_path:
            log(f"  ERROR: MFA alignment failed")
            return False
        
        # Parse TextGrid
        textgrid_path = run_mfa_alignment(wav_path, txt_path, mfa_output_dir)
        if not textgrid_path:
            log(f"  ERROR: MFA alignment failed")
            return False
        
        aligned_words = parse_textgrid(textgrid_path)
        if not aligned_words:
            log(f"  ERROR: No words extracted from TextGrid")
            return False
        
        log(f"  Aligned {len(aligned_words)} words")
        
        # Assign words to segments
        updated_segments = assign_words_to_segments(segments, aligned_words)
        transcript_data['segments'] = updated_segments
    
    # Save enhanced transcript
    output_path = output_dir / transcript_path.name
    
    # Create backup if file exists
    if output_path.exists():
        backup_path = output_path.with_suffix('.json.bak')
        shutil.copy2(output_path, backup_path)
        log(f"  Backup created: {backup_path.name}")
    
    try:
        with open(output_path, 'w', encoding='utf-8') as f:
            json.dump(transcript_data, f, ensure_ascii=False, indent=2)
        log(f"  ✓ Saved: {output_path.name}")
        return True
    except Exception as e:
        log(f"  ERROR saving output: {e}")
        return False


def main():
    parser = argparse.ArgumentParser(
        description='Force align podcast transcripts using Montreal Forced Aligner'
    )
    parser.add_argument(
        '--audio-dir',
        required=True,
        help='Directory containing podcast audio files (.mp3)'
    )
    parser.add_argument(
        '--transcript-dir',
        required=True,
        help='Directory containing transcript JSON files'
    )
    parser.add_argument(
        '--output-dir',
        required=True,
        help='Directory for output transcripts with word-level timestamps'
    )
    parser.add_argument(
        '--episodes',
        help='Comma-separated list of episode IDs to process (e.g., "001,002,003")'
    )
    
    args = parser.parse_args()
    
    # Validate directories
    audio_dir = Path(args.audio_dir)
    transcript_dir = Path(args.transcript_dir)
    output_dir = Path(args.output_dir)
    
    if not audio_dir.exists():
        log(f"ERROR: Audio directory not found: {audio_dir}")
        sys.exit(1)
    
    if not transcript_dir.exists():
        log(f"ERROR: Transcript directory not found: {transcript_dir}")
        sys.exit(1)
    
    # Create output directory
    output_dir.mkdir(parents=True, exist_ok=True)
    
    log("=" * 80)
    log("PODCAST TRANSCRIPT FORCED ALIGNMENT (Montreal Forced Aligner)")
    log("=" * 80)
    log(f"Audio directory: {audio_dir}")
    log(f"Transcript directory: {transcript_dir}")
    log(f"Output directory: {output_dir}")
    
    # Get list of transcripts to process
    transcript_files = []
    
    if args.episodes:
        # Process specific episodes
        episode_ids = [ep.strip() for ep in args.episodes.split(',')]
        log(f"Processing episodes: {', '.join(episode_ids)}")
        
        for ep_id in episode_ids:
            # Find transcript files matching episode ID
            pattern = f"*{ep_id}*.json"
            matches = list(transcript_dir.glob(pattern))
            
            # Exclude .chunks.json files
            matches = [f for f in matches if not f.name.endswith('.chunks.json')]
            
            if not matches:
                log(f"WARNING: No transcript found for episode {ep_id}")
            else:
                transcript_files.extend(matches)
    else:
        # Process all transcripts
        transcript_files = list(transcript_dir.glob("*.json"))
        # Exclude .chunks.json files
        transcript_files = [f for f in transcript_files if not f.name.endswith('.chunks.json')]
    
    if not transcript_files:
        log("ERROR: No transcript files found to process")
        sys.exit(1)
    
    log(f"\nFound {len(transcript_files)} transcript files to process")
    
    # Process each transcript
    stats = {
        'total': len(transcript_files),
        'success': 0,
        'failed': 0,
        'skipped': 0
    }
    
    for i, transcript_path in enumerate(transcript_files, 1):
        log(f"\n[{i}/{len(transcript_files)}]")
        
        success = process_transcript(transcript_path, audio_dir, output_dir)
        
        if success:
            stats['success'] += 1
        else:
            stats['failed'] += 1
    
    # Print summary
    log("\n" + "=" * 80)
    log("SUMMARY")
    log("=" * 80)
    log(f"Total files: {stats['total']}")
    log(f"Successful: {stats['success']}")
    log(f"Failed: {stats['failed']}")
    log(f"Skipped: {stats['skipped']}")
    log(f"\nOutput directory: {output_dir}")
    log("=" * 80)


if __name__ == '__main__':
    main()

# Made with Bob
