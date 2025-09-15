#!/usr/bin/env bash
# scripts/build-deps.sh
# Build third-party dependencies used by Whisper Talk LLaMA
# - whisper-cpp (libwhisper.dylib)
# - llama-cpp (libllama.dylib)
# Note: libpiper is optional here (it downloads ONNXRuntime). Build separately if needed.

set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

log(){ printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"; }

# Build whisper-cpp
log "Configuring whisper-cpp..."
cmake -S "$ROOT_DIR/whisper-cpp" -B "$ROOT_DIR/whisper-cpp/build" -DCMAKE_BUILD_TYPE=Release
log "Building whisper-cpp..."
cmake --build "$ROOT_DIR/whisper-cpp/build" --config Release -j 6

# Build llama-cpp
log "Configuring llama-cpp..."
cmake -S "$ROOT_DIR/llama-cpp" -B "$ROOT_DIR/llama-cpp/build" -DCMAKE_BUILD_TYPE=Release
log "Building llama-cpp (target: llama)..."
cmake --build "$ROOT_DIR/llama-cpp/build" --config Release --target llama -j 6

log "Done. Expected artifacts:"
log "  - $ROOT_DIR/whisper-cpp/build/src/libwhisper.dylib"
log "  - $ROOT_DIR/llama-cpp/build/bin/libllama.dylib"

