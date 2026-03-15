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

**Dead code removal** — concrete findings from codebase scan:

1. **`is_service_running_unlocked()`** (line 793): Defined but **never called** anywhere. A dead variant of `is_service_running()` (line 6800) which IS used. Remove the unlocked version.

2. **`extract_json_string()`** (line 5515): Missing `\b` and `\f` escape handling. The escape switch at lines 5543–5550 handles `\"`, `\\`, `\n`, `\t`, `\r`, `/` but silently drops `\b` (backspace) and `\f` (form-feed) — the fallback at line 5550 just emits the backslash character. Add explicit `\b` and `\f` cases.

3. **No `#if 0` blocks, no `#ifdef` conditionals, no commented-out code blocks** found in `frontend.cpp`. The file is clean of preprocessor dead code.

4. **No TODO/FIXME/HACK/STUB/SIMULATE markers** found in `frontend.cpp`.

5. **All 93 `void` methods, 47 `std::string` methods, 11 `bool` methods, and 5 `int` methods** are reachable from either `http_handler()`, the event loop, or the constructor. Verified by cross-referencing each function definition against call sites.

6. **All 129 JS functions** defined in `build_ui_js()` are referenced from either HTML `onclick` handlers in `build_ui_pages()`, other JS functions, or the initialization code at the end of the JS block. No orphaned JS functions found.

### Phase 4: Test Infrastructure (R3.x)

**Test files affected**: `tests/test_sanity.cpp`, `tests/test_interconnect.cpp`, `tests/test_sip_provider_unit.cpp`, `tests/test_kokoro_cpp.cpp`, `tests/test_integration.cpp`

**Build results** (actual `cmake -DBUILD_TESTS=ON` build executed):

All 5 test targets compile successfully with zero errors:
- `test_sanity` — compiles clean
- `test_interconnect` — compiles clean
- `test_sip_provider_unit` — compiles clean
- `test_integration` — compiles clean
- `test_kokoro_cpp` — compiles clean (conditional on kokoro-service target)

**Runtime test results** (actual execution):

| Test Binary | Tests | Result | Details |
|-------------|-------|--------|---------|
| `test_sanity` | 2/2 | **PASS** | All assertions pass |
| `test_interconnect` | 30/30 | **PASS** | All port/protocol tests pass |
| `test_sip_provider_unit` | 25/25 | **PASS** | All message format tests pass |
| `test_integration` | 1/1 | **FAIL** | `EndToEndTest.SingleCallFullPipeline` — `pipeline.all_alive()` returns false; services crash during startup (missing model files/dependencies in CI), test times out after 34s |
| `test_kokoro_cpp` | 1/4 | **FAIL** | Tests 2–4 fail: model files missing (`vocab.json`, `df_eva_voice.bin` not found at `bin/models/kokoro-german/`). Test 1 (sanity) passes. |

**Other test infrastructure**:
- `test_sip_provider` binary: defined in CMakeLists.txt (line 347) but NOT built as a test target (it's a runtime tool, not a gtest). Binary exists in `bin/` only if manually built.
- `run_pipeline_test.py`: exists at `tests/run_pipeline_test.py`. Not tested in this analysis (requires full running pipeline).

**Concrete Phase 4 tasks**:
1. `test_integration` failure is environmental — requires all pipeline services + model files to be available. The test code itself compiles and links correctly. **Action**: Add a prerequisite check in the test (or skip message) when services aren't available, rather than timing out silently.
2. `test_kokoro_cpp` failures are due to missing model files at expected paths. **Action**: Add `GTEST_SKIP()` guard when model files don't exist, so tests skip cleanly instead of failing with file-not-found.
3. Frontend test runner (`discover_tests()` at line 692) hardcodes 6 test binaries. **Action**: No code changes needed — the list matches CMakeLists.txt test targets. Consider adding `test_sip_provider` to the list since it exists as a runtime tool.
4. `check_test_status()` and `handle_test_log()` correctly parse gtest output — verified by successful test runs above.
5. Port numbers in tests match `interconnect.h` port map — verified by `test_interconnect` passing all 30 tests.

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

## 6. UI/UX Design — Visual Design System

Inspired by [informationisbeautiful.net](https://informationisbeautiful.net): data-forward presentation, bold typography, generous whitespace, vibrant color gradients, and card-based layouts that make complex data instantly comprehensible. Every screen should feel like a polished data visualization, not a developer admin panel.

### 6.1 Design Philosophy

1. **Data is the hero**: Numbers, status indicators, and charts dominate. Labels are secondary. The most important metric on any card should be the largest visual element.
2. **Progressive disclosure**: Summary → Detail. Dashboard shows health at a glance; click to drill into specifics. No walls of text.
3. **Color communicates meaning**: Status colors are immediate and unambiguous. Gradients add depth and visual interest without sacrificing clarity.
4. **Generous breathing room**: Whitespace is a feature, not waste. Cards have padding, sections have gaps, elements don't crowd.
5. **Fluid motion**: Subtle animations make the UI feel alive — status transitions, data updates, page switches.

### 6.2 Color System

**New CSS custom properties** (added to `:root` in `serve_index()`):

```css
:root {
  /* Existing vars preserved, plus: */

  /* Gradient palette (inspired by informationisbeautiful's bold color blocks) */
  --wt-gradient-hero: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
  --wt-gradient-success: linear-gradient(135deg, #11998e 0%, #38ef7d 100%);
  --wt-gradient-danger: linear-gradient(135deg, #eb3349 0%, #f45c43 100%);
  --wt-gradient-warning: linear-gradient(135deg, #f7971e 0%, #ffd200 100%);
  --wt-gradient-info: linear-gradient(135deg, #2193b0 0%, #6dd5ed 100%);
  --wt-gradient-neutral: linear-gradient(135deg, #bdc3c7 0%, #2c3e50 100%);
  --wt-gradient-pipeline: linear-gradient(90deg, #667eea, #764ba2, #f093fb, #f5576c, #fda085, #f9d423, #38ef7d);

  /* Semantic surface colors */
  --wt-surface-elevated: rgba(255,255,255,0.85);
  --wt-surface-sunken: rgba(0,0,0,0.02);

  /* Chart colors — 7 distinct hues for 7 pipeline services */
  --wt-chart-1: #667eea;  /* SIP - indigo */
  --wt-chart-2: #764ba2;  /* IAP - purple */
  --wt-chart-3: #f093fb;  /* VAD - pink */
  --wt-chart-4: #43e97b;  /* Whisper - green */
  --wt-chart-5: #fa709a;  /* LLaMA - rose */
  --wt-chart-6: #fee140;  /* Kokoro - gold */
  --wt-chart-7: #30cfd0;  /* OAP - cyan */

  /* Shadows */
  --wt-shadow-sm: 0 1px 3px rgba(0,0,0,0.04), 0 1px 2px rgba(0,0,0,0.06);
  --wt-shadow-md: 0 4px 16px rgba(0,0,0,0.08), 0 2px 4px rgba(0,0,0,0.04);
  --wt-shadow-lg: 0 12px 40px rgba(0,0,0,0.12), 0 4px 8px rgba(0,0,0,0.06);
  --wt-shadow-glow-success: 0 0 20px rgba(52,199,89,0.3);
  --wt-shadow-glow-danger: 0 0 20px rgba(255,59,48,0.3);
}

[data-bs-theme="dark"] {
  --wt-surface-elevated: rgba(50,50,52,0.85);
  --wt-surface-sunken: rgba(255,255,255,0.03);
  --wt-shadow-sm: 0 1px 3px rgba(0,0,0,0.2);
  --wt-shadow-md: 0 4px 16px rgba(0,0,0,0.3);
  --wt-shadow-lg: 0 12px 40px rgba(0,0,0,0.4);
}
```

### 6.3 Typography Scale

All text uses the existing `--wt-font` (SF Pro / system) stack. New size classes:

| Class | Size | Weight | Use |
|-------|------|--------|-----|
| `.wt-display` | 48px | 800 | Hero metric number on Dashboard |
| `.wt-headline` | 28px | 700 | Page titles (existing `wt-page-title`) |
| `.wt-title-lg` | 20px | 600 | Section headers, card group titles |
| `.wt-title` | 15px | 600 | Card titles (existing `wt-card-title`) |
| `.wt-body` | 13px | 400 | Body text, table cells |
| `.wt-caption` | 11px | 500 | Labels, badges, timestamps |
| `.wt-micro` | 10px | 600 | Tiny annotations, sparkline labels |

All with `letter-spacing: -0.01em` for tighter, more modern feel. Display and headline use `letter-spacing: -0.03em`.

```css
.wt-display { font-size:48px; font-weight:800; letter-spacing:-0.03em; line-height:1; }
.wt-headline { font-size:28px; font-weight:700; letter-spacing:-0.02em; }
.wt-title-lg { font-size:20px; font-weight:600; letter-spacing:-0.01em; }
.wt-caption { font-size:11px; font-weight:500; text-transform:uppercase; letter-spacing:0.5px; color:var(--wt-text-secondary); }
.wt-micro { font-size:10px; font-weight:600; letter-spacing:0.3px; }
```

### 6.4 Dashboard Page — Detailed Layout

The dashboard is a full-bleed page (no `max-width:960px` constraint) with a gradient hero section at top, then a responsive grid below.

```
┌──────────────────────────────────────────────────────────┐
│  HERO SECTION (gradient background --wt-gradient-hero)   │
│  ┌────────────────────────────────────────────────────┐  │
│  │  Pipeline Flow Visualization                       │  │
│  │                                                    │  │
│  │  ┌─────┐    ┌─────┐    ┌─────┐    ┌─────┐        │  │
│  │  │ SIP │───▶│ IAP │───▶│ VAD │───▶│ ASR │        │  │
│  │  │  ◉  │    │  ◉  │    │  ◉  │    │  ◉  │        │  │
│  │  └─────┘    └─────┘    └─────┘    └─────┘        │  │
│  │       ┌─────┐    ┌─────┐    ┌─────┐               │  │
│  │       │ LLM │───▶│ TTS │───▶│ OAP │               │  │
│  │       │  ◉  │    │  ◉  │    │  ◉  │               │  │
│  │       └─────┘    └─────┘    └─────┘               │  │
│  │                                                    │  │
│  │  Overall Health: ████████ HEALTHY                  │  │
│  └────────────────────────────────────────────────────┘  │
│                                                          │
│  METRICS ROW (4-column CSS grid, gap: 16px)              │
│  ┌─────────────┐ ┌─────────────┐ ┌─────────────┐ ┌──────│
│  │ gradient bg  │ │ gradient bg  │ │ gradient bg  │ │     │
│  │   5/7        │ │    2         │ │   14/16      │ │ 99. │
│  │ Services     │ │ Active Calls │ │ Tests Pass   │ │ Upt │
│  │   Online     │ │              │ │              │ │     │
│  └─────────────┘ └─────────────┘ └─────────────┘ └──────│
│                                                          │
│  CONTENT GRID (2-column, 60%/40% split)                  │
│  ┌───────────────────────┐ ┌───────────────────────────┐ │
│  │ Recent Activity Feed  │ │ Quick Actions              │ │
│  │                       │ │                            │ │
│  │ ┌──────────────────┐  │ │ ┌────────────────────────┐│ │
│  │ │ 12:04 SIP INFO.. │  │ │ │ ▶ Start All Services  ││ │
│  │ │ 12:03 VAD SPEECH │  │ │ │ ■ Stop All Services   ││ │
│  │ │ 12:02 ASR Transcr│  │ │ │ ▶ Run Pipeline Test   ││ │
│  │ │ 12:01 LLM Genera │  │ │ │ ↻ Restart Failed      ││ │
│  │ │ 12:00 TTS Synthes│  │ │ └────────────────────────┘│ │
│  │ └──────────────────┘  │ │                            │ │
│  │                       │ │ Service Health Sparklines  │ │
│  │                       │ │ SIP  ▁▂▃▅▇█▇▅▃▁ 42ms     │ │
│  │                       │ │ IAP  ▁▁▂▂▃▃▂▂▁▁ 18ms     │ │
│  │                       │ │ ASR  ▁▃▅▇▅▃▅▇▅▃ 156ms    │ │
│  └───────────────────────┘ └───────────────────────────┘ │
└──────────────────────────────────────────────────────────┘
```

#### 6.4.1 Pipeline Visualization (Hero)

- **Container**: Full-width card with `--wt-gradient-hero` background, border-radius 16px, padding 32px, white text
- **Pipeline nodes**: Each is a 64px rounded-rect (border-radius 14px) with:
  - White background with `backdrop-filter:blur(10px)` for glass effect
  - Service abbreviation (3–4 chars) in `--wt-title` weight
  - Status dot (12px) inside the node: green glow (`--wt-shadow-glow-success`) when online, red glow when down, gray when unknown
  - On hover: `transform:scale(1.08)` + shadow lift
- **Connectors**: SVG `<line>` elements or CSS `::after` pseudo-elements — 2px solid with animated dash pattern (`stroke-dasharray:6,4; animation: dash 1s linear infinite`) showing data flow direction. Color matches gradient position.
- **Health bar**: Horizontal progress-style bar at bottom of hero. Width = percentage of services online. Gradient fill: red→yellow→green based on ratio. Text label right-aligned: "HEALTHY" / "DEGRADED" / "CRITICAL"

```css
.wt-pipeline-hero {
  background: var(--wt-gradient-hero);
  border-radius: 16px;
  padding: 32px;
  color: #fff;
  position: relative;
  overflow: hidden;
}
.wt-pipeline-node {
  width: 64px; height: 64px;
  background: rgba(255,255,255,0.18);
  backdrop-filter: blur(10px);
  border-radius: 14px;
  display: flex; flex-direction: column;
  align-items: center; justify-content: center;
  cursor: pointer;
  transition: transform 0.2s ease, box-shadow 0.2s ease;
  border: 1.5px solid rgba(255,255,255,0.3);
}
.wt-pipeline-node:hover {
  transform: scale(1.08);
  box-shadow: var(--wt-shadow-lg);
}
.wt-pipeline-node .node-label {
  font-size: 11px; font-weight: 700;
  letter-spacing: 0.5px;
}
.wt-pipeline-node .node-status {
  width: 10px; height: 10px;
  border-radius: 50%; margin-top: 4px;
}
.wt-pipeline-node .node-status.online {
  background: var(--wt-success);
  box-shadow: var(--wt-shadow-glow-success);
}
.wt-pipeline-node .node-status.offline {
  background: rgba(255,255,255,0.3);
}
.wt-pipeline-node .node-status.error {
  background: var(--wt-danger);
  box-shadow: var(--wt-shadow-glow-danger);
}
.wt-pipeline-connector {
  height: 2px;
  background: rgba(255,255,255,0.4);
  flex: 1; max-width: 48px;
  position: relative;
}
.wt-pipeline-connector::after {
  content: '▸'; position: absolute;
  right: -6px; top: -8px;
  color: rgba(255,255,255,0.6);
  font-size: 14px;
}
@keyframes flowPulse {
  0% { opacity:0.3; } 50% { opacity:1; } 100% { opacity:0.3; }
}
```

#### 6.4.2 Metric Cards Row

Four cards in a CSS grid (`grid-template-columns: repeat(4, 1fr); gap: 16px`). Each card:

- **Background**: Individual gradient from the palette (services=`--wt-gradient-info`, calls=`--wt-gradient-hero`, tests=`--wt-gradient-success`, uptime=`--wt-gradient-neutral`)
- **Layout**: Vertical stack — big number (`wt-display` class, 48px, white), label below (`wt-caption`, 11px, rgba(255,255,255,0.7))
- **Border-radius**: 16px
- **Padding**: 24px
- **Shadow**: `--wt-shadow-md`
- **Hover**: `transform:translateY(-2px)` + `--wt-shadow-lg`
- **Optional sparkline**: Tiny inline Chart.js line (50px wide, 20px tall) at bottom right showing last 20 data points

```css
.wt-metric-card {
  background: var(--wt-gradient-info);
  border-radius: 16px;
  padding: 24px;
  color: #fff;
  position: relative;
  overflow: hidden;
  transition: transform 0.2s ease, box-shadow 0.2s ease;
}
.wt-metric-card:hover {
  transform: translateY(-2px);
  box-shadow: var(--wt-shadow-lg);
}
.wt-metric-card .metric-value {
  font-size: 48px; font-weight: 800;
  letter-spacing: -0.03em; line-height: 1;
}
.wt-metric-card .metric-label {
  font-size: 11px; font-weight: 600;
  text-transform: uppercase; letter-spacing: 0.5px;
  opacity: 0.7; margin-top: 8px;
}
.wt-metric-card .metric-delta {
  font-size: 12px; font-weight: 600;
  margin-top: 4px;
}
.wt-metric-card .metric-delta.positive { color: rgba(255,255,255,0.9); }
.wt-metric-card .metric-delta.negative { color: #ffd6d6; }
```

#### 6.4.3 Activity Feed

- **Card**: White/card-bg background, `--wt-shadow-sm`, border-radius 16px
- **Each entry**: Horizontal row with:
  - Colored left border (3px) matching log level (green=INFO, yellow=WARN, red=ERROR, gray=DEBUG)
  - Timestamp in `wt-micro` (10px, secondary color)
  - Service name as a colored pill badge (each service gets its `--wt-chart-N` color)
  - Message text truncated with ellipsis, monospace
- **Animation**: New entries slide in from top with `@keyframes slideIn { from { opacity:0; transform:translateY(-8px) } }`
- Max height: `calc(100vh - 480px)`, scrollable

#### 6.4.4 Quick Actions Panel

- **Card**: Same styling as activity feed
- **Buttons**: Full-width, stacked with 8px gap. Each button:
  - Height: 44px
  - Left icon (emoji), bold label, right arrow
  - Gradient background matching action type
  - "Start All" = `--wt-gradient-success`
  - "Stop All" = `--wt-gradient-danger`
  - "Run Tests" = `--wt-gradient-info`
  - "Restart Failed" = `--wt-gradient-warning`
  - Border-radius: 12px
  - Hover: brightness(1.1) + slight scale

### 6.5 Navigation Sidebar

Redesigned to be cleaner and more spacious:

```
┌─────────────────────────┐
│  WT  WhisperTalk         │  ← Logo + wordmark, 13px uppercase
│                          │
│  ● Dashboard             │  ← Active: accent bg, white text
│                          │
│  PIPELINE                │  ← Section label, 10px, uppercase, secondary color
│    Services      5/7     │  ← Badge: gradient pill
│    Live Logs             │
│                          │
│  TESTING                 │
│    Test Runner     6     │
│    Test Results          │
│    Beta Tests            │
│                          │
│  CONFIGURATION           │
│    Models                │
│    Database              │
│    Credentials           │
│                          │
│  ─────────────────────   │
│  Status: 5 svcs ● 2 SSE │  ← Footer bar
│  Theme: [Dark ▾]         │
└─────────────────────────┘
```

Key visual changes:
- **Active state**: Rounded rect (8px radius) with `--wt-accent` bg, white text, font-weight 600
- **Hover state**: Subtle `rgba(0,0,0,0.04)` fill (already exists, keep it)
- **Section titles**: 10px, uppercase, `letter-spacing:1px`, `--wt-text-secondary`, padding-top 16px for section spacing
- **Badges**: Pill shape with gradient background matching section color (pipeline=blue, testing=green)
- **Nav item padding**: Increased to `9px 14px` for more breathing room
- **Sidebar width**: Keep `240px`
- **Backdrop blur**: Keep existing `blur(20px)` glass effect

### 6.6 Services Page Enhancements

- **Service cards**: Each card gets a subtle left border (3px) in the service's `--wt-chart-N` color
- **Status indicator**: Replace plain dot with an animated ring (CSS `border` + `@keyframes spin`) for "starting" state; solid glow dot for online; static gray for offline
- **Quick-view metrics**: Below service name, show tiny inline stats: uptime, memory, last ping latency in `wt-micro` size
- **Hover**: Card lifts with `--wt-shadow-md`, left border thickens to 4px

### 6.7 Test Results Page

- **Hero row**: 3 gradient metric cards (Total Tests, Pass Rate %, Avg Latency) — same style as dashboard metrics but smaller (36px number)
- **Trend chart**: Full-width Chart.js line chart with:
  - Gradient fill under the line (`ctx.createLinearGradient` from chart color to transparent)
  - Smooth bezier curves (`tension:0.4`)
  - Interactive tooltip with custom styling (matching card design)
  - Time axis with date labels
  - Zoom plugin enabled for drill-down
- **Results table**: Below chart, with alternating row shading, sortable columns, status badges
- **Filter bar**: Pill-shaped filter buttons (test type, date range, model) with active state showing accent bg

### 6.8 Beta Testing Tabs

- **Tab bar**: Custom-styled Bootstrap tabs (override `.nav-tabs` styles):
  - No bottom border on container
  - Each tab: pill-shaped (`border-radius:8px`), padding `8px 20px`
  - Active tab: `--wt-accent` background, white text, subtle shadow
  - Inactive: transparent bg, `--wt-text-secondary` color
  - Hover: light accent bg (`rgba(0,113,227,0.08)`)
  - Tab transition: `background 0.2s, color 0.2s`
- **Test cards within tabs**: 
  - Collapsible with animated height transition (`max-height` + `overflow:hidden` + `transition:max-height 0.3s ease`; JS sets `el.style.maxHeight = el.scrollHeight + 'px'` on expand, `'0'` on collapse — see Section 6.9)
  - Status badge in top-right of card header: gradient pill showing last run result
  - "Run" button with gradient bg matching action type
- **Summary bar**: Above tabs, horizontal strip showing colored dots per test category (green=all pass, yellow=some fail, red=all fail, gray=never run)

### 6.9 Animation & Transition Specs

Most animations are CSS-only (no external animation library). Two exceptions require minimal JS assistance: page transitions (reflow trigger) and collapsible expand (scrollHeight measurement).

**Important — page transitions**: CSS transitions do **not** fire when `display` changes from `none` to `block`. The existing codebase uses `display:none`/`display:block` for page switching. To enable fade-in transitions, replace `display` toggling with `visibility`/`pointer-events`:

```css
.wt-page { 
  visibility:hidden; pointer-events:none; position:absolute; top:0; left:0; width:100%; 
  opacity:0; transform:translateY(8px); 
  transition:opacity 0.2s ease-out, transform 0.2s ease-out, visibility 0s linear 0.2s;
}
.wt-page.active { 
  visibility:visible; pointer-events:auto; position:relative;
  opacity:1; transform:translateY(0); 
  transition:opacity 0.2s ease-out, transform 0.2s ease-out, visibility 0s linear 0s;
}
```

The `visibility` transition delay trick: when hiding, `visibility:hidden` is delayed by 0.2s (matching fade duration) so it stays visible during fade-out; when showing, it applies immediately (0s delay).

The `showPage()` JS function must be updated to toggle `active` class instead of changing `display`/`style.display`. Remove any `display:none !important` from `.hidden` class usage on pages.

| Element | Trigger | Animation |
|---------|---------|-----------|
| Page switch | `showPage()` class toggle | `opacity 0→1, translateY(8px→0)` via visibility/pointer-events (no display toggle) |
| Card hover | `:hover` | `translateY(-2px)` + shadow elevation, 200ms ease |
| Status dot change | JS class toggle | `background-color 0.3s ease` + glow box-shadow |
| Pipeline connector | Continuous | `flowPulse` keyframes (opacity 0.3→1→0.3) 2s infinite |
| New log entry | Prepend to feed | `slideIn` (opacity+translateY) 300ms ease-out |
| Metric update | Data change | `countUp` effect via JS `setInterval` (20ms steps, 400ms total) |
| Toast notification | Show/hide | `translateY(20px→0) + opacity` 300ms, auto-dismiss 3s |
| Tab switch | Click | Content `opacity 0→1` 200ms, tab bar pill slides |
| Collapsible expand | Click toggle | `max-height` transition, JS measures `scrollHeight` (see below) |
| Service start/stop | Button click | Button ripple effect (CSS `::after` radial gradient) |

**Collapsible expand approach**: Uses `max-height` transition with JS-measured target value. This is not "CSS-only" — it requires a one-liner in the toggle handler:
```javascript
el.style.maxHeight = el.classList.contains('open') ? el.scrollHeight + 'px' : '0';
```
Combined with CSS:
```css
.wt-collapsible { max-height:0; overflow:hidden; transition:max-height 0.3s ease; }
.wt-collapsible.open { /* max-height set by JS */ }
```
This avoids the fixed-large-value hack and produces correct easing.

```css
@keyframes slideIn { from { opacity:0; transform:translateY(-8px); } to { opacity:1; transform:translateY(0); } }
@keyframes countPulse { 0% { transform:scale(1); } 50% { transform:scale(1.05); } 100% { transform:scale(1); } }
.metric-updated { animation: countPulse 0.4s ease; }
```

### 6.10 Responsive Breakpoints

Since this is a local-only tool (127.0.0.1), we target desktop screens but handle common widths:

| Breakpoint | Layout Changes |
|------------|---------------|
| `> 1400px` | Full layout, 4-column metric grid, 2-column content grid |
| `1024–1400px` | Same layout, tighter padding (24px→16px) |
| `768–1024px` | Metric grid: 2 columns, content grid: single column stacked |
| `< 768px` | Sidebar collapses to icon-only (48px), metric grid: 2 columns |

```css
@media (max-width:1024px) {
  .wt-content { padding:16px 20px; }
  .wt-metrics-grid { grid-template-columns: repeat(2, 1fr); }
  .wt-dashboard-content { grid-template-columns: 1fr; }
}
@media (max-width:768px) {
  .wt-sidebar { width:48px; min-width:48px; }
  .wt-sidebar .nav-text, .wt-sidebar-section-title, .wt-sidebar-header h1 { display:none; }
  .wt-nav-item { justify-content:center; padding:12px 0; }
  .wt-metric-card .metric-value { font-size:32px; }
}
```

### 6.11 Theme Compatibility

All new visual elements use CSS custom properties. Dark/slate/cyborg themes override:
- Surface colors (`--wt-surface-elevated`, `--wt-surface-sunken`)
- Shadow intensities (stronger for dark bg)
- Gradient overlays get slight opacity reduction in dark mode for softer contrast
- Pipeline hero: In dark mode, gradient opacity reduces to 0.85 and nodes use `rgba(0,0,0,0.3)` instead of white bg
- Metric cards: Gradients stay the same (they're already bold colors that work on dark)

```css
[data-bs-theme="dark"] .wt-pipeline-hero { opacity:0.95; }
[data-bs-theme="dark"] .wt-pipeline-node { background:rgba(0,0,0,0.3); border-color:rgba(255,255,255,0.15); }
[data-bs-theme="dark"] .wt-card { box-shadow: 0 1px 4px rgba(0,0,0,0.3); }
```

### 6.12 Chart.js Configuration Standards

All charts follow a consistent visual style:

```javascript
var CHART_DEFAULTS = {
  borderWidth: 2,
  tension: 0.4,
  pointRadius: 0,
  pointHoverRadius: 6,
  fill: true,
  backgroundColor: function(ctx) {
    var gradient = ctx.chart.ctx.createLinearGradient(0, 0, 0, ctx.chart.height);
    gradient.addColorStop(0, 'rgba(102,126,234,0.3)');
    gradient.addColorStop(1, 'rgba(102,126,234,0)');
    return gradient;
  }
};
var CHART_COLORS = ['#667eea','#764ba2','#f093fb','#43e97b','#fa709a','#fee140','#30cfd0'];
var CHART_GRID = { color:'rgba(0,0,0,0.04)', drawBorder:false };
var CHART_FONT = { family:getComputedStyle(document.documentElement).getPropertyValue('--wt-font').trim(), size:11 };
```

### 6.13 Implementation Constraints

- All CSS is inline in `serve_index()` within the existing `<style>` block — no external CSS files
- All JS is inline in `build_ui_js()` — no external JS files
- No additional CDN dependencies beyond what's already loaded (Bootstrap 5.3, Chart.js 4.4, Hammer.js)
- Total added CSS: ~3KB (minified inline)
- Total added JS: ~2KB for dashboard logic
- SVG pipeline diagram is generated as inline HTML string, not a separate SVG file
- Animations are CSS transitions/keyframes with two JS-assisted exceptions: (1) page transitions use `visibility`/`pointer-events` toggling via class (no `display` toggling), (2) collapsible panels use `el.scrollHeight` measurement for `max-height` target. No `requestAnimationFrame` loops.
- CountUp effect for metrics uses a simple JS `setInterval` with 20ms steps over 400ms

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
- Extract C++ magic numbers to named constants (8 constants identified)
- Extract JS magic numbers to named constants (45+ occurrences)
- Harden `is_read_only_query()` (strip comments, block load_extension)
- Sanitize `kill_ghost_processes()` input
- Improve `extract_json_string()` escape handling (add `\b`, `\f`)
- Remove dead code: `is_service_running_unlocked()` (line 793) — only dead function found
- **Verification**: Build succeeds, no regressions in API behavior

### Phase 4: Test Fixes (P1)
- All tests compile cleanly — no compilation fixes needed
- `test_integration`: Add `GTEST_SKIP()` guard when pipeline services unavailable
- `test_kokoro_cpp`: Add `GTEST_SKIP()` guard when model files missing
- Frontend test runner correctly handles gtest output (verified)
- Port numbers match `interconnect.h` (verified by test_interconnect 30/30 pass)
- **Verification**: `test_sanity`, `test_interconnect`, `test_sip_provider_unit` all pass; integration/kokoro skip cleanly when deps unavailable

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
