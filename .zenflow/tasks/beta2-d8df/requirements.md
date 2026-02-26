# Product Requirements Document: Beta-Stage Testing & Optimization

## 1. Overview

WhisperTalk is a real-time speech-to-speech system with a 7-service C++ pipeline: SIP Client -> IAP -> VAD -> Whisper -> LLaMA -> Kokoro -> OAP -> SIP Client (loop). The system includes a web-based frontend (embedded in `frontend.cpp`) that serves as the conductor for all testing, service management, and optimization.

This PRD defines the requirements for **beta-stage testing and optimization** across all 12 stages, performed sequentially through the frontend UI. The primary goals are **performance** (near-real-time latency) and **quality** (100% transcription accuracy), with all testing conducted via the browser-based frontend.

## 2. Test Infrastructure

### 2.1 Test Files
- **Location**: `Testfiles/` directory in project root
- **Contents**: 20 WAV files (`sample_01.wav` through `sample_20.wav`) with matching `.txt` ground-truth files
- **Format**: 16-bit PCM, mono, 44100 Hz
- **Language**: German
- **Duration range**: ~3-7 seconds per file
- **Additional**: `llama_prompts.json` with 10 German prompts for LLaMA quality testing

### 2.2 Frontend as Test Conductor
- All tests MUST be initiated, monitored, and evaluated through the frontend web UI at `http://localhost:8080`
- The frontend already has pages: Tests, Beta Testing, Models, Pipeline Services, Live Logs, Database
- The existing "Beta Testing" page contains: Audio Injection, SIP RTP Test, IAP Quality Test, SIP Lines Management, Whisper Accuracy Test, LLaMA Quality Test, Kokoro Quality Test, Log Level Controls, Test Results
- The existing "Models" page contains: Whisper/LLaMA model registration, benchmarking, and comparison tabs

### 2.3 Logging Requirements
- All services send logs to the frontend logging server (UDP port 22022)
- Log levels (ERROR, WARN, INFO, DEBUG, TRACE) must be configurable per-service from the frontend UI
- The log level configuration UI already exists in the Beta Testing page
- Logging must be crash-proof: log infrastructure failures must not crash services
- Log spam must be avoided through configurable depth

### 2.4 Test SIP Provider
- `test_sip_provider` binary acts as the SIP server/proxy for testing
- Handles SIP registration, call setup (INVITE/BYE), RTP relay between lines
- Supports audio injection: can inject WAV files into active calls as RTP streams
- Frontend communicates with test_sip_provider via its HTTP API (port 22011)

## 3. Stage Requirements

### Stage 1: SIP Client Testing
**Objective**: Validate SIP Client RTP routing and TCP connection handling with IAP.

**Requirements**:
- Run test with only one SIP line (second line disconnected until Stage 11)
- Inject audio via test_sip_provider into the call between line 1 and line 2
- Verify RTP packets are received and discarded by SIP Client when IAP is not running
- Start IAP, verify TCP connection established and packets forwarded
- Stop/Start IAP multiple times to test:
  - TCP connection teardown and re-establishment
  - RTP stream uptake and resumption
  - Master/Slave port management correctness
- All verification via frontend UI: RTP packet metrics table, TCP connection status indicator

**Existing UI**: Beta Testing page has "Test 1: SIP Client RTP Routing" card with start/stop buttons, RTP metrics table (Call ID, Line, RX/TX/Forwarded/Discarded packets, Duration), and TCP connection status

### Stage 2: IAP Optimization
**Objective**: Optimize IAP audio processing for speed and quality.

**Requirements**:
- Processing pipeline: G.711 u-law decode -> 8kHz to 16kHz upsample (15-tap half-band FIR)
- Target latency: 5-15ms per packet (max 50ms), no large buffers
- Investigate audio enhancement (filtering, equalizing) for better speech recognition, only if fast enough
- Measure: latency, SNR, THD for codec quality
- Pass criteria: SNR >= 3dB, THD <= 80%, Latency <= 50ms

**Existing UI**: Beta Testing page has "Test 2: IAP Codec Quality" card with offline codec testing (no IAP service required), file selection, SNR/THD/latency metrics table

### Stage 3: VAD Service Optimization
**Objective**: Optimize VAD for smooth, fast operation and correct interconnection behavior.

**Requirements**:
- Review and optimize VAD logic: energy-based detection, micro-pause detection (400ms silence), chunk constraints (0.5-4s)
- Review interconnection between VAD, IAP, Whisper, and LLaMA (shut-up mechanism: SPEECH_ACTIVE/SPEECH_IDLE signals)
- Optimize for performance and speed
- Current parameters: 50ms analysis frames, 2.0x threshold multiplier, 400ms silence timeout, 4s max chunks

### Stage 4: Whisper Service Accuracy Testing
**Objective**: Achieve excellent transcription accuracy with lightning-fast processing.

**Requirements**:
- Test TCP interconnection handling
- Send all 20 test files through the pipeline (SIP Provider -> SIP Client -> IAP -> VAD -> Whisper)
- Compare Whisper transcription output against ground-truth `.txt` files
- Optimize transcription accuracy + VAD parameters until no words/sentences are crippled
- Measure per-file: accuracy (%), latency (ms), word error rate
- Aggregate metrics: average accuracy, pass/warn/fail counts

**Existing UI**: Beta Testing page has "Whisper Accuracy Test" card with multi-file selection, VAD parameter sliders (Window 50-300ms, Threshold 1.0-4.0), accuracy results table, summary stats

### Stage 5: Credentials UI
**Objective**: Add a new "Credentials" section to the frontend.

**Requirements**:
- New sidebar navigation item under "System" section
- Sub-sections for:
  - HuggingFace: access token, login data entry and storage
  - GitHub: access token entry and storage
- Credentials stored securely in the SQLite database (frontend.db)
- Credentials used for model downloads in Stages 6 and 8

### Stage 6: Whisper Model Search & Benchmarking
**Objective**: Find better Whisper models and compare performance.

**Requirements**:
- Search HuggingFace for better-suited, faster, smaller Whisper models (German-finetuned, CoreML-accelerated)
- Add model download/registration capability to the frontend
- Add separate benchmark test for each model in the frontend
- Enhance the Models page with:
  - Performance comparison diagrams (latency, accuracy, model size)
  - Chart.js-based visualization (already loaded in frontend)
  - Easy side-by-side comparison

**Existing UI**: Models page has Whisper Models tab with model registration, benchmark runner (iterations per file), and Comparison tab with latency chart

### Stage 7: LLaMA Service Testing
**Objective**: Test LLaMA TCP interconnection and optimize for short, clear responses.

**Requirements**:
- Test TCP connection handling with Whisper (upstream) and Kokoro (downstream)
- Test response generation using llama_prompts.json test prompts
- Optimize for short, clear German responses
- Measure: response latency, word count, keyword matching, German language compliance
- Current model: Llama-3.2-1B-Instruct Q8_0

**Existing UI**: Beta Testing page has "Test 4: LLaMA Response Quality" card with prompt selection, custom prompt input, quality test and shut-up test buttons

### Stage 8: LLaMA Model Search & Benchmarking
**Objective**: Find better LLaMA models and compare performance.

**Requirements**:
- Search HuggingFace for better-suited, faster, smaller LLM models (German-finetuned, CoreML/Metal-accelerated)
- Add model registration for LLaMA models
- Add benchmark results to the Models page alongside Whisper benchmarks
- Performance comparison diagrams (latency, quality score, tokens/sec)

**Existing UI**: Models page has LLaMA Models tab with model registration, benchmark runner, and shared Comparison tab

### Stage 9: Shut-Up Mechanism Testing
**Objective**: Validate interrupt/barge-in functionality.

**Requirements**:
- Send speech to initiate a conversation (LLaMA generates response, Kokoro synthesizes)
- Send interrupting speech mid-sentence to test shut-up mechanism
- Verify: SPEECH_ACTIVE signal stops Kokoro playback, LLaMA cancels current generation
- Measure: interrupt latency (time from speech detection to TTS stop)

**Existing UI**: Beta Testing page has "Shut-up Test" button in LLaMA test card

### Stage 10: Kokoro TTS Testing
**Objective**: Test Kokoro for interconnection, quality, and speed.

**Requirements**:
- Test TCP connection handling with LLaMA (upstream) and OAP (downstream)
- Optimize for bug-free, lightning-fast, recognizable, natural German speech synthesis
- Measure: synthesis latency, real-time factor (RTF), audio quality
- Current setup: PyTorch/CoreML, espeak-ng phonemization, 24kHz output

**Existing UI**: Beta Testing page has "Test 5: Kokoro TTS Quality" card with custom phrase input, quality test, benchmark (3/5/10 iterations)

### Stage 11: OAP & Full Loop Testing
**Objective**: Test outbound audio processing and validate full-loop audio fidelity.

**Requirements**:
- Test OAP TCP interconnection and 24kHz->8kHz downsampling + G.711 encoding
- Connect the second SIP line
- Full loop test:
  1. Inject one test file through pipeline via SIP Provider
  2. LLaMA generates response
  3. Kokoro synthesizes response audio
  4. OAP encodes to RTP, sends through SIP Client to line 2
  5. Line 2 receives audio, which gets transcribed by Whisper
  6. Compare: LLaMA's original text response == Whisper's transcription of line 2 audio
- This validates the entire pipeline end-to-end

### Stage 12: Full Pipeline Stress Test
**Objective**: 2-minute continuous operation test to find bottlenecks.

**Requirements**:
- Start full pipeline via frontend
- Run continuous audio injection for 2 minutes
- Monitor: per-service latency, memory usage, CPU usage, packet loss, buffer overflows
- Identify bottlenecks and optimize
- All monitoring via frontend logging and metrics

## 4. Cross-Cutting Requirements

### 4.1 Bug Fixing
- All bugs discovered during testing MUST be fixed before proceeding to the next stage
- Code must be cleaned of old/unnecessary parts as testing progresses
- No step is complete until every test passes and every known issue is resolved

### 4.2 No Stubs or Simulations
- Every function must have a real, working implementation
- No placeholder methods, simulated processes, or hardcoded return values
- If a dependency is not available, the step must not be marked complete

### 4.3 Performance Targets
- IAP: 5-15ms per packet (max 50ms)
- VAD: micro-pause detection ~400ms, chunk size 1.5-4s
- Whisper: near-real-time transcription of 2-4s chunks
- LLaMA: short responses (<25 words typical), fast token generation via Metal
- Kokoro: RTF < 1.0 (faster than real-time), natural German speech
- Full pipeline: end-to-end latency suitable for real-time conversation

### 4.4 Frontend-Driven Testing
- All service start/stop/restart operations via frontend Pipeline Services page
- All test execution via frontend Beta Testing page
- All model management via frontend Models page
- All log monitoring via frontend Live Logs page or embedded service logs
- The test agent interacts with the frontend via browser automation (Playwright), not direct API calls

### 4.5 Interconnection Testing Pattern
Each stage includes TCP interconnection testing:
- Service start/stop resilience (upstream/downstream going offline)
- Auto-reconnection behavior
- Data flow verification (packets sent/received/forwarded/discarded)
- Management message propagation (CALL_END, SPEECH_ACTIVE/SPEECH_IDLE)

## 5. Existing Architecture Summary

### 5.1 Services & Ports
| Service | Base Port | Mgmt | Data | Cmd |
|---------|-----------|------|------|-----|
| SIP Client | 13100 | 13100 | 13101 | 13102 |
| IAP | 13110 | 13110 | 13111 | 13112 |
| VAD | 13115 | 13115 | 13116 | 13117 |
| Whisper | 13120 | 13120 | 13121 | 13122 |
| LLaMA | 13130 | 13130 | 13131 | 13132 |
| Kokoro | 13140 | 13140 | 13141 | 13142 |
| OAP | 13150 | 13150 | 13151 | 13152 |
| Frontend | 13160 | 13160 | 13161 | - |

### 5.2 Frontend
- Single C++ file (`frontend.cpp`, ~6900 lines) embedding HTML/CSS/JS
- Mongoose HTTP server on port 8080
- SQLite database for logs, test results, settings, model data
- SSE (Server-Sent Events) for real-time log streaming
- Chart.js for performance visualization
- Bootstrap 5 for styling

### 5.3 Test SIP Provider
- Standalone binary (`test_sip_provider`, ~1000 lines)
- SIP registration, INVITE/BYE handling
- RTP relay between lines
- Audio injection from WAV files (converts to G.711 u-law RTP)
- HTTP API on port 22011

### 5.4 Interconnect
- `interconnect.h` (~1200 lines) provides peer-to-peer TCP communication
- Fixed port assignments per service
- Packet format: 4-byte call_id + 4-byte payload_size + payload
- Management messages: CALL_END, SPEECH_ACTIVE, SPEECH_IDLE, PING/PONG
- PacketTrace for latency tracking across pipeline hops

## 6. Assumptions & Decisions

- **Single-machine deployment**: All services run on localhost (127.0.0.1), Apple Silicon Mac
- **German language focus**: All test files and optimization targets are for German speech
- **One line for stages 1-10**: Only line 1 is used; line 2 is connected in Stage 11
- **Sequential stages**: Each stage completes before the next begins
- **Credentials storage**: Will use SQLite `settings` table (key-value) with appropriate security considerations (values should not be logged)
- **Model search**: HuggingFace API will be used for model discovery; models must be GGML/GGUF format compatible with whisper.cpp and llama.cpp
