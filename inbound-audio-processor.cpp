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

struct CallState {
    int id;
    std::chrono::steady_clock::time_point last_activity;
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
        
        while (running_ && g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            cleanup_inactive_calls();
        }
        running_ = false;
        
        processor_thread.join();
        interconnect_.shutdown();
    }

private:
    // ITU-T G.711 μ-law decode table. For each companded byte value (0-255):
    //   1. Complement the byte (μ-law stores inverted)
    //   2. Extract sign (bit 7), segment/exponent (bits 6-4), quantization (bits 3-0)
    //   3. Reconstruct linear magnitude: ((quantization * 2 + 33) << segment) - 33
    //   4. Apply sign and normalize to [-1.0, 1.0]
    void init_g711_tables() {
        for (int i = 0; i < 256; ++i) {
            int mu = ~i;
            int sign = mu & 0x80;
            int segment = (mu >> 4) & 0x07;
            int quantization = mu & 0x0F;
            int magnitude = ((quantization << 1) + 33) << segment;
            magnitude -= 33;
            ulaw_table[i] = (sign ? -magnitude : magnitude) / 32768.0f;
        }
    }

    void processing_loop() {
        while (running_ && g_running) {
            whispertalk::Packet pkt;
            if (!interconnect_.recv_from_upstream(pkt, 100)) {
                continue;
            }

            if (!pkt.is_valid() || pkt.payload_size < 12) {
                continue;
            }

            pkt.trace.record(whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR, 0);
            auto state = get_or_create_call(pkt.call_id);
            state->last_activity = std::chrono::steady_clock::now();

            size_t rtp_header_size = 12;
            size_t payload_len = pkt.payload_size - rtp_header_size;
            const uint8_t* rtp_payload = pkt.payload.data() + rtp_header_size;

            // Decode G.711 μ-law → float32 and upsample 8kHz → 16kHz with anti-alias filter.
            // Each RTP packet is 160 μ-law bytes (20ms @ 8kHz) → 320 float32 samples (20ms @ 16kHz).
            //
            // Step 1: Decode μ-law to float32 at 8kHz.
            // Step 2: Zero-stuff (insert 0 between each sample) to get 16kHz.
            // Step 3: Apply half-band FIR low-pass filter (cutoff ~3.8kHz) to remove
            //         spectral copies above 4kHz that would otherwise alias and confuse
            //         Whisper's feature extraction. The 15-tap Hamming-windowed sinc filter
            //         provides ~40dB stopband attenuation with minimal group delay (~0.5ms).
            static const float hb_filter[] = {
                -0.0076f, 0.0000f, 0.0527f, 0.0000f, -0.1681f, 0.0000f, 0.6230f,
                 1.0000f,
                 0.6230f, 0.0000f, -0.1681f, 0.0000f, 0.0527f, 0.0000f, -0.0076f
            };
            static constexpr int HB_LEN = 15;
            static constexpr int HB_CENTER = 7;

            std::vector<float> decoded(payload_len);
            for (size_t i = 0; i < payload_len; ++i) {
                decoded[i] = ulaw_table[rtp_payload[i]];
            }

            std::vector<float> pcm(payload_len * 2);
            for (size_t n = 0; n < payload_len * 2; ++n) {
                float sum = 0.0f;
                for (int k = 0; k < HB_LEN; ++k) {
                    int src_idx_2x = (int)n - k + HB_CENTER;
                    if (src_idx_2x < 0 || src_idx_2x >= (int)(payload_len * 2)) continue;
                    if (src_idx_2x & 1) continue;
                    int orig = src_idx_2x / 2;
                    sum += decoded[orig] * hb_filter[k];
                }
                pcm[n] = sum * 2.0f;
            }

            whispertalk::Packet out_pkt(pkt.call_id, pcm.data(), pcm.size() * sizeof(float));
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
