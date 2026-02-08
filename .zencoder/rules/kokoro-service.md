# Kokoro TTS Service

## Overview
The **Kokoro TTS Service** (`kokoro_service.py`) is a high-fidelity Text-to-Speech engine optimized for Apple Silicon.

## Internal Function
- **Synthesis**: Uses the Kokoro model and PyTorch (MPS) to convert text into 24kHz float32 PCM audio.
- **Language Support**: Specifically optimized for German and English.
- **Streaming**: Streams synthesized audio chunks directly to the `Outbound Audio Processor`.

## Inbound Connections
- **LLaMA Service (TCP)**: Receives response text for synthesis on port 8090.

## Outbound Connections
- **Outbound Audio Processor (TCP)**: Streams float32 PCM audio to the processor on unique ports starting at 8090.
