# Product Requirements Document: WhisperTalk Real-Time Optimization

## Executive Summary

WhisperTalk is a real-time speech-to-speech communication system optimized for low-latency telephony conversations. This document outlines requirements for optimizing the system to handle multiple concurrent calls with crash-proof inter-service communication, proper call lifecycle management, and sub-second response latency.

## Current State Analysis

### Architecture
The system consists of 6 microservices in a linear pipeline:

1. **SIP Client** (`sip-client-main.cpp`) - SIP/RTP gateway
2. **Inbound Audio Processor** (`inbound-audio-processor.cpp`) - G.711 → PCM conversion
3. **Whisper Service** (`whisper-service.cpp`) - ASR using CoreML
4. **LLaMA Service** (`llama-service.cpp`) - LLM conversation using Metal
5. **Kokoro Service** (`kokoro_service.py`) - German TTS using MPS
6. **Outbound Audio Processor** (`outbound-audio-processor.cpp`) - PCM → G.711 conversion

### Communication Patterns
- **SIP Client → Inbound Processor**: UDP (port 9001), RTP with 4-byte call_id prefix
- **Inbound → Whisper**: TCP (ports 13000+call_id), float32 PCM stream
- **Whisper → LLaMA**: TCP (port 8083), text with call_id prefix
- **LLaMA → Kokoro**: TCP (port 8090), text with call_id prefix
- **Kokoro → Outbound**: TCP (ports 8090+call_id), float32 PCM stream
- **Outbound → SIP Client**: UDP (port 9002), G.711 with call_id prefix

### Identified Gaps
1. No multi-instance SIP client support
2. Incomplete call lifecycle signaling (no explicit call-end propagation)
3. Partial crash resilience (some services dump to /dev/null, others don't)
4. VAD tuning needed for German speech patterns
5. LLaMA doesn't support interruption on new input
6. Missing comprehensive test suite
7. No performance monitoring/metrics

---

## Requirements

### 1. Multi-Instance SIP Client Support

**REQ-1.1: Multiple SIP Clients**
- **Description**: Support multiple independent SIP client instances connecting to the same inbound audio processor
- **Rationale**: Enable horizontal scaling and redundancy for call handling
- **Acceptance Criteria**:
  - Multiple sip-client processes can run simultaneously
  - Each can register with different SIP servers/accounts
  - Call IDs remain unique across all instances
  - No port conflicts between instances

**REQ-1.2: Call ID Coordination**
- **Description**: Implement collision-free call ID generation across multiple SIP clients
- **Rationale**: Prevent call mixing/conflicts in downstream services
- **Acceptance Criteria**:
  - Call IDs are globally unique
  - ID space supports at least 1000 concurrent calls
  - Simple modulo-based port mapping (e.g., whisper port = 13000 + (call_id % 100))

---

### 2. Call Lifecycle Management

**REQ-2.1: Call End Signal Propagation**
- **Description**: Implement explicit "CALL_END:call_id" signaling through entire pipeline
- **Rationale**: Clean resource cleanup and proper conversation termination
- **Signal Flow**:
  ```
  SIP Client (BYE received)
    → Inbound Audio Processor (close stream, forward signal)
      → Whisper Service (close transcription, forward signal)
        → LLaMA Service (clear conversation, forward signal)
          → Kokoro Service (stop synthesis, forward signal)
            → Outbound Audio Processor (close stream)
  ```
- **Acceptance Criteria**:
  - BYE message triggers cleanup in all services within 500ms
  - Each service closes its call-specific thread/socket
  - No resource leaks after call termination
  - Graceful handling if downstream service is unavailable

**REQ-2.2: Call Start Signal**
- **Description**: SIP Client sends "CALL_START:call_id" to prepare downstream services
- **Rationale**: Pre-allocate resources and establish connections before audio flows
- **Acceptance Criteria**:
  - Signal sent immediately after INVITE is accepted
  - Outbound processor activates TCP listener before audio arrives
  - Services log call initialization with timestamp

---

### 3. Crash-Proof Inter-Service Communication

**REQ-3.1: Inbound Audio Processor Resilience**
- **Description**: Continue processing audio even when Whisper service is unavailable
- **Behavior**:
  - Audio decoding continues regardless of downstream state
  - If Whisper disconnected: dump PCM to /dev/null (no accumulation)
  - Attempt reconnection every 2 seconds
  - On reconnection: resume normal streaming (no audio backlog)
- **Acceptance Criteria**:
  - No crashes when Whisper service stops/restarts
  - Memory usage remains stable (<100MB per call)
  - Reconnection succeeds within 3 seconds of Whisper restart
  - Test: Kill Whisper during active call, restart after 10s, verify audio resumes

**REQ-3.2: Whisper Service Resilience**
- **Description**: Handle disconnections from both inbound and LLaMA services gracefully
- **Behavior**:
  - Accept new inbound connections on per-call ports (13000+call_id)
  - If LLaMA unavailable: buffer transcriptions (max 10 messages) or discard oldest
  - Log connection status changes
- **Acceptance Criteria**:
  - No crashes when LLaMA service stops/restarts
  - Transcription continues even if LLaMA is down
  - Test: Kill LLaMA during transcription, verify Whisper continues processing

**REQ-3.3: LLaMA Service Resilience**
- **Description**: Handle disconnections from Whisper and Kokoro services
- **Behavior**:
  - Accept connections on port 8083 with timeout
  - If Kokoro unavailable: discard responses (no buffering)
  - Maintain conversation state in KV cache regardless of connection status
- **Acceptance Criteria**:
  - No crashes when Kokoro stops/restarts
  - Conversation context preserved across Kokoro restarts
  - Test: Kill Kokoro, send 5 messages to LLaMA, restart Kokoro, verify next response includes context

**REQ-3.4: Kokoro Service Resilience**
- **Description**: Handle disconnections from LLaMA and outbound processor
- **Behavior**:
  - If outbound processor unavailable: dump audio to /dev/null
  - Attempt reconnection every 2 seconds
  - Synthesis continues regardless of downstream state
- **Acceptance Criteria**:
  - No crashes when outbound processor stops/restarts
  - Memory usage stable (no audio accumulation)
  - Test: Kill outbound processor, send TTS requests, verify Kokoro continues

**REQ-3.5: Outbound Audio Processor Resilience**
- **Description**: Handle Kokoro disconnections and SIP client unavailability
- **Behavior**:
  - If Kokoro disconnects: close stream, clean up buffer
  - If SIP client UDP fails: continue processing (UDP is fire-and-forget)
  - Accept new Kokoro connections on per-call ports (8090+call_id)
- **Acceptance Criteria**:
  - No crashes when Kokoro or SIP client fails
  - 20ms RTP scheduling maintains timing even without new audio

---

### 4. Voice Activity Detection (VAD) Optimization

**REQ-4.1: German Speech Optimization**
- **Description**: Refine VAD to accurately segment German speech without cutting sentences
- **Current Issue**: 100ms windows may cut compound words or multi-clause sentences
- **Requirements**:
  - Energy threshold tuned for 8kHz telephony audio (current: 0.00005)
  - Silence detection: 800ms minimum before segmenting (current: 800ms is good)
  - Maximum segment length: 8 seconds (safety limit)
  - No mid-word cuts (detect prosodic boundaries)
- **Acceptance Criteria**:
  - German test sentences transcribed without mid-sentence breaks
  - Latency: Start-of-speech to Whisper processing < 1 second
  - Test corpus: 50 German sentences with various lengths (5-30 words)

**REQ-4.2: VAD Metrics**
- **Description**: Log VAD performance metrics for tuning
- **Metrics**:
  - Speech segments detected per call
  - Average segment length
  - False positives (silence classified as speech)
  - False negatives (speech classified as silence)
- **Acceptance Criteria**:
  - Metrics logged per call with call_id
  - Optional file export for analysis

---

### 5. LLaMA Conversation Management

**REQ-5.1: Sentence Completion Detection**
- **Description**: LLaMA waits for complete sentences before responding
- **Rationale**: Avoid answering mid-sentence or incomplete questions
- **Implementation**:
  - Detect sentence endings: `.`, `?`, `!`, or 2+ seconds of silence
  - Buffer partial sentences until complete
  - Maximum wait time: 5 seconds before forced response
- **Acceptance Criteria**:
  - No responses to incomplete sentences in test scenarios
  - Test: Send "Wie ist" followed 1s later by "das Wetter?" - verify single response

**REQ-5.2: Response Interruption**
- **Description**: Stop LLaMA generation when new user input arrives
- **Rationale**: Natural conversation allows interruptions
- **Implementation**:
  - LLaMA monitors for new messages during generation
  - On new input: stop token generation immediately
  - Clear partial response from TTS pipeline
  - Start processing new input
- **Acceptance Criteria**:
  - Response stops within 200ms of new input
  - Test: Start long response (>30 tokens), interrupt with new input, verify switch < 200ms

**REQ-5.3: Concise Responses**
- **Description**: Generate short, sharp responses (max 15 words)
- **Current State**: System prompt already enforces this
- **Acceptance Criteria**:
  - 95% of responses under 15 words
  - No rambling or verbose answers
  - Test corpus: 100 common questions, verify response lengths

---

### 6. Real-Time Performance

**REQ-6.1: End-to-End Latency**
- **Description**: Minimize total latency from speech input to audio output
- **Targets**:
  - VAD detection: < 100ms
  - Whisper transcription: < 500ms (for 3s audio segment)
  - LLaMA generation: < 300ms (for 15-word response)
  - Kokoro synthesis: < 200ms (for 15 words)
  - Total pipeline: < 1.5 seconds from end-of-speech to start-of-response
- **Acceptance Criteria**:
  - Measure latency per call with timestamps at each stage
  - 90th percentile meets targets
  - Test: 50 conversation turns, log all latencies

**REQ-6.2: Apple Silicon Optimization**
- **Description**: Leverage Metal/MPS/CoreML acceleration
- **Current State**: Already implemented for all AI services
- **Verification**:
  - Whisper uses CoreML backend
  - LLaMA uses Metal acceleration
  - Kokoro uses MPS (Metal Performance Shaders)
- **Acceptance Criteria**:
  - Confirm GPU utilization during inference (Activity Monitor)
  - Compare CPU-only vs GPU-accelerated (should be 5-10x faster)

---

### 7. Testing Requirements

**REQ-7.1: Standalone Service Tests**
- **Description**: Each service must be testable independently
- **Required Tests**:
  - **SIP Client**: Simulated INVITE/BYE, verify RTP forwarding
  - **Inbound Processor**: Send mock RTP, verify PCM output quality
  - **Whisper Service**: Feed PCM audio file, verify transcription accuracy
  - **LLaMA Service**: Send text input, verify response quality and latency
  - **Kokoro Service**: Send text, verify audio output quality
  - **Outbound Processor**: Send PCM, verify G.711 output and timing
- **Acceptance Criteria**:
  - Each test runs in isolation (no dependencies on other services)
  - Test scripts in `tests/` directory with clear README
  - All tests pass before integration testing

**REQ-7.2: Pipeline Integration Tests**
- **Description**: Test complete end-to-end pipeline
- **Test Scenarios**:
  1. Single call: INVITE → conversation → BYE
  2. Multiple concurrent calls (10 simultaneous)
  3. Service crash recovery (kill/restart each service during call)
  4. Long conversation (20+ turns, verify memory stability)
  5. Rapid call churn (100 calls in 5 minutes)
- **Acceptance Criteria**:
  - All scenarios complete successfully
  - No memory leaks (stable RSS over time)
  - Latency targets met in all scenarios
  - Test harness with automated pass/fail

**REQ-7.3: VAD Test Suite**
- **Description**: Dedicated tests for Voice Activity Detection accuracy
- **Test Data**:
  - 50 German sentences (clean audio)
  - 20 sentences with background noise
  - 10 sentences with overlapping speech
  - 10 silent segments (false positive test)
- **Acceptance Criteria**:
  - >95% correct segmentation on clean audio
  - >90% correct segmentation with noise
  - <5% false positives on silence

---

## Non-Functional Requirements

### NFR-1: Reliability
- Services must auto-recover from partner crashes within 3 seconds
- No data loss on service restart (conversation state persisted in LLaMA)
- Uptime target: 99.9% (excluding planned maintenance)

### NFR-2: Scalability
- Support 100 concurrent calls on M1 Max (32GB RAM)
- Linear scaling with core count for parallel processing
- Graceful degradation under overload (queue calls, don't crash)

### NFR-3: Maintainability
- Each service in single file (current architecture)
- Clear logging with call_id context
- Documentation updated when changing inter-service protocols

### NFR-4: Performance Monitoring
- Optional metrics collection (latency, throughput, errors)
- Metrics exportable to file or stdout
- Minimal overhead (<1% CPU)

---

## Out of Scope

The following are explicitly **not** part of this optimization:
- Database integration (stateless services by design)
- Service discovery (fixed ports, localhost only)
- Authentication/encryption (trusted local network)
- Multi-language support beyond German/English
- WebRTC or other transport protocols
- GUI or web interface

---

## Success Criteria

The optimization is complete when:
1. ✅ Multiple SIP client instances operate simultaneously without conflicts
2. ✅ Call lifecycle signals propagate through entire pipeline
3. ✅ All services survive partner crashes and auto-reconnect
4. ✅ VAD accurately segments German speech (>95% accuracy)
5. ✅ LLaMA interrupts responses on new input (<200ms)
6. ✅ End-to-end latency < 1.5 seconds (90th percentile)
7. ✅ All standalone and integration tests pass
8. ✅ 100 concurrent calls supported with stable performance

---

## Assumptions and Constraints

### Assumptions
- Services run on macOS with Apple Silicon (M1/M2/M3)
- Local network (127.0.0.1) with negligible latency
- German language is primary focus (English secondary)
- SIP server is external and reliable

### Constraints
- Must maintain current architecture (6 services, linear pipeline)
- No external dependencies beyond existing libraries
- Services remain standalone executables
- CoreML/Metal/MPS acceleration required (no CPU-only fallback for prod)

---

## Dependencies

### External
- whisper.cpp (CoreML backend)
- llama.cpp (Metal backend)
- Kokoro TTS (Python with PyTorch MPS)
- SIP infrastructure (external SIP server)

### Internal
- Build system: CMake 3.22+
- Compiler: Clang with C++17 support
- Python: 3.9+ for Kokoro service

---

## Glossary

- **VAD**: Voice Activity Detection - algorithm to detect speech vs silence
- **RTF**: Real-Time Factor - ratio of processing time to audio duration (lower is better)
- **CoreML**: Apple's machine learning framework with hardware acceleration
- **Metal**: Apple's GPU programming framework
- **MPS**: Metal Performance Shaders - optimized ML operations for Apple GPUs
- **G.711**: ITU telephony audio codec (μ-law variant used)
- **RTP**: Real-time Transport Protocol for audio streaming
- **SIP**: Session Initiation Protocol for call signaling

---

## Document History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-02-09 | Zencoder | Initial PRD based on task requirements and codebase analysis |
