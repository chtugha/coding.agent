// Outbound Audio Processor (Consolidated)
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
#include <cmath>

struct CallState {
    int id;
    int tcp_socket = -1;
    int listen_socket = -1;
    std::atomic<bool> connected{false};
    std::mutex mutex;
    std::vector<uint8_t> buffer;
    std::chrono::steady_clock::time_point last_activity;
};

class OutboundAudioProcessor {
public:
    OutboundAudioProcessor() : running_(true) {
        init_g711_tables();
    }

    void run() {
        std::thread scheduler_thread(&OutboundAudioProcessor::scheduler_loop, this);
        std::thread ctrl_thread(&OutboundAudioProcessor::control_socket_loop, this);
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            cleanup_inactive_calls();
        }
        scheduler_thread.join();
        ctrl_thread.join();
    }

private:
    void init_g711_tables() {
        // Simple μ-law encode table
        for (int i = -32768; i <= 32767; ++i) {
            // This is a slow way to init, but only done once
        }
        // Actually we'll just use a formula in realtime if needed, or a 65k table.
        // For brevity in this consolidation, we'll use a standard formula.
    }

    uint8_t linear_to_ulaw(int16_t pcm) {
        int mask = 0x7FFF;
        int sign = 0;
        if (pcm < 0) {
            pcm = -pcm;
            sign = 0x80;
        }
        pcm += 128 + 4; // bias
        if (pcm > mask) pcm = mask;
        int exponent = 7;
        for (int exp_mask = 0x4000; (pcm & exp_mask) == 0 && exponent > 0; exp_mask >>= 1) exponent--;
        int mantissa = (pcm >> (exponent + 3)) & 0x0F;
        return ~(sign | (exponent << 4) | mantissa);
    }

    void scheduler_loop() {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in dest_addr{};
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(9002);
        dest_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        auto next = std::chrono::steady_clock::now();
        while (running_) {
            std::vector<std::shared_ptr<CallState>> active;
            {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                for (auto& p : calls_) active.push_back(p.second);
            }

            for (auto& state : active) {
                uint8_t frame[160 + 4];
                uint32_t cid_net = htonl(state->id);
                memcpy(frame, &cid_net, 4);

                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->buffer.size() >= 160) {
                    memcpy(frame + 4, state->buffer.data(), 160);
                    state->buffer.erase(state->buffer.begin(), state->buffer.begin() + 160);
                } else {
                    memset(frame + 4, 0xFF, 160); // Silence
                }
                sendto(sock, frame, sizeof(frame), 0, (struct sockaddr*)&dest_addr, sizeof(dest_addr));
            }

            next += std::chrono::milliseconds(20);
            std::this_thread::sleep_until(next);
        }
    }

    void control_socket_loop() {
        const char* path = "/tmp/outbound-audio-processor.ctrl";
        unlink(path);
        int lsock = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un addr{}; addr.sun_family = AF_UNIX; strncpy(addr.sun_path, path, sizeof(addr.sun_path)-1);
        bind(lsock, (struct sockaddr*)&addr, sizeof(addr));
        listen(lsock, 5);

        while (running_) {
            int csock = accept(lsock, NULL, NULL);
            if (csock < 0) continue;
            char buf[128];
            ssize_t n = recv(csock, buf, sizeof(buf)-1, 0);
            if (n > 0) {
                buf[n] = '\0';
                std::string cmd(buf);
                if (cmd.find("ACTIVATE:") == 0) {
                    int cid = std::stoi(cmd.substr(9));
                    activate_call(cid);
                }
            }
            close(csock);
        }
    }

    void activate_call(int cid) {
        auto state = get_or_create_call(cid);
        if (state->listen_socket != -1) return;

        state->listen_socket = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1; setsockopt(state->listen_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(8090 + cid);
        addr.sin_addr.s_addr = INADDR_ANY;
        bind(state->listen_socket, (struct sockaddr*)&addr, sizeof(addr));
        listen(state->listen_socket, 1);

        std::thread([this, state]() {
            while (running_ && state->listen_socket != -1) {
                int csock = accept(state->listen_socket, NULL, NULL);
                if (csock < 0) continue;
                state->connected = true;
                std::cout << "🔗 TTS connected for call " << state->id << std::endl;
                
                float pcm_buf[1024];
                while (running_ && state->connected) {
                    ssize_t n = recv(csock, pcm_buf, sizeof(pcm_buf), 0);
                    if (n <= 0) break;
                    
                    size_t samples = n / 4;
                    std::vector<uint8_t> ulaw;
                    // Simple downsampling (assumes 24kHz or similar to 8kHz)
                    // We'll just take every 3rd sample if 24kHz, or more generally:
                    for (size_t i = 0; i < samples; i += 3) {
                        int16_t s16 = static_cast<int16_t>(pcm_buf[i] * 32767.0f);
                        ulaw.push_back(linear_to_ulaw(s16));
                    }

                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->buffer.insert(state->buffer.end(), ulaw.begin(), ulaw.end());
                    state->last_activity = std::chrono::steady_clock::now();
                }
                state->connected = false;
                close(csock);
            }
        }).detach();
        std::cout << "👂 Listening for TTS on port " << (8090 + cid) << std::endl;
    }

    std::shared_ptr<CallState> get_or_create_call(int cid) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(cid)) return calls_[cid];
        auto state = std::make_shared<CallState>();
        state->id = cid;
        state->last_activity = std::chrono::steady_clock::now();
        calls_[cid] = state;
        return state;
    }

    void cleanup_inactive_calls() {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(calls_mutex_);
        for (auto it = calls_.begin(); it != calls_.end(); ) {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second->last_activity).count() > 60) {
                std::lock_guard<std::mutex> clock(it->second->mutex);
                if (it->second->listen_socket != -1) close(it->second->listen_socket);
                it = calls_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::atomic<bool> running_;
    std::mutex calls_mutex_;
    std::map<int, std::shared_ptr<CallState>> calls_;
};

int main() {
    OutboundAudioProcessor proc;
    proc.run();
    return 0;
}
