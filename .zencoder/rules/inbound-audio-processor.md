# Inbound Audio Processor

## Overview
The **Inbound Audio Processor** (`inbound-audio-processor.cpp`) converts telephony-grade audio (G.711) from the SIP Client into high-quality PCM audio for the Whisper ASR service.

## Internal Function
- **Decoding**: Decodes G.711 μ-law RTP payloads into 16-bit PCM.
- **Conversion**: Normalizes audio to float32 and resamples from 8kHz to 16kHz (linear interpolation).
- **Streaming**: Streams the converted audio to the `Whisper Service` over a persistent TCP connection.
- **Resilience**: If the `Whisper Service` is unavailable, it continues to process audio but "dumps" (discards) the output until a reconnection is successful.

## Inbound Connections
- **SIP Client (UDP)**: Receives prefixed RTP packets on UDP port 9001.

## Outbound Connections
- **Whisper Service (TCP)**: Streams float32 PCM audio on ports `13000 + (call_id % 100)`.
