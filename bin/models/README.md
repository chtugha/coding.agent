# Prodigy Models

Models are too large for git. Download them manually to this directory.

## Whisper (ASR) - ~1.0 GB

```bash
# Option A: Download pre-quantized model
wget https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-large-v3-q5_0.bin \
  -O bin/models/ggml-large-v3-q5_0.bin

# Option B: Download full model and quantize
cd /tmp && git clone https://github.com/ggerganov/whisper.cpp whisper-cpp-full
cd whisper-cpp-full && bash models/download-ggml-model.sh large-v3
./build/bin/whisper-quantize models/ggml-large-v3.bin models/ggml-large-v3-q5_0.bin q5_0
cp models/ggml-large-v3-q5_0.bin <project-root>/bin/models/
```

## Whisper CoreML Encoder (macOS only)

Requires Xcode command line tools and Python environment with coremltools.

```bash
# Set up Python environment
conda create -n whisper-coreml python=3.12
conda activate whisper-coreml
pip install ane_transformers coremltools openai-whisper torch

# Generate .mlpackage from whisper model
cd /tmp/whisper-cpp-full
python3 models/convert-whisper-to-coreml.py --model large-v3 --encoder-only True --optimize-ane True

# Compile to .mlmodelc
xcrun coremlc compile models/coreml-encoder-large-v3.mlpackage models/
mv models/coreml-encoder-large-v3.mlmodelc models/ggml-large-v3-encoder.mlmodelc
cp -r models/ggml-large-v3-encoder.mlmodelc <project-root>/bin/models/
```

The CoreML model must be named `ggml-large-v3-encoder.mlmodelc` and placed in the same
directory as the whisper model. whisper.cpp auto-discovers it at runtime.

First run on a new device triggers ANE compilation (can take 1-2 minutes).

## LLaMA (LLM) - ~1.2 GB

```bash
# Via huggingface-cli
huggingface-cli download bartowski/Llama-3.2-1B-Instruct-GGUF \
  Llama-3.2-1B-Instruct-Q8_0.gguf --local-dir bin/models/

# Or via wget
wget https://huggingface.co/bartowski/Llama-3.2-1B-Instruct-GGUF/resolve/main/Llama-3.2-1B-Instruct-Q8_0.gguf \
  -O bin/models/Llama-3.2-1B-Instruct-Q8_0.gguf
```

## Kokoro TTS (Phase 3)

Kokoro model files will be added when the C++ port is implemented.

## Expected Directory Contents

```
bin/models/
  .gitkeep
  ggml-large-v3-q5_0.bin           (~1.0 GB)
  ggml-large-v3-encoder.mlmodelc/  (CoreML encoder)
  Llama-3.2-1B-Instruct-Q8_0.gguf (~1.2 GB)
```

## SHA256 Checksums

```
ggml-large-v3-q5_0.bin:            (run: shasum -a 256 bin/models/ggml-large-v3-q5_0.bin)
Llama-3.2-1B-Instruct-Q8_0.gguf:  (run: shasum -a 256 bin/models/Llama-3.2-1B-Instruct-Q8_0.gguf)
```
