# Prodigy

A high-performance, real-time speech-to-speech system designed for low-latency telephony communication. Prodigy integrates **Whisper** (ASR), **LLaMA** (LLM), and a generic **TTS stage** (with hot-pluggable **Kokoro** or **NeuTTS** engines) into a linear microservice pipeline, using a standalone SIP client as an RTP gateway. Optimized for Apple Silicon (CoreML/Metal) with no PyTorch runtime dependency.

An optional **RAG sidecar** (`tomedo-crawl`) connects to a [Tomedo](https://www.tomedo.de/) electronic medical records (EMR) server, crawls patient data, and feeds LLaMA with per-caller context so the AI can greet patients by name and give medically-informed responses.

## Architecture

```
                         Telephony Network
                              |
                         [SIP Client] ──────────────────────────► tomedo-crawl
                          /        \           POST /caller           (port 13181)
                    RTP in          RTP out                              │
                        |              ^                    GET /caller  │
                       IAP            OAP              GET /query        │
                        |              ^                                 ▼
                       VAD            TTS stage                    [Ollama embed]
                        |              ^  ▲                        [Vector store ]
                        |              |  │ (engine dock, port 13143)
                        |              |  └─ Kokoro engine OR NeuTTS engine
                     Whisper ────► LLaMA ◄──────────────────────────────┘
                                                    RAG context injection

                     [Frontend] (web UI + log aggregation)
```

The pipeline is a linear chain of C++ programs. Every adjacent pair communicates over two persistent TCP connections (management + data). The frontend manages all services and provides a web UI at `http://0.0.0.0:8080/`.

`tomedo-crawl` is a **sidecar** — it is not in the audio data path. Communication with other pipeline services is via its own HTTP REST API (port 13181).

## Requirements

- **OS**: macOS Apple Silicon (M1/M2/M3/M4)
- **Language**: C++17, Python 3.9+
- **Build**: CMake 3.22+, Ninja (recommended)
- **Dependencies** (installed automatically by `runmetoinstalleverythingfirst`):
  - [whisper.cpp](https://github.com/ggerganov/whisper.cpp) (compiled with CoreML + Metal)
  - [llama.cpp](https://github.com/ggerganov/llama.cpp) (compiled with Metal)
  - [espeak-ng](https://github.com/espeak-ng/espeak-ng) (`brew install espeak-ng`)
  - macOS frameworks: Accelerate, Metal, CoreML, Foundation

## Quick Install & Build

```bash
# Step 1: Install everything (Homebrew, Miniconda, models, CoreML exports)
./runmetoinstalleverythingfirst

# Step 2: Build all services
./runmetobuildeverything

# Step 3: Launch
cd bin && ./frontend
# Web UI: http://localhost:8080
```

`runmetobuildeverything` auto-clones `whisper-cpp` and `llama-cpp` if missing, detects Ninja for fast parallel builds, and bypasses the macOS Xcode license check using the Command Line Tools SDK directly.

### Manual Build

```bash
# Build whisper.cpp (CoreML + Metal, static)
cmake -G Ninja -S whisper-cpp -B whisper-cpp/build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DWHISPER_COREML=ON -DGGML_METAL=ON \
  -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=OFF
cmake --build whisper-cpp/build -j

# Build llama.cpp (Metal, static)
cmake -G Ninja -S llama-cpp -B llama-cpp/build \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DGGML_METAL=ON
cmake --build llama-cpp/build -j

# Build Prodigy
cmake -G Ninja -S . -B build \
  -DCMAKE_BUILD_TYPE=Release -DKOKORO_COREML=ON -DBUILD_TESTS=ON
cmake --build build -j
```

### CMake Options

| Option | Default | Description |
|--------|---------|-------------|
| `KOKORO_COREML` | `ON` | Enable CoreML ANE acceleration for Kokoro decoder |
| `BUILD_TESTS` | `ON` | Build unit/integration tests (requires GoogleTest); disable with `-DBUILD_TESTS=OFF` |
| `ESPEAK_NG_DATA_DIR` | auto-detected | Path to espeak-ng-data directory |

## Models

All model files are placed in `bin/models/`. `runmetoinstalleverythingfirst` downloads and prepares all of these automatically.

### Whisper (ASR)

| File | Size | Purpose |
|------|------|---------|
| `ggml-large-v3-turbo-q5_0.bin` | ~547 MB | Default ASR model (best speed/accuracy balance) |
| `ggml-large-v3-q5_0.bin` | ~1.0 GB | Higher accuracy ASR model |
| `ggml-large-v3-turbo-encoder.mlmodelc/` | varies | CoreML ANE encoder for large-v3-turbo |
| `ggml-large-v3-encoder.mlmodelc/` | varies | CoreML ANE encoder for large-v3 |

### LLaMA (LLM)

| File | Size | Purpose |
|------|------|---------|
| `Llama-3.2-1B-Instruct-Q8_0.gguf` | ~1.2 GB | Response generation (Metal-accelerated) |

### Kokoro (TTS)

Located in `bin/models/kokoro-german/`:

| File | Purpose |
|------|---------|
| `coreml/kokoro_duration.mlmodelc/` | Duration model (CoreML ANE) |
| `coreml/kokoro_f0n_{3s,5s,10s}.mlmodelc/` | F0/N predictor buckets (CoreML ANE) |
| `decoder_variants/*.mlmodelc/` | Split decoder models (CoreML ANE) |
| `<voice>_voice.bin` | Voice style embedding (256-dim float32). Available: `df_eva_voice.bin`, `dm_bernd_voice.bin` |
| `vocab.json` | Phoneme-to-token mapping |

### NeuTTS (alternative TTS)

Located in `bin/models/neutts-nano-german/`:

| File | Size | Purpose |
|------|------|---------|
| `neutts-nano-german-Q4_0.gguf` | ~185 MB | LLaMA-based speech backbone |
| `neucodec_decoder.mlmodelc/` | ~3.4 GB | NeuCodec CoreML decoder |
| `ref_codes.bin` | - | Pre-computed reference voice codec codes |
| `ref_text.txt` | - | Reference voice phoneme transcript |

## Port Map

All services bind to `127.0.0.1`:

| Service | Mgmt Port | Data Port | Cmd Port | Notes |
|---------|-----------|-----------|----------|-------|
| SIP Client | 13100 | 13101 | 13102 | + SIP UDP 5060 + RTP UDP 10000+ |
| IAP | 13110 | 13111 | 13112 | |
| VAD | 13115 | 13116 | 13117 | |
| Whisper | 13120 | 13121 | 13122 | |
| LLaMA | 13130 | 13131 | 13132 | |
| TTS stage (dock) | 13140 | 13141 | 13142 | Engine dock listens on 13143 |
| Kokoro engine | — | — | 13144 | Docks into TTS stage on 13143 |
| NeuTTS engine | — | — | 13174 | Docks into TTS stage on 13143 |
| OAP | 13150 | 13151 | 13152 | |
| Frontend | - | - | - | HTTP 8080, Log UDP 22022 |
| tomedo-crawl | 13180 | **13181** | 13182 | REST API on 13181; 13180/13182 reserved |

---

## Services

### 1. SIP Client (`bin/sip-client`)

RTP gateway and SIP stack. Handles SIP registration with Digest authentication, incoming/outgoing call management, and routes raw RTP audio between the telephony network and the internal pipeline.

**Key behaviors:**
- Minimal SIP stack over raw UDP (port 5060 by default)
- MD5 Digest authentication with `WWW-Authenticate` challenge parsing
- Re-registers every 60 seconds
- Multi-line: supports N simultaneous SIP registrations (`--lines 0` is valid for test-only mode)
- Inbound RTP forwarded to IAP as raw Packet frames (12-byte RTP header included; IAP strips it)
- Outbound G.711 frames from OAP wrapped in RTP headers (seq, timestamp, SSRC) and sent via UDP
- Stale call auto-hangup after 60 seconds of no RTP traffic
- RTP port base: 10000, incremented by 2 per call

**Command-Line Parameters:**

| Argument | Default | Description |
|----------|---------|-------------|
| `[--lines N] [<user> <server> [port]]` | 0 lines | Lines to register at startup; positional args only needed when `lines > 0` |
| `--log-level <LEVEL>` | `INFO` | Log verbosity: ERROR, WARN, INFO, DEBUG, TRACE |

**Runtime Commands (cmd port 13102):**

| Command | Description |
|---------|-------------|
| `ADD_LINE <user> <server> <port> <password>` | Register a new SIP account dynamically (space-delimited; use `-` for no password) |
| `REMOVE_LINE <index>` | Unregister and remove a SIP line by index |
| `LIST_LINES` | List all registered SIP lines |
| `GET_STATS` | JSON RTP counters for all active calls (rx/tx packets, bytes, forwarded, discarded) |
| `PING` | Health check → `PONG` |
| `STATUS` | Registered lines, active calls, connection state |
| `SET_LOG_LEVEL:<LEVEL>` | Change log verbosity without restart |

---

### 2. Inbound Audio Processor (`bin/inbound-audio-processor`)

Converts G.711 μ-law telephony audio (8kHz) to float32 PCM (16kHz) for the VAD service.

**Signal chain:**
1. **G.711 μ-law decode**: 256-entry ITU-T lookup table; each byte → float32 in [-1.0, 1.0]
2. **8kHz→16kHz upsample**: 15-tap Hamming-windowed sinc FIR half-band filter (cutoff ~3.8kHz, ~40dB stopband). Zero-stuffs input, then filters to remove spectral copies above 4kHz.

Each 160-byte RTP payload (20ms @ 8kHz) produces 320 float32 samples (20ms @ 16kHz). Continues processing and discards output if VAD is unavailable; auto-reconnects when VAD comes back online.

**Command-Line Parameters:**

| Argument | Default | Description |
|----------|---------|-------------|
| `--log-level <LEVEL>` | `INFO` | Log verbosity |

**Runtime Commands (cmd port 13112):**

| Command | Description |
|---------|-------------|
| `PING` | Health check → `PONG` |
| `STATUS` | Active call count, upstream/downstream state, avg/max per-packet latency (μs) |
| `SET_LOG_LEVEL:<LEVEL>` | Change log verbosity without restart |

---

### 3. VAD Service (`bin/vad-service`)

Energy-based Voice Activity Detection. Segments continuous 16kHz PCM into speech chunks (0.5–8 seconds) for Whisper.

**Algorithm:**
- **Adaptive noise floor**: EMA update (alpha=0.05) during silence frames; time constant ~1 second
- **Onset detection**: requires 3 consecutive frames above `threshold × noise_floor` to confirm speech start
- **End detection**: 400ms of consecutive sub-threshold frames triggers speech-end
- **Micro-pause detection**: short pauses (~400ms) between words trigger early submission rather than waiting for full silence — reduces Whisper inference latency since inference time scales with chunk length
- **Smart-split**: when max chunk length is reached during speech, finds the lowest-energy frame near the boundary to avoid cutting mid-word
- **Pre-speech context**: 400ms (8 frames × 50ms) before confirmed onset is prepended to each chunk
- **RMS energy gate**: chunks with RMS < 0.005 discarded as near-silence
- **SPEECH_ACTIVE/SPEECH_IDLE signals**: broadcast downstream to the TTS stage (teed to the docked engine) and OAP for TTS interruption and SPEECH_IDLE-driven warm-up

**Command-Line Parameters:**

| Argument | Default | Description |
|----------|---------|-------------|
| `--vad-threshold <mult>` | `2.0` | Energy threshold multiplier over noise floor |
| `--vad-silence-ms <ms>` | `400` | Silence duration to end speech segment |
| `--vad-max-chunk-ms <ms>` | `8000` | Maximum speech chunk duration |
| `--log-level <LEVEL>` | `INFO` | Log verbosity |

**Runtime Commands (cmd port 13117):**

| Command | Description |
|---------|-------------|
| `PING` | Health check → `PONG` |
| `STATUS` | Noise floor, threshold, silence_ms, max_chunk_ms, active calls, upstream/downstream state |
| `SET_VAD_THRESHOLD:<mult>` | Update threshold multiplier at runtime |
| `SET_VAD_SILENCE_MS:<ms>` | Update silence detection duration at runtime |
| `SET_VAD_MAX_CHUNK_MS:<ms>` | Update max chunk length at runtime |
| `SET_LOG_LEVEL:<LEVEL>` | Change log verbosity without restart |

---

### 4. Whisper Service (`bin/whisper-service`)

Automatic Speech Recognition (ASR). Receives pre-segmented speech chunks from VAD and returns transcribed text to LLaMA.

**Inference details:**
- **Backend**: whisper.cpp with CoreML ANE (Apple Neural Engine) + Metal fallback
- **Decoding**: Greedy strategy (not beam search). On 2–8s segments, greedy is 3–5× faster than beam_size=5 with negligible accuracy difference. Temperature fallback with `temp_inc=0.2` handles uncertain segments.
- **Telephony-optimized parameters**: `no_speech_thold=0.9` (prevents early decoder stop on G.711-degraded audio), `entropy_thold=2.8` (tolerant of codec uncertainty)
- **No audio normalization**: audio passed directly to Whisper (matches whisper-cli defaults for optimal accuracy on G.711 input)
- **RMS energy pre-check**: rejects chunks with RMS < 0.005 to prevent hallucinations on near-silence
- **Packet buffering**: if LLaMA is disconnected, buffers up to 64 transcription packets and drains them on reconnect
- **Hallucination filter** (default OFF, runtime-toggleable): exact-match detection of common Whisper hallucination strings (e.g., "Untertitel", "Copyright", "Musik"); repetition detection; trailing suffix stripping

**Command-Line Parameters:**

| Argument | Default | Description |
|----------|---------|-------------|
| `--language <lang>` / `-l <lang>` | `de` | Whisper language code |
| `--model <path>` / `-m <path>` | `models/ggml-large-v3-turbo-q5_0.bin` | Path to GGML model file |
| `--log-level <LEVEL>` | `INFO` | Log verbosity |

**Runtime Commands (cmd port 13122):**

| Command | Description |
|---------|-------------|
| `PING` | Health check → `PONG` |
| `STATUS` | Model name, upstream/downstream state, hallucination filter state |
| `HALLUCINATION_FILTER:ON` / `OFF` | Enable/disable hallucination filter |
| `HALLUCINATION_FILTER:STATUS` | Query filter state |
| `SET_LOG_LEVEL:<LEVEL>` | Change log verbosity without restart |

---

### 5. LLaMA Service (`bin/llama-service`)

Generates a spoken German reply from transcribed text using Llama-3.2-1B-Instruct.

**Inference details:**
- **Model**: Llama-3.2-1B-Instruct Q8_0 GGUF, all layers on Metal GPU (`n_gpu_layers=-1`)
- **Template**: `llama_chat_apply_template()` — uses the model's built-in chat template for correct role tagging; no manual prompt formatting
- **Sampling**: Greedy (`llama_sampler_init_greedy`). Max 64 tokens per response. Stops at sentence-ending punctuation (`.`, `?`, `!`) or EOS.
- **Context**: 2048 tokens, 4 threads
- **German system prompt**: enforces always-German, max 1 sentence / 15 words, polite and natural tone. ~320ms average latency on Apple M-series.
- **Session isolation**: each call gets its own `LlamaCall` struct with independent message history and KV cache sequence ID. Context cleared on `CALL_END`.
- **Shut-up mechanism**: `SPEECH_ACTIVE` from VAD aborts active generation immediately (~5–13ms interrupt latency). Worker loop defers new responses while speech is active.
- **Tokenizer resilience**: retries with progressively larger buffer (up to 4×) if `llama_tokenize()` returns a negative value

**Command-Line Parameters:**

| Argument | Default | Description |
|----------|---------|-------------|
| `--model <path>` / `-m <path>` | `models/Llama-3.2-1B-Instruct-Q8_0.gguf` | Path to GGUF model |
| `--log-level <LEVEL>` | `INFO` | Log verbosity |

**Runtime Commands (cmd port 13132):**

| Command | Description |
|---------|-------------|
| `PING` | Health check → `PONG` |
| `STATUS` | Model name, active calls, upstream/downstream state, speech active flag |
| `SET_LOG_LEVEL:<LEVEL>` | Change log verbosity without restart |

---

### 6. TTS Stage (`bin/tts-service`) and Engines

The TTS stage is a generic pipeline node that sits between LLaMA and OAP. It
owns the interconnect sockets (mgmt 13140, data 13141, cmd 13142) and a
dedicated **engine dock** on port **13143**. Concrete TTS engines
(`bin/kokoro-service`, `bin/neutts-service`) are **not** pipeline nodes any
more — they are client processes that connect to the dock, authenticate via
a one-line JSON HELLO, and stream audio back through it.

**Engine slot model (last-connect-wins).** The dock holds at most one active
engine. State transitions:

```
                 HELLO ok             TCP close / SHUTDOWN ack
  [NO ENGINE] ─────────────▶ [ACTIVE=X] ─────────────────────▶ [NO ENGINE]
                                  │
                                  │  new engine Y completes HELLO ok
                                  ▼
                            [SWAPPING X→Y]
                                  │  dock sends CUSTOM SHUTDOWN to X;
                                  │  X closes TCP (≤ 2 s) or is force-closed
                                  ▼
                              [ACTIVE=Y]
```

When no engine is docked, LLaMA text frames are dropped with a
rate-limited WARN log; mgmt signals (CALL_END / SPEECH_ACTIVE /
SPEECH_IDLE) are still auto-forwarded to OAP. On every swap the dock
emits a `CUSTOM FLUSH_TTS` mgmt frame to OAP so residual PCM is
discarded before the new engine's audio arrives.

**Engine dock protocol (TCP, loopback only):**
1. Engine connects to `127.0.0.1:13143`.
2. Engine sends one-line JSON HELLO: `{"name":"kokoro","sample_rate":24000,"channels":1,"format":"f32le"}\n`.
3. Dock replies `OK\n` (accepts) or `ERR <reason>\n` (rejects — the current active engine is untouched).
4. After OK, frames are tag-prefixed: `0x01` = serialized data `Packet`, `0x02` = mgmt (`MgmtMsgType` + optional payload). The dock ferries LLaMA→engine text, engine→OAP audio, mgmt signals, and PING/PONG keepalives.
5. On receipt of `CUSTOM SHUTDOWN` the engine process joins its workers, releases model handles, and calls `std::_Exit(0)`.

**Runtime Commands (TTS dock cmd port 13142):**

| Command | Description |
|---------|-------------|
| `PING` | Health check → `PONG` |
| `STATUS` | `ACTIVE <engine-name>` when docked, `NONE` otherwise |
| `SET_LOG_LEVEL:<LEVEL>` | Change dock log verbosity without restart |

#### 6a. Kokoro engine (`bin/kokoro-service`)

Text-to-speech using the Kokoro model. Receives text from the dock and streams 24kHz float32 PCM audio back through it. No PyTorch dependency — all inference via CoreML on Apple Neural Engine.

**Phonemization pipeline:**
1. **espeak-ng** (via `libespeak-ng`) converts input text → IPA phoneme string. Language auto-detected (`de` / `en-us`) via `detect_german()`.
2. **Phoneme cache** (LRU, 10,000 entries): avoids re-running espeak-ng for repeated phrases.
3. **KokoroVocab**: greedy longest-match scan (up to 4 chars per token, UTF-8 aware) maps phonemes → int64 token IDs from `vocab.json`. Input padded to 512 tokens.

**Two-stage CoreML inference:**
- **Stage 1 — Duration model** (`kokoro_duration.mlmodelc`): predicts per-phoneme durations, generates alignment tensors (`pred_dur`, `d`, `t_en`, `s`, `ref_s`). Style encoding from `<voice>_voice.bin` (256-dim reference embedding).
- **Stage 1b — F0/N predictor** (`kokoro_f0n_{3s,5s,10s}.mlmodelc`): three bucketed models (3s/5s/10s) predict fundamental frequency (`f0_pred`) and voicing (`n_pred`) from the duration model's `d` and `s` outputs. Bucket selected by utterance length. These condition the harmonic/noise excitation signal — without them, speech sounds hoarse/unvoiced.
- **Stage 2 — Decoder** (`decoder_variants/*.mlmodelc`): split decoder generates the audio waveform from alignment tensors + F0/N conditioning. All models run with `MLComputeUnitsAll` (ANE + GPU + CPU).

**Audio output processing:**
- `normalize_audio()`: scales to 0.90 peak ceiling (skips near-silent audio and already-normalized output)
- `apply_fade_in()`: 48-sample linear ramp at onset to prevent click artifacts
- Sends audio to OAP in 4800-sample chunks (200ms @ 24kHz) for smooth buffer filling

**SPEECH_ACTIVE handling:** Abandoned synthesis immediately if VAD signals caller speech. Per-call synthesis threads, so multi-line calls synthesize in parallel.

**Command-Line Parameters:**

| Argument | Default | Description |
|----------|---------|-------------|
| `--voice <NAME>` | `df_eva` | Voice to use (`df_eva`, `dm_bernd`) |
| `--log-level <LEVEL>` | `INFO` | Log verbosity |

**Runtime Commands (Kokoro engine cmd port 13144):**

| Command | Description |
|---------|-------------|
| `PING` | Health check → `PONG` |
| `STATUS` | Active calls, dock connection state, current speed |
| `SET_SPEED:<0.5–2.0>` | Set synthesis speed (1.0 = normal, clamped to [0.5, 2.0]) |
| `GET_SPEED` | Query current speed |
| `TEST_SYNTH:<text>` | Synthesize text and return timing/peak/RMS stats (no audio output) |
| `BENCHMARK:<text>\|<N>` | Run N synthesis iterations; returns avg/p50/p95 latency and RTF |
| `SYNTH_WAV:<path>\|<text>` | Synthesize text and save to WAV file at `<path>` (relative paths only) |
| `SET_LOG_LEVEL:<LEVEL>` | Change log verbosity without restart |

On a `CUSTOM SHUTDOWN` frame from the dock the engine joins its synthesis workers and exits; it does **not** restart the pipeline.

---

#### 6b. NeuTTS engine (`bin/neutts-service`)

Alternative TTS engine using the NeuTTS Nano German model. Like Kokoro it is a dock client — the dock's single engine slot means only one TTS engine serves traffic at a time, and starting a second engine transparently swaps it in (last-connect-wins).

**Inference pipeline:**
1. espeak-ng converts input text → IPA phonemes (language `de`, with stress markers)
2. Builds a NeuTTS prompt: `user: Convert the text to speech:<|TEXT_PROMPT_START|>{ref_phones} {phones}<|TEXT_PROMPT_END|>\nassistant:<|SPEECH_GENERATION_START|>{ref_codes}`
3. Tokenize and feed to NeuTTS backbone (llama.cpp, Q4_0 GGUF) with temperature=1.0, top_k=50 autoregressive sampling
4. Extract `<|speech_N|>` tokens as integer codec codes
5. Stop at `<|SPEECH_GENERATION_END|>` or EOS
6. Decode codes through NeuCodec CoreML decoder → 24kHz float32 PCM

**Reference voice:** Pre-computed codec codes (`ref_codes.bin`) and phonemized text (`ref_text.txt`) loaded at startup to define voice timbre and style.

**Audio post-processing:** Same as Kokoro — normalize to 0.90 peak, 48-sample fade-in.

**Command-Line Parameters:**

| Argument | Default | Description |
|----------|---------|-------------|
| `--log-level <LEVEL>` | `INFO` | Log verbosity |

**Runtime Commands (NeuTTS engine cmd port 13174):**

| Command | Description |
|---------|-------------|
| `PING` | Health check → `PONG` |
| `STATUS` | Active calls, dock connection state |
| `TEST_SYNTH:<text>` | Synthesize and return timing stats |
| `SYNTH_WAV:<path>\|<text>` | Synthesize text to a WAV file at the given path |
| `SET_LOG_LEVEL:<LEVEL>` | Change log verbosity without restart |

On a `CUSTOM SHUTDOWN` frame from the dock the engine joins its synthesis workers and exits.

---

### 8. Outbound Audio Processor (`bin/outbound-audio-processor`)

Converts 24kHz float32 PCM from the TTS stage into 160-byte G.711 μ-law frames for the SIP client. Maintains constant 20ms output cadence.

**Signal chain (per call):**
1. **DC blocking** (first-order high-pass): α = 0.9947697 (~20Hz cutoff). Removes DC offset and LF rumble. Initialized with the first sample value to avoid onset click.
2. **Presence boost** (optional, default OFF): High-shelf biquad IIR filter, +3dB shelf at 2500Hz (Audio EQ Cookbook, S=1). Adds air/clarity to the telephone band.
3. **Anti-aliasing FIR** (63-tap, Hamming-windowed sinc): Cutoff 3400/12000 (normalized). ~43dB stopband attenuation. Coefficients computed once at startup, shared across all calls. Per-call `fir_history[31]` preserves filter state across chunks.
4. **3:1 Decimation**: Keep every 3rd filtered sample (24kHz → 8kHz).
5. **G.711 μ-law encode** (ITU-T compliant): `ULAW_CLIP=32635`, `ULAW_BIAS=132`. Encodes int16 PCM to 8-bit μ-law byte.

**Output scheduler:** Dedicated sender thread fires every 20ms using `steady_clock`. Sends exactly 160 bytes per tick. If the TTS buffer is empty (silence, no engine docked, or a `FLUSH_TTS` just drained residual PCM during an engine swap), sends `0xFF` (μ-law silence) to maintain RTP clock continuity. Scheduler resync guard: if OS sleep/load spike causes >100ms drift, snaps `next_tick` to `now` instead of firing a burst of catch-up frames.

**SPEECH_ACTIVE handling:** Clears all per-call buffers and resets FIR/DC/biquad state immediately when VAD signals caller speech. A configurable sidetone guard (default 1500ms) suppresses flushes arriving shortly after new TTS audio — prevents echo from triggering a spurious flush.

**WAV recording** (optional): When enabled, records the 8kHz int16 PCM output per call. Written to disk on `CALL_END`.

**Command-Line Parameters:**

| Argument | Default | Description |
|----------|---------|-------------|
| `--save-wav-dir <dir>` | (disabled) | Enable WAV recording and set output directory |
| `--log-level <LEVEL>` | `INFO` | Log verbosity |

**Runtime Commands (cmd port 13152):**

| Command | Description |
|---------|-------------|
| `PING` | Health check → `PONG` |
| `STATUS` | Active calls, buffer lengths, upstream/downstream state |
| `SAVE_WAV:ON` / `OFF` / `STATUS` | Toggle WAV recording |
| `SET_SAVE_WAV_DIR:<dir>` | Set WAV output directory |
| `PRESENCE_BOOST:ON` / `OFF` / `STATUS` | Toggle +3dB presence boost biquad |
| `SET_SIDETONE_GUARD_MS:<ms>` | Set SPEECH_ACTIVE guard window (default 800ms) |
| `TEST_ENCODE:<freq>\|<dur_ms>` | Generate sine wave, encode, measure μ-law RMS output |
| `SET_LOG_LEVEL:<LEVEL>` | Change log verbosity without restart |

---

### 9. Frontend (`bin/frontend`)

Central control plane. Serves the web UI, manages service lifecycles, aggregates logs, and exposes all configuration via REST API.

**Storage**: SQLite database (`whispertalk.db`) — persists service configurations, log level settings, and test results.

**Log aggregation**: Each service sends structured log entries as UDP datagrams to port 22022. Frontend stores them in SQLite (ring-buffered in memory for fast `/recent` queries) and streams them live via SSE.

**Full HTTP API (port 8080):**

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/services` | List all managed services + status |
| POST | `/api/services/start` | Start a service `{name, args}` |
| POST | `/api/services/stop` | Stop a service `{name}` |
| POST | `/api/services/restart` | Restart a service `{name}` |
| GET/POST | `/api/services/config` | Read/write per-service config (persisted in SQLite) |
| GET | `/api/logs` | Paginated log query `{limit, offset, service, level}` |
| GET | `/api/logs/recent` | Last N entries from in-memory ring buffer |
| GET | `/api/logs/stream` | SSE live log stream |
| POST | `/api/settings/log_level` | Set per-service log level (propagated to running service immediately) |
| POST | `/api/db/query` | Execute SELECT query (read-only guard) |
| POST | `/api/db/write_mode` | Toggle write mode for unsafe queries |
| GET | `/api/db/schema` | Return SQLite schema |
| GET | `/api/whisper/models` | List available GGML model files in `models/` |
| POST | `/api/whisper/accuracy_test` | Run offline Whisper accuracy test on a WAV file |
| POST | `/api/whisper/hallucination_filter` | Enable/disable Whisper hallucination filter |
| GET | `/api/tts/status` | TTS-dock engine slot: `{"engine":"kokoro"}`, `{"engine":"neutts"}`, or `{"engine":null}` when no engine is docked |
| GET/POST | `/api/vad/config` | Read/write VAD parameters (propagated to running service) |
| GET/POST | `/api/oap/wav_recording` | Read/write OAP WAV recording settings |
| POST | `/api/sip/add-line` | Register a new SIP account |
| POST | `/api/sip/remove-line` | Remove a SIP account |
| GET | `/api/sip/lines` | List registered SIP lines |
| GET | `/api/sip/stats` | RTP counters per active call |
| POST | `/api/iap/quality_test` | Offline G.711 codec round-trip quality test |
| GET | `/api/testfiles` | List WAV+TXT sample pairs in `Testfiles/` |
| POST | `/api/testfiles/scan` | Rescan `Testfiles/` directory |
| POST | `/api/tests/start` | Run a test binary |
| POST | `/api/tests/stop` | Kill a running test |
| GET | `/api/tests/*/history` | Test run history |
| GET | `/api/tests/*/log` | Test stdout/stderr |
| GET | `/api/test_results` | Pipeline WER test results |
| GET | `/api/status` | System uptime, service health summary |

**Web UI features:**
- Service management: start/stop/restart each pipeline service independently
- Real-time log streaming with per-service and per-level filtering
- Log level control: checkboxes (ERROR/WARN/INFO/DEBUG/TRACE) applied immediately and persisted
- VAD configuration: threshold, silence duration, max chunk length — runtime update without restart
- Whisper configuration: model selection, hallucination filter toggle
- Kokoro configuration: synthesis speed slider, SYNTH_WAV test
- OAP configuration: WAV recording toggle + directory, presence boost toggle
- SIP management: add/remove SIP lines, view RTP statistics
- Beta testing page: audio injection into live calls via Test SIP Provider
- Test infrastructure: ASR accuracy tests, pipeline WER tests, LLaMA quality tests, codec quality tests
- **tomedo-crawl** configuration: Tomedo server IP/port, mTLS certificate upload, Ollama subservice management, crawl schedule, vector store status

---

### 10. tomedo-crawl (`bin/tomedo-crawl`)

RAG sidecar that crawls a Tomedo EMR server and provides per-patient context to the LLaMA service.

**Components:**
- **Tomedo crawler**: fetches patient list, diagnoses, medications, appointments, and phone numbers via mutual TLS HTTPS.
- **Vector store**: hnswlib HNSW in-memory ANN index + SQLite persistence (encrypted with SQLCipher).
- **Phone index**: local SQLite table mapping digit-normalised phone numbers to patient IDs; enables sub-100 ms caller identification from a SIP phone number.
- **Ollama client**: calls `POST /api/embeddings` to generate float32 embeddings for each text chunk.
- **HTTP API** (port 13181): serves `/health`, `/query`, `/caller`, `/crawl/trigger`, `/ollama/*`, `/config`.

**Command-Line Parameters:**

| Argument | Default | Description |
|----------|---------|-------------|
| `[db-path]` | `tomedo-crawl.db` | Path to the encrypted SQLite database |
| `--verbose` | off | Enable DEBUG log level |
| `--skip-initial-crawl` | off | Do not crawl at startup |
| `--phone-only` | off | Update phone index only, skip embeddings |
| `--no-embed` | off | Index phone numbers without generating embeddings |
| `--top-k N` | `3` | Default result count for /query |
| `--chunk-size N` | `512` | Text chunk size in estimated tokens |
| `--overlap N` | `64` | Token overlap between consecutive chunks |
| `--workers N` | `4` | Embedding worker thread count |

See [`docs/tomedo-crawl.md`](docs/tomedo-crawl.md) for the full API reference, database schema, Tomedo API details, and security model.

---

## Interconnect Communication

All inter-service communication uses `interconnect.h` (a shared header, no external library):

- **Management channel** (base port +0): Typed control messages — `CALL_START`, `CALL_END`, `SPEECH_ACTIVE`, `SPEECH_IDLE`, `PING`/`PONG`
- **Data channel** (base port +1): Binary `Packet` frames — variable-length payloads tagged with `call_id` and `PacketType` (audio PCM, text, G.711)
- **Command port** (base port +2): TCP command interface, one connection per request, 10s recv timeout
- **TCP_NODELAY**: Enabled on all connections for minimum latency
- **Auto-reconnect**: Downstream connections retry every 200ms until reachable; upstream server accepts reconnections at any time
- **LogForwarder**: Sends structured log entries as UDP datagrams to `FRONTEND_LOG_PORT` (22022)

---

## Log Level Control

Every service supports 5 levels: `ERROR`, `WARN`, `INFO`, `DEBUG`, `TRACE`.

**Three ways to set log level:**

1. **Startup argument**: `--log-level DEBUG`
2. **Frontend UI**: Log level checkboxes — applied immediately to the running service, persisted in SQLite for restarts
3. **Direct command**: Send `SET_LOG_LEVEL:DEBUG` to the service's cmd port via TCP

---

## Testing

### Pipeline WER Test

```bash
python3 tests/run_pipeline_test.py <MODEL_NAME> [TESTFILES_DIR]
```

Injects WAV samples through the full pipeline via Test SIP Provider, collects Whisper transcriptions from the frontend log API, and computes character-level similarity against ground truth.

- **PASS**: ≥ 99.5% similarity
- **WARN**: ≥ 90% similarity  
- **FAIL**: < 90% similarity

Test samples: `Testfiles/sample_NN.wav` + `sample_NN.txt` pairs.

### Test SIP Provider (`bin/test_sip_provider`)

B2BUA test tool that injects audio files into the pipeline as if they were real phone calls. Supports WAV recording of both legs of each conference call.

```bash
./test_sip_provider --port 5060 --http-port 22011 --testfiles-dir Testfiles
```

**HTTP API (port 22011):**

| Method | Endpoint | Description |
|--------|----------|-------------|
| POST | `/conference` | Create a test call with optional audio injection |
| POST | `/hangup` | Hang up a call |
| GET | `/calls` | List active calls |
| GET/POST | `/wav_recording` | Read/write WAV recording settings |
| POST | `/inject` | Inject an audio file into a call leg |

### Audio Quality Collection (`tests/run_stage7.py`)

End-to-end diagnostic script:

```bash
python3 tests/run_stage7.py [--iterations N]
```

Starts all services, connects test calls, enables WAV recording, injects samples, collects logs, and saves WAV files from both OAP and Test SIP Provider. Produces `stage7_output/run_N/` directories with `pipeline.log`, `oap_call_*.wav`, and `tsp_call_*.wav`.

### Unit/Integration Tests

```bash
./runmetobuildeverything
cd build && ctest --output-on-failure
```

Tests are built by default (`BUILD_TESTS=ON`). To skip them: `./runmetobuildeverything --no-tests`

Test binaries: `test_sanity`, `test_interconnect`, `test_kokoro_cpp`, `test_integration`.

---

## Whisper Model Benchmarks

**Hardware**: Apple M4, macOS 25.2.0  
**whisper.cpp**: v1.8.3 (CoreML + Metal)

### whisper-cli Direct Test (5 samples, clean 16kHz PCM)

All model/backend combinations achieved 5/5 perfect transcription on clean input.

| Model | Size | Backend | Avg Time |
|-------|------|---------|----------|
| large-v3 | 2.9 GB | CoreML + ANE | ~2580ms |
| large-v3-q5_0 | 1.0 GB | CoreML + ANE | ~2075ms |
| large-v3-turbo | 1.5 GB | CoreML + ANE | ~1575ms |
| large-v3-turbo-q5_0 | 547 MB | CoreML + ANE | ~1060ms |

### Full Pipeline Test (20 samples, G.711 μ-law via SIP/RTP)

Audio path: WAV → 8kHz μ-law G.711 → RTP → SIP Client → IAP (8→16kHz) → Whisper

| Model | Size | Backend | PASS | WARN | FAIL | Avg ms |
|-------|------|---------|------|------|------|--------|
| **large-v3** | **2.9 GB** | **Metal** | **12** | **8** | **0** | **1627** |
| large-v3 | 2.9 GB | CoreML | 11 | 8 | 1* | 1301 |
| large-v3-q5_0 | 1.0 GB | Metal | 11 | 9 | 0 | 1789 |
| large-v3-turbo | 1.5 GB | CoreML | 9 | 10 | 1* | 688 |
| large-v3-turbo-q5_0 | 547 MB | CoreML | 8 | 11 | 1** | 686 |

\* CoreML warmup caused first-inference timeout  
\** Sample_01 failed (41.8%) due to CoreML warmup causing VAD to miss first half

**Scoring**: PASS = ≥99.5% similarity, WARN = ≥90%, FAIL = <90%

### Recommendations

- **Accuracy priority**: `large-v3` + Metal — 1627ms avg, no warmup delay, best accuracy
- **Speed priority**: `large-v3-turbo` + CoreML — 688ms avg after initial warmup

### Key Findings

1. **Quantization (q5_0)**: negligible accuracy impact vs. full-precision models
2. **CoreML warmup**: 20–35s first-inference compilation cost, one-time per service lifetime
3. **Turbo trade-off**: ~2× faster, slightly more WARN results. 4-layer decoder occasionally misses nuances in G.711-degraded audio.
4. **Consistent failures**: some samples fail across all model configs due to G.711 codec artifacts, not model limitations
