---
description: Summary of the Inbound Audio Processor program
alwaysApply: true
---

# Inbound Audio Processor

## Overview
The **Inbound Audio Processor** (`inbound-audio-processor.cpp`) is the bridge between the SIP Client and the Whisper ASR service. It transforms telephony-grade audio into a format suitable for high-accuracy speech recognition.

## Internal Function
- **Decoding**: Decodes G.711 μ-law/a-law or PCM16 audio packets received via RTP.
- **Resampling**: Upsamples audio from 8kHz (standard telephony) to 16kHz (required by Whisper).
- **Format Conversion**: Converts 16-bit integer samples to 32-bit floating-point samples.
- **TCP Management**: Maintains persistent TCP connections to the `WhisperService` for each active call.
- **Multi-Call Support**: Manages independent `CallState` objects for concurrent calls, ensuring audio streams remain isolated.

## Inbound Connections
- **SIP Client (UDP)**: Receives RTP packets from the SIP client on a designated UDP port.
- **Control Socket (Unix Domain)**: Receives commands (e.g., `ACTIVATE:call_id`) via `/tmp/inbound-audio-processor.ctrl`.

## Outbound Connections
- **Whisper Service (TCP)**: Streams float32 PCM audio to `WhisperService` on ports starting at 13000. Each call uses a unique port derived from the call ID.
