# Product Requirements Document: WhisperTalk Frontend Overhaul

## 1. Overview

The WhisperTalk frontend (`frontend.cpp`, ~10,500 lines) is the central control plane for a real-time speech-to-speech telephony system. It serves a single-page web application, manages 7 pipeline microservices, aggregates logs via UDP, stores data in SQLite, and provides test infrastructure. This PRD covers four major workstreams to bring the frontend to production quality.

## 2. Problem Statements

### 2.1 SQLite Readonly Database Error (P0 - Critical)

**Current behavior**: When started from `bin/` directory, the frontend emits `flush_log_queue: insert failed: attempt to write a readonly database` every 500ms.

**Root cause**: `init_database()` at line 484 opens `sqlite3_open("frontend.db", &db_)` using a relative path. The `main()` function (line 10435+) attempts to `chdir()` to the project root when launched from `bin/`, but this logic has edge cases:
- The chdir logic depends on `argv[0]` containing a path with `/`. When invoked as just `./frontend` from within `bin/`, `exe_dir` resolves to `.` which is skipped. The fallback check (`stat("bin/frontend", &st)`) then tries to detect the mistake, but if CWD is already `bin/`, `chdir` to parent may fail or the DB file may already have been created with wrong permissions.
- The database is opened *in the constructor* (line 282) before `main()` can correct the CWD, because the `FrontendServer` object is constructed at line 10481 after the chdir block. However, the chdir happens before construction, so the real issue is that certain invocation paths (e.g., `cd bin && ./frontend` where argv[0] is `./frontend`) cause the chdir logic to set CWD incorrectly or not at all.

**Requirements**:
- R1.1: The database path must be resolved to an absolute path based on the detected project root, not rely on CWD.
- R1.2: If the database file cannot be opened for writing, the frontend must emit a clear fatal error and exit, rather than silently failing every 500ms.
- R1.3: The chdir logic must handle all invocation scenarios: `bin/frontend`, `./bin/frontend`, `cd bin && ./frontend`, absolute paths, and symlinks.

### 2.2 Code Quality, Bugs, and Security Issues (P1 - High)

**Current state**: The entire frontend is a single monolithic 10,488-line C++ file with HTML/CSS/JS embedded as raw string literals.

**Issues identified**:

#### Magic Numbers
- `500` (ms) - log flush interval (line 341)
- `4096` - UDP buffer size (log receiver)
- `10000` - default row limit for DB queries
- `22011` - TEST_SIP_PROVIDER_PORT (line 393, partially named)
- `1000` - MAX_RECENT_LOGS (line 392, partially named)
- `100` - mg_mgr_poll timeout (line 336)
- `3000`, `5000` - JS setInterval timers for status/test/service polling (line 3926-3928)
- `1500` - test log poll interval (line 2554)
- `30` - days for log rotation (line 1191)
- `20` - hardcoded SIP line count throughout JS code

#### Security Issues
- `is_read_only_query()` SQL injection guard can be bypassed (e.g., with CTEs, ATTACH, or creative formatting)
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
- R2.2: Extract all JS magic numbers/intervals into named constants at the top of the JS block
- R2.3: Fix `is_read_only_query()` to use a proper SQL statement parser or whitelist approach (parameterized query execution, or parse the first keyword after stripping comments/whitespace)
- R2.4: Sanitize all inputs to `popen()`/shell commands to prevent command injection
- R2.5: Remove dead code, unused functions, and commented-out blocks
- R2.6: Replace `extract_json_string` with a lightweight JSON parser (e.g., nlohmann/json is likely too heavy; consider a minimal single-header parser, or keep the hand-rolled approach but make it robust and well-tested)
- R2.7: Document security assumptions (local-only access) and bind HTTP to `127.0.0.1` (already done at line 324)
- R2.8: Remove any stub/simulation code and replace with real implementations or clearly mark as TODO with tracking

### 2.3 Broken Tests (P1 - High)

**Current state**: Test binaries (`test_sanity`, `test_interconnect`, `test_sip_provider_unit`, `test_kokoro_cpp`, `test_integration`, `test_sip_provider`) are discovered by scanning `bin/test_*`. The test infrastructure in the frontend hardcodes test discovery at line 690. The `run_pipeline_test.py` Python script runs full WER tests.

**Issues**:
- Tests may have fallen out of sync with service API changes (port numbers, message formats, protocol changes)
- The frontend test runner infrastructure may not correctly parse test output from updated test binaries
- Test result display in the UI may not match current test output formats
- The pipeline test script relies on specific log message formats that may have changed

**Requirements**:
- R3.1: Audit each test binary against current service implementations and fix compilation/runtime failures
- R3.2: Update the frontend test runner to handle current test binary output formats
- R3.3: Ensure `run_pipeline_test.py` works with current log message formats and API endpoints
- R3.4: Verify test result storage and display in the frontend UI matches actual test output
- R3.5: Add error reporting when tests fail to compile or are missing dependencies

### 2.4 Frontend UI Redesign (P0 - High)

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

- SC1: `cd bin && ./frontend` starts without any SQLite errors
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
2. **P0**: Dashboard and navigation redesign (R4.x) - primary user-facing improvement
3. **P1**: Code quality and security fixes (R2.x) - technical debt reduction
4. **P1**: Fix broken tests (R3.x) - dependent on understanding current service state
