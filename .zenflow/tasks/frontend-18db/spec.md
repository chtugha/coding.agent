# Frontend.cpp Technical Specification

## Complexity Assessment: **HARD**

This is a complex, multi-component system requiring:
- Integration with existing interconnect protocol
- Real-time log streaming from multiple services
- Process lifecycle management
- WebSocket implementation for live updates
- macOS application bundling
- Database schema design
- Multi-threaded architecture coordination

---

## Overview

Build a comprehensive frontend management system for WhisperTalk as a static C++ binary that serves a Bootstrap-based web interface for managing, monitoring, and controlling all services and tests.

## Technical Context

### Language & Runtime
- **Language**: C++ (C++17)
- **Platform**: macOS (Apple Silicon), minimum Sonoma (14.0)
- **Build**: CMake 3.22+, static linking where possible

### Dependencies (Already Integrated)
- **Mongoose** (`mongoose.h/c`): Embedded web server with WebSocket support
- **SQLite** (`sqlite3.h/c`): Embedded SQL database
- **Interconnect** (`interconnect.h`): Master/slave service discovery and communication
- **Bootstrap 5.3**: Frontend CSS/JS framework (CDN-loaded)

### Architecture Goals
1. **Maximum Static Linking**: Single binary with minimal external dependencies
2. **Embedded Resources**: Web UI served from binary (mongoose packed FS or inline strings)
3. **Real-time Updates**: WebSocket-based live log streaming and status updates
4. **macOS Native**: Bundle as .app with proper Info.plist and icon

---

## Implementation Approach

### 1. Interconnect Integration

**Current State**: Frontend has basic InterconnectNode initialization but doesn't expose logging port info to other services.

**Required Changes**:
- Frontend registers as `ServiceType::FRONTEND` master/slave node
- Advertise logging port via interconnect negotiation protocol
- Services discover frontend's logging port and connect if available
- No changes to service business logic - only add optional log forwarding

**Mechanism**:
```cpp
// In interconnect.h - add capability announcement
struct ServiceCapabilities {
    bool has_logging_server;
    uint16_t logging_port;
};

// Frontend advertises via heartbeat metadata
// Services query capabilities and connect if available
```

### 2. Logging Server Architecture

**UDP Log Receiver** (Current):
- Simple UDP socket on `interconnect_port + 10`
- Services send fire-and-forget log messages
- Resilient to frontend restarts

**Format** (Proposed):
```
[TIMESTAMP] SERVICE_NAME LEVEL CALL_ID MESSAGE
```

**Storage**:
- Circular in-memory buffer (1000 recent entries)
- SQLite persistent storage with indexed queries
- Automatic log rotation/cleanup (>30 days old)

### 3. Service Management

**Discovery**:
- Query interconnect for all running services
- Track service status via heartbeat
- Display connection topology

**Lifecycle Control** (Read-Only):
- Frontend **displays** service status but **does not start/stop** main services
- Main services are managed externally (systemd, launchd, manual)
- Only tests can be started/stopped via frontend

**Real-time Status**:
- Online/offline state
- Active call count
- Ports (neg_in, neg_out, down_in/out, up_in/out)
- Last heartbeat timestamp
- Connection state (upstream/downstream)

### 4. Test Management

**Test Discovery**:
- Scan `bin/` directory for test binaries
- Metadata stored in hardcoded map (name → description, default args)
- Auto-detect executables matching `test_*` pattern

**Test Execution**:
- `fork()` + `execv()` with redirected stdout/stderr
- Log file: `logs/TEST_NAME_TIMESTAMP.log`
- Track PID, start/end time, exit code
- Store run history in SQLite

**Test Parameters**:
- Configurable CLI arguments via web UI
- Model selection for integration tests
- Duration, port, verbosity settings
- Persist last-used parameters per test

### 5. Web Interface Architecture

**Backend (Mongoose)**:
- HTTP server on port 8080 (configurable)
- RESTful JSON APIs for data
- WebSocket endpoint `/ws` for real-time updates
- Static HTML/CSS/JS served inline

**Frontend (Bootstrap 5.3 + Vanilla JS)**:
- **Two Main Sections**:
  1. **Testing**: Manage and monitor test binaries
  2. **Main Services**: Monitor production services (read-only)
  
- **Shared Components**:
  - Live log viewer (WebSocket stream)
  - Database query interface (custom SQLite admin)
  - Service topology visualization

**Real-time Updates**:
- WebSocket broadcasts:
  - New log entries
  - Service status changes
  - Test lifecycle events
  - Call start/end notifications

### 6. Database Schema

```sql
-- Logs with full-text search
CREATE TABLE logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT NOT NULL,
    service TEXT NOT NULL,
    call_id INTEGER,
    level TEXT,      -- DEBUG, INFO, WARN, ERROR
    message TEXT,
    INDEX idx_timestamp,
    INDEX idx_service,
    INDEX idx_call_id
);

-- Test execution history
CREATE TABLE test_runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    test_name TEXT NOT NULL,
    start_time INTEGER,
    end_time INTEGER,
    exit_code INTEGER,
    arguments TEXT,  -- JSON array
    log_file TEXT,
    INDEX idx_test_name,
    INDEX idx_start_time
);

-- Service status snapshots
CREATE TABLE service_status (
    service TEXT PRIMARY KEY,
    status TEXT,           -- online, offline, degraded
    last_seen INTEGER,
    call_count INTEGER,
    ports TEXT,            -- JSON object
    metadata TEXT          -- JSON: connections, latency, etc.
);

-- Call trace history (for debugging)
CREATE TABLE call_traces (
    call_id INTEGER PRIMARY KEY,
    start_time INTEGER,
    end_time INTEGER,
    trace_json TEXT,       -- PacketTrace serialized
    total_latency_ms REAL
);
```

### 7. REST API Endpoints

**Tests**:
- `GET /api/tests` → List all tests with status
- `POST /api/tests/start` → Start test with parameters
- `POST /api/tests/stop` → Stop running test
- `GET /api/tests/:name/logs` → Stream log file
- `GET /api/tests/:name/history` → Past run results

**Services**:
- `GET /api/services` → All services with live status
- `GET /api/services/:name` → Detailed service info
- `GET /api/services/:name/logs` → Filtered logs for service

**Logs**:
- `GET /api/logs?service=X&level=Y&limit=N` → Query logs
- `GET /api/logs/recent` → Last 100 entries
- `WebSocket /ws/logs` → Live log stream

**Database**:
- `POST /api/db/query` → Execute SQL query (whitelist SELECTs)
- `GET /api/db/schema` → Database schema info
- `GET /api/db/stats` → Database size, table counts

**System**:
- `GET /api/status` → Frontend health, uptime, stats
- `GET /api/topology` → Service connection graph

### 8. macOS App Bundling

**Directory Structure**:
```
WhisperTalk.app/
  Contents/
    MacOS/
      frontend           # Main binary (renamed)
    Resources/
      icon.icns          # Application icon
      frontend.db        # SQLite database (copied on first run)
    Info.plist           # Bundle metadata
    PkgInfo              # Type/creator codes
```

**Info.plist** (Minimal):
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
    <key>LSMinimumSystemVersion</key>
    <string>14.0</string>
    <key>LSUIElement</key>
    <false/>
</dict>
</plist>
```

**Build Script**: `scripts/package-frontend-app.sh`
- Creates .app bundle structure
- Copies binary and resources
- Code signs (if certificate available)
- Creates DMG installer

---

## Source Code Structure Changes

### Files to Modify

#### 1. **`frontend.cpp`** (Major Enhancement)
**Changes**:
- Add WebSocket handler for real-time updates
- Implement service discovery via interconnect queries
- Add test parameter configuration UI
- Enhance logging with structured format
- Remove hardcoded HTML, use mongoose packed FS or optimized inline strings
- Add service-specific log filtering
- Implement call trace viewer

**Estimated LOC**: 1200 → 2500 lines

#### 2. **`interconnect.h`** (Minor Addition)
**Changes**:
- Add `ServiceCapabilities` struct for metadata exchange
- Add `query_service_capabilities()` method
- Add optional logging port announcement in heartbeat
- Add method to query all active services

**Estimated LOC**: +150 lines (new methods in existing class)

#### 3. **Service Files** (Minimal Changes - Logging Integration)
Affected: `whisper-service.cpp`, `llama-service.cpp`, `kokoro-service.cpp`, `sip-client-main.cpp`, `inbound-audio-processor.cpp`, `outbound-audio-processor.cpp`

**Changes** (per file):
```cpp
// Add at startup:
std::optional<uint16_t> logging_port = interconnect_.query_frontend_logging_port();
if (logging_port) {
    setup_log_forwarding(*logging_port);
}

// Helper function (add once):
void forward_log(const std::string& level, const std::string& msg) {
    if (log_socket_ >= 0) {
        std::string log_msg = format_log(level, msg);
        sendto(log_socket_, log_msg.c_str(), log_msg.size(), 0, 
               (struct sockaddr*)&log_addr_, sizeof(log_addr_));
    }
}
```

**Estimated LOC per service**: +30 lines (non-invasive, optional feature)

#### 4. **`CMakeLists.txt`** (Minor Update)
**Changes**:
- Ensure frontend links all required libraries
- Add install target for .app bundle (macOS)
- Add custom target for `package-frontend-app.sh`

### New Files to Create

#### 1. **`scripts/package-frontend-app.sh`**
Bash script to create macOS application bundle and DMG.

#### 2. **`Resources/Info.plist.in`**
Template for macOS app bundle metadata.

#### 3. **`Resources/icon.icns`** (Optional)
Application icon for macOS (can use placeholder initially).

---

## Data Model / API Changes

### Interconnect Protocol Extensions

**New Negotiation Message Type**:
```cpp
enum class NegotiationType : uint8_t {
    // ... existing types ...
    QUERY_CAPABILITIES = 10,
    ANNOUNCE_CAPABILITIES = 11
};
```

**Capabilities Exchange**:
```cpp
struct ServiceCapabilities {
    uint8_t service_type;
    bool has_logging_server;
    uint16_t logging_port;
    bool has_metrics_server;
    uint16_t metrics_port;
    char reserved[32];  // Future expansion
};
```

### Log Message Format (UDP)

**Wire Format** (Text-based for debuggability):
```
[2024-02-18T12:47:10.123Z] WHISPER_SERVICE INFO 42 VAD detected speech start
```

**Fields**:
1. ISO 8601 timestamp with milliseconds
2. Service name (enum string)
3. Log level (DEBUG/INFO/WARN/ERROR)
4. Call ID (0 if not call-specific)
5. Message (remaining text)

### WebSocket Protocol

**Client → Server**:
```json
{
    "type": "subscribe",
    "channels": ["logs", "services", "tests"]
}
```

**Server → Client**:
```json
{
    "type": "log",
    "timestamp": "2024-02-18T12:47:10.123Z",
    "service": "WHISPER_SERVICE",
    "level": "INFO",
    "call_id": 42,
    "message": "VAD detected speech start"
}
```

```json
{
    "type": "service_status",
    "service": "LLAMA_SERVICE",
    "online": true,
    "calls": 2,
    "last_seen": 1708262830
}
```

```json
{
    "type": "test_status",
    "test": "test_integration",
    "running": true,
    "pid": 12345,
    "start_time": 1708262800
}
```

---

## Verification Approach

### Unit Tests (Extend `tests/test_frontend.cpp` - NEW)

1. **Database Operations**:
   - Schema creation
   - Log insertion/query
   - Test run history tracking

2. **Interconnect Integration**:
   - Frontend registration as FRONTEND service
   - Capability announcement
   - Service discovery queries

3. **Log Parsing**:
   - Parse structured log messages
   - Handle malformed input
   - Timestamp parsing

### Integration Tests

1. **Service Discovery**:
   - Start multiple services
   - Verify frontend discovers all
   - Verify status updates

2. **Log Forwarding**:
   - Services send logs to frontend
   - Verify storage in SQLite
   - Verify WebSocket broadcast

3. **Test Lifecycle**:
   - Start test via API
   - Monitor log output
   - Capture exit code
   - Verify history storage

### Manual Verification

1. **Web UI**:
   - Open http://localhost:8080
   - Verify all tabs load
   - Test responsive design (mobile/desktop)

2. **Real-time Updates**:
   - Start a test
   - Verify live log updates
   - Verify service status changes

3. **Database Admin**:
   - Execute SELECT queries
   - Verify results display
   - Test query error handling

4. **macOS App**:
   - Double-click WhisperTalk.app
   - Verify icon appears in Dock
   - Verify app opens browser to localhost:8080
   - Test quit and restart

### Build & Test Commands

```bash
# Build
mkdir -p build && cd build
cmake .. -DBUILD_TESTS=ON
make frontend

# Run
./bin/frontend --port 8080

# Test
make test

# Package macOS app
./scripts/package-frontend-app.sh

# Open app
open dist/WhisperTalk.app
```

---

## Key Design Decisions

### 1. Why SQLite over MySQL?
- **Embedded**: No external database server required
- **Static**: Can be linked into binary
- **Simple**: Perfect for local-first app
- **Portable**: Single file database

### 2. Why No PHP?
- **Complexity**: Embedding PHP into C++ is non-trivial
- **Size**: libphp increases binary size significantly
- **Static**: PHP requires runtime, violates static binary goal
- **Modern Web**: Bootstrap + vanilla JS is sufficient

### 3. Why WebSocket for Logs?
- **Real-time**: Sub-second latency for log updates
- **Efficient**: Binary protocol, low overhead
- **Standard**: Native browser support
- **Scalable**: Handles 100+ logs/sec easily

### 4. Why Read-Only Service Management?
- **Safety**: Prevents accidental service crashes via UI
- **Separation**: Services managed by system (launchd/systemd)
- **Scope**: Frontend is for monitoring, not orchestration
- **Tests Exception**: Tests are ephemeral and safe to control

### 5. Why UDP for Log Transport?
- **Fire-and-forget**: Services don't block on logging
- **Resilient**: Services continue if frontend is down
- **Simple**: No connection management overhead
- **Fast**: Minimal latency impact

---

## Risk Mitigation

### Performance Risks

**Risk**: WebSocket broadcasting to multiple clients causes CPU spike.
**Mitigation**: 
- Throttle broadcasts (100 logs/sec max)
- Use message batching
- Limit max concurrent WebSocket connections (10)

**Risk**: SQLite writes block main thread.
**Mitigation**:
- Use separate thread for DB writes
- Batch inserts (every 100ms or 50 logs)
- Use WAL mode for concurrent reads

### Security Risks

**Risk**: SQL injection via `/api/db/query`.
**Mitigation**:
- Whitelist allowed statements (SELECT only)
- Use sqlite3_prepare_v2 with parameter binding
- Limit query execution time (5s timeout)

**Risk**: Arbitrary command execution via test parameters.
**Mitigation**:
- Whitelist allowed test binaries
- Sanitize all arguments
- Use `execv()` not `system()`

### Compatibility Risks

**Risk**: Frontend doesn't detect old services (pre-logging support).
**Mitigation**:
- Logging is optional - services work without it
- Graceful degradation (show "logs unavailable")
- Version detection via interconnect protocol

---

## Success Criteria

1. ✅ Frontend displays all 6 services with real-time status
2. ✅ All 6 tests can be started/stopped via web UI
3. ✅ Live log streaming shows messages within 100ms
4. ✅ Database query interface executes SELECT statements
5. ✅ macOS app bundle launches and opens browser
6. ✅ Services forward logs without code changes to business logic
7. ✅ Frontend survives service crashes and restarts
8. ✅ Binary is <50MB and runs without external dependencies (except system libs)

---

## Future Enhancements (Out of Scope)

- Call trace visualization (timeline diagram)
- Prometheus metrics export
- Multi-frontend cluster (load balancing)
- Remote access (authentication required)
- Log export (CSV, JSON)
- Service configuration editor
- Performance profiling integration
