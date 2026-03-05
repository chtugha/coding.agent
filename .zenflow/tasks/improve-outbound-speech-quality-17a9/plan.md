# Spec and build

## Configuration
- **Artifacts Path**: `.zenflow/tasks/improve-outbound-speech-quality-17a9`

---

## Agent Instructions

Ask the user questions when anything is unclear or needs their input.
If blocked, mark the step with `[!]` before stopping.

---

## Workflow Steps

### [x] Step: Technical Specification

See spec at `.zenflow/tasks/improve-outbound-speech-quality-17a9/spec.md`.

Difficulty: **medium** — 6 independent changes across 4 files.

---

### [ ] Step: SIP client starts without default lines

Modify `sip-client-main.cpp`:
- Remove `std::max(1, atoi(optarg))` enforcement → allow `--lines 0`
- Change `main()` arg validation: positional `<user> <server>` only required when `lines > 0`
- Update usage string

Modify `frontend.cpp`:
- Change DB seed `default_args` for `SIP_CLIENT` from `'--lines 2 alice 127.0.0.1 5060'` to `''`
- Add exact-match migration: reset `default_args=''` only where value is exactly `'--lines 1 alice 127.0.0.1 5060'` or `'--lines 2 alice 127.0.0.1 5060'`

Verify: `bin/sip-client` starts with no arguments and exits cleanly (or runs with 0 lines).

---

### [ ] Step: WAV recording in test_sip_provider

Modify `tests/test_sip_provider.cpp`:
- Add `--save-wav-dir <dir>` CLI flag (default: empty = disabled)
- Add `save_wav_enabled_` (atomic bool), `save_wav_dir_` (string), `save_wav_mutex_`
- Add per-leg receive buffer in `ActiveCall`: `std::vector<std::vector<uint8_t>> leg_wav_buffers`
- In `conference_relay_thread`: when enabled, extract 160-byte μ-law payload (skip 12-byte RTP header) and append to `leg_wav_buffers[from_idx]`
- In `shutdown_call()`: write per-leg WAV files (μ-law → int16 PCM → RIFF WAV at 8kHz mono)
- Add HTTP endpoints: `GET /wav_recording` and `POST /wav_recording {"enabled":bool,"dir":"string"}`

Verify: build succeeds; endpoint responds correctly.

---

### [ ] Step: WAV recording in OAP

Modify `outbound-audio-processor.cpp`:
- Add `--save-wav-dir <dir>` CLI flag (default: empty = disabled)
- Add `save_wav_enabled_` (atomic bool), `save_wav_dir_` (string), `save_wav_mutex_` to `OutboundAudioProcessor`
- Add `std::vector<int16_t> wav_samples` to `CallState`
- In `downsample_and_encode_into()`: when enabled, capture each computed `int16_t s16` (8kHz PCM) into `state->wav_samples`
- In `handle_call_end()`: if enabled and `wav_samples` non-empty, write `oap_call_<id>_<timestamp>.wav` (8kHz mono 16-bit PCM RIFF/WAVE)
- Add cmd-port commands in `handle_command()`: `SAVE_WAV:ON`, `SAVE_WAV:OFF`, `SAVE_WAV:STATUS`, `SET_SAVE_WAV_DIR:<dir>`

Modify `frontend.cpp` (backend C++ part):
- Add `handle_oap_wav_recording()` handler (mirrors `handle_whisper_hallucination_filter()` pattern)
  - GET: sends `SAVE_WAV:STATUS` to OAP cmd port (13152) via `tcp_command()`, returns JSON
  - POST: sends `SAVE_WAV:ON/OFF` and `SET_SAVE_WAV_DIR:<dir>` as needed
- Register `/api/oap/wav_recording` in the HTTP route dispatch

Verify: build succeeds; `SAVE_WAV:STATUS` command responds; `/api/oap/wav_recording` endpoint responds.

---

### [ ] Step: Frontend SIP provider config panel

Modify `frontend.cpp` (HTML + JS):
- Add `div#sipProviderConfig` HTML block (parallel to `sipClientConfig`):
  - Checkbox: "Save incoming audio as WAV" (id: `sipProviderSaveWav`)
  - Text input: "Save to directory" (id: `sipProviderWavDir`)
  - Status span: `id="sipProviderWavStatus"`
- In `updateSvcDetail()` JS: show `sipProviderConfig` when `s.name === 'TEST_SIP_PROVIDER'`; call `loadSipProviderWavConfig()`; hide otherwise
- Add JS function `loadSipProviderWavConfig()`: GET using existing `TSP_PORT` JS variable
- Add JS function `saveSipProviderWavConfig()`: POST to same URL
- Wire checkbox `onchange` → `saveSipProviderWavConfig()`

Verify: selecting TEST_SIP_PROVIDER service shows the config panel; checkbox/dir persists via HTTP.

---

### [ ] Step: Frontend OAP config panel

Modify `frontend.cpp` (HTML + JS):
- Add `div#oapConfig` HTML block (parallel to `sipClientConfig` and `whisperConfig`):
  - Checkbox: "Save outgoing audio as WAV" (id: `oapSaveWav`)
  - Text input: "Save to directory" (id: `oapWavDir`)
  - Status span: `id="oapWavStatus"`
- In `updateSvcDetail()` JS: show `oapConfig` when `s.name === 'OUTBOUND_AUDIO_PROCESSOR'`; call `loadOapWavConfig()`; hide otherwise
- Add JS function `loadOapWavConfig()`: `GET /api/oap/wav_recording`
- Add JS function `saveOapWavConfig()`: `POST /api/oap/wav_recording`
- Wire checkbox `onchange` → `saveOapWavConfig()`

Verify: selecting OUTBOUND_AUDIO_PROCESSOR service shows the config panel.

---

### [ ] Step: Fix audio injection UI on beta-testing page

Modify `frontend.cpp`:
- Change label "Inject into Leg (username)" → "Inject into active testline"
- Replace hardcoded `<option value="a">Leg A (first)</option>` / `<option value="b">Leg B (second)</option>` with a single disabled placeholder: `<option value="" disabled>-- No active testlines --</option>`
- Update `refreshInjectLegs()` fallback: when no active call/legs, render only the disabled placeholder
- Add `refreshInjectLegs()` to beta-testing page load handlers (two locations: line ~2426 and ~5198)

Verify: on page load the dropdown shows active testlines (or the placeholder); label is updated.

---

### [ ] Step: Build and verify

- Run full build: `cd coding.agent && mkdir -p build && cd build && cmake .. && make -j4`
- Fix any compilation errors
- Run unit tests: `bin/test_sip_provider_unit` (only present if compiled with GTest; skip if not available)
- Write report to `.zenflow/tasks/improve-outbound-speech-quality-17a9/report.md`
