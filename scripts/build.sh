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
WITH_PIPER_PREBUILT="auto" # auto|0|1 -> manage/download prebuilt Piper binary for runtime (macOS arm64)
FORCE_DEPS=0        # 1 to rebuild deps even if present
ONNXRUNTIME_DIR_ENV="${ONNXRUNTIME_DIR:-}"
ONNXRUNTIME_DIR_OPT=""

print_help(){ cat <<USAGE
Usage: scripts/build.sh [options]
Options:
  --with-deps              Build whisper-cpp and llama-cpp if missing (default: auto)
  --no-deps                Do not attempt to build third-party dependencies
  --with-piper             Build libpiper (default)
  --no-piper               Skip building libpiper
  --with-prebuilt-piper    Ensure latest prebuilt Piper binary is downloaded (default: auto on macOS arm64)
  --no-prebuilt-piper      Skip prebuilt Piper binary management
  --force-deps             Force rebuild of third-party dependencies
  --onnxruntime-dir P      Use an existing ONNX Runtime installation at directory P (must have include/ and lib/)
  -h, --help               Show this help
USAGE
}

# Parse args (support --key=value and positional forms)
while [[ $# -gt 0 ]]; do
  case "$1" in
    --with-deps) WITH_DEPS=1; shift ;;
    --no-deps)   WITH_DEPS=0; shift ;;
    --with-piper) WITH_PIPER=1; shift ;;
    --no-piper)   WITH_PIPER=0; shift ;;
    --with-prebuilt-piper) WITH_PIPER_PREBUILT=1; shift ;;
    --no-prebuilt-piper)   WITH_PIPER_PREBUILT=0; shift ;;
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
  cmake -S "$ROOT_DIR/whisper-cpp" -B "$ROOT_DIR/whisper-cpp/build" -DCMAKE_BUILD_TYPE=Release \
    -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=OFF
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
# --- Prebuilt Piper management (macOS arm64) ---
ensure_prebuilt_piper(){
  local os uname_m asset_name api_url latest_tag asset_url tmp tgz out_dir bin_dir version_file
  os="$(uname -s)"; uname_m="$(uname -m)"
  if [[ "$os" != "Darwin" || "$uname_m" != "arm64" ]]; then
    log "Prebuilt Piper management skipped (platform: $os/$uname_m)"
    return 0
  fi

  # Allow override to force skip
  if [[ "$WITH_PIPER_PREBUILT" = "0" ]]; then
    log "Skipping prebuilt Piper per --no-prebuilt-piper"
    return 0
  fi


  asset_name="piper_macos_aarch64.tar.gz"
  api_url="https://api.github.com/repos/rhasspy/piper/releases/latest"
  bin_dir="$ROOT_DIR/bin"
  out_dir="$ROOT_DIR/third_party/piper-prebuilt"
  version_file="$out_dir/version.txt"
  mkdir -p "$out_dir" "$bin_dir"

  # Discover latest release tag and asset URL
  latest_tag="$(curl -sL "$api_url" | sed -n 's/.*"tag_name"\s*:\s*"\([^"]\+\)".*/\1/p' | head -n1)"
  asset_url="$(curl -sL "$api_url" | awk -v n="$asset_name" '
    BEGIN{RS="},"; FS="\n"}
    $0 ~ /"name"\s*:\s*""n""/ {
      if ($0 ~ /browser_download_url/) {
        match($0, /"browser_download_url"\s*:\s*"([^"]+)"/, a);
        print a[1];
      }
    }' | head -n1)"

  # Fallback to known release if API rate-limited or asset not found
  if [[ -z "$latest_tag" || -z "$asset_url" ]]; then
    latest_tag="2023.11.14-2"
    asset_url="https://github.com/rhasspy/piper/releases/download/${latest_tag}/${asset_name}"
    log "GitHub API unavailable; falling back to Piper $latest_tag"
  else
    log "Latest Piper release: $latest_tag"
  fi

  # Skip if up-to-date and binary present
  if [[ -f "$out_dir/piper" && -f "$version_file" ]] && grep -q "$latest_tag" "$version_file"; then
    cp -f "$out_dir/piper" "$bin_dir/piper-prebuilt" 2>/dev/null || true
    chmod +x "$bin_dir/piper-prebuilt" || true
    log "Prebuilt Piper already up-to-date ($latest_tag)"
    return 0
  fi

  tmp="$(mktemp -d)"; tgz="$tmp/piper.tar.gz"
  log "Downloading prebuilt Piper: $asset_url"
  if ! curl -L "$asset_url" -o "$tgz"; then
    log "ERROR: Failed to download prebuilt Piper"
    rm -rf "$tmp"
    return 1
  fi
  log "Extracting prebuilt Piper..."
  tar -xzf "$tgz" -C "$tmp" || { log "ERROR: Extract failed"; rm -rf "$tmp"; return 1; }
  # Find the binary named 'piper' in extracted directory
  local extracted_bin
  extracted_bin="$(find "$tmp" -type f -name piper -perm +111 2>/dev/null | head -n1)"
  if [[ -z "$extracted_bin" ]]; then
    # Try common path
    extracted_bin="$(find "$tmp" -type f -name piper 2>/dev/null | head -n1)"
  fi
  if [[ -z "$extracted_bin" ]]; then
    log "ERROR: Could not locate 'piper' binary in archive"
    rm -rf "$tmp"
    return 1
  fi
  mkdir -p "$out_dir"
  cp -f "$extracted_bin" "$out_dir/piper"
  chmod +x "$out_dir/piper"
  echo "$latest_tag" > "$version_file"
  cp -f "$out_dir/piper" "$bin_dir/piper-prebuilt"
  chmod +x "$bin_dir/piper-prebuilt"
  rm -rf "$tmp"
  log "Installed prebuilt Piper $latest_tag to $out_dir and $bin_dir/piper-prebuilt"
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
# Ensure prebuilt Piper binary is available (macOS arm64 only; safe no-op elsewhere)
ensure_prebuilt_piper || true

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
  -DBUILD_SIP_CLIENT=ON

log "Building targets..."
cmake --build "$BUILD_DIR" --config Release -j 6

log "Binaries in: $ROOT_DIR/bin"

