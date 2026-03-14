# Investigation: Real Call Test 1 — Pipeline Debug

## Bug Summary

Two issues were observed during real call testing:

1. **Primary (confirmed)**: LLaMA service emits `Prompt decode failed` on the **second and all subsequent calls** — no AI response is generated.
2. **Secondary (likely)**: NeuTTS synthesis silently produced no audio on the first call — the user heard nothing.

---

## Logs Retrieved

Frontend log server: `http://localhost:8080/api/logs`

### Call 1 (call_id=1) — with neutts-service (03:36:17–03:36:44)

```
03:36:17 SIP_CLIENT         call=1 - Accepted call on line 0 port 10001
03:36:17 INBOUND_AUDIO_PROCESSOR call=1 - Created call state
03:36:17 VAD_SERVICE        call=1 - Created VAD session
03:36:20 VAD_SERVICE        call=1 - VAD chunk -> Whisper: 20800 samples (1300ms)
03:36:21 WHISPER_SERVICE    call=1 - Transcription (755ms): "Hallo."
03:36:21 LLAMA_SERVICE      call=1 - Created conversation context
03:36:21 LLAMA_SERVICE      call=1 - Response (143ms): Guten Tag.
03:36:21 KOKORO_SERVICE     call=1 - Started NeuTTS synthesis thread    ← NeuTTS starts
                                       [NO further KOKORO_SERVICE logs until call-end]
03:36:34 VAD_SERVICE        call=1 - VAD chunk -> Whisper: 32000 samples (2000ms)
03:36:34 WHISPER_SERVICE    call=1 - Transcription (570ms): "Hallo, hören Sie mich?"
03:36:34 LLAMA_SERVICE      call=1 - Response (161ms): Ja, ich höre dich.
03:36:44 [all services]     call=1 - Call ended / cleaning up
```

NeuTTS never logged "Synthesized X samples" — synthesis silently produced no audio.

### Call 2 (call_id=2) — with kokoro-service (03:37:17–03:37:30)

```
03:37:17 SIP_CLIENT         call=2 - Accepted call on line 0 port 10002
03:37:17 INBOUND_AUDIO_PROCESSOR call=2 - Created call state
03:37:17 VAD_SERVICE        call=2 - Created VAD session
03:37:20 VAD_SERVICE        call=2 - VAD chunk -> Whisper: 21600 samples (1350ms)
03:37:20 WHISPER_SERVICE    call=2 - Transcription (501ms): "Guten Tag."
03:37:20 LLAMA_SERVICE      call=2 - Created conversation context
03:37:20 LLAMA_SERVICE      call=2 - ERROR: Prompt decode failed          ← BUG
03:37:25 VAD_SERVICE        call=2 - VAD chunk -> Whisper: 20800 samples (1300ms)
03:37:25 WHISPER_SERVICE    call=2 - Transcription (467ms): "Hallo?"
03:37:25 LLAMA_SERVICE      call=2 - ERROR: Prompt decode failed          ← BUG again
03:37:30 [all services]     call=2 - Call ended
```

Bug was reproduced via `TEST_PROMPT` cmd: `echo "TEST_PROMPT:Hallo" | nc 127.0.0.1 13132` → also returned `Prompt decode failed` immediately.

---

## Root Cause Analysis

### Bug 1 — LLaMA "Prompt decode failed" (CONFIRMED)

**File**: `llama-service.cpp`, context init (~line 105–109)

The llama context is initialized with default params:

```cpp
llama_context_params cparams = llama_context_default_params();
cparams.n_ctx = 2048;
cparams.n_threads = 4;
cparams.n_threads_batch = 4;
// n_seq_max and kv_unified NOT set → use defaults
```

The default values (from `llama-cpp/src/llama-context.cpp` line 2879, 2905):
- `n_seq_max = 1`
- `kv_unified = false`

With `kv_unified=false` and `n_seq_max=1`, the **batch allocator** in `llama-cpp/src/llama-batch.cpp` validates that every token's `seq_id < n_seq_max`:

```cpp
// llama-batch.cpp lines 61–64
if (batch.seq_id[i][s] < 0 || batch.seq_id[i][s] >= (llama_seq_id) n_seq_max) {
    LLAMA_LOG_ERROR("%s: invalid seq_id[%d][%d] = %d >= %d\n", ...);
    return false;  // ← llama_decode fails
}
```

In `llama-service.cpp`, the service assigns monotonically increasing sequence IDs:

```cpp
// llama-service.cpp line 421
call->seq_id = next_seq_id_.fetch_add(1);
```

- Call 1: `seq_id = 0` → valid (0 < 1 = n_seq_max) ✓
- Call 2: `seq_id = 1` → **INVALID** (1 >= 1 = n_seq_max) ✗ → `llama_decode` returns non-zero → "Prompt decode failed"
- All subsequent calls and TEST_PROMPT commands also fail

**Fix**: Set `cparams.n_seq_max` to `LLAMA_MAX_SEQ` (= 256, defined in `llama-cpp/src/llama-cparams.h`) in `llama-service.cpp`. Also wrap `next_seq_id_` modulo 256 to prevent long-term overflow.

### Bug 2 — NeuTTS no audio output (LIKELY CAUSE: silent synthesis failure)

**File**: `neutts-service.cpp`

Evidence:
- "Started NeuTTS synthesis thread" logged at 03:36:21
- No "Synthesized X samples" log ever appeared (this IS forwarded via `log_fwd_.forward`)
- "Call ended, cleaning up synthesis thread" appeared at 03:36:44 (23 seconds later)

The synthesis failure path in `call_worker` (neutts-service.cpp ~line 834) only logs to **stderr** (not `log_fwd_`), which is why nothing appeared in the frontend:

```cpp
if (samples.empty() || samples.size() > MAX_AUDIO_SAMPLES) {
    std::fprintf(stderr, "Invalid audio output...");  // NOT forwarded to frontend
    continue;
}
```

Most likely cause: `synthesize()` returned an empty vector. This happens when:
- `speech_codes.empty()` (generation loop produced no valid `<|speech_N|>` tokens), OR
- `llama_decode` for the prompt batch failed (returns non-zero at line 371)

The NeuTTS backbone uses seq_id=0 always (no n_seq_max issue). The context size is 2048 with a prompt of ~300 tokens and potentially 1500 generated tokens (fits within 2048 for short phrases). The most likely cause is **the model generates no valid speech codes** — either EOS is sampled immediately or the `<|SPEECH_GENERATION_END|>` token is generated as token 1.

This might indicate a **model format or prompt format issue** with the neutts-nano-german model, or that the model is simply too slow (1500 tokens × generation time may exceed real-time requirements).

Note: The user switched to kokoro-service which is working correctly. The NeuTTS issue is secondary.

---

## Affected Components

| Component | Bug | Severity |
|-----------|-----|----------|
| `llama-service.cpp` | `n_seq_max=1` → seq_id=1+ invalid | **Critical** — breaks all calls after first |
| `neutts-service.cpp` | Silent synthesis failure (no audio output) | High — but user has switched to kokoro |

---

## Proposed Solution

### Fix 1 — llama-service.cpp (Primary, must fix)

In the context initialization, add:

```cpp
cparams.n_seq_max = 256;  // LLAMA_MAX_SEQ — support up to 256 distinct call sequences
```

And wrap the seq_id allocation to prevent overflow:

```cpp
call->seq_id = next_seq_id_.fetch_add(1) % 256;
```

This ensures all seq_ids (0–255) are within the valid range, and after 256 calls the IDs wrap around (safe because `llama_memory_seq_rm` always clears the sequence before reuse).

### Fix 2 — neutts-service.cpp (Secondary)

Add a `log_fwd_.forward(ERROR, ...)` call in the synthesis failure path so failures are visible in the frontend log, making future debugging easier:

```cpp
if (samples.empty() || samples.size() > MAX_AUDIO_SAMPLES) {
    log_fwd_.forward(LogLevel::ERROR, ctx->call_id,
        "NeuTTS synthesis produced no audio (empty samples)");
    std::fprintf(stderr, "Invalid audio output for call %u: %zu samples\n", ...);
    continue;
}
```

Also add a log when `llama_decode` fails in `synthesize()`:

```cpp
if (llama_decode(ctx_, batch) != 0) {
    llama_batch_free(batch);
    // log_fwd_ not available in NeuTTSPipeline — use stderr + consider moving logging up
    std::fprintf(stderr, "NeuTTS: prompt decode failed\n");
    return {};
}
```

The deeper NeuTTS model quality issue (why no speech codes are generated) may require additional investigation with direct testing of the synthesis pipeline via the `SYNTH_WAV` command.

---

## Implementation Notes

- The `llama-service.cpp` fix is a 2-line change in the context initialization block
- Both fixes are backwards-compatible — no protocol or API changes needed
- After fixing `llama-service.cpp`, the service must be rebuilt and restarted
- Kokoro-service is confirmed working and is the recommended TTS for now
