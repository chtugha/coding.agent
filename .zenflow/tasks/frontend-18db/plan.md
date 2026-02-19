# Spec and build

## Configuration
- **Artifacts Path**: `.zenflow/tasks/frontend-18db`
- **Complexity**: HARD
- **Estimated Duration**: 6-8 implementation sessions

---

## Agent Instructions

Ask the user questions when anything is unclear or needs their input. This includes:
- Ambiguous or incomplete requirements
- Technical decisions that affect architecture or user experience
- Trade-offs that require business context

Do not make assumptions on important decisions — get clarification first.

---

## Workflow Steps

### [x] Step: Technical Specification

Created comprehensive technical specification in `spec.md` covering:
- Confirmed architecture: SQLite + JavaScript + Bootstrap UI, SSE for live logs, 5 themes, macOS .app bundle
- Interconnect integration: ~40 lines added for log port propagation
- Service changes: ~10 lines each for optional UDP log forwarding
- Full REST API design, database schema, security considerations
- Verification approach and risk mitigation

---

### [x] Step 1: Interconnect Log Port Propagation

**Objective**: Extend interconnect.h to propagate a frontend logging port to all services.

**Tasks**:
- Add `frontend_log_port_` field to `InterconnectNode`
- Add `SET_LOG_PORT` message handler in master's negotiation loop
- Include log port in `SYNC_REGISTRY` broadcasts
- Parse log port from `SYNC_REGISTRY` on slave side
- Add public getter `frontend_log_port()` and setter `set_frontend_log_port()`
- Add `log_forward()` helper function for services to use
- Verify existing tests still pass (`test_interconnect`)

**Files Modified**:
- `interconnect.h` (~40 lines added)

**Verification**:
```bash
cd build && cmake .. -DBUILD_TESTS=ON && make test_interconnect && ./bin/test_interconnect
```

---

### [x] Step 2: Service Log Forwarding Integration

**Objective**: Add optional UDP log forwarding to each service (~10 lines each). No functional changes.

**Tasks**:
- In each service, after `interconnect_.initialize()`, check `frontend_log_port()`
- If non-zero, create UDP socket and forward log messages
- Use the `log_forward()` helper from interconnect.h
- Verify each service still compiles and existing tests pass

**Files Modified**:
- `sip-client-main.cpp` (~10 lines)
- `inbound-audio-processor.cpp` (~10 lines)
- `outbound-audio-processor.cpp` (~10 lines)
- `whisper-service.cpp` (~10 lines)
- `llama-service.cpp` (~10 lines)
- `kokoro-service.cpp` (~10 lines)

**Verification** (COMPLETED with real binaries and models):
- All 7 service binaries + 5 test binaries compile with zero warnings
- whisper-cpp (CoreML+Metal) and llama-cpp (Metal) built as static libs
- Models downloaded: Whisper large-v3 q5_0, LLaMA 3.2-1B Q8_0, Whisper CoreML encoder, Kokoro German CoreML
- 82 tests passed: test_sanity(2), test_interconnect(47), test_sip_provider_unit(25), test_kokoro_cpp(7), test_integration SingleCallFullPipeline(1)
- Full 6-service end-to-end pipeline ran 30s call with real models on Apple M4

---

### [x] Step 3: Frontend Logging Server Enhancement

**Objective**: Enhance the existing logging infrastructure with structured parsing, batched writes, and log rotation.

**Tasks**:
- Parse structured log messages: `SERVICE_NAME LEVEL CALL_ID MESSAGE`
- Implement batched SQLite writes (queue + 500ms flush)
- Add log rotation (delete >30 day entries)
- Enhance database schema (add composite index, service_config, settings tables)
- Hardcoded log port to `FRONTEND_LOG_PORT = 22022` (constant in interconnect.h)
- Pre-populate `service_config` table with default service entries
- Fixed JSON body parsing for test start/stop/db query endpoints
- Removed dynamic log port propagation (SET_LOG_PORT, LP= in SYNC_REGISTRY)
- Simplified all 6 services to use hardcoded port directly

**Files Modified**:
- `interconnect.h` (added FRONTEND_LOG_PORT constant, removed SET_LOG_PORT/LP= protocol, removed reinit/set_frontend_log_port)
- `frontend.cpp` (structured parsing, batched writes, log rotation, schema, JSON parsing)
- All 6 service files (simplified to use FRONTEND_LOG_PORT directly)

**Verification** (COMPLETED):
- All 12 binaries compile with zero warnings
- test_sanity: 2/2 passed
- test_sip_provider_unit: 25/25 passed
- test_interconnect: 51/51 passed (excl. 10-min stress)
- test_kokoro_cpp: 7/7 passed
- test_integration SingleCallFullPipeline: 1/1 passed (30s call, real models, Apple M4)
- Frontend logging: 6/6 UDP messages received, parsed, persisted to SQLite

---

### [x] Step 4: SSE Live Log Streaming

**Objective**: Implement Server-Sent Events for real-time log delivery to browser.

**Tasks**:
- Add SSE connection tracking in Mongoose event handler
- Implement `/api/logs/stream` endpoint with filter params (service, level)
- Push new log entries to SSE clients on each `mg_mgr_poll` cycle (thread-safe via queue)
- Handle client disconnect cleanup via MG_EV_CLOSE
- Limit max concurrent SSE connections to 20

**Files Modified**:
- `frontend.cpp` (~90 lines added: SSE queue, flush, handle_sse_stream, remove_sse_connection)

**Verification** (COMPLETED):
- All 12 binaries compile with zero warnings
- SSE unfiltered: 3/3 UDP messages received as SSE events with correct JSON
- SSE filtered: service filter correctly delivers only matching messages
- Max connections: 21st connection correctly rejected with HTTP 503
- Disconnect cleanup: new connections work after killing previous SSE clients
- test_sanity: 2/2 passed
- test_sip_provider_unit: 25/25 passed
- test_interconnect: 51/51 passed (excl. 10-min stress)
- test_kokoro_cpp: 7/7 passed

---

### [x] Step 5: Service Lifecycle Management + REST API

**Objective**: Implement service start/stop/restart functionality and all REST API endpoints.

**Tasks**:
- Implement service start via `fork()`+`execv()` with args from SQLite config
- Implement service stop via `kill(SIGTERM)` with SIGKILL fallback
- Add service status polling via `waitpid` and interconnect heartbeat
- Implement all REST API endpoints from spec (tests, services, logs, db, settings)
- Enhance test management with parameter persistence and run history
- Fixed SQL injection in `serve_logs_api()` — now uses parameterized queries

**Files Modified**:
- `frontend.cpp` (~500 lines added/changed)

**Verification** (COMPLETED):
- All 12 binaries compile with zero warnings
- test_sanity: 2/2 passed
- test_sip_provider_unit: 25/25 passed
- test_interconnect: 51/51 passed (excl. 10-min stress)
- test_kokoro_cpp: 7/7 passed
- REST API verified: services list, tests list, test start/stop/history/log, service start/stop/restart/config, logs with filters, DB query/schema, settings, status
- SQL injection vulnerability fixed (parameterized queries)
- Test run history persisted to SQLite with exit codes

---

### [ ] Step 6: Complete Web UI

**Objective**: Build the full Bootstrap web interface with Testing and Main sections, theme switcher, and database admin.

**Tasks**:
- Build navbar with Testing/Main/Logs/Database tabs and theme dropdown
- Testing overview page: test cards with status, start/stop buttons
- Per-test detail: parameter editing form, live log (SSE EventSource), history table
- Services overview page: service cards/table with status from interconnect
- Per-service detail: parameter editing, live log, start/stop/restart controls
- Database admin: SQL textarea, results table, schema viewer
- Theme switcher: 5 themes (Default, Dark, Slate, Flatly, Cyborg), persist in SQLite
- Embed Bootswatch CSS for themes 3-5 as string literals

**Files Modified**:
- `frontend.cpp` (~800 lines for HTML/JS generation and theme CSS)

**Verification**:
- Open http://localhost:9999 in browser
- Verify all tabs render correctly
- Verify theme switching works and persists
- Verify SSE log streaming in test/service detail views
- Verify test start/stop works from UI
- Verify database admin executes queries

---

### [ ] Step 7: macOS App Bundling

**Objective**: Package frontend as a macOS .app bundle with auto-open browser.

**Tasks**:
- Add `system("open http://localhost:<port>")` call after HTTP server starts
- Create `scripts/package-frontend-app.sh` (bundle structure, Info.plist, codesign)
- Add `package-frontend-app` CMake custom target
- Test `.app` launch on macOS

**Files Modified**:
- `frontend.cpp` (1 line: browser auto-open)
- `CMakeLists.txt` (custom target addition)

**Files Created**:
- `scripts/package-frontend-app.sh`

**Verification**:
```bash
cd build && cmake .. && make frontend
cd .. && bash scripts/package-frontend-app.sh
open WhisperTalk.app
```

---

### [ ] Step 8: Final Testing & Verification

**Objective**: End-to-end verification that all components work together.

**Tasks**:
- Run full build: `cd build && cmake .. -DBUILD_TESTS=ON && make`
- Run all existing tests: `ctest` — must all pass
- Start frontend, verify all API endpoints respond correctly
- Verify SSE log streaming with simulated UDP messages
- Verify service start/stop from UI
- Verify test start/stop with custom parameters
- Verify theme persistence
- Verify database admin
- Verify macOS .app bundle
- Write implementation report to `report.md`

**Verification**:
```bash
cd build && cmake .. -DBUILD_TESTS=ON && make && ctest
```
