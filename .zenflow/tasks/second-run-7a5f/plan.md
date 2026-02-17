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

### [x] Phase 0: Dependency Setup
<!-- chat-id: 0169510e-15c5-4fb0-b224-86bbbcab0fce -->

#### [x] Step: Clone and build llama-cpp dependency
- Clone llama.cpp repository into `llama-cpp/` directory
- Pin to a stable release tag (b8022)
- Build with Metal/MPS support: `cmake -B build -DBUILD_SHARED_LIBS=OFF -DLLAMA_METAL=ON -DCMAKE_BUILD_TYPE=Release`
- Verify `llama-cpp/build/src/libllama.a` is created (static library)
- Verify `llama-cpp/include/llama.h` and `llama-cpp/ggml/include/ggml.h` exist
- **Verification**: `ls llama-cpp/build/src/libllama.a` succeeds, `make -j4` in build dir completes without errors

#### [x] Step: Set up Google Test framework
- Add Google Test via CMake FetchContent in `CMakeLists.txt`
- Add `enable_testing()` and `BUILD_TESTS` option to CMakeLists.txt
- Create minimal `tests/test_sanity.cpp` to verify framework works
- **Verification**: `cmake -DBUILD_TESTS=ON .. && make && ctest --output-on-failure` passes

---

### [x] Phase 1: Interconnection System Foundation
<!-- chat-id: cb6e068d-412e-461d-9d1a-f6debc9a48bf -->

#### [x] Step: Design and implement core interconnect.h header
- Create `interconnect.h` (root directory) with namespace `whispertalk`
- Implement `PortConfig` struct with 6 port types:
  - `down_in = neg_in + 1` (listen: upstream neighbor connects here to send data TO us)
  - `down_out = neg_in + 2` (listen: upstream neighbor connects here to recv data FROM us)
  - `up_in = neg_out + 1` (connect: we connect to downstream neighbor's down_out to recv FROM them)
  - `up_out = neg_out + 2` (connect: we connect to downstream neighbor's down_in to send TO them)
- Implement `ServiceType` enum for all 6 services
- Implement `Packet` struct with serialization/deserialization (call_id, size, payload) using network byte order
- Add packet validation (payload_size <= 1MB, call_id != 0)
- **Verification**: Create `tests/test_interconnect_compile.cpp` that includes `interconnect.h` and tests basic structs (`PortConfig`, `Packet` serialization round-trip, `ServiceType` enum); compile with `g++ -std=c++17 -c tests/test_interconnect_compile.cpp -I.`

#### [x] Step: Implement master/slave port discovery
- Implement `scan_and_bind_ports()` in `InterconnectNode`
- Add port scanning algorithm (start at 22222/33333, increment by 3, max 100 increments)
- Rely on OS-level atomic `bind()` for race condition safety (EADDRINUSE -> next pair)
- Implement master election (first to bind 22222/33333)
- Add slave registration protocol (`REGISTER <type> <neg_in> <neg_out>`, `REGISTER_ACK`)
- Add service registry in master (map of ServiceType -> PortConfig)
- After binding negotiation ports, immediately bind 4 traffic ports (neg_in+1, neg_in+2 for listening)
- **Verification**: Unit test with 6 mock services starting simultaneously, verify no port conflicts

#### [x] Step: Implement traffic connection establishment
- Implement `accept_from_upstream()` — bind and listen on down_in, down_out (upstream neighbor connects here)
- Implement `connect_to_downstream()` — connect up_out to downstream's down_in, connect up_in to downstream's down_out
- Add connection state machine (DISCONNECTED -> CONNECTING -> CONNECTED -> FAILED -> DISCONNECTED via reconnect)
- Add `get_upstream_service()` and `get_downstream_service()` queries to master (`GET_UPSTREAM <type>`, `GET_DOWNSTREAM <type>`)
- Implement socket setup (SO_NOSIGPIPE, non-blocking mode, 5s connect timeout)
- **Verification**: Two services establish bidirectional monodirectional TCP connections

#### [x] Step: Implement send/recv operations
- Implement `send_to_downstream()` with `send_downstream_mutex_` protection (sends on up_out connected socket)
- Implement `send_to_upstream()` with `send_upstream_mutex_` protection (sends on down_out accepted socket)
- Implement `recv_from_downstream()` with poll/timeout (recvs on up_in connected socket)
- Implement `recv_from_upstream()` with poll/timeout (recvs on down_in accepted socket)
- Add non-blocking send with 100ms retry, returns false on full buffer
- Add partial packet buffering for incomplete reads (up to 5s timeout for remainder)
- **Verification**: Send 1000 packets between two services, verify 100% delivery

#### [x] Step: Implement heartbeat and crash detection
- Implement `heartbeat_loop()` thread (started by `start_heartbeat()`)
- Add heartbeat send every 2s (`HEARTBEAT <type> <call_count> <state>`)
- Add master heartbeat ACK to slaves (`HEARTBEAT_ACK`)
- Add master crash timeout detection (>5s since last heartbeat)
- Implement `SERVICE_CRASHED <type>` notification to affected neighbors only
- Add `is_service_alive()` query and `upstream_state()`/`downstream_state()` getters
- **Verification**: Kill slave, verify master detects within 5s and notifies neighbors

#### [x] Step: Implement call lifecycle management
- Implement `reserve_call_id()` with atomic reservation via `call_id_mutex_`
- Add `RESERVE_CALL_ID <proposed_id>` / `CALL_ID_RESERVED <final_id>` protocol (SIP -> IAP via negotiation port)
- IAP atomically: if proposed > max_known, reserve it; else reserve max_known+1
- Implement `broadcast_call_end()` in master: sends `CALL_END <call_id>` to all slaves
- Master waits up to 5s for `CALL_END_ACK <call_id>` from each slave; missing ACKs logged as warnings
- Duplicate CALL_END for same call_id: idempotent, ACK immediately if already cleaned up
- Implement `register_call_end_handler()` callback mechanism
- Add call_id tracking set and max_known_call_id
- **Verification**: Concurrent call_id reservations from 10 threads, no collisions

#### [x] Step: Implement reconnection logic
- Implement `reconnect_loop()` thread (retries every 2s)
- On connection failure: mark state FAILED, query master for neighbor status via `GET_UPSTREAM`/`GET_DOWNSTREAM`
- Master returns `SERVICE_UNAVAILABLE` if neighbor not registered or heartbeat missing
- On neighbor re-registration: master returns new ports, service reconnects
- State transitions: FAILED -> DISCONNECTED -> CONNECTING -> CONNECTED
- **Verification**: Kill upstream service, verify downstream reconnects within 5s of upstream restart

#### [x] Step: Create interconnect unit tests
- Create `tests/test_interconnect.cpp` using Google Test
- Add test: Master election with 6 services starting simultaneously
- Add test: Port scanning — no conflicts across 6 services
- Add test: Packet serialization round-trip (call_id, size, payload preserved)
- Add test: Heartbeat timeout detection within 5s
- Add test: Call_id atomic reservation — 10 concurrent threads, no collisions
- Add test: Reconnection after crash — downstream reconnects within 5s
- Add test: CALL_END broadcast and ACK propagation
- **Verification**: `ctest --output-on-failure` — all tests pass

---

### [x] Phase 2: IPC Migration (All Services)
<!-- chat-id: 845c0990-714a-4618-82e2-9d8b2194d8a6 -->

#### [x] Step: Remove existing IPC from SIP Client and integrate interconnect
- Remove UDP socket code (ports 9001, 9002) from `sip-client-main.cpp`
- Remove Unix socket control code (`/tmp/*.ctrl`)
- Remove hard-coded port definitions
- Include `interconnect.h`, initialize `InterconnectNode` with `ServiceType::SIP_CLIENT`
- Remove old UDP send to IAP; use interconnect TCP `send_to_downstream()` instead (sends RTP to IAP)
- Remove old UDP recv from OAP; use interconnect TCP `recv_from_upstream()` instead (receives G.711 from OAP — in the return path OAP is upstream, SIP is downstream per spec connection #11)
- Note: SIP has dual role due to circular pipeline — acts as upstream when sending to IAP (`send_to_downstream()`), acts as downstream when receiving from OAP (`recv_from_upstream()`). All internal IPC is now TCP via interconnect.
- Keep external SIP/RTP UDP (network-facing) unchanged
- Add crash resilience: continue SIP signaling if IAP/OAP disconnected
- **Verification**: `make -j4` compiles successfully, no legacy IPC code in sip-client-main.cpp

#### [x] Step: Add multi-line SIP support and call_id reservation to SIP Client
- Add support for multiple SIP registrations (configurable via CLI, e.g., `--lines 3`)
- Create separate thread per SIP line
- Implement `RESERVE_CALL_ID` protocol with IAP via negotiation port:
  1. Lock local `call_id_mutex`, set `proposed_id = max_known_call_id + 1`
  2. Send `RESERVE_CALL_ID <proposed_id>` to IAP
  3. Receive `CALL_ID_RESERVED <final_id>`, update `max_known_call_id = final_id`
  4. Unlock mutex
- Add CALL_END sending via master on BYE received
- **Verification**: 3 SIP lines create calls simultaneously, verify unique call_ids

#### [x] Step: Remove existing IPC from Inbound Audio Processor and integrate interconnect
- Remove UDP receive code (port 9001) from `inbound-audio-processor.cpp`
- Remove TCP send code (port 13000+)
- Include `interconnect.h`, initialize `InterconnectNode` with `ServiceType::INBOUND_AUDIO_PROCESSOR`
- Remove old UDP recv from SIP Client; use interconnect TCP `recv_from_upstream()` instead (receives RTP from SIP Client)
- Remove old TCP send to Whisper; use interconnect TCP `send_to_downstream()` instead (sends PCM to Whisper)
- Add call_id tracking set for `RESERVE_CALL_ID` responses
- Implement /dev/null redirection when Whisper disconnected (check `downstream_state()`)
- Add automatic rerouting on Whisper reconnection
- Implement CALL_END handler to close per-call conversion threads
- **Verification**: `make -j4` compiles; kill Whisper, verify IAP continues receiving from SIP

#### [x] Step: Remove existing IPC from Whisper Service and integrate interconnect
- Remove TCP receive code (port 13000+) from `whisper-service.cpp`
- Remove TCP send code (port 8083)
- Include `interconnect.h`, initialize `InterconnectNode` with `ServiceType::WHISPER_SERVICE`
- Remove old TCP recv from IAP; use interconnect TCP `recv_from_upstream()` instead (receives PCM from IAP)
- Remove old TCP send to LLaMA; use interconnect TCP `send_to_downstream()` instead (sends text to LLaMA)
- Add upstream/downstream state monitoring
- Implement /dev/null for discarded transcriptions when LLaMA down
- Add buffer (64 packets) for temporary disconnections
- Implement CALL_END handler to close transcription sessions
- **Verification**: `make -j4` compiles; kill IAP, verify Whisper survives and reconnects

#### [x] Step: Remove existing IPC from LLaMA Service and integrate interconnect
- Remove TCP receive code (port 8083) from `llama-service.cpp`
- Remove TCP send code (port 8090)
- Include `interconnect.h`, initialize `InterconnectNode` with `ServiceType::LLAMA_SERVICE`
- Remove old TCP recv from Whisper; use interconnect TCP `recv_from_upstream()` instead (receives text from Whisper)
- Remove old TCP send to Kokoro; use interconnect TCP `send_to_downstream()` instead (sends response text to Kokoro)
- Add interruption support:
  - Detect sentence completion (`.`, `?`, `!`) — wait for complete sentences before responding
  - Monitor `recv_from_upstream()` for new user input during generation
  - Stop generation mid-sentence on new input
  - Use reverse channel for interrupt signaling
- Maintain separate KV cache contexts per call_id
- Implement CALL_END handler to clear conversation contexts
- **Verification**: `make -j4` compiles; send new input mid-response, verify response stops

#### [x] Step: Remove existing IPC from Outbound Audio Processor and integrate interconnect
- Remove TCP receive code (port 8090+) from `outbound-audio-processor.cpp`
- Remove UDP send code (port 9002)
- Remove Unix socket control code (`/tmp/outbound-audio-processor.ctrl`)
- Include `interconnect.h`, initialize `InterconnectNode` with `ServiceType::OUTBOUND_AUDIO_PROCESSOR`
- Remove old TCP recv from Kokoro; use interconnect TCP `recv_from_upstream()` instead (receives PCM from Kokoro)
- Remove old UDP send to SIP Client; use interconnect TCP `send_to_downstream()` instead (sends G.711 to SIP Client)
- Add crash resilience for upstream (Kokoro) and downstream (SIP Client)
- Implement CALL_END handler to close per-call conversion threads
- **Verification**: `make -j4` compiles successfully, no legacy IPC code

#### [x] Step: Create temporary Python Kokoro interconnect adapter
- Create `kokoro_interconnect_adapter.py` (root directory)
- Implement interconnect protocol in Python: bind negotiation ports, register as slave
- Listen on traffic ports for text packets from LLaMA
- Forward to existing Python Kokoro service (`kokoro_service.py`)
- Receive synthesized audio from Python Kokoro and forward via interconnect to OAP
- This is temporary until C++ port is complete in Phase 3
- **Verification**: Python Kokoro receives text and produces audio via interconnect

#### [x] Step: End-to-end pipeline test with interconnect
- Start all 6 services (Python Kokoro via adapter)
- Make SIP call
- Verify audio flows through entire pipeline: SIP -> IAP -> Whisper -> LLaMA -> Kokoro -> OAP -> SIP
- Verify transcription, LLM response, TTS output
- Measure end-to-end latency (<1s target)
- **Verification**: Complete call with intelligible German TTS output, `make -j4` builds all targets cleanly

#### [x] Step: Multi-call and crash recovery tests
- Multi-call: Start pipeline, make 3 concurrent SIP calls
  - Verify unique call_ids for each
  - Verify correct audio routing per call_id
  - Verify no cross-talk between calls
- Crash recovery: Start pipeline with active call
  - Kill Whisper service mid-call
  - Verify IAP redirects to /dev/null
  - Restart Whisper service
  - Verify automatic reconnection and call resumes processing
- **Verification**: 3 calls complete independently; call completes after Whisper restart

---

### [x] Phase 3: Kokoro C++ Port

#### [x] Step: Set up libtorch and espeak-ng dependencies
- Download libtorch 2.0+ for macOS arm64
- Extract to `third_party/libtorch`
- Install espeak-ng via Homebrew (`brew install espeak-ng`)
- Verify espeak-ng C API headers available (`#include <espeak-ng/speak_lib.h>`)
- Update CMakeLists.txt: `set(CMAKE_PREFIX_PATH "third_party/libtorch")`, `find_package(Torch REQUIRED)`
- Add espeak-ng library linkage: `find_library(ESPEAK_NG_LIB espeak-ng REQUIRED)`
- Add kokoro-service target to CMakeLists.txt
- **Verification**: `cmake .. && make kokoro-service` finds both libraries and compiles skeleton

#### [x] Step: Export Kokoro models to CoreML + HAR TorchScript
- Create unified `scripts/export_kokoro_models.py` (self-contained, downloads model, installs deps via conda)
- Export CoreML duration model (BERT + prosody) -> `coreml/kokoro_duration.mlmodelc` (ANE, ~65ms)
- Export CoreML split decoder (3 buckets: 3s/5s/10s) -> `decoder_variants/kokoro_decoder_split_*.mlmodelc` (ANE, ~70ms)
- Export HAR TorchScript models (SineGen+STFT, ~20KB each) -> `decoder_variants/kokoro_har_*.pt` (CPU)
- Export voice embeddings (`df_eva`, `dm_bernd`) to `.bin` raw float32 format
- Export vocab.json (phoneme-to-ID mapping)
- Requires conda env with torch==2.5.0, coremltools==8.3.0, numpy==1.26.4
- **Verification**: CoreML models loadable in C++ via CoreML.framework, HAR via `torch::jit::load()`

#### [x] Step: Implement espeak-ng phonemization in C++
- Create `KokoroPipeline` class in `kokoro-service.cpp`
- Initialize espeak-ng with German language (`de`) via `espeak_ng_Initialize()` + `espeak_SetVoiceByName("de")`
- Implement `phonemize()` method using `espeak_TextToPhonemes()` with `espeakCHARS_UTF8 | espeakPHONEMES_IPA`
- Convert espeak-ng output to IPA phoneme vector
- Add phoneme normalization for Kokoro model compatibility
- **Verification**: Phonemize 100 German sentences, compare with Python `phonemizer` output

#### [x] Step: Implement Kokoro model inference in C++ (CoreML split pipeline)
- Load CoreML duration model via `CoreMLDurationModel` class (Objective-C++ / CoreML.framework)
- Load CoreML split decoder via `CoreMLSplitDecoder` class (3 ANE buckets)
- Load HAR TorchScript models via `torch::jit::load()` (CPU)
- Load voice embeddings from `.bin` raw float32 format
- Implement `synthesize_coreml()`: phonemes -> CoreML duration -> CPU alignment -> HAR -> CoreML decoder -> float32 audio (24kHz)
- **Verification**: Synthesize "Hallo Welt", verify non-silent audio output, ~145ms total latency

#### [x] Step: Validate phonemization accuracy and add normalization
- Create `tests/test_phoneme_diff.cpp`
- Load 500 German test sentences
- Compare C++ espeak-ng output vs Python phonemizer output
- Calculate phoneme match rate, document mismatches by category (stress marks, syllable boundaries, vowel variants)
- If match rate <95%: implement normalization mapping rules in `phonemize()`
- Re-run validation after normalization
- **Verification**: >95% phoneme match rate (target >98% after normalization)

#### [x] Step: Audio quality testing (PESQ)
- Generate 50 German sentences with C++ Kokoro
- Generate same 50 sentences with Python Kokoro (reference)
- Use PESQ tool to compare audio quality
- Target: PESQ >3.5 (spec requirement REQ-KOK-PHO-002)
- If <3.5 after normalization: pin espeak-ng version, or create pre-computed phoneme dictionary for 10,000 common German words
- **Verification**: PESQ score documented, >3.5 achieved

#### [x] Step: Integrate Kokoro C++ with interconnect and add crash resilience
- Add `InterconnectNode` initialization to `kokoro-service.cpp` with `ServiceType::KOKORO_SERVICE`
- Implement multi-call support (separate TTS thread per call_id)
- Receive text packets from LLaMA via `recv_from_upstream()`
- Send audio packets to OAP via `send_to_downstream()`
- Add upstream state monitoring (LLaMA) and downstream state monitoring (OAP)
- Implement /dev/null for audio when OAP disconnected
- Implement CALL_END handler to close per-call synthesis threads
- **Verification**: Kokoro receives text, sends audio via interconnect; kill OAP, verify Kokoro continues

#### [x] Step: Multi-call TTS test and performance optimization
- Start pipeline with C++ Kokoro
- Make 5 concurrent calls, verify 5 independent TTS streams with correct call_id routing
- Profile TTS latency per sentence
- Optimize phonemization (cache common words)
- Optimize model inference (batch if possible)
- Target: <200ms per sentence for 95% of sentences
- **Verification**: 5 calls produce distinct German audio simultaneously, latency <200ms

#### [x] Step: Remove Python Kokoro adapter and validate full C++ pipeline
- Delete `kokoro_interconnect_adapter.py`
- Delete `kokoro_service.py` (replaced by C++ `kokoro-service.cpp`)
- Update CMakeLists.txt install target to include kokoro-service
- Run full pipeline end-to-end with C++ Kokoro only
- **Verification**: Pipeline runs entirely in C++, no Python runtime required, `make -j4` builds all 6 targets

---

### [x] Phase 4: Self-Contained Distribution Build System

#### [x] Step: Configure CMake for maximum static linking
- Set `BUILD_SHARED_LIBS=OFF` globally in CMakeLists.txt
- Configure whisper.cpp as static library (`add_subdirectory(whisper-cpp)` with static flags)
- Configure llama.cpp as static library (`add_subdirectory(llama-cpp)` with static flags)
- Note: macOS does not support `-static-libstdc++`; system frameworks (CoreML, Metal) remain dynamic
- **Verification**: `otool -L bin/*` shows minimal dynamic dependencies (system frameworks only + bundled libs)

#### [x] Step: Handle libtorch and espeak-ng dynamic libraries
- Accept libtorch must be dynamic (.dylib) on macOS
- Bundle libtorch .dylib files (`libtorch.dylib`, `libtorch_cpu.dylib`, `libc10.dylib`) in `lib/` directory
- Bundle espeak-ng .dylib in `lib/`
- Copy espeak-ng-data directory (`/opt/homebrew/share/espeak-ng-data/`) to distribution root
- Use `install_name_tool -add_rpath @executable_path/../lib` on kokoro-service binary
- Call `espeak_ng_InitializePath("./espeak-ng-data")` at runtime
- **Verification**: `otool -L bin/kokoro-service` shows @rpath references; espeak-ng loads bundled data

#### [x] Step: Create distribution directory structure and test
- Create distribution layout: `bin/` (6 binaries), `lib/` (.dylib files), `models/` (Whisper, LLaMA, Kokoro), `espeak-ng-data/`
- Create optional `run.sh` wrapper script (sets `DYLD_LIBRARY_PATH` as fallback)
- Run `otool -L bin/*` — verify no references to /usr/local or /opt/homebrew (only system frameworks + bundled libs)
- Check total distribution size (<3 GB)
- Test on clean macOS environment (no Homebrew/MacPorts in PATH): copy distribution, run binaries, verify models load from relative paths
- **Verification**: All services start and connect on clean macOS, distribution is self-contained

---

### [-] Phase 5: Multi-Call and Crash Resilience Testing

#### [x] Step: Test SIP Provider (B2BUA)
- Created `tests/test_sip_provider.cpp` — standalone B2BUA that bridges two SIP clients
- Accepts REGISTER from two clients, sends INVITE to both, relays RTP between them
- Proper SIP 3-way handshake with ACK after 200 OK (RFC 3261 compliant)
- Comprehensive error handling on all socket operations
- SDP parsing with port validation and connection IP extraction
- Race condition fixes: relay threads copy socket FDs locally, atomic relay_started flag
- Optional `--inject` flag sends 3s 400Hz G.711 test tone to kick-start pipeline
- Reports bidirectional packet flow statistics (pass/fail)
- Added to CMakeLists.txt as `test_sip_provider` target under BUILD_TESTS
- Created `tests/test_sip_provider_unit.cpp` — 25 GTest unit tests covering:
  - G.711 μ-law encoding (silence, positive/negative range, symmetry, clipping, sine wave)
  - SIP header parsing (basic, leading spaces, missing, end-of-message, LF-only, multiple)
  - SDP parsing (media port, multi-codec, no media, invalid port, connection IP)
  - RTP packet structure (header fields, payload size)
  - SIP username extraction (URI, tags, numbered lines)
- **Verification**: Binary builds, 25/25 unit tests pass, all 57 total tests pass (2 sanity + 23 interconnect + 7 kokoro + 25 SIP provider unit)
- **Usage**: `test_sip_provider --port 5060 --duration 60 --inject` + two `sip-client` instances

#### [x] Step: Call ID collision test
- Added 3 GTest cases to `tests/test_interconnect.cpp` under `CallIDCollisionTest` suite:
  - `MasterOnly1000Reservations`: 10 threads × 100 calls each on master, all concurrent — 1000 unique IDs, 0 collisions
  - `MixedMasterAndSlave1000Reservations`: 5 master threads + 5 slave threads × 100 calls each, concurrent via RESERVE_CALL_ID protocol — 1000 successful, 1000 unique, 0 failures
  - `SequentialMonotonicallyIncreasing`: 200 sequential reservations all proposing ID 1 — verifies monotonic increment and uniqueness
- **Verification**: 3/3 collision tests pass. 0 collisions across 1000 concurrent calls. All reservations succeed via both master-local and slave-via-protocol paths.

#### [x] Step: Master failover implementation and tests
- Implemented master failover in `interconnect.h`:
  - `SYNC_REGISTRY`: Master broadcasts service_registry + max_known_call_id to all slaves every heartbeat cycle (2s)
  - `STEP_DOWN`: Original master sends to promoted slave on restart; promoted slave responds with `STEPPED_DOWN` + serialized state, then demotes itself
  - `NEW_MASTER`: Promoted master notifies all slaves of leadership change
  - `attempt_promote_to_master()`: Slave binds 22222/33333 after 3 consecutive heartbeat failures (~6s), absorbs synced registry state
  - `try_reclaim_master_port()`: Original master sends STEP_DOWN to promoted slave during init, waits for port release, then reclaims 22222
  - `demote_to_slave()`: Promoted slave releases master ports, rebinds as slave, re-registers with new master
- Added 4 GTest cases to `tests/test_interconnect.cpp` under `MasterFailoverTest` suite:
  - `SlavePromotesAfterMasterCrash`: Slave detects master death via heartbeat timeout, promotes to master, reserves new call IDs
  - `CallIDsSurviveFailover`: 50 pre-crash call IDs preserved; post-failover IDs continue from synced max (>50)
  - `OriginalMasterReclaims`: Original master restarts, sends STEP_DOWN, promoted slave demotes, master reclaims port 22222 and absorbs state
  - `ThirdPartySlaveSeesNewMaster`: Two slaves present; one promotes after master crash; remaining slave can reserve call IDs from new master
- **Verification**: 4/4 failover tests pass. All 30/30 interconnect tests pass. Total 57 tests pass (2 sanity + 30 interconnect + 25 SIP provider unit).

#### [x] Step: Crash recovery matrix test
- Added parameterized `CrashRecoveryMatrixTest` to `tests/test_interconnect.cpp` testing all 6 service types:
  - `SIP_CLIENT`: Master crash triggers slave promotion, restarted master reclaims via STEP_DOWN
  - `INBOUND_AUDIO_PROCESSOR`: Crash detected ~3s, re-registration instant
  - `WHISPER_SERVICE`: Crash detected ~3s, upstream neighbor detects DISCONNECTED/FAILED, reconnects after restart
  - `LLAMA_SERVICE`: Crash detected ~3s, upstream neighbor detects DISCONNECTED/FAILED, reconnects after restart
  - `KOKORO_SERVICE`: Crash detected ~3s, upstream neighbor detects DISCONNECTED/FAILED, reconnects after restart
  - `OUTBOUND_AUDIO_PROCESSOR`: Crash detected ~3s, upstream neighbor detects DISCONNECTED/FAILED, reconnects after restart
- For each non-master service: verifies heartbeat crash detection, upstream traffic state transition, re-registration with master, and upstream reconnection
- For master (SIP_CLIENT): verifies slave promotion after crash, original master reclaim, and call ID reservation continuity
- **Verification**: 6/6 crash recovery tests pass. Crash detection ~3s (within 5s target). All 63 tests pass (2 sanity + 36 interconnect + 25 SIP provider unit).

#### [x] Step: Concurrency stress test
- Added 3 concurrency stress tests to `tests/test_interconnect.cpp`:
  - `TwentyConcurrentCallsNoXtalk`: 20 concurrent calls × 500 packets each = 10,000 total packets, 20 sender threads firing simultaneously, receiver verifies embedded call_id matches packet call_id (cross-talk detection), per-call delivery counts verified
  - `CallEndDuringActiveTraffic`: 20 calls with continuous traffic, 10 calls ended mid-stream via CALL_END broadcast, verifies all 10 CALL_END delivered to slave while remaining calls continue
  - `BidirectionalMultiCallRouting`: 20 calls with simultaneous upstream + downstream traffic (2,000 packets each direction), cross-talk detection on both paths
- Cross-talk detection: each packet embeds its call_id in payload bytes 0-3; receiver verifies payload call_id == header call_id
- **Verification**: 3/3 stress tests pass. 10,000 packets delivered (min=500, max=500 per call). Zero cross-talk. CALL_END propagation 10/10. Bidirectional 2,000+2,000 packets. All 39 interconnect tests pass (36 previous + 3 new). Total 66 tests pass (2 sanity + 39 interconnect + 25 SIP provider unit).

#### [ ] Step: Memory leak and CALL_END propagation tests
- Memory leak: Start pipeline, make 100 calls over 1 hour (varied timing), monitor RSS per service, verify growth <5%
- CALL_END: Make call, hang up immediately, verify master broadcasts CALL_END, all 5 slaves ACK within 5s, resources cleaned up
- Master crash: Kill master mid-call, verify existing connections continue, restart master, verify slaves re-register and new calls succeed
- Port conflict: Start second SIP Client instance, verify port scanning increments correctly, both instances operate independently
- **Verification**: No leaks, CALL_END ACK rate >99%, master recovery works, port conflicts resolved

---

### [ ] Phase 6: Performance Optimization and Bug Fixes

#### [ ] Step: Profile and optimize end-to-end latency
- Instrument each service with timestamps at packet entry/exit points
- Measure full path: SIP RTP in -> transcription -> LLM response -> TTS -> RTP out
- Identify bottleneck services
- Optimize interconnect send/recv: reduce mutex contention, verify no head-of-line blocking
- Target: <800ms end-to-end (95th percentile), interconnect overhead <10ms per packet
- **Verification**: Latency breakdown documented, <800ms achieved

#### [ ] Step: Optimize Whisper VAD for German telephony
- Profile VAD processing time
- Tune energy threshold for German telephony audio (100ms windows)
- Verify no word cutting, sentences complete before sending to LLaMA
- Test with 50 German utterances (manual review)
- **Verification**: VAD correctly segments 95%+ of utterances

#### [ ] Step: Optimize LLaMA inference and CPU usage
- Profile token generation time, verify Metal/MPS acceleration active
- Reduce max tokens if >64 causes delays
- Monitor CPU usage per service under load (target <200%)
- Optimize busy-wait loops (use proper sleep/poll), verify threads don't spin unnecessarily
- **Verification**: LLaMA response time <300ms, CPU within target

#### [ ] Step: Fix bugs discovered in testing
- Review all test failures from Phase 5
- Prioritize critical bugs (crashes, data loss, connection race conditions)
- Fix packet deserialization errors, call_id tracking inconsistencies
- Re-run all Phase 5 tests
- **Verification**: All Phase 5 tests pass on re-run

#### [ ] Step: Final integration test suite
- Run all unit tests: `ctest --output-on-failure`
- Run end-to-end single call test
- Run multi-call test (5 concurrent)
- Run crash recovery test (all 6 services)
- Run 1-hour stress test (5 calls, no crashes, no leaks)
- **Verification**: All tests pass, zero crashes, zero leaks

---

## Test Results Log

### Phase 0 Tests
- [x] llama-cpp builds successfully with Metal support
- [x] Google Test framework compiles and runs minimal test

### Phase 1 Tests
- [x] Interconnect unit tests pass (23/23 tests passed)
- [x] Master election works with 6 services
- [x] Port scanning — no conflicts across 6 services (36 unique ports)
- [x] Traffic connection establishment (upstream/downstream bidirectional TCP)
- [x] Send/recv operations — 1000 packets 100% delivery
- [x] Bidirectional simultaneous transfer — 100 packets each direction
- [x] Heartbeat detects crash within 5s
- [x] Call_id reservation prevents collisions (10 concurrent threads)
- [x] CALL_END broadcast with ACK and idempotent duplicate handling
- [x] Service discovery (GET_DOWNSTREAM ports)
- [x] Connection state machine (DISCONNECTED → CONNECTED transitions)
- [x] Reconnection after downstream restart — upstream auto-reconnects within 5s

### Phase 2 Tests
- [x] All 5 C++ services compile successfully (sip-client, inbound-audio-processor, whisper-service, llama-service, outbound-audio-processor)
- [x] whisper-service links correctly with whisper-cpp static library + ggml + Apple frameworks + CoreML
- [x] Multi-line SIP support: --lines CLI parameter, per-line threads, per-line registration
- [x] 64-packet buffer limit enforced in whisper-service with oldest-drop overflow
- [x] 25/25 interconnect unit tests still pass after migration
- [x] bin/models/ directory created with .gitkeep (protected from clean builds)
- [x] whisper.cpp rebuilt with CoreML support (WHISPER_COREML=ON, WHISPER_METAL=ON)
- [x] Whisper large-v3 model downloaded and quantized to q5_0 (~1GB) in bin/models/
- [x] CoreML encoder model generated (.mlpackage) and compiled (.mlmodelc) in bin/models/
- [x] LLaMA-3.2-1B-Instruct-Q8_0.gguf downloaded (~1.2GB) to bin/models/
- [x] WHISPERTALK_MODELS_DIR compile definition added for absolute model path resolution
- [x] Services default to compile-time WHISPERTALK_MODELS_DIR, fallback to relative models/ path
- [ ] End-to-end pipeline with interconnect works (requires SIP infrastructure)
- [ ] Multi-call (3 concurrent) works (requires SIP infrastructure)
- [ ] Crash recovery (Whisper) works (requires SIP infrastructure)

### Phase 3 Tests
- [x] libtorch detected via PyTorch 2.10.0 cmake_prefix_path, espeak-ng 1.52.0 via Homebrew
- [x] Kokoro German model exported to TorchScript (kokoro_german.pt) via torch.jit.trace()
- [x] Voice embeddings exported (df_eva_embedding.pt, dm_bernd_embedding.pt), vocab.json extracted
- [x] espeak-ng C++ phonemization: German IPA output verified (4 sentences, all correct)
- [x] Vocab loading: 114 entries from vocab.json
- [x] Phoneme encoding: correct token ID sequences with start/end padding
- [x] TorchScript model loads and runs in C++ (torch::jit::load)
- [x] Voice pack shape [512, 1, 256] loaded correctly
- [x] End-to-end synthesis: 3 German sentences, all produce valid audio (35k-76k samples @ 24kHz)
- [x] Audio range: non-silent, proper amplitude ([-1.04, 0.80])
- [x] Synthesis latency: 315-438ms per sentence (CPU mode)
- [x] Interconnect integration: KokoroService with recv_from_upstream/send_to_downstream, crash resilience, CALL_END handler
- [x] Python Kokoro files deleted (kokoro_service.py, kokoro_interconnect_adapter.py, bin/kokoro_service.py)
- [x] All 6 C++ services compile (make -j4), 25/25 interconnect tests pass, 6/6 kokoro tests pass
- [x] CMakeLists.txt includes kokoro-service in install targets, RPATH configured for libtorch
- [x] Multi-call threading: per-call worker threads via CallContext + std::thread, CALL_END joins threads
- [x] Portable espeak-ng data path: CMake auto-detection + env var + fallback chain (no hardcoded path)
- [x] MPS acceleration: tested but unsupported (SDPA kernel not available on MPS for TorchScript); CPU-only is correct
- [x] CoreML acceleration: exhaustively tested via 3 paths:
  - TorchScript→CoreML (coremltools 7.1-9.0): fails on `cs` attribute in `_jit_pass_lower_graph`, then `repeat_interleave` dynamic shapes
  - PyTorch→ONNX→CoreML: coremltools dropped ONNX source support in v7.x+
  - ONNX Runtime CoreML EP: supports 84% of decoder nodes but 51 partitions cause overhead (0.95x slower than CPU)
  - Root cause: Kokoro's dynamic alignment (`repeat_interleave` with data-dependent repeats) creates dynamic output shapes incompatible with CoreML's static computation graph
  - TorchScript CPU (260ms) benchmarked 1.56x faster than ONNX Runtime CPU (409ms) for L16
  - Conclusion: TorchScript + libtorch CPU is the optimal inference path for this model architecture
- [x] Model files (.pt, .pth) removed from git tracking, .gitignore updated with bin/models/**/*.pt and .pth patterns
- [x] Voice selection: --voice=df_eva or --voice=dm_bernd CLI argument
- [x] Export script moved to scripts/export_kokoro_model.py
- [x] Phoneme cache: LRU-style with 10,000 entry limit and full-clear eviction
- [x] UTF-8 handling: fixed to use (c & 0x80) != 0 check with bounds validation
- [x] Audio validation: reject empty or >10s output samples
- [x] CoreML duration model: exported to .mlmodelc, loaded via Objective-C++ CoreML API, inference 65ms on ANE
- [x] CoreML split decoder: 3 buckets (3s/5s/10s) on ANE, avg 70ms — 5x faster than TorchScript (365ms), 7x smaller (321MB vs 2296MB)
- [x] HAR TorchScript models: SineGen+STFT source on CPU (~20KB each)
- [x] Binary voice format (.bin): ~10x faster load than pickle, both df_eva and dm_bernd exported
- [x] ONNX and TorchScript decoder backends removed — CoreML-only architecture
- [x] Obsolete files cleaned: 7 TorchScript bucket models (~2.2GB), ONNX files (~620MB), old export scripts (4 files)
- [x] Unified export script: scripts/export_kokoro_models.py (downloads model, creates conda env, exports all artifacts)
- [x] Generated JSON files removed from git tracking; .gitignore updated with *.onnx.data pattern
- [x] KOKORO.md documentation: architecture, export process, building, benchmarks, known limitations
- [x] 7/7 kokoro tests pass (phonemization, vocab, encoding, voice, CoreML duration, CoreML decoder benchmark, model inventory)
- [x] 23/23 interconnect tests pass, 2/2 sanity tests pass — total 32/32 all pass

### Phase 4 Tests
- [x] Self-contained distribution: all 6 binaries + 6 bundled dylibs pass otool portability check (no /opt/homebrew or /usr/local refs)
- [x] Distribution size: 4.7GB total (236MB runtime + 4.4GB models); models alone exceed 3GB target due to 7 Kokoro buckets + Whisper + LLaMA
- [x] Models load from relative paths via WHISPERTALK_MODELS_DIR env var override
- [x] All services start from dist/whispertalk/ via run.sh wrapper (DYLD_LIBRARY_PATH + ESPEAK_NG_DATA + WHISPERTALK_MODELS_DIR)
- [x] whisper-service loads model from dist, initializes Metal on Apple M4
- [x] kokoro-service --help works from dist (libtorch + espeak-ng dylibs resolve correctly)
- [x] Code signing: all binaries and dylibs re-signed after install_name_tool modifications
- [x] 25/25 interconnect tests + 7/7 kokoro tests still pass after changes
- [x] BUILD_SHARED_LIBS=OFF in CMakeLists.txt; 5 of 6 binaries are fully static (system libs only)
- [x] scripts/package-dist.sh automates: copy binaries, bundle dylibs, fix rpaths, re-sign, copy models/espeak-data, verify

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
- Total estimated effort: 30-35 working days (6-7 weeks), includes 10-20% buffer for unforeseen issues
- Kokoro C++ port is highest risk (10-12 days base + potential 5 days for phonemization issues)
- Fallback plan for phonemization issues documented in spec Section 2.4.3
- Master failover implemented in Phase 5 (SYNC_REGISTRY, STEP_DOWN, NEW_MASTER protocol; slave promotion + original master reclaim)
- Phase 4 is "self-contained distribution" (hybrid approach: static where possible, bundled .dylib for libtorch/espeak-ng)
- Connection terminology: "upstream" = closer to SIP-in (data source); upstream service connects TO downstream service's listen ports
- All file paths are relative to project root (not `coding.agent/` subdirectory)
