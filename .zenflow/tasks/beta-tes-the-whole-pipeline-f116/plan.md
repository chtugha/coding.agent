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

### [ ] Step: Log Level Filtering — interconnect.h + All Services

Implement real-time log level control per service.

- Add `LogLevel` enum (`ERROR=0, WARN=1, INFO=2, DEBUG=3, TRACE=4`) to `interconnect.h`
- Add `set_level(LogLevel)`, `level_from_string(const char*)`, `log_level_` field to `LogForwarder`
- Update `LogForwarder::forward()` to check level before sending (early return if below threshold)
- Add `--log-level <LEVEL>` CLI argument to all 7 services (default: INFO)
- Add `SET_LOG_LEVEL:<LEVEL>` command to each service's command port handler → calls `log_fwd_.set_level()`
- Update `frontend.cpp` `handle_log_level_settings` POST: after DB write, call `send_negotiation_command(service, "SET_LOG_LEVEL:" + level)` with graceful offline handling
- Files: `interconnect.h`, `inbound-audio-processor.cpp`, `vad-service.cpp`, `whisper-service.cpp`, `llama-service.cpp`, `kokoro-service.cpp`, `outbound-audio-processor.cpp`, `sip-client-main.cpp`, `frontend.cpp`
- Verify: build succeeds; start whisper-service with `--log-level ERROR`, change to DEBUG via UI, verify DEBUG logs appear without restart

### [ ] Step: Logging Robustness Audit & Hardening

Verify and harden the full logging chain so tests can be read reliably.

- Audit `LogForwarder::forward()` buffer sizes vs max log message size
- Audit `frontend.cpp` `process_log_message()` for malformed/oversized datagrams
- Audit `frontend.cpp` `run_log_server()` recv buffer (4096 bytes vs max possible message)
- Ensure `MAX_RECENT_LOGS` ring buffer is properly enforced in `recent_logs_` deque
- Verify SQLite log writes don't block log reception thread (async if needed)
- Add defensive bounds checking where missing
- Files: `interconnect.h`, `frontend.cpp`
- Verify: send oversized/malformed UDP to port 22022 → frontend does not crash; logs continue flowing

### [ ] Step: Interconnect Communication Testing & Speed Improvements

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

### [ ] Step: Full Pipeline Test — WER & Latency Benchmarking

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
- Target: ≥90% similarity (WARN+PASS), Whisper inference ≤300ms avg
- Files: `vad-service.cpp`, `whisper-service.cpp`, `llama-service.cpp` as needed
- Also: `tests/run_pipeline_test.py` — add per-stage latency reporting if log data supports it

### [ ] Step: Documentation

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
