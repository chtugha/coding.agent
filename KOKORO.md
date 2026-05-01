# Kokoro C++ TTS Service — CoreML Pipeline

## Overview

The Kokoro TTS service (`kokoro-service.cpp`) is a C++ implementation of the Kokoro German text-to-speech engine, optimized for Apple Silicon via CoreML (ANE). It replaces the original Python `kokoro_service.py` with a native binary.

**Pipeline role.** As of the 2026-04 TTS redesign, Kokoro is **not** an interconnect pipeline node. It is a **dock client** that connects to the generic TTS stage (`bin/tts-service`) via a small TCP protocol on `127.0.0.1:13143`. The TTS stage owns the LLaMA↔OAP pipeline slot; Kokoro only produces audio in response to text frames the dock forwards to it. A sibling engine (e.g. `bin/neutts-service`) can dock at any time and will hot-swap Kokoro out.

### Dock handshake (engine-side)

On startup Kokoro:

1. Connects to `127.0.0.1:13143`.
2. Sends one line of JSON HELLO, terminated with `\n`:
   ```json
   {"name":"kokoro","sample_rate":24000,"channels":1,"format":"f32le"}
   ```
3. Reads one line back. `OK\n` → the slot is ours, proceed. `ERR <reason>\n` → close and retry after 200 ms.
4. After `OK`, every frame on the socket is tag-prefixed: `0x01` for a serialized `Packet` (text in from LLaMA, audio out to OAP), `0x02` for a mgmt frame (`MgmtMsgType` byte + optional length-prefixed payload). PING/PONG keepalive runs on the same socket at 200 ms cadence.

### SHUTDOWN handler

The dock sends `CUSTOM SHUTDOWN` (`MgmtMsgType::CUSTOM` with payload `"SHUTDOWN"`) when another engine has won the slot. Kokoro's handler calls `shutdown_all_calls()` (joins per-call synthesis workers, releases CoreML model handles) and then `std::_Exit(0)`. The dock waits up to 2 s for the TCP close before force-closing. A replacement engine may restart Kokoro later via the frontend; it will simply re-dock.

### Cmd port

Engine-local diagnostics live on **cmd port 13144** (moved from 13142, which now belongs to the dock). `TEST_SYNTH`, `BENCHMARK`, `SYNTH_WAV`, `SET_SPEED`, `SET_LOG_LEVEL`, `PING`, `STATUS` behave as before.

**Architecture**: CoreML split pipeline with HAR source on CPU

| Component | Technology | Device | Latency |
|-----------|-----------|--------|---------|
| Phonemization (espeak-ng) | libespeak-ng (cached) | CPU | ~5ms |
| Phonemization (neural G2P) | DeepPhonemizer CoreML | ANE | ~8ms |
| Duration model | CoreML (BERT + prosody) | ANE | ~65ms |
| Alignment | repeat_interleave (C++) | CPU | <1ms |
| HAR source | TorchScript (SineGen+STFT) | CPU | ~5ms |
| Split decoder | CoreML vocoder (3 buckets) | ANE | ~70ms |
| **Total** | | | **~145ms** |

## Prerequisites

- macOS with Apple Silicon (M1/M2/M3/M4)
- [conda](https://docs.conda.io/) (miniconda or miniforge)
- [espeak-ng](https://github.com/espeak-ng/espeak-ng): `brew install espeak-ng`
- [PyTorch](https://pytorch.org/) (system Python, for libtorch C++ headers): `pip install torch`

## Model Export (from scratch)

The unified export script downloads the Kokoro German model, creates a conda environment with compatible dependency versions, and exports all CoreML artifacts.

```bash
python3 scripts/export_kokoro_models.py
```

This will:
1. Create/reuse conda env `kokoro_coreml` (Python 3.11, torch==2.5.0, coremltools==8.3.0)
2. Download the Kokoro German model from HuggingFace (~312 MB)
3. Download voice embeddings (df_eva, dm_bernd)
4. Export CoreML duration model → `bin/models/kokoro-german/coreml/kokoro_duration.mlmodelc`
5. Export CoreML split decoder (3 buckets) → `bin/models/kokoro-german/decoder_variants/kokoro_decoder_split_{3s,5s,10s}.mlmodelc`
6. Export HAR TorchScript models → `bin/models/kokoro-german/decoder_variants/kokoro_har_{3s,5s,10s}.pt`
7. Export voice packs → `bin/models/kokoro-german/{df_eva,dm_bernd}_voice.bin`
8. Export vocabulary → `bin/models/kokoro-german/vocab.json`

### Export options

```bash
# Skip dependency installation (already set up)
python3 scripts/export_kokoro_models.py --no-install

# Skip model download (already downloaded)
python3 scripts/export_kokoro_models.py --no-download

# Export specific components only
python3 scripts/export_kokoro_models.py --duration-only
python3 scripts/export_kokoro_models.py --decoder-only
python3 scripts/export_kokoro_models.py --voices-only
```

### Why specific versions?

- **torch==2.5.0**: coremltools 8.3 is incompatible with PyTorch 2.10+ (`AttributeError: 'torch._C.Node' object has no attribute 'cs'`)
- **coremltools==8.3.0**: Last version to support `torch.jit.trace` → CoreML conversion without errors
- **numpy==1.26.4**: Required for coremltools 8.3 compatibility (numpy 2.x breaks it)

## Building

```bash
cd build && cmake .. -DKOKORO_COREML=ON && make -j4
```

The build auto-detects:
- **libtorch**: Via `python3 -c "import torch; print(torch.utils.cmake_prefix_path)"`
- **espeak-ng**: Searches `/opt/homebrew/lib`, `/usr/local/lib`
- **espeak-ng data**: Searches `/opt/homebrew/share/espeak-ng-data`, `/usr/local/share/espeak-ng-data`

### Build requirements

- CMake 3.22+
- C++17 compiler with Objective-C++ support
- macOS Frameworks: CoreML, Foundation

## Running

```bash
./bin/kokoro-service [--voice df_eva|dm_bernd] [--g2p auto|neural|espeak]
```

The service:
1. Loads CoreML duration model, split decoder, HAR models, voice pack, and vocab
2. Initializes espeak-ng for German phonemization
3. Opens a TCP connection to the TTS dock on `127.0.0.1:13143`, sends the HELLO line described above, and waits for `OK\n`
4. Reads text packets the dock forwards from LLaMA
5. Synthesizes speech and sends audio packets back to the dock, which forwards them to the Outbound Audio Processor
6. On `CUSTOM SHUTDOWN` from the dock, joins worker threads and exits

### Environment variables

| Variable | Description | Default |
|----------|-------------|---------|
| `WHISPERTALK_MODELS_DIR` | Path to models directory | Compile-time default or `models/` |
| `ESPEAK_NG_DATA` | Path to espeak-ng-data directory | Auto-detected |

## Architecture Details

### Why CoreML Split?

Three decoder backends were benchmarked:

| Backend | Avg Latency | Model Size | Device |
|---------|------------|------------|--------|
| TorchScript (CPU) | 365ms | 2296 MB | CPU |
| ONNX Runtime | 301ms | 1450 MB | CPU |
| **CoreML Split (ANE)** | **70ms** | **321 MB** | **ANE** |

CoreML Split is **5x faster** and **7x smaller** than TorchScript.

### Why split the decoder?

The Kokoro vocoder uses `hn-nsf` (harmonic-noise source filter) which requires complex number operations (`torch.stft` with complex output). CoreML does not support complex tensor operations. The solution splits the pipeline:

1. **HAR source** (SineGen + STFT): Runs on CPU via TorchScript (~20KB models). Computes harmonic source from F0 predictions.
2. **Decoder-only** (vocoder without source): Runs on ANE via CoreML. Takes pre-computed HAR source as input.

### Decoder buckets

Three fixed-size decoder models handle different utterance lengths:

| Bucket | ASR Frames | F0 Frames | HAR Time | Max Duration |
|--------|-----------|-----------|----------|-------------|
| 3s | 72 | 144 | 8641 | ~3 seconds |
| 5s | 120 | 240 | 14401 | ~5 seconds |
| 10s | 240 | 480 | 28801 | ~10 seconds |

Inputs shorter than the bucket size are zero-padded; the waveform is trimmed to actual length.

### Duration model

Fixed 512-token input (padded with zeros, masked via attention_mask). Outputs:
- `pred_dur`: Duration prediction per token
- `d`: Duration encoder hidden states (for alignment)
- `t_en`: Text encoder output
- `s`: Style vector (prosody)
- `ref_s_out`: Reference style passthrough

### Alignment (CPU)

The `repeat_interleave` operation (mapping token durations to frame-level features) is data-dependent and cannot be compiled into CoreML. It runs on CPU in C++ using a simple loop that repeats each text encoder column by its predicted duration.

### Phonemization

Two backends are available, selected by `--g2p`:

- **`espeak` (default fallback)**: espeak-ng C API with German voice (`de`). Thread-safe via mutex. Results cached (up to 10,000 entries with clear-all eviction).
- **`neural`**: DeepPhonemizer German G2P model (`de_g2p.mlmodelc`) loaded from `$WHISPERTALK_MODELS_DIR/g2p/`. Produces more accurate IPA for German compound words, medical terms, and loanwords. Falls back to espeak-ng for non-German text.
- **`auto`**: uses neural G2P for German text (detected via `detect_german()`), espeak-ng otherwise. If the neural G2P model is absent, silently falls back to espeak-ng.

The `G2PBackend` enum is defined in `neural-g2p.h` and shared across all engine services.

### Prosody State Carryover

For multi-chunk responses, the `ref_s_out` tensor (256-dim float32 style vector) emitted by the duration model for chunk N is stored in `CallContext::last_ref_s` and injected as `ref_s` input to the duration model for chunk N+1. This ensures that the prosody context of a synthesized clause carries over to the next one, avoiding the "flat intonation reset" that occurred when each chunk was synthesized independently with a fresh voice-pack embedding lookup. No CoreML model re-export is required — the duration model already accepts `ref_s` as an external input tensor.

## Multi-Call Support

Each incoming call gets its own `CallContext` with a dedicated worker thread. Text packets are dispatched to the correct thread by `call_id`. Worker threads process text from a queue, synthesize audio, and send it downstream.

CALL_END signals terminate the worker thread and clean up resources for that call.

## Crash Resilience

- If the dock disconnects (TCP close on port 13143), Kokoro retries the HELLO every 200 ms until accepted again.
- If LLaMA or OAP are down, the dock drops/queues frames — Kokoro is unaware and keeps its socket open.
- If the dock sends `CUSTOM SHUTDOWN`, Kokoro exits cleanly via `std::_Exit(0)` after joining workers. The frontend may restart it.

## File Layout

```
bin/models/kokoro-german/
├── coreml/
│   ├── kokoro_duration.mlmodelc      # CoreML duration model (ANE)
│   └── coreml_config.json
├── decoder_variants/
│   ├── kokoro_decoder_split_3s.mlmodelc   # CoreML decoder bucket (ANE)
│   ├── kokoro_decoder_split_5s.mlmodelc
│   ├── kokoro_decoder_split_10s.mlmodelc
│   ├── kokoro_har_3s.pt                   # HAR TorchScript (CPU)
│   ├── kokoro_har_5s.pt
│   ├── kokoro_har_10s.pt
│   └── split_config.json
├── df_eva_voice.bin          # Voice embedding (raw float32)
├── dm_bernd_voice.bin
└── vocab.json                # Phoneme-to-ID mapping
```

## Tests

```bash
./bin/test_kokoro_cpp
```

7 tests:
1. espeak-ng initialization and German phonemization
2. Vocab loading (114 entries)
3. Phoneme encoding with UTF-8 support
4. Voice pack loading (bin format, 512×256)
5. CoreML duration model load and inference (65ms on ANE)
6. CoreML split decoder benchmark (avg 71ms on ANE)
7. Model size inventory

## Benchmark Results

Measured on Apple Silicon (M-series):

- **CoreML duration**: 65ms (ANE)
- **CoreML split decoder**: avg 70ms, min 70ms (ANE)
- **Phonemization**: ~5ms (CPU, cached <1ms)
- **HAR source**: ~5ms (CPU, TorchScript)
- **End-to-end per sentence**: ~145ms

## Known Limitations

- macOS-only (CoreML requires Apple frameworks)
- Requires libtorch dynamic library at runtime (for HAR models and tensor operations)
- espeak-ng dynamic library at runtime
- Maximum utterance length is capped at 120 seconds of PCM buffer (dock-side limit). Individual decoder inference still uses the 3s/5s/10s buckets internally; longer utterances are concatenated chunk-by-chunk.
- Phoneme cache uses simple clear-all eviction (not LRU)
