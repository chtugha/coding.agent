# Technical Specification: WhisperTalk Frontend Overhaul

## 1. Technical Context

- **Language**: C++17, with HTML/CSS/JS embedded as raw string literals
- **Build system**: CMake 3.22+, single translation unit (`frontend.cpp` ~10,488 lines)
- **Dependencies (compile-time)**: `mongoose.c/.h` (HTTP server), `sqlite3.c/.h` (database), `interconnect.h` (shared protocol)
- **Dependencies (runtime/CDN)**: Bootstrap 5.3.0 JS, Chart.js 4.4.0, chartjs-plugin-zoom 2.0.1, Hammer.js 2.0.8
- **Test framework**: Google Test v1.17 (via FetchContent), enabled with `-DBUILD_TESTS=ON`
- **OS**: macOS (Apple Silicon primary), C++17 with POSIX APIs
- **Existing patterns**: All HTML/CSS/JS returned from `serve_index()`, `build_ui_pages()`, `build_ui_js()` methods as raw string literals (`R"WT(...)WT"`, `R"PG(...)PG"`, `R"JS(...)JS"`)

## 2. Architecture Summary

```
frontend.cpp (single file)
├── Utility functions (escape_json, detect_german, score_llama_response, etc.)
├── class FrontendServer
│   ├── init_database()         — SQLite setup, schema creation, seeding
│   ├── discover_tests()        — hardcoded test binary list
│   ├── load_services()         — read service configs from DB
│   ├── start() / event loop    — mongoose poll loop, timers for flush/rotate/status
│   ├── serve_index()           — full HTML page (CSS + nav + pages)
│   ├── build_ui_pages()        — HTML for all page divs
│   ├── build_ui_js()           — all JS logic (~7000 lines)
│   ├── HTTP handlers           — 40+ API endpoint handlers
│   ├── Log infrastructure      — UDP receiver, queue, flush, SSE, rotation
│   └── Async task system       — background workers for tests/downloads
└── main()                      — signal setup, chdir logic, server instantiation
```

**Key file sections (line ranges)**:
- Lines 1–112: Includes, globals, utility structs (LogEntry, TestInfo, ServiceInfo, TestFileInfo)
- Lines 157–272: Utility functions (escape_json, detect_german, score_llama_response)
- Lines 274–481: FrontendServer class, constructor, async task management
- Lines 483–688: init_database(), DB schema, seeding
- Lines 690–777: discover_tests(), check_test_status(), load_services()
- Lines 800–846: start_service(), kill_ghost_processes(), is_allowed_binary()
- Lines 1263–1415: http_handler() — main router (40+ endpoints)
- Lines 1416–1554: serve_index() — full HTML output (CSS, sidebar nav, theme)
- Lines 1556–2432: build_ui_pages() — HTML for tests, services, beta-testing, models, logs, database, credentials pages
- Lines 2435–3930: build_ui_js() — core JS: navigation, fetchTests, fetchServices, fetchStatus, polling, UI updates
- Lines 3942–4100: SIP lines grid, conference management JS
- Lines 5515–5558: extract_json_string() — hand-rolled JSON parser
- Lines 9423–9462: handle_status() — /api/status endpoint
- Lines 10320–10346: is_read_only_query() — SQL guard
- Lines 10348–10427: handle_db_write_mode(), handle_db_query()
- Lines 10430–10487: main()

## 3. Implementation Approach

All changes are in `frontend.cpp` (source) and test files under `tests/`. No new files are created except artifacts. The build system (`CMakeLists.txt`) is unchanged per requirements.

### Phase 1: SQLite Readonly Fix (R1.x)

**Problem**: `init_database()` at line 484 uses `sqlite3_open("frontend.db", &db_)` — a relative path. Even after chdir logic succeeds, the DB may be opened read-only if file permissions are wrong, and the code doesn't detect this.

**Changes**:

1. **`main()` (lines 10435–10468)**: After chdir logic, resolve `project_root` to an absolute path via `getcwd()`. Pass it to `FrontendServer` constructor.

2. **`FrontendServer` constructor (line 276)**: Accept `std::string project_root` parameter. Store as `project_root_` member.

3. **`init_database()` (line 484)**: 
   - Build absolute DB path: `project_root_ + "/frontend.db"`
   - After `sqlite3_open()` succeeds, call `sqlite3_db_readonly(db_, "main")`. If returns 1, emit fatal error with resolved path and `exit(1)`.
   - Call `sqlite3_db_config(db_, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, NULL)` to disable load_extension.

4. **`main()` chdir logic**: Add handling for absolute path invocation and symlink resolution. After all chdir attempts, verify `bin/frontend` exists at final CWD — if not, emit error and exit.

### Phase 2: Dashboard & Navigation Redesign (R4.x)

**Changes to `serve_index()`** (lines 1416–1554):

1. **New sidebar navigation structure**:
   ```
   Dashboard (default, home icon)       → page-dashboard
   Pipeline:
     Services                            → page-services (existing)
     Live Logs                           → page-logs (existing)
   Testing:
     Test Runner                         → page-tests (existing)
     Test Results                        → page-test-results (NEW)
     Component Tests (tab group)         → page-beta-testing (reorganized)
     Pipeline Tests (tab group)          → merged into beta-testing tabs
   Configuration:
     Models                              → page-models (existing)
     Database                            → page-database (existing)
     Credentials                         → page-credentials (existing)
   ```

2. **New Dashboard page** (add to `build_ui_pages()`):
   - Pipeline diagram: SVG-based horizontal node chain (SIP→IAP→VAD→Whisper→LLaMA→Kokoro/NeuTTS→OAP) with dynamic status dots per service
   - Metrics row: cards showing services online/total, active calls (from SIP stats), recent test pass/fail, system uptime
   - Recent activity feed: last 10 log entries from in-memory ring buffer
   - Quick actions: Start All / Stop All / Run Pipeline Test buttons
   - Color-coded overall health: green/yellow/red badge

3. **New `/api/dashboard` endpoint** (add to `http_handler()`):
   - Returns combined JSON: service status for all 7+ services, recent logs (last 10), test summary (pass/fail counts from DB), uptime
   - Reuses existing data from `services_`, `recent_logs_`, and DB queries
   - Serves the pipeline topology as static JSON (node names + connections)

4. **Dashboard JS** (add to `build_ui_js()`):
   - `fetchDashboard()` — polls `/api/dashboard` every 3s, updates pipeline node colors, metrics, activity feed
   - Pipeline SVG: each node is a `<div>` with CSS class toggled by service online status; connectors are CSS borders/arrows
   - `showPage('dashboard')` becomes the default on load (currently `'tests'`)

5. **Beta Testing reorganization** (modify `build_ui_pages()` lines 1754–2193):
   - Add tab bar at top: "Component Tests" | "Pipeline Tests" | "Tools"
   - Component Tests tab: SIP RTP Routing, IAP Codec Quality, Whisper Accuracy, LLaMA Quality, Kokoro TTS
   - Pipeline Tests tab: Shut-Up Mechanism, Full Pipeline Round-Trip, Pipeline Resilience, Stress Tests
   - Tools tab: Audio Injection, SIP Lines Management, Log Levels
   - Use Bootstrap tabs (already loaded via CDN) — `<ul class="nav nav-tabs">` with tab panes
   - Add test summary bar at top: colored badges showing last run status per test category

6. **New Test Results page** (add to `build_ui_pages()`):
   - Aggregates results from multiple DB tables: `whisper_accuracy_tests`, `model_benchmark_runs`, `iap_quality_tests`, `service_test_runs`
   - Chart.js line/bar charts for trends over time (accuracy, latency, WER)
   - Filter controls: test type dropdown, date range, model selector
   - Pulls pipeline WER results from `/api/test_results` (existing endpoint)

### Phase 3: Code Quality & Security (R2.x)

**C++ Magic Numbers** — Extract to `static constexpr` at class level or file scope:

| Current | Constant Name | Location |
|---------|--------------|----------|
| `500` ms | `LOG_FLUSH_INTERVAL_MS` | line 341 |
| `4096` | `UDP_BUFFER_SIZE` | log receiver |
| `10000` | `DB_QUERY_ROW_LIMIT` | line 10400 |
| `100` | `MG_POLL_TIMEOUT_MS` | line 336 |
| `30` days | `LOG_RETENTION_DAYS` | line 1191 |
| `2` s | `SERVICE_CHECK_INTERVAL_S` | line 345 |
| `30` s | `ASYNC_CLEANUP_INTERVAL_S` | line 349 |
| `100` | `RECENT_LOGS_API_LIMIT` | line 5501 |

**JS Magic Numbers** — Add constants block at start of `build_ui_js()`:

```javascript
var POLL_STATUS_MS=3000, POLL_TESTS_MS=3000, POLL_SERVICES_MS=5000;
var POLL_TEST_LOG_MS=1500, POLL_SIP_STATS_MS=2000;
var DELAY_SERVICE_REFRESH_MS=1000, DELAY_TEST_REFRESH_MS=500;
var DELAY_SIP_LINE_MS=200, TOAST_DURATION_MS=3000;
var POLL_BENCHMARK_MS=2000, POLL_ACCURACY_MS=3000;
var POLL_STRESS_MS=2000, POLL_PIPELINE_HEALTH_MS=10000;
var SIP_MAX_LINES=20, DB_RECONNECT_MS=3000;
// ... (all 45+ occurrences)
```

Replace all numeric literals in `setInterval`/`setTimeout` calls with these constants.

**Security hardening**:

1. **`init_database()`**: Add `sqlite3_db_config(db_, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, NULL)` right after opening.

2. **`is_read_only_query()`** (line 10320): Strip SQL comments (`--` to EOL, `/* ... */`) before keyword check. Add `LOAD_EXTENSION` to the blocked keywords check.

3. **`kill_ghost_processes()`** (line 824): Sanitize `binary_name` — reject any characters outside `[a-zA-Z0-9_.-]` before passing to `popen()`. Use an allow-list approach.

4. **`extract_json_string()`** (line 5515): Add `\b` and `\f` escape handling (currently missing). Already handles `\"`, `\\`, `\n`, `\t`, `\r`, `/`. The function already has bounds checking (`pos < json.size()`) and handles escaped quotes.

**Dead code removal**:
- Full scan for unused functions, unreachable code, commented-out blocks
- Verify all functions in the class are reachable from `http_handler()` or the event loop
- Remove any `#if 0` blocks, dead debug code, or orphaned handler functions

### Phase 4: Test Infrastructure (R3.x)

**Test files affected**: `tests/test_sanity.cpp`, `tests/test_interconnect.cpp`, `tests/test_sip_provider_unit.cpp`, `tests/test_kokoro_cpp.cpp`, `tests/test_integration.cpp`

**Approach**:
1. Attempt `cmake --build` with `-DBUILD_TESTS=ON` to identify compilation errors
2. Fix compilation errors in test files (stale API references to `interconnect.h` changes)
3. Verify port numbers in tests match current `interconnect.h` port map (SIP: 13100, IAP: 13110, VAD: 13115, Whisper: 13120, LLaMA: 13130, Kokoro: 13140, OAP: 13150, Frontend: 13160)
4. Update `test_integration.cpp` Pipeline struct if any service arguments/startup patterns changed
5. Verify `test_sip_provider_unit.cpp` message format expectations match current protocol
6. Frontend test runner: ensure `check_test_status()` and `handle_test_log()` correctly parse output from updated test binaries
7. Check `run_pipeline_test.py` for stale log message format patterns

## 4. Source Code Structure Changes

All changes are within `frontend.cpp`. No new files.

### New Members in `FrontendServer`:
- `std::string project_root_` — absolute path to project root
- `std::string db_path_` — absolute path to `frontend.db`

### New Methods:
- `std::string build_dashboard_page()` — returns HTML for dashboard
- `std::string build_test_results_page()` — returns HTML for test results
- `void handle_dashboard(struct mg_connection *c)` — `/api/dashboard` endpoint

### Modified Methods:
- `FrontendServer(uint16_t, std::string)` — accept project_root
- `init_database()` — use absolute path, verify writable, disable load_extension
- `serve_index()` — new nav structure
- `build_ui_pages()` — add dashboard page, test results page, reorganize beta testing with tabs
- `build_ui_js()` — add dashboard logic, named constants, fix magic numbers, add tab switching
- `http_handler()` — add routes for `/api/dashboard`, default page
- `is_read_only_query()` — strip comments, block LOAD_EXTENSION
- `kill_ghost_processes()` — sanitize binary_name
- `main()` — resolve absolute project root, pass to constructor

## 5. Data Model Changes

### New API Endpoint
- `GET /api/dashboard` — aggregated status JSON:
  ```json
  {
    "services": [{"name":"SIP_CLIENT","online":true,"managed":true}, ...],
    "recent_logs": [...last 10...],
    "test_summary": {"total":5,"passed":3,"failed":2},
    "pipeline": [
      {"id":"SIP_CLIENT","label":"SIP","next":"INBOUND_AUDIO_PROCESSOR"},
      ...
    ]
  }
  ```

### No Schema Changes
All existing tables remain unchanged. Dashboard data is derived from existing tables and in-memory state.

## 6. UI/UX Design

### Dashboard Layout
```
┌─────────────────────────────────────────┐
│  Dashboard                              │
├─────────────────────────────────────────┤
│  ┌─────────────────────────────────┐    │
│  │  Pipeline   (horizontal flow)    │    │
│  │  [SIP]→[IAP]→[VAD]→[ASR]→       │    │
│  │  [LLM]→[TTS]→[OAP]             │    │
│  │  (each node: colored dot)        │    │
│  └─────────────────────────────────┘    │
│                                         │
│  ┌──────┐ ┌──────┐ ┌──────┐ ┌──────┐  │
│  │ Svcs │ │ Calls│ │ Tests│ │Health│   │
│  │ 5/7  │ │  2   │ │ 3/5  │ │  OK  │   │
│  └──────┘ └──────┘ └──────┘ └──────┘  │
│                                         │
│  ┌──────────────┐ ┌──────────────────┐  │
│  │ Quick Actions│ │ Recent Activity  │  │
│  │ Start All    │ │ [log entries]    │  │
│  │ Stop All     │ │                  │  │
│  │ Run Tests    │ │                  │  │
│  └──────────────┘ └──────────────────┘  │
└─────────────────────────────────────────┘
```

### Navigation Sidebar (New)
```
Dashboard          (home icon, default)
──── Pipeline ────
  Services         (gear icon, badge: 5/7)
  Live Logs        (clipboard icon)
──── Testing ─────
  Test Runner      (flask icon, badge: 6)
  Test Results     (chart icon)
  Beta Tests       (target icon)
──── Config ──────
  Models           (robot icon)
  Database         (cabinet icon)
  Credentials      (key icon)
```

### Beta Testing Tabs
```
┌────────────────┬────────────────┬──────────┐
│ Component Tests│ Pipeline Tests │  Tools   │
├────────────────┴────────────────┴──────────┤
│  (tab content — one test card visible)      │
│  Each card: collapsible, with status badge  │
└─────────────────────────────────────────────┘
```

### Visual Design Principles
- Card-based layout (existing `.wt-card` pattern)
- Pipeline nodes: 48px circles with service abbreviation, colored border (green/red/gray)
- Metric cards: large number, small label below, subtle background
- Progressive disclosure: summary up top, drill-down on click
- All existing themes (default, dark, slate, flatly, cyborg) work unchanged — design uses CSS variables already defined
- Subtle CSS transitions for status changes (0.3s ease)

## 7. Delivery Phases

### Phase 1: SQLite Fix (P0)
- Fix `init_database()` with absolute path and readonly check
- Harden `main()` chdir logic
- Disable `load_extension` at init
- **Verification**: Start from `bin/`, `./bin/frontend`, `cd bin && ./frontend`, absolute path — no errors

### Phase 2: Dashboard & Navigation (P1)
- Add Dashboard page with pipeline visualization and metrics
- Reorganize sidebar navigation
- Add Test Results page
- Reorganize Beta Testing with Bootstrap tabs
- Make Dashboard the default page
- **Verification**: Open browser, verify dashboard loads with live status, navigation is logical

### Phase 3: Code Quality & Security (P1)
- Extract C++ magic numbers to named constants
- Extract JS magic numbers to named constants
- Harden `is_read_only_query()` (strip comments, block load_extension)
- Sanitize `kill_ghost_processes()` input
- Improve `extract_json_string()` escape handling
- Remove dead code
- **Verification**: Build succeeds, no regressions in API behavior

### Phase 4: Test Fixes (P1)
- Build tests with `-DBUILD_TESTS=ON`, fix compilation errors
- Update stale port numbers, message formats, API references in test files
- Verify frontend test runner correctly handles test output
- Check `run_pipeline_test.py` for stale patterns
- **Verification**: `cmake --build . --target test_sanity test_interconnect` succeeds, tests pass

## 8. Verification Approach

1. **Build check**: `cmake -DBUILD_TESTS=ON .. && make -j$(nproc)` — must compile cleanly
2. **SQLite fix**: Manual invocation from various directories, check for absence of "readonly database" errors
3. **UI verification**: Open `http://localhost:8080` in browser, verify:
   - Dashboard loads as default page
   - Pipeline diagram shows service status
   - Navigation matches new structure
   - Beta Testing page has tabbed layout
   - Test Results page shows aggregated data
   - All existing pages still work
4. **Test execution**: Run `bin/test_sanity`, `bin/test_interconnect` — verify passing
5. **Code quality**: Grep for remaining numeric literals in `setInterval`/`setTimeout`, verify all replaced
6. **Security**: Verify `is_read_only_query("SELECT load_extension(...)")` returns `false`
