# LLaMA Service

## Overview
The **LLaMA Service** (`llama-service.cpp`) generates intelligence responses based on the transcribed text from Whisper.

## Internal Function
- **Conversational Logic**: Manages short-term history and generates concise (1-2 sentence) responses.
- **Inference**: Uses `llama.cpp` (Apple Silicon optimized via Metal) to run GGUF models.
- **Session Isolation**: Each call has an independent conversational context.

## Inbound Connections
- **Whisper Service (TCP)**: Receives transcribed text on port 8083.

## Outbound Connections
- **Kokoro TTS (TCP)**: Sends generated response text to the TTS service on port 8090.
