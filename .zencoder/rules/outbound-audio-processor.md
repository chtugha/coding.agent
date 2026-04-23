---
description: Outbound Audio Processor Summary
alwaysApply: true
---

# Outbound Audio Processor

## Overview
The **Outbound Audio Processor** (`outbound-audio-processor.cpp`) handles the audio pipeline from the TTS stage back to the SIP Client.

## Internal Function
- **Audio Acquisition**: Receives 24kHz float32 PCM audio from the generic TTS stage (`tts-service`, `ServiceType::TTS_SERVICE`) via TCP interconnect. The TTS stage sources the PCM from whichever engine (Kokoro or NeuTTS) is currently docked on its engine-dock port (13143); OAP is agnostic to the concrete engine.
- **FLUSH_TTS mgmt handler**: the TTS dock sends `CUSTOM FLUSH_TTS` on every engine-swap and engine-disconnect so OAP drops residual PCM without triggering the SPEECH_ACTIVE sidetone guard.
- **SPEECH_IDLE warm-up**: on `SPEECH_IDLE` OAP pre-warms per-call state (`CallState`, zeroed FIR/DC/biquad histories, `first_chunk=false`) so the first PCM frame of a turn skips the lazy-init branch.
- **Downsampling**: 24kHz → 8kHz (ratio 3:1) via 63-tap Hamming-windowed sinc anti-aliasing FIR (cutoff 3400Hz, ~43 dB stopband attenuation at 4kHz) + decimate by 3. The FIR uses the correct sinc formula: center tap = `AA_CUTOFF`, off-center taps = `sin(π·fc·k)/(π·k)`, then Hamming-windowed and DC-normalized.
- **Presence boost** (default OFF, runtime-toggleable): Optional first-order biquad high-shelf (+3 dB at 2500 Hz, S=1) applied per-call before the AA filter. Boosts consonant clarity and voice presence that telephone bandwidth rolloff removes. Toggle via `PRESENCE_BOOST:ON/OFF` on cmd port 13152.
- **G.711 encoding**: Each float32 sample clipped to [-1, 1] and encoded to μ-law using the standard ITU-T G.711 segment/quantization formula.
- **Scheduling**: Uses a high-precision 20ms timer (steady_clock) to send constant-rate 160-byte G.711 frames back to the SIP Client, filling gaps with ULAW_SILENCE (0xFF) to maintain RTP clock continuity.
- **Multi-Call**: Manages independent CallState (buffer, FIR history, read_pos) per active call_id. buffer.compact() reclaims memory lazily.
- **SPEECH_ACTIVE handling**: When a SPEECH_ACTIVE signal arrives, flushes the call's audio buffer — but only if `speech_active_guard_ms_` (default 1500ms, runtime-configurable via `SET_SIDETONE_GUARD_MS`) has elapsed since the last TTS audio was received. This prevents PBX sidetone echo (the outgoing TTS RTP reflected back by the PBX) from spuriously flushing the buffer within ~200-500ms of playback starting. Genuine caller interruptions arrive after the guard window expires.

## Inbound Connections
- **TTS stage (TCP)**: Receives float32 PCM audio via interconnect from `tts-service` on ports 13150 (mgmt) and 13151 (data). Upstream is resolved automatically via `upstream_of(OAP_SERVICE) == TTS_SERVICE`.

## Outbound Connections
- **SIP Client (TCP)**: Sends 160-byte G.711 frames to SIP Client via interconnect on ports 13100 (mgmt) and 13101 (data).

## Command-Line Parameters
- `--log-level <LEVEL>`: Initial log verbosity (ERROR/WARN/INFO/DEBUG/TRACE, default: INFO)

## Runtime Commands (cmd port 13152)
- `PING`: Health check (returns `PONG`)
- `STATUS`: Returns active call count, upstream/downstream state
- `SET_LOG_LEVEL:<LEVEL>`: Change log verbosity at runtime without restart
- `SET_SIDETONE_GUARD_MS:<ms>`: Change the sidetone guard holdoff window (0 to disable, default 1500)
- `PRESENCE_BOOST:ON` / `PRESENCE_BOOST:OFF`: Enable/disable presence boost (default OFF)
- `PRESENCE_BOOST:STATUS`: Query current presence boost state
