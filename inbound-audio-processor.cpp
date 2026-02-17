// Inbound Audio Processor (Interconnect-based)
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <chrono>
#include <cstring>
#include "interconnect.h"

struct CallState {
    int id;
    std::chrono::steady_clock::time_point last_activity;
    std::vector<float> buffer;
    std::mutex mutex;
    bool speech_signaled = false;
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

        std::cout << "🔗 Interconnect initialized (master=" << interconnect_.is_master() << ")" << std::endl;

        if (!interconnect_.connect_to_downstream()) {
            std::cout << "⚠️  Downstream (Whisper) not available yet - will auto-reconnect" << std::endl;
        }

        interconnect_.register_call_end_handler([this](uint32_t call_id) {
            this->handle_call_end(call_id);
        });

        interconnect_.register_speech_signal_handler([this](uint32_t call_id, bool active) {
            if (!active) {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                auto it = calls_.find(call_id);
                if (it != calls_.end()) {
                    it->second->speech_signaled = false;
                }
            }
        });

        return true;
    }

    void run() {
        std::thread processor_thread(&InboundAudioProcessor::processing_loop, this);
        
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            cleanup_inactive_calls();
        }
        
        processor_thread.join();
        interconnect_.shutdown();
    }

private:
    void init_g711_tables() {
        for (int i = 0; i < 256; ++i) {
            int mu = ~i;
            int sign = (mu & 0x80);
            int exponent = (mu & 0x70) >> 4;
            int mantissa = mu & 0x0F;
            int sample = (mantissa << (exponent + 3)) + (1 << (exponent + 2)) - 33;
            if (exponent > 0) sample += (0x21 << exponent);
            ulaw_table[i] = (sign ? -sample : sample) / 32768.0f;
        }
    }

    void processing_loop() {
        while (running_) {
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

            std::vector<float> pcm(payload_len * 2);
            for (size_t i = 0; i < payload_len; ++i) {
                float s = ulaw_table[rtp_payload[i]];
                pcm[i*2] = s;
                float next = (i+1 < payload_len) ? ulaw_table[rtp_payload[i+1]] : s;
                pcm[i*2 + 1] = 0.5f * (s + next);
            }

            float energy = 0;
            for (size_t i = 0; i < pcm.size(); ++i) {
                energy += pcm[i] * pcm[i];
            }
            energy /= static_cast<float>(pcm.size());

            if (energy > 0.00003f && !state->speech_signaled) {
                state->speech_signaled = true;
                interconnect_.broadcast_speech_signal(pkt.call_id, true);
            }

            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->buffer.insert(state->buffer.end(), pcm.begin(), pcm.end());
            }

            if (state->buffer.size() >= 1600) {
                std::vector<float> chunk;
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    size_t chunk_size = std::min(state->buffer.size(), size_t(16000));
                    chunk.assign(state->buffer.begin(), state->buffer.begin() + chunk_size);
                    state->buffer.erase(state->buffer.begin(), state->buffer.begin() + chunk_size);
                }

                whispertalk::Packet out_pkt(pkt.call_id, chunk.data(), chunk.size() * sizeof(float));
                out_pkt.trace = pkt.trace;
                out_pkt.trace.record(whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR, 1);
                if (!interconnect_.send_to_downstream(out_pkt)) {
                    if (interconnect_.downstream_state() != whispertalk::ConnectionState::CONNECTED) {
                        std::cout << "⚠️  [" << pkt.call_id << "] Whisper disconnected, dumping stream to /dev/null" << std::endl;
                    }
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
        return state;
    }

    void handle_call_end(uint32_t call_id) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(call_id)) {
            std::cout << "🛑 Call " << call_id << " ended, cleaning up" << std::endl;
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
};

int main() {
    InboundAudioProcessor proc;
    if (!proc.init()) {
        return 1;
    }
    proc.run();
    return 0;
}
