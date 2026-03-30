# Prodigy — Mandatory Architecture Reference

READ THIS BEFORE MAKING ANY CHANGES TO THE PIPELINE.

## Pipeline Architecture: Per-Call Isolated Instances

Each SIP caller gets their own INDEPENDENT pipeline instance. The pipeline is NOT shared between callers.

### Example: 2-caller conference (Alice and Bob)

```
Alice calls in -> gets her own pipeline instance:
  Alice's RTP -> IAP(call=1) -> VAD(call=1) -> Whisper(call=1) -> LLaMA(call=1) -> Kokoro(call=1) -> OAP(call=1) -> RTP to BOB

Bob calls in -> gets his own pipeline instance:
  Bob's RTP -> IAP(call=2) -> VAD(call=2) -> Whisper(call=2) -> LLaMA(call=2) -> Kokoro(call=2) -> OAP(call=2) -> RTP to ALICE
```

### Critical rules:

1. Each call_id is a SEPARATE conversation with its own state (VAD session, Whisper context, LLaMA chat history, Kokoro synthesis thread, OAP buffer).
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
