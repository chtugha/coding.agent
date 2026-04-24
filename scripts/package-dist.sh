#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
DIST_DIR="${1:-$ROOT_DIR/dist/prodigy}"

log(){ printf '[%s] %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*"; }

TORCH_LIB=$(python3 -c "import torch, os; print(os.path.join(os.path.dirname(torch.__file__), 'lib'))" 2>/dev/null || true)
if [[ -z "$TORCH_LIB" || ! -d "$TORCH_LIB" ]]; then
    log "ERROR: Cannot find PyTorch lib directory. Ensure torch is installed."
    exit 1
fi

ESPEAK_DATA=""
for d in /opt/homebrew/share/espeak-ng-data /usr/local/share/espeak-ng-data /usr/share/espeak-ng-data; do
    if [[ -d "$d" ]]; then ESPEAK_DATA="$d"; break; fi
done
if [[ -z "$ESPEAK_DATA" ]]; then
    log "ERROR: Cannot find espeak-ng-data directory."
    exit 1
fi

ESPEAK_DYLIB=""
for d in /opt/homebrew/lib /usr/local/lib /usr/lib; do
    if [[ -f "$d/libespeak-ng.1.dylib" ]]; then ESPEAK_DYLIB="$d/libespeak-ng.1.dylib"; break; fi
    if [[ -f "$d/libespeak-ng.dylib" ]]; then ESPEAK_DYLIB="$d/libespeak-ng.dylib"; break; fi
done
if [[ -z "$ESPEAK_DYLIB" ]]; then
    log "ERROR: Cannot find libespeak-ng dylib."
    exit 1
fi

PCAUDIO_DYLIB=""
for d in /opt/homebrew/lib /usr/local/lib /usr/lib; do
    if [[ -f "$d/libpcaudio.0.dylib" ]]; then PCAUDIO_DYLIB="$d/libpcaudio.0.dylib"; break; fi
    if [[ -f "$d/libpcaudio.dylib" ]]; then PCAUDIO_DYLIB="$d/libpcaudio.dylib"; break; fi
done

log "Creating distribution at: $DIST_DIR"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"/{bin,lib,models,espeak-ng-data}

log "Copying binaries..."
SERVICES=(sip-client inbound-audio-processor vad-service outbound-audio-processor whisper-service llama-service kokoro-service neutts-service frontend tomedo-crawl)
for svc in "${SERVICES[@]}"; do
    src="$ROOT_DIR/bin/$svc"
    if [[ ! -f "$src" ]]; then
        log "WARNING: $svc not found at $src, skipping"
        continue
    fi
    cp "$src" "$DIST_DIR/bin/"
    log "  $svc ($(du -h "$src" | cut -f1))"
done

log "Bundling libtorch dylibs..."
TORCH_DYLIBS=(libc10.dylib libtorch.dylib libtorch_cpu.dylib libomp.dylib)
for lib in "${TORCH_DYLIBS[@]}"; do
    src="$TORCH_LIB/$lib"
    if [[ ! -f "$src" ]]; then
        log "WARNING: $lib not found at $src"
        continue
    fi
    cp "$src" "$DIST_DIR/lib/"
    log "  $lib ($(du -h "$src" | cut -f1))"
done

log "Bundling espeak-ng dylib..."
cp "$ESPEAK_DYLIB" "$DIST_DIR/lib/libespeak-ng.1.dylib"
if [[ -n "$PCAUDIO_DYLIB" ]]; then
    cp "$PCAUDIO_DYLIB" "$DIST_DIR/lib/libpcaudio.0.dylib"
fi
log "  libespeak-ng.1.dylib ($(du -h "$ESPEAK_DYLIB" | cut -f1))"

log "Copying espeak-ng-data..."
cp -R "$ESPEAK_DATA"/* "$DIST_DIR/espeak-ng-data/"
log "  espeak-ng-data ($(du -sh "$DIST_DIR/espeak-ng-data" | cut -f1))"

log "Fixing rpaths and install names..."

fix_kokoro() {
    local binary="$DIST_DIR/bin/kokoro-service"
    [[ -f "$binary" ]] || return 0

    install_name_tool -add_rpath "@executable_path/../lib" "$binary" 2>/dev/null || true

    local espeak_old
    espeak_old=$(otool -L "$binary" | grep "libespeak-ng" | awk '{print $1}')
    if [[ -n "$espeak_old" && "$espeak_old" != "@rpath/"* ]]; then
        install_name_tool -change "$espeak_old" "@rpath/libespeak-ng.1.dylib" "$binary"
    fi
}

fix_dylib_ids() {
    for lib in "$DIST_DIR/lib/"*.dylib; do
        local basename
        basename=$(basename "$lib")
        install_name_tool -id "@rpath/$basename" "$lib" 2>/dev/null || true
    done

    local libomp="$DIST_DIR/lib/libomp.dylib"
    if [[ -f "$libomp" ]]; then
        install_name_tool -id "@rpath/libomp.dylib" "$libomp" 2>/dev/null || true
    fi

    local libtorch_cpu="$DIST_DIR/lib/libtorch_cpu.dylib"
    if [[ -f "$libtorch_cpu" ]]; then
        local old_omp
        old_omp=$(otool -L "$libtorch_cpu" | grep "libomp" | awk '{print $1}')
        if [[ -n "$old_omp" && "$old_omp" != "@rpath/"* ]]; then
            install_name_tool -change "$old_omp" "@rpath/libomp.dylib" "$libtorch_cpu"
        fi
    fi

    local espeak="$DIST_DIR/lib/libespeak-ng.1.dylib"
    if [[ -f "$espeak" ]]; then
        install_name_tool -id "@rpath/libespeak-ng.1.dylib" "$espeak" 2>/dev/null || true
        if [[ -f "$DIST_DIR/lib/libpcaudio.0.dylib" ]]; then
            local old_pcaudio
            old_pcaudio=$(otool -L "$espeak" | grep "libpcaudio" | awk '{print $1}')
            if [[ -n "$old_pcaudio" && "$old_pcaudio" != "@rpath/"* ]]; then
                install_name_tool -change "$old_pcaudio" "@rpath/libpcaudio.0.dylib" "$espeak"
            fi
            install_name_tool -id "@rpath/libpcaudio.0.dylib" "$DIST_DIR/lib/libpcaudio.0.dylib" 2>/dev/null || true
        fi
    fi
}

fix_kokoro
fix_dylib_ids

log "Re-signing binaries and libraries (macOS code signature)..."
for f in "$DIST_DIR/bin/"* "$DIST_DIR/lib/"*.dylib; do
    [[ -f "$f" ]] && codesign -s - --force "$f" 2>/dev/null || true
done

log "Copying models..."
MODELS_SRC="$ROOT_DIR/bin/models"
if [[ -f "$MODELS_SRC/ggml-large-v3-q5_0.bin" ]]; then
    cp "$MODELS_SRC/ggml-large-v3-q5_0.bin" "$DIST_DIR/models/"
    log "  Whisper model ($(du -h "$MODELS_SRC/ggml-large-v3-q5_0.bin" | cut -f1))"
fi
if [[ -f "$MODELS_SRC/Llama-3.2-1B-Instruct-Q8_0.gguf" ]]; then
    cp "$MODELS_SRC/Llama-3.2-1B-Instruct-Q8_0.gguf" "$DIST_DIR/models/"
    log "  LLaMA model ($(du -h "$MODELS_SRC/Llama-3.2-1B-Instruct-Q8_0.gguf" | cut -f1))"
fi
KOKORO_SRC="$MODELS_SRC/kokoro-german"
if [[ -d "$KOKORO_SRC" ]]; then
    mkdir -p "$DIST_DIR/models/kokoro-german/coreml"
    mkdir -p "$DIST_DIR/models/kokoro-german/decoder_variants"
    for f in "$KOKORO_SRC"/*_voice.bin "$KOKORO_SRC"/vocab.json; do
        [[ -f "$f" ]] && cp "$f" "$DIST_DIR/models/kokoro-german/"
    done
    if [[ -d "$KOKORO_SRC/coreml/kokoro_duration.mlmodelc" ]]; then
        cp -R "$KOKORO_SRC/coreml/kokoro_duration.mlmodelc" "$DIST_DIR/models/kokoro-german/coreml/"
    fi
    if [[ -f "$KOKORO_SRC/coreml/coreml_config.json" ]]; then
        cp "$KOKORO_SRC/coreml/coreml_config.json" "$DIST_DIR/models/kokoro-german/coreml/"
    fi
    for f in "$KOKORO_SRC/decoder_variants"/kokoro_decoder_split_*.mlmodelc; do
        [[ -d "$f" ]] && cp -R "$f" "$DIST_DIR/models/kokoro-german/decoder_variants/"
    done
    for f in "$KOKORO_SRC/decoder_variants"/kokoro_har_*.pt; do
        [[ -f "$f" ]] && cp "$f" "$DIST_DIR/models/kokoro-german/decoder_variants/"
    done
    if [[ -f "$KOKORO_SRC/decoder_variants/split_config.json" ]]; then
        cp "$KOKORO_SRC/decoder_variants/split_config.json" "$DIST_DIR/models/kokoro-german/decoder_variants/"
    fi
    log "  Kokoro models ($(du -sh "$DIST_DIR/models/kokoro-german" | cut -f1))"
fi

NEUTTS_SRC="$MODELS_SRC/neutts-nano-german"
if [[ -d "$NEUTTS_SRC" ]]; then
    mkdir -p "$DIST_DIR/models/neutts-nano-german"
    for f in "$NEUTTS_SRC"/neutts-nano-german-Q8_0.gguf "$NEUTTS_SRC"/ref_codes.bin "$NEUTTS_SRC"/ref_text.txt; do
        [[ -f "$f" ]] && cp "$f" "$DIST_DIR/models/neutts-nano-german/"
    done
    if [[ -d "$NEUTTS_SRC/neucodec_decoder.mlmodelc" ]]; then
        cp -R "$NEUTTS_SRC/neucodec_decoder.mlmodelc" "$DIST_DIR/models/neutts-nano-german/"
    fi
    log "  NeuTTS models ($(du -sh "$DIST_DIR/models/neutts-nano-german" | cut -f1))"
fi

if [[ -d "$ROOT_DIR/bin/tls" ]]; then
    mkdir -p "$DIST_DIR/tls"
    cp -R "$ROOT_DIR/bin/tls/"* "$DIST_DIR/tls/" 2>/dev/null || true
    log "  TLS certificates copied"
fi

cat > "$DIST_DIR/run.sh" << 'RUNEOF'
#!/usr/bin/env bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export DYLD_LIBRARY_PATH="$DIR/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
export ESPEAK_NG_DATA="$DIR/espeak-ng-data"
export WHISPERTALK_MODELS_DIR="$DIR/models"

if [[ $# -eq 0 ]]; then
    echo "Prodigy Distribution"
    echo "Usage: $0 <service> [args...]"
    echo "Services: sip-client, inbound-audio-processor, vad-service, outbound-audio-processor,"
    echo "          whisper-service, llama-service, kokoro-service, neutts-service,"
    echo "          frontend, tomedo-crawl"
    exit 0
fi

SERVICE="$1"; shift
exec "$DIR/bin/$SERVICE" "$@"
RUNEOF
chmod +x "$DIST_DIR/run.sh"

log ""
log "=== Verification ==="

log "Checking binary dependencies..."
ERRORS=0
for svc in "${SERVICES[@]}"; do
    binary="$DIST_DIR/bin/$svc"
    [[ -f "$binary" ]] || continue
    bad=$(otool -L "$binary" | grep -v "^$binary" | grep -v "/usr/lib/" | grep -v "/System/" | grep -v "@rpath/" | grep -v "^	$" || true)
    if [[ -n "$bad" ]]; then
        log "  WARNING: $svc has non-portable dependencies:"
        echo "$bad" | while read -r line; do log "    $line"; done
        ERRORS=$((ERRORS + 1))
    else
        log "  OK: $svc"
    fi
done

log "Checking bundled dylib dependencies..."
for lib in "$DIST_DIR/lib/"*.dylib; do
    basename=$(basename "$lib")
    bad=$(otool -L "$lib" | grep -v "^$lib" | grep -v "/usr/lib/" | grep -v "/System/" | grep -v "@rpath/" | grep -v "^	$" || true)
    if [[ -n "$bad" ]]; then
        log "  WARNING: $basename has non-portable references:"
        echo "$bad" | while read -r line; do log "    $line"; done
        ERRORS=$((ERRORS + 1))
    else
        log "  OK: $basename"
    fi
done

TOTAL_SIZE=$(du -sh "$DIST_DIR" | cut -f1)
log ""
log "=== Distribution Summary ==="
log "Location: $DIST_DIR"
log "Total size: $TOTAL_SIZE"
log "Binaries: $(ls "$DIST_DIR/bin/" | wc -l | tr -d ' ')"
log "Libraries: $(ls "$DIST_DIR/lib/"*.dylib 2>/dev/null | wc -l | tr -d ' ')"
log ""
ls -lh "$DIST_DIR/bin/" | tail -n +2
log ""
ls -lh "$DIST_DIR/lib/" | tail -n +2
log ""

if [[ $ERRORS -gt 0 ]]; then
    log "WARNING: $ERRORS non-portable dependency issue(s) found"
    exit 1
fi

log "Distribution is self-contained!"
