# Waveform Alignment Algorithm: Research & Upgrade Proposals

## Current V3 Algorithm Analysis

### Architecture Overview
The V3 algorithm uses a **smoothed energy pattern matching** approach:

1. **Transcript Pattern Generation** (Lines 11-29)
   - Creates binary pattern: speech=1.0, silence=0.0
   - Sample rate: 2 Hz (500ms resolution)
   - Gaussian smoothing: σ=0.1s (50ms)
   - Output: Smoothed envelope representing speech regions

2. **Audio Pattern Generation** (Lines 32-62)
   - RMS energy calculation: 500ms windows
   - Normalization: 0-1 range
   - Binary thresholding: 0.3 (captures ~88% speech)
   - Gaussian smoothing: σ=0.1s to match transcript
   - Output: Smoothed energy envelope

3. **Alignment via Cross-Correlation** (Lines 65-150)
   - Window: 30s of transcript pattern
   - Search: First 120s of audio
   - Step size: 1s (2 samples at 2 Hz)
   - Metric: Pearson correlation coefficient
   - Output: Best offset with correlation score

### Performance Characteristics

**Strengths:**
- ✅ Achieves 0.05-0.5% duration accuracy
- ✅ Fast processing: 2 Hz sample rate
- ✅ Robust to noise via Gaussian smoothing
- ✅ Simple, interpretable algorithm

**Bottlenecks:**
- ⚠️ RMS calculation: O(n) for each 500ms window
- ⚠️ Cross-correlation: O(m×n) where m=search_range, n=window_size
- ⚠️ Gaussian filtering: O(n×k) where k=kernel_size
- ⚠️ Sequential processing: No parallelization

**Current Timing (90-minute audio):**
- Pattern generation: ~2-3 seconds
- Cross-correlation: ~1-2 seconds
- Total: ~5 seconds per episode

---

## Research: Fast Audio Pattern Creation Techniques

### 1. Spectral Features (MFCC/Mel-Spectrogram)
**Concept:** Use frequency-domain features instead of time-domain energy

**Advantages:**
- More discriminative than RMS energy
- Captures timbral characteristics
- Better for speech/music distinction
- Standard in audio ML pipelines

**Implementation:**
```python
import librosa

# Fast mel-spectrogram computation
mel_spec = librosa.feature.melspectrogram(
    y=audio, sr=sr, 
    n_fft=2048, hop_length=sr//2,  # 500ms hops
    n_mels=40  # Compact representation
)
# Aggregate to single energy value per frame
energy = np.mean(mel_spec, axis=0)
```

**Speed:** ~2-3x faster than RMS (FFT is highly optimized)

### 2. Downsampling + Energy Calculation
**Concept:** Downsample audio first, then calculate energy

**Advantages:**
- Reduces computation by factor of downsample_ratio
- Maintains essential energy characteristics
- Minimal quality loss for alignment purposes

**Implementation:**
```python
from scipy import signal

# Downsample to 8kHz (from 48kHz = 6x reduction)
audio_ds = signal.decimate(audio, 6, ftype='fir')
sr_ds = sr // 6

# Calculate energy on downsampled audio
window_size = int(0.5 * sr_ds)  # 500ms
energy = []
for i in range(0, len(audio_ds) - window_size, window_size):
    window = audio_ds[i:i+window_size]
    energy.append(np.sqrt(np.mean(window**2)))
```

**Speed:** ~6x faster (proportional to downsample ratio)

### 3. FFT-Based Cross-Correlation
**Concept:** Use FFT for correlation instead of direct computation

**Advantages:**
- O(n log n) instead of O(n²)
- Highly optimized implementations (FFTW, cuFFT)
- Can leverage GPU acceleration

**Implementation:**
```python
from scipy import signal

# FFT-based correlation (much faster)
correlation = signal.correlate(
    audio_pattern, 
    transcript_pattern, 
    mode='valid', 
    method='fft'  # Use FFT instead of direct
)
best_offset = np.argmax(correlation)
```

**Speed:** ~10-100x faster for large patterns

### 4. Multi-Resolution Pyramid
**Concept:** Coarse-to-fine alignment using multiple resolutions

**Advantages:**
- Fast initial alignment at low resolution
- Refinement at higher resolution only near best match
- Reduces search space exponentially

**Implementation:**
```python
# Level 1: 0.5 Hz (2s resolution) - fast search
coarse_offset = align_at_resolution(audio, transcript, sr=0.5)

# Level 2: 2 Hz (500ms) - refine around coarse_offset
search_range = (coarse_offset - 10, coarse_offset + 10)
medium_offset = align_at_resolution(audio, transcript, sr=2, search_range)

# Level 3: 10 Hz (100ms) - final refinement
search_range = (medium_offset - 2, medium_offset + 2)
fine_offset = align_at_resolution(audio, transcript, sr=10, search_range)
```

**Speed:** ~5-10x faster (reduces search space)

### 5. Vectorized Operations (NumPy/CuPy)
**Concept:** Replace loops with vectorized operations

**Advantages:**
- Leverages SIMD instructions
- Can use GPU (CuPy)
- Minimal code changes

**Implementation:**
```python
# Instead of loop for RMS:
# for i in range(0, len(audio) - window_size, hop):
#     window = audio[i:i+window_size]
#     rms = np.sqrt(np.mean(window**2))

# Vectorized approach:
from numpy.lib.stride_tricks import sliding_window_view

windows = sliding_window_view(audio, window_size)[::hop]
rms = np.sqrt(np.mean(windows**2, axis=1))
```

**Speed:** ~3-5x faster (SIMD optimization)

### 6. Caching & Preprocessing
**Concept:** Pre-compute and cache audio patterns

**Advantages:**
- Amortizes cost across multiple alignments
- Enables batch processing
- Reduces redundant computation

**Implementation:**
```python
import pickle
import hashlib

def get_audio_pattern_cached(audio_path, sr):
    # Generate cache key from file hash
    with open(audio_path, 'rb') as f:
        file_hash = hashlib.md5(f.read()).hexdigest()
    
    cache_path = f"/tmp/audio_patterns/{file_hash}.pkl"
    
    if os.path.exists(cache_path):
        with open(cache_path, 'rb') as f:
            return pickle.load(f)
    
    # Compute pattern
    audio, sr = librosa.load(audio_path, sr=sr)
    pattern = generate_audio_pattern(audio, sr)
    
    # Cache for future use
    with open(cache_path, 'wb') as f:
        pickle.dump(pattern, f)
    
    return pattern
```

**Speed:** Instant for cached files

---

## Proposed Upgrades

### Upgrade 1: FFT-Based Correlation with Downsampling
**Priority:** HIGH  
**Expected Speedup:** 10-20x  
**Complexity:** LOW

**Changes:**
1. Downsample audio to 8kHz before pattern generation
2. Replace direct correlation with FFT-based correlation
3. Use vectorized RMS calculation

**Implementation:**
```python
def generate_audio_pattern_fast(audio, sr, target_sr=2):
    """Fast audio pattern using downsampling + FFT."""
    from scipy import signal
    
    # Downsample to 8kHz (6x reduction from 48kHz)
    downsample_factor = sr // 8000
    if downsample_factor > 1:
        audio = signal.decimate(audio, downsample_factor, ftype='fir')
        sr = sr // downsample_factor
    
    # Vectorized RMS calculation
    window_size = int(0.5 * sr)
    hop = window_size
    
    from numpy.lib.stride_tricks import sliding_window_view
    windows = sliding_window_view(audio, window_size)[::hop]
    energy = np.sqrt(np.mean(windows**2, axis=1))
    
    # Normalize and threshold
    if len(energy) > 0 and energy.max() > 0:
        energy = energy / energy.max()
    
    binary_pattern = (energy > 0.3).astype(np.float32)
    smoothed = gaussian_filter1d(binary_pattern, sigma=max(0.2, target_sr*0.05))
    
    return smoothed

def align_audio_to_transcript_fast(audio, sr, transcript_segments):
    """Fast alignment using FFT-based correlation."""
    from scipy import signal as scipy_signal
    
    # Generate patterns
    transcript_pattern = generate_transcript_pattern(transcript_segments, ...)
    audio_pattern = generate_audio_pattern_fast(audio, sr)
    
    # FFT-based correlation (much faster)
    window_size = int(30.0 * pattern_sr)
    pattern_window = transcript_pattern[:window_size]
    
    # Search in first 2 minutes
    max_search = min(int(120 * pattern_sr), len(audio_pattern) - window_size)
    search_audio = audio_pattern[:max_search + window_size]
    
    # Use FFT correlation
    correlation = scipy_signal.correlate(
        search_audio, 
        pattern_window, 
        mode='valid',
        method='fft'
    )
    
    best_offset = np.argmax(correlation)
    best_corr = correlation[best_offset] / (np.linalg.norm(pattern_window) * 
                                            np.linalg.norm(search_audio[best_offset:best_offset+window_size]))
    
    return best_offset, best_corr
```

**Benefits:**
- 10-20x faster pattern generation
- 10-100x faster correlation
- Maintains accuracy (downsampling to 8kHz preserves speech)
- Drop-in replacement for V3

**Testing Required:**
- Verify accuracy on test episodes (150, 151, 152)
- Benchmark speed improvement
- Validate correlation scores remain meaningful

---

### Upgrade 2: Multi-Resolution Pyramid Alignment
**Priority:** MEDIUM  
**Expected Speedup:** 5-10x  
**Complexity:** MEDIUM

**Changes:**
1. Implement 3-level pyramid: 0.5 Hz → 2 Hz → 10 Hz
2. Coarse search at low resolution
3. Refinement at higher resolutions

**Implementation:**
```python
def align_multi_resolution(audio, sr, transcript_segments):
    """Multi-resolution pyramid alignment."""
    
    # Level 1: Coarse alignment (0.5 Hz = 2s resolution)
    print("  Level 1: Coarse alignment (2s resolution)...")
    coarse_offset, coarse_corr = align_at_resolution(
        audio, sr, transcript_segments,
        pattern_sr=0.5,
        search_range=(0, 120),  # Search 0-120s
        step=2.0  # 2s steps
    )
    
    if coarse_corr < 0.2:
        print("  WARNING: Low coarse correlation!")
        return audio, [(0, len(audio)/sr)]
    
    # Level 2: Medium refinement (2 Hz = 500ms resolution)
    print(f"  Level 2: Medium refinement around {coarse_offset:.1f}s...")
    search_start = max(0, coarse_offset - 10)
    search_end = min(120, coarse_offset + 10)
    
    medium_offset, medium_corr = align_at_resolution(
        audio, sr, transcript_segments,
        pattern_sr=2.0,
        search_range=(search_start, search_end),
        step=0.5  # 500ms steps
    )
    
    # Level 3: Fine refinement (10 Hz = 100ms resolution)
    print(f"  Level 3: Fine refinement around {medium_offset:.1f}s...")
    search_start = max(0, medium_offset - 2)
    search_end = min(120, medium_offset + 2)
    
    fine_offset, fine_corr = align_at_resolution(
        audio, sr, transcript_segments,
        pattern_sr=10.0,
        search_range=(search_start, search_end),
        step=0.1  # 100ms steps
    )
    
    print(f"  Final alignment: {fine_offset:.2f}s (correlation={fine_corr:.3f})")
    
    # Extract audio from fine_offset to end
    start_time = fine_offset
    end_time = len(audio) / sr
    
    return audio[int(start_time*sr):], [(start_time, end_time)]
```

**Benefits:**
- 5-10x faster (reduced search space)
- Higher precision (100ms vs 500ms)
- More robust (multi-scale matching)
- Better for difficult alignments

**Testing Required:**
- Verify 100ms precision improves MFA accuracy
- Test on episodes with ads/music
- Benchmark speed vs V3

---

### Upgrade 3: Mel-Spectrogram Features + GPU Acceleration
**Priority:** LOW (for future)  
**Expected Speedup:** 20-50x (with GPU)  
**Complexity:** HIGH

**Changes:**
1. Replace RMS energy with mel-spectrogram features
2. Use librosa for fast feature extraction
3. Optional: CuPy for GPU acceleration

**Implementation:**
```python
def generate_audio_pattern_spectral(audio, sr, target_sr=2):
    """Generate pattern using mel-spectrogram features."""
    import librosa
    
    # Compute mel-spectrogram (fast with librosa)
    hop_length = int(0.5 * sr)  # 500ms hops
    mel_spec = librosa.feature.melspectrogram(
        y=audio, 
        sr=sr,
        n_fft=2048,
        hop_length=hop_length,
        n_mels=40,
        fmin=80,  # Focus on speech frequencies
        fmax=8000
    )
    
    # Convert to dB and aggregate
    mel_spec_db = librosa.power_to_db(mel_spec, ref=np.max)
    
    # Aggregate across frequency bins (mean or max)
    energy = np.mean(mel_spec_db, axis=0)
    
    # Normalize to 0-1
    energy = (energy - energy.min()) / (energy.max() - energy.min())
    
    # Threshold and smooth
    binary_pattern = (energy > 0.3).astype(np.float32)
    smoothed = gaussian_filter1d(binary_pattern, sigma=max(0.2, target_sr*0.05))
    
    return smoothed

# Optional: GPU acceleration with CuPy
def generate_audio_pattern_gpu(audio, sr, target_sr=2):
    """GPU-accelerated pattern generation using CuPy."""
    import cupy as cp
    
    # Transfer to GPU
    audio_gpu = cp.asarray(audio)
    
    # Vectorized RMS on GPU
    window_size = int(0.5 * sr)
    hop = window_size
    
    # Sliding window on GPU
    windows = cp.lib.stride_tricks.sliding_window_view(audio_gpu, window_size)[::hop]
    energy = cp.sqrt(cp.mean(windows**2, axis=1))
    
    # Normalize and threshold on GPU
    energy = energy / cp.max(energy)
    binary_pattern = (energy > 0.3).astype(cp.float32)
    
    # Transfer back to CPU for Gaussian filter
    binary_pattern_cpu = cp.asnumpy(binary_pattern)
    smoothed = gaussian_filter1d(binary_pattern_cpu, sigma=max(0.2, target_sr*0.05))
    
    return smoothed
```

**Benefits:**
- More discriminative features (better for music/speech)
- 20-50x faster with GPU
- Industry-standard approach
- Enables ML-based improvements

**Requirements:**
- librosa (already used in project)
- CuPy (optional, for GPU)
- CUDA-capable GPU (optional)

**Testing Required:**
- Verify mel-spectrogram improves accuracy
- Benchmark CPU vs GPU performance
- Test on diverse audio (music, ads, speech)

---

## Recommended Implementation Order

### Phase 1: Quick Wins (Week 1)
**Upgrade 1: FFT + Downsampling**
- Implement vectorized RMS calculation
- Add FFT-based correlation
- Add downsampling to 8kHz
- Test on 3 episodes
- Expected: 10-20x speedup, maintain accuracy

### Phase 2: Precision Improvement (Week 2)
**Upgrade 2: Multi-Resolution Pyramid**
- Implement 3-level pyramid
- Test 100ms precision impact on MFA
- Validate on 10 episodes
- Expected: 5-10x speedup, better precision

### Phase 3: Advanced Features (Future)
**Upgrade 3: Spectral Features + GPU**
- Implement mel-spectrogram features
- Add GPU acceleration (optional)
- Test on difficult cases (music, ads)
- Expected: 20-50x speedup with GPU

---

## Performance Projections

### Current V3 Performance
- **Single episode (90 min):** ~5 seconds
- **373 episodes:** ~30 minutes
- **Accuracy:** 0.05-0.5% duration difference

### After Upgrade 1 (FFT + Downsampling)
- **Single episode:** ~0.5 seconds (10x faster)
- **373 episodes:** ~3 minutes (10x faster)
- **Accuracy:** Same (0.05-0.5%)

### After Upgrade 2 (Multi-Resolution)
- **Single episode:** ~0.1 seconds (50x faster)
- **373 episodes:** ~37 seconds (50x faster)
- **Accuracy:** Better (100ms precision)

### After Upgrade 3 (Spectral + GPU)
- **Single episode:** ~0.02 seconds (250x faster)
- **373 episodes:** ~7 seconds (250x faster)
- **Accuracy:** Best (robust to music/ads)

---

## Conclusion

The V3 algorithm is solid and achieves excellent accuracy. The proposed upgrades focus on:

1. **Speed:** 10-250x faster processing
2. **Precision:** 500ms → 100ms alignment
3. **Robustness:** Better handling of music/ads

**Recommended:** Start with Upgrade 1 (FFT + Downsampling) for immediate 10-20x speedup with minimal risk.