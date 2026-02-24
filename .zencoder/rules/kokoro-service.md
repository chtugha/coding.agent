---
description: Kokoro TTS Service Summary
alwaysApply: true
---

# Kokoro TTS Service

## Overview
The **Kokoro TTS Service** (`kokoro-service.cpp`) is a high-fidelity Text-to-Speech engine optimized for Apple Silicon with CoreML ANE acceleration.

## Internal Function
- **Synthesis**: Uses the Kokoro model with PyTorch/CoreML to convert text into 24kHz float32 PCM audio.
- **Language Support**: Specifically optimized for German and English using espeak-ng for phonemization.
- **Streaming**: Streams synthesized audio chunks directly to the `Outbound Audio Processor`.
- **CoreML Split Decoder**: Uses CoreML Apple Neural Engine acceleration for the decoder portion of the model.

## Inbound Connections
- **LLaMA Service (TCP)**: Receives response text via interconnect on ports 13140 (mgmt) and 13141 (data).

## Outbound Connections
- **Outbound Audio Processor (TCP)**: Streams float32 PCM audio to OAP on ports 13150 (mgmt) and 13151 (data).
