#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <chrono>
#include <cstring>
#include <signal.h>
#include "interconnect.h"

static std::atomic<bool> g_running{true};
static void sig_handler(int) { g_running = false; }

static constexpr size_t RTP_HEADER_SIZE = 12;
static constexpr size_t ULAW_FRAME_SIZE = 160;
static constexpr int HB_LEN = 15;
static constexpr int HB_CENTER = 7;

struct CallState {
    int id;
    std::chrono::steady_clock::time_point last_activity;
    float fir_history[HB_CENTER] = {};
    float decoded[ULAW_FRAME_SIZE];
    float ext[HB_CENTER + ULAW_FRAME_SIZE];
    float pcm[ULAW_FRAME_SIZE * 2];
};

class InboundAudioProcessor {
public:
    InboundAudioProcessor() : running_(true), interconnect_(whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR) {
        init_g711_tables();
    }

    bool init() {
        if (!interconnect_.initialize()) {
            std::cerr << "Failed to initialize interconnect" << std::endl;
            return false;
        }

        std::cout << "Interconnect initialized (peer-to-peer)" << std::endl;

        log_fwd_.init(whispertalk::FRONTEND_LOG_PORT, whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR);

        if (!interconnect_.connect_to_downstream()) {
            std::cout << "⚠️  Downstream (Whisper) not available yet - will auto-reconnect" << std::endl;
        }

        interconnect_.register_call_end_handler([this](uint32_t call_id) {
            this->handle_call_end(call_id);
        });

        return true;
    }

    void run() {
        std::thread processor_thread(&InboundAudioProcessor::processing_loop, this);
        std::thread cmd_thread(&InboundAudioProcessor::command_listener_loop, this);

        while (running_ && g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            cleanup_inactive_calls();
        }
        running_ = false;

        int sock = cmd_sock_.exchange(-1);
        if (sock >= 0) ::close(sock);
        processor_thread.join();
        cmd_thread.join();
        interconnect_.shutdown();
    }

private:
    void command_listener_loop() {
        uint16_t port = whispertalk::service_cmd_port(whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR);
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return;
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "IAP cmd: bind port " << port << " failed" << std::endl;
            ::close(sock);
            return;
        }
        listen(sock, 4);
        cmd_sock_.store(sock);
        std::cout << "IAP command listener on port " << port << std::endl;
        while (running_ && g_running) {
            struct pollfd pfd{sock, POLLIN, 0};
            if (poll(&pfd, 1, 200) <= 0) continue;
            int csock = accept(sock, nullptr, nullptr);
            if (csock < 0) continue;
            struct timeval tv{10, 0};
            setsockopt(csock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[4096];
            int n = (int)recv(csock, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                std::string cmd(buf);
                while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) cmd.pop_back();
                std::string response = handle_iap_command(cmd);
                send(csock, response.c_str(), response.size(), 0);
            }
            ::close(csock);
        }
    }

    std::string handle_iap_command(const std::string& cmd) {
        if (cmd == "PING") return "PONG\n";
        if (cmd == "STATUS") {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            return "ACTIVE_CALLS:" + std::to_string(calls_.size())
                + ":UPSTREAM:" + (interconnect_.upstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected")
                + ":DOWNSTREAM:" + (interconnect_.downstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected")
                + "\n";
        }
        return "ERROR:Unknown command\n";
    }

    void init_g711_tables() {
        for (int i = 0; i < 256; ++i) {
            int mu = ~i;
            int sign = mu & 0x80;
            int segment = (mu >> 4) & 0x07;
            int quantization = mu & 0x0F;
            int magnitude = ((quantization << 1) + 33) << (segment + 2);
            magnitude -= 132;
            ulaw_table[i] = (sign ? -magnitude : magnitude) / 32768.0f;
        }
    }

    void processing_loop() {
        static const float hb_filter[HB_LEN] = {
            -0.0076f, 0.0000f, 0.0527f, 0.0000f, -0.1681f, 0.0000f, 0.6230f,
             1.0000f,
             0.6230f, 0.0000f, -0.1681f, 0.0000f, 0.0527f, 0.0000f, -0.0076f
        };

        while (running_ && g_running) {
            whispertalk::Packet pkt;
            if (!interconnect_.recv_from_upstream(pkt, 100)) {
                continue;
            }

            if (!pkt.is_valid() || pkt.payload_size < RTP_HEADER_SIZE) {
                continue;
            }

            pkt.trace.record(whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR, 0);
            auto state = get_or_create_call(pkt.call_id);
            state->last_activity = std::chrono::steady_clock::now();

            size_t payload_len = pkt.payload_size - RTP_HEADER_SIZE;
            if (payload_len > ULAW_FRAME_SIZE) payload_len = ULAW_FRAME_SIZE;
            const uint8_t* rtp_payload = pkt.payload.data() + RTP_HEADER_SIZE;

            for (size_t i = 0; i < payload_len; ++i) {
                state->decoded[i] = ulaw_table[rtp_payload[i]];
            }

            for (int i = 0; i < HB_CENTER; ++i) state->ext[i] = state->fir_history[i];
            for (size_t i = 0; i < payload_len; ++i) state->ext[HB_CENTER + i] = state->decoded[i];
            int ext_len = HB_CENTER + (int)payload_len;

            size_t out_len = payload_len * 2;
            for (size_t n = 0; n < out_len; ++n) {
                float sum = 0.0f;
                for (int k = 0; k < HB_LEN; ++k) {
                    int src_idx_2x = (int)n - k + HB_CENTER;
                    if (src_idx_2x & 1) continue;
                    int ext_idx = src_idx_2x / 2 + HB_CENTER;
                    if (ext_idx < 0 || ext_idx >= ext_len) continue;
                    sum += state->ext[ext_idx] * hb_filter[k];
                }
                state->pcm[n] = sum * 2.0f;
            }

            if (payload_len >= (size_t)HB_CENTER) {
                for (int i = 0; i < HB_CENTER; ++i) {
                    state->fir_history[i] = state->decoded[payload_len - HB_CENTER + i];
                }
            } else {
                int shift = (int)payload_len;
                for (int i = 0; i < HB_CENTER - shift; ++i) state->fir_history[i] = state->fir_history[i + shift];
                for (int i = 0; i < shift; ++i) state->fir_history[HB_CENTER - shift + i] = state->decoded[i];
            }

            whispertalk::Packet out_pkt(pkt.call_id, state->pcm, out_len * sizeof(float));
            out_pkt.trace = pkt.trace;
            out_pkt.trace.record(whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR, 1);
            if (!interconnect_.send_to_downstream(out_pkt)) {
                if (interconnect_.downstream_state() != whispertalk::ConnectionState::CONNECTED) {
                    log_fwd_.forward("WARN", pkt.call_id, "Whisper disconnected, dumping stream");
                }
            }
        }
    }

    std::shared_ptr<CallState> get_or_create_call(uint32_t cid) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(cid)) return calls_[cid];
        auto state = std::make_shared<CallState>();
        state->id = cid;
        state->last_activity = std::chrono::steady_clock::now();
        calls_[cid] = state;
        std::cout << "📞 Created call state for call_id " << cid << std::endl;
        log_fwd_.forward("INFO", cid, "Created call state");
        return state;
    }

    void handle_call_end(uint32_t call_id) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(call_id)) {
            std::cout << "🛑 Call " << call_id << " ended, cleaning up" << std::endl;
            log_fwd_.forward("INFO", call_id, "Call ended, cleaning up");
            calls_.erase(call_id);
        }
    }

    void cleanup_inactive_calls() {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(calls_mutex_);
        for (auto it = calls_.begin(); it != calls_.end(); ) {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second->last_activity).count() > 60) {
                std::cout << "🧹 Cleaning up inactive call " << it->first << std::endl;
                it = calls_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::atomic<bool> running_;
    std::atomic<int> cmd_sock_{-1};
    float ulaw_table[256];
    std::mutex calls_mutex_;
    std::map<uint32_t, std::shared_ptr<CallState>> calls_;
    whispertalk::InterconnectNode interconnect_;
    whispertalk::LogForwarder log_fwd_;
};

int main() {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    InboundAudioProcessor proc;
    if (!proc.init()) {
        return 1;
    }
    proc.run();
    return 0;
}
