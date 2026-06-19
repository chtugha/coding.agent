# WhisperX MPS Limitation - Critical Finding

## Problem Discovered

**WhisperX does NOT support MPS (Metal Performance Shaders) on Apple Silicon.**

### Root Cause

WhisperX uses `faster-whisper` as its backend, which in turn uses `ctranslate2` for inference. The `ctranslate2` library only supports:
- ✅ CPU
- ✅ CUDA (NVIDIA GPUs)
- ❌ MPS (Apple Silicon)

### Error Message
```
ValueError: unsupported device mps
```

This occurs when trying to load WhisperX with `device="mps"`.

## Alternative Solutions for Apple Silicon

### Option 1: Native PyTorch Whisper with MPS ⭐ RECOMMENDED

Use OpenAI's original Whisper implementation with PyTorch MPS backend.

**Advantages:**
- ✅ Native MPS support (10-20x speedup)
- ✅ Segment-level timestamps included
- ✅ Simple implementation
- ❌ NO word-level timestamps (need separate alignment)

**Implementation:**
```python
import whisper
import torch

device = "mps" if torch.backends.mps.is_available() else "cpu"
model = whisper.load_model("large-v3", device=device)

result = model.transcribe(
    audio_path,
    language="de",
    fp16=False  # MPS doesn't support fp16
)

# result contains segments with start/end times
# Need separate alignment for word-level timestamps
```

**Then add word timestamps with:**
- Montreal Forced Aligner (MFA) - what we're currently using
- OR wav2vec2 alignment models
- OR WhisperX alignment component separately

### Option 2: Whisper.cpp with Metal Backend

Use whisper.cpp compiled with Metal support.

**Advantages:**
- ✅ Very fast (30-50x speedup)
- ✅ Native Metal acceleration
- ✅ Low memory usage
- ❌ NO word-level timestamps
- ❌ More complex setup (C++ compilation)

**Implementation:**
```bash
# Clone and build with Metal support
git clone https://github.com/ggerganov/whisper.cpp
cd whisper.cpp
make clean
WHISPER_METAL=1 make -j

# Download model
bash ./models/download-ggml-model.sh large-v3

# Transcribe
./main -m models/ggml-large-v3.bin -f audio.wav -l de
```

### Option 3: MLX Whisper (Apple's MLX Framework)

Use MLX (Apple's machine learning framework) for Whisper.

**Advantages:**
- ✅ Optimized for Apple Silicon
- ✅ Good performance
- ✅ Python interface
- ❌ NO word-level timestamps
- ⚠️ Newer, less mature

**Implementation:**
```bash
pip install mlx-whisper

# Python usage
import mlx_whisper

result = mlx_whisper.transcribe(
    audio_path,
    path_or_hf_repo="mlx-community/whisper-large-v3-mlx"
)
```

### Option 4: WhisperX on CPU (Current Fallback)

Use WhisperX with CPU, accepting slower performance.

**Advantages:**
- ✅ Word-level timestamps included
- ✅ All WhisperX features work
- ❌ Slow (no GPU acceleration)
- ❌ 373 episodes would take ~52 days

## Recommended Approach for This Project

### Best Solution: Native PyTorch Whisper + MFA

**Step 1: Transcribe with PyTorch Whisper (MPS)**
```python
import whisper
import torch

device = "mps"
model = whisper.load_model("large-v3", device=device)
result = model.transcribe(audio_path, language="de", fp16=False)
```

**Estimated time:** 3-5 minutes per 90-minute episode
**Total for 373 episodes:** ~20-30 hours (1-1.5 days)

**Step 2: Add word timestamps with MFA**
```bash
mfa align corpus_dir german_mfa german_mfa output_dir
```

**Estimated time:** 5-10 minutes per episode
**Total for 373 episodes:** ~30-60 hours (1.5-2.5 days)

**Combined total:** ~2.5-4 days for all 373 episodes

### Why This Approach?

1. **Fast transcription:** PyTorch Whisper with MPS is 10-20x faster than CPU
2. **Accurate alignment:** MFA provides precise word-level timestamps
3. **Proven workflow:** We already have MFA working successfully
4. **Reasonable total time:** 2.5-4 days vs 52 days on CPU
5. **Simple implementation:** Both tools are well-documented

## Performance Comparison

| Approach | Transcription | Alignment | Total/Episode | Total/373 Episodes |
|----------|--------------|-----------|---------------|-------------------|
| WhisperX CPU | 200 min | Built-in | 200 min | 52 days |
| WhisperX MPS | ❌ Not supported | - | - | - |
| PyTorch Whisper MPS + MFA | 3-5 min | 5-10 min | 8-15 min | 2.5-4 days |
| Whisper.cpp Metal + MFA | 2-3 min | 5-10 min | 7-13 min | 2-3.5 days |
| MLX Whisper + MFA | 4-6 min | 5-10 min | 9-16 min | 3-4.5 days |

## Implementation Plan

### Phase 1: Test PyTorch Whisper with MPS (Immediate)

Create test script to validate:
1. MPS acceleration works
2. Transcription quality matches original
3. Actual speed measurements
4. Memory usage acceptable

### Phase 2: Process 10 Test Episodes

Run full pipeline on episodes 1-10:
1. Transcribe with PyTorch Whisper (MPS)
2. Align with MFA
3. Compare with original transcripts
4. Validate word timestamp accuracy

### Phase 3: Scale to All 373 Episodes

If test results are good:
1. Process all episodes in batches
2. Monitor for errors/issues
3. Validate random samples
4. Generate final dataset

## Code Example: Complete Pipeline

```python
import whisper
import torch
import subprocess
import os

def transcribe_with_mps(audio_path, output_path):
    """Transcribe audio with PyTorch Whisper using MPS."""
    device = "mps"
    model = whisper.load_model("large-v3", device=device)
    
    result = model.transcribe(
        audio_path,
        language="de",
        fp16=False,
        verbose=False
    )
    
    # Save transcript
    with open(output_path, 'w') as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
    
    return result

def align_with_mfa(audio_path, transcript_path, output_dir):
    """Add word timestamps using MFA."""
    # Create MFA corpus
    corpus_dir = "mfa_corpus"
    os.makedirs(corpus_dir, exist_ok=True)
    
    # Copy audio and create text file
    # ... (MFA setup code)
    
    # Run MFA
    cmd = [
        "mfa", "align",
        corpus_dir,
        "german_mfa",
        "german_mfa", 
        output_dir,
        "--clean"
    ]
    subprocess.run(cmd, check=True)
    
    return parse_mfa_output(output_dir)

# Process episode
result = transcribe_with_mps("episode.wav", "transcript.json")
words = align_with_mfa("episode.wav", "transcript.json", "mfa_output")
```

## Conclusion

**WhisperX MPS was a false lead** - it doesn't actually support Apple Silicon acceleration. However, we have excellent alternatives:

1. **PyTorch Whisper + MFA** (recommended)
2. **Whisper.cpp + MFA** (fastest, more complex)
3. **MLX Whisper + MFA** (newer, promising)

All three approaches will be **10-30x faster** than CPU-only WhisperX, completing the 373 episodes in **2-4 days** instead of 52 days.

---

**Next Steps:**
1. Create test script with PyTorch Whisper + MPS
2. Run on 10 episodes to validate
3. Compare results with original transcripts
4. Proceed with full dataset if successful

**Created:** 2026-06-19  
**Issue:** WhisperX MPS not supported (ctranslate2 limitation)  
**Solution:** Use PyTorch Whisper with MPS + MFA for word timestamps