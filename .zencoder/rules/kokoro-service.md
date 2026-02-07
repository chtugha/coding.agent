---
description: Summary of the Kokoro TTS Service program
alwaysApply: true
---

# Kokoro TTS Service

## Overview
The **Kokoro TTS Service** (`kokoro_service.py`) is a high-fidelity Text-to-Speech engine. It serves as an optimized replacement for Piper, providing natural-sounding voices for both English and German.

## Internal Function
- **Synthesis**: Uses the Kokoro model and PyTorch to convert text into 24kHz float32 PCM audio.
- **Language Support**: Includes a custom German pipeline with dedicated models and voices.
- **Hardware Acceleration**: Optimized for Apple Silicon using Metal Performance Shaders (MPS).
- **Concurrency**: Handles multiple synthesis requests simultaneously for concurrent calls.

## Inbound Connections
- **LLaMA Service (TCP)**: Receives response text for synthesis on port 8090.
- **Outbound Audio Processor (UDP)**: Receives registration notifications from the audio processor to know where to send synthesized audio.

## Outbound Connections
- **Outbound Audio Processor (TCP)**: Streams synthesized PCM audio chunks to the audio processor on unique ports starting at 8090.
