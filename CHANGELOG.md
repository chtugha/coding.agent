# Changelog

## TTS Speed & Naturalness Optimizations (2026-05)

### Added

- **Clause-boundary streaming in `llama-service`** (`llama-service.cpp`): LLaMA now triggers TTS synthesis at commas, semicolons, and German dash patterns (em-dash, en-dash, ` - `) in addition to sentence-end punctuation. Reduces perceived time-to-first-audio by ~100–200 ms. Early-streaming guard raised to `MAX_EARLY_STREAM_CHUNKS = 4` (was 2) to accommodate multi-chunk sentences.

- **Neural G2P for German** (`neural-g2p.h`, `neural-g2p.mm`): New `NeuralG2P` class loads a DeepPhonemizer German checkpoint exported to CoreML (`de_g2p.mlmodelc`). Kokoro and Matcha engines use it when `--g2p neural` is set; espeak-ng remains the fallback for other languages and when the model is absent. The `G2PBackend` enum (`AUTO`, `NEURAL`, `ESPEAK`) is defined in the shared `neural-g2p.h` header consumed by all engine services.
  - Export script: `scripts/export_g2p_model.py` — downloads DeepPhonemizer German checkpoint, converts to CoreML, saves `de_g2p.mlmodelc` + vocab files to `bin/models/g2p/`.

- **Prosody state carryover in Kokoro** (`kokoro-service.cpp`): `KokoroPipeline::synthesize()` now accepts `prev_ref_s` / `ref_s_out` parameters. The `ref_s_out` tensor (256-dim style vector) from chunk N is stored in `CallContext::last_ref_s` and fed as `ref_s` input to chunk N+1's duration model. Intonation continuity across synthesized chunks is preserved without any CoreML model re-export. Thread-safe: each call worker owns its own `ref_s` copy.

- **VITS2 engine** (`vits2-service.cpp`, `libpiper/`): New TTS engine using [Piper TTS](https://github.com/rhasspy/piper) via the `libpiper` C API (ONNX Runtime 1.22). Docks into `tts-service` on cmd port **13175**. Supports German Piper voice models (`.onnx` + `.onnx.json`). `libpiper` is compiled as a static library; ONNX Runtime dylib is bundled at `@executable_path`.
  - Model setup script: `scripts/setup_vits2_models.py` — downloads `de_DE-thorsten-high.onnx` and config to `bin/models/vits2-german/`.

- **Matcha-TTS engine** (`matcha-service.cpp`): New TTS engine based on Matcha-TTS (flow-matching) + HiFi-GAN, both exported to CoreML. ODE flow is baked into a static CoreML graph (10 Euler steps unrolled at export time). Docks into `tts-service` on cmd port **13176**. Three bucketed flow models (3s/5s/10s). Supports neural G2P for German via `neural-g2p.h`.
  - Export script: `scripts/export_matcha_models.py` — exports text encoder, baked ODE flow, HiFi-GAN vocoder, and vocabulary to `bin/models/matcha-german/coreml/`.

- **Port constants** (`tts-common.h`): `kVITS2EngineCmdPort = 13175`, `kMatchaEngineCmdPort = 13176`.

- **Dashboard: per-engine TTS config** (`frontend.cpp`, `frontend-ui.h`, `javascript.h`, `database.h`):
  - Four new REST endpoints: `GET/POST /api/tts/engine_config`, `GET /api/tts/available_voices`, `GET /api/tts/available_g2p`.
  - VITS2 and Matcha config panels in the service detail view (Voice, G2P, Language dropdowns + Save button with async status feedback).
  - Language-switch lifecycle: changing language triggers per-engine preset computation, stops incompatible engines, and restarts the active engine — all in a detached thread so the HTTP response is returned immediately.
  - Engine `disabled_reason` field: greyed-out UI when no compatible model directory is installed for the selected language.
  - Settings persisted in SQLite: `tts_engine_config_{engine}_{voice|g2p|lang}`. Defaults: kokoro=`df_eva/auto/de`, neutts=`default/espeak/de`, vits2=`default/auto/de`, matcha=`default/auto/de`.
  - Input validation: `lang` allowlisted against known codes; `voice`/`g2p_backend` restricted to `[A-Za-z0-9._-]`.

### Changed

- `libpiper/CMakeLists.txt`: `add_library(piper SHARED …)` → `add_library(piper STATIC …)` to satisfy the project's static-linking policy.
- `database.h` seed: Added `VITS2_ENGINE` (`bin/vits2-service`) and `MATCHA_ENGINE` (`bin/matcha-service`) rows alongside existing engine entries.
- `frontend.cpp` service vectors: `all_svcs` and `tts_svcs` now include `VITS2_ENGINE` and `MATCHA_ENGINE`. Engine dispatch in `run_test_setup_async()` and `SET_LOG_LEVEL` handler extended from 2-way to 4-way.

---

## TTS pipeline redesign (2026-04)

### Added
- **Generic TTS stage (`bin/tts-service`)**: new pipeline node sitting between LLaMA and OAP that owns the interconnect sockets (`ServiceType::TTS_SERVICE`, base port 13140) and a dedicated **engine dock** on port 13143. Concrete TTS engines are no longer pipeline nodes — they connect as dock clients.
- **Engine-dock protocol (loopback-only TCP on 13143)**: engines open a TCP connection, send one-line JSON HELLO `{"name":..., "sample_rate":24000, "channels":1, "format":"f32le"}`, and receive `OK\n` or `ERR <reason>\n`. After OK, frames are tag-prefixed (`0x01` = `Packet`, `0x02` = mgmt + optional payload). `127.0.0.1`-only; HELLO name constrained to `[A-Za-z0-9_-]{1..32}`.
- **Single-slot, last-connect-wins state machine**: at most one engine is active. A new engine completing HELLO triggers a swap — the dock sends `CUSTOM SHUTDOWN` to the outgoing engine, a `CUSTOM FLUSH_TTS` to OAP, and switches the slot. Old-engine TCP close is bounded at 2 s before force-close.
- **`GET /api/tts/status`** REST endpoint returning `{"engine":"kokoro"}`, `{"engine":"neutts"}`, or `{"engine":null}`.
- **Per-engine cmd ports**: Kokoro engine on **13144**, NeuTTS engine on **13174** (was 13142 for both before — they shared the pipeline slot).
- **Frontend service table**: `TTS_SERVICE` (dock), `KOKORO_ENGINE` (binary `kokoro-service`), `NEUTTS_ENGINE` (binary `neutts-service`). Dashboard pipeline node shows the currently docked engine under the TTS node, queried from `/api/tts/status`.
- **SPEECH_IDLE warm-up**: LLaMA, Kokoro, NeuTTS, and OAP now each implement a `prewarm_call(call_id)` path that the dock invokes by forwarding SPEECH_IDLE to the active engine. Eliminates several lazy-init hot spots on the first TTS frame of a turn.
- **PacketTrace latency fields**: `t_engine_out` / `t_oap_in` monotonic timestamps for the engine→OAP hop, enabling the median ≤ 2 ms / p99 ≤ 5 ms forwarding-budget verification.

### Changed
- **`ServiceType` renamed**: `KOKORO_SERVICE` → `TTS_SERVICE` (wire value `5` preserved for `PacketTrace` compatibility). `NEUTTS_SERVICE` removed. `service_type_to_string` and `PacketTrace::service_type_name` updated (`"KOK"` → `"TTS"`; `"NEU"` dropped).
- **Upstream / downstream topology**: `LLAMA_SERVICE → TTS_SERVICE → OAP_SERVICE`. Engines are not part of the chain.
- **Engine lifecycle**: Kokoro / NeuTTS use the new header-only `EngineClient` helper (`tts-engine-client.h`) instead of `InterconnectNode`. On `CUSTOM SHUTDOWN` the engine joins synthesis workers, releases model handles, and calls `std::_Exit(0)`.
- **Kokoro / NeuTTS max utterance buffer**: bumped from 10 s to 120 s of PCM (covers worst-case multi-chunk LLaMA responses).

### Removed
- **`/api/switch_tts` REST endpoint** and frontend `handle_switch_tts` handler. Operators switch engines by starting the desired engine process — the dock handles the swap transparently.
- **`InterconnectNode::set_downstream_override` / `clear_downstream_override` / `pause_downstream` / `resume_downstream`** and the corresponding `downstream_override_` / `downstream_paused_` state.
- **LLaMA `SET_TTS:KOKORO` / `SET_TTS:NEUTTS` cmd-port handlers** — redundant now that the downstream resolves to `TTS_SERVICE`.
- **Kokoro `check_tts_exclusion()` startup dance** — the single-slot dock is the sole arbiter.
- **NeuTTS `ServiceType::NEUTTS_SERVICE` port block (13170–13172)**.

### Fixed
- **Dock `std::terminate` on engine disconnect**: the disconnect path reset the slot without joining its recv/ping threads. Replaced with an off-hot-path watcher thread that joins and closes the fd (tracked in `swap_watchers_` so `shutdown()` joins them on dock exit).
- **Engine self-join deadlock on `CUSTOM SHUTDOWN`**: the SHUTDOWN handler ran on the `EngineClient` thread and tried to join itself. Replaced with `shutdown_all_calls()` followed by a clean shutdown sequence; the engine tears down all per-call state before exiting.
- **macOS Keychain TLS key-fetch hang** in headless contexts: `prodigy_tls::get_interconnect_key()` moved to a file-based store (`bin/ic_key.bin`, chmod 600). Unblocks every encrypted interconnect on first call.

## Frontend Overhaul (2026-03)

### Fixed
- **SQLite readonly database error**: Resolved `flush_log_queue: insert failed: attempt to write a readonly database` when started from `bin/` directory. Database path is now resolved to an absolute path based on project root. Added `sqlite3_db_readonly()` check after open — emits a clear fatal error with the resolved path instead of silently failing every 500ms.
- **CWD resolution**: Improved `main()` chdir logic to handle all invocation paths: `bin/frontend`, `./bin/frontend`, `cd bin && ./frontend`, absolute paths, and symlinks. Falls back with clear error if `bin/frontend` is not found at the resolved project root.
- **Test binaries gracefully skip** when dependencies are unavailable: `test_integration` uses `GTEST_SKIP()` when pipeline services are not running; `test_kokoro_cpp` tests 2-4 skip when model files are missing. Previously these timed out or failed with unhelpful errors.
- **Test runner output parsing**: Verified compatibility between gtest output format and `check_test_status()` / `handle_test_log()` parsing logic.
- **Pipeline test script**: Verified `run_pipeline_test.py` log message patterns and API endpoint calls match current implementation.

### Security
- **SQL query guard hardened**: `is_read_only_query()` now strips SQL comments (`--` and `/* */`) before keyword checking to prevent comment-based bypass. Added `LOAD_EXTENSION` substring check.
- **Extension loading disabled**: `sqlite3_db_config(SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0)` called in `init_database()` to disable extension loading at runtime.
- **popen input sanitized**: `kill_ghost_processes()` validates `binary_name` against `[a-zA-Z0-9_.-]` regex before passing to `popen()`. Invalid names are silently rejected.
- **JSON escape handling**: Added `\b` (backspace) and `\f` (form-feed) escape cases to `extract_json_string()`.
- **Local-only binding documented**: Added inline comment at `mg_http_listen` call documenting the `127.0.0.1` security assumption.

### Removed
- **Dead code**: Removed `is_service_running_unlocked()` — was never called anywhere in the codebase.

### Changed
- **C++ magic numbers replaced**: All numeric literals in the event loop, log infrastructure, and test runners extracted to `static constexpr` named constants with descriptive names and unit suffixes (e.g., `LOG_FLUSH_INTERVAL_MS`, `UDP_BUFFER_SIZE`, `SERVICE_CHECK_INTERVAL_S`).
- **JS magic numbers replaced**: All 45+ `setInterval`/`setTimeout` numeric literals extracted to a named-constants block at the top of the JS section (e.g., `POLL_STATUS_MS`, `DELAY_SERVICE_REFRESH_MS`, `SIP_MAX_LINES`).
- **Navigation restructured**: Sidebar reorganized into logical sections: Dashboard (default) > Pipeline (Services, Live Logs) > Testing (Tests) > Configuration (Models, Database, Credentials). The Tests page contains four tabs: Component Tests, Pipeline Tests, Tools, and Test Results.
- **Dashboard is now the default page** (was previously Tests).
- **Page transitions**: Changed from `display:none/block` toggle to CSS `visibility`/`opacity`/`pointer-events` animation for smoother transitions. `.hidden` class retained for inline element toggling.
- **Tests page split into tabs**: Component Tests, Pipeline Tests, Tools, and Test Results organized as tabs with a summary status bar. Test panels are now collapsible. Formerly "Beta Tests"; Test Runner and standalone Test Results pages removed.

### Added
- **Dashboard page**: Pipeline visualization with horizontal node chain showing live service status. Metric cards (Services Online, Running Tests, Tests Passed, Uptime) with count-up animation. Activity feed showing recent log entries. Quick action buttons (Start All, Stop All, Restart Failed). Overall health badge (Healthy/Degraded/Offline).
- **Dashboard API endpoint** (`GET /api/dashboard`): Returns combined JSON with service statuses, recent logs, test summary, uptime, and pipeline topology.
- **Test Results page**: Aggregated test results with summary metric cards, Chart.js trend chart (with zoom plugin), filter bar (test type, date range), and sortable results table.
- **Test Results Summary API endpoint** (`GET /api/test_results_summary`): Queries multiple DB tables (service_test_runs, whisper_accuracy_tests, model_benchmark_runs, iap_quality_tests) and returns unified results with summary statistics.
- **CSS design system**: Comprehensive custom properties for gradients (`--wt-gradient-*`), shadows (`--wt-shadow-*`), surfaces (`--wt-surface-*`), and chart colors (`--wt-chart-*`). New component classes for pipeline nodes, metric cards, collapsible panels, and beta test tabs.
- **Responsive breakpoints**: `@media (max-width:1024px)` for tablet layout, `@media (max-width:768px)` for mobile (icon-only sidebar).
- **Dark theme overrides**: `[data-bs-theme="dark"]` rules for pipeline hero, nodes, cards, and surfaces.
- **Animations**: `slideIn`, `countPulse`, `flowPulse` keyframe animations for dashboard elements.
- **Documentation**: Added `docs/frontend-guide.md` (user-facing) and `docs/frontend-architecture.md` (developer/maintenance) covering all new features and architecture.
