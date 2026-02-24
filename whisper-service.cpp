// Whisper Service — Adaptive chunked VAD with greedy streaming.
//
// Pipeline: IAP → [receiver_loop] → audio_buffer → [processing_loop/VAD] → chunks → [transcribe] → downstream
//
// VAD strategy: Energy-based with adaptive micro-pause detection.
// Instead of waiting for full sentence silence (1500ms), we detect short pauses (~400ms)
// between words/phrases and submit smaller chunks (1.5-4s) to Whisper. This cuts latency
// dramatically because Whisper inference time scales ~quadratically with input length.
// Context coherence is maintained by passing the previous transcription as initial_prompt.
//
// Decoding: GREEDY (not beam search). On short 2-4s segments, greedy is ~3-5x faster
// than beam_size=5 with negligible accuracy difference. Temperature fallback handles errors.
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <chrono>
#include <cstring>
#include <deque>
#include <cmath>
#include <signal.h>
#include <getopt.h>
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
    float noise_floor = 0.00005f;
    int frame_count = 0;
    size_t vad_pos = 0;
    size_t speech_start = 0;
    std::chrono::steady_clock::time_point last_activity;
    std::chrono::steady_clock::time_point speech_signal_time;
    size_t last_buffer_size = 0;
    std::chrono::steady_clock::time_point last_buffer_growth;
    // Context carry-over: previous chunk's transcription used as initial_prompt
    // for the next chunk to maintain coherence across chunk boundaries.
    std::string last_transcription;
};

class WhisperService {
    static constexpr size_t MAX_BUFFER_PACKETS = 64;

    // VAD parameters — tuned for low-latency chunked streaming.
    // vad_frame_size_: 50ms frames (800 samples @ 16kHz) — finer granularity than 100ms
    //   for detecting short pauses between words without cutting mid-phoneme.
    size_t vad_frame_size_ = 800;
    float vad_threshold_mult_ = 2.0f;
    // Minimum energy threshold to distinguish speech from G.711 codec noise floor.
    // G.711 μ-law silence (0xFF/0x7F) decodes to ±0.000885 → energy ~0.00000078.
    // Set min_energy well above this to prevent false VAD triggers on silence.
    float vad_min_energy_ = 0.00005f;
    // vad_silence_frames_: 8 frames × 50ms = 400ms — triggers on word-boundary pauses
    //   instead of full sentence gaps. Short enough for fast turnaround, long enough
    //   to not split mid-word (German phonemes are typically <200ms).
    int vad_silence_frames_ = 8;
    // vad_max_speech_samples_: 4s max chunk — keeps Whisper inference under ~500ms.
    //   Whisper's attention is O(n²) on audio length; 4s vs 10s is ~6x faster.
    size_t vad_max_speech_samples_ = 16000 * 4;
    // vad_min_speech_samples_: 0.5s minimum — don't transcribe noise bursts or clicks.
    size_t vad_min_speech_samples_ = 16000 / 2;
    int vad_context_frames_ = 4;
    int speech_signal_timeout_s_ = 10;
    int vad_inactivity_flush_ms_ = 1000;
    bool vad_logging_enabled_ = true;

public:
    WhisperService(const std::string& model_path, const std::string& language = "de") 
        : running_(true), 
          model_path_(model_path),
          language_(language),
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

    void set_vad_params(int window_ms, float threshold_mult, int silence_ms = -1) {
        if (window_ms >= 10 && window_ms <= 500) {
            vad_frame_size_ = static_cast<size_t>(16 * window_ms);
        }
        if (threshold_mult >= 0.5f && threshold_mult <= 10.0f) {
            vad_threshold_mult_ = threshold_mult;
        }
        if (silence_ms > 0) {
            vad_silence_frames_ = silence_ms / std::max(1, (int)(vad_frame_size_ / 16));
        }
        int window_actual = (int)(vad_frame_size_ / 16);
        int silence_actual = vad_silence_frames_ * window_actual;
        std::cout << "VAD config: window=" << window_actual << "ms"
                  << " threshold=" << vad_threshold_mult_
                  << " silence=" << silence_actual << "ms"
                  << " max_chunk=" << (vad_max_speech_samples_ / 16000) << "s"
                  << " min_chunk=" << (vad_min_speech_samples_ * 1000 / 16000) << "ms"
                  << std::endl;
    }

    bool init() {
        if (!interconnect_.initialize()) {
            std::cerr << "Failed to initialize interconnect" << std::endl;
            return false;
        }

        std::cout << "Interconnect initialized (peer-to-peer)" << std::endl;

        log_fwd_.init(whispertalk::FRONTEND_LOG_PORT, whispertalk::ServiceType::WHISPER_SERVICE);

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
                    while (pos + vad_frame_size_ <= call->audio_buffer.size()) {
                        float energy = 0;
                        for (size_t i = 0; i < vad_frame_size_; ++i) {
                            energy += call->audio_buffer[pos + i] * call->audio_buffer[pos + i];
                        }
                        energy /= static_cast<float>(vad_frame_size_);

                        call->frame_count++;
                        if (!call->in_speech) {
                            float nf = call->noise_floor * 0.95f + energy * 0.05f;
                            // Clamp noise floor above G.711 codec quantization noise (~0.000001).
                            // Without this clamp, the noise floor adapts arbitrarily low during
                            // digital silence, causing the threshold to drop and triggering
                            // false VAD activations on codec noise.
                            call->noise_floor = std::max(nf, 0.000005f);
                        }
                        float threshold = std::max(call->noise_floor * vad_threshold_mult_, vad_min_energy_);

                        if (energy > threshold) {
                            if (!call->in_speech) {
                                call->in_speech = true;
                                size_t context = vad_frame_size_ * vad_context_frames_;
                                call->speech_start = (pos > context) ? pos - context : 0;
                                if (vad_logging_enabled_) {
                                    log_fwd_.forward("DEBUG", call->id,
                                        "VAD speech_start at sample %zu (energy=%.6f threshold=%.6f noise_floor=%.6f)",
                                        pos, energy, threshold, call->noise_floor);
                                }
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

                        pos += vad_frame_size_;

                        if (call->in_speech && call->silence_count > vad_silence_frames_) {
                            to_process.assign(
                                call->audio_buffer.begin() + call->speech_start,
                                call->audio_buffer.begin() + pos);
                            if (vad_logging_enabled_) {
                                double dur_ms = to_process.size() / 16.0;
                                log_fwd_.forward("DEBUG", call->id,
                                    "VAD speech_end (silence) — %zu samples (%.0fms) queued for transcription",
                                    to_process.size(), dur_ms);
                            }
                            call->audio_buffer.erase(call->audio_buffer.begin(),
                                call->audio_buffer.begin() + pos);
                            pos = 0;
                            call->in_speech = false;
                            call->silence_count = 0;
                            call->speech_start = 0;
                            call->noise_floor = 0.00005f;
                            break;
                        }

                        size_t speech_len = pos - call->speech_start;
                        if (call->in_speech && speech_len > vad_max_speech_samples_) {
                            to_process.assign(
                                call->audio_buffer.begin() + call->speech_start,
                                call->audio_buffer.begin() + pos);
                            if (vad_logging_enabled_) {
                                double dur_ms = to_process.size() / 16.0;
                                log_fwd_.forward("DEBUG", call->id,
                                    "VAD speech_end (max_length) — %zu samples (%.0fms) queued for transcription",
                                    to_process.size(), dur_ms);
                            }
                            call->audio_buffer.erase(call->audio_buffer.begin(),
                                call->audio_buffer.begin() + pos);
                            pos = 0;
                            call->in_speech = false;
                            call->silence_count = 0;
                            call->speech_start = 0;
                            call->noise_floor = 0.00005f;
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
                        if (inactivity > vad_inactivity_flush_ms_) {
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
                                call->noise_floor = 0.00005f;
                                if (call->speech_signaled) {
                                    call->speech_signaled = false;
                                    interconnect_.broadcast_speech_signal(call->id, false);
                                }
                                if (vad_logging_enabled_) {
                                    double dur_ms = to_process.size() / 16.0;
                                    log_fwd_.forward("DEBUG", call->id,
                                        "VAD speech_end (inactivity %dms) — %zu samples (%.0fms) queued",
                                        vad_inactivity_flush_ms_, to_process.size(), dur_ms);
                                }
                                std::cout << "🔄 [" << call->id << "] Inactivity flush: " << to_process.size() << " samples → SPEECH_IDLE" << std::endl;
                            }
                        }
                    }

                    if (!call->in_speech) {
                        size_t keep_frames = vad_context_frames_ + 2;
                        size_t keep = vad_frame_size_ * keep_frames;
                        if (call->vad_pos > keep) {
                            call->audio_buffer.erase(call->audio_buffer.begin(),
                                call->audio_buffer.begin() + (call->vad_pos - keep));
                            call->vad_pos = keep;
                        }
                    }

                    if (call->speech_signaled) {
                        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                            now - call->speech_signal_time).count();
                        if (elapsed > speech_signal_timeout_s_) {
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
                            std::cerr << "⚠️  [" << call->id << "] SPEECH_ACTIVE timeout (" << speech_signal_timeout_s_ << "s) — forcing SPEECH_IDLE" << std::endl;
                        }
                    }
                }

                if (!to_process.empty()) {
                    transcribe_and_send(call->id, to_process);
                }
            }
        }
    }

    // Transcribes a short audio chunk (typically 1-4s) and sends the text downstream.
    // Uses GREEDY decoding for speed — on short chunks the accuracy difference vs beam search
    // is negligible, but inference is 3-5x faster. Context from the previous chunk's
    // transcription is passed via initial_prompt to maintain coherence across chunk boundaries.
    // audio_ctx is set proportionally to the audio length to avoid Whisper wasting compute
    // on empty spectrogram frames beyond the actual audio.
    // Detects common Whisper hallucination patterns on near-silence audio.
    // Returns true if the text is likely a hallucination.
    bool is_hallucination(const std::string& text) {
        if (text.empty()) return false;
        std::string t = text;
        while (!t.empty() && t.front() == ' ') t.erase(t.begin());
        while (!t.empty() && t.back() == ' ') t.pop_back();
        if (t.size() < 3) return true;

        // Strip trailing punctuation for pattern matching so "Vielen Dank." matches.
        std::string core = t;
        while (!core.empty() && (core.back() == '.' || core.back() == '!' ||
               core.back() == '?' || core.back() == ',')) {
            core.pop_back();
        }
        while (!core.empty() && core.back() == ' ') core.pop_back();

        // Only match when the ENTIRE transcription is a hallucination pattern.
        // Previously used find() which caused false positives when patterns
        // appeared as part of legitimate speech (e.g. "Vielen Dank" in a sentence).
        static const char* patterns[] = {
            "Untertitel",
            "Vielen Dank",
            "Tschüss",
            "Bis zum nächsten Mal",
            "Copyright",
            "www.",
            "SWR",
            "Amara.org",
            "Danke fürs Zuschauen",
            "Ich habe mich nicht mehr",
            "Und ich habe mich nicht mehr",
            "Ich habe es mir nicht mehr",
            "es mir nicht mehr",
            "Danke für",
            "Bis bald",
            "Auf Wiedersehen",
            "Guten Tag",
            "Herzlich willkommen",
            "Musik",
            "Applaus",
            "[Musik]",
            "[Applaus]",
            "MwSt",
            nullptr
        };
        for (int i = 0; patterns[i]; ++i) {
            if (core == patterns[i]) return true;
        }
        // Detect repetitive text (same short phrase repeated)
        if (t.size() > 20) {
            std::string half = t.substr(0, t.size() / 2);
            if (t.find(half, half.size()) != std::string::npos) return true;
        }
        return false;
    }

    void transcribe_and_send(uint32_t call_id, const std::vector<float>& audio) {
        if (audio.size() < vad_min_speech_samples_) {
            if (vad_logging_enabled_) {
                log_fwd_.forward("DEBUG", call_id,
                    "Skipping chunk: %zu samples (%.0fms) below minimum %zu",
                    audio.size(), audio.size() / 16.0, vad_min_speech_samples_);
            }
            return;
        }

        // RMS energy check: reject chunks that are near-silence to prevent hallucinations.
        // G.711 codec noise floor is ~0.001 RMS; real speech is typically >0.01 RMS.
        float rms = 0.0f;
        for (size_t i = 0; i < audio.size(); ++i) rms += audio[i] * audio[i];
        rms = std::sqrt(rms / audio.size());

        float peak = 0.0f;
        for (auto s : audio) peak = std::max(peak, std::abs(s));

        if (vad_logging_enabled_) {
            log_fwd_.forward("DEBUG", call_id,
                "Audio chunk: %zu samples (%.0fms) RMS=%.4f peak=%.4f",
                audio.size(), audio.size() / 16.0, rms, peak);
        }

        if (rms < 0.005f) {
            if (vad_logging_enabled_) {
                log_fwd_.forward("DEBUG", call_id,
                    "Skipping low-energy chunk: RMS=%.6f (%.0fms)", rms, audio.size() / 16.0);
            }
            return;
        }

        // No normalization — whisper-cli doesn't normalize and produces correct
        // transcriptions on G.711 round-tripped audio. Normalization changes the
        // signal characteristics and confuses the model.

        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.language = language_.c_str();
        wparams.n_threads = 4;
        wparams.no_timestamps = true;
        wparams.single_segment = false;
        wparams.greedy.best_of = 1;
        wparams.temperature = 0.0f;
        wparams.temperature_inc = 0.2f;
        wparams.entropy_thold = 2.4f;
        wparams.logprob_thold = -1.0f;
        wparams.no_speech_thold = 0.6f;

        // Use full encoder context (audio_ctx=0 means use all 1500 frames).
        // Restricting audio_ctx to the actual audio length degraded transcription
        // quality — Whisper's encoder attention mechanism works better with full context
        // even when most frames are silence, matching whisper-cli default behavior.
        wparams.audio_ctx = 0;

        wparams.no_context = true;
        wparams.initial_prompt = nullptr;

        auto t0 = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(whisper_mutex_);
        int result = whisper_full(ctx_, wparams, audio.data(), audio.size());

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
                if (is_hallucination(text)) {
                    if (vad_logging_enabled_) {
                        log_fwd_.forward("DEBUG", call_id, "Hallucination filtered: %s", text.c_str());
                    }
                    std::cout << "👻 [" << call_id << "] Hallucination filtered: " << text << std::endl;
                    {
                        std::lock_guard<std::mutex> clk(calls_mutex_);
                        auto it = calls_.find(call_id);
                        if (it != calls_.end()) it->second->last_transcription.clear();
                    }
                    return;
                }

                {
                    std::lock_guard<std::mutex> clk(calls_mutex_);
                    auto it = calls_.find(call_id);
                    if (it != calls_.end()) {
                        it->second->last_transcription = text;
                    }
                }

                double audio_dur_ms = audio.size() / 16.0;
                double rtf = whisper_ms / audio_dur_ms;
                std::cout << "📝 [" << call_id << "] Transcription (" << whisper_ms << "ms, RTF=" 
                          << std::fixed << std::setprecision(2) << rtf << "): " << text << std::endl;
                log_fwd_.forward("INFO", call_id, "Transcription (%lldms): %s", whisper_ms, text.c_str());

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
            log_fwd_.forward("ERROR", call_id, "Whisper transcription failed");
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
        log_fwd_.forward("INFO", cid, "Created transcription session");
        return call;
    }

    void handle_call_end(uint32_t call_id) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(call_id)) {
            std::cout << "🛑 Call " << call_id << " ended, closing transcription session" << std::endl;
            log_fwd_.forward("INFO", call_id, "Call ended, closing transcription session");
            calls_.erase(call_id);
        }
    }

    std::atomic<bool> running_;
    std::string model_path_;
    std::string language_;
    struct whisper_context* ctx_ = nullptr;
    std::mutex whisper_mutex_;
    std::mutex calls_mutex_;
    std::map<uint32_t, std::shared_ptr<WhisperCall>> calls_;
    whispertalk::InterconnectNode interconnect_;
    whispertalk::LogForwarder log_fwd_;
    
    std::mutex buffer_mutex_;
    std::deque<whispertalk::Packet> buffered_packets_;
};

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    std::string model_path;
    std::string language = "de";

    int vad_window_ms = 50;
    float vad_threshold = 2.0f;
    int vad_silence_ms = 400;

    static struct option long_opts[] = {
        {"language", required_argument, 0, 'l'},
        {"model", required_argument, 0, 'm'},
        {"vad-window-ms", required_argument, 0, 'w'},
        {"vad-threshold", required_argument, 0, 't'},
        {"vad-silence-ms", required_argument, 0, 's'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "l:m:w:t:s:", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'l': language = optarg; break;
            case 'm': model_path = optarg; break;
            case 'w': vad_window_ms = atoi(optarg); break;
            case 't': vad_threshold = atof(optarg); break;
            case 's': vad_silence_ms = atoi(optarg); break;
            default: break;
        }
    }

    if (model_path.empty()) {
        if (optind < argc) {
            model_path = argv[optind];
        } else {
            const char* env_models = std::getenv("WHISPERTALK_MODELS_DIR");
            std::string models_dir = env_models ? env_models :
#ifdef WHISPERTALK_MODELS_DIR
                WHISPERTALK_MODELS_DIR;
#else
                "models";
#endif
            model_path = models_dir + "/ggml-large-v3-turbo-q5_0.bin";
        }
    }

    std::cout << "Whisper model: " << model_path << std::endl;
    std::cout << "Language: " << language << std::endl;

    try {
        WhisperService service(model_path, language);
        service.set_vad_params(vad_window_ms, vad_threshold, vad_silence_ms);
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
