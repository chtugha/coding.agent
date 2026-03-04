---
description: Outbound Audio Processor Summary
alwaysApply: true
---

# Outbound Audio Processor

## Overview
The **Outbound Audio Processor** (`outbound-audio-processor.cpp`) handles the audio pipeline from the TTS engine (Kokoro) back to the SIP Client.

## Internal Function
- **Audio Acquisition**: Receives 24kHz float32 PCM audio from the `Kokoro TTS Service` via TCP interconnect.
- **Downsampling**: 24kHz → 8kHz (ratio 3:1) via 15-tap Hamming-windowed sinc anti-aliasing FIR (cutoff 3400Hz) + decimate by 3.
- **G.711 encoding**: Each float32 sample clipped to [-1, 1] and encoded to μ-law using the standard ITU-T G.711 segment/quantization formula.
- **Scheduling**: Uses a high-precision 20ms timer (steady_clock) to send constant-rate 160-byte G.711 frames back to the SIP Client, filling gaps with ULAW_SILENCE (0xFF) to maintain RTP clock continuity.
- **Multi-Call**: Manages independent CallState (buffer, FIR history, read_pos) per active call_id. buffer.compact() reclaims memory lazily.
- **SPEECH_ACTIVE handling**: Clears all call audio buffers immediately when SPEECH_ACTIVE signal arrives, stopping stale TTS audio.

## Inbound Connections
- **Kokoro TTS (TCP)**: Receives float32 PCM audio via interconnect on ports 13150 (mgmt) and 13151 (data).

## Outbound Connections
- **SIP Client (TCP)**: Sends 160-byte G.711 frames to SIP Client via interconnect on ports 13100 (mgmt) and 13101 (data).

## Command-Line Parameters
- `--log-level <LEVEL>`: Initial log verbosity (ERROR/WARN/INFO/DEBUG/TRACE, default: INFO)

## Runtime Commands (cmd port 13152)
- `PING`: Health check (returns `PONG`)
- `STATUS`: Returns active call count, buffer lengths, upstream/downstream state
- `SET_LOG_LEVEL:<LEVEL>`: Change log verbosity at runtime without restart
