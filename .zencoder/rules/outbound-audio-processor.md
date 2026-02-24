---
description: Outbound Audio Processor Summary
alwaysApply: true
---

# Outbound Audio Processor

## Overview
The **Outbound Audio Processor** (`outbound-audio-processor.cpp`) handles the audio pipeline from the TTS engine (Kokoro) back to the SIP Client.

## Internal Function
- **Audio Acquisition**: Receives 24kHz float32 PCM audio from the `Kokoro TTS Service` via TCP interconnect.
- **Downsampling**: Converts audio to 8kHz and encodes it into G.711 μ-law format.
- **Scheduling**: Uses a high-precision 20ms timer to send constant-rate audio frames back to the `SIP Client`, filling gaps with silence if necessary.
- **Multi-Call**: Manages independent buffers and TCP listeners for each active call.

## Inbound Connections
- **Kokoro TTS (TCP)**: Receives float32 PCM audio via interconnect on ports 13150 (mgmt) and 13151 (data).

## Outbound Connections
- **SIP Client (TCP)**: Sends 160-byte G.711 frames to SIP Client via interconnect on ports 13100 (mgmt) and 13101 (data).
