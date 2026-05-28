// inbound-audio-processor.cpp — G.711 μ-law decoder + multi-downstream upsampler.
//
// Pipeline position: SIP_CLIENT → [IAP] → VAD (classic) or MOSHI_SERVICE (moshi-rag)
//
// Receives raw RTP packets (G.711 μ-law, 8kHz, 20ms frames = 160 bytes payload)
// from the SIP_CLIENT via the interconnect data channel. For each packet:
//   1. Strip RTP header (12 bytes).
//   2. Decode μ-law bytes → float32 using a precomputed 256-entry LUT (ITU-T G.711).
//      Each byte maps to a float in [-1.0, 1.0]. The LUT is built in init_g711_tables()
//      using the standard segment/quantization decode formula.
//   3. For each registered downstream: upsample to the negotiated sample rate.
//      Classic mode: 8kHz→16kHz (320 samples, 20ms) via 15-tap half-band FIR → VAD.
//      Moshi-rag mode: 8kHz→24kHz (480 samples, 20ms) via 3-phase polyphase FIR
//      → MOSHI_SERVICE only (no VAD/Whisper path). FIR state (fir_history) is per-call
//      and per-downstream to avoid
//      contamination between concurrent calls or between different upsamplers.
//   4. Forward PCM to each downstream via send_to_downstream(pkt, target).
//
// Per-call state (CallState): maintains per-downstream FIR history buffers and output
// PCM arrays across RTP packets so filters are continuous across frame boundaries.
// Inactive calls are cleaned up after 60 seconds with no packets.
// CALL_END from upstream triggers immediate cleanup.
//
// Resilience: If a downstream is disconnected, IAP discards audio for that target only.
//   A per-target throttled warning is logged once every DISC_WARN_INTERVAL_S (5s).
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
static constexpr int CALL_INACTIVITY_TIMEOUT_S = 60;
static constexpr int LOG_INTERVAL_PKTS = 500;
static constexpr int UPSTREAM_RECV_TIMEOUT_MS = 100;
static constexpr int CMD_POLL_TIMEOUT_MS = 200;
static constexpr int CMD_LISTEN_BACKLOG = 4;
static constexpr int CMD_RECV_TIMEOUT_S = 10;
static constexpr size_t CMD_BUF_SIZE = 4096;

struct PerDownstreamCallState {
    float fir_history_16k[whispertalk::IAP_FIR_CENTER] = {};
    float fir_history_24k[whispertalk::IAP_FIR_24K_CENTER] = {};
    float pcm_16k[whispertalk::IAP_ULAW_FRAME * 2];
    float pcm_24k[whispertalk::IAP_ULAW_OUT_24K];
    uint64_t clip_count = 0;
};

struct CallState {
    std::chrono::steady_clock::time_point last_activity;
    float decoded[whispertalk::IAP_ULAW_FRAME];
    std::map<whispertalk::ServiceType, PerDownstreamCallState> downstream_state;
};

static constexpr int DISC_WARN_INTERVAL_S = 5;

class InboundAudioProcessor {
public:
    InboundAudioProcessor() : running_(true), interconnect_(whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR) {
        init_g711_tables();
    }

    void set_moshi_rag_mode(bool m) {
        moshi_rag_mode_ = m;
    }

    bool init() {
        if (moshi_rag_mode_) {
            interconnect_.add_downstream_target(whispertalk::ServiceType::MOSHI_SERVICE);
        } else {
            interconnect_.add_downstream_target(whispertalk::ServiceType::VAD_SERVICE);
        }

        if (!interconnect_.initialize()) {
            std::cerr << "Failed to initialize interconnect" << std::endl;
            return false;
        }

        std::cout << "Interconnect initialized (peer-to-peer)" << std::endl;

        log_fwd_.init(whispertalk::FRONTEND_LOG_PORT, whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR);

        interconnect_.connect_all_downstreams();

        interconnect_.register_call_end_handler([this](uint32_t call_id) {
            this->handle_call_end(call_id);
        });

        log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "Inbound Audio Processor initialized and running");

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
        listen(sock, CMD_LISTEN_BACKLOG);
        cmd_sock_.store(sock);
        std::cout << "IAP command listener on port " << port << std::endl;
        while (running_ && g_running) {
            struct pollfd pfd{sock, POLLIN, 0};
            if (poll(&pfd, 1, CMD_POLL_TIMEOUT_MS) <= 0) continue;
            int csock = accept(sock, nullptr, nullptr);
            if (csock < 0) continue;
            struct timeval tv{CMD_RECV_TIMEOUT_S, 0};
            setsockopt(csock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[CMD_BUF_SIZE];
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
            std::string result;
            char buf[512];
            snprintf(buf, sizeof(buf),
                "ACTIVE_CALLS:%zu:MODE:%s:UPSTREAM:%s:PKT_LATENCY_AVG_US:%.1f:PKT_LATENCY_MAX_US:%.1f:PKT_COUNT:%llu",
                calls_.size(),
                moshi_rag_mode_ ? "moshi-rag" : "classic",
                interconnect_.upstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected",
                pkt_count_.load() > 0 ? latency_sum_.load() / pkt_count_.load() : 0.0,
                latency_max_.load(),
                (unsigned long long)pkt_count_.load());
            result = buf;
            auto ds_states = interconnect_.downstream_connection_states();
            for (const auto& [target, state, rate] : ds_states) {
                const char* state_str = (state == whispertalk::ConnectionState::CONNECTED) ? "connected" : "disconnected";
                char ds_buf[128];
                snprintf(ds_buf, sizeof(ds_buf), ":DS_%s:%s:%u",
                    whispertalk::service_type_to_string(target), state_str, rate);
                result += ds_buf;
            }
            result += "\n";
            return result;
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
        auto ds_targets = interconnect_.downstream_connection_states();

        while (running_ && g_running) {
            whispertalk::Packet pkt;
            if (!interconnect_.recv_from_upstream(pkt, UPSTREAM_RECV_TIMEOUT_MS)) {
                continue;
            }

            if (!pkt.is_valid() || pkt.payload_size < RTP_HEADER_SIZE) {
                continue;
            }

            auto t0 = std::chrono::steady_clock::now();
            pkt.trace.record(whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR, 0);
            auto state = get_or_create_call(pkt.call_id, t0);

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

            for (const auto& ds_entry : ds_targets) {
                auto target = std::get<0>(ds_entry);
                uint32_t rate = interconnect_.negotiated_sample_rate_for(target);
                auto& ds = state->downstream_state[target];

                size_t out_len;
                float* out_buf;
                if (rate == 24000) {
                    out_len = whispertalk::iap_fir_upsample_frame_24k(state->decoded, payload_len, ds.pcm_24k, ds.fir_history_24k);
                    out_buf = ds.pcm_24k;
                } else {
                    out_len = whispertalk::iap_fir_upsample_frame(state->decoded, payload_len, ds.pcm_16k, ds.fir_history_16k);
                    out_buf = ds.pcm_16k;
                }

                float up_peak = 0;
                for (size_t i = 0; i < out_len; ++i) {
                    float a = std::abs(out_buf[i]);
                    if (a > up_peak) up_peak = a;
                }

                if (up_peak > 1.0f) {
                    ds.clip_count++;
                    if ((ds.clip_count % LOG_INTERVAL_PKTS) == 1) {
                        log_fwd_.forward(whispertalk::LogLevel::WARN, pkt.call_id,
                            "Upsampler clipping (#%llu, %s): decoded_peak=%.4f upsample_peak=%.4f gain=%.2fx",
                            (unsigned long long)ds.clip_count,
                            whispertalk::service_type_to_string(target),
                            dec_peak, up_peak, dec_peak > 0 ? up_peak / dec_peak : 0.0f);
                    }
                }

                whispertalk::Packet out_pkt(pkt.call_id, out_buf, out_len * sizeof(float));
                out_pkt.trace = pkt.trace;
                out_pkt.trace.record(whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR, 1);
                if (!interconnect_.send_to_downstream(out_pkt, target)) {
                    auto now = std::chrono::steady_clock::now();
                    auto& last_warn = last_disc_warn_per_target_[target];
                    if (std::chrono::duration_cast<std::chrono::seconds>(now - last_warn).count() >= DISC_WARN_INTERVAL_S) {
                        log_fwd_.forward(whispertalk::LogLevel::WARN, pkt.call_id,
                            "Downstream %s disconnected, discarding audio",
                            whispertalk::service_type_to_string(target));
                        last_warn = now;
                    }
                }
            }

            auto t1 = std::chrono::steady_clock::now();
            double pkt_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            double old_sum = latency_sum_.load(std::memory_order_relaxed);
            while (!latency_sum_.compare_exchange_weak(old_sum, old_sum + pkt_us, std::memory_order_relaxed)) {}
            double cur_max = latency_max_.load(std::memory_order_relaxed);
            while (pkt_us > cur_max && !latency_max_.compare_exchange_weak(cur_max, pkt_us, std::memory_order_relaxed)) {}
            uint64_t count = pkt_count_.fetch_add(1, std::memory_order_relaxed) + 1;

            if ((count % LOG_INTERVAL_PKTS) == 0) {
                double avg_us = latency_sum_.load(std::memory_order_relaxed) / count;
                char msg[128];
                snprintf(msg, sizeof(msg), "Per-packet latency: avg=%.1fus max=%.1fus (%llu pkts)",
                         avg_us, latency_max_.load(std::memory_order_relaxed), (unsigned long long)count);
                log_fwd_.forward(whispertalk::LogLevel::DEBUG, pkt.call_id, msg);
            }
        }
    }

    std::shared_ptr<CallState> get_or_create_call(uint32_t cid, std::chrono::steady_clock::time_point now) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto it = calls_.find(cid);
        if (it != calls_.end()) {
            it->second->last_activity = now;
            return it->second;
        }
        auto state = std::make_shared<CallState>();
        state->last_activity = now;
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
            if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second->last_activity).count() > CALL_INACTIVITY_TIMEOUT_S) {
                std::cout << "Cleaning up inactive call " << it->first << std::endl;
                it = calls_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::atomic<bool> running_;
    std::atomic<int> cmd_sock_{-1};
    bool moshi_rag_mode_{false};
    float ulaw_table[256];
    std::mutex calls_mutex_;
    std::map<uint32_t, std::shared_ptr<CallState>> calls_;
    std::atomic<uint64_t> pkt_count_{0};
    std::atomic<double>   latency_sum_{0.0};
    std::atomic<double>   latency_max_{0.0};
    std::map<whispertalk::ServiceType, std::chrono::steady_clock::time_point> last_disc_warn_per_target_;
    whispertalk::InterconnectNode interconnect_;
    whispertalk::LogForwarder log_fwd_;
};

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    std::string log_level = "INFO";
    bool moshi_rag_mode = false;

    static struct option long_opts[] = {
        {"log-level",      required_argument, 0, 'L'},
        {"moshi-rag-mode", no_argument,       0, 'M'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "L:M", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'L': log_level = optarg; break;
            case 'M': moshi_rag_mode = true; break;
            default: break;
        }
    }

    InboundAudioProcessor proc;
    if (moshi_rag_mode) {
        proc.set_moshi_rag_mode(true);
    }
    if (!proc.init()) {
        return 1;
    }
    proc.set_log_level(log_level.c_str());
    proc.run();
    return 0;
}
