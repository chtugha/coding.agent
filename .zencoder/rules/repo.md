---
description: Repository Information Overview
alwaysApply: true
---

# Prodigy Information

## Summary
Prodigy is a high-performance, real-time speech-to-speech system designed for low-latency communication. It features a simplified microservice pipeline that integrates **Whisper** (ASR), **LLaMA** (LLM), and **Kokoro** (TTS). The system uses a standalone SIP client as an RTP gateway and is optimized for Apple Silicon (CoreML/Metal).

## Structure
The project is organized as a linear pipeline of 7 core C++ programs.
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

## Naming Conventions
- The project was renamed from "WhisperTalk" to "Prodigy". All user-visible strings, docs, and comments use "Prodigy".
- **`namespace whispertalk`** and **`WHISPERTALK_MODELS_DIR`** are intentionally preserved. The C++ namespace is used ~240 times across all 7 services and shared headers (`interconnect.h`, `tts-common.h`) — renaming it would be a high-risk refactor with no user-visible benefit. The env var is a runtime contract for existing deployments. CSS custom properties also retain the `--wt-*` prefix for the same reason.
- Do **not** reintroduce "WhisperTalk" in user-facing strings, docs, or comments. Do **not** rename `namespace whispertalk` or `WHISPERTALK_MODELS_DIR` without a dedicated refactor task.

## Mandatory Development Rules
- **No Stubs or Simulated Features**: NEVER implement stubs, placeholder methods, simulated processes, or hardcoded return values. Every function and method MUST contain a real, working implementation. If a dependency is not yet available, the step MUST NOT be marked complete.
- **Fix All Bugs Before Proceeding**: ALL bugs, test failures, and edge cases MUST be fixed before moving to the next step. A step is NOT complete until every test passes and every known issue is resolved. Do not defer fixes to later phases.

## Documentation Rules
- **Summary Updates**: The program summaries in this folder MUST be updated every time changes are made to the internal function or connection mechanisms of the respective programs.
- **Complexity**: Keep services consolidated into single files whenever possible.
- **Decoupling**: Services should only communicate with their direct inbound and outbound neighbors. Database and discovery services are removed to minimize fault surface area.

## Pipeline
```
SIP Client → IAP → VAD → Whisper → LLaMA → Kokoro → OAP → SIP Client (loop)
```

## Core Components
- **SIP Client**: [./coding.agent/sip-client-main.cpp](./coding.agent/sip-client-main.cpp) ([Summary](./sip-client.md))
- **Inbound Audio Processor**: [./coding.agent/inbound-audio-processor.cpp](./coding.agent/inbound-audio-processor.cpp) ([Summary](./inbound-audio-processor.md))
- **VAD Service**: [./coding.agent/vad-service.cpp](./coding.agent/vad-service.cpp) ([Summary](./vad-service.md))
- **Whisper Service**: [./coding.agent/whisper-service.cpp](./coding.agent/whisper-service.cpp) ([Summary](./whisper-service.md))
- **LLaMA Service**: [./coding.agent/llama-service.cpp](./coding.agent/llama-service.cpp) ([Summary](./llama-service.md))
- **Kokoro TTS Service**: [./coding.agent/kokoro-service.cpp](./coding.agent/kokoro-service.cpp) ([Summary](./kokoro-service.md))
- **Outbound Audio Processor**: [./coding.agent/outbound-audio-processor.cpp](./coding.agent/outbound-audio-processor.cpp) ([Summary](./outbound-audio-processor.md))

## Build & Installation
```bash
# Build main components
cd coding.agent && mkdir -p build && cd build && cmake .. && make
```
