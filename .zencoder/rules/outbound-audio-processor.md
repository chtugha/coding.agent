---
description: Summary of the Outbound Audio Processor program
alwaysApply: true
---

# Outbound Audio Processor

## Overview
The **Outbound Audio Processor** (`outbound-audio-processor.cpp`) handles the audio pipeline from the TTS engine (Kokoro/Piper) back to the SIP Client, enabling the system to speak to the caller.

## Internal Function
- **Audio Acquisition**: Receives high-quality PCM audio from the TTS service via TCP.
- **Resampling/Processing**: Downsamples audio (typically 22.05kHz or 24kHz) to 8kHz and applies anti-aliasing filters.
- **Encoding**: Converts float32 samples to G.711 μ-law format for telephony transmission.
- **Continuous Scheduling**: Runs a high-precision scheduler to output exactly 20ms of audio every 20ms, filling gaps with silence or comfort noise to maintain RTP timing.

## Inbound Connections
- **TTS Service (TCP)**: Receives float32 PCM audio on ports starting at 8090.
- **Control Socket (Unix Domain)**: Receives commands via `/tmp/outbound-audio-processor.ctrl`.

## Outbound Connections
- **SIP Client (UDP)**: Sends encoded G.711 RTP-ready packets to the SIP Client's RTP input port.
