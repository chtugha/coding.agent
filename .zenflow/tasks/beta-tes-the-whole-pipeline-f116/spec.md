# Technical Specification: Beta Pipeline Testing & Optimization

## Difficulty Assessment: HARD

Multi-week, multi-component task spanning:
- Real-time log level control (frontend → services, live)
- Crash-proof logging infrastructure verification and hardening
- Interconnect behavior testing per service pair
- Full-pipeline WER and latency benchmarking
- Bottleneck identification and performance tuning
- Comprehensive documentation for all 7 services + frontend

---

## Technical Context

**Language**: C++17, Python 3.9+  
**Build System**: CMake ≥ 3.22 (target `bin/` directory)  
**Platform**: macOS Apple Silicon (CoreML/Metal acceleration)  
**Core Library**: `interconnect.h` — shared peer-to-peer TCP interconnect, `LogForwarder` (UDP to frontend)

### Key Port Map
| Service | Base | Mgmt | Data | Cmd |
|---------|------|------|------|-----|
| SIP_CLIENT | 13100 | 13100 | 13101 | 13102 |
| IAP | 13110 | 13110 | 13111 | 13112 |
| VAD | 13115 | 13115 | 13116 | 13117 |
| WHISPER | 13120 | 13120 | 13121 | 13122 |
| LLAMA | 13130 | 13130 | 13131 | 13132 |
| KOKORO | 13140 | 13140 | 13141 | 13142 |
| OAP | 13150 | 13150 | 13151 | 13152 |
| FRONTEND | 13160 | — | — | — |
| Frontend Log (UDP) | — | — | — | 22022 |

---

## Current State Analysis

### What Already Exists

1. **Log Level UI** (`frontend.cpp` ~line 1996–2003, JS at ~2817–2838):
   - "Service Log Levels" card in Settings page
   - `loadLogLevels()` / `saveAllLogLevels()` JavaScript functions
   - `GET/POST /api/settings/log_level` API stores levels in SQLite
   - **Gap**: Only persists to DB. Does NOT push to running services.

2. **LogForwarder** (`interconnect.h` class `LogForwarder`, lines 1228–1274):
   - UDP sender (loopback port 22022) used by all services
   - `forward(level, call_id, fmt, ...)` — sends regardless of log level
   - **Gap**: No level filtering. Always sends. No `set_level()`.

3. **Command Ports**: All 7 services have TCP command listeners:
   - IAP (13112), VAD (13117), Whisper (13122), LLaMA (13132), Kokoro (13142), OAP (13152), SIP (13102)
   - All support `PING` → `PONG` baseline
   - **Gap**: None handle `SET_LOG_LEVEL` command.

4. **Frontend log receiver** (`frontend.cpp` process_log_message): Parses UDP datagrams, stores in SQLite. Functional.

5. **Pipeline test** (`tests/run_pipeline_test.py`): Injects WAV samples via Test SIP Provider, reads transcriptions from `/api/logs`, computes similarity vs ground truth. 20 samples in `Testfiles/`.

6. **Test SIP Provider** (`tests/test_sip_provider.cpp`): Compiled as `bin/test_sip_provider`. Exposes HTTP API on port 22011. Accepts `/inject` POST with `{file, leg}`.

---

## Implementation Approach

### Area 1: Log Level Filtering (interconnect.h + all services)

**Problem**: `LogForwarder.forward()` sends all log messages regardless of level. Frontend saves levels to DB but never notifies running services.

**Solution**:
1. Add `LogLevel` enum and filtering to `LogForwarder` in `interconnect.h`:
   ```cpp
   enum class LogLevel : int { ERROR=0, WARN=1, INFO=2, DEBUG=3, TRACE=4 };
   static LogLevel level_from_string(const char* s);
   void set_level(LogLevel lvl);
   // forward() checks: if numeric_level(level) > log_level_, return early
   ```
2. Add `--log-level <LEVEL>` CLI argument to each service (startup default).
3. Add `SET_LOG_LEVEL:<LEVEL>` command to each service's command port handler → calls `log_fwd_.set_level(...)`.
4. In `frontend.cpp` `handle_log_level_settings` POST handler: after saving to DB, call `send_negotiation_command(service, "SET_LOG_LEVEL:" + level)` for the affected service. Ignore errors if service is offline.

**Files modified**:
- `interconnect.h`: `LogForwarder` class — add `LogLevel` enum, `log_level_` field, `set_level()`, `level_from_string()`, update `forward()`
- `inbound-audio-processor.cpp`: add `--log-level` arg + `SET_LOG_LEVEL` command handler
- `vad-service.cpp`: same
- `whisper-service.cpp`: same
- `llama-service.cpp`: same
- `kokoro-service.cpp`: same
- `outbound-audio-processor.cpp`: same
- `sip-client-main.cpp`: same
- `frontend.cpp`: push `SET_LOG_LEVEL` when log level saved + service enum mapping

### Area 2: Logging Robustness

**Problem**: Verify the full logging chain is crash-proof. Check:
- Frontend UDP socket survives unexpected datagrams (malformed, oversized)
- Services don't crash if frontend is down (LogForwarder uses non-blocking UDP sendto)
- Log ring buffer (`recent_logs_` deque) has proper `MAX_RECENT_LOGS` cap
- SQLite log writes don't block service log reception

**Files to audit and harden**:
- `interconnect.h` `LogForwarder::forward()` — verify buffer sizes (currently `msg[2048]`, `buf[2200]`)
- `frontend.cpp` `process_log_message()` — verify parse robustness on malformed input
- `frontend.cpp` `run_log_server()` — verify recv buffer size vs max message size

### Area 3: Interconnect / Service Communication Testing

**Test targets** (service start/stop/reconnect scenarios):
- IAP reconnects to VAD after VAD restart → data resumes
- VAD reconnects to Whisper after Whisper restart → chunks forwarded
- Whisper reconnects to LLaMA after LLaMA restart → buffered packets drain
- LLaMA reconnects to Kokoro after Kokoro restart → response forwarded
- SPEECH_ACTIVE signal propagates correctly through pipeline
- CALL_END propagates and cleans state in each service

**Speed improvement candidates** in `interconnect.h`:
- `DOWNSTREAM_RECONNECT_MS = 500` — could reduce to 200ms for faster recovery
- TCP_NODELAY already set — verify on all sockets
- Whisper buffer: currently `MAX_BUFFER_PACKETS = 64` — appropriate for burst handling

**Files modified**:
- `interconnect.h`: potentially reduce `DOWNSTREAM_RECONNECT_MS`, ensure TCP_NODELAY on all sockets

### Area 4: Full Pipeline Performance Testing

**Test flow**:
1. Start all services (SIP, IAP, VAD, Whisper, LLaMA, Kokoro, OAP, Frontend)
2. Start Test SIP Provider
3. Inject all 20 WAV samples via `run_pipeline_test.py`
4. Measure latency per stage: VAD flush time, Whisper inference ms, LLaMA response ms, Kokoro synthesis ms
5. Check for bottlenecks in log output

**Performance targets**:
- Whisper inference: ≤ 300ms for 1-4s chunks (Apple Silicon CoreML)
- LLaMA response: ≤ 500ms (Llama-3.2-1B-Instruct, greedy, max 64 tokens)
- Kokoro synthesis: ≤ 300ms for short responses
- End-to-end: Speech end → audio playback start ≤ 1500ms (real-time threshold)
- WER: ≥ 90% similarity (Levenshtein-based), target ≥ 99.5% PASS rate

**Files modified if bottlenecks found**:
- `vad-service.cpp`: VAD params tuning (silence threshold, chunk size)
- `whisper-service.cpp`: Whisper params tuning (decoding strategy, language detection)
- `llama-service.cpp`: LLaMA params tuning (max tokens, sampling)

### Area 5: Documentation

**Files to document** (code-level inline docs, not separate MD files unless already existing .md files):
- `interconnect.h` — architecture overview, port map, `InterconnectNode` API, `LogForwarder` usage
- `inbound-audio-processor.cpp` — FIR filter design, upsample pipeline
- `vad-service.cpp` — VAD algorithm params
- `whisper-service.cpp` — decoding strategy, hallucination filter
- `llama-service.cpp` — chat template, shut-up mechanism
- `kokoro-service.cpp` — CoreML split decoder, phonemization
- `outbound-audio-processor.cpp` — downsampling, G.711 encoding, jitter buffer
- `sip-client-main.cpp` — SIP/RTP routing
- `frontend.cpp` — HTTP API surface, log processing, test infrastructure
- `tests/run_pipeline_test.py` — WER computation, test runner
- Update `.zencoder/rules/*.md` summaries to reflect any changes

---

## Source Code Structure Changes

### Modified Files
| File | Change |
|------|--------|
| `interconnect.h` | `LogForwarder`: add `LogLevel` enum, `set_level()`, `level_from_string()`, filtering in `forward()` |
| `inbound-audio-processor.cpp` | `--log-level` arg, `SET_LOG_LEVEL` command |
| `vad-service.cpp` | `--log-level` arg, `SET_LOG_LEVEL` command |
| `whisper-service.cpp` | `--log-level` arg, `SET_LOG_LEVEL` command |
| `llama-service.cpp` | `--log-level` arg, `SET_LOG_LEVEL` command |
| `kokoro-service.cpp` | `--log-level` arg, `SET_LOG_LEVEL` command |
| `outbound-audio-processor.cpp` | `--log-level` arg, `SET_LOG_LEVEL` command |
| `sip-client-main.cpp` | `--log-level` arg, `SET_LOG_LEVEL` command |
| `frontend.cpp` | Push `SET_LOG_LEVEL` to service on level save; verify logging robustness |
| `tests/run_pipeline_test.py` | Enhance with per-stage latency reporting, bottleneck flags |

### No New Files
All changes are in-place modifications to existing source files.

---

## API / Interface Changes

### New Command Protocol (all services)
```
Frontend → Service cmd port (TCP, one-shot):
  "SET_LOG_LEVEL:ERROR"  → "OK\n"
  "SET_LOG_LEVEL:WARN"   → "OK\n"
  "SET_LOG_LEVEL:INFO"   → "OK\n"
  "SET_LOG_LEVEL:DEBUG"  → "OK\n"
  "SET_LOG_LEVEL:TRACE"  → "OK\n"
  "SET_LOG_LEVEL:INVALID" → "ERROR:Unknown level\n"
```

### Updated CLI Arguments (all services)
```
--log-level <LEVEL>   Set initial log verbosity (ERROR|WARN|INFO|DEBUG|TRACE), default: INFO
```

### Frontend API: `POST /api/settings/log_level` (enhanced)
- Existing: saves to SQLite
- New: also calls `send_negotiation_command(service, "SET_LOG_LEVEL:" + level)` and returns success/error for live push

---

## Verification Approach

### Build
```bash
cd coding.agent && mkdir -p build && cd build && cmake .. && make -j$(nproc) 2>&1
```

### Log Level Verification
1. Start whisper-service with `--log-level ERROR`
2. Inject audio → verify only ERROR logs appear in frontend
3. Use frontend Settings → Service Log Levels → set Whisper to DEBUG → Save All
4. Inject audio → verify DEBUG logs now appear without restarting service

### Pipeline WER Test
```bash
python3 tests/run_pipeline_test.py "ggml-large-v3-turbo-q5_0" Testfiles
# Target: ≥18/20 PASS (≥90%), avg inference ≤300ms
```

### Interconnect Reconnect Test
```bash
# Kill whisper-service → verify VAD buffers → restart whisper → verify drain
# Kill llama-service → verify whisper buffers → restart llama → verify drain
```

### Latency Profiling
- Read frontend `/api/logs` for timestamps: VAD flush → Whisper transcription → LLaMA response → Kokoro synthesis start
- Target end-to-end ≤ 1500ms from VAD flush to OAP audio send

---

## Risk Assessment

| Risk | Mitigation |
|------|-----------|
| SET_LOG_LEVEL push fails (service offline) | Frontend handles silently; level applied at next start via CLI arg stored in DB |
| LogForwarder buffer overflow on TRACE level | Already bounded: `msg[2048]`, `buf[2200]`; verify sizes are sufficient |
| Whisper CoreML model not loaded | Check model path; test with CPU fallback |
| LLaMA slow on large context | Context cleared on CALL_END; bounded by 64 token max |
| VAD micro-pause too aggressive | Test with slow German speakers; tune `--vad-silence-ms` if needed |
