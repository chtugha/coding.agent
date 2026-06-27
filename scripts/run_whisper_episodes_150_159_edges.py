#!/usr/bin/env python3
"""
Run Whisper.cpp on first and last 5 minutes of episodes 150-159.
Generates word-level timestamps with merged BPE tokens.
Uses CoreML for fast processing and runs episodes in parallel.
"""

import subprocess
import os
import json
import time
import soundfile as sf
from concurrent.futures import ThreadPoolExecutor, as_completed

# Directories
CLEANED_AUDIO_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/podcast_clean"
OUTPUT_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/podcast_clean/whisper_output"

# Whisper.cpp settings
WHISPER_BIN = "./whisper-cpp/build/bin/whisper-cli"
MODEL_PATH = "./bin/models/ggml-large-v3-turbo-q5_0.bin"

# Episode range
START_EPISODE = 150
END_EPISODE = 159

# Duration for edges (5 minutes = 300 seconds = 300,000 ms)
EDGE_DURATION_MS = 300000

def merge_bpe_tokens(transcription):
    """
    Merge BPE tokens into complete words.
    Tokens that don't start with a space are continuations of the previous word.
    """
    merged_words = []
    current_word = None
    
    for segment in transcription:
        text = segment['text']
        start_ms = segment['offsets']['from']
        end_ms = segment['offsets']['to']
        
        # Check if this token starts a new word (begins with space or is first token)
        if text.startswith(' ') or current_word is None:
            # Save previous word if exists
            if current_word is not None:
                merged_words.append(current_word)
            
            # Start new word
            current_word = {
                'text': text.strip(),
                'start': start_ms / 1000.0,  # Convert to seconds
                'end': end_ms / 1000.0,
                'start_ms': start_ms,
                'end_ms': end_ms
            }
        else:
            # This is a continuation of the current word (no leading space)
            if current_word is not None:
                current_word['text'] += text
                current_word['end'] = end_ms / 1000.0
                current_word['end_ms'] = end_ms
    
    # Don't forget the last word
    if current_word is not None:
        merged_words.append(current_word)
    
    return merged_words

def run_whisper_on_segment(audio_file, output_base, offset_ms, duration_ms, segment_type):
    """
    Run Whisper.cpp on a segment of audio.
    
    Args:
        audio_file: Path to audio file
        output_base: Base path for output (without extension)
        offset_ms: Start offset in milliseconds
        duration_ms: Duration in milliseconds
        segment_type: "start" or "end"
    """
    print(f"\n   Running Whisper.cpp on {segment_type} segment...")
    print(f"   Offset: {offset_ms/1000:.1f}s, Duration: {duration_ms/1000:.1f}s")
    
    cmd = [
        WHISPER_BIN,
        "-m", MODEL_PATH,
        "-f", audio_file,
        "-l", "de",
        "-oj",  # Output JSON with word timestamps
        "-of", output_base,
        "-ot", str(offset_ms),  # Offset
        "-d", str(duration_ms),  # Duration
        "-ml", "1",  # Max segment length
        "-t", "8",  # Threads
    ]
    
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
        
        if result.returncode != 0:
            print(f"   ❌ Whisper failed with return code {result.returncode}")
            return False
        
        return True
        
    except subprocess.TimeoutExpired:
        print(f"   ❌ Whisper timed out")
        return False
    except Exception as e:
        print(f"   ❌ Whisper error: {e}")
        return False

def process_episode(episode_num):
    """Process first and last 5 minutes of an episode"""
    print("\n" + "="*80)
    print(f"Processing Episode {episode_num}")
    print("="*80)
    
    # Paths
    audio_file = os.path.join(CLEANED_AUDIO_DIR, f"episode_{episode_num:03d}_cleaned.wav")
    
    if not os.path.exists(audio_file):
        print(f"❌ Audio file not found: {audio_file}")
        return False
    
    print(f"\n📁 Audio: {os.path.basename(audio_file)}")
    
    # Get audio duration
    try:
        audio, sr = sf.read(audio_file)
        duration_s = len(audio) / sr
        duration_ms = int(duration_s * 1000)
        print(f"   Duration: {duration_s:.1f}s ({duration_s/60:.1f} min)")
    except Exception as e:
        print(f"❌ Error reading audio: {e}")
        return False
    
    # Create output directory for this episode
    episode_output_dir = os.path.join(OUTPUT_DIR, f"episode_{episode_num:03d}")
    os.makedirs(episode_output_dir, exist_ok=True)
    
    results = {}
    
    # Process START (first 5 minutes)
    print(f"\n1️⃣  Processing FIRST 5 minutes...")
    start_output_base = os.path.join(episode_output_dir, f"episode_{episode_num:03d}_start")
    start_success = run_whisper_on_segment(
        audio_file, 
        start_output_base, 
        0,  # Start from beginning
        EDGE_DURATION_MS,
        "start"
    )
    
    if start_success:
        # Load and merge tokens
        start_json = start_output_base + ".json"
        if os.path.exists(start_json):
            with open(start_json, 'r', encoding='utf-8') as f:
                data = json.load(f)
            
            print(f"   ✓ Raw segments: {len(data.get('transcription', []))}")
            
            # Merge BPE tokens
            merged_words = merge_bpe_tokens(data['transcription'])
            print(f"   ✓ Merged words: {len(merged_words)}")
            
            # Save merged output
            merged_output = {
                'episode': episode_num,
                'segment': 'start',
                'source': 'whisper.cpp',
                'model': data.get('params', {}).get('model', 'unknown'),
                'language': data.get('result', {}).get('language', 'de'),
                'offset_seconds': 0,
                'duration_seconds': EDGE_DURATION_MS / 1000,
                'word_count': len(merged_words),
                'words': merged_words
            }
            
            merged_json = os.path.join(episode_output_dir, f"episode_{episode_num:03d}_start_merged.json")
            with open(merged_json, 'w', encoding='utf-8') as f:
                json.dump(merged_output, f, indent=2, ensure_ascii=False)
            
            print(f"   ✓ Saved: {os.path.basename(merged_json)}")
            
            # Show sample
            if merged_words:
                print(f"   Sample: [{merged_words[0]['start']:.3f}s] {merged_words[0]['text']}")
            
            results['start'] = True
        else:
            print(f"   ❌ Output file not found")
            results['start'] = False
    else:
        results['start'] = False
    
    # Process END (last 5 minutes)
    print(f"\n2️⃣  Processing LAST 5 minutes...")
    
    # Calculate offset for last 5 minutes
    end_offset_ms = max(0, duration_ms - EDGE_DURATION_MS)
    end_duration_ms = duration_ms - end_offset_ms
    
    end_output_base = os.path.join(episode_output_dir, f"episode_{episode_num:03d}_end")
    end_success = run_whisper_on_segment(
        audio_file,
        end_output_base,
        end_offset_ms,
        end_duration_ms,
        "end"
    )
    
    if end_success:
        # Load and merge tokens
        end_json = end_output_base + ".json"
        if os.path.exists(end_json):
            with open(end_json, 'r', encoding='utf-8') as f:
                data = json.load(f)
            
            print(f"   ✓ Raw segments: {len(data.get('transcription', []))}")
            
            # Merge BPE tokens and adjust timestamps
            merged_words = merge_bpe_tokens(data['transcription'])
            
            # Adjust timestamps to be relative to full audio
            offset_s = end_offset_ms / 1000.0
            for word in merged_words:
                word['start'] += offset_s
                word['end'] += offset_s
                word['start_ms'] += end_offset_ms
                word['end_ms'] += end_offset_ms
            
            print(f"   ✓ Merged words: {len(merged_words)}")
            
            # Save merged output
            merged_output = {
                'episode': episode_num,
                'segment': 'end',
                'source': 'whisper.cpp',
                'model': data.get('params', {}).get('model', 'unknown'),
                'language': data.get('result', {}).get('language', 'de'),
                'offset_seconds': offset_s,
                'duration_seconds': end_duration_ms / 1000,
                'word_count': len(merged_words),
                'words': merged_words
            }
            
            merged_json = os.path.join(episode_output_dir, f"episode_{episode_num:03d}_end_merged.json")
            with open(merged_json, 'w', encoding='utf-8') as f:
                json.dump(merged_output, f, indent=2, ensure_ascii=False)
            
            print(f"   ✓ Saved: {os.path.basename(merged_json)}")
            
            # Show sample
            if merged_words:
                print(f"   Sample: [{merged_words[0]['start']:.3f}s] {merged_words[0]['text']}")
            
            results['end'] = True
        else:
            print(f"   ❌ Output file not found")
            results['end'] = False
    else:
        results['end'] = False
    
    # Summary
    success = results.get('start', False) and results.get('end', False)
    if success:
        print(f"\n✅ Episode {episode_num} completed successfully!")
    else:
        print(f"\n⚠️  Episode {episode_num} partially completed")
        if not results.get('start'):
            print(f"   - Start segment failed")
        if not results.get('end'):
            print(f"   - End segment failed")
    
    return success

def main():
    print("="*80)
    print("Whisper.cpp - Episodes 150-159 (First & Last 5 Minutes)")
    print("="*80)
    print(f"\nAudio directory: {CLEANED_AUDIO_DIR}")
    print(f"Output directory: {OUTPUT_DIR}")
    print(f"Model: {MODEL_PATH}")
    print(f"\nProcessing episodes {START_EPISODE} to {END_EPISODE}")
    print(f"Segments: First 5 min + Last 5 min")
    print(f"Parallel processing: 3 episodes at a time")
    
    # Check if Whisper binary exists
    if not os.path.exists(WHISPER_BIN):
        print(f"\n❌ Whisper binary not found: {WHISPER_BIN}")
        return
    
    # Check for CoreML encoder
    coreml_encoder = "./bin/models/ggml-large-v3-turbo-encoder.mlmodelc"
    if os.path.exists(coreml_encoder):
        print(f"✓ CoreML encoder found (fast processing enabled)")
    else:
        print(f"⚠️  CoreML encoder not found (will be slower)")
    
    # Create output directory
    os.makedirs(OUTPUT_DIR, exist_ok=True)
    
    # Process episodes in parallel (3 at a time to avoid overwhelming the system)
    from concurrent.futures import ThreadPoolExecutor, as_completed
    
    results = {}
    total_start = time.time()
    
    episodes = list(range(START_EPISODE, END_EPISODE + 1))
    
    with ThreadPoolExecutor(max_workers=3) as executor:
        future_to_episode = {executor.submit(process_episode, ep): ep for ep in episodes}
        
        for future in as_completed(future_to_episode):
            episode_num = future_to_episode[future]
            try:
                success = future.result()
                results[episode_num] = success
            except Exception as e:
                print(f"\n❌ Episode {episode_num} failed with exception: {e}")
                results[episode_num] = False
    
    total_time = time.time() - total_start
    
    # Summary
    print("\n" + "="*80)
    print("PROCESSING SUMMARY")
    print("="*80)
    
    successful = sum(1 for v in results.values() if v)
    failed = sum(1 for v in results.values() if not v)
    
    print(f"\n✅ Successful: {successful}/{len(results)}")
    print(f"❌ Failed: {failed}/{len(results)}")
    print(f"⏱️  Total time: {total_time:.1f}s ({total_time/60:.1f} min)")
    
    if failed > 0:
        print(f"\nFailed episodes:")
        for ep, success in results.items():
            if not success:
                print(f"  - Episode {ep}")
    
    print(f"\n📁 Output location: {OUTPUT_DIR}")
    print("="*80)

if __name__ == "__main__":
    main()

# Made with Bob
