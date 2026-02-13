# Product Requirements Document: WhisperTalk Standalone Architecture Redesign

## 1. Executive Summary

This document outlines the requirements for a comprehensive architectural redesign of the WhisperTalk real-time speech-to-speech system. The redesign focuses on:

- **Standalone Operation**: Each microservice runs as a self-contained static binary with no external runtime dependencies
- **Unified Communication**: A new TCP-based master/slave interconnection system replacing all existing IPC mechanisms (UDP, TCP, Unix sockets)
- **Native Implementation**: Conversion of the Python-based Kokoro TTS service to C++ using libtorch
- **Resilience**: Crash-proof inter-service communication with automatic reconnection and stream redirection
- **Multi-Call Support**: Robust handling of multiple simultaneous SIP calls across all services with call_id tracking

## 2. Current State Analysis

### 2.1 Existing Architecture

**Services:**
1. SIP Client (C++, 247 lines) - SIP signaling and RTP gateway
2. Inbound Audio Processor (C++, 156 lines) - G.711 decode and 8kHz->16kHz upsampling
3. Whisper Service (C++, 207 lines) - ASR using whisper.cpp with CoreML
4. LLaMA Service (C++, 265 lines) - Conversational AI using llama.cpp with CoreML
5. Kokoro TTS Service (Python, 549 lines) - Text-to-speech using PyTorch/MPS
6. Outbound Audio Processor (C++, 210 lines) - PCM->G.711 encoding and RTP packetization

**Current IPC Mechanisms (all to be removed):**
- UDP: SIP Client <-> Inbound/Outbound Processors (ports 9001, 9002)
- TCP: Point-to-point between services (ports 13000+, 8083, 8090+)
- Unix Domain Sockets: Control signals (e.g., `/tmp/outbound-audio-processor.ctrl`)

**Current Dependencies:**
- whisper.cpp library (CoreML optimized)
- llama.cpp library (CoreML optimized)
- Python 3.9+ runtime with PyTorch, Kokoro, numpy libraries
- System libraries (pthread, network, etc.)

### 2.2 Identified Problems

1. **Mixed IPC protocols** create complexity and inconsistent error handling
2. **Python dependency** prevents true standalone deployment
3. **Hard-coded ports** limit scalability and create port conflicts
4. **No central orchestration** makes crash recovery ad-hoc per service
5. **Fragile connections** between services with incomplete reconnection logic
6. **Multiple call handling** varies in robustness across services

## 3. Goals and Non-Goals

### 3.1 Goals

- **G1**: All services run as standalone static binaries without requiring Python, system libraries (beyond essential libc), or model files to be separately installed
- **G2**: Unified TCP-only communication protocol across all services (remove ALL existing UDP, TCP, Unix socket IPC)
- **G3**: Master/slave architecture with automatic service discovery and registration
- **G4**: Crash-resilient connections with automatic reconnection and stream redirection to /dev/null
- **G5**: Consistent multi-call support with call_id tracking across the entire pipeline
- **G6**: German language support maintained in native C++ TTS implementation
- **G7**: Real-time performance preserved (sub-1s end-to-end latency)
- **G8**: Support for multiple concurrent SIP lines per SIP client instance

### 3.2 Non-Goals

- **NG1**: Supporting languages other than German and English (current scope only)
- **NG2**: Distributed deployment across multiple machines (localhost-only for now)
- **NG3**: Changing the core ML models (Whisper, LLaMA, Kokoro) - only integration approach changes
- **NG4**: Web or REST API interfaces - focus remains on SIP telephony
- **NG5**: Hot-swapping of services without connection reset

## 4. Detailed Requirements

### 4.1 Interconnection System Design

#### 4.1.1 Master/Slave Architecture

**REQ-ICS-001**: The interconnection system shall use TCP exclusively for all inter-service communication. All existing UDP, TCP, Unix socket, stream, or other IPC mechanisms shall be removed from every service.

**REQ-ICS-002**: Each service shall have two negotiation ports:
- **Incoming negotiation port**: Starting at 22222, incremented by 3
- **Outgoing negotiation port**: Starting at 33333, incremented by 3

**REQ-ICS-003**: Port allocation shall follow this discovery process:
1. Service attempts to bind to ports 22222 and 33333
2. If both are free and binding succeeds, service becomes **Master**
3. If either port is already occupied (by another service), service scans upward in increments of 3 (22225/33336, 22228/33339, 22231/33342, ...) until a free pair is found
4. Service binds the free pair, becomes **Slave**, and registers itself with Master at 22222/33333

**REQ-ICS-004**: The Master shall maintain a registry of all active services containing:
- Service type (sip-client, inbound-audio-processor, whisper-service, llama-service, kokoro-service, outbound-audio-processor)
- Service instance ID
- Negotiation port numbers (incoming and outgoing)
- Connection status (active, crashed, reconnecting)
- Last heartbeat timestamp

**REQ-ICS-005**: Slaves shall send periodic heartbeat messages (every 2 seconds) to the Master via negotiation ports.

**REQ-ICS-006**: The Master shall detect crashed services when heartbeat is missing for >5 seconds and notify affected upstream/downstream services.

#### 4.1.2 Traffic Ports (4 per service)

**REQ-ICS-007**: Each service shall have **4 traffic ports** (plus 2 negotiation ports) for actual data transfer -- 2 on the downstream side and 2 on the upstream side. Port numbers are calculated as follows:
- **Downstream incoming traffic port**: negotiation incoming port + 1
- **Downstream outgoing traffic port**: negotiation incoming port + 2
- **Upstream incoming traffic port**: negotiation outgoing port + 1
- **Upstream outgoing traffic port**: negotiation outgoing port + 2

Port layout example for all 6 services:

| Service | Neg In | Neg Out | Down In (NI+1) | Down Out (NI+2) | Up In (NO+1) | Up Out (NO+2) |
|---------|--------|---------|----------------|-----------------|--------------|---------------|
| SIP Client (Master) | 22222 | 33333 | 22223 | 22224 | 33334 | 33335 |
| Inbound Audio Proc | 22225 | 33336 | 22226 | 22227 | 33337 | 33338 |
| Whisper Service | 22228 | 33339 | 22229 | 22230 | 33340 | 33341 |
| LLaMA Service | 22231 | 33342 | 22232 | 22233 | 33343 | 33344 |
| Kokoro Service | 22234 | 33345 | 22235 | 22236 | 33346 | 33347 |
| Outbound Audio Proc | 22237 | 33348 | 22238 | 22239 | 33349 | 33350 |

**REQ-ICS-008**: Traffic port connections shall always be initiated by the **downstream** service connecting to its **upstream** service.

Concrete connection example between Inbound Audio Processor and Whisper Service (from PR spec):
- IAP connects its upstream in port (33337) to Whisper's downstream out port (22230)
- IAP connects its upstream out port (33338) to Whisper's downstream in port (22229)

Port math verification:
- IAP: neg_in=22225, neg_out=33336 -> upstream in=33337 (NO+1), upstream out=33338 (NO+2)
- Whisper: neg_in=22228, neg_out=33339 -> downstream in=22229 (NI+1), downstream out=22230 (NI+2)
- Connection: IAP 33337 <-> Whisper 22230, IAP 33338 <-> Whisper 22229

**REQ-ICS-009**: The Master shall provide a service discovery API via negotiation ports:
- `GET_UPSTREAM <service-type>` -> returns negotiation ports of upstream service
- `GET_DOWNSTREAM <service-type>` -> returns negotiation ports of downstream service
- `GET_STATUS <service-type>` -> returns connection status

**REQ-ICS-010**: The connection between services is aware of crashes/disappearances of their corresponding neighbors. Status is checked for reconnection over the Master.

#### 4.1.3 Protocol and Packet Structure

**REQ-ICS-011**: All traffic packets shall include:
```
[4 bytes: call_id] [4 bytes: packet_size] [N bytes: payload]
```

**REQ-ICS-012**: Packet size shall be negotiated between services in the **upstream direction**:
- Each service advertises its required packet size to its upstream partner via the negotiation port **before** connecting traffic ports
- Upstream service confirms or rejects based on buffer constraints

**REQ-ICS-013**: Call termination shall propagate via Master:
- Service detecting call end (typically SIP client) sends `CALL_END <call_id>` to Master via negotiation port
- Master broadcasts `CALL_END <call_id>` to every slave via their negotiation ports
- Each service cleans up resources for that call_id

#### 4.1.4 Implementation Approach

**REQ-ICS-014**: The interconnection system shall be implemented as a single C++ code file (`interconnect.h`) included in the code of all services.

**REQ-ICS-015**: The header shall provide:
- `InterconnectNode` class with master/slave role management
- `TrafficConnection` class for upstream/downstream data transfer (4 traffic ports)
- `CallEndHandler` callback interface
- Port scanning and binding utilities (increment by 3, check for occupied ports)
- Packet serialization/deserialization functions
- Heartbeat and crash detection logic
- Reconnection management via Master

### 4.2 SIP Client Requirements

**REQ-SIP-001**: The SIP client shall support multiple concurrent SIP lines (registrations to different SIP accounts). Several SIP lines shall be able to be started and run in parallel.

**REQ-SIP-002**: Each SIP line shall run in a separate thread with independent state management.

**REQ-SIP-003**: The SIP client shall create unique call_ids starting from 1, incrementing for each new call across all SIP lines. Every additional active call gets `last_highest_known_call_id + 1`.

**REQ-SIP-004**: Before assigning a call_id, the SIP client shall query the inbound-audio-processor via negotiation port with `CHECK_CALL_ID <call_id>`.

**REQ-SIP-005**: If the call_id is already in the known call_id list of inbound-audio-processor, the processor shall respond with `HIGHEST_CALL_ID <max_id>`, and SIP client shall change call_id to `max_id + 1`. This avoids duplicate call_ids in the pipeline.

**REQ-SIP-006**: When a call ends (BYE received or sent), SIP client shall send `CALL_END <call_id>` over the negotiation ports via the Master to every slave. The SIP client will be the Master most of the time itself.

**REQ-SIP-007**: The SIP client shall convert incoming network RTP to the interconnect packet format and send to inbound-audio-processor via traffic ports.

**REQ-SIP-008**: The SIP client shall receive encoded G.711 frames from outbound-audio-processor via traffic ports and transmit as RTP to the network.

**REQ-SIP-009**: All existing UDP-based internal communication shall be removed and replaced with TCP traffic ports. External SIP/RTP to the network remains UDP.

### 4.3 Inbound Audio Processor Requirements

**REQ-IAP-001**: The inbound-audio-processor shall accept RTP packets (via interconnect traffic ports) from SIP client and decode G.711 u-law to PCM.

**REQ-IAP-002**: Decoded audio shall be upsampled from 8kHz to 16kHz using linear interpolation (current approach).

**REQ-IAP-003**: The processor shall maintain separate conversion threads for each active call_id.

**REQ-IAP-004**: If the downstream whisper-service crashes/disappears, the streams inside the inbound-audio-processor shall NOT die. Audio shall be redirected immediately to /dev/null (discarded) while continuing to process incoming audio from SIP client.

**REQ-IAP-005**: On reconnection/restart of whisper-service, audio shall be rerouted again to whisper-service immediately for all active calls.

**REQ-IAP-006**: On receiving `CALL_END <call_id>`, the processor shall close the corresponding audio-conversion thread and free resources.

**REQ-IAP-007**: The processor shall respond to `CHECK_CALL_ID` queries from SIP client with the highest known call_id in its list.

### 4.4 Whisper Service Requirements

**REQ-WHI-001**: The whisper-service shall use whisper.cpp with CoreML models (no change from current).

**REQ-WHI-002**: The service shall convert audio of multiple incoming call_ids simultaneously without cutting words or crippling sentences, in real-time.

**REQ-WHI-003**: The service shall implement refined energy-based VAD using 100ms windows (1600 samples at 16kHz). Focus on speed and correct VAD.

**REQ-WHI-004**: VAD parameters:
- Energy threshold: 0.00005 (tuned for telephony)
- Silence trigger: 8 consecutive 100ms windows (~800ms) after speech detected
- Maximum segment length: 8 seconds (safety limit)

**REQ-WHI-005**: Transcription shall use German language model (`wparams.language = "de"`).

**REQ-WHI-006**: The service shall maintain separate transcription contexts for each call_id.

**REQ-WHI-007**: Transcribed text shall be passed on to llama-service via traffic ports with call_id prefix.

**REQ-WHI-008**: The inbound and outbound interfaces shall be crash-proof: the service shall survive connection problems/crashes of inbound-audio-processor or llama-service and attempt reconnection without dropping calls.

**REQ-WHI-009**: Upon `CALL_END <call_id>`, the corresponding conversion thread is closed and transcription resources are released.

### 4.5 LLaMA Service Requirements

**REQ-LLM-001**: The llama-service shall use llama.cpp with CoreML/Metal/MPS acceleration on Apple Silicon (no change from current).

**REQ-LLM-002**: The service shall use the Llama-3.2-1B-Instruct model with Q8_0 quantization.

**REQ-LLM-003**: Each call_id shall have an independent conversation context using separate KV cache sequence IDs. LLaMA can handle multiple conversations simultaneously.

**REQ-LLM-004**: The German system prompt shall remain:
```
Du bist ein extrem effizienter Telefon-Assistent. Antworte IMMER auf DEUTSCH. 
Deine Antworten sind extrem kurz (max. 15 Worter). Sei hoflich aber komm sofort zum Punkt.
```

**REQ-LLM-005**: The service shall wait patiently until sentences are finished before it talks. Sentence completion is detected by punctuation: `.`, `?`, `!`.

**REQ-LLM-006**: If new user input arrives from whisper (over the sip-line) while generating a response, the service shall stop its answer in mid-sentence and process the new input.

**REQ-LLM-007**: Responses shall be as short and sharp as possible without being impolite. Response generation shall be limited to 64 tokens maximum (enforced for speed).

**REQ-LLM-008**: Generated responses shall be sent with their corresponding call_id to kokoro-service via traffic ports.

**REQ-LLM-009**: The service shall be crash-proof upon disconnection/crash of its inbound (whisper) and outbound (kokoro) partners, with automatic reconnection.

**REQ-LLM-010**: On `CALL_END <call_id>`, the corresponding conversation shall be closed: KV cache cleared for that sequence ID, conversation resources freed.

### 4.6 Kokoro TTS Service Requirements (C++ Conversion)

**REQ-KOK-001**: The kokoro-service shall be rewritten in C++ using libtorch (PyTorch C++ API) or a similar approach.

**REQ-KOK-002**: The service shall use German language models for TTS.

**REQ-KOK-003**: The German model path shall be configurable, with default: `models/kokoro-german/kokoro-german-v1_1-de.pth`.

**REQ-KOK-004**: The service shall load the model at startup and keep it resident in memory.

**REQ-KOK-005**: For Apple Silicon, the service shall use MPS (Metal Performance Shaders) device: `torch::Device(torch::kMPS)`.

**REQ-KOK-006**: The phonemization pipeline shall be ported to C++ if necessary. Options:
- **Option A**: Port the Python phonemization pipeline to C++
- **Option B**: Embed a lightweight phonemizer library (e.g., espeak-ng via C API)
- **Option C**: Pre-compute phoneme mappings for common German words

**Assumption**: Option A (port Python pipeline) unless user specifies otherwise.

**REQ-KOK-007**: Audio synthesis shall output 24kHz float32 PCM (matching current Kokoro output).

**REQ-KOK-008**: The service shall handle multiple simultaneous inputs, converting them into PCM audio with corresponding call_ids in separate threads.

**REQ-KOK-009**: Synthesized PCM audio shall be sent with the corresponding call_id to outbound-audio-processor via traffic ports.

**REQ-KOK-010**: If outbound-audio-processor crashes/disconnects, kokoro shall NOT die. It shall dump the stream to /dev/null and reroute the stream on reconnection of the outbound audio processor. Same resilience for llama-service disconnection.

**REQ-KOK-011**: On `CALL_END <call_id>`, the service shall close its own corresponding conversion threads.

**REQ-KOK-012**: The service shall maintain German voice presets (e.g., `df_eva`, `dm_bernd`) loaded from `.pt` files.

### 4.7 Outbound Audio Processor Requirements

**REQ-OAP-001**: The outbound-audio-processor runs standalone. It shall receive 24kHz float32 PCM audio from kokoro-service via traffic ports.

**REQ-OAP-002**: Audio shall be downsampled from 24kHz to 8kHz (take every 3rd sample, matching current approach).

**REQ-OAP-003**: Downsampled PCM shall be encoded to G.711 u-law and the RTP stream passed on to the SIP client with the corresponding call_id.

**REQ-OAP-004**: Encoded frames shall be packetized into 160-byte chunks and sent to sip-client via traffic ports.

**REQ-OAP-005**: The processor shall use a 20ms high-precision timer for constant-rate frame transmission.

**REQ-OAP-006**: If the buffer is empty, the processor shall send silence frames (0xFF in u-law).

**REQ-OAP-007**: The processor can handle multiple input streams from kokoro with the corresponding call_id. Separate buffer and timing threads shall be maintained for each call_id.

**REQ-OAP-008**: Unix domain socket control interface shall be **removed** entirely.

**REQ-OAP-009**: The outbound-audio-processor is crash-proof. It shall survive disconnection/crash of kokoro or SIP client and attempt reconnection.

**REQ-OAP-010**: On `CALL_END <call_id>` signal, the corresponding conversion thread shall be closed, buffers flushed, and connections closed.

### 4.8 Static Binary Requirements

**REQ-BIN-001**: All services shall be compiled as static binaries that run without any installed prerequisites completely on their own.

**REQ-BIN-002**: Minimum static linking requirements:
- C++ standard library
- pthread library
- Network libraries (socket, etc.)

**REQ-BIN-003**: For whisper.cpp and llama.cpp, libraries shall be statically linked into the respective service binaries.

**REQ-BIN-004**: For libtorch (Kokoro service), the PyTorch C++ libraries shall be statically linked.

**REQ-BIN-005**: CoreML frameworks (for Whisper and LLaMA) may remain dynamically linked as they are Apple system frameworks always present on macOS.

**REQ-BIN-006**: Model files (.gguf, .pth, .pt) shall be loaded at runtime from configurable paths (not embedded in binaries due to size).

**REQ-BIN-007**: Binaries shall be tested on a clean macOS system without Python, pip, or development tools installed.

**REQ-BIN-008**: The build system (CMake) shall support a `STATIC_BUILD=ON` option to enable full static linking.

### 4.9 Performance Requirements

**REQ-PERF-001**: End-to-end latency (audio in -> audio out) shall remain below 1 second for typical conversational exchanges. Concentrate on speed.

**REQ-PERF-002**: Whisper VAD shall detect speech endpoints within 800ms of silence. Concentrate on correct VAD.

**REQ-PERF-003**: LLaMA response generation shall complete within 200ms for typical responses (<=15 words in German).

**REQ-PERF-004**: Kokoro TTS synthesis shall achieve Real-Time Factor (RTF) < 0.3 on Apple M1/M2/M3 hardware.

**REQ-PERF-005**: The interconnection system overhead shall add no more than 10ms latency per service hop.

**REQ-PERF-006**: The system shall support at least 10 concurrent calls without degradation in per-call latency.

**REQ-PERF-007**: The whole pipeline shall enable real-time conversation with LLaMA (or any other LLM connected between Kokoro and Whisper) on multiple SIP lines.

### 4.10 Testing Requirements

**REQ-TEST-001**: Each service shall have standalone unit tests verifying:
- Startup and initialization
- Port binding and negotiation
- Packet parsing and serialization
- Graceful shutdown
- Run tests for every single program on its own first

**REQ-TEST-002**: Integration tests shall verify:
- Master election and slave registration
- Service discovery via Master
- Traffic port connection establishment (all 4 traffic ports per service)
- Call_id propagation across the pipeline
- Call termination and cleanup

**REQ-TEST-003**: Crash resilience tests shall verify:
- Automatic reconnection when a service restarts
- Stream redirection to /dev/null during disconnection
- No dropped calls during reconnection
- Service survives neighbor crashes

**REQ-TEST-004**: Multi-call tests shall verify:
- Multiple simultaneous call_ids flowing through the pipeline
- Correct audio and text routing per call_id
- Independent conversation contexts in LLaMA
- No cross-talk between calls
- Call_id negotiation avoids duplicates

**REQ-TEST-005**: Performance tests shall verify all performance requirements (PERF-001 through PERF-007).

**REQ-TEST-006**: End-to-end pipeline test shall simulate:
- Multiple SIP lines registered
- Concurrent incoming calls
- Real-time conversation with interruptions (barge-in)
- Call termination and cleanup
- Run tests for the whole pipeline after individual service tests pass

## 5. Architecture Decisions

### 5.1 Assumptions

**A1**: The first service to start will obviously not find open 22222 and 33333 ports and declares itself the master. Any service type can become master.

**A2**: Services will be started manually or via a startup script. Future work may include a dedicated orchestrator daemon.

**A3**: All services run on `localhost` (127.0.0.1). No remote connections are supported in this phase.

**A4**: The port increment of 3 is chosen to accommodate base negotiation port + 2 traffic ports (upstream in/out or downstream in/out) within each service's port "slot" on each side.

**A5**: Static linking of libtorch may result in large binaries (>500MB for Kokoro service). This is acceptable for deployment on macOS with sufficient disk space.

**A6**: CoreML acceleration for Whisper and LLaMA is retained as-is. No migration to libtorch is required for those services.

**A7**: The SIP client will typically be the first service started and therefore the Master most of the time.

### 5.2 Open Questions for Clarification

**Q1**: For Kokoro German phonemization, which approach is preferred?
- A) Port Python phonemizer to C++
- B) Use espeak-ng C API
- C) Pre-computed phoneme dictionary

**Q2**: Should the system support master failover (new master election if master crashes), or is manual restart acceptable?

**Q3**: For static linking, should we aim for "fully static" binaries (including libc via musl/alpine-style), or is dynamic linking to system libraries (libc, libpthread) acceptable?

**Q4**: Should conversation state (LLaMA KV cache) be persisted to disk for recovery after crashes, or is losing context on crash acceptable?

**Q5**: Should the packet size negotiation be per-service-type (fixed once) or per-call (dynamic)?

**Q6**: Are there specific German voices required beyond `df_eva` and `dm_bernd`?

**Q7**: Should we provide configuration files for model paths and ports, or keep them as command-line arguments?

## 6. Success Criteria

The redesign is considered successful when:

1. All six services run as static binaries without Python or external runtime dependencies
2. The interconnection system successfully establishes master/slave architecture with automatic service discovery
3. Multiple concurrent SIP calls flow through the entire pipeline with correct call_id routing
4. Services automatically reconnect after crashes without dropping active calls
5. End-to-end latency remains below 1 second
6. German TTS quality matches the current Kokoro Python implementation
7. All unit, integration, crash resilience, and performance tests pass
8. The system runs for 1 hour with 5 concurrent calls without memory leaks or crashes

## 7. Out of Scope (Future Work)

- Distributed deployment across multiple machines
- Dynamic load balancing and service scaling
- Support for additional languages beyond German/English
- Web or REST API interfaces
- Persistent conversation state across service restarts
- Encryption and authentication for IPC
- Systemd/launchd service management integration

## 8. Risks and Mitigations

| Risk | Impact | Mitigation |
|------|--------|------------|
| Libtorch static linking may be complex or unsupported | High | Use dynamic linking for libtorch as fallback; still remove Python dependency |
| German phonemization in C++ may not match Python quality | High | Extensive testing with native German speakers; fallback to espeak-ng |
| Port scanning may cause conflicts with other system services | Medium | Allow configurable base port ranges via environment variables |
| Master crash leaves system orphaned | Medium | Implement master re-election or require manual master restart (document procedure) |
| Static binaries may be too large for deployment | Low | Use compression; document storage requirements (expected <2GB total) |
| CoreML models may not load in statically linked binaries | Medium | Test early; use dynamic linking for CoreML frameworks if needed |

## 9. Timeline Estimate (for Planning Phase)

- **Phase 1**: Interconnection system design and implementation (interconnect.h) -- 3 days
- **Phase 2**: Remove existing IPC from all services, integrate interconnect.h -- 2 days
- **Phase 3**: Kokoro C++ conversion with libtorch and German phonemization -- 5 days
- **Phase 4**: Static binary build system and dependency resolution -- 3 days
- **Phase 5**: Testing (unit, integration, crash resilience, multi-call) -- 4 days
- **Phase 6**: Performance optimization and bug fixes -- 3 days

**Total Estimated Effort**: ~20 working days (4 weeks for a single developer)

## 10. Appendix: Data Flow Diagram

```
[SIP Network]
     | RTP (UDP, external only)
     v
[SIP Client] <-- call_id creation, multi-line support, typically Master
     | traffic ports (TCP, interconnect)
     v
[Inbound Audio Processor] <-- G.711 decode, 8->16kHz, /dev/null on downstream crash
     | traffic ports (TCP, interconnect)
     v
[Whisper Service] <-- VAD, ASR (German), crash-proof both sides
     | traffic ports (TCP, interconnect)
     v
[LLaMA Service] <-- conversational AI (German), interruption support
     | traffic ports (TCP, interconnect)
     v
[Kokoro TTS Service] <-- German TTS (C++/libtorch), /dev/null on downstream crash
     | traffic ports (TCP, interconnect)
     v
[Outbound Audio Processor] <-- 24->8kHz downsample, G.711 encode
     | traffic ports (TCP, interconnect)
     v
[SIP Client]
     | RTP (UDP, external only)
     v
[SIP Network]
```

### Port Layout Example (all 6 services)

```
Service                  NegIn  NegOut  DownIn(NI+1) DownOut(NI+2) UpIn(NO+1) UpOut(NO+2)
-----------------------------------------------------------------------------------------
SIP Client (Master)      22222  33333   22223        22224         33334      33335
Inbound Audio Proc       22225  33336   22226        22227         33337      33338
Whisper Service          22228  33339   22229        22230         33340      33341
LLaMA Service            22231  33342   22232        22233         33343      33344
Kokoro Service           22234  33345   22235        22236         33346      33347
Outbound Audio Proc      22237  33348   22238        22239         33349      33350
```

### Traffic Connection Example (IAP <-> Whisper)

```
IAP UpIn  (33337)  <----connects---->  Whisper DownOut (22230)
IAP UpOut (33338)  <----connects---->  Whisper DownIn  (22229)

Connection initiated by downstream service (Whisper) to upstream service (IAP).
```

All inter-service flows use the new TCP-based interconnection system with master/slave orchestration.

---

**Document Status**: DRAFT v3 -- Corrected traffic port mapping per updated PR #2
**Last Updated**: 2026-02-13
**Author**: AI System Architect
