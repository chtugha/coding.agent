# Technical Specification: Pipeline Test Suite & Bugfix

## 1. Technical Context

- **Language**: C++17 (all pipeline services, frontend, SIP provider), Python 3.9+ (unused for this task)
- **Build**: CMake 3.22+, output to `bin/`
- **Platform**: macOS Apple Silicon (CoreML/Metal)
- **Key Libraries**: mongoose (HTTP server, already compiled into frontend and available as `mongoose.c`/`mongoose.h`), SQLite3 (frontend DB), whisper.cpp, llama.cpp, libtorch + espeak-ng (Kokoro)
- **Interconnect**: Custom TCP protocol via `interconnect.h` — linear pipeline topology, master/slave negotiation on ports 22222/33333+, heartbeat, speech signals, call lifecycle management
- **Test Framework**: Google Test (fetched via CMake FetchContent when `BUILD_TESTS=ON`)

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

The `TestSipProvider` class gains a `mg_mgr` member. During `init()`, call `mg_mgr_init` and `mg_http_listen` on `http://0.0.0.0:22011`. In the main `run()` loop, call `mg_mgr_poll(&mgr_, 0)` alongside the existing SIP message handling (non-blocking, 0ms timeout).

HTTP handler routes:
- `GET /files` — scan `Testfiles/` directory, return JSON array of `{name, size_bytes}`
- `POST /inject` — parse JSON body `{"file":"sample_01.wav","leg":"a"}`, load WAV, resample, inject as RTP
- `GET /status` — return current call state, relay stats, injection status

CORS: Add `Access-Control-Allow-Origin: *\r\n` to all response headers. Handle `OPTIONS` preflight with 204.

#### 3.1.2 WAV Loading and Resampling

Add a `load_wav_file(path) -> {samples, sample_rate, channels}` function:
- Read RIFF/WAV header to extract `fmt` chunk: sample rate, bit depth, channels
- Read `data` chunk into `std::vector<int16_t>` (or convert from float32 if needed)
- If stereo, downmix to mono

Add a `resample_to_8khz(samples, source_rate) -> std::vector<int16_t>` function:
- Linear interpolation resampler (matches IAP's approach)
- Handles 44.1kHz, 22.05kHz, 16kHz → 8kHz

#### 3.1.3 RTP Injection from File

Refactor existing `inject_audio` method. The current method generates a 400Hz tone — replace with a general `inject_rtp_stream(call, leg, ulaw_samples)` method:
- Takes pre-encoded u-law samples
- Sends 160-sample RTP frames at 20ms intervals using `std::this_thread::sleep_for`
- Manages RTP headers (seq, timestamp += 160, SSRC)
- Runs in a detached thread per injection; tracks active injection with `std::atomic<bool>`

New `inject_file(filename, leg)` method:
1. Load WAV via `load_wav_file`
2. Resample to 8kHz via `resample_to_8khz`
3. Encode each sample via existing `linear_to_ulaw`
4. Call `inject_rtp_stream`

#### 3.1.4 CMake Changes

Add mongoose to the test_sip_provider target:

```cmake
add_executable(test_sip_provider tests/test_sip_provider.cpp mongoose.c)
target_compile_definitions(test_sip_provider PRIVATE MG_ENABLE_PACKED_FS=0)
```

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

The frontend (as master or via master) can query the SIP client's negotiation port from the service registry, connect, send the command, read the response, and relay it as HTTP JSON.

### 3.4 Interconnect Extension (interconnect.h)

Add a custom handler registration mechanism:

```cpp
void register_custom_negotiation_handler(
    std::function<std::string(const std::string&)> handler) {
    custom_negotiation_handler_ = handler;
}
```

In `handle_negotiation_message`, after all existing `if/else if` chains, add:

```cpp
if (response.empty() && custom_negotiation_handler_) {
    response = custom_negotiation_handler_(msg);
}
```

This allows any service to extend the negotiation protocol without modifying `interconnect.h` for each new command.

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
cd coding.agent && mkdir -p build && cd build && cmake .. -DBUILD_TESTS=ON && make -j$(sysctl -n hw.ncpu)
```

### Functional Verification per Phase
- **Phase 1**: `curl http://localhost:22011/files` returns JSON array; `curl -X POST -d '{"file":"sample_01.wav","leg":"a"}' http://localhost:22011/inject` triggers injection
- **Phase 2**: Connect to SIP client negotiation port, send `ADD_LINE test 127.0.0.1`, verify `LINE_ADDED` response
- **Phase 3**: Open `http://localhost:8080`, verify Tests tab shows injection controls
- **Phase 4-9**: Run pipeline incrementally, inject test files, verify output at each stage; compare Whisper transcriptions against ground truth `.txt` files

### Transcription Accuracy Metric
For Stage 3, compare Whisper output against `Testfiles/*.txt` using normalized string comparison (lowercased, trimmed). Target: exact match or near-exact match (allowing minor punctuation/spacing differences).

## 7. Key Technical Decisions

1. **Mongoose in SIP Provider**: Use existing `mongoose.c`/`mongoose.h` already in the project. The SIP provider's main loop already has a poll-based structure; `mg_mgr_poll` integrates cleanly with 0ms timeout.

2. **WAV Parsing**: Implement minimal WAV parser in `test_sip_provider.cpp` (read RIFF header, fmt chunk, data chunk). No external library needed — WAV is a simple format and we only need PCM16/float32 mono/stereo.

3. **Resampling**: Linear interpolation (same approach as IAP's 8kHz→16kHz upsampling). For 44.1kHz→8kHz, ratio = 0.1814. This is adequate for telephony-quality audio.

4. **Custom Negotiation Handler**: Rather than hardcoding new commands in `interconnect.h`, add a callback mechanism so the SIP client can register its own command parser. This keeps `interconnect.h` generic and the SIP client responsible for its own commands.

5. **Frontend Proxy vs Direct**: The frontend JS calls the SIP provider's HTTP API directly (port 22011) for file injection — this avoids adding proxy logic to the frontend for test-only features. For line management, the frontend uses its interconnect connection to the SIP client since those commands go through the negotiation protocol.

6. **Thread Safety**: Audio injection runs in a separate thread. Track with `std::atomic<bool> injecting_` per call. New injections cancel in-progress ones (set `injecting_ = false`, wait for thread to finish, then start new one).
