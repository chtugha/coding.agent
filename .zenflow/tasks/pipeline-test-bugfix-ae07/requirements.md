# Pipeline Test & Bugfix - Product Requirements Document

## 1. Overview

Build a comprehensive, incremental test suite for the WhisperTalk pipeline. Each service is tested in isolation and then integrated one-by-one, starting from the frontend. The SIP provider (`test_sip_provider`) is enhanced to inject real speech audio files (from `Testfiles/`) into calls on demand via the frontend. All bugs discovered during testing are fixed immediately. The primary optimization goal is near-real-time performance.

## 2. Test Data

### 2.1 Testfiles Directory

Location: `Testfiles/` in the project root. Contains 10 German speech samples from the Thorsten-Voice dataset (CC0, TV-2022.10-Neutral subset, Rode Podcaster microphone):

| File | Duration | Transcription |
|------|----------|---------------|
| sample_01.wav | 4.2s | bei fettiger haut sind diese verstopft, abfallstoffe bleiben stecken. |
| sample_02.wav | 3.6s | ende juli, anfang august könnten entscheidungen fallen. |
| sample_03.wav | 3.1s | erzieher werden schon jetzt, lehrer nach den ferien getestet. |
| sample_04.wav | 3.1s | bei dieser retro-brille ist der rahmen die besonderheit. |
| sample_05.wav | 5.2s | als hauptlieferant gilt die konventionelle landwirtschaft mit phosphorhaltigen düngemitteln. |
| sample_06.wav | 4.9s | auch das kommissionspapier benennt diese technologie als besonders risikoträchtig. |
| sample_07.wav | 3.1s | der mann machte ihm zeichen, zu ihm hinüber zu kommen. |
| sample_08.wav | 3.7s | der experte operiert nicht, verordnet aber medikamente. |
| sample_09.wav | 4.5s | münchen bleibt trotz erneuter schwächephasen tabellenführer der fußball-bundesliga. |
| sample_10.wav | 4.5s | wie er im schäbigen stripclub des städtchens seine blutigen klamotten wechselt. |

- Format: 44.1kHz, mono, PCM_16, WAV
- Each `.wav` has a matching `.txt` with the exact expected transcription

### 2.2 File Format Handling

The SIP provider must convert any WAV file (potentially different sample rates: 44.1kHz, 22.05kHz, 16kHz) to 8kHz G.711 u-law for RTP injection. This requires:
- Resampling to 8kHz
- PCM-to-ulaw encoding
- Packetization into 160-sample RTP frames at 20ms intervals

## 3. SIP Provider Enhancement

### 3.1 Audio File Injection

The existing `test_sip_provider` currently only injects a synthetic 400Hz tone. It must be enhanced to:
- Accept a command to inject a specific audio file from `Testfiles/` into a call leg
- Convert the file (any common audio format: WAV at various sample rates) to G.711 u-law RTP packets
- Send at real-time rate (20ms per 160-sample frame) into the specified call leg
- Support injection on demand (triggered from frontend), not just at call start

### 3.2 Frontend Integration

The frontend must provide:
- A list of available sound files from `Testfiles/`
- A button/control to inject a selected file into an active call
- The ability to start the SIP provider from the Tests tab (already partially exists)

### 3.3 Communication: Frontend -> SIP Provider

The SIP provider needs a control channel to receive injection commands at runtime. **Three options were considered** (user decision pending):

1. **Unix Domain Socket** (`/tmp/test-sip-provider.ctrl`): Text commands like `INJECT <filename> <leg>`. Consistent with OAP's existing `/tmp/outbound-audio-processor.ctrl` pattern.
2. **Embedded HTTP API**: Mongoose-based REST endpoint in the SIP provider.
3. **Interconnect Protocol Extension**: Extend the negotiation channel to carry control commands.

**Recommendation**: Option 1 (Unix socket) for simplicity and consistency.

## 4. SIP Client: Dynamic Line Management

### 4.1 Requirement

The SIP client must accept commands to add and remove phone lines while running. This enables scaling from 1-line tests up to 6+ simultaneous lines without restart.

### 4.2 Chosen Approach: Interconnect Negotiation Protocol Extension

Extend the existing interconnect negotiation channel to carry line management commands. The frontend (already an `InterconnectNode`) sends commands to the SIP client via the master negotiation port:

- `ADD_LINE <user> <server_ip> [password]` — creates a new SIP line, registers it, and returns the assigned line index
- `REMOVE_LINE <index>` — deregisters and tears down the specified line
- `LIST_LINES` — returns current line status

**Rationale**: A new line only needs a LAN IP, user, and password — the overhead is minimal. This keeps all control within the existing interconnect infrastructure, avoids new socket types, and the negotiation channel already has message parsing and request/response semantics.

### 4.3 Frontend Integration

The frontend must provide:
- A control to set the number of lines before starting the SIP client
- Controls to add/remove lines on a running SIP client
- Display of active line status

## 5. Incremental Pipeline Test Flow

Testing proceeds strictly in order. Each stage must pass before the next service is added. The frontend is always the starting point.

### Stage 1: Frontend + SIP Provider + SIP Client

1. Start frontend
2. Start SIP provider from frontend (with audio injection capability)
3. Start SIP client with 2 lines (line 1 active, line 2 listening)
4. Verify SIP registration and call establishment between lines
5. Inject a test audio file from `Testfiles/` into line 1
6. Verify RTP packets flow through the SIP client (dumped because IAP is not running)
7. Test with additional lines (2 more, 4 more) — manually configurable in frontend

### Stage 2: + Inbound Audio Processor (IAP)

1. Inject audio file again
2. Start IAP — verify TCP connection to SIP client establishes
3. Verify G.711 -> float32 PCM conversion and 8kHz -> 16kHz resampling
4. Stop IAP — verify SIP client handles disconnection gracefully (dumps to /dev/null)
5. Restart IAP — verify reconnection and stream resumption
6. Repeat start/stop multiple times to stress-test TCP connection handling and master/slave port management
7. **Optimize**: Evaluate if conversion can be made faster (lookup tables, SIMD, buffer strategies)

### Stage 3: + Whisper Service

1. Verify TCP interconnect between IAP and Whisper
2. Inject test audio through the running pipeline
3. Compare Whisper transcription output against `Testfiles/*.txt` ground truth
4. **Optimize VAD**: Tune parameters so no words/sentences are crippled:
   - VAD_THRESHOLD_MULT, VAD_SILENCE_FRAMES, VAD_CONTEXT_FRAMES
   - Ensure complete sentence capture without premature cutoffs
5. **Optimize transcription**: Model parameters, threading, CoreML utilization
6. Target: excellent German transcription accuracy at lightning speed

### Stage 4: + LLaMA Service

1. Verify TCP interconnect between Whisper and LLaMA
2. Send speech through pipeline and verify LLaMA generates responses
3. **Test shut-up mechanism**: 
   - Send speech to initiate conversation
   - Send more speech while LLaMA is generating — verify generation interrupts
   - Verify SPEECH_ACTIVE/SPEECH_IDLE signals propagate correctly
4. **Optimize**: Short, clear German responses; fast generation; effective interruption

### Stage 5: + Kokoro TTS Service

1. Verify TCP interconnect between LLaMA and Kokoro
2. Verify speech synthesis produces valid float32 PCM audio
3. **Optimize**: Bug-free speech creation, fast synthesis

### Stage 6: + Outbound Audio Processor (OAP)

1. Verify TCP interconnect between Kokoro and OAP
2. Verify 24kHz float32 -> 8kHz G.711 u-law conversion
3. Verify 20ms frame scheduling to SIP client
4. **Full loop test**: Line 2 receives speech generated by Kokoro (from line 1's conversation). Whisper on line 2 transcribes Kokoro's output. Compare Whisper's transcription of line 2 against what LLaMA originally sent to Kokoro. This validates end-to-end TTS quality.
5. **Optimize**: Kokoro speech quality based on round-trip transcription accuracy

## 6. Logging & Observability

All services must log to the frontend via the existing UDP log mechanism:
- Interconnect state changes (connected/disconnected/reconnected)
- Packet counts and latency traces
- Transcription results and LLaMA responses
- Error conditions

The frontend's Live Logs tab must display these in real-time.

## 7. Performance Targets

| Metric | Target |
|--------|--------|
| IAP conversion latency | < 1ms per RTP packet |
| Whisper transcription | < 1s for 3-5s utterances |
| LLaMA response generation | < 500ms for short responses |
| Kokoro TTS synthesis | < 2s for short sentences |
| End-to-end latency (speech in -> speech out) | < 5s |
| RTP scheduling jitter | < 2ms |

## 8. Bug Fix Policy

- All bugs discovered during testing are fixed immediately before proceeding
- No stubs, simulations, or placeholder implementations
- Code cleanup: remove dead code and unnecessary parts as they are discovered
- Every fix must be verified by re-running the relevant test stage

## 9. Assumptions & Decisions

- **Audio format**: Test files are 44.1kHz WAV; the SIP provider handles resampling to 8kHz
- **Control channel**: Unix domain socket for SIP provider injection commands — consistent with OAP pattern
- **Line management**: Interconnect negotiation protocol extension (user confirmed)
- **Initial test config**: 2 lines (1 active, 1 listening); manual scaling via frontend
- **Language**: All test audio is German; Whisper and LLaMA are configured for German
