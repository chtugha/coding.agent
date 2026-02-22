# Product Requirements Document: Beta-Testing Infrastructure

**Project**: WhisperTalk  
**Feature**: Comprehensive Beta-Stage Testing and Optimization Suite  
**Date**: 2026-02-22  
**Status**: Requirements Definition

---

## Executive Summary

This PRD defines a comprehensive beta-testing infrastructure for the WhisperTalk speech-to-speech pipeline. The system will enable systematic, frontend-driven testing of all six core services (SIP Client, IAP, Whisper, LLaMA, Kokoro, OAP) using standardized audio test files, with emphasis on performance optimization, multi-line telephony testing, and model benchmarking.

---

## Background & Context

### Current State
- **Pipeline**: 6 services in linear audio processing chain
- **Test Files**: 20 German speech samples (sample_01 to sample_20.wav) with transcription ground truth (.txt files)
- **Frontend**: Web-based control interface at http://localhost:8080
- **Test SIP Provider**: B2BUA with audio injection capability (`test_sip_provider`)
- **Hardware**: Apple Silicon (M4) with CoreML/Metal optimization

### Problem Statement
The pipeline requires systematic beta testing to:
1. Verify service interconnection resilience (TCP reconnection, master/slave failover)
2. Optimize transcription accuracy and eliminate word/sentence truncation (VAD tuning)
3. Benchmark and compare ML models (Whisper, LLaMA) for German language performance
4. Test multi-line call handling (1, 2, 4, 6 concurrent lines)
5. Validate end-to-end speech quality via round-trip testing (Kokoro → Whisper verification)

---

## Core Principles

### Development Philosophy
1. **NO STUBS OR SIMULATIONS**: Every feature must be fully functional - no placeholder implementations
2. **FIX ALL BUGS IMMEDIATELY**: No deferring issues to "later phases"
3. **FRONTEND-FIRST**: All testing conducted via browser UI, not direct API calls
4. **PERFORMANCE & QUALITY**: Target near-real-time speed with 100% accuracy
5. **COMPREHENSIVE LOGGING**: Crash-proof logging with configurable depth per service
6. **PROGRESSIVE TESTING**: Test services individually before integration
7. **THOROUGH DOCUMENTATION**: Enable future developers/agents to understand and extend the system

---

## User Stories

### US-1: Audio Injection Testing
**As a** QA engineer  
**I want to** inject pre-recorded audio files into active SIP calls via the frontend  
**So that** I can test the pipeline with standardized, repeatable inputs

**Acceptance Criteria**:
- Frontend displays list of Testfiles/*.wav files in test interface
- User can select a .wav file and click "Inject" to send it to an active call
- Audio injection works for any active line (line 1, 2, 3, or 4)
- Frontend shows injection status (in progress, completed, failed)

### US-2: Multi-Line Call Management
**As a** test operator  
**I want to** dynamically add/remove phone lines while the SIP client is running  
**So that** I can test 1-line, 2-line, 4-line, and 6-line scenarios without restarting services

**Acceptance Criteria**:
- Frontend has "SIP Lines" control panel
- User can add a new line (specify line ID, SIP credentials) via UI
- User can remove an existing line via UI
- Changes take effect immediately without service restart
- Active calls on a line are gracefully terminated before removal

### US-3: Service Interconnection Testing
**As a** reliability engineer  
**I want to** start/stop downstream services while upstream services are running  
**So that** I can verify TCP reconnection, buffering, and error handling

**Acceptance Criteria**:
- User can stop IAP while SIP client sends RTP → verify buffering or graceful discard
- User can restart IAP → verify TCP reconnection and audio stream resumption
- User can stop Whisper while IAP streams audio → verify IAP resilience
- All state transitions logged to frontend console
- No service crashes or deadlocks

### US-4: Transcription Accuracy Benchmarking
**As a** ML engineer  
**I want to** measure Whisper transcription accuracy against ground truth .txt files  
**So that** I can quantify model performance and optimize VAD settings

**Acceptance Criteria**:
- Frontend displays "Whisper Accuracy Test" page
- User selects test files (e.g., sample_01 to sample_10)
- System injects audio → captures Whisper output → compares to .txt files
- Frontend displays per-file accuracy (% similarity), avg latency, pass/fail status
- Results stored in database for historical comparison

### US-5: Model Performance Comparison
**As a** performance engineer  
**I want to** benchmark multiple Whisper and LLaMA models side-by-side  
**So that** I can select the optimal model for production deployment

**Acceptance Criteria**:
- Frontend "Models" section lists available models (from Hugging Face or local)
- User can add a new model (specify path, config, backend: CoreML/Metal)
- User can run "Model Benchmark Test" (select model, test files, iterations)
- Results displayed in comparison table (accuracy %, avg latency, memory usage)
- Interactive charts/diagrams for visual comparison
- Test results exportable as CSV/JSON

### US-6: Log Depth Configuration
**As a** developer  
**I want to** configure log verbosity (ERROR/WARN/INFO/DEBUG) per service via frontend  
**So that** I can reduce log spam during performance testing while maintaining debug capability

**Acceptance Criteria**:
- Frontend "Settings" page has "Log Configuration" section
- Dropdown per service: ERROR, WARN, INFO, DEBUG, TRACE
- Changes applied in real-time (no service restart required)
- Log filters persist across frontend page reloads
- Default: INFO for all services

### US-7: End-to-End Speech Quality Validation
**As a** quality assurance engineer  
**I want to** verify Kokoro TTS output by transcribing it on a second line  
**So that** I can measure round-trip speech accuracy (text → speech → text)

**Acceptance Criteria**:
- Line 1: User input → Whisper → LLaMA → Kokoro → OAP
- Line 2: Receives audio from Line 1 → IAP → Whisper
- Frontend displays: Original LLaMA text vs. Line 2 Whisper transcription
- System calculates similarity score (expected: 100% for perfect TTS)
- Identifies problematic phonemes or words for Kokoro optimization

### US-8: Progressive Service Testing
**As a** integration tester  
**I want to** test services in isolation before full pipeline integration  
**So that** I can identify and fix bugs at each layer

**Acceptance Criteria**:
- **Test 1**: SIP Client RTP routing (inject audio, verify RTP dump when IAP offline)
- **Test 2**: IAP conversion quality (measure resampling artifacts, SNR)
- **Test 3**: Whisper accuracy + VAD (no sentence truncation, word-level precision)
- **Test 4**: LLaMA response quality (German language, concise answers, shut-up mechanism)
- **Test 5**: Kokoro TTS quality (pronunciation, naturalness, latency)
- **Test 6**: OAP encoding/scheduling (G.711 quality, 20ms timing precision)
- Each test has dedicated frontend panel with specific controls and metrics

---

## Functional Requirements

### FR-1: Testfiles Management
- **FR-1.1**: System discovers Testfiles/*.wav and Testfiles/*.txt at runtime
- **FR-1.2**: Frontend API endpoint `/api/testfiles` returns list of available files
- **FR-1.3**: Frontend displays testfiles in dropdown/list UI component
- **FR-1.4**: Each .wav file includes metadata: duration, sample rate, file size
- **FR-1.5**: Frontend validates .wav format before injection (8kHz/16kHz PCM or μ-law)

### FR-2: Audio Injection
- **FR-2.1**: Test SIP Provider exposes HTTP API for audio injection
  - `POST /api/inject` with `{call_id, file_path, target_leg}`
- **FR-2.2**: Frontend "Inject Audio" button triggers injection to selected line
- **FR-2.3**: Audio converted to G.711 μ-law RTP packets at 20ms intervals
- **FR-2.4**: Injection supports multiple formats: WAV (PCM), WAV (μ-law), raw PCM
- **FR-2.5**: System logs injection start/stop events to frontend console
- **FR-2.6**: Graceful handling if file not found or invalid format

### FR-3: SIP Client Multi-Line Support
- **FR-3.1**: SIP client maintains 1-6 concurrent call sessions
- **FR-3.2**: Each line has independent: SIP Call-ID, RTP ports, call state
- **FR-3.3**: Frontend API: `POST /api/sip/add-line` with `{line_id, username, password, server}`
- **FR-3.4**: Frontend API: `POST /api/sip/remove-line` with `{line_id}`
- **FR-3.5**: Frontend API: `GET /api/sip/lines` returns current lines with status
- **FR-3.6**: Line add/remove triggers `ACTIVATE`/`DEACTIVATE` control messages to processors
- **FR-3.7**: Frontend displays line status: Idle, Registering, Ringing, Active, Terminated
- **FR-3.8**: Test configuration presets: 1-line, 2-line, 4-line, 6-line

### FR-4: Service Start/Stop Sequencing
- **FR-4.1**: Frontend displays service dependency graph (SIP → IAP → Whisper → LLaMA → Kokoro → OAP)
- **FR-4.2**: "Start All Services" button launches in correct order with 1s delays
- **FR-4.3**: "Stop All Services" button shuts down in reverse order
- **FR-4.4**: Individual service controls remain available for manual orchestration
- **FR-4.5**: Frontend displays TCP connection status between services (green/red indicators)

### FR-5: Whisper Accuracy Testing
- **FR-5.1**: Frontend "Whisper Test" page with test file selector (multi-select)
- **FR-5.2**: Test workflow:
  1. Inject audio file → SIP Client → IAP → Whisper
  2. Capture Whisper transcription output
  3. Load ground truth from Testfiles/sample_XX.txt
  4. Calculate Levenshtein similarity (0-100%)
  5. Display result: PASS (≥99.5%), WARN (≥90%), FAIL (<90%)
- **FR-5.3**: Results table: File, Ground Truth, Whisper Output, Similarity %, Latency, Status
- **FR-5.4**: Aggregate statistics: Total PASS/WARN/FAIL, Avg Accuracy, Avg Latency
- **FR-5.5**: Export results as JSON for archival

### FR-6: Model Benchmarking Framework
- **FR-6.1**: Frontend "Models" page with tabs: Whisper, LLaMA
- **FR-6.2**: Model registry table: Name, Path, Size, Backend, Status
- **FR-6.3**: "Add Model" form:
  - Whisper: model path, CoreML encoder path, language, backend (CoreML/Metal)
  - LLaMA: model path, context size, n_gpu_layers, quantization
- **FR-6.4**: "Run Benchmark" test:
  - Select model, test files (multi-select), iterations (1-10)
  - System restarts service with new model, runs tests, captures metrics
- **FR-6.5**: Results comparison table (side-by-side):
  - Columns: Model, Backend, Accuracy %, Avg Latency, P50/P95/P99, Memory MB
- **FR-6.6**: Interactive charts: Bar chart (latency by model), Line chart (accuracy over time)
- **FR-6.7**: "Fetch from Hugging Face" feature (optional): search and download models

### FR-7: Logging Infrastructure
- **FR-7.1**: All services send logs to Frontend UDP port 22022
- **FR-7.2**: Log format: `[timestamp] [service] [level] [call_id] message`
- **FR-7.3**: Frontend stores last 10,000 log entries in ring buffer
- **FR-7.4**: Frontend persists all logs to SQLite `logs` table
- **FR-7.5**: Per-service log level configuration:
  - API: `POST /api/settings` with `{key: "log_level_whisper", value: "DEBUG"}`
  - Services check config every 5s and adjust internal log level
- **FR-7.6**: Frontend log viewer filters: service, level, call_id, text search
- **FR-7.7**: Auto-scroll toggle, clear logs button, export logs button

### FR-8: Round-Trip TTS Validation
- **FR-8.1**: Frontend "TTS Validation" test page
- **FR-8.2**: Test setup: Line 1 (full pipeline), Line 2 (Whisper only)
- **FR-8.3**: Test workflow:
  1. Inject text prompt to LLaMA on Line 1
  2. LLaMA generates response text (log original)
  3. Kokoro synthesizes speech
  4. OAP sends to SIP Client → routes to Line 2
  5. Line 2 IAP → Whisper transcribes speech
  6. Compare original LLaMA text vs Line 2 transcription
- **FR-8.4**: Results: Original Text, TTS Transcription, Similarity %, Status
- **FR-8.5**: Phoneme-level diff highlighting for mismatches

### FR-9: Progressive Service Tests
Each test has dedicated frontend panel with:
- **Test controls**: Start, Stop, Run Single Iteration, Run 10 Iterations
- **Live metrics**: Current status, elapsed time, packet counts, error counts
- **Pass/Fail criteria**: Clearly defined success conditions
- **Historical results**: Last 10 test runs with timestamps and outcomes

**Test 1: SIP Client RTP Routing**
- FR-9.1.1: Start SIP Client only (no IAP)
- FR-9.1.2: Inject audio via Test SIP Provider
- FR-9.1.3: Verify RTP packets logged but discarded (IAP offline)
- FR-9.1.4: Start IAP → verify immediate TCP connection
- FR-9.1.5: Re-inject audio → verify packets reach IAP
- FR-9.1.6: Metrics: RTP packets sent, RTP packets received, TCP connection time

**Test 2: IAP Conversion Quality**
- FR-9.2.1: Inject 16kHz clean PCM audio
- FR-9.2.2: Capture IAP output (float32 PCM)
- FR-9.2.3: Measure SNR, THD, frequency response
- FR-9.2.4: Test audio enhancement filters (optional)
- FR-9.2.5: Metrics: Conversion latency, SNR dB, THD %

**Test 3: Whisper Accuracy + VAD**
- FR-9.3.1: Inject all 20 test samples
- FR-9.3.2: Compare transcriptions to ground truth
- FR-9.3.3: VAD validation: No sentence truncation, no word clipping
- FR-9.3.4: Adjustable VAD parameters: window size (100ms default), energy threshold
- FR-9.3.5: Metrics: Accuracy %, VAD false positives/negatives, inference latency

**Test 4: LLaMA Response Quality**
- FR-9.4.1: Send 10 test prompts (German questions)
- FR-9.4.2: Evaluate responses: conciseness, relevance, grammar
- FR-9.4.3: Test shut-up mechanism: interrupt mid-response with new input
- FR-9.4.4: Metrics: Response latency, token count, interrupt latency

**Test 5: Kokoro TTS Quality**
- FR-9.5.1: Synthesize 10 test phrases
- FR-9.5.2: Evaluate naturalness (subjective), pronunciation accuracy (via Whisper)
- FR-9.5.3: Metrics: Synthesis latency, audio chunk size, buffer underruns

**Test 6: OAP Encoding/Scheduling**
- FR-9.6.1: Receive TTS audio from Kokoro
- FR-9.6.2: Encode to G.711, packetize as RTP (20ms frames)
- FR-9.6.3: Verify precise 20ms timing (jitter analysis)
- FR-9.6.4: Metrics: Encoding latency, RTP jitter, packet loss, silence insertion

---

## Non-Functional Requirements

### NFR-1: Performance
- **NFR-1.1**: End-to-end latency (speech in → speech out): ≤2000ms target, ≤3000ms acceptable
- **NFR-1.2**: Whisper transcription: ≤1500ms for 3-second audio (large-v3-turbo)
- **NFR-1.3**: LLaMA response generation: ≤500ms for concise reply
- **NFR-1.4**: Kokoro synthesis: ≤300ms for 10-word phrase
- **NFR-1.5**: Frontend UI responsiveness: ≤100ms for user actions

### NFR-2: Reliability
- **NFR-2.1**: Service crash recovery: Auto-restart within 5s (if enabled)
- **NFR-2.2**: TCP reconnection: ≤3 retry attempts with exponential backoff
- **NFR-2.3**: Logging resilience: No service crashes due to logging failures
- **NFR-2.4**: Frontend uptime: 99.9% during testing sessions

### NFR-3: Accuracy
- **NFR-3.1**: Whisper transcription: ≥95% accuracy on clean 16kHz audio
- **NFR-3.2**: Whisper transcription: ≥90% accuracy on G.711 degraded audio
- **NFR-3.3**: Round-trip TTS validation: ≥95% similarity (text → speech → text)
- **NFR-3.4**: VAD accuracy: ≤1% sentence truncation rate

### NFR-4: Scalability
- **NFR-4.1**: Support 1-6 concurrent phone lines without performance degradation
- **NFR-4.2**: Log buffer handles 10,000 entries without memory issues
- **NFR-4.3**: Model benchmark tests up to 10 models sequentially

### NFR-5: Usability
- **NFR-5.1**: Frontend works in Chrome, Safari, Firefox (latest versions)
- **NFR-5.2**: No command-line interaction required for standard tests
- **NFR-5.3**: Test results understandable to non-technical QA engineers
- **NFR-5.4**: All features accessible via keyboard navigation

### NFR-6: Maintainability
- **NFR-6.1**: Code documentation: JSDoc/Doxygen comments on all public APIs
- **NFR-6.2**: Inline comments explaining non-obvious logic
- **NFR-6.3**: README.md in each service directory with architecture overview
- **NFR-6.4**: Database schema documented with column descriptions

---

## Technical Constraints

### TC-1: Platform
- **TC-1.1**: macOS 14+ (Apple Silicon M1/M2/M3/M4 required for CoreML)
- **TC-1.2**: Metal backend requires macOS GPU drivers
- **TC-1.3**: No Windows/Linux support required for beta testing

### TC-2: Dependencies
- **TC-2.1**: whisper.cpp v1.8.3+ (CoreML enabled)
- **TC-2.2**: llama.cpp v3800+ (Metal enabled)
- **TC-2.3**: Kokoro TTS with PyTorch MPS backend
- **TC-2.4**: Bootstrap 5.3 for frontend UI
- **TC-2.5**: SQLite 3.40+ for logging database

### TC-3: Network
- **TC-3.1**: All services run on localhost (127.0.0.1)
- **TC-3.2**: UDP ports: 9001 (IAP input), 9002 (OAP output)
- **TC-3.3**: TCP ports: 13000+ (Whisper sessions), 8083 (LLaMA), 8090+ (Kokoro/OAP)
- **TC-3.4**: HTTP: 8080 (Frontend), 22011 (Test SIP Provider)

### TC-4: Audio Formats
- **TC-4.1**: Testfiles: 8kHz or 16kHz WAV (PCM or μ-law)
- **TC-4.2**: Internal pipeline: 16kHz float32 PCM
- **TC-4.3**: SIP/RTP: 8kHz G.711 μ-law (PCMU)

---

## Out of Scope

### Explicitly NOT included in this phase:
1. **Multi-language support**: Only German language testing
2. **Cloud deployment**: Local development environment only
3. **Production SIP server integration**: Test SIP Provider only
4. **Mobile frontend**: Desktop browser only
5. **Real-time collaboration**: Single-user testing
6. **Load testing**: Max 6 concurrent lines
7. **Security hardening**: No authentication, no encryption
8. **Distributed deployment**: Single-machine setup

---

## Success Metrics

### Quantitative
1. **Test Coverage**: All 6 services have ≥3 dedicated test scenarios
2. **Bug Fix Rate**: 100% of discovered bugs fixed before next service test
3. **Whisper Accuracy**: ≥95% on clean audio, ≥90% on G.711 audio
4. **End-to-End Latency**: ≤2000ms for 80% of tests
5. **Multi-Line Stability**: 4-line call test runs for ≥10 minutes without crashes

### Qualitative
1. **Frontend Usability**: QA engineer can run full test suite without developer assistance
2. **Log Clarity**: Errors are traceable to root cause within 5 minutes
3. **Documentation Completeness**: New developer can understand architecture in ≤1 hour
4. **Code Cleanliness**: No commented-out code, no TODO markers, no debug prints

---

## Assumptions & Dependencies

### Assumptions
1. Testfiles/*.wav and *.txt are manually curated and correct
2. Test SIP Provider runs on same machine as pipeline services
3. User has basic understanding of SIP/RTP concepts
4. CoreML models are pre-converted and available in whisper-cpp/models/

### Dependencies
1. **Upstream**: whisper.cpp and llama.cpp repositories (for model updates)
2. **Frontend**: Bootstrap CDN availability (for UI components)
3. **Hardware**: Apple Silicon Mac with ≥16GB RAM
4. **Disk**: ≥10GB free space for models and logs

---

## Open Questions

### Resolved Assumptions (no user input needed)
These decisions are made based on codebase analysis and will proceed without user confirmation:

1. **Q: Log storage duration?**  
   **A**: SQLite logs persist indefinitely, 10k ring buffer in memory. User can manually clear via frontend.

2. **Q: Model download automation?**  
   **A**: Manual download initially. Hugging Face integration is optional enhancement (FR-6.7).

3. **Q: Audio enhancement filters in IAP?**  
   **A**: Test if latency impact is ≤10ms. Implement only if measurable quality gain.

4. **Q: Multi-line test preset configurations?**  
   **A**: Provide 1-line, 2-line, 4-line presets. 6-line requires manual setup.

5. **Q: Model benchmark iteration count?**  
   **A**: Default 3 iterations, user-configurable 1-10 via frontend.

6. **Q: Chart library for visualization?**  
   **A**: Chart.js (lightweight, CDN-hosted, no build step).

---

## Glossary

- **B2BUA**: Back-to-Back User Agent (SIP proxy that bridges two call legs)
- **CoreML**: Apple's machine learning framework (hardware-accelerated on ANE)
- **G.711**: ITU-T standard for 8kHz audio codec (μ-law variant for North America/Japan)
- **IAP**: Inbound Audio Processor (decodes G.711 → 16kHz float32 PCM)
- **Metal**: Apple's GPU compute framework
- **OAP**: Outbound Audio Processor (encodes float32 PCM → G.711)
- **RTP**: Real-Time Transport Protocol (for audio packet transmission)
- **SIP**: Session Initiation Protocol (for call setup/teardown)
- **SSE**: Server-Sent Events (for log streaming to frontend)
- **VAD**: Voice Activity Detection (silence suppression)

---

## Appendix A: Test File Specification

### Format
- **Naming**: `sample_NN.wav` and `sample_NN.txt` (NN = 01 to 20)
- **Audio**: WAV container, PCM 16-bit or μ-law 8-bit, mono, 8kHz or 16kHz
- **Text**: UTF-8, single line or paragraph, German language

### Example
```
Testfiles/
├── sample_01.wav  (373 KB, ~2.3s, "bei fettiger haut sind diese...")
├── sample_01.txt  ("bei fettiger haut sind diese verstopft, abfallstoffe bleiben stecken.")
├── sample_02.wav
├── sample_02.txt
...
└── sample_20.txt
```

### Ground Truth Quality
- Text is manually transcribed or verified
- Capitalization, punctuation consistent with spoken audio
- No speaker labels or timestamps (continuous speech)

---

## Appendix B: API Endpoints Reference

### Frontend HTTP API
```
GET  /api/status              - System status (services online, tests running)
GET  /api/tests               - List all test binaries
POST /api/tests/start         - Start a test {name, args}
POST /api/tests/stop          - Stop a running test {name}
GET  /api/services            - List all pipeline services
POST /api/services/start      - Start a service {name, args}
POST /api/services/stop       - Stop a service {name}
GET  /api/logs                - Query log database {service, level, limit}
GET  /api/logs/stream         - SSE endpoint for live logs
POST /api/settings            - Update setting {key, value}
GET  /api/whisper/models      - List available Whisper models
POST /api/sip/add-line        - Add phone line {line_id, username, password}
POST /api/sip/remove-line     - Remove phone line {line_id}
GET  /api/sip/lines           - List current lines with status
GET  /api/testfiles           - List Testfiles/*.wav with metadata [NEW]
POST /api/whisper/benchmark   - Run Whisper accuracy test {model, files} [NEW]
POST /api/llama/benchmark     - Run LLaMA benchmark {model, prompts} [NEW]
GET  /api/models              - List all registered models (Whisper, LLaMA) [NEW]
POST /api/models/add          - Register new model {type, name, path, config} [NEW]
```

### Test SIP Provider HTTP API
```
GET  /api/status              - Provider status (registered users, active calls)
GET  /api/files               - List available test audio files
POST /api/inject              - Inject audio {call_id, file, target_leg} [ENHANCED]
POST /api/call/hangup         - Terminate call {call_id}
```

---

## Appendix C: Service Testing Checklist

### Per-Service Validation
Each service must pass these checks before integration:

**SIP Client**
- [ ] Registers with Test SIP Provider
- [ ] Receives INVITE and sends 200 OK
- [ ] Routes inbound RTP to IAP (UDP 9001)
- [ ] Routes outbound RTP from OAP (UDP 9002)
- [ ] Supports 1-6 concurrent lines
- [ ] Logs all SIP messages to frontend

**IAP**
- [ ] Decodes G.711 μ-law to PCM int16
- [ ] Resamples 8kHz → 16kHz cleanly
- [ ] Converts int16 → float32 normalized [-1, 1]
- [ ] Streams to Whisper via TCP (port 13000+)
- [ ] Reconnects if Whisper restarts
- [ ] Logs packet counts, TCP state

**Whisper Service**
- [ ] Loads model (large-v3, large-v3-turbo, etc.)
- [ ] Applies CoreML or Metal backend
- [ ] VAD detects speech/silence (100ms windows)
- [ ] Transcribes German with ≥95% accuracy
- [ ] Streams text to LLaMA (TCP 8083)
- [ ] Logs inference time, VAD events

**LLaMA Service**
- [ ] Loads model (Llama-3.2-1B-Instruct Q8_0)
- [ ] German system prompt active
- [ ] Generates concise responses (≤30 words)
- [ ] Shut-up mechanism interrupts generation
- [ ] Streams response to Kokoro (TCP 8090)
- [ ] Logs token counts, generation time

**Kokoro TTS**
- [ ] Synthesizes German speech
- [ ] 24kHz float32 PCM output
- [ ] Streams to OAP via TCP
- [ ] Pronunciation accuracy ≥95% (via round-trip)
- [ ] Logs synthesis latency

**OAP**
- [ ] Receives 24kHz float32 from Kokoro
- [ ] Resamples 24kHz → 8kHz
- [ ] Encodes to G.711 μ-law
- [ ] Packetizes as RTP (20ms frames, 160 bytes)
- [ ] Sends to SIP Client (UDP 9002)
- [ ] Precise 20ms timing (jitter ≤2ms)
- [ ] Logs buffer levels, jitter stats

---

## Document Control

**Version**: 1.0  
**Last Updated**: 2026-02-22  
**Author**: AI Requirements Analyst  
**Approvers**: [Pending]  
**Next Review**: After Technical Specification phase
