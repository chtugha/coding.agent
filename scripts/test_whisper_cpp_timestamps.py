#!/usr/bin/env python3
"""
Test whisper.cpp with different timestamp output formats.
This will help us understand what timestamp data is available.
"""

import os
import sys
import json
import time
import subprocess

# Unbuffered output
sys.stdout = os.fdopen(sys.stdout.fileno(), 'w', buffering=1)

# Paths
WHISPER_CLI = "./whisper-cpp/build/bin/whisper-cli"
MODEL_PATH = "./bin/models/ggml-large-v3-turbo-q5_0.bin"
AUDIO_PATH = "/Volumes/eHDD/moshi-rag-data/datasets/mlx_whisper_test/episode_150_first_5min.wav"
OUTPUT_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/whisper_cpp_test"

# Create output directory
os.makedirs(OUTPUT_DIR, exist_ok=True)

def run_whisper_cpp_test(test_name, extra_args):
    """Run whisper.cpp with specific arguments and measure performance."""
    print(f"\n{'='*70}")
    print(f"Test: {test_name}")
    print(f"{'='*70}")
    
    output_file = os.path.join(OUTPUT_DIR, f"test_{test_name}")
    
    cmd = [
        WHISPER_CLI,
        "-m", MODEL_PATH,
        "-l", "de",  # German language
        "-t", "8",   # 8 threads
        "-of", output_file,
        *extra_args,
        AUDIO_PATH
    ]
    
    print(f"Command: {' '.join(cmd)}")
    print(flush=True)
    
    start_time = time.time()
    result = subprocess.run(cmd, capture_output=True, text=True)
    elapsed = time.time() - start_time
    
    print(f"Exit code: {result.returncode}")
    print(f"Time: {elapsed:.2f}s")
    
    if result.returncode != 0:
        print(f"STDERR: {result.stderr}")
        return None
    
    # Check what files were created
    created_files = []
    for ext in ['.json', '.txt', '.srt', '.vtt', '.csv']:
        filepath = output_file + ext
        if os.path.exists(filepath):
            size = os.path.getsize(filepath)
            created_files.append((filepath, size))
            print(f"  Created: {os.path.basename(filepath)} ({size} bytes)")
    
    return {
        'test_name': test_name,
        'elapsed': elapsed,
        'files': created_files,
        'stdout': result.stdout[:500] if result.stdout else None
    }

def main():
    """Main entry point."""
    print("="*70)
    print("whisper.cpp Timestamp Format Testing")
    print("="*70)
    print()
    
    # Check files exist
    if not os.path.exists(WHISPER_CLI):
        print(f"ERROR: whisper-cli not found: {WHISPER_CLI}")
        return 1
    
    if not os.path.exists(MODEL_PATH):
        print(f"ERROR: Model not found: {MODEL_PATH}")
        return 1
    
    if not os.path.exists(AUDIO_PATH):
        print(f"ERROR: Audio not found: {AUDIO_PATH}")
        return 1
    
    print(f"whisper-cli: {WHISPER_CLI}")
    print(f"Model: {MODEL_PATH}")
    print(f"Audio: {AUDIO_PATH}")
    print(f"Output: {OUTPUT_DIR}")
    print()
    
    results = []
    
    # Test 1: Basic JSON output
    result = run_whisper_cpp_test("basic_json", ["-oj"])
    if result:
        results.append(result)
    
    # Test 2: Full JSON output (more details)
    result = run_whisper_cpp_test("full_json", ["-ojf"])
    if result:
        results.append(result)
    
    # Test 3: SRT output (subtitle format with timestamps)
    result = run_whisper_cpp_test("srt", ["-osrt"])
    if result:
        results.append(result)
    
    # Test 4: Word-level output (karaoke format)
    result = run_whisper_cpp_test("words", ["-owts"])
    if result:
        results.append(result)
    
    # Test 5: CSV output
    result = run_whisper_cpp_test("csv", ["-ocsv"])
    if result:
        results.append(result)
    
    # Test 6: Full JSON + Word timestamps
    result = run_whisper_cpp_test("full_json_words", ["-ojf", "-owts"])
    if result:
        results.append(result)
    
    # Summary
    print()
    print("="*70)
    print("Summary")
    print("="*70)
    for r in results:
        print(f"{r['test_name']:20s} {r['elapsed']:6.2f}s  {len(r['files'])} files")
    
    # Analyze JSON outputs
    print()
    print("="*70)
    print("Analyzing JSON Outputs")
    print("="*70)
    
    for test_name in ["basic_json", "full_json", "full_json_words"]:
        json_file = os.path.join(OUTPUT_DIR, f"test_{test_name}.json")
        if os.path.exists(json_file):
            print(f"\n{test_name}:")
            with open(json_file, 'r') as f:
                data = json.load(f)
            
            # Show structure
            print(f"  Keys: {list(data.keys())}")
            
            if 'transcription' in data:
                trans = data['transcription']
                if isinstance(trans, list) and len(trans) > 0:
                    print(f"  Segments: {len(trans)}")
                    print(f"  First segment keys: {list(trans[0].keys())}")
                    print(f"  First segment: {json.dumps(trans[0], indent=2)[:300]}...")
    
    return 0

if __name__ == "__main__":
    sys.exit(main())

# Made with Bob