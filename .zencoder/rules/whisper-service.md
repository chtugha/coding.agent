---
description: Summary of the Whisper Service program
alwaysApply: true
---

# Whisper Service

## Overview
The **Whisper Service** (`whisper-service.cpp`) is the core Automatic Speech Recognition (ASR) engine, based on `whisper.cpp` and optimized for Apple Silicon (CoreML).

## Internal Function
- **Session Management**: Manages concurrent transcription sessions using a pool of `WhisperSession` objects. Sessions are dynamically assigned to `call_id`s.
- **Multi-Call Routing**: Listens on a base port (13000) for UDP registration and spawns TCP listeners on unique ports for each call.
- **VAD (Voice Activity Detection)**: Implements energy-based VAD to detect speech segments and trigger inference.
- **Inference**: Executes the Whisper model to transcribe audio chunks into text. Optimized for Apple Silicon (CoreML/MPS).
- **German Optimization**: Specifically configured for German ASR with correct language parameters.

## Inbound Connections
- **Inbound Audio Processor (TCP)**: Receives float32 PCM audio streams on unique ports derived from the call ID.
- **Registration (UDP)**: Receives `REGISTER:call_id` messages on port 13000 to initialize sessions.

## Outbound Connections
- **LLaMA Service (TCP)**: Sends transcribed text to `LlamaService` on port 8083 for natural language processing.
