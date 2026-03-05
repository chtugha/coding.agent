# Technical Specification: Improve Outbound Speech Quality / Test Infrastructure

## Difficulty: Medium

Multiple independent changes across 3 files with limited per-change complexity but needing careful integration.

---

## Technical Context

- **Language**: C++17, embedded HTML/JavaScript (single-file frontend)
- **Build**: CMake, macOS/Apple Silicon
- **Key files**:
  - `sip-client-main.cpp` — SIP/RTP gateway
  - `tests/test_sip_provider.cpp` — B2BUA test provider (SIP + HTTP control)
  - `frontend.cpp` — Mongoose-based HTTP server with embedded single-page app

---

## Change 1: WAV Recording in test_sip_provider

### Purpose
Allow saving received audio (TTS playback from sip-client) as WAV files per leg per call for offline quality analysis.

### Approach
- Add `--save-wav-dir <dir>` CLI flag (default: empty = disabled)
- Add instance members:
  - `std::atomic<bool> save_wav_enabled_{false}` 
  - `std::string save_wav_dir_` (protected by `save_wav_mutex_`)
  - `std::mutex save_wav_mutex_`
- In `ActiveCall`, add per-leg receive buffer: `std::vector<std::vector<uint8_t>> leg_wav_buffers` (μ-law samples, one vector per leg index)
- In `conference_relay_thread`: when `save_wav_enabled_`, extract 160-byte μ-law payload from each RTP packet (skip 12-byte header) and append to `call->leg_wav_buffers[from_idx]`
- In `shutdown_call()`: if `save_wav_enabled_` and buffers non-empty, write one WAV file per leg:
  - Filename: `call_<id>_<username>_<timestamp>.wav`
  - Decode μ-law → int16 PCM using ITU-T G.711 lookup table (linear decode)
  - Write 8kHz mono 16-bit PCM WAV header + samples
- Add HTTP endpoints on existing mongoose event loop:
  - `GET /wav_recording` → `{"enabled":true/false,"dir":"..."}`
  - `POST /wav_recording {"enabled":true/false,"dir":"/path"}` → `{"success":true}`

### WAV file format
Standard RIFF/WAVE: 44-byte header (chunk sizes filled in), PCM format (audio_format=1), 1 channel, 8000 Hz, 16-bit.

### μ-law decode
Use standard ITU-T G.711 table (reverse of the encode in existing code):
```
int16_t ulaw_to_linear(uint8_t u) {
    u = ~u;
    int sign = u & 0x80;
    int exponent = (u >> 4) & 0x07;
    int mantissa = u & 0x0F;
    int sample = ((mantissa << 3) + 0x84) << exponent;
    return sign ? (0x84 - sample) : (sample - 0x84);
}
```

---

## Change 2: Frontend SIP Provider Config Panel

### Purpose
Expose WAV recording controls in the frontend UI when the TEST_SIP_PROVIDER service is selected.

### Approach
- Add a new `div#sipProviderConfig` div in the HTML (parallel to `div#sipClientConfig`)
  - Checkbox: "Save incoming audio as WAV" (id: `sipProviderSaveWav`)
  - Text input: "Save to directory" (id: `sipProviderWavDir`, placeholder e.g. `/tmp/whispertalk-wav`)
  - Status span: `id="sipProviderWavStatus"`
- Update `updateSvcDetail()` JS function: reveal `sipProviderConfig` when `s.name === 'TEST_SIP_PROVIDER'`, hide otherwise
- Add JS functions:
  - `loadSipProviderWavConfig()`: `GET http://localhost:22011/wav_recording` → populate checkbox + input
  - `saveSipProviderWavConfig()`: `POST http://localhost:22011/wav_recording` with current checkbox/dir values
  - Checkbox `onchange` → call `saveSipProviderWavConfig()`

---

## Change 3: Fix Audio Injection Section in Beta Testing Page

### Current state
- Label: "Inject into Leg (username)"
- Dropdown: hardcoded `<option value="a">Leg A (first)</option>` / `<option value="b">Leg B (second)</option>`
- `refreshInjectLegs()` does fetch `/calls` but falls back to those hardcoded options when no call is active
- `refreshInjectLegs()` is not called on page load

### Fix
1. Change label text to "Inject into active testline"
2. Remove hardcoded fallback options; replace with: `<option value="" disabled>-- No active testlines --</option>`
3. In `refreshInjectLegs()`: always fetch from test_sip_provider; when no calls/legs, show the single disabled placeholder
4. In `showPage()` (line ~2426) and the secondary reload at line ~5198: add `refreshInjectLegs()` call alongside the other beta-testing initialization calls

---

## Change 4: SIP Client Starts Without Default Lines

### Current state
- `sip-client-main.cpp` enforces `std::max(1, atoi(optarg))` for `--lines`, requiring ≥1 line
- `main()` requires `<user> <server>` positional arguments regardless
- Frontend seed: `'--lines 2 alice 127.0.0.1 5060'`

### Fix in sip-client-main.cpp
- Remove `std::max(1, ...)` from the `--lines` handler → allow 0
- Change `main()` argument validation: if `lines == 0`, skip positional args check; only validate user/server when `lines > 0`
- Update usage string to reflect optional positional args

### Fix in frontend.cpp
- Change seed `default_args` for `SIP_CLIENT` from `'--lines 2 alice 127.0.0.1 5060'` to `''`
- Add migration: `UPDATE service_config SET default_args='' WHERE service='SIP_CLIENT' AND (default_args LIKE '%--lines%' OR default_args LIKE '%alice%')`

---

## Source Files Modified

| File | Change |
|---|---|
| `tests/test_sip_provider.cpp` | WAV recording feature + HTTP endpoints |
| `frontend.cpp` | SIP provider config panel, injection fix, default args |
| `sip-client-main.cpp` | Allow 0 lines, no required positional args when lines=0 |

---

## API/Interface Changes

### test_sip_provider new HTTP endpoints
- `GET /wav_recording` → `{"enabled":bool,"dir":"string"}`
- `POST /wav_recording` body: `{"enabled":bool,"dir":"string"}` → `{"success":true}`

### sip-client CLI change
- `--lines 0` now valid (starts with no registered SIP lines)
- Positional `<user> <server>` only required when `--lines > 0`

---

## Verification

1. Build: `cd coding.agent && mkdir -p build && cd build && cmake .. && make`
2. Verify `bin/sip-client` starts without arguments (0 lines)
3. Verify `bin/test_sip_provider --save-wav-dir /tmp` starts and `/wav_recording` endpoint responds
4. Frontend: verify SIP provider config panel appears when selecting TEST_SIP_PROVIDER service
5. Frontend: verify Audio Injection label change and auto-refresh of testlines on page load
6. Run: `bin/test_sip_provider_unit` for unit tests
