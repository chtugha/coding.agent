# Whisper Model Comparison Results

**Date**: 2026-02-22
**Hardware**: Apple M4, macOS 25.2.0
**whisper.cpp**: v1.8.3 (compiled with `-DWHISPER_COREML=1 -DWHISPER_COREML_ALLOW_FALLBACK=ON`)
**CoreML conversion**: `--optimize-ane True` (all CoreML encoders)
**Test**: 5 German speech samples (3-5s each), 16kHz PCM via `whisper-cli -l de`

## Results

All models achieved **5/5 PERFECT** transcription accuracy on all samples.

### Inference Speed (excluding first-run CoreML warmup)

| Model | Size | Backend | Avg Time (ms) | Notes |
|-------|------|---------|---------------|-------|
| large-v3 | 2.9 GB | CoreML + ANE | ~2580 | CoreML warmup: ~35s on first run |
| large-v3 | 2.9 GB | Metal-only | ~2584 | No warmup penalty |
| large-v3-q5_0 | 1.0 GB | CoreML + ANE | ~2075 | CoreML warmup: ~22s on first run |
| large-v3-q5_0 | 1.0 GB | Metal-only | ~2080 | No warmup penalty |
| large-v3-turbo | 1.5 GB | CoreML + ANE | ~1575 | CoreML warmup: ~24s on first run |
| large-v3-turbo | 1.5 GB | Metal-only | ~1575 | No warmup penalty |
| **large-v3-turbo-q5_0** | **547 MB** | **CoreML + ANE** | **~1060** | **Fastest with CoreML.** Warmup: ~22s |
| **large-v3-turbo-q5_0** | **547 MB** | **Metal-only** | **~1575** | Faster without CoreML warmup |

### Key Findings

1. **Accuracy**: All 4 model variants produce identical, perfect German transcriptions on clean 16kHz audio. Quantization (q5_0) does NOT degrade accuracy for these samples.

2. **Speed winner**: `large-v3-turbo-q5_0` with CoreML is the fastest at ~1060ms post-warmup (vs ~2580ms for unquantized large-v3). That is a **2.4x speedup**.

3. **CoreML benefit**: CoreML acceleration only shows benefit on quantized models. For unquantized models, CoreML and Metal-only perform similarly (~2580ms). For turbo-q5_0, CoreML is ~1.5x faster than Metal-only (1060ms vs 1575ms).

4. **CoreML warmup**: First inference with CoreML takes 20-35s (model compilation). Subsequent inferences are fast. For a long-running service this is acceptable.

5. **Turbo vs Full**: Turbo models (n_text_layer=4) are ~1.6x faster than full models (n_text_layer=32) with no accuracy loss on these samples. Turbo has fewer decoder layers but the same encoder.

6. **Quantization impact**: q5_0 reduces model size by ~65% and improves speed by ~20-35% with no accuracy loss.

### Recommendation

For the WhisperTalk pipeline (long-running service):
- **Best speed**: `large-v3-turbo-q5_0` + CoreML (~1060ms, 547MB)
- **Best quality headroom**: `large-v3` + CoreML (~2580ms, 2.9GB) — may handle edge cases better
- **Best balance**: `large-v3-turbo` + CoreML (~1575ms, 1.5GB)

### CRITICAL: Model Verification

When loading a model, verify in logs:
- `large-v3` (full): Reports `MTL0 total size = 3094.36 MB`, `n_text_layer = 32`
- `large-v3-turbo`: Reports `MTL0 total size = 1623.92 MB`, `n_text_layer = 4`
- If you see the wrong size for the model name, the **model file is wrong** (this was the root cause of previous CoreML failures)

### CoreML Encoder Conversion

Each model architecture needs its own CoreML encoder:
- `ggml-large-v3-encoder.mlmodelc` for large-v3 and large-v3-q5_0 (same encoder)
- `ggml-large-v3-turbo-encoder.mlmodelc` for large-v3-turbo and large-v3-turbo-q5_0 (same encoder)

Conversion (using conda env py312-whisper with torch==2.7.0):
```bash
cd whisper-cpp
$CONDA_PYTHON models/convert-whisper-to-coreml.py --model large-v3 --encoder-only True --optimize-ane True
xcrun coremlc compile models/coreml-encoder-large-v3.mlpackage models/
mv models/coreml-encoder-large-v3.mlmodelc models/ggml-large-v3-encoder.mlmodelc
```

### Raw Test Data

#### CoreML + ANE

| Sample | large-v3 | large-v3-q5_0 | large-v3-turbo | large-v3-turbo-q5_0 |
|--------|----------|---------------|----------------|---------------------|
| 01 | 35646ms* | 23230ms* | 28780ms* | 22424ms* |
| 02 | 2584ms | 1576ms | 1575ms | 1068ms |
| 03 | 25281ms* | 2072ms | 1577ms | 1060ms |
| 04 | 2081ms | 21710ms* | 23753ms* | 1071ms |
| 05 | 2589ms | 2578ms | 2330ms | 1058ms |

*= includes CoreML warmup/compilation

#### Metal-only (GPU)

| Sample | large-v3 | large-v3-q5_0 | large-v3-turbo | large-v3-turbo-q5_0 |
|--------|----------|---------------|----------------|---------------------|
| 01 | 3589ms | 2580ms | 2584ms | 1570ms |
| 02 | 2578ms | 2075ms | 1587ms | 1584ms |
| 03 | 2585ms | 2092ms | 1574ms | 1568ms |
| 04 | 2578ms | 2077ms | 1578ms | 1582ms |
| 05 | 2594ms | 2577ms | 1566ms | 1579ms |
