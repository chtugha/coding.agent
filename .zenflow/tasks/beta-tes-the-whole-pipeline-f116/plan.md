# Spec and build

## Configuration
- **Artifacts Path**: {@artifacts_path} → `.zenflow/tasks/{task_id}`

---

## Agent Instructions

Ask the user questions when anything is unclear or needs their input. This includes:
- Ambiguous or incomplete requirements
- Technical decisions that affect architecture or user experience
- Trade-offs that require business context

Do not make assumptions on important decisions — get clarification first.

If you are blocked and need user clarification, mark the current step with `[!]` in plan.md before stopping.

---

## Workflow Steps

### [x] Step: Technical Specification
<!-- chat-id: c9268b1a-b895-408e-a301-e4706b6a8bab -->

Assess the task's difficulty, as underestimating it leads to poor outcomes.
- easy: Straightforward implementation, trivial bug fix or feature
- medium: Moderate complexity, some edge cases or caveats to consider
- hard: Complex logic, many caveats, architectural considerations, or high-risk changes

Create a technical specification for the task that is appropriate for the complexity level:
- Review the existing codebase architecture and identify reusable components.
- Define the implementation approach based on established patterns in the project.
- Identify all source code files that will be created or modified.
- Define any necessary data model, API, or interface changes.
- Describe verification steps using the project's test and lint commands.

Save the output to `{@artifacts_path}/spec.md` with:
- Technical context (language, dependencies)
- Implementation approach
- Source code structure changes
- Data model / API / interface changes
- Verification approach

---

### [x] Step: Log Level Filtering — interconnect.h + All Services
<!-- chat-id: 08d6c4cc-c262-4a35-8c72-33c8360ca0fc -->

Implement real-time log level control per service.

- Add `LogLevel` enum (`ERROR=0, WARN=1, INFO=2, DEBUG=3, TRACE=4`) to `interconnect.h`
- Add `set_level(LogLevel)`, `level_from_string(const char*)`, `log_level_` field to `LogForwarder`
- Update `LogForwarder::forward()` to check level before sending (early return if below threshold)
- Add `--log-level <LEVEL>` CLI argument to all 7 services (default: INFO)
- Add `SET_LOG_LEVEL:<LEVEL>` command to each service's command port handler → calls `log_fwd_.set_level()`
- Update `frontend.cpp` `handle_log_level_settings` POST: after DB write, call `send_negotiation_command(service, "SET_LOG_LEVEL:" + level)` with graceful offline handling (service may be down — not an error)
- Modify `frontend.cpp` `start_service()`: after the `VAD_SERVICE` special-case block (~line 785), read `log_level_<NAME>` from SQLite and append `--log-level <LEVEL>` to `use_args` so DB-persisted levels are applied on every (re)start even when the service was offline when the level was changed
- Files: `interconnect.h`, `inbound-audio-processor.cpp`, `vad-service.cpp`, `whisper-service.cpp`, `llama-service.cpp`, `kokoro-service.cpp`, `outbound-audio-processor.cpp`, `sip-client-main.cpp`, `frontend.cpp`
- Verify: (1) start whisper-service with `--log-level ERROR` → only ERROR logs in frontend; (2) change to DEBUG via UI Save All → DEBUG logs appear immediately without restart; (3) stop whisper-service, set level to WARN via UI, restart → verify WARN level is active from first log line

### [x] Step: Logging Robustness Audit & Hardening
<!-- chat-id: 1664e83a-3115-4f02-a9ae-7db962f32245 -->

Verify and harden the full logging chain so tests can be read reliably.

- Audit `LogForwarder::forward()` buffer sizes: `msg[2048]`, `buf[2200]` — confirm TRACE-level messages cannot overflow; truncation must be safe (no UB)
- Audit `frontend.cpp` `process_log_message()`: add early-return guard for strings that don't match `SERVICE LEVEL CALLID MESSAGE` format; verify no crash on malformed/short/empty datagrams
- Audit `frontend.cpp` `run_log_server()` recv buffer (4096 bytes): max outbound UDP is 2200 bytes — document sizes as compatible; no resize needed
- Ensure `MAX_RECENT_LOGS` ring buffer cap is enforced correctly in `recent_logs_` deque (pop_front when over limit)
- SQLite log write latency: time the INSERT in `enqueue_log` with `std::chrono`. **Decision criterion**: if write takes > 1ms avg, introduce async write queue (`std::queue<LogEntry>` + dedicated writer thread + mutex). If < 1ms, add a comment documenting the measurement and close the item — no speculative refactor.
- Files: `interconnect.h`, `frontend.cpp`
- Verify: send malformed UDP to port 22022 (`echo "garbage" | nc -u 127.0.0.1 22022`) → frontend does not crash; subsequent valid logs still appear in UI

### [x] Step: Interconnect Communication Testing & Speed Improvements
<!-- chat-id: a043a4ef-7d5e-4889-b40d-d456ab657ee0 -->

Test and improve service-pair communication and reconnect behavior.

- Test each service pair connect/disconnect/reconnect in sequence:
  - IAP ↔ VAD, VAD ↔ Whisper, Whisper ↔ LLaMA, LLaMA ↔ Kokoro, Kokoro ↔ OAP
- Verify SPEECH_ACTIVE/SPEECH_IDLE propagates correctly through pipeline
- Verify CALL_END cleans up state in each downstream service
- Verify Whisper packet buffer (MAX_BUFFER_PACKETS=64) drains after LLaMA reconnect
- Check `DOWNSTREAM_RECONNECT_MS` (currently 500ms) — reduce to 200ms if safe
- Verify TCP_NODELAY is applied on all accepted/connected sockets
- Fix any reconnect or buffering bugs found
- Files: `interconnect.h`, service `.cpp` files as needed
- Verify: kill/restart each service mid-pipeline; data flow resumes within 500ms

### [x] Step: Full Pipeline Test — WER & Latency Benchmarking
<!-- chat-id: accee2bc-b3af-404d-bf9e-fb153007ffd3 -->

Run full 1-minute pipeline test, measure WER and latency, find bottlenecks.

- Ensure all services are running (SIP, IAP, VAD, Whisper, LLaMA, Kokoro, OAP, Frontend, TestSipProvider)
- Wait 30s for startup stabilization
- Run `python3 tests/run_pipeline_test.py "ggml-large-v3-turbo-q5_0" Testfiles`
- Read results from frontend UI logging (transcription entries in `/api/logs`)
- Measure: Whisper inference ms (from log "Transcription (Xms)"), end-to-end latency
- Identify bottlenecks: which stage is slowest? Any timeouts?
- Fix VAD parameters if chunks are too long or short
- Fix Whisper parameters if WER is below 90%
- Fix LLaMA parameters if response latency is above 500ms
- **WER targets**: zero FAILs (< 90%) is the hard floor; maximise PASS count (≥ 99.5%). Samples landing in WARN (90–99.5%) are acceptable but trigger tuning. Whisper inference ≤ 300ms avg.
- Files: `vad-service.cpp`, `whisper-service.cpp`, `llama-service.cpp` as needed
- Also: `tests/run_pipeline_test.py` — add per-stage latency reporting if log data supports it

### [x] Step: Documentation
<!-- chat-id: b5f0104a-8255-41b9-bf36-236de62a0ddd -->

Add thorough inline code documentation to all services and the frontend.

- `interconnect.h`: architecture overview header comment, `InterconnectNode` API doc, `LogForwarder` usage doc, port map comment
- `inbound-audio-processor.cpp`: FIR filter design rationale, upsample pipeline flow
- `vad-service.cpp`: VAD algorithm description, all tunable parameters documented
- `whisper-service.cpp`: decoding strategy rationale, hallucination filter logic
- `llama-service.cpp`: chat template usage, shut-up mechanism, session isolation
- `kokoro-service.cpp`: CoreML split decoder, phonemization pipeline
- `outbound-audio-processor.cpp`: downsampling, G.711 encoding, jitter buffer logic
- `sip-client-main.cpp`: SIP/RTP routing, session management
- `frontend.cpp`: HTTP API endpoint index (top-level comment), log processing flow, test infrastructure
- `tests/run_pipeline_test.py`: WER computation explanation, expected flow
- Update `.zencoder/rules/*.md` summaries if any internal mechanisms changed
- After completion, write `{@artifacts_path}/report.md`
