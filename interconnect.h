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
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

namespace whispertalk {

enum class ServiceType : uint8_t {
    SIP_CLIENT = 1,
    INBOUND_AUDIO_PROCESSOR = 2,
    WHISPER_SERVICE = 3,
    LLAMA_SERVICE = 4,
    KOKORO_SERVICE = 5,
    OUTBOUND_AUDIO_PROCESSOR = 6
};

inline const char* service_type_to_string(ServiceType type) {
    switch (type) {
        case ServiceType::SIP_CLIENT: return "SIP_CLIENT";
        case ServiceType::INBOUND_AUDIO_PROCESSOR: return "INBOUND_AUDIO_PROCESSOR";
        case ServiceType::WHISPER_SERVICE: return "WHISPER_SERVICE";
        case ServiceType::LLAMA_SERVICE: return "LLAMA_SERVICE";
        case ServiceType::KOKORO_SERVICE: return "KOKORO_SERVICE";
        case ServiceType::OUTBOUND_AUDIO_PROCESSOR: return "OUTBOUND_AUDIO_PROCESSOR";
        default: return "UNKNOWN";
    }
}

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

    Packet() : call_id(0), payload_size(0) {}
    
    Packet(uint32_t cid, const void* data, uint32_t size) 
        : call_id(cid), payload_size(size) {
        if (size > 0 && data != nullptr) {
            payload.resize(size);
            memcpy(payload.data(), data, size);
        }
    }

    bool is_valid() const {
        return call_id != 0 && payload_size <= MAX_PAYLOAD_SIZE && payload.size() == payload_size;
    }

    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> buffer(8 + payload_size);
        uint32_t net_call_id = htonl(call_id);
        uint32_t net_size = htonl(payload_size);
        memcpy(buffer.data(), &net_call_id, 4);
        memcpy(buffer.data() + 4, &net_size, 4);
        if (payload_size > 0) {
            memcpy(buffer.data() + 8, payload.data(), payload_size);
        }
        return buffer;
    }

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
          running_(false),
          max_known_call_id_(0),
          neg_in_sock_(-1),
          neg_out_sock_(-1) {}

    ~InterconnectNode() {
        shutdown();
    }

    bool initialize() {
        if (!scan_and_bind_ports()) {
            return false;
        }

        running_ = true;

        neg_thread_ = std::thread(&InterconnectNode::negotiation_loop, this);
        heartbeat_thread_ = std::thread(&InterconnectNode::heartbeat_loop, this);

        return true;
    }

    void shutdown() {
        running_ = false;
        
        if (neg_thread_.joinable()) neg_thread_.join();
        if (heartbeat_thread_.joinable()) heartbeat_thread_.join();

        close_socket(neg_in_sock_);
        close_socket(neg_out_sock_);
    }

    bool is_master() const { return is_master_; }
    const PortConfig& ports() const { return ports_; }

    uint32_t reserve_call_id(uint32_t proposed_id) {
        if (is_master_) {
            std::lock_guard<std::mutex> lock(call_id_mutex_);
            uint32_t final_id = (proposed_id > max_known_call_id_) ? proposed_id : max_known_call_id_ + 1;
            max_known_call_id_ = final_id;
            active_call_ids_.insert(final_id);
            return final_id;
        } else {
            int sock = connect_to_port("127.0.0.1", 22222);
            if (sock < 0) return 0;

            std::string msg = "RESERVE_CALL_ID " + std::to_string(proposed_id);
            if (!send_all(sock, msg.c_str(), msg.size())) {
                close(sock);
                return 0;
            }

            char buffer[64];
            ssize_t n = recv(sock, buffer, sizeof(buffer) - 1, 0);
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

    void broadcast_call_end(uint32_t call_id) {
        {
            std::lock_guard<std::mutex> lock(call_id_mutex_);
            active_call_ids_.erase(call_id);
        }

        if (is_master_) {
            std::string msg = "CALL_END " + std::to_string(call_id);
            std::lock_guard<std::mutex> reg_lock(registry_mutex_);
            
            for (const auto& [type, config] : service_registry_) {
                int sock = connect_to_port("127.0.0.1", config.neg_in);
                if (sock >= 0) {
                    send_all(sock, msg.c_str(), msg.size());
                    close(sock);
                }
            }
        }

        if (call_end_handler_) {
            call_end_handler_(call_id);
        }
    }

    void register_call_end_handler(std::function<void(uint32_t)> handler) {
        call_end_handler_ = handler;
    }

    bool is_service_alive(ServiceType svc_type) const {
        if (!is_master_) return false;
        
        std::lock_guard<std::mutex> lock(registry_mutex_);
        auto it = last_heartbeat_.find(svc_type);
        if (it == last_heartbeat_.end()) return false;
        
        auto elapsed = std::chrono::steady_clock::now() - it->second;
        return elapsed < std::chrono::seconds(5);
    }

    ConnectionState upstream_state() const { return ConnectionState::DISCONNECTED; }
    ConnectionState downstream_state() const { return ConnectionState::DISCONNECTED; }

private:
    ServiceType type_;
    bool is_master_;
    std::atomic<bool> running_;
    PortConfig ports_;

    std::mutex call_id_mutex_;
    uint32_t max_known_call_id_;
    std::set<uint32_t> active_call_ids_;

    int neg_in_sock_;
    int neg_out_sock_;

    std::thread neg_thread_;
    std::thread heartbeat_thread_;

    mutable std::mutex registry_mutex_;
    std::map<ServiceType, PortConfig> service_registry_;
    std::map<ServiceType, std::chrono::steady_clock::time_point> last_heartbeat_;

    std::function<void(uint32_t)> call_end_handler_;

    bool scan_and_bind_ports() {
        const uint16_t BASE_NEG_IN = 22222;
        const uint16_t BASE_NEG_OUT = 33333;
        const uint16_t INCREMENT = 3;
        const int MAX_ATTEMPTS = 100;

        for (int attempt = 0; attempt < MAX_ATTEMPTS; ++attempt) {
            uint16_t try_neg_in = BASE_NEG_IN + (attempt * INCREMENT);
            uint16_t try_neg_out = BASE_NEG_OUT + (attempt * INCREMENT);

            int sock_in = create_bind_socket(try_neg_in);
            if (sock_in < 0) continue;

            int sock_out = create_bind_socket(try_neg_out);
            if (sock_out < 0) {
                close(sock_in);
                continue;
            }

            neg_in_sock_ = sock_in;
            neg_out_sock_ = sock_out;
            ports_ = PortConfig(try_neg_in, try_neg_out);

            if (attempt == 0) {
                is_master_ = true;
            } else {
                is_master_ = false;
                if (!register_with_master()) {
                    close_socket(neg_in_sock_);
                    close_socket(neg_out_sock_);
                    return false;
                }
            }

            return true;
        }

        return false;
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
        pollfd pfd;
        pfd.fd = sock;
        pfd.events = POLLIN;

        if (poll(&pfd, 1, 1000) > 0) {
            recv(sock, buffer, sizeof(buffer) - 1, 0);
        }

        close(sock);
        return true;
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
        ssize_t n = recv(sock, buffer, sizeof(buffer) - 1, 0);
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
                uint16_t neg_in = std::stoul(msg.substr(sp1 + 1, sp2 - sp1 - 1));
                uint16_t neg_out = std::stoul(msg.substr(sp2 + 1));
                
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
        } else {
            if (msg.substr(0, 9) == "CALL_END ") {
                uint32_t call_id = std::stoul(msg.substr(9));
                
                {
                    std::lock_guard<std::mutex> lock(call_id_mutex_);
                    active_call_ids_.erase(call_id);
                }
                
                if (call_end_handler_) {
                    call_end_handler_(call_id);
                }
            }
        }

        if (!response.empty()) {
            send_all(sock, response.c_str(), response.size());
        }

        close(sock);
    }

    void heartbeat_loop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(2));
            
            if (!is_master_) {
                int sock = connect_to_port("127.0.0.1", 22222);
                if (sock >= 0) {
                    std::string msg = "HEARTBEAT " + std::to_string(static_cast<int>(type_));
                    send_all(sock, msg.c_str(), msg.size());
                    
                    char buffer[64];
                    recv(sock, buffer, sizeof(buffer) - 1, 0);
                    close(sock);
                }
            } else {
                std::lock_guard<std::mutex> lock(registry_mutex_);
                for (auto it = last_heartbeat_.begin(); it != last_heartbeat_.end(); ) {
                    auto elapsed = std::chrono::steady_clock::now() - it->second;
                    if (elapsed > std::chrono::seconds(5)) {
                        ServiceType crashed_type = it->first;
                        it = last_heartbeat_.erase(it);
                        service_registry_.erase(crashed_type);
                    } else {
                        ++it;
                    }
                }
            }
        }
    }

    int create_bind_socket(uint16_t port) {
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

    void close_socket(int& sock) {
        if (sock >= 0) {
            close(sock);
            sock = -1;
        }
    }
};

}
