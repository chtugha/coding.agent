---
description: Build Environment and Configuration Reference
alwaysApply: true
---

# WhisperTalk Build Reference

## Prerequisites

Run `./runmetoinstalleverythingfirst` before building. It installs all dependencies and downloads/converts all models.

Required:
- macOS with Apple Silicon (arm64)
- Homebrew, CMake 3.22+, git, ccache
- Miniconda with `whispertalk` conda environment (Python 3.12, PyTorch, coremltools)
- espeak-ng (`brew install espeak-ng`)
- whisper.cpp cloned at `./whisper-cpp/` (full git clone from https://github.com/ggerganov/whisper.cpp)
- llama.cpp cloned at `./llama-cpp/` (full git clone from https://github.com/ggerganov/llama.cpp)

## Build Commands

Use `./runmetobuildeverything` to build everything in the correct order. Manual steps below.

## CMake Flags — whisper.cpp

```bash
cmake -S whisper-cpp -B whisper-cpp/build \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DWHISPER_COREML=ON \
    -DGGML_METAL=ON \
    -DGGML_CCACHE=OFF \
    -DWHISPER_BUILD_TESTS=OFF \
    -DWHISPER_BUILD_EXAMPLES=OFF

cmake --build whisper-cpp/build --config Release -j$(sysctl -n hw.ncpu)
```

Key flags:
- `DWHISPER_COREML=ON` — enables CoreML ANE acceleration for the encoder
- `DGGML_METAL=ON` — enables Metal GPU fallback (replaces deprecated `DWHISPER_METAL`)
- `DGGML_CCACHE=OFF` — disables ccache (replaces deprecated `DWHISPER_CCACHE`)
- `DBUILD_SHARED_LIBS=OFF` — produces static `libwhisper.a` (required by main CMakeLists.txt)
- `DCMAKE_BUILD_TYPE=Release` — optimized build, always use for production

### Output Libraries (whisper.cpp)

| Library | Path |
|---------|------|
| libwhisper.a | `whisper-cpp/build/src/libwhisper.a` |
| libwhisper.coreml.a | `whisper-cpp/build/src/libwhisper.coreml.a` |
| libggml.a | `whisper-cpp/build/ggml/src/libggml.a` |
| libggml-base.a | `whisper-cpp/build/ggml/src/libggml-base.a` |
| libggml-cpu.a | `whisper-cpp/build/ggml/src/libggml-cpu.a` |
| libggml-blas.a | `whisper-cpp/build/ggml/src/ggml-blas/libggml-blas.a` |
| libggml-metal.a | `whisper-cpp/build/ggml/src/ggml-metal/libggml-metal.a` |

### Headers (whisper.cpp)

- `whisper-cpp/include/` — main whisper.h
- `whisper-cpp/ggml/include/` — ggml headers

## CMake Flags — llama.cpp

```bash
cmake -S llama-cpp -B llama-cpp/build \
    -DCMAKE_BUILD_TYPE=Release \
    -DBUILD_SHARED_LIBS=OFF \
    -DGGML_METAL=ON

cmake --build llama-cpp/build --config Release -j$(sysctl -n hw.ncpu)
```

Key flags:
- `DGGML_METAL=ON` — enables Metal GPU acceleration (replaces deprecated `DLLAMA_METAL`)
- `DBUILD_SHARED_LIBS=OFF` — produces static `libllama.a`
- `DCMAKE_BUILD_TYPE=Release` — always use Release

### Output Libraries (llama.cpp)

| Library | Path |
|---------|------|
| libllama.a | `llama-cpp/build/src/libllama.a` |
| libggml.a | `llama-cpp/build/ggml/src/libggml.a` |
| libggml-base.a | `llama-cpp/build/ggml/src/libggml-base.a` |
| libggml-cpu.a | `llama-cpp/build/ggml/src/libggml-cpu.a` |
| libggml-blas.a | `llama-cpp/build/ggml/src/ggml-blas/libggml-blas.a` |
| libggml-metal.a | `llama-cpp/build/ggml/src/ggml-metal/libggml-metal.a` |

### Headers (llama.cpp)

- `llama-cpp/include/` — main llama.h
- `llama-cpp/ggml/include/` — ggml headers

## CMake Flags — Main Project (WhisperTalk)

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DKOKORO_COREML=ON \
    -DBUILD_TESTS=ON

cmake --build build --config Release -j$(sysctl -n hw.ncpu)
```

Key flags:
- `DKOKORO_COREML=ON` — enables CoreML ANE acceleration for Kokoro TTS decoder
- `DBUILD_TESTS=ON` — builds unit/integration tests (requires Google Test, auto-fetched)
- `DCMAKE_BUILD_TYPE=Release` — always use Release

### Auto-detected Dependencies

- **libtorch**: detected via `python3 -c "import torch; print(torch.utils.cmake_prefix_path)"`
- **espeak-ng library**: searched at `/opt/homebrew/lib`, `/usr/local/lib`
- **espeak-ng headers**: searched at `/opt/homebrew/include`, `/usr/local/include`
- **espeak-ng data**: searched at `/opt/homebrew/share/espeak-ng-data`, `/usr/local/share/espeak-ng-data`

### macOS Frameworks (linked automatically)

- Accelerate, Metal, MetalKit, Foundation, CoreML

### Output Binaries

All binaries go to `bin/`:
- `bin/sip-client`
- `bin/inbound-audio-processor`
- `bin/vad-service`
- `bin/whisper-service`
- `bin/llama-service`
- `bin/kokoro-service`
- `bin/outbound-audio-processor`
- `bin/frontend`

## Models Directory

All models go in `bin/models/`:

| Model | File | Source |
|-------|------|--------|
| Whisper (fastest) | `ggml-large-v3-turbo-q5_0.bin` | HuggingFace ggerganov/whisper.cpp |
| Whisper (best accuracy) | `ggml-large-v3-q5_0.bin` | HuggingFace ggerganov/whisper.cpp |
| Whisper CoreML encoder (large-v3) | `ggml-large-v3-encoder.mlmodelc/` | Generated from whisper.cpp conversion script |
| Whisper CoreML encoder (turbo) | `ggml-large-v3-turbo-encoder.mlmodelc/` | Generated from whisper.cpp conversion script |
| LLaMA | `Llama-3.2-1B-Instruct-Q8_0.gguf` | HuggingFace bartowski |
| Kokoro TTS | `kokoro-german/` | Generated by `scripts/export_kokoro_models.py` |

## Header Include Paths (main CMakeLists.txt)

```cmake
include_directories(
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/llama-cpp/include
    ${CMAKE_SOURCE_DIR}/llama-cpp/ggml/include
    ${CMAKE_SOURCE_DIR}/whisper-cpp/include
    ${CMAKE_SOURCE_DIR}/whisper-cpp/ggml/include
)
```

## Kokoro Model Export

Kokoro models are exported by `scripts/export_kokoro_models.py`. It creates its own conda env `kokoro_coreml` with pinned versions (torch==2.5.0, coremltools==8.3.0, numpy==1.26.4) because newer versions are incompatible with the CoreML conversion pipeline.

See `KOKORO.md` for full details.

## Build Order

1. Build whisper.cpp (produces static libraries)
2. Build llama.cpp (produces static libraries)
3. Build main project (links against whisper.cpp and llama.cpp static libraries)

The main project's CMakeLists.txt expects the static libraries at the paths listed above. Always build dependencies before the main project.
