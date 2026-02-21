# Frontend.cpp Technical Specification

## Complexity Assessment: **HARD**

---

## Confirmed Architecture Decisions

These decisions were explicitly confirmed by the user:

1. **SQLite + JavaScript** (not PHP/MySQL) — single static binary with SQLite embedded, Bootstrap+JS UI
2. **UDP log forwarding** — services forward logs to frontend's logging server; frontend also reads log files
3. **SSE (Server-Sent Events)** — for live log streaming from C++ server to browser
4. **5 pre-bundled themes** — switchable within the frontend UI (no PHP/Bootswatch)
5. **Regular macOS app** — dock icon, auto-opens browser on launch

---

## Overview

Build a comprehensive frontend management system for WhisperTalk as a **single static C++ binary** that:
- Serves a Bootstrap-based web UI via Mongoose
- Uses SQLite for persistent storage (logs, test history, service config, settings)
- Provides a logging server (UDP) that services connect to via the interconnect protocol
- Manages test lifecycle (start/stop/configure)
- Manages service lifecycle (start/stop/configure)
- Displays live logs via SSE
- Offers a SQLite database admin interface
- Bundles as a macOS `.app` (minimum Sonoma, Apple Silicon)

---

## Technical Context

### Language & Runtime
- **Language**: C++ (C++17)
- **Platform**: macOS (Apple Silicon), minimum Sonoma (14.0)
- **Build**: CMake 3.22+

### Dependencies (Already in Codebase)
- **Mongoose** (`mongoose.h/c`): Embedded web server — supports HTTP, SSE natively
- **SQLite** (`sqlite3.h/c`): Embedded SQL database
- **Interconnect** (`interconnect.h`): Master/slave service discovery and communication
- **Bootstrap 5.3**: Frontend CSS framework (CDN or embedded)

---

## Implementation Approach

### 1. Interconnect Integration — Logging Port Propagation

**Goal**: Frontend advertises its logging port to all services via the existing master/slave protocol. Services discover the port and optionally forward logs.

**Current state**: Frontend already creates an `InterconnectNode(ServiceType::FRONTEND)` and computes `log_port_ = interconnect_.ports().neg_in + 10`. It already has a UDP log receiver loop.

**Required changes to `interconnect.h`**:

Minimal addition — add a new negotiation message type so the master can propagate a "frontend logging port" to all registered services:

- Master (whichever service holds port 22222) stores a `frontend_log_port_` field
- Frontend sends `SET_LOG_PORT <port>` to master on startup
- Master includes `LOG_PORT <port>` in its `SYNC_REGISTRY` broadcasts to all slaves
- Each service stores the received log port and uses it if non-zero
- New public method: `uint16_t frontend_log_port() const` — returns 0 if unknown

This adds ~40 lines to `interconnect.h` (a new field, message handler case, getter). No structural changes.

**Required changes to each service** (minimal, ~10 lines each):

At startup after `interconnect_.initialize()`, each service checks `interconnect_.frontend_log_port()`. If non-zero, it creates a UDP socket and forwards log messages (fire-and-forget). A simple helper function wraps `fprintf(stderr, ...)` to also send via UDP if connected.

This is the **only change** to service files.

### 2. Logging Server (Dual-Source)

**Already exists** in `frontend.cpp:251-321` — UDP receiver on `log_port_`, circular buffer, SQLite writes.

**Two log sources**:

1. **UDP log forwarding** (real-time, from running services):
   - Services send structured messages via UDP to frontend's log port
   - Frontend receives, parses, stores in circular buffer + SQLite
   - This is the primary real-time source for service logs

2. **Log file tailing** (for tests and fallback):
   - Tests redirect stdout/stderr to `logs/<test_name>_<timestamp>.log`
   - Frontend tails active test log files using `kqueue` (macOS native file event API)
   - On each file change event, read new bytes, parse into log entries
   - Also used as fallback for services that started before frontend (read their existing log files)
   - Log file locations: `logs/` directory, discovered by convention (`logs/<service_name>.log`)

**Enhancements needed**:
- Parse structured log messages: `SERVICE_NAME LEVEL CALL_ID MESSAGE`
- Proper SQLite field population (currently hardcodes `ServiceType::SIP_CLIENT`)
- Batched SQLite writes (queue + flush every 500ms) to avoid per-message I/O
- Log rotation: delete entries older than 30 days on startup and periodically
- Index on `(service, timestamp)` for efficient filtered queries
- kqueue-based file watcher for test log files
- Maximum disk usage: cap log directory at 500MB, oldest files deleted first

### 3. SSE Live Log Streaming

**Mechanism**: Mongoose handles `GET /api/logs/stream` as an SSE endpoint.

When a browser connects to `/api/logs/stream?service=X&level=Y`:
- Mongoose connection is flagged as SSE (`c->data` stores filter state)
- On each `mg_mgr_poll` cycle, new log entries are checked and pushed as `data: {...}\n\n` to all SSE connections
- Connection auto-closes if client disconnects
- Client uses `EventSource` API (~20 lines JS) to listen and append to DOM

**SSE format**:
```
data: {"timestamp":"2026-02-18T13:00:00","service":"WHISPER_SERVICE","level":"INFO","call_id":42,"message":"Transcription complete"}

```

**Filter support via URL params**:
- `service` — filter by service name
- `level` — minimum log level (DEBUG, INFO, WARN, ERROR)
- `test` — filter logs for a specific test name (matches by PID/log source)

### 4. Web UI Architecture

The UI is served as inline HTML strings from the C++ binary (current approach in `frontend.cpp:352-415`). This will be significantly expanded.

**Two main sections** as requested:

#### A. Testing Section
1. **Overview page**: Cards for each discovered test showing:
   - Name, description, binary path
   - Status badge: Running (green) / Stopped (gray) / Failed (red)
   - Last run result (exit code, duration)
   - Quick-start button
2. **Per-test detail page** (`/test/<name>`):
   - a) Overview: description, binary path, default args, last 5 run history
   - b) Start/Restart controls with parameter editing (text inputs for args, model selector dropdown)
   - c) Live log field (SSE-driven, auto-scrolling `<pre>` element)
   - d) Last test result summary (exit code, duration, stdout/stderr tail)

#### B. Main (Services) Section
1. **Overview page**: Table/cards for all 6 services showing:
   - Service name, status (online/offline via interconnect heartbeat)
   - Active call count
   - Port config (neg_in, neg_out)
   - Last heartbeat timestamp
2. **Per-service detail page** (`/service/<name>`):
   - a) Overview: ports, connection state (upstream/downstream), call stats
   - b) Start/Restart controls with parameter editing:
     - SIP Client: SIP server IP, port, username, password
     - Whisper: model path selection
     - LLaMA: model path selection
     - Kokoro: voice selection, language
     - IAP/OAP: no special params
   - c) Live log field (SSE-driven, filtered to this service)

#### C. Database Admin
- SQL query textarea with execute button
- Results displayed as Bootstrap table
- Schema viewer (list tables, columns, indexes)
- Pre-built query shortcuts (recent logs, test history, service stats)
- **Safety**: Only SELECT queries allowed by default; an in-memory toggle (resets on page refresh) to enable write queries. Write mode shows a confirmation dialog ("Are you sure? This will modify the database.") before execution. No authentication required (localhost-only threat model)

#### D. Theme Switcher
5 pre-bundled themes. Implementation:

**Storage**: Theme name stored in SQLite `settings` table (`key='theme'`, `value='dark'`). Loaded on page serve; default is `default`.

**Switching mechanism**:
- Themes 1-2 use Bootstrap's built-in `data-bs-theme` attribute on `<html>`:
  - Default: `<html data-bs-theme="light">`
  - Dark: `<html data-bs-theme="dark">`
- Themes 3-5 use Bootswatch CSS served from an endpoint `/css/theme/<name>`:
  - The CSS is stored as `static const char*` string literals in `frontend.cpp`
  - On page load, the `<link>` tag's `href` is set based on the stored theme
- Theme dropdown in navbar sends `POST /api/settings` with `{"key":"theme","value":"<name>"}`
- After POST succeeds, JS reloads the page to apply the new theme server-side

**Themes**:
  1. **Default** — Bootstrap 5.3 default (light)
  2. **Dark** — Bootstrap dark mode (`data-bs-theme="dark"`)
  3. **Slate** — dark blue/gray professional look
  4. **Flatly** — clean flat design
  5. **Cyborg** — dark with neon accents

For themes 3-5, embed the Bootswatch CSS directly as `static const char*` string literals (~15KB each, ~45KB total).

### 5. Service Lifecycle Management

**Services section** allows starting, stopping, and restarting the 6 pipeline services.

**Dual status tracking**:
- **Interconnect status**: `is_service_alive()` detects services started externally (before frontend)
- **PID tracking**: For services started BY the frontend via fork/exec

**Start** (only if service is not already running):
- Validate binary exists at configured path and is executable
- `fork()`+`execv()` with configured args
- Redirect stdout/stderr to `logs/<service_name>.log`
- Store PID in `service_status` table

**Stop**:
- `kill(pid, SIGTERM)`, wait up to 5 seconds
- If still running: `kill(pid, SIGKILL)`
- Clear PID from `service_status`

**Restart** (for parameter changes):
- Stop current instance
- Start with new parameters from the form

**Externally started services**:
- If a service is detected via interconnect heartbeat but was NOT started by frontend, the UI shows it as "running (external)"
- Stop/restart buttons are disabled for external services (frontend doesn't own the PID)
- Only live logs and status monitoring are available

**Service config persistence**:
- Binary path + arguments stored in SQLite `service_config` table
- Edited parameters are saved to DB before start
- Persist across frontend restarts

### 6. Test Lifecycle Management

**Already partially implemented** in `frontend.cpp:482-538`. Enhancements:

- Parameter editing UI before start (form with default_args pre-filled)
- Persist custom args to SQLite so they survive restart
- Store test run history (start_time, end_time, exit_code, args) — schema already exists
- Capture stdout/stderr to log file AND forward to logging server
- Display last N test results with pass/fail badges

### 7. REST API Endpoints

**Tests**:
- `GET /api/tests` — list all tests with status
- `GET /api/tests/<name>` — detail for one test
- `POST /api/tests/<name>/start` — start test (body: `{"args": [...]}`)
- `POST /api/tests/<name>/stop` — stop test
- `GET /api/tests/<name>/history` — past run results
- `GET /api/tests/<name>/log` — current log file contents (tail)

**Services**:
- `GET /api/services` — all services with live status from interconnect
- `GET /api/services/<name>` — detailed service info
- `POST /api/services/<name>/start` — start service (body: `{"args": [...]}`)
- `POST /api/services/<name>/stop` — stop service
- `GET /api/services/<name>/config` — get stored config
- `POST /api/services/<name>/config` — update stored config

**Logs**:
- `GET /api/logs?service=X&level=Y&limit=N&offset=O` — query from SQLite
- `GET /api/logs/stream?service=X&level=Y` — SSE live stream
- `GET /api/logs/recent` — last 100 entries from memory

**Database**:
- `POST /api/db/query` — execute SQL query (body: `{"query": "..."}`)
- `GET /api/db/schema` — table/column listing

**Settings**:
- `GET /api/settings` — current settings (theme, etc.)
- `POST /api/settings` — update settings

**System**:
- `GET /api/status` — frontend health, uptime, connected services count

### 8. Database Schema

```sql
CREATE TABLE IF NOT EXISTS logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT NOT NULL,
    service TEXT NOT NULL,
    call_id INTEGER DEFAULT 0,
    level TEXT DEFAULT 'INFO',
    message TEXT,
    source TEXT DEFAULT 'udp'
);
CREATE INDEX IF NOT EXISTS idx_logs_svc_ts ON logs(service, timestamp);
CREATE INDEX IF NOT EXISTS idx_logs_level ON logs(level);

CREATE TABLE IF NOT EXISTS test_runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    test_name TEXT NOT NULL,
    start_time INTEGER,
    end_time INTEGER,
    exit_code INTEGER,
    arguments TEXT,
    log_file TEXT
);
CREATE INDEX IF NOT EXISTS idx_test_runs_name ON test_runs(test_name, start_time DESC);

CREATE TABLE IF NOT EXISTS service_config (
    service TEXT PRIMARY KEY,
    binary_path TEXT NOT NULL,
    arguments TEXT DEFAULT '[]',
    auto_start INTEGER DEFAULT 0,
    description TEXT
);

CREATE TABLE IF NOT EXISTS service_status (
    service TEXT PRIMARY KEY,
    status TEXT DEFAULT 'offline',
    pid INTEGER DEFAULT 0,
    last_seen INTEGER,
    call_count INTEGER DEFAULT 0,
    ports TEXT
);

CREATE TABLE IF NOT EXISTS settings (
    key TEXT PRIMARY KEY,
    value TEXT
);
```

**Schema versioning**: Store `schema_version` in `settings` table. On startup, check version and run migration SQL if needed. All tables use `CREATE TABLE IF NOT EXISTS` for forward compatibility.

**Pre-populated `service_config`**:
| service | binary_path | arguments |
|---------|------------|-----------|
| SIP_CLIENT | bin/sip-client | `["--port","5060"]` |
| INBOUND_AUDIO_PROCESSOR | bin/inbound-audio-processor | `[]` |
| WHISPER_SERVICE | bin/whisper-service | `["bin/models/ggml-large-v3-turbo.bin"]` |
| LLAMA_SERVICE | bin/llama-service | `["bin/models/llama-3.2-1b-instruct-q8_0.gguf"]` |
| KOKORO_SERVICE | bin/kokoro-service | `[]` |
| OUTBOUND_AUDIO_PROCESSOR | bin/outbound-audio-processor | `[]` |

### 9. macOS App Bundling

**Structure**:
```
WhisperTalk.app/
  Contents/
    MacOS/
      frontend
    Resources/
      icon.icns
    Info.plist
    PkgInfo
```

**Info.plist**:
```xml
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleExecutable</key>
    <string>frontend</string>
    <key>CFBundleIdentifier</key>
    <string>ai.whispertalk.frontend</string>
    <key>CFBundleName</key>
    <string>WhisperTalk</string>
    <key>CFBundleVersion</key>
    <string>1.0</string>
    <key>CFBundleShortVersionString</key>
    <string>1.0</string>
    <key>LSMinimumSystemVersion</key>
    <string>14.0</string>
    <key>LSUIElement</key>
    <false/>
    <key>NSHighResolutionCapable</key>
    <true/>
</dict>
</plist>
```

**Auto-open browser**: After HTTP server starts successfully, open browser in a child process:
```cpp
if (fork() == 0) {
    execl("/usr/bin/open", "open",
          ("http://localhost:" + std::to_string(port)).c_str(), NULL);
    _exit(0);
}
```
Uses `execl` instead of `system()` to avoid shell expansion. Frontend continues running if browser is closed (it's a server, not a GUI app).

**Build script**: `scripts/package-frontend-app.sh` creates the `.app` bundle, copies binary, signs if cert available.

---

## Source Code Changes

### Files to Modify

#### 1. `frontend.cpp` (Major Enhancement)
Current: 613 lines. Target: ~3000-4000 lines (bulk is inline HTML/CSS string literals for UI and embedded Bootswatch themes).

Changes:
- Add SSE handler for live log streaming
- Add service lifecycle management (start/stop/restart via fork/exec)
- Add per-service and per-test detail API endpoints
- Expand HTML UI with Testing and Main sections, theme switcher
- Add service config persistence in SQLite
- Add settings API (theme storage)
- Enhance log message parsing (structured format)
- Add batched SQLite log writes
- Add log rotation
- Auto-open browser on startup
- Add parameter editing forms in UI

#### 2. `interconnect.h` (Minor Addition, ~40 lines)
Changes:
- Add `frontend_log_port_` field (uint16_t, default 0)
- Add `SET_LOG_PORT` negotiation message handler (master stores the port)
- Include log port in `SYNC_REGISTRY` broadcast
- Parse log port from `SYNC_REGISTRY` on slave side
- Add `uint16_t frontend_log_port() const` getter
- Add `void set_frontend_log_port(uint16_t)` for frontend to call

#### 3. Service files (Minimal, ~10 lines each)
Files: `sip-client-main.cpp`, `inbound-audio-processor.cpp`, `outbound-audio-processor.cpp`, `whisper-service.cpp`, `llama-service.cpp`, `kokoro-service.cpp`

Each gets:
- After `interconnect_.initialize()`, check `interconnect_.frontend_log_port()`
- If non-zero, create a UDP socket to `127.0.0.1:<log_port>`
- Wrap existing `fprintf(stderr, ...)` / `std::cerr` calls with a helper that also sends via UDP
- Helper is a simple free function, ~15 lines, defined once in interconnect.h or inline in each file

#### 4. `CMakeLists.txt` (Minor)
- Add `package-frontend-app` custom target for `.app` bundling

### Files to Create

#### 1. `scripts/package-frontend-app.sh`
Shell script to create macOS `.app` bundle structure, copy binary, generate Info.plist, codesign.

---

## Security Considerations

- **SQL injection**: Database admin endpoint validates queries. Default mode: SELECT-only. Write mode requires explicit toggle (stored in session state, not persistent).
- **Command injection**: Service/test args are passed as explicit `execv` argv, never through `system()` or shell. No shell expansion occurs.
- **Path traversal**: Log file paths are validated to stay within the `logs/` directory.
- **CORS**: Not needed — frontend is served from same origin.
- **Parameter validation**: Model paths validated to exist on disk before start. Port numbers validated as 1-65535. SIP credentials are not logged. Arguments passed via `execv` argv array (no shell expansion, no injection).

---

## Error Handling

### Service/Test Crashes
- `waitpid(WNOHANG)` polling in main loop detects crashed child processes
- Exit code and signal stored in DB; UI shows "crashed" badge with exit code
- Orphaned child processes: on frontend shutdown, `SIGTERM` all tracked PIDs, then `SIGKILL` after 5s timeout

### Frontend Crash Recovery
- On startup, scan `service_status` table for stale PIDs (from previous frontend instance)
- For each stale PID, check if process is still running via `kill(pid, 0)`
- If running: adopt it (resume PID tracking). If dead: clear from DB
- Running tests continue independently of frontend (they're separate processes)

### SQLite Corruption
- Use `PRAGMA journal_mode=WAL` for crash safety
- On open failure, attempt `PRAGMA integrity_check`; if corrupt, rename DB and create fresh

### Interconnect Unavailable
- If frontend can't bind interconnect ports (another master exists), log warning
- Frontend still functions for manual service management; just no live interconnect status
- Retry interconnect initialization every 30 seconds in background

### Concurrent Test Execution
- Multiple different tests CAN run simultaneously
- Same test CANNOT run twice concurrently (check `is_running` before start; return error if already running)
- Each test tracks its own PID independently

### UDP Log Packet Loss
- UDP is fire-and-forget; lost packets are simply missing from logs
- No retry mechanism (by design — logging must not impact service performance)
- If log volume is extremely high, batch size limits prevent SQLite overload

---

## Verification Approach

1. **Build**: `cd build && cmake .. && make frontend` — must compile cleanly
2. **Unit test**: Run `./bin/frontend --port 9999 &` then curl API endpoints
3. **SSE test**: `curl -N http://localhost:9999/api/logs/stream` — verify event stream format
4. **Integration**: Start frontend + one service, verify log forwarding appears in UI
5. **Theme test**: Switch themes in UI, verify persistence across page reloads
6. **App bundle**: Run `scripts/package-frontend-app.sh`, verify `.app` launches and opens browser
7. **Existing tests**: `cd build && cmake .. -DBUILD_TESTS=ON && make && ctest` — must all still pass (interconnect changes must not break existing protocol)

---

## Risk Mitigation

| Risk | Mitigation |
|------|-----------|
| SSE connection leak | Track SSE connections; clean up on disconnect; limit max concurrent SSE clients to 20 |
| Log volume overwhelms SQLite | Batched writes (500ms flush); circular buffer caps memory; 30-day rotation; 500MB disk cap |
| Service start failure | Validate binary exists and is executable before fork; report clear error in API response |
| Interconnect protocol change breaks services | New message types are additive; unknown messages already ignored by existing handlers |
| Theme CSS size | ~15KB per Bootswatch theme; 5 themes = ~75KB — negligible in binary |
| Port conflicts | HTTP port configurable via `--port`; log port derived from interconnect; conflict logged clearly |
| Frontend crashes during test execution | Tests are independent processes; they continue running; frontend recovers PIDs on restart |
| Multiple frontends running simultaneously | Only one can bind the HTTP port; second instance fails with clear error |
| UDP log packet loss | Accepted by design; no retry; logging must not impact service latency |
| Apple Gatekeeper blocks unsigned app | Build script includes ad-hoc signing; document right-click → Open fallback |
| Incompatible interconnect protocol | SET_LOG_PORT is new; old services without it simply ignore unknown messages (already handled) |
| Services started without frontend | Detected via interconnect heartbeat; shown as "external" in UI; log file tailing as fallback |

---

## Success Criteria

1. Single binary `frontend` serves complete web UI on configurable HTTP port
2. Testing section: list tests, start/stop with custom args, view live logs, view history
3. Main section: list services, start/stop with custom args, view live logs, view status
4. SSE delivers log updates to browser within 1 second of receipt
5. Database admin allows SQL queries against SQLite
6. 5 themes switchable, persisted across sessions
7. Interconnect propagates log port; services forward logs when frontend is available
8. macOS `.app` bundle launches and auto-opens browser
9. All existing tests continue to pass
