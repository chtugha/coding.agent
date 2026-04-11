// outbound-audio-processor.cpp — 24kHz→8kHz downsampler + G.711 encoder + RTP scheduler.
//
// Pipeline position: Kokoro → [OAP] → SIP_CLIENT
//
// Receives 24kHz float32 PCM audio chunks from the Kokoro TTS service and converts
// them to 160-byte G.711 μ-law frames for transmission to the SIP client.
//
// Downsampling pipeline (24kHz → 8kHz, ratio 3:1):
//   1. Anti-aliasing FIR filter: 63-tap Hamming-windowed sinc, cutoff 3400/12000
//      (~3400Hz, preserves telephone speech band, ~43dB stopband attenuation above 4kHz).
//      Filter coefficients are computed once via get_aa_coeffs() and cached.
//      Per-call FIR history (fir_history[AA_HALF_TAPS]) preserves state across frames.
//   2. Decimate by 3: keep every 3rd filtered sample, reducing 24kHz → 8kHz.
//   3. Clip to [-1, 1] and encode each float32 sample to μ-law via the standard
//      ITU-T G.711 segment/quantization formula.
//
// Output scheduling — constant-rate 20ms timer:
//   A dedicated sender thread fires every 20ms (hardware timer based on steady_clock).
//   Each tick sends exactly 160 G.711 bytes to the SIP_CLIENT via the interconnect
//   data channel. If the TTS buffer is empty (Kokoro silent or not connected), the
//   sender emits ULAW_SILENCE frames (0xFF) to maintain RTP clock continuity.
//
// Per-call state (CallState):
//   buffer:    raw G.711 byte queue fed by the Kokoro receive thread.
//   read_pos:  logical read head; compact() reclaims memory when read_pos > COMPACT_THRESHOLD.
//   fir_history: per-call anti-aliasing filter state; avoids cross-call contamination.
//   ext[]:     pre-allocated extended buffer (history + one input batch) to avoid
//              per-frame heap allocation.
//   ulaw_buf[]: per-call scratch buffer for the downsampled/encoded output.
//
// SPEECH_ACTIVE handling:
//   When upstream signals SPEECH_ACTIVE (caller speaking), OAP clears all call buffers
//   to stop playing stale TTS audio immediately (avoids feedback over the caller).
//
// CMD port (OAP base+2 = 13152): PING, STATUS, SET_LOG_LEVEL, SAVE_WAV:ON/OFF/STATUS, SET_SAVE_WAV_DIR.
//   STATUS returns active calls, buffer lengths, upstream/downstream state.
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
#include <fstream>
#include <ctime>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#include <getopt.h>
#include "interconnect.h"

static constexpr int    AA_FILTER_TAPS    = 63;
static constexpr int    AA_HALF_TAPS      = AA_FILTER_TAPS / 2;
static constexpr double AA_CUTOFF         = 3400.0 / 12000.0;
static constexpr int    DOWNSAMPLE_RATIO  = 3;
static constexpr size_t ULAW_FRAME_SIZE   = 160;
static constexpr uint8_t ULAW_SILENCE     = 0xFF;

static constexpr int    OUTPUT_SAMPLE_RATE = 8000;
static constexpr int    INPUT_SAMPLE_RATE  = 24000;
static constexpr float  PCM_SCALE          = 32767.0f;
static constexpr int    ULAW_BIAS          = 132;  // ITU-T G.711 μ-law bias (0x84)
static constexpr int    ULAW_CLIP          = 32635;
static constexpr int    FRAME_PERIOD_MS    = ULAW_FRAME_SIZE * 1000 / OUTPUT_SAMPLE_RATE;  // 20ms
static constexpr int    STALE_CALL_TIMEOUT_S   = 60;
static constexpr int    CLEANUP_INTERVAL_S     = 10;
static constexpr size_t COMPACT_THRESHOLD      = 4096;
static constexpr int    SCHEDULER_RESYNC_THRESHOLD_MS = 100;

static const float* get_aa_coeffs() {
    static float coeffs[AA_FILTER_TAPS];
    static bool init = false;
    if (!init) {
        double sum = 0;
        for (int n = 0; n < AA_FILTER_TAPS; n++) {
            int k = n - AA_HALF_TAPS;
            double hamming = 0.54 - 0.46 * std::cos(2.0 * M_PI * n / (AA_FILTER_TAPS - 1));
            double sinc_val = (k == 0) ? AA_CUTOFF : std::sin(M_PI * AA_CUTOFF * k) / (M_PI * k);
            coeffs[n] = static_cast<float>(sinc_val * hamming);
            sum += coeffs[n];
        }
        for (int n = 0; n < AA_FILTER_TAPS; n++) coeffs[n] = static_cast<float>(coeffs[n] / sum);
        init = true;
    }
    return coeffs;
}

// High-shelf biquad for presence boost: +3 dB at 2500 Hz, 24 kHz input.
// Coefficients computed via Audio EQ Cookbook (Robert Bristow-Johnson), S=1.
// DC gain = 1.0 (no bass change), Nyquist gain = 1.413 (+3 dB).
static constexpr float PRES_B0 =  1.30829f;
static constexpr float PRES_B1 = -1.53650f;
static constexpr float PRES_B2 =  0.55845f;
static constexpr float PRES_A1 = -1.03951f;
static constexpr float PRES_A2 =  0.36942f;

// DC-blocking first-order high-pass: α = 1 − 2π·fc/fs ≈ 1 − 2π·20/24000 ≈ 0.994764
static constexpr float DC_BLOCK_ALPHA = 0.9947697f;

// Max samples per Kokoro chunk (Kokoro sends CHUNK_SAMPLES = 4800 @ 24kHz = 200ms).
// 6000 gives 25 % headroom without preallocating 5× too much per call.
static constexpr size_t OAP_MAX_PREALLOC_SAMPLES = 6000;
// Default guard window (ms) suppressing SPEECH_ACTIVE flushes immediately after TTS audio
// arrives. Configurable at runtime via SET_SIDETONE_GUARD_MS:<ms> on the CMD port.
static constexpr int SPEECH_ACTIVE_GUARD_MS_DEFAULT = 800;

static int64_t steady_now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

struct CallState {
    uint32_t id;
    std::mutex mutex;
    std::vector<uint8_t> buffer;
    size_t read_pos = 0;
    std::atomic<int64_t> last_activity_ns{0};
    // Set each time new TTS audio is received from Kokoro. Used by the SPEECH_ACTIVE
    // guard to distinguish sidetone echo (arrives <500ms after playback starts) from
    // genuine caller interruption (arrives >800ms after last audio chunk).
    std::chrono::steady_clock::time_point last_audio_received{};
    float fir_history[AA_HALF_TAPS] = {};
    bool first_chunk = true;
    float ext[AA_HALF_TAPS + OAP_MAX_PREALLOC_SAMPLES];
    uint8_t ulaw_buf[OAP_MAX_PREALLOC_SAMPLES / DOWNSAMPLE_RATIO];
    std::vector<int16_t> wav_samples;
    std::vector<int16_t> wav_input_samples;
    float pres_x1=0,pres_x2=0,pres_y1=0,pres_y2=0;
    float dc_x_prev=0, dc_y_prev=0;

    void compact() {
        if (read_pos > COMPACT_THRESHOLD && read_pos > buffer.size() / 2) {
            buffer.erase(buffer.begin(), buffer.begin() + read_pos);
            read_pos = 0;
        }
    }
};

class OutboundAudioProcessor {
public:
    OutboundAudioProcessor() : running_(true), interconnect_(whispertalk::ServiceType::OUTBOUND_AUDIO_PROCESSOR) {
    }

    void set_log_level(const char* level) {
        log_fwd_.set_level(level);
    }

    void set_save_wav_dir(const std::string& dir) {
        std::lock_guard<std::mutex> wl(save_wav_mutex_);
        save_wav_dir_ = dir;
        if (!dir.empty()) mkdir(dir.c_str(), 0755);
    }

    void enable_save_wav(bool enabled) {
        save_wav_enabled_.store(enabled);
    }

    bool init() {
        if (!interconnect_.initialize()) {
            std::cerr << "OAP: Failed to initialize interconnect" << std::endl;
            return false;
        }

        log_fwd_.init(whispertalk::FRONTEND_LOG_PORT, whispertalk::ServiceType::OUTBOUND_AUDIO_PROCESSOR);
        log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "Interconnect initialized");

        if (!interconnect_.connect_to_downstream()) {
            log_fwd_.forward(whispertalk::LogLevel::WARN, 0, "Downstream (SIP) not available yet - will auto-reconnect");
        }

        interconnect_.register_call_end_handler([this](uint32_t call_id) {
            this->handle_call_end(call_id);
        });

        interconnect_.register_speech_signal_handler([this](uint32_t call_id, bool active) {
            if (active) {
                handle_speech_active(call_id);
            }
        });

        return true;
    }

    void run() {
        std::thread receiver_thread(&OutboundAudioProcessor::receiver_loop, this);
        std::thread scheduler_thread(&OutboundAudioProcessor::scheduler_loop, this);
        std::thread cmd_thread(&OutboundAudioProcessor::command_listener_loop, this);
        
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
        int sock = cmd_sock_.exchange(-1);
        if (sock >= 0) ::close(sock);
        receiver_thread.join();
        scheduler_thread.join();
        cmd_thread.join();
        interconnect_.shutdown();
    }

    void stop() {
        running_ = false;
        int sock = cmd_sock_.exchange(-1);
        if (sock >= 0) ::close(sock);
    }

private:
    uint8_t linear_to_ulaw(int16_t pcm_in) {
        int pcm = static_cast<int>(pcm_in);
        int sign = 0;
        if (pcm < 0) { pcm = -pcm; sign = 0x80; }
        if (pcm > ULAW_CLIP) pcm = ULAW_CLIP;
        pcm += ULAW_BIAS;
        int exponent = 7;
        for (int exp_mask = 0x4000; (pcm & exp_mask) == 0 && exponent > 0; exp_mask >>= 1) exponent--;
        int mantissa = (pcm >> (exponent + 3)) & 0x0F;
        return static_cast<uint8_t>(~(sign | (exponent << 4) | mantissa));
    }

    void command_listener_loop() {
        uint16_t port = whispertalk::service_cmd_port(whispertalk::ServiceType::OUTBOUND_AUDIO_PROCESSOR);
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return;
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            log_fwd_.forward(whispertalk::LogLevel::ERROR, 0, "OAP cmd: bind port %d failed", port);
            ::close(sock);
            return;
        }
        listen(sock, 4);
        cmd_sock_.store(sock);
        log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "OAP command listener on port %d", port);
        while (running_) {
            struct pollfd pfd{sock, POLLIN, 0};
            int r = poll(&pfd, 1, 200);
            if (r <= 0) continue;
            int csock = accept(sock, nullptr, nullptr);
            if (csock < 0) continue;
            struct timeval tv{10, 0};
            setsockopt(csock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[4096];
            int n = (int)recv(csock, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                std::string cmd(buf);
                while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r'))
                    cmd.pop_back();
                std::string response = handle_command(cmd);
                send(csock, response.c_str(), response.size(), 0);
            }
            ::close(csock);
        }
    }

    std::string handle_command(const std::string& cmd) {
        if (cmd == "PING") return "PONG\n";
        if (cmd.rfind("SET_LOG_LEVEL:", 0) == 0) {
            std::string level = cmd.substr(14);
            log_fwd_.set_level(level.c_str());
            return "OK\n";
        }
        if (cmd.rfind("SET_SIDETONE_GUARD_MS:", 0) == 0) {
            try {
                int ms = std::stoi(cmd.substr(22));
                if (ms < 0) ms = 0;
                speech_active_guard_ms_ = ms;
                log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "Sidetone guard set to %dms", ms);
                return "OK\n";
            } catch (...) { return "ERROR Invalid value\n"; }
        }
        if (cmd == "SAVE_WAV:ON") {
            save_wav_enabled_.store(true);
            log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "WAV recording enabled");
            return "SAVE_WAV:ON\n";
        }
        if (cmd == "SAVE_WAV:OFF") {
            save_wav_enabled_.store(false);
            log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "WAV recording disabled");
            return "SAVE_WAV:OFF\n";
        }
        if (cmd == "SAVE_WAV:STATUS") {
            std::lock_guard<std::mutex> wl(save_wav_mutex_);
            std::string status = save_wav_enabled_.load() ? "SAVE_WAV:ON" : "SAVE_WAV:OFF";
            status += ":DIR:" + save_wav_dir_ + "\n";
            return status;
        }
        if (cmd == "PRESENCE_BOOST:ON") {
            presence_enabled_.store(true);
            log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "Presence boost enabled (+3dB @ 2.5kHz)");
            return "PRESENCE_BOOST:ON\n";
        }
        if (cmd == "PRESENCE_BOOST:OFF") {
            presence_enabled_.store(false);
            log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "Presence boost disabled");
            return "PRESENCE_BOOST:OFF\n";
        }
        if (cmd == "PRESENCE_BOOST:STATUS") {
            return std::string(presence_enabled_.load() ? "PRESENCE_BOOST:ON\n" : "PRESENCE_BOOST:OFF\n");
        }
        if (cmd.rfind("SET_SAVE_WAV_DIR:", 0) == 0) {
            std::string dir = cmd.substr(17);
            while (!dir.empty() && (dir.back() == '\n' || dir.back() == '\r' || dir.back() == ' '))
                dir.pop_back();
            {
                std::lock_guard<std::mutex> wl(save_wav_mutex_);
                save_wav_dir_ = dir;
            }
            if (!dir.empty()) mkdir(dir.c_str(), 0755);
            log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "WAV save dir set to: %s", dir.c_str());
            return "OK\n";
        }
        if (cmd == "STATUS") {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            std::string result = "ACTIVE_CALLS:" + std::to_string(calls_.size());
            result += ":DOWNSTREAM:" + std::string(
                interconnect_.downstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected");
            result += ":UPSTREAM:" + std::string(
                interconnect_.upstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected");
            result += "\n";
            return result;
        }
        if (cmd.rfind("TEST_ENCODE:", 0) == 0) {
            int freq = 400;
            int dur_ms = 500;
            size_t sep = cmd.find('|', 12);
            if (sep != std::string::npos) {
                freq = std::max(100, std::min(4000, std::atoi(cmd.substr(12, sep - 12).c_str())));
                dur_ms = std::max(100, std::min(5000, std::atoi(cmd.substr(sep + 1).c_str())));
            } else if (cmd.size() > 12) {
                freq = std::max(100, std::min(4000, std::atoi(cmd.substr(12).c_str())));
            }

            size_t in_samples = (size_t)(INPUT_SAMPLE_RATE * dur_ms / 1000);
            std::vector<float> input(in_samples);
            for (size_t i = 0; i < in_samples; i++) {
                input[i] = 0.8f * std::sin(2.0f * static_cast<float>(M_PI) * freq * i / INPUT_SAMPLE_RATE);
            }

            auto t0 = std::chrono::steady_clock::now();
            std::vector<uint8_t> ulaw = downsample_and_encode(input.data(), in_samples);
            size_t out_len = ulaw.size();
            auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - t0).count();

            double ulaw_rms = 0;
            for (auto v : ulaw) {
                double decoded = static_cast<double>(ulaw_to_linear(v));
                ulaw_rms += decoded * decoded;
            }
            ulaw_rms = std::sqrt(ulaw_rms / out_len) / 32768.0;

            return "ENCODE_RESULT:" + std::to_string(elapsed) + "us:"
                + std::to_string(in_samples) + "->" + std::to_string(out_len) + ":"
                + std::to_string(freq) + "Hz:" + std::to_string(dur_ms) + "ms:"
                + "rms=" + std::to_string(ulaw_rms) + "\n";
        }
        return "ERROR:Unknown command\n";
    }

    void downsample_and_encode_into(const float* input, size_t in_samples,
                                    CallState& state, std::vector<uint8_t>& out_ulaw) {
        const float* coeffs = get_aa_coeffs();
        size_t out_len = in_samples / DOWNSAMPLE_RATIO;

        size_t ext_len = AA_HALF_TAPS + in_samples;
        float* ext;
        std::vector<float> ext_heap;
        if (in_samples <= OAP_MAX_PREALLOC_SAMPLES) {
            ext = state.ext;
        } else {
            ext_heap.resize(ext_len);
            ext = ext_heap.data();
        }
        std::memcpy(ext, state.fir_history, AA_HALF_TAPS * sizeof(float));
        if (state.first_chunk && in_samples > 0) {
            float dc = input[0];
            for (int i = 0; i < AA_HALF_TAPS; i++) ext[i] = dc;
            state.first_chunk = false;
        }
        for (size_t i = 0; i < in_samples; i++) {
            float x = input[i];
            float y = x - state.dc_x_prev + DC_BLOCK_ALPHA * state.dc_y_prev;
            state.dc_x_prev = x;
            state.dc_y_prev = y;
            ext[AA_HALF_TAPS + i] = y;
        }
        if (presence_enabled_.load()) {
            for (size_t i = 0; i < in_samples; i++) {
                float x = ext[AA_HALF_TAPS + i];
                float y = PRES_B0*x + PRES_B1*state.pres_x1 + PRES_B2*state.pres_x2
                          - PRES_A1*state.pres_y1 - PRES_A2*state.pres_y2;
                state.pres_x2 = state.pres_x1; state.pres_x1 = x;
                state.pres_y2 = state.pres_y1; state.pres_y1 = y;
                ext[AA_HALF_TAPS + i] = std::max(-1.0f, std::min(1.0f, y));
            }
        }

        uint8_t* ulaw;
        std::vector<uint8_t> ulaw_heap;
        if (in_samples <= OAP_MAX_PREALLOC_SAMPLES) {
            ulaw = state.ulaw_buf;
        } else {
            ulaw_heap.resize(out_len);
            ulaw = ulaw_heap.data();
        }

        bool do_wav = save_wav_enabled_.load();
        size_t safe_out = (in_samples > (size_t)AA_HALF_TAPS) ?
            (in_samples - AA_HALF_TAPS) / DOWNSAMPLE_RATIO : 0;
        for (size_t i = 0; i < safe_out; i++) {
            const float* src = ext + i * DOWNSAMPLE_RATIO;
            float filtered = 0.0f;
            for (int t = 0; t < AA_FILTER_TAPS; t++)
                filtered += src[t] * coeffs[t];
            int16_t s16 = static_cast<int16_t>(std::max(-1.0f, std::min(1.0f, filtered)) * PCM_SCALE);
            ulaw[i] = linear_to_ulaw(s16);
            if (do_wav) state.wav_samples.push_back(s16);
        }
        for (size_t i = safe_out; i < out_len; i++) {
            float filtered = 0.0f;
            for (int t = 0; t < AA_FILTER_TAPS; t++) {
                int idx = static_cast<int>(i * DOWNSAMPLE_RATIO) + t;
                if (idx < static_cast<int>(ext_len))
                    filtered += ext[idx] * coeffs[t];
            }
            int16_t s16 = static_cast<int16_t>(std::max(-1.0f, std::min(1.0f, filtered)) * PCM_SCALE);
            ulaw[i] = linear_to_ulaw(s16);
            if (do_wav) state.wav_samples.push_back(s16);
        }

        if (in_samples >= (size_t)AA_HALF_TAPS) {
            std::memcpy(state.fir_history, ext + in_samples, AA_HALF_TAPS * sizeof(float));
        } else {
            size_t keep = AA_HALF_TAPS - in_samples;
            std::memmove(state.fir_history, state.fir_history + in_samples, keep * sizeof(float));
            std::memcpy(state.fir_history + keep, ext + AA_HALF_TAPS, in_samples * sizeof(float));
        }

        out_ulaw.insert(out_ulaw.end(), ulaw, ulaw + out_len);
    }

    std::vector<uint8_t> downsample_and_encode(const float* input, size_t in_samples) {
        CallState tmp;
        tmp.id = 0;
        std::vector<uint8_t> result;
        downsample_and_encode_into(input, in_samples, tmp, result);
        return result;
    }

    int16_t ulaw_to_linear(uint8_t u) {
        u = ~u;
        int sign     = (u & 0x80) ? -1 : 1;
        int exponent = (u >> 4) & 0x07;
        int mantissa = u & 0x0F;
        int sample = ((mantissa << 1) + 33) << (exponent + 2);
        sample -= ULAW_BIAS;
        return static_cast<int16_t>(sign * sample);
    }

    void receiver_loop() {
        while (running_) {
            whispertalk::Packet pkt;
            if (!interconnect_.recv_from_upstream(pkt, 100)) {
                continue;
            }

            if (!pkt.is_valid() || pkt.payload_size <= sizeof(int32_t)) {
                continue;
            }

            pkt.trace.record(whispertalk::ServiceType::OUTBOUND_AUDIO_PROCESSOR, 0);
            auto state = get_or_create_call(pkt.call_id);

            size_t audio_bytes = pkt.payload_size - sizeof(int32_t);
            if (audio_bytes == 0 || (audio_bytes % sizeof(float)) != 0) continue;
            size_t sample_count = audio_bytes / sizeof(float);
            const float* pcm_buf = reinterpret_cast<const float*>(pkt.payload.data() + sizeof(int32_t));

            std::lock_guard<std::mutex> lock(state->mutex);
            auto now = std::chrono::steady_clock::now();
            state->last_activity_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count(), std::memory_order_relaxed);
            state->last_audio_received = now;
            if (save_wav_enabled_.load()) {
                for (size_t i = 0; i < sample_count; i++) {
                    float s = std::max(-1.0f, std::min(1.0f, pcm_buf[i]));
                    state->wav_input_samples.push_back(static_cast<int16_t>(s * PCM_SCALE));
                }
            }
            downsample_and_encode_into(pcm_buf, sample_count, *state, state->buffer);
        }
    }

    void scheduler_loop() {
        auto next = std::chrono::steady_clock::now();
        auto last_cleanup = std::chrono::steady_clock::now();
        while (running_) {
            auto now = std::chrono::steady_clock::now();

            // Resync if the schedule has drifted more than SCHEDULER_RESYNC_THRESHOLD_MS behind
            // real time (e.g. after a system sleep or heavy load spike). Without this, the
            // scheduler would fire in a tight burst to "catch up", flooding the SIP client
            // with back-to-back frames and causing audible distortion.
            if (next < now - std::chrono::milliseconds(SCHEDULER_RESYNC_THRESHOLD_MS)) {
                log_fwd_.forward(whispertalk::LogLevel::WARN, 0,
                    "Scheduler drift detected — resyncing timer (was %lldms behind)",
                    (long long)std::chrono::duration_cast<std::chrono::milliseconds>(now - next).count());
                next = now;
            }

            if (now - last_cleanup > std::chrono::seconds(CLEANUP_INTERVAL_S)) {
                last_cleanup = now;
                std::lock_guard<std::mutex> lock(calls_mutex_);
                int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
                for (auto it = calls_.begin(); it != calls_.end(); ) {
                    auto age = (now_ns - it->second->last_activity_ns.load(std::memory_order_relaxed)) / 1000000000LL;
                    if (age > STALE_CALL_TIMEOUT_S) {
                        log_fwd_.forward(whispertalk::LogLevel::WARN, it->first, "Stale call removed after %lds idle", age);
                        it = calls_.erase(it);
                    } else {
                        ++it;
                    }
                }
            }

            std::vector<std::shared_ptr<CallState>> active;
            {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                for (auto& p : calls_) active.push_back(p.second);
            }

            for (auto& state : active) {
                uint8_t frame[ULAW_FRAME_SIZE];
                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    size_t avail = state->buffer.size() - state->read_pos;
                    if (avail >= ULAW_FRAME_SIZE) {
                        memcpy(frame, state->buffer.data() + state->read_pos, ULAW_FRAME_SIZE);
                        state->read_pos += ULAW_FRAME_SIZE;
                        state->compact();
                    } else {
                        memset(frame, ULAW_SILENCE, ULAW_FRAME_SIZE);
                        if (avail == 0)
                            state->first_chunk = true;
                    }
                }

                whispertalk::Packet pkt(state->id, frame, ULAW_FRAME_SIZE);
                pkt.trace.record(whispertalk::ServiceType::OUTBOUND_AUDIO_PROCESSOR, 1);
                if (!interconnect_.send_to_downstream(pkt)) {
                    if (interconnect_.downstream_state() != whispertalk::ConnectionState::CONNECTED) {
                        log_fwd_.forward(whispertalk::LogLevel::WARN, state->id, "SIP disconnected, discarding audio");
                    }
                }
            }

            next += std::chrono::milliseconds(FRAME_PERIOD_MS);
            std::this_thread::sleep_until(next);
        }
    }

    std::shared_ptr<CallState> get_or_create_call(uint32_t cid) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto it = calls_.find(cid);
        if (it != calls_.end()) return it->second;
        auto state = std::make_shared<CallState>();
        state->id = cid;
        state->last_activity_ns.store(steady_now_ns(), std::memory_order_relaxed);
        calls_[cid] = state;
        log_fwd_.forward(whispertalk::LogLevel::INFO, cid, "Created outbound audio state");
        return state;
    }

    void handle_speech_active(uint32_t call_id) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto it = calls_.find(call_id);
        if (it == calls_.end()) return;
        auto& state = it->second;
        std::lock_guard<std::mutex> sl(state->mutex);

        // Suppress flushes that arrive within SPEECH_ACTIVE_GUARD_MS of new TTS audio.
        // PBX sidetone (loopback of outgoing audio back into the RTP stream) causes
        // spurious SPEECH_ACTIVE signals within 200-500ms of playback start.
        // Guard at 800ms safely filters sidetone while allowing faster barge-in.
        auto now = std::chrono::steady_clock::now();
        auto ms_since_audio = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - state->last_audio_received).count();
        int guard_ms = speech_active_guard_ms_.load();
        if (guard_ms > 0 && ms_since_audio < guard_ms) {
            log_fwd_.forward(whispertalk::LogLevel::DEBUG, call_id,
                "SPEECH_ACTIVE suppressed — sidetone guard active (%ldms since last TTS audio, guard=%dms)",
                (long)ms_since_audio, guard_ms);
            return;
        }

        size_t flushed = state->buffer.size() - state->read_pos;
        state->buffer.clear();
        state->read_pos = 0;
        std::memset(state->fir_history, 0, sizeof(state->fir_history));
        state->first_chunk = true;
        state->dc_x_prev = 0.0f;
        state->dc_y_prev = 0.0f;
        state->pres_x1 = state->pres_x2 = state->pres_y1 = state->pres_y2 = 0.0f;
        log_fwd_.forward(whispertalk::LogLevel::WARN, call_id, "SPEECH_ACTIVE — flushed %zu bytes of audio buffer", flushed);
    }

    void write_wav_file(uint32_t call_id, const std::vector<int16_t>& samples, const std::string& dir,
                        uint32_t sample_rate, const std::string& suffix, const std::string& ts) {
        if (samples.empty()) return;
        std::string fname = "oap_call_" + std::to_string(call_id) + "_" + ts;
        if (!suffix.empty()) fname += "_" + suffix;
        fname += ".wav";
        std::string path = dir + "/" + fname;

        std::ofstream f(path, std::ios::binary);
        if (!f) {
            log_fwd_.forward(whispertalk::LogLevel::ERROR, call_id, "WAV write failed: cannot open %s", path.c_str());
            return;
        }

        uint16_t num_channels = 1;
        uint16_t bits_per_sample = 16;
        uint32_t data_size = static_cast<uint32_t>(samples.size() * sizeof(int16_t));
        uint32_t byte_rate = sample_rate * num_channels * (bits_per_sample / 8);
        uint16_t block_align = num_channels * (bits_per_sample / 8);
        uint32_t chunk_size = 36 + data_size;

        auto write32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
        auto write16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };

        f.write("RIFF", 4);
        write32(chunk_size);
        f.write("WAVE", 4);
        f.write("fmt ", 4);
        write32(16);
        write16(1);
        write16(num_channels);
        write32(sample_rate);
        write32(byte_rate);
        write16(block_align);
        write16(bits_per_sample);
        f.write("data", 4);
        write32(data_size);
        f.write(reinterpret_cast<const char*>(samples.data()), data_size);
        f.close();

        log_fwd_.forward(whispertalk::LogLevel::INFO, call_id,
            "WAV saved: %s (%zu samples, %.2fs)", path.c_str(), samples.size(),
            (double)samples.size() / sample_rate);
    }

    void handle_call_end(uint32_t call_id) {
        std::shared_ptr<CallState> state_copy;
        std::string wav_dir;
        bool wav_enabled;
        {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            auto it = calls_.find(call_id);
            if (it != calls_.end()) {
                log_fwd_.forward(whispertalk::LogLevel::INFO, call_id, "Call ended, cleaning up outbound audio");
                state_copy = it->second;
                calls_.erase(it);
            }
        }
        {
            std::lock_guard<std::mutex> wl(save_wav_mutex_);
            wav_enabled = save_wav_enabled_.load();
            wav_dir = save_wav_dir_;
        }
        if (state_copy && wav_enabled && !wav_dir.empty()) {
            std::vector<int16_t> out_samples, in_samples;
            {
                std::lock_guard<std::mutex> sl(state_copy->mutex);
                out_samples = std::move(state_copy->wav_samples);
                in_samples = std::move(state_copy->wav_input_samples);
            }
            std::time_t now = std::time(nullptr);
            char ts_buf[32];
            struct tm tm_buf{};
            localtime_r(&now, &tm_buf);
            std::strftime(ts_buf, sizeof(ts_buf), "%Y%m%d_%H%M%S", &tm_buf);
            std::string ts(ts_buf);
            write_wav_file(call_id, out_samples, wav_dir, 8000, "output", ts);
            write_wav_file(call_id, in_samples, wav_dir, 24000, "input", ts);
        }
    }

    std::atomic<bool> running_;
    std::atomic<int> cmd_sock_{-1};
    // Sidetone guard: SPEECH_ACTIVE flushes are suppressed if TTS audio was
    // received within this many ms. Configurable via SET_SIDETONE_GUARD_MS.
    std::atomic<int> speech_active_guard_ms_{SPEECH_ACTIVE_GUARD_MS_DEFAULT};
    std::mutex calls_mutex_;
    std::map<uint32_t, std::shared_ptr<CallState>> calls_;
    whispertalk::InterconnectNode interconnect_;
    whispertalk::LogForwarder log_fwd_;
    std::atomic<bool> save_wav_enabled_{false};
    std::mutex save_wav_mutex_;
    std::string save_wav_dir_;
    std::atomic<bool> presence_enabled_{false};
};

int main(int argc, char** argv) {
    std::string log_level = "INFO";
    std::string save_wav_dir;

    static struct option long_opts[] = {
        {"log-level",     required_argument, 0, 'L'},
        {"save-wav-dir",  required_argument, 0, 'W'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "L:W:", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'L': log_level = optarg; break;
            case 'W': save_wav_dir = optarg; break;
            default: break;
        }
    }

    OutboundAudioProcessor proc;
    if (!proc.init()) {
        return 1;
    }
    proc.set_log_level(log_level.c_str());
    if (!save_wav_dir.empty()) {
        proc.set_save_wav_dir(save_wav_dir);
        proc.enable_save_wav(true);
    }
    proc.run();
    return 0;
}
