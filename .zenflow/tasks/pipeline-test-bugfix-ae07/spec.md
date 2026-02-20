# Technical Specification: Pipeline Test Suite & Bugfix

## 1. Technical Context

- **Language**: C++17 (all pipeline services, frontend, SIP provider), Python 3.9+ (unused for this task)
- **Build**: CMake 3.22+, output to `bin/`
- **Platform**: macOS Apple Silicon (CoreML/Metal)
- **Key Libraries**: mongoose (HTTP server, already compiled into frontend and available as `mongoose.c`/`mongoose.h`), SQLite3 (frontend DB), whisper.cpp, llama.cpp, libtorch + espeak-ng (Kokoro)
- **Interconnect**: Custom TCP protocol via `interconnect.h` — linear pipeline topology, master/slave negotiation on ports 22222/33333+, heartbeat, speech signals, call lifecycle management
- **Test Framework**: Google Test (fetched via CMake FetchContent when `BUILD_TESTS=ON`)
- **Working Directory**: All services and test tools assume they are launched from the project root directory. The frontend, when launching `test_sip_provider` via `fork()/execv()`, must set CWD to project root. Alternatively, the SIP provider accepts `--testfiles-dir <path>` (default: `Testfiles/` relative to CWD). The `Testfiles/` directory is located at the project root alongside `CMakeLists.txt`. Since `CMAKE_RUNTIME_OUTPUT_DIRECTORY` is `${CMAKE_SOURCE_DIR}/bin`, the frontend should `chdir()` to the parent of `bin/` before `execv()`, or pass `--testfiles-dir ../Testfiles` when launching from `bin/`.

## 2. Source Code Structure Changes

### 2.1 Modified Files

| File | Changes |
|------|---------|
| `tests/test_sip_provider.cpp` | Add mongoose HTTP server (port 22011), WAV file loading/resampling, on-demand audio injection, CORS support, file listing endpoint |
| `sip-client-main.cpp` | Add `ADD_LINE`, `REMOVE_LINE`, `LIST_LINES` negotiation commands; dynamic line creation/teardown while running |
| `interconnect.h` | Extend `handle_negotiation_message` to route `ADD_LINE`/`REMOVE_LINE`/`LIST_LINES` to the SIP client via a registered custom handler |
| `frontend.cpp` | Add "Audio File Injection" section in Tests tab HTML; add `/api/sip-provider/files`, `/api/sip-provider/inject`, `/api/sip-provider/status` proxy endpoints; add SIP client line management UI and proxy endpoints |
| `inbound-audio-processor.cpp` | Potential performance optimizations (ulaw LUT already exists, evaluate buffer chunking strategy) |
| `whisper-service.cpp` | VAD parameter tuning, transcription accuracy optimization |
| `llama-service.cpp` | Response quality tuning, shut-up mechanism validation |
| `kokoro-service.cpp` | Synthesis quality validation |
| `outbound-audio-processor.cpp` | 24kHz→8kHz downsampling validation, frame scheduling |
| `CMakeLists.txt` | Link mongoose into `test_sip_provider` target |

### 2.2 New Files

None. All changes are modifications to existing files.

## 3. Implementation Approach

### 3.1 SIP Provider Enhancement (test_sip_provider.cpp)

#### 3.1.1 Mongoose HTTP Server Embedding

The `TestSipProvider` class gains a `mg_mgr` member. During `init()`, call `mg_mgr_init` and `mg_http_listen` on `http://0.0.0.0:22011`. In the main `run()` loop, integrate mongoose polling:

```cpp
while (g_running) {
    mg_mgr_poll(&mgr_, 1);  // 1ms timeout for mongoose HTTP
    
    // Non-blocking SIP recv (reduce existing 1s timeout to 1ms)
    struct timeval tv = {0, 1000};
    setsockopt(sip_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    ssize_t n = recvfrom(sip_sock_, buf, sizeof(buf) - 1, 0, ...);
    if (n > 0) handle_sip_message(msg, sender);
    
    // Existing call state checks...
}
```

The key change: reduce the SIP `recvfrom` timeout from 1 second to 1ms so both SIP and HTTP can be serviced responsively in the same loop.

HTTP handler routes:
- `GET /files` — scan `Testfiles/` directory, return JSON array of `{name, size_bytes}`
- `POST /inject` — parse JSON body `{"file":"sample_01.wav","leg":"a"}`, load WAV, resample, inject as RTP
- `GET /status` — return current call state, relay stats, injection status

CORS: Add `Access-Control-Allow-Origin: *\r\n` to all response headers. Handle `OPTIONS` preflight with 204.

HTTP response format:
- Success: `POST /inject` → 200 `{"success":true,"injecting":"sample_01.wav","leg":"a"}`; `GET /files` → 200 `{"files":[{"name":"sample_01.wav","size_bytes":373130},...]}`; `GET /status` → 200 `{"call_active":true,"relay_stats":{...},"injecting":"sample_01.wav"|null}`
- Errors: 400 (bad request), 404 (file not found), 409 (no active call), 500 (runtime failure) — all with `{"error":"<message>"}`

#### 3.1.2 WAV Loading and Resampling

**WAV Parser** — add a `load_wav_file(path) -> WavData` function returning `{std::vector<int16_t> samples, uint32_t sample_rate, uint16_t channels}`:

1. Read RIFF header (12 bytes): validate `"RIFF"` magic, read file size, validate `"WAVE"` format
2. Scan chunks linearly: read chunk ID (4 bytes) + chunk size (4 bytes)
3. Parse `"fmt "` chunk: extract `audio_format` (1=PCM, 3=IEEE float), `num_channels`, `sample_rate`, `bits_per_sample`
4. Parse `"data"` chunk: read raw sample data
5. Convert to int16 mono:
   - PCM_16: direct read (or byte-swap if needed)
   - PCM_24: shift right 8 bits to truncate to int16
   - FLOAT32 (format=3): multiply by 32767, clamp, cast to int16
   - Stereo: average L+R channels to mono
6. Error handling: return empty result with error string if RIFF header invalid, format unsupported (e.g. ADPCM, compressed), or file exceeds 50MB safety limit
7. Supported: PCM_16, PCM_24, FLOAT32 mono/stereo at any sample rate
8. Rejected: Compressed formats (ADPCM, MP3-in-WAV, etc.)

**Resampler** — add a `resample_to_8khz(samples, source_rate) -> std::vector<int16_t>` function:

For downsampling (e.g. 44.1kHz → 8kHz, ratio 0.1814), a simple low-pass filter is required before decimation to prevent aliasing:

1. **Anti-aliasing filter**: Apply a simple FIR low-pass filter before decimation. Use a windowed-sinc filter with cutoff at 3.4kHz (Nyquist for 8kHz target is 4kHz, with margin). A 15-tap FIR filter provides adequate attenuation for telephony-quality audio:
   - Generate filter coefficients: sinc function windowed with Hamming window, normalized
   - Apply via convolution on the source samples
2. **Decimation**: After filtering, resample using linear interpolation at the target rate:
   - For each output sample at position `i`, compute source position `src_pos = i * (source_rate / 8000.0)`
   - Interpolate between the two nearest filtered source samples
3. Special cases:
   - Source rate == 8000: no resampling needed, return as-is
   - Source rate == 16000: simple 2:1 decimation (average pairs), no filter needed (already below Nyquist)
   - Source rate == 44100, 22050: full filter + decimate path

#### 3.1.3 RTP Injection from File

Refactor existing `inject_audio` method. The current method generates a 400Hz tone — replace with a general `inject_rtp_stream(call, leg, ulaw_samples)` method:
- Takes pre-encoded u-law samples
- Sends 160-sample RTP frames at 20ms intervals using `std::this_thread::sleep_for`
- Manages RTP headers: seq (incrementing), timestamp (+= 160 per frame), SSRC (random per call via `std::random_device`)

**Thread management for injection**:
- Use `std::thread` (joinable, NOT detached), stored in `ActiveCall::inject_thread`
- Track injection state with `std::atomic<bool> injecting_` per call
- In the injection loop, check `injecting_` flag before each 20ms sleep. If `false`, exit loop immediately
- Only one injection per call at a time. To cancel an in-progress injection: set `injecting_ = false`, then `join()` the thread (max wait ~20ms per frame)
- New injection requests while one is active: cancel the current one first, wait for join, then start new thread

New `inject_file(filename, leg)` method:
1. Load WAV via `load_wav_file`
2. Resample to 8kHz via `resample_to_8khz`
3. Encode each sample via existing `linear_to_ulaw`
4. Cancel any in-progress injection (set `injecting_ = false`, join thread)
5. Set `injecting_ = true`, start new `inject_rtp_stream` thread

#### 3.1.4 CMake Changes

Modify the existing `test_sip_provider` target in `CMakeLists.txt` (currently lines 334-336) to add mongoose:

```cmake
add_executable(test_sip_provider tests/test_sip_provider.cpp mongoose.c)
target_link_libraries(test_sip_provider PRIVATE Threads::Threads)
target_compile_definitions(test_sip_provider PRIVATE MG_ENABLE_PACKED_FS=0)
set_property(TARGET test_sip_provider PROPERTY CXX_STANDARD 17)
```

This replaces the existing 3-line block. `Threads::Threads` is required for `std::thread`, `CXX_STANDARD 17` for structured bindings and other C++17 features, and `MG_ENABLE_PACKED_FS=0` disables mongoose's embedded filesystem (not needed).

### 3.2 SIP Client Dynamic Line Management (sip-client-main.cpp)

#### 3.2.1 Interconnect Protocol Extension

Add a custom negotiation message handler to the SIP client. The `InterconnectNode` already supports arbitrary messages in `handle_negotiation_message`. The approach:

1. Add a `register_custom_negotiation_handler(std::function<std::string(const std::string&)>)` method to `InterconnectNode` in `interconnect.h`
2. In `handle_negotiation_message`, after all existing command parsing, check if the message matches no known command and delegate to the custom handler
3. The SIP client registers a handler that processes:
   - `ADD_LINE <user> <server_ip> [password]` → creates SipLine, starts registration loop, returns `LINE_ADDED <index>`
   - `REMOVE_LINE <index>` → stops registration, closes socket, removes line, returns `LINE_REMOVED <index>`
   - `LIST_LINES` → returns `LINES <index>:<user>:<registered> ...` for each line

#### 3.2.2 Dynamic Line Lifecycle

New methods on `SipClient`:
- `add_line(user, server_ip, password) -> int` — creates `SipLine`, assigns next index, starts `sip_loop` and `registration_loop` threads, returns index
- `remove_line(index) -> bool` — signals line threads to stop, closes sockets, removes from `lines_` vector, cleans up associated calls

Thread safety: `lines_` access protected by existing `calls_mutex_` or a new `lines_mutex_`.

### 3.3 Frontend UI Enhancement (frontend.cpp)

#### 3.3.1 Tests Tab: Audio File Injection Section

Add HTML below the existing `testsContainer` div:

```
<hr><h5>Audio File Injection (SIP Provider)</h5>
<div class="row">
  <div class="col-md-4">
    <select id="injectFile" class="form-select"></select>
  </div>
  <div class="col-md-2">
    <select id="injectLeg" class="form-select">
      <option value="a">Leg A</option>
      <option value="b">Leg B</option>
    </select>
  </div>
  <div class="col-md-2">
    <button class="btn btn-warning" onclick="injectAudio()">Inject</button>
  </div>
  <div class="col-md-4">
    <span id="injectStatus" class="text-muted">Ready</span>
  </div>
</div>
```

JS functions:
- `fetchInjectFiles()` — `GET http://localhost:22011/files`, populates dropdown
- `injectAudio()` — `POST http://localhost:22011/inject` with selected file/leg, updates status span

#### 3.3.2 SIP Client Line Management Section

Add HTML in Tests tab for line management:
- Input fields: user, server IP, password
- "Add Line" button → calls frontend API which forwards to SIP client via interconnect
- Line status table showing active lines
- "Remove" button per line

Frontend proxies these commands to the SIP client by connecting to its negotiation port and sending the `ADD_LINE`/`REMOVE_LINE`/`LIST_LINES` messages. The frontend is already an `InterconnectNode` and can use `connect_to_port` to reach the SIP client's negotiation port (looked up via registry or known offset).

#### 3.3.3 Frontend API Endpoints

Add to `http_handler`:
- `POST /api/sip/add-line` — extract user/server/password from JSON body, send `ADD_LINE` to SIP client via interconnect negotiation
- `POST /api/sip/remove-line` — extract index, send `REMOVE_LINE`
- `GET /api/sip/lines` — send `LIST_LINES`, return result as JSON

**Frontend Interconnect Role and Proxy Mechanism**:

The frontend initializes as `InterconnectNode(ServiceType::FRONTEND)`. It can be either master (if first to start on port 22222) or slave. Either way, it can discover the SIP client's negotiation port:

1. **If frontend is master**: Look up `ServiceType::SIP_CLIENT` directly from the local `service_registry_` via `query_service_ports(ServiceType::SIP_CLIENT)`
2. **If frontend is slave**: Send `GET_DOWNSTREAM SIP_CLIENT` (or equivalent) to the master to get the SIP client's ports. Alternatively, use the synced registry from SYNC_REGISTRY broadcasts.

Once the SIP client's `neg_in` port is known, the frontend's proxy handler:
1. Opens a TCP connection to `127.0.0.1:<sip_client_neg_in_port>` using `connect_to_port()`
2. Sends the text command (e.g. `ADD_LINE alice 127.0.0.1`)
3. Reads the response with a 2-second timeout
4. Closes the TCP socket
5. Parses the response and returns it as JSON to the browser

Add a helper method to `FrontendServer`:
```cpp
std::string send_negotiation_command(ServiceType target, const std::string& cmd);
```
This encapsulates the connect → send → recv → close pattern. Returns empty string on failure.

**No authentication** for these endpoints — consistent with existing `/api/tests/*` endpoints. Acceptable for localhost-only test environment.

### 3.4 Interconnect Extension (interconnect.h)

Add a custom handler registration mechanism:

```cpp
std::function<std::string(const std::string&)> custom_negotiation_handler_;

void register_custom_negotiation_handler(
    std::function<std::string(const std::string&)> handler) {
    custom_negotiation_handler_ = handler;
}
```

In `handle_negotiation_message`, at the **end** of the function (just before the `if (!response.empty())` send block), add:

```cpp
if (response.empty() && custom_negotiation_handler_) {
    response = custom_negotiation_handler_(msg);
}
```

**Master/Slave Topology Clarification**:

The custom handler runs on **whichever node receives the message on its negotiation port**. The line management flow works as follows:

1. The SIP client registers a custom handler during `init()` that handles `ADD_LINE`/`REMOVE_LINE`/`LIST_LINES`
2. The frontend needs to send these commands **directly to the SIP client's negotiation port**, NOT to the master
3. The frontend looks up the SIP client's negotiation port from the master's service registry via `query_service_ports(ServiceType::SIP_CLIENT)` (if frontend is master, this is a local lookup; if not, it queries the master)
4. The frontend opens a TCP connection to the SIP client's `neg_in` port and sends the command directly
5. The SIP client's `handle_negotiation_message` receives the message, finds no built-in match, delegates to the custom handler, and sends back the response

This approach works regardless of which node is master because:
- The custom handler is invoked on the SIP client's own negotiation listener
- The sender (frontend) just needs to know the SIP client's negotiation port
- The master's service registry provides port discovery
- No changes to master-specific routing logic are needed

## 4. Data Flow: Audio File Injection

```
Frontend (browser)
  → HTTP POST /inject {file, leg} to SIP Provider (port 22011)
  → SIP Provider loads WAV, resamples 44.1kHz→8kHz, encodes u-law
  → SIP Provider sends 160-byte RTP frames @ 20ms to SIP Client's RTP port (leg A or B)
  → SIP Client receives RTP, creates Packet with call_id prefix
  → SIP Client sends Packet to IAP via interconnect TCP (if connected, else dumps)
  → IAP decodes u-law→float32, upsamples 8kHz→16kHz
  → IAP sends to Whisper via interconnect
  → Whisper VAD + transcription → sends text to LLaMA
  → LLaMA generates response → sends to Kokoro
  → Kokoro synthesizes → sends float32 PCM to OAP
  → OAP downsamples 24kHz→8kHz, encodes u-law → sends frames to SIP Client
  → SIP Client sends RTP to remote party (line 2's RTP port)
```

## 5. Incremental Delivery Phases

Phases 1-3 are implementation setup. Phases 4-9 map to the 6 testing stages in requirements.md:
- Phase 4 → Requirements Stage 1 (Frontend + SIP Provider + SIP Client)
- Phase 5 → Requirements Stage 2 (+ IAP)
- Phase 6 → Requirements Stage 3 (+ Whisper)
- Phase 7 → Requirements Stage 4 (+ LLaMA)
- Phase 8 → Requirements Stage 5 (+ Kokoro)
- Phase 9 → Requirements Stage 6 (+ OAP, Full Loop)

### Phase 1: SIP Provider Enhancement + CMake
- Mongoose HTTP server in test_sip_provider
- WAV loading, resampling, u-law encoding
- `/files`, `/inject`, `/status` endpoints
- CMake: link mongoose into test_sip_provider
- **Verify**: Build, start SIP provider, `curl` endpoints, confirm file listing and injection

### Phase 2: SIP Client Dynamic Line Management
- Custom negotiation handler in interconnect.h
- `ADD_LINE`/`REMOVE_LINE`/`LIST_LINES` in SIP client
- **Verify**: Start SIP client, send negotiation commands via `nc` or test script, verify lines added/removed

### Phase 3: Frontend UI
- Audio injection section in Tests tab
- Line management section in Tests tab
- Proxy API endpoints
- **Verify**: Open browser, start SIP provider, verify dropdown populates, injection triggers, line management works

### Phase 4: Stage 1 Testing (Frontend + SIP Provider + SIP Client)
- Start all three from frontend
- Inject audio, verify RTP flow through SIP client (dumped without IAP)
- Test with multiple lines
- Fix all bugs

### Phase 5: Stage 2 Testing (+ IAP)
- Start IAP, verify TCP connection handling
- Stop/restart IAP multiple times, verify resilience
- Optimize IAP conversion speed if needed
- Fix all bugs

### Phase 6: Stage 3 Testing (+ Whisper)
- Connect Whisper, verify TCP interconnect
- Send audio through pipeline, compare transcription to `Testfiles/*.txt`
- Tune VAD parameters for complete sentence capture
- Optimize transcription speed and accuracy
- Fix all bugs

### Phase 7: Stage 4 Testing (+ LLaMA)
- Connect LLaMA, verify TCP interconnect
- Test response generation quality
- Test shut-up mechanism (inject speech during generation)
- Optimize response speed and quality
- Fix all bugs

### Phase 8: Stage 5 Testing (+ Kokoro)
- Connect Kokoro, verify TCP interconnect
- Validate speech synthesis quality
- Optimize synthesis speed
- Fix all bugs

### Phase 9: Stage 6 Testing (+ OAP, Full Loop)
- Connect OAP, verify TCP interconnect
- Full round-trip: Line 1 speaks → Whisper → LLaMA → Kokoro → OAP → SIP Client → Line 2
- Line 2's Whisper transcribes Kokoro's output
- Compare Line 2's transcription against LLaMA's original text
- Optimize end-to-end quality
- Fix all bugs

## 6. Verification Approach

### Build Verification
```bash
cmake -B build -DBUILD_TESTS=ON && cmake --build build -j$(sysctl -n hw.ncpu)
```
Run from the project root directory (where `CMakeLists.txt` is located). Binaries output to `bin/`.

### Functional Verification per Phase
- **Phase 1**: `curl http://localhost:22011/files` returns JSON array; `curl -X POST -d '{"file":"sample_01.wav","leg":"a"}' http://localhost:22011/inject` triggers injection
- **Phase 2**: Connect to SIP client negotiation port, send `ADD_LINE test 127.0.0.1`, verify `LINE_ADDED` response
- **Phase 3**: Open `http://localhost:8080`, verify Tests tab shows injection controls
- **Phase 4-9**: Run pipeline incrementally, inject test files, verify output at each stage; compare Whisper transcriptions against ground truth `.txt` files

### Transcription Accuracy Metric

For Stage 3, compare Whisper output against `Testfiles/*.txt` ground truth:

1. **Capture**: After injecting a test file, wait for Whisper to emit a complete utterance (indicated by `SPEECH_IDLE` signal or observing the transcription log line `📝 [call_id] Transcription ...`). Extract the transcribed text from Whisper's stdout log output.
2. **Normalize both strings** (expected from `.txt` and actual from Whisper):
   - Convert to lowercase
   - Strip leading/trailing whitespace
   - Remove all punctuation (`.,;:!?-`)
   - Collapse multiple spaces to single space
3. **Comparison**:
   - **PASS**: 100% normalized string match
   - **WARN**: ≥90% character-level similarity (Levenshtein distance / max length)
   - **FAIL**: <90% similarity
4. **Logging**: Results logged to frontend via UDP log with format:
   `TRANSCRIPTION_TEST sample_01.wav PASS expected='bei fettiger haut...' got='bei fettiger haut...'`
5. **Iterative tuning**: If FAIL/WARN, adjust VAD parameters (`VAD_THRESHOLD_MULT`, `VAD_SILENCE_FRAMES`, `VAD_CONTEXT_FRAMES`) and re-run until PASS for all 10 samples.

## 7. Key Technical Decisions

1. **Mongoose in SIP Provider**: Use existing `mongoose.c`/`mongoose.h` already in the project. The SIP provider's main loop already has a poll-based structure; `mg_mgr_poll` integrates cleanly with 0ms timeout.

2. **WAV Parsing**: Implement minimal WAV parser in `test_sip_provider.cpp` (read RIFF header, fmt chunk, data chunk). No external library needed — WAV is a simple format and we only need PCM16/float32 mono/stereo.

3. **Resampling**: FIR low-pass filter + linear interpolation for downsampling (44.1kHz→8kHz). Anti-aliasing filter with 3.4kHz cutoff prevents aliasing artifacts. Simple 2:1 decimation for 16kHz→8kHz. See Section 3.1.2 for details.

4. **Custom Negotiation Handler**: Rather than hardcoding new commands in `interconnect.h`, add a callback mechanism so the SIP client can register its own command parser. This keeps `interconnect.h` generic and the SIP client responsible for its own commands.

5. **Frontend Proxy vs Direct**: The frontend JS calls the SIP provider's HTTP API directly (port 22011) for file injection — this avoids adding proxy logic to the frontend for test-only features. For line management, the frontend uses its interconnect connection to the SIP client since those commands go through the negotiation protocol.

6. **Thread Safety**: Audio injection runs in a joinable `std::thread` per call. Track with `std::atomic<bool> injecting_`. New injections cancel in-progress ones (set flag false, join thread, start new). See Section 3.1.3 for details.

7. **File Size Limits**: WAV files capped at 50MB (~5 minutes at 44.1kHz stereo 16-bit) to prevent memory exhaustion during loading.

8. **Concurrent Injection**: Only one injection per call at a time. New injection request cancels any in-progress injection (no queuing).

9. **RTP SSRC**: Use `std::random_device` to generate random SSRC per injection, consistent with existing `inject_audio` pattern (`0xDEAD0001` in current code; replace with random).
