# Technical Specification: Improve Outbound Speech Quality / Test Infrastructure

## Difficulty: Hard

Multiple independent infrastructure changes (items 1–6) plus an iterative diagnostic/fix cycle (item 7) requiring audio signal analysis and root-cause driven code changes to Kokoro and/or OAP.

---

## Technical Context

- **Language**: C++17, embedded HTML/JavaScript (single-file frontend), Python 3.9+
- **Build**: CMake, macOS/Apple Silicon
- **Key files**:
  - `sip-client-main.cpp` — SIP/RTP gateway
  - `tests/test_sip_provider.cpp` — B2BUA test provider (SIP + HTTP control)
  - `outbound-audio-processor.cpp` — 24kHz→8kHz downsampler + G.711 encoder
  - `kokoro-service.cpp` — TTS engine (Kokoro, 24kHz float32 PCM output)
  - `frontend.cpp` — Mongoose-based HTTP server with embedded single-page app

---

## Change 1: WAV Recording in test_sip_provider

### Purpose
Allow saving the audio received by the test SIP provider (TTS playback coming back from the sip-client's OAP) as WAV files per leg per call, for offline audio quality analysis.

### Approach
- Add `--save-wav-dir <dir>` CLI flag (default: empty = disabled)
- Add instance members in `TestSipProvider`:
  - `std::atomic<bool> save_wav_enabled_{false}`
  - `std::string save_wav_dir_` protected by `save_wav_mutex_`
  - `std::mutex save_wav_mutex_`
- In `ActiveCall`, add per-leg receive buffer: `std::vector<std::vector<uint8_t>> leg_wav_buffers` (μ-law samples, one vector per leg index); resize in `initiate_conference()`
- In `conference_relay_thread`: when `save_wav_enabled_`, extract 160-byte μ-law payload from each received RTP packet (skip 12-byte RTP header) and append to `call->leg_wav_buffers[from_idx]`
- In `shutdown_call()`: if `save_wav_enabled_` and any buffer non-empty, write one WAV file per leg:
  - Filename: `tsp_call_<id>_<username>_<timestamp>.wav`
  - Decode μ-law → int16 PCM using ITU-T G.711 decode
  - Write 8kHz mono 16-bit PCM RIFF/WAVE file
- Add HTTP endpoints on existing mongoose event loop:
  - `GET /wav_recording` → `{"enabled":bool,"dir":"string"}`
  - `POST /wav_recording {"enabled":bool,"dir":"string"}` → `{"success":true}`

### WAV file format
Standard RIFF/WAVE: 44-byte header, PCM format (audio_format=1), 1 channel, 8000 Hz, 16-bit little-endian, chunk sizes filled in after sample collection.

### μ-law decode
```cpp
static int16_t ulaw_to_linear(uint8_t u) {
    u = ~u;
    int sign = u & 0x80;
    int exponent = (u >> 4) & 0x07;
    int mantissa = u & 0x0F;
    int sample = ((mantissa << 3) + 0x84) << exponent;
    return sign ? (int16_t)(0x84 - sample) : (int16_t)(sample - 0x84);
}
```

---

## Change 2: Frontend SIP Provider Config Panel

### Purpose
Expose WAV recording controls in the frontend UI when the TEST_SIP_PROVIDER service is selected.

### Approach
- Add `div#sipProviderConfig` HTML div (parallel to `div#sipClientConfig`):
  - Checkbox: "Save incoming audio as WAV" (id: `sipProviderSaveWav`)
  - Text input: "Save to directory" (id: `sipProviderWavDir`, placeholder `/tmp/whispertalk-wav`)
  - Status span: `id="sipProviderWavStatus"`
- In `updateSvcDetail()` JS function: reveal `sipProviderConfig` when `s.name === 'TEST_SIP_PROVIDER'`, call `loadSipProviderWavConfig()`; hide otherwise
- JS functions (use existing `TSP_PORT` JS variable, not hardcoded port):
  - `loadSipProviderWavConfig()`: `fetch('http://localhost:'+TSP_PORT+'/wav_recording')` → populate checkbox + input
  - `saveSipProviderWavConfig()`: POST to same URL with current checkbox/dir values
  - Checkbox `onchange` → call `saveSipProviderWavConfig()`

---

## Change 3: Fix Audio Injection Section in Beta Testing Page

### Current state
- Label: "Inject into Leg (username)"
- Dropdown: hardcoded `<option value="a">Leg A (first)</option>` / `<option value="b">Leg B (second)</option>`
- `refreshInjectLegs()` fetches `/calls` from test_sip_provider but falls back to hardcoded options when no call is active
- `refreshInjectLegs()` is not called on page load

### Fix
1. Change label text to "Inject into active testline"
2. Replace hardcoded fallback options with a single disabled placeholder: `<option value="" disabled>-- No active testlines --</option>`
3. Update `refreshInjectLegs()`: when no active call/legs, show only the disabled placeholder
4. Add `refreshInjectLegs()` to both beta-testing page load handlers (line ~2426 and line ~5198)

---

## Change 4: SIP Client Starts Without Default Lines

### Current state
- `sip-client-main.cpp`: `std::max(1, atoi(optarg))` enforces ≥1 line for `--lines`
- `main()` requires `<user> <server>` positional arguments regardless of `--lines`
- Frontend DB seed: `'--lines 2 alice 127.0.0.1 5060'`

### Fix in sip-client-main.cpp
- Remove `std::max(1, ...)` → allow `--lines 0`
- Change `main()`: positional `<user> <server>` only validated when `lines > 0`
- Update usage string

### Fix in frontend.cpp
- Change DB seed for `SIP_CLIENT` `default_args` from `'--lines 2 alice 127.0.0.1 5060'` to `''`
- Add exact-match migration:
  ```sql
  UPDATE service_config SET default_args='' WHERE service='SIP_CLIENT'
    AND (default_args='--lines 1 alice 127.0.0.1 5060'
         OR default_args='--lines 2 alice 127.0.0.1 5060');
  ```

---

## Change 5: WAV Recording in OAP (outbound-audio-processor.cpp)

### Purpose
Allow saving the downsampled 8kHz PCM audio (the exact audio sent to the caller's phone before G.711 encode) as WAV files per call, for outbound TTS quality diagnosis.

### Approach
- Add `--save-wav-dir <dir>` CLI flag (default: empty = disabled)
- Add instance members in `OutboundAudioProcessor`:
  - `std::atomic<bool> save_wav_enabled_{false}`
  - `std::string save_wav_dir_` protected by `save_wav_mutex_`
  - `std::mutex save_wav_mutex_`
- Add `std::vector<int16_t> wav_samples` field to `CallState`
- In `downsample_and_encode_into()`: after computing each `int16_t s16` (clipped 8kHz PCM), when `save_wav_enabled_`, append it to `state->wav_samples`
- In `handle_call_end()`: if enabled and `wav_samples` non-empty, write `oap_call_<id>_<timestamp>.wav` (8kHz mono 16-bit PCM RIFF/WAVE); clear `wav_samples` after writing
- Add cmd-port commands in `handle_command()` (consistent with `SET_SIDETONE_GUARD_MS` pattern):
  - `SAVE_WAV:ON` → set `save_wav_enabled_ = true`, return `OK\n`
  - `SAVE_WAV:OFF` → set `save_wav_enabled_ = false`, return `OK\n`
  - `SAVE_WAV:STATUS` → return `SAVE_WAV:ON dir=<dir>\n` or `SAVE_WAV:OFF\n`
  - `SET_SAVE_WAV_DIR:<dir>` → set `save_wav_dir_`, return `OK\n`

### Frontend proxy
- Add `handle_oap_wav_recording()` C++ handler in `frontend.cpp` following `handle_whisper_hallucination_filter()` pattern:
  - `GET /api/oap/wav_recording`: sends `SAVE_WAV:STATUS` to OAP cmd port (13152) via `tcp_command()`, parses response, returns `{"enabled":bool,"dir":"string"}`
  - `POST /api/oap/wav_recording`: parses `enabled` + `dir` from JSON body; sends `SET_SAVE_WAV_DIR:<dir>` and `SAVE_WAV:ON` or `SAVE_WAV:OFF`; returns `{"success":true}` or `{"error":"...","live_update":false}` when OAP offline
- Register `/api/oap/wav_recording` in the HTTP route dispatch in `frontend.cpp`

---

## Change 6: Frontend OAP Config Panel

### Purpose
Expose WAV recording controls in the frontend UI when the OUTBOUND_AUDIO_PROCESSOR service is selected.

### Approach
- Add `div#oapConfig` HTML div (parallel to `div#sipClientConfig` and `div#whisperConfig`):
  - Checkbox: "Save outgoing audio as WAV" (id: `oapSaveWav`)
  - Text input: "Save to directory" (id: `oapWavDir`, placeholder `/tmp/whispertalk-wav`)
  - Status span: `id="oapWavStatus"`
- In `updateSvcDetail()` JS: reveal `oapConfig` when `s.name === 'OUTBOUND_AUDIO_PROCESSOR'`, call `loadOapWavConfig()`; hide otherwise
- JS functions:
  - `loadOapWavConfig()`: `fetch('/api/oap/wav_recording')` → populate checkbox + input; on error show "(offline)"
  - `saveOapWavConfig()`: POST to `/api/oap/wav_recording` with current checkbox/dir values
  - Checkbox `onchange` → call `saveOapWavConfig()`

---

## Change 7: Stage 7 — Automated Quality Collection + Kokoro/OAP Optimization

### Purpose
Automate a 10-run diagnostic loop that captures WAV output + logs at each stage, then diagnose and fix the root cause of the "Darth Vader" / stutter distortion in the outbound audio path.

### Sub-step A: Implement the collection script

Create `tests/run_stage7.py` — a Python 3 script that:
1. Starts all 7 services + test_sip_provider via the frontend HTTP API (`POST /api/service/start`)
2. Waits 10 seconds for warmup
3. Connects 1 line from sip-client to test_sip_provider via test_sip_provider `/conference` endpoint (using the first registered SIP user from `/users`)
4. Enables WAV saving in test_sip_provider (`POST http://localhost:22011/wav_recording {"enabled":true,"dir":"<output_dir>"}`)
5. Enables WAV saving in OAP (`POST http://localhost:8080/api/oap/wav_recording {"enabled":true,"dir":"<output_dir>"}`) — frontend proxy port from existing config
6. Injects one sample from `Testfiles/` into the active testline (`POST http://localhost:22011/inject {"file":"sample_01.wav","leg":"<user>","no_silence":true}`)
7. Waits for pipeline completion: polls until injection is no longer active (injecting=false in `/status`) + additional 5s buffer for Kokoro+OAP to finish
8. Collects logs from frontend API (`GET /api/logs?limit=500`) and writes to `<output_dir>/run_<n>/pipeline.log`
9. Hangs up the call (`POST http://localhost:22011/hangup`) — WAV files are written on call end
10. Copies WAV files to `<output_dir>/run_<n>/`
11. Repeats 10 times with `--iterations N` (default 10), using different sample files if available

Output structure: `stage7_output/run_N/pipeline.log`, `tsp_call_*.wav`, `oap_call_*.wav`

The script should output a summary of collected files at the end.

### Sub-step B: Diagnose and fix audio quality

**Run the collection script** to gather 10 data samples. The implementing agent then:

1. **Listen to / inspect both WAVs per run**:
   - OAP WAV: 8kHz PCM before G.711 encode — if this sounds clean → problem is in G.711 encode or RTP framing
   - TSP WAV: 8kHz PCM received back at test_sip_provider — if this sounds bad but OAP sounds clean → problem is in G.711 encode, RTP transmission, or sip-client decode
   - If OAP WAV already sounds distorted → problem is upstream: Kokoro synthesis, OAP downsampling, or normalization

2. **Analyze logs** for:
   - Buffer underruns in OAP scheduler (silence frames being sent instead of audio)
   - Timing jitter in the 20ms scheduler loop
   - Kokoro chunk sizes / sample rates
   - Any FIR filter or normalization artifacts

3. **Known suspect areas** (investigate in order):
   - **OAP scheduler timing**: the 20ms timer (`sleep_until(next)`) drifts on macOS under load; accumulated drift → gaps → stutter
   - **FIR anti-alias filter**: check if the 15-tap Hamming-windowed sinc at 24kHz→8kHz is actually correct (cutoff=3400/12000 = 0.283); verify no DC offset or gain error
   - **Kokoro output normalization**: `output_normalization` in `kokoro-service.cpp` clips to 0.95 ceiling — verify it doesn't clip mid-speech causing flat-tops
   - **Buffer fragmentation**: Kokoro sends chunks of varying sizes; OAP accumulates them; check if small chunks at chunk boundaries cause FIR state contamination
   - **G.711 encode bias**: `linear_to_ulaw()` adds bias=132 — verify it matches the ITU-T standard value (should be 132 = 0x84)
   - **RTP timestamp continuity**: verify OAP always increments `session->ts += 160` even for silence frames

4. **Fix, rebuild, and re-run Stage 7** until the OAP WAV sounds clean (no stutter, no distortion) and the TSP WAV matches closely.

---

## Source Files Modified

| File | Change |
|---|---|
| `tests/test_sip_provider.cpp` | WAV recording feature + HTTP endpoints |
| `outbound-audio-processor.cpp` | WAV recording feature + cmd-port commands |
| `frontend.cpp` | SIP provider + OAP config panels, injection fix, default args, OAP proxy endpoint |
| `sip-client-main.cpp` | Allow 0 lines, no required positional args when lines=0 |
| `tests/run_stage7.py` | New diagnostic collection script |
| `kokoro-service.cpp` | (Conditional) fixes identified by Stage 7 analysis |

---

## API/Interface Changes

### test_sip_provider new HTTP endpoints
- `GET /wav_recording` → `{"enabled":bool,"dir":"string"}`
- `POST /wav_recording` body: `{"enabled":bool,"dir":"string"}` → `{"success":true}`

### OAP new cmd-port commands (port 13152)
- `SAVE_WAV:ON` / `SAVE_WAV:OFF` — enable/disable WAV recording
- `SAVE_WAV:STATUS` — returns current state + dir
- `SET_SAVE_WAV_DIR:<dir>` — set output directory

### Frontend new API endpoints
- `GET /api/oap/wav_recording` → `{"enabled":bool,"dir":"string"}`
- `POST /api/oap/wav_recording` body: `{"enabled":bool,"dir":"string"}` → `{"success":true}`

### sip-client CLI change
- `--lines 0` now valid (starts with no registered SIP lines)
- Positional `<user> <server>` only required when `--lines > 0`

---

## Verification

1. Build: `cd coding.agent && mkdir -p build && cd build && cmake .. && make`
2. `bin/sip-client` starts without arguments (0 lines)
3. `bin/test_sip_provider --save-wav-dir /tmp` starts; `GET http://localhost:22011/wav_recording` responds
4. `bin/outbound-audio-processor --save-wav-dir /tmp` starts; `SAVE_WAV:STATUS` cmd on port 13152 responds
5. Frontend: SIP provider config panel shows when selecting TEST_SIP_PROVIDER service
6. Frontend: OAP config panel shows when selecting OUTBOUND_AUDIO_PROCESSOR service
7. Frontend: Audio Injection label updated; testlines dropdown auto-refreshes on page load
8. `tests/run_stage7.py` runs successfully and produces WAV + log files in output directory
9. Post-fix: OAP WAV files contain intelligible, clean speech (no stutter, no distortion)
10. `bin/test_sip_provider_unit` passes (only present if compiled with GTest)
