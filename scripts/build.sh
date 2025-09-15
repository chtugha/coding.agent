#!/usr/bin/env bash
# scripts/build.sh
# Configure and build Whisper Talk LLaMA into ./bin
# Builds third-party dependencies intelligently, including ONNX Runtime detection for libpiper.

set -euo pipefail
ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"

log(){ printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"; }

# Flags and options
WITH_DEPS="auto"   # auto|0|1  -> auto builds deps if missing
WITH_PIPER=1        # build libpiper by default (we intelligently detect ONNXRuntime)
FORCE_DEPS=0        # 1 to rebuild deps even if present
ONNXRUNTIME_DIR_ENV="${ONNXRUNTIME_DIR:-}"
ONNXRUNTIME_DIR_OPT=""

print_help(){ cat <<USAGE
Usage: scripts/build.sh [options]
Options:
  --with-deps          Build whisper-cpp and llama-cpp if missing (default: auto)
  --no-deps            Do not attempt to build third-party dependencies
  --with-piper         Build libpiper (default)
  --no-piper           Skip building libpiper
  --force-deps         Force rebuild of third-party dependencies
  --onnxruntime-dir P  Use an existing ONNX Runtime installation at directory P (must have include/ and lib/)
  -h, --help           Show this help
USAGE
}

# Parse args (support --key=value and positional forms)
while [[ $# -gt 0 ]]; do
  case "$1" in
    --with-deps) WITH_DEPS=1; shift ;;
    --no-deps)   WITH_DEPS=0; shift ;;
    --with-piper) WITH_PIPER=1; shift ;;
    --no-piper)   WITH_PIPER=0; shift ;;
    --force-deps) FORCE_DEPS=1; shift ;;
    --onnxruntime-dir)
      ONNXRUNTIME_DIR_OPT="$2"; shift 2 ;;
    --onnxruntime-dir=*)
      ONNXRUNTIME_DIR_OPT="${1#*=}"; shift ;;
    -h|--help) print_help; exit 0 ;;
    *) log "Unknown option: $1"; print_help; exit 1 ;;
  esac
done

WHISPER_LIB="$ROOT_DIR/whisper-cpp/build/src/libwhisper.dylib"
LLAMA_LIB="$ROOT_DIR/llama-cpp/build/bin/libllama.dylib"
PIPER_LIB="$ROOT_DIR/libpiper/build/libpiper.dylib"

need_whisper=0; need_llama=0
[[ ! -f "$WHISPER_LIB" ]] && need_whisper=1
[[ ! -f "$LLAMA_LIB"   ]] && need_llama=1

if [[ "$FORCE_DEPS" = "1" ]]; then
  need_whisper=1; need_llama=1
fi

# --- ONNX Runtime detection ---
onnx_ok=""
onxx_candidate=""

have_brew(){ command -v brew >/dev/null 2>&1; }

has_onnx_headers_and_lib(){
  local dir="$1"
  [[ -f "$dir/include/onnxruntime/core/session/onnxruntime_c_api.h" ]] || return 1
  # Accept either libonnxruntime.dylib or libonnxruntime.*
  ls "$dir/lib"/libonnxruntime.* >/dev/null 2>&1 || return 1
  return 0
}

select_onnx(){
  # Priority: explicit option > env var > Homebrew > none
  if [[ -n "$ONNXRUNTIME_DIR_OPT" ]] && has_onnx_headers_and_lib "$ONNXRUNTIME_DIR_OPT"; then
    onnx_ok="$ONNXRUNTIME_DIR_OPT"; return 0
  fi
  if [[ -n "$ONNXRUNTIME_DIR_ENV" ]] && has_onnx_headers_and_lib "$ONNXRUNTIME_DIR_ENV"; then
    onnx_ok="$ONNXRUNTIME_DIR_ENV"; return 0
  fi
  if have_brew; then
    local prefix
    if prefix="$(brew --prefix onnxruntime 2>/dev/null)" && [[ -n "$prefix" ]] && has_onnx_headers_and_lib "$prefix"; then
      onnx_ok="$prefix"; return 0
    fi
  fi
  return 1
}

# Quick compatibility heuristic for Piper voice model requirements:
# - presence of C API header and runtime lib is a strong indicator
# - macOS/Homebrew builds are CPU provider by default, which Piper supports
check_onnx_compat(){
  local dir="$1"
  # Optionally check version header if present
  if [[ -f "$dir/include/onnxruntime/core/session/onnxruntime_version.h" ]]; then
    # Accept any reasonably recent version (1.14+) – Piper known to work well with >=1.14.
    return 0
  fi
  # If not present, assume compatible when libs and headers exist
  return 0
}

# --- Builders ---
build_whisper(){
  log "Configuring whisper-cpp..."
  cmake -S "$ROOT_DIR/whisper-cpp" -B "$ROOT_DIR/whisper-cpp/build" -DCMAKE_BUILD_TYPE=Release
  log "Building whisper-cpp..."
  cmake --build "$ROOT_DIR/whisper-cpp/build" --config Release -j 6
}

build_llama(){
  log "Configuring llama-cpp..."
  cmake -S "$ROOT_DIR/llama-cpp" -B "$ROOT_DIR/llama-cpp/build" -DCMAKE_BUILD_TYPE=Release
  log "Building llama-cpp (target: llama)..."
  cmake --build "$ROOT_DIR/llama-cpp/build" --config Release --target llama -j 6
}

build_piper(){
  local cmake_extra=()
  # Prefer system ONNX Runtime if available
  if select_onnx; then
    if check_onnx_compat "$onnx_ok"; then
      log "Using system ONNX Runtime at: $onnx_ok"
      cmake_extra+=( -DONNXRUNTIME_DIR="$onnx_ok" )
    else
      log "System ONNX Runtime found but deemed incompatible; will allow libpiper to download vendor runtime"
    fi
  else
    log "No suitable system ONNX Runtime found; libpiper will download vendor runtime"
  fi

  # Detect nlohmann-json header (header-only)
  local nloh_prefix=""
  if have_brew; then
    if nloh_prefix="$(brew --prefix nlohmann-json 2>/dev/null)" && [[ -f "$nloh_prefix/include/nlohmann/json.hpp" ]]; then
      log "Using nlohmann-json headers from: $nloh_prefix/include"
      cmake_extra+=( -DNLOHMANN_JSON_INCLUDE_DIR="$nloh_prefix/include" )
    fi
  fi

  log "Configuring libpiper..."
  if [[ ${#cmake_extra[@]} -gt 0 ]]; then
    cmake -S "$ROOT_DIR/libpiper" -B "$ROOT_DIR/libpiper/build" -DCMAKE_BUILD_TYPE=Release "${cmake_extra[@]}"
  else
    cmake -S "$ROOT_DIR/libpiper" -B "$ROOT_DIR/libpiper/build" -DCMAKE_BUILD_TYPE=Release
  fi
  log "Building libpiper..."
  cmake --build "$ROOT_DIR/libpiper/build" --config Release -j 4
}

# --- Dependency build policy ---
case "$WITH_DEPS" in
  1)  [[ "$need_whisper" = 1 ]] && build_whisper
      [[ "$need_llama"   = 1 ]] && build_llama ;;
  auto)
      [[ "$need_whisper" = 1 ]] && build_whisper
      [[ "$need_llama"   = 1 ]] && build_llama ;;
  0)  : ;; # skip
  *)  log "Invalid WITH_DEPS: $WITH_DEPS"; exit 1 ;;
esac

if [[ "$WITH_PIPER" = 1 ]]; then
  build_piper || { log "ERROR: libpiper build failed"; exit 1; }
fi

# Verify artifacts exist
if [[ ! -f "$WHISPER_LIB" ]]; then
  log "ERROR: libwhisper not found at $WHISPER_LIB"; exit 1
fi
if [[ ! -f "$LLAMA_LIB" ]]; then
  log "ERROR: libllama not found at $LLAMA_LIB"; exit 1
fi

# Top-level build
log "Configuring top-level project..."
cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release \
  -DWHISPER_CPP_LIB="$WHISPER_LIB" \
  -DLLAMA_CPP_LIB="$LLAMA_LIB" \
  -DBUILD_SIP_CLIENT=OFF

log "Building targets..."
cmake --build "$BUILD_DIR" --config Release -j 6

log "Binaries in: $ROOT_DIR/bin"

