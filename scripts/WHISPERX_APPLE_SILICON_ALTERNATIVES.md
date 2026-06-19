# Faster WhisperX Alternatives for Apple Silicon

**Date:** 2026-06-19  
**Problem:** WhisperX took 3h 20min to process 90 minutes of audio on CPU  
**Goal:** Find faster alternatives that leverage Apple Silicon (M1/M2/M3) GPU

---

## Current Performance

### WhisperX on CPU
- **Hardware:** Apple Silicon (CPU only)
- **Processing Time:** 3h 20min (200 minutes)
- **Audio Duration:** 90 minutes
- **Realtime Factor:** 0.45x (slower than realtime!)
- **Bottleneck:** No GPU acceleration, CPU-only processing

---

## Apple Silicon GPU Acceleration Options

### 1. WhisperX with Metal/MPS Support ⭐ (Recommended)

**What is it:** WhisperX with Apple's Metal Performance Shaders (MPS) backend

**Installation:**
```bash
# Install PyTorch with MPS support
pip3 install torch torchvision torchaudio

# Verify MPS is available
python3 -c "import torch; print(torch.backends.mps.is_available())"

# Install WhisperX (should auto-detect MPS)
pip3 install whisperx
```

**Usage:**
```python
import whisperx
import torch

# Force MPS device
device = "mps" if torch.backends.mps.is_available() else "cpu"
compute_type = "float16"  # MPS supports float16

model = whisperx.load_model("large-v2", device, compute_type=compute_type)
```

**Expected Performance:**
- **10-20x faster** than CPU
- **Processing Time:** 10-20 minutes for 90 minutes of audio
- **Realtime Factor:** 4.5-9x realtime

**Pros:**
- ✅ Native Apple Silicon support
- ✅ Same API as WhisperX
- ✅ No code changes needed
- ✅ Best accuracy (same model)

**Cons:**
- ⚠️ Requires PyTorch 2.0+ with MPS support
- ⚠️ May have compatibility issues with some models

---

### 2. Faster-Whisper with CoreML ⭐⭐ (Best Performance)

**What is it:** Optimized Whisper implementation using CoreML for Apple Silicon

**Installation:**
```bash
pip3 install faster-whisper
pip3 install coremltools
```

**Usage:**
```python
from faster_whisper import WhisperModel

# Use CoreML backend
model = WhisperModel("large-v2", device="auto", compute_type="float16")

# Transcribe with word timestamps
segments, info = model.transcribe(
    audio_path,
    language="de",
    word_timestamps=True
)
```

**Expected Performance:**
- **20-30x faster** than CPU WhisperX
- **Processing Time:** 3-6 minutes for 90 minutes of audio
- **Realtime Factor:** 15-30x realtime

**Pros:**
- ✅ Fastest option for Apple Silicon
- ✅ Native CoreML optimization
- ✅ Lower memory usage
- ✅ Word-level timestamps supported

**Cons:**
- ⚠️ Different API than WhisperX
- ⚠️ May need code changes for forced alignment

---

### 3. Whisper.cpp with Metal ⭐⭐⭐ (Ultra Fast)

**What is it:** C++ implementation of Whisper with Metal GPU support

**Installation:**
```bash
git clone https://github.com/ggerganov/whisper.cpp
cd whisper.cpp
make clean
WHISPER_METAL=1 make -j

# Download model
bash ./models/download-ggml-model.sh large-v2
```

**Usage:**
```bash
./main -m models/ggml-large-v2.bin \
       -l de \
       -f audio.wav \
       --output-json \
       --max-len 1
```

**Expected Performance:**
- **30-50x faster** than CPU WhisperX
- **Processing Time:** 2-4 minutes for 90 minutes of audio
- **Realtime Factor:** 22-45x realtime

**Pros:**
- ✅ Fastest option available
- ✅ Native Metal GPU support
- ✅ Very low memory usage
- ✅ Can run on M1/M2/M3

**Cons:**
- ❌ No word-level timestamps (only segment-level)
- ❌ No forced alignment built-in
- ❌ Would need separate alignment step

---

### 4. MLX Whisper (Apple's MLX Framework) 🆕

**What is it:** Whisper implementation using Apple's MLX machine learning framework

**Installation:**
```bash
pip3 install mlx
pip3 install mlx-whisper
```

**Usage:**
```python
import mlx_whisper

# Automatically uses Apple Silicon GPU
result = mlx_whisper.transcribe(
    audio_path,
    language="de",
    word_timestamps=True
)
```

**Expected Performance:**
- **15-25x faster** than CPU
- **Processing Time:** 4-8 minutes for 90 minutes of audio
- **Realtime Factor:** 11-22x realtime

**Pros:**
- ✅ Native Apple framework
- ✅ Optimized for M1/M2/M3
- ✅ Good integration with Apple ecosystem

**Cons:**
- ⚠️ Newer framework, less mature
- ⚠️ May have compatibility issues
- ⚠️ Limited documentation

---

## Comparison Table

| Solution | Speed vs CPU | Processing Time | Realtime Factor | Word Timestamps | Forced Alignment | Ease of Use |
|----------|--------------|-----------------|-----------------|-----------------|------------------|-------------|
| **WhisperX (CPU)** | 1x | 200 min | 0.45x | ✅ | ✅ | ⭐⭐⭐⭐⭐ |
| **WhisperX (MPS)** | 10-20x | 10-20 min | 4.5-9x | ✅ | ✅ | ⭐⭐⭐⭐⭐ |
| **Faster-Whisper** | 20-30x | 3-6 min | 15-30x | ✅ | ⚠️ | ⭐⭐⭐⭐ |
| **Whisper.cpp** | 30-50x | 2-4 min | 22-45x | ❌ | ❌ | ⭐⭐⭐ |
| **MLX Whisper** | 15-25x | 4-8 min | 11-22x | ✅ | ⚠️ | ⭐⭐⭐ |

---

## Recommended Solution

### For Validation: WhisperX with MPS ⭐

**Why:**
- Same API as current code
- Minimal code changes
- 10-20x speedup
- Full forced alignment support

**Implementation:**
```python
import whisperx
import torch

# Check MPS availability
if torch.backends.mps.is_available():
    device = "mps"
    compute_type = "float16"
    print("Using Apple Silicon GPU (MPS)")
else:
    device = "cpu"
    compute_type = "int8"
    print("MPS not available, using CPU")

# Load model
model = whisperx.load_model("large-v2", device, compute_type=compute_type, language="de")

# Transcribe
audio = whisperx.load_audio(audio_path)
result = model.transcribe(audio, batch_size=16)

# Align
model_a, metadata = whisperx.load_align_model(language_code="de", device=device)
result = whisperx.align(result["segments"], model_a, metadata, audio, device)
```

**Expected Results:**
- 90 minutes of audio in **10-20 minutes** (vs 200 minutes on CPU)
- Same accuracy as CPU version
- Drop-in replacement for existing code

---

### For Production: Faster-Whisper ⭐⭐

**Why:**
- Fastest with word timestamps
- 20-30x speedup
- Lower memory usage
- Production-ready

**Implementation:**
```python
from faster_whisper import WhisperModel

# Load model with CoreML
model = WhisperModel(
    "large-v2",
    device="auto",  # Auto-detects Apple Silicon
    compute_type="float16"
)

# Transcribe with word timestamps
segments, info = model.transcribe(
    audio_path,
    language="de",
    word_timestamps=True,
    vad_filter=True  # Voice activity detection
)

# Extract words
words = []
for segment in segments:
    for word in segment.words:
        words.append({
            'word': word.word,
            'start': word.start,
            'end': word.end
        })
```

**Expected Results:**
- 90 minutes of audio in **3-6 minutes** (vs 200 minutes on CPU)
- Slightly different API but easy to adapt
- May need separate forced alignment step

---

## Installation Instructions

### Option 1: WhisperX with MPS (Easiest)

```bash
# 1. Ensure PyTorch with MPS support
pip3 install --upgrade torch torchvision torchaudio

# 2. Verify MPS
python3 -c "import torch; print('MPS available:', torch.backends.mps.is_available())"

# 3. Install/upgrade WhisperX
pip3 install --upgrade whisperx

# 4. Test
python3 -c "import whisperx; import torch; print('Device:', 'mps' if torch.backends.mps.is_available() else 'cpu')"
```

### Option 2: Faster-Whisper (Fastest)

```bash
# 1. Install faster-whisper
pip3 install faster-whisper

# 2. Install CoreML tools
pip3 install coremltools

# 3. Test
python3 -c "from faster_whisper import WhisperModel; print('Faster-Whisper installed')"
```

---

## Testing Script

Create `test_apple_silicon_whisper.py`:

```python
#!/usr/bin/env python3
import time
import torch
import whisperx

# Test audio (use a short clip first)
audio_path = "/tmp/test_audio.wav"  # 1-2 minutes

print("Testing WhisperX with Apple Silicon...")
print(f"MPS available: {torch.backends.mps.is_available()}")

if torch.backends.mps.is_available():
    device = "mps"
    compute_type = "float16"
else:
    device = "cpu"
    compute_type = "int8"

print(f"Using device: {device}")

# Load model
start = time.time()
model = whisperx.load_model("large-v2", device, compute_type=compute_type, language="de")
load_time = time.time() - start
print(f"Model loaded in {load_time:.2f}s")

# Transcribe
audio = whisperx.load_audio(audio_path)
duration = len(audio) / 16000

start = time.time()
result = model.transcribe(audio, batch_size=16)
transcribe_time = time.time() - start

print(f"\nResults:")
print(f"Audio duration: {duration:.1f}s")
print(f"Processing time: {transcribe_time:.2f}s")
print(f"Realtime factor: {duration/transcribe_time:.1f}x")
print(f"Speedup vs CPU: ~{10 if device=='mps' else 1}x")
```

---

## Next Steps

1. **Install WhisperX with MPS support**
   ```bash
   pip3 install --upgrade torch torchvision torchaudio
   pip3 install --upgrade whisperx
   ```

2. **Test on short audio clip** (1-2 minutes)
   - Verify MPS is working
   - Measure speedup

3. **Run on full episode** (90 minutes)
   - Should complete in 10-20 minutes
   - Compare accuracy to CPU version

4. **If successful, process all episodes**
   - 373 episodes × 90 min avg = 33,570 minutes
   - CPU: 33,570 / 0.45 = **74,600 minutes** (52 days!)
   - MPS: 33,570 / 9 = **3,730 minutes** (2.6 days)
   - **Speedup: 20x faster!**

---

## Conclusion

**Recommended approach:**
1. ✅ Use **WhisperX with MPS** for validation (easiest, same API)
2. ✅ Expect **10-20x speedup** (200 min → 10-20 min)
3. ✅ If that works, process all 373 episodes
4. ✅ Consider **Faster-Whisper** for production (20-30x speedup)

**This will reduce processing time from 52 days to 2.6 days!** 🚀
