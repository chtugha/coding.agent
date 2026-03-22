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
//
// VadCall per-call state:
//   audio_buffer:          ring of incoming float32 PCM samples (16kHz).
//   in_speech/silence_count/onset_count: FSM tracking speech vs. silence.
//   noise_floor:           adaptive estimate; updated each frame during silence.
//   frame_energies:        per-frame RMS² from the confirmed onset frame onward,
//                          used by smart-split to find a low-energy cut point.
//   energies_sample_origin: buffer position at which frame_energies[0] starts.
//   speech_sum_sq / speech_sample_count: running sum-of-squares for RMS check in
//                          send_chunk_downstream() without rescanning the buffer.
//
// CMD port (VAD base+2 = 13117): accepts PING, STATUS, SET_LOG_LEVEL,
//   SET_VAD_THRESHOLD, SET_VAD_SILENCE_MS, SET_VAD_MAX_CHUNK_MS,
//   SET_VAD_ONSET_GAP commands.
//   STATUS returns: noise_floor, threshold_mult, silence_frames, max_chunk_ms,
//   active call count, upstream/downstream state.
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
static constexpr int DISC_WARN_INTERVAL_S = 5;
static constexpr float NOISE_FLOOR_INIT = 0.00005f;
static constexpr float NOISE_FLOOR_HARD_MIN = 0.000005f;
static constexpr float NOISE_FLOOR_EMA_ALPHA = 0.05f;
static constexpr float RMS_SILENCE_GATE = 0.005f;

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
    int onset_gap = 0;
    // Adaptive noise floor estimate. Initialized above the G.711 μ-law codec noise
    // floor: G.711 silence bytes (0xFF/0x7F) decode to ±0.000885 → energy ~0.00000078.
    // We set min_floor at 0.000005 and init at 0.00005 to avoid false triggers while
    // still adapting to the actual ambient noise level.
    float noise_floor = NOISE_FLOOR_INIT;
    size_t vad_pos = 0;
    size_t speech_start = 0;
    size_t tentative_speech_start = 0;
    // Absolute buffer position where frame_energies[0] begins. Set to the onset
    // confirmation frame position so smart-split can map energy indices back to
    // buffer positions accurately (speech_start includes pre-speech context frames
    // which are not represented in frame_energies).
    size_t energies_sample_origin = 0;
    std::chrono::steady_clock::time_point speech_signal_time;
    size_t last_buffer_size = 0;
    std::chrono::steady_clock::time_point last_buffer_growth;
    std::vector<float> frame_energies;
    // Cumulative sum-of-squares for the current speech segment,
    // tracked during frame processing to avoid re-scanning in send_chunk_downstream.
    float speech_sum_sq = 0.0f;
    size_t speech_sample_count = 0;
};

class VadService {
    // vad_frame_size_: 50ms frames (800 samples @ 16kHz) — finer granularity than 100ms
    //   for detecting short pauses between words without cutting mid-phoneme.
    size_t vad_frame_size_ = 800;
    std::atomic<float> vad_threshold_mult_{2.0f};
    // Minimum energy threshold to distinguish speech from G.711 codec noise floor.
    // G.711 μ-law silence (0xFF/0x7F) decodes to ±0.000885 → energy ~0.00000078.
    // Set min_energy well above this to prevent false VAD triggers on silence.
    float vad_min_energy_ = NOISE_FLOOR_INIT;
    // vad_silence_frames_: 8 frames × 50ms = 400ms — triggers on word-boundary pauses
    //   instead of full sentence gaps. Short enough for fast turnaround, long enough
    //   to not split mid-word (German phonemes are typically <200ms).
    std::atomic<int> vad_silence_frames_{8};
    // vad_max_speech_samples_: 8s max chunk — Whisper large-v3-turbo handles 8s
    //   chunks in ~1s on Apple Silicon. Longer chunks preserve sentence boundaries.
    std::atomic<size_t> vad_max_speech_samples_{VAD_SAMPLE_RATE * 8};
    // vad_min_speech_samples_: 500ms — reject clicks and noise bursts.
    size_t vad_min_speech_samples_ = VAD_SAMPLE_RATE / 2;
    // vad_context_frames_: include 8 frames (400ms) of pre-speech context audio so the
    //   chunk captures the onset of speech including weak initial vowels/consonants.
    int vad_context_frames_ = 8;
    // vad_onset_frames_: require 2 consecutive above-threshold frames to confirm
    //   speech onset. Reduced from 3 to detect quiet vowel onsets (e.g. "Abfall-"
    //   after a comma pause) that may produce only 1 borderline-threshold frame
    //   before exceeding it. 2 frames still prevents single-frame noise spikes.
    int vad_onset_frames_ = 2;
    std::atomic<int> vad_onset_gap_tolerance_{1};
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
            vad_threshold_mult_.store(threshold_mult);
        }
        if (silence_ms > 0) {
            int frame_ms = std::max(1, (int)(vad_frame_size_ / VAD_SAMPLES_PER_MS));
            int frames = silence_ms / frame_ms;
            vad_silence_frames_.store(std::max(1, frames));
        }
        if (max_chunk_ms > 0) {
            size_t samples = static_cast<size_t>(VAD_SAMPLES_PER_MS) * max_chunk_ms;
            if (samples < vad_min_speech_samples_ * 2) {
                samples = vad_min_speech_samples_ * 2;
            }
            vad_max_speech_samples_.store(samples);
        }
        print_config();
    }

    void print_config() {
        int frame_ms = (int)(vad_frame_size_ / VAD_SAMPLES_PER_MS);
        int silence_ms = vad_silence_frames_.load() * frame_ms;
        int max_ms = (int)(vad_max_speech_samples_.load() / VAD_SAMPLES_PER_MS);
        std::cout << "VAD config: window=" << frame_ms << "ms"
                  << " threshold=" << vad_threshold_mult_.load()
                  << " silence=" << silence_ms << "ms"
                  << " max_chunk=" << max_ms << "ms"
                  << " min_chunk=" << (vad_min_speech_samples_ * 1000 / VAD_SAMPLE_RATE) << "ms"
                  << std::endl;
    }

    void set_log_level(const char* level) {
        log_fwd_.set_level(level);
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
        call.onset_gap = 0;
        call.speech_start = 0;
        call.tentative_speech_start = 0;
        call.energies_sample_origin = 0;
        call.vad_pos = 0;
        call.frame_energies.clear();
        call.speech_signaled = false;
        call.speech_sum_sq = 0.0f;
        call.speech_sample_count = 0;
        return was_signaled;
    }

    // Removes `consumed` samples from the front of the audio buffer and resets
    // all position-tracking fields. Also refreshes last_buffer_size and
    // last_buffer_growth so that the inactivity-flush timer restarts from the
    // moment of compaction rather than from when audio last arrived — this
    // prevents a premature inactivity flush on the leftover segment after a
    // max-length split where no new audio arrives for >1s.
    void compact_buffer(VadCall& call, size_t consumed) {
        if (consumed >= call.audio_buffer.size()) {
            call.audio_buffer.clear();
        } else if (consumed > 0) {
            call.audio_buffer.erase(call.audio_buffer.begin(),
                                    call.audio_buffer.begin() + static_cast<ptrdiff_t>(consumed));
        }
        call.vad_pos = 0;
        call.speech_start = 0;
        call.tentative_speech_start = 0;
        call.last_buffer_size = call.audio_buffer.size();
        call.last_buffer_growth = std::chrono::steady_clock::now();
    }

    // Finds the best split point near the max-chunk boundary by locating the
    // lowest-energy frame in the last smart_split_window_frames_ (6) frames.
    // This avoids cutting mid-word: energy dips correspond to inter-word silence
    // or weak consonant transitions — natural word boundaries.
    //
    // frame_energies[] tracks per-frame mean energy starting from the onset
    // confirmation frame (energies_sample_origin), NOT from speech_start (which
    // includes vad_context_frames_ of pre-speech audio). This offset matters
    // because the split position must be mapped back to an absolute buffer index.
    //
    // Algorithm:
    //   1. Search window: last smart_split_window_frames_ entries in frame_energies[].
    //   2. Find the frame with the minimum energy (tie → prefer later frame).
    //   3. Convert frame index → buffer position:
    //      split = energies_sample_origin + (min_idx + 1) * vad_frame_size_
    //      The +1 ensures we split AFTER the quiet frame, keeping it in the current chunk.
    //   4. Safety clamps: split must be within buffer bounds and after speech_start.
    size_t find_smart_split_point(VadCall& call, size_t max_end) {
        if (call.frame_energies.size() < 2) return max_end;

        // Search only the tail of the energy array (last 6 frames = 300ms @ 50ms/frame).
        size_t search_start = 0;
        if (call.frame_energies.size() > smart_split_window_frames_) {
            search_start = call.frame_energies.size() - smart_split_window_frames_;
        }

        // Linear scan for minimum energy frame (prefer later frame on tie via <=).
        float min_energy = call.frame_energies[search_start];
        size_t min_idx = search_start;
        for (size_t i = search_start + 1; i < call.frame_energies.size(); ++i) {
            if (call.frame_energies[i] <= min_energy) {
                min_energy = call.frame_energies[i];
                min_idx = i;
            }
        }

        // Map energy index back to absolute buffer position.
        // energies_sample_origin = buffer position where frame_energies[0] starts.
        // +1 on min_idx: split at the END of the quiet frame (keep it in this chunk).
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
        if (cmd.rfind("SET_LOG_LEVEL:", 0) == 0) {
            std::string level = cmd.substr(14);
            log_fwd_.set_level(level.c_str());
            return "OK\n";
        }
        if (cmd.rfind("SET_VAD_THRESHOLD:", 0) == 0) {
            try {
                float val = std::stof(cmd.substr(18));
                if (val >= 0.5f && val <= 10.0f) {
                    vad_threshold_mult_.store(val);
                    log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "VAD threshold set to %.2f", val);
                    return "OK\n";
                }
                return "ERROR:Value out of range (0.5-10.0)\n";
            } catch (...) { return "ERROR:Invalid value\n"; }
        }
        if (cmd.rfind("SET_VAD_SILENCE_MS:", 0) == 0) {
            try {
                int ms = std::stoi(cmd.substr(19));
                if (ms > 0) {
                    int frame_ms = std::max(1, (int)(vad_frame_size_ / VAD_SAMPLES_PER_MS));
                    int frames = std::max(1, ms / frame_ms);
                    vad_silence_frames_.store(frames);
                    log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "VAD silence set to %dms (%d frames)",
                                     frames * frame_ms, frames);
                    return "OK\n";
                }
                return "ERROR:Value must be > 0\n";
            } catch (...) { return "ERROR:Invalid value\n"; }
        }
        if (cmd.rfind("SET_VAD_MAX_CHUNK_MS:", 0) == 0) {
            try {
                int ms = std::stoi(cmd.substr(21));
                if (ms > 0) {
                    size_t samples = static_cast<size_t>(VAD_SAMPLES_PER_MS) * ms;
                    if (samples < vad_min_speech_samples_ * 2) {
                        samples = vad_min_speech_samples_ * 2;
                    }
                    vad_max_speech_samples_.store(samples);
                    int actual_ms = (int)(samples / VAD_SAMPLES_PER_MS);
                    log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "VAD max chunk set to %dms", actual_ms);
                    return "OK:" + std::to_string(actual_ms) + "ms\n";
                }
                return "ERROR:Value must be > 0\n";
            } catch (...) { return "ERROR:Invalid value\n"; }
        }
        if (cmd.rfind("SET_VAD_ONSET_GAP:", 0) == 0) {
            try {
                int val = std::stoi(cmd.substr(18));
                if (val >= 0 && val <= 5) {
                    vad_onset_gap_tolerance_.store(val);
                    log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "VAD onset gap tolerance set to %d frames", val);
                    return "OK\n";
                }
                return "ERROR:Value out of range (0-5)\n";
            } catch (...) { return "ERROR:Invalid value\n"; }
        }
        if (cmd == "STATUS") {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            size_t speech_active = 0;
            for (const auto& [id, call] : calls_) {
                std::lock_guard<std::mutex> cl(call->mutex);
                if (call->speech_signaled) speech_active++;
            }
            int frame_ms = (int)(vad_frame_size_ / VAD_SAMPLES_PER_MS);
            int silence_ms = vad_silence_frames_.load() * frame_ms;
            int max_ms = (int)(vad_max_speech_samples_.load() / VAD_SAMPLES_PER_MS);
            return "ACTIVE_CALLS:" + std::to_string(calls_.size())
                + ":SPEECH_ACTIVE:" + std::to_string(speech_active)
                + ":UPSTREAM:" + (interconnect_.upstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected")
                + ":DOWNSTREAM:" + (interconnect_.downstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected")
                + ":WINDOW_MS:" + std::to_string(frame_ms)
                + ":THRESHOLD:" + format_threshold(vad_threshold_mult_.load())
                + ":SILENCE_MS:" + std::to_string(silence_ms)
                + ":MAX_CHUNK_MS:" + std::to_string(max_ms)
                + ":ONSET_GAP:" + std::to_string(vad_onset_gap_tolerance_.load())
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
                data_cv_.wait_for(lk, std::chrono::milliseconds(has_calls ? 25 : 50));
            }

            std::vector<std::shared_ptr<VadCall>> active;
            {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                active.reserve(calls_.size());
                for (auto& p : calls_) active.push_back(p.second);
            }

            for (auto& call : active) {
                std::vector<float> to_send;
                float chunk_sum_sq = 0.0f;
                size_t chunk_sample_count = 0;
                bool needs_idle_broadcast = false;
                bool needs_active_broadcast = false;
                uint32_t call_id = call->id;
                {
                    std::lock_guard<std::mutex> lock(call->mutex);

                    // --- VAD finite state machine ---
                    // Each iteration processes one vad_frame_size_ frame (50ms = 800 samples).
                    // States:  IDLE (in_speech=false, onset_count=0)
                    //       → ONSET (in_speech=false, onset_count>0)    [energy above threshold]
                    //       → SPEECH (in_speech=true)                   [onset_count >= vad_onset_frames_]
                    //       → back to IDLE                              [silence_count > vad_silence_frames_
                    //                                                    OR speech_len > vad_max_speech_samples_]
                    // A single below-threshold frame during ONSET resets onset_count to 0 (→ IDLE).
                    const float thresh_mult = vad_threshold_mult_.load();
                    const int silence_frames = vad_silence_frames_.load();
                    const size_t max_speech = vad_max_speech_samples_.load();
                    const int onset_gap_tol = vad_onset_gap_tolerance_.load();
                    size_t pos = call->vad_pos;
                    while (pos + vad_frame_size_ <= call->audio_buffer.size()) {
                        // Compute mean energy (mean of squared samples) for this frame.
                        float energy = 0;
                        for (size_t i = 0; i < vad_frame_size_; ++i) {
                            float s = call->audio_buffer[pos + i];
                            energy += s * s;
                        }
                        energy /= static_cast<float>(vad_frame_size_);

                        // Adapt noise floor only when not in speech and no onset pending.
                        // EMA formula: nf = (1-α)·nf_old + α·energy, α=0.05.
                        //   α=0.05 → time constant ≈ 1/α = 20 frames = 1 second @ 50ms/frame.
                        //   This is slow enough to not track speech energy up, but fast enough
                        //   to adapt to changing background noise within a few seconds.
                        //   Hard floor 0.000005 prevents drift below G.711 quantization noise
                        //   (G.711 silence ≈ energy 0.00000078).
                        if (!call->in_speech && call->onset_count == 0) {
                            float nf = call->noise_floor * (1.0f - NOISE_FLOOR_EMA_ALPHA) + energy * NOISE_FLOOR_EMA_ALPHA;
                            call->noise_floor = std::max(nf, NOISE_FLOOR_HARD_MIN);
                        }
                        // Speech threshold = max(noise_floor × multiplier, min_energy).
                        // min_energy (0.00005) acts as absolute floor for very quiet environments.
                        float threshold = std::max(call->noise_floor * thresh_mult, vad_min_energy_);

                        if (energy > threshold) {
                            if (!call->in_speech) {
                                call->onset_count++;
                                call->onset_gap = 0;
                                if (call->onset_count == 1) {
                                    size_t context = vad_frame_size_ * vad_context_frames_;
                                    call->tentative_speech_start = (pos > context) ? pos - context : 0;
                                }
                                if (call->onset_count >= vad_onset_frames_) {
                                    call->in_speech = true;
                                    call->speech_start = call->tentative_speech_start;
                                    call->frame_energies.clear();
                                    call->frame_energies.reserve(max_speech / vad_frame_size_ + 1);
                                    call->speech_sum_sq = 0.0f;
                                    call->speech_sample_count = 0;
                                    call->energies_sample_origin = pos;
                                    if (vad_logging_enabled_) {
                                        log_fwd_.forward(whispertalk::LogLevel::DEBUG, call->id,
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
                            if (!call->in_speech && call->onset_count > 0) {
                                call->onset_gap++;
                                if (call->onset_gap > onset_gap_tol) {
                                    call->onset_count = 0;
                                    call->onset_gap = 0;
                                }
                            } else {
                                call->onset_count = 0;
                                call->onset_gap = 0;
                            }
                            if (call->in_speech) {
                                call->silence_count++;
                            }
                        }

                        if (call->in_speech) {
                            call->frame_energies.push_back(energy);
                            // Accumulate sum-of-squares for RMS calculation. energy is
                            // mean(s²) for this frame, so energy * frame_size = sum(s²).
                            // This avoids re-scanning the entire chunk in send_chunk_downstream
                            // just to compute RMS for the energy gate check.
                            call->speech_sum_sq += energy * static_cast<float>(vad_frame_size_);
                            call->speech_sample_count += vad_frame_size_;
                        }

                        pos += vad_frame_size_;

                        // Silence-triggered speech end: enough consecutive silent frames detected.
                        if (call->in_speech && call->silence_count >= silence_frames) {
                            size_t buf_sz = call->audio_buffer.size();
                            if (call->speech_start <= pos && pos <= buf_sz) {
                                to_send.assign(
                                    call->audio_buffer.begin() + call->speech_start,
                                    call->audio_buffer.begin() + pos);
                                chunk_sum_sq = call->speech_sum_sq;
                                chunk_sample_count = call->speech_sample_count;
                                if (vad_logging_enabled_) {
                                    double dur_ms = to_send.size() / (double)VAD_SAMPLES_PER_MS;
                                    log_fwd_.forward(whispertalk::LogLevel::DEBUG, call->id,
                                        "VAD speech_end (silence) — %zu samples (%.0fms) queued for transcription",
                                        to_send.size(), dur_ms);
                                }
                            } else {
                                // Invariant violation: speech_start is out-of-range. Recover
                                // whatever audio is available from speech_start to buf_sz.
                                log_fwd_.forward(whispertalk::LogLevel::WARN, call->id,
                                    "VAD: bounds error at silence end — speech_start=%zu pos=%zu buf=%zu — recovering partial",
                                    call->speech_start, pos, buf_sz);
                                if (call->speech_start < buf_sz) {
                                    to_send.assign(
                                        call->audio_buffer.begin() + call->speech_start,
                                        call->audio_buffer.end());
                                    chunk_sum_sq = call->speech_sum_sq;
                                    chunk_sample_count = call->speech_sample_count;
                                }
                                pos = buf_sz;
                            }
                            compact_buffer(*call, pos);
                            pos = 0;
                            needs_idle_broadcast = reset_call_state(*call);
                            break;
                        }

                        // Max-length triggered speech end with smart split.
                        // Guard against size_t underflow: if speech_start > pos (invariant
                        // violation from state corruption), reset rather than triggering
                        // spurious max-length splits from the wrap-around value.
                        if (call->in_speech && call->speech_start > pos) {
                            size_t buf_sz = call->audio_buffer.size();
                            log_fwd_.forward(whispertalk::LogLevel::WARN, call->id,
                                "VAD: speech_start(%zu) > pos(%zu) buf(%zu) — invariant violation, resetting",
                                call->speech_start, pos, buf_sz);
                            if (call->speech_start < buf_sz) {
                                to_send.assign(
                                    call->audio_buffer.begin() + call->speech_start,
                                    call->audio_buffer.end());
                                chunk_sum_sq = call->speech_sum_sq;
                                chunk_sample_count = call->speech_sample_count;
                            }
                            compact_buffer(*call, buf_sz);
                            pos = 0;
                            needs_idle_broadcast = reset_call_state(*call);
                            break;
                        }
                        size_t speech_len = pos - call->speech_start;
                        if (call->in_speech && speech_len > max_speech) {
                            size_t split = find_smart_split_point(*call, pos);
                            size_t buf_sz = call->audio_buffer.size();
                            if (call->speech_start <= split && split <= buf_sz) {
                                to_send.assign(
                                    call->audio_buffer.begin() + call->speech_start,
                                    call->audio_buffer.begin() + split);
                            } else {
                                // Recover: extract from speech_start to min(pos, buf_sz).
                                log_fwd_.forward(whispertalk::LogLevel::WARN, call->id,
                                    "VAD: bounds error at max-length split — speech_start=%zu split=%zu buf=%zu — recovering",
                                    call->speech_start, split, buf_sz);
                                split = std::min(pos, buf_sz);
                                if (call->speech_start < split) {
                                    to_send.assign(
                                        call->audio_buffer.begin() + call->speech_start,
                                        call->audio_buffer.begin() + split);
                                } else if (call->speech_start < buf_sz) {
                                    to_send.assign(
                                        call->audio_buffer.begin() + call->speech_start,
                                        call->audio_buffer.end());
                                    split = buf_sz;
                                }
                            }
                            chunk_sum_sq = call->speech_sum_sq;
                            chunk_sample_count = call->speech_sample_count;
                            if (vad_logging_enabled_) {
                                double dur_ms = to_send.size() / (double)VAD_SAMPLES_PER_MS;
                                bool was_smart = (split != pos);
                                log_fwd_.forward(whispertalk::LogLevel::DEBUG, call->id,
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
                                chunk_sum_sq = call->speech_sum_sq;
                                chunk_sample_count = call->speech_sample_count;
                                call->audio_buffer.clear();
                                call->vad_pos = 0;
                                call->last_buffer_size = 0;
                                needs_idle_broadcast = reset_call_state(*call);
                                if (vad_logging_enabled_) {
                                    double dur_ms = to_send.size() / (double)VAD_SAMPLES_PER_MS;
                                    log_fwd_.forward(whispertalk::LogLevel::DEBUG, call->id,
                                        "VAD speech_end (inactivity %dms) — %zu samples (%.0fms) queued",
                                        vad_inactivity_flush_ms_, to_send.size(), dur_ms);
                                }
                            }
                        }
                    }

                    // Trim non-speech buffer, keeping context frames for next onset detection.
                    // Only trim when accumulated excess exceeds 4x the keep threshold to avoid
                    // frequent small deque erases (each front-erase is O(elements_removed)).
                    if (!call->in_speech) {
                        size_t keep_frames = vad_context_frames_ + 2;
                        size_t keep = vad_frame_size_ * keep_frames;
                        size_t min_trim = keep * 4;
                        if (call->vad_pos > keep + min_trim) {
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
                                chunk_sum_sq = call->speech_sum_sq;
                                chunk_sample_count = call->speech_sample_count;
                                call->audio_buffer.clear();
                                call->vad_pos = 0;
                                call->last_buffer_size = 0;
                                log_fwd_.forward(whispertalk::LogLevel::WARN, call->id,
                                    "SPEECH_ACTIVE timeout (%ds) — forcing SPEECH_IDLE", speech_signal_timeout_s_);
                            }
                            needs_idle_broadcast = reset_call_state(*call);
                        }
                    }
                } // release call->mutex

                // Broadcast speech signals outside the mutex to avoid holding the
                // lock during a TCP send that could block/stall the receiver thread.
                // speech_signal_time was already set inside the lock at onset detection.
                if (needs_active_broadcast) {
                    interconnect_.broadcast_speech_signal(call_id, true);
                    log_fwd_.forward(whispertalk::LogLevel::DEBUG, call_id, "SPEECH_ACTIVE broadcast");
                }
                if (needs_idle_broadcast) {
                    interconnect_.broadcast_speech_signal(call_id, false);
                }

                if (!to_send.empty()) {
                    send_chunk_downstream(call_id, to_send, chunk_sum_sq, chunk_sample_count);
                }
            }
        }
    }

    // Validates and sends a speech chunk to Whisper via the interconnect.
    // Three-gate filter before transmission:
    //   1. Minimum length gate: reject chunks shorter than vad_min_speech_samples_ (500ms)
    //      to filter out clicks and noise bursts that passed onset detection.
    //   2. RMS energy gate: reject chunks with RMS < 0.005 (near-silence) to prevent
    //      Whisper hallucinations on effectively-silent audio that passed VAD.
    //   3. If both pass, wrap the audio in a Packet and send downstream.
    // pre_sum_sq/pre_count: pre-computed sum-of-squares from the FSM loop, avoiding
    // a full rescan of the audio buffer. Falls back to on-the-fly computation if zero.
    void send_chunk_downstream(uint32_t call_id, const std::vector<float>& audio,
                                float pre_sum_sq = 0.0f, size_t pre_count = 0) {
        // Gate 1: minimum chunk length (500ms = 8000 samples @ 16kHz).
        if (audio.size() < vad_min_speech_samples_) {
            if (vad_logging_enabled_) {
                log_fwd_.forward(whispertalk::LogLevel::DEBUG, call_id,
                    "Skipping chunk: %zu samples (%.0fms) below minimum %zu",
                    audio.size(), audio.size() / (double)VAD_SAMPLES_PER_MS, vad_min_speech_samples_);
            }
            return;
        }

        // Gate 2: RMS energy check. Use pre-computed sum-of-squares if available
        // (accumulated during frame processing), otherwise scan the audio buffer.
        float rms;
        if (pre_count > 0) {
            rms = std::sqrt(pre_sum_sq / static_cast<float>(pre_count));
        } else {
            float sum_sq = 0.0f;
            for (size_t i = 0; i < audio.size(); ++i) {
                float s = audio[i];
                sum_sq += s * s;
            }
            rms = std::sqrt(sum_sq / static_cast<float>(audio.size()));
        }

        if (rms < RMS_SILENCE_GATE) {
            if (vad_logging_enabled_) {
                log_fwd_.forward(whispertalk::LogLevel::DEBUG, call_id,
                    "Skipping low-energy chunk: RMS=%.6f (%.0fms)", rms, audio.size() / (double)VAD_SAMPLES_PER_MS);
            }
            return;
        }

        // Peak scan on contiguous vector memory (fast, ~cache-friendly).
        float peak = 0.0f;
        for (size_t i = 0; i < audio.size(); ++i) {
            float a = std::abs(audio[i]);
            if (a > peak) peak = a;
        }

        if (vad_logging_enabled_) {
            log_fwd_.forward(whispertalk::LogLevel::INFO, call_id,
                "VAD chunk -> Whisper: %zu samples (%.0fms) RMS=%.4f peak=%.4f",
                audio.size(), audio.size() / (double)VAD_SAMPLES_PER_MS, rms, peak);
        }

        whispertalk::Packet pkt(call_id, audio.data(), audio.size() * sizeof(float));
        pkt.trace.record(whispertalk::ServiceType::VAD_SERVICE, 0);
        pkt.trace.record(whispertalk::ServiceType::VAD_SERVICE, 1);
        if (!interconnect_.send_to_downstream(pkt)) {
            if (interconnect_.downstream_state() != whispertalk::ConnectionState::CONNECTED) {
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - last_disc_warn_).count() >= DISC_WARN_INTERVAL_S) {
                    log_fwd_.forward(whispertalk::LogLevel::WARN, call_id, "Whisper disconnected, discarding speech chunk");
                    last_disc_warn_ = now;
                }
            }
        }
    }

    std::shared_ptr<VadCall> get_or_create_call(uint32_t cid) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto it = calls_.find(cid);
        if (it != calls_.end()) return it->second;
        auto call = std::make_shared<VadCall>();
        call->id = cid;
        call->last_buffer_growth = std::chrono::steady_clock::now();
        calls_[cid] = call;
        std::cout << "Created VAD session for call_id " << cid << std::endl;
        log_fwd_.forward(whispertalk::LogLevel::INFO, cid, "Created VAD session");
        return call;
    }

    void handle_call_end(uint32_t call_id) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto it = calls_.find(call_id);
        if (it != calls_.end()) {
            std::cout << "Call " << call_id << " ended, closing VAD session" << std::endl;
            log_fwd_.forward(whispertalk::LogLevel::INFO, call_id, "Call ended, closing VAD session");
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
    std::chrono::steady_clock::time_point last_disc_warn_{};
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
    std::string log_level = "INFO";

    static struct option long_opts[] = {
        {"vad-window-ms",    required_argument, 0, 'w'},
        {"vad-threshold",    required_argument, 0, 't'},
        {"vad-silence-ms",   required_argument, 0, 's'},
        {"vad-max-chunk-ms", required_argument, 0, 'c'},
        {"log-level",        required_argument, 0, 'L'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "w:t:s:c:L:", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'w': vad_window_ms = atoi(optarg); break;
            case 't': vad_threshold = atof(optarg); break;
            case 's': vad_silence_ms = atoi(optarg); break;
            case 'c': vad_max_chunk_ms = atoi(optarg); break;
            case 'L': log_level = optarg; break;
            default: break;
        }
    }

    std::cout << "VAD Service starting..." << std::endl;

    VadService service;
    service.set_vad_params(vad_window_ms, vad_threshold, vad_silence_ms, vad_max_chunk_ms);
    if (!service.init()) {
        return 1;
    }
    service.set_log_level(log_level.c_str());
    service.run();

    return 0;
}
