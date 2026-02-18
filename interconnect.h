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

namespace whispertalk {

enum class ServiceType : uint8_t {
    SIP_CLIENT = 1,
    INBOUND_AUDIO_PROCESSOR = 2,
    WHISPER_SERVICE = 3,
    LLAMA_SERVICE = 4,
    KOKORO_SERVICE = 5,
    OUTBOUND_AUDIO_PROCESSOR = 6,
    FRONTEND = 7
};

inline const char* service_type_to_string(ServiceType type) {
    switch (type) {
        case ServiceType::SIP_CLIENT: return "SIP_CLIENT";
        case ServiceType::INBOUND_AUDIO_PROCESSOR: return "INBOUND_AUDIO_PROCESSOR";
        case ServiceType::WHISPER_SERVICE: return "WHISPER_SERVICE";
        case ServiceType::LLAMA_SERVICE: return "LLAMA_SERVICE";
        case ServiceType::KOKORO_SERVICE: return "KOKORO_SERVICE";
        case ServiceType::OUTBOUND_AUDIO_PROCESSOR: return "OUTBOUND_AUDIO_PROCESSOR";
        case ServiceType::FRONTEND: return "FRONTEND";
        default: return "UNKNOWN";
    }
}

inline ServiceType upstream_of(ServiceType type) {
    switch (type) {
        case ServiceType::INBOUND_AUDIO_PROCESSOR: return ServiceType::SIP_CLIENT;
        case ServiceType::WHISPER_SERVICE: return ServiceType::INBOUND_AUDIO_PROCESSOR;
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
        case ServiceType::INBOUND_AUDIO_PROCESSOR: return ServiceType::WHISPER_SERVICE;
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
            case 3: return "WHI";
            case 4: return "LLM";
            case 5: return "KOK";
            case 6: return "OAP";
            default: return "???";
        }
    }
};

struct PortConfig {
    uint16_t neg_in;
    uint16_t neg_out;
    uint16_t down_in;
    uint16_t down_out;
    uint16_t up_in;
    uint16_t up_out;

    PortConfig() : neg_in(0), neg_out(0), down_in(0), down_out(0), up_in(0), up_out(0) {}

    PortConfig(uint16_t neg_in_port, uint16_t neg_out_port) 
        : neg_in(neg_in_port), neg_out(neg_out_port) {
        down_in = neg_in + 1;
        down_out = neg_in + 2;
        up_in = neg_out + 1;
        up_out = neg_out + 2;
    }

    bool is_valid() const {
        return neg_in > 0 && neg_out > 0;
    }
};

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

class InterconnectNode {
public:
    InterconnectNode(ServiceType type) 
        : type_(type), 
          is_master_(false),
          was_original_master_(false),
          running_(false),
          master_heartbeat_failures_(0),
          max_known_call_id_(0),
          neg_in_sock_(-1),
          neg_out_sock_(-1),
          down_in_listen_sock_(-1),
          down_out_listen_sock_(-1),
          down_in_accepted_(-1),
          down_out_accepted_(-1),
          up_in_sock_(-1),
          up_out_sock_(-1),
          upstream_state_(ConnectionState::DISCONNECTED),
          downstream_state_(ConnectionState::DISCONNECTED) {}

    ~InterconnectNode() {
        shutdown();
    }

    bool initialize() {
        if (!scan_and_bind_ports()) {
            return false;
        }

        if (!bind_traffic_listen_ports()) {
            close_socket(neg_in_sock_);
            close_socket(neg_out_sock_);
            return false;
        }

        running_ = true;

        neg_thread_ = std::thread(&InterconnectNode::negotiation_loop, this);
        heartbeat_thread_ = std::thread(&InterconnectNode::heartbeat_loop, this);
        accept_thread_ = std::thread(&InterconnectNode::accept_loop, this);
        reconnect_thread_ = std::thread(&InterconnectNode::reconnect_loop, this);

        return true;
    }

    void shutdown() {
        if (!running_.exchange(false)) return;

        close_socket(up_in_sock_);
        close_socket(up_out_sock_);
        close_socket(down_in_accepted_);
        close_socket(down_out_accepted_);
        close_socket(down_in_listen_sock_);
        close_socket(down_out_listen_sock_);
        close_socket(neg_in_sock_);
        close_socket(neg_out_sock_);

        if (neg_thread_.joinable()) neg_thread_.join();
        if (heartbeat_thread_.joinable()) heartbeat_thread_.join();
        if (accept_thread_.joinable()) accept_thread_.join();
        if (reconnect_thread_.joinable()) reconnect_thread_.join();
    }

    bool is_master() const { return is_master_; }
    bool was_promoted() const { return is_master_ && !was_original_master_; }
    const PortConfig& ports() const { return ports_; }
    ServiceType type() const { return type_; }

    ConnectionState upstream_state() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return upstream_state_;
    }

    ConnectionState downstream_state() const {
        std::lock_guard<std::mutex> lock(state_mutex_);
        return downstream_state_;
    }

    bool connect_to_downstream() {
        PortConfig downstream_ports = query_downstream_ports();
        if (!downstream_ports.is_valid()) {
            return false;
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            upstream_state_ = ConnectionState::CONNECTING;
        }

        int out_sock = connect_to_port_with_timeout("127.0.0.1", downstream_ports.down_in, 5000);
        if (out_sock < 0) {
            std::lock_guard<std::mutex> lock(state_mutex_);
            upstream_state_ = ConnectionState::FAILED;
            return false;
        }

        int in_sock = connect_to_port_with_timeout("127.0.0.1", downstream_ports.down_out, 5000);
        if (in_sock < 0) {
            close(out_sock);
            std::lock_guard<std::mutex> lock(state_mutex_);
            upstream_state_ = ConnectionState::FAILED;
            return false;
        }

        setup_socket_options(out_sock);
        setup_socket_options(in_sock);

        {
            std::lock_guard<std::mutex> lock(traffic_mutex_);
            close_socket(up_out_sock_);
            close_socket(up_in_sock_);
            up_out_sock_ = out_sock;
            up_in_sock_ = in_sock;
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            upstream_state_ = ConnectionState::CONNECTED;
        }

        return true;
    }

    bool send_to_downstream(const Packet& pkt) {
        std::lock_guard<std::mutex> lock(send_downstream_mutex_);
        int sock;
        {
            std::lock_guard<std::mutex> tl(traffic_mutex_);
            sock = up_out_sock_;
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
            std::fprintf(stderr, "send_to_downstream: packet %zu > %zu, heap fallback\n",
                         total, SEND_BUF_SIZE);
            auto data = pkt.serialize();
            if (!send_all_with_timeout(sock, data.data(), data.size(), 100)) {
                mark_upstream_failed();
                return false;
            }
        }
        return true;
    }

    bool send_to_upstream(const Packet& pkt) {
        std::lock_guard<std::mutex> lock(send_upstream_mutex_);
        int sock;
        {
            std::lock_guard<std::mutex> tl(traffic_mutex_);
            sock = down_out_accepted_;
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
            std::fprintf(stderr, "send_to_upstream: packet %zu > %zu, heap fallback\n",
                         total, SEND_BUF_SIZE);
            auto data = pkt.serialize();
            if (!send_all_with_timeout(sock, data.data(), data.size(), 100)) {
                mark_downstream_failed();
                return false;
            }
        }
        return true;
    }

    bool recv_from_upstream(Packet& pkt, int timeout_ms = 100) {
        int sock;
        {
            std::lock_guard<std::mutex> lock(traffic_mutex_);
            sock = down_in_accepted_;
        }
        if (sock < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            return false;
        }
        return recv_packet(sock, pkt, timeout_ms);
    }

    bool recv_from_downstream(Packet& pkt, int timeout_ms = 100) {
        int sock;
        {
            std::lock_guard<std::mutex> lock(traffic_mutex_);
            sock = up_in_sock_;
        }
        if (sock < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(timeout_ms));
            return false;
        }
        return recv_packet(sock, pkt, timeout_ms);
    }

    uint32_t reserve_call_id(uint32_t proposed_id) {
        if (is_master_) {
            std::lock_guard<std::mutex> lock(call_id_mutex_);
            uint32_t final_id = (proposed_id > max_known_call_id_) ? proposed_id : max_known_call_id_ + 1;
            max_known_call_id_ = final_id;
            active_call_ids_.insert(final_id);
            return final_id;
        } else {
            int sock = connect_to_master();
            if (sock < 0) return 0;

            std::string msg = "RESERVE_CALL_ID " + std::to_string(proposed_id);
            if (!send_all(sock, msg.c_str(), msg.size())) {
                close(sock);
                return 0;
            }

            char buffer[64];
            ssize_t n = recv_with_timeout(sock, buffer, sizeof(buffer) - 1, 2000);
            close(sock);

            if (n <= 0) return 0;
            buffer[n] = '\0';

            std::string response(buffer);
            if (response.substr(0, 17) == "CALL_ID_RESERVED ") {
                uint32_t reserved_id = std::stoul(response.substr(17));
                std::lock_guard<std::mutex> lock(call_id_mutex_);
                if (reserved_id > max_known_call_id_) {
                    max_known_call_id_ = reserved_id;
                }
                active_call_ids_.insert(reserved_id);
                return reserved_id;
            }

            return 0;
        }
    }

    int connect_to_master() {
        return connect_to_port("127.0.0.1", 22222);
    }

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

        if (is_master_) {
            std::string msg = "CALL_END " + std::to_string(call_id);
            std::vector<std::pair<ServiceType, uint16_t>> targets;

            {
                std::lock_guard<std::mutex> reg_lock(registry_mutex_);
                for (const auto& [type, config] : service_registry_) {
                    targets.push_back({type, config.neg_in});
                }
            }

            std::set<ServiceType> acked;
            std::mutex ack_mutex;

            std::vector<std::thread> send_threads;
            for (const auto& target : targets) {
                ServiceType stype = target.first;
                uint16_t port = target.second;
                send_threads.emplace_back([&, stype, port]() {
                    int sock = connect_to_port("127.0.0.1", port);
                    if (sock < 0) return;
                    if (send_all(sock, msg.c_str(), msg.size())) {
                        char buf[64];
                        ssize_t n = recv_with_timeout(sock, buf, sizeof(buf) - 1, 5000);
                        if (n > 0) {
                            buf[n] = '\0';
                            std::string resp(buf);
                            std::string expected = "CALL_END_ACK " + std::to_string(call_id);
                            if (resp == expected) {
                                std::lock_guard<std::mutex> al(ack_mutex);
                                acked.insert(stype);
                            }
                        }
                    }
                    close(sock);
                });
            }

            for (auto& t : send_threads) {
                if (t.joinable()) t.join();
            }
        }
    }

    void register_call_end_handler(std::function<void(uint32_t)> handler) {
        call_end_handler_ = handler;
    }

    void broadcast_speech_signal(uint32_t call_id, bool active) {
        std::string msg = (active ? "SPEECH_ACTIVE " : "SPEECH_IDLE ") + std::to_string(call_id);

        if (is_master_) {
            if (active) {
                std::lock_guard<std::mutex> lock(speech_mutex_);
                speech_active_calls_.insert(call_id);
            } else {
                std::lock_guard<std::mutex> lock(speech_mutex_);
                speech_active_calls_.erase(call_id);
            }
            if (speech_signal_handler_) {
                speech_signal_handler_(call_id, active);
            }

            std::vector<std::pair<ServiceType, uint16_t>> targets;
            {
                std::lock_guard<std::mutex> reg_lock(registry_mutex_);
                for (const auto& [type, config] : service_registry_) {
                    targets.push_back({type, config.neg_in});
                }
            }

            for (const auto& [stype, port] : targets) {
                int sock = connect_to_port("127.0.0.1", port);
                if (sock < 0) continue;
                send_all(sock, msg.c_str(), msg.size());
                close(sock);
            }
        } else {
            uint16_t master_port = 22222;
            int sock = connect_to_port("127.0.0.1", master_port);
            if (sock >= 0) {
                send_all(sock, msg.c_str(), msg.size());
                close(sock);
            }
        }
    }

    void register_speech_signal_handler(std::function<void(uint32_t, bool)> handler) {
        speech_signal_handler_ = handler;
    }

    bool is_speech_active(uint32_t call_id) const {
        std::lock_guard<std::mutex> lock(speech_mutex_);
        return speech_active_calls_.count(call_id) > 0;
    }

    bool is_service_alive(ServiceType svc_type) const {
        if (!is_master_) return false;
        
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = last_heartbeat_.find(svc_type);
        if (it == last_heartbeat_.end()) return false;
        
        auto elapsed = std::chrono::steady_clock::now() - it->second;
        return elapsed < std::chrono::seconds(5);
    }

    size_t active_call_count() const {
        std::lock_guard<std::mutex> lock(call_id_mutex_);
        return active_call_ids_.size();
    }

    size_t ended_call_count() const {
        std::lock_guard<std::mutex> lock(call_id_mutex_);
        return ended_call_ids_.size();
    }

    size_t registered_service_count() const {
        if (!is_master_) return 0;
        std::lock_guard<std::mutex> lock(registry_mutex_);
        return service_registry_.size();
    }

    PortConfig query_service_ports(ServiceType svc_type) const {
        if (is_master_) {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = service_registry_.find(svc_type);
            if (it != service_registry_.end()) return it->second;
            return PortConfig();
        }
        return PortConfig();
    }

private:
    ServiceType type_;
    bool is_master_;
    bool was_original_master_;
    std::atomic<bool> running_;
    int master_heartbeat_failures_;
    static constexpr size_t SEND_BUF_SIZE = 65536;
    static constexpr int MASTER_FAILURE_THRESHOLD = 3;
    static constexpr int PORT_RECLAIM_WAIT_MS = 500;
    static constexpr int HEARTBEAT_INTERVAL_S = 2;
    static constexpr int HEARTBEAT_RECV_TIMEOUT_MS = 1000;
    static constexpr int SYNC_RECV_TIMEOUT_MS = 500;
    static constexpr int STEP_DOWN_RECV_TIMEOUT_MS = 3000;
    static constexpr int DEMOTION_REGISTER_RETRIES = 3;
    static constexpr int DEMOTION_REGISTER_BACKOFF_MS = 200;
    PortConfig ports_;

    mutable std::mutex call_id_mutex_;
    uint32_t max_known_call_id_;
    std::set<uint32_t> active_call_ids_;
    std::set<uint32_t> ended_call_ids_;

    int neg_in_sock_;
    int neg_out_sock_;

    int down_in_listen_sock_;
    int down_out_listen_sock_;
    int down_in_accepted_;
    int down_out_accepted_;

    int up_in_sock_;
    int up_out_sock_;

    mutable std::mutex state_mutex_;
    ConnectionState upstream_state_;
    ConnectionState downstream_state_;

    mutable std::mutex traffic_mutex_;
    std::mutex send_downstream_mutex_;
    std::mutex send_upstream_mutex_;

    std::thread neg_thread_;
    std::thread heartbeat_thread_;
    std::thread accept_thread_;
    std::thread reconnect_thread_;

    mutable std::mutex registry_mutex_;
    std::map<ServiceType, PortConfig> service_registry_;
    std::map<ServiceType, std::chrono::steady_clock::time_point> last_heartbeat_;

    std::function<void(uint32_t)> call_end_handler_;
    std::function<void(uint32_t, bool)> speech_signal_handler_;
    mutable std::mutex speech_mutex_;
    std::set<uint32_t> speech_active_calls_;

    PortConfig downstream_neighbor_ports_;
    std::mutex downstream_neighbor_mutex_;

    std::mutex synced_registry_mutex_;
    std::map<ServiceType, PortConfig> synced_registry_;
    uint32_t synced_max_call_id_ = 0;

    bool scan_and_bind_ports() {
        const uint16_t BASE_NEG_IN = 22222;
        const uint16_t BASE_NEG_OUT = 33333;
        const uint16_t INCREMENT = 3;
        const int MAX_ATTEMPTS = 100;

        int sock_in = create_listen_socket(BASE_NEG_IN);
        if (sock_in >= 0) {
            int sock_out = create_listen_socket(BASE_NEG_OUT);
            if (sock_out >= 0) {
                neg_in_sock_ = sock_in;
                neg_out_sock_ = sock_out;
                ports_ = PortConfig(BASE_NEG_IN, BASE_NEG_OUT);
                is_master_ = true;
                was_original_master_ = true;
                check_and_reclaim_master();
                return true;
            }
            close(sock_in);
        } else {
            if (try_reclaim_master_port()) {
                return true;
            }
        }

        for (int attempt = 1; attempt < MAX_ATTEMPTS; ++attempt) {
            uint16_t try_neg_in = BASE_NEG_IN + (attempt * INCREMENT);
            uint16_t try_neg_out = BASE_NEG_OUT + (attempt * INCREMENT);

            sock_in = create_listen_socket(try_neg_in);
            if (sock_in < 0) continue;

            int sock_out = create_listen_socket(try_neg_out);
            if (sock_out < 0) {
                close(sock_in);
                continue;
            }

            neg_in_sock_ = sock_in;
            neg_out_sock_ = sock_out;
            ports_ = PortConfig(try_neg_in, try_neg_out);
            is_master_ = false;

            if (!register_with_master()) {
                close_socket(neg_in_sock_);
                close_socket(neg_out_sock_);
                return false;
            }

            return true;
        }

        return false;
    }

    bool try_reclaim_master_port() {
        int sock = connect_to_port("127.0.0.1", 22222);
        if (sock < 0) return false;

        std::string msg = "STEP_DOWN";
        if (!send_all(sock, msg.c_str(), msg.size())) {
            close(sock);
            return false;
        }

        char buf[256];
        ssize_t n = recv_with_timeout(sock, buf, sizeof(buf) - 1, STEP_DOWN_RECV_TIMEOUT_MS);
        close(sock);

        std::string state_str;
        if (n > 0) {
            buf[n] = '\0';
            std::string resp(buf);
            if (resp.substr(0, 12) == "STEPPED_DOWN") {
                if (resp.size() > 13) {
                    state_str = resp.substr(13);
                }
            } else {
                return false;
            }
        } else {
            return false;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(PORT_RECLAIM_WAIT_MS));

        int sock_in = create_listen_socket(22222);
        if (sock_in < 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(PORT_RECLAIM_WAIT_MS));
            sock_in = create_listen_socket(22222);
            if (sock_in < 0) return false;
        }

        int sock_out = create_listen_socket(33333);
        if (sock_out < 0) {
            close(sock_in);
            return false;
        }

        neg_in_sock_ = sock_in;
        neg_out_sock_ = sock_out;
        ports_ = PortConfig(22222, 33333);
        is_master_ = true;
        was_original_master_ = true;

        if (!state_str.empty()) {
            absorb_stepped_down_state(state_str);
        }

        std::fprintf(stderr, "[%s] Reclaimed master role from promoted slave\n",
                    service_type_to_string(type_));
        return true;
    }

    bool bind_traffic_listen_ports() {
        down_in_listen_sock_ = create_listen_socket(ports_.down_in);
        if (down_in_listen_sock_ < 0) return false;

        down_out_listen_sock_ = create_listen_socket(ports_.down_out);
        if (down_out_listen_sock_ < 0) {
            close_socket(down_in_listen_sock_);
            return false;
        }
        return true;
    }

    bool register_with_master() {
        int sock = connect_to_port("127.0.0.1", 22222);
        if (sock < 0) return false;

        std::string msg = "REGISTER " + std::to_string(static_cast<int>(type_)) + " " +
                         std::to_string(ports_.neg_in) + " " + std::to_string(ports_.neg_out);
        
        if (!send_all(sock, msg.c_str(), msg.size())) {
            close(sock);
            return false;
        }

        char buffer[64];
        ssize_t n = recv_with_timeout(sock, buffer, sizeof(buffer) - 1, 2000);
        close(sock);

        if (n <= 0) return false;
        buffer[n] = '\0';
        return std::string(buffer) == "REGISTER_ACK";
    }

    PortConfig query_downstream_ports() {
        ServiceType downstream = downstream_of(type_);

        if (is_master_) {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = service_registry_.find(downstream);
            if (it != service_registry_.end()) return it->second;
            return PortConfig();
        }

        int sock = connect_to_port("127.0.0.1", 22222);
        if (sock < 0) return PortConfig();

        std::string msg = "GET_DOWNSTREAM " + std::to_string(static_cast<int>(type_));
        if (!send_all(sock, msg.c_str(), msg.size())) {
            close(sock);
            return PortConfig();
        }

        char buffer[128];
        ssize_t n = recv_with_timeout(sock, buffer, sizeof(buffer) - 1, 2000);
        close(sock);

        if (n <= 0) return PortConfig();
        buffer[n] = '\0';
        std::string resp(buffer);

        if (resp.substr(0, 6) == "PORTS ") {
            return parse_ports_response(resp);
        }

        return PortConfig();
    }

    PortConfig query_upstream_ports() {
        ServiceType upstream = upstream_of(type_);

        if (is_master_) {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            auto it = service_registry_.find(upstream);
            if (it != service_registry_.end()) return it->second;
            return PortConfig();
        }

        int sock = connect_to_port("127.0.0.1", 22222);
        if (sock < 0) return PortConfig();

        std::string msg = "GET_UPSTREAM " + std::to_string(static_cast<int>(type_));
        if (!send_all(sock, msg.c_str(), msg.size())) {
            close(sock);
            return PortConfig();
        }

        char buffer[128];
        ssize_t n = recv_with_timeout(sock, buffer, sizeof(buffer) - 1, 2000);
        close(sock);

        if (n <= 0) return PortConfig();
        buffer[n] = '\0';
        std::string resp(buffer);

        if (resp.substr(0, 6) == "PORTS ") {
            return parse_ports_response(resp);
        }

        return PortConfig();
    }

    PortConfig parse_ports_response(const std::string& resp) {
        size_t sp1 = resp.find(' ', 6);
        if (sp1 == std::string::npos) return PortConfig();
        uint16_t ni = static_cast<uint16_t>(std::stoul(resp.substr(6, sp1 - 6)));
        uint16_t no = static_cast<uint16_t>(std::stoul(resp.substr(sp1 + 1)));
        return PortConfig(ni, no);
    }

    void negotiation_loop() {
        while (running_) {
            pollfd pfd;
            pfd.fd = neg_in_sock_;
            pfd.events = POLLIN;
            
            int ret = poll(&pfd, 1, 100);
            if (ret <= 0) continue;

            sockaddr_in client_addr;
            socklen_t addr_len = sizeof(client_addr);
            int client_sock = accept(neg_in_sock_, (sockaddr*)&client_addr, &addr_len);
            if (client_sock < 0) continue;

            std::thread([this, client_sock]() {
                handle_negotiation_message(client_sock);
            }).detach();
        }
    }

    void handle_negotiation_message(int sock) {
        char buffer[1024];
        ssize_t n = recv_with_timeout(sock, buffer, sizeof(buffer) - 1, 2000);
        if (n <= 0) {
            close(sock);
            return;
        }

        buffer[n] = '\0';
        std::string msg(buffer);
        std::string response;

        if (is_master_) {
            if (msg.substr(0, 9) == "REGISTER ") {
                size_t sp1 = msg.find(' ', 9);
                size_t sp2 = msg.find(' ', sp1 + 1);
                
                ServiceType svc_type = static_cast<ServiceType>(std::stoi(msg.substr(9, sp1 - 9)));
                uint16_t neg_in = static_cast<uint16_t>(std::stoul(msg.substr(sp1 + 1, sp2 - sp1 - 1)));
                uint16_t neg_out = static_cast<uint16_t>(std::stoul(msg.substr(sp2 + 1)));
                
                std::lock_guard<std::mutex> lock(registry_mutex_);
                service_registry_[svc_type] = PortConfig(neg_in, neg_out);
                last_heartbeat_[svc_type] = std::chrono::steady_clock::now();
                
                response = "REGISTER_ACK";
            }
            else if (msg.substr(0, 16) == "RESERVE_CALL_ID ") {
                uint32_t proposed = std::stoul(msg.substr(16));
                uint32_t reserved = reserve_call_id(proposed);
                response = "CALL_ID_RESERVED " + std::to_string(reserved);
            }
            else if (msg.substr(0, 10) == "HEARTBEAT ") {
                ServiceType svc_type = static_cast<ServiceType>(std::stoi(msg.substr(10)));
                std::lock_guard<std::mutex> lock(registry_mutex_);
                last_heartbeat_[svc_type] = std::chrono::steady_clock::now();
                response = "HEARTBEAT_ACK";
            }
            else if (msg.substr(0, 15) == "GET_DOWNSTREAM ") {
                ServiceType requester = static_cast<ServiceType>(std::stoi(msg.substr(15)));
                ServiceType downstream = downstream_of(requester);
                
                std::lock_guard<std::mutex> lock(registry_mutex_);
                auto it = service_registry_.find(downstream);
                if (it != service_registry_.end()) {
                    response = "PORTS " + std::to_string(it->second.neg_in) + " " + std::to_string(it->second.neg_out);
                } else {
                    response = "SERVICE_UNAVAILABLE";
                }
            }
            else if (msg.substr(0, 13) == "GET_UPSTREAM ") {
                ServiceType requester = static_cast<ServiceType>(std::stoi(msg.substr(13)));
                ServiceType upstream = upstream_of(requester);

                if (upstream == type_) {
                    response = "PORTS " + std::to_string(ports_.neg_in) + " " + std::to_string(ports_.neg_out);
                } else {
                    std::lock_guard<std::mutex> lock(registry_mutex_);
                    auto it = service_registry_.find(upstream);
                    if (it != service_registry_.end()) {
                        response = "PORTS " + std::to_string(it->second.neg_in) + " " + std::to_string(it->second.neg_out);
                    } else {
                        response = "SERVICE_UNAVAILABLE";
                    }
                }
            }
        }

        if (msg.substr(0, 14) == "SYNC_REGISTRY ") {
            std::string data = msg.substr(14);
            size_t sp = data.find(' ');
            uint32_t max_cid = std::stoul(sp == std::string::npos ? data : data.substr(0, sp));

            std::map<ServiceType, PortConfig> registry;
            if (sp != std::string::npos) {
                std::string entries = data.substr(sp + 1);
                size_t pos = 0;
                while (pos < entries.size()) {
                    size_t next = entries.find(' ', pos);
                    std::string entry = (next == std::string::npos) ? entries.substr(pos) : entries.substr(pos, next - pos);
                    if (!entry.empty()) {
                        size_t c1 = entry.find(':');
                        size_t c2 = entry.find(':', c1 + 1);
                        if (c1 != std::string::npos && c2 != std::string::npos) {
                            ServiceType svc = static_cast<ServiceType>(std::stoi(entry.substr(0, c1)));
                            uint16_t ni = static_cast<uint16_t>(std::stoul(entry.substr(c1 + 1, c2 - c1 - 1)));
                            uint16_t no = static_cast<uint16_t>(std::stoul(entry.substr(c2 + 1)));
                            registry[svc] = PortConfig(ni, no);
                        }
                    }
                    if (next == std::string::npos) break;
                    pos = next + 1;
                }
            }

            {
                std::lock_guard<std::mutex> lock(synced_registry_mutex_);
                synced_registry_ = registry;
                synced_max_call_id_ = max_cid;
            }
            response = "SYNC_ACK";
        }
        else if (msg == "STEP_DOWN") {
            if (is_master_ && !was_original_master_) {
                std::string state;
                {
                    std::lock_guard<std::mutex> cid_lock(call_id_mutex_);
                    state = std::to_string(max_known_call_id_);
                }
                {
                    std::lock_guard<std::mutex> reg_lock(registry_mutex_);
                    for (const auto& [svc, cfg] : service_registry_) {
                        state += " " + std::to_string(static_cast<int>(svc)) +
                                 ":" + std::to_string(cfg.neg_in) +
                                 ":" + std::to_string(cfg.neg_out);
                    }
                }
                response = "STEPPED_DOWN " + state;
                send_all(sock, response.c_str(), response.size());
                close(sock);

                demote_to_slave();
                return;
            } else {
                response = "NOT_PROMOTED";
            }
        }
        else if (msg.substr(0, 11) == "NEW_MASTER ") {
            master_heartbeat_failures_ = 0;
            response = "NEW_MASTER_ACK";
        }

        if (msg.substr(0, 14) == "SPEECH_ACTIVE " || msg.substr(0, 12) == "SPEECH_IDLE ") {
            bool active = (msg.substr(0, 14) == "SPEECH_ACTIVE ");
            uint32_t call_id = std::stoul(msg.substr(active ? 14 : 12));

            {
                std::lock_guard<std::mutex> lock(speech_mutex_);
                if (active) speech_active_calls_.insert(call_id);
                else speech_active_calls_.erase(call_id);
            }

            if (speech_signal_handler_) {
                speech_signal_handler_(call_id, active);
            }

            if (is_master_) {
                std::vector<std::pair<ServiceType, uint16_t>> targets;
                {
                    std::lock_guard<std::mutex> reg_lock(registry_mutex_);
                    for (const auto& [type, config] : service_registry_) {
                        targets.push_back({type, config.neg_in});
                    }
                }
                std::string relay = msg;
                for (const auto& [stype, port] : targets) {
                    int rsock = connect_to_port("127.0.0.1", port);
                    if (rsock < 0) continue;
                    send_all(rsock, relay.c_str(), relay.size());
                    ::close(rsock);
                }
            }

            response = active ? "SPEECH_ACTIVE_ACK" : "SPEECH_IDLE_ACK";
        }

        if (msg.substr(0, 9) == "CALL_END ") {
            uint32_t call_id = std::stoul(msg.substr(9));
            
            bool already_cleaned = false;
            {
                std::lock_guard<std::mutex> lock(call_id_mutex_);
                if (ended_call_ids_.count(call_id) > 0) {
                    already_cleaned = true;
                }
                ended_call_ids_.insert(call_id);
                active_call_ids_.erase(call_id);
            }
            
            if (!already_cleaned && call_end_handler_) {
                call_end_handler_(call_id);
            }

            response = "CALL_END_ACK " + std::to_string(call_id);
        }

        if (!response.empty()) {
            send_all(sock, response.c_str(), response.size());
        }

        close(sock);
    }

    void accept_loop() {
        while (running_) {
            bool need_down_in = false;
            bool need_down_out = false;
            {
                std::lock_guard<std::mutex> lock(traffic_mutex_);
                need_down_in = (down_in_accepted_ < 0);
                need_down_out = (down_out_accepted_ < 0);
            }

            if (need_down_in && down_in_listen_sock_ >= 0) {
                pollfd pfd;
                pfd.fd = down_in_listen_sock_;
                pfd.events = POLLIN;
                if (poll(&pfd, 1, 100) > 0) {
                    sockaddr_in addr;
                    socklen_t len = sizeof(addr);
                    int accepted = accept(down_in_listen_sock_, (sockaddr*)&addr, &len);
                    if (accepted >= 0) {
                        setup_socket_options(accepted);
                        std::lock_guard<std::mutex> lock(traffic_mutex_);
                        if (down_in_accepted_ >= 0) {
                            close(down_in_accepted_);
                        }
                        down_in_accepted_ = accepted;
                        update_downstream_state_locked();
                    }
                }
            }

            if (need_down_out && down_out_listen_sock_ >= 0) {
                pollfd pfd;
                pfd.fd = down_out_listen_sock_;
                pfd.events = POLLIN;
                if (poll(&pfd, 1, 100) > 0) {
                    sockaddr_in addr;
                    socklen_t len = sizeof(addr);
                    int accepted = accept(down_out_listen_sock_, (sockaddr*)&addr, &len);
                    if (accepted >= 0) {
                        setup_socket_options(accepted);
                        std::lock_guard<std::mutex> lock(traffic_mutex_);
                        if (down_out_accepted_ >= 0) {
                            close(down_out_accepted_);
                        }
                        down_out_accepted_ = accepted;
                        update_downstream_state_locked();
                    }
                }
            }

            if (!need_down_in && !need_down_out) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
            }
        }
    }

    void update_downstream_state_locked() {
        ConnectionState new_state;
        if (down_in_accepted_ >= 0 && down_out_accepted_ >= 0) {
            new_state = ConnectionState::CONNECTED;
        } else if (down_in_accepted_ >= 0 || down_out_accepted_ >= 0) {
            new_state = ConnectionState::CONNECTING;
        } else {
            new_state = ConnectionState::DISCONNECTED;
        }
        std::lock_guard<std::mutex> lock(state_mutex_);
        downstream_state_ = new_state;
    }

    void heartbeat_loop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(HEARTBEAT_INTERVAL_S));
            if (!running_) break;
            
            if (!is_master_) {
                bool hb_ok = send_heartbeat_to_master();
                if (hb_ok) {
                    master_heartbeat_failures_ = 0;
                } else {
                    master_heartbeat_failures_++;
                    if (master_heartbeat_failures_ >= MASTER_FAILURE_THRESHOLD) {
                        std::fprintf(stderr, "[%s] Master unreachable (%d failures) — attempting promotion\n",
                                    service_type_to_string(type_), master_heartbeat_failures_);
                        if (attempt_promote_to_master()) {
                            master_heartbeat_failures_ = 0;
                        }
                    }
                }
            } else {
                std::vector<ServiceType> crashed;
                {
                    std::lock_guard<std::mutex> lock(registry_mutex_);
                    for (auto it = last_heartbeat_.begin(); it != last_heartbeat_.end(); ) {
                        auto elapsed = std::chrono::steady_clock::now() - it->second;
                        if (elapsed > std::chrono::seconds(5)) {
                            crashed.push_back(it->first);
                            ServiceType crashed_type = it->first;
                            it = last_heartbeat_.erase(it);
                            service_registry_.erase(crashed_type);
                        } else {
                            ++it;
                        }
                    }
                }

                for (ServiceType ct : crashed) {
                    notify_neighbors_of_crash(ct);
                }

                sync_registry_to_slaves();
            }

            check_traffic_connections();
        }
    }

    bool send_heartbeat_to_master() {
        int sock = connect_to_port("127.0.0.1", 22222);
        if (sock < 0) return false;

        std::string msg = "HEARTBEAT " + std::to_string(static_cast<int>(type_));
        if (!send_all(sock, msg.c_str(), msg.size())) {
            close(sock);
            return false;
        }

        char buffer[64];
        ssize_t n = recv_with_timeout(sock, buffer, sizeof(buffer) - 1, HEARTBEAT_RECV_TIMEOUT_MS);
        close(sock);
        return (n > 0);
    }

    void sync_registry_to_slaves() {
        std::string payload;
        std::vector<std::pair<ServiceType, uint16_t>> targets;
        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            std::lock_guard<std::mutex> cid_lock(call_id_mutex_);
            payload = "SYNC_REGISTRY " + std::to_string(max_known_call_id_);
            for (const auto& [svc, cfg] : service_registry_) {
                payload += " " + std::to_string(static_cast<int>(svc)) +
                           ":" + std::to_string(cfg.neg_in) +
                           ":" + std::to_string(cfg.neg_out);
                targets.push_back({svc, cfg.neg_in});
            }
        }

        for (const auto& [svc, port] : targets) {
            int sock = connect_to_port("127.0.0.1", port);
            if (sock < 0) continue;
            send_all(sock, payload.c_str(), payload.size());
            char buf[32];
            recv_with_timeout(sock, buf, sizeof(buf) - 1, SYNC_RECV_TIMEOUT_MS);
            close(sock);
        }
    }

    bool attempt_promote_to_master() {
        const uint16_t MASTER_NEG_IN = 22222;
        const uint16_t MASTER_NEG_OUT = 33333;

        int new_neg_in = create_listen_socket(MASTER_NEG_IN);
        if (new_neg_in < 0) {
            return false;
        }

        int new_neg_out = create_listen_socket(MASTER_NEG_OUT);
        if (new_neg_out < 0) {
            close(new_neg_in);
            return false;
        }

        close_socket(neg_in_sock_);
        close_socket(neg_out_sock_);

        neg_in_sock_ = new_neg_in;
        neg_out_sock_ = new_neg_out;

        PortConfig old_ports = ports_;
        ports_ = PortConfig(MASTER_NEG_IN, MASTER_NEG_OUT);
        is_master_ = true;

        {
            std::lock_guard<std::mutex> lock(synced_registry_mutex_);
            std::lock_guard<std::mutex> reg_lock(registry_mutex_);
            service_registry_ = synced_registry_;
            service_registry_.erase(type_);
            {
                std::lock_guard<std::mutex> cid_lock(call_id_mutex_);
                if (synced_max_call_id_ > max_known_call_id_) {
                    max_known_call_id_ = synced_max_call_id_;
                }
            }
        }

        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            for (auto it = last_heartbeat_.begin(); it != last_heartbeat_.end(); ) {
                it = last_heartbeat_.erase(it);
            }
        }

        rebind_traffic_ports_for_promotion(old_ports);

        std::fprintf(stderr, "[%s] PROMOTED to master (port 22222)\n",
                    service_type_to_string(type_));

        notify_slaves_of_new_master();

        return true;
    }

    void rebind_traffic_ports_for_promotion(const PortConfig& /*old_ports*/) {
        close_socket(down_in_listen_sock_);
        close_socket(down_out_listen_sock_);
        close_socket(down_in_accepted_);
        close_socket(down_out_accepted_);

        down_in_listen_sock_ = create_listen_socket(ports_.down_in);
        down_out_listen_sock_ = create_listen_socket(ports_.down_out);

        if (down_in_listen_sock_ < 0 || down_out_listen_sock_ < 0) {
            std::fprintf(stderr, "[%s] Warning: could not rebind traffic ports after promotion\n",
                        service_type_to_string(type_));
        }

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            downstream_state_ = ConnectionState::DISCONNECTED;
        }
    }

    void notify_slaves_of_new_master() {
        std::vector<std::pair<ServiceType, uint16_t>> targets;
        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            for (const auto& [svc, cfg] : service_registry_) {
                targets.push_back({svc, cfg.neg_in});
            }
        }

        std::string msg = "NEW_MASTER " + std::to_string(static_cast<int>(type_));
        for (const auto& [svc, port] : targets) {
            int sock = connect_to_port("127.0.0.1", port);
            if (sock < 0) continue;
            send_all(sock, msg.c_str(), msg.size());
            char buf[32];
            recv_with_timeout(sock, buf, sizeof(buf) - 1, 500);
            close(sock);
        }
    }

    void check_and_reclaim_master() {
        std::vector<uint16_t> slave_ports;
        {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            for (const auto& [svc, cfg] : service_registry_) {
                slave_ports.push_back(cfg.neg_in);
            }
        }

        for (uint16_t port : slave_ports) {
            int sock = connect_to_port("127.0.0.1", port);
            if (sock < 0) continue;
            std::string msg = "STEP_DOWN";
            send_all(sock, msg.c_str(), msg.size());
            char buf[64];
            ssize_t n = recv_with_timeout(sock, buf, sizeof(buf) - 1, 2000);
            close(sock);

            if (n > 0) {
                buf[n] = '\0';
                std::string resp(buf);
                if (resp.substr(0, 12) == "STEPPED_DOWN") {
                    std::fprintf(stderr, "[%s] Reclaimed master from promoted slave at port %u\n",
                                service_type_to_string(type_), port);
                    if (resp.size() > 13) {
                        absorb_stepped_down_state(resp.substr(13));
                    }
                }
            }
        }
    }

    void absorb_stepped_down_state(const std::string& state_str) {
        size_t sp = state_str.find(' ');
        if (sp == std::string::npos && !state_str.empty()) {
            uint32_t their_max = std::stoul(state_str);
            std::lock_guard<std::mutex> lock(call_id_mutex_);
            if (their_max > max_known_call_id_) {
                max_known_call_id_ = their_max;
            }
            return;
        }
        if (sp == std::string::npos) return;

        uint32_t their_max = std::stoul(state_str.substr(0, sp));
        {
            std::lock_guard<std::mutex> lock(call_id_mutex_);
            if (their_max > max_known_call_id_) {
                max_known_call_id_ = their_max;
            }
        }

        std::string entries = state_str.substr(sp + 1);
        std::lock_guard<std::mutex> lock(registry_mutex_);
        size_t pos = 0;
        while (pos < entries.size()) {
            size_t next = entries.find(' ', pos);
            std::string entry = (next == std::string::npos) ? entries.substr(pos) : entries.substr(pos, next - pos);
            if (!entry.empty()) {
                size_t c1 = entry.find(':');
                size_t c2 = entry.find(':', c1 + 1);
                if (c1 != std::string::npos && c2 != std::string::npos) {
                    ServiceType svc = static_cast<ServiceType>(std::stoi(entry.substr(0, c1)));
                    uint16_t ni = static_cast<uint16_t>(std::stoul(entry.substr(c1 + 1, c2 - c1 - 1)));
                    uint16_t no = static_cast<uint16_t>(std::stoul(entry.substr(c2 + 1)));
                    if (service_registry_.find(svc) == service_registry_.end()) {
                        service_registry_[svc] = PortConfig(ni, no);
                        last_heartbeat_[svc] = std::chrono::steady_clock::now();
                    }
                }
            }
            if (next == std::string::npos) break;
            pos = next + 1;
        }
    }

    bool demote_to_slave() {
        std::fprintf(stderr, "[%s] Demoting from master to slave\n", service_type_to_string(type_));

        is_master_ = false;
        was_original_master_ = false;

        close_socket(neg_in_sock_);
        close_socket(neg_out_sock_);
        close_socket(down_in_listen_sock_);
        close_socket(down_out_listen_sock_);
        close_socket(down_in_accepted_);
        close_socket(down_out_accepted_);

        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            downstream_state_ = ConnectionState::DISCONNECTED;
        }

        const uint16_t BASE_NEG_IN = 22222;
        const uint16_t BASE_NEG_OUT = 33333;
        const uint16_t INCREMENT = 3;
        const int MAX_ATTEMPTS = 100;
        bool bound = false;

        for (int attempt = 1; attempt < MAX_ATTEMPTS; ++attempt) {
            uint16_t try_neg_in = BASE_NEG_IN + (attempt * INCREMENT);
            uint16_t try_neg_out = BASE_NEG_OUT + (attempt * INCREMENT);

            int sock_in = create_listen_socket(try_neg_in);
            if (sock_in < 0) continue;

            int sock_out = create_listen_socket(try_neg_out);
            if (sock_out < 0) {
                close(sock_in);
                continue;
            }

            neg_in_sock_ = sock_in;
            neg_out_sock_ = sock_out;
            ports_ = PortConfig(try_neg_in, try_neg_out);
            bound = true;
            break;
        }

        if (!bound) {
            std::fprintf(stderr, "[%s] Failed to bind slave ports after demotion\n",
                        service_type_to_string(type_));
            return false;
        }

        down_in_listen_sock_ = create_listen_socket(ports_.down_in);
        down_out_listen_sock_ = create_listen_socket(ports_.down_out);

        bool registered = false;
        for (int retry = 0; retry < DEMOTION_REGISTER_RETRIES; ++retry) {
            if (register_with_master()) {
                registered = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(
                DEMOTION_REGISTER_BACKOFF_MS * (1 << retry)));
        }
        if (!registered) {
            std::fprintf(stderr, "[%s] Failed to register with master after %d retries\n",
                        service_type_to_string(type_), DEMOTION_REGISTER_RETRIES);
        }

        master_heartbeat_failures_ = 0;

        std::fprintf(stderr, "[%s] Demoted to slave (ports %u/%u)\n",
                    service_type_to_string(type_), ports_.neg_in, ports_.neg_out);
        return true;
    }

    void notify_neighbors_of_crash(ServiceType crashed_type) {
        ServiceType up = upstream_of(crashed_type);
        ServiceType down = downstream_of(crashed_type);

        std::string msg = "SERVICE_CRASHED " + std::to_string(static_cast<int>(crashed_type));

        auto send_crash_notification = [&](ServiceType target) {
            uint16_t port = 0;
            if (target == type_) {
                if (call_end_handler_) {}
                return;
            }
            {
                std::lock_guard<std::mutex> lock(registry_mutex_);
                auto it = service_registry_.find(target);
                if (it != service_registry_.end()) {
                    port = it->second.neg_in;
                }
            }
            if (port > 0) {
                int sock = connect_to_port("127.0.0.1", port);
                if (sock >= 0) {
                    send_all(sock, msg.c_str(), msg.size());
                    close(sock);
                }
            }
        };

        send_crash_notification(up);
        send_crash_notification(down);
    }

    void reconnect_loop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            if (!running_) break;

            ConnectionState us;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                us = upstream_state_;
            }

            if (us == ConnectionState::DISCONNECTED || us == ConnectionState::FAILED) {
                connect_to_downstream();
            }
        }
    }

    void check_traffic_connections() {
        bool upstream_dead = false;
        bool downstream_dead = false;

        {
            std::lock_guard<std::mutex> lock(traffic_mutex_);
            if (up_out_sock_ >= 0) {
                char probe;
                int flags = fcntl(up_out_sock_, F_GETFL);
                fcntl(up_out_sock_, F_SETFL, flags | O_NONBLOCK);
                ssize_t r = recv(up_out_sock_, &probe, 1, MSG_PEEK);
                fcntl(up_out_sock_, F_SETFL, flags);
                if (r == 0) {
                    close_socket(up_out_sock_);
                    close_socket(up_in_sock_);
                    upstream_dead = true;
                }
            }

            if (down_in_accepted_ >= 0) {
                char probe;
                int flags = fcntl(down_in_accepted_, F_GETFL);
                fcntl(down_in_accepted_, F_SETFL, flags | O_NONBLOCK);
                ssize_t r = recv(down_in_accepted_, &probe, 1, MSG_PEEK);
                fcntl(down_in_accepted_, F_SETFL, flags);
                if (r == 0) {
                    close_socket(down_in_accepted_);
                    close_socket(down_out_accepted_);
                    downstream_dead = true;
                }
            }
        }

        if (upstream_dead || downstream_dead) {
            std::lock_guard<std::mutex> sl(state_mutex_);
            if (upstream_dead) upstream_state_ = ConnectionState::FAILED;
            if (downstream_dead) downstream_state_ = ConnectionState::DISCONNECTED;
        }
    }

    void mark_upstream_failed() {
        {
            std::lock_guard<std::mutex> lock(traffic_mutex_);
            close_socket(up_out_sock_);
            close_socket(up_in_sock_);
        }
        std::lock_guard<std::mutex> sl(state_mutex_);
        upstream_state_ = ConnectionState::FAILED;
    }

    void mark_downstream_failed() {
        {
            std::lock_guard<std::mutex> lock(traffic_mutex_);
            close_socket(down_in_accepted_);
            close_socket(down_out_accepted_);
        }
        std::lock_guard<std::mutex> sl(state_mutex_);
        downstream_state_ = ConnectionState::DISCONNECTED;
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
            close(sock);
            return -1;
        }

        if (listen(sock, 10) < 0) {
            close(sock);
            return -1;
        }

        return sock;
    }

    int connect_to_port(const char* host, uint16_t port) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return -1;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(host);
        addr.sin_port = htons(port);

        if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
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
            close(sock);
            return -1;
        }

        if (ret == 0) {
            fcntl(sock, F_SETFL, flags);
            return sock;
        }

        pollfd pfd;
        pfd.fd = sock;
        pfd.events = POLLOUT;
        int poll_ret = poll(&pfd, 1, timeout_ms);

        if (poll_ret <= 0) {
            close(sock);
            return -1;
        }

        int error = 0;
        socklen_t errlen = sizeof(error);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &errlen);
        if (error != 0) {
            close(sock);
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

            pollfd pfd;
            pfd.fd = sock;
            pfd.events = POLLOUT;
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
        pollfd pfd;
        pfd.fd = sock;
        pfd.events = POLLIN;
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

            pollfd pfd;
            pfd.fd = sock;
            pfd.events = POLLIN;
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
            close(sock);
            sock = -1;
        }
    }
};

}
