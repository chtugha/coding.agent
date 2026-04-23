# Prodigy Frontend Architecture Guide

## 1. Architecture Overview

The frontend is a single-file C++ application (`frontend.cpp`, ~11,500 lines) that embeds all HTML, CSS, and JavaScript as raw string literals. It compiles to a single binary `bin/frontend`.

### Key Method Responsibilities

| Method | Purpose |
|--------|---------|
| `main()` | Signal setup, CWD resolution (chdir + symlink), project root detection, server instantiation |
| `FrontendServer(port, root)` | Constructor: init DB, discover tests, load services, scan test files |
| `start()` | Event loop: mongoose poll, log flush, service health check, async task cleanup, log rotation |
| `serve_index()` | Delegates to `build_ui_html()` which composes the full HTML response |
| `build_ui_html()` | Assembles `<head>` (CSS, CDN links) + sidebar nav + `build_ui_pages()` + `build_ui_js()` |
| `build_ui_pages()` | Returns HTML for all page divs (dashboard, tests, services, beta-testing, etc.) |
| `build_ui_js()` | Returns all JS logic: navigation, polling, fetch handlers, UI updates |
| `http_handler()` | Router: dispatches 50+ API endpoints to handler methods |
| `init_database()` | Opens SQLite, verifies writable, disables load_extension, creates schema, runs migrations |
| `discover_tests()` | Populates hardcoded test binary list (6 entries) |
| `load_services()` | Reads service configs from `service_config` DB table |

### Data Flow

```
UDP:22022 ─→ log_receiver_loop() ─→ enqueue_log() ─→ flush_log_queue() ─→ SQLite logs table
                                                    └─→ recent_logs_ ring buffer ─→ SSE broadcast
                                                                                 └─→ /api/dashboard
```

## 2. How to Add a New Pipeline Node to the Dashboard

1. **HTML** in `build_ui_pages()`: Add a new `wt-pipeline-node` div inside the `.pipeline-flow` container:
   ```html
   <div class="wt-pipeline-connector"></div>
   <div class="wt-pipeline-node" id="pipeline-node-NEW_SERVICE">
     <span class="node-label">NEW</span>
     <span class="node-status offline" id="pipeline-status-NEW_SERVICE"></span>
   </div>
   ```

2. **API** in `handle_dashboard()`: Add `"NEW_SERVICE"` to the static `pipeline` JSON array.

3. **Service config**: Add the service to the `service_config` table (via `init_database()` seed or manually) so it appears in the `services_` vector.

4. The JS `fetchDashboard()` function automatically maps service names to pipeline node status dots using the `pipeline` array from the API response.

## 3. How to Add a New Test Category to Beta Testing

1. **HTML** in `build_ui_pages()`: Add the test panel HTML inside the appropriate `tab-pane` div:
   - Component tests: `#beta-component`
   - Pipeline tests: `#beta-pipeline`
   - Tools: `#beta-tools`

2. Use the collapsible pattern:
   ```html
   <div class="wt-card">
     <div class="wt-card-header" onclick="toggleCollapsible('new-test-body')">
       <span class="wt-card-title">New Test Name</span>
     </div>
     <div class="wt-collapsible" id="new-test-body">
       <!-- test controls and results -->
     </div>
   </div>
   ```

3. **JS** in `build_ui_js()`: Add the test logic (start handler, poll function, result display).

4. **API**: If the test needs a backend endpoint, add a handler method and route in `http_handler()`.

5. **Summary dot**: Add to `updateBetaSummaryDots()` to reflect test status in the summary bar.

## 4. How to Add a New Page

1. **HTML** in `build_ui_pages()`: Add a new page div:
   ```html
   <div class="wt-page" id="page-newpage">
     <div class="wt-content">
       <h2 class="wt-page-title">New Page</h2>
       <!-- content -->
     </div>
   </div>
   ```

2. **Nav entry** in `build_ui_html()`: Add a nav item in the appropriate sidebar section:
   ```html
   <a class="wt-nav-item" data-page="newpage" onclick="showPage('newpage')">
     <span class="nav-icon">&#x1F4C4;</span><span class="nav-text">New Page</span>
   </a>
   ```

3. **JS** in `showPage()`: Add activation logic:
   ```javascript
   if(p==='newpage'){fetchNewPageData();}
   ```

4. **Polling** (optional): If the page needs live updates, create `startNewPagePoll()` / `stopNewPagePoll()` functions and call them from `showPage()`.

## 5. How to Add a New API Endpoint

1. **Handler method**: Add a new method to `FrontendServer`:
   ```cpp
   void handle_new_endpoint(struct mg_connection *c, struct mg_http_message *hm) {
       // implementation
       mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"ok\":true}");
   }
   ```

2. **Route** in `http_handler()`: Add a new `else if` clause:
   ```cpp
   } else if (mg_strcmp(hm->uri, mg_str("/api/new/endpoint")) == 0) {
       handle_new_endpoint(c, hm);
   }
   ```

3. **Document** the endpoint in the file-header API index comment (search for `// HTTP API index` near the top of the file).

### TTS dock status (`GET /api/tts/status`)

The frontend exposes the TTS engine slot via a single read-only endpoint.
`handle_tts_status()` opens a short-lived TCP connection to the dock's cmd
port (`127.0.0.1:13142`), writes `STATUS\n`, parses the reply
(`ACTIVE <name>\n` or `NONE\n`), and returns:

```json
{"engine": "kokoro"}   // dock up, Kokoro currently docked
{"engine": "neutts"}   // dock up, NeuTTS currently docked
{"engine": null}        // dock up, no engine docked
```

There is **no** selection endpoint — operators switch engines by starting
the desired engine via `/api/services/start`; the dock performs the
last-connect-wins swap on its own. `fetchDashboard()` polls this endpoint
every `POLL_STATUS_MS` and updates the `#pipeline-tts-engine` sub-label
under the TTS pipeline node.

## 6. CSS Design System Reference

### Custom Properties (--wt-*)

| Group | Properties | Purpose |
|-------|-----------|---------|
| Layout | `--wt-sidebar-width`, `--wt-radius`, `--wt-radius-lg` | Sizing and border radius |
| Colors | `--wt-bg`, `--wt-card-bg`, `--wt-border`, `--wt-text`, `--wt-text-secondary` | Core palette |
| Semantic | `--wt-accent`, `--wt-success`, `--wt-danger`, `--wt-warning` | Status colors |
| Gradients | `--wt-gradient-hero/success/danger/warning/info/neutral/pipeline` | Background gradients |
| Surfaces | `--wt-surface-elevated`, `--wt-surface-sunken` | Depth layering |
| Shadows | `--wt-shadow-sm/md/lg`, `--wt-shadow-glow-success/danger` | Elevation |
| Chart | `--wt-chart-1` through `--wt-chart-7` | Chart.js dataset colors |
| Typography | `--wt-font`, `--wt-mono` | Font stacks |

### Component Classes

| Class | Purpose |
|-------|---------|
| `.wt-card` | Standard content card with border, radius, hover shadow |
| `.wt-metric-card` | Dashboard metric card (gradient bg, large value) |
| `.wt-pipeline-hero` | Pipeline visualization container (gradient hero) |
| `.wt-pipeline-node` | Individual pipeline service node (64x64, glassmorphism) |
| `.wt-pipeline-connector` | Animated connector line between nodes |
| `.wt-btn`, `.wt-btn-primary/danger/secondary` | Button styles |
| `.wt-nav-item` | Sidebar navigation link |
| `.wt-page` | Page container (visibility/opacity transition) |
| `.wt-collapsible` | Expandable panel (max-height transition) |
| `.wt-beta-tabs` | Tab bar for Beta Testing sub-tabs |
| `.wt-test-summary-bar` | Summary dots bar above beta test tabs |

### Animation Classes

| Animation | Purpose |
|-----------|---------|
| `@keyframes pulse` | Status dot pulsing (running state) |
| `@keyframes slideIn` | Element entrance (opacity + translateY) |
| `@keyframes countPulse` | Metric value update (scale bounce) |
| `@keyframes flowPulse` | Pipeline connector flow animation |
| `.metric-updated` | Applied to metric values after count-up completes |

## 7. Named Constants Reference

### C++ Constants (file scope, `static constexpr`)

| Constant | Value | Description |
|----------|-------|-------------|
| `LOG_FLUSH_INTERVAL_MS` | 500 | Batch-INSERT cadence for log writer |
| `UDP_BUFFER_SIZE` | 4096 | Max UDP datagram size |
| `DB_QUERY_ROW_LIMIT` | 10000 | Max rows from /api/db/query |
| `MG_POLL_TIMEOUT_MS` | 100 | Mongoose event-loop poll timeout |
| `LOG_RETENTION_DAYS` | 30 | Delete logs older than this |
| `SERVICE_CHECK_INTERVAL_S` | 2 | Reap dead child processes interval |
| `ASYNC_CLEANUP_INTERVAL_S` | 30 | Clean finished async tasks interval |
| `RECENT_LOGS_API_LIMIT` | 100 | /api/logs/recent max entries |
| `DASHBOARD_RECENT_LOGS_LIMIT` | 10 | /api/dashboard activity feed entries |
| `SIGTERM_GRACE_US` | 500000 | 500ms grace after SIGTERM |
| `SERVICE_STARTUP_WAIT_US` | 200000 | 200ms post-ghost-kill delay |
| `TRANSCRIPTION_SETTLE_MS` | 5000 | Wait before reading transcription |
| `ACCURACY_INTER_FILE_MS` | 2000 | Pause between accuracy test files |

### JS Constants (declared at top of `build_ui_js()`)

| Constant | Value | Description |
|----------|-------|-------------|
| `POLL_STATUS_MS` | 3000 | Dashboard/status poll interval |
| `POLL_TESTS_MS` | 3000 | Test list poll interval |
| `POLL_SERVICES_MS` | 5000 | Service list poll interval |
| `POLL_TEST_LOG_MS` | 1500 | Test output poll interval |
| `POLL_SIP_STATS_MS` | 2000 | SIP RTP stats poll interval |
| `POLL_TEST_RESULTS_MS` | 5000 | Test results page poll interval |
| `SIP_MAX_LINES` | 20 | Maximum SIP lines in grid |
| `COUNTUP_STEP_MS` | 20 | Animation frame interval for count-up |
| `COUNTUP_DURATION_MS` | 400 | Total count-up animation duration |
| `TOAST_DURATION_MS` | 3000 | Toast notification display time |
| `STATUS_CLEAR_MS` | 5000 | Status message auto-clear timeout |

## 8. Test Infrastructure

### discover_tests()

Hardcoded list of test binaries (6 entries):

| Binary | Description |
|--------|-------------|
| `test_sanity` | Basic sanity checks (2 tests) |
| `test_interconnect` | Inter-service communication (30 tests) |
| `test_sip_provider_unit` | SIP provider unit tests (25 tests) |
| `test_kokoro_cpp` | Kokoro TTS engine tests (4 tests, 2-4 may SKIP without models) — the engine is tested in isolation; full-pipeline TTS runs go through `test_integration` against the TTS dock |
| `test_integration` | Full pipeline integration (SKIPs without running services) |
| `test_sip_provider` | SIP provider integration tests |

To add a new test binary:
1. Add to `CMakeLists.txt` test targets
2. Add entry to `discover_tests()` in `frontend.cpp`
3. Rebuild

### run_pipeline_test.py

Python script for full WER (Word Error Rate) pipeline tests. Expects:
- Log message pattern: `"Transcription (Xms): <text>"`
- API endpoints: `/api/services/start`, `/api/tests/start`, etc.

### Test Result Storage

Results are stored in multiple DB tables depending on test type:
- `service_test_runs`: General test results (service, type, status, metrics JSON)
- `whisper_accuracy_tests`: Whisper ASR accuracy per file
- `model_benchmark_runs`: Model benchmark aggregates
- `iap_quality_tests`: IAP codec quality metrics
- `tts_validation_tests`: TTS round-trip validation
- `test_runs`: Test binary execution history (exit code, log file)

## 9. tomedo-crawl / RAG Integration

### Frontend-Side Architecture

The frontend communicates with tomedo-crawl (port 13181) via a set of `/api/rag/*` proxy endpoints.  It also maintains configuration in the tomedo-crawl encrypted SQLite database (`tomedo-crawl.db`) via `rag_db_set_config()` / `rag_db_sync_all_config()`.

### New API Endpoints

| Method | Endpoint | Description |
|--------|----------|-------------|
| GET | `/api/rag/health` | Proxy GET 13181/health |
| GET/POST | `/api/rag/config` | Read/write tomedo-crawl config in its SQLite DB |
| POST | `/api/rag/cert_upload` | Receive PEM file upload, write to disk, update config |
| POST | `/api/rag/trigger_crawl` | Proxy POST 13181/crawl/trigger |
| GET | `/api/rag/ollama/models` | Proxy GET 13181/ollama/models |
| POST | `/api/rag/ollama/start` | Proxy POST 13181/ollama/start |
| POST | `/api/rag/ollama/stop` | Proxy POST 13181/ollama/stop |
| POST | `/api/rag/ollama/pull` | Proxy POST 13181/ollama/pull |
| POST | `/api/rag/wipe_vectors` | Proxy POST 13181/wipe |

### `rag_db_sync_all_config()`

Called by `handle_start_service()` before starting TOMEDO_CRAWL_SERVICE.  Reads all RAG-related settings from the frontend's `settings` SQLite table and writes them to the tomedo-crawl database's `config` table, ensuring the service always starts with the latest frontend configuration.

### Dashboard Integration

Two additional pipeline nodes are rendered for tomedo-crawl:
- **RAG** node (`pipeline-node-TOMEDO_CRAWL_SERVICE`, purple border): reflects tomedo-crawl process status.
- **Ollama** node (`pipeline-node-OLLAMA`, orange border): reflects `ollama_running` from the `/health` response.

The `ragDashInfo` span shows real-time indexed document count and last crawl time.

An overlay (`ollamaAlertOverlay`) appears when `/health` reports `ollama_installed: false`, giving the user **OK** (dismiss) and **Install** (trigger `ollama` download + install) options.

### Service Arguments Builder

The TOMEDO_CRAWL config panel generates the `service arguments` string from UI controls rather than free-text input.  `buildRagArgs()` is called by every control's `onchange` handler and writes to `svcDetailArgs`.

## 10. Security Model

### Local-Only Access

The HTTP server binds to `127.0.0.1` (loopback only). All security assumptions depend on this:
- No authentication on any endpoint
- No TLS encryption
- No CSRF protection
- Credentials stored in plaintext in SQLite

### SQL Query Guard

The `/api/db/query` endpoint is protected by multiple layers:
1. `db_write_mode_` flag (default: off) — blocks non-read-only queries
2. `is_read_only_query()` — whitelist-based: allows SELECT, EXPLAIN, safe PRAGMAs only
3. `strip_sql_comments()` — prevents comment-based bypass
4. `LOAD_EXTENSION` substring check — blocks extension loading via SQL
5. `sqlite3_db_config(SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0)` — runtime extension disable
6. `sqlite3_prepare_v2` — single-statement only (prevents multi-statement injection)

### Input Sanitization

- `kill_ghost_processes()`: Binary name validated against `[a-zA-Z0-9_.-]` regex before `popen()`
- `is_allowed_binary()`: Rejects paths with `..`, absolute paths, non-`bin/` prefixes
- `extract_json_string()`: Handles all JSON escape sequences including `\b` and `\f`
