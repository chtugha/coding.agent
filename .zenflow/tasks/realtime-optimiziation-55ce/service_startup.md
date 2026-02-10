# WhisperTalk Service Startup Guide

**Date**: 2026-02-10  
**Phase**: Phase 0 - Baseline Measurement  
**Status**: Documentation Complete

---

## Overview

WhisperTalk consists of 6 core services that must be started in a specific order to establish the full pipeline. This document provides startup commands, verification methods, and troubleshooting guidance.

---

## Prerequisites

### 1. System Requirements
- **OS**: macOS (Apple Silicon preferred for CoreML acceleration)
- **File Descriptor Limit**: ≥4096 (for Whisper's port range)
  ```bash
  ulimit -n 4096  # Set before starting services
  ```

### 2. Dependencies
All dependencies must be built (see [dependencies.md](./dependencies.md)):
- ✅ whisper-cpp (CoreML support)
- ✅ llama-cpp (Metal/MPS support)
- ✅ All 6 services compiled in `bin/` directory

### 2.1. Library Setup (CRITICAL)

After building, shared libraries must be accessible to binaries. Create symlinks in `bin/`:

```bash
cd bin
ln -sf ../whisper-cpp/build/src/libwhisper.1.dylib .
ln -sf ../whisper-cpp/build/src/libwhisper.coreml.dylib .
ln -sf ../llama-cpp/build/bin/libllama.0.dylib .
```

**Why**: Binaries use `@rpath` to locate libraries in the same directory. Without symlinks, services will fail with:
```
dyld: Library not loaded: @rpath/libwhisper.1.dylib
```

**Status**: ✅ Already configured (created during Phase 0 verification)

### 3. Required Models

**CRITICAL**: None of the required ML models are currently available in the repository. All three model sets must be downloaded/created before services can start.

#### Whisper (ASR)
- **Path**: `models/whisper/`
- **Required Files**: CoreML model files (`.mlmodelc` bundle or `.coreml`)
- **Recommended Models**:
  - `ggml-base.en.bin` + CoreML encoder (English)
  - `ggml-base.bin` + CoreML encoder (Multilingual, supports German)
- **Download**: 
  ```bash
  # Option 1: Download from whisper.cpp models
  cd models/whisper
  curl -L -O https://huggingface.co/ggml-org/whisper.cpp/resolve/main/ggml-base.bin
  
  # Option 2: Convert to CoreML (requires whisper.cpp tools)
  # See: https://github.com/ggerganov/whisper.cpp#core-ml-support
  ```
- **Size**: ~140MB (base model)
- **Status**: ❌ **NOT PRESENT** - Must be downloaded

#### LLaMA (LLM)
- **Path**: `models/llama/Llama-3.2-1B-Instruct-Q8_0.gguf`
- **Required Format**: GGUF (quantized, Q8_0 recommended)
- **Download**:
  ```bash
  mkdir -p models/llama
  cd models/llama
  # Download from Hugging Face or llama.cpp repository
  curl -L -O https://huggingface.co/lmstudio-community/Llama-3.2-1B-Instruct-GGUF/resolve/main/Llama-3.2-1B-Instruct-Q8_0.gguf
  ```
- **Size**: ~1.3GB (Q8_0 quantization)
- **Status**: ❌ **NOT PRESENT** - Must be downloaded

#### Kokoro (TTS)
- **Path**: `models/kokoro-german/`
- **Required Files**:
  - `kokoro-german-v1_1-de.pth` (PyTorch model weights)
  - `config.json` (model configuration)
  - `voices/*.pt` (optional: pre-loaded voice tensors)
- **Download**:
  ```bash
  mkdir -p models/kokoro-german/voices
  cd models/kokoro-german
  # Download from kokoro-tts repository
  # See: https://github.com/thewh1teagle/kokoro-onnx
  ```
- **Fallback**: If German model unavailable, Kokoro will load English-only pipeline
- **Size**: ~500MB (German model + voices)
- **Status**: ❌ **NOT PRESENT** - Must be downloaded (German TTS will not work without this)

#### Python Dependencies for Kokoro
```bash
pip3 install torch kokoro-tts
```

---

## Service Startup Order

Services must be started in this specific order to ensure TCP connection establishment:

```
1. Outbound Audio Processor  (creates control socket)
2. Kokoro TTS Service         (TCP listener on 8090)
3. LLaMA Service              (TCP listener on 8083)
4. Whisper Service            (TCP listeners on 13000+)
5. Inbound Audio Processor    (UDP listener on 9001)
6. SIP Client                 (SIP/RTP gateway)
```

**Rationale**: Each service connects to the next downstream service. Starting them in reverse pipeline order ensures listeners are ready before connectors attempt to establish connections.

---

## Startup Commands

### 1. Outbound Audio Processor
```bash
cd /Users/ollama/.zenflow/worktrees/realtime-optimiziation-55ce
./bin/outbound-audio-processor
```

**Arguments**: None  
**Listeners**:
- TCP: Ports `8090 + (call_id % 100)` (for Kokoro audio streams)
- UDP: Port 9002 (for outbound RTP to SIP Client)
- Unix Socket: `/tmp/outbound-audio-processor.ctrl` (for activation signals)

**Verification**:
```bash
# Check process is running
pgrep -f outbound-audio-processor

# Check control socket created
ls -l /tmp/outbound-audio-processor.ctrl

# Expected output: Started, listening for signals
tail -f /tmp/outbound-audio-processor.log  # If logging enabled
```

**Expected Console Output**:
```
Outbound Audio Processor started
Listening on control socket: /tmp/outbound-audio-processor.ctrl
UDP sender ready on port 9002
```

---

### 2. Kokoro TTS Service
```bash
cd /Users/ollama/.zenflow/worktrees/realtime-optimiziation-55ce
python3 ./bin/kokoro_service.py
# Or if executable:
# ./bin/kokoro_service.py
```

**Arguments**: None (uses default ports, auto-detects MPS/Metal)  
**Listeners**:
- TCP: Port 8090 (for LLaMA text input)
- UDP: Port 13001 (for registration signals - currently unused)

**Verification**:
```bash
# Check process
pgrep -f kokoro_service

# Check TCP port listening
lsof -i TCP:8090

# Expected: Python process listening on 8090
```

**Expected Console Output**:
```
✅ Using Metal Performance Shaders (Apple Silicon)
🔄 Loading English pipeline...
✅ English pipeline loaded
🔄 Loading German pipeline (custom model)...
✅ German pipeline loaded
📥 Pre-loading German voice: <voice_name>
✅ German pipeline loaded
🎤 Kokoro TTS Service initialized
   Default Voice: df_eva
   Device: mps
   TCP Port: 8090
   UDP Port: 13001
```

**Possible Errors**:
- `❌ Kokoro not installed`: Run `pip3 install torch kokoro-tts`
- `⚠️ Using CPU (slower)`: MPS not available, will use CPU (expect high latency)
- `ℹ️ German model not found`: Missing `models/kokoro-german/`, only English TTS available

---

### 3. LLaMA Service
```bash
cd /Users/ollama/.zenflow/worktrees/realtime-optimiziation-55ce
./bin/llama-service models/llama/Llama-3.2-1B-Instruct-Q8_0.gguf
# Or use default path:
# ./bin/llama-service
```

**Arguments**: 
- `[model_path]` (optional): Path to GGUF model (defaults to `models/llama/Llama-3.2-1B-Instruct-Q8_0.gguf`)

**Listeners**:
- TCP: Port 8083 (for Whisper transcription input)

**Verification**:
```bash
# Check process
pgrep -f llama-service

# Check TCP port
lsof -i TCP:8083

# Check Metal backend loaded
# (Look for "Metal" in process output)
```

**Expected Console Output**:
```
Loading model from models/llama/Llama-3.2-1B-Instruct-Q8_0.gguf...
Model loaded successfully
Backend: Metal (Apple Silicon)
Context size: 2048 tokens
LLaMA Service listening on port 8083
```

**Possible Errors**:
- `Failed to load model: <path>`: Model file not found or corrupted
- `Metal backend unavailable`: Falling back to CPU (expect slower responses)

---

### 4. Whisper Service
```bash
cd /Users/ollama/.zenflow/worktrees/realtime-optimiziation-55ce
./bin/whisper-service models/whisper/ggml-base-encoder.coreml
```

**Arguments**: 
- `<model_path>` (required): Path to CoreML-converted Whisper model

**Listeners**:
- TCP: Ports `13000 + (call_id % 100)` (for Inbound Processor audio streams)
  - Supports up to 100 concurrent calls (13000-13099)

**Verification**:
```bash
# Check process
pgrep -f whisper-service

# Check if any listeners are up (initially none until first call)
lsof -i TCP:13000-13099 | grep whisper

# Check CoreML loaded
# (Look for CoreML initialization in output)
```

**Expected Console Output**:
```
Usage: whisper-service <model_path>
# After providing model:
Loading Whisper model from models/whisper/ggml-base-encoder.coreml...
CoreML backend: enabled
Model loaded successfully
Whisper Service ready
Listening for calls on ports 13000+
```

**Possible Errors**:
- `Usage: whisper-service <model_path>`: Missing model path argument
- `Failed to load model`: CoreML model not found or incompatible
- `CoreML not available`: macOS version too old or not on Apple Silicon

---

### 5. Inbound Audio Processor
```bash
cd /Users/ollama/.zenflow/worktrees/realtime-optimiziation-55ce
./bin/inbound-audio-processor
```

**Arguments**: None  
**Listeners**:
- UDP: Port 9001 (for RTP packets from SIP Client)

**Connections**:
- TCP: Ports `13000 + (call_id % 100)` → Whisper Service

**Verification**:
```bash
# Check process
pgrep -f inbound-audio-processor

# Check UDP port
lsof -i UDP:9001

# Expected: Listening on 9001
```

**Expected Console Output**:
```
Inbound Audio Processor started
Listening on UDP port 9001 for RTP packets
Whisper Service target: localhost:13000+
```

**Resilience Note**: If Whisper Service is not running, Inbound Processor will continue running but dump audio to `/dev/null` until Whisper reconnects.

---

### 6. SIP Client
```bash
cd /Users/ollama/.zenflow/worktrees/realtime-optimiziation-55ce
./bin/sip-client testuser@whisprtalk.local sip.server.com 5060
```

**Arguments**: 
- `<user>`: SIP username/URI (e.g., `user@domain.com`)
- `<server>`: SIP server hostname/IP
- `[port]`: SIP server port (default: 5060)

**Listeners**:
- SIP UDP: Dynamic port (randomly assigned by OS)
- RTP UDP: Dynamic ports per call
- Audio UDP: Port 9002 (receives audio from Outbound Processor)

**Connections**:
- UDP: Port 9001 → Inbound Audio Processor (sends RTP)
- Unix Socket: `/tmp/outbound-audio-processor.ctrl` (sends ACTIVATE signals)

**Verification**:
```bash
# Check process
pgrep -f sip-client

# Check SIP registration
# (Look for "REGISTER" and "200 OK" in output)

# Check if receiving audio
lsof -i UDP:9002 | grep sip-client
```

**Expected Console Output**:
```
SIP Client starting...
User: testuser@whisprtalk.local
Server: sip.server.com:5060
Local IP: 127.0.0.1
Local SIP port: <dynamic_port>
Sending REGISTER...
REGISTER successful (200 OK)
Waiting for incoming calls...
```

**Note**: The SIP Client will attempt to register with the SIP server. If no SIP server is available, it will fail to register but can still be tested with simulated RTP packets.

---

## Quick Start Script

Create a startup script for development:

```bash
#!/bin/bash
# start_services.sh

# Ensure we're in the project root
cd "$(dirname "$0")"

# Set file descriptor limit
ulimit -n 4096

# Verify libraries are symlinked
if [ ! -L "bin/libwhisper.1.dylib" ]; then
    echo "Creating library symlinks..."
    cd bin
    ln -sf ../whisper-cpp/build/src/libwhisper.1.dylib .
    ln -sf ../whisper-cpp/build/src/libwhisper.coreml.dylib .
    ln -sf ../llama-cpp/build/bin/libllama.0.dylib .
    cd ..
fi

# Start services in correct order (background mode)
echo "Starting Outbound Audio Processor..."
./bin/outbound-audio-processor &
sleep 1

echo "Starting Kokoro TTS Service..."
python3 ./bin/kokoro_service.py &
sleep 2

echo "Starting LLaMA Service..."
./bin/llama-service &
sleep 2

echo "Starting Whisper Service..."
./bin/whisper-service models/whisper/ggml-base-encoder.coreml &
sleep 2

echo "Starting Inbound Audio Processor..."
./bin/inbound-audio-processor &
sleep 1

# SIP Client (foreground, so we can see logs)
echo "Starting SIP Client..."
./bin/sip-client testuser@localhost localhost 5060
```

**Usage**:
```bash
chmod +x start_services.sh
./start_services.sh
```

**Note**: This script will fail if models are not present (see "Required Models" section).

---

## Stopping Services

### Graceful Shutdown
```bash
# Kill SIP Client (foreground process)
Ctrl+C

# Kill other services
pkill -f outbound-audio-processor
pkill -f kokoro_service
pkill -f llama-service
pkill -f whisper-service
pkill -f inbound-audio-processor
```

### Force Kill (if hung)
```bash
pkill -9 -f outbound-audio-processor
pkill -9 -f kokoro_service
pkill -9 -f llama-service
pkill -9 -f whisper-service
pkill -9 -f inbound-audio-processor
```

### Cleanup
```bash
# Remove control sockets
rm -f /tmp/outbound-audio-processor.ctrl
rm -f /tmp/inbound-audio-processor.ctrl  # If created
rm -f /tmp/whisper-service.ctrl          # If created
rm -f /tmp/llama-service.ctrl            # If created
rm -f /tmp/kokoro-service.ctrl           # If created
```

---

## Troubleshooting

### Services Won't Start

**Problem**: Service immediately exits  
**Solutions**:
- Check model files exist (see "Required Models" section)
- Verify build completed: `ls -lh bin/`
- Check dependencies: Review `dependencies.md`

**Problem**: "Address already in use"  
**Solutions**:
- Check if service already running: `pgrep -f <service_name>`
- Kill existing process: `pkill -f <service_name>`
- Check port usage: `lsof -i TCP:<port>` or `lsof -i UDP:<port>`

**Problem**: "Too many open files"  
**Solutions**:
- Increase file descriptor limit: `ulimit -n 4096`
- Check current limit: `ulimit -n`

### Connection Issues

**Problem**: Service can't connect to downstream partner  
**Solutions**:
- Verify startup order (see "Service Startup Order")
- Check downstream service is listening: `lsof -i TCP:<port>`
- Review logs for connection errors

**Problem**: SIP Client can't reach SIP server  
**Solutions**:
- Verify server address and port are correct
- Check network connectivity: `ping <server>`
- For local testing, use a local SIP server (e.g., Asterisk)

### Performance Issues

**Problem**: High latency or slow responses  
**Solutions**:
- Check CPU usage: `top -pid $(pgrep llama-service)`
- Verify Metal/MPS acceleration is enabled (check service startup logs)
- Check CoreML is loaded for Whisper (not CPU fallback)
- Monitor memory usage: `ps aux | grep -E 'whisper|llama|kokoro'`

**Problem**: Kokoro using CPU instead of MPS  
**Solutions**:
- Verify PyTorch MPS support: `python3 -c "import torch; print(torch.backends.mps.is_available())"`
- Reinstall PyTorch with MPS support: `pip3 install --upgrade torch`

---

## Testing Without SIP Server

For development and testing, you can simulate calls without a real SIP server:

### Option 1: Use Test Script
```bash
# Send simulated RTP packets to Inbound Processor
python3 tests/multi_call_test.py
```

This script sends 3 concurrent calls (call IDs: 1001, 1002, 1003) with silent audio for 10 seconds.

### Option 2: Manual TCP Testing
```bash
# Connect to Whisper Service directly
echo "Hello, this is a test" | nc localhost 8083

# Connect to LLaMA Service directly
echo "Wie geht es Ihnen?" | nc localhost 8083
```

---

## Verification Checklist

After starting all services, verify the pipeline is ready:

- [ ] All 6 processes running (`pgrep -f <service>`)
- [ ] File descriptor limit set (`ulimit -n` shows ≥4096)
- [ ] Models loaded (check console output for each service)
- [ ] Ports listening:
  - [ ] UDP 9001 (Inbound Processor)
  - [ ] UDP 9002 (SIP Client)
  - [ ] TCP 8083 (LLaMA)
  - [ ] TCP 8090 (Kokoro)
- [ ] Control sockets created:
  - [ ] `/tmp/outbound-audio-processor.ctrl`
- [ ] No error messages in console output
- [ ] SIP Client registered (if SIP server available)

---

## Known Limitations (Current System)

1. **No model files provided**: All three model sets (Whisper, LLaMA, Kokoro) must be downloaded manually
2. **No orchestration**: Services must be started manually in correct order
3. **No health checks**: No automatic verification that services are ready
4. **No automatic restart**: If a service crashes, it must be manually restarted
5. **Limited logging**: No centralized logging, console output only
6. **No metrics collection**: Performance data not captured (added in Phase 7)
7. **No call lifecycle signals**: CALL_START/CALL_END not implemented yet (Phase 1)

---

## Next Steps

- **Phase 0**: Use this guide to start services and measure baseline performance
- **Phase 1**: Implement call lifecycle signaling (CALL_START/CALL_END)
- **Phase 2**: Support multiple SIP Client instances
- **Future**: Create automated orchestration and health monitoring

---

**Document Version**: 1.0  
**Last Updated**: 2026-02-10  
**Verified**: Manual startup test (pending model availability)
