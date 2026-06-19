#!/usr/bin/env python3
"""
Align existing transcript to audio using WhisperX directly.
No transcription needed - just forced alignment!

This is much faster than MFA and produces state-of-the-art results.
"""

import json
import sys
import time
from pathlib import Path


def align_transcript_with_whisperx(audio_path, transcript_segments, language="de", device="cpu"):
    """
    Align existing transcript to audio using WhisperX's forced alignment.
    
    Args:
        audio_path: Path to audio file
        transcript_segments: List of segments with 'text', 'start', 'end'
        language: Language code (default: "de")
        device: Device to use (default: "cpu")
    
    Returns:
        Aligned segments with word-level timestamps
    """
    import whisperx
    
    print(f"Loading audio: {audio_path}")
    audio = whisperx.load_audio(audio_path)
    print(f"  Duration: {len(audio)/16000:.1f}s")
    
    print(f"\nLoading WhisperX alignment model for {language}...")
    model_a, metadata = whisperx.load_align_model(
        language_code=language,
        device=device
    )
    print("  Model loaded")
    
    print(f"\nAligning {len(transcript_segments)} segments...")
    start_time = time.time()
    
    result = whisperx.align(
        transcript_segments,
        model_a,
        metadata,
        audio,
        device=device,
        return_char_alignments=False
    )
    
    elapsed = time.time() - start_time
    print(f"  Alignment complete in {elapsed:.1f}s")
    
    # Count words
    total_words = sum(len(seg.get('words', [])) for seg in result['segments'])
    print(f"  Aligned {total_words} words")
    
    return result


def main():
    print("=" * 70)
    print("WhisperX Direct Transcript Alignment - Episode 151")
    print("=" * 70)
    print()
    
    # Paths
    audio_path = "/tmp/fixed_alignment_test/episode_151_cleaned_fixed.wav"
    transcript_path = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/Gemischtes.Hack.Podcast.Transcript/transcripts/episode_151_alle_heissen_tim_und_alles_muss_raus.json"
    output_path = "/tmp/fixed_alignment_test/episode_151_whisperx_direct.json"
    
    # Load existing transcript
    print("Loading existing transcript...")
    with open(transcript_path, 'r', encoding='utf-8') as f:
        data = json.load(f)
    
    segments = data['segments']
    print(f"  Loaded {len(segments)} segments")
    
    # Align with WhisperX
    result = align_transcript_with_whisperx(audio_path, segments, language="de")
    
    # Save result
    print(f"\nSaving aligned transcript...")
    with open(output_path, 'w', encoding='utf-8') as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
    
    print(f"  Saved to: {output_path}")
    
    # Show statistics
    print(f"\n{'='*70}")
    print("Statistics")
    print(f"{'='*70}")
    
    total_words = sum(len(seg.get('words', [])) for seg in result['segments'])
    total_duration = max(seg['end'] for seg in result['segments'])
    
    print(f"Segments: {len(result['segments'])}")
    print(f"Words: {total_words}")
    print(f"Duration: {total_duration:.1f}s")
    
    # Show first 10 words
    print(f"\nFirst 10 words:")
    word_count = 0
    for seg in result['segments']:
        for word in seg.get('words', []):
            if word_count < 10:
                print(f"  {word_count+1}. [{word['start']:.3f} - {word['end']:.3f}] {word['word']}")
                word_count += 1
            else:
                break
        if word_count >= 10:
            break
    
    print(f"\n{'='*70}")
    print("Success!")
    print(f"{'='*70}")
    print()
    print("Next steps:")
    print("1. Compare with MFA results")
    print("2. Measure speed difference")
    print("3. If successful, replace MFA in pipeline")
    
    return True


if __name__ == "__main__":
    try:
        success = main()
        sys.exit(0 if success else 1)
    except Exception as e:
        print(f"\n✗ Error: {e}")
        import traceback
        traceback.print_exc()
        sys.exit(1)

# Made with Bob