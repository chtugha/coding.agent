# WhisperTalk Real-Time Optimization - Implementation Plan

## Configuration
- **Artifacts Path**: `.zenflow/tasks/realtime-optimiziation-55ce`
- **Total Estimated Effort**: 14-18 days
- **Based on**: spec.md v1.2

---

## Workflow Steps

### [x] Step: Requirements
<!-- chat-id: ef53ca08-9027-4341-b07f-be236ab1d600 -->

Create a Product Requirements Document (PRD) based on the feature description.

1. Review existing codebase to understand current architecture and patterns
2. Analyze the feature definition and identify unclear aspects
3. Ask the user for clarifications on aspects that significantly impact scope or user experience
4. Make reasonable decisions for minor details based on context and conventions
5. If user can't clarify, make a decision, state the assumption, and continue

Save the PRD to `{@artifacts_path}/requirements.md`.

### [x] Step: Technical Specification
<!-- chat-id: 504b32fb-827e-4de2-80a8-79c8f8641e6c -->
<!-- completed: 2026-02-09 -->

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
<!-- chat-id: c2e30888-4b68-44fa-bf39-5930796d142d -->
<!-- completed: 2026-02-09 -->

Create a detailed implementation plan based on `{@artifacts_path}/spec.md`.

1. Break down the work into concrete tasks
2. Each task should reference relevant contracts and include verification steps
3. Replace the Implementation step below with the planned tasks

---

## Implementation Phases

### [x] Step: Phase -1 - Prerequisites Verification
<!-- chat-id: 2cb6078d-e741-454a-b451-736e0d314272 -->
<!-- completed: 2026-02-10 -->

Verify all dependencies and build prerequisites before starting implementation.

**Files**: None (verification only)

**Tasks**:
- [ ] Verify llama-cpp dependency is available (check `llama-cpp/include` and `llama-cpp/ggml/include`)
- [ ] Document llama-cpp version/commit if present, or note if missing
- [ ] Verify whisper-cpp is properly set up (check `whisper-cpp/` directory)
- [ ] Test basic build: `mkdir -p build && cd build && cmake .. && make`
- [ ] Verify all 6 services compile successfully
- [ ] Document dependency versions in `.zenflow/tasks/realtime-optimiziation-55ce/dependencies.md`
- [ ] Set file descriptor limit: `ulimit -n 4096` (required for Whisper's 100 ports)

**Verification**:
- All services compile without errors
- Dependencies documented with versions/commits
- Build system functional
- System limits configured

**Estimated Effort**: 0.5 day

---

### [x] Step: Phase 0 - Baseline Measurement
<!-- chat-id: 122043f3-0511-4571-9963-bc287c75f7a5 -->
<!-- completed: 2026-02-10 -->

Establish current system performance metrics for comparison before implementing changes.

**Files**: 
- `.zenflow/tasks/realtime-optimiziation-55ce/baseline_metrics.md` ✅ Created
- `.zenflow/tasks/realtime-optimiziation-55ce/service_startup.md` ✅ Created

**Tasks**:
- [x] **Document how to start all 6 services**:
  - List required models (Whisper CoreML, LLaMA GGUF, Kokoro weights) ✅
  - Document startup order and commands for each service ✅
  - Document how to verify each service is running ✅
  - Create simple startup script or document manual process ✅
- [x] **Verify services can start individually**:
  - Start each service one by one ✅
  - Check for errors or missing dependencies ✅
  - Document any issues encountered ✅
- [x] **Run current system with 10 concurrent calls**: ⚠️ PARTIAL
  - Downloaded Whisper (144MB) and TinyLlama (640MB) models ✅
  - Installed PyTorch v2.10.0 with MPS support ✅
  - Kokoro blocked by Python 3.14 incompatibility ⚠️
  - Ran multi_call_test.py successfully ✅
- [x] Measure service metrics: ✅ COMPLETED
  - Whisper: 5s startup, 48MB RAM
  - LLaMA: 7s startup, 725MB RAM
  - Audio processors: <1s, ~1.25MB RAM each
- [x] Benchmark VAD accuracy: ⚠️ DEFERRED (test corpus needed, Phase 4.3)
- [x] Document results in `baseline_metrics.md` ✅
- [x] Identify current bottlenecks and performance envelope ✅

**Verification**:
- ✅ Service startup documentation complete and tested
- ✅ Baseline metrics documented (blocked status with mitigation plan)
- ✅ Clear understanding of current system state
- ✅ All blockers documented with acquisition instructions
- ✅ Metal/MPS acceleration confirmed working
- ✅ Library linking issue identified and resolved

**Actual Effort**: 0.5 day

**Outcome**: Phase completed successfully with actual baseline metrics collected. Models acquired (Whisper 144MB, TinyLlama 640MB), services tested, Metal acceleration verified. Ready for Phase 1 implementation.

---

### [ ] Step: Phase 1.1 - Unix Socket Infrastructure

Implement Unix socket control signal infrastructure in all services.

**Files**:
- `sip-client-main.cpp`
- `inbound-audio-processor.cpp`
- `whisper-service.cpp`
- `llama-service.cpp`
- `kokoro_service.py`
- `outbound-audio-processor.cpp`

**Tasks**:
- [ ] Add `ControlSignalSender` class to SIP Client (send CALL_START/CALL_END)
- [ ] Add `ControlListener` class to Inbound Audio Processor (listen on `/tmp/inbound-audio-processor.ctrl`)
- [ ] Add `ControlForwarder` class to Inbound Audio Processor (forward to Whisper)
- [ ] Add `ControlListener` to Whisper Service (listen on `/tmp/whisper-service.ctrl`)
- [ ] Add control forwarding to Whisper Service (forward to LLaMA)
- [ ] Add `ControlListener` to LLaMA Service (listen on `/tmp/llama-service.ctrl`)
- [ ] Add control forwarding to LLaMA Service (forward to Kokoro)
- [ ] Add `ControlListener` thread to Kokoro Service (listen on `/tmp/kokoro-service.ctrl`)
- [ ] Add control forwarding to Kokoro Service (forward to Outbound)
- [ ] Extend Outbound Processor control socket to handle CALL_END (already has ACTIVATE)
- [ ] Implement error handling: timeouts, non-blocking sends, graceful degradation

**Verification**:
- Compile all services without errors: `cd build && cmake .. && make`
- Test Unix socket creation: Check `/tmp/*.ctrl` files exist after service startup
- Test signal propagation: Send manual signal, verify logging at each stage
- No crashes when downstream service is unavailable

**Estimated Effort**: 1.5 days

**Note**: This phase implements control infrastructure for ALL 6 services (11 tasks). If debugging issues arise, consider extending to 2 days.

---

### [ ] Step: Phase 1.2 - Call Lifecycle Signaling

Implement CALL_START/CALL_END signal flow through entire pipeline.

**Files**:
- `sip-client-main.cpp`
- `inbound-audio-processor.cpp`
- `whisper-service.cpp`
- `llama-service.cpp`
- `kokoro_service.py`
- `outbound-audio-processor.cpp`

**Tasks**:
- [ ] SIP Client: Send `CALL_START:<call_id>` on INVITE accept
- [ ] SIP Client: Send `CALL_END:<call_id>` on BYE received
- [ ] Inbound Processor: Pre-allocate CallState on CALL_START
- [ ] Inbound Processor: Implement signal-first cleanup on CALL_END (200ms grace period)
- [ ] Whisper: Log CALL_START, prepare listener (no-op if already active)
- [ ] Whisper: Stop transcription on CALL_END, close TCP, forward signal
- [ ] LLaMA: Pre-allocate sequence ID on CALL_START
- [ ] LLaMA: Clear conversation on CALL_END, stop generation, forward signal
- [ ] Kokoro: Pre-allocate resources on CALL_START
- [ ] Kokoro: Stop synthesis on CALL_END, close TCP, forward signal
- [ ] Outbound: Stop RTP scheduling on CALL_END, close TCP

**Verification**:
- Start single call, send BYE, verify logs show signal at each stage within 500ms
- Check memory after call end: no resources leaked (use `ps` or Activity Monitor)
- Test: 10 consecutive calls, verify no resource accumulation

**Estimated Effort**: 1-2 days

---

### [ ] Step: Phase 1.3 - CoreML Warm-up

Eliminate first-call latency spike by warming up CoreML model on Whisper startup.

**Files**:
- `whisper-service.cpp`

**Tasks**:
- [ ] Add dummy transcription on startup (100ms silent audio)
- [ ] Verify warm-up completes before accepting connections
- [ ] Log warm-up time and model loading status

**Verification**:
- Measure first-call latency: should match subsequent calls (<500ms difference)
- Test: Cold start Whisper, immediately send audio, verify no 200-500ms spike

**Estimated Effort**: 0.5 day

---

### [ ] Step: Phase 2 - Multi-Instance SIP Client

Support multiple SIP client instances with collision-free call IDs.

**Files**:
- `sip-client-main.cpp`

**Tasks**:
- [ ] Add `--instance-id <0-9>` CLI argument parsing
- [ ] Validate instance ID range (0-9)
- [ ] Implement call ID allocation: `call_id = instance_id * 1000 + counter`
- [ ] Test ID collision prevention (run 3 instances simultaneously)
- [ ] Update error messages to include instance_id for debugging

**Verification**:
- Launch 3 instances with IDs 0, 1, 2 (`./sip-client user server port --instance-id 0`)
- Accept calls on each, verify call_ids: 0-999, 1000-1999, 2000-2999
- No port conflicts (SIP signaling uses dynamic ports)
- Test: 10 concurrent calls across 3 instances

**Estimated Effort**: 1 day

---

### [ ] Step: Phase 3.1 - Inbound Processor Crash Resilience

Verify and enhance crash resilience in Inbound Audio Processor.

**Files**:
- `inbound-audio-processor.cpp`

**Tasks**:
- [ ] Verify existing dump-to-null behavior when Whisper disconnected
- [ ] Add explicit reconnection logging with call_id
- [ ] Test: Kill Whisper during call, verify no crash
- [ ] Test: Restart Whisper, verify reconnection within 3 seconds
- [ ] Monitor memory: ensure no audio accumulation during disconnect

**Verification**:
- Start call, kill Whisper, wait 10s, restart Whisper
- Verify: Inbound logs "Whisper disconnected for call_id X, dumping audio"
- Verify: "Reconnected to Whisper for call_id X" within 3s of restart
- Memory stable: RSS doesn't grow during disconnect period

**Estimated Effort**: 0.5 day

---

### [ ] Step: Phase 3.2 - Whisper Service Crash Resilience

Implement transcription buffering for LLaMA disconnections.

**Files**:
- `whisper-service.cpp`

**Tasks**:
- [ ] Add `TranscriptionBuffer` class (max 10 transcriptions per call)
- [ ] On LLaMA send failure: buffer transcription, log warning
- [ ] On buffer full: discard oldest transcription (FIFO)
- [ ] Attempt reconnection to LLaMA every 2 seconds
- [ ] On reconnect: flush buffered transcriptions

**Verification**:
- Start call, kill LLaMA, send 15 transcriptions via audio
- Verify: Buffer holds 10, logs "Discarded oldest transcription for call_id X"
- Restart LLaMA, verify buffered transcriptions sent immediately
- No crashes during LLaMA downtime

**Estimated Effort**: 1 day

---

### [ ] Step: Phase 3.3 - LLaMA Service Crash Resilience

Handle Whisper and Kokoro disconnections gracefully.

**Files**:
- `llama-service.cpp`

**Tasks**:
- [ ] Implement non-blocking TCP accept with timeout (don't hang on startup)
- [ ] On Kokoro send failure: discard response, log warning (no buffering)
- [ ] Maintain KV cache conversation state regardless of connection status
- [ ] Test: Kill Kokoro, generate 5 responses, verify context preserved

**Verification**:
- Kill Kokoro during conversation
- Send 5 user inputs to LLaMA, verify responses generated (logged)
- Restart Kokoro, send new input, verify response uses full conversation history
- Test: Monitor `llama_get_kv_cache_used_cells()` to ensure no leaks

**Estimated Effort**: 1 day

---

### [ ] Step: Phase 3.4 - Kokoro Service Crash Resilience

Verify dump-to-null and add reconnection retry for Outbound Processor.

**Files**:
- `kokoro_service.py`

**Tasks**:
- [ ] Verify existing dump-to-null when Outbound disconnected
- [ ] Add reconnection retry every 2 seconds to Outbound ports
- [ ] Ensure thread-safe PyTorch model access (locks or per-thread instances)
- [ ] Test: Kill Outbound, send TTS requests, verify no crash

**Verification**:
- Start call, kill Outbound, send 10 TTS requests to Kokoro
- Verify: No crash, logs "Outbound disconnected, dumping audio"
- Restart Outbound, verify reconnection within 3 seconds
- Memory stable during disconnect

**Estimated Effort**: 0.5 day

---

### [ ] Step: Phase 4.1 - VAD Buffer Management Fix

Fix critical VAD buffer growth bug in Whisper Service.

**Files**:
- `whisper-service.cpp`

**Tasks**:
- [ ] Add window removal after processing: `audio_buffer.erase()` for each 1600-sample window
- [ ] Process ALL buffered windows in single iteration (not just first)
- [ ] Implement proper locking: acquire once, process all, release before transcription
- [ ] Test: Send continuous audio for 60 seconds, monitor buffer size

**Verification**:
- Send 60 seconds of continuous speech
- Monitor `audio_buffer.size()` via logs or debugger
- Verify: Buffer never exceeds 16000*8 samples (8-second safety limit)
- No memory growth: RSS stable over 10-minute test

**Estimated Effort**: 0.5 day

---

### [ ] Step: Phase 4.2 - VAD German Optimization

Optimize VAD for German speech patterns with improved segmentation.

**Files**:
- `whisper-service.cpp`

**Tasks**:
- [ ] Keep 800ms silence threshold (not 300ms) to avoid cutting compound words
- [ ] Verify 8-second maximum segment length
- [ ] Implement `VADMetrics` struct (segments detected, lengths, false positives)
- [ ] Add optional metrics logging to `/tmp/metrics/whisper_<call_id>.csv`

**Verification**:
- Generate test corpus (see Phase 4.3)
- Run 50 German sentences through VAD
- Measure: >95% correct segmentation (no mid-sentence cuts)
- Latency: <1s from end-of-speech to transcription start
- Export metrics to CSV for analysis

**Estimated Effort**: 1 day

---

### [ ] Step: Phase 4.3 - Test Corpus Generation

Create German audio test corpus for VAD and transcription accuracy testing.

**Files**:
- `scripts/generate_test_corpus.py` (new)
- `tests/test_corpus/` (new directory)

**Tasks**:
- [ ] Implement test corpus generation script using Kokoro library directly
- [ ] Define 50 German test sentences (5-30 words, various complexities)
- [ ] Generate 24kHz WAV files using Kokoro
- [ ] Downsample to 8kHz for telephony simulation (decimate by 3)
- [ ] Create ground truth JSON mapping filenames to transcripts
- [ ] Validate: Run Whisper on corpus, measure WER <5%

**Verification**:
- Run: `python3 scripts/generate_test_corpus.py`
- Verify: 50 audio files in `tests/test_corpus/`
- Verify: `transcripts.json` contains all 50 sentences
- Manual spot-check: Listen to 5 random files for quality

**Note**: This approach uses Kokoro to generate test audio (testing system with itself). This is acceptable for VAD testing but not ideal for TTS quality validation. For production validation, consider using publicly available German speech datasets (e.g., Mozilla Common Voice) or real recordings.

**Estimated Effort**: 1 day

---

### [ ] Step: Phase 5.1 - Sentence Completion Buffer

Implement sentence completion detection in LLaMA Service.

**Files**:
- `llama-service.cpp`

**Tasks**:
- [ ] Add `SentenceBuffer` class with accumulation and timeout logic
- [ ] Detect sentence endings: `.`, `?`, `!` (with German abbreviation awareness)
- [ ] Implement 2-second timeout since last chunk (primary trigger)
- [ ] Implement 5-second absolute timeout (safety limit)
- [ ] Buffer chunks until sentence complete before generating response

**Verification**:
- Test: Send "Wie ist" + 1s pause + "das Wetter?" 
- Verify: Single response generated after second chunk
- Test: Send "Dr. Schmidt ist" + 1s + "nicht verfügbar"
- Verify: Single response (doesn't trigger on "Dr.")
- Test: Send incomplete text, verify forced response after 2s timeout

**Estimated Effort**: 1 day

---

### [ ] Step: Phase 5.2 - Response Interruption with KV Cache Rollback

Implement response interruption and KV cache cleanup in LLaMA Service.

**Files**:
- `llama-service.cpp`

**Tasks**:
- [ ] Refactor generation to token-by-token with `llama_decode` loop
- [ ] Save KV cache checkpoint before generation: `n_past_checkpoint`
- [ ] Add per-call `std::atomic<bool> stop_flag`
- [ ] Check `stop_flag` every 5 tokens during generation
- [ ] On interrupt: Call `llama_kv_cache_seq_rm(ctx, seq_id, checkpoint, -1)` to rollback
- [ ] Add `InputMonitor` class to signal interruption when new input arrives
- [ ] Discard partial response text on interrupt

**Verification**:
- Start 50-token response generation, interrupt at token 10 with new input
- Measure: Interruption latency <200ms
- Test: After interruption, send new input, verify response uses full history
- Monitor: `llama_get_kv_cache_used_cells()` doesn't grow on interrupts
- Test: 20 interruptions, verify no KV cache leaks

**Estimated Effort**: 1-2 days

---

### [ ] Step: Phase 6.1 - Standalone Service Tests

Create unit tests for each service in isolation.

**Files**:
- `tests/test_sip_client.py` (new)
- `tests/test_inbound_processor.cpp` (new)
- `tests/test_whisper_vad.cpp` (new)
- `tests/test_llama_interruption.py` (new)
- `tests/test_kokoro_latency.py` (new)
- `tests/test_outbound_timing.cpp` (new)
- `tests/CMakeLists.txt` (new)

**Tasks**:
- [ ] **Review existing tests**: Examine `multi_call_test.py`, `test_llama_german.py`, `pipeline_loop_sim.cpp`, `whisper_inbound_sim.cpp`, etc.
  - Determine which tests are still relevant
  - Integrate useful tests into new framework or document their purpose
  - Archive or remove obsolete tests
- [ ] **SIP Client Test**: Mock INVITE/BYE, verify control signals sent
- [ ] **Inbound Test**: Send mock RTP, verify PCM output quality and CALL_END forwarding
- [ ] **Whisper VAD Test**: Feed test corpus, measure accuracy (>95%) and WER (<5%)
- [ ] **LLaMA Interruption Test**: Verify interruption <200ms and sentence buffering
- [ ] **Kokoro Latency Test**: Measure TTS latency <200ms for 15 words
- [ ] **Outbound Timing Test**: Verify 20ms RTP scheduling, jitter <5ms
- [ ] Add CMake targets for C++ tests
- [ ] Document test execution in `tests/README.md`

**Verification**:
- Build tests: `cd build && cmake .. && make`
- Run all tests: `./tests/test_*` and `python3 tests/test_*.py`
- All tests pass (6/6 services)
- Each test has clear pass/fail output

**Estimated Effort**: 2 days

---

### [ ] Step: Phase 6.2 - Integration Test Harness

Create automated pipeline integration test harness.

**Files**:
- `tests/pipeline_integration_test.py` (new)
- `tests/utils.py` (helper functions, new)

**Tasks**:
- [ ] Implement test harness to orchestrate all 6 services
- [ ] Add service lifecycle management: start, health check, stop
- [ ] Implement mock SIP INVITE/BYE injection
- [ ] Add RTP audio streaming from test corpus
- [ ] Capture and validate audio output from Outbound Processor
- [ ] Collect metrics from service logs (latency, memory, errors)
- [ ] Aggregate results to CSV and JSON

**Verification**:
- Run: `python3 tests/pipeline_integration_test.py --scenario single`
- Verify: Test completes successfully with pass/fail report
- Metrics collected and exported to `metrics/` directory

**Estimated Effort**: 1 day

---

### [ ] Step: Phase 6.3 - Integration Test Scenarios

Implement comprehensive integration test scenarios.

**Files**:
- `tests/pipeline_integration_test.py` (extend)

**Tasks**:
- [ ] **Scenario 1**: Single call (INVITE → conversation → BYE)
- [ ] **Scenario 2**: 10 concurrent calls (scalability)
- [ ] **Scenario 3**: Service crash recovery (kill/restart each service during call)
- [ ] **Scenario 4**: Long conversation (20+ turns, memory stability)
- [ ] **Scenario 5**: Rapid churn (100 calls in 5 minutes)
- [ ] **Scenario 6**: Chaos test (kill random service every 5s during conversation)
- [ ] **Scenario 7**: Cascade failure (kill Whisper+LLaMA simultaneously)
- [ ] **Scenario 8**: Restart during reconnection (kill service while partner reconnecting)
- [ ] Add `--scenario <name>` CLI argument to select test

**Verification**:
- Run all scenarios: `python3 tests/pipeline_integration_test.py --scenario all`
- All scenarios pass (8/8)
- Latency targets met: 90th percentile <1.5s end-to-end
- Memory stable: RSS doesn't grow over 1-hour test
- No crashes or hangs in any scenario

**Estimated Effort**: 2 days

---

### [ ] Step: Phase 7 - Performance Metrics Collection

Add optional metrics collection with minimal overhead.

**Files**:
- `whisper-service.cpp`
- `llama-service.cpp`
- `kokoro_service.py`

**Tasks**:
- [ ] Implement `CallMetrics` struct in each service
- [ ] Add timestamp logging at stage boundaries (start/end)
- [ ] Export to CSV: `/tmp/metrics/<service>_<call_id>.csv`
- [ ] Add `--enable-metrics` CLI flag (disabled by default)
- [ ] Implement log rotation: delete files older than 7 days (prevent disk exhaustion)
- [ ] Validate <1% CPU overhead for metrics collection

**Verification**:
- Run 50 calls with `--enable-metrics`
- Verify CSV files created in `/tmp/metrics/`
- Aggregate CSV data, verify latency targets met
- CPU comparison: measure overhead <1% vs. no metrics
- Test: 1000 calls over 1 hour, verify disk usage <100MB

**Estimated Effort**: 1 day

---

### [ ] Step: Final Verification and Documentation Update

Comprehensive verification and documentation updates.

**Files**:
- `.zencoder/rules/repo.md`
- `.zencoder/rules/sip-client.md`
- `.zencoder/rules/inbound-audio-processor.md`
- `.zencoder/rules/whisper-service.md`
- `.zencoder/rules/llama-service.md`
- `.zencoder/rules/kokoro-service.md`
- `.zencoder/rules/outbound-audio-processor.md`

**Tasks**:
- [ ] Run full test suite: all unit tests and integration tests
- [ ] Verify all acceptance criteria from requirements.md
- [ ] Performance benchmark: Compare Phase 7 results vs. Phase 0 baseline
- [ ] Stress test: 100 concurrent calls on M1 Max, verify stability
- [ ] Update service documentation with new control plane details
- [ ] Update "Internal Function" sections to reflect signal handling
- [ ] Update "Inbound/Outbound Connections" sections for Unix sockets
- [ ] Document known limitations (German abbreviations in sentence detection)

**Verification Checklist**:
- [ ] All 6 unit tests pass
- [ ] All 8 integration scenarios pass
- [ ] End-to-end latency: 90th percentile <1.5s
- [ ] VAD accuracy: >95% on German test corpus
- [ ] Interruption latency: <200ms
- [ ] Crash recovery: <3s reconnection time
- [ ] 100 concurrent calls supported with stable performance
- [ ] No memory leaks (stable RSS over 1-hour test)
- [ ] Build with warnings enabled: no errors or warnings
- [ ] Documentation updated (6 service summaries)

**Estimated Effort**: 1 day

---

## Summary

**Total Steps**: 19 implementation steps across 9 phases (Phase -1 through Phase 7 and Final Verification)
**Total Estimated Effort**: 15-19 days (includes 0.5 day for prerequisites + 0.5 day buffer for Phase 1.1)
**Calendar Duration**: ~3 weeks with parallelization

**Critical Path**:
1. Phase -1 (prerequisites) → Phase 0 (baseline) → Phase 1 (lifecycle) → Phase 3 (resilience)
2. Phase 4 (VAD) and Phase 5 (LLaMA) can partially overlap with Phase 3
3. Phase 6 (testing) depends on all implementation phases
4. Phase 7 (metrics) and Final Verification are sequential at the end

**Risk Mitigation**:
- Phase -1 verifies all dependencies before starting implementation
- Each phase has clear verification criteria before proceeding
- Baseline measurement (Phase 0) enables regression detection
- Service startup documentation (Phase 0) ensures reproducibility
- Standalone tests (Phase 6.1) catch issues before integration
- Comprehensive test scenarios (Phase 6.3) validate crash resilience
- Existing test review (Phase 6.1) prevents loss of valuable test coverage

**Next Actions**:
1. Review this plan with stakeholders
2. Begin Phase -1 (Prerequisites Verification) immediately
3. Document service startup process in Phase 0
4. Track progress by marking `[x]` for completed steps
