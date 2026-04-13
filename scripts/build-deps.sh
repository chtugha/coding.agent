#!/usr/bin/env bash
# scripts/build-deps.sh
# Build third-party dependencies used by Whisper Talk LLaMA
# - whisper-cpp (libwhisper.dylib)
# - llama-cpp (libllama.dylib)
# Note: libpiper is optional here (it downloads ONNXRuntime). Build separately if needed.

set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"

log(){ printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"; }

ensure_repo(){
  local dir="$1" url="$2"
  if [[ ! -f "$dir/CMakeLists.txt" ]]; then
    log "Cloning $url -> $dir ..."
    rm -rf "$dir"
    git clone --depth=1 "$url" "$dir"
  fi
}

ensure_repo "$ROOT_DIR/whisper-cpp" "https://github.com/ggerganov/whisper.cpp.git"
ensure_repo "$ROOT_DIR/llama-cpp"   "https://github.com/ggerganov/llama.cpp.git"

setup_macos_env(){
  if [[ "$(uname -s)" = "Darwin" ]]; then
    local clt_sdk="/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk"
    local clt_cc="/Library/Developer/CommandLineTools/usr/bin/cc"
    if [[ -d "$clt_sdk" && -x "$clt_cc" ]]; then
      export SDKROOT="$clt_sdk"
      export CC="$clt_cc"
      export CXX="/Library/Developer/CommandLineTools/usr/bin/c++"
    fi
  fi
}
setup_macos_env

CMAKE_GEN=""
if command -v ninja >/dev/null 2>&1; then CMAKE_GEN="-G Ninja"; fi

NCPU=$(sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Build whisper-cpp
log "Configuring whisper-cpp..."
cmake $CMAKE_GEN -S "$ROOT_DIR/whisper-cpp" -B "$ROOT_DIR/whisper-cpp/build" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DWHISPER_COREML=ON -DGGML_METAL=ON -DGGML_CCACHE=OFF -DGGML_OPENMP=OFF \
  -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=OFF
log "Building whisper-cpp (-j${NCPU})..."
cmake --build "$ROOT_DIR/whisper-cpp/build" --config Release -j "${NCPU}"

# Build llama-cpp
log "Configuring llama-cpp..."
cmake $CMAKE_GEN -S "$ROOT_DIR/llama-cpp" -B "$ROOT_DIR/llama-cpp/build" -DCMAKE_BUILD_TYPE=Release \
  -DBUILD_SHARED_LIBS=OFF -DGGML_METAL=ON -DGGML_OPENMP=OFF
log "Building llama-cpp (target: llama, -j${NCPU})..."
cmake --build "$ROOT_DIR/llama-cpp/build" --config Release --target llama -j "${NCPU}"

log "Done. Expected artifacts:"
log "  - $ROOT_DIR/whisper-cpp/build/src/libwhisper.a"
log "  - $ROOT_DIR/llama-cpp/build/src/libllama.a"

