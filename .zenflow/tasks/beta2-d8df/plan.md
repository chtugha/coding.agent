# Full SDD workflow

## Configuration
- **Artifacts Path**: {@artifacts_path} → `.zenflow/tasks/{task_id}`

---

## Agent Instructions

If you are blocked and need user clarification, mark the current step with `[!]` in plan.md before stopping.

---

## Workflow Steps

### [x] Step: Requirements
<!-- chat-id: f85e16a6-bb33-46cf-97df-acd8a457481c -->

Create a Product Requirements Document (PRD) based on the feature description.

1. Review existing codebase to understand current architecture and patterns
2. Analyze the feature definition and identify unclear aspects
3. Ask the user for clarifications on aspects that significantly impact scope or user experience
4. Make reasonable decisions for minor details based on context and conventions
5. If user can't clarify, make a decision, state the assumption, and continue

Save the PRD to `{@artifacts_path}/requirements.md`.

### [x] Step: Technical Specification
<!-- chat-id: 8992f0b3-0b0b-4d48-a198-0b9bb8c3f61a -->

Create a technical specification based on the PRD in `{@artifacts_path}/requirements.md`.

1. Review existing codebase architecture and identify reusable components
2. Define the implementation approach

Save to `{@artifacts_path}/spec.md` with:
- Technical context (language, dependencies)
- Implementation approach referencing existing code patterns
- Source code structure changes
- Data model / API / interface changes
- Delivery phases (incremental, testable milestones)
- Verification approach using project lint/test commands

### [x] Step: Planning
<!-- chat-id: f72716f3-231b-451b-be06-6ddadd87fc62 -->

Create a detailed implementation plan based on `{@artifacts_path}/spec.md`.

1. Break down the work into concrete tasks
2. Each task should reference relevant contracts and include verification steps
3. Replace the Implementation step below with the planned tasks

Rule of thumb for step size: each step should represent a coherent unit of work (e.g., implement a component, add an API endpoint). Avoid steps that are too granular (single function) or too broad (entire feature).

Important: unit tests must be part of each implementation task, not separate tasks. Each task should implement the code and its tests together, if relevant.

Save to `{@artifacts_path}/plan.md`.

### [x] Step: Stage 1 — SIP Client RTP Routing Test
<!-- chat-id: dd3b8e11-af65-4135-b0c4-675af6070b75 -->

Test SIP Client RTP routing and TCP connection handling with IAP.

**Pre-requisites**: Build all binaries (`mkdir -p build && cd build && cmake .. && make`).

1. **Start frontend + test_sip_provider** via Playwright:
   - Launch `bin/frontend` and `bin/test_sip_provider`
   - Open `http://localhost:8080`, navigate to Pipeline Services page
   - Start SIP Client service (only 1 line configured)
2. **RTP discard test** (IAP not running):
   - Navigate to Beta Testing page
   - Select a test file (e.g. `sample_02.wav`), inject audio via "Inject Audio" button
   - Read RTP metrics table: verify RX packets > 0, Discarded > 0, Forwarded = 0
   - Read logs via Live Logs page to confirm SIP Client received packets
3. **IAP start/stop/reconnection cycle**:
   - Start IAP via Pipeline Services page
   - Verify TCP Connection Status shows "Connected"
   - Re-inject audio, verify Forwarded > 0, Discarded = 0
   - Stop IAP, verify TCP status shows "Disconnected"
   - Re-inject audio, verify packets are discarded again
   - Restart IAP, verify reconnection and forwarding resumes
   - Repeat start/stop 2-3 times to confirm reliability
4. **Bug fixing**: Fix any issues found in SIP Client TCP handling, RTP forwarding, service management, or frontend UI. Clean dead code. Look for stubs, simulations or Magic numbers and replace them.
5. **Verification**: RTP metrics toggle correctly between forwarded/discarded based on IAP state. All operations driven via Playwright clicking frontend buttons.

**Files**: `sip-client-main.cpp`, `inbound-audio-processor.cpp`, `interconnect.h`, `frontend.cpp`, `tests/test_sip_provider.cpp`

### [x] Step: Stage 2 — IAP Codec Quality Optimization
<!-- chat-id: 38ad8d86-d009-4608-b58c-b6208c82a255 -->

Optimize IAP audio processing for speed and quality.

1. **Run IAP codec quality test** via Beta Testing page:
   - Navigate to Beta Testing → Test 2: IAP Codec Quality
   - Select each test file (or all 20), run quality test
   - Read results: SNR, THD, latency per file
2. **Profile per-packet latency** (total target: 5-15ms per packet, max 50ms):
   - If total latency > 15ms, investigate bottlenecks in G.711 decode and FIR upsample
   - The 15-tap FIR on 160 samples should be sub-millisecond on Apple Silicon; focus on overall pipeline latency
   - Ensure FIR history persists across packet boundaries (`CallState::fir_history`)
3. **Audio enhancement investigation**:
   - Evaluate adding pre-emphasis filter (6dB/octave high-pass) for speech clarity
   - Only add if processing adds < 1ms overhead
   - Benchmark before/after with SNR metrics
4. **Optimize**: Eliminate any large buffers; ensure total per-packet processing < 50ms, target 5-15ms
5. **Verification**: All 20 files pass (SNR >= 3dB, THD <= 80%, latency <= 50ms). Results visible in Beta Testing page via Playwright.

**Files**: `inbound-audio-processor.cpp`, `frontend.cpp` (IAP quality test handler)

### [x] Step: Stage 3 — VAD Service Optimization
<!-- chat-id: 45290a4e-67e9-476e-ae4a-c5c2d405fc58 -->

Optimize VAD for smooth, fast operation and correct interconnection.

1. **Review VAD logic** in `vad-service.cpp`:
   - Energy-based detection with adaptive noise floor
   - Micro-pause detection (400ms silence = 8 frames × 50ms)
   - Chunk constraints: min 0.5s, max 4s (`vad_max_speech_samples_` = 64000)
   - Pre-speech context (4 frames)
   - Inactivity flush (1000ms timeout)
2. **Address long-file chunking**:
   - 15 of 20 test files exceed 4s and will be force-split
   - Add `--vad-max-chunk-ms` CLI flag to make max chunk configurable
   - Consider increasing to 5-6s or adding smart-split (energy dip detection near boundary)
   - Expose via `/api/whisper/vad_config` (note: this endpoint lives on the frontend for historical reasons, not on the VAD service itself) and add slider to Beta Testing UI
3. **Test interconnection**:
   - Start pipeline: SIP Client + IAP + VAD (via Pipeline Services page)
   - Inject audio, verify VAD segments arrive downstream
   - Test SPEECH_ACTIVE/SPEECH_IDLE signal propagation
   - Test VAD behavior when Whisper is not yet connected (buffering/discarding)
4. **Optimize performance**:
   - Profile VAD frame processing time
   - Tune parameters: window, threshold, silence timeout
   - Ensure no unnecessary copies or allocations in hot path
5. **Verification**: VAD correctly segments all 20 files. Signals propagate. Parameters configurable from frontend. Final accuracy confirmed jointly with Stage 4.

**Files**: `vad-service.cpp`, `frontend.cpp` (VAD config UI, accuracy test), `interconnect.h`

### [x] Step: Stage 4 — Whisper Accuracy Testing & Optimization
<!-- chat-id: 25ae514f-334e-4a1b-aca1-f2abfa30c498 -->

Achieve excellent transcription accuracy with fast processing.

1. **Test TCP interconnection**:
   - Start full pipeline up to Whisper: SIP Client + IAP + VAD + Whisper (via Pipeline Services)
   - Verify Whisper connects to VAD and LLaMA port management
   - Test packet buffering when LLaMA is disconnected
2. **Run Whisper Accuracy Test** for all 20 files via Beta Testing page:
   - Select all 20 files in the accuracy test multi-select
   - Click "Run Accuracy Test"
   - Read per-file results: accuracy %, latency ms
   - Read summary: total, pass/warn/fail counts, average accuracy
3. **Iterate on VAD + Whisper parameters**:
   - If accuracy < 95% average, adjust VAD parameters (window, threshold, max chunk)
   - Test hallucination filter effectiveness on German speech
   - Verify greedy decoding + temperature fallback works correctly
   - Re-run accuracy test after each parameter change
4. **Optimize for split chunks**:
   - Check files that are VAD-split (samples 01, 05, 06, 09, 10, 11-20)
   - Ensure each split chunk produces >= 90% accuracy individually
   - If force-split at 4s degrades accuracy, adjust max chunk or add smart-split
5. **Bug fixing**: Fix hallucination issues, encoding problems, connection handling bugs
6. **Verification**: Average accuracy >= 95% across all 20 files. Each split chunk >= 90%. All results visible in frontend.

**Files**: `whisper-service.cpp`, `vad-service.cpp`, `frontend.cpp` (accuracy test handler/UI)

### [x] Step: Stage 5 — Credentials UI
<!-- chat-id: e1097987-e861-43f1-80fd-466c9c5fa978 -->

Add Credentials page to frontend for HuggingFace and GitHub tokens.

1. **Add sidebar navigation**:
   - Add new nav item `<a class="wt-nav-item" data-page="credentials">` in System section (after Database)
   - Add `showPage('credentials')` handler that calls `loadCredentials()`
2. **Add Credentials page HTML**:
   - New `<div class="wt-page" id="page-credentials">` with:
     - HuggingFace section: access token field (`input type="password"`), save button
     - GitHub section: access token field (`input type="password"`), save button
   - Style consistent with existing pages (wt-card, wt-field, wt-btn patterns)
3. **Add JS functions**:
   - `loadCredentials()`: GET `/api/settings`, populate fields (tokens will be masked as `"***"`)
   - `saveCredential(key, value)`: POST `/api/settings` with key/value
   - Status feedback on save success/failure
4. **API security**:
   - Modify GET `/api/settings` handler to mask `hf_token` and `github_token` values (return `"***"`)
   - Ensure credentials are never logged or included in SSE streams
5. **Verification**: Via Playwright — navigate to Credentials page, enter tokens, save, reload page, verify fields show masked values. Verify tokens stored in `settings` table. Verify GET `/api/settings` returns `"***"` for credential keys.

**Files**: `frontend.cpp` (sidebar nav, page HTML, JS functions, settings API handler)

### [x] Step: Stage 6 — Whisper Model Search & Benchmarking
<!-- chat-id: f0bab62b-1f76-41a9-9495-38f6fab6b8d1 -->

Search HuggingFace for better Whisper models and compare performance.

1. **Implement HuggingFace model search API** (new endpoint, does not exist yet):
   - New endpoint `POST /api/models/search` in `frontend.cpp`
   - Calls HuggingFace API (`https://huggingface.co/api/models`) with filters: task=automatic-speech-recognition, language=de
   - Uses stored `hf_token` from settings if available (unauthenticated fallback with warning)
   - Returns model list with name, downloads, likes, tags
2. **Implement model download API** (new endpoint, does not exist yet):
   - New endpoint `POST /api/models/download` in `frontend.cpp`
   - Streams model file from HuggingFace to `models/` directory
   - Requires `hf_token` for gated models (clear error if missing)
   - Progress tracking via async task
3. **Enhance Models page UI**:
   - Add search panel to Whisper Models tab: search input, filter dropdowns, results list with download buttons
   - Enhance Comparison tab with multi-chart layout: accuracy bar chart, latency bar chart, model size chart
   - Use existing Chart.js for new chart instances
4. **Benchmark alternative models**:
   - Download promising German-finetuned / CoreML-compatible Whisper models
   - Run benchmarks via existing benchmark runner on Models page
   - Compare results in enhanced comparison charts
5. **Verification**: Via Playwright — search for models, download one, register it, run benchmark, view comparison charts. All operations via frontend UI.

**Files**: `frontend.cpp` (new API endpoints, enhanced Models page HTML/JS)

### [x] Step: Stage 7 — LLaMA Service Testing & Optimization
<!-- chat-id: aa061a23-a9ec-4b37-9e5f-12d7c4403b85 -->

Test LLaMA TCP interconnection and optimize for short, clear German responses.

1. **Test TCP interconnection**:
   - Start pipeline up to LLaMA: SIP Client + IAP + VAD + Whisper + LLaMA (via Pipeline Services)
   - Verify LLaMA connects to Whisper (upstream) data port
   - Test LLaMA behavior when Kokoro (downstream) is not connected
   - Stop/start LLaMA multiple times to test reconnection
2. **Run quality tests** via Beta Testing page:
   - Navigate to Beta Testing → Test 4: LLaMA Response Quality
   - Select test prompts from `llama_prompts.json`
   - Click "Run Quality Test"
   - Read results: response text, score (keyword 40% + brevity 30% + German 30%), latency, word count
3. **Optimize system prompt**:
   - Tune German system prompt for concise responses within per-prompt `max_words` limits (15-30)
   - Ensure responses stay in German
   - Optimize for fast token generation via Metal acceleration
4. **Test shut-up mechanism** (prep for Stage 9):
   - Verify `SPEECH_ACTIVE` management message is handled
   - Verify generation stops when `generating = false` is set
5. **Verification**: Average score > 70%. Responses within max_words. German detected. TCP reconnection works. All via Playwright.

**Files**: `llama-service.cpp`, `frontend.cpp` (LLaMA quality test handler/UI)

### [x] Step: Stage 8 — LLaMA Model Search & Benchmarking
<!-- chat-id: b3543f1d-65f0-4a11-86c0-72cb80bde6cf -->

Search HuggingFace for better LLaMA models and compare performance.

1. **Extend model search for LLaMA** (reuses `/api/models/search` and `/api/models/download` endpoints built in Stage 6):
   - Add task filter `text-generation` to `/api/models/search`
   - Filter for GGUF format, German-capable models
2. **Benchmark LLaMA models**:
   - Register alternative models via Models page → LLaMA Models tab
   - Run benchmarks via existing LLaMA benchmark runner
   - Measure: latency, quality score, tokens/sec, German compliance
3. **Enhance comparison charts**:
   - Add LLaMA-specific metrics to Comparison tab (tokens/sec, quality score)
   - Ensure both Whisper and LLaMA benchmarks are visible in unified comparison view
4. **Verification**: Via Playwright — search, download, register LLaMA model, benchmark, view comparison. Charts render correctly with both Whisper and LLaMA data.

**Files**: `frontend.cpp` (model search/download/benchmark, comparison charts)

### [x] Step: Stage 9 — Shut-Up Mechanism Testing
<!-- chat-id: cddf4fbf-a308-46c3-a59f-25ecdf2e3ed3 -->

Validate interrupt/barge-in functionality.

1. **Start full pipeline** up to OAP: SIP Client + IAP + VAD + Whisper + LLaMA + Kokoro + OAP (via Pipeline Services) — OAP is needed for Kokoro audio to reach the SIP Client and actually stop playback
2. **Test interrupt flow** via Beta Testing page:
   - Click "Shut-up Test" button in LLaMA test card
   - This injects speech to trigger LLaMA response, then injects interrupting speech mid-generation
   - Verify SPEECH_ACTIVE signal stops Kokoro playback
   - Verify LLaMA cancels current generation
3. **Measure interrupt latency**:
   - Time from speech detection (VAD SPEECH_ACTIVE) to TTS stop
   - Target: < 500ms
4. **Test edge cases**:
   - Interrupt at very start of generation
   - Interrupt near end of generation
   - Rapid successive interrupts
5. **Verification**: Shut-up test shows interrupt within < 500ms. Generation stops. TTS stops. Results visible in frontend.

**Files**: `llama-service.cpp`, `kokoro-service.cpp`, `vad-service.cpp`, `frontend.cpp` (shut-up test handler)

### [x] Step: Stage 10 — Kokoro TTS Testing & Optimization
<!-- chat-id: 05e1523c-263d-46a2-a265-6cfe282b50c4 -->

Test Kokoro for interconnection, quality, and speed.

1. **Test TCP interconnection**:
   - Verify Kokoro connects to LLaMA (upstream) and OAP (downstream)
   - Test cmd port 13142 `TEST_SYNTH:<text>` interface
   - Stop/start Kokoro to test reconnection
2. **Run quality test** via Beta Testing page:
   - Navigate to Beta Testing → Test 5: Kokoro TTS Quality
   - Enter German test phrases
   - Click "Run Quality Test"
   - Read results: synthesis latency, RTF, audio characteristics
3. **Run benchmark**:
   - Select iteration count (3/5/10)
   - Click "Benchmark" button
   - Read average latency, RTF across iterations
4. **Optimize**:
   - Verify CoreML ANE acceleration is active
   - Optimize for RTF < 1.0 (faster than real-time)
   - Verify espeak-ng phonemization for German
   - Fix any bugs in audio streaming to OAP
5. **Verification**: Quality test passes. RTF < 1.0. Natural German speech output. All via Playwright.

**Files**: `kokoro-service.cpp`, `frontend.cpp` (Kokoro quality test/benchmark handler/UI)

### [ ] Step: Stage 11 — OAP & Full Loop Test

Test outbound audio processing and validate full-loop audio fidelity with 2 lines.

1. **Test OAP interconnection**:
   - Start OAP via Pipeline Services
   - Verify TCP connection to Kokoro (upstream) and SIP Client (downstream)
   - Verify 24kHz→8kHz downsampling + G.711 encoding
   - Verify 20ms scheduling timer accuracy
2. **Connect second SIP line**:
   - Navigate to Beta Testing → SIP Lines Management
   - Add second line via UI
   - Verify both lines registered with test_sip_provider
3. **Implement full-loop test**:
   - Add full-loop test button/card to Beta Testing page (or reuse existing flow)
   - Add `calculate_word_error_rate()` function (word-level Levenshtein, case-insensitive, punctuation-stripped)
   - Add `/api/full_loop_test` endpoint if needed
4. **Run full-loop test**:
   - Inject one test file through Line 1 pipeline
   - LLaMA generates response → Kokoro synthesizes → OAP encodes → SIP Client → Line 2
   - Line 2 audio feeds back to Whisper for re-transcription
   - Compare LLaMA's original text vs Line 2 Whisper transcription
   - Target WER <= 10%
5. **Verification**: Full loop test passes with WER <= 10%. Both texts logged side-by-side. All via Playwright.

**Files**: `outbound-audio-processor.cpp`, `sip-client-main.cpp`, `frontend.cpp` (full-loop test UI/handler, WER function)

### [ ] Step: Stage 12 — Full Pipeline Stress Test

2-minute continuous operation test to find and fix bottlenecks.

1. **Add stress test UI** to Beta Testing page (if not already present):
   - Start/stop button for continuous 2-minute test
   - Real-time metrics display: per-service latency, memory, CPU, packet loss, buffer overflows
   - Progress indicator (elapsed time / 2 minutes)
2. **Run stress test** via Playwright:
   - Start full pipeline via Pipeline Services
   - Navigate to Beta Testing, start stress test
   - Monitor metrics for 2 minutes of continuous audio injection
   - Read final results: stability, throughput, error count
3. **Identify and fix bottlenecks**:
   - Analyze per-service latency distribution
   - Check for memory leaks (growing RSS over time)
   - Check for packet loss or buffer overflows
   - Optimize any service exceeding latency targets
4. **Final code cleanup sweep** (note: each prior stage should clean code incrementally; this is a final pass):
   - Remove any remaining dead code across all services
   - Ensure all services log correctly to frontend
   - Verify crash-proof logging throughout
5. **Verification**: 2 minutes stable operation. No crashes. No memory leaks. Acceptable latency. All metrics visible in frontend.

**Files**: All service files, `frontend.cpp` (stress test UI/handler), `interconnect.h`
