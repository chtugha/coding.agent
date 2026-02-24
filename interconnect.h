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

namespace whispertalk {

static constexpr uint16_t FRONTEND_LOG_PORT = 22022;

enum class ServiceType : uint8_t {
    SIP_CLIENT = 1,
    INBOUND_AUDIO_PROCESSOR = 2,
    VAD_SERVICE = 8,
    WHISPER_SERVICE = 3,
    LLAMA_SERVICE = 4,
    KOKORO_SERVICE = 5,
    OUTBOUND_AUDIO_PROCESSOR = 6,
    FRONTEND = 7
};

inline bool is_pipeline_service(ServiceType type) {
    switch (type) {
        case ServiceType::SIP_CLIENT:
        case ServiceType::INBOUND_AUDIO_PROCESSOR:
        case ServiceType::VAD_SERVICE:
        case ServiceType::WHISPER_SERVICE:
        case ServiceType::LLAMA_SERVICE:
        case ServiceType::KOKORO_SERVICE:
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
        case ServiceType::KOKORO_SERVICE: return "KOKORO_SERVICE";
        case ServiceType::OUTBOUND_AUDIO_PROCESSOR: return "OUTBOUND_AUDIO_PROCESSOR";
        case ServiceType::FRONTEND: return "FRONTEND";
        default: return "UNKNOWN";
    }
}

// Pipeline topology:
//   SIP_CLIENT -> IAP -> VAD -> WHISPER -> LLAMA -> KOKORO -> OAP -> SIP_CLIENT (loop)
// "downstream" = the service we SEND data TO (next in pipeline)
// "upstream"   = the service that sends data TO US (previous in pipeline)
inline ServiceType upstream_of(ServiceType type) {
    switch (type) {
        case ServiceType::INBOUND_AUDIO_PROCESSOR: return ServiceType::SIP_CLIENT;
        case ServiceType::VAD_SERVICE: return ServiceType::INBOUND_AUDIO_PROCESSOR;
        case ServiceType::WHISPER_SERVICE: return ServiceType::VAD_SERVICE;
        case ServiceType::LLAMA_SERVICE: return ServiceType::WHISPER_SERVICE;
        case ServiceType::KOKORO_SERVICE: return ServiceType::LLAMA_SERVICE;
        case ServiceType::OUTBOUND_AUDIO_PROCESSOR: return ServiceType::KOKORO_SERVICE;
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
        case ServiceType::LLAMA_SERVICE: return ServiceType::KOKORO_SERVICE;
        case ServiceType::KOKORO_SERVICE: return ServiceType::OUTBOUND_AUDIO_PROCESSOR;
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
            case 5: return "KOK";
            case 6: return "OAP";
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
//   KOKORO     (base 13140): mgmt_listen=13140, data_listen=13141
//   OAP        (base 13150): mgmt_listen=13150, data_listen=13151
//   FRONTEND   (base 13160): mgmt_listen=13160, data_listen=13161
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
        case ServiceType::KOKORO_SERVICE:             return 13140;
        case ServiceType::OUTBOUND_AUDIO_PROCESSOR:   return 13150;
        case ServiceType::FRONTEND:                   return 13160;
        default: return 0;
    }
}

inline uint16_t service_mgmt_port(ServiceType type) { return service_base_port(type); }
inline uint16_t service_data_port(ServiceType type) { return service_base_port(type) + 1; }
// Command port: for out-of-band text commands from the frontend (e.g., ADD_LINE, GET_STATS).
// Only services that need frontend commands use this (+2 offset).
inline uint16_t service_cmd_port(ServiceType type) { return service_base_port(type) + 2; }

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

// Peer-to-peer interconnect node. No master/slave. No heartbeats.
// Each service listens on 2 fixed TCP ports (mgmt + data) for its upstream neighbor.
// Each service connects to 2 fixed TCP ports on its downstream neighbor.
// TCP keepalive + send/recv errors provide instant crash detection.
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

        int mgmt_sock = connect_to_port_with_timeout("127.0.0.1", ds_mgmt, 2000);
        if (mgmt_sock < 0) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            downstream_state_ = ConnectionState::DISCONNECTED;
            return false;
        }

        int data_sock = connect_to_port_with_timeout("127.0.0.1", ds_data, 2000);
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

        size_t total = pkt.serialized_size();
        if (total <= SEND_BUF_SIZE) {
            thread_local uint8_t buf[SEND_BUF_SIZE];
            pkt.serialize_into(buf);
            if (!send_all_with_timeout(sock, buf, total, 100)) {
                mark_downstream_failed();
                return false;
            }
        } else {
            auto data = pkt.serialize();
            if (!send_all_with_timeout(sock, data.data(), data.size(), 100)) {
                mark_downstream_failed();
                return false;
            }
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

        size_t total = pkt.serialized_size();
        if (total <= SEND_BUF_SIZE) {
            thread_local uint8_t buf[SEND_BUF_SIZE];
            pkt.serialize_into(buf);
            if (!send_all_with_timeout(sock, buf, total, 100)) {
                mark_upstream_failed();
                return false;
            }
        } else {
            auto data = pkt.serialize();
            if (!send_all_with_timeout(sock, data.data(), data.size(), 100)) {
                mark_upstream_failed();
                return false;
            }
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
        if (!ok && sock >= 0) {
            if (is_socket_dead(sock)) {
                mark_upstream_failed();
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
        if (!ok && sock >= 0) {
            if (is_socket_dead(sock)) {
                mark_downstream_failed();
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
    std::string send_custom_to_downstream(const std::string& msg, int timeout_ms = 2000) {
        int sock;
        {
            std::lock_guard<std::mutex> lock(downstream_mutex_);
            sock = downstream_mgmt_sock_;
        }
        if (sock < 0) return "";

        uint8_t buf[3 + 65535];
        buf[0] = static_cast<uint8_t>(MgmtMsgType::CUSTOM);
        uint16_t len = static_cast<uint16_t>(std::min(msg.size(), (size_t)65535));
        uint16_t net_len = htons(len);
        memcpy(buf + 1, &net_len, 2);
        memcpy(buf + 3, msg.data(), len);

        std::lock_guard<std::mutex> lock(send_downstream_mgmt_mutex_);
        if (!send_all_with_timeout(sock, buf, 3 + len, timeout_ms)) return "";

        uint8_t resp_hdr[3];
        if (!recv_exact(sock, resp_hdr, 3, timeout_ms)) return "";
        uint16_t resp_len;
        memcpy(&resp_len, resp_hdr + 1, 2);
        resp_len = ntohs(resp_len);
        if (resp_len == 0) return "";

        std::string resp(resp_len, '\0');
        if (!recv_exact(sock, resp.data(), resp_len, timeout_ms)) return "";
        return resp;
    }

private:
    ServiceType type_;
    std::atomic<bool> running_;
    static constexpr size_t SEND_BUF_SIZE = 65536;
    static constexpr int DOWNSTREAM_RECONNECT_MS = 500;

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
                if (poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
                    sockaddr_in addr;
                    socklen_t len = sizeof(addr);
                    int accepted = accept(mgmt_listen_sock_, (sockaddr*)&addr, &len);
                    if (accepted >= 0) {
                        setup_socket_options(accepted);
                        std::lock_guard<std::mutex> lock(upstream_mutex_);
                        if (upstream_mgmt_accepted_ >= 0) ::close(upstream_mgmt_accepted_);
                        upstream_mgmt_accepted_ = accepted;
                        update_upstream_state();
                        std::fprintf(stderr, "[%s] Upstream mgmt connected\n",
                                    service_type_to_string(type_));
                    }
                }
            }

            if (need_data && data_listen_sock_ >= 0) {
                pollfd pfd = {data_listen_sock_, POLLIN, 0};
                if (poll(&pfd, 1, 50) > 0 && (pfd.revents & POLLIN)) {
                    sockaddr_in addr;
                    socklen_t len = sizeof(addr);
                    int accepted = accept(data_listen_sock_, (sockaddr*)&addr, &len);
                    if (accepted >= 0) {
                        setup_socket_options(accepted);
                        std::lock_guard<std::mutex> lock(upstream_mutex_);
                        if (upstream_data_accepted_ >= 0) ::close(upstream_data_accepted_);
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
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }

            pollfd pfd = {sock, POLLIN, 0};
            int pr = poll(&pfd, 1, 100);
            if (pr <= 0) continue;
            if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                mark_upstream_failed();
                continue;
            }

            uint8_t type_byte;
            if (!recv_exact(sock, &type_byte, 1, 500)) {
                // Connection closed or errored — recv_exact returns false on EOF (n==0)
                // and on read errors. Mark upstream failed so reconnect logic kicks in.
                mark_upstream_failed();
                continue;
            }

            MgmtMsgType msg_type = static_cast<MgmtMsgType>(type_byte);

            bool mark_failed = false;
            switch (msg_type) {
                case MgmtMsgType::CALL_END: {
                    uint32_t net_cid;
                    if (!recv_exact(sock, &net_cid, 4, 500)) { mark_failed = true; break; }
                    uint32_t cid = ntohl(net_cid);
                    handle_remote_call_end(cid);
                    break;
                }
                case MgmtMsgType::SPEECH_ACTIVE:
                case MgmtMsgType::SPEECH_IDLE: {
                    uint32_t net_cid;
                    if (!recv_exact(sock, &net_cid, 4, 500)) { mark_failed = true; break; }
                    uint32_t cid = ntohl(net_cid);
                    bool active = (msg_type == MgmtMsgType::SPEECH_ACTIVE);
                    {
                        std::lock_guard<std::mutex> lock(speech_mutex_);
                        if (active) speech_active_calls_.insert(cid);
                        else speech_active_calls_.erase(cid);
                    }
                    if (speech_signal_handler_) {
                        speech_signal_handler_(cid, active);
                    }
                    // Forward downstream
                    send_mgmt_to_downstream(msg_type, cid);
                    break;
                }
                case MgmtMsgType::PING: {
                    uint8_t pong = static_cast<uint8_t>(MgmtMsgType::PONG);
                    std::lock_guard<std::mutex> lock(send_upstream_mgmt_mutex_);
                    send_all_with_timeout(sock, &pong, 1, 100);
                    break;
                }
                case MgmtMsgType::PONG:
                    break;
                case MgmtMsgType::CUSTOM: {
                    uint16_t net_len;
                    if (!recv_exact(sock, &net_len, 2, 500)) { mark_failed = true; break; }
                    uint16_t len = ntohs(net_len);
                    if (len == 0) { mark_failed = true; break; }
                    std::string msg(len, '\0');
                    if (!recv_exact(sock, msg.data(), len, 2000)) { mark_failed = true; break; }

                    std::string response;
                    if (custom_handler_) {
                        response = custom_handler_(msg);
                    }

                    uint16_t resp_len = static_cast<uint16_t>(std::min(response.size(), (size_t)65535));
                    std::vector<uint8_t> resp_buf(3 + resp_len);
                    resp_buf[0] = static_cast<uint8_t>(MgmtMsgType::CUSTOM);
                    uint16_t net_resp_len = htons(resp_len);
                    memcpy(resp_buf.data() + 1, &net_resp_len, 2);
                    if (resp_len > 0) memcpy(resp_buf.data() + 3, response.data(), resp_len);
                    {
                        std::lock_guard<std::mutex> lock(send_upstream_mgmt_mutex_);
                        send_all_with_timeout(sock, resp_buf.data(), 3 + resp_len, 2000);
                    }
                    break;
                }
                default:
                    break;
            }
            if (mark_failed) {
                mark_upstream_failed();
                continue;
            }
        }
    }

    void handle_remote_call_end(uint32_t call_id) {
        bool already_ended = false;
        {
            std::lock_guard<std::mutex> lock(call_id_mutex_);
            if (ended_call_ids_.count(call_id) > 0) already_ended = true;
            ended_call_ids_.insert(call_id);
            active_call_ids_.erase(call_id);
        }

        if (!already_ended && call_end_handler_) {
            call_end_handler_(call_id);
        }

        // Forward call_end downstream
        send_mgmt_to_downstream(MgmtMsgType::CALL_END, call_id);
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
        if (!send_all_with_timeout(sock, buf, 5, 100)) {
            // Don't mark failed for mgmt send failure — data channel matters more
        }
    }

    void mark_upstream_failed() {
        {
            std::lock_guard<std::mutex> lock(upstream_mutex_);
            close_socket(upstream_mgmt_accepted_);
            close_socket(upstream_data_accepted_);
        }
        std::lock_guard<std::mutex> sl(state_mutex_);
        upstream_state_ = ConnectionState::DISCONNECTED;
    }

    void mark_downstream_failed() {
        {
            std::lock_guard<std::mutex> lock(downstream_mutex_);
            close_socket(downstream_mgmt_sock_);
            close_socket(downstream_data_sock_);
        }
        std::lock_guard<std::mutex> sl(state_mutex_);
        downstream_state_ = ConnectionState::DISCONNECTED;
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

        if (listen(sock, 4) < 0) {
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
        int idle = 2;
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
#endif
#ifdef TCP_KEEPINTVL
        int intvl = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
#endif
#ifdef TCP_KEEPCNT
        int cnt = 2;
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
    }

    bool send_all(int sock, const void* data, size_t len) {
        const uint8_t* ptr = static_cast<const uint8_t*>(data);
        size_t sent = 0;
        while (sent < len) {
            ssize_t n = send(sock, ptr + sent, len - sent, 0);
            if (n <= 0) return false;
            sent += n;
        }
        return true;
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

    ssize_t recv_with_timeout(int sock, void* buf, size_t len, int timeout_ms) {
        pollfd pfd = {sock, POLLIN, 0};
        int ret = poll(&pfd, 1, timeout_ms);
        if (ret <= 0) return -1;
        if (pfd.revents & POLLERR) return -1;
        if (pfd.revents & POLLIN) return recv(sock, buf, len, 0);
        if (pfd.revents & POLLHUP) return -1;
        return -1;
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

    bool recv_packet(int sock, Packet& pkt, int timeout_ms) {
        uint8_t header[8];
        if (!recv_exact(sock, header, 8, timeout_ms)) {
            return false;
        }

        uint32_t net_call_id, net_size;
        memcpy(&net_call_id, header, 4);
        memcpy(&net_size, header + 4, 4);

        pkt.call_id = ntohl(net_call_id);
        pkt.payload_size = ntohl(net_size);

        if (pkt.call_id == 0 || pkt.payload_size > Packet::MAX_PAYLOAD_SIZE) {
            return false;
        }

        pkt.payload.resize(pkt.payload_size);
        if (pkt.payload_size > 0) {
            if (!recv_exact(sock, pkt.payload.data(), pkt.payload_size, 5000)) {
                return false;
            }
        }

        return true;
    }

    void close_socket(int& sock) {
        if (sock >= 0) {
            ::shutdown(sock, SHUT_RDWR);
            ::close(sock);
            sock = -1;
        }
    }
};

class LogForwarder {
public:
    LogForwarder() : sock_(-1), port_(0) {}

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

    void forward(const char* level, uint32_t call_id, const char* fmt, ...) {
        if (sock_ < 0) return;
        char msg[2048];
        va_list args;
        va_start(args, fmt);
        int mlen = vsnprintf(msg, sizeof(msg), fmt, args);
        va_end(args);
        if (mlen <= 0) return;

        char buf[2200];
        int blen = snprintf(buf, sizeof(buf), "%s %s %u %.*s",
                            svc_name_, level, call_id, mlen, msg);
        if (blen > 0) {
            sendto(sock_, buf, static_cast<size_t>(blen), 0,
                   (struct sockaddr*)&addr_, sizeof(addr_));
        }
    }

    bool active() const { return sock_ >= 0 && port_ > 0; }

private:
    int sock_;
    uint16_t port_;
    const char* svc_name_ = "UNKNOWN";
    struct sockaddr_in addr_;
};

}
