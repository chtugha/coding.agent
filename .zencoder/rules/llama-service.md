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
- **Concise Logic**: Specifically tuned to generate short, phone-friendly responses (1-2 sentences).
- **Optimization**: Uses KV-cache reuse to speed up subsequent responses in a conversation.

## Inbound Connections
- **Whisper Service (TCP)**: Receives transcribed text chunks on port 8083.

## Outbound Connections
- **TTS Service (TCP)**: Sends generated response text to the TTS engine (Kokoro/Piper) on port 8090.
- **Database (Internal)**: Logs LLM responses in `whisper_talk.db`.
