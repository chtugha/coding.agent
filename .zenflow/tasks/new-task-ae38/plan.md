# Beta-Testing Infrastructure Implementation Plan

## Configuration
- **Artifacts Path**: `.zenflow/tasks/new-task-ae38`
- **Project**: WhisperTalk
- **Task**: Beta-Testing Infrastructure

---

## Agent Instructions

If you are blocked and need user clarification, mark the current step with `[!]` in plan.md before stopping.

---

## Workflow Steps

### [x] Step: Requirements

Create a Product Requirements Document (PRD) based on the feature description.

### [ ] Step: Technical Specification

Create a technical specification based on the PRD.

### [x] Step: Planning

Create a detailed implementation plan.

---

## Implementation Steps

### [ ] Phase 1: Core Testing Infrastructure

Build foundational testing capabilities that all subsequent tests will depend on.

**Deliverables:**
- Testfiles API endpoint and frontend UI
- Enhanced logging configuration per service
- Frontend test results storage and display
- Basic test orchestration framework

**Tasks:**
- [ ] Implement `/api/testfiles` endpoint in frontend.cpp to discover and list Testfiles/*.wav and *.txt files with metadata (duration, sample rate, file size)
- [ ] Add frontend UI "Test Files" panel displaying available test files in a selectable list with file metadata
- [ ] Implement per-service log level configuration: API endpoint `/api/settings/log_level` accepting service name and level (ERROR/WARN/INFO/DEBUG/TRACE)
- [ ] Add frontend UI "Settings" page with dropdown selectors for each service log level, persisting to database
- [ ] Create database schema for test results: tables for `whisper_tests`, `llama_tests`, `kokoro_tests`, `model_benchmarks` with timestamps, metrics, and pass/fail status
- [ ] Implement frontend test results viewer with filtering, sorting, and export to JSON functionality
- [ ] Add Chart.js integration to frontend for visualizing test metrics (latency, accuracy, comparison charts)

**Verification:**
- Frontend displays all 20 test files from Testfiles/ directory
- Log level changes take effect without service restart
- Test results persist to database and display in UI
- Charts render correctly with sample data

---

### [ ] Phase 2: Audio Injection & SIP Multi-Line Support

Enable audio injection into active calls and dynamic line management.

**Deliverables:**
- Test SIP Provider audio injection HTTP API
- Frontend UI for triggering audio injection
- SIP Client multi-line add/remove functionality
- Line management UI in frontend

**Tasks:**
- [ ] Enhance test_sip_provider.cpp: add `/api/inject` POST endpoint accepting `{call_id, file_path, target_leg}`, load WAV file, convert to G.711 μ-law RTP, inject at 20ms intervals
- [ ] Add frontend "Audio Injection" panel with dropdown for selecting test file, dropdown for selecting active line/call, "Inject to Leg A/B" buttons, and injection status display
- [ ] Implement `/api/sip/add-line` in sip-client-main.cpp: accept `{line_id, username, password, server}`, register new SIP line dynamically, send `ACTIVATE:call_id` to processors
- [ ] Implement `/api/sip/remove-line` in sip-client-main.cpp: gracefully terminate active calls, unregister line, send `DEACTIVATE:call_id` to processors
- [ ] Implement `/api/sip/lines` GET endpoint returning array of lines with status (Idle/Registering/Ringing/Active/Terminated), Call-ID, RTP ports
- [ ] Add frontend "SIP Lines" panel with table showing current lines and status, "Add Line" form, "Remove Line" button per line, preset buttons for 1/2/4-line configurations
- [ ] Update sip-client-main.cpp to maintain up to 6 concurrent call sessions with independent state per line
- [ ] Add comprehensive logging to SIP client for line add/remove events, registration status, call state transitions

**Verification:**
- Inject sample_01.wav into active call via frontend button → verify audio received on remote end
- Add line via frontend → verify SIP registration successful → verify line appears in status table
- Remove active line → verify call terminated gracefully → verify processors notified
- Test 2-line and 4-line preset configurations work correctly

---

### [ ] Phase 3: Progressive Service Testing - SIP Client & IAP

Implement isolated tests for SIP Client RTP routing and IAP conversion quality.

**Deliverables:**
- SIP Client RTP routing test
- IAP conversion quality test
- Frontend test panels for both tests
- Test metrics collection and display

**Tasks:**
- [ ] Create "Test 1: SIP Client RTP Routing" frontend panel with controls: Start Test, Stop Test, Inject Audio button, metrics display (RTP packets sent/received, TCP connection status)
- [ ] Implement test logic: start SIP client only (no IAP) → inject audio → verify RTP logged but discarded → start IAP → verify TCP connection → re-inject → verify packets reach IAP
- [ ] Add RTP packet counting and logging to sip-client-main.cpp for test verification
- [ ] Create "Test 2: IAP Conversion Quality" frontend panel with controls: Start Test, Select Test File, Run Test, metrics display (conversion latency, SNR, THD)
- [ ] Implement IAP audio quality measurement: inject 16kHz clean PCM → capture IAP float32 output → calculate SNR (Signal-to-Noise Ratio) and THD (Total Harmonic Distortion)
- [ ] Research and optionally implement audio enhancement filters in inbound-audio-processor.cpp (equalizer, noise reduction) only if latency impact ≤10ms
- [ ] Add test result logging to database with timestamp, file tested, metrics, pass/fail criteria (SNR ≥40dB, THD ≤1%, latency ≤50ms)
- [ ] Implement frontend display of IAP test results with historical comparison chart

**Verification:**
- Run SIP RTP test → verify RTP packets counted correctly → verify IAP connects and receives audio
- Run IAP conversion test on all 20 samples → verify SNR/THD metrics calculated → verify results stored in database
- If audio enhancement implemented, verify latency impact is ≤10ms

---

### [ ] Phase 4: Whisper Accuracy Testing & VAD Optimization

Implement transcription accuracy benchmarking against ground truth and VAD tuning.

**Deliverables:**
- Whisper accuracy test framework
- Ground truth comparison with Levenshtein similarity
- VAD parameter tuning UI
- Accuracy test results with detailed metrics

**Tasks:**
- [ ] Create "Test 3: Whisper Accuracy" frontend panel with multi-select file picker, VAD parameter sliders (window size, energy threshold), Start Test button, results table
- [ ] Implement test workflow in frontend: inject selected audio files → capture Whisper transcription output via log stream → load ground truth from Testfiles/*.txt → calculate Levenshtein distance/similarity (0-100%)
- [ ] Add Levenshtein distance calculation utility function to frontend.cpp for string comparison
- [ ] Implement per-file result display: File name, Ground Truth text, Whisper Output text, Similarity %, Latency ms, Status (PASS ≥99.5%, WARN ≥90%, FAIL <90%)
- [ ] Add aggregate statistics display: Total files tested, PASS/WARN/FAIL counts, Average accuracy %, Average latency ms
- [ ] Store test results in `whisper_tests` database table with foreign key to test file, model used, VAD parameters, timestamp
- [ ] Implement VAD parameter adjustment in whisper-service.cpp: expose configuration for window size (default 100ms) and energy threshold, reload on settings change
- [ ] Add VAD event logging to whisper-service.cpp: log speech start/end detection, window energy levels for debugging
- [ ] Create accuracy trend chart in frontend showing accuracy % over time for historical test runs

**Verification:**
- Run accuracy test on all 20 samples with default VAD → verify similarity scores calculated → verify results stored
- Adjust VAD window size to 200ms → re-run test → verify different results → verify no sentence truncation
- Export test results as JSON → verify format and completeness

---

### [ ] Phase 5: Whisper Model Benchmarking

Build framework for comparing multiple Whisper models side-by-side.

**Deliverables:**
- Model registry system
- Model benchmark test runner
- Comparison charts and tables
- Model performance database

**Tasks:**
- [ ] Create database schema for model registry: `models` table with columns (id, type [whisper/llama], name, path, backend, config_json, added_date, status)
- [ ] Implement `/api/models` GET endpoint returning all registered models grouped by type
- [ ] Implement `/api/models/add` POST endpoint accepting `{type, name, path, backend, config}` and inserting into models table
- [ ] Create frontend "Models" page with tabs for Whisper and LLaMA, table showing registered models (Name, Path, Backend, Status), Add Model form
- [ ] Implement `/api/whisper/benchmark` POST endpoint accepting `{model_id, test_files[], iterations}`, restart Whisper service with selected model, run accuracy test, return aggregated metrics
- [ ] Add model benchmark results table: `model_benchmarks` with columns (model_id, test_date, accuracy_avg, latency_p50, latency_p95, latency_p99, memory_mb, pass_count, fail_count)
- [ ] Create frontend "Model Comparison" view with side-by-side table (columns: Model, Backend, Accuracy %, Avg Latency, P95 Latency, Memory MB) and interactive bar chart for latency comparison
- [ ] Add search and download feature (optional): frontend form to search Hugging Face by keyword, display results, download model to local path
- [ ] Document recommended Whisper models for German (e.g., large-v3-turbo, German-finetuned variants) in frontend help text

**Verification:**
- Register 2 Whisper models (e.g., large-v3 and large-v3-turbo) via frontend
- Run benchmark on both models with same 10 test files → verify results differ → verify stored in database
- View model comparison chart → verify visual differences clear → verify sorting by metrics works

---

### [ ] Phase 6: LLaMA Response Quality & Model Benchmarking

Test LLaMA response quality and implement model comparison framework.

**Deliverables:**
- LLaMA response quality test
- Shut-up mechanism test
- LLaMA model benchmarking
- German prompt test suite

**Tasks:**
- [ ] Create test prompt dataset: 10 German questions in Testfiles/llama_prompts.json with expected response characteristics (concise, relevant, polite)
- [ ] Create "Test 4: LLaMA Response Quality" frontend panel with prompt selector, Send Prompt button, response display, metrics (latency, token count, relevance score)
- [ ] Implement test workflow: send test prompts to LLaMA service via TCP → capture response text → measure latency and token count → display in frontend
- [ ] Add "Shut-up Test" button in LLaMA test panel: send initial prompt → mid-response send interruption prompt → verify LLaMA stops generating and switches context
- [ ] Implement shut-up mechanism verification: log timestamps of interruption signal and actual stop time → calculate interrupt latency → display in frontend
- [ ] Create `/api/llama/benchmark` POST endpoint accepting `{model_id, test_prompts[], iterations}`, restart LLaMA service with selected model, run prompts, collect metrics
- [ ] Add LLaMA benchmark results to `model_benchmarks` table with additional columns: avg_tokens, interrupt_latency_ms
- [ ] Add LLaMA model registration form in frontend Models page with fields specific to LLaMA (context size, n_gpu_layers, quantization level)
- [ ] Create comparison chart for LLaMA models showing response latency and token efficiency
- [ ] Document recommended LLaMA models for German conversation in frontend help text

**Verification:**
- Send 10 test prompts → verify responses concise and relevant → verify latency logged
- Trigger shut-up mechanism mid-response → verify LLaMA stops within 500ms → verify new response starts
- Benchmark 2 LLaMA models (e.g., Llama-3.2-1B vs 3B) → verify comparison data accurate

---

### [ ] Phase 7: Kokoro TTS Quality Testing

Test Kokoro synthesis quality and optimization.

**Deliverables:**
- Kokoro synthesis quality test
- TTS latency measurement
- Pronunciation accuracy testing

**Tasks:**
- [ ] Create test phrase dataset: 10 German phrases in Testfiles/kokoro_phrases.json with varying phonetic complexity
- [ ] Create "Test 5: Kokoro TTS Quality" frontend panel with phrase selector, Synthesize button, audio playback, metrics display (synthesis latency, audio chunk size, buffer underruns)
- [ ] Implement test workflow: send test phrases to Kokoro service → capture synthesized audio → measure synthesis latency → stream to frontend for playback
- [ ] Add audio quality metrics: measure chunk delivery timing, detect buffer underruns (gaps in audio stream), log to database
- [ ] Implement pronunciation accuracy test (preliminary for Phase 8): synthesize phrase → send to Line 2 Whisper → compare input text to transcription → calculate similarity
- [ ] Optimize kokoro-service.cpp for performance: profile synthesis pipeline, reduce unnecessary copying, optimize MPS backend usage if applicable
- [ ] Add comprehensive logging to Kokoro service: synthesis start/end, chunk sizes, MPS allocation, errors
- [ ] Store Kokoro test results in database: phrase tested, synthesis_latency_ms, audio_duration_ms, buffer_underruns, timestamp

**Verification:**
- Synthesize all 10 test phrases → verify audio plays correctly → verify latency ≤300ms per phrase
- Monitor buffer underruns → verify zero underruns during normal operation
- Run preliminary pronunciation test → verify transcription similarity ≥90%

---

### [ ] Phase 8: OAP & End-to-End TTS Validation

Test OAP encoding/scheduling and implement round-trip TTS validation using two phone lines.

**Deliverables:**
- OAP encoding quality test
- RTP scheduling precision test
- Round-trip TTS validation (Kokoro → Whisper)
- End-to-end speech quality metrics

**Tasks:**
- [ ] Create "Test 6: OAP Encoding/Scheduling" frontend panel with Start Test button, metrics display (encoding latency, RTP jitter, packet loss, silence insertion count)
- [ ] Implement test workflow: trigger TTS synthesis → OAP receives 24kHz float32 → measure encoding latency → verify 20ms RTP packet timing → measure jitter
- [ ] Add RTP timing analysis to outbound-audio-processor.cpp: log scheduled send time vs actual send time, calculate jitter statistics (mean, max, std dev)
- [ ] Implement packet loss detection: sequence number validation, gaps detection, logging
- [ ] Add silence insertion tracking: log when silence frames inserted to maintain timing, count insertions
- [ ] Create "TTS Round-Trip Validation" frontend panel with 2-line setup display, Test Phrase input, Run Test button, results comparison (Original Text vs Transcription, Similarity %, Phoneme diff)
- [ ] Implement round-trip test workflow: Line 1 receives text prompt → LLaMA generates response (log original) → Kokoro synthesizes → OAP encodes → SIP Client routes to Line 2 → Line 2 IAP decodes → Whisper transcribes (log output) → compare original vs transcription
- [ ] Add phoneme-level diff highlighting in frontend: identify mismatched characters/phonemes between original and transcription, highlight in red
- [ ] Store round-trip test results in database: original_text, transcribed_text, similarity_%, problematic_phonemes, timestamp
- [ ] Generate TTS quality report: aggregate problematic phonemes across tests, identify patterns for Kokoro optimization
- [ ] Document end-to-end latency breakdown: measure time at each stage (Whisper → LLaMA → Kokoro → OAP → Whisper2), display waterfall chart in frontend

**Verification:**
- Run OAP encoding test → verify RTP jitter ≤2ms → verify no packet loss in ideal conditions
- Configure 2 lines → run round-trip test with 10 phrases → verify similarity ≥95% → identify any problematic phonemes
- View end-to-end latency waterfall chart → verify total latency ≤3000ms

---

### [ ] Phase 9: Service Interconnection Resilience Testing

Test TCP reconnection, buffering, and error handling across service boundaries.

**Deliverables:**
- Service start/stop orchestration UI
- TCP connection status monitoring
- Reconnection resilience tests
- Service dependency visualization

**Tasks:**
- [ ] Implement service dependency graph visualization in frontend: display SIP → IAP → Whisper → LLaMA → Kokoro → OAP as connected nodes with status indicators (green=connected, red=disconnected, yellow=connecting)
- [ ] Add "Start All Services" button in frontend: launch services in dependency order with 1-second delays, update graph status in real-time
- [ ] Add "Stop All Services" button: shut down services in reverse dependency order, verify graceful shutdown
- [ ] Implement TCP connection status monitoring: each service reports connection state to frontend via logging or status API, frontend displays per-connection status
- [ ] Create "Interconnection Test" frontend panel with controls: Select Service to Restart, Restart Service button, monitor logs for reconnection attempts
- [ ] Implement reconnection resilience test workflow: Start full pipeline → inject audio → verify end-to-end → stop Whisper service → verify IAP logs "Whisper disconnected" → restart Whisper → verify IAP logs "Whisper reconnected" → re-inject audio → verify end-to-end works
- [ ] Test buffering behavior: verify IAP continues processing RTP even when Whisper offline (dumps audio or buffers temporarily)
- [ ] Test all service pair reconnections: IAP↔Whisper, Whisper↔LLaMA, LLaMA↔Kokoro, Kokoro↔OAP
- [ ] Add exponential backoff logging for TCP reconnection attempts (attempt 1, 2, 3 with delays), verify max 3 attempts before giving up
- [ ] Document interconnection resilience requirements in service README files

**Verification:**
- Start all services via frontend button → verify services start in correct order → verify graph shows all green
- Stop Whisper while pipeline running → verify IAP handles gracefully → restart Whisper → verify reconnection automatic
- Repeat for all service pairs → verify no crashes or deadlocks

---

### [ ] Phase 10: Multi-Line Stress Testing & Final Optimization

Test system with 1, 2, 4, and 6 concurrent phone lines to verify scalability.

**Deliverables:**
- Multi-line test configurations (1/2/4/6 lines)
- Concurrent call handling verification
- Performance degradation analysis
- Final optimization recommendations

**Tasks:**
- [ ] Create "Multi-Line Stress Test" frontend panel with preset configurations (1-line, 2-line, 4-line, 6-line), Start Test button, per-line metrics table
- [ ] Implement 1-line baseline test: single call, inject audio, run full pipeline, measure latency and accuracy, store baseline metrics
- [ ] Implement 2-line test: two concurrent calls, inject different audio files simultaneously, verify both transcriptions correct, measure latency per line
- [ ] Implement 4-line test: four concurrent calls, inject audio with 500ms stagger, verify no cross-talk between lines, measure resource usage (CPU, memory)
- [ ] Implement 6-line test: six concurrent calls (maximum), inject audio, verify system stability, measure performance degradation vs baseline
- [ ] Add per-line metrics collection: latency (Whisper, LLaMA, Kokoro), accuracy, packet loss, jitter, log to database with line_id
- [ ] Create performance comparison table in frontend: compare 1-line vs 2-line vs 4-line vs 6-line metrics side-by-side
- [ ] Identify performance bottlenecks: profile CPU usage per service, identify serialization points, document in test results
- [ ] Implement final optimizations based on profiling: reduce locking contention, optimize audio buffer sizes, tune TCP parameters
- [ ] Run final regression test suite: all accuracy tests, all model benchmarks, all interconnection tests with 2-line configuration
- [ ] Generate final test report: summary of all test results, pass/fail counts, performance metrics, known issues, optimization recommendations

**Verification:**
- Run 1-line test → establish baseline latency ≤2000ms, accuracy ≥95%
- Run 2-line test → verify latency ≤2500ms, accuracy ≥95% on both lines
- Run 4-line test → verify latency ≤3000ms, accuracy ≥90% on all lines, no crashes
- Run 6-line test → verify system stability, document performance limits

---

### [ ] Phase 11: Documentation & Code Cleanup

Comprehensive documentation and code cleanup per development rules.

**Deliverables:**
- Updated service summary documents
- Inline code documentation
- Service README files
- Frontend user guide
- Test suite documentation

**Tasks:**
- [ ] Update service summary documents in .zencoder/rules/ for all modified services (sip-client.md, inbound-audio-processor.md, whisper-service.md, llama-service.md, kokoro-service.md, outbound-audio-processor.md)
- [ ] Add JSDoc/Doxygen comments to all new public APIs in frontend.cpp (testfiles, models, benchmarks, sip endpoints)
- [ ] Add inline comments to complex logic in all services: VAD algorithm, TCP reconnection, RTP scheduling, model loading
- [ ] Create README.md in tests/ directory documenting test suite architecture, how to run tests, how to add new tests
- [ ] Create README.md in Testfiles/ directory explaining test file format, ground truth requirements, how to add new test files
- [ ] Add frontend user guide accessible via /help route: explain all test panels, metrics definitions, how to interpret results
- [ ] Document database schema in frontend.cpp comments: all tables, columns, foreign keys, indexes
- [ ] Clean up dead code: remove old stubs, commented-out sections, unused functions across all services
- [ ] Strip unnecessary logging (debug statements) from production code paths, keep only INFO/WARN/ERROR in critical paths
- [ ] Run code formatter on all modified C++ files for consistency
- [ ] Verify all developer rules compliance: no stubs, all bugs fixed, comprehensive logging, frontend-driven testing

**Verification:**
- All service summaries accurately reflect current implementation
- All new functions have documentation comments
- Test suite README clear and complete
- Frontend user guide accessible and helpful
- No dead code or stubs remain in codebase

---

## Verification Strategy

### Per-Phase Verification
Each phase must pass its verification criteria before proceeding to the next phase.

### Integration Testing
After Phase 10, run full integration test suite:
1. All 20 audio samples through full pipeline (1-line)
2. Model benchmarks for Whisper and LLaMA (at least 2 models each)
3. Round-trip TTS validation (10 phrases)
4. Multi-line stress test (2-line and 4-line)
5. Service interconnection resilience (all service pairs)

### Acceptance Criteria
- **Accuracy**: Whisper transcription ≥95% on clean audio, ≥90% on G.711
- **Latency**: End-to-end ≤3000ms for full pipeline
- **Stability**: Zero crashes during 1-hour 2-line stress test
- **Usability**: All tests runnable via frontend UI with clear results
- **Documentation**: Complete service summaries, API docs, user guide

---

## Notes

### Development Rules Compliance
- **NO STUBS**: Every feature fully functional before marking step complete
- **FIX ALL BUGS**: Address issues immediately, no deferring
- **FRONTEND-FIRST**: All testing via browser UI, not CLI
- **PERFORMANCE**: Optimize for near-real-time speed throughout
- **LOGGING**: Crash-proof logging with configurable depth
- **DOCUMENTATION**: Thorough docs for future maintainers

### Risk Mitigation
- **Model Availability**: If recommended models not available, document alternatives
- **Performance Targets**: If targets not achievable, document actual performance and bottlenecks
- **Audio Enhancement**: Only implement if ≤10ms latency impact, otherwise skip
- **6-Line Testing**: If hardware limits prevent 6-line test, document max tested configuration

### Future Enhancements (Out of Scope)
- Automated test scheduling (cron jobs)
- Email/Slack notifications for test failures
- Hugging Face auto-download integration
- Advanced audio quality metrics (PESQ, POLQA)
- Visual waveform comparison
- A/B testing framework for model selection
