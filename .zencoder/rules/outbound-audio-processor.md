# Outbound Audio Processor

## Overview
The **Outbound Audio Processor** (`outbound-audio-processor.cpp`) handles the audio pipeline from the TTS engine (Kokoro) back to the SIP Client.

## Internal Function
- **Audio Acquisition**: Receives 24kHz float32 PCM audio from the `Kokoro TTS Service` via TCP.
- **Downsampling**: Converts audio to 8kHz and encodes it into G.711 μ-law format.
- **Scheduling**: Uses a high-precision 20ms timer to send constant-rate audio frames back to the `SIP Client`, filling gaps with silence if necessary.
- **Multi-Call**: Manages independent buffers and TCP listeners for each active call.

## Inbound Connections
- **Kokoro TTS (TCP)**: Receives float32 PCM audio on ports `8090 + (call_id % 100)`.
- **Control Socket (Unix)**: Receives `ACTIVATE:call_id` on `/tmp/outbound-audio-processor.ctrl`.

## Outbound Connections
- **SIP Client (UDP)**: Sends 160-byte G.711 frames (prefixed with `call_id`) to UDP port 9002.
