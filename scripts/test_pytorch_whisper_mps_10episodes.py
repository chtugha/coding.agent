#!/usr/bin/env python3
"""
Test native PyTorch Whisper with MPS backend on first and last 5 minutes of 10 cleaned episodes.
Compare transcripts with original transcripts to validate accuracy.
"""

import os
import sys
import json
import time
import librosa
import soundfile as sf
import whisper
import torch
from pathlib import Path
from difflib import SequenceMatcher

# Unbuffered output
sys.stdout = os.fdopen(sys.stdout.fileno(), 'w', buffering=1)

# Paths
CLEANED_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/podcast_clean"
TRANSCRIPT_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/transcripts"
OUTPUT_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/pytorch_whisper_mps_test"

# Create output directory
os.makedirs(OUTPUT_DIR, exist_ok=True)

def extract_audio_segment(audio_path, start_sec, duration_sec):
    """Extract a segment from audio file."""
    audio, sr = librosa.load(audio_path, sr=16000, mono=True, offset=start_sec, duration=duration_sec)
    return audio, sr

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

def test_episode(episode_num):
    """Test first and last 5 minutes of an episode."""
    print(f"\n{'='*70}", flush=True)
    print(f"Testing Episode {episode_num:03d}", flush=True)
    print(f"{'='*70}", flush=True)
    
    # Find cleaned audio file
    audio_pattern = f"episode_{episode_num:03d}_*_cleaned.wav"
    audio_files = list(Path(CLEANED_DIR).glob(audio_pattern))
    
    if not audio_files:
        print(f"  ERROR: No cleaned audio found", flush=True)
        return None
    
    audio_path = str(audio_files[0])
    print(f"  Audio: {os.path.basename(audio_path)}", flush=True)
    
    # Find transcript
    transcript_files = list(Path(TRANSCRIPT_DIR).glob(f"episode_{episode_num:03d}_*.json"))
    if not transcript_files:
        print(f"  ERROR: No transcript found", flush=True)
        return None
    
    transcript_path = str(transcript_files[0])
    
    # Get audio duration
    audio_info = sf.info(audio_path)
    duration = audio_info.duration
    print(f"  Duration: {duration:.1f}s ({duration/60:.1f} min)", flush=True)
    
    results = {
        'episode': episode_num,
        'audio_path': audio_path,
        'transcript_path': transcript_path,
        'duration': duration,
        'segments': []
    }
    
    # Test first 5 minutes
    print(f"\n  Testing first 5 minutes...", flush=True)
    first_5min_audio, sr = extract_audio_segment(audio_path, 0, 300)
    first_5min_original = get_original_transcript_text(transcript_path, 0, 300)
    
    # Save segment
    segment_path = os.path.join(OUTPUT_DIR, f"ep{episode_num:03d}_first_5min.wav")
    sf.write(segment_path, first_5min_audio, int(sr))
    
    # Transcribe with PyTorch Whisper MPS
    start_time = time.time()
    whisper_result = model.transcribe(
        segment_path,
        language="de",
        fp16=False,  # MPS doesn't support fp16
        verbose=False
    )
    transcribe_time = time.time() - start_time
    
    whisper_text = ' '.join([seg['text'].strip() for seg in whisper_result['segments']])
    similarity = calculate_similarity(first_5min_original, whisper_text)
    
    print(f"    Transcription time: {transcribe_time:.2f}s", flush=True)
    print(f"    Original length: {len(first_5min_original)} chars", flush=True)
    print(f"    Whisper length: {len(whisper_text)} chars", flush=True)
    print(f"    Similarity: {similarity*100:.1f}%", flush=True)
    
    results['segments'].append({
        'type': 'first_5min',
        'start': 0,
        'end': 300,
        'original_text': first_5min_original,
        'whisper_text': whisper_text,
        'similarity': similarity,
        'transcribe_time': transcribe_time,
        'whisper_segments': whisper_result['segments']
    })
    
    # Test last 5 minutes
    print(f"\n  Testing last 5 minutes...", flush=True)
    last_5min_start = max(0, duration - 300)
    last_5min_audio, sr = extract_audio_segment(audio_path, last_5min_start, 300)
    last_5min_original = get_original_transcript_text(transcript_path, last_5min_start, duration)
    
    # Save segment
    segment_path = os.path.join(OUTPUT_DIR, f"ep{episode_num:03d}_last_5min.wav")
    sf.write(segment_path, last_5min_audio, int(sr))
    
    # Transcribe with PyTorch Whisper MPS
    start_time = time.time()
    whisper_result = model.transcribe(
        segment_path,
        language="de",
        fp16=False,
        verbose=False
    )
    transcribe_time = time.time() - start_time
    
    whisper_text = ' '.join([seg['text'].strip() for seg in whisper_result['segments']])
    similarity = calculate_similarity(last_5min_original, whisper_text)
    
    print(f"    Transcription time: {transcribe_time:.2f}s", flush=True)
    print(f"    Original length: {len(last_5min_original)} chars", flush=True)
    print(f"    Whisper length: {len(whisper_text)} chars", flush=True)
    print(f"    Similarity: {similarity*100:.1f}%", flush=True)
    
    results['segments'].append({
        'type': 'last_5min',
        'start': last_5min_start,
        'end': duration,
        'original_text': last_5min_original,
        'whisper_text': whisper_text,
        'similarity': similarity,
        'transcribe_time': transcribe_time,
        'whisper_segments': whisper_result['segments']
    })
    
    # Save results
    output_path = os.path.join(OUTPUT_DIR, f"episode_{episode_num:03d}_comparison.json")
    with open(output_path, 'w', encoding='utf-8') as f:
        json.dump(results, f, indent=2, ensure_ascii=False)
    
    avg_similarity = sum(s['similarity'] for s in results['segments']) / len(results['segments'])
    avg_time = sum(s['transcribe_time'] for s in results['segments']) / len(results['segments'])
    
    print(f"\n  Average similarity: {avg_similarity*100:.1f}%", flush=True)
    print(f"  Average transcription time: {avg_time:.2f}s per 5 minutes", flush=True)
    print(f"  Speedup: {300/avg_time:.1f}x realtime", flush=True)
    
    return results

def main():
    """Main entry point."""
    print("="*70, flush=True)
    print("PyTorch Whisper MPS Test on 10 Episodes (First & Last 5 Minutes)", flush=True)
    print("="*70, flush=True)
    
    # Check MPS availability
    if not torch.backends.mps.is_available():
        print("ERROR: MPS not available on this system", flush=True)
        print("Falling back to CPU", flush=True)
        device = "cpu"
    else:
        device = "mps"
        print(f"MPS available: True", flush=True)
    
    print(f"Device: {device}", flush=True)
    print(f"Output directory: {OUTPUT_DIR}", flush=True)
    print(flush=True)
    
    # Load PyTorch Whisper model with MPS
    print("Loading PyTorch Whisper model...", flush=True)
    
    global model
    model = whisper.load_model("large-v3", device=device)
    print("Model loaded successfully", flush=True)
    print(flush=True)
    
    # Test episodes 1-10
    all_results = []
    for episode_num in range(1, 11):
        result = test_episode(episode_num)
        if result:
            all_results.append(result)
    
    # Summary
    print("\n" + "="*70, flush=True)
    print("Summary", flush=True)
    print("="*70, flush=True)
    
    if all_results:
        total_segments = sum(len(r['segments']) for r in all_results)
        avg_similarity = sum(
            s['similarity'] 
            for r in all_results 
            for s in r['segments']
        ) / total_segments
        
        avg_time = sum(
            s['transcribe_time'] 
            for r in all_results 
            for s in r['segments']
        ) / total_segments
        
        speedup = 300 / avg_time
        
        print(f"Episodes tested: {len(all_results)}/10", flush=True)
        print(f"Total segments: {total_segments}", flush=True)
        print(f"Average similarity: {avg_similarity*100:.1f}%", flush=True)
        print(f"Average time per 5min: {avg_time:.2f}s", flush=True)
        print(f"Speedup: {speedup:.1f}x realtime", flush=True)
        print(f"Estimated time for 90min episode: {(90*60)/speedup/60:.1f} minutes", flush=True)
        
        # Save summary
        summary = {
            'device': device,
            'episodes_tested': len(all_results),
            'total_segments': total_segments,
            'average_similarity': avg_similarity,
            'average_time_per_5min': avg_time,
            'speedup': speedup,
            'estimated_time_per_90min_episode': (90*60)/speedup/60,
            'results': all_results
        }
        
        summary_path = os.path.join(OUTPUT_DIR, "summary.json")
        with open(summary_path, 'w', encoding='utf-8') as f:
            json.dump(summary, f, indent=2, ensure_ascii=False)
        
        print(f"\nResults saved to: {OUTPUT_DIR}", flush=True)
    
    return 0

if __name__ == "__main__":
    sys.exit(main())

# Made with Bob