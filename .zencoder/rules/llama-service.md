---
description: Summary of the LLaMA Service program
alwaysApply: true
---

# LLaMA Service

## Overview
The **LLaMA Service** (`llama-service.cpp`) provides the brain of the system, using `llama.cpp` to generate intelligent, context-aware responses to user speech.

## Internal Function
- **Session Tracking**: Maintains conversation history for each active call using `LlamaSession`.
- **Response Generation**: Processes transcribed text through a LLaMA model (e.g., LLaMA-3 or similar GGUF models).
- **German Localization**: Uses a specialized German system prompt for a warm, empathetic assistant. Handles German backchannel detection (e.g., "genau", "verstehe").
- **Multi-Call Concurrency**: Handles multiple concurrent TCP streams from `WhisperService`. Each call is assigned a KV cache sequence ID (`seq_id`) for serialized but isolated inference.
- **VAD Thresholding**: Implements dynamic silence detection (500ms for sentences, 1200ms otherwise) to trigger responses.

## Inbound Connections
- **Whisper Service (TCP)**: Receives transcribed text chunks on port 8083. Supports multiple concurrent connections.

## Outbound Connections
- **TTS Service (TCP)**: Sends generated response text to the TTS engine (Kokoro/Piper) on port 8090.
