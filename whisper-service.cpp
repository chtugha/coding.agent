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
#include <signal.h>
#include "interconnect.h"
#include "whisper-cpp/include/whisper.h"

static std::atomic<bool> g_running{true};
static void sig_handler(int) { g_running = false; }

struct WhisperCall {
    uint32_t id;
    std::vector<float> audio_buffer;
    std::mutex mutex;
    bool in_speech = false;
    bool speech_signaled = false;
    int silence_count = 0;
    float noise_floor = 0.0001f;
    int frame_count = 0;
    size_t vad_pos = 0;
    size_t speech_start = 0;
    std::chrono::steady_clock::time_point last_activity;
    std::chrono::steady_clock::time_point speech_signal_time;
    size_t last_buffer_size = 0;
    std::chrono::steady_clock::time_point last_buffer_growth;
};

class WhisperService {
    static constexpr size_t MAX_BUFFER_PACKETS = 64;
    // 100ms frames @ 16kHz = 1600 samples (standard VAD frame size)
    static constexpr size_t VAD_FRAME_SIZE = 1600;
    static constexpr float VAD_THRESHOLD_MULT = 2.0f;
    static constexpr float VAD_MIN_ENERGY = 0.000002f;
    static constexpr int VAD_SILENCE_FRAMES = 30;
    // Max 10s speech before forced flush to prevent unbounded buffering
    static constexpr size_t VAD_MAX_SPEECH_SAMPLES = 16000 * 10;
    static constexpr int VAD_CONTEXT_FRAMES = 10;
    // Safety timeout: force SPEECH_IDLE if no utterance completes within 10s
    static constexpr int SPEECH_SIGNAL_TIMEOUT_S = 10;
    // Inactivity flush: if in_speech and no new audio for this many ms, flush buffer
    static constexpr int VAD_INACTIVITY_FLUSH_MS = 2000;

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
        
        while (running_ && g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        running_ = false;
        
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
        while (running_ && g_running) {
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

                    size_t pos = call->vad_pos;
                    while (pos + VAD_FRAME_SIZE <= call->audio_buffer.size()) {
                        float energy = 0;
                        for (size_t i = 0; i < VAD_FRAME_SIZE; ++i) {
                            energy += call->audio_buffer[pos + i] * call->audio_buffer[pos + i];
                        }
                        energy /= static_cast<float>(VAD_FRAME_SIZE);

                        call->frame_count++;
                        if (!call->in_speech) {
                            call->noise_floor = call->noise_floor * 0.95f + energy * 0.05f;
                        }
                        float threshold = std::max(call->noise_floor * VAD_THRESHOLD_MULT, VAD_MIN_ENERGY);

                        if (energy > threshold) {
                            if (!call->in_speech) {
                                call->in_speech = true;
                                size_t context = VAD_FRAME_SIZE * VAD_CONTEXT_FRAMES;
                                call->speech_start = (pos > context) ? pos - context : 0;
                                if (!call->speech_signaled) {
                                    call->speech_signaled = true;
                                    call->speech_signal_time = std::chrono::steady_clock::now();
                                    interconnect_.broadcast_speech_signal(call->id, true);
                                    std::cout << "🗣️  [" << call->id << "] SPEECH_ACTIVE broadcast (VAD)" << std::endl;
                                }
                            }
                            call->silence_count = 0;
                        } else if (call->in_speech) {
                            call->silence_count++;
                        }

                        pos += VAD_FRAME_SIZE;

                        if (call->in_speech && call->silence_count > VAD_SILENCE_FRAMES) {
                            to_process.assign(
                                call->audio_buffer.begin() + call->speech_start,
                                call->audio_buffer.begin() + pos);
                            call->audio_buffer.erase(call->audio_buffer.begin(),
                                call->audio_buffer.begin() + pos);
                            pos = 0;
                            call->in_speech = false;
                            call->silence_count = 0;
                            call->speech_start = 0;
                            call->noise_floor = 0.0001f;
                            break;
                        }

                        size_t speech_len = pos - call->speech_start;
                        if (call->in_speech && speech_len > VAD_MAX_SPEECH_SAMPLES) {
                            to_process.assign(
                                call->audio_buffer.begin() + call->speech_start,
                                call->audio_buffer.begin() + pos);
                            call->audio_buffer.erase(call->audio_buffer.begin(),
                                call->audio_buffer.begin() + pos);
                            pos = 0;
                            call->in_speech = false;
                            call->silence_count = 0;
                            call->speech_start = 0;
                            call->noise_floor = 0.0001f;
                            break;
                        }
                    }
                    call->vad_pos = pos;

                    auto now = std::chrono::steady_clock::now();
                    if (call->audio_buffer.size() != call->last_buffer_size) {
                        call->last_buffer_size = call->audio_buffer.size();
                        call->last_buffer_growth = now;
                    }

                    if (call->in_speech && to_process.empty() && call->last_buffer_size > 0) {
                        auto inactivity = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - call->last_buffer_growth).count();
                        if (inactivity > VAD_INACTIVITY_FLUSH_MS) {
                            size_t end = call->audio_buffer.size();
                            if (end > call->speech_start) {
                                to_process.assign(
                                    call->audio_buffer.begin() + call->speech_start,
                                    call->audio_buffer.end());
                                call->audio_buffer.clear();
                                call->vad_pos = 0;
                                call->in_speech = false;
                                call->silence_count = 0;
                                call->speech_start = 0;
                                call->last_buffer_size = 0;
                                call->noise_floor = 0.0001f;
                                if (call->speech_signaled) {
                                    call->speech_signaled = false;
                                    interconnect_.broadcast_speech_signal(call->id, false);
                                }
                                std::cout << "🔄 [" << call->id << "] Inactivity flush: " << to_process.size() << " samples → SPEECH_IDLE" << std::endl;
                            }
                        }
                    }

                    if (!call->in_speech) {
                        size_t keep_frames = VAD_CONTEXT_FRAMES + 2;
                        size_t keep = VAD_FRAME_SIZE * keep_frames;
                        if (call->vad_pos > keep) {
                            call->audio_buffer.erase(call->audio_buffer.begin(),
                                call->audio_buffer.begin() + (call->vad_pos - keep));
                            call->vad_pos = keep;
                        }
                    }

                    if (call->speech_signaled) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                            now - call->speech_signal_time).count();
                        if (elapsed > SPEECH_SIGNAL_TIMEOUT_S) {
                            if (call->in_speech && call->audio_buffer.size() > call->speech_start) {
                                to_process.assign(
                                    call->audio_buffer.begin() + call->speech_start,
                                    call->audio_buffer.end());
                                call->audio_buffer.clear();
                                call->vad_pos = 0;
                                call->in_speech = false;
                                call->silence_count = 0;
                                call->speech_start = 0;
                                call->last_buffer_size = 0;
                                std::cout << "🔄 [" << call->id << "] Timeout flush: " << to_process.size() << " samples" << std::endl;
                            }
                            call->speech_signaled = false;
                            interconnect_.broadcast_speech_signal(call->id, false);
                            std::cerr << "⚠️  [" << call->id << "] SPEECH_ACTIVE timeout (" << SPEECH_SIGNAL_TIMEOUT_S << "s) — forcing SPEECH_IDLE" << std::endl;
                        }
                    }
                }

                if (!to_process.empty()) {
                    transcribe_and_send(call->id, to_process);
                }
            }
        }
    }

    void transcribe_and_send(uint32_t call_id, const std::vector<float>& audio) {
        std::vector<float> normalized(audio.size());
        float peak = 0.0f;
        for (auto s : audio) peak = std::max(peak, std::abs(s));
        if (peak > 0.0001f) {
            float gain = 0.9f / peak;
            for (size_t i = 0; i < audio.size(); ++i)
                normalized[i] = audio[i] * gain;
        } else {
            normalized = audio;
        }

        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
        wparams.language = "de";
        wparams.n_threads = 4;
        wparams.no_timestamps = true;
        wparams.single_segment = true;
        wparams.no_context = true;
        wparams.beam_search.beam_size = 5;
        wparams.temperature = 0.0f;
        wparams.temperature_inc = 0.0f;
        wparams.initial_prompt = "Erzieher, Lehrer, benennt, verordnet, schäbigen, Schwächephasen, Fußball-Bundesliga, Zeichen";

        auto t0 = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(whisper_mutex_);
        int result = whisper_full(ctx_, wparams, normalized.data(), normalized.size());

        {
            std::lock_guard<std::mutex> clk(calls_mutex_);
            auto it = calls_.find(call_id);
            if (it != calls_.end()) {
                it->second->speech_signaled = false;
            }
        }
        interconnect_.broadcast_speech_signal(call_id, false);
        std::cout << "🤐 [" << call_id << "] SPEECH_IDLE broadcast" << std::endl;

        if (result == 0) {
            auto t1 = std::chrono::steady_clock::now();
            auto whisper_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            int n_segments = whisper_full_n_segments(ctx_);
            std::string text;
            for (int i = 0; i < n_segments; ++i) {
                text += whisper_full_get_segment_text(ctx_, i);
            }
            if (!text.empty()) {
                std::cout << "📝 [" << call_id << "] Transcription (" << whisper_ms << "ms): " << text << std::endl;

                whispertalk::Packet pkt(call_id, text.c_str(), text.length());
                pkt.trace.record(whispertalk::ServiceType::WHISPER_SERVICE, 0);
                pkt.trace.record(whispertalk::ServiceType::WHISPER_SERVICE, 1);
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
        } else {
            std::cerr << "⚠️  [" << call_id << "] Whisper transcription failed" << std::endl;
        }
    }

    std::shared_ptr<WhisperCall> get_or_create_call(uint32_t cid) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(cid)) return calls_[cid];
        auto call = std::make_shared<WhisperCall>();
        call->id = cid;
        call->last_activity = std::chrono::steady_clock::now();
        call->last_buffer_growth = call->last_activity;
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
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

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
