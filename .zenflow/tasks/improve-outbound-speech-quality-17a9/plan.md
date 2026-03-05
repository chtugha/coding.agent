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

Difficulty: **medium** — 4 independent changes across 3 files.

---

### [ ] Step: SIP client starts without default lines

Modify `sip-client-main.cpp`:
- Remove `std::max(1, atoi(optarg))` enforcement → allow `--lines 0`
- Change `main()` arg validation: positional `<user> <server>` only required when `lines > 0`
- Update usage string

Modify `frontend.cpp`:
- Change DB seed `default_args` for `SIP_CLIENT` from `'--lines 2 alice 127.0.0.1 5060'` to `''`
- Add migration to reset any existing `--lines N alice ...` args to `''`

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

Verify: build succeeds; endpoint responds; test that setting enabled=true then starting a call produces WAV files.

---

### [ ] Step: Frontend SIP provider config panel

Modify `frontend.cpp`:
- Add `div#sipProviderConfig` HTML block (parallel to `sipClientConfig`):
  - Checkbox: "Save incoming audio as WAV" (id: `sipProviderSaveWav`)
  - Text input: "Save to directory" (id: `sipProviderWavDir`)
  - Status span: `id="sipProviderWavStatus"`
- In `updateSvcDetail()` JS: show `sipProviderConfig` when `s.name === 'TEST_SIP_PROVIDER'`; call `loadSipProviderWavConfig()`; hide otherwise
- Add JS function `loadSipProviderWavConfig()`: GET from `http://localhost:22011/wav_recording`
- Add JS function `saveSipProviderWavConfig()`: POST to `http://localhost:22011/wav_recording`
- Wire checkbox `onchange` → `saveSipProviderWavConfig()`

Verify: selecting TEST_SIP_PROVIDER service shows the config panel; checkbox/dir persists via HTTP.

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
- Run unit tests: `bin/test_sip_provider_unit` (if it exists in bin/)
- Write report to `.zenflow/tasks/improve-outbound-speech-quality-17a9/report.md`
