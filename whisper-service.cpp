// Whisper Service — Automatic Speech Recognition using whisper.cpp
//
// Pipeline position: VAD → [Whisper] → LLaMA
//
// Receives pre-segmented float32 PCM audio chunks (16kHz) from the VAD Service.
// Each chunk is a complete speech segment (typically 1-8 seconds) already validated
// by VAD — no voice activity detection is performed here.
//
// Decoding: GREEDY (not beam search). On short 2-8s segments, greedy is ~3-5x faster
// than beam_size=5 with negligible accuracy difference. Temperature fallback handles errors.
//
// No audio normalization — whisper-cli doesn't normalize and produces correct
// transcriptions on G.711 round-tripped audio. Normalization changes signal
// characteristics and confuses the model.
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

static constexpr int WSP_SAMPLE_RATE = 16000;
static constexpr int WSP_SAMPLES_PER_MS = WSP_SAMPLE_RATE / 1000;

class WhisperService {
    static constexpr size_t MAX_BUFFER_PACKETS = 64;
    size_t min_speech_samples_ = WSP_SAMPLE_RATE / 2;
    std::atomic<bool> hallucination_filter_enabled_{false};

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

    bool init() {
        if (!interconnect_.initialize()) {
            std::cerr << "Failed to initialize interconnect" << std::endl;
            return false;
        }

        std::cout << "Interconnect initialized (peer-to-peer)" << std::endl;

        log_fwd_.init(whispertalk::FRONTEND_LOG_PORT, whispertalk::ServiceType::WHISPER_SERVICE);

        if (!interconnect_.connect_to_downstream()) {
            std::cout << "Downstream (LLaMA) not available yet - will auto-reconnect" << std::endl;
        }

        interconnect_.register_call_end_handler([this](uint32_t call_id) {
            this->handle_call_end(call_id);
        });

        return true;
    }

    void set_log_level(const char* level) {
        log_fwd_.set_level(level);
    }

    void run() {
        std::thread receiver_thread(&WhisperService::receiver_loop, this);
        std::thread cmd_thread(&WhisperService::command_listener_loop, this);

        while (running_ && g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        running_ = false;

        int sock = cmd_sock_.exchange(-1);
        if (sock >= 0) ::close(sock);
        receiver_thread.join();
        cmd_thread.join();
        interconnect_.shutdown();
    }

private:
    void command_listener_loop() {
        uint16_t port = whispertalk::service_cmd_port(whispertalk::ServiceType::WHISPER_SERVICE);
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return;
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Whisper cmd: bind port " << port << " failed" << std::endl;
            ::close(sock);
            return;
        }
        listen(sock, 4);
        cmd_sock_.store(sock);
        std::cout << "Whisper command listener on port " << port << std::endl;
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
                std::string response = handle_whisper_command(cmd);
                send(csock, response.c_str(), response.size(), 0);
            }
            ::close(csock);
        }
    }

    std::string handle_whisper_command(const std::string& cmd) {
        if (cmd == "PING") return "PONG\n";
        if (cmd.rfind("SET_LOG_LEVEL:", 0) == 0) {
            std::string level = cmd.substr(14);
            log_fwd_.set_level(level.c_str());
            return "OK\n";
        }
        if (cmd == "HALLUCINATION_FILTER:ON") {
            hallucination_filter_enabled_.store(true);
            log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "Hallucination filter enabled");
            return "OK:HALLUCINATION_FILTER:ON\n";
        }
        if (cmd == "HALLUCINATION_FILTER:OFF") {
            hallucination_filter_enabled_.store(false);
            log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "Hallucination filter disabled");
            return "OK:HALLUCINATION_FILTER:OFF\n";
        }
        if (cmd == "HALLUCINATION_FILTER:STATUS") {
            return std::string("HALLUCINATION_FILTER:") + (hallucination_filter_enabled_.load() ? "ON" : "OFF") + "\n";
        }
        if (cmd == "STATUS") {
            std::string model_file = model_path_;
            size_t slash = model_file.rfind('/');
            if (slash != std::string::npos) model_file = model_file.substr(slash + 1);
            return "MODEL:" + model_file
                + ":UPSTREAM:" + (interconnect_.upstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected")
                + ":DOWNSTREAM:" + (interconnect_.downstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected")
                + ":HALLUCINATION_FILTER:" + (hallucination_filter_enabled_.load() ? "ON" : "OFF")
                + "\n";
        }
        return "ERROR:Unknown command\n";
    }

    // Receives pre-segmented audio chunks from VAD and transcribes each immediately.
    // Each packet from VAD is a complete speech segment ready for transcription.
    void receiver_loop() {
        while (running_ && g_running) {
            whispertalk::Packet pkt;
            if (!interconnect_.recv_from_upstream(pkt, 100)) {
                continue;
            }

            if (!pkt.is_valid() || pkt.payload_size == 0 || (pkt.payload_size % sizeof(float)) != 0) {
                continue;
            }

            size_t sample_count = pkt.payload_size / sizeof(float);
            const float* samples = reinterpret_cast<const float*>(pkt.payload.data());
            transcribe_and_send(pkt.call_id, samples, sample_count);
        }
    }

    static const char* hallucination_patterns_[];

    bool is_hallucination(const std::string& text) {
        if (text.empty()) return false;
        size_t start = 0, end = text.size();
        while (start < end && text[start] == ' ') start++;
        while (end > start && text[end - 1] == ' ') end--;
        if (end - start < 3) return true;
        std::string t = text.substr(start, end - start);

        std::string core = t;
        while (!core.empty() && (core.back() == '.' || core.back() == '!' ||
               core.back() == '?' || core.back() == ',')) {
            core.pop_back();
        }
        while (!core.empty() && core.back() == ' ') core.pop_back();

        for (int i = 0; hallucination_patterns_[i]; ++i) {
            if (core == hallucination_patterns_[i]) return true;
        }
        if (t.size() > 20) {
            std::string half = t.substr(0, t.size() / 2);
            if (t.find(half, half.size()) != std::string::npos) return true;
        }
        return false;
    }

    std::string strip_trailing_hallucinations(const std::string& text) {
        std::string result = text;
        bool changed = true;
        while (changed) {
            changed = false;
            std::string trimmed = result;
            while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '.' ||
                   trimmed.back() == '!' || trimmed.back() == '?'))
                trimmed.pop_back();

            for (int i = 0; hallucination_patterns_[i]; ++i) {
                std::string pat = hallucination_patterns_[i];
                if (trimmed.size() > pat.size() + 2) {
                    size_t boundary = trimmed.size() - pat.size();
                    std::string suffix = trimmed.substr(boundary);
                    if (suffix == pat && (boundary == 0 || trimmed[boundary - 1] == ' ')) {
                        result = trimmed.substr(0, boundary);
                        while (!result.empty() && (result.back() == ' ' || result.back() == ','))
                            result.pop_back();
                        changed = true;
                        break;
                    }
                }
            }

            static const char* suffix_patterns[] = {
                "Untertitelung des ZDF, 2020", "Untertitelung des ZDF, 2021",
                "Untertitelung des ZDF, 2022", "Untertitelung des ZDF, 2023",
                "Untertitelung des ZDF, 2024", "Untertitelung des ZDF, 2025",
                "Untertitelung des ZDF", "Untertitelung",
                "Untertitel der", "Untertitel von",
                "des ZDF, 2020", "des ZDF",
                "Vielen Dank fürs Zuschauen",
                "Vielen Dank für die Aufmerksamkeit",
                "Danke fürs Zuschauen",
                "Hier geht's", "Hier gehts", "Mehr dazu",
                nullptr
            };
            for (int i = 0; suffix_patterns[i]; ++i) {
                std::string pat = suffix_patterns[i];
                if (trimmed.size() > pat.size() + 2) {
                    size_t boundary = trimmed.size() - pat.size();
                    std::string suffix = trimmed.substr(boundary);
                    if (suffix == pat && (boundary == 0 || trimmed[boundary - 1] == ' ')) {
                        result = trimmed.substr(0, boundary);
                        while (!result.empty() && (result.back() == ' ' || result.back() == ','))
                            result.pop_back();
                        changed = true;
                        break;
                    }
                }
            }
        }

        while (!result.empty() && result.back() == ' ') result.pop_back();
        return result;
    }

    // Transcribes a speech chunk and sends the text downstream to LLaMA.
    // Uses GREEDY decoding for speed — on short chunks the accuracy difference vs beam search
    // is negligible, but inference is 3-5x faster. audio_ctx=0 uses full encoder context
    // matching whisper-cli default behavior.
    void transcribe_and_send(uint32_t call_id, const float* audio, size_t audio_len) {
        if (audio_len < min_speech_samples_) {
            log_fwd_.forward(whispertalk::LogLevel::DEBUG, call_id,
                "Skipping chunk: %zu samples (%.0fms) below minimum %zu",
                audio_len, audio_len / (double)WSP_SAMPLES_PER_MS, min_speech_samples_);
            return;
        }

        float sum_sq = 0.0f, peak = 0.0f;
        for (size_t i = 0; i < audio_len; ++i) {
            float s = audio[i];
            sum_sq += s * s;
            float a = std::abs(s);
            if (a > peak) peak = a;
        }
        float rms = std::sqrt(sum_sq / audio_len);

        log_fwd_.forward(whispertalk::LogLevel::DEBUG, call_id,
            "Audio chunk: %zu samples (%.0fms) RMS=%.4f peak=%.4f",
            audio_len, audio_len / (double)WSP_SAMPLES_PER_MS, rms, peak);

        if (rms < 0.005f) {
            log_fwd_.forward(whispertalk::LogLevel::DEBUG, call_id,
                "Skipping low-energy chunk: RMS=%.6f (%.0fms)", rms, audio_len / (double)WSP_SAMPLES_PER_MS);
            return;
        }

        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wparams.language = language_.c_str();
        wparams.n_threads = 8;
        wparams.no_timestamps = true;
        wparams.single_segment = false;
        wparams.greedy.best_of = 1;
        wparams.temperature = 0.0f;
        wparams.temperature_inc = 0.2f;
        wparams.entropy_thold = 2.8f;
        wparams.logprob_thold = -1.0f;
        wparams.no_speech_thold = 0.9f;

        wparams.audio_ctx = 0;

        wparams.no_context = true;
        wparams.initial_prompt = nullptr;

        auto t0 = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lock(whisper_mutex_);
        int result = whisper_full(ctx_, wparams, audio, (int)audio_len);

        if (result == 0) {
            auto t1 = std::chrono::steady_clock::now();
            auto whisper_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
            int n_segments = whisper_full_n_segments(ctx_);
            std::string text;
            for (int i = 0; i < n_segments; ++i) {
                text += whisper_full_get_segment_text(ctx_, i);
            }
            if (!text.empty()) {
                if (hallucination_filter_enabled_.load(std::memory_order_relaxed)) {
                    if (is_hallucination(text)) {
                        log_fwd_.forward(whispertalk::LogLevel::DEBUG, call_id, "Hallucination filtered: %s", text.c_str());
                        std::cout << "[" << call_id << "] Hallucination filtered: " << text << std::endl;
                        return;
                    }

                    std::string cleaned = strip_trailing_hallucinations(text);
                    if (cleaned.empty() || cleaned.size() < 3) {
                        log_fwd_.forward(whispertalk::LogLevel::DEBUG, call_id, "Stripped to empty: %s", text.c_str());
                        return;
                    }
                    if (cleaned != text) {
                        log_fwd_.forward(whispertalk::LogLevel::DEBUG, call_id, "Stripped hallucination tail: '%s' -> '%s'", text.c_str(), cleaned.c_str());
                    }
                    text = cleaned;
                }

                double audio_dur_ms = audio_len / (double)WSP_SAMPLES_PER_MS;
                double rtf = whisper_ms / audio_dur_ms;
                std::cout << "[" << call_id << "] Transcription (" << whisper_ms << "ms, RTF=" 
                          << std::fixed << std::setprecision(2) << rtf << "): " << text << std::endl;
                log_fwd_.forward(whispertalk::LogLevel::INFO, call_id, "Transcription (%lldms): %s", whisper_ms, text.c_str());

                whispertalk::Packet pkt(call_id, text.c_str(), text.length());
                pkt.trace.record(whispertalk::ServiceType::WHISPER_SERVICE, 0);
                pkt.trace.record(whispertalk::ServiceType::WHISPER_SERVICE, 1);
                // Snapshot buffer, then send outside the lock to avoid holding
                // buffer_mutex_ during blocking network I/O.
                std::vector<whispertalk::Packet> to_drain;
                {
                    std::lock_guard<std::mutex> buf_lock(buffer_mutex_);
                    to_drain.assign(std::make_move_iterator(buffered_packets_.begin()),
                                   std::make_move_iterator(buffered_packets_.end()));
                    buffered_packets_.clear();
                }
                // Drain old buffered packets first (preserves chronological order).
                size_t drained = 0;
                for (auto& p : to_drain) {
                    if (!interconnect_.send_to_downstream(p)) break;
                    ++drained;
                }
                {
                    std::lock_guard<std::mutex> buf_lock(buffer_mutex_);
                    // Move unsent remainder back into the deque.
                    for (size_t i = drained; i < to_drain.size(); ++i) {
                        buffered_packets_.push_back(std::move(to_drain[i]));
                    }
                    // If drain failed (buffer still has entries), queue current packet too.
                    if (!buffered_packets_.empty()) {
                        if (buffered_packets_.size() >= MAX_BUFFER_PACKETS) {
                            buffered_packets_.pop_front();
                        }
                        buffered_packets_.push_back(pkt);
                        std::cout << "[" << call_id << "] LLaMA disconnected, buffering transcription ("
                                  << buffered_packets_.size() << " in queue)" << std::endl;
                        log_fwd_.forward(whispertalk::LogLevel::WARN, call_id,
                            "LLaMA disconnected, buffering transcription (%zu in queue)",
                            buffered_packets_.size());
                        return;
                    }
                }
                // Buffer empty — send current packet directly.
                if (!interconnect_.send_to_downstream(pkt)) {
                    std::lock_guard<std::mutex> buf_lock(buffer_mutex_);
                    buffered_packets_.push_back(pkt);
                    std::cout << "[" << call_id << "] LLaMA disconnected, buffering transcription ("
                              << buffered_packets_.size() << " in queue)" << std::endl;
                    log_fwd_.forward(whispertalk::LogLevel::WARN, call_id,
                        "LLaMA disconnected, buffering transcription (%zu in queue)",
                        buffered_packets_.size());
                }
            }
        } else {
            std::cerr << "[" << call_id << "] Whisper transcription failed" << std::endl;
            log_fwd_.forward(whispertalk::LogLevel::ERROR, call_id, "Whisper transcription failed");
        }
    }

    void handle_call_end(uint32_t call_id) {
        std::cout << "Call " << call_id << " ended" << std::endl;
        log_fwd_.forward(whispertalk::LogLevel::INFO, call_id, "Call ended");
    }

    std::atomic<bool> running_;
    std::atomic<int> cmd_sock_{-1};
    std::string model_path_;
    std::string language_;
    struct whisper_context* ctx_ = nullptr;
    std::mutex whisper_mutex_;
    whispertalk::InterconnectNode interconnect_;
    whispertalk::LogForwarder log_fwd_;
    
    std::mutex buffer_mutex_;
    std::deque<whispertalk::Packet> buffered_packets_;
};

const char* WhisperService::hallucination_patterns_[] = {
    "Untertitel",
    "Bis zum nächsten Mal",
    "Copyright",
    "www.",
    "Amara.org",
    "Danke fürs Zuschauen",
    "Vielen Dank fürs Zuschauen",
    "Vielen Dank für die Aufmerksamkeit",
    "Musik",
    "Applaus",
    "[Musik]",
    "[Applaus]",
    "MwSt",
    nullptr
};

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    std::string model_path;
    std::string language = "de";
    std::string log_level = "INFO";

    static struct option long_opts[] = {
        {"language",  required_argument, 0, 'l'},
        {"model",     required_argument, 0, 'm'},
        {"log-level", required_argument, 0, 'L'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "l:m:L:", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'l': language = optarg; break;
            case 'm': model_path = optarg; break;
            case 'L': log_level = optarg; break;
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
        if (!service.init()) {
            return 1;
        }
        service.set_log_level(log_level.c_str());
        service.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
