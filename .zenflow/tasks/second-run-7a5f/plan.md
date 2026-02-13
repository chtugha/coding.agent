# Full SDD workflow

## Configuration
- **Artifacts Path**: {@artifacts_path} → `.zenflow/tasks/{task_id}`

---

## Workflow Steps

### [x] Step: Requirements
<!-- chat-id: 10a02007-4f6e-4f89-a844-2d4fc85e9cd7 -->

Create a Product Requirements Document (PRD) based on the feature description.

1. Review existing codebase to understand current architecture and patterns
2. Analyze the feature definition and identify unclear aspects
3. Ask the user for clarifications on aspects that significantly impact scope or user experience
4. Make reasonable decisions for minor details based on context and conventions
5. If user can't clarify, make a decision, state the assumption, and continue

Save the PRD to `{@artifacts_path}/requirements.md`.

### [x] Step: Technical Specification
<!-- chat-id: 39b0a4a7-31e4-4ff1-bbde-03f0d4c096af -->

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
<!-- chat-id: 96b0c96f-522c-49d6-b62f-4ef48daa6b99 -->

Create a detailed implementation plan based on `{@artifacts_path}/spec.md`.

1. Break down the work into concrete tasks
2. Each task should reference relevant contracts and include verification steps
3. Replace the Implementation step below with the planned tasks

Rule of thumb for step size: each step should represent a coherent unit of work (e.g., implement a component, add an API endpoint). Avoid steps that are too granular (single function) or too broad (entire feature).

Important: unit tests must be part of each implementation task, not separate tasks. Each task should implement the code and its tests together, if relevant.

If the feature is trivial and doesn't warrant full specification, update this workflow to remove unnecessary steps and explain the reasoning to the user.

Save to `{@artifacts_path}/plan.md`.

---

## Implementation Tasks

### [ ] Phase 1: Interconnection System Foundation

#### [ ] Step: Design and implement core interconnect.h header
- Create `coding.agent/interconnect.h` with namespace `whispertalk`
- Implement `PortConfig` struct with 6 port types (neg_in, neg_out, down_in, down_out, up_in, up_out)
- Implement `ServiceType` enum for all 6 services
- Implement `Packet` struct with serialization/deserialization (call_id, size, payload)
- Implement port calculation formulas (neg_in+1, neg_in+2, neg_out+1, neg_out+2)
- Add packet validation (size limits, call_id validation)
- **Verification**: Compile header-only, no syntax errors

#### [ ] Step: Implement master/slave port discovery
- Implement `scan_and_bind_ports()` in `InterconnectNode`
- Add port scanning algorithm (start at 22222/33333, increment by 3)
- Add atomic `bind()` checks for race condition handling
- Implement master election (first to bind 22222/33333)
- Add slave registration protocol (`REGISTER`, `REGISTER_ACK`)
- Add service registry in master (map of ServiceType -> PortConfig)
- **Verification**: Unit test with 6 mock services, verify no port conflicts

#### [ ] Step: Implement traffic connection establishment
- Implement `accept_from_downstream()` - bind and listen on down_in, down_out
- Implement `connect_to_upstream()` - connect to downstream neighbor's ports
- Add connection state machine (DISCONNECTED -> CONNECTING -> CONNECTED -> FAILED)
- Add `get_upstream_service()` and `get_downstream_service()` queries to master
- Implement socket setup (SO_NOSIGPIPE, non-blocking mode, timeouts)
- **Verification**: Two services establish bidirectional connections

#### [ ] Step: Implement send/recv operations
- Implement `send_to_downstream()` with mutex protection
- Implement `send_to_upstream()` with mutex protection
- Implement `recv_from_downstream()` with poll/timeout
- Implement `recv_from_upstream()` with poll/timeout
- Add non-blocking send with retry logic
- Add partial packet buffering for incomplete reads
- **Verification**: Send 1000 packets between two services, verify delivery

#### [ ] Step: Implement heartbeat and crash detection
- Implement `heartbeat_loop()` thread
- Add heartbeat send every 2s (HEARTBEAT message)
- Add master heartbeat timeout detection (>5s)
- Implement `SERVICE_CRASHED` notification broadcast
- Add `is_service_alive()` query
- Add `upstream_state()` and `downstream_state()` getters
- **Verification**: Kill slave, verify master detects within 5s

#### [ ] Step: Implement call lifecycle management
- Implement `reserve_call_id()` with atomic reservation
- Add `CHECK_CALL_ID` / `CALL_ID_RESERVED` protocol
- Implement `broadcast_call_end()` in master
- Add `CALL_END` broadcast to all slaves
- Implement `register_call_end_handler()` callback mechanism
- Add call_id tracking set and max_known_call_id
- **Verification**: Concurrent call_id reservations from 10 threads, no collisions

#### [ ] Step: Implement reconnection logic
- Implement `reconnect_loop()` thread
- Add automatic reconnection every 2s on connection failure
- Add query to master for neighbor status
- Implement connection re-establishment after crash
- Add state transition handling during reconnection
- **Verification**: Kill upstream service, verify downstream reconnects within 5s

#### [ ] Step: Create interconnect unit tests
- Create `coding.agent/tests/test_interconnect.cpp`
- Add test: Master election with 6 services starting simultaneously
- Add test: Port scanning and no conflicts
- Add test: Packet serialization round-trip
- Add test: Heartbeat timeout detection
- Add test: Call_id atomic reservation
- Add test: Reconnection after crash
- Add test: CALL_END propagation
- Use Google Test framework
- **Verification**: All tests pass with `ctest --output-on-failure`

---

### [ ] Phase 2: IPC Migration (All Services)

#### [ ] Step: Remove existing IPC from SIP Client
- Remove UDP socket code (ports 9001, 9002)
- Remove Unix socket control code
- Remove hard-coded port definitions
- Integrate `interconnect.h`
- Initialize `InterconnectNode` with `ServiceType::SIP_CLIENT`
- Replace UDP send/recv with interconnect traffic port send/recv
- Keep external SIP/RTP UDP (network-facing) unchanged
- **Verification**: Compile successfully, no legacy IPC code remains

#### [ ] Step: Add multi-line SIP support and call_id reservation to SIP Client
- Add support for multiple SIP registrations (configurable via CLI)
- Create separate thread per SIP line
- Implement `RESERVE_CALL_ID` protocol with IAP
- Add atomic call_id negotiation (mutex-protected)
- Implement call_id increment logic (max_known + 1)
- Add CALL_END sending via master on BYE received
- **Verification**: 3 SIP lines create calls simultaneously, verify unique call_ids

#### [ ] Step: Remove existing IPC from Inbound Audio Processor
- Remove UDP receive code (port 9001)
- Remove TCP send code (port 13000+)
- Integrate `interconnect.h`
- Initialize `InterconnectNode` with `ServiceType::INBOUND_AUDIO_PROCESSOR`
- Replace UDP recv with interconnect recv from SIP Client
- Replace TCP send with interconnect send to Whisper
- Add call_id tracking set for `CHECK_CALL_ID` responses
- **Verification**: Compile successfully

#### [ ] Step: Add crash resilience to Inbound Audio Processor
- Implement /dev/null redirection when Whisper disconnected
- Add downstream state monitoring
- Continue processing audio from SIP even if Whisper crashed
- Add automatic rerouting on Whisper reconnection
- Implement CALL_END handler to close conversion threads
- **Verification**: Kill Whisper, verify IAP continues receiving from SIP

#### [ ] Step: Remove existing IPC from Whisper Service
- Remove TCP receive code (port 13000+)
- Remove TCP send code (port 8083)
- Integrate `interconnect.h`
- Initialize `InterconnectNode` with `ServiceType::WHISPER_SERVICE`
- Replace TCP recv with interconnect recv from IAP
- Replace TCP send with interconnect send to LLaMA
- **Verification**: Compile successfully

#### [ ] Step: Add crash resilience to Whisper Service
- Add upstream state monitoring (IAP)
- Add downstream state monitoring (LLaMA)
- Implement /dev/null for discarded transcriptions when LLaMA down
- Add buffer (64 packets) for temporary disconnections
- Implement CALL_END handler to close transcription sessions
- **Verification**: Kill IAP, verify Whisper survives and reconnects

#### [ ] Step: Remove existing IPC from LLaMA Service
- Remove TCP receive code (port 8083)
- Remove TCP send code (port 8090)
- Integrate `interconnect.h`
- Initialize `InterconnectNode` with `ServiceType::LLAMA_SERVICE`
- Replace TCP recv with interconnect recv from Whisper
- Replace TCP send with interconnect send to Kokoro
- **Verification**: Compile successfully

#### [ ] Step: Add interruption support to LLaMA Service
- Implement sentence completion detection (`.`, `?`, `!`)
- Add mid-sentence interruption on new user input
- Use reverse channel (recv_from_upstream) for interruption signals
- Maintain separate KV cache contexts per call_id
- Implement CALL_END handler to clear conversation contexts
- **Verification**: Send new input mid-response, verify response stops

#### [ ] Step: Remove existing IPC from Outbound Audio Processor
- Remove TCP receive code (port 8090+)
- Remove UDP send code (port 9002)
- Remove Unix socket control code
- Integrate `interconnect.h`
- Initialize `InterconnectNode` with `ServiceType::OUTBOUND_AUDIO_PROCESSOR`
- Replace TCP recv with interconnect recv from Kokoro
- Replace UDP send with interconnect send to SIP Client
- **Verification**: Compile successfully

#### [ ] Step: Create temporary Python Kokoro interconnect adapter
- Create `coding.agent/kokoro_interconnect_adapter.py`
- Listen on interconnect ports (22235, 22236)
- Forward to existing Python Kokoro service
- Receive from Python Kokoro and forward to interconnect
- This is temporary until C++ port is complete
- **Verification**: Python Kokoro receives text via interconnect

#### [ ] Step: End-to-end pipeline test with interconnect
- Start all 6 services (Python Kokoro via adapter)
- Make SIP call
- Verify audio flows through entire pipeline
- Verify transcription, LLM response, TTS output
- Measure end-to-end latency (<1s target)
- **Verification**: Complete call with intelligible German TTS output

#### [ ] Step: Multi-call test with new interconnect
- Start pipeline
- Make 3 concurrent SIP calls
- Verify unique call_ids for each
- Verify correct audio routing per call_id
- Verify no cross-talk between calls
- **Verification**: 3 calls complete independently

#### [ ] Step: Crash recovery test
- Start pipeline with active call
- Kill Whisper service mid-call
- Verify IAP redirects to /dev/null
- Restart Whisper service
- Verify automatic reconnection
- Verify call resumes processing
- **Verification**: Call completes after service restart

---

### [ ] Phase 3: Kokoro C++ Port

#### [ ] Step: Set up libtorch and espeak-ng dependencies
- Download libtorch 2.0+ for macOS arm64
- Extract to `coding.agent/third_party/libtorch`
- Install espeak-ng via Homebrew
- Verify espeak-ng C API headers available
- Update CMakeLists.txt to find_package(Torch)
- Add espeak-ng library linkage
- **Verification**: CMake finds both libraries

#### [ ] Step: Export Kokoro models to TorchScript
- Create `coding.agent/export_kokoro_model.py`
- Load existing Python Kokoro model
- Export to TorchScript via `torch.jit.script()`
- Save model to `models/kokoro_german.pt`
- Export voice embeddings (`df_eva`, `dm_bernd`) to `.pt` files
- **Verification**: TorchScript files created and loadable in Python

#### [ ] Step: Implement espeak-ng phonemization in C++
- Create `KokoroPipeline` class in `coding.agent/kokoro-service.cpp`
- Initialize espeak-ng with German language (`de`)
- Implement `phonemize()` method using `espeak_TextToPhonemes()`
- Convert espeak-ng output to IPA phoneme vector
- Add phoneme normalization for Kokoro compatibility
- **Verification**: Phonemize 100 German sentences, compare with Python

#### [ ] Step: Implement Kokoro model inference in C++
- Load TorchScript model via `torch::jit::load()`
- Load voice embeddings via `torch::load()`
- Set device to MPS (`torch::Device(torch::kMPS)`)
- Implement `synthesize()` method
- Convert phonemes to sequence tensor
- Run model forward pass
- Extract audio tensor (float32, 24kHz)
- **Verification**: Synthesize "Hallo Welt", verify audio output

#### [ ] Step: Validate phonemization accuracy
- Create `coding.agent/tests/test_phoneme_diff.cpp`
- Load 500 German test sentences
- Compare C++ espeak-ng output vs Python phonemizer
- Calculate phoneme match rate
- Document mismatches
- **Verification**: >95% phoneme match rate (REQ-KOK-PHO-002)

#### [ ] Step: Implement phoneme normalization layer
- Analyze phoneme mismatches from validation
- Create mapping rules (stress marks, syllable boundaries, vowel variants)
- Implement normalization in `phonemize()` method
- Re-run validation tests
- **Verification**: Phoneme match rate improves to >98%

#### [ ] Step: Audio quality testing (PESQ)
- Generate 50 German sentences with C++ Kokoro
- Generate same 50 sentences with Python Kokoro (reference)
- Use PESQ tool to compare audio quality
- Target: PESQ >3.5 (spec requirement)
- If <3.5, activate fallback plan (pre-computed dictionary)
- **Verification**: PESQ score documented, >3.5 achieved

#### [ ] Step: Integrate Kokoro C++ with interconnect
- Add `InterconnectNode` initialization to kokoro-service.cpp
- Implement multi-call support (separate TTS stream per call_id)
- Receive text packets from LLaMA via interconnect
- Send audio packets to OAP via interconnect
- Use threads for concurrent synthesis
- Implement CALL_END handler
- **Verification**: Kokoro receives text, sends audio via interconnect

#### [ ] Step: Add crash resilience to Kokoro Service
- Add upstream state monitoring (LLaMA)
- Add downstream state monitoring (OAP)
- Implement /dev/null for audio when OAP disconnected
- Add buffer for temporary disconnections
- Continue synthesis even if OAP crashed
- **Verification**: Kill OAP, verify Kokoro continues processing

#### [ ] Step: Multi-call TTS test
- Start pipeline with C++ Kokoro
- Make 5 concurrent calls
- Verify 5 independent TTS streams
- Verify correct audio routing per call_id
- Measure per-call latency
- **Verification**: 5 calls produce distinct German audio simultaneously

#### [ ] Step: Kokoro performance optimization
- Profile TTS latency per sentence
- Optimize phonemization (cache common words)
- Optimize model inference (batch if possible)
- Target: <200ms per sentence
- **Verification**: Latency <200ms for 95% of sentences

#### [ ] Step: Remove Python Kokoro adapter
- Delete `kokoro_interconnect_adapter.py`
- Remove Python Kokoro service from documentation
- Update build scripts to only build C++ Kokoro
- **Verification**: Pipeline runs entirely C++, no Python runtime

---

### [ ] Phase 4: Static Binary Build System

#### [ ] Step: Configure CMake for static linking
- Set `BUILD_SHARED_LIBS=OFF` globally
- Add `-static-libstdc++ -static-libgcc` flags (where applicable on macOS)
- Configure whisper.cpp as static library
- Configure llama.cpp as static library (clone if needed)
- Link pthread statically where possible
- **Verification**: `ldd` shows minimal dynamic dependencies

#### [ ] Step: Handle libtorch dynamic libraries
- Accept libtorch must be dynamic (.dylib)
- Bundle libtorch .dylib files in `lib/` directory
- Use `install_name_tool -add_rpath @executable_path/../lib`
- Set rpath on kokoro-service binary
- **Verification**: `otool -L kokoro-service` shows @rpath references

#### [ ] Step: Bundle espeak-ng dependencies
- Copy espeak-ng .dylib to `lib/`
- Copy espeak-ng-data directory to distribution root
- Set `ESPEAK_DATA_PATH` environment variable or use `espeak_ng_InitializePath()`
- Use rpath for espeak-ng library
- **Verification**: espeak-ng loads phoneme data from bundled directory

#### [ ] Step: Create distribution directory structure
- Create `bin/` with all 6 binaries
- Create `lib/` with .dylib files
- Create `models/` with Whisper, LLaMA, Kokoro models
- Create `espeak-ng-data/` with phoneme dictionaries
- Create `run.sh` wrapper script (optional, sets DYLD_LIBRARY_PATH)
- **Verification**: Directory structure matches spec Section 2.5.3

#### [ ] Step: Test on clean macOS environment
- Set up macOS VM or clean test machine
- Copy distribution directory
- Run binaries without Homebrew/MacPorts in PATH
- Verify models load from relative paths
- Verify no "library not found" errors
- **Verification**: All services start and connect successfully

#### [ ] Step: Verify static linking with otool
- Run `otool -L bin/*` for all binaries
- Verify only system frameworks and bundled libs
- No references to /usr/local or /opt/homebrew (except system)
- Check total distribution size (<3 GB)
- **Verification**: Distribution is self-contained

---

### [ ] Phase 5: Multi-Call and Crash Resilience Testing

#### [ ] Step: Call ID collision test
- Create test script: 10 SIP lines simultaneously create calls
- Verify no duplicate call_ids
- Verify all reservations complete successfully
- Run 1000 calls total
- **Verification**: No call_id collisions detected

#### [ ] Step: Crash recovery matrix test
- For each service type (SIP, IAP, Whisper, LLaMA, Kokoro, OAP):
  - Start pipeline with active call
  - Kill service mid-call
  - Verify neighbors detect crash within 5s
  - Verify neighbors redirect streams or buffer
  - Restart service
  - Verify reconnection within 5s
  - Verify call resumes
- **Verification**: All 6 service types recover successfully

#### [ ] Step: Concurrency stress test
- Start 20 concurrent calls
- Run for 10 minutes
- Monitor latency per call
- Verify no calls fail
- Verify call_id routing correct for all 20
- **Verification**: All 20 calls complete, avg latency <1.5s

#### [ ] Step: Memory leak test
- Start pipeline
- Make 100 calls over 1 hour (varied timing)
- Monitor RSS (Resident Set Size) for each service
- Verify RSS growth <5% over 1 hour
- Use valgrind or Instruments (macOS) if leaks suspected
- **Verification**: No memory leaks detected

#### [ ] Step: CALL_END propagation test
- Make call, hang up immediately
- Verify CALL_END broadcast from SIP Client
- Verify all 5 slaves receive CALL_END
- Verify all slaves ACK within 5s
- Verify resources cleaned up in all services
- **Verification**: CALL_END ACK success rate >99%

#### [ ] Step: Master crash and recovery test
- Start pipeline with master (SIP Client typically)
- Make active call
- Kill master mid-call
- Verify existing connections continue
- Verify new call attempts fail gracefully
- Restart master
- Verify slaves re-register
- Verify new calls succeed
- **Verification**: System recovers after master restart

#### [ ] Step: Port conflict test
- Start pipeline
- Start second instance of SIP Client
- Verify port scanning increments correctly
- Verify second instance becomes slave
- Both instances operate independently
- **Verification**: Two SIP Clients run simultaneously

---

### [ ] Phase 6: Performance Optimization and Bug Fixes

#### [ ] Step: Profile end-to-end latency
- Instrument each service with timestamps
- Measure SIP RTP in -> transcription -> LLM response -> TTS -> RTP out
- Identify bottleneck services
- Target <800ms (95th percentile)
- **Verification**: Latency breakdown documented

#### [ ] Step: Optimize Whisper VAD
- Profile VAD processing time
- Tune energy threshold for German telephony
- Verify no word cutting
- Verify sentences complete before sending to LLaMA
- Test with 50 German utterances (manual review)
- **Verification**: VAD correctly segments 95%+ of utterances

#### [ ] Step: Optimize LLaMA inference
- Profile token generation time
- Verify Metal/MPS acceleration active
- Reduce max tokens if >64 causes delays
- Test response quality vs speed tradeoff
- **Verification**: LLaMA response time <300ms per response

#### [ ] Step: Optimize interconnect send/recv
- Profile packet send/recv overhead
- Reduce mutex contention if detected
- Use lock-free queues if beneficial
- Verify no head-of-line blocking
- **Verification**: Interconnect overhead <10ms per packet

#### [ ] Step: Fix bugs discovered in testing
- Review all test failures from Phase 5
- Prioritize critical bugs (crashes, data loss)
- Fix connection race conditions
- Fix packet deserialization errors
- Fix call_id tracking inconsistencies
- **Verification**: All Phase 5 tests re-run and pass

#### [ ] Step: CPU usage optimization
- Monitor CPU usage per service under load
- Target <200% per service
- Optimize busy-wait loops (use proper sleep/poll)
- Verify threads don't spin unnecessarily
- **Verification**: CPU usage within target

#### [ ] Step: Final integration test suite
- Run all unit tests: `ctest --output-on-failure`
- Run end-to-end single call test
- Run multi-call test (5 concurrent)
- Run crash recovery test (all 6 services)
- Run 1-hour stress test (5 calls)
- **Verification**: All tests pass, no crashes, no leaks

#### [ ] Step: Documentation and deployment guide
- Update README with new architecture
- Document interconnection system design
- Document port layout and connection topology
- Document build process for static binaries
- Document deployment steps
- Document troubleshooting (port conflicts, model loading)
- **Verification**: Fresh developer can build and run from README

---

## Test Results Log

### Phase 1 Tests
- [ ] Interconnect unit tests pass
- [ ] Master election works with 6 services
- [ ] Heartbeat detects crash within 5s
- [ ] Call_id reservation prevents collisions

### Phase 2 Tests
- [ ] End-to-end pipeline with interconnect works
- [ ] Multi-call (3 concurrent) works
- [ ] Crash recovery (Whisper) works

### Phase 3 Tests
- [ ] Phonemization accuracy >95%
- [ ] PESQ score >3.5
- [ ] Multi-call TTS (5 concurrent) works
- [ ] C++ Kokoro latency <200ms

### Phase 4 Tests
- [ ] Static binaries run on clean macOS
- [ ] Distribution size <3 GB
- [ ] Models load from relative paths

### Phase 5 Tests
- [ ] Call ID collision test (1000 calls): 0 collisions
- [ ] Crash recovery matrix: all 6 services pass
- [ ] Concurrency stress (20 calls, 10 min): all pass
- [ ] Memory leak test (100 calls, 1 hour): <5% growth
- [ ] CALL_END propagation: >99% ACK rate

### Phase 6 Tests
- [ ] End-to-end latency <800ms (95th percentile)
- [ ] VAD accuracy >95%
- [ ] CPU per service <200%
- [ ] Final integration suite: all pass

---

## Notes
- Total estimated effort: 27-29 working days (5-6 weeks)
- Kokoro C++ port is highest risk (10-12 days)
- Fallback plan for phonemization issues documented in spec Section 2.4.3
- Master failover deferred to future work
- Static binary approach is hybrid (dynamic libtorch, bundled)
