---
description: LLaMA Service Summary
alwaysApply: true
---

# LLaMA Service

## Overview
The **LLaMA Service** (`llama-service.cpp`) generates intelligent responses based on the transcribed text from Whisper.

## Internal Function
- **Inference**: Uses the **Llama-3.2-1B-Instruct** model (Q8_0 GGUF) with `llama_chat_apply_template` for robust, template-aware conversation logic. Greedy sampling with max 64 tokens per response. Stops at sentence-ending punctuation (`.`, `?`, `!`) or EOS.
- **German Optimization**: Specialized German system prompt enforcing: always German, max 1 sentence / 15 words, polite and natural. Avg quality score ~70% across 10 test prompts, 90% German detection rate (word-boundary matching), ~320ms avg latency. Full pipeline tested: SIP→IAP→VAD→Whisper→LLaMA with audio injection via test SIP provider.
- **Apple Silicon Optimization**: Native Metal/MPS acceleration for extremely low-latency response generation.
- **Session Isolation**: Each call has an independent conversational context managed via sequence IDs in the KV cache. Context cleared on CALL_END.
- **Shut-up Mechanism**: SPEECH_ACTIVE signal interrupts active generation (sets `generating = false`). Worker loop defers new responses while speech is active. Interrupt latency ~5-13ms.
- **Tokenizer Resilience**: Handles negative token counts from `llama_tokenize` by retrying with larger buffer.

## Inbound Connections
- **Whisper Service (TCP)**: Receives transcribed text via interconnect on ports 13130 (mgmt) and 13131 (data).

## Outbound Connections
- **Kokoro TTS (TCP)**: Sends generated response text to Kokoro on ports 13140 (mgmt) and 13141 (data).

## Command-Line Parameters
- `--model <path>` / `-m <path>`: Path to GGUF model file (default: models/Llama-3.2-1B-Instruct-Q8_0.gguf)
- `--log-level <LEVEL>`: Initial log verbosity (ERROR/WARN/INFO/DEBUG/TRACE, default: INFO)

## Runtime Commands (cmd port 13132)
- `PING`: Health check (returns `PONG`)
- `STATUS`: Returns model name, active calls, upstream/downstream state, speech active state
- `SET_LOG_LEVEL:<LEVEL>`: Change log verbosity at runtime without restart
