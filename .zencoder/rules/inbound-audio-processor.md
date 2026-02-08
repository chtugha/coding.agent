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
- **Dynamic Session Management**: Manages independent `CallState` objects for concurrent calls. It extracts a 4-byte `call_num_id` prefix from incoming UDP packets and automatically activates Whisper sessions on the first packet.
- **ID Negotiation Service**: Provides a Unix Domain Socket control interface to assign unique `call_num_id`s to connecting SIP client instances, ensuring isolation in the multi-call pipeline.
- **TCP Management**: Maintains persistent TCP connections to the `WhisperService` for each active call session.

## Inbound Connections
- **SIP Client (UDP)**: Receives RTP packets from the SIP client on port 9001 (default). Packets are prefixed with a 4-byte big-endian `call_num_id`.
- **Control Socket (Unix Domain)**: Listens on `/tmp/inbound-audio-processor.ctrl` for `GET_ID` requests from SIP clients.

## Outbound Connections
- **Whisper Service (TCP)**: Streams float32 PCM audio to `WhisperService` on unique ports calculated based on the negotiated `call_num_id`.
