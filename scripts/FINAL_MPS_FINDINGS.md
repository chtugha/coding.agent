# Final MPS Acceleration Findings - Complete Analysis

## Critical Discoveries

### 1. WhisperX Does NOT Support MPS ❌
**Error:** `ValueError: unsupported device mps`

**Root Cause:** WhisperX uses `faster-whisper` → `ctranslate2` → only supports CPU and CUDA

### 2. PyTorch Whisper MPS Support is Broken ❌
**Error:** `NotImplementedError: Could not run 'aten::_sparse_coo_tensor_with_dims_and_tensors' with arguments from the 'SparseMPS' backend`

**Root Cause:** PyTorch's MPS backend doesn't support sparse tensor operations required by Whisper model

This is a known limitation in PyTorch 2.x MPS implementation.

## What Actually Works on Apple Silicon

### Option 1: Whisper.cpp with Metal ✅ FASTEST
**Implementation:** C++ with Metal acceleration

**Performance:**
- 30-50x faster than CPU
- ~2-3 minutes per 90-minute episode
- Total for 373 episodes: ~2-3 days

**Limitations:**
- ❌ NO word-level timestamps
- Requires C++ compilation
- Need separate alignment step (MFA)

**Setup:**
```bash
git clone https://github.com/ggerganov/whisper.cpp
cd whisper.cpp
WHISPER_METAL=1 make -j
bash ./models/download-ggml-model.sh large-v3
./main -m models/ggml-large-v3.bin -f audio.wav -l de
```

### Option 2: MLX Whisper ✅ RECOMMENDED
**Implementation:** Apple's MLX framework (optimized for Apple Silicon)

**Performance:**
- 15-25x faster than CPU
- ~4-6 minutes per 90-minute episode
- Total for 373 episodes: ~3-4 days

**Advantages:**
- ✅ Python interface (easy to use)
- ✅ Native Apple Silicon optimization
- ✅ Active development by Apple
- ❌ NO word-level timestamps (need MFA)

**Setup:**
```bash
pip install mlx-whisper

# Python usage
import mlx_whisper
result = mlx_whisper.transcribe(
    audio_path,
    path_or_hf_repo="mlx-community/whisper-large-v3-mlx",
    language="de"
)
```

### Option 3: WhisperX on CPU ✅ SLOWEST BUT COMPLETE
**Implementation:** WhisperX with CPU backend

**Performance:**
- No acceleration
- ~200 minutes per 90-minute episode
- Total for 373 episodes: ~52 days

**Advantages:**
- ✅ Word-level timestamps included
- ✅ All features work
- ✅ No additional alignment needed

**Disadvantages:**
- ❌ Very slow (no GPU acceleration)

## Recommended Solution: MLX Whisper + MFA

### Why MLX Whisper?

1. **Python-based** - Easy integration with existing pipeline
2. **Apple-optimized** - Built specifically for Apple Silicon
3. **Good performance** - 15-25x speedup
4. **Stable** - Maintained by Apple
5. **Simple setup** - Just `pip install mlx-whisper`

### Complete Pipeline

**Step 1: Transcribe with MLX Whisper**
```python
import mlx_whisper
import json

def transcribe_episode(audio_path, output_path):
    result = mlx_whisper.transcribe(
        audio_path,
        path_or_hf_repo="mlx-community/whisper-large-v3-mlx",
        language="de",
        verbose=False
    )
    
    with open(output_path, 'w') as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
    
    return result
```

**Estimated time:** 4-6 minutes per 90-minute episode

**Step 2: Add word timestamps with MFA**
```python
import subprocess

def align_with_mfa(audio_path, transcript_path, output_dir):
    # Create MFA corpus
    corpus_dir = "mfa_corpus"
    # ... setup code ...
    
    cmd = [
        "mfa", "align",
        corpus_dir,
        "german_mfa",
        "german_mfa",
        output_dir,
        "--clean"
    ]
    subprocess.run(cmd, check=True)
```

**Estimated time:** 5-10 minutes per episode

**Total per episode:** 9-16 minutes  
**Total for 373 episodes:** 3-4 days

## Performance Comparison Table

| Solution | Transcription | Alignment | Total/Episode | Total/373 | Word Timestamps |
|----------|--------------|-----------|---------------|-----------|-----------------|
| WhisperX CPU | 200 min | Built-in | 200 min | 52 days | ✅ Yes |
| WhisperX MPS | ❌ Not supported | - | - | - | - |
| PyTorch Whisper MPS | ❌ Broken | - | - | - | - |
| **MLX Whisper + MFA** | **4-6 min** | **5-10 min** | **9-16 min** | **3-4 days** | ✅ Yes (via MFA) |
| Whisper.cpp + MFA | 2-3 min | 5-10 min | 7-13 min | 2-3 days | ✅ Yes (via MFA) |

## Implementation Plan

### Phase 1: Install MLX Whisper
```bash
pip install mlx-whisper
```

### Phase 2: Test on 10 Episodes

Create test script:
```python
#!/usr/bin/env python3
import mlx_whisper
import time
from pathlib import Path

def test_mlx_whisper(episode_num):
    audio_files = list(Path(CLEANED_DIR).glob(f"episode_{episode_num:03d}_*_cleaned.wav"))
    audio_path = str(audio_files[0])
    
    start = time.time()
    result = mlx_whisper.transcribe(
        audio_path,
        path_or_hf_repo="mlx-community/whisper-large-v3-mlx",
        language="de"
    )
    elapsed = time.time() - start
    
    print(f"Episode {episode_num}: {elapsed:.1f}s")
    return result

# Test episodes 1-10
for ep in range(1, 11):
    test_mlx_whisper(ep)
```

### Phase 3: Full Pipeline

1. Transcribe all 373 episodes with MLX Whisper
2. Run MFA alignment on all episodes
3. Validate random samples
4. Generate final dataset

## Why Not Other Options?

### CoreML Whisper
- ❌ No word timestamps
- ❌ More complex setup
- ❌ Not significantly faster than MLX

### Native PyTorch Whisper
- ❌ MPS support broken (sparse tensor issue)
- ❌ Would need CPU fallback anyway

### Faster-Whisper
- ❌ No MPS support (ctranslate2 limitation)
- ❌ Only CPU or CUDA

## Conclusion

**MLX Whisper + MFA is the best solution** for this project:

1. ✅ **Works on Apple Silicon** (unlike WhisperX MPS and PyTorch Whisper MPS)
2. ✅ **Good performance** (15-25x speedup, 3-4 days total)
3. ✅ **Python-based** (easy integration)
4. ✅ **Word timestamps** (via MFA)
5. ✅ **Proven technology** (maintained by Apple)

**Alternative:** Whisper.cpp is slightly faster (2-3 days) but requires C++ compilation and is more complex to integrate.

## Next Steps

1. Install MLX Whisper: `pip install mlx-whisper`
2. Test on 10 episodes to validate performance
3. Compare transcripts with originals
4. If successful, process all 373 episodes
5. Run MFA alignment
6. Validate final dataset quality

---

**Created:** 2026-06-19  
**Status:** WhisperX MPS and PyTorch Whisper MPS both fail on Apple Silicon  
**Solution:** Use MLX Whisper (Apple's framework) + MFA for word timestamps  
**Expected completion:** 3-4 days for 373 episodes