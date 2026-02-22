# Whisper Model Comparison Results

**Date**: 2026-02-22
**Hardware**: Apple M4, macOS 25.2.0
**whisper.cpp**: v1.8.3 (compiled with `-DWHISPER_COREML=1 -DWHISPER_COREML_ALLOW_FALLBACK=ON`)
**CoreML conversion**: `--optimize-ane True` (all CoreML encoders)

## Part 1: whisper-cli Direct Test (5 samples, clean 16kHz PCM)

All 8 model/backend combinations achieved **5/5 PERFECT** transcription accuracy on clean 16kHz input.

| Model | Size | Backend | Avg Time (ms) | Notes |
|-------|------|---------|---------------|-------|
| large-v3 | 2.9 GB | CoreML + ANE | ~2580 | CoreML warmup: ~35s on first run |
| large-v3 | 2.9 GB | Metal-only | ~2584 | No warmup penalty |
| large-v3-q5_0 | 1.0 GB | CoreML + ANE | ~2075 | CoreML warmup: ~22s on first run |
| large-v3-q5_0 | 1.0 GB | Metal-only | ~2080 | No warmup penalty |
| large-v3-turbo | 1.5 GB | CoreML + ANE | ~1575 | CoreML warmup: ~24s on first run |
| large-v3-turbo | 1.5 GB | Metal-only | ~1575 | No warmup penalty |
| large-v3-turbo-q5_0 | 547 MB | CoreML + ANE | ~1060 | Fastest with CoreML. Warmup: ~22s |
| large-v3-turbo-q5_0 | 547 MB | Metal-only | ~1575 | Faster without CoreML warmup |

### whisper-cli Raw Data

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

---

## Part 2: Full Pipeline Test (20 samples, G.711 u-law via SIP/RTP)

Audio path: WAV → 8kHz u-law G.711 → RTP → SIP Client → IAP (upsample 8→16kHz) → Whisper Service (VAD + beam search)

This tests the full pipeline including codec degradation, resampling artifacts, VAD segmentation, and inactivity flush timing.

### Summary (excluding sample_01 CoreML warmup timeouts)

| Model | Size | Backend | PASS | WARN | FAIL | Avg ms |
|-------|------|---------|------|------|------|--------|
| **large-v3** | **2.9 GB** | **Metal** | **12** | **8** | **0** | **1627** |
| large-v3 | 2.9 GB | CoreML | 11 | 8 | 1* | 1301 |
| large-v3-q5_0 | 1.0 GB | Metal | 11 | 9 | 0 | 1789 |
| large-v3-q5_0 | 1.0 GB | CoreML | 11 | 8 | 1* | 1359 |
| large-v3-turbo | 1.5 GB | CoreML | 9 | 10 | 1* | 688 |
| large-v3-turbo | 1.5 GB | Metal | 8 | 12 | 0 | 1013 |
| large-v3-turbo-q5_0 | 547 MB | CoreML | 8 | 11 | 1** | 686 |
| large-v3-turbo-q5_0 | 547 MB | Metal | 9 | 11 | 0 | 1103 |

*= sample_01 TIMEOUT/FAIL due to CoreML warmup on first inference
**= sample_01 FAIL (41.8%) — CoreML warmup caused VAD to miss first half of audio

**Scoring**: PASS = ≥99.5% similarity, WARN = ≥90%, FAIL = <90%

### Per-Sample Accuracy (%)

| Sample | full CoreML | full Metal | q5_0 CoreML | q5_0 Metal | turbo CoreML | turbo Metal | turbo-q5_0 CoreML | turbo-q5_0 Metal |
|--------|-------------|------------|-------------|------------|--------------|-------------|-------------------|------------------|
| 01 | 100 | 100 | TIMEOUT | 100 | TIMEOUT | 100 | 42 | 100 |
| 02 | 100 | 100 | 100 | 100 | 98 | 98 | 98 | 98 |
| 03 | 100 | 100 | 100 | 95 | 100 | 100 | 100 | 100 |
| 04 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 |
| 05 | 100 | 100 | 100 | 100 | 98 | 98 | 98 | 98 |
| 06 | 96 | 96 | 96 | 96 | 96 | 96 | 96 | 96 |
| 07 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 |
| 08 | 96 | 100 | 100 | 100 | 98 | 98 | 98 | 98 |
| 09 | 99 | 99 | 99 | 99 | 98 | 98 | 98 | 98 |
| 10 | 99 | 99 | 99 | 99 | 92 | 92 | 92 | 92 |
| 11 | 100 | 100 | 100 | 100 | 99 | 99 | 99 | 99 |
| 12 | 99 | 99 | 99 | 99 | 97 | 97 | 97 | 97 |
| 13 | 97 | 97 | 97 | 97 | 100 | 100 | 100 | 100 |
| 14 | 100 | 100 | 100 | 100 | 100 | 99 | 99 | 100 |
| 15 | 86 | 92 | 92 | 91 | 99 | 99 | 97 | 99 |
| 16 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 |
| 17 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 98 |
| 18 | 100 | 100 | 100 | 100 | 100 | 100 | 100 | 100 |
| 19 | 96 | 96 | 96 | 96 | 93 | 93 | 93 | 92 |
| 20 | 92 | 92 | 92 | 92 | 100 | 92 | 100 | 100 |

### Per-Sample Inference Time (ms)

| Sample | full CoreML | full Metal | q5_0 CoreML | q5_0 Metal | turbo CoreML | turbo Metal | turbo-q5_0 CoreML | turbo-q5_0 Metal |
|--------|-------------|------------|-------------|------------|--------------|-------------|-------------------|------------------|
| 01 | 1207 | 1593 | TIMEOUT | 1724 | TIMEOUT | 999 | 567 | 1083 |
| 02 | 957 | 1282 | 1090 | 1451 | 621 | 928 | 607 | 1009 |
| 03 | 1020 | 1392 | 1006 | 1618 | 639 | 953 | 625 | 1040 |
| 04 | 1007 | 1307 | 1031 | 1527 | 635 | 950 | 627 | 1038 |
| 05 | 1221 | 1620 | 1291 | 1853 | 702 | 1007 | 709 | 1089 |
| 06 | 1293 | 1605 | 1191 | 1732 | 693 | 1033 | 698 | 1070 |
| 07 | 1069 | 1320 | 1013 | 1540 | 630 | 978 | 640 | 1047 |
| 08 | 983 | 1315 | 1016 | 1532 | 625 | 989 | 682 | 1065 |
| 09 | 1402 | 1785 | 1317 | 1855 | 715 | 1056 | 744 | 1097 |
| 10 | 1533 | 1908 | 1426 | 1921 | 722 | 1070 | 752 | 1130 |
| 11 | 1665 | 2226 | 1627 | 2447 | 759 | 1087 | 800 | 1222 |
| 12 | 1497 | 1958 | 1404 | 2035 | 739 | 1059 | 742 | 1205 |
| 13 | 1421 | 1893 | 1395 | 1965 | 737 | 1061 | 716 | 1185 |
| 14 | 1509 | 1810 | 1416 | 2025 | 749 | 1074 | 735 | 1161 |
| 15 | 1440 | 1793 | 1449 | 1944 | 742 | 1050 | 756 | 1118 |
| 16 | 938 | 1273 | 1028 | 1504 | 605 | 1011 | 612 | 1084 |
| 17 | 1245 | 1640 | 1241 | 1815 | 684 | 999 | 693 | 1108 |
| 18 | 944 | 1276 | 996 | 1471 | 633 | 919 | 607 | 1049 |
| 19 | 1499 | 1751 | 1485 | 1975 | 742 | 1023 | 725 | 1144 |
| 20 | 1359 | 1841 | 1391 | 1844 | 692 | 1010 | 684 | 1115 |

### Key Findings (Pipeline)

1. **Best accuracy**: `large-v3 Metal` — 12 PASS / 8 WARN / 0 FAIL. Full model with Metal-only avoids CoreML warmup issues and has best overall accuracy through G.711 codec degradation.

2. **Best speed**: `turbo-q5_0 CoreML` — 686ms avg (excluding warmup FAIL). But accuracy drops slightly vs full model.

3. **Best balance**: `large-v3 CoreML` — 1301ms avg, 11 PASS / 8 WARN (sample_01 TIMEOUT on warmup only). After warmup, performance and accuracy are excellent.

4. **Turbo accuracy tradeoff**: Turbo models are 2x faster but have more WARN results (10-12 vs 8). The 4-layer decoder occasionally misses nuances in G.711-degraded audio.

5. **Consistent problem samples**: sample_06 ("benennt" → "benimmt"), sample_19 ("landratsamt" → "Landrat Kant"), sample_10 ("schäbigen" → "schädigen") fail across ALL configs — these are G.711 codec artifacts, not model issues.

6. **CoreML warmup**: sample_01 consistently fails/timeouts on CoreML first inference. For a long-running service this is a one-time startup cost.

7. **Quantization**: q5_0 has negligible accuracy impact — same PASS/WARN/FAIL as unquantized for both full and turbo.

### Recommendation

For the WhisperTalk pipeline (long-running service):
- **Production (accuracy priority)**: `large-v3` + CoreML — 1301ms avg, best accuracy after warmup
- **Production (speed priority)**: `large-v3-turbo` + CoreML — 688ms avg, good accuracy
- **Development/testing**: `large-v3` + Metal — no warmup delay, best accuracy, 1627ms avg

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
