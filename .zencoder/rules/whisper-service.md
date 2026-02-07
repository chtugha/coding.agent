---
description: Summary of the Whisper Service program
alwaysApply: true
---

# Whisper Service

## Overview
The **Whisper Service** (`whisper-service.cpp`) is the core Automatic Speech Recognition (ASR) engine, based on `whisper.cpp` and optimized for Apple Silicon (CoreML).

## Internal Function
- **Session Management**: Manages concurrent transcription sessions using a pool of `WhisperSession` objects.
- **VAD (Voice Activity Detection)**: Implements energy-based VAD to detect speech segments and trigger inference.
- **Inference**: Executes the Whisper model to transcribe audio chunks into text.
- **Performance Optimization**: Uses a "warm context" (pre-loaded model) to avoid latency on the first call and leverages Metal Performance Shaders (MPS).

## Inbound Connections
- **Inbound Audio Processor (TCP)**: Receives float32 PCM audio streams on ports around 13000.
- **SIP Client (UDP Poke)**: Receives registration notifications on port 13000 to proactively initialize sessions.

## Outbound Connections
- **LLaMA Service (TCP)**: Sends transcribed text to `LlamaService` on port 8083 for natural language processing.
- **Database (Internal)**: Appends transcriptions to the call record in `whisper_talk.db`.
