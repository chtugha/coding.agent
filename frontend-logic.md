# Frontend Logic Catalogue

This file catalogues complex logic decisions and their resolution paths during the frontend overhaul.

---

## Entry Format
- **Problem**: Brief description
- **Path**: Logic path to the solution
- **Resolution**: How it was resolved

---

## 1. Null Pointer Dereference in sqlite3_column_text Calls
- **Problem**: Several handlers cast sqlite3_column_text return values directly to std::string without null checks, causing UB on NULL columns.
- **Path**: frontend.cpp → load_services() lines 835-836, handle_models_get() line 6178+
- **Resolution**: Added null checks for all sqlite3_column_text calls before dereferencing.

## 2. Missing JSON Escaping in HTTP Responses
- **Problem**: handle_sip_lines() builds JSON with unescaped user/server/port fields from SIP protocol. handle_log_level_settings() injects service name directly into format string. handle_iap_quality_test() injects wav.error unescaped.
- **Path**: frontend.cpp → handle_sip_lines() lines 2265-2270, handle_log_level_settings() line 5623, handle_iap_quality_test() line 2550
- **Resolution**: Wrapped all user-derived values in escape_json() before embedding in JSON responses.

## 3. Path Traversal in File-Based API Handlers
- **Problem**: handle_iap_quality_test() constructs wav_path as "Testfiles/" + user-provided file name without validating for ".." or absolute paths.
- **Path**: frontend.cpp → handle_iap_quality_test() line 2546
- **Resolution**: Added path traversal validation (reject "..", leading "/", and non-whitelisted chars) before constructing file paths.

## 4. Extraction Pattern for log-server.h and database.h
- **Problem**: How to extract member functions to separate .h files while maintaining access to private class members.
- **Path**: Existing pattern in javascript.h/frontend-ui.h: declare member function in class body, define as inline FrontendServer::method() in .h, include after class definition.
- **Resolution**: Follow same pattern — forward-declare member functions in class body, implement in separate .h files included after class closing brace.

## 5. Detached Threads Without Lifecycle Management
- **Problem**: Four locations use std::thread(...).detach(): handle_switch_tts() (line 1636), handle_service_stop() for NEUTTS (line 1655), run_pipeline_stress_async() (line 4310), and model download (line 6214).
- **Path**: frontend.cpp lines 1636, 1655, 4310, 6214
- **Resolution**: The TTS-related threads (1636, 1655) are fire-and-forget operations that complete quickly. The stress test (4310) and model download (6214) are long-running but tracked via shared progress objects. All acceptable for local-only server; documented as known limitation.

## 6. Additional Null Pointer Dereferences Found in Triple Audit
- **Problem**: Two more locations with unchecked sqlite3_column_text: model lookup in handle_llama_benchmark (lines 3190-3192) and handle_models_get benchmark detail (lines 6412-6415) cast directly to std::string.
- **Path**: frontend.cpp → handle_llama_benchmark() model lookup, handle_models_get() benchmark detail section
- **Resolution**: Added null checks with fallback to empty string for all four column reads in both locations.

## 7. Neon Forge UI Design Implementation
- **Problem**: Replacing the tab-based navigation with a collapsible sidebar rail while preserving all page transitions and badge updates.
- **Path**: css.h (sidebar width transitions, nav-text hide/show), frontend.cpp (icon SVG, font imports), frontend-ui.h (sidebar HTML structure)
- **Resolution**: 64px collapsed rail expanding to 220px on hover via CSS transitions. Nav text and section titles hidden with opacity:0/width:0, revealed on sidebar hover. Active item uses left border accent + glow line animation.

## 8. Dashboard Synthwave Redesign
- **Problem**: Adding scanline overlays, neon borders, and pulsing data feed indicator that fits the cyberpunk aesthetic.
- **Path**: frontend-ui.h (dashboard section), css.h (gradient borders, scanline pseudo-elements), javascript.h (feed pulse animation reset)
- **Resolution**: Scanline overlay via body::before pseudo-element. Metric cards use colored neon borders matching their gradient. Real-Time Feed has a pulsing cyan dot that triggers rapid triple-pulse via animation reset trick on data change.

## 9. JavaScript ES6+ Refactoring Strategy
- **Problem**: Modernizing ~3200 lines of JS while preserving all HTML onclick handler references and DOM ID bindings.
- **Path**: javascript.h — all named functions kept as function declarations (hoisted), all anonymous callbacks converted to arrows, all var→const/let, all concatenation→template literals
- **Resolution**: Systematic pass through all sections: dashboard, services, tests, beta-testing, models, database, credentials. Zero remaining var declarations or anonymous function callbacks. Build verified clean.

## 10. SQL Injection via LIMIT Parameter
- **Problem**: handle_whisper_accuracy_results() concatenated a raw query string parameter directly into a SQL LIMIT clause, allowing arbitrary SQL injection.
- **Path**: frontend.cpp → handle_whisper_accuracy_results(), limit_str from URL query concatenated into SQL string
- **Resolution**: Parse limit to int with atoi(), range-validate (1-1000), then use std::to_string() for safe concatenation.

## 11. Argument Injection in Service Start
- **Problem**: VAD settings and log_level from the database were appended to command-line args without validation, allowing injection of arbitrary flags via crafted setting values.
- **Path**: frontend.cpp → start_service(), vad_w/t/s/c/g and log_level settings concatenated into use_args
- **Resolution**: Added is_numeric() lambda to validate all VAD settings. Added space-check for log_level to reject values containing spaces.

## 12. XSS via HTML Attribute Injection in onclick Handlers
- **Problem**: Service and test card rendering used safeAttr (backslash-escape only) inside onclick="fn('${safeAttr}')" — insufficient against HTML entity injection.
- **Path**: javascript.h → service card rendering and test card rendering, safeAttr pattern
- **Resolution**: Replaced with data-* attributes + this.dataset.* access, eliminating inline string interpolation in event handlers entirely.

## 13. ATTACH DATABASE Arbitrary File Read
- **Problem**: In write mode, a user could run ATTACH DATABASE to mount arbitrary files on disk as SQLite databases and read their contents, bypassing all query restrictions.
- **Path**: database.h → handle_db_query(), no check for ATTACH/DETACH keywords
- **Resolution**: Added unconditional blocklist for ATTACH and DETACH keywords alongside existing DROP TABLE/TRUNCATE guards.

## 14. sysconf(_SC_OPEN_MAX) Error Handling
- **Problem**: sysconf(_SC_OPEN_MAX) can return -1 on error; casting to int produces a negative loop bound that silently skips FD cleanup in forked child processes.
- **Path**: frontend.cpp → two fork() sites (start_service and handle_test_start)
- **Resolution**: Store return in long, check for < 0, fall back to 1024 on error before the close loop.
