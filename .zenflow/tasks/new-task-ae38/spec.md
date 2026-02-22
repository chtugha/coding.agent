# Technical Specification: Beta-Testing Infrastructure

**Project**: WhisperTalk  
**Feature**: Comprehensive Beta-Stage Testing and Optimization Suite  
**Date**: 2026-02-22  
**Version**: 1.0

---

## 1. Technical Context

### 1.1 Technology Stack

**Core Services (C++17)**:
- Build System: CMake 3.22+
- HTTP Server: Mongoose (embedded, single-file)
- Database: SQLite3 (embedded)
- Threading: std::thread, std::mutex, std::atomic
- Networking: POSIX sockets (TCP/UDP)
- ML Inference:
  - Whisper: whisper.cpp with CoreML/Metal acceleration
  - LLaMA: llama.cpp with Metal/MPS acceleration
  - Kokoro: PyTorch (libtorch C++ API) with CoreML acceleration

**Test Infrastructure**:
- Test SIP Provider: C++ with Mongoose HTTP API
- Frontend: C++ backend + embedded HTML/CSS/JavaScript
- Audio Processing: G.711 μ-law codec, PCM conversion

**Platform**:
- macOS 14+ (Apple Silicon: M1/M2/M3/M4)
- CoreML and Metal frameworks for GPU acceleration
- espeak-ng for phoneme generation

### 1.2 Existing Architecture

**Service Pipeline**:
```
SIP Client (UDP 5060, 9001/9002)
    ↓ RTP packets (UDP)
Inbound Audio Processor (TCP 13000+)
    ↓ float32 PCM audio
Whisper Service (TCP 8083)
    ↓ transcribed text
LLaMA Service (TCP 8090)
    ↓ response text
Kokoro Service (TCP 8090+)
    ↓ synthesized audio
Outbound Audio Processor (UDP 9002)
    ↓ G.711 RTP packets
SIP Client → Network
```

**Frontend Architecture**:
- Single C++ binary (`frontend.cpp`, 2443 lines)
- Mongoose HTTP server on port 8080
- SQLite database for logs, test runs, service status
- Server-Sent Events (SSE) for real-time log streaming
- Embedded HTML/CSS/JavaScript (no separate static files)
- Process management: fork/exec for services and tests

**Interconnect System** (`interconnect.h`, 1963 lines):
- Unified logging to UDP port 22022
- Service type enumeration (7 service types)
- Packet tracing with microsecond timestamps
- Master/Slave TCP connection patterns
- Control socket protocol (Unix domain sockets)

**Test SIP Provider** (`test_sip_provider.cpp`, 1001 lines):
- B2BUA (Back-to-Back User Agent) for 2-party calls
- RTP relay between two SIP clients
- Audio injection capability (WAV file → G.711 RTP)
- HTTP API for control (Mongoose)
- Multi-line support (up to 6 concurrent calls)

### 1.3 Existing Database Schema

**Tables**:
```sql
CREATE TABLE logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    timestamp TEXT NOT NULL,
    service TEXT NOT NULL,
    call_id INTEGER,
    level TEXT,
    message TEXT,
    INDEX(timestamp), INDEX(service)
);

CREATE TABLE test_runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    test_name TEXT NOT NULL,
    start_time INTEGER,
    end_time INTEGER,
    exit_code INTEGER,
    output_log TEXT
);

CREATE TABLE service_status (
    service TEXT PRIMARY KEY,
    status TEXT,
    last_seen INTEGER,
    call_count INTEGER,
    ports TEXT
);

CREATE TABLE service_config (
    service TEXT PRIMARY KEY,
    config_json TEXT
);
```

---

## 2. Implementation Approach

### 2.1 Design Philosophy

**Extend, Don't Rewrite**:
- Build on existing `frontend.cpp` service/test management
- Enhance `test_sip_provider.cpp` with multi-line and injection APIs
- Add new database tables for test results and model benchmarks
- Extend embedded HTML/JavaScript frontend with new pages

**Frontend-Driven Testing**:
- All test operations initiated via browser UI
- Real-time feedback via Server-Sent Events
- No command-line interaction required
- Service orchestration handled by frontend backend

**Progressive Integration**:
- Implement test infrastructure before optimization logic
- Test each service in isolation before full pipeline tests
- Add model benchmarking framework after accuracy testing works
- Incremental delivery with testable milestones

### 2.2 Key Technical Decisions

**Audio Injection Strategy**:
- Extend `test_sip_provider.cpp` with HTTP API: `POST /api/inject`
- Support WAV file formats: PCM (8/16/24kHz), μ-law, a-law
- Real-time conversion to G.711 RTP packets (20ms frames)
- Per-call injection control (inject to specific line)

**Multi-Line Architecture**:
- SIP client manages 1-6 concurrent calls (existing Call-ID mapping)
- IAP/OAP use call_id % 100 for port allocation (existing pattern)
- Test provider uses call_id for routing RTP streams
- Frontend displays line status grid (similar to existing service grid)

**Accuracy Measurement**:
- Levenshtein distance algorithm for text comparison
- Python `difflib` integration for detailed diff visualization
- Store ground truth .txt files in `Testfiles/` directory
- Threshold-based pass/fail: ≥99.5% = PASS, ≥90% = WARN, <90% = FAIL

**Model Benchmarking**:
- Service restart with new model parameters
- Capture latency using PacketTrace timestamps (existing infrastructure)
- Store results in new `model_benchmark_runs` table
- Frontend chart library: Chart.js (embedded inline, ~70KB minified)

**Log Depth Configuration**:
- Extend `service_config` table with `log_level` field
- Services poll SQLite every 5 seconds for config changes
- C++ log macros check level before logging
- Frontend dropdown per service (ERROR/WARN/INFO/DEBUG/TRACE)

### 2.3 Architecture Extensions

#### 2.3.1 Test SIP Provider Enhancements

**New HTTP API Endpoints**:
```
POST /api/inject
  Body: {call_id: 1, file_path: "Testfiles/sample_01.wav", target_leg: "a"}
  Response: {status: "injecting", duration_ms: 3200}

GET /api/calls
  Response: {calls: [{id: 1, leg_a: "alice", leg_b: "bob", duration: 15, ...}]}

POST /api/line/add
  Body: {line_id: 3, username: "charlie", password: "1234", server: "127.0.0.1:5060"}
  Response: {status: "registered", line_id: 3}

POST /api/line/remove
  Body: {line_id: 3}
  Response: {status: "removed"}
```

**Multi-Line Data Structures**:
```cpp
// Extend ActiveCall with line_id
struct ActiveCall {
    int id;              // call_id (1-based)
    int line_id;         // line identifier
    CallLeg leg_a;
    CallLeg leg_b;
    // ... existing fields
};

// Track registered lines
std::map<int, RegisteredLine> lines_;
std::mutex lines_mutex_;
```

#### 2.3.2 Frontend Backend Extensions

**New API Endpoints** (add to `frontend.cpp`):
```
GET /api/testfiles
  Response: {files: [{name: "sample_01.wav", size: 364380, duration: 3.2, ...}]}

POST /api/test/whisper-accuracy
  Body: {files: ["sample_01", "sample_02"], model: "large-v3-turbo"}
  Response: {test_id: 42, status: "running"}

GET /api/test/whisper-accuracy/42
  Response: {status: "completed", results: [{file: "sample_01", ...}]}

POST /api/model/add
  Body: {service: "whisper", name: "distil-large-v3", path: "models/...", ...}
  Response: {model_id: 5}

GET /api/models/{service}
  Response: {models: [{id: 1, name: "large-v3-turbo", ...}]}

POST /api/model/benchmark
  Body: {model_id: 5, test_files: ["sample_01", ...], iterations: 10}
  Response: {benchmark_id: 7}

GET /api/benchmark/7
  Response: {status: "completed", metrics: {accuracy: 96.5, avg_latency: 1234, ...}}

POST /api/settings/log-level
  Body: {service: "whisper-service", level: "DEBUG"}
  Response: {status: "updated"}
```

**Process Management Extensions**:
```cpp
// Add to FrontendServer class
struct TestRun {
    int id;
    std::string type;  // "whisper-accuracy", "model-benchmark", etc.
    std::string status; // "pending", "running", "completed", "failed"
    std::chrono::steady_clock::time_point start_time;
    std::vector<TestResult> results;
};

std::map<int, TestRun> active_tests_;
std::atomic<int> next_test_id_{1};
```

#### 2.3.3 Database Schema Additions

**New Tables**:
```sql
-- Test files metadata
CREATE TABLE testfiles (
    name TEXT PRIMARY KEY,
    size_bytes INTEGER,
    duration_sec REAL,
    sample_rate INTEGER,
    channels INTEGER,
    ground_truth TEXT,  -- content of corresponding .txt file
    last_modified INTEGER
);

-- Whisper accuracy test results
CREATE TABLE whisper_accuracy_tests (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    test_run_id INTEGER,
    file_name TEXT,
    model_name TEXT,
    ground_truth TEXT,
    transcription TEXT,
    similarity_percent REAL,
    latency_ms INTEGER,
    status TEXT,  -- "PASS", "WARN", "FAIL"
    timestamp INTEGER
);

-- Model registry
CREATE TABLE models (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    service TEXT,  -- "whisper", "llama", "kokoro"
    name TEXT,
    path TEXT,
    backend TEXT,  -- "CoreML", "Metal", "CPU"
    size_mb INTEGER,
    config_json TEXT,  -- model-specific parameters
    added_timestamp INTEGER
);

-- Model benchmark results
CREATE TABLE model_benchmark_runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    model_id INTEGER,
    test_files TEXT,  -- JSON array
    iterations INTEGER,
    avg_accuracy REAL,
    avg_latency_ms INTEGER,
    p50_latency_ms INTEGER,
    p95_latency_ms INTEGER,
    p99_latency_ms INTEGER,
    memory_mb INTEGER,
    timestamp INTEGER,
    FOREIGN KEY(model_id) REFERENCES models(id)
);

-- Round-trip TTS validation
CREATE TABLE tts_validation_tests (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    line1_call_id INTEGER,
    line2_call_id INTEGER,
    original_text TEXT,
    tts_transcription TEXT,
    similarity_percent REAL,
    phoneme_errors TEXT,  -- JSON array of problematic phonemes
    timestamp INTEGER
);

-- SIP line configurations
CREATE TABLE sip_lines (
    line_id INTEGER PRIMARY KEY,
    username TEXT,
    password TEXT,
    server TEXT,
    port INTEGER,
    status TEXT,  -- "idle", "registering", "registered", "in_call"
    last_registered INTEGER
);

-- Service test history (extends test_runs)
CREATE TABLE service_test_runs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    service TEXT,  -- "sip-client", "iap", "whisper", etc.
    test_type TEXT,  -- "rtp-routing", "conversion", "accuracy", etc.
    status TEXT,  -- "PASS", "FAIL", "PARTIAL"
    metrics_json TEXT,  -- test-specific metrics
    timestamp INTEGER
);
```

---

## 3. Source Code Structure Changes

### 3.1 New Files

**None** — All functionality integrated into existing files following the "ALWAYS prefer editing existing files" principle.

### 3.2 File Modifications

#### 3.2.1 `frontend.cpp` (Major Extensions)

**Additions** (~1000 lines):
- **Testfiles API**: Scan `Testfiles/` directory, parse WAV headers, load .txt files
- **Whisper Accuracy Test**: Orchestrate audio injection → capture Whisper output → compare with ground truth
- **Model Management API**: CRUD for model registry, benchmark execution
- **Chart.js Integration**: Embed minified library inline (or CDN link)
- **Frontend Pages**: Add HTML/JavaScript for:
  - Testing Dashboard (tabs: SIP, IAP, Whisper, LLaMA, Kokoro, OAP)
  - Model Comparison Page
  - TTS Validation Page
  - Log Configuration Panel

**Code Organization**:
```cpp
// Add after existing FrontendServer methods

// Testfiles management
void serve_testfiles_api(struct mg_connection *c);
void scan_testfiles_directory();
std::vector<TestFileInfo> testfiles_;

// Whisper accuracy testing
void handle_whisper_accuracy_test(struct mg_connection *c, struct mg_http_message *hm);
void run_whisper_accuracy_test_async(int test_id, std::vector<std::string> files, std::string model);
double calculate_levenshtein_similarity(const std::string& a, const std::string& b);

// Model management
void serve_models_api(struct mg_connection *c, struct mg_http_message *hm);
void handle_model_add(struct mg_connection *c, struct mg_http_message *hm);
void handle_model_benchmark(struct mg_connection *c, struct mg_http_message *hm);

// TTS validation
void handle_tts_validation_test(struct mg_connection *c, struct mg_http_message *hm);

// SIP line management
void handle_sip_line_add(struct mg_connection *c, struct mg_http_message *hm);
void handle_sip_line_remove(struct mg_connection *c, struct mg_http_message *hm);
void serve_sip_lines_api(struct mg_connection *c);

// Log level configuration
void handle_log_level_update(struct mg_connection *c, struct mg_http_message *hm);
```

**HTML/JavaScript Pages** (embedded in `serve_index()`):
- Extend existing tabbed interface
- Add "Testing" tab with service-specific sub-tabs
- Add "Models" tab with comparison charts
- Add "Configuration" tab for log levels and SIP lines

#### 3.2.2 `test_sip_provider.cpp` (Moderate Extensions)

**Additions** (~300 lines):
- **Multi-Line Support**: Track multiple calls in `std::map<int, ActiveCall>`
- **Audio Injection API**: Parse WAV files, convert to RTP, inject into call
- **Line Management API**: Add/remove lines dynamically
- **HTTP Endpoints**: Extend Mongoose routes

**Code Changes**:
```cpp
// Add HTTP endpoints
if (mg_strcmp(hm->uri, mg_str("/api/inject")) == 0) {
    handle_inject_audio(c, hm);
} else if (mg_strcmp(hm->uri, mg_str("/api/line/add")) == 0) {
    handle_line_add(c, hm);
} else if (mg_strcmp(hm->uri, mg_str("/api/line/remove")) == 0) {
    handle_line_remove(c, hm);
} else if (mg_strcmp(hm->uri, mg_str("/api/calls")) == 0) {
    serve_calls_status(c);
}

// Audio injection implementation
void handle_inject_audio(struct mg_connection *c, struct mg_http_message *hm) {
    // Parse JSON: call_id, file_path, target_leg
    // Load WAV file (reuse existing load_wav_file())
    // Spawn injection thread (reuse existing inject_thread pattern)
    // Return status
}
```

#### 3.2.3 Service Files (Minor Extensions)

**All Service Files** (`sip-client-main.cpp`, `inbound-audio-processor.cpp`, `whisper-service.cpp`, `llama-service.cpp`, `kokoro-service.cpp`, `outbound-audio-processor.cpp`):

**Additions** (~50 lines each):
- **Log Level Configuration**: Poll SQLite `service_config` table every 5s
- **Conditional Logging**: Wrap log calls with level checks

**Pattern**:
```cpp
// Add to each service
class ServiceConfig {
public:
    void load_from_db() {
        // Query: SELECT config_json FROM service_config WHERE service='whisper-service'
        // Parse log_level field
        current_log_level_ = parse_level(config_json);
    }
    
    bool should_log(LogLevel level) {
        return level >= current_log_level_;
    }

private:
    LogLevel current_log_level_ = LogLevel::INFO;
};

// Usage
if (config.should_log(LogLevel::DEBUG)) {
    interconnect_.log(call_id, "DEBUG", "VAD window: %.2f dB", energy);
}
```

**Config Polling Thread**:
```cpp
std::thread config_thread([&config]() {
    while (running) {
        config.load_from_db();
        std::this_thread::sleep_for(std::chrono::seconds(5));
    }
});
```

#### 3.2.4 `interconnect.h` (Minor Extensions)

**Additions** (~100 lines):
- **Log Levels Enum**: `enum class LogLevel { ERROR, WARN, INFO, DEBUG, TRACE }`
- **Helper Functions**: `log_level_from_string()`, `log_level_to_string()`

---

## 4. Data Model & API Changes

### 4.1 REST API Summary

| Endpoint | Method | Request Body | Response | Purpose |
|----------|--------|--------------|----------|---------|
| `/api/testfiles` | GET | — | `{files: [...]}` | List available test audio files |
| `/api/test/whisper-accuracy` | POST | `{files: [...], model: "..."}` | `{test_id: N}` | Start Whisper accuracy test |
| `/api/test/whisper-accuracy/{id}` | GET | — | `{status: "...", results: [...]}` | Get test results |
| `/api/model/add` | POST | `{service, name, path, ...}` | `{model_id: N}` | Register new model |
| `/api/models/{service}` | GET | — | `{models: [...]}` | List models for service |
| `/api/model/benchmark` | POST | `{model_id, test_files, iterations}` | `{benchmark_id: N}` | Start model benchmark |
| `/api/benchmark/{id}` | GET | — | `{status, metrics}` | Get benchmark results |
| `/api/sip/lines` | GET | — | `{lines: [...]}` | List SIP lines |
| `/api/sip/line/add` | POST | `{line_id, username, ...}` | `{status}` | Add SIP line |
| `/api/sip/line/remove` | POST | `{line_id}` | `{status}` | Remove SIP line |
| `/api/settings/log-level` | POST | `{service, level}` | `{status}` | Update log level |
| `/api/inject` | POST | `{call_id, file_path, target_leg}` | `{status}` | Inject audio into call (test_sip_provider) |

### 4.2 Data Flow Examples

#### Example 1: Whisper Accuracy Test
```
Frontend                    Backend                     Test SIP Provider           Whisper Service
   │                           │                               │                          │
   │─── POST /api/test/... ───►│                               │                          │
   │◄── {test_id: 42} ─────────│                               │                          │
   │                           │─── Start whisper-service ────►│                          │
   │                           │─── POST /api/inject ─────────►│                          │
   │                           │                               │──── RTP packets ────────►│
   │                           │◄── log: "Transcription: ..." ────────────────────────────│
   │◄── SSE: log event ────────│                               │                          │
   │                           │─── Compare with ground truth ─│                          │
   │                           │─── Save to DB ────────────────│                          │
   │─── GET /api/test/42 ──────►│                               │                          │
   │◄── {results: [...]} ──────│                               │                          │
```

#### Example 2: Multi-Line Setup
```
Frontend                    Backend                     SIP Client
   │                           │                               │
   │─── POST /api/sip/line/add ►│                               │
   │    {line_id: 2, ...}       │                               │
   │◄── {status: "ok"} ─────────│                               │
   │                           │─── Unix socket: ACTIVATE:2 ───►│
   │                           │                               │─── SIP REGISTER ───►
   │                           │◄── log: "Line 2 registered" ───│
   │◄── SSE: log event ────────│                               │
```

---

## 5. Delivery Phases

### Phase 1: Core Infrastructure (Week 1)
**Scope**: Testfiles management, audio injection, multi-line support

**Tasks**:
1. Extend `frontend.cpp` with `/api/testfiles` endpoint
2. Implement `scan_testfiles_directory()` with WAV header parsing
3. Add `testfiles` table to database schema
4. Extend `test_sip_provider.cpp` with `/api/inject` endpoint
5. Implement multi-line data structures in test provider
6. Add `/api/sip/line/add` and `/api/sip/line/remove` endpoints
7. Add frontend UI for SIP line management (embedded HTML)

**Deliverables**:
- Frontend displays list of 20 test files
- User can add/remove SIP lines via UI
- User can inject audio into a call via UI

**Verification**:
```bash
# Start frontend
./bin/frontend

# Open browser: http://localhost:8080
# Navigate to "Testing" tab
# Verify: 20 test files listed
# Verify: Can add line 2 (user: bob)
# Start test_sip_provider and sip-client
# Verify: Can inject sample_01.wav into call
```

### Phase 2: Service Interconnection Testing (Week 2)
**Scope**: Start/stop service controls, TCP connection monitoring

**Tasks**:
1. Extend service status API to show TCP connection states
2. Add frontend UI controls for individual service start/stop
3. Implement "Start All" and "Stop All" service sequences
4. Add service dependency graph visualization (HTML/CSS)
5. Extend logging to capture TCP reconnection events
6. Create Test 1 (SIP Client RTP Routing) frontend panel

**Deliverables**:
- Frontend shows green/red TCP connection indicators
- User can start/stop services individually
- Test 1 panel shows RTP packet counts and TCP connection time

**Verification**:
```bash
# Start frontend
# Start test_sip_provider
# Click "Start SIP Client" → verify status = running
# Click "Inject Audio" → verify RTP packets sent (IAP offline)
# Click "Start IAP" → verify TCP connection within 1s
# Re-inject audio → verify IAP receives packets
```

### Phase 3: Whisper Accuracy Testing (Week 3)
**Scope**: Ground truth comparison, accuracy metrics, VAD tuning

**Tasks**:
1. Implement Levenshtein distance algorithm in `frontend.cpp`
2. Add `/api/test/whisper-accuracy` endpoint
3. Create async test execution with result storage in database
4. Add `whisper_accuracy_tests` table
5. Create frontend "Whisper Test" page with file selector
6. Implement results table with pass/fail indicators
7. Add Test 3 (Whisper Accuracy + VAD) panel

**Deliverables**:
- User can select multiple test files and run accuracy test
- Frontend displays per-file similarity percentage
- Results stored in database with PASS/WARN/FAIL status

**Verification**:
```bash
# Run accuracy test on sample_01 to sample_10
# Verify: All files show ≥95% accuracy
# Verify: Results saved in whisper_accuracy_tests table
# Modify ground truth to introduce error
# Verify: Test shows FAIL status
```

### Phase 4: Model Benchmarking Framework (Week 4)
**Scope**: Model registry, benchmark execution, comparison charts

**Tasks**:
1. Add `models` and `model_benchmark_runs` tables
2. Implement `/api/model/add` and `/api/models/{service}` endpoints
3. Create model benchmark test orchestration
4. Embed Chart.js library in frontend HTML
5. Create "Models" page with comparison table and charts
6. Implement service restart with new model parameters
7. Add latency capture using PacketTrace

**Deliverables**:
- User can add new Whisper models via UI
- User can run benchmark test comparing 2+ models
- Frontend displays bar chart of latency and line chart of accuracy

**Verification**:
```bash
# Add model: distil-large-v3
# Run benchmark: large-v3-turbo vs distil-large-v3
# Verify: Comparison table shows accuracy and latency for both
# Verify: Chart displays results visually
```

### Phase 5: LLaMA & Kokoro Testing (Week 5)
**Scope**: LLaMA response quality, shut-up mechanism, TTS validation

**Tasks**:
1. Create Test 4 (LLaMA Response Quality) panel
2. Implement shut-up mechanism test (interrupt detection)
3. Create Test 5 (Kokoro TTS Quality) panel
4. Implement round-trip TTS validation (2-line setup)
5. Add `tts_validation_tests` table
6. Create "TTS Validation" page with phoneme diff highlighting
7. Add LLaMA and Kokoro models to benchmark framework

**Deliverables**:
- User can test LLaMA with 10 German prompts
- Shut-up mechanism validated (interrupt latency measured)
- Round-trip TTS test shows original vs transcribed text

**Verification**:
```bash
# Run LLaMA test with German questions
# Verify: Responses are concise (<50 words)
# Send interrupt signal mid-response
# Verify: Response stops within 500ms
# Run TTS validation test
# Verify: ≥95% similarity between original and transcribed text
```

### Phase 6: OAP & Full Pipeline Testing (Week 6)
**Scope**: OAP encoding/scheduling, end-to-end tests, optimization

**Tasks**:
1. Create Test 6 (OAP Encoding/Scheduling) panel
2. Implement RTP jitter analysis
3. Create full pipeline integration test (all 6 services)
4. Add progressive service test results page
5. Implement test history tracking (last 10 runs per test)
6. Add log level configuration UI
7. Performance optimization: identify and fix bottlenecks

**Deliverables**:
- User can run all 6 progressive tests sequentially
- Test history shows pass/fail trends over time
- Log level configurable per service without restart
- End-to-end latency ≤3000ms

**Verification**:
```bash
# Run all progressive tests
# Verify: All tests PASS
# Check end-to-end latency
# Verify: ≤3000ms from speech in to speech out
# Configure Whisper log level to DEBUG
# Verify: Whisper logs show DEBUG messages within 5s
# Configure back to INFO
# Verify: DEBUG messages stop
```

### Phase 7: Documentation & Polish (Week 7)
**Scope**: Inline documentation, README updates, UI refinements

**Tasks**:
1. Add JSDoc comments to all new functions
2. Add inline comments explaining complex logic
3. Update service README.md files with testing instructions
4. Document database schema with column descriptions
5. Add tooltips to frontend UI elements
6. Implement keyboard navigation for accessibility
7. Final bug fixes and edge case handling

**Deliverables**:
- All code documented with comments
- README.md in each service directory
- Frontend tooltips explain each control
- No crashes or deadlocks under stress testing

**Verification**:
- Code review: All public functions have JSDoc comments
- Manual UI testing: All buttons and controls work correctly
- Stress test: Run 100 consecutive tests without crashes
- Accessibility test: Navigate frontend using only keyboard

---

## 6. Verification Approach

### 6.1 Unit Testing

**Existing Tests** (reuse where applicable):
- `test_sip_provider_unit.cpp`: 25 tests for G.711, SIP parsing, RTP
- `test_interconnect.cpp`: TCP connection, logging infrastructure
- `test_integration.cpp`: End-to-end pipeline tests

**New Tests** (add to existing test files):
```cpp
// Add to test_sip_provider_unit.cpp
TEST(AudioInjectionTest, WavFileLoading) { /* ... */ }
TEST(AudioInjectionTest, G711Conversion) { /* ... */ }
TEST(MultiLineTest, LineRegistration) { /* ... */ }

// Add to test_integration.cpp
TEST(WhisperAccuracyTest, LevenshteinSimilarity) { /* ... */ }
TEST(ModelBenchmarkTest, LatencyCapture) { /* ... */ }
```

### 6.2 Integration Testing

**Test Scenarios** (execute via frontend UI):
1. **Single-Line Test**: Inject audio → verify all services process → verify response
2. **Multi-Line Test**: Setup 2 lines → inject on line 1 → verify line 2 receives response
3. **Service Resilience Test**: Stop IAP mid-stream → restart → verify recovery
4. **Accuracy Test**: Run all 20 test files → verify ≥95% average accuracy
5. **Model Benchmark Test**: Compare 3 Whisper models → verify latency captured
6. **TTS Round-Trip Test**: Line 1 generates speech → Line 2 transcribes → verify ≥95% match

### 6.3 Performance Benchmarks

**Target Metrics** (from NFR requirements):
- End-to-end latency: ≤3000ms
- Whisper transcription: ≤1500ms (3-second audio)
- LLaMA response: ≤500ms
- Kokoro synthesis: ≤300ms (10 words)
- Frontend UI response: ≤100ms

**Measurement Tools**:
- `PacketTrace::total_ms()` for end-to-end latency
- Per-service latency captured in logs (existing `interconnect_.log()`)
- Browser DevTools Network tab for frontend response time

### 6.4 Acceptance Criteria

**Phase 1-3 (Core + Whisper)**:
- [ ] All 20 test files listed in frontend
- [ ] Audio injection works on all test files
- [ ] Multi-line setup (2 lines) functional
- [ ] Whisper accuracy test runs successfully
- [ ] Average accuracy ≥95% on clean audio

**Phase 4-5 (Models + LLaMA/Kokoro)**:
- [ ] Model registry stores ≥3 Whisper models
- [ ] Benchmark comparison charts display correctly
- [ ] LLaMA responses are concise (<50 words)
- [ ] Shut-up mechanism works (interrupt latency ≤500ms)
- [ ] TTS validation shows ≥95% round-trip accuracy

**Phase 6-7 (OAP + Polish)**:
- [ ] All 6 progressive tests PASS
- [ ] RTP jitter analysis functional
- [ ] Log level configuration works without restart
- [ ] End-to-end latency ≤3000ms
- [ ] No crashes under 100 consecutive test runs
- [ ] All code documented with comments

### 6.5 Build & Lint Commands

**Build**:
```bash
cd /Users/ollama/.zenflow/worktrees/new-task-ae38
mkdir -p build && cd build
cmake .. -DBUILD_TESTS=ON
make -j$(sysctl -n hw.ncpu)
```

**Run Unit Tests**:
```bash
./bin/test_sip_provider_unit
./bin/test_interconnect
./bin/test_integration
```

**Static Analysis** (optional, if clang-tidy available):
```bash
clang-tidy frontend.cpp -- -std=c++17 -I. -I/opt/homebrew/include
```

**Runtime Verification**:
```bash
# Start frontend
./bin/frontend

# Open browser
open http://localhost:8080

# Execute all tests via UI
# Verify no errors in logs
```

---

## 7. Risk Mitigation

### 7.1 Technical Risks

**Risk**: Multi-line SIP handling causes port conflicts  
**Mitigation**: Use existing `call_id % 100` pattern for port allocation; test with ≤6 concurrent lines

**Risk**: Large model benchmarks exhaust memory  
**Mitigation**: Run benchmarks sequentially (not in parallel); add memory monitoring

**Risk**: Audio injection timing issues cause packet loss  
**Mitigation**: Reuse existing 20ms RTP packetization logic from `test_sip_provider.cpp`

**Risk**: Frontend becomes too large (>5000 lines)  
**Mitigation**: Use embedded HTML/JavaScript (already established pattern); keep C++ logic concise

### 7.2 Integration Risks

**Risk**: Levenshtein algorithm too slow for long transcriptions  
**Mitigation**: Use optimized implementation (Wagner-Fischer algorithm, O(m*n)); add timeout

**Risk**: Service restart during benchmark test causes data loss  
**Mitigation**: Store intermediate results in database after each iteration

**Risk**: Log spam during DEBUG level overwhelms SQLite  
**Mitigation**: Implement ring buffer (10,000 entries) in memory; persist to DB in batches

### 7.3 Performance Risks

**Risk**: Chart.js slows down frontend on large datasets  
**Mitigation**: Limit chart data to 100 points; use aggregation for larger datasets

**Risk**: 100 consecutive tests cause disk space issues (logs)  
**Mitigation**: Add log rotation (delete logs older than 7 days)

**Risk**: CoreML model loading adds latency to benchmark startup  
**Mitigation**: Measure and report model loading time separately from inference time

---

## 8. Open Questions & Assumptions

### 8.1 Assumptions

1. **Test files are valid**: All 20 WAV files in `Testfiles/` are properly formatted (PCM or μ-law)
2. **Ground truth is accurate**: .txt files contain 100% correct transcriptions
3. **Models are pre-downloaded**: User manually downloads models from Hugging Face before adding to registry
4. **Single frontend user**: No concurrent access to frontend UI (no auth/session management)
5. **macOS only**: No cross-platform compatibility required (leverages CoreML/Metal)

### 8.2 Clarifications Needed (Optional)

1. **Hugging Face integration**: Should frontend auto-download models, or is manual download acceptable?
   - **Decision**: Manual download acceptable for beta testing (reduces complexity)

2. **Phoneme-level diff**: How granular should TTS validation phoneme errors be?
   - **Decision**: Word-level diff sufficient; phoneme-level optional for future enhancement

3. **Test file formats**: Should frontend support MP3/FLAC, or only WAV?
   - **Decision**: WAV only (simpler, no codec dependencies)

4. **Chart interactivity**: Should charts support zoom/pan, or static is fine?
   - **Decision**: Static charts sufficient (Chart.js default behavior)

---

## 9. Summary

This technical specification defines a comprehensive beta-testing infrastructure for WhisperTalk, building on the existing C++ service architecture. The implementation extends `frontend.cpp` and `test_sip_provider.cpp` with new APIs, database tables, and embedded UI components, enabling frontend-driven testing of all 6 pipeline services with multi-line support, model benchmarking, and automated accuracy validation.

**Key Technical Highlights**:
- **Zero new files**: All functionality integrated into existing codebase
- **Mongoose + SQLite**: Leverages existing embedded HTTP server and database
- **PacketTrace integration**: Reuses existing latency measurement infrastructure
- **Progressive testing**: 6 dedicated test panels for systematic service validation
- **Model benchmarking**: Extensible framework for comparing Whisper, LLaMA, Kokoro models
- **Real-time feedback**: Server-Sent Events for live log streaming to browser
- **Production-ready**: No stubs, no simulations, all features fully functional

**Implementation Effort**: ~2500 lines of new C++ code, ~500 lines of embedded HTML/JavaScript, 7-week phased delivery.
