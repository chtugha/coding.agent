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

Difficulty: **hard** — 6 infrastructure changes across 4 files + an iterative diagnostic/fix cycle for audio quality.

---

### [x] Step: SIP client starts without default lines
<!-- chat-id: 44d50f9e-ab73-4466-bf2c-90b454a9c101 -->

Modify `sip-client-main.cpp`:
- Remove `std::max(1, atoi(optarg))` enforcement → allow `--lines 0`
- Change `main()` arg validation: positional `<user> <server>` only required when `lines > 0`
- Update usage string

Modify `frontend.cpp`:
- Change DB seed `default_args` for `SIP_CLIENT` to `''`
- Add exact-match migration: reset only where value is exactly `'--lines 1 alice 127.0.0.1 5060'` or `'--lines 2 alice 127.0.0.1 5060'`

Verify: `bin/sip-client` starts with no arguments and runs with 0 lines.

---

### [ ] Step: WAV recording in test_sip_provider

Modify `tests/test_sip_provider.cpp`:
- Add `--save-wav-dir <dir>` CLI flag (default: empty = disabled)
- Add `save_wav_enabled_`, `save_wav_dir_`, `save_wav_mutex_` to `TestSipProvider`
- Add `std::vector<std::vector<uint8_t>> leg_wav_buffers` to `ActiveCall` (one per leg)
- In `conference_relay_thread`: when enabled, extract 160-byte μ-law payload (skip 12-byte RTP header) and append to `leg_wav_buffers[from_idx]`
- In `shutdown_call()`: write per-leg WAV files (μ-law → int16 PCM → 8kHz mono RIFF/WAVE), filename prefix `tsp_call_`
- Add HTTP `GET /wav_recording` and `POST /wav_recording` endpoints

Verify: build succeeds; endpoint responds; WAV files created on call hangup when enabled.

---

### [ ] Step: WAV recording in OAP

Modify `outbound-audio-processor.cpp`:
- Add `--save-wav-dir <dir>` CLI flag (default: empty = disabled)
- Add `save_wav_enabled_`, `save_wav_dir_`, `save_wav_mutex_` to `OutboundAudioProcessor`
- Add `std::vector<int16_t> wav_samples` to `CallState`
- In `downsample_and_encode_into()`: when enabled, append each computed `int16_t s16` to `state.wav_samples` (`state` is a `CallState&` reference)
- In `handle_call_end()`: take a local `shared_ptr` copy of the CallState, erase from `calls_` map, release the lock, then write `oap_call_<id>_<timestamp>.wav` outside the lock using the local copy (prevents `wav_samples` being destroyed before write)
- Add cmd-port commands in `handle_command()`: `SAVE_WAV:ON`, `SAVE_WAV:OFF`, `SAVE_WAV:STATUS`, `SET_SAVE_WAV_DIR:<dir>`

Modify `frontend.cpp` (C++ backend):
- Add `handle_oap_wav_recording()` handler (mirrors `handle_whisper_hallucination_filter()`)
  - GET: sends `SAVE_WAV:STATUS` to port 13152 via `tcp_command()`, returns JSON
  - POST: sends `SET_SAVE_WAV_DIR:<dir>` + `SAVE_WAV:ON/OFF`, returns JSON
- Register `/api/oap/wav_recording` in HTTP route dispatch

Verify: build succeeds; `SAVE_WAV:STATUS` responds on port 13152; `/api/oap/wav_recording` responds.

---

### [ ] Step: Frontend SIP provider config panel

Modify `frontend.cpp` (HTML + JS):
- Add `div#sipProviderConfig` HTML block (parallel to `sipClientConfig`):
  - Checkbox "Save incoming audio as WAV" (id: `sipProviderSaveWav`)
  - Text input "Save to directory" (id: `sipProviderWavDir`)
  - Status span `id="sipProviderWavStatus"`
- In `updateSvcDetail()` JS: show `sipProviderConfig` + call `loadSipProviderWavConfig()` when `s.name === 'TEST_SIP_PROVIDER'`; hide otherwise
- Add JS `loadSipProviderWavConfig()`: GET from `'http://localhost:'+TSP_PORT+'/wav_recording'`
- Add JS `saveSipProviderWavConfig()`: POST to same URL
- Wire checkbox `onchange` → `saveSipProviderWavConfig()`

Verify: selecting TEST_SIP_PROVIDER service shows config panel; checkbox/dir persists.

---

### [ ] Step: Frontend OAP config panel

Modify `frontend.cpp` (HTML + JS):
- Add `div#oapConfig` HTML block (parallel to `sipClientConfig`, `whisperConfig`):
  - Checkbox "Save outgoing audio as WAV" (id: `oapSaveWav`)
  - Text input "Save to directory" (id: `oapWavDir`)
  - Status span `id="oapWavStatus"`
- In `updateSvcDetail()` JS: show `oapConfig` + call `loadOapWavConfig()` when `s.name === 'OUTBOUND_AUDIO_PROCESSOR'`; hide otherwise
- Add JS `loadOapWavConfig()`: GET from `/api/oap/wav_recording`
- Add JS `saveOapWavConfig()`: POST to `/api/oap/wav_recording`
- Wire checkbox `onchange` → `saveOapWavConfig()`

Verify: selecting OUTBOUND_AUDIO_PROCESSOR service shows config panel.

---

### [ ] Step: Fix audio injection UI on beta-testing page

Modify `frontend.cpp`:
- Change label "Inject into Leg (username)" → "Inject into active testline"
- Replace hardcoded `<option value="a">Leg A (first)</option>` / `<option value="b">Leg B (second)</option>` with: `<option value="" disabled>-- No active testlines --</option>`
- Update `refreshInjectLegs()` fallback to render only the disabled placeholder when no call is active
- Add `refreshInjectLegs()` to both beta-testing page load handlers (~line 2426 and ~line 5198)

Verify: label updated; dropdown shows active testlines on page load (or placeholder when none).

---

### [ ] Step: Stage 7a — Implement diagnostic collection script

Create `tests/run_stage7.py`:
- Start all 7 services + test_sip_provider via frontend API
- Wait 10s for warmup
- Connect 1 line via test_sip_provider `/conference`
- Enable WAV saving in test_sip_provider and OAP
- Inject 1 sample (no_silence=true)
- Poll until injection complete + 5s buffer for Kokoro/OAP finish
- Collect logs from frontend API
- Hang up (WAV files are written on hangup)
- Copy WAV files to run output directory
- Repeat 10 times (configurable via `--iterations N`)
- Output: `stage7_output/run_N/pipeline.log`, `tsp_call_*.wav`, `oap_call_*.wav`

Verify: script runs end-to-end and produces WAV + log files.

---

### [ ] Step: Stage 7b — Build, run collection, diagnose and fix audio quality

1. Build everything: `cd coding.agent && mkdir -p build && cd build && cmake .. && make -j4`
2. Run `python3 tests/run_stage7.py` to collect 10 data samples
3. Inspect OAP WAV files to isolate the distortion stage:
   - OAP WAV clean → problem is in G.711 encode, RTP framing, or sip-client re-encode
   - OAP WAV distorted → problem is in Kokoro output, OAP downsampler, or scheduler timing
4. Inspect TSP WAV files and compare with OAP WAV
5. Analyze logs for buffer underruns, timing jitter, chunk size anomalies
6. Fix identified issues (suspects: OAP scheduler drift, FIR filter coefficients, Kokoro normalization, buffer fragmentation at chunk boundaries, G.711 bias)
7. Rebuild and re-run Stage 7 until OAP and TSP WAV files contain clean, natural speech
8. Write report to `.zenflow/tasks/improve-outbound-speech-quality-17a9/report.md`
