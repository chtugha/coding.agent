// Whisper Service (Interconnect-based)
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include "interconnect.h"
#include "whisper-cpp/include/whisper.h"

struct WhisperCall {
    uint32_t id;
    std::vector<float> audio_buffer;
    std::mutex mutex;
    bool in_speech = false;
    int silence_count = 0;
    std::chrono::steady_clock::time_point last_activity;
};

class WhisperService {
    static constexpr size_t MAX_BUFFER_PACKETS = 64;

public:
    WhisperService(const std::string& model_path) 
        : running_(true), 
          model_path_(model_path),
          interconnect_(whispertalk::ServiceType::WHISPER_SERVICE) {
        whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu = true;
        ctx_ = whisper_init_from_file_with_params(model_path.c_str(), cparams);
        if (!ctx_) {
            throw std::runtime_error("Failed to load Whisper model: " + model_path);
        }
    }

    ~WhisperService() {
        if (ctx_) whisper_free(ctx_);
    }

    bool init() {
        if (!interconnect_.initialize()) {
            std::cerr << "Failed to initialize interconnect" << std::endl;
            return false;
        }

        std::cout << "🔗 Interconnect initialized (master=" << interconnect_.is_master() << ")" << std::endl;

        if (!interconnect_.connect_to_downstream()) {
            std::cout << "⚠️  Downstream (LLaMA) not available yet - will auto-reconnect" << std::endl;
        }

        interconnect_.register_call_end_handler([this](uint32_t call_id) {
            this->handle_call_end(call_id);
        });

        return true;
    }

    void run() {
        std::thread receiver_thread(&WhisperService::receiver_loop, this);
        std::thread processor_thread(&WhisperService::processing_loop, this);
        
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        receiver_thread.join();
        processor_thread.join();
        interconnect_.shutdown();
    }

private:
    void receiver_loop() {
        while (running_) {
            whispertalk::Packet pkt;
            if (!interconnect_.recv_from_upstream(pkt, 100)) {
                continue;
            }

            if (!pkt.is_valid() || pkt.payload_size == 0 || (pkt.payload_size % sizeof(float)) != 0) {
                continue;
            }

            auto call = get_or_create_call(pkt.call_id);
            call->last_activity = std::chrono::steady_clock::now();

            size_t sample_count = pkt.payload_size / sizeof(float);
            const float* samples = reinterpret_cast<const float*>(pkt.payload.data());

            std::lock_guard<std::mutex> lock(call->mutex);
            call->audio_buffer.insert(call->audio_buffer.end(), samples, samples + sample_count);
        }
    }

    void processing_loop() {
        while (running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));

            std::vector<std::shared_ptr<WhisperCall>> active;
            {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                for (auto& p : calls_) active.push_back(p.second);
            }

            for (auto& call : active) {
                std::vector<float> to_process;
                {
                    std::lock_guard<std::mutex> lock(call->mutex);
                    
                    while (call->audio_buffer.size() >= 1600) {
                        float energy = 0;
                        for (int i = 0; i < 1600; ++i) {
                            energy += call->audio_buffer[i] * call->audio_buffer[i];
                        }
                        energy /= 1600.0f;

                        if (energy > 0.00005f) {
                            call->in_speech = true;
                            call->silence_count = 0;
                        } else {
                            if (call->in_speech) call->silence_count++;
                        }

                        if (call->in_speech && call->silence_count > 8) {
                            to_process = std::move(call->audio_buffer);
                            call->in_speech = false;
                            call->silence_count = 0;
                            break;
                        } else if (call->audio_buffer.size() > 16000 * 8) {
                            to_process = std::move(call->audio_buffer);
                            call->in_speech = false;
                            call->silence_count = 0;
                            break;
                        }
                        
                        break;
                    }
                }

                if (!to_process.empty()) {
                    transcribe_and_send(call->id, to_process);
                }
            }
        }
    }

    void transcribe_and_send(uint32_t call_id, const std::vector<float>& audio) {
        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.language = "de";
        wparams.n_threads = 4;
        wparams.no_timestamps = true;

        std::lock_guard<std::mutex> lock(whisper_mutex_);
        if (whisper_full(ctx_, wparams, audio.data(), audio.size()) == 0) {
            int n_segments = whisper_full_n_segments(ctx_);
            std::string text;
            for (int i = 0; i < n_segments; ++i) {
                text += whisper_full_get_segment_text(ctx_, i);
            }
            if (!text.empty()) {
                std::cout << "📝 [" << call_id << "] Transcription: " << text << std::endl;
                
                whispertalk::Packet pkt(call_id, text.c_str(), text.length());
                if (!interconnect_.send_to_downstream(pkt)) {
                    if (interconnect_.downstream_state() != whispertalk::ConnectionState::CONNECTED) {
                        std::cout << "⚠️  [" << call_id << "] LLaMA disconnected, discarding transcription to /dev/null" << std::endl;
                        
                        std::lock_guard<std::mutex> buf_lock(buffer_mutex_);
                        if (buffered_packets_.size() < MAX_BUFFER_PACKETS) {
                            buffered_packets_.push_back(pkt);
                        } else {
                            buffered_packets_.pop_front();
                            buffered_packets_.push_back(pkt);
                        }
                    }
                } else {
                    std::lock_guard<std::mutex> buf_lock(buffer_mutex_);
                    while (!buffered_packets_.empty()) {
                        auto& buffered = buffered_packets_.front();
                        if (interconnect_.send_to_downstream(buffered)) {
                            buffered_packets_.pop_front();
                        } else {
                            break;
                        }
                    }
                }
            }
        }
    }

    std::shared_ptr<WhisperCall> get_or_create_call(uint32_t cid) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(cid)) return calls_[cid];
        auto call = std::make_shared<WhisperCall>();
        call->id = cid;
        call->last_activity = std::chrono::steady_clock::now();
        calls_[cid] = call;
        std::cout << "📞 Created transcription session for call_id " << cid << std::endl;
        return call;
    }

    void handle_call_end(uint32_t call_id) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(call_id)) {
            std::cout << "🛑 Call " << call_id << " ended, closing transcription session" << std::endl;
            calls_.erase(call_id);
        }
    }

    std::atomic<bool> running_;
    std::string model_path_;
    struct whisper_context* ctx_ = nullptr;
    std::mutex whisper_mutex_;
    std::mutex calls_mutex_;
    std::map<uint32_t, std::shared_ptr<WhisperCall>> calls_;
    whispertalk::InterconnectNode interconnect_;
    
    std::mutex buffer_mutex_;
    std::deque<whispertalk::Packet> buffered_packets_;
};

int main(int argc, char** argv) {
    const char* env_models = std::getenv("WHISPERTALK_MODELS_DIR");
    std::string models_dir = env_models ? env_models :
#ifdef WHISPERTALK_MODELS_DIR
        WHISPERTALK_MODELS_DIR;
#else
        "models";
#endif
    std::string model_path = models_dir + "/ggml-large-v3-q5_0.bin";
    if (argc >= 2) model_path = argv[1];

    try {
        WhisperService service(model_path);
        if (!service.init()) {
            return 1;
        }
        service.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
