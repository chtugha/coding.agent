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
// Smart-split: When max chunk length is reached during speech, the split point is placed
// at the lowest-energy frame boundary near the end to avoid cutting mid-word.
//
// Speech signal management: Broadcasts SPEECH_ACTIVE/SPEECH_IDLE signals downstream
// to coordinate with other services (e.g., Kokoro stops TTS playback on speech detect).
#include <iostream>
#include <vector>
#include <deque>
#include <string>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <map>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <signal.h>
#include <getopt.h>
#include "interconnect.h"

static constexpr int VAD_SAMPLE_RATE = 16000;
static constexpr int VAD_SAMPLES_PER_MS = VAD_SAMPLE_RATE / 1000;

static std::atomic<bool> g_running{true};
static void sig_handler(int) { g_running = false; }

struct VadCall {
    uint32_t id;
    std::deque<float> audio_buffer;
    std::mutex mutex;
    bool in_speech = false;
    bool speech_signaled = false;
    int silence_count = 0;
    int onset_count = 0;
    float noise_floor = 0.00005f;
    int frame_count = 0;
    size_t vad_pos = 0;
    size_t speech_start = 0;
    size_t tentative_speech_start = 0;
    std::chrono::steady_clock::time_point last_activity;
    std::chrono::steady_clock::time_point speech_signal_time;
    size_t last_buffer_size = 0;
    std::chrono::steady_clock::time_point last_buffer_growth;
    std::vector<float> frame_energies;
};

class VadService {
    size_t vad_frame_size_ = 800;           // 50ms @ 16kHz
    float vad_threshold_mult_ = 2.0f;
    float vad_min_energy_ = 0.00005f;
    int vad_silence_frames_ = 8;            // 8 × 50ms = 400ms
    size_t vad_max_speech_samples_ = VAD_SAMPLE_RATE * 4;  // 4s default
    size_t vad_min_speech_samples_ = VAD_SAMPLE_RATE / 2;  // 500ms
    int vad_context_frames_ = 4;
    int vad_onset_frames_ = 3;
    int speech_signal_timeout_s_ = 10;
    int vad_inactivity_flush_ms_ = 1000;
    bool vad_logging_enabled_ = true;
    size_t smart_split_window_frames_ = 6;  // look back 6 frames for energy dip

public:
    VadService()
        : running_(true),
          interconnect_(whispertalk::ServiceType::VAD_SERVICE) {}

    void set_vad_params(int window_ms, float threshold_mult, int silence_ms, int max_chunk_ms) {
        if (window_ms >= 10 && window_ms <= 500) {
            vad_frame_size_ = static_cast<size_t>(VAD_SAMPLES_PER_MS * window_ms);
        }
        if (threshold_mult >= 0.5f && threshold_mult <= 10.0f) {
            vad_threshold_mult_ = threshold_mult;
        }
        if (silence_ms > 0) {
            int frame_ms = std::max(1, (int)(vad_frame_size_ / VAD_SAMPLES_PER_MS));
            vad_silence_frames_ = silence_ms / frame_ms;
            if (vad_silence_frames_ < 1) vad_silence_frames_ = 1;
        }
        if (max_chunk_ms > 0) {
            vad_max_speech_samples_ = static_cast<size_t>(VAD_SAMPLES_PER_MS) * max_chunk_ms;
            if (vad_max_speech_samples_ < vad_min_speech_samples_ * 2) {
                vad_max_speech_samples_ = vad_min_speech_samples_ * 2;
            }
        }
        print_config();
    }

    void print_config() {
        int frame_ms = (int)(vad_frame_size_ / VAD_SAMPLES_PER_MS);
        int silence_ms = vad_silence_frames_ * frame_ms;
        int max_ms = (int)(vad_max_speech_samples_ / VAD_SAMPLES_PER_MS);
        std::cout << "VAD config: window=" << frame_ms << "ms"
                  << " threshold=" << vad_threshold_mult_
                  << " silence=" << silence_ms << "ms"
                  << " max_chunk=" << max_ms << "ms"
                  << " min_chunk=" << (vad_min_speech_samples_ * 1000 / VAD_SAMPLE_RATE) << "ms"
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
        std::thread cmd_thread(&VadService::command_listener_loop, this);

        while (running_ && g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        running_ = false;
        data_cv_.notify_all();

        int sock = cmd_sock_.exchange(-1);
        if (sock >= 0) ::close(sock);
        receiver_thread.join();
        processor_thread.join();
        cmd_thread.join();
        interconnect_.shutdown();
    }

private:
    void reset_call_state(VadCall& call) {
        call.in_speech = false;
        call.silence_count = 0;
        call.onset_count = 0;
        call.speech_start = 0;
        call.noise_floor = 0.00005f;
        call.frame_energies.clear();
        if (call.speech_signaled) {
            call.speech_signaled = false;
            interconnect_.broadcast_speech_signal(call.id, false);
        }
    }

    void compact_buffer(VadCall& call, size_t consumed) {
        if (consumed > 0 && consumed <= call.audio_buffer.size()) {
            for (size_t i = 0; i < consumed; ++i) {
                call.audio_buffer.pop_front();
            }
        } else if (consumed > call.audio_buffer.size()) {
            call.audio_buffer.clear();
        }
        call.vad_pos = 0;
        call.speech_start = 0;
        call.last_buffer_size = call.audio_buffer.size();
    }

    size_t find_smart_split_point(VadCall& call, size_t max_end) {
        if (call.frame_energies.size() < 2) return max_end;

        size_t search_start = 0;
        if (call.frame_energies.size() > smart_split_window_frames_) {
            search_start = call.frame_energies.size() - smart_split_window_frames_;
        }

        float min_energy = call.frame_energies[search_start];
        size_t min_idx = search_start;
        for (size_t i = search_start + 1; i < call.frame_energies.size(); ++i) {
            if (call.frame_energies[i] <= min_energy) {
                min_energy = call.frame_energies[i];
                min_idx = i;
            }
        }

        size_t split = call.speech_start + (min_idx + 1) * vad_frame_size_;
        if (split > call.audio_buffer.size()) split = call.audio_buffer.size();
        if (split <= call.speech_start) split = max_end;
        return split;
    }

    void command_listener_loop() {
        uint16_t port = whispertalk::service_cmd_port(whispertalk::ServiceType::VAD_SERVICE);
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return;
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "VAD cmd: bind port " << port << " failed" << std::endl;
            ::close(sock);
            return;
        }
        listen(sock, 4);
        cmd_sock_.store(sock);
        std::cout << "VAD command listener on port " << port << std::endl;
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
                std::string response = handle_vad_command(cmd);
                send(csock, response.c_str(), response.size(), 0);
            }
            ::close(csock);
        }
    }

    std::string handle_vad_command(const std::string& cmd) {
        if (cmd == "PING") return "PONG\n";
        if (cmd == "STATUS") {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            size_t speech_active = 0;
            for (const auto& [id, call] : calls_) {
                std::lock_guard<std::mutex> cl(call->mutex);
                if (call->speech_signaled) speech_active++;
            }
            int frame_ms = (int)(vad_frame_size_ / VAD_SAMPLES_PER_MS);
            int silence_ms = vad_silence_frames_ * frame_ms;
            int max_ms = (int)(vad_max_speech_samples_ / VAD_SAMPLES_PER_MS);
            return "ACTIVE_CALLS:" + std::to_string(calls_.size())
                + ":SPEECH_ACTIVE:" + std::to_string(speech_active)
                + ":UPSTREAM:" + (interconnect_.upstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected")
                + ":DOWNSTREAM:" + (interconnect_.downstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected")
                + ":WINDOW_MS:" + std::to_string(frame_ms)
                + ":THRESHOLD:" + std::to_string(vad_threshold_mult_).substr(0, 4)
                + ":SILENCE_MS:" + std::to_string(silence_ms)
                + ":MAX_CHUNK_MS:" + std::to_string(max_ms)
                + "\n";
        }
        return "ERROR:Unknown command\n";
    }

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

            size_t sample_count = pkt.payload_size / sizeof(float);
            const float* samples = reinterpret_cast<const float*>(pkt.payload.data());

            {
                std::lock_guard<std::mutex> lock(call->mutex);
                call->audio_buffer.insert(call->audio_buffer.end(), samples, samples + sample_count);
                call->last_activity = std::chrono::steady_clock::now();
            }
            data_cv_.notify_one();
        }
    }

    void processing_loop() {
        while (running_ && g_running) {
            {
                std::unique_lock<std::mutex> lk(data_mutex_);
                data_cv_.wait_for(lk, std::chrono::milliseconds(5));
            }

            std::vector<std::shared_ptr<VadCall>> active;
            {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                active.reserve(calls_.size());
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
                            float s = call->audio_buffer[pos + i];
                            energy += s * s;
                        }
                        energy /= static_cast<float>(vad_frame_size_);

                        call->frame_count++;
                        if (!call->in_speech && call->onset_count == 0) {
                            float nf = call->noise_floor * 0.95f + energy * 0.05f;
                            call->noise_floor = std::max(nf, 0.000005f);
                        }
                        float threshold = std::max(call->noise_floor * vad_threshold_mult_, vad_min_energy_);

                        if (energy > threshold) {
                            if (!call->in_speech) {
                                call->onset_count++;
                                if (call->onset_count == 1) {
                                    size_t context = vad_frame_size_ * vad_context_frames_;
                                    call->tentative_speech_start = (pos > context) ? pos - context : 0;
                                }
                                if (call->onset_count >= vad_onset_frames_) {
                                    call->in_speech = true;
                                    call->speech_start = call->tentative_speech_start;
                                    call->frame_energies.clear();
                                    if (vad_logging_enabled_) {
                                        log_fwd_.forward("DEBUG", call->id,
                                            "VAD speech_start at sample %zu (energy=%.6f threshold=%.6f noise_floor=%.6f onset=%d)",
                                            pos, energy, threshold, call->noise_floor, call->onset_count);
                                    }
                                    if (!call->speech_signaled) {
                                        call->speech_signaled = true;
                                        call->speech_signal_time = std::chrono::steady_clock::now();
                                        interconnect_.broadcast_speech_signal(call->id, true);
                                        std::cout << "[" << call->id << "] SPEECH_ACTIVE broadcast (VAD)" << std::endl;
                                    }
                                }
                            }
                            call->silence_count = 0;
                        } else {
                            call->onset_count = 0;
                            if (call->in_speech) {
                                call->silence_count++;
                            }
                        }

                        if (call->in_speech) {
                            call->frame_energies.push_back(energy);
                        }

                        pos += vad_frame_size_;

                        if (call->in_speech && call->silence_count > vad_silence_frames_) {
                            to_send.assign(
                                call->audio_buffer.begin() + call->speech_start,
                                call->audio_buffer.begin() + pos);
                            if (vad_logging_enabled_) {
                                double dur_ms = to_send.size() / (double)VAD_SAMPLES_PER_MS;
                                log_fwd_.forward("DEBUG", call->id,
                                    "VAD speech_end (silence) — %zu samples (%.0fms) queued for transcription",
                                    to_send.size(), dur_ms);
                            }
                            compact_buffer(*call, pos);
                            pos = 0;
                            reset_call_state(*call);
                            break;
                        }

                        size_t speech_len = pos - call->speech_start;
                        if (call->in_speech && speech_len > vad_max_speech_samples_) {
                            size_t split = find_smart_split_point(*call, pos);
                            to_send.assign(
                                call->audio_buffer.begin() + call->speech_start,
                                call->audio_buffer.begin() + split);
                            if (vad_logging_enabled_) {
                                double dur_ms = to_send.size() / (double)VAD_SAMPLES_PER_MS;
                                bool was_smart = (split != pos);
                                log_fwd_.forward("DEBUG", call->id,
                                    "VAD speech_end (max_length%s) — %zu samples (%.0fms) queued for transcription",
                                    was_smart ? ", smart-split" : "", to_send.size(), dur_ms);
                            }
                            size_t leftover_start = split;
                            bool has_leftover = (split < pos);

                            compact_buffer(*call, leftover_start);
                            pos = 0;

                            if (has_leftover && call->audio_buffer.size() > 0) {
                                call->in_speech = true;
                                call->speech_start = 0;
                                call->silence_count = 0;
                                call->onset_count = vad_onset_frames_;
                                call->frame_energies.clear();
                            } else {
                                reset_call_state(*call);
                            }
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
                                call->last_buffer_size = 0;
                                reset_call_state(*call);
                                if (vad_logging_enabled_) {
                                    double dur_ms = to_send.size() / (double)VAD_SAMPLES_PER_MS;
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
                            size_t trim = call->vad_pos - keep;
                            for (size_t i = 0; i < trim; ++i) call->audio_buffer.pop_front();
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
                                call->last_buffer_size = 0;
                                std::cout << "[" << call->id
                                          << "] SPEECH_ACTIVE timeout (" << speech_signal_timeout_s_ << "s) — forcing SPEECH_IDLE" << std::endl;
                            }
                            reset_call_state(*call);
                        }
                    }
                }

                if (!to_send.empty()) {
                    send_chunk_downstream(call->id, to_send);
                }
            }
        }
    }

    void send_chunk_downstream(uint32_t call_id, const std::vector<float>& audio) {
        if (audio.size() < vad_min_speech_samples_) {
            if (vad_logging_enabled_) {
                log_fwd_.forward("DEBUG", call_id,
                    "Skipping chunk: %zu samples (%.0fms) below minimum %zu",
                    audio.size(), audio.size() / (double)VAD_SAMPLES_PER_MS, vad_min_speech_samples_);
            }
            return;
        }

        float sum_sq = 0.0f;
        float peak = 0.0f;
        for (size_t i = 0; i < audio.size(); ++i) {
            float s = audio[i];
            sum_sq += s * s;
            float a = std::abs(s);
            if (a > peak) peak = a;
        }
        float rms = std::sqrt(sum_sq / audio.size());

        if (rms < 0.005f) {
            if (vad_logging_enabled_) {
                log_fwd_.forward("DEBUG", call_id,
                    "Skipping low-energy chunk: RMS=%.6f (%.0fms)", rms, audio.size() / (double)VAD_SAMPLES_PER_MS);
            }
            return;
        }

        if (vad_logging_enabled_) {
            log_fwd_.forward("INFO", call_id,
                "VAD chunk -> Whisper: %zu samples (%.0fms) RMS=%.4f peak=%.4f",
                audio.size(), audio.size() / (double)VAD_SAMPLES_PER_MS, rms, peak);
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
        auto it = calls_.find(cid);
        if (it != calls_.end()) return it->second;
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
        auto it = calls_.find(call_id);
        if (it != calls_.end()) {
            std::cout << "Call " << call_id << " ended, closing VAD session" << std::endl;
            log_fwd_.forward("INFO", call_id, "Call ended, closing VAD session");
            calls_.erase(it);
        }
    }

    std::atomic<bool> running_;
    std::atomic<int> cmd_sock_{-1};
    std::mutex calls_mutex_;
    std::mutex data_mutex_;
    std::condition_variable data_cv_;
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
    int vad_max_chunk_ms = 4000;

    static struct option long_opts[] = {
        {"vad-window-ms",    required_argument, 0, 'w'},
        {"vad-threshold",    required_argument, 0, 't'},
        {"vad-silence-ms",   required_argument, 0, 's'},
        {"vad-max-chunk-ms", required_argument, 0, 'c'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "w:t:s:c:", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'w': vad_window_ms = atoi(optarg); break;
            case 't': vad_threshold = atof(optarg); break;
            case 's': vad_silence_ms = atoi(optarg); break;
            case 'c': vad_max_chunk_ms = atoi(optarg); break;
            default: break;
        }
    }

    std::cout << "VAD Service starting..." << std::endl;

    VadService service;
    service.set_vad_params(vad_window_ms, vad_threshold, vad_silence_ms, vad_max_chunk_ms);
    if (!service.init()) {
        return 1;
    }
    service.run();

    return 0;
}
