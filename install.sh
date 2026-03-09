#!/bin/bash

echo ""
echo "=== DEPRECATED ==="
echo "install.sh has been replaced by two scripts:"
echo "  1. ./runmetoinstalleverythingfirst  (install prerequisites + download models)"
echo "  2. ./runmetobuildeverything          (build all binaries)"
echo ""
echo "Please use those scripts instead."
echo "See .zencoder/rules/build.md for build configuration reference."
echo "=================="
echo ""

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
exec "$SCRIPT_DIR/runmetoinstalleverythingfirst"

set -e

SCRIPT_DIR="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"
cd "$SCRIPT_DIR"

echo "==============================================="
echo "WhisperTalk Installation Script"
echo "==============================================="
echo ""

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info() { echo -e "${GREEN}[INFO]${NC} $1"; }
warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
error() { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }

check_macos() {
    if [[ "$(uname -s)" != "Darwin" ]]; then
        error "This script is designed for macOS only (Apple Silicon required)"
    fi
    
    if [[ "$(uname -m)" != "arm64" ]]; then
        error "Apple Silicon (arm64) required. Found: $(uname -m)"
    fi
    
    info "✓ Running on macOS $(sw_vers -productVersion) (Apple Silicon)"
}

check_and_install_homebrew() {
    info "Checking Homebrew..."
    
    if ! command -v brew &> /dev/null; then
        warn "Homebrew not found. Installing..."
        /bin/bash -c "$(curl -fsSL https://raw.githubusercontent.com/Homebrew/install/HEAD/install.sh)"
        
        eval "$(/opt/homebrew/bin/brew shellenv)"
        echo 'eval "$(/opt/homebrew/bin/brew shellenv)"' >> ~/.zshrc
        
        info "✓ Homebrew installed"
    else
        info "✓ Homebrew already installed: $(brew --version | head -1)"
    fi
}

install_system_deps() {
    info "Installing system dependencies via Homebrew..."
    
    DEPS=(
        "cmake"
        "wget"
        "git"
        "espeak-ng"
    )
    
    for dep in "${DEPS[@]}"; do
        if ! brew list "$dep" &> /dev/null; then
            info "  Installing $dep..."
            brew install "$dep"
        else
            info "  ✓ $dep already installed"
        fi
    done
}

check_and_install_miniconda() {
    info "Checking Miniconda..."
    
    if ! command -v conda &> /dev/null; then
        warn "Miniconda not found. Installing..."
        
        MINICONDA_URL="https://repo.anaconda.com/miniconda/Miniconda3-latest-MacOSX-arm64.sh"
        curl -fsSL -o /tmp/miniconda.sh "$MINICONDA_URL"
        bash /tmp/miniconda.sh -b -p "$HOME/miniconda3"
        rm /tmp/miniconda.sh
        
        eval "$($HOME/miniconda3/bin/conda shell.zsh hook)"
        echo 'eval "$($HOME/miniconda3/bin/conda shell.zsh hook)"' >> ~/.zshrc
        
        info "✓ Miniconda installed"
    else
        info "✓ Miniconda already installed: $(conda --version)"
    fi
    
    eval "$(conda shell.zsh hook)" 2>/dev/null || true
}

setup_python_env() {
    info "Setting up Python environment..."
    
    if ! conda env list | grep -q "^whispertalk "; then
        info "  Creating whispertalk conda environment..."
        conda create -n whispertalk python=3.12 -y
    else
        info "  ✓ whispertalk environment already exists"
    fi
    
    eval "$(conda shell.zsh hook)"
    conda activate whispertalk
    
    info "  Installing Python dependencies..."
    pip install --quiet --upgrade pip
    pip install --quiet torch torchvision torchaudio
    pip install --quiet coremltools ane_transformers
    pip install --quiet openai-whisper
    pip install --quiet huggingface-hub
    
    info "✓ Python environment ready"
}

clone_and_build_whisper_cpp() {
    info "Building whisper.cpp..."
    
    if [ ! -d "whisper-cpp" ]; then
        info "  Cloning whisper.cpp..."
        git clone https://github.com/ggerganov/whisper.cpp.git whisper-cpp
        cd whisper-cpp
        git checkout b8022
        cd ..
    else
        info "  ✓ whisper.cpp already cloned"
    fi
    
    cd whisper-cpp
    
    if [ ! -d "build" ]; then
        mkdir build
    fi
    
    cd build
    info "  Configuring whisper.cpp with CoreML and Metal..."
    cmake .. \
        -DBUILD_SHARED_LIBS=OFF \
        -DWHISPER_COREML=ON \
        -DWHISPER_METAL=ON \
        -DCMAKE_BUILD_TYPE=Release
    
    info "  Building whisper.cpp (this may take a few minutes)..."
    make -j$(sysctl -n hw.ncpu)
    
    cd ../..
    info "✓ whisper.cpp built successfully"
}

clone_and_build_llama_cpp() {
    info "Building llama.cpp..."
    
    if [ ! -d "llama-cpp" ]; then
        info "  Cloning llama.cpp..."
        git clone https://github.com/ggerganov/llama.cpp.git llama-cpp
        cd llama-cpp
        git checkout b8022
        cd ..
    else
        info "  ✓ llama.cpp already cloned"
    fi
    
    cd llama-cpp
    
    if [ ! -d "build" ]; then
        mkdir build
    fi
    
    cd build
    info "  Configuring llama.cpp with Metal..."
    cmake .. \
        -DBUILD_SHARED_LIBS=OFF \
        -DLLAMA_METAL=ON \
        -DCMAKE_BUILD_TYPE=Release
    
    info "  Building llama.cpp (this may take a few minutes)..."
    make -j$(sysctl -n hw.ncpu)
    
    cd ../..
    info "✓ llama.cpp built successfully"
}

build_whispertalk_services() {
    info "Building WhisperTalk services..."
    
    if [ ! -d "build" ]; then
        mkdir build
    fi
    
    cd build
    
    info "  Configuring CMake..."
    cmake .. -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON
    
    info "  Building all services (this may take a few minutes)..."
    make -j$(sysctl -n hw.ncpu)
    
    cd ..
    
    info "✓ WhisperTalk services built:"
    ls -lh bin/sip-client bin/inbound-audio-processor bin/outbound-audio-processor \
        bin/whisper-service bin/llama-service bin/kokoro-service bin/frontend 2>/dev/null | \
        awk '{print "    " $9, "(" $5 ")"}'
}

download_whisper_model() {
    info "Downloading Whisper model..."
    
    if [ -f "bin/models/ggml-large-v3-q5_0.bin" ]; then
        info "  ✓ Whisper model already exists"
        return
    fi
    
    info "  Downloading ggml-large-v3-q5_0.bin (~1.0 GB)..."
    wget -q --show-progress \
        https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-q5_0.bin \
        -O bin/models/ggml-large-v3-q5_0.bin
    
    info "✓ Whisper model downloaded"
}

generate_whisper_coreml() {
    info "Generating Whisper CoreML encoder..."
    
    if [ -d "bin/models/ggml-large-v3-encoder.mlmodelc" ]; then
        info "  ✓ CoreML encoder already exists"
        return
    fi
    
    eval "$(conda shell.zsh hook)"
    conda activate whispertalk
    
    cd /tmp
    
    if [ ! -d "whisper-cpp-full" ]; then
        info "  Cloning whisper.cpp for model conversion..."
        git clone https://github.com/ggerganov/whisper.cpp whisper-cpp-full
    fi
    
    cd whisper-cpp-full
    
    if [ ! -f "models/ggml-large-v3.bin" ]; then
        info "  Downloading full Whisper model for conversion..."
        bash models/download-ggml-model.sh large-v3
    fi
    
    info "  Converting to CoreML (this may take 5-10 minutes)..."
    python3 models/convert-whisper-to-coreml.py \
        --model large-v3 \
        --encoder-only True \
        --optimize-ane True
    
    info "  Compiling CoreML model..."
    xcrun coremlc compile models/coreml-encoder-large-v3.mlpackage models/
    mv models/coreml-encoder-large-v3.mlmodelc models/ggml-large-v3-encoder.mlmodelc
    
    cp -r models/ggml-large-v3-encoder.mlmodelc "$SCRIPT_DIR/bin/models/"
    
    cd "$SCRIPT_DIR"
    info "✓ CoreML encoder generated"
}

download_llama_model() {
    info "Downloading LLaMA model..."
    
    if [ -f "bin/models/Llama-3.2-1B-Instruct-Q8_0.gguf" ]; then
        info "  ✓ LLaMA model already exists"
        return
    fi
    
    info "  Downloading Llama-3.2-1B-Instruct-Q8_0.gguf (~1.2 GB)..."
    
    eval "$(conda shell.zsh hook)"
    conda activate whispertalk
    
    if command -v huggingface-cli &> /dev/null; then
        huggingface-cli download bartowski/Llama-3.2-1B-Instruct-GGUF \
            Llama-3.2-1B-Instruct-Q8_0.gguf \
            --local-dir bin/models/ \
            --local-dir-use-symlinks False
    else
        wget -q --show-progress \
            https://huggingface.co/bartowski/Llama-3.2-1B-Instruct-GGUF/resolve/main/Llama-3.2-1B-Instruct-Q8_0.gguf \
            -O bin/models/Llama-3.2-1B-Instruct-Q8_0.gguf
    fi
    
    info "✓ LLaMA model downloaded"
}

export_kokoro_models() {
    info "Exporting Kokoro TTS models..."
    
    if [ -d "bin/models/coreml" ] && [ -f "bin/models/coreml/kokoro_duration.mlmodelc" ]; then
        info "  ✓ Kokoro models already exist"
        return
    fi
    
    eval "$(conda shell.zsh hook)"
    conda activate whispertalk
    
    info "  Running export script (this may take 10-15 minutes)..."
    python3 scripts/export_kokoro_models.py
    
    info "✓ Kokoro models exported"
}

verify_installation() {
    info "Verifying installation..."
    
    SERVICES=(
        "bin/sip-client"
        "bin/inbound-audio-processor"
        "bin/outbound-audio-processor"
        "bin/whisper-service"
        "bin/llama-service"
        "bin/kokoro-service"
        "bin/frontend"
    )
    
    MODELS=(
        "bin/models/ggml-large-v3-q5_0.bin"
        "bin/models/ggml-large-v3-encoder.mlmodelc"
        "bin/models/Llama-3.2-1B-Instruct-Q8_0.gguf"
    )
    
    KOKORO_MODELS=(
        "bin/models/coreml/kokoro_duration.mlmodelc"
        "bin/models/decoder_variants"
        "bin/models/voice_packs"
    )
    
    ALL_OK=true
    
    for service in "${SERVICES[@]}"; do
        if [ -f "$service" ]; then
            info "  ✓ $service"
        else
            warn "  ✗ $service NOT FOUND"
            ALL_OK=false
        fi
    done
    
    for model in "${MODELS[@]}"; do
        if [ -e "$model" ]; then
            info "  ✓ $model"
        else
            warn "  ✗ $model NOT FOUND"
            ALL_OK=false
        fi
    done
    
    for model in "${KOKORO_MODELS[@]}"; do
        if [ -e "$model" ]; then
            info "  ✓ $model"
        else
            warn "  ✗ $model NOT FOUND"
            ALL_OK=false
        fi
    done
    
    if $ALL_OK; then
        echo ""
        echo -e "${GREEN}===============================================${NC}"
        echo -e "${GREEN}Installation Complete!${NC}"
        echo -e "${GREEN}===============================================${NC}"
        echo ""
        info "Start the services with:"
        echo "  ./bin/sip-client --lines 2"
        echo "  ./bin/inbound-audio-processor"
        echo "  ./bin/whisper-service"
        echo "  ./bin/llama-service"
        echo "  ./bin/kokoro-service"
        echo "  ./bin/outbound-audio-processor"
        echo "  ./bin/frontend --port 8080"
        echo ""
        info "Or use the test suite:"
        echo "  cd build && ctest --output-on-failure"
        echo ""
    else
        warn "Some components are missing. Check the warnings above."
        return 1
    fi
}

main() {
    check_macos
    check_and_install_homebrew
    install_system_deps
    check_and_install_miniconda
    setup_python_env
    
    clone_and_build_whisper_cpp
    clone_and_build_llama_cpp
    build_whispertalk_services
    
    mkdir -p bin/models
    download_whisper_model
    generate_whisper_coreml
    download_llama_model
    export_kokoro_models
    
    verify_installation
}

if [ "${1}" == "--help" ] || [ "${1}" == "-h" ]; then
    echo "WhisperTalk Installation Script"
    echo ""
    echo "Usage: ./install.sh [OPTIONS]"
    echo ""
    echo "This script will:"
    echo "  1. Install Homebrew (if not present)"
    echo "  2. Install system dependencies (cmake, wget, git, espeak-ng)"
    echo "  3. Install Miniconda (if not present)"
    echo "  4. Create Python environment with PyTorch, CoreML tools"
    echo "  5. Clone and build whisper.cpp and llama.cpp"
    echo "  6. Build all WhisperTalk services"
    echo "  7. Download Whisper and LLaMA models (~2.2 GB)"
    echo "  8. Generate CoreML models for Whisper encoder"
    echo "  9. Export Kokoro TTS models (~300 MB)"
    echo ""
    echo "Requirements:"
    echo "  - macOS Sonoma or later"
    echo "  - Apple Silicon (M1/M2/M3/M4)"
    echo "  - ~10 GB free disk space"
    echo "  - Internet connection"
    echo ""
    echo "Options:"
    echo "  -h, --help    Show this help message"
    echo ""
    exit 0
fi

main
