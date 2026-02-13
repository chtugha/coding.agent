# Technical Specification: WhisperTalk Standalone Architecture Redesign

**Version**: 1.0  
**Date**: 2026-02-13  
**Status**: DRAFT

---

## 1. Technical Context

### 1.1 Languages and Frameworks

**Primary Languages:**
- **C++17**: All services (including new Kokoro C++ port)
- **Build System**: CMake 3.22+
- **Target Platform**: macOS (Apple Silicon, arm64)

**Core Dependencies:**
- **whisper.cpp**: ASR engine with CoreML acceleration (existing)
- **llama.cpp**: LLM engine with Metal/MPS acceleration (existing)
- **libtorch** (PyTorch C++ API): For Kokoro TTS C++ port (new)
- **espeak-ng**: For German phonemization (new, via C API)
- **System Libraries**: pthread, socket, Standard C++ library

### 1.2 Current Architecture Overview

**Existing Services (6 components):**

| Component | Language | Lines | IPC Mechanisms |
|-----------|----------|-------|----------------|
| SIP Client | C++ | 247 | UDP (9001, 9002), Unix sockets |
| Inbound Audio Processor | C++ | 156 | UDP (9001), TCP (13000+) |
| Whisper Service | C++ | 207 | TCP (13000+, 8083) |
| LLaMA Service | C++ | 265 | TCP (8083, 8090) |
| Kokoro TTS Service | **Python** | 549 | TCP (8090+), UDP (13001) |
| Outbound Audio Processor | C++ | 210 | TCP (8090+), UDP (9002), Unix sockets |

**Problems to Address:**
1. Mixed IPC protocols (UDP, TCP, Unix sockets) → inconsistent error handling
2. Python dependency → prevents standalone deployment
3. Hard-coded ports → limits scalability, causes conflicts
4. No central orchestration → ad-hoc crash recovery
5. Incomplete reconnection logic → fragile connections
6. Variable multi-call robustness across services

---

## 2. Implementation Approach

### 2.1 Architecture Overview

The redesign transforms WhisperTalk from a loosely-coupled collection of services into a **unified master/slave TCP-based pipeline** with automatic service discovery, crash recovery, and consistent multi-call support.

**Key Design Principles:**
1. **Single IPC Protocol**: TCP-only for all inter-service communication
2. **Standalone Binaries**: Static linking where possible, minimal dynamic dependencies
3. **Master/Slave Orchestration**: Automatic service discovery, registration, and crash detection
4. **Crash Resilience**: All connections aware of partner status, automatic reconnection via master
5. **Call ID Tracking**: Consistent multi-call support with atomic call_id negotiation
6. **Native C++ Stack**: Eliminate Python dependency via libtorch-based Kokoro port

### 2.2 Interconnection System Design

#### 2.2.1 Core Abstraction: `interconnect.h`

A single header-only library included in all services, providing:

```cpp
// interconnect.h
namespace whispertalk {

// Port configuration
struct PortConfig {
    uint16_t neg_in;      // Negotiation incoming (22222+3n)
    uint16_t neg_out;     // Negotiation outgoing (33333+3n)
    uint16_t down_in;     // Downstream traffic in (neg_in+1)
    uint16_t down_out;    // Downstream traffic out (neg_in+2)
    uint16_t up_in;       // Upstream traffic in (neg_out+1)
    uint16_t up_out;      // Upstream traffic out (neg_out+2)
};

// Service types
enum class ServiceType {
    SIP_CLIENT,
    INBOUND_AUDIO_PROCESSOR,
    WHISPER_SERVICE,
    LLAMA_SERVICE,
    KOKORO_SERVICE,
    OUTBOUND_AUDIO_PROCESSOR
};

// Packet structure: [4B call_id][4B size][NB payload]
struct Packet {
    uint32_t call_id;
    uint32_t size;
    std::vector<uint8_t> payload;
    
    static Packet deserialize(const uint8_t* data, size_t len);
    std::vector<uint8_t> serialize() const;
};

// Master/Slave node
class InterconnectNode {
public:
    InterconnectNode(ServiceType type);
    
    // Initialization
    bool init();  // Scans ports, becomes master/slave
    bool is_master() const { return is_master_; }
    
    // Service registry (master only)
    void register_slave(ServiceType type, const PortConfig& ports);
    PortConfig get_upstream_service(ServiceType requester) const;
    PortConfig get_downstream_service(ServiceType requester) const;
    
    // Traffic connections
    bool connect_to_upstream();
    bool connect_to_downstream();
    
    // Send/receive packets
    bool send_upstream(const Packet& pkt);
    bool send_downstream(const Packet& pkt);
    bool recv_upstream(Packet& pkt, int timeout_ms = 0);
    bool recv_downstream(Packet& pkt, int timeout_ms = 0);
    
    // Call lifecycle
    void broadcast_call_end(uint32_t call_id);  // Master only
    void register_call_end_handler(std::function<void(uint32_t)> handler);
    
    // Heartbeat and crash detection
    void start_heartbeat();  // Runs in background thread
    bool is_service_alive(ServiceType type) const;
    
private:
    ServiceType type_;
    bool is_master_;
    PortConfig ports_;
    
    int neg_in_sock_;     // Listen on negotiation port
    int neg_out_sock_;    // Connect to master negotiation port
    
    int down_in_sock_;    // Listen for downstream partner
    int down_out_sock_;   // Listen for downstream partner
    int up_in_sock_;      // Connect to upstream partner
    int up_out_sock_;     // Connect to upstream partner
    
    std::map<ServiceType, PortConfig> service_registry_;  // Master only
    std::map<ServiceType, std::chrono::steady_clock::time_point> last_heartbeat_;
    
    std::function<void(uint32_t)> call_end_handler_;
    
    PortConfig scan_and_bind_ports();
    void heartbeat_loop();
    void negotiation_loop();
};

} // namespace whispertalk
```

#### 2.2.2 Port Allocation Strategy

**Base Ports:**
- Negotiation incoming: 22222 (master), 22225, 22228, 22231, 22234, 22237, ...
- Negotiation outgoing: 33333 (master), 33336, 33339, 33342, 33345, 33348, ...

**Traffic Ports** (derived from negotiation ports):
- Downstream In: `neg_in + 1`
- Downstream Out: `neg_in + 2`
- Upstream In: `neg_out + 1`
- Upstream Out: `neg_out + 2`

**Port Discovery Algorithm:**
1. Try to bind to 22222 and 33333
2. If both succeed → become **Master**
3. If either fails → scan upward in increments of 3 until both ports in a pair are free
4. Bind to free pair → become **Slave** → register with master at 22222/33333

#### 2.2.3 Connection Topology

Services connect in a linear pipeline:

```
SIP Client → Inbound Audio → Whisper → LLaMA → Kokoro → Outbound Audio → SIP Client
                                                                              (loop back)
```

**Connection Rules:**
- **Downstream** service initiates connections to **upstream** service
- Each service pair uses **two TCP connections** (bidirectional channels):
  - **Connection 1**: Upstream UpOut → Downstream DownIn (primary data flow)
  - **Connection 2**: Downstream DownOut → Upstream UpIn (control/backpressure)

**Example** (Inbound Audio ↔ Whisper):
- Whisper (downstream) connects to Inbound Audio (upstream):
  - Whisper's DownIn (22229) ← connects to ← IAP's UpOut (33338)
  - Whisper's DownOut (22230) → connects to → IAP's UpIn (33337)

#### 2.2.4 Protocol Design

**Negotiation Protocol** (via negotiation ports):

| Message | Direction | Purpose |
|---------|-----------|---------|
| `REGISTER <type> <neg_in> <neg_out>` | Slave → Master | Service registration |
| `HEARTBEAT <type> <call_count> <state>` | Slave → Master | Alive signal (every 2s) |
| `GET_UPSTREAM <type>` | Service → Master | Query upstream ports |
| `GET_DOWNSTREAM <type>` | Service → Master | Query downstream ports |
| `CHECK_CALL_ID <call_id>` | SIP Client → IAP | Check if call_id is taken |
| `HIGHEST_CALL_ID <max_id>` | IAP → SIP Client | Return max known call_id |
| `CALL_END <call_id>` | Any → Master → All | Broadcast call termination |

**Traffic Protocol** (via traffic ports):

All packets follow this structure:
```
[4 bytes: call_id (network byte order)]
[4 bytes: payload_size (network byte order)]
[N bytes: payload]
```

Payload types vary by service:
- **SIP Client → IAP**: RTP frames (G.711 encoded audio)
- **IAP → Whisper**: Float32 PCM audio (16kHz, mono)
- **Whisper → LLaMA**: UTF-8 text (transcribed sentences)
- **LLaMA → Kokoro**: UTF-8 text (generated responses)
- **Kokoro → Outbound**: Float32 PCM audio (24kHz, mono)
- **Outbound → SIP Client**: G.711 encoded audio

### 2.3 Kokoro C++ Port (Python → C++)

#### 2.3.1 Current Python Architecture

**Key Components:**
1. **Kokoro Model**: PyTorch-based neural TTS (lives in Python package)
2. **Phonemizer**: Text → IPA phonemes (uses `phonemizer` Python package → espeak-ng)
3. **Inference**: MPS-accelerated model forward pass
4. **Voice Models**: German voices (`df_eva`, `dm_bernd`)

**Dependencies:**
- `torch` (PyTorch with MPS support)
- `kokoro` package (model architecture + pretrained weights)
- `phonemizer` (wrapper around espeak-ng)
- `numpy`

#### 2.3.2 C++ Port Strategy

**Approach**: Use **libtorch** (PyTorch C++ API) + **espeak-ng C API**

**Component Mapping:**

| Python Component | C++ Equivalent | Implementation |
|------------------|----------------|----------------|
| `KPipeline` | `KokoroPipeline` class | Load model via `torch::jit::load()` or manual weights |
| `KModel.forward()` | `torch::Tensor forward()` | libtorch inference on MPS device |
| `phonemizer.phonemize()` | `espeak_ng_Synthesize()` | Direct espeak-ng C API calls |
| Voice loading | `torch::load()` | Load `.pth` voice embeddings |
| MPS device | `torch::Device(torch::kMPS)` | Metal acceleration via libtorch |

**Model Loading Options:**
1. **TorchScript** (preferred): Export Python model to `.pt` via `torch.jit.script()`, load in C++ via `torch::jit::load()`
2. **Manual Weights**: Load `.pth` checkpoint, reconstruct model in C++

**Phonemization Pipeline:**

German text requires **grapheme-to-phoneme (G2P)** conversion before TTS:
```
Text → espeak-ng (German rules) → IPA phonemes → Kokoro model → Audio
```

**C++ Implementation:**
```cpp
#include <torch/script.h>
#include <torch/torch.h>
#include <espeak-ng/speak_lib.h>

class KokoroPipeline {
public:
    KokoroPipeline(const std::string& model_path, 
                   const std::string& voice_path,
                   const std::string& lang_code = "de");
    
    std::vector<float> synthesize(const std::string& text);
    
private:
    torch::jit::script::Module model_;
    torch::Tensor voice_embedding_;
    torch::Device device_{torch::kMPS};
    
    std::vector<std::string> phonemize(const std::string& text);
    torch::Tensor text_to_sequence(const std::vector<std::string>& phonemes);
};
```

**espeak-ng Integration:**
```cpp
std::vector<std::string> KokoroPipeline::phonemize(const std::string& text) {
    espeak_ng_InitializePath(nullptr);
    espeak_ng_Initialize(nullptr);
    espeak_SetVoiceByName("de");  // German voice
    
    int flags = espeakCHARS_UTF8 | espeakPHONEMES_IPA;
    const char* phonemes = espeak_TextToPhonemes((const void**)&text.c_str(), 
                                                   espeakCHARS_UTF8, flags);
    
    // Parse IPA string into phoneme tokens
    return parse_ipa(phonemes);
}
```

**Model Export (one-time, from Python):**
```python
# export_kokoro_model.py
import torch
from kokoro.model import KModel

model = KModel(config="config.json", model="kokoro-german-v1_1-de.pth")
model.eval()

# Export via TorchScript
scripted_model = torch.jit.script(model)
scripted_model.save("kokoro_german.pt")

# Export voice embeddings
voice = torch.load("voices/df_eva.pt")
torch.save(voice, "df_eva_embedding.pt")
```

#### 2.3.3 Build System Integration

**CMakeLists.txt additions:**
```cmake
# Find libtorch
set(CMAKE_PREFIX_PATH "/path/to/libtorch")
find_package(Torch REQUIRED)

# Find espeak-ng (via pkg-config or manual)
find_library(ESPEAK_NG_LIB espeak-ng REQUIRED)

# Kokoro Service
add_executable(kokoro-service kokoro-service.cpp)
target_link_libraries(kokoro-service PRIVATE 
    Threads::Threads
    ${TORCH_LIBRARIES}
    ${ESPEAK_NG_LIB}
)

# Set libtorch C++17 ABI compatibility
set_property(TARGET kokoro-service PROPERTY CXX_STANDARD 17)
```

**Static Linking Considerations:**
- libtorch does not officially support full static linking on macOS
- **Hybrid approach**: Static link libtorch libraries where possible, dynamic link CoreML frameworks
- Use `otool -L` to verify dependencies, bundle libtorch `.dylib` files with binaries

### 2.4 Static Binary Strategy

#### 2.4.1 Dependency Analysis

**Per-Service Dependencies:**

| Service | Static-Linkable | Must Be Dynamic | Bundled Assets |
|---------|-----------------|-----------------|----------------|
| SIP Client | pthread, C++ std | None | None |
| Inbound Audio | pthread, C++ std | None | None |
| Whisper Service | pthread, C++ std | whisper.cpp, CoreML | Whisper model (.bin) |
| LLaMA Service | pthread, C++ std | llama.cpp, CoreML | LLaMA model (.gguf) |
| Kokoro Service | pthread, C++ std | libtorch, CoreML, espeak-ng | Model (.pt), voices (.pt), espeak data |
| Outbound Audio | pthread, C++ std | None | None |

#### 2.4.2 Build Strategy

**Goal**: "Mostly Static" binaries that require only:
- macOS system frameworks (CoreFoundation, Metal, Accelerate)
- Self-contained model files in predictable paths

**CMake Configuration:**
```cmake
# Global static linking preferences
set(CMAKE_FIND_LIBRARY_SUFFIXES .a .dylib)  # Prefer static libs

# Link flags
set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static-libgcc -static-libstdc++")

# Static link whisper.cpp and llama.cpp (rebuild as static)
add_subdirectory(whisper-cpp)
add_subdirectory(llama-cpp)

# Force static library builds
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
```

**whisper.cpp / llama.cpp static builds:**
```bash
# Build whisper.cpp as static library
cd whisper-cpp
cmake -B build -DBUILD_SHARED_LIBS=OFF -DWHISPER_COREML=ON
cmake --build build

# Build llama.cpp as static library
cd llama-cpp
cmake -B build -DBUILD_SHARED_LIBS=OFF -DLLAMA_METAL=ON
cmake --build build
```

**Model Bundling:**
- Store models in `models/` directory alongside binaries
- Use relative paths: `./models/whisper-base.bin`, `./models/llama-3.2-1b-q8.gguf`
- Services check for models at startup, fail gracefully if missing

#### 2.4.3 Distribution Structure

Final deployment directory:
```
whispertalk/
├── bin/
│   ├── sip-client                 (~500 KB)
│   ├── inbound-audio-processor    (~300 KB)
│   ├── whisper-service            (~50 MB, includes libwhisper.a)
│   ├── llama-service              (~80 MB, includes libllama.a)
│   ├── kokoro-service             (~600 MB, includes libtorch)
│   └── outbound-audio-processor   (~300 KB)
├── lib/                           (libtorch, espeak-ng dylibs if needed)
│   ├── libtorch_cpu.dylib
│   ├── libtorch.dylib
│   └── libespeak-ng.dylib
├── models/
│   ├── whisper-base.bin           (~150 MB)
│   ├── llama-3.2-1b-q8.gguf       (~1.3 GB)
│   ├── kokoro_german.pt           (~200 MB)
│   └── voices/
│       ├── df_eva_embedding.pt
│       └── dm_bernd_embedding.pt
└── espeak-ng-data/                (phoneme dictionaries)
```

**Total Size Estimate**: ~2.5 GB (acceptable for local deployment)

---

## 3. Source Code Structure Changes

### 3.1 New Files

**Core Infrastructure:**
- `interconnect.h`: Master/slave interconnection system (header-only, ~800 lines)

**New C++ Services:**
- `kokoro-service.cpp`: C++ port of Python Kokoro service (~600 lines estimated)

**Build System:**
- `export_kokoro_model.py`: One-time script to export Kokoro models to TorchScript

**Utilities:**
- `test_interconnect.cpp`: Unit tests for interconnection system
- `test_kokoro_cpp.cpp`: Validation tests for Kokoro C++ port

### 3.2 Modified Files

**All Existing Services** (6 files):
- `sip-client-main.cpp`: Remove UDP/Unix sockets, integrate `interconnect.h`, add multi-line support
- `inbound-audio-processor.cpp`: Remove UDP/TCP, integrate `interconnect.h`, add call_id registry
- `whisper-service.cpp`: Remove hard-coded TCP, integrate `interconnect.h`, add crash recovery
- `llama-service.cpp`: Remove hard-coded TCP, integrate `interconnect.h`, add interruption logic
- `outbound-audio-processor.cpp`: Remove UDP/Unix sockets, integrate `interconnect.h`

**Build System:**
- `CMakeLists.txt`: Add libtorch, espeak-ng, static linking flags, kokoro-service target

**Documentation:**
- `.zencoder/rules/repo.md`: Update architecture description
- `.zencoder/rules/kokoro-service.md`: Rewrite for C++ implementation
- `.zencoder/rules/interconnection.md`: New doc for interconnect system

### 3.3 Deleted Files

- `kokoro_service.py`: Replaced by `kokoro-service.cpp`
- All Python dependencies (no longer needed)

---

## 4. Data Model / API / Interface Changes

### 4.1 Call ID Management

**Current**: Ad-hoc per service, incremental starting from 1  
**New**: Centralized atomic allocation with collision avoidance

**New Protocol:**
```
SIP Client (creates call):
    local_call_id = last_highest_call_id + 1
    
    SEND to IAP: CHECK_CALL_ID <local_call_id>
    RECV from IAP: HIGHEST_CALL_ID <max_id> OR OK
    
    if (response == HIGHEST_CALL_ID):
        local_call_id = max_id + 1
    
    broadcast to all: CALL_START <local_call_id>
```

**Thread Safety:**
- SIP client serializes call_id negotiation via mutex (REQ-SIP-010)
- IAP maintains atomic max_call_id counter

### 4.2 Packet Format Standardization

**All Traffic Packets:**
```c
struct PacketHeader {
    uint32_t call_id;       // Network byte order
    uint32_t payload_size;  // Network byte order
};
// Followed by `payload_size` bytes of payload
```

**Serialization Helpers:**
```cpp
std::vector<uint8_t> Packet::serialize() const {
    std::vector<uint8_t> data(8 + payload.size());
    uint32_t cid_net = htonl(call_id);
    uint32_t size_net = htonl(size);
    memcpy(&data[0], &cid_net, 4);
    memcpy(&data[4], &size_net, 4);
    memcpy(&data[8], payload.data(), payload.size());
    return data;
}

Packet Packet::deserialize(const uint8_t* data, size_t len) {
    Packet pkt;
    memcpy(&pkt.call_id, data, 4);
    pkt.call_id = ntohl(pkt.call_id);
    memcpy(&pkt.size, data + 4, 4);
    pkt.size = ntohl(pkt.size);
    pkt.payload.assign(data + 8, data + 8 + pkt.size);
    return pkt;
}
```

### 4.3 Service Lifecycle States

**Per-Call State Machine:**
```
IDLE → ACTIVE → TERMINATING → CLEANUP → IDLE
```

**Transitions:**
- `IDLE → ACTIVE`: On receiving first packet for call_id
- `ACTIVE → TERMINATING`: On receiving CALL_END broadcast
- `TERMINATING → CLEANUP`: After flushing buffers, closing threads
- `CLEANUP → IDLE`: Resources freed, ready for new call

**Per-Service State:**
```
STARTING → REGISTERING → CONNECTED → CRASHED → RECONNECTING → CONNECTED
```

**Transitions:**
- `STARTING → REGISTERING`: After port binding
- `REGISTERING → CONNECTED`: After successful master registration
- `CONNECTED → CRASHED`: On heartbeat timeout or socket error
- `CRASHED → RECONNECTING`: On periodic retry timer (every 2s)
- `RECONNECTING → CONNECTED`: On successful reconnection

### 4.4 Crash Recovery Mechanisms

**Upstream Crash (e.g., IAP crashes while Whisper is running):**
1. Whisper detects socket error on recv()
2. Whisper queries master: `GET_UPSTREAM WHISPER_SERVICE`
3. Master returns `SERVICE_UNAVAILABLE` (no heartbeat from IAP)
4. Whisper redirects incoming stream to `/dev/null` (discards data)
5. Every 2s, Whisper queries master again
6. When IAP restarts and re-registers:
   - Master returns IAP's new ports
   - Whisper reconnects to IAP
   - Whisper resumes processing (no call state lost)

**Downstream Crash (e.g., LLaMA crashes while Whisper is running):**
1. Whisper detects socket error on send()
2. Whisper queries master: `GET_DOWNSTREAM WHISPER_SERVICE`
3. Master returns `SERVICE_UNAVAILABLE`
4. Whisper continues transcribing, buffers text (or discards if buffer full)
5. When LLaMA restarts:
   - Whisper reconnects
   - Whisper sends buffered text (or resumes from current transcription)

**Master Crash:**
- All slaves continue operating with existing traffic connections
- New call_id negotiations fail (SIP client rejects new calls)
- No new services can join
- **Mitigation**: Manual master restart required (out of scope: master failover)

---

## 5. Delivery Phases (Incremental Milestones)

### Phase 1: Interconnection System Foundation (3 days)

**Goals:**
- Implement and test `interconnect.h` in isolation
- Validate master/slave port discovery
- Validate heartbeat and crash detection

**Deliverables:**
- `interconnect.h` (header-only library)
- `test_interconnect.cpp` (unit tests)
- Documentation: port allocation, protocol messages

**Tests:**
- Start 6 mock services, verify master election
- Kill master, verify slaves detect loss
- Restart master, verify re-registration
- Simulate packet send/receive between adjacent services

**Acceptance Criteria:**
- 6 services auto-discover ports and register with master
- Heartbeat detects crashed service within 5 seconds
- Packet serialization round-trips correctly

### Phase 2: IPC Migration (All Services) (4 days)

**Goals:**
- Remove all existing IPC code (UDP, Unix sockets, hard-coded TCP)
- Integrate `interconnect.h` into all 6 services
- Maintain functional pipeline (using existing Kokoro Python)

**Deliverables:**
- Modified: `sip-client-main.cpp`, `inbound-audio-processor.cpp`, `whisper-service.cpp`, `llama-service.cpp`, `kokoro_service.py` (temp TCP adapter), `outbound-audio-processor.cpp`
- Each service initializes `InterconnectNode`, connects to neighbors
- Temporary Python Kokoro adapter uses TCP via interconnect ports

**Tests:**
- End-to-end pipeline: SIP call → transcription → LLM response → TTS → audio out
- Multi-call test: 3 concurrent calls with unique call_ids
- Crash test: Kill Whisper, verify IAP redirects to /dev/null and reconnects

**Acceptance Criteria:**
- Single call completes successfully with <1s latency
- 3 concurrent calls maintain correct call_id routing
- Service crash/restart doesn't drop active calls

### Phase 3: Kokoro C++ Port (5 days)

**Goals:**
- Convert Python Kokoro to C++ using libtorch + espeak-ng
- Match Python TTS quality for German
- Integrate into interconnect system

**Deliverables:**
- `kokoro-service.cpp` (new C++ implementation)
- `export_kokoro_model.py` (one-time model export script)
- `test_kokoro_cpp.cpp` (quality validation)
- TorchScript models: `kokoro_german.pt`, voice embeddings

**Tests:**
- Phonemization accuracy: Compare C++ espeak-ng output vs Python
- Audio quality: A/B test C++ vs Python outputs (PESQ/STOI metrics)
- Performance: Latency comparable to Python (~100-200ms per sentence)
- Multi-call: 5 concurrent TTS streams without quality degradation

**Acceptance Criteria:**
- German phonemization matches Python (>95% phoneme accuracy)
- Synthesized audio perceptually indistinguishable from Python (PESQ >4.0)
- C++ version runs without Python runtime
- Concurrent TTS streams process independently

### Phase 4: Static Binary Build System (3 days)

**Goals:**
- Configure CMake for static linking
- Bundle all dependencies
- Verify standalone execution

**Deliverables:**
- Updated `CMakeLists.txt` with static linking flags
- Build scripts for static whisper.cpp / llama.cpp
- Deployment directory structure with bundled libs and models

**Tests:**
- Build on clean macOS machine (no Homebrew/MacPorts)
- Execute binaries with `DYLD_LIBRARY_PATH` unset
- Verify model loading from relative paths
- Size check: total distribution <3 GB

**Acceptance Criteria:**
- All binaries run on fresh macOS install (only system frameworks required)
- `otool -L` shows no dependencies outside `/System/Library` and bundled libs
- Models load successfully from `./models/` directory

### Phase 5: Multi-Call and Crash Resilience Testing (4 days)

**Goals:**
- Validate atomic call_id negotiation
- Validate crash recovery for all services
- Stress test with high call concurrency

**Deliverables:**
- Test suite: `test_multi_call.cpp`, `test_crash_resilience.cpp`
- Automated scripts for chaos testing (random service kills)
- Performance benchmarks: latency, throughput, memory usage

**Tests:**
- **Call ID Collision Test**: 10 SIP lines simultaneously create calls
- **Crash Recovery Matrix**: Kill each service type during active calls
- **Concurrency Stress Test**: 20 concurrent calls for 10 minutes
- **Memory Leak Test**: 100 calls over 1 hour, monitor RSS

**Acceptance Criteria:**
- No call_id collisions in 1000 calls
- Service crashes recover within 5 seconds
- 20 concurrent calls maintain <1.5s average latency
- No memory leaks (RSS growth <5% over 1 hour)

### Phase 6: Performance Optimization and Bug Fixes (3 days)

**Goals:**
- Profile and optimize hotspots
- Fix bugs discovered in testing
- Tune VAD parameters for German

**Deliverables:**
- Profiling reports (Instruments / gprof)
- Optimized code paths (especially in audio processing)
- Final VAD parameter tuning
- Bug fix log

**Tests:**
- Re-run all Phase 5 tests
- Latency benchmarks: target <800ms end-to-end
- CPU usage: <200% per service under load (dual-core utilization)

**Acceptance Criteria:**
- All Phase 5 tests pass
- End-to-end latency <800ms (95th percentile)
- VAD correctly segments German speech (manual review of 50 calls)

---

## 6. Verification Approach

### 6.1 Lint and Type Checking

**C++ Linting:**
- **Tool**: `clang-tidy` with C++17 checks
- **Configuration**: `.clang-tidy` at repository root
- **Command**: `find . -name "*.cpp" -exec clang-tidy {} \;`

**CMake Linting:**
- **Tool**: `cmake-lint`
- **Command**: `cmake-lint CMakeLists.txt`

**Automated Pre-Commit:**
- Use `pre-commit` framework with hooks for clang-format, clang-tidy

### 6.2 Unit Tests

**Frameworks:**
- C++: Google Test (`gtest`)
- Integration: Custom Python scripts (existing `tests/` directory)

**Unit Test Coverage:**
| Component | Test File | Coverage |
|-----------|-----------|----------|
| Interconnect | `test_interconnect.cpp` | Port discovery, packet serialization, heartbeat |
| Kokoro C++ | `test_kokoro_cpp.cpp` | Phonemization, model inference, audio quality |
| Call ID Manager | `test_call_id.cpp` | Atomic allocation, collision avoidance |

**Build and Run:**
```bash
mkdir build && cd build
cmake -DBUILD_TESTS=ON ..
make
ctest --output-on-failure
```

### 6.3 Integration Tests

**Test Scenarios:**
1. **Single Call End-to-End**: SIP INVITE → audio → transcription → LLM → TTS → audio out → BYE
2. **Multi-Call**: 5 concurrent calls with different call_ids
3. **Crash Recovery**: Kill service mid-call, verify reconnection and continuation
4. **Call ID Collision**: Concurrent call_id negotiations from multiple SIP lines

**Existing Tests (to be adapted):**
- `tests/multi_call_test.py`: Adapt for new interconnect ports
- `tests/test_multi_call_vad.py`: Validate VAD with new pipeline

**New Tests:**
- `tests/test_interconnect_integration.py`: Launch all 6 services, verify registration
- `tests/test_crash_recovery.py`: Chaos testing (random kills)

### 6.4 Performance Benchmarks

**Metrics:**
| Metric | Target | Measurement Tool |
|--------|--------|------------------|
| End-to-end latency | <800ms (95th %ile) | Custom timestamping in logs |
| CPU usage per service | <200% (dual-core) | `top`, `htop` |
| Memory per service | <500 MB | `ps`, `htop` |
| Concurrent calls | 20+ | Load generator |

**Benchmarking Tool:**
- `tests/benchmark_pipeline.py`: Automated load testing script

**Profiling:**
- **macOS Instruments**: Time Profiler, Allocations, Network
- **Linux (if cross-platform)**: `perf`, `valgrind --tool=massif`

### 6.5 Acceptance Criteria

**Must-Pass Before Release:**
1. All unit tests pass (100% in `ctest`)
2. Integration tests pass (all scenarios in `tests/`)
3. 1-hour stress test with 5 concurrent calls (no crashes, no memory leaks)
4. German TTS quality verified by native speaker (subjective)
5. Static binaries run on clean macOS install

**Performance Gates:**
- Latency: <800ms (95th percentile)
- Memory: <500 MB per service
- Concurrency: 20 calls without degradation

---

## 7. Dependencies and Tools

### 7.1 Required Libraries

**System (macOS):**
- Xcode Command Line Tools (clang, make)
- CMake 3.22+

**Third-Party (C++):**
- whisper.cpp (existing, rebuild as static)
- llama.cpp (existing, rebuild as static)
- libtorch 2.0+ (download pre-built from PyTorch website)
- espeak-ng (install via Homebrew: `brew install espeak-ng`)
- Google Test (for unit tests): `brew install googletest` or bundled

**Models (to be downloaded/converted):**
- Whisper Base German: `whisper-base-de.bin` (~150 MB)
- LLaMA 3.2 1B Q8: `llama-3.2-1b-instruct-q8_0.gguf` (~1.3 GB)
- Kokoro German Model: `kokoro-german-v1_1-de.pth` (~200 MB)
- Kokoro Voices: `df_eva.pt`, `dm_bernd.pt` (~5 MB each)

### 7.2 Build Environment Setup

**One-Time Setup:**
```bash
# Install system dependencies
brew install cmake espeak-ng googletest

# Download libtorch (macOS arm64)
cd third_party/
wget https://download.pytorch.org/libtorch/cpu/libtorch-macos-arm64-2.0.0.zip
unzip libtorch-macos-arm64-2.0.0.zip

# Build whisper.cpp as static
cd whisper-cpp
cmake -B build -DBUILD_SHARED_LIBS=OFF -DWHISPER_COREML=ON
cmake --build build --config Release

# Build llama.cpp as static
cd ../llama-cpp
cmake -B build -DBUILD_SHARED_LIBS=OFF -DLLAMA_METAL=ON
cmake --build build --config Release

# Export Kokoro models (requires Python one last time)
python3 -m venv venv
source venv/bin/activate
pip install torch kokoro phonemizer
python export_kokoro_model.py
deactivate

# Remove Python environment (no longer needed)
rm -rf venv
```

**Regular Build:**
```bash
mkdir build && cd build
cmake ..
make -j$(sysctl -n hw.ncpu)
```

### 7.3 Deployment Checklist

**Before Distribution:**
1. Run full test suite: `ctest --output-on-failure`
2. Verify static linking: `otool -L bin/*`
3. Package directory structure (bin, lib, models, espeak-ng-data)
4. Test on clean macOS VM (no Homebrew)
5. Document model download instructions

**Runtime Environment:**
- macOS 12.0+ (Monterey or later)
- Apple Silicon (arm64) or Intel (x86_64 with Rosetta)
- 4 GB RAM minimum (8 GB recommended for concurrent calls)
- 5 GB disk space (for binaries + models)

---

## 8. Risk Mitigation

### 8.1 Technical Risks

| Risk | Impact | Mitigation | Status |
|------|--------|------------|--------|
| libtorch static linking fails | High | Use dynamic linking, bundle .dylib files | Accepted |
| espeak-ng phonemization differs from Python | High | Extensive testing, manual tuning of espeak rules | Monitored |
| Kokoro C++ audio quality degraded | Critical | A/B testing, fallback to Python temporarily | Blocking |
| Port conflicts with system services | Medium | Allow configurable base ports via env vars | Planned |
| Master crash orphans system | Medium | Document manual restart, defer failover to future | Accepted |
| Static binaries too large (>5 GB) | Low | Use compression, strip debug symbols | Monitored |

### 8.2 Schedule Risks

**20-day estimate may slip due to:**
1. Kokoro C++ port complexity (espeak-ng integration)
2. libtorch build issues on macOS
3. Unforeseen bugs in interconnect system

**Mitigation:**
- Buffer 5 extra days for unknowns
- Prioritize core functionality over optimizations
- Defer non-critical features (e.g., master failover)

### 8.3 Quality Risks

**German TTS quality is subjective:**
- Mitigation: Involve native German speaker in testing
- Acceptance: PESQ >4.0 (objective), manual approval (subjective)

**Crash recovery may have edge cases:**
- Mitigation: Extensive chaos testing in Phase 5
- Acceptance: 95% success rate in automated crash tests

---

## 9. Open Questions (from PRD)

**Resolved for Spec:**

**Q1**: Kokoro phonemization approach?  
**A1**: Use espeak-ng C API (best balance of quality and integration effort)

**Q2**: Master failover support?  
**A2**: Not in this phase; manual restart is acceptable

**Q3**: Static linking approach?  
**A3**: Hybrid: static link whisper/llama, dynamic link libtorch/CoreML (bundle dylibs)

**Q4**: Persist LLaMA conversation state?  
**A4**: No persistence; context lost on crash is acceptable

**Q5**: Packet size negotiation per-service or per-call?  
**A5**: Per-service-type (fixed once during registration)

**Q6**: German voices required?  
**A6**: `df_eva` (female) and `dm_bernd` (male) as in Python version

**Q7**: Configuration files vs command-line args?  
**A7**: Command-line args for simplicity; config files deferred to future

---

## 10. Success Metrics

**The implementation is successful when:**

1. ✅ All 6 services compile to static/mostly-static binaries
2. ✅ No Python runtime required at execution time
3. ✅ Interconnection system auto-discovers services and handles 20+ concurrent calls
4. ✅ All services survive crashes of neighbors without dropping calls
5. ✅ End-to-end latency <800ms (95th percentile)
6. ✅ German TTS quality matches Python Kokoro (PESQ >4.0)
7. ✅ 1-hour stress test with 5 calls completes without crashes or leaks
8. ✅ All unit and integration tests pass

---

## 11. Appendix: Reference Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                        SIP Network (UDP)                        │
└────────────────────────────┬────────────────────────────────────┘
                             │ RTP (G.711)
                             ▼
                    ┌─────────────────┐
                    │  SIP Client     │ (Master, ports 22222/33333)
                    │  - Multi-line   │
                    │  - Call ID nego │
                    └────┬───────▲────┘
                         │       │ Traffic ports (TCP)
        ┌────────────────┘       └────────────────┐
        │ UpOut (33335)              DownIn (22223)│
        ▼                                          │
┌───────────────────┐                      ┌──────┴────────────┐
│ Inbound Audio     │                      │ Outbound Audio    │
│ Processor         │                      │ Processor         │
│ (22225/33336)     │                      │ (22237/33348)     │
│ - G.711 decode    │                      │ - G.711 encode    │
│ - 8→16kHz         │                      │ - 24→8kHz         │
└──────┬────────────┘                      └───────▲───────────┘
       │ UpOut (33338)                        DownIn (22238)
       ▼                                            │
┌──────────────────┐                         ┌─────┴───────────┐
│ Whisper Service  │                         │ Kokoro Service  │
│ (22228/33339)    │                         │ (C++, libtorch) │
│ - VAD (100ms)    │                         │ (22234/33345)   │
│ - ASR (German)   │                         │ - espeak-ng     │
└──────┬───────────┘                         └────────▲────────┘
       │ UpOut (33341)                           DownIn (22235)
       ▼                                               │
┌──────────────────────────────────────────────────────┘
│ LLaMA Service (22231/33342)
│ - Conversational AI (German)
│ - Interruption support
│ - Multi-call contexts
└───────────────────────────────────────────────────────┘

All connections: TCP via InterconnectNode (interconnect.h)
Master: Manages service registry, broadcasts CALL_END
```

---

**Document Version**: 1.0 FINAL  
**Approved for Planning Phase**: 2026-02-13  
**Next Step**: Create detailed implementation plan in `plan.md`
