// whisper-service.cpp — Automatic Speech Recognition (ASR) stage.
//
// Pipeline position: VAD → [Whisper] → LLaMA
//
// Receives pre-segmented float32 PCM audio chunks (16kHz) from the VAD Service.
// Each chunk is a complete speech segment (typically 1-8 seconds) already validated
// by VAD — no voice activity detection is performed here.
//
// Decoding strategy:
//   BEAM_SEARCH (beam_size=5, patience=1.0). On G.711-degraded telephony audio,
//   beam search significantly outperforms greedy decoding — it reduces word-level
//   errors on codec-distorted speech (e.g. "Betriebssystem" misread as "Betriebsstimm").
//   Temperature fallback (temp_inc=0.2) retries on high entropy or failed compression
//   ratio checks.
//
//   Telephony-optimized parameters:
//     no_speech_thold=0.6  — aggressively rejects silence segments on G.711 audio.
//     entropy_thold=2.4    — rejects noisy/uncertain transcriptions from codec artifacts.
//     initial_prompt       — neutral German sentence to anchor language detection.
//
// Hallucination filter (default OFF, runtime-toggleable via cmd port or frontend UI):
//   Exact-match list of known Whisper hallucination strings (e.g. "Untertitel",
//   "Copyright", "Musik"). Trailing hallucination stripping removes known suffixes
//   with word-boundary-aware matching. Repetitive-text detection also enabled.
//   Toggle: "HALLUCINATION_FILTER:ON/OFF" on cmd port 13122.
//
// RMS energy check: Rejects chunks with RMS < 0.005 as near-silence to prevent
//   hallucinations on effectively-silent frames passed by VAD.
//
// Packet buffering: If LLaMA is disconnected, buffers up to MAX_BUFFER_PACKETS (64)
//   transcription packets and drains them atomically on reconnect.
//
// Audio normalization: NOT applied. whisper-cli defaults to no normalization and
//   produces correct transcriptions on G.711 round-tripped audio. Normalization
//   changes signal characteristics and degrades model accuracy.
//
// CMD port (Whisper base+2 = 13122): PING, STATUS, HALLUCINATION_FILTER:ON/OFF/STATUS,
//   SET_LOG_LEVEL commands. STATUS returns model name, filter state, connection state.
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
#include <cmath>
#include <signal.h>
#include <getopt.h>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include "interconnect.h"
#include "whisper-cpp/include/whisper.h"

static std::atomic<bool> g_running{true};
static void sig_handler(int) { g_running = false; }

static constexpr int WSP_SAMPLE_RATE     = 16000;
static constexpr int WSP_SAMPLES_PER_MS  = WSP_SAMPLE_RATE / 1000;
static constexpr int WSP_N_THREADS       = 4;
static constexpr float RMS_SILENCE_THOLD = 0.005f;
static constexpr float WSP_TEMP_INC      = 0.2f;
static constexpr float WSP_ENTROPY_THOLD = 2.4f;
static constexpr float WSP_NO_SPEECH_THOLD = 0.6f;
static constexpr int MIN_TEXT_LEN        = 3;
static constexpr int REPETITION_MIN_LEN  = 20;
static constexpr int CMD_POLL_TIMEOUT_MS = 200;
static constexpr int CMD_RECV_TIMEOUT_S  = 10;
static constexpr int CMD_BUF_SIZE        = 4096;

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
        if (listen(sock, 4) < 0) {
            std::cerr << "Whisper cmd: listen failed" << std::endl;
            ::close(sock);
            return;
        }
        cmd_sock_.store(sock);
        std::cout << "Whisper command listener on port " << port << std::endl;
        while (running_ && g_running) {
            struct pollfd pfd{sock, POLLIN, 0};
            if (poll(&pfd, 1, CMD_POLL_TIMEOUT_MS) <= 0) continue;
            int csock = accept(sock, nullptr, nullptr);
            if (csock < 0) continue;
            struct timeval tv{CMD_RECV_TIMEOUT_S, 0};
            setsockopt(csock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[CMD_BUF_SIZE];
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

    // Checks if the entire transcription is a known Whisper hallucination.
    // Steps: (1) trim whitespace, (2) reject very short strings (<3 chars) as noise,
    // (3) strip trailing punctuation to get "core" text, (4) exact-match against
    // hallucination_patterns_[] (common Whisper artifacts like "Untertitel", "Copyright"),
    // (5) detect repetitive text (first half repeated in second half → hallucination loop).
    bool is_hallucination(const std::string& text) {
        if (text.empty()) return false;
        // Trim leading/trailing whitespace.
        size_t start = 0, end = text.size();
        while (start < end && text[start] == ' ') start++;
        while (end > start && text[end - 1] == ' ') end--;
        if (end - start < (size_t)MIN_TEXT_LEN) return true;
        std::string t = text.substr(start, end - start);

        // Strip trailing punctuation to normalize "Untertitel." → "Untertitel".
        std::string core = t;
        while (!core.empty() && (core.back() == '.' || core.back() == '!' ||
               core.back() == '?' || core.back() == ',')) {
            core.pop_back();
        }
        while (!core.empty() && core.back() == ' ') core.pop_back();

        // Exact-match against known hallucination strings.
        for (int i = 0; hallucination_patterns_[i]; ++i) {
            if (core == hallucination_patterns_[i]) return true;
        }
        // Repetition detector: if the first half of the text appears again in the
        // second half, it's a decoder loop artifact (e.g., "Danke Danke Danke...").
        if (t.size() > (size_t)REPETITION_MIN_LEN) {
            std::string half = t.substr(0, t.size() / 2);
            if (t.find(half, half.size()) != std::string::npos) return true;
        }
        return false;
    }

    // Iteratively strips known hallucination suffixes from the end of a transcription.
    // Whisper sometimes appends spurious phrases to valid text (e.g., "Guten Tag. Untertitel").
    // This function repeatedly checks if the text ends with a known pattern at a word
    // boundary and removes it. The loop continues until no more suffixes match.
    // Two pattern lists are checked: hallucination_patterns_[] (single words like
    // "Untertitel") and suffix_patterns[] (multi-word phrases like "Untertitelung des ZDF").
    std::string strip_trailing_hallucinations(const std::string& text) {
        std::string result = text;
        bool changed = true;
        while (changed) {
            changed = false;
            // Strip trailing punctuation/whitespace before matching.
            std::string trimmed = result;
            while (!trimmed.empty() && (trimmed.back() == ' ' || trimmed.back() == '.' ||
                   trimmed.back() == '!' || trimmed.back() == '?'))
                trimmed.pop_back();

            // Pass 1: check against single-word hallucination patterns.
            for (int i = 0; hallucination_patterns_[i]; ++i) {
                std::string pat = hallucination_patterns_[i];
                if (trimmed.size() > pat.size() + 2) {
                    size_t boundary = trimmed.size() - pat.size();
                    std::string suffix = trimmed.substr(boundary);
                    // Word-boundary check: the character before the suffix must be a space
                    // (or the suffix starts at position 0), ensuring we don't match mid-word.
                    if (suffix == pat && (boundary == 0 || trimmed[boundary - 1] == ' ')) {
                        result = trimmed.substr(0, boundary);
                        while (!result.empty() && (result.back() == ' ' || result.back() == ','))
                            result.pop_back();
                        changed = true;
                        break;
                    }
                }
            }

            // Pass 2: check against multi-word suffix patterns (longer, more specific).
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

        if (rms < RMS_SILENCE_THOLD) {
            log_fwd_.forward(whispertalk::LogLevel::DEBUG, call_id,
                "Skipping low-energy chunk: RMS=%.6f (%.0fms)", rms, audio_len / (double)WSP_SAMPLES_PER_MS);
            return;
        }

        whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
        wparams.language = language_.c_str();
        wparams.n_threads = WSP_N_THREADS;
        wparams.no_timestamps = true;
        wparams.single_segment = false;
        wparams.beam_search.beam_size = 5;
        wparams.beam_search.patience = 1.0f;
        wparams.temperature = 0.0f;
        wparams.temperature_inc = WSP_TEMP_INC;
        wparams.entropy_thold = WSP_ENTROPY_THOLD;
        wparams.logprob_thold = -1.0f;
        wparams.no_speech_thold = WSP_NO_SPEECH_THOLD;

        wparams.audio_ctx = 0;

        wparams.no_context = true;
        wparams.initial_prompt = "Guten Tag, willkommen bei der Telefonzentrale.";

        std::lock_guard<std::mutex> lock(whisper_mutex_);
        auto t0 = std::chrono::steady_clock::now();
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
                        return;
                    }

                    std::string cleaned = strip_trailing_hallucinations(text);
                    if (cleaned.empty() || cleaned.size() < (size_t)MIN_TEXT_LEN) {
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
                log_fwd_.forward(whispertalk::LogLevel::INFO, call_id,
                    "Transcription (%lldms, RTF=%.2f): %s", whisper_ms, rtf, text.c_str());

                whispertalk::Packet pkt(call_id, text.c_str(), text.length());
                pkt.trace.record(whispertalk::ServiceType::WHISPER_SERVICE, 0);
                pkt.trace.record(whispertalk::ServiceType::WHISPER_SERVICE, 1);

                if (has_buffered_.load(std::memory_order_relaxed)) {
                    std::vector<whispertalk::Packet> to_drain;
                    {
                        std::lock_guard<std::mutex> buf_lock(buffer_mutex_);
                        to_drain.assign(std::make_move_iterator(buffered_packets_.begin()),
                                       std::make_move_iterator(buffered_packets_.end()));
                        buffered_packets_.clear();
                        has_buffered_.store(false, std::memory_order_relaxed);
                    }
                    size_t drained = 0;
                    for (auto& p : to_drain) {
                        if (!interconnect_.send_to_downstream(p)) break;
                        ++drained;
                    }
                    if (drained < to_drain.size()) {
                        std::lock_guard<std::mutex> buf_lock(buffer_mutex_);
                        for (size_t i = drained; i < to_drain.size(); ++i) {
                            buffered_packets_.push_back(std::move(to_drain[i]));
                        }
                        if (buffered_packets_.size() >= MAX_BUFFER_PACKETS) {
                            buffered_packets_.pop_front();
                        }
                        buffered_packets_.push_back(pkt);
                        has_buffered_.store(true, std::memory_order_relaxed);
                        log_fwd_.forward(whispertalk::LogLevel::WARN, call_id,
                            "LLaMA disconnected, buffering transcription (%zu in queue)",
                            buffered_packets_.size());
                        return;
                    }
                }

                if (!interconnect_.send_to_downstream(pkt)) {
                    std::lock_guard<std::mutex> buf_lock(buffer_mutex_);
                    if (buffered_packets_.size() >= MAX_BUFFER_PACKETS) {
                        buffered_packets_.pop_front();
                    }
                    buffered_packets_.push_back(pkt);
                    has_buffered_.store(true, std::memory_order_relaxed);
                    log_fwd_.forward(whispertalk::LogLevel::WARN, call_id,
                        "LLaMA disconnected, buffering transcription (%zu in queue)",
                        buffered_packets_.size());
                }
            }
        } else {
            log_fwd_.forward(whispertalk::LogLevel::ERROR, call_id, "Whisper transcription failed");
        }
    }

    void handle_call_end(uint32_t call_id) {
        {
            std::lock_guard<std::mutex> buf_lock(buffer_mutex_);
            auto it = buffered_packets_.begin();
            while (it != buffered_packets_.end()) {
                if (it->call_id == call_id) {
                    it = buffered_packets_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        log_fwd_.forward(whispertalk::LogLevel::INFO, call_id, "Call ended, cleared buffered packets");
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
    std::atomic<bool> has_buffered_{false};
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
    "Ich spreche Deutsch",
    "Ich spreche die Frage",
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
