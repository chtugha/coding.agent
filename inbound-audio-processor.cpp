// Inbound Audio Processor (Consolidated)
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/un.h>
#include <fcntl.h>

class ControlSignalSender {
public:
    bool send_signal(const std::string& socket_path, const std::string& message) {
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) return false;

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            return false;
        }

        ssize_t sent = send(sock, message.c_str(), message.length(), MSG_NOSIGNAL);
        close(sock);
        return sent > 0;
    }
};

class ControlListener {
public:
    ControlListener(const std::string& socket_path, std::function<void(const std::string&)> callback)
        : socket_path_(socket_path), callback_(callback), running_(false) {}

    ~ControlListener() {
        stop();
    }

    void start() {
        running_ = true;
        listen_thread_ = std::thread(&ControlListener::listen_loop, this);
    }

    void stop() {
        running_ = false;
        if (listen_thread_.joinable()) {
            listen_thread_.join();
        }
        unlink(socket_path_.c_str());
    }

private:
    void listen_loop() {
        unlink(socket_path_.c_str());
        int lsock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (lsock < 0) {
            std::cerr << "❌ Failed to create control socket: " << socket_path_ << std::endl;
            return;
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

        if (bind(lsock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "❌ Failed to bind control socket: " << socket_path_ << std::endl;
            close(lsock);
            return;
        }

        listen(lsock, 10);
        std::cout << "🎧 Control listener started: " << socket_path_ << std::endl;

        while (running_) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(lsock, &readfds);
            
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;

            int ret = select(lsock + 1, &readfds, NULL, NULL, &tv);
            if (ret <= 0) continue;

            int csock = accept(lsock, NULL, NULL);
            if (csock < 0) continue;

            char buf[256];
            ssize_t n = recv(csock, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                std::string msg(buf);
                callback_(msg);
            }
            close(csock);
        }
        close(lsock);
    }

    std::string socket_path_;
    std::function<void(const std::string&)> callback_;
    std::atomic<bool> running_;
    std::thread listen_thread_;
};

struct CallState {
    int id;
    int tcp_socket = -1;
    std::atomic<bool> connected{false};
    std::mutex mutex;
    std::chrono::steady_clock::time_point last_activity;
    std::chrono::steady_clock::time_point last_reconnect_attempt;
};

class InboundAudioProcessor {
public:
    InboundAudioProcessor() : running_(true),
        ctrl_listener_("/tmp/inbound-audio-processor.ctrl",
                       [this](const std::string& msg) { handle_control_signal(msg); }) {
        init_g711_tables();
    }

    void run() {
        ctrl_listener_.start();
        std::thread udp_thread(&InboundAudioProcessor::udp_receiver_loop, this);
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            cleanup_inactive_calls();
        }
        udp_thread.join();
        ctrl_listener_.stop();
    }

private:
    void handle_control_signal(const std::string& msg) {
        if (msg.find("CALL_START:") == 0) {
            int call_id = std::stoi(msg.substr(11));
            std::cout << "🚦 CALL_START received for call_id " << call_id << std::endl;
            
            auto state = get_or_create_call(call_id);
            state->last_activity = std::chrono::steady_clock::now();
            std::cout << "📋 Pre-allocated CallState for call_id " << call_id << std::endl;
            
            ctrl_sender_.send_signal("/tmp/whisper-service.ctrl", msg);
        } else if (msg.find("CALL_END:") == 0) {
            int call_id = std::stoi(msg.substr(9));
            std::cout << "🚦 CALL_END received for call_id " << call_id << std::endl;
            
            ctrl_sender_.send_signal("/tmp/whisper-service.ctrl", msg);
            
            std::thread([this, call_id]() {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                
                std::lock_guard<std::mutex> lock(calls_mutex_);
                if (calls_.count(call_id)) {
                    auto state = calls_[call_id];
                    std::lock_guard<std::mutex> clock(state->mutex);
                    if (state->tcp_socket != -1) {
                        close(state->tcp_socket);
                        state->tcp_socket = -1;
                    }
                    state->connected = false;
                    calls_.erase(call_id);
                    std::cout << "🧹 Cleaned up call_id " << call_id << " (200ms grace period)" << std::endl;
                }
            }).detach();
        }
    }

    void init_g711_tables() {
        for (int i = 0; i < 256; ++i) {
            // μ-law decode (simplified)
            int mu = ~i;
            int sign = (mu & 0x80);
            int exponent = (mu & 0x70) >> 4;
            int mantissa = mu & 0x0F;
            int sample = (mantissa << (exponent + 3)) + (1 << (exponent + 2)) - 33;
            if (exponent > 0) sample += (0x21 << exponent);
            ulaw_table[i] = (sign ? -sample : sample) / 32768.0f;
        }
    }

    void udp_receiver_loop() {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(9001);
        addr.sin_addr.s_addr = INADDR_ANY;
        bind(sock, (struct sockaddr*)&addr, sizeof(addr));

        uint8_t buf[2048];
        while (running_) {
            struct sockaddr_in sender{};
            socklen_t slen = sizeof(sender);
            ssize_t n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&sender, &slen);
            if (n < 16) continue;

            uint32_t cid_net; memcpy(&cid_net, buf, 4);
            int cid = ntohl(cid_net);
            
            auto state = get_or_create_call(cid);
            state->last_activity = std::chrono::steady_clock::now();

            // Decode and Upsample (8kHz PCMU -> 16kHz float32)
            size_t payload_len = n - 16;
            std::vector<float> pcm(payload_len * 2);
            const uint8_t* rtp_payload = buf + 16;
            for (size_t i = 0; i < payload_len; ++i) {
                float s = ulaw_table[rtp_payload[i]];
                pcm[i*2] = s;
                float next = (i+1 < payload_len) ? ulaw_table[rtp_payload[i+1]] : s;
                pcm[i*2 + 1] = 0.5f * (s + next);
            }

            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->connected) {
                if (send(state->tcp_socket, pcm.data(), pcm.size() * 4, MSG_NOSIGNAL) < 0) {
                    state->connected = false;
                    close(state->tcp_socket);
                    state->tcp_socket = -1;
                    std::cout << "💔 Whisper disconnected for call " << cid << ", dumping stream." << std::endl;
                }
            } else {
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - state->last_reconnect_attempt).count() > 2) {
                    state->last_reconnect_attempt = now;
                    int sock = socket(AF_INET, SOCK_STREAM, 0);
                    struct sockaddr_in addr{};
                    addr.sin_family = AF_INET;
                    addr.sin_port = htons(13000 + cid);
                    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
                    
                    struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 100000; // 100ms
                    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

                    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
                        state->tcp_socket = sock;
                        state->connected = true;
                        std::cout << "🔗 Whisper connected for call " << cid << std::endl;
                        send(sock, pcm.data(), pcm.size() * 4, MSG_NOSIGNAL);
                    } else {
                        close(sock);
                    }
                }
            }
        }
    }

    std::shared_ptr<CallState> get_or_create_call(int cid) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(cid)) return calls_[cid];
        auto state = std::make_shared<CallState>();
        state->id = cid;
        calls_[cid] = state;
        return state;
    }

    void cleanup_inactive_calls() {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(calls_mutex_);
        for (auto it = calls_.begin(); it != calls_.end(); ) {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second->last_activity).count() > 60) {
                std::lock_guard<std::mutex> clock(it->second->mutex);
                if (it->second->tcp_socket != -1) close(it->second->tcp_socket);
                it = calls_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::atomic<bool> running_;
    float ulaw_table[256];
    std::mutex calls_mutex_;
    std::map<int, std::shared_ptr<CallState>> calls_;
    ControlListener ctrl_listener_;
    ControlSignalSender ctrl_sender_;
};

int main() {
    InboundAudioProcessor proc;
    proc.run();
    return 0;
}
