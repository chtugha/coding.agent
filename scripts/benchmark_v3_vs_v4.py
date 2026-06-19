#!/usr/bin/env python3
"""
Benchmark V3 vs V4 waveform alignment algorithms.
Tests speed and accuracy on episode 151.
"""

import json
import time
import numpy as np
import soundfile as sf

# Import both versions
from fix_waveform_alignment_v3 import align_audio_to_transcript_continuous as align_v3
from fix_waveform_alignment_v4_optimized import align_audio_to_transcript_fast as align_v4


def load_test_data():
    """Load episode 151 audio and transcript."""
    print("Loading test data...")
    
    # Load audio
    audio_path = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/Gemischtes.Hack.Podcast.Audio/episode_151_alle_heissen_tim_und_alles_muss_raus.wav"
    audio, sr = sf.read(audio_path)
    
    # Convert stereo to mono if needed
    if len(audio.shape) > 1:
        audio = np.mean(audio, axis=1)
    
    print(f"  Audio: {len(audio)/sr:.1f}s at {sr} Hz")
    
    # Load transcript
    transcript_path = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/Gemischtes.Hack.Podcast.Transcript/transcripts/episode_151_alle_heissen_tim_und_alles_muss_raus.json"
    with open(transcript_path, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    segments = data['segments']
    print(f"  Transcript: {len(segments)} segments")
    
    return audio, sr, segments


def benchmark_algorithm(name, align_func, audio, sr, segments):
    """Benchmark a single algorithm."""
    print(f"\n{'='*70}")
    print(f"Testing {name}")
    print(f"{'='*70}\n")
    
    start_time = time.time()
    cleaned_audio, regions = align_func(audio, sr, segments)
    elapsed = time.time() - start_time
    
    # Calculate metrics
    original_duration = len(audio) / sr
    cleaned_duration = len(cleaned_audio) / sr
    transcript_duration = max(seg['end'] for seg in segments)
    difference = abs(cleaned_duration - transcript_duration)
    percent_diff = (difference / transcript_duration) * 100
    
    print(f"\n{name} Results:")
    print(f"  Processing time: {elapsed:.2f}s")
    print(f"  Original duration: {original_duration:.1f}s")
    print(f"  Cleaned duration: {cleaned_duration:.1f}s")
    print(f"  Transcript duration: {transcript_duration:.1f}s")
    print(f"  Difference: {difference:.1f}s ({percent_diff:.2f}%)")
    print(f"  Regions kept: {len(regions)}")
    
    return {
        'name': name,
        'time': elapsed,
        'original_duration': original_duration,
        'cleaned_duration': cleaned_duration,
        'transcript_duration': transcript_duration,
        'difference': difference,
        'percent_diff': percent_diff,
        'regions': len(regions)
    }


def main():
    print("="*70)
    print("Waveform Alignment Benchmark: V3 vs V4")
    print("="*70)
    print()
    
    # Load test data
    audio, sr, segments = load_test_data()
    
    # Benchmark V3
    results_v3 = benchmark_algorithm("V3 (Original)", align_v3, audio, sr, segments)
    
    # Benchmark V4
    results_v4 = benchmark_algorithm("V4 (Optimized)", align_v4, audio, sr, segments)
    
    # Compare results
    print(f"\n{'='*70}")
    print("Comparison")
    print(f"{'='*70}\n")
    
    speedup = results_v3['time'] / results_v4['time']
    accuracy_diff = abs(results_v4['difference'] - results_v3['difference'])
    
    print(f"Speed:")
    print(f"  V3 time: {results_v3['time']:.2f}s")
    print(f"  V4 time: {results_v4['time']:.2f}s")
    print(f"  Speedup: {speedup:.1f}x faster")
    
    print(f"\nAccuracy:")
    print(f"  V3 difference: {results_v3['difference']:.1f}s ({results_v3['percent_diff']:.2f}%)")
    print(f"  V4 difference: {results_v4['difference']:.1f}s ({results_v4['percent_diff']:.2f}%)")
    print(f"  Accuracy change: {accuracy_diff:.1f}s")
    
    if accuracy_diff < 1.0:
        print(f"  ✓ Accuracy maintained (< 1s difference)")
    else:
        print(f"  ⚠ Accuracy changed by {accuracy_diff:.1f}s")
    
    print(f"\nConclusion:")
    if speedup >= 10 and accuracy_diff < 1.0:
        print(f"  ✓ V4 achieves {speedup:.1f}x speedup while maintaining accuracy!")
        print(f"  ✓ Ready for production use")
    elif speedup >= 5:
        print(f"  ✓ V4 achieves {speedup:.1f}x speedup")
        if accuracy_diff < 1.0:
            print(f"  ✓ Accuracy maintained")
        else:
            print(f"  ⚠ Accuracy needs review")
    else:
        print(f"  ⚠ Speedup ({speedup:.1f}x) lower than expected (10-20x)")
    
    print(f"\nProjected time for 373 episodes:")
    print(f"  V3: {results_v3['time'] * 373 / 60:.1f} minutes")
    print(f"  V4: {results_v4['time'] * 373 / 60:.1f} minutes")
    print(f"  Time saved: {(results_v3['time'] - results_v4['time']) * 373 / 60:.1f} minutes")


if __name__ == "__main__":
    main()

# Made with Bob