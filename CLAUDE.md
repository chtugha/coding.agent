# Prodigy — Mandatory Architecture Reference

READ THIS BEFORE MAKING ANY CHANGES TO THE PIPELINE.

## Pipeline Architecture: Per-Call Isolated Instances

Each SIP caller gets their own INDEPENDENT pipeline instance. The pipeline is NOT shared between callers.

### Example: 2-caller conference (Alice and Bob)

```
Alice calls in -> gets her own pipeline instance:
  Alice's RTP -> IAP(call=1) -> VAD(call=1) -> Whisper(call=1) -> LLaMA(call=1) -> TTS(call=1) -> OAP(call=1) -> RTP to BOB

Bob calls in -> gets his own pipeline instance:
  Bob's RTP -> IAP(call=2) -> VAD(call=2) -> Whisper(call=2) -> LLaMA(call=2) -> TTS(call=2) -> OAP(call=2) -> RTP to ALICE
```

The TTS stage is a generic pipeline node (`ServiceType::TTS_SERVICE`). A
concrete TTS engine (Kokoro or NeuTTS) docks into the TTS stage over
127.0.0.1:13143; at most one engine is active ("last-connect-wins").
Per-call state (VAD session, Whisper context, LLaMA KV, engine synth
thread, OAP buffer) is still isolated by call_id.

### Critical rules:

1. Each call_id is a SEPARATE conversation with its own state (VAD session, Whisper context, LLaMA chat history, engine synthesis thread, OAP buffer).
2. The TTS output of Alice's pipeline goes to BOB (the other party), NOT back to Alice.
3. The TTS output of Bob's pipeline goes to ALICE (the other party), NOT back to Bob.
4. In a conference bridge, audio from one leg is relayed to ALL OTHER legs. This is normal telephony behavior — it is NOT echo.
5. When Bob receives Alice's TTS audio via the conference bridge, Bob's pipeline processes it as legitimate input. This is the intended behavior.

### Loopback Test Methodology

The loopback test creates TWO AI pipeline instances in a conference call. Audio is injected into one leg to seed the conversation. Both AIs then talk to each other indefinitely — each treating the other's TTS output as caller input. This is INTENTIONAL and the PRIMARY test method.

Purpose:
- Generates sustained bidirectional audio flow through both full pipelines
- Tests all components under realistic load (VAD, Whisper, LLaMA, Kokoro, OAP)
- Allows analyzing per-pipeline behavior by filtering logs on call_id
- The conversation topic doesn't matter — we need continuous audio to test and optimize

Do NOT:
- Add "echo detection" or "cross-call deduplication" — both AIs responding to each other IS the test
- Treat cross-call audio as a bug — the conference bridge relay is working as designed
- Try to prevent the "feedback loop" — it is the desired test behavior

### What this means for debugging:

- If call=2 transcribes text that matches call=1's LLaMA response, that is CORRECT BEHAVIOR — the conference bridge relayed call=1's TTS output to call=2's ear.
- Each service multiplexes calls by call_id. The services themselves are shared processes, but the per-call state is isolated.
- To analyze one pipeline in isolation, filter logs by call_id.

### Audio routing (SIP Client <-> test_sip_provider):

- SIP Client manages multiple lines (alice=line 0, bob=line 1). Each line has its own RTP port.
- test_sip_provider's conference bridge relays RTP: audio received from leg A is forwarded to all OTHER legs (B, C, ...).
- OAP sends TTS audio tagged with call_id. The SIP Client maps call_id to the correct RTP port/line.

## Build

```bash
cd build && ninja -j$(sysctl -n hw.ncpu)
```

## Test

```bash
# Start frontend first, then use API to start services, add SIP lines, create conference, inject audio
```

## Lint / Typecheck

This is a C++17 project. Build with ninja to catch compilation errors. No separate lint command.

## DEPRECATED: C++ moshi-service (bin/moshi-service)

The old C++ `moshi-service` (`moshi-service.cpp`, binary `bin/moshi-service`) is **FULLY DEPRECATED**.
It has been replaced by the pure Rust standalone service `bin/moshi-rag-service` (built from `moshi-rag/rust/`).

- **NEVER** start, reference, or configure `bin/moshi-service` — it will refuse to run (exits immediately with a deprecation notice).
- The `MOSHI_SERVICE` service type in `interconnect.h` now exclusively maps to `bin/moshi-rag-service`.
- The database migration in `database.h` automatically redirects any existing `bin/moshi-service` → `bin/moshi-rag-service`.
- The CMakeLists.txt does NOT build the old C++ binary. Only the Rust service is built.
- `moshi-service.cpp` is kept for historical reference only.
- The two pipelines (classic VAD→Whisper→LLaMA→TTS and Moshi) **cannot run simultaneously** — stop one before starting the other.

## German STT Fine-Tuning (stt-1b-de)

The `kyutai/stt-1b-en_fr` base model has been successfully fine-tuned on ~600 hours of German speech (using Modal H200 GPU over 3000 steps).

- **Vocabulary Extension**: The SentencePiece tokenizer has been extended from 8000 to **8004** tokens to support German umlauts (`ä`, `ö`, `ü`, `ß`). The extended tokenizer model is located at `/Users/whisper/tokenizer/tokenizer_model/tokenizer_de.model`.
- **Model Checkpoints**: Downloaded checkpoints are located at `/Users/whisper/tokenizer/checkpoints/stt_1b_de/`. The step 3000 consolidated checkpoint contains `lora.safetensors` (~385 MB).
- **Merged Model Weights**: The merged models are exported to `/Users/whisper/tokenizer/exported/stt-1b-de/model.safetensors` (bfloat16, ~2 GB) and the quantized version `/Users/whisper/tokenizer/exported/stt-1b-de-q8/model.safetensors` (int8, ~1 GB).
- **Configuration**: To configure `./moshi-rag/rust/moshi-backend/config.json` or `./moshi-rag/rust/moshi-rag-backend/config.json` to load the German STT model:
  - Update `stt_lm_model_file` to point to `/Users/whisper/tokenizer/exported/stt-1b-de/model.safetensors` (or the quantized `-q8` variant).
  - Update `stt_text_tokenizer_file` to point to `/Users/whisper/tokenizer/tokenizer_model/tokenizer_de.model`.

## Agent Rules

- NEVER wait/sleep/poll for more than 15 minutes in a single action. If a process takes longer (model loading, builds, etc.), use short polls (30-60s) with a timeout counter and bail out with a status report if the limit is reached. This prevents the agent from going idle/unresponsive.
- For moshi-backend: model warmup takes ~4 seconds with Q8 GGUF on Metal. If it hasn't finished in 2 minutes, something is wrong.
- The moshi model (moshiko) is 7.7B params. On this 16GB M4 Mac, use the Q8 GGUF model (~8GB) with Metal. BF16 safetensors (15GB) causes swap thrashing. Steady-state LM step latency: ~101ms (1.26x real-time at 12.5Hz).
- The `matmul_dtype` function in `moshi-core/src/nn.rs` MUST return BF16 for Metal (not just CUDA). This is critical for Q8 performance (30x speedup).

## Browser Automation — MANDATORY RULE

- ALWAYS use local Playwright (Python: `from playwright.sync_api import sync_playwright`) for ANY browser interaction, UI testing, or web automation.
- NEVER use the embedded Zencoder browser tools (browser_click, browser_navigate, browser_screenshot, etc.).
- This applies to ALL agents including subagents. When delegating browser tasks to subagents, explicitly instruct them to use Playwright.
- Playwright is installed at: `python3 -c "from playwright.sync_api import sync_playwright"` (Python 3.9, user site-packages).
