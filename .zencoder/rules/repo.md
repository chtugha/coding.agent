---
description: Repository Information Overview
alwaysApply: true
---

# WhisperTalk Information

## Summary
WhisperTalk is a high-performance, real-time speech-to-speech system designed for low-latency communication. It features a simplified microservice pipeline that integrates **Whisper** (ASR), **LLaMA** (LLM), and **Kokoro** (TTS). The system uses a standalone SIP client as an RTP gateway and is optimized for Apple Silicon (CoreML/Metal).

## Structure
The project is organized as a linear pipeline of 5 core C++ programs and 1 Python service.
- [./coding.agent/](./coding.agent/): Core workspace containing all source code.
- [./coding.agent/whisper-cpp/](./coding.agent/whisper-cpp/): Integration for the whisper.cpp ASR engine.
- [./coding.agent/llama-cpp/](./coding.agent/llama-cpp/): Integration for the llama.cpp LLM engine.
- [./coding.agent/bin/](./coding.agent/bin/): Target directory for compiled binaries.
- [./coding.agent/scripts/](./coding.agent/scripts/): Build automation scripts.
- [./coding.agent/tests/](./coding.agent/tests/): Custom simulation tools.

## Language & Runtime
**Language**: C++ (C++17), Python 3.9+  
**Build System**: CMake (Minimum version 3.22)  
**Optimization**: macOS (Apple Silicon optimized with CoreML and MPS)

## Mandatory Development Rules
- **No Stubs or Simulated Features**: NEVER implement stubs, placeholder methods, simulated processes, or hardcoded return values. Every function and method MUST contain a real, working implementation. If a dependency is not yet available, the step MUST NOT be marked complete.
- **Fix All Bugs Before Proceeding**: ALL bugs, test failures, and edge cases MUST be fixed before moving to the next step. A step is NOT complete until every test passes and every known issue is resolved. Do not defer fixes to later phases.

## Documentation Rules
- **Summary Updates**: The program summaries in this folder MUST be updated every time changes are made to the internal function or connection mechanisms of the respective programs.
- **Complexity**: Keep services consolidated into single files whenever possible.
- **Decoupling**: Services should only communicate with their direct inbound and outbound neighbors. Database and discovery services are removed to minimize fault surface area.

## Core Components
- **SIP Client**: [./coding.agent/sip-client-main.cpp](./coding.agent/sip-client-main.cpp) ([Summary](./sip-client.md))
- **Inbound Audio Processor**: [./coding.agent/inbound-audio-processor.cpp](./coding.agent/inbound-audio-processor.cpp) ([Summary](./inbound-audio-processor.md))
- **Outbound Audio Processor**: [./coding.agent/outbound-audio-processor.cpp](./coding.agent/outbound-audio-processor.cpp) ([Summary](./outbound-audio-processor.md))
- **Whisper Service**: [./coding.agent/whisper-service.cpp](./coding.agent/whisper-service.cpp) ([Summary](./whisper-service.md))
- **LLaMA Service**: [./coding.agent/llama-service.cpp](./coding.agent/llama-service.cpp) ([Summary](./llama-service.md))
- **Kokoro TTS Service**: [./coding.agent/kokoro_service.py](./coding.agent/kokoro_service.py) ([Summary](./kokoro-service.md))

## Build & Installation
```bash
# Build main components
cd coding.agent && mkdir -p build && cd build && cmake .. && make
```
