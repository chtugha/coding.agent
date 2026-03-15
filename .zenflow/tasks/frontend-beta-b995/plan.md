# Full SDD workflow

## Configuration
- **Artifacts Path**: {@artifacts_path} → `.zenflow/tasks/{task_id}`

---

## Agent Instructions

---

## Workflow Steps

### [x] Step: Requirements
<!-- chat-id: ba05aa73-42ba-4a04-ba02-11cf764d932e -->

Create a Product Requirements Document (PRD) based on the feature description.

1. Review existing codebase to understand current architecture and patterns
2. Analyze the feature definition and identify unclear aspects
3. Ask the user for clarifications on aspects that significantly impact scope or user experience
4. Make reasonable decisions for minor details based on context and conventions
5. If user can't clarify, make a decision, state the assumption, and continue

Save the PRD to `{@artifacts_path}/requirements.md`.

### [x] Step: Technical Specification
<!-- chat-id: 70e2ff12-6acd-4cc4-9bd7-5ecf4b73223e -->

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
<!-- chat-id: 63cfa7b5-b920-4813-ade0-25e12381a957 -->

Create a detailed implementation plan based on `{@artifacts_path}/spec.md`.

**Note on line numbers**: Line references below are from the original `frontend.cpp` (10,488 lines). After each step, subsequent line numbers may shift — implementors should use function/string search rather than relying on exact line numbers for later steps.

### [ ] Step: Fix SQLite readonly database error

Fix the P0 critical bug where `frontend.db` is opened with a relative path and no read-only check.

**File**: `frontend.cpp`

Changes in `main()` (lines 10435–10468):
- After chdir logic completes, resolve an absolute `project_root` via `getcwd()`
- Add symlink resolution via `realpath()` for the executable path
- After all chdir attempts, verify `bin/frontend` exists — if not, emit clear error and exit
- Pass `project_root` string to `FrontendServer` constructor

Changes in `FrontendServer`:
- Add `std::string project_root_` and `std::string db_path_` members
- Modify constructor (line 276) to accept and store `std::string project_root`
- Update `FrontendServer server(port)` call in `main()` (line 10481) to pass `project_root`

Changes in `init_database()` (line 483):
- Build absolute DB path: `db_path_ = project_root_ + "/frontend.db"`
- Replace `sqlite3_open("frontend.db", &db_)` with `sqlite3_open(db_path_.c_str(), &db_)`
- After open succeeds, check `sqlite3_db_readonly(db_, "main") == 1` → emit fatal error with resolved path and `exit(1)`
- Call `sqlite3_db_config(db_, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, NULL)` to disable load_extension

**Verification**: Build with `cmake .. && make -j$(sysctl -n hw.logicalcpu)`. Confirm no compilation errors.

### [ ] Step: C++ code quality — magic numbers, security hardening, dead code removal

All changes in `frontend.cpp`.

**Extract C++ magic numbers** to `static constexpr` at file scope (before class definition):
- `500` ms → `LOG_FLUSH_INTERVAL_MS` (line 341)
- `4096` → `UDP_BUFFER_SIZE` (log receiver)
- `10000` → `DB_QUERY_ROW_LIMIT` (line ~10400)
- `100` → `MG_POLL_TIMEOUT_MS` (line 336)
- `30` days → `LOG_RETENTION_DAYS` (line 1191)
- `2` s → `SERVICE_CHECK_INTERVAL_S` (line 345)
- `30` s → `ASYNC_CLEANUP_INTERVAL_S` (line 349)
- `100` → `RECENT_LOGS_API_LIMIT` (line ~5501)

Replace all corresponding numeric literals with the named constants.

**Security hardening**:
- `is_read_only_query()` (line 10320): Strip SQL comments (`--` to EOL, `/* ... */`) before keyword check. Add check that `upper` does not contain `LOAD_EXTENSION` substring. The `sqlite3_db_config` disable in init_database already blocks it at runtime, but belt-and-suspenders.
- `kill_ghost_processes()` (line 824): Validate `binary_name` against `[a-zA-Z0-9_.-]` regex (no `/` — only bare binary names should be passed). If invalid chars found, return without executing popen.
- `extract_json_string()` (line 5515): Add `\b` (backspace) and `\f` (form-feed) escape cases at lines 5543–5550.

**Dead code removal**:
- Remove `is_service_running_unlocked()` (lines 793–800) — never called anywhere.

**R2.7 — Security assumptions verification**:
- Verify HTTP server binds to `127.0.0.1` (line 324 — already done per requirements, just confirm it hasn't changed).
- Add a code comment at the `mg_http_listen` call documenting the local-only security assumption.

**Verification**: Build with `cmake .. && make -j$(sysctl -n hw.logicalcpu)`. Run `bin/test_sanity` and `bin/test_interconnect` to confirm no regressions.

### [ ] Step: CSS design system, page transitions, and responsive breakpoints

All changes in `frontend.cpp` within `serve_index()` (the `<style>` block, lines 1419–1498).

**New CSS custom properties** — Add to `:root` block (line 1420):
- Gradient vars: `--wt-gradient-hero`, `--wt-gradient-success`, `--wt-gradient-danger`, `--wt-gradient-warning`, `--wt-gradient-info`, `--wt-gradient-neutral`
- Shadow vars: `--wt-shadow-sm`, `--wt-shadow-md`, `--wt-shadow-lg`, `--wt-shadow-glow-success`, `--wt-shadow-glow-danger`
- Surface vars: `--wt-surface-elevated`, `--wt-surface-sunken`
- All per spec section 6.2

**Page transition mechanism** — Replace display-based switching **only for `.wt-page` elements**:
- Change `.wt-page{display:none}` / `.wt-page.active{display:block}` (lines 1497–1498) to visibility/opacity/pointer-events approach per spec section 6.9
- Add `.wt-main{position:relative;overflow-y:auto}` override (line 1437)
- Active page gets `position:relative`, inactive pages get `position:absolute;top:0;left:0;width:100%`
- Add `@keyframes slideIn`, `@keyframes countPulse`, `@keyframes flowPulse` animations
- **KEEP `.hidden{display:none !important}` (line 1496)** — it is used by ~50 `classList.add/remove('hidden')` calls for inline element toggling (service config panels, test detail/overview, schema view, etc.). These are fine-grained show/hide within pages, NOT page-level transitions. Only `.wt-page` uses the new visibility/opacity mechanism.

**New component CSS classes** per spec sections 6.4–6.8:
- `.wt-pipeline-hero`, `.wt-pipeline-node`, `.wt-pipeline-connector` — dashboard pipeline diagram
- `.wt-metric-card`, `.metric-value`, `.metric-label`, `.metric-delta` — dashboard metric cards
- `.wt-collapsible` — expand/collapse for beta test panels
- Nav tab overrides for Beta Testing tabs (`.wt-beta-tabs .nav-link` etc.)
- Card hover lift animation (already partially exists, enhance with transitions)

**Dark theme overrides** per spec section 6.11:
- `[data-bs-theme="dark"]` overrides for pipeline hero, nodes, cards, surfaces

**Responsive breakpoints** per spec section 6.10:
- `@media (max-width:1024px)` — tighter padding, 2-col metrics
- `@media (max-width:768px)` — icon-only sidebar, smaller metric values

**Verification**: Build succeeds. Visual inspection deferred to integration step.

### [ ] Step: Dashboard page — HTML, API endpoint, and JS

**HTML** — Add `build_dashboard_page()` method or add dashboard div to `build_ui_pages()`:
- Pipeline hero section: horizontal node chain (SIP→IAP→VAD→Whisper→LLaMA→Kokoro/NeuTTS→OAP) using CSS flex with `.wt-pipeline-hero`, `.wt-pipeline-node`, `.wt-pipeline-connector` classes. Each node has a status dot with ID for JS updates (e.g., `id="pipeline-node-SIP_CLIENT"`)
- Metrics row: 4 `.wt-metric-card` cards in a CSS grid — Services Online, Active Calls, Test Pass/Fail, Uptime
- Two-column grid below: Activity Feed (left, recent log entries) and Quick Actions (right, Start All / Stop All / Run Pipeline Test / Restart Failed buttons)
- Overall health badge (green/yellow/red) in the hero section

**API endpoint** — Add `handle_dashboard()` method + route in `http_handler()`:
- `GET /api/dashboard` returns JSON combining: service statuses (reuse `services_` vector), recent logs (last 10 from `recent_logs_` ring buffer), test summary (pass/fail counts from `service_test_runs` table), uptime (compute from server start time), pipeline topology (static JSON array of node names and connections)
- Add `std::chrono::steady_clock::time_point start_time_` member initialized in constructor for uptime calculation

**JS** — Add to `build_ui_js()`:
- `fetchDashboard()` function: fetches `/api/dashboard`, updates pipeline node status dots, metric card values (with countUp animation), activity feed entries (with slideIn animation), health badge color
- Poll `fetchDashboard()` every 3s when dashboard page is active (stop polling when switching away)
- Quick action buttons call existing endpoints: `/api/services/start_all`, `/api/services/stop_all`, `/api/tests/start` for pipeline test

**Note**: This step creates the `page-dashboard` div and JS but does NOT yet set it as default — navigation is updated in the next step.

**Verification**: Build succeeds. The `page-dashboard` div exists in the DOM.

### [ ] Step: Navigation sidebar restructure

Changes in `frontend.cpp` within `serve_index()` (lines 1500–1541).

**Depends on**: Dashboard page step (above) must be completed first — this step references `page-dashboard` and `page-test-results` divs that must already exist in `build_ui_pages()`.

**Replace the current sidebar HTML** with the new structure:
```
Dashboard (default, home icon)       → page-dashboard
PIPELINE section:
  Services                            → page-services
  Live Logs                           → page-logs
TESTING section:
  Test Runner                         → page-tests
  Test Results                        → page-test-results (new)
  Beta Tests                          → page-beta-testing
CONFIGURATION section:
  Models                              → page-models
  Database                            → page-database
  Credentials                         → page-credentials
```

- Dashboard nav item gets `active` class by default (instead of Tests)
- Services badge shows `id="svcBadge"`
- Test Runner badge shows `id="testsBadge"`
- Keep theme dropdown and status bar at bottom unchanged

**Update `showPage()` JS** in `build_ui_js()`:
- Change the page transition logic from `display:none/block` to the CSS class-based `active` toggle per spec §6.9. Critical ordering: add `.active` to the new page **before** removing `.active` from the old page, so the container always has exactly one `position:relative` child and never hits a zero-height frame.
- Update default page from `'tests'` to `'dashboard'` in initialization — `showPage('dashboard')` called on page load
- Ensure `showPage()` updates nav item active states correctly for new nav structure

**Verification**: Build succeeds. Dashboard loads as default page on startup.

### [ ] Step: Beta Testing page reorganization with Bootstrap tabs

Changes in `frontend.cpp` within `build_ui_pages()` (lines 1754–2193+ for the beta testing page).

**Add tab bar** at top of `page-beta-testing` div using Bootstrap tabs:
```html
<ul class="nav wt-beta-tabs">
  <li><a class="nav-link active" data-bs-toggle="tab" href="#beta-component">Component Tests</a></li>
  <li><a class="nav-link" data-bs-toggle="tab" href="#beta-pipeline">Pipeline Tests</a></li>
  <li><a class="nav-link" data-bs-toggle="tab" href="#beta-tools">Tools</a></li>
</ul>
<div class="tab-content">...</div>
```

**Organize existing test panels** into tabs:
- **Component Tests tab**: SIP RTP Routing (Test 1), IAP Codec Quality (Test 2), Whisper Accuracy (Test 4), LLaMA Quality (Test 5), Kokoro TTS Quality (Test 7)
- **Pipeline Tests tab**: Shut-Up Mechanism (Tests 5b/6), Full Pipeline Round-Trip (Test 8), Pipeline Resilience (Test 9), Stress Tests (Test 10)
- **Tools tab**: Test Audio Files, Audio Injection, SIP Lines Management grid

**Add test summary bar** above tabs showing colored status dots per test category (reads from last run results in the page state).

**Make test panels collapsible** using `.wt-collapsible` CSS class + JS `maxHeight` toggle per spec section 6.9.

**Verification**: Build succeeds. Open Beta Tests page, confirm tabs work and panels are organized.

### [ ] Step: Test Results page

**HTML** — Add new `page-test-results` div to `build_ui_pages()`:
- Hero row: 3 metric cards (Total Tests, Pass Rate %, Avg Latency) using `.wt-metric-card` style
- Full-width Chart.js line chart for trends over time (WER, accuracy, latency) with gradient fills, bezier curves, zoom plugin
- Filter bar: dropdown for test type, date range inputs, model selector — using `.wt-filter-bar` layout
- Results table below chart with sortable columns, status badges, alternating row shading

**API** — May reuse existing endpoints (`/api/test_results`, `/api/whisper_accuracy`, DB query endpoint) or add a new aggregation endpoint `GET /api/test_results_summary` that queries `whisper_accuracy_tests`, `model_benchmark_runs`, `iap_quality_tests`, `service_test_runs` tables and returns combined JSON.

**JS** — Add to `build_ui_js()`:
- `fetchTestResults()` function: fetches data, renders Chart.js chart with `CHART_DEFAULTS` config per spec section 6.12, populates results table
- Filter change handlers that re-fetch/re-render
- Chart.js instance management (destroy old chart before creating new one to avoid memory leaks)
- Poll on page activation, stop when switching away

**Verification**: Build succeeds. Open Test Results page, confirm chart renders and data loads.

### [ ] Step: JS magic numbers extraction

All changes in `frontend.cpp` within `build_ui_js()` (lines 2435–3930+).

**Add named constants block** at the very start of `build_ui_js()` return value:
```javascript
var POLL_STATUS_MS=3000, POLL_TESTS_MS=3000, POLL_SERVICES_MS=5000;
var POLL_TEST_LOG_MS=1500, POLL_SIP_STATS_MS=2000;
var DELAY_SERVICE_REFRESH_MS=1000, DELAY_TEST_REFRESH_MS=500;
var DELAY_SIP_LINE_MS=200, TOAST_DURATION_MS=3000;
var POLL_BENCHMARK_MS=2000, POLL_ACCURACY_MS=3000;
var POLL_STRESS_MS=2000, POLL_PIPELINE_HEALTH_MS=10000;
var SIP_MAX_LINES=20, DB_RECONNECT_MS=3000;
var DELAY_RESTART_MS=2000, DELAY_SIP_REFRESH_MS=300;
var POLL_SHUTUP_MS=1500, POLL_LLAMA_QUALITY_MS=2000;
var POLL_KOKORO_QUALITY_MS=2000, POLL_KOKORO_BENCH_MS=2000;
var POLL_TTS_ROUNDTRIP_MS=3000, POLL_FULL_LOOP_MS=3000;
var DELAY_SAVE_FEEDBACK_MS=1500, DELAY_MODEL_SELECT_MS=500;
var STATUS_CLEAR_MS=5000, POLL_LLAMA_BENCH_MS=2000;
var POLL_PIPELINE_STRESS_MS=2000;
var COUNTUP_STEP_MS=20, COUNTUP_DURATION_MS=400;
```

**Replace all 45+ numeric literals** in `setInterval()` and `setTimeout()` calls with the corresponding named constants. The spec section 2.2 in requirements.md lists every occurrence with line numbers.

Also replace the hardcoded `20` in SIP line grid/loops with `SIP_MAX_LINES`.

**Verification**: Build succeeds. `grep -n 'setInterval\|setTimeout' frontend.cpp` shows only named constants as the delay argument (no bare numeric literals except 0).

### [ ] Step: Test infrastructure fixes

**R3.1 — Graceful skip guards**:

**File**: `tests/test_integration.cpp`
- Add `GTEST_SKIP()` guard at start of `EndToEndTest.SingleCallFullPipeline` test: check if pipeline services are available (e.g., try connecting to the expected ports). If unavailable, `GTEST_SKIP() << "Pipeline services not running"` instead of timing out after 34s.

**File**: `tests/test_kokoro_cpp.cpp`
- Add `GTEST_SKIP()` guard at start of tests 2–4: check if model files exist at expected paths (`bin/models/kokoro-german/vocab.json`, `bin/models/kokoro-german/df_eva_voice.bin`). If missing, `GTEST_SKIP() << "Model files not found at bin/models/kokoro-german/"`.

**R3.2 — Test runner output format compatibility**:

**File**: `frontend.cpp` — `check_test_status()` and `handle_test_log()`
- Run each passing test binary (`test_sanity`, `test_interconnect`, `test_sip_provider_unit`) from the frontend UI and verify the live output and results are captured correctly. Compare the gtest XML/text output format against what `check_test_status()` expects.
- If any format mismatch is found, update the parsing logic.

**R3.3 — `run_pipeline_test.py` verification**:

**File**: `tests/run_pipeline_test.py`
- Review the log message patterns it expects (e.g., `"Transcription (Xms): <text>"`) against current `flush_log_queue` output format and SSE log format.
- If patterns have drifted, update the regex/string matches in the Python script.
- Verify the API endpoints it calls (`/api/services/start`, `/api/tests/start`, etc.) still match current `http_handler()` routes.

**R3.4 — Test result display verification**:

**File**: `frontend.cpp` — `build_ui_pages()` (tests page) and `build_ui_js()` (fetchTests, test history rendering)
- Verify that test run rows stored in `service_test_runs` DB table are correctly displayed in the Tests page history table.
- Verify `discover_tests()` hardcoded list matches current CMakeLists.txt test targets (currently 6 entries — confirm no new tests have been added).
- R3.5: Add user-visible error message in the frontend when a test binary fails to start (e.g., missing binary, permission denied) — verify `handle_test_start()` error response is displayed in the UI.

**Verification**: Build with `cmake -DBUILD_TESTS=ON .. && make -j$(sysctl -n hw.logicalcpu)`. Run all test binaries:
- `bin/test_sanity` — expect 2/2 PASS
- `bin/test_interconnect` — expect 30/30 PASS
- `bin/test_sip_provider_unit` — expect 25/25 PASS
- `bin/test_integration` — expect SKIP (not FAIL) when services unavailable
- `bin/test_kokoro_cpp` — expect test 1 PASS, tests 2–4 SKIP when model files missing

### [ ] Step: Build verification and visual QA

Final integration verification:

- [ ] Full build: `cmake -DBUILD_TESTS=ON .. && make -j$(sysctl -n hw.logicalcpu)` — zero warnings, zero errors
- [ ] Run all test binaries — confirm pass/skip results match expectations
- [ ] Start frontend from project root: `bin/frontend` — no SQLite errors
- [ ] Start frontend from bin dir: `cd bin && ./frontend` — no SQLite errors
- [ ] Open `http://localhost:8080` in browser:
  - [ ] Dashboard loads as default page with pipeline visualization
  - [ ] Navigation sidebar matches new structure (Dashboard → Pipeline → Testing → Configuration)
  - [ ] Pipeline diagram shows service status nodes with live updates
  - [ ] Metric cards display data with animations
  - [ ] Beta Tests page has 3 tabs (Component, Pipeline, Tools) that switch correctly
  - [ ] Test Results page shows chart and table
  - [ ] All existing pages (Services, Tests, Logs, Database, Credentials, Models) still work
  - [ ] Theme switching still works across all 5 themes
  - [ ] Dark mode renders correctly with overrides
- [ ] Grep for remaining bare numeric literals in setInterval/setTimeout — none found
- [ ] Verify `is_read_only_query("SELECT load_extension('...')")` returns false
