// interconnect.h — Prodigy shared inter-service communication layer.
//
// Architecture overview:
//   All pipeline services communicate exclusively through this header.
//   The pipeline is a linear chain of 7 C++ processes:
//
//     SIP_CLIENT → IAP → VAD → WHISPER → LLAMA → TTS → OAP → SIP_CLIENT (loop)
//
//   Every adjacent pair shares two persistent TCP connections:
//     • mgmt channel (base port +0): carries typed control messages
//       (CALL_END, SPEECH_ACTIVE/IDLE, PING/PONG, CUSTOM).
//     • data channel (base port +1): carries binary Packet frames
//       (audio PCM, text payloads, G.711 frames).
//
//   InterconnectNode encapsulates both directions for a single service:
//     • Listen sockets accept the upstream neighbor's connections.
//     • Outbound sockets connect to the downstream neighbor's listen ports.
//     • A background reconnect loop retries downstream connections every
//       DOWNSTREAM_RECONNECT_MS (200ms) until the neighbor is reachable.
//
//   LogForwarder sends structured log entries as UDP datagrams to the
//   frontend log server (port 22022). Each datagram is a plain-text line:
//     "<SERVICE> <LEVEL> <CALL_ID> <message>"
//   The log_level_ gate filters below-threshold messages before send.
//
//   Shared utilities (also used by frontend.cpp for the offline IAP quality
//   test) include the G.711 μ-law → float32 LUT and the polyphase FIR
//   half-band upsample kernel (iap_fir_upsample_frame).
//
// Port map (all on 127.0.0.1):
//   SIP_CLIENT (13100/13101/13102), IAP (13110/13111/13112)
//   VAD (13115/13116/13117), WHISPER (13120/13121/13122)
//   LLAMA (13130/13131/13132), TTS (13140/13141/13142/13143)
//   OAP (13150/13151/13152), FRONTEND (13160/13161/13162)
//   TOMEDO_CRAWL (13180/13181/13182)
//   Log UDP: 22022
//
// Usage pattern for a service:
//   1. Construct InterconnectNode(ServiceType::MY_SERVICE)
//   2. Call initialize() — binds listen ports and starts background threads.
//   3. Call connect_to_downstream() — non-blocking; reconnect loop handles retries.
//   4. Register call_end_handler / speech_signal_handler as needed.
//   5. In processing loop: recv_from_upstream() / send_to_downstream().
//   6. On shutdown: call shutdown() (or let destructor do it).

#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <thread>
#include <atomic>
#include <functional>
#include <chrono>
#include <set>
#include <algorithm>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <cerrno>
#include <cstdio>
#include <cstdarg>

#include "tls_cert.h"

namespace whispertalk {

static constexpr uint16_t FRONTEND_LOG_PORT = 22022;

// Half-band FIR low-pass filter for 8kHz → 16kHz upsampling.
// 15-tap Hamming-windowed sinc, cutoff ~3.8kHz, ~40dB stopband attenuation.
// Used by IAP (inbound-audio-processor.cpp) and the offline codec quality test
// (frontend.cpp handle_iap_quality_test). Both must use the same filter to
// ensure test results match real pipeline behaviour.
static constexpr int IAP_FIR_LEN    = 15;
static constexpr int IAP_FIR_CENTER = 7;
static constexpr int IAP_ULAW_FRAME = 160;

inline const float* iap_fir_coeffs() {
    static const float coeffs[IAP_FIR_LEN] = {
        -0.0076f, 0.0000f, 0.0527f, 0.0000f, -0.1681f, 0.0000f, 0.6230f,
         1.0000f,
         0.6230f, 0.0000f, -0.1681f, 0.0000f, 0.0527f, 0.0000f, -0.0076f
    };
    return coeffs;
}

// Polyphase FIR half-band upsample: 8kHz → 16kHz via 2× zero-stuff + 15-tap filter.
// Exploits half-band structure: odd taps (1,3,5,9,11,13) are zero, center tap (7) = 1.0.
//   Odd outputs:  out[2i+1] = x[i - 3]  (delayed passthrough)
//   Even outputs: out[2i]   = sum of 8 non-zero taps  (8 MACs vs 15 branchy iterations)
// Each polyphase branch has DC gain = 1.0 (even coeffs sum to 1.0, odd = center tap 1.0).
// ~3.7× fewer operations than the naive FIR loop.
// `history` must point to IAP_FIR_CENTER floats that persist across calls.
// Returns number of output samples written (= in_len * 2).
inline size_t iap_fir_upsample_frame(const float* in, size_t in_len,
                                      float* out, float* history) {
    if (in_len > (size_t)IAP_ULAW_FRAME) in_len = IAP_ULAW_FRAME;
    if (in_len == 0) return 0;

    static constexpr float H0 = -0.0076f;
    static constexpr float H2 =  0.0527f;
    static constexpr float H4 = -0.1681f;
    static constexpr float H6 =  0.6230f;

    float ext[IAP_FIR_CENTER + IAP_ULAW_FRAME];
    for (int i = 0; i < IAP_FIR_CENTER; i++) ext[i] = history[i];
    for (size_t i = 0; i < in_len; i++) ext[IAP_FIR_CENTER + i] = in[i];

    size_t out_len = in_len * 2;
    const int N = (int)in_len;

    for (int i = 0; i < N; i++) {
        const float* x = ext + i;
        float even = H0 * x[7] + H2 * x[6] + H4 * x[5] + H6 * x[4]
                   + H6 * x[3] + H4 * x[2] + H2 * x[1] + H0 * x[0];
        out[2 * i] = even;
        out[2 * i + 1] = ext[i + 4];
    }

    if (in_len >= (size_t)IAP_FIR_CENTER) {
        for (int i = 0; i < IAP_FIR_CENTER; i++)
            history[i] = in[in_len - IAP_FIR_CENTER + i];
    } else {
        int shift = (int)in_len;
        for (int i = 0; i < IAP_FIR_CENTER - shift; i++) history[i] = history[i + shift];
        for (int i = 0; i < shift; i++) history[IAP_FIR_CENTER - shift + i] = in[i];
    }
    return out_len;
}

// ServiceType — identifies each process in the Prodigy system.
//
// Pipeline services (is_pipeline_service() == true):
//   SIP_CLIENT, INBOUND_AUDIO_PROCESSOR, VAD_SERVICE, WHISPER_SERVICE,
//   LLAMA_SERVICE, TTS_SERVICE, OUTBOUND_AUDIO_PROCESSOR
//
// Sidecar services (is_pipeline_service() == false):
//   FRONTEND (13160) — web UI and log aggregator; not in the audio path.
//   TOMEDO_CRAWL_SERVICE (13180) — RAG sidecar; connects to the Tomedo EMR
//     server and populates a local vector store.  llama-service queries it
//     via plain HTTP (GET /query, GET /caller/{id}) to retrieve patient context
//     before inference.  It is NOT wired into the interconnect pipeline graph
//     (no up/downstream_of() entry, no Packet frames) — all communication is
//     via its own REST API at port 13181.
enum class ServiceType : uint8_t {
    SIP_CLIENT = 1,
    INBOUND_AUDIO_PROCESSOR = 2,
    VAD_SERVICE = 8,
    WHISPER_SERVICE = 3,
    LLAMA_SERVICE = 4,
    TTS_SERVICE = 5,            // generic TTS stage/dock; engines (kokoro, neutts, ...) connect via engine-dock port
    OUTBOUND_AUDIO_PROCESSOR = 6,
    FRONTEND = 7,
    TOMEDO_CRAWL_SERVICE = 10   // RAG sidecar; REST API only, not in pipeline graph
};

inline bool is_pipeline_service(ServiceType type) {
    switch (type) {
        case ServiceType::SIP_CLIENT:
        case ServiceType::INBOUND_AUDIO_PROCESSOR:
        case ServiceType::VAD_SERVICE:
        case ServiceType::WHISPER_SERVICE:
        case ServiceType::LLAMA_SERVICE:
        case ServiceType::TTS_SERVICE:
        case ServiceType::OUTBOUND_AUDIO_PROCESSOR:
            return true;
        default:
            return false;
    }
}

inline const char* service_type_to_string(ServiceType type) {
    switch (type) {
        case ServiceType::SIP_CLIENT: return "SIP_CLIENT";
        case ServiceType::INBOUND_AUDIO_PROCESSOR: return "INBOUND_AUDIO_PROCESSOR";
        case ServiceType::VAD_SERVICE: return "VAD_SERVICE";
        case ServiceType::WHISPER_SERVICE: return "WHISPER_SERVICE";
        case ServiceType::LLAMA_SERVICE: return "LLAMA_SERVICE";
        case ServiceType::TTS_SERVICE: return "TTS_SERVICE";
        case ServiceType::OUTBOUND_AUDIO_PROCESSOR: return "OUTBOUND_AUDIO_PROCESSOR";
        case ServiceType::FRONTEND: return "FRONTEND";
        case ServiceType::TOMEDO_CRAWL_SERVICE: return "TOMEDO_CRAWL";
        default: return "UNKNOWN";
    }
}

// Pipeline topology:
//   SIP_CLIENT -> IAP -> VAD -> WHISPER -> LLAMA -> TTS -> OAP -> SIP_CLIENT (loop)
// "downstream" = the service we SEND data TO (next in pipeline)
// "upstream"   = the service that sends data TO US (previous in pipeline)
inline ServiceType upstream_of(ServiceType type) {
    switch (type) {
        case ServiceType::INBOUND_AUDIO_PROCESSOR: return ServiceType::SIP_CLIENT;
        case ServiceType::VAD_SERVICE: return ServiceType::INBOUND_AUDIO_PROCESSOR;
        case ServiceType::WHISPER_SERVICE: return ServiceType::VAD_SERVICE;
        case ServiceType::LLAMA_SERVICE: return ServiceType::WHISPER_SERVICE;
        case ServiceType::TTS_SERVICE: return ServiceType::LLAMA_SERVICE;
        case ServiceType::OUTBOUND_AUDIO_PROCESSOR: return ServiceType::TTS_SERVICE;
        case ServiceType::SIP_CLIENT: return ServiceType::OUTBOUND_AUDIO_PROCESSOR;
        default: return ServiceType::SIP_CLIENT;
    }
}

inline ServiceType downstream_of(ServiceType type) {
    switch (type) {
        case ServiceType::SIP_CLIENT: return ServiceType::INBOUND_AUDIO_PROCESSOR;
        case ServiceType::INBOUND_AUDIO_PROCESSOR: return ServiceType::VAD_SERVICE;
        case ServiceType::VAD_SERVICE: return ServiceType::WHISPER_SERVICE;
        case ServiceType::WHISPER_SERVICE: return ServiceType::LLAMA_SERVICE;
        case ServiceType::LLAMA_SERVICE: return ServiceType::TTS_SERVICE;
        case ServiceType::TTS_SERVICE: return ServiceType::OUTBOUND_AUDIO_PROCESSOR;
        case ServiceType::OUTBOUND_AUDIO_PROCESSOR: return ServiceType::SIP_CLIENT;
        default: return ServiceType::SIP_CLIENT;
    }
}

struct PacketTrace {
    static constexpr int MAX_HOPS = 8;
    struct Hop {
        uint8_t service_id;
        uint8_t direction;
        uint64_t timestamp_us;
    };
    Hop hops[MAX_HOPS];
    uint8_t hop_count = 0;

    void record(ServiceType svc, uint8_t dir) {
        if (hop_count < MAX_HOPS) {
            hops[hop_count].service_id = static_cast<uint8_t>(svc);
            hops[hop_count].direction = dir;
            hops[hop_count].timestamp_us = now_us();
            hop_count++;
        }
    }

    static uint64_t now_us() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    double total_ms() const {
        if (hop_count < 2) return 0;
        return (hops[hop_count - 1].timestamp_us - hops[0].timestamp_us) / 1000.0;
    }

    double hop_ms(int i) const {
        if (i <= 0 || i >= hop_count) return 0;
        return (hops[i].timestamp_us - hops[i - 1].timestamp_us) / 1000.0;
    }

    void print_trace() const {
        if (hop_count == 0) return;
        std::fprintf(stderr, "  TRACE: ");
        for (int i = 0; i < hop_count; i++) {
            if (i > 0) std::fprintf(stderr, " -> ");
            std::fprintf(stderr, "%s[%s](+%.2fms)",
                service_type_name(hops[i].service_id),
                hops[i].direction == 0 ? "IN" : "OUT",
                i > 0 ? hop_ms(i) : 0.0);
        }
        std::fprintf(stderr, " = %.2fms total\n", total_ms());
    }

    static const char* service_type_name(uint8_t id) {
        switch (id) {
            case 1: return "SIP";
            case 2: return "IAP";
            case 8: return "VAD";
            case 3: return "WHI";
            case 4: return "LLM";
            case 5: return "TTS";
            case 6: return "OAP";
            case 7: return "FRN";
            case 10: return "RAG";
            default: return "???";
        }
    }
};

// Fixed port assignment per service. Each service gets a base port.
// From that base: +0 = mgmt listen (from upstream), +1 = data listen (from upstream)
// The service CONNECTS to its downstream neighbor's listen ports.
//
// Port map (all on 127.0.0.1):
//   SIP_CLIENT (base 13100): mgmt_listen=13100, data_listen=13101
//   IAP        (base 13110): mgmt_listen=13110, data_listen=13111
//   VAD        (base 13115): mgmt_listen=13115, data_listen=13116
//   WHISPER    (base 13120): mgmt_listen=13120, data_listen=13121
//   LLAMA      (base 13130): mgmt_listen=13130, data_listen=13131
//   TTS        (base 13140): mgmt_listen=13140, data_listen=13141, engine_listen=13143
//   OAP        (base 13150): mgmt_listen=13150, data_listen=13151
//   FRONTEND   (base 13160): mgmt_listen=13160, data_listen=13161
//   TOMEDO_CRAWL (base 13180): mgmt_listen=13180, data_listen=13181
//
// Data flow example: SIP sends data to IAP by connecting to IAP's data_listen (13111).
// IAP sends management msgs to SIP by connecting to SIP's mgmt_listen (13100).
inline uint16_t service_base_port(ServiceType type) {
    switch (type) {
        case ServiceType::SIP_CLIENT:                return 13100;
        case ServiceType::INBOUND_AUDIO_PROCESSOR:   return 13110;
        case ServiceType::VAD_SERVICE:               return 13115;
        case ServiceType::WHISPER_SERVICE:            return 13120;
        case ServiceType::LLAMA_SERVICE:              return 13130;
        case ServiceType::TTS_SERVICE:                return 13140;
        case ServiceType::OUTBOUND_AUDIO_PROCESSOR:   return 13150;
        case ServiceType::FRONTEND:                   return 13160;
        case ServiceType::TOMEDO_CRAWL_SERVICE:       return 13180;
        default: return 0;
    }
}

inline uint16_t service_mgmt_port(ServiceType type) { return service_base_port(type); }
inline uint16_t service_data_port(ServiceType type) { return service_base_port(type) + 1; }
// Command port: for out-of-band text commands from the frontend (e.g., ADD_LINE, GET_STATS).
// Only services that need frontend commands use this (+2 offset).
inline uint16_t service_cmd_port(ServiceType type) { return service_base_port(type) + 2; }
// Engine-dock port: TTS_SERVICE only. TTS engines (kokoro, neutts, ...) open a local
// TCP connection to this port and send a HELLO line to dock with the generic TTS
// stage. Returns 0 for services that do not expose an engine-dock listener.
inline uint16_t service_engine_port(ServiceType type) {
    switch (type) {
        case ServiceType::TTS_SERVICE: return service_base_port(type) + 3;
        default: return 0;
    }
}

struct Packet {
    static constexpr uint32_t MAX_PAYLOAD_SIZE = 1024 * 1024;
    
    uint32_t call_id;
    uint32_t payload_size;
    std::vector<uint8_t> payload;
    PacketTrace trace;

    Packet() : call_id(0), payload_size(0) {}
    
    Packet(uint32_t cid, const void* data, uint32_t size) 
        : call_id(cid), payload_size(std::min(size, MAX_PAYLOAD_SIZE)) {
        if (size > MAX_PAYLOAD_SIZE) {
            std::fprintf(stderr, "Packet: payload %u exceeds max %u, truncating\n",
                         size, MAX_PAYLOAD_SIZE);
        }
        if (payload_size > 0 && data != nullptr) {
            payload.resize(payload_size);
            memcpy(payload.data(), data, payload_size);
        }
    }

    bool is_valid() const {
        return call_id != 0 && payload_size <= MAX_PAYLOAD_SIZE && payload.size() == payload_size;
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buffer(8 + payload_size);
        serialize_into(buffer.data());
        return buffer;
    }

    void serialize_into(uint8_t* buffer) const {
        uint32_t net_call_id = htonl(call_id);
        uint32_t net_size = htonl(payload_size);
        memcpy(buffer, &net_call_id, 4);
        memcpy(buffer + 4, &net_size, 4);
        if (payload_size > 0) {
            memcpy(buffer + 8, payload.data(), payload_size);
        }
    }

    size_t serialized_size() const { return 8 + payload_size; }

    static bool deserialize(const void* data, size_t len, Packet& out) {
        if (len < 8) return false;
        
        const uint8_t* ptr = static_cast<const uint8_t*>(data);
        uint32_t net_call_id, net_size;
        memcpy(&net_call_id, ptr, 4);
        memcpy(&net_size, ptr + 4, 4);
        
        out.call_id = ntohl(net_call_id);
        out.payload_size = ntohl(net_size);
        
        if (out.call_id == 0 || out.payload_size > MAX_PAYLOAD_SIZE) {
            return false;
        }
        
        if (len < 8 + out.payload_size) {
            return false;
        }
        
        out.payload.resize(out.payload_size);
        if (out.payload_size > 0) {
            memcpy(out.payload.data(), ptr + 8, out.payload_size);
        }
        
        return true;
    }
};

enum class ConnectionState {
    DISCONNECTED,
    CONNECTING,
    CONNECTED,
    FAILED
};

// Management message types sent over the mgmt channel.
// Format: 1-byte type, then type-specific payload.
enum class MgmtMsgType : uint8_t {
    CALL_END       = 1,   // payload: 4 bytes call_id
    SPEECH_ACTIVE  = 2,   // payload: 4 bytes call_id
    SPEECH_IDLE    = 3,   // payload: 4 bytes call_id
    PING           = 4,   // no payload (keepalive probe)
    PONG           = 5,   // no payload (keepalive response)
    CUSTOM         = 10,  // payload: 2-byte len + string
};

// InterconnectNode — per-service TCP communication hub.
//
// Lifecycle:
//   initialize()              → bind listen sockets, start 3 background threads.
//   connect_to_downstream()   → one-shot attempt; reconnect_loop retries forever.
//   send_to_downstream(pkt)   → send data packet to next service in pipeline.
//   recv_from_upstream(pkt)   → blocking receive from previous service (with timeout).
//   broadcast_call_end(id)    → notify downstream chain that a call has terminated.
//   broadcast_speech_signal() → propagate VAD SPEECH_ACTIVE/IDLE downstream.
//   shutdown()                → close all sockets, join threads (idempotent).
//
// Thread model (3 background threads per node):
//   accept_thread_:              waits for upstream to connect on our listen ports;
//                                polls for dead connections and resets them.
//   downstream_connect_thread_:  polls downstream state; reconnects every 200ms on failure.
//   mgmt_recv_thread_:           reads typed messages from upstream mgmt socket;
//                                dispatches call_end / speech / ping / custom handlers.
//
// Safety: All socket accesses are guarded by per-direction mutexes. Sockets are
// closed with shutdown(SHUT_RDWR) before close() so blocked threads unblock.
class InterconnectNode {
public:
    InterconnectNode(ServiceType type) 
        : type_(type), 
          running_(false),
          max_known_call_id_(0),
          mgmt_listen_sock_(-1),
          data_listen_sock_(-1),
          upstream_mgmt_accepted_(-1),
          upstream_data_accepted_(-1),
          downstream_mgmt_sock_(-1),
          downstream_data_sock_(-1),
          upstream_state_(ConnectionState::DISCONNECTED),
          downstream_state_(ConnectionState::DISCONNECTED) {}

    ~InterconnectNode() {
        shutdown();
    }

    bool initialize() {
        if (!is_pipeline_service(type_)) {
            running_ = true;
            return true;
        }

        uint16_t mgmt_port = service_mgmt_port(type_);
        uint16_t data_port = service_data_port(type_);

        mgmt_listen_sock_ = create_listen_socket(mgmt_port);
        if (mgmt_listen_sock_ < 0) {
            std::fprintf(stderr, "[%s] Failed to bind mgmt port %u\n",
                        service_type_to_string(type_), mgmt_port);
            return false;
        }

        data_listen_sock_ = create_listen_socket(data_port);
        if (data_listen_sock_ < 0) {
            close_socket(mgmt_listen_sock_);
            std::fprintf(stderr, "[%s] Failed to bind data port %u\n",
                        service_type_to_string(type_), data_port);
            return false;
        }

        running_ = true;

        accept_thread_ = std::thread(&InterconnectNode::accept_loop, this);
        downstream_connect_thread_ = std::thread(&InterconnectNode::downstream_connect_loop, this);
        mgmt_recv_thread_ = std::thread(&InterconnectNode::mgmt_recv_loop, this);

        std::fprintf(stderr, "[%s] Interconnect ready: mgmt=%u data=%u\n",
                    service_type_to_string(type_), mgmt_port, data_port);
        return true;
    }

    void shutdown() {
        if (!running_.exchange(false)) return;

        close_socket(upstream_mgmt_accepted_);
        close_socket(upstream_data_accepted_);
        close_socket(downstream_mgmt_sock_);
        close_socket(downstream_data_sock_);
        close_socket(mgmt_listen_sock_);
        close_socket(data_listen_sock_);

        if (accept_thread_.joinable()) accept_thread_.join();
        if (downstream_connect_thread_.joinable()) downstream_connect_thread_.join();
        if (mgmt_recv_thread_.joinable()) mgmt_recv_thread_.join();
    }

    ServiceType type() const { return type_; }

    ConnectionState upstream_state() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return upstream_state_;
    }

    ConnectionState downstream_state() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return downstream_state_;
    }

    // Connect to downstream neighbor's listen ports.
    // Called once at startup; reconnect_loop handles retries.
    bool connect_to_downstream() {
        if (!is_pipeline_service(type_)) return false;

        ServiceType ds = downstream_of(type_);
        uint16_t ds_mgmt = service_mgmt_port(ds);
        uint16_t ds_data = service_data_port(ds);

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            downstream_state_ = ConnectionState::CONNECTING;
        }

        int mgmt_sock = connect_to_port_with_timeout("127.0.0.1", ds_mgmt, CONNECT_TIMEOUT_MS);
        if (mgmt_sock < 0) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            downstream_state_ = ConnectionState::DISCONNECTED;
            return false;
        }

        int data_sock = connect_to_port_with_timeout("127.0.0.1", ds_data, CONNECT_TIMEOUT_MS);
        if (data_sock < 0) {
            ::close(mgmt_sock);
            std::lock_guard<std::mutex> lock(state_mutex_);
            downstream_state_ = ConnectionState::DISCONNECTED;
            return false;
        }

        setup_socket_options(mgmt_sock);
        setup_socket_options(data_sock);

        {
            std::lock_guard<std::mutex> lock(downstream_mutex_);
            close_socket(downstream_mgmt_sock_);
            close_socket(downstream_data_sock_);
            downstream_mgmt_sock_ = mgmt_sock;
            downstream_data_sock_ = data_sock;
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            downstream_state_ = ConnectionState::CONNECTED;
        }

        std::fprintf(stderr, "[%s] Connected to downstream %s (mgmt=%u data=%u)\n",
                    service_type_to_string(type_),
                    service_type_to_string(ds), ds_mgmt, ds_data);
        return true;
    }

    // Send a data packet to our downstream neighbor (next in pipeline).
    // SIP -> IAP: SIP calls send_to_downstream, which writes to IAP's data_listen.
    bool send_to_downstream(const Packet& pkt) {
        std::lock_guard<std::mutex> lock(send_downstream_mutex_);
        int sock;
        {
            std::lock_guard<std::mutex> dl(downstream_mutex_);
            sock = downstream_data_sock_;
        }
        if (sock < 0) return false;

        auto data = pkt.serialize();
        if (!send_encrypted(sock, data.data(), data.size(), DATA_SEND_TIMEOUT_MS)) {
            mark_downstream_failed();
            return false;
        }
        return true;
    }

    // Send a data packet back to upstream neighbor (reverse direction, e.g. OAP -> SIP).
    bool send_to_upstream(const Packet& pkt) {
        std::lock_guard<std::mutex> lock(send_upstream_mutex_);
        int sock;
        {
            std::lock_guard<std::mutex> ul(upstream_mutex_);
            sock = upstream_data_accepted_;
        }
        if (sock < 0) return false;

        auto data = pkt.serialize();
        if (!send_encrypted(sock, data.data(), data.size(), DATA_SEND_TIMEOUT_MS)) {
            mark_upstream_failed();
            return false;
        }
        return true;
    }

    // Receive data from upstream neighbor (they connected to our data_listen port).
    bool recv_from_upstream(Packet& pkt, int timeout_ms = 100) {
        int sock;
        {
            std::lock_guard<std::mutex> lock(upstream_mutex_);
            sock = upstream_data_accepted_;
        }
        if (sock < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            return false;
        }
        bool ok = recv_packet(sock, pkt, timeout_ms);
        if (!ok) {
            std::lock_guard<std::mutex> lock(upstream_mutex_);
            if (upstream_data_accepted_ == sock && is_socket_dead(sock)) {
                mark_upstream_failed_locked();
            }
        }
        return ok;
    }

    // Receive data from downstream neighbor (we connected to their data_listen, they reply).
    bool recv_from_downstream(Packet& pkt, int timeout_ms = 100) {
        int sock;
        {
            std::lock_guard<std::mutex> lock(downstream_mutex_);
            sock = downstream_data_sock_;
        }
        if (sock < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            return false;
        }
        bool ok = recv_packet(sock, pkt, timeout_ms);
        if (!ok) {
            std::lock_guard<std::mutex> lock(downstream_mutex_);
            if (downstream_data_sock_ == sock && is_socket_dead(sock)) {
                mark_downstream_failed_locked();
            }
        }
        return ok;
    }

    // Call ID management — local only, no master needed.
    // Each service can independently assign call IDs since only SIP_CLIENT creates them.
    uint32_t reserve_call_id(uint32_t proposed_id) {
        std::lock_guard<std::mutex> lock(call_id_mutex_);
        uint32_t final_id = (proposed_id > max_known_call_id_) ? proposed_id : max_known_call_id_ + 1;
        max_known_call_id_ = final_id;
        active_call_ids_.insert(final_id);
        return final_id;
    }

    // Broadcast CALL_END to downstream neighbor via mgmt channel.
    void broadcast_call_end(uint32_t call_id) {
        bool already_ended = false;
        {
            std::lock_guard<std::mutex> lock(call_id_mutex_);
            if (ended_call_ids_.count(call_id) > 0) {
                already_ended = true;
            }
            ended_call_ids_.insert(call_id);
            active_call_ids_.erase(call_id);
            prune_ended_call_ids();
        }
        {
            std::lock_guard<std::mutex> lock(speech_mutex_);
            speech_active_calls_.erase(call_id);
        }

        if (!already_ended && call_end_handler_) {
            call_end_handler_(call_id);
        }

        send_mgmt_to_downstream(MgmtMsgType::CALL_END, call_id);
    }

    void register_call_end_handler(std::function<void(uint32_t)> handler) {
        call_end_handler_ = handler;
    }

    void broadcast_speech_signal(uint32_t call_id, bool active) {
        {
            std::lock_guard<std::mutex> lock(speech_mutex_);
            if (active) speech_active_calls_.insert(call_id);
            else speech_active_calls_.erase(call_id);
        }

        if (speech_signal_handler_) {
            speech_signal_handler_(call_id, active);
        }

        MgmtMsgType msg_type = active ? MgmtMsgType::SPEECH_ACTIVE : MgmtMsgType::SPEECH_IDLE;
        send_mgmt_to_downstream(msg_type, call_id);
    }

    void register_speech_signal_handler(std::function<void(uint32_t, bool)> handler) {
        speech_signal_handler_ = handler;
    }

    void register_custom_negotiation_handler(std::function<std::string(const std::string&)> handler) {
        custom_handler_ = handler;
    }

    bool is_speech_active(uint32_t call_id) const {
        std::lock_guard<std::mutex> lock(speech_mutex_);
        return speech_active_calls_.count(call_id) > 0;
    }

    bool has_ended(uint32_t call_id) const {
        std::lock_guard<std::mutex> lock(call_id_mutex_);
        return ended_call_ids_.count(call_id) > 0;
    }

    size_t active_call_count() const {
        std::lock_guard<std::mutex> lock(call_id_mutex_);
        return active_call_ids_.size();
    }

    size_t ended_call_count() const {
        std::lock_guard<std::mutex> lock(call_id_mutex_);
        return ended_call_ids_.size();
    }

    uint16_t frontend_log_port() const { return FRONTEND_LOG_PORT; }

    // Send a custom command to our downstream's mgmt channel and wait for response.
    std::string send_custom_to_downstream(const std::string& msg, int timeout_ms = CUSTOM_MSG_TIMEOUT_MS) {
        int sock;
        {
            std::lock_guard<std::mutex> lock(downstream_mutex_);
            sock = downstream_mgmt_sock_;
        }
        if (sock < 0) return "";

        uint16_t len = static_cast<uint16_t>(std::min(msg.size(), (size_t)65535));
        std::vector<uint8_t> buf(3 + len);
        buf[0] = static_cast<uint8_t>(MgmtMsgType::CUSTOM);
        uint16_t net_len = htons(len);
        memcpy(buf.data() + 1, &net_len, 2);
        memcpy(buf.data() + 3, msg.data(), len);

        std::lock_guard<std::mutex> lock(send_downstream_mgmt_mutex_);
        if (!send_encrypted(sock, buf.data(), buf.size(), timeout_ms)) return "";

        std::vector<uint8_t> resp_plain;
        if (!recv_encrypted(sock, resp_plain, timeout_ms)) return "";
        if (resp_plain.size() < 3) return "";
        if (resp_plain[0] != static_cast<uint8_t>(MgmtMsgType::CUSTOM)) return "";
        uint16_t resp_len;
        memcpy(&resp_len, resp_plain.data() + 1, 2);
        resp_len = ntohs(resp_len);
        if (resp_len == 0 || resp_plain.size() < (size_t)(3 + resp_len)) return "";

        return std::string(reinterpret_cast<char*>(resp_plain.data() + 3), resp_len);
    }

private:
    ServiceType type_;
    std::atomic<bool> running_;
    static constexpr size_t SEND_BUF_SIZE = 65536;
    static constexpr int DOWNSTREAM_RECONNECT_MS = 200;
    static constexpr size_t MAX_ENDED_CALL_IDS = 1000;
    static constexpr int ACCEPT_POLL_TIMEOUT_MS = 50;
    static constexpr int IDLE_POLL_MS = 100;
    static constexpr int MGMT_RECV_TIMEOUT_MS = 500;
    static constexpr int MGMT_SEND_TIMEOUT_MS = 100;
    static constexpr int DATA_SEND_TIMEOUT_MS = 100;
    static constexpr int CONNECT_TIMEOUT_MS = 2000;
    static constexpr int CUSTOM_MSG_TIMEOUT_MS = 2000;
    static constexpr int PAYLOAD_RECV_TIMEOUT_MS = 5000;
    static constexpr int LISTEN_BACKLOG = 4;
    static constexpr int TCP_KEEPALIVE_IDLE_S = 2;
    static constexpr int TCP_KEEPALIVE_INTVL_S = 1;
    static constexpr int TCP_KEEPALIVE_CNT = 2;

    mutable std::mutex call_id_mutex_;
    uint32_t max_known_call_id_;
    std::set<uint32_t> active_call_ids_;
    std::set<uint32_t> ended_call_ids_;

    int mgmt_listen_sock_;
    int data_listen_sock_;

    mutable std::mutex upstream_mutex_;
    int upstream_mgmt_accepted_;
    int upstream_data_accepted_;

    mutable std::mutex downstream_mutex_;
    int downstream_mgmt_sock_;
    int downstream_data_sock_;

    std::mutex send_downstream_mutex_;
    std::mutex send_upstream_mutex_;
    std::mutex send_downstream_mgmt_mutex_;
    std::mutex send_upstream_mgmt_mutex_;

    mutable std::mutex state_mutex_;
    ConnectionState upstream_state_;
    ConnectionState downstream_state_;

    std::thread accept_thread_;
    std::thread downstream_connect_thread_;
    std::thread mgmt_recv_thread_;

    std::function<void(uint32_t)> call_end_handler_;
    std::function<void(uint32_t, bool)> speech_signal_handler_;
    std::function<std::string(const std::string&)> custom_handler_;
    mutable std::mutex speech_mutex_;
    std::set<uint32_t> speech_active_calls_;

    // Accept incoming connections from upstream on our mgmt + data listen ports.
    void accept_loop() {
        while (running_) {
            bool need_mgmt = false, need_data = false;
            {
                std::lock_guard<std::mutex> lock(upstream_mutex_);
                need_mgmt = (upstream_mgmt_accepted_ < 0);
                need_data = (upstream_data_accepted_ < 0);
            }

            if (need_mgmt && mgmt_listen_sock_ >= 0) {
                pollfd pfd = {mgmt_listen_sock_, POLLIN, 0};
                if (poll(&pfd, 1, ACCEPT_POLL_TIMEOUT_MS) > 0 && (pfd.revents & POLLIN)) {
                    sockaddr_in addr;
                    socklen_t len = sizeof(addr);
                    int accepted = accept(mgmt_listen_sock_, (sockaddr*)&addr, &len);
                    if (accepted >= 0) {
                        setup_socket_options(accepted);
                        std::lock_guard<std::mutex> lock(upstream_mutex_);
                        close_socket(upstream_mgmt_accepted_);
                        upstream_mgmt_accepted_ = accepted;
                        update_upstream_state();
                        std::fprintf(stderr, "[%s] Upstream mgmt connected\n",
                                    service_type_to_string(type_));
                    }
                }
            }

            if (need_data && data_listen_sock_ >= 0) {
                pollfd pfd = {data_listen_sock_, POLLIN, 0};
                if (poll(&pfd, 1, ACCEPT_POLL_TIMEOUT_MS) > 0 && (pfd.revents & POLLIN)) {
                    sockaddr_in addr;
                    socklen_t len = sizeof(addr);
                    int accepted = accept(data_listen_sock_, (sockaddr*)&addr, &len);
                    if (accepted >= 0) {
                        setup_socket_options(accepted);
                        std::lock_guard<std::mutex> lock(upstream_mutex_);
                        close_socket(upstream_data_accepted_);
                        upstream_data_accepted_ = accepted;
                        update_upstream_state();
                        std::fprintf(stderr, "[%s] Upstream data connected\n",
                                    service_type_to_string(type_));
                    }
                }
            }

            if (!need_mgmt && !need_data) {
                {
                    std::lock_guard<std::mutex> lock(upstream_mutex_);
                    if (upstream_mgmt_accepted_ >= 0 && is_socket_dead(upstream_mgmt_accepted_)) {
                        std::fprintf(stderr, "[%s] Upstream mgmt peer disconnected\n",
                                    service_type_to_string(type_));
                        close_socket(upstream_mgmt_accepted_);
                        update_upstream_state();
                    }
                    if (upstream_data_accepted_ >= 0 && is_socket_dead(upstream_data_accepted_)) {
                        std::fprintf(stderr, "[%s] Upstream data peer disconnected\n",
                                    service_type_to_string(type_));
                        close_socket(upstream_data_accepted_);
                        update_upstream_state();
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(IDLE_POLL_MS));
            }
        }
    }

    void update_upstream_state() {
        ConnectionState new_state;
        if (upstream_mgmt_accepted_ >= 0 && upstream_data_accepted_ >= 0) {
            new_state = ConnectionState::CONNECTED;
        } else if (upstream_mgmt_accepted_ >= 0 || upstream_data_accepted_ >= 0) {
            new_state = ConnectionState::CONNECTING;
        } else {
            new_state = ConnectionState::DISCONNECTED;
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        upstream_state_ = new_state;
    }

    // Continuously try to connect to downstream neighbor.
    void downstream_connect_loop() {
        if (!is_pipeline_service(type_)) return;

        while (running_) {
            ConnectionState ds;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                ds = downstream_state_;
            }

            if (ds == ConnectionState::DISCONNECTED || ds == ConnectionState::FAILED) {
                connect_to_downstream();
            } else {
                bool dead = false;
                {
                    std::lock_guard<std::mutex> lock(downstream_mutex_);
                    if (downstream_data_sock_ >= 0 && is_socket_dead(downstream_data_sock_)) {
                        std::fprintf(stderr, "[%s] Downstream data connection lost\n",
                                    service_type_to_string(type_));
                        close_socket(downstream_data_sock_);
                        close_socket(downstream_mgmt_sock_);
                        dead = true;
                    }
                    if (downstream_mgmt_sock_ >= 0 && is_socket_dead(downstream_mgmt_sock_)) {
                        std::fprintf(stderr, "[%s] Downstream mgmt connection lost\n",
                                    service_type_to_string(type_));
                        close_socket(downstream_mgmt_sock_);
                        close_socket(downstream_data_sock_);
                        dead = true;
                    }
                }
                if (dead) {
                    std::lock_guard<std::mutex> sl(state_mutex_);
                    downstream_state_ = ConnectionState::DISCONNECTED;
                }
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(DOWNSTREAM_RECONNECT_MS));
        }
    }

    // Receive management messages from upstream neighbor.
    void mgmt_recv_loop() {
        while (running_) {
            int sock;
            {
                std::lock_guard<std::mutex> lock(upstream_mutex_);
                sock = upstream_mgmt_accepted_;
            }
            if (sock < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(IDLE_POLL_MS));
                continue;
            }

            pollfd pfd = {sock, POLLIN, 0};
            int pr = poll(&pfd, 1, IDLE_POLL_MS);
            if (pr <= 0) continue;
            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                mark_upstream_failed();
                continue;
            }

            std::vector<uint8_t> mgmt_plain;
            if (!recv_encrypted(sock, mgmt_plain, MGMT_RECV_TIMEOUT_MS)) {
                mark_upstream_failed();
                continue;
            }
            if (mgmt_plain.empty()) { mark_upstream_failed(); continue; }

            uint8_t type_byte = mgmt_plain[0];
            MgmtMsgType msg_type = static_cast<MgmtMsgType>(type_byte);

            bool mark_failed = false;
            switch (msg_type) {
                case MgmtMsgType::CALL_END: {
                    if (mgmt_plain.size() < 5) { mark_failed = true; break; }
                    uint32_t net_cid;
                    memcpy(&net_cid, mgmt_plain.data() + 1, 4);
                    uint32_t cid = ntohl(net_cid);
                    handle_remote_call_end(cid);
                    break;
                }
                case MgmtMsgType::SPEECH_ACTIVE:
                case MgmtMsgType::SPEECH_IDLE: {
                    if (mgmt_plain.size() < 5) { mark_failed = true; break; }
                    uint32_t net_cid;
                    memcpy(&net_cid, mgmt_plain.data() + 1, 4);
                    uint32_t cid = ntohl(net_cid);
                    bool active = (msg_type == MgmtMsgType::SPEECH_ACTIVE);
                    bool changed = false;
                    {
                        std::lock_guard<std::mutex> lock(speech_mutex_);
                        if (active) changed = speech_active_calls_.insert(cid).second;
                        else changed = speech_active_calls_.erase(cid) > 0;
                    }
                    if (changed) {
                        if (speech_signal_handler_) {
                            speech_signal_handler_(cid, active);
                        }
                        if (type_ != ServiceType::OUTBOUND_AUDIO_PROCESSOR &&
                            type_ != ServiceType::SIP_CLIENT) {
                            send_mgmt_to_downstream(msg_type, cid);
                        }
                    }
                    break;
                }
                case MgmtMsgType::PING: {
                    uint8_t pong = static_cast<uint8_t>(MgmtMsgType::PONG);
                    std::lock_guard<std::mutex> lock(send_upstream_mgmt_mutex_);
                    send_encrypted(sock, &pong, 1, MGMT_SEND_TIMEOUT_MS);
                    break;
                }
                case MgmtMsgType::PONG:
                    break;
                case MgmtMsgType::CUSTOM: {
                    if (mgmt_plain.size() < 3) { mark_failed = true; break; }
                    uint16_t net_len;
                    memcpy(&net_len, mgmt_plain.data() + 1, 2);
                    uint16_t len = ntohs(net_len);

                    std::string response;
                    if (len > 0 && mgmt_plain.size() >= (size_t)(3 + len)) {
                        std::string msg(reinterpret_cast<char*>(mgmt_plain.data() + 3), len);
                        if (custom_handler_) {
                            response = custom_handler_(msg);
                        }
                    }

                    uint16_t resp_len = static_cast<uint16_t>(std::min(response.size(), (size_t)65535));
                    std::vector<uint8_t> resp_buf(3 + resp_len);
                    resp_buf[0] = static_cast<uint8_t>(MgmtMsgType::CUSTOM);
                    uint16_t net_resp_len = htons(resp_len);
                    memcpy(resp_buf.data() + 1, &net_resp_len, 2);
                    if (resp_len > 0) memcpy(resp_buf.data() + 3, response.data(), resp_len);
                    {
                        std::lock_guard<std::mutex> lock(send_upstream_mgmt_mutex_);
                        send_encrypted(sock, resp_buf.data(), 3 + resp_len, CUSTOM_MSG_TIMEOUT_MS);
                    }
                    break;
                }
                default:
                    std::fprintf(stderr, "[%s] Unknown mgmt message type %u, disconnecting upstream\n",
                        service_type_to_string(type_), (unsigned)type_byte);
                    mark_failed = true;
                    break;
            }
            if (mark_failed) {
                mark_upstream_failed();
                continue;
            }
        }
    }

    void prune_ended_call_ids() {
        if (ended_call_ids_.size() > MAX_ENDED_CALL_IDS) {
            auto it = ended_call_ids_.begin();
            std::advance(it, ended_call_ids_.size() / 2);
            ended_call_ids_.erase(ended_call_ids_.begin(), it);
        }
    }

    void handle_remote_call_end(uint32_t call_id) {
        bool already_ended = false;
        {
            std::lock_guard<std::mutex> lock(call_id_mutex_);
            if (ended_call_ids_.count(call_id) > 0) already_ended = true;
            ended_call_ids_.insert(call_id);
            active_call_ids_.erase(call_id);
            prune_ended_call_ids();
        }
        {
            std::lock_guard<std::mutex> lock(speech_mutex_);
            speech_active_calls_.erase(call_id);
        }

        if (!already_ended) {
            if (call_end_handler_) {
                call_end_handler_(call_id);
            }
            send_mgmt_to_downstream(MgmtMsgType::CALL_END, call_id);
        }
    }

    // Send a management message (type + 4-byte call_id) to downstream mgmt channel.
    void send_mgmt_to_downstream(MgmtMsgType msg_type, uint32_t call_id) {
        int sock;
        {
            std::lock_guard<std::mutex> lock(downstream_mutex_);
            sock = downstream_mgmt_sock_;
        }
        if (sock < 0) return;

        uint8_t buf[5];
        buf[0] = static_cast<uint8_t>(msg_type);
        uint32_t net_cid = htonl(call_id);
        memcpy(buf + 1, &net_cid, 4);

        std::lock_guard<std::mutex> lock(send_downstream_mgmt_mutex_);
        send_encrypted(sock, buf, 5, MGMT_SEND_TIMEOUT_MS);
    }

    // Lock order: {upstream,downstream}_mutex_ → state_mutex_ (never reversed).
    // Caller MUST hold the corresponding connection mutex.
    void mark_upstream_failed_locked() {
        close_socket(upstream_mgmt_accepted_);
        close_socket(upstream_data_accepted_);
        std::lock_guard<std::mutex> sl(state_mutex_);
        upstream_state_ = ConnectionState::DISCONNECTED;
    }

    void mark_downstream_failed_locked() {
        close_socket(downstream_mgmt_sock_);
        close_socket(downstream_data_sock_);
        std::lock_guard<std::mutex> sl(state_mutex_);
        downstream_state_ = ConnectionState::DISCONNECTED;
    }

    void mark_upstream_failed() {
        std::lock_guard<std::mutex> lock(upstream_mutex_);
        mark_upstream_failed_locked();
    }

    void mark_downstream_failed() {
        std::lock_guard<std::mutex> lock(downstream_mutex_);
        mark_downstream_failed_locked();
    }

    bool is_socket_dead(int fd) {
        if (fd < 0) return false;
        struct pollfd pfd = {fd, POLLIN, 0};
        int ret = poll(&pfd, 1, 0);
        if (ret > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL))) return true;
        if (ret > 0 && (pfd.revents & POLLIN)) {
            char probe;
            ssize_t r = recv(fd, &probe, 1, MSG_PEEK | MSG_DONTWAIT);
            if (r == 0) return true;
        }
        return false;
    }

    int create_listen_socket(uint16_t port) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return -1;

        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);

        if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(sock);
            return -1;
        }

        if (listen(sock, LISTEN_BACKLOG) < 0) {
            ::close(sock);
            return -1;
        }

        return sock;
    }

    int connect_to_port_with_timeout(const char* host, uint16_t port, int timeout_ms) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return -1;

        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(host);
        addr.sin_port = htons(port);

        int ret = connect(sock, (sockaddr*)&addr, sizeof(addr));
        if (ret < 0 && errno != EINPROGRESS) {
            ::close(sock);
            return -1;
        }

        if (ret == 0) {
            fcntl(sock, F_SETFL, flags);
            return sock;
        }

        pollfd pfd = {sock, POLLOUT, 0};
        int poll_ret = poll(&pfd, 1, timeout_ms);

        if (poll_ret <= 0) {
            ::close(sock);
            return -1;
        }

        int error = 0;
        socklen_t errlen = sizeof(error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &errlen);
        if (error != 0) {
            ::close(sock);
            return -1;
        }

        fcntl(sock, F_SETFL, flags);
        return sock;
    }

    void setup_socket_options(int sock) {
#ifdef SO_NOSIGPIPE
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &opt, sizeof(opt));
#endif
        int opt_tcp = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &opt_tcp, sizeof(opt_tcp));

        // Enable TCP keepalive for fast dead-peer detection.
        int keepalive = 1;
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
#ifdef TCP_KEEPIDLE
        int idle = TCP_KEEPALIVE_IDLE_S;
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
        int intvl = TCP_KEEPALIVE_INTVL_S;
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#ifdef TCP_KEEPCNT
        int cnt = TCP_KEEPALIVE_CNT;
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
    }

    bool send_all_with_timeout(int sock, const void* data, size_t len, int timeout_ms) {
        const uint8_t* ptr = static_cast<const uint8_t*>(data);
        size_t sent = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (sent < len) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) return false;

            pollfd pfd = {sock, POLLOUT, 0};
            int pr = poll(&pfd, 1, static_cast<int>(remaining));
            if (pr <= 0) return false;
            if (pfd.revents & (POLLERR | POLLHUP)) return false;

            ssize_t n = send(sock, ptr + sent, len - sent, 0);
            if (n <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                return false;
            }
            sent += n;
        }
        return true;
    }

    bool recv_exact(int sock, void* buf, size_t len, int timeout_ms) {
        uint8_t* ptr = static_cast<uint8_t*>(buf);
        size_t received = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (received < len) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) return false;

            pollfd pfd = {sock, POLLIN, 0};
            int pr = poll(&pfd, 1, static_cast<int>(remaining));
            if (pr <= 0) return false;
            if (pfd.revents & POLLERR) return false;
            if (!(pfd.revents & POLLIN)) return false;

            ssize_t n = recv(sock, ptr + received, len - received, 0);
            if (n <= 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
                return false;
            }
            received += n;
        }
        return true;
    }

    bool send_encrypted(int sock, const void* data, size_t len, int timeout_ms) {
        size_t enc_size = prodigy_tls::ic_encrypted_size(len);
        std::vector<uint8_t> enc_buf(4 + enc_size);
        size_t actual_enc_len = 0;
        if (!prodigy_tls::ic_encrypt(static_cast<const uint8_t*>(data), len,
                                      enc_buf.data() + 4, actual_enc_len)) {
            return false;
        }
        uint32_t net_len = htonl(static_cast<uint32_t>(actual_enc_len));
        memcpy(enc_buf.data(), &net_len, 4);
        return send_all_with_timeout(sock, enc_buf.data(), 4 + actual_enc_len, timeout_ms);
    }

    bool recv_encrypted(int sock, std::vector<uint8_t>& plaintext, int timeout_ms) {
        uint32_t net_len;
        if (!recv_exact(sock, &net_len, 4, timeout_ms)) return false;
        uint32_t enc_len = ntohl(net_len);
        if (enc_len > Packet::MAX_PAYLOAD_SIZE + 256) return false;
        std::vector<uint8_t> enc_buf(enc_len);
        if (!recv_exact(sock, enc_buf.data(), enc_len, timeout_ms)) return false;
        plaintext.resize(enc_len);
        size_t plain_len = 0;
        if (!prodigy_tls::ic_decrypt(enc_buf.data(), enc_len,
                                      plaintext.data(), plain_len)) {
            return false;
        }
        plaintext.resize(plain_len);
        return true;
    }

    bool recv_packet(int sock, Packet& pkt, int timeout_ms) {
        std::vector<uint8_t> plaintext;
        if (!recv_encrypted(sock, plaintext, timeout_ms)) return false;
        if (plaintext.size() < 8) return false;
        return Packet::deserialize(plaintext.data(), plaintext.size(), pkt);
    }

    void close_socket(int& sock) {
        if (sock >= 0) {
            ::shutdown(sock, SHUT_RDWR);
            ::close(sock);
            sock = -1;
        }
    }
};

// LogLevel controls verbosity per service.
// Ordered by severity: ERROR(0) < WARN(1) < INFO(2) < DEBUG(3) < TRACE(4).
// Messages with level > current threshold are dropped before the UDP send.
// Runtime change: call set_level() or send "SET_LOG_LEVEL:<LEVEL>" to cmd port.
// Startup: pass --log-level <LEVEL> CLI argument (persisted in frontend DB).
enum class LogLevel : int {
    ERROR = 0,
    WARN  = 1,
    INFO  = 2,
    DEBUG = 3,
    TRACE = 4
};

inline const char* log_level_string(LogLevel lvl) {
    switch (lvl) {
        case LogLevel::ERROR: return "ERROR";
        case LogLevel::WARN:  return "WARN";
        case LogLevel::INFO:  return "INFO";
        case LogLevel::DEBUG: return "DEBUG";
        case LogLevel::TRACE: return "TRACE";
        default: return "INFO";
    }
}

inline LogLevel log_level_from_string(const char* s) {
    if (!s) return LogLevel::INFO;
    if (strcasecmp(s, "ERROR") == 0) return LogLevel::ERROR;
    if (strcasecmp(s, "WARN")  == 0) return LogLevel::WARN;
    if (strcasecmp(s, "INFO")  == 0) return LogLevel::INFO;
    if (strcasecmp(s, "DEBUG") == 0) return LogLevel::DEBUG;
    if (strcasecmp(s, "TRACE") == 0) return LogLevel::TRACE;
    return LogLevel::INFO;
}

// LogForwarder — UDP log sink for all pipeline services.
//
// Each service owns one LogForwarder instance. After init(), every call to
// forward() serializes the message as:
//   "<SERVICE> <LEVEL> <CALL_ID> <message>\0"
// and sendto() it to the frontend log server at 127.0.0.1:22022 (FRONTEND_LOG_PORT).
//
// The frontend process_log_message() parses this format, stores entries in SQLite,
// and exposes them via GET /api/logs for the UI and test scripts.
//
// Thread safety: forward() is safe to call from any thread. set_level() uses an
// atomic store so level changes take effect immediately without a lock.
//
// Buffer sizing:
//   msg[2048]: max message body; vsnprintf truncates safely at 2047 + null.
//   buf[2304]: prefix overhead max 41 bytes + 2047 body = 2088 < 2304.
//   Frontend recv buffer: 4096 bytes — larger than max datagram (2304).
class LogForwarder {
public:
    LogForwarder() : sock_(-1), port_(0), log_level_(static_cast<int>(LogLevel::INFO)) {}

    ~LogForwarder() {
        if (sock_ >= 0) ::close(sock_);
    }

    void init(uint16_t port, ServiceType svc) {
        if (port == 0) return;
        port_ = port;
        svc_name_ = service_type_to_string(svc);
        sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock_ >= 0) {
            memset(&addr_, 0, sizeof(addr_));
            addr_.sin_family = AF_INET;
            addr_.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr_.sin_port = htons(port_);
        }
    }

    void set_level(LogLevel level) {
        log_level_.store(static_cast<int>(level), std::memory_order_relaxed);
    }

    void set_level(const char* level_str) {
        set_level(log_level_from_string(level_str));
    }

    LogLevel get_level() const {
        return static_cast<LogLevel>(log_level_.load(std::memory_order_relaxed));
    }

    void forward(LogLevel lvl, uint32_t call_id, const char* fmt, ...) {
        if (sock_ < 0) return;
        if (static_cast<int>(lvl) > log_level_.load(std::memory_order_relaxed)) return;
        va_list args;
        va_start(args, fmt);
        vforward(log_level_string(lvl), call_id, fmt, args);
        va_end(args);
    }

    void forward(const char* level, uint32_t call_id, const char* fmt, ...) {
        if (sock_ < 0) return;
        if (static_cast<int>(log_level_from_string(level)) > log_level_.load(std::memory_order_relaxed)) return;
        va_list args;
        va_start(args, fmt);
        vforward(level, call_id, fmt, args);
        va_end(args);
    }

    bool active() const { return sock_ >= 0 && port_ > 0; }

private:
    void vforward(const char* level, uint32_t call_id, const char* fmt, va_list args) {
        // msg[2048]: max log message body. vsnprintf truncates safely at 2047 chars + null.
        char msg[2048];
        int mlen = vsnprintf(msg, sizeof(msg), fmt, args);
        if (mlen <= 0) return;
        if (mlen >= (int)sizeof(msg)) mlen = (int)sizeof(msg) - 1;

        // buf[2304]: format is "<SERVICE> <LEVEL> <CALL_ID> <message>"
        // Max prefix overhead: 23 (service) + 1 + 5 (level) + 1 + 10 (uint32) + 1 = 41 chars
        // Max total: 41 + 2047 = 2088 < 2304 — no overflow possible.
        // recv buffer on frontend side is 4096, well above this maximum.
        char buf[2304];
        int blen = snprintf(buf, sizeof(buf), "%s %s %u %.*s",
                            svc_name_, level, call_id, mlen, msg);
        if (blen > 0) {
            if (blen >= (int)sizeof(buf)) blen = (int)sizeof(buf) - 1;
            sendto(sock_, buf, static_cast<size_t>(blen), 0,
                   (struct sockaddr*)&addr_, sizeof(addr_));
        }
    }

    int sock_;
    uint16_t port_;
    const char* svc_name_ = "UNKNOWN";
    struct sockaddr_in addr_;
    std::atomic<int> log_level_;
};

}
