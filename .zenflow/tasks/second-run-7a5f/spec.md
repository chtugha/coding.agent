# Technical Specification: WhisperTalk Standalone Architecture Redesign

**Version**: 2.0  
**Date**: 2026-02-13  
**Status**: REVISED (addresses review feedback)

---

## 1. Technical Context

### 1.1 Languages and Frameworks

**Primary Languages:**
- **C++17**: All services (including new Kokoro C++ port)
- **Build System**: CMake 3.22+
- **Target Platform**: macOS (Apple Silicon, arm64)

**Core Dependencies:**
- **whisper.cpp**: ASR engine with CoreML acceleration (existing)
- **llama.cpp**: LLM engine with Metal/MPS acceleration (must be cloned separately)
- **libtorch** (PyTorch C++ API): For Kokoro TTS C++ port (new)
- **espeak-ng**: For German phonemization (new, via C API)
- **Google Test**: Unit testing framework
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
1. Mixed IPC protocols (UDP, TCP, Unix sockets) -> inconsistent error handling
2. Python dependency -> prevents standalone deployment
3. Hard-coded ports -> limits scalability, causes conflicts
4. No central orchestration -> ad-hoc crash recovery
5. Incomplete reconnection logic -> fragile connections
6. Variable multi-call robustness across services

---

## 2. Implementation Approach

### 2.1 Architecture Overview

The redesign transforms WhisperTalk into a **unified master/slave TCP-based pipeline** with automatic service discovery, crash recovery, and consistent multi-call support.

**Key Design Principles:**
1. **Single IPC Protocol**: TCP-only for all inter-service communication
2. **Standalone Binaries**: Static linking where possible, minimal dynamic dependencies
3. **Master/Slave Orchestration**: Automatic service discovery, registration, and crash detection
4. **Crash Resilience**: All connections aware of partner status, automatic reconnection via master
5. **Call ID Tracking**: Consistent multi-call support with atomic call_id reservation
6. **Native C++ Stack**: Eliminate Python dependency via libtorch-based Kokoro port

### 2.2 Interconnection System Design

#### 2.2.1 Core Abstraction: `interconnect.h`

A single header-only library included in all services:

```cpp
namespace whispertalk {

struct PortConfig {
    uint16_t neg_in;      // Negotiation incoming (22222+3n)
    uint16_t neg_out;     // Negotiation outgoing (33333+3n)
    uint16_t down_in;     // Downstream listen port (neg_in+1) -- upstream neighbor connects here to send TO us
    uint16_t down_out;    // Downstream listen port (neg_in+2) -- upstream neighbor connects here to recv FROM us
    uint16_t up_in;       // Upstream connect port (neg_out+1) -- we connect to downstream neighbor's down_out
    uint16_t up_out;      // Upstream connect port (neg_out+2) -- we connect to downstream neighbor's down_in
};

enum class ServiceType {
    SIP_CLIENT,
    INBOUND_AUDIO_PROCESSOR,
    WHISPER_SERVICE,
    LLAMA_SERVICE,
    KOKORO_SERVICE,
    OUTBOUND_AUDIO_PROCESSOR
};

struct Packet {
    uint32_t call_id;
    uint32_t size;
    std::vector<uint8_t> payload;
    static Packet deserialize(const uint8_t* data, size_t len);
    std::vector<uint8_t> serialize() const;
};

enum class ConnectionState { DISCONNECTED, CONNECTING, CONNECTED, FAILED };
enum class ServiceState { STARTING, REGISTERING, CONNECTED, CRASHED, RECONNECTING };

class InterconnectNode {
public:
    InterconnectNode(ServiceType type);
    ~InterconnectNode();

    bool init();
    bool is_master() const;
    void stop();

    // Master-only: service registry
    void register_slave(ServiceType type, const PortConfig& ports);
    PortConfig get_upstream_service(ServiceType requester) const;
    PortConfig get_downstream_service(ServiceType requester) const;

    // Traffic connections
    bool connect_to_upstream();    // Connects to downstream neighbor's listen ports
    bool accept_from_downstream(); // Listens for upstream neighbor's connections

    // Monodirectional send/recv (thread-safe, non-blocking with timeout)
    bool send_to_downstream(const Packet& pkt);    // Sends on down_out accepted socket
    bool recv_from_downstream(Packet& pkt, int timeout_ms = 100);  // Recvs on down_in accepted socket
    bool send_to_upstream(const Packet& pkt);       // Sends on up_out connected socket
    bool recv_from_upstream(Packet& pkt, int timeout_ms = 100);    // Recvs on up_in connected socket

    // Call lifecycle (atomic reservation)
    uint32_t reserve_call_id(uint32_t proposed_id);
    void broadcast_call_end(uint32_t call_id);
    void register_call_end_handler(std::function<void(uint32_t)> handler);

    // Health monitoring
    void start_heartbeat();
    bool is_service_alive(ServiceType type) const;
    ConnectionState upstream_state() const;
    ConnectionState downstream_state() const;

private:
    ServiceType type_;
    bool is_master_;
    PortConfig ports_;
    std::atomic<bool> running_{true};

    // Downstream sockets: this service LISTENS, upstream neighbor CONNECTS
    int down_in_listen_sock_;   // bind+listen on neg_in+1
    int down_out_listen_sock_;  // bind+listen on neg_in+2
    int down_in_accepted_;      // accepted fd (we RECV data from upstream neighbor on this)
    int down_out_accepted_;     // accepted fd (we SEND data to upstream neighbor on this)

    // Upstream sockets: this service CONNECTS to downstream neighbor's listen ports
    int up_in_sock_;   // connect to downstream neighbor's down_out (their neg_in+2)
    int up_out_sock_;  // connect to downstream neighbor's down_in (their neg_in+1)

    ConnectionState upstream_state_{ConnectionState::DISCONNECTED};
    ConnectionState downstream_state_{ConnectionState::DISCONNECTED};

    std::map<ServiceType, PortConfig> service_registry_;
    std::map<ServiceType, std::chrono::steady_clock::time_point> last_heartbeat_;
    std::set<uint32_t> reserved_call_ids_;
    uint32_t max_known_call_id_{0};

    std::function<void(uint32_t)> call_end_handler_;

    std::mutex registry_mutex_;
    std::mutex call_id_mutex_;
    std::mutex send_upstream_mutex_;
    std::mutex send_downstream_mutex_;

    std::thread heartbeat_thread_;
    std::thread negotiation_thread_;
    std::thread reconnect_thread_;

    PortConfig scan_and_bind_ports();
    void heartbeat_loop();
    void negotiation_loop();
    void reconnect_loop();
};

} // namespace whispertalk
```

#### 2.2.2 Port Allocation Strategy

**Base Ports:**
- Negotiation incoming: 22222 (master), 22225, 22228, 22231, 22234, 22237, ...
- Negotiation outgoing: 33333 (master), 33336, 33339, 33342, 33345, 33348, ...

**Traffic Ports** (derived from negotiation ports, 6 total ports per service):

| Port | Formula | Role | Direction |
|------|---------|------|-----------|
| down_in | neg_in + 1 | Listen for upstream neighbor to send data TO us | Incoming data |
| down_out | neg_in + 2 | Listen for upstream neighbor to recv data FROM us | Outgoing data |
| up_in | neg_out + 1 | Connect to downstream neighbor's down_out to recv FROM them | Incoming data |
| up_out | neg_out + 2 | Connect to downstream neighbor's down_in to send TO them | Outgoing data |

**Port Discovery Algorithm:**
1. Attempt `bind()` on ports 22222 and 33333
2. If both succeed -> become **Master**
3. If either fails with `EADDRINUSE` -> increment both by 3 and retry
4. `bind()` is atomic at OS level -- no TOCTOU race condition. If two services race for the same port, one gets `EADDRINUSE` and moves to the next pair automatically.
5. After binding a non-master pair -> become **Slave** -> register with master at 22222/33333
6. Maximum scan range: 100 increments (ports up to 22522/33633). Fail with error if exhausted.
7. After binding negotiation ports, immediately bind the 4 traffic ports (neg_in+1, neg_in+2 for listening; neg_out+1, neg_out+2 are connected later).

**Port Layout (all 6 services):**

| Service | NegIn | NegOut | down_in (L) | down_out (L) | up_in (C) | up_out (C) |
|---------|-------|--------|-------------|--------------|-----------|------------|
| SIP Client (Master) | 22222 | 33333 | 22223 | 22224 | 33334 | 33335 |
| Inbound Audio Proc | 22225 | 33336 | 22226 | 22227 | 33337 | 33338 |
| Whisper Service | 22228 | 33339 | 22229 | 22230 | 33340 | 33341 |
| LLaMA Service | 22231 | 33342 | 22232 | 22233 | 33343 | 33344 |
| Kokoro Service | 22234 | 33345 | 22235 | 22236 | 33346 | 33347 |
| Outbound Audio Proc | 22237 | 33348 | 22238 | 22239 | 33349 | 33350 |

(L) = Listening socket, (C) = Connecting socket

#### 2.2.3 Connection Topology

**Pipeline direction** (primary data flow):
```
SIP Client -> IAP -> Whisper -> LLaMA -> Kokoro -> OAP -> SIP Client (loop)
```

"Upstream" = closer to the data source (SIP-in). "Downstream" = closer to the data sink (SIP-out).

**Rule**: The **upstream** service initiates TCP connections to the **downstream** service's listening ports (`down_in`, `down_out`). Each TCP connection is **monodirectional** at the application level.

**Connection 1** (primary data flow): Upstream service connects its `up_out` to downstream service's `down_in`. The upstream service **sends** on this connection, the downstream service **receives**.

**Connection 2** (reverse channel): Upstream service connects its `up_in` to downstream service's `down_out`. The downstream service **sends** on this connection, the upstream service **receives**.

**Complete Connection Matrix:**

| # | Upstream Svc | Downstream Svc | Upstream connects | Downstream listens | Sender | Receiver | Content |
|---|-------------|---------------|-------------------|-------------------|--------|----------|---------|
| 1 | SIP | IAP | SIP up_out (33335) -> | IAP down_in (22226) | SIP | IAP | RTP frames (G.711) |
| 2 | SIP | IAP | SIP up_in (33334) -> | IAP down_out (22227) | IAP | SIP | Ack/backpressure |
| 3 | IAP | Whisper | IAP up_out (33338) -> | Whisper down_in (22229) | IAP | Whisper | Float32 PCM (16kHz) |
| 4 | IAP | Whisper | IAP up_in (33337) -> | Whisper down_out (22230) | Whisper | IAP | Ack/backpressure |
| 5 | Whisper | LLaMA | Whisper up_out (33341) -> | LLaMA down_in (22232) | Whisper | LLaMA | UTF-8 text (transcribed) |
| 6 | Whisper | LLaMA | Whisper up_in (33340) -> | LLaMA down_out (22233) | LLaMA | Whisper | Interrupt signal |
| 7 | LLaMA | Kokoro | LLaMA up_out (33344) -> | Kokoro down_in (22235) | LLaMA | Kokoro | UTF-8 text (response) |
| 8 | LLaMA | Kokoro | LLaMA up_in (33343) -> | Kokoro down_out (22236) | Kokoro | LLaMA | Ack |
| 9 | Kokoro | OAP | Kokoro up_out (33347) -> | OAP down_in (22238) | Kokoro | OAP | Float32 PCM (24kHz) |
| 10 | Kokoro | OAP | Kokoro up_in (33346) -> | OAP down_out (22239) | OAP | Kokoro | Ack |
| 11 | OAP | SIP | OAP up_out (33350) -> | SIP down_in (22223) | OAP | SIP | G.711 frames |
| 12 | OAP | SIP | OAP up_in (33349) -> | SIP down_out (22224) | SIP | OAP | Ack |

**Verification example (IAP <-> Whisper, connections 3 & 4):**
- IAP: neg_in=22225, neg_out=33336 -> up_out=33338, up_in=33337
- Whisper: neg_in=22228, neg_out=33339 -> down_in=22229, down_out=22230
- Connection 3: IAP connects from up_out(33338) to Whisper down_in(22229). IAP sends PCM, Whisper receives.
- Connection 4: IAP connects from up_in(33337) to Whisper down_out(22230). Whisper sends ack, IAP receives.

#### 2.2.4 Protocol Design

**Negotiation Protocol** (via negotiation ports):

| Message | Direction | Purpose |
|---------|-----------|---------|
| `REGISTER <type> <neg_in> <neg_out>` | Slave -> Master | Service registration |
| `REGISTER_ACK` | Master -> Slave | Registration confirmed |
| `HEARTBEAT <type> <call_count> <state>` | Slave -> Master | Alive signal (every 2s) |
| `HEARTBEAT_ACK` | Master -> Slave | Master alive confirmation |
| `GET_UPSTREAM <type>` | Service -> Master | Query upstream neighbor's ports |
| `GET_DOWNSTREAM <type>` | Service -> Master | Query downstream neighbor's ports |
| `PORTS <neg_in> <neg_out>` | Master -> Service | Response with neighbor's ports |
| `SERVICE_UNAVAILABLE` | Master -> Service | Neighbor not registered/crashed |
| `RESERVE_CALL_ID <proposed_id>` | SIP Client -> IAP | Atomically reserve a call_id |
| `CALL_ID_RESERVED <final_id>` | IAP -> SIP Client | Confirmed reserved call_id |
| `CALL_END <call_id>` | Any -> Master | Request call termination broadcast |
| `CALL_END <call_id>` | Master -> All Slaves | Broadcast call termination |
| `CALL_END_ACK <call_id>` | Slave -> Master | Confirm call cleanup complete |
| `SET_PACKET_SIZE <size>` | Downstream -> Upstream (via negotiation) | Advertise required packet size |
| `PACKET_SIZE_ACK <size>` | Upstream -> Downstream | Confirm packet size accepted |
| `SERVICE_CRASHED <type>` | Master -> Affected neighbors | Notify of neighbor crash |

**Traffic Protocol** (via traffic ports):

All packets follow this structure:
```
[4 bytes: call_id (network byte order)]
[4 bytes: payload_size (network byte order)]
[N bytes: payload]
```

Payload types per service pair:
- **SIP -> IAP**: RTP frames (G.711 encoded audio, 160 bytes per frame)
- **IAP -> Whisper**: Float32 PCM audio (16kHz, mono)
- **Whisper -> LLaMA**: UTF-8 text (transcribed sentences)
- **LLaMA -> Kokoro**: UTF-8 text (generated responses)
- **Kokoro -> OAP**: Float32 PCM audio (24kHz, mono)
- **OAP -> SIP**: G.711 encoded audio (160 bytes per frame)

#### 2.2.5 InterconnectNode Implementation Details

**Thread Safety:**
- `send_to_downstream()` and `send_to_upstream()` are each protected by their own mutex (`send_downstream_mutex_`, `send_upstream_mutex_`). Multiple threads can send to different directions concurrently.
- `recv_from_downstream()` and `recv_from_upstream()` use `poll()` with timeout. Only one thread should call each recv method (no internal mutex; caller must ensure single-reader).
- `reserve_call_id()` is protected by `call_id_mutex_`.
- Service registry operations are protected by `registry_mutex_`.

**Blocking Behavior:**
- `send_*()` methods: Non-blocking with internal retry. If the socket buffer is full, returns `false` after 100ms. Caller should discard or buffer.
- `recv_*()` methods: Block up to `timeout_ms` using `poll()`. Return `false` on timeout, `true` on data received.
- `init()`: Blocks during port scanning (< 1s typically).
- `connect_to_upstream()`: Blocks with 5s connect timeout. Returns `false` on failure.

**Buffer Management:**
- No internal buffering in InterconnectNode. Each service manages its own buffers.
- If a send fails (downstream slow/disconnected), the service is responsible for discarding data or buffering locally.
- Recommended per-service buffer: 64 packets (configurable). On overflow, oldest packets are dropped.

**Connection State Machine:**
```
DISCONNECTED -> CONNECTING -> CONNECTED -> FAILED -> DISCONNECTED (retry)
                                 ^                        |
                                 |________________________|
                                    (reconnect_loop, every 2s)
```

**Graceful Shutdown:**
1. `stop()` sets `running_ = false`
2. All background threads (`heartbeat_thread_`, `negotiation_thread_`, `reconnect_thread_`) check `running_` and exit their loops
3. All sockets are closed with `shutdown(SHUT_RDWR)` then `close()`
4. `~InterconnectNode()` joins all threads (blocks until they exit, max 3s via timed join)

**Heartbeat Thread Lifecycle:**
- Started by `start_heartbeat()` after `init()` succeeds
- Runs every 2s: sends `HEARTBEAT` to master, checks `last_heartbeat_` timestamps
- Master detects crash when heartbeat missing >5s, sends `SERVICE_CRASHED` to affected neighbors
- Thread exits when `running_ == false`

#### 2.2.6 Service Startup and Initialization

**Startup Order**: Services can start in **any order**. The first service to start becomes Master. All other services become Slaves.

**Initialization Sequence:**
1. Service calls `init()` -> scans ports, binds, determines master/slave role
2. If Slave: connects to master at 22222, sends `REGISTER`
3. If Master not reachable: retry every 2s until master is found (log warning)
4. After registration: calls `start_heartbeat()`
5. Queries master for upstream/downstream neighbor ports via `GET_UPSTREAM`/`GET_DOWNSTREAM`
6. If neighbor not yet registered: wait and retry every 2s (service operates in degraded mode)
7. Once neighbor ports known: establish traffic connections
8. If neighbor not yet listening: retry connection every 2s
9. Service enters CONNECTED state, ready to process data

**Degraded Mode**: A service that cannot reach its neighbors continues to run, discards incoming data to `/dev/null`, and retries connections every 2s.

### 2.3 Call ID Management

**Atomic Reservation Protocol** (replaces CHECK_CALL_ID):

```
SIP Client (any SIP line thread):
    mutex_lock(call_id_mutex)  // Local mutex in SIP client serializes all lines

    proposed_id = max_known_call_id + 1

    SEND to IAP via negotiation: RESERVE_CALL_ID <proposed_id>
    RECV from IAP: CALL_ID_RESERVED <final_id>

    // IAP atomically checks and reserves:
    //   if proposed_id > max_known: reserve it, return proposed_id
    //   if proposed_id <= max_known: reserve max_known+1, return max_known+1

    max_known_call_id = final_id

    mutex_unlock(call_id_mutex)

    // Use final_id for this call
    broadcast CALL_START <final_id> to all services via master
```

This eliminates the race condition where two SIP lines could get the same call_id, because:
1. Local mutex ensures only one SIP line negotiates at a time
2. IAP atomically reserves and returns the final ID in a single request/response

**CALL_END Propagation Protocol:**

```
1. SIP Client detects call end (BYE received/sent)
2. SIP Client sends CALL_END <call_id> to Master (itself, usually)
3. Master broadcasts CALL_END <call_id> to ALL slaves via their negotiation ports
4. Each slave:
   a. Flushes remaining buffered data for call_id
   b. Closes per-call threads and frees resources
   c. Sends CALL_END_ACK <call_id> to Master
5. Master waits up to 5s for all ACKs
6. Missing ACKs are logged as warnings (service may be crashed)
7. Duplicate CALL_END for same call_id: idempotent, ACK immediately if already cleaned up
8. CALL_END received before final data packets: service flushes buffers first, then ACKs
```

### 2.4 Kokoro C++ Port (Python -> C++)

#### 2.4.1 Current Python Architecture

**Key Components:**
1. **Kokoro Model**: PyTorch-based neural TTS
2. **Phonemizer**: Text -> IPA phonemes (uses `phonemizer` Python package -> espeak-ng)
3. **Inference**: MPS-accelerated model forward pass
4. **Voice Models**: German voices (`df_eva`, `dm_bernd`)

#### 2.4.2 C++ Port Strategy

**Approach**: Use **libtorch** (PyTorch C++ API) + **espeak-ng C API**

| Python Component | C++ Equivalent | Implementation |
|------------------|----------------|----------------|
| `KPipeline` | `KokoroPipeline` class | Load model via `torch::jit::load()` |
| `KModel.forward()` | `torch::Tensor forward()` | libtorch inference on MPS device |
| `phonemizer.phonemize()` | `espeak_ng_Synthesize()` | Direct espeak-ng C API calls |
| Voice loading | `torch::load()` | Load `.pth` voice embeddings |
| MPS device | `torch::Device(torch::kMPS)` | Metal acceleration via libtorch |

**Model Loading**: TorchScript (preferred). Export Python model to `.pt` via `torch.jit.script()`, load in C++ via `torch::jit::load()`.

**Phonemization Pipeline:**
```
Text -> espeak-ng (German rules) -> IPA phonemes -> Kokoro model -> Audio
```

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
    espeak_SetVoiceByName("de");
    int flags = espeakCHARS_UTF8 | espeakPHONEMES_IPA;
    const char* phonemes = espeak_TextToPhonemes(
        (const void**)&text.c_str(), espeakCHARS_UTF8, flags);
    return parse_ipa(phonemes);
}
```

**Model Export (one-time, from Python):**
```python
import torch
from kokoro.model import KModel

model = KModel(config="config.json", model="kokoro-german-v1_1-de.pth")
model.eval()
scripted_model = torch.jit.script(model)
scripted_model.save("kokoro_german.pt")

voice = torch.load("voices/df_eva.pt")
torch.save(voice, "df_eva_embedding.pt")
```

#### 2.4.3 Phonemization Validation and Fallback

**Risk**: espeak-ng C API may produce different IPA output than Python's `phonemizer` package, causing degraded audio quality.

**Validation Plan:**
1. Create a "phoneme diff" tool that runs the same 500 German sentences through both Python phonemizer and C++ espeak-ng, compares outputs
2. Target: >95% phoneme match rate
3. Document all mismatches and categorize (stress marks, syllable boundaries, vowel variants)

**Fallback Strategy:**
- If phoneme mismatch rate >5%: add a phoneme normalization layer in C++ that maps espeak-ng output to the format expected by the Kokoro model
- If PESQ score <3.5 after normalization: pin espeak-ng to the exact same version used by Python phonemizer and rebuild
- If quality still insufficient: create a pre-computed phoneme dictionary for the 10,000 most common German words, fall back to espeak-ng only for OOV words

#### 2.4.4 Build System Integration

```cmake
set(CMAKE_PREFIX_PATH "/path/to/libtorch")
find_package(Torch REQUIRED)
find_library(ESPEAK_NG_LIB espeak-ng REQUIRED)

add_executable(kokoro-service kokoro-service.cpp)
target_link_libraries(kokoro-service PRIVATE
    Threads::Threads ${TORCH_LIBRARIES} ${ESPEAK_NG_LIB})
set_property(TARGET kokoro-service PROPERTY CXX_STANDARD 17)
```

### 2.5 Static Binary Strategy

#### 2.5.1 Dependency Analysis

| Service | Static-Linkable | Must Be Dynamic | Bundled Assets |
|---------|-----------------|-----------------|----------------|
| SIP Client | pthread, C++ std | None | None |
| Inbound Audio | pthread, C++ std | None | None |
| Whisper Service | pthread, C++ std, libwhisper.a | CoreML frameworks | Whisper model (.bin) |
| LLaMA Service | pthread, C++ std, libllama.a | CoreML, Metal frameworks | LLaMA model (.gguf) |
| Kokoro Service | pthread, C++ std | libtorch (.dylib), CoreML, espeak-ng (.dylib) | Model (.pt), voices (.pt), espeak data |
| Outbound Audio | pthread, C++ std | None | None |

#### 2.5.2 Hybrid Binary Approach

**Goal**: "Mostly static" binaries. Static link standard libs, bundle necessary `.dylib` files.

**What is truly static**: SIP Client, IAP, OAP (no external dynamic deps beyond system frameworks).

**What requires dynamic libs**: Whisper (CoreML), LLaMA (CoreML+Metal), Kokoro (libtorch+espeak-ng).

**Runtime dependency management:**
- Bundle libtorch `.dylib` files in `lib/` directory
- Bundle espeak-ng `.dylib` and phoneme data files in `lib/` and `espeak-ng-data/`
- Use `@rpath` for libtorch: set `install_name_tool -add_rpath @executable_path/../lib` on kokoro-service binary
- CoreML and Metal frameworks: always present on macOS, no bundling needed
- Provide optional shell wrapper script (`run.sh`) that sets `DYLD_LIBRARY_PATH` as fallback

**CMake Configuration:**
```cmake
set(CMAKE_FIND_LIBRARY_SUFFIXES .a .dylib)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

add_subdirectory(whisper-cpp)

# llama.cpp must be cloned first (see Section 7.2)
add_subdirectory(llama-cpp)
```

**espeak-ng data bundling:**
- Copy `/opt/homebrew/share/espeak-ng-data/` to `espeak-ng-data/` in distribution
- Set `ESPEAK_DATA_PATH` environment variable at runtime, or call `espeak_ng_InitializePath("./espeak-ng-data")`

#### 2.5.3 Distribution Structure

```
whispertalk/
├── bin/
│   ├── sip-client                 (~500 KB)
│   ├── inbound-audio-processor    (~300 KB)
│   ├── whisper-service            (~50 MB, includes libwhisper.a)
│   ├── llama-service              (~80 MB, includes libllama.a)
│   ├── kokoro-service             (~600 MB, includes libtorch)
│   └── outbound-audio-processor   (~300 KB)
├── lib/
│   ├── libtorch_cpu.dylib
│   ├── libtorch.dylib
│   ├── libc10.dylib
│   └── libespeak-ng.dylib
├── models/
│   ├── whisper-base.bin           (~150 MB)
│   ├── llama-3.2-1b-q8.gguf      (~1.3 GB)
│   ├── kokoro_german.pt           (~200 MB)
│   └── voices/
│       ├── df_eva_embedding.pt
│       └── dm_bernd_embedding.pt
├── espeak-ng-data/                (phoneme dictionaries, ~15 MB)
│   ├── de_dict
│   ├── phondata
│   └── ...
└── run.sh                         (optional wrapper: sets DYLD_LIBRARY_PATH)
```

**Total Size Estimate**: ~2.5 GB

---

## 3. Source Code Structure Changes

### 3.1 New Files

- `interconnect.h`: Master/slave interconnection system (header-only, ~800 lines)
- `kokoro-service.cpp`: C++ port of Python Kokoro service (~600 lines)
- `export_kokoro_model.py`: One-time script to export Kokoro models to TorchScript
- `test_interconnect.cpp`: Unit tests for interconnection system
- `test_kokoro_cpp.cpp`: Validation tests for Kokoro C++ port
- `test_call_id.cpp`: Unit tests for atomic call_id reservation

### 3.2 Modified Files

- `sip-client-main.cpp`: Remove UDP/Unix sockets, integrate `interconnect.h`, add multi-line support, use RESERVE_CALL_ID
- `inbound-audio-processor.cpp`: Remove UDP/TCP, integrate `interconnect.h`, add call_id registry
- `whisper-service.cpp`: Remove hard-coded TCP, integrate `interconnect.h`, add crash recovery
- `llama-service.cpp`: Remove hard-coded TCP, integrate `interconnect.h`, add interruption logic
- `outbound-audio-processor.cpp`: Remove UDP/Unix sockets, integrate `interconnect.h`
- `CMakeLists.txt`: Add libtorch, espeak-ng, static linking flags, kokoro-service target, test targets

### 3.3 Deleted Files

- `kokoro_service.py`: Replaced by `kokoro-service.cpp`

---

## 4. Data Model / API / Interface Changes

### 4.1 Packet Format Standardization

**All Traffic Packets:**
```c
struct PacketHeader {
    uint32_t call_id;       // Network byte order
    uint32_t payload_size;  // Network byte order
};
// Followed by `payload_size` bytes of payload
```

**Serialization:**
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

### 4.2 Service Lifecycle States

**Per-Call State Machine:**
```
IDLE -> ACTIVE -> TERMINATING -> CLEANUP -> IDLE
```
- `IDLE -> ACTIVE`: On receiving first packet for call_id
- `ACTIVE -> TERMINATING`: On receiving CALL_END broadcast
- `TERMINATING -> CLEANUP`: After flushing buffers
- `CLEANUP -> IDLE`: Resources freed

**Per-Service State:**
```
STARTING -> REGISTERING -> CONNECTED -> CRASHED -> RECONNECTING -> CONNECTED
```
- `STARTING -> REGISTERING`: After port binding
- `REGISTERING -> CONNECTED`: After successful master registration + neighbor connections
- `CONNECTED -> CRASHED`: On heartbeat timeout or socket error
- `CRASHED -> RECONNECTING`: On periodic retry timer (every 2s)
- `RECONNECTING -> CONNECTED`: On successful reconnection

### 4.3 Crash Recovery Mechanisms

**Upstream Crash (e.g., IAP crashes while Whisper is running):**
1. Whisper detects socket error on `recv()`
2. Whisper sets `downstream_state_ = DISCONNECTED`
3. Whisper queries master: `GET_UPSTREAM WHISPER_SERVICE`
4. Master returns `SERVICE_UNAVAILABLE` (no heartbeat from IAP)
5. Whisper discards any incoming data (effectively `/dev/null`)
6. `reconnect_loop()` retries every 2s
7. When IAP restarts and re-registers, master returns IAP's new ports
8. Whisper reconnects to IAP, resumes processing

**Downstream Crash (e.g., LLaMA crashes while Whisper is running):**
1. Whisper detects socket error on `send()`
2. Whisper sets `upstream_state_ = DISCONNECTED`
3. Whisper continues transcribing, discards output (or buffers up to 64 packets)
4. `reconnect_loop()` retries every 2s
5. When LLaMA restarts, Whisper reconnects and resumes sending

**Master Crash:**
- All slaves continue with existing traffic connections
- New call_id negotiations fail (SIP client rejects new calls with busy signal)
- No new services can join
- Master must be manually restarted (master failover is out of scope)

### 4.4 Error Handling and Recovery

**TCP Connection Errors:**

| Error | Detection | Recovery |
|-------|-----------|----------|
| `ECONNREFUSED` | `connect()` returns -1 | Retry in 2s via `reconnect_loop()` |
| `ECONNRESET` | `recv()`/`send()` returns -1 | Mark connection FAILED, notify master, retry |
| `ETIMEDOUT` | `poll()` timeout | Log warning, retry |
| `EPIPE` / `SIGPIPE` | `send()` returns -1 | Ignore SIGPIPE (set `SO_NOSIGPIPE`), mark FAILED, retry |
| `EADDRINUSE` | `bind()` returns -1 | Increment port by 3, retry next pair |

**Malformed Packets:**

| Error | Detection | Recovery |
|-------|-----------|----------|
| payload_size > 1MB | Check after deserializing header | Log error, discard packet, read next |
| call_id = 0 | Check after deserializing | Log error, discard packet |
| Incomplete read | `recv()` returns fewer bytes than expected | Buffer partial data, wait for remainder (up to 5s timeout) |
| Connection closed mid-packet | `recv()` returns 0 | Discard partial, mark connection FAILED |

**Resource Exhaustion:**

| Error | Detection | Recovery |
|-------|-----------|----------|
| Too many calls (>100) | call_id counter check | Reject new calls with BUSY, log warning |
| Out of memory | `std::bad_alloc` | Log critical error, gracefully shut down service |
| File descriptor limit | `accept()`/`socket()` returns EMFILE | Log error, reject new connections, continue serving existing |

### 4.5 Logging Strategy

**Log Format** (structured, to stderr):
```
[YYYY-MM-DD HH:MM:SS.mmm] [LEVEL] [SERVICE] [COMPONENT] message key=value ...
```

**Log Levels**: DEBUG, INFO, WARN, ERROR

**Key Events to Log:**

| Event | Level | Example |
|-------|-------|---------|
| Service startup | INFO | `[INFO] [SIP_CLIENT] [init] started role=master neg_in=22222 neg_out=33333` |
| Slave registration | INFO | `[INFO] [MASTER] [registry] registered service=WHISPER neg_in=22228` |
| Traffic connection established | INFO | `[INFO] [IAP] [connect] connected to downstream=WHISPER port=22229` |
| Heartbeat timeout | WARN | `[WARN] [MASTER] [heartbeat] timeout service=WHISPER last_seen=5.2s` |
| Connection failed | ERROR | `[ERROR] [WHISPER] [connect] failed to connect upstream err=ECONNREFUSED` |
| Call start | INFO | `[INFO] [SIP_CLIENT] [call] started call_id=5` |
| Call end | INFO | `[INFO] [MASTER] [call] ended call_id=5 acks=5/6` |
| Packet error | WARN | `[WARN] [IAP] [traffic] malformed packet size=999999999` |
| Reconnection | INFO | `[INFO] [WHISPER] [reconnect] reconnected to upstream=IAP` |

**Log level controlled via environment variable**: `WHISPERTALK_LOG_LEVEL=DEBUG|INFO|WARN|ERROR` (default: INFO)

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

**Tests:**
- Start 6 mock services, verify master election
- Kill slave, verify master detects within 5s
- Restart slave, verify re-registration
- Simulate packet send/receive between adjacent services
- Race condition test: start 6 services simultaneously, verify no port conflicts

**Acceptance Criteria:**
- 6 services auto-discover ports and register with master
- Heartbeat detects crashed service within 5 seconds
- Packet serialization round-trips correctly
- No port conflicts when services start simultaneously

### Phase 2: IPC Migration (All Services) (4 days)

**Goals:**
- Remove all existing IPC code (UDP, Unix sockets, hard-coded TCP)
- Integrate `interconnect.h` into all 6 services
- Maintain functional pipeline (using existing Kokoro Python via temporary TCP adapter)

**Deliverables:**
- Modified: all 6 service source files
- Temporary Python Kokoro adapter using interconnect TCP ports

**Tests:**
- End-to-end pipeline: SIP call -> transcription -> LLM response -> TTS -> audio out
- Multi-call test: 3 concurrent calls with unique call_ids
- Crash test: Kill Whisper, verify IAP redirects to /dev/null and reconnects

**Acceptance Criteria:**
- Single call completes successfully with <1s latency
- 3 concurrent calls maintain correct call_id routing
- Service crash/restart doesn't drop active calls

### Phase 3: Kokoro C++ Port (10-12 days)

**Goals:**
- Convert Python Kokoro to C++ using libtorch + espeak-ng
- Match Python TTS quality for German
- Integrate into interconnect system

**Sub-phases:**
- **3a (2 days)**: Spike/prototype -- verify libtorch loads TorchScript model, espeak-ng produces usable phonemes
- **3b (4-5 days)**: Full implementation of KokoroPipeline with multi-call support
- **3c (2-3 days)**: Phoneme validation, A/B quality testing, tuning
- **3d (2 days)**: Integration with interconnect system

**Deliverables:**
- `kokoro-service.cpp`
- `export_kokoro_model.py`
- `test_kokoro_cpp.cpp`
- TorchScript models

**Tests:**
- Phonemization accuracy: Compare C++ espeak-ng output vs Python (>95% match)
- Audio quality: A/B test C++ vs Python outputs (PESQ >3.5, target >4.0)
- Performance: Latency comparable to Python (~100-200ms per sentence)
- Multi-call: 5 concurrent TTS streams

**Acceptance Criteria:**
- German phonemization >95% phoneme accuracy vs Python
- PESQ >3.5 (if <3.5, trigger fallback plan from Section 2.4.3)
- C++ version runs without Python runtime
- Concurrent TTS streams process independently

**Contingency**: If audio quality issues persist after tuning, add up to 5 additional days for phoneme normalization layer or dictionary approach.

### Phase 4: Static Binary Build System (3 days)

**Goals:**
- Configure CMake for static linking
- Bundle all dependencies
- Verify standalone execution

**Deliverables:**
- Updated `CMakeLists.txt`
- Build scripts for static whisper.cpp / llama.cpp
- Distribution directory with bundled libs, models, espeak-ng data
- Optional `run.sh` wrapper script

**Tests:**
- Build on clean macOS machine (no Homebrew/MacPorts for runtime)
- Execute binaries with `DYLD_LIBRARY_PATH` unset (using @rpath)
- Verify model loading from relative paths
- `otool -L` shows no dependencies outside `/System/Library` and bundled `lib/`

**Acceptance Criteria:**
- All binaries run on fresh macOS install (only system frameworks required)
- Models load from `./models/`
- espeak-ng data loads from `./espeak-ng-data/`
- Total distribution <3 GB

### Phase 5: Multi-Call and Crash Resilience Testing (4 days)

**Goals:**
- Validate atomic call_id reservation
- Validate crash recovery for all services
- Stress test with high call concurrency

**Tests:**
- **Call ID Collision Test**: 10 SIP lines simultaneously create calls
- **Crash Recovery Matrix**: Kill each service type during active calls
- **Concurrency Stress Test**: 20 concurrent calls for 10 minutes
- **Memory Leak Test**: 100 calls over 1 hour, monitor RSS
- **CALL_END propagation**: Verify all services ACK within 5s

**Acceptance Criteria:**
- No call_id collisions in 1000 calls
- Service crashes recover within 5 seconds
- 20 concurrent calls maintain <1.5s average latency
- No memory leaks (RSS growth <5% over 1 hour)
- CALL_END ACK success rate >99%

### Phase 6: Performance Optimization and Bug Fixes (3 days)

**Goals:**
- Profile and optimize hotspots
- Fix bugs discovered in testing
- Tune VAD parameters for German

**Tests:**
- Re-run all Phase 5 tests
- Latency benchmarks: target <800ms end-to-end
- CPU usage: <200% per service under load

**Acceptance Criteria:**
- All Phase 5 tests pass
- End-to-end latency <800ms (95th percentile)
- VAD correctly segments German speech (manual review of 50 utterances)

**Total Estimated Effort**: ~27-29 working days (5-6 weeks for a single developer)

---

## 6. Verification Approach

### 6.1 Lint and Type Checking

- **Tool**: `clang-tidy` with C++17 checks
- **Format**: `clang-format` (LLVM style)
- **Command**: `find . -name "*.cpp" -o -name "*.h" | xargs clang-tidy`
- **CMake lint**: `cmake-lint CMakeLists.txt`

### 6.2 Unit Tests

**Framework**: Google Test (`gtest`)

| Component | Test File | Key Tests |
|-----------|-----------|-----------|
| Interconnect | `test_interconnect.cpp` | Port discovery, packet serialization, heartbeat, race conditions |
| Kokoro C++ | `test_kokoro_cpp.cpp` | Phonemization, model inference, audio quality |
| Call ID | `test_call_id.cpp` | Atomic reservation, collision avoidance, concurrent requests |

**Build and Run:**
```bash
mkdir build && cd build
cmake -DBUILD_TESTS=ON ..
make
ctest --output-on-failure
```

### 6.3 Integration Tests

1. **Single Call End-to-End**: SIP INVITE -> audio -> transcription -> LLM -> TTS -> audio out -> BYE
2. **Multi-Call**: 5 concurrent calls with different call_ids
3. **Crash Recovery**: Kill service mid-call, verify reconnection
4. **CALL_END Propagation**: Verify all services ACK and clean up
5. **Call ID Collision**: Concurrent reservations from multiple SIP lines

### 6.4 Performance Benchmarks

| Metric | Target | Tool |
|--------|--------|------|
| End-to-end latency | <800ms (95th %ile) | Custom timestamping |
| CPU per service | <200% | `top` |
| Memory per service | <500 MB | `ps` |
| Concurrent calls | 20+ | Load generator |

### 6.5 Acceptance Criteria

**Must-Pass:**
1. All unit tests pass (100% in `ctest`)
2. Integration tests pass
3. 1-hour stress test with 5 concurrent calls (no crashes, no leaks)
4. German TTS quality verified (PESQ >3.5)
5. Static binaries run on clean macOS install

---

## 7. Dependencies and Tools

### 7.1 Required Libraries

**System (macOS):**
- Xcode Command Line Tools (clang, make)
- CMake 3.22+

**Third-Party:**
- whisper.cpp (existing, rebuild as static)
- llama.cpp (clone from GitHub, build as static)
- libtorch 2.0+ (download pre-built for macOS arm64)
- espeak-ng (`brew install espeak-ng`)
- Google Test (`brew install googletest` or bundled via CMake FetchContent)

**Models:**
- Whisper Base German: `whisper-base-de.bin` (~150 MB)
- LLaMA 3.2 1B Q8: `llama-3.2-1b-instruct-q8_0.gguf` (~1.3 GB)
- Kokoro German: `kokoro-german-v1_1-de.pth` (~200 MB, exported to TorchScript)
- Kokoro Voices: `df_eva.pt`, `dm_bernd.pt` (~5 MB each)

### 7.2 Build Environment Setup

```bash
# Install system dependencies
brew install cmake espeak-ng googletest

# Download libtorch (macOS arm64)
mkdir -p third_party && cd third_party
wget https://download.pytorch.org/libtorch/cpu/libtorch-macos-arm64-2.0.0.zip
unzip libtorch-macos-arm64-2.0.0.zip
cd ..

# Build whisper.cpp as static
cd whisper-cpp
cmake -B build -DBUILD_SHARED_LIBS=OFF -DWHISPER_COREML=ON
cmake --build build --config Release
cd ..

# Clone and build llama.cpp as static
git clone https://github.com/ggerganov/llama.cpp.git llama-cpp
cd llama-cpp
git checkout <stable-release-tag>  # Pin to specific version
cmake -B build -DBUILD_SHARED_LIBS=OFF -DLLAMA_METAL=ON
cmake --build build --config Release
cd ..

# Export Kokoro models (one-time, requires Python)
python3 -m venv venv
source venv/bin/activate
pip install torch kokoro phonemizer
python export_kokoro_model.py
deactivate
rm -rf venv

# Copy espeak-ng data for bundling
cp -r /opt/homebrew/share/espeak-ng-data/ espeak-ng-data/
```

**Regular Build:**
```bash
mkdir build && cd build
cmake ..
make -j$(sysctl -n hw.ncpu)
```

### 7.3 Deployment Checklist

1. Run full test suite: `ctest --output-on-failure`
2. Verify linking: `otool -L bin/*`
3. Package directory structure (bin, lib, models, espeak-ng-data)
4. Test on clean macOS VM
5. Verify `run.sh` wrapper works

**Runtime Environment:**
- macOS 12.0+ (Monterey or later)
- Apple Silicon (arm64) or Intel (x86_64 with Rosetta)
- 4 GB RAM minimum (8 GB recommended)
- 5 GB disk space

---

## 8. Risk Mitigation

### 8.1 Technical Risks

| Risk | Impact | Mitigation | Status |
|------|--------|------------|--------|
| libtorch static linking fails | High | Dynamic linking + bundled .dylib + @rpath | Accepted |
| espeak-ng phonemes differ from Python | High | Phoneme diff tool, normalization layer, dictionary fallback | Monitored |
| Kokoro C++ audio quality degraded | Critical | PESQ testing, phased approach with spike first | Blocking |
| Port conflicts with system services | Medium | Configurable base ports via env vars | Planned |
| Master crash orphans system | Medium | Manual restart documented, failover deferred | Accepted |
| espeak-ng data files not found at runtime | Medium | Bundle in distribution, configurable path | Planned |

### 8.2 Schedule Risks

**27-29 day estimate may slip due to:**
1. Kokoro C++ port complexity (espeak-ng integration, phoneme mismatches)
2. libtorch build issues on macOS arm64
3. Unforeseen bugs in interconnect system

**Mitigation:**
- Buffer 5 extra days for unknowns
- Spike/prototype phase for Kokoro before full implementation
- Prioritize core functionality over optimizations
- Defer non-critical features (master failover, hot-swap)

---

## 9. Open Questions (Resolved)

| Question | Resolution |
|----------|------------|
| Kokoro phonemization approach? | espeak-ng C API with validation + fallback plan |
| Master failover? | Not in this phase; manual restart |
| Static linking approach? | Hybrid: static where possible, bundle dylibs, @rpath |
| Persist LLaMA conversation state? | No; context lost on crash is acceptable |
| Packet size negotiation? | Per-service-type (fixed once during registration) |
| German voices required? | `df_eva` (female) and `dm_bernd` (male) |
| Config files vs CLI args? | CLI args for simplicity; config files deferred |
| Port connection direction? | Upstream service connects to downstream service's listen ports |
| TCP direction? | Monodirectional per connection (one sender, one receiver) |
| llama.cpp location? | Clone from GitHub into `llama-cpp/` directory |

---

## 10. Success Metrics

1. All 6 services compile to static/mostly-static binaries
2. No Python runtime required at execution time
3. Interconnection system auto-discovers services and handles 20+ concurrent calls
4. All services survive crashes of neighbors without dropping calls
5. End-to-end latency <800ms (95th percentile)
6. German TTS quality: PESQ >3.5 (target >4.0)
7. 1-hour stress test with 5 calls completes without crashes or leaks
8. All unit and integration tests pass

---

## 11. Appendix: Architecture Diagram

```
┌─────────────────────────────────────────────────────────────────┐
│                        SIP Network (UDP)                        │
└────────────────────────────┬────────────────────────────────────┘
                             │ RTP (G.711)
                             ▼
                    ┌─────────────────┐
                    │  SIP Client     │ (Master, 22222/33333)
                    │  - Multi-line   │
                    │  - RESERVE_CALL │
                    └────┬───────▲────┘
       up_out(33335)─────┘       └─────down_in(22223)
       connects to IAP              accepts from OAP
       down_in(22226)               up_out(33350)
                         ▼                              │
┌───────────────────┐                      ┌────────────────────┐
│ Inbound Audio     │                      │ Outbound Audio     │
│ Processor         │                      │ Processor          │
│ (22225/33336)     │                      │ (22237/33348)      │
│ - G.711 decode    │                      │ - G.711 encode     │
│ - 8->16kHz        │                      │ - 24->8kHz         │
└──────┬────────────┘                      └───────▲────────────┘
  up_out(33338)─────┐               ┌─────down_in(22238)
  connects to       │               │     accepts from
  Whisper           ▼               │     Kokoro
  down_in(22229)              up_out(33347)
┌──────────────────┐                         ┌──────────────────┐
│ Whisper Service  │                         │ Kokoro Service   │
│ (22228/33339)    │                         │ (C++, libtorch)  │
│ - VAD (100ms)    │                         │ (22234/33345)    │
│ - ASR (German)   │                         │ - espeak-ng      │
└──────┬───────────┘                         └────────▲─────────┘
  up_out(33341)─────┐               ┌─────down_in(22235)
  connects to       │               │     accepts from
  LLaMA             ▼               │     LLaMA
  down_in(22232)              up_out(33344)
┌───────────────────────────────────────────────────────┐
│ LLaMA Service (22231/33342)                           │
│ - Conversational AI (German)                          │
│ - Interruption support                                │
│ - Multi-call contexts                                 │
└───────────────────────────────────────────────────────┘

All connections: TCP via InterconnectNode (interconnect.h)
All TCP connections: monodirectional (one sender, one receiver)
Upstream service connects to downstream service's listen ports
Master: Manages registry, broadcasts CALL_END, handles RESERVE_CALL_ID
```

---

**Document Version**: 2.0 REVISED  
**Revision Date**: 2026-02-13  
**Changes from v1.0**: Addressed all 14 review items (port topology clarification, monodirectional TCP, race condition handling, InterconnectNode details, atomic RESERVE_CALL_ID, revised Kokoro estimate, hybrid binary strategy, error handling, CALL_END ACK protocol, phonemization fallback, startup order, logging, llama.cpp setup)  
**Next Step**: Create detailed implementation plan
