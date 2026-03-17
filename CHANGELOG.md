# Changelog

## Frontend Overhaul (2026-03)

### Fixed
- **SQLite readonly database error**: Resolved `flush_log_queue: insert failed: attempt to write a readonly database` when started from `bin/` directory. Database path is now resolved to an absolute path based on project root. Added `sqlite3_db_readonly()` check after open — emits a clear fatal error with the resolved path instead of silently failing every 500ms.
- **CWD resolution**: Improved `main()` chdir logic to handle all invocation paths: `bin/frontend`, `./bin/frontend`, `cd bin && ./frontend`, absolute paths, and symlinks. Falls back with clear error if `bin/frontend` is not found at the resolved project root.
- **Test binaries gracefully skip** when dependencies are unavailable: `test_integration` uses `GTEST_SKIP()` when pipeline services are not running; `test_kokoro_cpp` tests 2-4 skip when model files are missing. Previously these timed out or failed with unhelpful errors.
- **Test runner output parsing**: Verified compatibility between gtest output format and `check_test_status()` / `handle_test_log()` parsing logic.
- **Pipeline test script**: Verified `run_pipeline_test.py` log message patterns and API endpoint calls match current implementation.

### Security
- **SQL query guard hardened**: `is_read_only_query()` now strips SQL comments (`--` and `/* */`) before keyword checking to prevent comment-based bypass. Added `LOAD_EXTENSION` substring check.
- **Extension loading disabled**: `sqlite3_db_config(SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0)` called in `init_database()` to disable extension loading at runtime.
- **popen input sanitized**: `kill_ghost_processes()` validates `binary_name` against `[a-zA-Z0-9_.-]` regex before passing to `popen()`. Invalid names are silently rejected.
- **JSON escape handling**: Added `\b` (backspace) and `\f` (form-feed) escape cases to `extract_json_string()`.
- **Local-only binding documented**: Added inline comment at `mg_http_listen` call documenting the `127.0.0.1` security assumption.

### Removed
- **Dead code**: Removed `is_service_running_unlocked()` — was never called anywhere in the codebase.

### Changed
- **C++ magic numbers replaced**: All numeric literals in the event loop, log infrastructure, and test runners extracted to `static constexpr` named constants with descriptive names and unit suffixes (e.g., `LOG_FLUSH_INTERVAL_MS`, `UDP_BUFFER_SIZE`, `SERVICE_CHECK_INTERVAL_S`).
- **JS magic numbers replaced**: All 45+ `setInterval`/`setTimeout` numeric literals extracted to a named-constants block at the top of the JS section (e.g., `POLL_STATUS_MS`, `DELAY_SERVICE_REFRESH_MS`, `SIP_MAX_LINES`).
- **Navigation restructured**: Sidebar reorganized into logical sections: Dashboard (default) > Pipeline (Services, Live Logs) > Testing (Test Runner, Test Results, Beta Tests) > Configuration (Models, Database, Credentials).
- **Dashboard is now the default page** (was previously Tests).
- **Page transitions**: Changed from `display:none/block` toggle to CSS `visibility`/`opacity`/`pointer-events` animation for smoother transitions. `.hidden` class retained for inline element toggling.
- **Beta Testing page split into tabs**: Component Tests, Pipeline Tests, and Tools organized as Bootstrap tabs with a summary status bar. Test panels are now collapsible.

### Added
- **Dashboard page**: Pipeline visualization with horizontal node chain showing live service status. Metric cards (Services Online, Running Tests, Tests Passed, Uptime) with count-up animation. Activity feed showing recent log entries. Quick action buttons (Start All, Stop All, Restart Failed). Overall health badge (Healthy/Degraded/Offline).
- **Dashboard API endpoint** (`GET /api/dashboard`): Returns combined JSON with service statuses, recent logs, test summary, uptime, and pipeline topology.
- **Test Results page**: Aggregated test results with summary metric cards, Chart.js trend chart (with zoom plugin), filter bar (test type, date range), and sortable results table.
- **Test Results Summary API endpoint** (`GET /api/test_results_summary`): Queries multiple DB tables (service_test_runs, whisper_accuracy_tests, model_benchmark_runs, iap_quality_tests) and returns unified results with summary statistics.
- **CSS design system**: Comprehensive custom properties for gradients (`--wt-gradient-*`), shadows (`--wt-shadow-*`), surfaces (`--wt-surface-*`), and chart colors (`--wt-chart-*`). New component classes for pipeline nodes, metric cards, collapsible panels, and beta test tabs.
- **Responsive breakpoints**: `@media (max-width:1024px)` for tablet layout, `@media (max-width:768px)` for mobile (icon-only sidebar).
- **Dark theme overrides**: `[data-bs-theme="dark"]` rules for pipeline hero, nodes, cards, and surfaces.
- **Animations**: `slideIn`, `countPulse`, `flowPulse` keyframe animations for dashboard elements.
- **Documentation**: Added `docs/frontend-guide.md` (user-facing) and `docs/frontend-architecture.md` (developer/maintenance) covering all new features and architecture.
