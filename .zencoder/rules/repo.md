---
description: Repository Information Overview
alwaysApply: true
---

# WhisperTalk Information

## Summary
WhisperTalk is a high-performance, real-time speech-to-speech system designed for low-latency communication. It features a simplified microservice pipeline that integrates **Whisper** (ASR), **LLaMA** (LLM), and **Kokoro** (TTS). The system uses a standalone SIP client as an RTP gateway and is optimized for Apple Silicon (CoreML/Metal).

## Structure
The project is organized as a linear pipeline of 5 core C++ programs and 1 Python service.
- **Root directory**: Contains all 6 core service source files
- [./whisper-cpp/](./whisper-cpp/): Integration for the whisper.cpp ASR engine (submodule/dependency)
- [./bin/](./bin/): Target directory for compiled binaries
- [./scripts/](./scripts/): Build automation scripts
- [./tests/](./tests/): Custom simulation tools and test scripts
- [./libpiper/](./libpiper/): Piper TTS library (legacy/unused)

## Language & Runtime
**Language**: C++ (C++17), Python 3.9+  
**Build System**: CMake (Minimum version 3.22)  
**Optimization**: macOS (Apple Silicon optimized with CoreML and MPS)

## Documentation Rules
- **Summary Updates**: The program summaries in this folder MUST be updated every time changes are made to the internal function or connection mechanisms of the respective programs.
- **Complexity**: Keep services consolidated into single files whenever possible.
- **Decoupling**: Services should only communicate with their direct inbound and outbound neighbors. Database and discovery services are removed to minimize fault surface area.

## Core Components
- **SIP Client**: [./sip-client-main.cpp](./sip-client-main.cpp) ([Summary](./sip-client.md))
- **Inbound Audio Processor**: [./inbound-audio-processor.cpp](./inbound-audio-processor.cpp) ([Summary](./inbound-audio-processor.md))
- **Outbound Audio Processor**: [./outbound-audio-processor.cpp](./outbound-audio-processor.cpp) ([Summary](./outbound-audio-processor.md))
- **Whisper Service**: [./whisper-service.cpp](./whisper-service.cpp) ([Summary](./whisper-service.md))
- **LLaMA Service**: [./llama-service.cpp](./llama-service.cpp) ([Summary](./llama-service.md))
- **Kokoro TTS Service**: [./kokoro_service.py](./kokoro_service.py) ([Summary](./kokoro-service.md))

## Build & Installation
```bash
# Build main components
mkdir -p build && cd build && cmake .. && make
```
