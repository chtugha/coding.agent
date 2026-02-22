# Full SDD workflow

## Configuration
- **Artifacts Path**: {@artifacts_path} → `.zenflow/tasks/{task_id}`

---

## Build Rules

### whisper-cpp (submodule at `whisper-cpp/`)
```
cd whisper-cpp
cmake -G Ninja -B build \
  -DCMAKE_BUILD_TYPE=Release \
  -DWHISPER_COREML=1 \
  -DWHISPER_COREML_ALLOW_FALLBACK=ON \
  -DWHISPER_CCACHE=OFF \
  -DBUILD_SHARED_LIBS=OFF
cmake --build build --config Release -j$(sysctl -n hw.ncpu)
```
- **MUST** use Ninja (`-G Ninja`)
- **MUST** disable ccache (`-DWHISPER_CCACHE=OFF`)
- **MUST** build static libs (`-DBUILD_SHARED_LIBS=OFF`) — project CMakeLists.txt links `.a` files
- **MUST** enable CoreML with fallback (`-DWHISPER_COREML=1 -DWHISPER_COREML_ALLOW_FALLBACK=ON`) — allows Metal-only operation when CoreML encoder is absent

### Whisper Model Selection
- **USE**: `models/ggml-large-v3.bin` (unquantized, 2.9GB, n_text_layer=32) with **CoreML encoder** (`models/ggml-large-v3-encoder.mlmodelc`)
- **CRITICAL**: The model file MUST be the REAL large-v3 (2.9GB, n_text_layer=32), NOT large-v3-turbo (1.5GB, n_text_layer=4). Verify with: model reports "MTL0 total size = 3094.36 MB" in logs. If it says ~1624 MB, you have the WRONG model (turbo)!
- **whisper-cpp version**: MUST be 1.8.3+ (updated from 1.7.6 which had CoreML precision issues)
- **CoreML performance**: ~1200ms avg inference (after warmup), 12/20 PASS, 7/20 WARN (>90%), 1 FAIL
- **Previous CoreML failures were caused by**: (1) wrong model file (large-v3-turbo symlinked as large-v3), (2) old whisper-cpp 1.7.6 with different Metal memory layout
- **Number handling**: Whisper converts spoken German numbers to digits (e.g., "siebenundsechzig" → "67", "zweitausenddreiundzwanzig" → "2023"). This is ACCEPTABLE — LLaMA can handle digits. Ground truth files MUST use digit forms. Do NOT "fix" this by changing ground truths back to spelled-out forms.
- **Default args**: `--language de models/ggml-large-v3.bin`

### CoreML Model Conversion
- **MUST** use conda env `py312-whisper` with **torch==2.7.0** (coremltools 9.0 only tested up to torch 2.7)
- **DO NOT** use system Python (has torch 2.10+ which is incompatible with coremltools)
- Conda Python path: `/opt/homebrew/Caskroom/miniconda/base/envs/py312-whisper/bin/python3`
- Use `--optimize-ane True` (works correctly with whisper-cpp 1.8.3+ and real large-v3 model)
- Conversion command: `$CONDA_PYTHON models/convert-whisper-to-coreml.py --model large-v3 --encoder-only True --optimize-ane True`
- Then compile: `xcrun coremlc compile models/coreml-encoder-large-v3.mlpackage models/`
- Rename: `mv models/coreml-encoder-large-v3.mlmodelc models/ggml-large-v3-encoder.mlmodelc`

### Project binaries
```
cd /path/to/project
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target <target> -j$(sysctl -n hw.ncpu)
```

### Process Management Rules
1. ALWAYS check that only ONE frontend instance is running before tests
2. ALL services MUST be started/stopped via the frontend API (`curl http://127.0.0.1:8080/api/services/start|stop`)
3. Before starting any service, kill ghost processes of the same binary first
4. Verify with `lsof -nP -iTCP -sTCP:LISTEN` that each service has exactly one instance

---

## Workflow Steps

### [x] Step: Requirements
<!-- chat-id: 3fa7ea69-675b-44fa-bd5d-5465ad8b110a -->

Create a Product Requirements Document (PRD) based on the feature description.

1. Review existing codebase to understand current architecture and patterns
2. Analyze the feature definition and identify unclear aspects
3. Ask the user for clarifications on aspects that significantly impact scope or user experience
4. Make reasonable decisions for minor details based on context and conventions
5. If user can't clarify, make a decision, state the assumption, and continue

Save the PRD to `{@artifacts_path}/requirements.md`.

### [x] Step: Technical Specification

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

Detailed implementation plan created below, replacing the generic Implementation step with 9 concrete phases.

---

### [x] Step: SIP Provider Enhancement — Mongoose HTTP + WAV Loading + RTP Injection

**Files**: `tests/test_sip_provider.cpp`, `CMakeLists.txt`

**Scope**: Transform the test SIP provider from a fixed-duration tone generator into an HTTP-controlled audio file injector.

1. **CMake**: Modify `test_sip_provider` target to link `mongoose.c`, add `MG_ENABLE_PACKED_FS=0` definition (lines 334-336 in CMakeLists.txt)
2. **Mongoose embedding**: Add `mg_mgr` member to `TestSipProvider`, init in `init()`, poll in `run()` loop with 1ms timeout (reduce existing SIP `recvfrom` from 1s to 1ms). Add `--testfiles-dir` CLI argument (default `Testfiles/`)
3. **HTTP endpoints**: Implement `GET /files` (scan Testfiles dir, return JSON), `POST /inject` (parse `{"file","leg"}`, trigger injection), `GET /status` (call state + relay stats). Add CORS `Access-Control-Allow-Origin: *` to all responses. Handle OPTIONS preflight with 204. Error responses: 400/404/409/500 with `{"error":"..."}`
4. **WAV parser**: Implement `load_wav_file(path)` → `{vector<int16_t>, sample_rate, channels}`. Parse RIFF header, fmt chunk (PCM_16/PCM_24/FLOAT32), data chunk. Stereo→mono downmix. 50MB safety limit. Reject compressed formats
5. **Resampler**: Implement `resample_to_8khz(samples, source_rate)` with 15-tap windowed-sinc FIR low-pass filter (3.4kHz cutoff, Hamming window). Special cases: 8kHz passthrough, 16kHz simple 2:1 decimation
6. **RTP injection refactor**: Replace existing `inject_audio` (400Hz tone) with general `inject_rtp_stream(call, leg, ulaw_samples)`. Add `inject_file(filename, leg)` method that loads WAV → resamples → encodes u-law → starts injection thread. Use joinable `std::thread` with `atomic<bool> injecting_` per call for cancellation. New injection cancels in-progress one (flag + join)
7. **Remove duration-based termination**: The provider should run until SIGTERM (not auto-exit after N seconds). Remove `end_thread_` and `duration_` timeout logic. Keep `--duration` as optional idle timeout only

- **Verify**: `cmake -B build -DBUILD_TESTS=ON && cmake --build build -j$(sysctl -n hw.ncpu)`. Start provider, `curl http://localhost:22011/files` returns JSON. `curl -X POST -d '{"file":"sample_01.wav","leg":"a"}' http://localhost:22011/inject` returns success (even without active call → 409)

### [x] Step: SIP Client Dynamic Line Management via Interconnect Extension

**Files**: `interconnect.h`, `sip-client-main.cpp`

**Scope**: Add custom negotiation handler mechanism to interconnect and implement ADD_LINE/REMOVE_LINE/LIST_LINES in SIP client.

1. **interconnect.h**: Add `custom_negotiation_handler_` member (`std::function<std::string(const std::string&)>`) and `register_custom_negotiation_handler()` method. In `handle_negotiation_message()`, at the end (just before the `if (!response.empty())` send block at line 1127), add fallback: `if (response.empty() && custom_negotiation_handler_) response = custom_negotiation_handler_(msg);`
2. **sip-client-main.cpp — add_line()**: New method `add_line(user, server_ip, password) → int`. Creates `SipLine`, assigns next index, binds UDP socket, starts `sip_loop` and `registration_loop` threads. Protected by `lines_mutex_`
3. **sip-client-main.cpp — remove_line()**: New method `remove_line(index) → bool`. Signals line threads to stop (need per-line `atomic<bool> running` flag on `SipLine`), joins threads, closes socket, removes from `lines_`. Cleans up associated calls
4. **sip-client-main.cpp — list_lines()**: Returns formatted string of all lines with index, user, registration status
5. **Register custom handler**: In `init()`, after `interconnect_.initialize()`, register handler that parses `ADD_LINE <user> <server_ip> [password]`, `REMOVE_LINE <index>`, `LIST_LINES` and delegates to the above methods
6. **SipLine per-line stop flag**: Add `std::atomic<bool> line_running{true}` to `SipLine`. Modify `sip_loop` and `registration_loop` to check `line->line_running` instead of the global `running_`

- **Verify**: Build. Start SIP client with `--lines 1`. Use `nc` or test script to connect to SIP client's negotiation port and send `ADD_LINE alice2 127.0.0.1`. Verify `LINE_ADDED` response. Send `LIST_LINES`, verify output. Send `REMOVE_LINE <idx>`, verify removal

### [x] Step: Frontend UI — Audio Injection + Line Management

**Files**: `frontend.cpp`

**Scope**: Add audio file injection controls and SIP line management to the frontend Tests tab.

1. **Audio Injection HTML**: Add section below `testsContainer` div (in `serve_index` HTML): file dropdown (`select#injectFile`), leg selector (`select#injectLeg`), inject button, status span
2. **Audio Injection JS**: `fetchInjectFiles()` — `GET http://localhost:22011/files`, populates dropdown. `injectAudio()` — `POST http://localhost:22011/inject` with `{file, leg}`. Both called directly to SIP provider port 22011 (no proxy needed). Add periodic refresh via `setInterval`
3. **Line Management HTML**: Input fields (user, server IP, password), "Add Line" button, lines status table with "Remove" buttons per line
4. **Line Management JS + API endpoints**: Add `POST /api/sip/add-line`, `POST /api/sip/remove-line`, `GET /api/sip/lines` to `http_handler`. These proxy to SIP client via interconnect negotiation
5. **send_negotiation_command() helper**: New method on `FrontendServer` — looks up target service's `neg_in` port from `query_service_ports()` (if master) or synced registry (if slave), opens TCP connection, sends command, reads response with 2s timeout, closes socket, returns response string
6. **Fix handle_test_start JSON parsing**: Current code uses `mg_http_get_var` (form-encoded) but frontend sends JSON. Add JSON body parsing for test start/stop to match frontend's `JSON.stringify` calls

- **Verify**: Build. Start frontend, open `http://localhost:8080`. Tests tab shows injection controls. Start SIP provider from frontend, verify file dropdown populates. Line management section visible

### [x] Step: Stage 1 Testing — Frontend + SIP Provider + SIP Client Integration

**Scope**: End-to-end test of the first 3 components. Fix all bugs discovered.

1. Start frontend from terminal
2. Start SIP provider from frontend Tests tab
3. Start SIP client with 2 lines (from terminal: `bin/sip-client alice 127.0.0.1 5060 --lines 2`)
4. Verify both lines register with SIP provider (observe REGISTER logs)
5. Verify SIP provider initiates call between the 2 lines (INVITE/200 OK/ACK flow)
6. Inject `sample_01.wav` via frontend injection UI into leg A
7. Verify RTP packets flow through SIP client (dumped because IAP not running — observe SIP client logs)
8. Verify injection completes and status updates in frontend
9. Test line management: add a 3rd line via frontend, verify `LIST_LINES`, remove it
10. Fix all bugs encountered. Clean dead code. Ensure stable operation
11. Test with `--lines 4` and `--lines 6` (manual frontend config)

- **Verify**: All of above pass. No crashes, no hangs. RTP flow confirmed via relay stats in SIP provider

### [x] Step: Stage 2 Testing — Inbound Audio Processor (IAP) Integration

**Scope**: Add IAP to running pipeline. Test TCP resilience, optimize conversion.

1. With Frontend + SIP Provider + SIP Client already running, start IAP (`bin/inbound-audio-processor`)
2. Inject audio file again. Verify IAP receives RTP via interconnect, decodes G.711→float32, upsamples 8→16kHz
3. Verify IAP sends float32 PCM downstream (to Whisper port, which won't be running — verify dump behavior)
4. Stop IAP. Verify SIP client handles disconnection gracefully (logs "dumping" messages)
5. Restart IAP. Verify reconnection and stream resumption
6. Repeat start/stop 5+ times rapidly to stress-test TCP connection handling and master/slave port management
7. **Optimize IAP**: Review `processing_loop` in `inbound-audio-processor.cpp`. The current ulaw LUT is good. Check if buffer chunking (1600 sample threshold) causes latency. Consider sending smaller chunks more frequently. Measure conversion latency per RTP packet
8. Fix all bugs. Clean unnecessary code

- **Verify**: IAP start/stop cycles work reliably. No socket leaks. Conversion latency < 1ms per packet. Pipeline stable with IAP running

### [x] Step: Stage 3 Testing — Whisper Service Integration + Transcription Optimization

**Scope**: Connect Whisper to running pipeline. Optimize VAD and transcription accuracy.

1. Start Whisper service (`bin/whisper-service`)
2. Verify TCP interconnect between IAP and Whisper establishes
3. Inject `sample_01.wav` through pipeline
4. Capture Whisper transcription output from logs
5. Compare against `Testfiles/sample_01.txt` using normalized string comparison:
   - Lowercase both strings
   - Strip whitespace, remove punctuation `.,;:!?-—`
   - Collapse multiple spaces
   - Normalize numbers: Whisper outputs digits (e.g. "67") for spoken numbers ("siebenundsechzig"). This is correct and expected — LLaMA understands both forms. Ground truth files MUST contain the spoken form (what the speaker actually says). The scoring script MUST normalize numbers before comparison so both forms are treated as equivalent. DO NOT change ground truths to digit form just because Whisper outputs digits.
   - PASS: 100% match, WARN: ≥90% similarity, FAIL: <90%
6. Run all 20 test files through pipeline, record results
7. **Tune VAD parameters** if sentences are crippled:
   - `VAD_THRESHOLD_MULT` (currently 10.0) — lower if speech not detected
   - `VAD_SILENCE_FRAMES` (currently 6/600ms) — increase if sentences cut short
   - `VAD_CONTEXT_FRAMES` (currently 2/200ms) — increase if word beginnings lost
   - `VAD_MIN_ENERGY` (currently 0.00003) — adjust for RTP audio levels
8. **Optimize transcription speed**: Check `wparams.n_threads` (currently 4), ensure CoreML/GPU is active, measure inference time vs audio duration
9. Fix all bugs. Target: all 10 samples PASS transcription accuracy

- **Verify**: All 10 test files produce PASS or WARN transcription accuracy. Whisper inference < 1s for 3-5s utterances. TCP reconnection works after Whisper restart

### [ ] Step: Stage 4 Testing — LLaMA Service Integration + Shut-up Mechanism

**Scope**: Connect LLaMA to running pipeline. Test response quality and interruption.

1. Start LLaMA service (`bin/llama-service`)
2. Verify TCP interconnect between Whisper and LLaMA establishes
3. Inject speech through pipeline, verify LLaMA generates German response
4. Check response quality: short, clear, German. Adjust system prompt if needed (currently in `llama-service.cpp:185`)
5. **Test shut-up mechanism**:
   - Inject `sample_01.wav` to start a conversation
   - While LLaMA is generating, inject `sample_02.wav`
   - Verify `SPEECH_ACTIVE` signal interrupts LLaMA generation (observe "Speech detected — interrupting generation" log)
   - Verify `SPEECH_IDLE` triggers new response generation
6. Verify LLaMA sends response text downstream to Kokoro port (even if Kokoro not running — dump behavior)
7. **Optimize**: Check generation speed (MAX_TOKENS=48 currently). Ensure Metal/MPS acceleration active. Target < 500ms for short responses
8. Fix all bugs. Ensure reliable interruption and context management

- **Verify**: LLaMA generates coherent German responses. Shut-up mechanism works reliably. Generation time < 500ms. TCP reconnection works

### [ ] Step: Stage 5 Testing — Kokoro TTS Service Integration

**Scope**: Connect Kokoro TTS to running pipeline. Validate speech synthesis.

1. Start Kokoro service (`bin/kokoro-service`)
2. Verify TCP interconnect between LLaMA and Kokoro establishes
3. Inject speech through full pipeline: audio → Whisper → LLaMA → Kokoro
4. Verify Kokoro produces valid float32 PCM audio (24kHz)
5. Check synthesis quality: clear German speech, no artifacts
6. Verify Kokoro sends audio downstream to OAP port (even if OAP not running — dump behavior)
7. **Optimize**: Target < 2s synthesis for short sentences. Ensure MPS acceleration active
8. Fix all bugs

- **Verify**: Kokoro produces audible speech output. Synthesis time < 2s. TCP handling stable

### [ ] Step: Stage 6 Testing — OAP Integration + Full Round-Trip Quality Test

**Scope**: Connect OAP, complete the pipeline loop. Test end-to-end with 2-line round-trip.

1. Start OAP (`bin/outbound-audio-processor`)
2. Verify TCP interconnect between Kokoro and OAP establishes
3. Verify OAP receives float32 PCM, downsamples 24→8kHz, encodes G.711 u-law
4. Verify OAP sends 160-byte frames at 20ms intervals to SIP client
5. Verify SIP client forwards RTP to Line 2's RTP port
6. **Full round-trip test**:
   - Line 1 receives injected speech → Whisper transcribes → LLaMA responds → Kokoro synthesizes → OAP encodes → SIP Client → Line 2
   - Line 2's SIP client receives RTP from OAP
   - Start a second Whisper instance or configure Line 2's pipeline to transcribe incoming audio
   - Compare Line 2's Whisper transcription against LLaMA's original response text
   - This validates end-to-end TTS quality
7. **Optimize OAP**: Check 24→8kHz downsampling quality (currently `i += 3` decimation without filter — likely needs anti-aliasing). Verify 20ms frame scheduling jitter < 2ms
8. **Optimize Kokoro** based on round-trip transcription accuracy
9. Fix all bugs. End-to-end latency target < 5s

- **Verify**: Full pipeline operational. Round-trip transcription matches LLaMA output. End-to-end latency acceptable. No crashes under continuous operation
