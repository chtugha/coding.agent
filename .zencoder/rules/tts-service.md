---
description: TTS Stage (dock) Summary
alwaysApply: true
---

# TTS Stage (dock)

## Overview
The **TTS Stage** (`tts-service.cpp`) is the generic pipeline node that sits between LLaMA and OAP (`ServiceType::TTS_SERVICE`, base port 13140). It owns the LLaMA↔OAP interconnect slot and a dedicated **engine dock** on `127.0.0.1:13143` into which concrete TTS engines (Kokoro, NeuTTS, or future ones) hot-plug. Engines are no longer pipeline nodes; the dock arbitrates a single "active engine" slot and forwards text in / audio out.

## Pipeline role
- **Upstream**: `LLAMA_SERVICE` (mgmt 13140, data 13141).
- **Downstream**: `OAP_SERVICE` (mgmt 13150, data 13151).
- **Engine dock**: `127.0.0.1:13143` (loopback-only). Accepts one or more engine TCP connections; HELLO-validates each; keeps at most one active.
- **Cmd port**: 13142 (`PING`, `STATUS`, `SET_LOG_LEVEL:<LEVEL>`).

## Engine-slot state machine ("last connect wins")
```
                 HELLO ok             TCP close / SHUTDOWN ack
  [NO ENGINE] ─────────────▶ [ACTIVE=X] ─────────────────────▶ [NO ENGINE]
                                  │
                                  │  new engine Y HELLO ok
                                  ▼
                            [SWAPPING X→Y]
                                  │  SHUTDOWN to X, X closes (or 2 s timeout)
                                  ▼
                              [ACTIVE=Y]
```
- First engine to HELLO successfully becomes active.
- A later successful HELLO from a different engine triggers a swap: dock sends `CUSTOM SHUTDOWN` (mgmt tag `0x02`, payload `"SHUTDOWN"`) to the outgoing engine and `CUSTOM FLUSH_TTS` to OAP, flips the slot, and bounds the outgoing TCP close at 2 s before force-closing.
- A malformed HELLO replies `ERR <reason>\n` and closes the newcomer socket without disturbing the currently active engine.
- No engine docked → LLaMA text frames are dropped with a rate-limited WARN; SPEECH_ACTIVE / SPEECH_IDLE / CALL_END are still auto-forwarded to OAP by the inherited `InterconnectNode` mgmt path.

## Engine-dock protocol
- Transport: plain TCP on `127.0.0.1:13143`.
- Handshake: engine sends a single-line JSON HELLO `{"name":"<engine>", "sample_rate":24000, "channels":1, "format":"f32le"}` terminated by `\n`. Dock replies `OK\n` or `ERR <reason>\n`.
- HELLO constraints: `sample_rate == 24000`, `channels == 1`, `format == "f32le"`; `name` matches `[A-Za-z0-9_-]{1..32}`; line bounded at 1 KiB.
- After OK: tag-prefixed frames — `0x01` = serialized `Packet`, `0x02` = `MgmtMsgType` + optional CUSTOM payload.
- Keepalive: 200 ms PING/PONG loop on the engine socket; ≥ 3 missed PONGs clear the slot (same path as engine disconnect).

## Signals handled
- **Upstream (LLaMA) → active engine**: text `0x01` packets; CALL_END / SPEECH_ACTIVE / SPEECH_IDLE teed as `0x02` frames so engines can interrupt, warm up, or clean up per-call state.
- **Active engine → OAP**: PCM `Packet`s forwarded byte-for-byte (no re-encode, single-copy, `TCP_NODELAY`).
- **Dock → OAP (new)**: `CUSTOM FLUSH_TTS` on swap-start and on engine disconnect so OAP drops buffered PCM without running the SPEECH_ACTIVE sidetone guard.
- **Dock → outgoing engine (new)**: `CUSTOM SHUTDOWN` on swap-start.

## Latency budget
Engine-output → OAP-input: **≤ 2 ms median, ≤ 5 ms p99** on loopback. Invariants:
- Zero audio re-encoding; forwarding is single-copy.
- Bounded SPSC queues (capacity 8 audio frames); full queue drops oldest, never blocks.
- `active_fd_` is `std::atomic<int>`; slot-mutex held only during the swap transition, never across a network send.
- `TCP_NODELAY` on all four endpoints; no Nagle; `SO_SNDBUF`/`SO_RCVBUF` sized for ≥ 2 audio frames.
- Slot swap (HELLO ok → new slot active) completes in ≤ 1 ms on a quiescent system; old-engine drain runs off the hot path.

## Runtime Commands (cmd port 13142)
- `PING` → `PONG`
- `STATUS` → `ACTIVE <name>` or `NONE`
- `SET_LOG_LEVEL:<LEVEL>` → `OK`

## Frontend integration
- `GET /api/tts/status` → `{"engine":"kokoro"}`, `{"engine":"neutts"}`, or `{"engine":null}`. Polled by the dashboard to label the TTS pipeline node with the currently docked engine. There is no selection endpoint — operators start/stop the desired engine process and the dock handles the swap.
