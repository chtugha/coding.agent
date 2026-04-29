// inbound-audio-processor.cpp — G.711 μ-law decoder + 8kHz→16kHz (classic) / 8kHz→24kHz (Moshi) upsampler.
//
// Pipeline position: SIP_CLIENT → [IAP] → VAD (classic) or MOSHI_SERVICE (moshi)
//
// Receives raw RTP packets (G.711 μ-law, 8kHz, 20ms frames = 160 bytes payload)
// from the SIP_CLIENT via the interconnect data channel. For each packet:
//   1. Strip RTP header (12 bytes).
//   2. Decode μ-law bytes → float32 using a precomputed 256-entry LUT (ITU-T G.711).
//      Each byte maps to a float in [-1.0, 1.0]. The LUT is built in init_g711_tables()
//      using the standard segment/quantization decode formula.
//   3. Upsample 8kHz→16kHz via polyphase FIR half-band filter (iap_fir_upsample_frame()
//      from interconnect.h). Each 160-sample input frame produces 320 float32 samples.
//      Filter: 15-tap Hamming-windowed sinc, cutoff ~3.8kHz, ~40dB stopband attenuation.
//      State (fir_history) is per-call to avoid contamination between concurrent calls.
//   4. Forward the 320-sample (20ms @ 16kHz) float32 buffer downstream to VAD.
//
// Per-call state (CallState): maintains the FIR history buffer across RTP packets so
// the filter is continuous across frame boundaries. Inactive calls are cleaned up after
// 60 seconds with no packets. CALL_END from upstream triggers immediate cleanup.
//
// Resilience: If VAD is disconnected, IAP discards audio but keeps processing.
//   A throttled warning is logged once every DISC_WARN_INTERVAL_S (5s) to avoid log spam.
//
// Performance logging: Every 500 packets, logs avg/max per-packet processing latency
// (μs) at DEBUG level so bottlenecks can be identified without constant log spam.
//
// CMD port (IAP base+2 = 13112): accepts PING, STATUS, SET_LOG_LEVEL commands.
//   STATUS returns active call count, upstream/downstream state, avg/max latency.
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
#include <getopt.h>
#include "interconnect.h"

static std::atomic<bool> g_running{true};
static void sig_handler(int) { g_running = false; }

static constexpr size_t RTP_HEADER_SIZE = 12;

struct CallState {
    int id;
    std::chrono::steady_clock::time_point last_activity;
    float fir_history[whispertalk::IAP_FIR_24K_CENTER] = {};
    float decoded[whispertalk::IAP_ULAW_FRAME];
    float pcm[whispertalk::IAP_ULAW_OUT_24K];
    uint64_t clip_count = 0;
};

static constexpr int DISC_WARN_INTERVAL_S = 5;

class InboundAudioProcessor {
public:
    InboundAudioProcessor() : running_(true), interconnect_(whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR) {
        init_g711_tables();
    }

    void set_moshi_mode(bool m) {
        moshi_mode_ = m;
        if (m) {
            interconnect_.set_downstream_override(whispertalk::ServiceType::MOSHI_SERVICE);
        }
    }

    bool init() {
        if (!interconnect_.initialize()) {
            std::cerr << "Failed to initialize interconnect" << std::endl;
            return false;
        }

        std::cout << "Interconnect initialized (peer-to-peer)" << std::endl;

        log_fwd_.init(whispertalk::FRONTEND_LOG_PORT, whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR);

        if (!interconnect_.connect_to_downstream()) {
            std::cout << "⚠️  Downstream (" << (moshi_mode_ ? "Moshi" : "Whisper") << ") not available yet - will auto-reconnect" << std::endl;
        }

        interconnect_.register_call_end_handler([this](uint32_t call_id) {
            this->handle_call_end(call_id);
        });

        return true;
    }

    void set_log_level(const char* level) {
        log_fwd_.set_level(level);
    }

    void run() {
        std::thread processor_thread(&InboundAudioProcessor::processing_loop, this);
        std::thread cmd_thread(&InboundAudioProcessor::command_listener_loop, this);

        std::printf("IAP service fully loaded and ready\n");
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
        if (cmd.rfind("SET_LOG_LEVEL:", 0) == 0) {
            std::string level = cmd.substr(14);
            log_fwd_.set_level(level.c_str());
            return "OK\n";
        }
        if (cmd == "STATUS") {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            char buf[320];
            snprintf(buf, sizeof(buf),
                "ACTIVE_CALLS:%zu:MODE:%s:UPSTREAM:%s:DOWNSTREAM:%s:PKT_LATENCY_AVG_US:%.1f:PKT_LATENCY_MAX_US:%.1f:PKT_COUNT:%llu\n",
                calls_.size(),
                moshi_mode_ ? "moshi" : "classic",
                interconnect_.upstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected",
                interconnect_.downstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected",
                pkt_count_.load() > 0 ? latency_sum_.load() / pkt_count_.load() : 0.0,
                latency_max_.load(),
                (unsigned long long)pkt_count_.load());
            return std::string(buf);
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
        while (running_ && g_running) {
            whispertalk::Packet pkt;
            if (!interconnect_.recv_from_upstream(pkt, 100)) {
                continue;
            }

            if (!pkt.is_valid() || pkt.payload_size < RTP_HEADER_SIZE) {
                continue;
            }

            auto t0 = std::chrono::steady_clock::now();
            pkt.trace.record(whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR, 0);
            auto state = get_or_create_call(pkt.call_id);
            state->last_activity = t0;

            size_t payload_len = pkt.payload_size - RTP_HEADER_SIZE;
            if (payload_len > whispertalk::IAP_ULAW_FRAME) payload_len = whispertalk::IAP_ULAW_FRAME;
            const uint8_t* rtp_payload = pkt.payload.data() + RTP_HEADER_SIZE;

            for (size_t i = 0; i < payload_len; ++i) {
                state->decoded[i] = ulaw_table[rtp_payload[i]];
            }

            float dec_peak = 0;
            for (size_t i = 0; i < payload_len; ++i) {
                float a = std::abs(state->decoded[i]);
                if (a > dec_peak) dec_peak = a;
            }

            size_t out_len = moshi_mode_
                ? whispertalk::iap_fir_upsample_frame_24k(state->decoded, payload_len, state->pcm, state->fir_history)
                : whispertalk::iap_fir_upsample_frame(state->decoded, payload_len, state->pcm, state->fir_history);

            float up_peak = 0;
            for (size_t i = 0; i < out_len; ++i) {
                float a = std::abs(state->pcm[i]);
                if (a > up_peak) up_peak = a;
            }

            if (up_peak > 1.0f) {
                state->clip_count++;
                if ((state->clip_count % 500) == 1) {
                    log_fwd_.forward(whispertalk::LogLevel::WARN, pkt.call_id,
                        "Upsampler clipping (#%llu): decoded_peak=%.4f upsample_peak=%.4f gain=%.2fx",
                        (unsigned long long)state->clip_count,
                        dec_peak, up_peak, dec_peak > 0 ? up_peak / dec_peak : 0.0f);
                }
            }

            auto t1 = std::chrono::steady_clock::now();
            double pkt_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            double old_sum = latency_sum_.load(std::memory_order_relaxed);
            while (!latency_sum_.compare_exchange_weak(old_sum, old_sum + pkt_us, std::memory_order_relaxed)) {}
            double cur_max = latency_max_.load(std::memory_order_relaxed);
            while (pkt_us > cur_max && !latency_max_.compare_exchange_weak(cur_max, pkt_us, std::memory_order_relaxed)) {}
            uint64_t count = pkt_count_.fetch_add(1, std::memory_order_relaxed) + 1;

            if ((count % 500) == 0) {
                double avg_us = latency_sum_.load(std::memory_order_relaxed) / count;
                char msg[128];
                snprintf(msg, sizeof(msg), "Per-packet latency: avg=%.1fus max=%.1fus (%llu pkts)",
                         avg_us, latency_max_.load(std::memory_order_relaxed), (unsigned long long)count);
                log_fwd_.forward(whispertalk::LogLevel::DEBUG, pkt.call_id, msg);
            }

            whispertalk::Packet out_pkt(pkt.call_id, state->pcm, out_len * sizeof(float));
            out_pkt.trace = pkt.trace;
            out_pkt.trace.record(whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR, 1);
            if (!interconnect_.send_to_downstream(out_pkt)) {
                if (interconnect_.downstream_state() != whispertalk::ConnectionState::CONNECTED) {
                    auto now = std::chrono::steady_clock::now();
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_disc_warn_).count() >= DISC_WARN_INTERVAL_S) {
                        log_fwd_.forward(whispertalk::LogLevel::WARN, pkt.call_id, "Downstream disconnected, discarding audio");
                        last_disc_warn_ = now;
                    }
                }
            }
        }
    }

    std::shared_ptr<CallState> get_or_create_call(uint32_t cid) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto it = calls_.find(cid);
        if (it != calls_.end()) return it->second;
        auto state = std::make_shared<CallState>();
        state->id = cid;
        state->last_activity = std::chrono::steady_clock::now();
        calls_[cid] = state;
        std::cout << "Created call state for call_id " << cid << std::endl;
        log_fwd_.forward(whispertalk::LogLevel::INFO, cid, "Created call state");
        return state;
    }

    void handle_call_end(uint32_t call_id) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto it = calls_.find(call_id);
        if (it != calls_.end()) {
            std::cout << "Call " << call_id << " ended, cleaning up" << std::endl;
            log_fwd_.forward(whispertalk::LogLevel::INFO, call_id, "Call ended, cleaning up");
            calls_.erase(it);
        }
    }

    void cleanup_inactive_calls() {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(calls_mutex_);
        for (auto it = calls_.begin(); it != calls_.end(); ) {
            if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second->last_activity).count() > 60) {
                std::cout << "Cleaning up inactive call " << it->first << std::endl;
                it = calls_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::atomic<bool> running_;
    std::atomic<int> cmd_sock_{-1};
    std::atomic<bool> moshi_mode_{false};
    float ulaw_table[256];
    std::mutex calls_mutex_;
    std::map<uint32_t, std::shared_ptr<CallState>> calls_;
    std::atomic<uint64_t> pkt_count_{0};
    std::atomic<double>   latency_sum_{0.0};
    std::atomic<double>   latency_max_{0.0};
    std::chrono::steady_clock::time_point last_disc_warn_{};
    whispertalk::InterconnectNode interconnect_;
    whispertalk::LogForwarder log_fwd_;
};

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    std::string log_level = "INFO";
    bool moshi_mode = false;

    static struct option long_opts[] = {
        {"log-level",  required_argument, 0, 'L'},
        {"moshi-mode", no_argument,       0, 'M'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "L:M", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'L': log_level = optarg; break;
            case 'M': moshi_mode = true; break;
            default: break;
        }
    }

    InboundAudioProcessor proc;
    if (moshi_mode) {
        proc.set_moshi_mode(true);
    }
    if (!proc.init()) {
        return 1;
    }
    proc.set_log_level(log_level.c_str());
    proc.run();
    return 0;
}
