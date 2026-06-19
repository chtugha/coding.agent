#!/usr/bin/env python3
"""
Test MLX Whisper on first 5 minutes of cleaned episode 001.
Compare with original transcript to validate accuracy and measure speed.
"""

import os
import sys
import json
import time
import librosa
import soundfile as sf
import mlx_whisper
from pathlib import Path
from difflib import SequenceMatcher

# Unbuffered output
sys.stdout = os.fdopen(sys.stdout.fileno(), 'w', buffering=1)

# Paths
CLEANED_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/podcast_clean"
TRANSCRIPT_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/transcripts"
OUTPUT_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/mlx_whisper_test"

# Create output directory
os.makedirs(OUTPUT_DIR, exist_ok=True)

def extract_first_5_minutes(audio_path, output_path):
    """Extract first 5 minutes from audio file."""
    print(f"  Extracting first 5 minutes...", flush=True)
    audio, sr = librosa.load(audio_path, sr=16000, mono=True, offset=0, duration=300)
    sf.write(output_path, audio, int(sr))
    print(f"  Saved to: {output_path}", flush=True)
    return output_path, len(audio)/sr

def get_original_transcript_text(transcript_path, start_sec, end_sec):
    """Extract text from original transcript for given time range."""
    with open(transcript_path, 'r', encoding='utf-8') as f:
        transcript = json.load(f)
    
    segments = transcript.get('segments', [])
    text_parts = []
    
    for seg in segments:
        seg_start = seg.get('start', 0)
        seg_end = seg.get('end', 0)
        
        # Check if segment overlaps with our time range
        if seg_end >= start_sec and seg_start <= end_sec:
            text_parts.append(seg.get('text', '').strip())
    
    return ' '.join(text_parts)

def calculate_similarity(text1, text2):
    """Calculate similarity ratio between two texts."""
    return SequenceMatcher(None, text1.lower(), text2.lower()).ratio()

def main():
    """Main entry point."""
    print("="*70, flush=True)
    print("MLX Whisper Test on Episode 001 (First 5 Minutes)", flush=True)
    print("="*70, flush=True)
    print(flush=True)
    
    # Find cleaned episode 001
    audio_pattern = "episode_001_cleaned.wav"
    audio_files = list(Path(CLEANED_DIR).glob(audio_pattern))
    
    if not audio_files:
        print("ERROR: Episode 001 cleaned audio not found", flush=True)
        return 1
    
    audio_path = str(audio_files[0])
    print(f"Audio file: {os.path.basename(audio_path)}", flush=True)
    
    # Find transcript
    transcript_files = list(Path(TRANSCRIPT_DIR).glob("episode_001_*.json"))
    if not transcript_files:
        print("ERROR: Episode 001 transcript not found", flush=True)
        return 1
    
    transcript_path = str(transcript_files[0])
    print(f"Transcript: {os.path.basename(transcript_path)}", flush=True)
    print(flush=True)
    
    # Extract first 5 minutes
    segment_path = os.path.join(OUTPUT_DIR, "episode_001_first_5min.wav")
    segment_path, duration = extract_first_5_minutes(audio_path, segment_path)
    print(f"  Duration: {duration:.1f}s", flush=True)
    print(flush=True)
    
    # Get original transcript text
    print("Loading original transcript...", flush=True)
    original_text = get_original_transcript_text(transcript_path, 0, 300)
    print(f"  Original text length: {len(original_text)} chars", flush=True)
    print(f"  Original text preview: {original_text[:200]}...", flush=True)
    print(flush=True)
    
    # Transcribe with MLX Whisper
    print("Transcribing with MLX Whisper...", flush=True)
    print("  Loading model...", flush=True)
    
    start_time = time.time()
    result = mlx_whisper.transcribe(
        segment_path,
        path_or_hf_repo="mlx-community/whisper-large-v3-mlx",
        language="de",
        verbose=False
    )
    transcribe_time = time.time() - start_time
    
    print(f"  Transcription complete in {transcribe_time:.2f}s", flush=True)
    print(f"  Speedup: {duration/transcribe_time:.1f}x realtime", flush=True)
    print(flush=True)
    
    # Extract text from MLX result
    mlx_text = result.get('text', '').strip()
    print(f"MLX Whisper result:", flush=True)
    print(f"  Text length: {len(mlx_text)} chars", flush=True)
    print(f"  Text preview: {mlx_text[:200]}...", flush=True)
    print(flush=True)
    
    # Calculate similarity
    similarity = calculate_similarity(original_text, mlx_text)
    print(f"Comparison:", flush=True)
    print(f"  Similarity: {similarity*100:.1f}%", flush=True)
    print(flush=True)
    
    # Save results
    results = {
        'episode': 1,
        'segment': 'first_5min',
        'duration': duration,
        'transcribe_time': transcribe_time,
        'speedup': duration/transcribe_time,
        'original_text': original_text,
        'mlx_text': mlx_text,
        'similarity': similarity,
        'mlx_result': result
    }
    
    output_path = os.path.join(OUTPUT_DIR, "episode_001_first_5min_results.json")
    with open(output_path, 'w', encoding='utf-8') as f:
        json.dump(results, f, indent=2, ensure_ascii=False)
    
    print(f"Results saved to: {output_path}", flush=True)
    print(flush=True)
    
    # Summary
    print("="*70, flush=True)
    print("Summary", flush=True)
    print("="*70, flush=True)
    print(f"Duration: {duration:.1f}s ({duration/60:.1f} minutes)", flush=True)
    print(f"Transcription time: {transcribe_time:.2f}s", flush=True)
    print(f"Speedup: {duration/transcribe_time:.1f}x realtime", flush=True)
    print(f"Similarity: {similarity*100:.1f}%", flush=True)
    print(flush=True)
    
    # Estimate for full episode
    full_duration = 90 * 60  # 90 minutes
    estimated_time = full_duration / (duration/transcribe_time)
    print(f"Estimated time for 90-minute episode: {estimated_time/60:.1f} minutes", flush=True)
    print(f"Estimated time for 373 episodes: {(estimated_time * 373)/3600/24:.1f} days", flush=True)
    
    return 0

if __name__ == "__main__":
    sys.exit(main())

# Made with Bob