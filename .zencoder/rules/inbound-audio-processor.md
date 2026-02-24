---
description: Inbound Audio Processor Summary
alwaysApply: true
---

# Inbound Audio Processor

## Overview
The **Inbound Audio Processor** (`inbound-audio-processor.cpp`) converts telephony-grade audio (G.711) from the SIP Client into high-quality PCM audio for the VAD Service.

## Internal Function
- **Decoding**: Decodes G.711 μ-law RTP payloads using an ITU-T G.711 compliant lookup table (256 entries). Each μ-law byte maps to a float32 value in [-1.0, 1.0].
- **Upsampling**: Resamples 8kHz to 16kHz using a 15-tap half-band FIR low-pass filter (Hamming-windowed sinc, cutoff ~3.8kHz, ~40dB stopband attenuation). Zero-stuffs the input then applies the filter to remove spectral copies above 4kHz.
- **Streaming**: Streams the converted float32 PCM audio to the `VAD Service` over a persistent TCP connection. Each RTP packet (160 μ-law bytes = 20ms @ 8kHz) produces 320 float32 samples (20ms @ 16kHz).
- **Resilience**: If the `VAD Service` is unavailable, it continues to process audio but discards the output until a reconnection is successful.

## Inbound Connections
- **SIP Client (TCP)**: Receives RTP packets via interconnect on ports 13110 (mgmt) and 13111 (data).

## Outbound Connections
- **VAD Service (TCP)**: Streams float32 PCM audio (16kHz) to VAD on ports 13115 (mgmt) and 13116 (data).
