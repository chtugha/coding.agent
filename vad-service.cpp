// VAD Service — Voice Activity Detection as a standalone pipeline stage.
//
// Pipeline position: IAP → [VAD] → Whisper
//
// Receives continuous float32 PCM audio (16kHz) from IAP via interconnect,
// segments it into speech chunks using energy-based VAD, and forwards only
// the speech-containing audio segments to Whisper for transcription.
//
// VAD strategy: Energy-based with adaptive micro-pause detection.
// Instead of waiting for full sentence silence (1500ms), we detect short pauses (~400ms)
// between words/phrases and submit smaller chunks (1.5-4s) to Whisper. This cuts latency
// dramatically because Whisper inference time scales ~quadratically with input length.
// Context coherence is maintained by Whisper's initial_prompt mechanism.
//
// Speech signal management: Broadcasts SPEECH_ACTIVE/SPEECH_IDLE signals downstream
// to coordinate with other services (e.g., Kokoro stops TTS playback on speech detect).
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
#include <signal.h>
#include <getopt.h>
#include "interconnect.h"

static std::atomic<bool> g_running{true};
static void sig_handler(int) { g_running = false; }

struct VadCall {
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
};

class VadService {
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
    VadService() 
        : running_(true),
          interconnect_(whispertalk::ServiceType::VAD_SERVICE) {}

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

        log_fwd_.init(whispertalk::FRONTEND_LOG_PORT, whispertalk::ServiceType::VAD_SERVICE);

        if (!interconnect_.connect_to_downstream()) {
            std::cout << "Downstream (Whisper) not available yet - will auto-reconnect" << std::endl;
        }

        interconnect_.register_call_end_handler([this](uint32_t call_id) {
            this->handle_call_end(call_id);
        });

        return true;
    }

    void run() {
        std::thread receiver_thread(&VadService::receiver_loop, this);
        std::thread processor_thread(&VadService::processing_loop, this);

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
        while (running_ && g_running) {
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

            std::vector<std::shared_ptr<VadCall>> active;
            {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                for (auto& p : calls_) active.push_back(p.second);
            }

            for (auto& call : active) {
                std::vector<float> to_send;
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
                                    std::cout << "[" << call->id << "] SPEECH_ACTIVE broadcast (VAD)" << std::endl;
                                }
                            }
                            call->silence_count = 0;
                        } else if (call->in_speech) {
                            call->silence_count++;
                        }

                        pos += vad_frame_size_;

                        if (call->in_speech && call->silence_count > vad_silence_frames_) {
                            to_send.assign(
                                call->audio_buffer.begin() + call->speech_start,
                                call->audio_buffer.begin() + pos);
                            if (vad_logging_enabled_) {
                                double dur_ms = to_send.size() / 16.0;
                                log_fwd_.forward("DEBUG", call->id,
                                    "VAD speech_end (silence) — %zu samples (%.0fms) queued for transcription",
                                    to_send.size(), dur_ms);
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
                            to_send.assign(
                                call->audio_buffer.begin() + call->speech_start,
                                call->audio_buffer.begin() + pos);
                            if (vad_logging_enabled_) {
                                double dur_ms = to_send.size() / 16.0;
                                log_fwd_.forward("DEBUG", call->id,
                                    "VAD speech_end (max_length) — %zu samples (%.0fms) queued for transcription",
                                    to_send.size(), dur_ms);
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

                    if (call->in_speech && to_send.empty() && call->last_buffer_size > 0) {
                        auto inactivity = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now - call->last_buffer_growth).count();
                        if (inactivity > vad_inactivity_flush_ms_) {
                            size_t end = call->audio_buffer.size();
                            if (end > call->speech_start) {
                                to_send.assign(
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
                                    double dur_ms = to_send.size() / 16.0;
                                    log_fwd_.forward("DEBUG", call->id,
                                        "VAD speech_end (inactivity %dms) — %zu samples (%.0fms) queued",
                                        vad_inactivity_flush_ms_, to_send.size(), dur_ms);
                                }
                                std::cout << "[" << call->id << "] Inactivity flush: " << to_send.size() << " samples -> SPEECH_IDLE" << std::endl;
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
                                to_send.assign(
                                    call->audio_buffer.begin() + call->speech_start,
                                    call->audio_buffer.end());
                                call->audio_buffer.clear();
                                call->vad_pos = 0;
                                call->in_speech = false;
                                call->silence_count = 0;
                                call->speech_start = 0;
                                call->last_buffer_size = 0;
                                std::cout << "[" << call->id
                                          << "] SPEECH_ACTIVE timeout (" << speech_signal_timeout_s_ << "s) — forcing SPEECH_IDLE" << std::endl;
                            }
                            call->speech_signaled = false;
                            interconnect_.broadcast_speech_signal(call->id, false);
                        }
                    }
                }

                if (!to_send.empty()) {
                    send_chunk_downstream(call->id, to_send);
                }
            }
        }
    }

    // Forwards a speech chunk to Whisper via interconnect.
    // Applies minimum length and RMS energy filters to reject noise/clicks.
    void send_chunk_downstream(uint32_t call_id, const std::vector<float>& audio) {
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

        if (rms < 0.005f) {
            if (vad_logging_enabled_) {
                log_fwd_.forward("DEBUG", call_id,
                    "Skipping low-energy chunk: RMS=%.6f (%.0fms)", rms, audio.size() / 16.0);
            }
            return;
        }

        if (vad_logging_enabled_) {
            float peak = 0.0f;
            for (auto s : audio) peak = std::max(peak, std::abs(s));
            log_fwd_.forward("INFO", call_id,
                "VAD chunk -> Whisper: %zu samples (%.0fms) RMS=%.4f peak=%.4f",
                audio.size(), audio.size() / 16.0, rms, peak);
        }

        whispertalk::Packet pkt(call_id, audio.data(), audio.size() * sizeof(float));
        pkt.trace.record(whispertalk::ServiceType::VAD_SERVICE, 0);
        pkt.trace.record(whispertalk::ServiceType::VAD_SERVICE, 1);
        if (!interconnect_.send_to_downstream(pkt)) {
            if (interconnect_.downstream_state() != whispertalk::ConnectionState::CONNECTED) {
                log_fwd_.forward("WARN", call_id, "Whisper disconnected, discarding speech chunk");
            }
        }
    }

    std::shared_ptr<VadCall> get_or_create_call(uint32_t cid) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(cid)) return calls_[cid];
        auto call = std::make_shared<VadCall>();
        call->id = cid;
        call->last_activity = std::chrono::steady_clock::now();
        call->last_buffer_growth = call->last_activity;
        calls_[cid] = call;
        std::cout << "Created VAD session for call_id " << cid << std::endl;
        log_fwd_.forward("INFO", cid, "Created VAD session");
        return call;
    }

    void handle_call_end(uint32_t call_id) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(call_id)) {
            std::cout << "Call " << call_id << " ended, closing VAD session" << std::endl;
            log_fwd_.forward("INFO", call_id, "Call ended, closing VAD session");
            calls_.erase(call_id);
        }
    }

    std::atomic<bool> running_;
    std::mutex calls_mutex_;
    std::map<uint32_t, std::shared_ptr<VadCall>> calls_;
    whispertalk::InterconnectNode interconnect_;
    whispertalk::LogForwarder log_fwd_;
};

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    int vad_window_ms = 50;
    float vad_threshold = 2.0f;
    int vad_silence_ms = 400;

    static struct option long_opts[] = {
        {"vad-window-ms", required_argument, 0, 'w'},
        {"vad-threshold", required_argument, 0, 't'},
        {"vad-silence-ms", required_argument, 0, 's'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "w:t:s:", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'w': vad_window_ms = atoi(optarg); break;
            case 't': vad_threshold = atof(optarg); break;
            case 's': vad_silence_ms = atoi(optarg); break;
            default: break;
        }
    }

    std::cout << "VAD Service starting..." << std::endl;

    VadService service;
    service.set_vad_params(vad_window_ms, vad_threshold, vad_silence_ms);
    if (!service.init()) {
        return 1;
    }
    service.run();

    return 0;
}
