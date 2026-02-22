# Pipeline Test & Bugfix - Product Requirements Document

## 1. Overview

Build a comprehensive, incremental test suite for the WhisperTalk pipeline. Each service is tested in isolation and then integrated one-by-one, starting from the frontend. The SIP provider (`test_sip_provider`) is enhanced to inject real speech audio files (from `Testfiles/`) into calls on demand via the frontend. All bugs discovered during testing are fixed immediately. The primary optimization goal is near-real-time performance.

## 2. Test Data

### 2.1 Testfiles Directory

Location: `Testfiles/` in the project root. Contains 20 German speech samples committed to the repository.

**Source**: Thorsten-Voice dataset (CC0 license), subset `TV-2022.10-Neutral`, recorded with a Rode Podcaster microphone. Downloaded via the HuggingFace datasets API (`Thorsten-Voice/TV-44kHz-Full`). Samples 01-10 (3-5s) were selected for basic VAD and transcription testing. Samples 11-20 (6-7.2s) were selected as the 10 longest available samples for multi-sentence recognition and compound word testing.

**Number normalization**: Whisper outputs digits (e.g. "67") for spoken numbers ("siebenundsechzig"). This is correct and expected — LLaMA understands both forms. Ground truth files MUST contain the spoken form (what the speaker actually says). The scoring script normalizes numbers before comparison. DO NOT change ground truths to digit form.

| File | Duration | Transcription |
|------|----------|---------------|
| sample_01.wav | 4.2s | bei fettiger haut sind diese verstopft, abfallstoffe bleiben stecken. |
| sample_02.wav | 3.6s | ende juli, anfang august könnten entscheidungen fallen. |
| sample_03.wav | 3.1s | erzieher werden schon jetzt, lehrer nach den ferien getestet. |
| sample_04.wav | 3.1s | bei dieser retro-brille ist der rahmen die besonderheit. |
| sample_05.wav | 5.2s | als hauptlieferant gilt die konventionelle landwirtschaft mit phosphorhaltigen düngemitteln. |
| sample_06.wav | 4.9s | auch das kommissionspapier benennt diese technologie als besonders risikoträchtig. |
| sample_07.wav | 3.1s | der mann machte ihm ein zeichen, zu ihm hinüber zu kommen. |
| sample_08.wav | 3.7s | der experte operiert nicht, verordnet aber medikamente. |
| sample_09.wav | 4.5s | münchen bleibt trotz erneuter schwächephasen tabellenführer der fußball-bundesliga. |
| sample_10.wav | 4.5s | wie er im schäbigen stripclub des städtchens seine blutigen klamotten wechselt. |
| sample_11.wav | 7.2s | der siebenundsechzig-jährige erforscht spezifische eiweißstrukturen auf der zelloberfläche, sogenannte peptide. |
| sample_12.wav | 7.1s | im november vor zwei jahren habe ich einen beitrag im mozilla forum veröffentlicht und meine stimmspende angekündigt. |
| sample_13.wav | 6.8s | der neunundzwanzig-jährige tatverdächtige ist nach polizeiangaben vom donnerstag inzwischen in untersuchungshaft. |
| sample_14.wav | 6.6s | teilnehmende werkstätten können auch analyse-instrumente, spezialwerkzeug und reparaturanleitungen beziehen. |
| sample_15.wav | 6.4s | die leopoldina gilt als älteste naturwissenschaftlich-medizinische gelehrtengesellschaft in deutschland. |
| sample_16.wav | 6.3s | bis zweitausenddreiundzwanzig hat sie dazu zunächst dreihundertfünfzig millionen euro zur verfügung. |
| sample_17.wav | 6.3s | erstmals stellte zudem das vereinigte königreich mit zweitausendsiebenundachtzig die meisten einbürgerungen. |
| sample_18.wav | 6.3s | bis ende zweitausendneunzehn wurden hier bereits vierhundertfünfundneunzig millionen euro investiert. |
| sample_19.wav | 6.3s | im landratsamt im oberbayerischen kreis mühldorf ist der neunundvierzig-jährige kein unbekannter. |
| sample_20.wav | 6.2s | beauty-trend — der aufregendste beauty-trend für die festival- und badesaison zweitausendfünfzehn? |

- Format: 44.1kHz, mono, PCM_16, WAV (all files uniform for consistency)
- Each `.wav` has a matching `.txt` with the exact expected transcription

### 2.2 File Format Handling

All current test files are 44.1kHz WAV. The SIP provider must handle resampling from the source sample rate to 8kHz G.711 u-law for RTP injection. The implementation should be robust enough to accept WAV files at other common sample rates (22.05kHz, 16kHz) for future extensibility, but the initial test set is uniformly 44.1kHz. This requires:
- Reading WAV headers to detect source sample rate
- Resampling to 8kHz
- PCM-to-ulaw encoding
- Packetization into 160-sample RTP frames at 20ms intervals
- Proper RTP sequence number, timestamp (incrementing by 160 per frame), and SSRC management (following the pattern already established in `test_sip_provider.cpp`'s `inject_audio` method)

## 3. SIP Provider Enhancement

### 3.1 Audio File Injection

The existing `test_sip_provider` currently only injects a synthetic 400Hz tone. It must be enhanced to:
- Accept a command to inject a specific audio file from `Testfiles/` into a call leg
- Convert the file (any common audio format: WAV at various sample rates) to G.711 u-law RTP packets
- Send at real-time rate (20ms per 160-sample frame) into the specified call leg
- Support injection on demand (triggered from frontend), not just at call start

### 3.2 Frontend Integration

The frontend **Tests tab** must provide a new "Audio File Injection" section below the existing test runner:
- A dropdown listing available files from `Testfiles/` (fetched via `GET /files` on page load and refreshed periodically)
- A "Leg" selector (A or B)
- An "Inject" button that calls `POST /inject` with the selected file and leg
- A status line showing last injection result (success/error, filename, timestamp)
- The existing start/stop controls for the SIP provider remain as-is

### 3.3 Communication: Frontend -> SIP Provider

The SIP provider embeds a **mongoose HTTP server** on port **22011** for control commands. Mongoose is already compiled into the project (`mongoose.c`/`mongoose.h`). Endpoints:

- `POST /inject` — inject a specified audio file into a call leg (body: `{"file": "sample_01.wav", "leg": "a"}`)
- `GET /files` — list available audio files from `Testfiles/`
- `GET /status` — return current call state, relay stats, and injection status

The frontend browser JS calls these endpoints directly. Also testable via `curl`.

CORS: `Access-Control-Allow-Origin: *` on all responses (localhost test tool, any origin acceptable for test flexibility). Handle `OPTIONS` preflight with 204.

Error responses:
- 400 Bad Request: `{"error": "Invalid leg 'x', must be 'a' or 'b'"}` — malformed request
- 404 Not Found: `{"error": "File not found: sample_99.wav"}` — missing file
- 409 Conflict: `{"error": "No active call to inject into"}` — no call established
- 500 Internal Server Error: `{"error": "Failed to load WAV file: <reason>"}` — runtime failure

Success responses:
- `POST /inject` → 200: `{"success": true, "injecting": "sample_01.wav", "leg": "a"}`
- `GET /files` → 200: `{"files": [{"name": "sample_01.wav", "size_bytes": 373130}, ...]}`
- `GET /status` → 200: `{"call_active": true, "relay_stats": {...}, "injecting": "sample_01.wav" | null}`

**Rationale**: The SIP provider is a standalone test tool that simulates external SIP infrastructure — it is not part of the WhisperTalk pipeline interconnect chain and does not use `InterconnectNode`. Mongoose is already in the project and provides a clean, self-contained HTTP API with minimal code.

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
4. **Optimize VAD**: Tune parameters so no words/sentences are crippled. Current baselines from `whisper-service.cpp`:
   - `VAD_FRAME_SIZE` = 1600 (100ms @ 16kHz)
   - `VAD_THRESHOLD_MULT` = 10.0 (10x noise floor)
   - `VAD_MIN_ENERGY` = 0.00003
   - `VAD_SILENCE_FRAMES` = 6 (600ms before end-of-utterance)
   - `VAD_MAX_SPEECH_SAMPLES` = 160000 (10s max)
   - `VAD_CONTEXT_FRAMES` = 2 (200ms pre-speech context)
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

Initial estimates based on Apple Silicon (M-series) capabilities with CoreML/Metal acceleration. These are aspirational starting points to be validated and adjusted during testing — actual achievable targets will be refined as each service is profiled.

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

- **Directory name**: Created `Testfiles/` directory (task description said "Testifies" which appears to be a typo)
- **Transcription vs translation**: `.txt` files contain transcriptions (German audio -> German text), not cross-language translations. The pipeline performs speech-to-speech within the same language (German).
- **Audio format**: All initial test files are 44.1kHz WAV for consistency; the SIP provider is designed to handle various sample rates for future extensibility
- **Control channel**: Embedded mongoose HTTP server on port 22011 for SIP provider injection commands (port chosen to avoid conflicts with SIP 5060, interconnect 22222/33333, pipeline service ports 8083/8090/9001/9002/13000, and frontend 8080)
- **Line management**: Interconnect negotiation protocol extension (user confirmed)
- **Initial test config**: 2 lines (1 active, 1 listening); manual scaling via frontend
- **Language**: All test audio is German; Whisper and LLaMA are configured for German
- **Logging format**: Uses the existing UDP log protocol consumed by the frontend's `log_receiver_loop` (plain text messages on the configured log port)
