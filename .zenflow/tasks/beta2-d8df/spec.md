# Technical Specification: Beta-Stage Testing & Optimization

## 1. Technical Context

### 1.1 Language & Build
- **Language**: C++17 (services), embedded HTML/CSS/JS (frontend)
- **Build**: CMake 3.22+, `cmake .. && make` from `build/` directory
- **Platform**: macOS Apple Silicon (CoreML/Metal acceleration)
- **Dependencies**: whisper.cpp (ASR), llama.cpp (LLM), libtorch + espeak-ng (TTS), Mongoose (HTTP/WebSocket), SQLite3 (storage), Chart.js + Bootstrap 5 (UI)
- **Binaries**: Output to `bin/` directory

### 1.2 Source Files
| File | Lines | Role |
|------|-------|------|
| `frontend.cpp` | ~6900 | Monolithic frontend: HTTP server, embedded UI, API endpoints, test orchestration |
| `interconnect.h` | ~1210 | Shared header: TCP peer-to-peer communication, packet format, LogForwarder |
| `sip-client-main.cpp` | ~617 | SIP/RTP gateway, multi-line support, cmd port 13102 |
| `inbound-audio-processor.cpp` | ~228 | G.711 u-law decode + 8kHz→16kHz upsample (15-tap FIR) |
| `vad-service.cpp` | ~450 | Energy-based VAD, micro-pause detection, speech signal broadcast |
| `whisper-service.cpp` | ~345 | Greedy ASR via whisper.cpp, hallucination filter |
| `llama-service.cpp` | ~492 | LLaMA inference via llama.cpp, shut-up mechanism |
| `kokoro-service.cpp` | ~1097 | TTS via PyTorch/CoreML, espeak-ng phonemization |
| `outbound-audio-processor.cpp` | ~220 | 24kHz→8kHz downsample + G.711 encode + RTP scheduling |
| `tests/test_sip_provider.cpp` | ~1025 | SIP B2BUA test harness with audio injection + HTTP API (port 22011) |

### 1.3 Communication Architecture
All inter-service TCP on 127.0.0.1. Each service has:
- **mgmt port** (base+0): management messages (CALL_END, SPEECH_ACTIVE/IDLE, PING/PONG, CUSTOM)
- **data port** (base+1): audio/text data packets (8-byte header: 4-byte call_id + 4-byte payload_size + payload)
- **cmd port** (base+2): out-of-band frontend commands (SIP Client: ADD_LINE/GET_STATS; LLaMA: TEST_PROMPT; Kokoro: TEST_SYNTH)

Upstream listens, downstream connects. Auto-reconnect on disconnect via `downstream_connect_loop()`.

### 1.4 Frontend Architecture
- Single `frontend.cpp` file with embedded HTML/CSS/JS via raw string literals
- Mongoose HTTP server on port 8080
- SQLite database `frontend.db` with tables: logs, test_runs, service_status, service_config, settings, testfiles, whisper_accuracy_tests, iap_quality_tests, models, model_benchmark_runs, tts_validation_tests, sip_lines, service_test_runs
- SSE log streaming on `/api/logs/stream`
- UDP log receiver on port 22022 (services → frontend)
- Process management: fork/exec services, track PIDs, SIGTERM/SIGKILL lifecycle
- Async test execution via `AsyncTask` (thread-per-task with polling via `/api/async/status`)

### 1.5 Existing UI Pages
Sidebar navigation (in `build_ui_html()`):
- **Testing**: Tests, Beta Testing, Models
- **Services**: Pipeline Services
- **System**: Live Logs, Database

### 1.6 Existing API Endpoints
Tests: `/api/tests`, `/api/tests/start`, `/api/tests/stop`, `/api/tests/*/history`, `/api/tests/*/log`
Services: `/api/services`, `/api/services/start`, `/api/services/stop`, `/api/services/restart`, `/api/services/config`
Logs: `/api/logs`, `/api/logs/stream`, `/api/logs/recent`
DB: `/api/db/query`, `/api/db/write_mode`, `/api/db/schema`
Settings: `/api/settings`, `/api/settings/log_level`
SIP: `/api/sip/add-line`, `/api/sip/remove-line`, `/api/sip/lines`, `/api/sip/stats`
IAP: `/api/iap/quality_test`
Whisper: `/api/whisper/models`, `/api/whisper/accuracy_test`, `/api/whisper/vad_config`, `/api/whisper/accuracy_results`, `/api/whisper/benchmark`
LLaMA: `/api/llama/prompts`, `/api/llama/quality_test`, `/api/llama/shutup_test`, `/api/llama/benchmark`
Kokoro: `/api/kokoro/quality_test`, `/api/kokoro/benchmark`
Models: `/api/models`, `/api/models/add`, `/api/models/benchmarks`
Misc: `/api/status`, `/api/testfiles`, `/api/testfiles/scan`, `/api/test_results`, `/api/async/status`

### 1.7 Test Infrastructure
- 20 WAV test files (`Testfiles/sample_01.wav` – `sample_20.wav`) with matching `.txt` ground-truth files
- 10 LLaMA prompts in `Testfiles/llama_prompts.json` with per-prompt `max_words` and `expected_keywords`
- `test_sip_provider`: SIP B2BUA with HTTP API for audio injection, call management, RTP relay
- All testing driven via Playwright browser automation against `http://localhost:8080`

---

## 2. Implementation Approach

### 2.1 Core Principles
1. **Frontend-driven**: All testing via Playwright clicking buttons in the browser UI, not direct API calls
2. **Sequential stages**: Each stage fully completes (all bugs fixed, tests passing) before proceeding
3. **Incremental pipeline**: Start with SIP Client alone, add one service per stage
4. **No stubs**: Every function has real implementation; fix dependencies before marking complete
5. **Performance-first**: Optimize for near-real-time latency with excellent accuracy

### 2.2 Browser Automation Strategy
- Playwright (via the `playwright` skill) will be used to interact with `http://localhost:8080`
- Navigate sidebar pages, click buttons, read test results, configure settings
- Wait for async test completion by polling UI elements
- Read service logs via Live Logs page or embedded log views in Beta Testing page
- Start/stop services via Pipeline Services page

### 2.3 Test Execution Pattern (per stage)
1. Start required services via Pipeline Services page
2. Navigate to Beta Testing page
3. Run stage-specific test via UI button
4. Read results from UI (tables, metrics, logs)
5. If failures: debug via logs, fix code, rebuild, restart, re-test
6. When passing: clean up code, optimize, verify

---

## 3. Source Code Structure Changes

### 3.1 New: Credentials UI Section (Stage 5)

**Frontend changes** (`frontend.cpp`):
- Add new sidebar nav item `<a class="wt-nav-item" data-page="credentials">` in the "System" sidebar section, after the Database nav item
- Add new page `<div class="wt-page" id="page-credentials">` with:
  - HuggingFace section: access token field (`input type="password"`), save button
  - GitHub section: access token field (`input type="password"`), save button
- Add JS functions: `loadCredentials()`, `saveCredential(key, value)` using existing `/api/settings` endpoint
- Storage keys in `settings` table: `hf_token`, `github_token`
- Add `showPage('credentials')` handler to load credentials on page visit

**Storage security**: Credentials are stored as plaintext in the SQLite `settings` table on disk. This is an **accepted tradeoff** for a local single-machine development/testing tool. Credentials MUST NOT be:
- Logged by any service (LogForwarder or stdout/stderr)
- Echoed in API responses (the `/api/settings` GET response must mask credential values for the explicit allow-list `hf_token`, `github_token` only — returning `"***"` for those keys; all other settings are returned normally)
- Included in SSE log streams

**API changes**: Reuse existing `/api/settings` (GET/POST). No new endpoints needed. Settings API already supports arbitrary key-value pairs. Credentials will be masked by:
- Using `input type="password"` in UI
- Masking credential values in GET `/api/settings` responses (return `"***"` instead of actual value for `hf_token`, `github_token`)
- Never including credential values in SSE log streams
- Never logging credential values in service logs

### 3.2 Enhanced: Models Page (Stage 6, 8)

**Frontend changes** (`frontend.cpp`):
- Enhance Models page comparison tab with:
  - Multi-chart layout: accuracy bar chart, latency bar chart, model size comparison
  - Chart.js already loaded; add new chart instances for side-by-side comparison
  - Tabbed view: Whisper Models / LLaMA Models / Comparison
- Add HuggingFace model search functionality:
  - New API endpoint: `POST /api/models/search` (uses HF API `https://huggingface.co/api/models`)
  - Search filters: task type (ASR/text-generation), language (de), format (gguf/ggml)
  - Use stored `hf_token` from settings for authenticated API access
  - **Token-missing handling**: If `hf_token` is empty or not set, the search endpoint must still work (unauthenticated HF API), but display a warning in the UI: "No HuggingFace token configured. Search results may be limited by rate limiting (100 req/hr). Set a token in the Credentials page." If the HF API returns HTTP 401 (expired token) or 429 (rate limited), the error must be surfaced in the UI with a clear message and a link to the Credentials page.
  - Results displayed in a searchable list with download button
- Add model download: `POST /api/models/download` (streams from HF, saves to `models/`)
  - **Auth requirement**: Model download requires a valid `hf_token` for gated models. If the token is missing or invalid, return a clear error: "HuggingFace token required for this model. Set it in Credentials page."

**Database**: Existing `models` and `model_benchmark_runs` tables are sufficient.

### 3.3 Service Optimizations

#### IAP (`inbound-audio-processor.cpp`) — Stage 2
- Profile and measure per-packet processing time
- Investigate pre-emphasis filter (6dB/octave high-pass) for speech clarity, only if adds < 1ms
- Verify 15-tap FIR filter latency is within 5-15ms target
- Ensure FIR history state persists across packet boundaries (already implemented in `CallState::fir_history`)

#### VAD (`vad-service.cpp`) — Stage 3
- Review and tune: `vad_frame_size_` (50ms), `vad_threshold_mult_` (2.0), `vad_silence_frames_` (8 = 400ms), `vad_max_speech_samples_` (4s)
- Key optimization: `vad_max_speech_samples_` is hardcoded at 64000 (4s × 16kHz). For long files (>4s), force-split at 4s may cut mid-word. Options:
  - Increase to 5-6s if Whisper latency remains acceptable
  - Add smart-split: look for energy dips near the max boundary
  - Make configurable via CLI flag `--vad-max-chunk-ms`
- Verify SPEECH_ACTIVE/SPEECH_IDLE signal timing
- Test inactivity flush (1000ms timeout)

#### Whisper (`whisper-service.cpp`) — Stage 4
- Verify greedy decoding + temperature fallback
- Test hallucination filter effectiveness on German speech
- Optimize for the 20 test files: measure per-file accuracy + latency
- If accuracy < 95% on split chunks, coordinate with VAD parameter tuning
- Packet buffering: verify 64-packet buffer works when LLaMA is disconnected

#### LLaMA (`llama-service.cpp`) — Stage 7
- Test response quality with 10 prompts from `llama_prompts.json`
- Scoring: 40% keyword match + 30% brevity + 30% German detection (existing `score_llama_response()`)
- Optimize system prompt for concise German responses within per-prompt `max_words` limits
- Verify shut-up mechanism: `SPEECH_ACTIVE` → `generating = false` interrupt

#### Kokoro (`kokoro-service.cpp`) — Stage 10
- Test CoreML ANE acceleration path
- Measure RTF (Real-Time Factor), target < 1.0
- Verify espeak-ng phonemization for German
- Test cmd port 13142 `TEST_SYNTH:<text>` interface

#### OAP (`outbound-audio-processor.cpp`) — Stage 11
- Verify 24kHz→8kHz downsampling with 15-tap AA filter
- Verify G.711 u-law encoding
- Verify 20ms scheduling timer accuracy
- Test multi-call buffer isolation

### 3.4 SIP Client Multi-Line Testing (Stage 11)

**Approach**:
1. Configure SIP Client with 2 lines via frontend Pipeline Services page
2. Line 1: normal pipeline (audio → IAP → VAD → Whisper → LLaMA → Kokoro → OAP → Line 2)
3. Line 2: receives synthesized audio, feeds back into pipeline for Whisper re-transcription
4. Compare LLaMA's original text vs Line 2's Whisper transcription using word-level WER (case-insensitive, punctuation stripped)
5. WER threshold: ≤ 10%

**Frontend changes**: Add full-loop test button to Beta Testing page or reuse existing test flow.

---

## 4. Data Model / API / Interface Changes

### 4.1 New API Endpoints

| Method | Path | Purpose | Stage |
|--------|------|---------|-------|
| POST | `/api/models/search` | Search HuggingFace for models | 6, 8 |
| POST | `/api/models/download` | Download model from HuggingFace | 6, 8 |

### 4.2 New Settings Keys

| Key | Type | Purpose | Stage |
|-----|------|---------|-------|
| `hf_token` | string | HuggingFace access token | 5 |
| `github_token` | string | GitHub access token | 5 |

### 4.3 Existing API Extensions
- `/api/whisper/accuracy_test`: Already supports multi-file async testing. May need VAD parameter pass-through.
- `/api/models/benchmarks`: Already returns comparison data. May need enhanced chart rendering.

### 4.4 VAD Configuration Extensions
- New CLI flag for `vad-service`: `--vad-max-chunk-ms <ms>` (default: 4000)
- Expose via `/api/whisper/vad_config` GET/POST
- Add slider to Whisper Accuracy Test card in Beta Testing page

---

## 5. Delivery Phases

### Phase 1: Foundation & SIP Client (Stage 1)
- Start `test_sip_provider` and `sip-client` via frontend
- Use Playwright to inject audio, verify RTP metrics in Beta Testing page
- Test IAP start/stop/reconnection cycle
- Fix any bugs in SIP Client TCP handling, RTP forwarding, or frontend service management
- **Verification**: RTP packets show in stats table, forwarded count increments when IAP is up, discarded when IAP is down

### Phase 2: IAP Optimization (Stage 2)
- Run IAP codec quality test via Beta Testing page
- Measure SNR, THD, latency for all 20 test files
- Optimize if latency > 15ms; investigate audio enhancement
- **Verification**: All files PASS (SNR ≥ 3dB, THD ≤ 80%, latency ≤ 50ms), typical latency 5-15ms

### Phase 3: VAD Optimization (Stage 3)
- Start pipeline up to VAD (SIP Client + IAP + VAD)
- Review VAD logic, tune parameters for long files
- Test SPEECH_ACTIVE/SPEECH_IDLE signal propagation
- Add `--vad-max-chunk-ms` CLI flag if needed
- **Verification**: VAD correctly segments all 20 files; signals propagate downstream. Stage 3 pass gate is confirmed by Stage 4 Whisper accuracy: VAD-split chunks must produce ≥ 90% Whisper accuracy individually (no garbled partial-word transcriptions). If Stage 4 reveals VAD-induced accuracy drops on split files, return to Stage 3 to re-tune parameters. **Threshold interaction with Stage 4**: Stage 4 requires ≥ 95% *average* accuracy across all 20 files, while Stage 3 requires ≥ 90% on each *individual split chunk*. Both thresholds must be satisfied simultaneously — if the average hits 95% but specific split chunks fall below 90%, VAD parameters must be re-tuned until both gates pass.

### Phase 4: Whisper Accuracy (Stage 4)
- Start full pipeline up to Whisper (+ VAD + IAP + SIP Client)
- Run Whisper Accuracy Test for all 20 files via Beta Testing page
- Iterate on VAD + Whisper parameters until accuracy ≥ 95% on all files
- **Verification**: All 20 files PASS or WARN (≥ 80%), average accuracy ≥ 95%

### Phase 5: Credentials UI (Stage 5)
- Add Credentials page to frontend sidebar under System section
- HuggingFace and GitHub token fields with save/load via settings API
- **Verification**: Tokens saved to DB, loaded on page visit, masked in UI, not logged

### Phase 6: Whisper Model Search & Benchmarking (Stage 6)
- Add HuggingFace model search to Models page
- Add model download functionality
- Enhance comparison charts with multi-metric visualization
- Benchmark alternative Whisper models
- **Verification**: Models searchable, downloadable, benchmarkable, comparison charts render correctly

### Phase 7: LLaMA Testing (Stage 7)
- Start LLaMA service, test TCP interconnection
- Run quality tests with 10 prompts
- Optimize for short, clear German responses
- **Verification**: Average score > 70%, responses within max_words limits, German detected

### Phase 8: LLaMA Model Search & Benchmarking (Stage 8)
- Search HuggingFace for alternative LLaMA models
- Benchmark and compare in Models page
- **Verification**: LLaMA models benchmarkable, comparison charts include LLaMA metrics

### Phase 9: Shut-Up Mechanism (Stage 9)
- Test speech interrupt: inject audio during LLaMA generation
- Verify SPEECH_ACTIVE stops generation + TTS playback
- Measure interrupt latency
- **Verification**: Shut-up test shows interrupt within < 500ms, generation stops, TTS stops

### Phase 10: Kokoro TTS (Stage 10)
- Start Kokoro, test interconnection
- Run quality test and benchmark
- Optimize for RTF < 1.0 and natural German speech
- **Verification**: Kokoro quality test passes, RTF < 1.0, benchmark results in Models page

### Phase 11: Full Loop (Stage 11)
- Start OAP, connect Line 2
- Send test file through full pipeline
- Compare LLaMA text vs Line 2 Whisper transcription
- Target WER ≤ 10%
- **WER calculation method**: Case-insensitive, punctuation stripped (remove `.,!?;:"-`), then word-level Levenshtein distance: `WER = (substitutions + insertions + deletions) / reference_word_count * 100`. Reference = LLaMA's original text; hypothesis = Line 2 Whisper transcription. Note: the existing `calculate_levenshtein_similarity()` in `frontend.cpp` operates at **character-level** and cannot be reused directly. A **new** `calculate_word_error_rate()` function must be implemented that tokenizes both strings by whitespace and computes the Levenshtein edit distance on the resulting word arrays.
- **Verification**: Full loop test passes with WER ≤ 10%, both texts logged side-by-side for manual inspection

### Phase 12: Stress Test (Stage 12)
- Run full pipeline for 2 minutes continuous
- Monitor metrics: latency, memory, CPU, packet loss, buffer overflows
- Identify and optimize bottlenecks
- **Verification**: 2 minutes stable operation, no crashes, no memory leaks, acceptable latency

---

## 6. Verification Approach

### 6.1 Build Verification
```bash
cd coding.agent && mkdir -p build && cd build && cmake .. && make
```
All 8 binaries must compile cleanly with `-O2 -Wall -Wextra`.

### 6.2 Test Execution
All testing is performed via Playwright browser automation:
1. Start frontend: `bin/frontend`
2. Open `http://localhost:8080` via Playwright
3. Navigate to appropriate page, execute tests, read results
4. Service management via Pipeline Services page

### 6.3 Per-Stage Pass Criteria
| Stage | Primary Metric | Threshold |
|-------|---------------|-----------|
| 1 | RTP forward/discard toggling | IAP up → forwarded; IAP down → discarded |
| 2 | IAP codec quality | SNR ≥ 3dB, THD ≤ 80%, latency ≤ 50ms |
| 3 | VAD segmentation | Split chunks produce ≥ 90% Whisper accuracy individually; SPEECH_ACTIVE/IDLE signals propagate (confirmed in Stage 4) |
| 4 | Whisper accuracy | ≥ 95% average similarity across 20 files |
| 5 | Credentials | Token save/load works, values masked |
| 6 | Model comparison | Charts render, benchmarks stored |
| 7 | LLaMA quality | Average score > 70%, responses concise + German |
| 8 | Model comparison | LLaMA benchmarks comparable |
| 9 | Shut-up latency | Interrupt < 500ms |
| 10 | Kokoro RTF | < 1.0 (faster than real-time) |
| 11 | Full loop WER | ≤ 10% |
| 12 | Stress stability | 2 minutes, no crashes, no leaks |

### 6.4 Code Quality
- Clean dead code during each stage
- No stubs or placeholder implementations
- All services log to frontend via LogForwarder (UDP 22022)
- Log levels configurable per-service from frontend
- Crash-proof logging: log failures must not crash services
