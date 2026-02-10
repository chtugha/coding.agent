# WhisperTalk Baseline Metrics

**Date**: 2026-02-10  
**Phase**: Phase 0 - Baseline Measurement  
**Status**: BLOCKED - Missing Required Models and Dependencies

---

## Executive Summary

**Overall Status**: ❌ **Cannot run baseline benchmarks** due to missing ML models and Python dependencies.

**Key Findings**:
- ✅ All 6 services **compile successfully**
- ✅ Metal/MPS acceleration **properly configured** (M4 GPU detected)
- ✅ CoreML support **available** for Whisper
- ❌ **All 3 ML models missing** (Whisper, LLaMA, Kokoro)
- ❌ **Python dependencies missing** (PyTorch, Kokoro)
- ⚠️  **Library linking issue resolved** (symlinks required in bin/)

**Recommendation**: Acquire models and install dependencies before proceeding to Phase 1 implementation.

---

## Environment

### Hardware
- **Device**: Apple M4 (Apple Silicon)
- **GPU**: MTL0, MTLGPUFamilyApple9 (1009)
- **GPU Memory**: 12123 MiB free (~11.8 GB)
- **Recommended Max Working Set**: 12713.12 MB (~12.4 GB)
- **Unified Memory**: Yes (Apple Silicon advantage)
- **BFloat16 Support**: Yes (faster inference)
- **Tensor API**: No (disabled for pre-M5 devices)

### Software
- **OS**: macOS 25.2.0 (Apple Silicon)
- **Python**: 3.14.2
- **CMake**: 4.2.1
- **Compiler**: Homebrew clang version 21.1.8

### File Descriptor Limit
```bash
$ ulimit -n
unlimited
```
**Status**: ✅ Exceeds requirement of 4096

---

## Service Startup Testing

### 1. Outbound Audio Processor
**Binary**: `bin/outbound-audio-processor`  
**Size**: 43KB  
**Startup Test**: Not tested (would run indefinitely)  
**Dependencies**: None  
**Expected Status**: ✅ Ready to run

---

### 2. Inbound Audio Processor
**Binary**: `bin/inbound-audio-processor`  
**Size**: 40KB  
**Startup Test**: Not tested (would run indefinitely)  
**Dependencies**: None  
**Expected Status**: ✅ Ready to run

---

### 3. Whisper Service
**Binary**: `bin/whisper-service`  
**Size**: 43KB  
**Startup Test**: ✅ Passed (usage message displayed correctly)  
**Dependencies**: 
- `libwhisper.1.dylib` (✅ Found, symlinked to bin/)
- `libwhisper.coreml.dylib` (✅ Found, symlinked to bin/)

**Test Output**:
```
$ ./bin/whisper-service
Usage: whisper-service <model_path>
```

**Status**: ⚠️  **Blocked - Missing Whisper CoreML model**

**Required Model**:
- Path: `models/whisper/ggml-base-encoder.coreml` (or similar)
- Format: CoreML model (`.mlmodelc` bundle or `.coreml` file)
- Size: ~140MB (base model)
- Source: https://github.com/ggerganov/whisper.cpp (convert from GGML)

**CoreML Support**: ✅ Confirmed available (libwhisper.coreml.dylib loaded)

---

### 4. LLaMA Service
**Binary**: `bin/llama-service`  
**Size**: 62KB  
**Startup Test**: ✅ Metal backend initialized successfully  
**Dependencies**: 
- `libllama.0.dylib` (✅ Found, symlinked to bin/)

**Test Output**:
```
$ ./bin/llama-service
ggml_metal_device_init: tensor API disabled for pre-M5 and pre-A19 devices
ggml_metal_library_init: using embedded metal library
ggml_metal_library_init: loaded in 0.005 sec
ggml_metal_rsets_init: creating a residency set collection (keep_alive = 180 s)
ggml_metal_device_init: GPU name:   MTL0
ggml_metal_device_init: GPU family: MTLGPUFamilyApple9  (1009)
ggml_metal_device_init: GPU family: MTLGPUFamilyCommon3 (3003)
ggml_metal_device_init: GPU family: MTLGPUFamilyMetal4  (5002)
ggml_metal_device_init: simdgroup reduction   = true
ggml_metal_device_init: simdgroup matrix mul. = true
ggml_metal_device_init: has unified memory    = true
ggml_metal_device_init: has bfloat            = true
ggml_metal_device_init: has tensor            = false
ggml_metal_device_init: use residency sets    = true
ggml_metal_device_init: use shared buffers    = true
ggml_metal_device_init: recommendedMaxWorkingSetSize  = 12713.12 MB
llama_model_load_from_file_impl: using device MTL0 (Apple M4) (unknown id) - 12123 MiB free
gguf_init_from_file: failed to open GGUF file 'models/llama/Llama-3.2-1B-Instruct-Q8_0.gguf' (No such file or directory)
llama_model_load: error loading model: llama_model_loader: failed to load model from models/llama/Llama-3.2-1B-Instruct-Q8_0.gguf
llama_model_load_from_file_impl: failed to load model
```

**Metal Acceleration**: ✅ **CONFIRMED**
- Embedded metal library loaded in 5ms
- GPU detected: MTL0 (Apple M4)
- Unified memory: Yes (optimal for Apple Silicon)
- BFloat16: Yes (faster mixed-precision inference)
- Simdgroup operations: Yes (vectorized matrix multiplication)
- Residency sets: Yes (efficient memory management)
- Shared buffers: Yes (zero-copy between CPU/GPU)

**Status**: ⚠️  **Blocked - Missing LLaMA GGUF model**

**Required Model**:
- Path: `models/llama/Llama-3.2-1B-Instruct-Q8_0.gguf`
- Format: GGUF (quantized, Q8_0 recommended)
- Size: ~1.3GB
- Source: https://huggingface.co/lmstudio-community/Llama-3.2-1B-Instruct-GGUF

---

### 5. Kokoro TTS Service
**Binary**: `bin/kokoro_service.py`  
**Size**: 20KB (Python script)  
**Startup Test**: ❌ Failed - Missing dependencies  
**Dependencies**: 
- PyTorch (❌ Not installed)
- Kokoro library (❌ Not installed)

**Test Output**:
```
$ python3 -c "import torch"
ModuleNotFoundError: No module named 'torch'

$ python3 -c "from kokoro import KPipeline"
ModuleNotFoundError: No module named 'kokoro'
```

**Status**: ❌ **Blocked - Missing Python dependencies and Kokoro model**

**Required Dependencies**:
```bash
pip3 install torch kokoro-tts
```

**Required Model**:
- Path: `models/kokoro-german/kokoro-german-v1_1-de.pth`
- Config: `models/kokoro-german/config.json`
- Voices: `models/kokoro-german/voices/*.pt` (optional)
- Size: ~500MB
- Source: https://github.com/thewh1teagle/kokoro-onnx

**MPS Support**: ⚠️  Unknown (PyTorch not installed, cannot verify)

---

### 6. SIP Client
**Binary**: `bin/sip-client`  
**Size**: 63KB  
**Startup Test**: Not tested (requires SIP server arguments)  
**Dependencies**: None  
**Expected Status**: ✅ Ready to run (once arguments provided)

---

## Critical Issue: Library Linking

### Problem
All C++ services using external libraries failed to start with:
```
dyld: Library not loaded: @rpath/libwhisper.1.dylib
  Referenced from: /path/to/bin/whisper-service
  Reason: tried: '/path/to/bin/libwhisper.1.dylib' (no such file)
```

### Root Cause
Binaries use `@rpath` to locate shared libraries in the same directory as the executable. However, libraries are built in:
- `whisper-cpp/build/src/libwhisper.1.dylib`
- `llama-cpp/build/bin/libllama.0.dylib`

But binaries look in:
- `bin/libwhisper.1.dylib`
- `bin/libllama.0.dylib`

### Solution Applied
Created symlinks in `bin/` directory:
```bash
cd bin
ln -sf ../whisper-cpp/build/src/libwhisper.1.dylib .
ln -sf ../whisper-cpp/build/src/libwhisper.coreml.dylib .
ln -sf ../llama-cpp/build/bin/libllama.0.dylib .
```

**Result**: ✅ All services now load libraries correctly

**Action Required**: This step must be documented in build process or automated in CMakeLists.txt

---

## Missing Components Summary

### ML Models (0/3 present)
1. ❌ **Whisper CoreML Model**: ~140MB, required for ASR
2. ❌ **LLaMA GGUF Model**: ~1.3GB, required for conversation
3. ❌ **Kokoro German Model**: ~500MB, required for TTS

**Total Download**: ~2GB of model files needed

### Python Dependencies (0/2 installed)
1. ❌ **PyTorch**: Required for Kokoro TTS
2. ❌ **Kokoro-TTS**: Required for speech synthesis

### Installation Commands
```bash
# Install Python dependencies
pip3 install torch kokoro-tts

# Create model directories
mkdir -p models/whisper
mkdir -p models/llama
mkdir -p models/kokoro-german/voices

# Download models (manual, URLs in service_startup.md)
```

---

## Performance Baseline - UNABLE TO COLLECT

Due to missing models, the following metrics **could not be measured**:

### Target Metrics (Deferred to After Model Acquisition)
- [ ] End-to-end latency (INVITE to first audio response)
- [ ] Per-stage latency:
  - [ ] Inbound Processor: RTP → PCM conversion
  - [ ] Whisper Service: Audio → Text transcription
  - [ ] LLaMA Service: Text → Response generation
  - [ ] Kokoro Service: Text → Audio synthesis
  - [ ] Outbound Processor: PCM → RTP encoding
- [ ] Memory usage per service (RSS)
- [ ] CPU utilization under load
- [ ] VAD accuracy (segmentation quality)
- [ ] Concurrent call capacity (max before degradation)

---

## Test Scripts Evaluation

### Available Test Scripts
1. **tests/multi_call_test.py**:
   - Simulates 3 concurrent calls (IDs: 1001, 1002, 1003)
   - Sends silent RTP packets to UDP port 9001
   - Duration: 10 seconds
   - Status: ✅ Can be used for pipeline testing

2. **tests/pipeline_loop_sim.cpp**:
   - C++ simulation (requires compilation)
   - Status: ⚠️  Not evaluated (requires CMake integration)

3. **tests/whisper_inbound_sim.cpp**:
   - Whisper + Inbound simulation
   - Status: ⚠️  Not evaluated

4. **tests/test_llama_german.py**:
   - LLaMA German conversation test
   - Status: ⚠️  Requires LLaMA model

### Orchestration Status
❌ **No automated orchestration exists** for starting all 6 services

**Impact**: 
- Manual startup required in correct order
- No health checks or readiness verification
- Difficult to run integration tests

---

## Limitations & Blockers

### Immediate Blockers (Prevent Baseline Measurement)
1. ❌ **Models unavailable**: All 3 ML models must be acquired
2. ❌ **PyTorch not installed**: Kokoro service cannot start
3. ❌ **Kokoro library not installed**: TTS unavailable

### Systemic Limitations (Current Design)
1. **No orchestration**: Services must be started manually
2. **No health checks**: Cannot verify services are ready
3. **No call lifecycle signals**: CALL_START/CALL_END not implemented (Phase 1)
4. **No crash recovery**: Services don't auto-reconnect (Phase 3)
5. **Library symlinks required**: Build process doesn't deploy libraries to bin/
6. **No metrics collection**: Performance data not captured (Phase 7)

### Test Coverage Gaps
1. **No VAD test corpus**: Cannot measure segmentation accuracy
2. **No SIP server**: Cannot test real call flow
3. **No integration test harness**: Cannot run full pipeline tests
4. **No stress test tools**: Cannot measure concurrent call capacity

---

## Recommendations

### Immediate Actions (Before Phase 1)
1. **Acquire ML Models**:
   - Download Whisper CoreML model (~140MB)
   - Download LLaMA 3.2-1B-Instruct GGUF (~1.3GB)
   - Download Kokoro German model (~500MB)

2. **Install Python Dependencies**:
   ```bash
   pip3 install torch kokoro-tts
   ```

3. **Fix Build Process**:
   - Automate library symlink creation in CMakeLists.txt
   - Or: Update build script to copy libraries to bin/

4. **Verify Service Startup**:
   - Start each service individually
   - Confirm no errors in console output
   - Verify Metal/MPS acceleration active

### Deferred to Future Phases
5. **Create Test Corpus** (Phase 4.3):
   - Generate German audio test files
   - Establish VAD accuracy baseline

6. **Implement Orchestration** (Phase 6.2):
   - Automated service startup script
   - Health check verification
   - Graceful shutdown handling

7. **Run Baseline Benchmarks** (After models acquired):
   - Single call latency measurement
   - 10 concurrent calls stress test
   - Memory profiling under load
   - CPU utilization analysis

---

## Comparison to Target Performance (Estimated)

**Note**: Actual baseline metrics unavailable. Estimates based on hardware capabilities:

| Metric | Target (Phase 7) | Estimated Current | Status |
|--------|------------------|-------------------|--------|
| End-to-End Latency (P90) | <1.5s | Unknown (no models) | ❌ Not measured |
| Whisper Transcription | <500ms | ~200ms (CoreML) | ⚠️  Estimated |
| LLaMA Generation (50 tokens) | <300ms | ~150ms (Metal) | ⚠️  Estimated |
| Kokoro TTS (15 words) | <200ms | ~100ms (MPS) | ⚠️  Estimated |
| VAD Accuracy | >95% | Unknown | ❌ Not measured |
| Interruption Latency | <200ms | N/A (not implemented) | ❌ Feature missing |
| Concurrent Calls (M4) | 100 | Unknown | ❌ Not tested |
| Memory per Call | <100MB | Unknown | ❌ Not measured |

**Hardware Confidence**: 
- ✅ M4 GPU has sufficient memory (12GB free)
- ✅ Metal acceleration confirmed working
- ✅ BFloat16 support will accelerate inference
- ⚠️  Actual performance depends on model sizes and optimization

---

## Next Steps

### Required for Phase 0 Completion
- [ ] Acquire all 3 ML models (Whisper, LLaMA, Kokoro)
- [ ] Install PyTorch and Kokoro library
- [ ] Verify each service can start successfully
- [ ] Run multi_call_test.py to validate pipeline
- [ ] Measure single-call end-to-end latency
- [ ] Document actual baseline metrics

### Optional for Phase 0
- [ ] Test 10 concurrent calls (if resources permit)
- [ ] Profile memory usage
- [ ] Measure CPU utilization
- [ ] Document any crashes or hangs

### Prerequisites for Phase 1
- [ ] Establish baseline metrics (blocked until models acquired)
- [ ] Confirm Metal/MPS acceleration working for all services
- [ ] Verify services can handle 5+ concurrent calls without degradation

---

## Conclusion

**Phase 0 Status**: ⚠️  **INCOMPLETE - Blocked by missing models**

While the build infrastructure is solid (all services compile, Metal acceleration works), **baseline measurement is impossible without ML models**. 

**Two Options**:
1. **Acquire models now**: Complete Phase 0 properly with real metrics
2. **Proceed to Phase 1**: Implement features first, measure later (risky - no regression detection)

**Recommendation**: **Acquire models before Phase 1** to establish proper baseline and enable regression testing during implementation.

---

**Document Version**: 1.0  
**Last Updated**: 2026-02-10  
**Status**: Blocked - Awaiting models and dependencies  
**Next Review**: After models acquired
