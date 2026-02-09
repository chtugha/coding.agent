# Whisper Service

## Overview
The **Whisper Service** (`whisper-service.cpp`) performs Automatic Speech Recognition (ASR) on the incoming audio streams.

## Internal Function
- **VAD**: Implements a refined energy-based Voice Activity Detection using **100ms windows** to accurately segment German speech without cutting sentences.
- **Inference**: Uses `whisper.cpp` optimized for Apple Silicon via **CoreML/Metal**, providing near-real-time transcription.
- **Session Management**: Manages multiple concurrent transcription sessions using a pool of standalone `WhisperSession` objects.

## Inbound Connections
- **Inbound Audio Processor (TCP)**: Receives float32 PCM audio on ports starting at 13000.

## Outbound Connections
- **LLaMA Service (TCP)**: Sends transcribed text chunks to the `LLaMA Service` on port 8083.
