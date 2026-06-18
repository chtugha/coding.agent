# Installing WhisperX for Forced Alignment

WhisperX is a faster alternative to Whisper that includes forced alignment capabilities.

## Installation

```bash
# Install whisperx
pip install whisperx

# Or if you have CUDA GPU:
pip install whisperx --upgrade --force-reinstall
```

## Dependencies

WhisperX will automatically install:
- torch
- whisper (OpenAI)
- faster-whisper
- pyannote.audio (for speaker diarization)
- ctranslate2 (for faster inference)

## Usage

### Test on a single dataset (first 5 episodes):
```bash
python3 scripts/add_word_timestamps_forced_alignment.py \
  /Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast \
  --max-episodes 5
```

### Process entire dataset:
```bash
python3 scripts/add_word_timestamps_forced_alignment.py \
  /Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast
```

### Process with output to new directory:
```bash
python3 scripts/add_word_timestamps_forced_alignment.py \
  /Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast \
  --output /Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast_aligned
```

## Performance

Forced alignment is **much faster** than re-transcription:
- Re-transcription: ~1x realtime (1 hour audio = 1 hour processing)
- Forced alignment: ~10-20x realtime (1 hour audio = 3-6 minutes processing)

## What It Does

1. Loads existing transcript (segment-level timestamps)
2. Loads audio file
3. Uses forced alignment to find exact word boundaries
4. Adds `words` array to each segment with precise timestamps
5. Saves updated transcript

## Output Format

Before (segment-level only):
```json
{
  "start": 0.6,
  "end": 10.04,
  "text": "Hallo und willkommen",
  "speaker": "Speaker A"
}
```

After (with word-level):
```json
{
  "start": 0.6,
  "end": 10.04,
  "text": "Hallo und willkommen",
  "speaker": "Speaker A",
  "words": [
    {"word": "Hallo", "start": 0.6, "end": 1.2},
    {"word": "und", "start": 1.3, "end": 1.5},
    {"word": "willkommen", "start": 1.6, "end": 2.4}
  ]
}
```

## Next Steps

After adding word-level timestamps:
1. Re-run preparation script: `python3 scripts/prepare_german_dataset.py --dataset Gemischtes.Hack.Podcast --episodes 150`
2. Verify improved accuracy: `python3 scripts/verify_podcasts_enhanced.py /path/to/output --max-files 10`