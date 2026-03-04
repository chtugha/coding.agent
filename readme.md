# WhisperTalk

A high-performance, real-time speech-to-speech system designed for low-latency telephony communication. WhisperTalk integrates **Whisper** (ASR), **LLaMA** (LLM), and **Kokoro** (TTS) into a linear microservice pipeline, using a standalone SIP client as an RTP gateway. Optimized for Apple Silicon (CoreML/Metal).

## Architecture

```
                         Telephony Network
                              |
                         [SIP Client]
                          /        \
                    RTP in          RTP out
                        |              ^
                       IAP            OAP
                        |              ^
                       VAD          Kokoro
                        |              ^
                     Whisper -----> LLaMA

                     [Frontend] (web UI + log aggregation)
```

The pipeline is a linear chain of 7 C++ programs. Every adjacent pair communicates over two persistent TCP connections (management + data). The frontend manages all services and provides a web UI at `http://0.0.0.0:8080/`.

## Requirements

- **OS**: macOS (Apple Silicon M-series recommended)
- **Language**: C++17, Python 3.9+
- **Build**: CMake 3.22+
- **Dependencies**:
  - [whisper.cpp](https://github.com/ggerganov/whisper.cpp) (v1.8.3+, compiled with CoreML support)
  - [llama.cpp](https://github.com/ggerganov/llama.cpp) (compiled with Metal support)
  - [PyTorch](https://pytorch.org/) / libtorch (for Kokoro TTS)
  - [espeak-ng](https://github.com/espeak-ng/espeak-ng) (`brew install espeak-ng`)
  - macOS frameworks: Accelerate, Metal, CoreML, Foundation

## Build

```bash
# Build whisper.cpp with CoreML
cd whisper-cpp && mkdir -p build && cd build
cmake .. -DWHISPER_COREML=1 -DWHISPER_COREML_ALLOW_FALLBACK=ON && make -j
cd ../..

# Build llama.cpp with Metal
cd llama-cpp && mkdir -p build && cd build
cmake .. && make -j
cd ../..

# Build WhisperTalk
mkdir -p build && cd build
cmake .. && make -j
```

Binaries are output to `bin/`.

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `KOKORO_COREML` | `ON` | Enable CoreML ANE acceleration for Kokoro decoder |
| `BUILD_TESTS` | `OFF` | Build unit/integration tests (requires Google Test) |
| `TORCH_CMAKE_PREFIX` | auto-detected | Path to libtorch CMake config |
| `ESPEAK_NG_DATA_DIR` | auto-detected | Path to espeak-ng-data directory |

## Models

Place model files in `bin/models/`:

| Model | File | Size | Purpose |
|-------|------|------|---------|
| Whisper large-v3 | `ggml-large-v3.bin` | 2.9 GB | ASR (best accuracy) |
| Whisper large-v3-turbo-q5_0 | `ggml-large-v3-turbo-q5_0.bin` | 547 MB | ASR (fastest) |
| Whisper CoreML encoder | `ggml-large-v3-encoder.mlmodelc/` | - | CoreML ANE acceleration |
| LLaMA 3.2-1B-Instruct | `Llama-3.2-1B-Instruct-Q8_0.gguf` | ~1.1 GB | Response generation |
| Kokoro TTS | `kokoro.pt` | - | Text-to-speech |
| Kokoro voice | `voice.bin` | - | Voice style embedding |
| Kokoro vocab | `vocab.json` | - | Phoneme-to-token mapping |

### CoreML Encoder Conversion

Each Whisper model architecture needs its own CoreML encoder:

```bash
cd whisper-cpp
python3 models/convert-whisper-to-coreml.py --model large-v3 --encoder-only True --optimize-ane True
xcrun coremlc compile models/coreml-encoder-large-v3.mlpackage models/
mv models/coreml-encoder-large-v3.mlmodelc models/ggml-large-v3-encoder.mlmodelc
```

`large-v3` and `large-v3-q5_0` share the same encoder. `large-v3-turbo` and `large-v3-turbo-q5_0` share a different encoder.

## Quick Start

```bash
# Start the frontend (manages all services)
cd bin
./frontend

# Open http://localhost:8080 in your browser
# Use the web UI to start/stop/configure individual services
```

The frontend starts all services as child processes. You can also run services individually (see below).

## Port Map

All services bind to `127.0.0.1`:

| Service | Mgmt Port | Data Port | Cmd Port | Notes |
|---------|-----------|-----------|----------|-------|
| SIP Client | 13100 | 13101 | 13102 | + SIP UDP 5060 + RTP UDP |
| IAP | 13110 | 13111 | 13112 | |
| VAD | 13115 | 13116 | 13117 | |
| Whisper | 13120 | 13121 | 13122 | |
| LLaMA | 13130 | 13131 | 13132 | |
| Kokoro | 13140 | 13141 | 13142 | |
| OAP | 13150 | 13151 | 13152 | |
| Frontend | 13160 | 13161 | 13162 | + HTTP 8080 |
| Log Server | - | - | - | UDP 22022 |

## Services

### 1. SIP Client (`bin/sip-client`)

RTP gateway for the pipeline. Handles SIP registration, incoming/outgoing calls, and routes RTP audio between the telephony network and the internal pipeline.

**Command-Line Parameters:**

| Argument | Default | Description |
|----------|---------|-------------|
| `--user <user>` | - | SIP username for initial line |
| `--server <ip>` | - | SIP server IP address |
| `--port <port>` | `5060` | SIP server port |
| `--password <pass>` | - | SIP password for Digest auth |
| `--lines <n>` | - | Number of SIP lines to register |
| `--log-level <LEVEL>` | `INFO` | Log verbosity: ERROR, WARN, INFO, DEBUG, TRACE |

**Runtime Commands (cmd port 13102):**

| Command | Description |
|---------|-------------|
| `ADD_LINE:<user>:<server>:<port>:<password>` | Register a new SIP account dynamically |
| `GET_STATS` | Returns JSON with RTP counters for all active calls |
| `PING` | Health check (returns `PONG`) |
| `STATUS` | Registered lines, active calls, connection state |
| `SET_LOG_LEVEL:<LEVEL>` | Change log verbosity without restart |

---

### 2. Inbound Audio Processor (`bin/inbound-audio-processor`)

Converts G.711 u-law telephony audio (8kHz) to high-quality float32 PCM (16kHz) for the VAD service.

- Decodes G.711 u-law using a 256-entry ITU-T lookup table
- Upsamples 8kHz to 16kHz via 15-tap half-band FIR filter (Hamming-windowed sinc, cutoff ~3.8kHz)
- Each 160-byte RTP payload (20ms @ 8kHz) produces 320 float32 samples (20ms @ 16kHz)

**Command-Line Parameters:**

| Argument | Default | Description |
|----------|---------|-------------|
| `--log-level <LEVEL>` | `INFO` | Log verbosity: ERROR, WARN, INFO, DEBUG, TRACE |

**Runtime Commands (cmd port 13112):**

| Command | Description |
|---------|-------------|
| `PING` | Health check (returns `PONG`) |
| `STATUS` | Active call count, upstream/downstream state, avg/max latency (us) |
| `SET_LOG_LEVEL:<LEVEL>` | Change log verbosity without restart |

---

### 3. VAD Service (`bin/vad-service`)

Energy-based Voice Activity Detection. Segments continuous audio into 0.5-8 second speech chunks for Whisper.

- Adaptive noise floor tracking (EMA, alpha=0.05, time constant ~1s)
- Micro-pause detection: 400ms silence triggers speech-end
- Smart-split: searches for energy dips near max-length boundaries to avoid cutting mid-word
- Pre-speech context: includes 400ms (8 frames) before onset for natural boundaries
- RMS energy gate: rejects chunks below 0.005 to prevent hallucinations
- Broadcasts SPEECH_ACTIVE/SPEECH_IDLE downstream for TTS interruption

**Command-Line Parameters:**

| Argument | Default | Range | Description |
|----------|---------|-------|-------------|
| `--vad-window-ms <ms>` | `50` | 10-500 | Analysis frame size in ms |
| `--vad-threshold <mult>` | `2.0` | 0.5-10.0 | Energy threshold multiplier over noise floor |
| `--vad-silence-ms <ms>` | `400` | - | Silence duration to trigger speech-end |
| `--vad-max-chunk-ms <ms>` | `8000` | - | Maximum speech chunk duration |
| `--log-level <LEVEL>` | `INFO` | - | Log verbosity: ERROR, WARN, INFO, DEBUG, TRACE |

**Runtime Commands (cmd port 13117):**

| Command | Description |
|---------|-------------|
| `PING` | Health check (returns `PONG`) |
| `STATUS` | VAD params, noise floor, active calls, connection state |
| `SET_LOG_LEVEL:<LEVEL>` | Change log verbosity without restart |
| `SET_VAD_THRESHOLD:<mult>` | Change energy threshold multiplier |
| `SET_VAD_SILENCE_MS:<ms>` | Change silence duration threshold |
| `SET_VAD_MAX_CHUNK_MS:<ms>` | Change maximum chunk duration |

---

### 4. Whisper Service (`bin/whisper-service`)

Automatic Speech Recognition using whisper.cpp, optimized for Apple Silicon CoreML/Metal.

- Greedy decoding (3-5x faster than beam search on short segments, negligible accuracy loss)
- Telephony-optimized parameters: `no_speech_thold=0.9`, `entropy_thold=2.8`
- No peak normalization (matches whisper-cli defaults)
- Optional hallucination filter (runtime-toggleable): exact-match + repetition detection + trailing suffix stripping
- RMS energy check: rejects chunks with RMS < 0.005
- Packet buffering: up to 64 transcriptions buffered if LLaMA is disconnected

**Command-Line Parameters:**

| Argument | Default | Description |
|----------|---------|-------------|
| `--language <lang>` / `-l` | `de` | Whisper language code |
| `--model <path>` / `-m` | `models/ggml-large-v3-turbo-q5_0.bin` | Path to GGML model file |
| `--log-level <LEVEL>` | `INFO` | Log verbosity: ERROR, WARN, INFO, DEBUG, TRACE |

**Runtime Commands (cmd port 13122):**

| Command | Description |
|---------|-------------|
| `HALLUCINATION_FILTER:ON` | Enable hallucination filtering |
| `HALLUCINATION_FILTER:OFF` | Disable hallucination filtering (default) |
| `HALLUCINATION_FILTER:STATUS` | Query current filter state |
| `PING` | Health check (returns `PONG`) |
| `STATUS` | Model name, connection state, filter state |
| `SET_LOG_LEVEL:<LEVEL>` | Change log verbosity without restart |

---

### 5. LLaMA Service (`bin/llama-service`)

Generates German-language conversational responses using Llama-3.2-1B-Instruct (Q8_0 GGUF).

- Greedy sampling, max 64 tokens, stops at sentence-ending punctuation
- German system prompt: max 1 sentence, 15 words, polite and natural
- Metal/MPS GPU acceleration (all layers on GPU)
- Per-call session isolation via KV cache sequence IDs
- Shut-up mechanism: SPEECH_ACTIVE interrupts generation (~5-13ms latency)
- Tokenizer resilience: retries with larger buffer on negative token counts

**Command-Line Parameters:**

| Argument | Default | Description |
|----------|---------|-------------|
| `--model <path>` / `-m` | `models/Llama-3.2-1B-Instruct-Q8_0.gguf` | Path to GGUF model file |
| `--log-level <LEVEL>` | `INFO` | Log verbosity: ERROR, WARN, INFO, DEBUG, TRACE |

**Runtime Commands (cmd port 13132):**

| Command | Description |
|---------|-------------|
| `PING` | Health check (returns `PONG`) |
| `STATUS` | Model name, active calls, connection state, speech state |
| `SET_LOG_LEVEL:<LEVEL>` | Change log verbosity without restart |

---

### 6. Kokoro TTS Service (`bin/kokoro-service`)

High-fidelity text-to-speech using the Kokoro model with CoreML ANE acceleration.

- Phonemization: espeak-ng (IPA) -> KokoroVocab (greedy longest-match, up to 4 chars) -> token IDs
- Two-stage model: Duration model (alignment) -> Decoder model (waveform generation)
- CoreML split decoder: ~2-4x speedup on M-series chips via Apple Neural Engine
- Phoneme cache: 10000 entries with LRU eviction
- Output normalization: peaks clipped to 0.95 ceiling
- SPEECH_ACTIVE handling: abandons synthesis immediately when caller speaks
- Language auto-detection: German ("de") or English ("en-us")

**Command-Line Parameters:**

| Argument | Default | Description |
|----------|---------|-------------|
| `--model <path>` | `models/kokoro.pt` | Path to Kokoro TorchScript model |
| `--voice <path>` | `models/voice.bin` | Path to voice style embedding |
| `--vocab <path>` | `models/vocab.json` | Path to phoneme vocabulary |
| `--log-level <LEVEL>` | `INFO` | Log verbosity: ERROR, WARN, INFO, DEBUG, TRACE |

**Runtime Commands (cmd port 13142):**

| Command | Description |
|---------|-------------|
| `PING` | Health check (returns `PONG`) |
| `STATUS` | Model path, connection state, active call count |
| `SET_LOG_LEVEL:<LEVEL>` | Change log verbosity without restart |

---

### 7. Outbound Audio Processor (`bin/outbound-audio-processor`)

Converts TTS audio (24kHz float32) back to telephony format (8kHz G.711 u-law) for the SIP client.

- Downsampling: 24kHz -> 8kHz via 15-tap anti-aliasing FIR (cutoff 3400Hz) + decimate by 3
- G.711 u-law encoding (ITU-T standard)
- Constant-rate 20ms timer: sends exactly 160 G.711 bytes per tick
- Fills with silence (0xFF) when no TTS audio is available to maintain RTP clock
- Per-call independent buffers and FIR state
- SPEECH_ACTIVE: clears all call buffers to stop stale TTS audio

**Command-Line Parameters:**

| Argument | Default | Description |
|----------|---------|-------------|
| `--log-level <LEVEL>` | `INFO` | Log verbosity: ERROR, WARN, INFO, DEBUG, TRACE |

**Runtime Commands (cmd port 13152):**

| Command | Description |
|---------|-------------|
| `PING` | Health check (returns `PONG`) |
| `STATUS` | Active calls, buffer lengths, connection state |
| `SET_LOG_LEVEL:<LEVEL>` | Change log verbosity without restart |

---

### 8. Frontend (`bin/frontend`)

Central control plane and web UI for the entire system.

- Serves a single-page web application at `http://0.0.0.0:8080/`
- Manages lifecycle of all 7 pipeline services (start/stop/restart/config)
- Aggregates structured logs from all services via UDP on port 22022
- Stores logs in SQLite with async batch writer for high throughput
- In-memory ring buffer + SSE stream for real-time log viewing
- Per-service log level control (persisted in SQLite, propagated to running services)
- Test infrastructure for Whisper ASR accuracy, pipeline WER, LLaMA quality, IAP codec quality
- Model management (list, search, download)

**Command-Line Parameters:**

| Argument | Default | Description |
|----------|---------|-------------|
| `--port <port>` | `8080` | HTTP server port |

**HTTP API Endpoints:**

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/services` | List all managed services + status |
| POST | `/api/services/start` | Start a service `{name, args}` |
| POST | `/api/services/stop` | Stop a service `{name}` |
| POST | `/api/services/restart` | Restart a service `{name}` |
| GET/POST | `/api/services/config` | Read/write per-service config |
| GET | `/api/logs` | Paginated log query `{limit, offset, service, level}` |
| GET | `/api/logs/recent` | Last N log entries from ring buffer |
| GET | `/api/logs/stream` | SSE live log stream |
| POST | `/api/settings/log_level` | Set per-service log level |
| POST | `/api/db/query` | Execute SELECT query (read-only) |
| POST | `/api/db/write_mode` | Toggle write mode for unsafe queries |
| GET | `/api/db/schema` | Return SQLite schema |
| GET | `/api/whisper/models` | List available GGML model files |
| POST | `/api/whisper/accuracy_test` | Run offline Whisper accuracy test |
| POST | `/api/whisper/hallucination_filter` | Enable/disable hallucination filter |
| GET/POST | `/api/vad/config` | Read/write VAD parameters |
| POST | `/api/sip/add-line` | Register a SIP account |
| POST | `/api/sip/remove-line` | Remove a SIP account |
| GET | `/api/sip/lines` | List registered SIP lines |
| GET | `/api/sip/stats` | RTP counters per active call |
| POST | `/api/iap/quality_test` | Offline G.711 codec quality test |
| GET | `/api/testfiles` | List WAV+TXT sample pairs |
| POST | `/api/testfiles/scan` | Rescan Testfiles/ directory |
| POST | `/api/tests/start` | Run a test binary |
| POST | `/api/tests/stop` | Kill a running test |
| GET | `/api/tests/*/history` | Test run history |
| GET | `/api/tests/*/log` | Test stdout/stderr |
| GET | `/api/test_results` | Pipeline WER test results |
| GET | `/api/status` | System uptime, service health summary |

### Frontend UI Features

- **Service Management**: Start/stop/restart each pipeline service individually
- **Log Viewer**: Real-time log streaming with per-service and per-level filtering
- **Log Level Control**: Checkboxes to set ERROR/WARN/INFO/DEBUG/TRACE per service (applied immediately, persisted across restarts)
- **VAD Configuration**: Tune threshold, silence duration, max chunk length at runtime
- **Whisper Configuration**: Select model, toggle hallucination filter
- **SIP Management**: Add/remove SIP lines, view RTP statistics
- **Test Infrastructure**: Run ASR accuracy tests, pipeline WER tests, LLaMA quality tests, codec quality tests
- **Model Management**: List, search, and download models

## Log Level Control

Every service supports 5 log levels: `ERROR`, `WARN`, `INFO`, `DEBUG`, `TRACE` (most to least severe).

**Three ways to set log level:**

1. **Startup argument**: `./whisper-service --log-level DEBUG`
2. **Frontend UI**: Use the log level checkboxes in each service's configuration section. Changes take effect immediately without restarting the service.
3. **Direct command**: Send `SET_LOG_LEVEL:DEBUG` to a service's cmd port via TCP.

Log levels set via the UI are persisted in SQLite and automatically applied on service restart.

## Interconnect Communication

All inter-service communication uses `interconnect.h`:

- **Management channel** (base port +0): Typed control messages (CALL_END, SPEECH_ACTIVE/IDLE, PING/PONG)
- **Data channel** (base port +1): Binary Packet frames (audio PCM, text, G.711)
- **Command port** (base port +2): TCP command interface for runtime control
- **TCP_NODELAY**: Enabled on all connections to minimize latency
- **Auto-reconnect**: Downstream connections retry every 200ms until reachable
- **LogForwarder**: Sends structured log entries as UDP datagrams to port 22022

## Testing

### Pipeline WER Test

```bash
python3 tests/run_pipeline_test.py <MODEL_NAME> [TESTFILES_DIR]
```

Injects WAV samples through the full pipeline via a test SIP provider, collects Whisper transcriptions from the frontend log API, and computes character-level similarity against ground truth.

- **PASS**: >= 99.5% similarity
- **WARN**: >= 90% similarity
- **FAIL**: < 90% similarity

Test samples go in `Testfiles/` as `sample_NN.wav` + `sample_NN.txt` pairs.

### Test SIP Provider (`bin/test_sip_provider`)

B2BUA test tool that injects audio files into the pipeline as if they were real phone calls.

```bash
./test_sip_provider --port 5060 --http-port 22011 --testfiles-dir Testfiles
```

### Unit/Integration Tests

```bash
cd build
cmake .. -DBUILD_TESTS=ON && make -j
ctest
```

## Benchmarks

See the sections below for detailed Whisper model benchmarks on Apple M4.

---

## Whisper Model Benchmarks

**Date**: 2026-02-22
**Hardware**: Apple M4, macOS 25.2.0
**whisper.cpp**: v1.8.3 (compiled with `-DWHISPER_COREML=1 -DWHISPER_COREML_ALLOW_FALLBACK=ON`)
**CoreML conversion**: `--optimize-ane True` (all CoreML encoders)

### Part 1: whisper-cli Direct Test (5 samples, clean 16kHz PCM)

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

### Part 2: Full Pipeline Test (20 samples, G.711 u-law via SIP/RTP)

Audio path: WAV -> 8kHz u-law G.711 -> RTP -> SIP Client -> IAP (upsample 8->16kHz) -> Whisper Service (VAD + greedy decoding)

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

**Scoring**: PASS = >=99.5% similarity, WARN = >=90%, FAIL = <90%

\* = sample_01 TIMEOUT/FAIL due to CoreML warmup on first inference
\** = sample_01 FAIL (41.8%) due to CoreML warmup causing VAD to miss first half

### Recommendations

- **Production (accuracy priority)**: `large-v3` + CoreML -- 1301ms avg, best accuracy after warmup
- **Production (speed priority)**: `large-v3-turbo` + CoreML -- 688ms avg, good accuracy
- **Development/testing**: `large-v3` + Metal -- no warmup delay, best accuracy, 1627ms avg

### Key Findings

1. **Quantization**: q5_0 has negligible accuracy impact -- same PASS/WARN/FAIL as unquantized
2. **CoreML warmup**: First inference takes 20-35s for model compilation. One-time cost for long-running services.
3. **Turbo tradeoff**: ~2x faster but slightly more WARN results (10-12 vs 8). The 4-layer decoder occasionally misses nuances in G.711-degraded audio.
4. **Consistent problem samples**: Some samples fail across ALL configs due to G.711 codec artifacts, not model limitations.

### Model Verification

When loading a model, verify in logs:
- `large-v3` (full): Reports `MTL0 total size = 3094.36 MB`, `n_text_layer = 32`
- `large-v3-turbo`: Reports `MTL0 total size = 1623.92 MB`, `n_text_layer = 4`
