# Investigation: Second Call Failure & NeuTTS Latency

## Bug Summary

Two distinct bugs were reported:
1. **Latency**: LLaMA response → audible speech via NeuTTS is too slow
2. **Second call failure**: After hangup, a second call shows Whisper activity but no LLaMA activity

---

## Bug 1: NeuTTS Latency

### Root Cause

`call_worker` in `neutts-service.cpp` synthesizes the **entire audio response before sending any of it** to OAP:

```cpp
{
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    samples = pipeline_.synthesize(text, &ctx->interrupted);  // blocks until all audio done
}
// Only AFTER full synthesis:
send_audio_to_downstream(ctx->call_id, samples);
```

Inside `synthesize()`, the pipeline is:
1. `phonemize()` — fast (~1ms, cached)
2. Tokenize full prompt (includes `ref_codes_str` = hundreds of fixed tokens)
3. `llama_decode()` — decode all prompt tokens including ref_codes from scratch (every call, no KV cache reuse)
4. Autoregressive sampling loop — up to 400 iterations, ~10ms per step → **1–4 seconds for a short sentence**
5. `codec_decoder_->decode(speech_codes)` — CoreML decode, fast (~50–100ms)
6. **Then** send all audio

For a typical 10-word German response (~100–200 speech codes), the user waits ~1–3 seconds before hearing anything.

### Affected Files

- `neutts-service.cpp` — `NeuTTSPipeline::synthesize()`, `NeuTTSService::call_worker()`

### Proposed Solution: Two complementary optimizations

#### A. KV cache prefix pinning for ref_codes_prompt (reduces time-to-first-code)

The prompt structure is:
```
"user: Convert the text to speech:<|TEXT_PROMPT_START|>{ref_phones} {input_phones}<|TEXT_PROMPT_END|>\nassistant:<|SPEECH_GENERATION_START|>{ref_codes_str}"
```

- `ref_phones` and `ref_codes_str` are **constant** across all synthesis calls (loaded once from `ref_text.txt` / `ref_codes.bin`)
- `input_phones` varies per call and appears **before** `ref_codes_str` in the prompt

The common prefix `"user: Convert the text to speech:<|TEXT_PROMPT_START|>{ref_phones} "` (~50–200 tokens depending on reference voice length) can be pre-decoded once at init and its KV states kept in the llama context with `llama_memory_seq_keep()`. Each synthesis would then start the `llama_decode` from that cached position, skipping re-computation of the ref_phones prefix on every call.

Note: `ref_codes_str` (~256 tokens for `<|speech_N|>` tokens) appears after `input_phones`, so its KV states depend on `input_phones` and cannot be cached. But the prefix up to and including `ref_phones` is always identical and can be pinned.

**Expected gain**: saves ~50–200ms per synthesis call (the prefix decode cost), reducing time-to-first-code from the initial `llama_decode`.

#### B. Stream audio in batches during synthesis (reduces time-to-first-audio)

NeuCodec is a VQ-codec where each code independently maps to a fixed 480-sample (20ms @ 24kHz) audio segment. **Verified**: `CoreMLNeuCodecDecoder::decode()` already implements zero-padding — it allocates a fixed `[1,1,256]` input, copies `actual_T` codes, zeroes the remainder (`std::memset(dst + actual_T, 0, ...)`), and trims output to `(actual_T - 1) * 480` samples (lines 196–223). Partial batches are fully supported without any CoreML model changes.

Refactor `synthesize()` to accept a callback `std::function<void(const std::vector<float>&)>` and invoke it after each batch of N codes (e.g., 64 codes = ~1.28s of audio at 20ms/code):

```
// During autoregressive loop, every BATCH_SIZE codes:
auto chunk = codec_decoder_->decode(batch_codes);
callback(chunk);  // sends directly to OAP
batch_codes.clear();
```

This reduces **time-to-first-audio** from ~`(total_codes × 10ms + CoreML)` to ~`(batch_size × 10ms + CoreML)`.

- For 64-code batches: first audio after ~640ms instead of ~2s
- No changes to CoreML model required (zero-padding already in place)
- `call_worker` must hold `pipeline_mutex_` for the full streaming synthesis (unchanged)

**Implementation sketch in `call_worker`:**
```cpp
auto start = std::chrono::steady_clock::now();
{
    std::lock_guard<std::mutex> lock(pipeline_mutex_);
    pipeline_.synthesize_streaming(text, &ctx->interrupted,
        [&](const std::vector<float>& chunk) {
            if (!ctx->interrupted.load())
                send_audio_to_downstream(ctx->call_id, chunk);
        });
}
```

**Implementation sketch in `NeuTTSPipeline::synthesize_streaming()`:**
```cpp
// Inside the sampling loop, after every BATCH_CODES codes or at end:
if (speech_codes_pending.size() >= BATCH_CODES || end_condition) {
    auto chunk = codec_decoder_->decode(speech_codes_pending);
    apply_normalization_and_fade(chunk);
    callback(chunk);
    speech_codes_pending.clear();
}
```

---

## Bug 2: Second Call — No LLaMA Activity

### Root Cause

**`speech_active_calls_` in `InterconnectNode` is never cleared on `CALL_END`.**

This causes `LlamaService::worker_loop` to block indefinitely waiting for a `SPEECH_IDLE` signal that will never arrive.

### Exact Failure Sequence

1. **Call 1 active**: User speaks → VAD sends `SPEECH_ACTIVE` → propagates downstream → LLaMA's `interconnect_.speech_active_calls_` gets `call1_id` inserted
2. **Worker processes call1**: LLaMA `worker_loop` dequeues call1's text item, checks `is_speech_active(call1_id)` = **true**, enters:
   ```cpp
   while (interconnect_.is_speech_active(item.call_id) && running_) {
       std::this_thread::sleep_for(std::chrono::milliseconds(50));  // waits here
   }
   ```
3. **Caller hangs up mid-speech**: SIP client receives BYE → calls `broadcast_call_end(call1_id)` → `CALL_END` propagates through VAD → Whisper → LLaMA
4. **VAD erases the VadCall** for call1 (`handle_call_end` just calls `calls_.erase(it)`, no `SPEECH_IDLE` broadcast). Note: VAD has a `speech_signal_timeout_s_` mechanism (default 10s) that force-sends `SPEECH_IDLE` after a timeout — but this only fires while the VadCall is still alive inside `processing_loop`. Once the call is erased on CALL_END, the timeout can never trigger. The `SPEECH_IDLE` is permanently lost.
5. **`CALL_END` reaches LLaMA's interconnect** (`handle_remote_call_end`): erases call1 from `active_call_ids_`, calls `call_end_handler_`, forwards downstream — **but does NOT clear `speech_active_calls_`**
6. **LLaMA `worker_loop` is stuck**: `is_speech_active(call1_id)` still returns `true`, `running_` is still `true` → infinite 50ms sleep loop
7. **Call 2 starts**: New call (call2_id ≠ call1_id). Whisper transcribes and sends text to LLaMA's `work_queue_`
8. **No processing**: Worker is stuck on call1 → call2 items pile up in `work_queue_` unprocessed → **zero LLaMA log activity for call2**

### The Missing Cleanup

In `interconnect.h`, both `broadcast_call_end()` and `handle_remote_call_end()` update `active_call_ids_` and `ended_call_ids_` but miss `speech_active_calls_`:

```cpp
// interconnect.h, handle_remote_call_end (line 1025):
void handle_remote_call_end(uint32_t call_id) {
    // ... updates active_call_ids_, ended_call_ids_ ...
    // MISSING: speech_active_calls_.erase(call_id)
}

// interconnect.h, broadcast_call_end (line 654):
void broadcast_call_end(uint32_t call_id) {
    // ... same issue ...
}
```

### Affected Files

- `interconnect.h` — `broadcast_call_end()`, `handle_remote_call_end()`
- `llama-service.cpp` — `worker_loop()` (secondary hardening)

### Proposed Fix

**Primary fix in `interconnect.h`**: Clear `speech_active_calls_` for the ended call in both places:

```cpp
// In broadcast_call_end, inside the call_id_mutex_ lock:
{
    std::lock_guard<std::mutex> lock(call_id_mutex_);
    // ... existing code ...
    ended_call_ids_.insert(call_id);
    active_call_ids_.erase(call_id);
}
// NEW: clear speech state outside call_id_mutex_ (uses speech_mutex_)
{
    std::lock_guard<std::mutex> lock(speech_mutex_);
    speech_active_calls_.erase(call_id);
}
```

Apply the same pattern in `handle_remote_call_end()`.

**Secondary hardening in `llama-service.cpp`**: The `worker_loop` speech-wait should also respect call end status. A possible approach is to skip processing if the call was already ended:

```cpp
// In worker_loop, after dequeuing item:
{
    std::lock_guard<std::mutex> cl(calls_mutex_);
    // Check if call was ended before we processed it
    if (interconnect_.ended_call_ids_has(item.call_id)) {
        continue;  // discard stale work item
    }
}
```

However, `ended_call_ids_` is private in `InterconnectNode`. To enable this check cleanly, add a public accessor to `InterconnectNode`:

```cpp
// In interconnect.h, InterconnectNode public section:
bool has_ended(uint32_t call_id) const {
    std::lock_guard<std::mutex> lock(call_id_mutex_);
    return ended_call_ids_.count(call_id) > 0;
}
```

This allows `worker_loop` to discard stale items for already-ended calls without exposing the full set. The primary fix in `interconnect.h` is still necessary; this is defense-in-depth.

### Secondary Issue: LLaMA `handle_call_end` blocks `mgmt_recv_loop`

`LlamaService::handle_call_end` is invoked from `mgmt_recv_loop` thread and acquires `llama_mutex_` (also held during generation):

```cpp
void handle_call_end(uint32_t call_id) {
    std::lock_guard<std::mutex> llama_lock(llama_mutex_);  // blocks if generating
    std::lock_guard<std::mutex> calls_lock(calls_mutex_);
    // ... llama_memory_seq_rm ...
}
```

If LLaMA is mid-generation when CALL_END arrives, `mgmt_recv_loop` blocks for the remainder of generation (~100–500ms). During this window:
- No other mgmt messages from Whisper are processed
- CALL_END forwarding to NeuTTS is delayed

**Proposed improvement**: Set `generating = false` atomically first (doesn't need llama_mutex_), then schedule KV cache cleanup asynchronously or defer to next `process_call` entry:

```cpp
void handle_call_end(uint32_t call_id) {
    // First: interrupt generation immediately (no lock needed, atomic)
    std::shared_ptr<LlamaCall> call_to_clean;
    {
        std::lock_guard<std::mutex> calls_lock(calls_mutex_);
        auto it = calls_.find(call_id);
        if (it != calls_.end()) {
            it->second->generating = false;  // interrupt generation
            call_to_clean = it->second;
            calls_.erase(it);
        }
    }
    // Then: clean KV cache (may need to wait for llama_mutex_ but we've already
    // interrupted generation so the wait should be minimal)
    if (call_to_clean) {
        std::lock_guard<std::mutex> llama_lock(llama_mutex_);
        llama_memory_t mem = llama_get_memory(ctx_);
        llama_memory_seq_rm(mem, call_to_clean->seq_id, -1, -1);
        log_fwd_.forward(whispertalk::LogLevel::INFO, call_id, "Call ended, clearing conversation context");
    }
}
```

This reduces the window where `mgmt_recv_loop` is blocked because `generating = false` will cause the generation loop to exit quickly, so the `llama_mutex_` wait is short.

---

## Summary Table

| Issue | Root Cause | Affected File | Fix |
|---|---|---|---|
| NeuTTS latency (time-to-first-code) | ref_phones prefix re-decoded from scratch every call | `neutts-service.cpp` | KV cache prefix pinning for constant `ref_phones` prefix |
| NeuTTS latency (time-to-first-audio) | Full synthesis before first audio send | `neutts-service.cpp` | Streaming synthesis via batch callback (zero-padding already supported) |
| Second call silent | `speech_active_calls_` not cleared on CALL_END | `interconnect.h` | Clear `speech_active_calls_` in `broadcast_call_end` + `handle_remote_call_end` |
| Second call silent (defense-in-depth) | `worker_loop` has no call-ended check in speech-wait loop | `interconnect.h` + `llama-service.cpp` | Add `has_ended()` accessor; discard stale work items |
| mgmt_recv_loop blocking | `handle_call_end` holds `llama_mutex_` on mgmt thread | `llama-service.cpp` | Set `generating=false` before acquiring `llama_mutex_` |

---

## Implementation Notes

### Changes Made

#### `interconnect.h`
- **`broadcast_call_end`**: Added block after `call_id_mutex_` section to erase `call_id` from `speech_active_calls_` under `speech_mutex_`.
- **`handle_remote_call_end`**: Same cleanup added.
- **`has_ended()`**: Added new public accessor that checks `ended_call_ids_` under `call_id_mutex_`.

#### `llama-service.cpp`
- **`worker_loop`**: Added `has_ended()` check before entering speech-active wait loop (discards stale items for already-ended calls). Added `has_ended()` check inside the sleep loop to break early. Added post-wait `has_ended()` check to discard items if call ended during wait.
- **`handle_call_end`**: Refactored to set `generating=false` and erase from `calls_` under `calls_mutex_` before acquiring `llama_mutex_`. This unblocks generation immediately and reduces `mgmt_recv_loop` blocking window.

#### `neutts-service.cpp`
- Added `#include <functional>` header.
- **`NeuTTSPipeline::synthesize_streaming()`**: New method that accepts a `std::function<void(const std::vector<float>&)>` callback. Runs the same autoregressive loop but flushes decoded audio in batches of 64 codes (~1.28s) via the callback. Applies per-chunk normalization and fade-in on first chunk only.
- **`call_worker`**: Replaced `synthesize()` + post-send block with `synthesize_streaming()` call that sends each chunk directly to OAP as it becomes available. First audio now arrives ~640ms into generation instead of after full synthesis.

#### `tests/test_interconnect.cpp`
- Added `SpeechActiveTest.SpeechActiveClearedOnBroadcastCallEnd`: verifies `is_speech_active` returns false after `broadcast_call_end`.
- Added `SpeechActiveTest.SpeechActiveClearedOnHandleRemoteCallEnd`: verifies both upstream and downstream nodes clear speech state after CALL_END propagates.
- Added `SpeechActiveTest.HasEndedReturnsFalseForActiveCall`: verifies `has_ended()` lifecycle correctness.
