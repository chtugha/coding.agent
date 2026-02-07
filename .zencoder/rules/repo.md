---
description: Repository Information Overview
alwaysApply: true
---

# WhisperTalk Information

## Summary
WhisperTalk is a high-performance, real-time speech-to-speech system designed for low-latency communication. It features a decoupled microservice architecture that integrates **Whisper** (Automatic Speech Recognition), **LLaMA** (Large Language Model), and **Kokoro/Piper** (Text-to-Speech). The system uses a SIP client as an RTP gateway and is optimized for Apple Silicon via **CoreML/Metal Performance Shaders (MPS)**.

## Structure
The project is organized as a set of independent services communicating over network protocols (TCP/UDP) or shared databases.
- [./coding.agent/](./coding.agent/): Core workspace containing all source code.
- [./coding.agent/whisper-cpp/](./coding.agent/whisper-cpp/): Integration for the whisper.cpp ASR engine with CoreML support.
- [./coding.agent/llama-cpp/](./coding.agent/llama-cpp/): Integration for the llama.cpp LLM engine.
- [./coding.agent/libpiper/](./coding.agent/libpiper/): C++ integration for the Piper TTS engine.
- [./coding.agent/bin/](./coding.agent/bin/): Target directory for all compiled C++ binaries and Python services.
- [./coding.agent/scripts/](./coding.agent/scripts/): Build automation and dependency management scripts.
- [./coding.agent/tests/](./coding.agent/tests/): Custom simulation tools and performance testing scripts.

## Language & Runtime
**Language**: C++ (C++17), Python 3  
**Build System**: CMake (Minimum version 3.22)  
**Package Manager**: Homebrew (for macOS dependencies), pip (for Python requirements)  
**Optimization**: macOS (Apple Silicon optimized with CoreML and MPS)

## Dependencies
**Main Dependencies**:
- **C++ Components**: SQLite3, OpenSSL, Threads.
- **AI Engines**: [whisper.cpp](https://github.com/ggerganov/whisper.cpp), [llama.cpp](https://github.com/ggerganov/llama.cpp), [libpiper](https://github.com/rhasspy/piper).
- **Python Components**: `torch` (with MPS support), `kokoro`, `numpy`.

## Build & Installation
```bash
# Build all dependencies and main components
cd coding.agent && ./scripts/build.sh

# Build dependencies separately if needed
cd coding.agent && ./scripts/build-deps.sh
```

## Documentation Rules
**IMPORTANT**: The program summaries in this folder must be updated every time changes are made to the internal function or connection mechanisms of the respective programs.

## Main Files & Resources
- **SIP Client**: [./coding.agent/sip-client-main.cpp](./coding.agent/sip-client-main.cpp) ([Summary](./sip-client.md))
- **Inbound Audio Processor**: [./coding.agent/inbound-audio-processor.cpp](./coding.agent/inbound-audio-processor.cpp) ([Summary](./inbound-audio-processor.md))
- **Outbound Audio Processor**: [./coding.agent/outbound-audio-processor.cpp](./coding.agent/outbound-audio-processor.cpp) ([Summary](./outbound-audio-processor.md))
- **Whisper Service**: [./coding.agent/whisper-service.cpp](./coding.agent/whisper-service.cpp) ([Summary](./whisper-service.md))
- **LLaMA Service**: [./coding.agent/llama-service.cpp](./coding.agent/llama-service.cpp) ([Summary](./llama-service.md))
- **Kokoro TTS Service**: [./coding.agent/kokoro_service.py](./coding.agent/kokoro_service.py) ([Summary](./kokoro-service.md))
- **Database**: `whisper_talk.db` (Managed via [./coding.agent/database.cpp](./coding.agent/database.cpp)) ([Summary](./database.md))

## Testing & Validation
**Framework**: Custom C++ simulation tools and Python validation scripts.
**Test Location**: [./coding.agent/tests/](./coding.agent/tests/)
**Naming Convention**: `*_sim.cpp`, `*_test.py`


**Run Commands**:
```bash
# Run service health and connectivity tests
./coding.agent/test-services.sh

# Run full pipeline simulation (ASR -> LLM -> TTS)
./coding.agent/test_pipeline_loop.sh

# Run Word Error Rate (WER) performance tests
./coding.agent/run_wer_test.sh
```
