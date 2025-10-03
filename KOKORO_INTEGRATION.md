# Kokoro TTS Service Integration

## Overview

This document describes the integration of Kokoro TTS as a hybrid Python-based service to replace the current Piper ONNX implementation. Kokoro provides 10-100x faster synthesis with better audio quality using PyTorch with Metal Performance Shaders (MPS) acceleration on Apple Silicon.

## Architecture

### Service Design
- **Language**: Python 3.11 with PyTorch
- **Model**: Kokoro-82M (82 million parameters, Apache license)
- **Acceleration**: Metal Performance Shaders (MPS) for Apple Silicon
- **Audio Quality**: 24kHz, float32 PCM
- **Protocol**: TCP for audio streaming, UDP for registration (same as Piper)

### Key Components

1. **kokoro_service.py** - Main service implementation
   - TCP server on port 8090 (configurable) for LLaMA connections
   - UDP listener on port 13001 (configurable) for audio processor registrations
   - Sessionless design using numeric call_id for routing
   - Multi-threaded to handle concurrent calls

2. **start-kokoro.sh** - Service launcher script
   - Activates Python virtual environment
   - Configurable via environment variables
   - Proper error handling and validation

3. **Database Integration** - Service management
   - `kokoro_service_enabled` - Enable/disable toggle
   - `kokoro_voice` - Voice selection (af_sky, af_bella, etc.)
   - `kokoro_service_status` - Service status tracking

## Installation

### Prerequisites
```bash
# Python 3.11 (required - Kokoro doesn't support 3.13)
brew install python@3.11

# espeak-ng for phonemization
brew install espeak-ng
```

### Virtual Environment Setup
```bash
# Create virtual environment
python3.11 -m venv venv-kokoro

# Activate and install dependencies
./venv-kokoro/bin/pip install --upgrade pip
./venv-kokoro/bin/pip install torch kokoro soundfile
```

### Verification
```bash
# Test PyTorch MPS support
./venv-kokoro/bin/python3 -c "import torch; print('MPS available:', torch.backends.mps.is_available())"

# Test Kokoro installation
./venv-kokoro/bin/python3 -c "from kokoro import KPipeline; print('Kokoro installed')"
```

## Usage

### Starting the Service

**Method 1: Direct Launch**
```bash
./venv-kokoro/bin/python3 kokoro_service.py \
    --voice af_sky \
    --tcp-port 8090 \
    --udp-port 13001 \
    --device mps
```

**Method 2: Using Launcher Script**
```bash
# With defaults
./start-kokoro.sh

# With custom configuration
KOKORO_VOICE=af_bella KOKORO_TCP_PORT=8091 ./start-kokoro.sh
```

### Available Voices
- `af_sky` - American Female (default)
- `af_bella` - American Female
- `af_sarah` - American Female
- `am_adam` - American Male
- `am_michael` - American Male
- `bf_emma` - British Female
- `bf_isabella` - British Female
- `bm_george` - British Male
- `bm_lewis` - British Male

### Configuration Options

**Environment Variables:**
- `KOKORO_VOICE` - Voice to use (default: af_sky)
- `KOKORO_TCP_PORT` - TCP port for LLaMA connections (default: 8090)
- `KOKORO_UDP_PORT` - UDP port for registrations (default: 13001)
- `KOKORO_DEVICE` - PyTorch device: mps, cuda, or cpu (default: mps)

## Protocol

### TCP Protocol (LLaMA → Kokoro)

**1. HELLO Message**
```
[4 bytes: length] [N bytes: call_id as UTF-8]
```

**2. Text Chunks**
```
[4 bytes: length] [N bytes: text as UTF-8]
```

**3. Audio Response**
```
[4 bytes: audio_length] [4 bytes: sample_rate] [4 bytes: chunk_id] [N bytes: float32 audio]
```

**4. BYE Message**
```
[4 bytes: 0xFFFFFFFF]
```

### UDP Protocol (Audio Processor → Kokoro)

**Registration:**
```
REGISTER:<call_id>
```

**Deregistration:**
```
BYE:<call_id>
```

## Performance

### Synthesis Speed
- **Piper (ONNX)**: ~5-10 seconds for typical sentence
- **Kokoro (PyTorch MPS)**: ~0.3-0.5 seconds for typical sentence
- **Speedup**: 10-20x faster

### Audio Quality
- **Sample Rate**: 24kHz (vs Piper's 22.05kHz)
- **Model Size**: 82M parameters (vs Piper's ~20M)
- **Quality**: Significantly more natural and expressive

### Resource Usage
- **Memory**: ~500MB (model + PyTorch runtime)
- **GPU**: Utilizes Apple Silicon Neural Engine via MPS
- **CPU**: Minimal when using MPS acceleration

## Database Schema

### New Configuration Keys

```sql
INSERT OR IGNORE INTO system_config (key, value) VALUES 
    ('kokoro_service_enabled', 'false'),
    ('kokoro_voice', 'af_sky'),
    ('kokoro_service_status', 'stopped');
```

### Database Methods

**C++ API:**
```cpp
// Enable/disable service
bool get_kokoro_service_enabled();
bool set_kokoro_service_enabled(bool enabled);

// Voice configuration
std::string get_kokoro_voice();
bool set_kokoro_voice(const std::string& voice);

// Status tracking
std::string get_kokoro_service_status();
bool set_kokoro_service_status(const std::string& status);
```

## Integration with Existing System

### Coexistence with Piper
- Both services can be installed simultaneously
- Only one should be active at a time
- Piper binaries and code remain unchanged
- Easy toggle between services via database configuration

### Port Assignments
- **TCP Port 8090**: LLaMA → TTS service connection
- **UDP Port 13001**: Audio processor registration
- Same ports as Piper for drop-in compatibility

### Call Flow
1. Outbound audio processor sends `REGISTER:<call_id>` to UDP 13001
2. LLaMA connects to TCP 8090, sends `HELLO` with call_id
3. LLaMA sends text chunks to synthesize
4. Kokoro synthesizes and streams audio back
5. LLaMA sends `BYE` when done
6. Audio processor sends `BYE:<call_id>` to UDP 13001

## Testing

### Unit Test
```bash
# Start service
./start-kokoro.sh &

# Run test
./venv-kokoro/bin/python3 test_kokoro_service.py

# Expected output:
# ✅ Connected!
# 👋 Sent HELLO with call_id=999
# 📝 Sent text: Hello, this is a test...
# 🔊 Received 400800 bytes of audio data
# ✅ Test completed successfully!
```

### Integration Test
1. Start HTTP server
2. Enable Kokoro service via API
3. Make a test call
4. Verify audio quality and speed

## Troubleshooting

### Service Won't Start
```bash
# Check Python version
python3.11 --version  # Should be 3.11.x

# Check virtual environment
ls -la venv-kokoro/bin/python3

# Check dependencies
./venv-kokoro/bin/pip list | grep -E "torch|kokoro"
```

### MPS Not Available
```bash
# Verify MPS support
./venv-kokoro/bin/python3 -c "import torch; print(torch.backends.mps.is_available())"

# If false, check macOS version (requires macOS 12.3+)
sw_vers

# Fall back to CPU
KOKORO_DEVICE=cpu ./start-kokoro.sh
```

### Audio Quality Issues
- Check sample rate conversion in audio processor
- Verify float32 format is preserved
- Check for buffer underruns in logs

## Future Enhancements

### Planned Features
1. **Voice Cloning**: Support for custom voice embeddings
2. **Streaming Synthesis**: Real-time audio generation
3. **Caching**: Cache frequently used phrases
4. **Multi-language**: Support for non-English languages
5. **Emotion Control**: Adjust speaking style and emotion

### Performance Optimizations
1. **Model Quantization**: Reduce memory footprint
2. **Batch Processing**: Handle multiple calls simultaneously
3. **Preloading**: Cache voice embeddings
4. **Connection Pooling**: Reuse TCP connections

## Files Modified/Created

### New Files
- `kokoro_service.py` - Main service implementation
- `start-kokoro.sh` - Service launcher script
- `test_kokoro_service.py` - Unit test script
- `KOKORO_INTEGRATION.md` - This documentation
- `venv-kokoro/` - Python virtual environment

### Modified Files
- `database.h` - Added Kokoro service management methods
- `database.cpp` - Implemented Kokoro database methods
- `http-server` - Recompiled with updated database

### Unchanged Files (Piper Preserved)
- `piper-service.cpp` - Piper implementation preserved
- `piper-service.h` - Piper headers preserved
- All Piper binaries and models remain intact

## License

- **Kokoro Model**: Apache 2.0 License
- **PyTorch**: BSD License
- **Integration Code**: Same as project license

## References

- Kokoro GitHub: https://github.com/hexgrad/kokoro
- Kokoro CoreML: https://github.com/mattmireles/kokoro-coreml
- PyTorch MPS: https://pytorch.org/docs/stable/notes/mps.html

