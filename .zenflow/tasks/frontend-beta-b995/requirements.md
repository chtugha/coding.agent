# Product Requirements Document: WhisperTalk Frontend Overhaul

## 1. Overview

The WhisperTalk frontend (`frontend.cpp`, ~10,500 lines) is the central control plane for a real-time speech-to-speech telephony system. It serves a single-page web application, manages 7 pipeline microservices, aggregates logs via UDP, stores data in SQLite, and provides test infrastructure. This PRD covers four major workstreams to bring the frontend to production quality.

## 2. Problem Statements

### 2.1 SQLite Readonly Database Error (P0 - Critical)

**Current behavior**: When started from `bin/` directory, the frontend emits `flush_log_queue: insert failed: attempt to write a readonly database` every 500ms.

**Root cause**: `init_database()` at line 484 opens `sqlite3_open("frontend.db", &db_)` using a relative path. The `main()` function (line 10435+) performs `chdir()` *before* constructing `FrontendServer` (line 10481), so the constructor always runs after the CWD correction attempt. The chdir fallback logic (lines 10455–10466) does correctly handle common cases like `cd bin && ./frontend` — it detects CWD ends in `"bin"` and chdirs to the parent.

The actual failure mechanism involves `sqlite3_open` behavior and file permissions:
- `sqlite3_open("frontend.db", &db_)` is called with the default `SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE` flags. Per SQLite documentation, if the file exists but is **not writable** by the current user, `sqlite3_open` **succeeds** (returns `SQLITE_OK`) but silently opens the database in **read-only mode**.
- The code at line 484–488 only checks `rc != SQLITE_OK`, so it does not detect the read-only fallback.
- All subsequent writes (every 500ms in `flush_log_queue`) fail with "attempt to write a readonly database".
- This occurs when: (a) a `frontend.db` file was previously created with different permissions (e.g., by a different user, or during a build step that ran as root), (b) the project root directory itself is not writable, or (c) an invocation path not covered by the chdir logic causes the DB to be created in a non-writable location. The relative path makes this fragile.

Two underlying issues:
1. The DB path is a bare relative filename depending on CWD, rather than an absolute path derived from the executable location.
2. The code does not verify the database was opened in read-write mode after `sqlite3_open` returns.

**Requirements**:
- R1.1: The database path must be resolved to an absolute path based on the detected project root, not rely on CWD.
- R1.2: After `sqlite3_open` succeeds, verify the database is writable via `sqlite3_db_readonly(db_, "main") == 0`. If read-only, emit a clear fatal error with the resolved DB path and exit, rather than silently failing every 500ms.
- R1.3: The chdir logic must handle all invocation scenarios: `bin/frontend`, `./bin/frontend`, `cd bin && ./frontend`, absolute paths, and symlinks.

### 2.2 Code Quality, Bugs, and Security Issues (P1 - High)

**Current state**: The entire frontend is a single monolithic 10,488-line C++ file with HTML/CSS/JS embedded as raw string literals.

**Issues identified**:

#### Magic Numbers (C++)
- `500` (ms) - log flush interval (line 341)
- `4096` - UDP buffer size (log receiver)
- `10000` - default row limit for DB queries (line 10400)
- `100` - mg_mgr_poll timeout (line 336)
- `30` - days for log rotation (line 1191)
- `2` (seconds) - service status check interval (line 345)
- `30` (seconds) - async task cleanup interval (line 349)
- Note: `MAX_RECENT_LOGS` (1000) and `TEST_SIP_PROVIDER_PORT` (22011) are already named `static constexpr` constants — no action needed.

#### Magic Numbers (JS — 45+ occurrences of `setInterval`/`setTimeout` with numeric literals)
- `3000` - fetchStatus interval (line 3926), reconnectLogSSE retry (line 2925), TTS roundtrip/full loop poll (lines 3673, 3741), status message clear timeouts (lines 3007, 3022, 3025, 3048, 3051, 3074), toast display duration (line 3074), accuracy poll (line 5109)
- `5000` - fetchServices interval (line 3928), refreshCallLineMap interval (line 3092), status clear timeout (line 2996)
- `2000` - restart service delay (lines 2832, 2854), SIP RTP stats refresh (line 4099), LLaMA quality poll (line 3238), Kokoro quality/bench poll (lines 3386, 3440), benchmark poll (line 4455), LLaMA benchmark poll (line 4936), pipeline stress poll (line 3583)
- `1500` - test log poll (line 2554), shutup pipeline poll (line 3330), save button feedback (line 2841)
- `1000` - service start/stop refresh delays (lines 2820, 2826, 2846, 2850, 4013), LLaMA shutup poll (line 3286)
- `500` - fetchTests delay after start/stop (lines 2531, 2538), sipRefreshActiveLines delay (line 2769), refreshCallLineMap initial delay (line 3093), model select delay (line 4365)
- `300` - sipRefreshActiveLines delay (line 2804), call-line-map debounce (line 3099), toast fade-out (line 3074)
- `200` - SIP line add sequential delay (line 4018)
- `10000` - pipeline health check interval (line 3498)
- `20` - hardcoded SIP line count in grid/loops throughout JS code

#### Security Issues
- `is_read_only_query()` SQL guard only checks the first keyword (SELECT/EXPLAIN/PRAGMA). While `handle_db_query` already uses `sqlite3_prepare_v2` (single-statement only, so multi-statement injection like `SELECT 1; DROP TABLE` is NOT possible), the guard could still be bypassed if SQLite extensions are enabled (e.g., `SELECT load_extension(...)`) or via PRAGMA writes if the safe-list check is incomplete
- Credentials (SIP passwords, HuggingFace tokens) stored in plaintext in SQLite `settings` table
- `kill_ghost_processes()` (line 824) uses `popen("pgrep -f ...")` with unsanitized binary name - command injection risk
- `curl` invoked via `fork()/execvp()` for HuggingFace API calls with token written to temp file
- `db_write_mode_` flag allows arbitrary SQL execution with no authentication
- No CSRF protection on any POST endpoints
- No authentication on the web interface at all (acceptable for local-only use but should be documented/enforced)

#### Dead Code / Stubs
- Need full scan for unused functions, unreachable code paths, commented-out blocks
- Check for simulation/stub code in test infrastructure that should be real implementations

#### Deprecated Patterns
- Hand-rolled JSON parsing (`extract_json_string`) instead of using a proper JSON library
- Bootstrap JS loaded from CDN but CSS is fully custom - inconsistent approach
- `fork()/execvp()` pattern for curl calls could use libcurl or a lightweight HTTP client

**Requirements**:
- R2.1: Extract all magic numbers into named constants (C++ `constexpr` or `static const`)
- R2.2: Extract all 45+ JS `setInterval`/`setTimeout` numeric literals into a named-constants block at the top of the JS script section (e.g., `var POLL_STATUS_MS=3000, POLL_TESTS_MS=3000, POLL_SERVICES_MS=5000, ...`)
- R2.3: Harden `is_read_only_query()`: disable `load_extension` via `sqlite3_db_config(db_, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, NULL)` at init, strip SQL comments before keyword check, and verify PRAGMA whitelist is complete. Note: `handle_db_query` already uses `sqlite3_prepare_v2` (single-statement), so multi-statement injection is not possible.
- R2.4: Sanitize all inputs to `popen()`/shell commands to prevent command injection
- R2.5: Remove dead code, unused functions, and commented-out blocks
- R2.6: Keep the hand-rolled `extract_json_string` (adding an external JSON library is disproportionate for this codebase) but add explicit bounds-checking, handle escaped quotes inside values, and ensure it doesn't read past the end of the input string
- R2.7: Document security assumptions (local-only access) and bind HTTP to `127.0.0.1` (already done at line 324)
- R2.8: Remove any stub/simulation code and replace with real implementations or clearly mark as TODO with tracking

### 2.3 Broken Tests (P1 - High)

**Current state**: Test binaries are defined in a **hardcoded vector** in `discover_tests()` (line 692-700): `test_sanity`, `test_interconnect`, `test_sip_provider_unit`, `test_kokoro_cpp`, `test_integration`, `test_sip_provider`. There is no filesystem scanning or glob — new test binaries must be manually added to this list. The `run_pipeline_test.py` Python script runs full WER tests.

**Issues**:
- Tests may have fallen out of sync with service API changes (port numbers, message formats, protocol changes in `interconnect.h`)
- The frontend test runner infrastructure may not correctly parse test output from updated test binaries
- Test result display in the UI may not match current test output formats
- The pipeline test script relies on specific log message formats (e.g., `"Transcription (Xms): <text>"`) that may have changed
- Test discovery is hardcoded — adding new tests requires modifying `discover_tests()` source code

**Requirements**:
- R3.1: Build each test binary and run it to identify specific compilation errors and runtime failures. Check for stale port numbers (e.g., ports defined in `interconnect.h`), changed message format structs, and missing dependencies.
- R3.2: Update the frontend test runner to handle current test binary output formats
- R3.3: Ensure `run_pipeline_test.py` works with current log message formats and API endpoints
- R3.4: Verify test result storage and display in the frontend UI matches actual test output
- R3.5: Add error reporting when tests fail to compile or are missing dependencies

### 2.4 Frontend UI Redesign (P1 - High)

**Current navigation structure**:
```
Testing:
  - Tests (default page, active on load)
  - Beta Testing (massive page with 9+ test panels)
  - Models
Services:
  - Pipeline
System:
  - Live Logs
  - Database
  - Credentials
```

**Problems**:
1. No dashboard/home page - first thing the user sees is a list of test binaries, which is not useful
2. "Beta Testing" is a single massive scrollable page with 9+ test panels (SIP RTP routing, IAP codec quality, Whisper accuracy, LLaMA quality, shut-up mechanism, Kokoro TTS, full pipeline round-trip, pipeline resilience, stress tests) - overwhelming and hard to navigate
3. Test results are buried inside "Beta Testing" instead of being easily accessible
4. No visual representation of the pipeline architecture
5. No at-a-glance system health overview
6. Navigation categories don't reflect user workflow (testing is prominent but pipeline management is secondary)

**Requirements**:

#### R4.1: Dashboard (New Default Page)
- Show a visual pipeline diagram (SIP → IAP → VAD → Whisper → LLaMA → Kokoro/NeuTTS → OAP) with real-time service status indicators (running/stopped/error) on each node
- Display key metrics at a glance: services online count, active calls, recent test pass/fail summary, system uptime
- Show recent activity feed (last N log entries, recent test runs, service start/stop events)
- Quick-action buttons: Start All Services, Stop All Services, Run Pipeline Test
- Color-coded health status: green (all services running), yellow (partial), red (critical services down)

#### R4.2: Reorganized Navigation
New proposed structure:
```
Dashboard (default, home icon)
Pipeline:
  - Services (current Pipeline page - start/stop/configure services)
  - Live Logs
Testing:
  - Test Runner (current Tests page - run test binaries)
  - Test Results (pipeline WER results, accuracy results, benchmark history)
  - Beta Tests (reorganized - individual test panels as sub-sections or tabs, not one giant page)
Configuration:
  - Models
  - Database
  - Credentials
```

#### R4.3: Beta Testing Page Reorganization
- Break the monolithic Beta Testing page into tabbed sub-sections or separate sub-pages
- Group related tests logically:
  - **Component Tests**: SIP RTP Routing, IAP Codec Quality, Whisper Accuracy, LLaMA Quality, Kokoro TTS Quality
  - **Pipeline Tests**: Full Pipeline Round-Trip, Shut-Up Mechanism, Pipeline Resilience, Stress Tests
  - **Tools**: Audio Injection, SIP Lines Management, Test Audio Files
- Each test panel should be collapsible or accessible via tabs to reduce visual clutter
- Add a test summary bar at the top showing last run status of each test category

#### R4.4: Visual Design Improvements
- Follow information design principles (clarity, hierarchy, progressive disclosure)
- Use consistent card-based layout with clear visual hierarchy
- Improve data density without sacrificing readability
- Add subtle animations for state transitions (service start/stop, test progress)
- Ensure responsive layout works on various screen sizes
- Maintain existing theme support (default, dark, slate, flatly, cyborg)

#### R4.5: Test Results Integration
- Create a dedicated Test Results page that aggregates:
  - Pipeline WER test results (from `/tmp/pipeline_results_*.json`)
  - Whisper accuracy test history (from `whisper_accuracy_tests` table)
  - Model benchmark results (from `model_benchmark_runs` table)
  - IAP quality test results (from `iap_quality_tests` table)
  - Service test results (from `service_test_runs` table)
- Show trend charts for key metrics over time
- Allow filtering by test type, date range, model

## 3. Non-Requirements / Out of Scope

- Splitting `frontend.cpp` into multiple files (desirable but not in this task scope - would require build system changes)
- Adding authentication/authorization to the web interface (local-only assumption is acceptable)
- Changing the inter-service communication protocol (TCP/interconnect.h is stable)
- Modifying any service other than the frontend
- Changing the build system (CMakeLists.txt)

## 4. Assumptions

- A4.1: The frontend will continue to be a single C++ binary serving an embedded SPA
- A4.2: HTML/CSS/JS will remain embedded as raw string literals in C++ (no separate build toolchain)
- A4.3: All changes must maintain backward compatibility with existing API endpoints
- A4.4: The frontend is only accessed locally (127.0.0.1) - no public internet exposure
- A4.5: Existing theme system (5 themes) must be preserved
- A4.6: Chart.js (already loaded from CDN) will be used for all charting needs
- A4.7: Bootstrap JS (already loaded from CDN) can be used for UI components (tabs, modals, tooltips)

## 5. Success Criteria

- SC1: Frontend starts without any SQLite errors under all invocation methods (`bin/frontend`, `./bin/frontend`, `cd bin && ./frontend`, absolute paths). Additionally, if `frontend.db` exists with wrong permissions, the frontend must exit with a clear error message rather than spamming stderr.
- SC2: All magic numbers replaced with named constants
- SC3: No dead code or stubs remaining (verified by code review)
- SC4: Security issues (SQL injection guard, command injection, credential handling) addressed
- SC5: All test binaries that compile successfully can be run from the frontend UI with correct result display
- SC6: `run_pipeline_test.py` executes successfully when all services are running
- SC7: Dashboard page loads as default with pipeline visualization and real-time status
- SC8: Navigation is reorganized per R4.2
- SC9: Beta Testing page is broken into navigable sub-sections per R4.3
- SC10: Existing functionality is preserved (no regressions in API endpoints or service management)

## 6. Priority Order

1. **P0**: Fix SQLite readonly error (R1.x) - blocking normal operation
2. **P1**: Dashboard and navigation redesign (R4.x) - primary user-facing improvement
3. **P1**: Code quality and security fixes (R2.x) - technical debt reduction
4. **P1**: Fix broken tests (R3.x) - dependent on understanding current service state
