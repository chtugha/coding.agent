// Outbound Audio Processor (Interconnect-based)
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include "interconnect.h"

struct CallState {
    uint32_t id;
    std::mutex mutex;
    std::vector<uint8_t> buffer;
    size_t read_pos = 0;
    std::chrono::steady_clock::time_point last_activity;

    void compact() {
        if (read_pos > 4096 && read_pos > buffer.size() / 2) {
            buffer.erase(buffer.begin(), buffer.begin() + read_pos);
            read_pos = 0;
        }
    }
};

class OutboundAudioProcessor {
public:
    OutboundAudioProcessor() : running_(true), interconnect_(whispertalk::ServiceType::OUTBOUND_AUDIO_PROCESSOR) {
    }

    bool init() {
        if (!interconnect_.initialize()) {
            std::cerr << "Failed to initialize interconnect" << std::endl;
            return false;
        }

        std::cout << "🔗 Interconnect initialized (master=" << interconnect_.is_master() << ")" << std::endl;

        uint16_t lp = interconnect_.frontend_log_port();
        if (lp) log_fwd_.init(lp, whispertalk::ServiceType::OUTBOUND_AUDIO_PROCESSOR);

        if (!interconnect_.connect_to_downstream()) {
            std::cout << "⚠️  Downstream (SIP) not available yet - will auto-reconnect" << std::endl;
        }

        interconnect_.register_call_end_handler([this](uint32_t call_id) {
            this->handle_call_end(call_id);
        });

        return true;
    }

    void run() {
        std::thread receiver_thread(&OutboundAudioProcessor::receiver_loop, this);
        std::thread scheduler_thread(&OutboundAudioProcessor::scheduler_loop, this);
        
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        receiver_thread.join();
        scheduler_thread.join();
        interconnect_.shutdown();
    }

private:
    uint8_t linear_to_ulaw(int16_t pcm) {
        int mask = 0x7FFF;
        int sign = 0;
        if (pcm < 0) {
            pcm = -pcm;
            sign = 0x80;
        }
        pcm += 128 + 4;
        if (pcm > mask) pcm = mask;
        int exponent = 7;
        for (int exp_mask = 0x4000; (pcm & exp_mask) == 0 && exponent > 0; exp_mask >>= 1) exponent--;
        int mantissa = (pcm >> (exponent + 3)) & 0x0F;
        return ~(sign | (exponent << 4) | mantissa);
    }

    void receiver_loop() {
        while (running_) {
            whispertalk::Packet pkt;
            if (!interconnect_.recv_from_upstream(pkt, 100)) {
                continue;
            }

            if (!pkt.is_valid() || pkt.payload_size == 0 || (pkt.payload_size % sizeof(float)) != 0) {
                continue;
            }

            pkt.trace.record(whispertalk::ServiceType::OUTBOUND_AUDIO_PROCESSOR, 0);
            auto state = get_or_create_call(pkt.call_id);
            state->last_activity = std::chrono::steady_clock::now();

            size_t sample_count = pkt.payload_size / sizeof(float);
            const float* pcm_buf = reinterpret_cast<const float*>(pkt.payload.data());

            std::vector<uint8_t> ulaw;
            for (size_t i = 0; i < sample_count; i += 3) {
                int16_t s16 = static_cast<int16_t>(pcm_buf[i] * 32767.0f);
                ulaw.push_back(linear_to_ulaw(s16));
            }

            std::lock_guard<std::mutex> lock(state->mutex);
            state->buffer.insert(state->buffer.end(), ulaw.begin(), ulaw.end());
        }
    }

    void scheduler_loop() {
        auto next = std::chrono::steady_clock::now();
        while (running_) {
            std::vector<std::shared_ptr<CallState>> active;
            {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                for (auto& p : calls_) active.push_back(p.second);
            }

            for (auto& state : active) {
                uint8_t frame[160];
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    size_t avail = state->buffer.size() - state->read_pos;
                    if (avail >= 160) {
                        memcpy(frame, state->buffer.data() + state->read_pos, 160);
                        state->read_pos += 160;
                        state->compact();
                    } else {
                        memset(frame, 0xFF, 160);
                    }
                }

                whispertalk::Packet pkt(state->id, frame, 160);
                pkt.trace.record(whispertalk::ServiceType::OUTBOUND_AUDIO_PROCESSOR, 1);
                if (!interconnect_.send_to_downstream(pkt)) {
                    if (interconnect_.downstream_state() != whispertalk::ConnectionState::CONNECTED) {
                        std::cout << "⚠️  [" << state->id << "] SIP disconnected, discarding audio" << std::endl;
                        log_fwd_.forward("WARN", state->id, "SIP disconnected, discarding audio");
                    }
                }
            }

            next += std::chrono::milliseconds(20);
            std::this_thread::sleep_until(next);
        }
    }

    std::shared_ptr<CallState> get_or_create_call(uint32_t cid) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(cid)) return calls_[cid];
        auto state = std::make_shared<CallState>();
        state->id = cid;
        state->last_activity = std::chrono::steady_clock::now();
        calls_[cid] = state;
        std::cout << "📞 Created outbound audio state for call_id " << cid << std::endl;
        log_fwd_.forward("INFO", cid, "Created outbound audio state");
        return state;
    }

    void handle_call_end(uint32_t call_id) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(call_id)) {
            std::cout << "🛑 Call " << call_id << " ended, cleaning up outbound audio" << std::endl;
            log_fwd_.forward("INFO", call_id, "Call ended, cleaning up outbound audio");
            calls_.erase(call_id);
        }
    }

    std::atomic<bool> running_;
    std::mutex calls_mutex_;
    std::map<uint32_t, std::shared_ptr<CallState>> calls_;
    whispertalk::InterconnectNode interconnect_;
    whispertalk::LogForwarder log_fwd_;
};

int main() {
    OutboundAudioProcessor proc;
    if (!proc.init()) {
        return 1;
    }
    proc.run();
    return 0;
}
