# Technical Specification: Improve Outbound Speech Quality / Test Infrastructure

## Difficulty: Medium

Multiple independent changes across 4 files with limited per-change complexity but needing careful integration.

---

## Technical Context

- **Language**: C++17, embedded HTML/JavaScript (single-file frontend)
- **Build**: CMake, macOS/Apple Silicon
- **Key files**:
  - `sip-client-main.cpp` — SIP/RTP gateway
  - `tests/test_sip_provider.cpp` — B2BUA test provider (SIP + HTTP control)
  - `outbound-audio-processor.cpp` — 24kHz→8kHz downsampler + G.711 encoder
  - `frontend.cpp` — Mongoose-based HTTP server with embedded single-page app

---

## Change 1: WAV Recording in test_sip_provider

### Purpose
Allow saving received audio (TTS playback from sip-client, as received at the relay ports) as WAV files per leg per call for offline quality analysis.

### Approach
- Add `--save-wav-dir <dir>` CLI flag (default: empty = disabled)
- Add instance members:
  - `std::atomic<bool> save_wav_enabled_{false}`
  - `std::string save_wav_dir_` protected by `save_wav_mutex_`
  - `std::mutex save_wav_mutex_`
- In `ActiveCall`, add per-leg receive buffer: `std::vector<std::vector<uint8_t>> leg_wav_buffers` (μ-law samples, one vector per leg index); initialized in `initiate_conference()`
- In `conference_relay_thread`: when `save_wav_enabled_`, extract 160-byte μ-law payload from each RTP packet (skip 12-byte header) and append to `call->leg_wav_buffers[from_idx]`
- In `shutdown_call()`: if `save_wav_enabled_` and buffers non-empty, write one WAV file per leg:
  - Filename: `call_<id>_<username>_<timestamp>.wav`
  - Decode μ-law → int16 PCM using ITU-T G.711 decode
  - Write 8kHz mono 16-bit PCM RIFF/WAVE file
- Add HTTP endpoints on existing mongoose event loop:
  - `GET /wav_recording` → `{"enabled":true/false,"dir":"..."}`
  - `POST /wav_recording {"enabled":true/false,"dir":"/path"}` → `{"success":true}`

### WAV file format
Standard RIFF/WAVE: 44-byte header, PCM format (audio_format=1), 1 channel, 8000 Hz, 16-bit little-endian.

### μ-law decode
```cpp
int16_t ulaw_to_linear(uint8_t u) {
    u = ~u;
    int sign = u & 0x80;
    int exponent = (u >> 4) & 0x07;
    int mantissa = u & 0x0F;
    int sample = ((mantissa << 3) + 0x84) << exponent;
    return sign ? (int16_t)(0x84 - sample) : (int16_t)(sample - 0x84);
}
```

---

## Change 2: WAV Recording in OAP (outbound-audio-processor.cpp)

### Purpose
Allow saving the downsampled 8kHz PCM audio (the exact audio sent to the caller's phone, before G.711 encode) as WAV files per call, for diagnosing outbound TTS audio quality.

### Approach
- Add `--save-wav-dir <dir>` CLI flag (default: empty = disabled)
- Add instance members in `OutboundAudioProcessor`:
  - `std::atomic<bool> save_wav_enabled_{false}`
  - `std::string save_wav_dir_` protected by `save_wav_mutex_`
  - `std::mutex save_wav_mutex_`
- Add `wav_samples` field to `CallState`: `std::vector<int16_t> wav_samples`
- In `downsample_and_encode_into()`: after computing each `int16_t s16` (clipped PCM at 8kHz), when `save_wav_enabled_`, append it to `state->wav_samples`
- In `handle_call_end()`: if enabled and `wav_samples` non-empty, write WAV file:
  - Filename: `oap_call_<id>_<timestamp>.wav`
  - Format: 8kHz mono 16-bit PCM RIFF/WAVE (same structure as Change 1)
- Add cmd-port commands (consistent with `SET_SIDETONE_GUARD_MS` pattern) in `handle_command()`:
  - `SAVE_WAV:ON` → enable, return `OK\n`
  - `SAVE_WAV:OFF` → disable, return `OK\n`
  - `SAVE_WAV:STATUS` → return `SAVE_WAV:ON\n` or `SAVE_WAV:OFF\n` plus dir
  - `SET_SAVE_WAV_DIR:<dir>` → set directory, return `OK\n`

### Frontend proxy
- Add a new frontend API endpoint `POST /api/oap/wav_recording` and `GET /api/oap/wav_recording` in `frontend.cpp` (C++ backend handler `handle_oap_wav_recording()`), following the same pattern as `handle_whisper_hallucination_filter()`:
  - GET: sends `SAVE_WAV:STATUS` to OAP cmd port 13152 via `tcp_command()`, returns JSON
  - POST: sends `SAVE_WAV:ON` or `SAVE_WAV:OFF` and optionally `SET_SAVE_WAV_DIR:<dir>`, returns JSON

---

## Change 3: Frontend SIP Provider Config Panel

### Purpose
Expose WAV recording controls in the frontend UI when the TEST_SIP_PROVIDER service is selected.

### Approach
- Add a new `div#sipProviderConfig` div in the HTML (parallel to `div#sipClientConfig`)
  - Checkbox: "Save incoming audio as WAV" (id: `sipProviderSaveWav`)
  - Text input: "Save to directory" (id: `sipProviderWavDir`, placeholder `/tmp/whispertalk-wav`)
  - Status span: `id="sipProviderWavStatus"`
- Update `updateSvcDetail()` JS function: reveal `sipProviderConfig` when `s.name === 'TEST_SIP_PROVIDER'`, hide otherwise; call `loadSipProviderWavConfig()`
- Add JS functions using existing `TSP_PORT` JS variable (not hardcoded 22011):
  - `loadSipProviderWavConfig()`: `GET http://localhost:'+TSP_PORT+'/wav_recording` → populate checkbox + input
  - `saveSipProviderWavConfig()`: `POST http://localhost:'+TSP_PORT+'/wav_recording` with current checkbox/dir values
  - Checkbox `onchange` → call `saveSipProviderWavConfig()`

---

## Change 4: Frontend OAP Config Panel

### Purpose
Expose WAV recording controls in the frontend UI when the OUTBOUND_AUDIO_PROCESSOR service is selected.

### Approach
- Add a new `div#oapConfig` div in the HTML (parallel to `div#sipClientConfig` and `div#whisperConfig`)
  - Checkbox: "Save outgoing audio as WAV" (id: `oapSaveWav`)
  - Text input: "Save to directory" (id: `oapWavDir`, placeholder `/tmp/whispertalk-wav`)
  - Status span: `id="oapWavStatus"`
- Update `updateSvcDetail()` JS function: reveal `oapConfig` when `s.name === 'OUTBOUND_AUDIO_PROCESSOR'`, hide otherwise; call `loadOapWavConfig()`
- Add JS functions calling the frontend proxy endpoints:
  - `loadOapWavConfig()`: `GET /api/oap/wav_recording` → populate checkbox + input
  - `saveOapWavConfig()`: `POST /api/oap/wav_recording` with current checkbox/dir values
  - Checkbox `onchange` → call `saveOapWavConfig()`
- Register `/api/oap/wav_recording` in the frontend's HTTP route handler dispatch

---

## Change 5: Fix Audio Injection Section in Beta Testing Page

### Current state
- Label: "Inject into Leg (username)"
- Dropdown: hardcoded `<option value="a">Leg A (first)</option>` / `<option value="b">Leg B (second)</option>`
- `refreshInjectLegs()` does fetch `/calls` from test_sip_provider but falls back to the hardcoded options when no call is active
- `refreshInjectLegs()` is not called on page load

### Fix
1. Change label text to "Inject into active testline"
2. Remove hardcoded fallback options; replace with: `<option value="" disabled>-- No active testlines --</option>`
3. In `refreshInjectLegs()`: when no active call/legs, show only the disabled placeholder
4. In `showPage()` (line ~2426) and the secondary reload at line ~5198: add `refreshInjectLegs()` call

---

## Change 6: SIP Client Starts Without Default Lines

### Current state
- `sip-client-main.cpp` enforces `std::max(1, atoi(optarg))` for `--lines`, requiring ≥1 line
- `main()` requires `<user> <server>` positional arguments regardless
- Frontend DB seed: `'--lines 2 alice 127.0.0.1 5060'`

### Fix in sip-client-main.cpp
- Remove `std::max(1, ...)` from `--lines` handler → allow 0
- Change `main()` argument validation: positional `<user> <server>` only required when `lines > 0`
- Update usage string to reflect optional positional args

### Fix in frontend.cpp
- Change DB seed `default_args` for `SIP_CLIENT` from `'--lines 2 alice 127.0.0.1 5060'` to `''`
- Add exact-match migration (avoids false positives on custom args):
  ```sql
  UPDATE service_config SET default_args='' WHERE service='SIP_CLIENT'
    AND (default_args='--lines 1 alice 127.0.0.1 5060'
         OR default_args='--lines 2 alice 127.0.0.1 5060');
  ```

---

## Source Files Modified

| File | Change |
|---|---|
| `tests/test_sip_provider.cpp` | WAV recording feature + HTTP endpoints |
| `outbound-audio-processor.cpp` | WAV recording feature + cmd-port commands |
| `frontend.cpp` | SIP provider + OAP config panels, injection fix, default args, OAP proxy endpoint |
| `sip-client-main.cpp` | Allow 0 lines, no required positional args when lines=0 |

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
2. Verify `bin/sip-client` starts without arguments (0 lines)
3. Verify `bin/test_sip_provider --save-wav-dir /tmp` starts and `GET /wav_recording` responds
4. Verify `bin/outbound-audio-processor --save-wav-dir /tmp` starts and `SAVE_WAV:STATUS` cmd responds
5. Frontend: verify SIP provider config panel appears when selecting TEST_SIP_PROVIDER service
6. Frontend: verify OAP config panel appears when selecting OUTBOUND_AUDIO_PROCESSOR service
7. Frontend: verify Audio Injection label change and auto-refresh of testlines on page load
8. Run: `bin/test_sip_provider_unit` for unit tests (binary only present if compiled with GTest; skip if not available)
