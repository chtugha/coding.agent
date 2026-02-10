# WhisperTalk Dependencies Documentation

**Date**: 2026-02-10  
**Phase**: Phase -1 Prerequisites Verification  
**Status**: ✅ All dependencies verified and built successfully

---

## System Information

- **OS**: macOS 25.2.0 (Apple Silicon)
- **Architecture**: arm64
- **File Descriptor Limit**: unlimited (system default, exceeds 4096 requirement)
- **Persistent Configuration**: Not set (see System Limits Configuration section)

---

## Build Toolchain

- **CMake**: 4.2.1
- **Compiler**: Homebrew clang version 21.1.8 (AppleClang 17.0.0.17000603)
- **Python**: 3.14.2
- **Build System**: CMake 3.22+ (minimum required)
- **C++ Standard**: C++17

---

## Core Dependencies

### Dependency Types

**Integrated Components**: Maintained as part of the WhisperTalk repository  
**External Dependencies**: Cloned from external repositories during build

---

### 1. whisper-cpp (Integrated Component)

**Status**: ✅ Present and built  
**Location**: `./whisper-cpp/`  
**Library**: `whisper-cpp/build/src/libwhisper.dylib` → `libwhisper.1.dylib`  
**Type**: **Integrated component** (tracked in WhisperTalk repository, not a git submodule)

**Upstream Version**:
- Based on whisper.cpp **v1.7.6** (stable release)
- Source: https://github.com/ggml-org/whisper.cpp
- Integrated and maintained within WhisperTalk codebase

**Note**: This is **not** an external dependency. It is part of the WhisperTalk repository and shares the same git history. The code is based on upstream whisper.cpp v1.7.6 but may include WhisperTalk-specific modifications.

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

### 2. llama-cpp (External Dependency)

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
- whisper-cpp: Present (integrated component) but not built
- llama-cpp: **Missing** (external dependency not cloned)
- Services: 3/6 could compile without libraries (SIP, Inbound, Outbound)

### Actions Taken
1. Verified file descriptor limit: unlimited (exceeds 4096 requirement, no action needed)
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
- **Current**: unlimited (system default, exceeds requirement) ✅
- **Status**: Not configured persistently

**Important**: The `ulimit -n` command is **session-specific** and does not persist across shell sessions or reboots. While the system default of "unlimited" exceeds the 4096 requirement, explicit configuration is recommended for production deployments.

### Recommendations for Production

**Option 1: Service Startup Scripts** (Recommended)
```bash
#!/bin/bash
# startup-whisper-service.sh
ulimit -n 4096  # Set limit before launching service
exec ./bin/whisper-service /path/to/model.coreml
```

**Option 2: Shell Profile** (For development)
```bash
echo "ulimit -n 4096" >> ~/.zshrc
# Restart shell or run: source ~/.zshrc
```

**Option 3: launchd Configuration** (For macOS system services)
```xml
<key>SoftResourceLimits</key>
<dict>
    <key>NumberOfFiles</key>
    <integer>4096</integer>
</dict>
```

**Verification**:
```bash
ulimit -n  # Check current limit before starting services
```

---

## Known Issues & Notes

1. **llama-cpp not tracked in git**: The llama-cpp directory is not a git submodule. Developers must manually clone it or run the build script with `--with-deps`.

2. **whisper-cpp is integrated, not external**: Unlike llama-cpp, whisper-cpp is part of the WhisperTalk repository and should NOT be added as a git submodule. If considering dependency management improvements, only llama-cpp should be considered for submodule conversion:
   ```gitmodules
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

### Update whisper-cpp (Integrated Component)

whisper-cpp is part of the WhisperTalk repository. Updates come from the main repository:

```bash
# Update WhisperTalk repository (includes whisper-cpp)
git pull origin main

# Rebuild whisper-cpp if needed
cd whisper-cpp/build
cmake .. && make -j6
cd ../..
```

To sync with upstream whisper.cpp (manual process):
```bash
# This requires manual merging - consult team before attempting
# 1. Add upstream remote (one-time)
cd whisper-cpp
git remote add upstream https://github.com/ggml-org/whisper.cpp.git

# 2. Fetch upstream changes
git fetch upstream

# 3. Merge carefully (may have conflicts with WhisperTalk modifications)
git merge upstream/master
```

### Update llama-cpp (External Dependency)

```bash
cd llama-cpp
git pull origin master  # or: git fetch && git checkout <specific-version>
cd build
cmake .. && make -j6
cd ../..
```

### Rebuild WhisperTalk Services

After updating any dependencies:

```bash
bash scripts/build.sh --no-deps --no-piper
```

---

**Verified by**: Zencoder AI Assistant  
**Phase**: Phase -1 - Prerequisites Verification ✅ Complete
