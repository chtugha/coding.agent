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
// at the lowest-energy frame boundary near the end to avoid cutting mid-word. The
// frame_energies vector tracks per-frame energy starting from the onset confirmation
// frame (onset_frame_3), and the split calculation applies an origin correction via
// energies_sample_origin to map back to absolute buffer positions.
//
// Speech signal management: Broadcasts SPEECH_ACTIVE/SPEECH_IDLE signals downstream
// to coordinate with other services (e.g., Kokoro stops TTS playback on speech detect).
#include <iostream>
#include <iomanip>
#include <sstream>
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
    // Adaptive noise floor estimate. Initialized above the G.711 μ-law codec noise
    // floor: G.711 silence bytes (0xFF/0x7F) decode to ±0.000885 → energy ~0.00000078.
    // We set min_floor at 0.000005 and init at 0.00005 to avoid false triggers while
    // still adapting to the actual ambient noise level.
    float noise_floor = 0.00005f;
    int frame_count = 0;
    size_t vad_pos = 0;
    size_t speech_start = 0;
    size_t tentative_speech_start = 0;
    // Absolute buffer position where frame_energies[0] begins. Set to the onset
    // confirmation frame position so smart-split can map energy indices back to
    // buffer positions accurately (speech_start includes pre-speech context frames
    // which are not represented in frame_energies).
    size_t energies_sample_origin = 0;
    std::chrono::steady_clock::time_point last_activity;
    std::chrono::steady_clock::time_point speech_signal_time;
    size_t last_buffer_size = 0;
    std::chrono::steady_clock::time_point last_buffer_growth;
    std::vector<float> frame_energies;
};

class VadService {
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
    // vad_max_speech_samples_: 8s max chunk — Whisper large-v3-turbo handles 8s
    //   chunks in ~1s on Apple Silicon. Longer chunks preserve sentence boundaries.
    size_t vad_max_speech_samples_ = VAD_SAMPLE_RATE * 8;
    // vad_min_speech_samples_: 500ms — reject clicks and noise bursts.
    size_t vad_min_speech_samples_ = VAD_SAMPLE_RATE / 2;
    // vad_context_frames_: include 8 frames (400ms) of pre-speech context audio so the
    //   chunk captures the onset of speech including weak initial vowels/consonants.
    int vad_context_frames_ = 8;
    // vad_onset_frames_: require 3 consecutive above-threshold frames to confirm
    //   speech onset, preventing single-frame noise spikes from triggering.
    int vad_onset_frames_ = 3;
    int speech_signal_timeout_s_ = 10;
    // vad_inactivity_flush_ms_: if no new audio arrives for 1000ms while speech is
    //   active, flush the buffer immediately (handles end-of-stream).
    int vad_inactivity_flush_ms_ = 1000;
    bool vad_logging_enabled_ = true;
    // smart_split_window_frames_: when max chunk length is reached, search the last
    //   6 frames for the lowest-energy point to place the split boundary.
    size_t smart_split_window_frames_ = 6;

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
    // Resets VAD state for a call after a chunk has been emitted.
    // Does NOT broadcast speech signal — the caller must do so after releasing
    // call->mutex to avoid holding the mutex during a TCP send.
    // Returns true if speech_signaled was set (caller should broadcast SPEECH_IDLE).
    bool reset_call_state(VadCall& call) {
        bool was_signaled = call.speech_signaled;
        call.in_speech = false;
        call.silence_count = 0;
        call.onset_count = 0;
        call.speech_start = 0;
        call.energies_sample_origin = 0;
        // Reset noise floor to init value (above G.711 codec noise floor) so
        // adaptive tracking restarts cleanly for the next speech segment.
        call.noise_floor = 0.00005f;
        call.frame_energies.clear();
        call.speech_signaled = false;
        return was_signaled;
    }

    // Removes `consumed` samples from the front of the audio buffer.
    // Uses deque::erase for bulk removal (lower overhead than N pop_front() calls).
    void compact_buffer(VadCall& call, size_t consumed) {
        if (consumed >= call.audio_buffer.size()) {
            call.audio_buffer.clear();
        } else if (consumed > 0) {
            call.audio_buffer.erase(call.audio_buffer.begin(),
                                    call.audio_buffer.begin() + static_cast<ptrdiff_t>(consumed));
        }
        call.vad_pos = 0;
        call.speech_start = 0;
        call.last_buffer_size = call.audio_buffer.size();
    }

    // Finds the best split point near the max-chunk boundary by locating the
    // lowest-energy frame in the last smart_split_window_frames_ frames.
    // frame_energies tracks energy starting from energies_sample_origin (the onset
    // confirmation frame), not from speech_start (which includes pre-speech context).
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

        // Map energy index back to buffer position using the recorded origin offset.
        // energies_sample_origin is where frame_energies[0] starts in the buffer.
        size_t split = call.energies_sample_origin + (min_idx + 1) * vad_frame_size_;
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

    static std::string format_threshold(float val) {
        std::ostringstream oss;
        oss << std::fixed << std::setprecision(2) << val;
        return oss.str();
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
                + ":THRESHOLD:" + format_threshold(vad_threshold_mult_)
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
                bool has_calls;
                {
                    std::lock_guard<std::mutex> cl(calls_mutex_);
                    has_calls = !calls_.empty();
                }
                data_cv_.wait_for(lk, std::chrono::milliseconds(has_calls ? 5 : 50));
            }

            std::vector<std::shared_ptr<VadCall>> active;
            {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                active.reserve(calls_.size());
                for (auto& p : calls_) active.push_back(p.second);
            }

            for (auto& call : active) {
                std::vector<float> to_send;
                bool needs_idle_broadcast = false;
                bool needs_active_broadcast = false;
                uint32_t call_id = call->id;
                {
                    std::lock_guard<std::mutex> lock(call->mutex);

                    size_t pos = call->vad_pos;
                    std::vector<float> frame_buf(vad_frame_size_);
                    while (pos + vad_frame_size_ <= call->audio_buffer.size()) {
                        std::copy(call->audio_buffer.begin() + pos,
                                  call->audio_buffer.begin() + pos + vad_frame_size_,
                                  frame_buf.begin());
                        float energy = 0;
                        for (size_t i = 0; i < vad_frame_size_; ++i) {
                            float s = frame_buf[i];
                            energy += s * s;
                        }
                        energy /= static_cast<float>(vad_frame_size_);

                        call->frame_count++;
                        // Adapt noise floor only when not in speech and no onset pending.
                        // Uses exponential moving average (alpha=0.05) with a hard minimum
                        // of 0.000005 to stay above G.711 quantization noise.
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
                                    // Record where frame_energies[0] starts (onset confirmation frame).
                                    // This is distinct from speech_start which includes context frames.
                                    call->energies_sample_origin = pos;
                                    if (vad_logging_enabled_) {
                                        log_fwd_.forward("DEBUG", call->id,
                                            "VAD speech_start at sample %zu (energy=%.6f threshold=%.6f noise_floor=%.6f onset=%d)",
                                            pos, energy, threshold, call->noise_floor, call->onset_count);
                                    }
                                    if (!call->speech_signaled) {
                                        call->speech_signaled = true;
                                        call->speech_signal_time = std::chrono::steady_clock::now();
                                        needs_active_broadcast = true;
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

                        // Silence-triggered speech end: enough consecutive silent frames detected.
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
                            needs_idle_broadcast = reset_call_state(*call);
                            break;
                        }

                        // Max-length triggered speech end with smart split.
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
                                // Continue speech from the leftover audio after split.
                                call->in_speech = true;
                                call->speech_start = 0;
                                call->silence_count = 0;
                                call->onset_count = 0;
                                call->frame_energies.clear();
                                call->energies_sample_origin = 0;
                                // Don't broadcast IDLE — speech is still active.
                            } else {
                                needs_idle_broadcast = reset_call_state(*call);
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

                    // Inactivity flush: no new audio for vad_inactivity_flush_ms_ while
                    // speech is active — flush remaining buffer (handles end-of-stream).
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
                                needs_idle_broadcast = reset_call_state(*call);
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

                    // Trim non-speech buffer, keeping context frames for next onset detection.
                    if (!call->in_speech) {
                        size_t keep_frames = vad_context_frames_ + 2;
                        size_t keep = vad_frame_size_ * keep_frames;
                        if (call->vad_pos > keep) {
                            size_t trim = call->vad_pos - keep;
                            call->audio_buffer.erase(call->audio_buffer.begin(),
                                                     call->audio_buffer.begin() + static_cast<ptrdiff_t>(trim));
                            call->vad_pos = keep;
                        }
                    }

                    // Speech signal timeout: force IDLE after speech_signal_timeout_s_ to
                    // prevent permanent SPEECH_ACTIVE state from stuck sessions.
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
                            needs_idle_broadcast = reset_call_state(*call);
                        }
                    }
                } // release call->mutex

                // Broadcast speech signals outside the mutex to avoid holding the
                // lock during a TCP send that could block/stall the receiver thread.
                if (needs_active_broadcast) {
                    {
                        std::lock_guard<std::mutex> lock(call->mutex);
                        call->speech_signal_time = std::chrono::steady_clock::now();
                    }
                    interconnect_.broadcast_speech_signal(call_id, true);
                    std::cout << "[" << call_id << "] SPEECH_ACTIVE broadcast (VAD)" << std::endl;
                }
                if (needs_idle_broadcast) {
                    interconnect_.broadcast_speech_signal(call_id, false);
                }

                if (!to_send.empty()) {
                    send_chunk_downstream(call_id, to_send);
                }
            }
        }
    }

    // Forwards a speech chunk to Whisper via interconnect.
    // Applies minimum length and RMS energy filters to reject noise/clicks.
    // RMS energy check: G.711 codec noise floor is ~0.001 RMS; real speech is
    // typically >0.01 RMS. Threshold at 0.005 rejects near-silence to prevent
    // Whisper hallucinations on quiet segments.
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
    // data_mutex_ + data_cv_: lightweight sleep-interrupt mechanism for the processing
    // loop. data_mutex_ does not guard any shared state — it exists solely to satisfy
    // the condition_variable API. The CV is notified by receiver_loop when new audio
    // arrives, allowing processing_loop to wake immediately instead of fixed-interval polling.
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
    int vad_max_chunk_ms = 8000;

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
