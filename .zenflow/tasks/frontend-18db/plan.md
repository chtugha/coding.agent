# Frontend.cpp Implementation Plan

## Configuration
- **Artifacts Path**: `.zenflow/tasks/frontend-18db`
- **Complexity**: HARD
- **Estimated Duration**: 4-6 implementation sessions

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
- Architecture approach (static binary, Bootstrap UI, SQLite, WebSocket)
- Interconnect integration strategy
- Database schema and API design
- macOS app bundling plan
- Verification approach

---

### [ ] Step 1: Interconnect Capability System

**Objective**: Extend the interconnect protocol to support service capability announcement and discovery, enabling services to advertise optional features like logging endpoints.

**Tasks**:
- Add `ServiceCapabilities` struct to `interconnect.h`
- Implement capability announcement in heartbeat protocol
- Add `query_service_capabilities()` method to InterconnectNode
- Add `query_all_services()` method to get list of active services with status
- Write unit tests for capability exchange in `tests/test_interconnect.cpp`

**Files Modified**:
- `interconnect.h` (+150 lines for capability system)
- `tests/test_interconnect.cpp` (+100 lines for tests)

**Verification**:
```bash
cd build && make test_interconnect
./bin/test_interconnect --gtest_filter="*Capability*"
```

**Completion Criteria**:
- Services can announce capabilities via interconnect
- Master can query any service's capabilities
- Master can list all active services
- All tests pass

---

### [ ] Step 2: Frontend Logging Server Core

**Objective**: Implement the core logging infrastructure in frontend.cpp including UDP receiver, SQLite storage, and in-memory circular buffer.

**Tasks**:
- Enhance SQLite schema with proper indexes and log retention
- Implement UDP log receiver with structured message parsing
- Add circular in-memory log buffer (1000 entries max)
- Create background thread for batched SQLite writes
- Add log rotation/cleanup (delete logs >30 days old)
- Implement service status tracking via interconnect queries
- Announce logging port via interconnect capabilities

**Files Modified**:
- `frontend.cpp` (logging infrastructure: +400 lines)

**Verification**:
```bash
./bin/frontend --port 8080 &
echo "[2024-02-18T12:00:00.000Z] TEST_SERVICE INFO 0 Test log message" | nc -u localhost 9012
# Check logs appear in SQLite
sqlite3 frontend.db "SELECT * FROM logs WHERE service='TEST_SERVICE';"
```

**Completion Criteria**:
- UDP socket receives log messages
- Messages parsed into structured format
- Logs stored in SQLite with correct timestamps
- In-memory buffer maintains last 1000 entries
- Old logs auto-deleted after 30 days
- Frontend announces logging port via interconnect

---

### [ ] Step 3: Service Logging Integration

**Objective**: Add optional log forwarding to all 6 services without changing business logic.

**Tasks**:
- Add log forwarding helper functions to each service
- Query frontend logging port via interconnect at startup
- Forward key log events (start, stop, errors, call events)
- Ensure logging is non-blocking and optional (no failures if frontend down)

**Files Modified** (each service +30 lines):
- `sip-client-main.cpp`
- `inbound-audio-processor.cpp`
- `outbound-audio-processor.cpp`
- `whisper-service.cpp`
- `llama-service.cpp`
- `kokoro-service.cpp`

**Verification**:
```bash
# Start frontend
./bin/frontend --port 8080 &

# Start a service
./bin/whisper-service &

# Check logs appear
sqlite3 frontend.db "SELECT * FROM logs WHERE service='WHISPER_SERVICE' ORDER BY timestamp DESC LIMIT 10;"
```

**Completion Criteria**:
- All services discover frontend logging port
- Services forward logs via UDP
- Services continue working if frontend is unavailable
- No business logic changes (only logging additions)
- Logs visible in SQLite database

---

### [ ] Step 4: REST API Implementation

**Objective**: Implement all REST endpoints for tests, services, logs, and database queries.

**Tasks**:
- Implement test management APIs (list, start, stop, logs, history)
- Implement service status APIs (list, detail, filtered logs)
- Implement log query APIs with filtering (service, level, call_id, time range)
- Implement database query API with SQL whitelist (SELECT only)
- Add error handling and input validation
- Test all endpoints with curl/httpie

**Files Modified**:
- `frontend.cpp` (API handlers: +500 lines)

**Verification**:
```bash
# Start frontend
./bin/frontend --port 8080 &

# Test APIs
curl http://localhost:8080/api/services
curl http://localhost:8080/api/tests
curl http://localhost:8080/api/logs/recent
curl -X POST http://localhost:8080/api/tests/start -d '{"test":"test_sanity"}'
curl -X POST http://localhost:8080/api/db/query -d '{"query":"SELECT * FROM logs LIMIT 10"}'
```

**Completion Criteria**:
- All API endpoints return valid JSON
- Test start/stop works correctly
- Service status reflects reality (via interconnect)
- Log queries support filtering
- Database queries work with SELECT, reject INSERT/UPDATE/DELETE
- Proper HTTP status codes and error messages

---

### [ ] Step 5: WebSocket Real-time Updates

**Objective**: Implement WebSocket endpoint for real-time log streaming and status updates.

**Tasks**:
- Add WebSocket handler to mongoose (`/ws` endpoint)
- Implement subscription system (clients subscribe to channels)
- Broadcast log entries to all subscribed clients
- Broadcast service status changes
- Broadcast test lifecycle events
- Add connection management (track clients, handle disconnects)
- Implement message throttling (100 msg/sec per client)

**Files Modified**:
- `frontend.cpp` (WebSocket: +300 lines)

**Verification**:
```bash
# Start frontend
./bin/frontend --port 8080 &

# Test WebSocket with websocat or browser DevTools
websocat ws://localhost:8080/ws
# Send: {"type":"subscribe","channels":["logs","services"]}
# Verify real-time messages arrive

# Or test with JavaScript in browser console:
# ws = new WebSocket('ws://localhost:8080/ws');
# ws.onmessage = (e) => console.log(JSON.parse(e.data));
```

**Completion Criteria**:
- WebSocket connections established successfully
- Clients receive real-time log messages
- Service status updates broadcast automatically
- Test status changes broadcast when tests start/stop
- Multiple clients supported simultaneously
- Graceful handling of client disconnects

---

### [ ] Step 6: Complete Web UI

**Objective**: Build the full Bootstrap-based web interface with all features.

**Tasks**:
- Enhance HTML structure with proper Bootstrap layout
- Implement Tests tab:
  - Test list with status cards
  - Start/stop buttons with parameter inputs
  - Live log viewer (WebSocket-powered)
  - Test history table
- Implement Services tab:
  - Service status table with real-time updates
  - Connection topology display
  - Per-service log filtering
- Implement Logs tab:
  - Live log stream with auto-scroll
  - Filtering controls (service, level, call_id)
  - Search functionality
- Implement Database tab:
  - SQL query editor with syntax highlighting
  - Results table with pagination
  - Schema browser
- Add dark mode support (Bootstrap theme)
- Optimize JavaScript (minimize polling, use WebSocket)
- Make responsive for mobile/tablet

**Files Modified**:
- `frontend.cpp` (HTML/CSS/JS inline strings: +800 lines)

**Verification**:
```bash
./bin/frontend --port 8080
# Open http://localhost:8080 in browser
# Test all tabs and features
# Test on different screen sizes
# Verify real-time updates work
# Test with multiple browser tabs open
```

**Completion Criteria**:
- All 4 tabs render correctly
- Tests can be started/stopped via UI
- Live logs update in real-time (<100ms latency)
- Service status updates automatically
- Database queries execute and display results
- UI is responsive on desktop and mobile
- No console errors in browser DevTools

---

### [ ] Step 7: macOS App Bundling

**Objective**: Create macOS .app bundle with proper structure, icon, and installer.

**Tasks**:
- Create `Info.plist` template with proper metadata
- Create packaging script `scripts/package-frontend-app.sh`
- Generate or source application icon (`Resources/icon.icns`)
- Modify frontend to auto-open browser on startup (macOS only)
- Create DMG installer with drag-to-Applications
- Add code signing (optional, for distribution)
- Test app bundle on clean macOS system

**Files Created**:
- `Resources/Info.plist.in`
- `Resources/icon.icns`
- `scripts/package-frontend-app.sh`

**Files Modified**:
- `CMakeLists.txt` (add install targets for .app bundle)
- `frontend.cpp` (add auto-open browser on macOS: +50 lines)

**Verification**:
```bash
# Build and package
./scripts/package-frontend-app.sh

# Test app
open dist/WhisperTalk.app

# Verify:
# - App appears in Dock with icon
# - Browser opens automatically to http://localhost:8080
# - App can be quit via Dock or Cmd+Q
# - App can be dragged to /Applications
```

**Completion Criteria**:
- .app bundle structure is correct
- Info.plist has all required keys
- Icon displays properly
- App launches frontend binary
- Browser opens automatically on macOS
- DMG installer created
- App runs on fresh macOS system without dependencies

---

### [ ] Step 8: Testing & Documentation

**Objective**: Comprehensive testing and user documentation.

**Tasks**:
- Create frontend unit tests (`tests/test_frontend.cpp`):
  - Database schema and queries
  - Log parsing and formatting
  - WebSocket message encoding
  - Service discovery via interconnect
- Run integration tests:
  - Start all services + frontend
  - Verify logs appear
  - Verify service status correct
  - Start/stop tests via UI
- Create integration test script `tests/test_frontend_integration.sh`
- Write report documenting:
  - Implementation summary
  - Testing results
  - Known limitations
  - Deployment instructions

**Files Created**:
- `tests/test_frontend.cpp`
- `tests/test_frontend_integration.sh`
- `.zenflow/tasks/frontend-18db/report.md`

**Files Modified**:
- `CMakeLists.txt` (add test_frontend target)

**Verification**:
```bash
# Build tests
cd build && cmake .. -DBUILD_TESTS=ON && make

# Run unit tests
./bin/test_frontend

# Run integration test
cd .. && ./tests/test_frontend_integration.sh

# All tests pass
make test
```

**Completion Criteria**:
- All unit tests pass
- Integration test passes (all services communicate)
- No memory leaks (verified with valgrind on Linux or leaks on macOS)
- Report.md written with implementation summary
- All 8 success criteria from spec.md met

---

## Success Criteria (from spec.md)

- [ ] Frontend displays all 6 services with real-time status
- [ ] All 6 tests can be started/stopped via web UI
- [ ] Live log streaming shows messages within 100ms
- [ ] Database query interface executes SELECT statements
- [ ] macOS app bundle launches and opens browser
- [ ] Services forward logs without business logic changes
- [ ] Frontend survives service crashes and restarts
- [ ] Binary is <50MB and runs without external dependencies

---

## Notes

- Each step should be completed and verified before moving to the next
- Run `make frontend && ./bin/frontend` after each significant change
- Keep browser DevTools open to catch JavaScript errors
- Test with multiple services running simultaneously
- Use `sqlite3 frontend.db` to inspect database state
