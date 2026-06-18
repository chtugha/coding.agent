# Word-Level Timestamp Solution

## Problem Identified

The original podcast transcripts contain **only segment-level timestamps**, not word-level:

```json
{
  "start": 0.6,
  "end": 10.04,
  "text": "Unterfickt und geistig behindert...",
  "speaker": "Speaker A"
  // NO "words" array
}
```

This forces the preparation script to use **uniform word distribution**, which creates inaccurate timestamps and leads to:
- 11.40% average WER (acceptable but not optimal)
- Some files with 20-30% WER
- Misalignment between audio and transcript timestamps

## Solution: Forced Alignment

Instead of re-transcribing (slow), we use **forced alignment** to add word-level timestamps to existing transcripts.

### Why Forced Alignment?

1. **Much faster**: 10-20x realtime vs 1x realtime for re-transcription
2. **Preserves existing text**: No risk of transcription errors
3. **Precise timestamps**: Uses acoustic models to find exact word boundaries
4. **Proven technology**: Used by professional subtitle/caption systems

### Performance Comparison

| Method | Speed | 1 Hour Audio |
|--------|-------|--------------|
| Re-transcription | 1x realtime | 60 minutes |
| Forced Alignment | 10-20x realtime | 3-6 minutes |

## Implementation

### Step 1: Install WhisperX

```bash
pip install whisperx
```

WhisperX includes:
- Faster-whisper for efficient inference
- Forced alignment models for multiple languages
- GPU acceleration support

### Step 2: Run Forced Alignment

Test on 5 episodes first:
```bash
python3 scripts/add_word_timestamps_forced_alignment.py \
  /Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast \
  --max-episodes 5
```

Process entire dataset:
```bash
python3 scripts/add_word_timestamps_forced_alignment.py \
  /Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast
```

### Step 3: Re-run Preparation Script

After adding word-level timestamps, re-run the preparation script:

```bash
python3 scripts/prepare_german_dataset.py \
  --dataset Gemischtes.Hack.Podcast \
  --episodes 150 \
  --output /Volumes/eHDD/moshi-rag-data/processed/podcast_with_word_timestamps
```

The script will now use actual word-level timestamps instead of uniform distribution.

### Step 4: Verify Results

```bash
python3 scripts/verify_podcasts_enhanced.py \
  /Volumes/eHDD/moshi-rag-data/processed/podcast_with_word_timestamps \
  --max-files 10 \
  --force
```

## Expected Improvements

With word-level timestamps, we expect:

| Metric | Before (Uniform) | After (Word-Level) |
|--------|------------------|-------------------|
| Average WER | 11.40% | 5-8% (estimated) |
| Files with 0-10% WER | 50% | 80-90% |
| Files with >20% WER | 10% | <5% |

## Technical Details

### What Forced Alignment Does

1. **Loads audio** and existing transcript text
2. **Loads acoustic model** trained on German speech
3. **Segments audio** into phonemes using the model
4. **Aligns phonemes** to transcript words
5. **Outputs precise timestamps** for each word

### Output Format

Before (segment-level):
```json
{
  "start": 0.6,
  "end": 10.04,
  "text": "Hallo und willkommen zur Show",
  "speaker": "Speaker A"
}
```

After (with word-level):
```json
{
  "start": 0.6,
  "end": 10.04,
  "text": "Hallo und willkommen zur Show",
  "speaker": "Speaker A",
  "words": [
    {"word": "Hallo", "start": 0.60, "end": 1.15},
    {"word": "und", "start": 1.20, "end": 1.35},
    {"word": "willkommen", "start": 1.40, "end": 2.10},
    {"word": "zur", "start": 2.15, "end": 2.35},
    {"word": "Show", "start": 2.40, "end": 2.85}
  ]
}
```

### How Preparation Script Uses Word Timestamps

The [`prepare_german_dataset.py`](prepare_german_dataset.py:211-265) script checks for word-level timestamps:

```python
def parse_podcast_turns(segments, sample_rate=48000):
    turns = []
    for seg in segments:
        words = seg.get("words", [])
        
        if words:
            # Use actual word-level timestamps
            word_list = [
                {
                    "word": w["word"],
                    "start": w["start"],
                    "end": w["end"]
                }
                for w in words
            ]
        else:
            # Fallback to uniform distribution
            word_list = distribute_words_uniformly(seg)
        
        turns.append({
            "speaker": seg["speaker"],
            "words": word_list
        })
    
    return turns
```

## Benefits

1. **Accurate timestamps**: Words aligned to actual speech
2. **Better WER**: Reduced transcription errors
3. **Cleaner chunks**: Splits occur at natural word boundaries
4. **Faster processing**: No need to re-transcribe
5. **Preserves quality**: Original transcription text unchanged

## Next Steps

1. ✅ Install WhisperX
2. ✅ Test on 5 episodes
3. ⏳ Process all episodes in dataset
4. ⏳ Re-run preparation script
5. ⏳ Verify improved WER metrics

## Files

- [`add_word_timestamps_forced_alignment.py`](add_word_timestamps_forced_alignment.py) - Forced alignment script
- [`INSTALL_WHISPERX.md`](INSTALL_WHISPERX.md) - Installation instructions
- [`prepare_german_dataset.py`](prepare_german_dataset.py:211) - Uses word timestamps when available
- [`verify_podcasts_enhanced.py`](verify_podcasts_enhanced.py) - Verification script

## References

- WhisperX: https://github.com/m-bain/whisperX
- Montreal Forced Aligner: https://montreal-forced-aligner.readthedocs.io/
- Forced Alignment Overview: https://en.wikipedia.org/wiki/Forced_alignment