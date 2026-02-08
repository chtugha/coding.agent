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

struct CallState {
    int id;
    int tcp_socket = -1;
    int listen_socket = -1;
    std::atomic<bool> connected{false};
    std::mutex mutex;
    std::chrono::steady_clock::time_point last_activity;
};

class InboundAudioProcessor {
public:
    InboundAudioProcessor() : running_(true) {
        init_g711_tables();
    }

    void run() {
        std::thread udp_thread(&InboundAudioProcessor::udp_receiver_loop, this);
        std::thread ctrl_thread(&InboundAudioProcessor::control_socket_loop, this);
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            cleanup_inactive_calls();
        }
        udp_thread.join();
        ctrl_thread.join();
    }

private:
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
            size_t payload_len = n - 16; // 4 bytes CID + 12 bytes RTP header
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
                }
            }
        }
    }

    void control_socket_loop() {
        const char* path = "/tmp/inbound-audio-processor.ctrl";
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
        addr.sin_port = htons(13000 + cid);
        addr.sin_addr.s_addr = INADDR_ANY;
        bind(state->listen_socket, (struct sockaddr*)&addr, sizeof(addr));
        listen(state->listen_socket, 1);

        std::thread([this, state]() {
            while (running_ && state->listen_socket != -1) {
                int csock = accept(state->listen_socket, NULL, NULL);
                if (csock < 0) continue;
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->tcp_socket != -1) close(state->tcp_socket);
                state->tcp_socket = csock;
                state->connected = true;
                std::cout << "🔗 Whisper connected for call " << state->id << std::endl;
            }
        }).detach();
        std::cout << "👂 Listening for Whisper on port " << (13000 + cid) << std::endl;
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
                if (it->second->listen_socket != -1) close(it->second->listen_socket);
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
};

int main() {
    InboundAudioProcessor proc;
    proc.run();
    return 0;
}
