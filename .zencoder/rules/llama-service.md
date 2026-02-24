---
description: LLaMA Service Summary
alwaysApply: true
---

# LLaMA Service

## Overview
The **LLaMA Service** (`llama-service.cpp`) generates intelligent responses based on the transcribed text from Whisper.

## Internal Function
- **Inference**: Uses the **Llama-3.2-1B-Instruct** model (Q8_0 GGUF) with `llama_chat_apply_template` for robust, template-aware conversation logic.
- **German Optimization**: Features a specialized German system prompt designed for concise, empathetic, and natural phone interactions.
- **Apple Silicon Optimization**: Native Metal/MPS acceleration for extremely low-latency response generation.
- **Session Isolation**: Each call has an independent conversational context managed via sequence IDs in the KV cache.

## Inbound Connections
- **Whisper Service (TCP)**: Receives transcribed text via interconnect on ports 13130 (mgmt) and 13131 (data).

## Outbound Connections
- **Kokoro TTS (TCP)**: Sends generated response text to Kokoro on ports 13140 (mgmt) and 13141 (data).
