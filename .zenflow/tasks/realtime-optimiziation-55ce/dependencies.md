# WhisperTalk Dependencies Documentation

**Date**: 2026-02-10  
**Phase**: Phase -1 Prerequisites Verification  
**Status**: ✅ All dependencies verified and built successfully

---

## System Information

- **OS**: macOS 25.2.0 (Apple Silicon)
- **Architecture**: arm64
- **File Descriptor Limit**: 4096 (configured)
- **Initial Limit**: unlimited (system default)

---

## Build Toolchain

- **CMake**: 4.2.1
- **Compiler**: Homebrew clang version 21.1.8 (AppleClang 17.0.0.17000603)
- **Python**: 3.14.2
- **Build System**: CMake 3.22+ (minimum required)
- **C++ Standard**: C++17

---

## Core Dependencies

### 1. whisper-cpp

**Status**: ✅ Present and built  
**Location**: `./whisper-cpp/`  
**Library**: `whisper-cpp/build/src/libwhisper.dylib` → `libwhisper.1.dylib`

**Version Information**:
- **Commit**: `7120b1324152bdd54a4872780b793f9a17168fbd`
- **Date**: 2026-02-10 09:34:30 +0100
- **Message**: Planning
- **Short Hash**: `7120b13`

**Build Configuration**:
- CoreML enabled (macOS optimization)
- Metal backend enabled
- BLAS backend enabled (Accelerate framework)
- ggml version: 0.0.99

**Headers**:
- `whisper-cpp/include/whisper.h`
- `whisper-cpp/ggml/include/ggml.h`
- `whisper-cpp/ggml/include/ggml-alloc.h`
- `whisper-cpp/ggml/include/ggml-backend.h`
- `whisper-cpp/ggml/include/ggml-metal.h`
- `whisper-cpp/ggml/include/gguf.h`

---

### 2. llama-cpp

**Status**: ✅ Cloned and built  
**Location**: `./llama-cpp/`  
**Library**: `llama-cpp/build/bin/libllama.dylib` → `libllama.0.dylib`  
**Source**: https://github.com/ggerganov/llama.cpp.git

**Version Information**:
- **Commit**: `6948adc90d77949e7802616d4c030396cf03b9c7`
- **Date**: 2026-02-10 10:57:48 +0100
- **Message**: ggml : use noexcept overload for is_regular_file in backend registration (#19452)
- **Short Hash**: `6948adc`

**Build Configuration**:
- CoreML enabled (via `GGML_COREML=ON`)
- Metal backend enabled
- CPU backend with ARM optimizations (dotprod, i8mm, nosve, sme)
- BLAS backend enabled (Accelerate framework)

**Note**: llama-cpp was not present initially and was cloned during prerequisites verification.

**Headers**:
- `llama-cpp/include/*.h`
- `llama-cpp/ggml/include/*.h`

---

## Built Services

All 6 core services compiled successfully:

| Service | Binary | Size | Type | Status |
|---------|--------|------|------|--------|
| SIP Client | `bin/sip-client` | 63K | Mach-O 64-bit arm64 | ✅ Built |
| Inbound Audio Processor | `bin/inbound-audio-processor` | 40K | Mach-O 64-bit arm64 | ✅ Built |
| Outbound Audio Processor | `bin/outbound-audio-processor` | 43K | Mach-O 64-bit arm64 | ✅ Built |
| Whisper Service | `bin/whisper-service` | 43K | Mach-O 64-bit arm64 | ✅ Built |
| LLaMA Service | `bin/llama-service` | 62K | Mach-O 64-bit arm64 | ✅ Built |
| Kokoro TTS Service | `bin/kokoro_service.py` | 20K | Python script (UTF-8) | ✅ Present |

---

## Additional Components

### Piper TTS (Prebuilt Binary)

**Status**: ✅ Downloaded  
**Location**: `bin/piper-prebuilt`  
**Size**: 3.1M  
**Version**: 2023.11.14-2  
**Source**: https://github.com/rhasspy/piper/releases/download/2023.11.14-2/piper_macos_aarch64.tar.gz

**Note**: Piper is not currently used by the core pipeline (Kokoro is the active TTS engine), but was downloaded by the build script for future compatibility.

---

## Build Process

### Initial State
- whisper-cpp: Present but not built
- llama-cpp: **Missing** (not cloned)
- Services: 3/6 could compile without libraries (SIP, Inbound, Outbound)

### Actions Taken
1. Set file descriptor limit: `ulimit -n 4096`
2. Cloned llama-cpp from official repository (depth=1)
3. Built whisper-cpp with CoreML support (~2 seconds)
4. Built llama-cpp with CoreML support (~33 seconds)
5. Built all 6 WhisperTalk services (~1 second)
6. Downloaded prebuilt Piper binary for macOS arm64

### Build Command
```bash
bash scripts/build.sh --with-deps --no-piper
```

**Total Build Time**: ~36 seconds (excluding download time)

---

## Verification Results

### CMake Configuration
✅ Passed - All include directories found  
✅ Passed - All required headers accessible  
✅ Passed - C++17 support confirmed

### Library Linking
✅ whisper-service links to `libwhisper.dylib`  
✅ llama-service links to `libllama.dylib`  
✅ All services use `-rpath @executable_path` for portable deployment

### Binary Validation
✅ All 5 C++ services are valid Mach-O 64-bit arm64 executables  
✅ All binaries located in `bin/` directory  
✅ No missing symbols or link errors

---

## System Limits Configuration

### File Descriptors
- **Required**: 4096 (for Whisper's 100 ports + system overhead)
- **Current**: 4096 ✅
- **Command**: `ulimit -n 4096`

**Note**: The system default was "unlimited", but we set to 4096 as specified in requirements. For production, consider keeping unlimited if system resources allow.

### Recommendations for Production
- Verify `ulimit -n` is set in service startup scripts
- Consider adding to shell profile: `echo "ulimit -n 4096" >> ~/.zshrc`
- Or configure via launchd plist for system services

---

## Known Issues & Notes

1. **llama-cpp not tracked in git**: The llama-cpp directory is not a git submodule. Developers must manually clone it or run the build script.

2. **No .gitmodules**: Dependencies are not configured as git submodules. Consider adding:
   ```gitmodules
   [submodule "whisper-cpp"]
       path = whisper-cpp
       url = https://github.com/ggerganov/whisper.cpp.git
   [submodule "llama-cpp"]
       path = llama-cpp
       url = https://github.com/ggerganov/llama.cpp.git
   ```

3. **Build warnings**: 
   - whisper-cpp: 2 warnings about category method implementations (non-critical)
   - CMake deprecation warnings (non-critical, CMake 3.10 compatibility)

4. **Missing documentation**: No top-level README or INSTALL guide. Consider creating setup documentation for new developers.

---

## Next Steps for Phase 0

With all dependencies verified and built, the system is ready for:
- ✅ Service startup testing
- ✅ Baseline performance measurement
- ✅ Model availability verification (Whisper CoreML, LLaMA GGUF, Kokoro weights)

---

## Dependency Update Procedure

To update dependencies in the future:

```bash
# Update whisper-cpp
cd whisper-cpp
git pull
cd build
cmake .. && make -j6

# Update llama-cpp
cd llama-cpp
git pull
cd build
cmake .. && make -j6

# Rebuild WhisperTalk services
cd ../..
bash scripts/build.sh --no-deps --no-piper
```

---

**Verified by**: Zencoder AI Assistant  
**Phase**: Phase -1 - Prerequisites Verification ✅ Complete
