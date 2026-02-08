# Whisper Service

## Overview
The **Whisper Service** (`whisper-service.cpp`) performs Automatic Speech Recognition (ASR) on the incoming audio streams.

## Internal Function
- **VAD**: Implements energy-based Voice Activity Detection to segment audio.
- **Inference**: Uses `whisper.cpp` (Apple Silicon optimized via CoreML/MPS) to transcribe audio segments into text.
- **Session Pooling**: Reuses a single "warm" Whisper context across all sessions to minimize memory and latency.

## Inbound Connections
- **Inbound Audio Processor (TCP)**: Receives float32 PCM audio on ports starting at 13000.

## Outbound Connections
- **LLaMA Service (TCP)**: Sends transcribed text chunks to the `LLaMA Service` on port 8083.
