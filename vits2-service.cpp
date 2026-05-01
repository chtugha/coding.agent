// vits2-service.cpp
//
// Pipeline position: LLaMA → [TTS dock] → (engine: VITS2/Piper) → [TTS dock] → OAP
//
// TTS engine using Piper VITS2 models via libpiper (ONNX Runtime). Connects to
// the generic TTS dock (`tts-service`) via the EngineClient hotplug protocol.
//
// Inference pipeline:
//   1. Piper handles text → phonemes → audio internally (espeak-ng + VITS2 ONNX)
//   2. Optional: pre-phonemize German text via NeuralG2P before passing to Piper
//   3. Resample output to 24kHz if Piper model outputs at a different rate
//   4. Normalize + fade-in, send via EngineClient to TTS dock → OAP
//
// CMD port (VITS2 engine diagnostic port 13175): PING, STATUS, SET_LOG_LEVEL,
//   TEST_SYNTH, SYNTH_WAV. Separate from the TTS dock's cmd port (13142).

#include "interconnect.h"
#include "tts-engine-client.h"
#include "tts-common.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#include <getopt.h>
#include <cmath>

#include "piper.h"

#ifdef __APPLE__
#include "neural-g2p.h"
#endif

using namespace whispertalk;

static constexpr int VITS2_SAMPLE_RATE = static_cast<int>(whispertalk::tts::kTTSSampleRate);
static constexpr uint16_t VITS2_ENGINE_CMD_PORT = whispertalk::tts::kVITS2EngineCmdPort;
static constexpr size_t MAX_AUDIO_SAMPLES = 120 * VITS2_SAMPLE_RATE;
static constexpr size_t DOWNSTREAM_CHUNK_SAMPLES = whispertalk::tts::kTTSMaxFrameSamples;
static constexpr int CMD_RECV_TIMEOUT_SEC = 30;
static constexpr int CMD_POLL_TIMEOUT_MS = 200;
static constexpr int WORKER_WAIT_TIMEOUT_MS = 500;
static constexpr size_t CMD_BUF_SIZE = 4096;

static std::vector<float> resample_linear(const std::vector<float>& input, int src_rate, int dst_rate) {
    if (src_rate == dst_rate) return input;
    if (input.empty()) return {};
    double ratio = static_cast<double>(dst_rate) / static_cast<double>(src_rate);
    size_t out_size = static_cast<size_t>(input.size() * ratio);
    if (out_size == 0) return {};
    std::vector<float> output(out_size);
    for (size_t i = 0; i < out_size; i++) {
        double src_pos = i / ratio;
        size_t src_idx = static_cast<size_t>(src_pos);
        double frac = src_pos - src_idx;
        if (src_idx + 1 < input.size()) {
            output[i] = static_cast<float>(input[src_idx] * (1.0 - frac) + input[src_idx + 1] * frac);
        } else {
            output[i] = input[src_idx < input.size() ? src_idx : input.size() - 1];
        }
    }
    return output;
}

class VITS2Pipeline {
public:
    ~VITS2Pipeline() {
        if (synth_) {
            piper_free(synth_);
            synth_ = nullptr;
        }
    }

    bool initialize(const std::string& models_dir, const std::string& voice_name) {
        std::string vits2_dir = models_dir + "/vits2-german";
        std::string model_path = vits2_dir + "/" + voice_name + ".onnx";
        std::string config_path = vits2_dir + "/" + voice_name + ".onnx.json";

        struct stat st;
        if (stat(model_path.c_str(), &st) != 0) {
            std::fprintf(stderr, "[vits2] Model not found: %s\n", model_path.c_str());
            std::fprintf(stderr, "[vits2] Run scripts/setup_vits2_models.py to download models\n");
            return false;
        }

        std::string espeak_data = tts::resolve_espeak_data_dir();
        if (espeak_data.empty()) {
            std::fprintf(stderr, "[vits2] Cannot find espeak-ng-data directory\n");
            return false;
        }
        std::printf("[vits2] Using espeak-ng data: %s\n", espeak_data.c_str());

        synth_ = piper_create(model_path.c_str(),
                               stat(config_path.c_str(), &st) == 0 ? config_path.c_str() : nullptr,
                               espeak_data.c_str());
        if (!synth_) {
            std::fprintf(stderr, "[vits2] piper_create failed for model: %s\n", model_path.c_str());
            return false;
        }

        model_path_ = model_path;
        std::printf("[vits2] Piper VITS2 loaded: %s\n", model_path.c_str());

#ifdef __APPLE__
        const char* env_models = std::getenv("WHISPERTALK_MODELS_DIR");
        std::string g2p_dir = (env_models ? std::string(env_models) : models_dir) + "/g2p";
        std::string g2p_model = g2p_dir + "/de_g2p.mlmodelc";
        if (stat(g2p_model.c_str(), &st) == 0) {
            neural_g2p_ = std::make_unique<NeuralG2P>();
            if (!neural_g2p_->load(g2p_model)) {
                std::fprintf(stderr, "[vits2] Neural G2P load failed, falling back to Piper built-in\n");
                neural_g2p_.reset();
            } else {
                std::printf("[vits2] Neural G2P loaded: %s\n", g2p_model.c_str());
                std::fprintf(stderr, "[vits2] WARNING: Piper C API does not accept pre-computed IPA input. "
                             "Neural G2P is loaded but cannot be used for synthesis; "
                             "Piper's built-in espeak-ng phonemizer will be used instead.\n");
            }
        }
#endif

        std::printf("[vits2] Warming up synthesis engine...\n");
        auto warmup = synthesize("Hallo.");
        if (warmup.empty()) {
            std::fprintf(stderr, "[vits2] Warmup synthesis failed\n");
        } else {
            std::printf("[vits2] Warmup done (%zu samples)\n", warmup.size());
        }

        return true;
    }

    void set_g2p_backend(G2PBackend backend) {
        g2p_backend_ = backend;
    }

    std::vector<float> synthesize(const std::string& text, std::atomic<bool>* interrupted = nullptr) {
        std::vector<float> result;
        synthesize_streaming(text, interrupted, [&](std::vector<float> chunk) {
            result.insert(result.end(), chunk.begin(), chunk.end());
        });
        return result;
    }

    template<typename Callback>
    void synthesize_streaming(const std::string& text, std::atomic<bool>* interrupted,
                              Callback&& callback) {
        if (!synth_) return;

        piper_synthesize_options opts = piper_default_synthesize_options(synth_);

        const std::string& synth_text = text;

        {
            std::lock_guard<std::mutex> lock(synth_mutex_);
            if (interrupted && interrupted->load()) return;

            int rc = piper_synthesize_start(synth_, synth_text.c_str(), &opts);
            if (rc != PIPER_OK) {
                std::fprintf(stderr, "[vits2] piper_synthesize_start failed: %d\n", rc);
                return;
            }

            piper_audio_chunk chunk{};
            size_t total_samples = 0;
            bool first_chunk = true;

            while (true) {
                if (interrupted && interrupted->load()) break;

                rc = piper_synthesize_next(synth_, &chunk);
                if (rc == PIPER_ERR_GENERIC) {
                    std::fprintf(stderr, "[vits2] piper_synthesize_next error\n");
                    break;
                }

                if (chunk.num_samples > 0 && chunk.samples) {
                    std::vector<float> samples(chunk.samples, chunk.samples + chunk.num_samples);

                    if (chunk.sample_rate != VITS2_SAMPLE_RATE) {
                        samples = resample_linear(samples, chunk.sample_rate, VITS2_SAMPLE_RATE);
                    }

                    if (total_samples + samples.size() > MAX_AUDIO_SAMPLES) {
                        size_t allowed = MAX_AUDIO_SAMPLES - total_samples;
                        if (allowed == 0) break;
                        samples.resize(allowed);
                    }

                    float peak = 0.0f;
                    for (float s : samples) {
                        float a = std::abs(s);
                        if (a > peak) peak = a;
                    }
                    if (peak > 0.90f) {
                        float scale = 0.90f / peak;
                        for (float& s : samples) s *= scale;
                    }

                    if (first_chunk) {
                        tts::apply_fade_in(samples);
                        first_chunk = false;
                    }

                    total_samples += samples.size();
                    callback(std::move(samples));
                }

                if (rc == PIPER_DONE || chunk.is_last) break;
            }
        }
    }

    const std::string& model_path() const { return model_path_; }

private:
    piper_synthesizer* synth_ = nullptr;
    std::mutex synth_mutex_;
    std::string model_path_;
    G2PBackend g2p_backend_ = G2PBackend::AUTO;
#ifdef __APPLE__
    std::unique_ptr<NeuralG2P> neural_g2p_;
#endif
};

struct CallContext {
    uint32_t call_id;
    std::queue<std::string> text_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::thread worker;
    std::atomic<bool> active{true};
    std::atomic<bool> interrupted{false};

    std::queue<std::vector<float>> audio_queue;
    std::mutex audio_mutex;
    std::condition_variable audio_cv;
    std::thread audio_sender;
};

class VITS2Service {
public:
    VITS2Service() = default;

    bool initialize(const std::string& model_dir, const std::string& voice_name,
                    G2PBackend g2p_backend) {
        const char* env_models = std::getenv("WHISPERTALK_MODELS_DIR");
        std::string models_dir = !model_dir.empty() ? model_dir :
                                 (env_models ? env_models :
#ifdef WHISPERTALK_MODELS_DIR
                                  WHISPERTALK_MODELS_DIR
#else
                                  "models"
#endif
                                 );

        pipeline_.set_g2p_backend(g2p_backend);

        if (!pipeline_.initialize(models_dir, voice_name)) {
            std::fprintf(stderr, "[vits2] Failed to initialize VITS2 pipeline\n");
            return false;
        }

        log_fwd_.init(FRONTEND_LOG_PORT, ServiceType::TTS_SERVICE);

        std::printf("[vits2] Service initialized (Piper VITS2, ONNX Runtime)\n");

        engine_.set_name("vits2");
        EngineAudioFormat fmt;
        fmt.sample_rate = VITS2_SAMPLE_RATE;
        fmt.channels = 1;
        fmt.format = "f32le";
        engine_.set_audio_format(fmt);

        engine_.register_call_end_handler([this](uint32_t call_id) {
            handle_call_end(call_id);
        });

        engine_.register_speech_signal_handler([this](uint32_t call_id, bool active) {
            if (active) {
                handle_speech_active(call_id);
            } else {
                prewarm_call(call_id);
            }
        });

        engine_.register_custom_handler("SHUTDOWN", [this]() {
            std::fprintf(stderr, "[vits2] received SHUTDOWN from TTS dock — signalling exit\n");
            running_.store(false);
        });

        if (!engine_.start()) {
            std::fprintf(stderr, "[vits2] Failed to start TTS engine client\n");
            return false;
        }

        return true;
    }

    void run() {
        std::thread cmd_thread(&VITS2Service::command_listener_loop, this);

        std::printf("[vits2] Service ready - connecting to TTS dock at 127.0.0.1:%u\n",
                    (unsigned)service_engine_port(ServiceType::TTS_SERVICE));

        while (running_) {
            Packet pkt;
            if (engine_.recv_text(pkt, 100)) {
                dispatch_text_packet(pkt);
            }
        }

        shutdown_all_calls();

        int s1 = cmd_sock_.exchange(-1);
        if (s1 >= 0) ::close(s1);
        if (cmd_thread.joinable()) cmd_thread.join();
    }

    void shutdown() {
        running_ = false;
        int s2 = cmd_sock_.exchange(-1);
        if (s2 >= 0) ::close(s2);
        shutdown_all_calls();
        engine_.shutdown();
    }

    void set_log_level(const char* level) {
        log_fwd_.set_level(level);
    }

private:
    void command_listener_loop() {
        uint16_t port = VITS2_ENGINE_CMD_PORT;
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return;

        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);

        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::fprintf(stderr, "[vits2] cmd: bind port %d failed\n", port);
            ::close(sock);
            return;
        }
        listen(sock, 4);
        cmd_sock_.store(sock);
        std::printf("[vits2] Command listener on port %d\n", port);

        while (running_) {
            struct pollfd pfd{sock, POLLIN, 0};
            int r = poll(&pfd, 1, CMD_POLL_TIMEOUT_MS);
            if (r <= 0) continue;

            int csock = accept(sock, nullptr, nullptr);
            if (csock < 0) continue;

            struct timeval tv{CMD_RECV_TIMEOUT_SEC, 0};
            setsockopt(csock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            char buf[CMD_BUF_SIZE];
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
        if (cmd.rfind("TEST_SYNTH:", 0) == 0) {
            std::string text = cmd.substr(11);
            std::vector<float> samples;
            auto start = std::chrono::steady_clock::now();
            samples = pipeline_.synthesize(text);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (samples.empty()) return "ERROR:synthesis failed\n";

            double duration_s = (double)samples.size() / VITS2_SAMPLE_RATE;
            double rtf = elapsed > 0 ? (elapsed / 1000.0) / duration_s : 0.0;

            float raw_peak = 0.0f;
            for (float s : samples) {
                float a = std::abs(s);
                if (a > raw_peak) raw_peak = a;
            }

            return "SYNTH_RESULT:" + std::to_string(elapsed) + "ms:"
                + std::to_string(samples.size()) + ":" + std::to_string(VITS2_SAMPLE_RATE) + ":"
                + std::to_string(duration_s) + "s:rtf=" + std::to_string(rtf)
                + ":peak=" + std::to_string(raw_peak)
                + ":engine=vits2\n";
        }
        if (cmd.rfind("SYNTH_WAV:", 0) == 0) {
            std::string rest = cmd.substr(10);
            size_t sep = rest.find('|');
            if (sep == std::string::npos) return "ERROR:format SYNTH_WAV:<path>|<text>\n";
            std::string path = rest.substr(0, sep);
            std::string text = rest.substr(sep + 1);
            if (path.empty() || text.empty()) return "ERROR:empty path or text\n";
            if (path.find("..") != std::string::npos || path[0] == '/')
                return "ERROR:invalid path\n";

            std::vector<float> samples;
            auto start = std::chrono::steady_clock::now();
            samples = pipeline_.synthesize(text);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (samples.empty()) return "ERROR:synthesis failed\n";

            std::ofstream out(path, std::ios::binary);
            if (!out.is_open()) return "ERROR:cannot open " + path + "\n";

            uint32_t data_size = (uint32_t)(samples.size() * sizeof(int16_t));
            uint32_t file_size = 36 + data_size;
            int32_t sr = VITS2_SAMPLE_RATE;
            int16_t channels = 1;
            int16_t bits = 16;
            int32_t byte_rate = sr * channels * bits / 8;
            int16_t block_align = channels * bits / 8;

            out.write("RIFF", 4);
            out.write(reinterpret_cast<char*>(&file_size), 4);
            out.write("WAVE", 4);
            out.write("fmt ", 4);
            int32_t fmt_size = 16;
            out.write(reinterpret_cast<char*>(&fmt_size), 4);
            int16_t fmt_tag = 1;
            out.write(reinterpret_cast<char*>(&fmt_tag), 2);
            out.write(reinterpret_cast<char*>(&channels), 2);
            out.write(reinterpret_cast<char*>(&sr), 4);
            out.write(reinterpret_cast<char*>(&byte_rate), 4);
            out.write(reinterpret_cast<char*>(&block_align), 2);
            out.write(reinterpret_cast<char*>(&bits), 2);
            out.write("data", 4);
            out.write(reinterpret_cast<char*>(&data_size), 4);

            for (float s : samples) {
                int16_t pcm = static_cast<int16_t>(std::max(-1.0f, std::min(1.0f, s)) * 32767.0f);
                out.write(reinterpret_cast<char*>(&pcm), 2);
            }
            out.flush();
            if (!out.good()) {
                out.close();
                std::remove(path.c_str());
                return "ERROR:write failed\n";
            }
            out.close();

            double duration_s = (double)samples.size() / VITS2_SAMPLE_RATE;
            double rtf = elapsed > 0 ? (elapsed / 1000.0) / duration_s : 0.0;

            return "WAV_RESULT:" + std::to_string(elapsed) + "ms:"
                + std::to_string(samples.size()) + ":" + std::to_string(VITS2_SAMPLE_RATE) + ":"
                + std::to_string(duration_s) + "s:rtf=" + std::to_string(rtf)
                + ":path=" + path + "\n";
        }
        if (cmd == "PING") {
            return "PONG\n";
        }
        if (cmd.rfind("SET_LOG_LEVEL:", 0) == 0) {
            std::string level = cmd.substr(14);
            log_fwd_.set_level(level.c_str());
            return "OK\n";
        }
        if (cmd == "STATUS") {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            return "ACTIVE_CALLS:" + std::to_string(calls_.size())
                + ":DOCK:" + (engine_.is_connected() ? "connected" : "disconnected")
                + ":MODEL:" + pipeline_.model_path()
                + ":ENGINE:vits2\n";
        }
        return "ERROR:Unknown command\n";
    }

    void prewarm_call(uint32_t call_id) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto [it, inserted] = calls_.try_emplace(call_id, nullptr);
        if (inserted) {
            auto ctx = std::make_shared<CallContext>();
            ctx->call_id = call_id;
            ctx->worker = std::thread(&VITS2Service::call_worker, this, ctx);
            ctx->audio_sender = std::thread(&VITS2Service::audio_sender_loop, this, ctx);
            it->second = ctx;
            log_fwd_.forward(LogLevel::DEBUG, call_id, "Prewarmed VITS2 synthesis thread on SPEECH_IDLE");
        }
        it->second->interrupted = false;
    }

    void dispatch_text_packet(const Packet& pkt) {
        std::string text(reinterpret_cast<const char*>(pkt.payload.data()), pkt.payload.size());

        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto [it, inserted] = calls_.try_emplace(pkt.call_id, nullptr);
        if (inserted) {
            auto ctx = std::make_shared<CallContext>();
            ctx->call_id = pkt.call_id;
            ctx->worker = std::thread(&VITS2Service::call_worker, this, ctx);
            ctx->audio_sender = std::thread(&VITS2Service::audio_sender_loop, this, ctx);
            it->second = ctx;
            std::printf("[vits2] Started synthesis thread for call %u\n", pkt.call_id);
            log_fwd_.forward(LogLevel::INFO, pkt.call_id, "Started VITS2 synthesis thread");
        }

        auto& ctx = it->second;
        ctx->interrupted = false;
        {
            std::lock_guard<std::mutex> qlock(ctx->queue_mutex);
            ctx->text_queue.push(text);
        }
        ctx->queue_cv.notify_one();
    }

    void call_worker(std::shared_ptr<CallContext> ctx) {
        while (ctx->active && running_) {
            std::string text;
            {
                std::unique_lock<std::mutex> lock(ctx->queue_mutex);
                ctx->queue_cv.wait_for(lock, std::chrono::milliseconds(WORKER_WAIT_TIMEOUT_MS),
                    [&]{ return !ctx->text_queue.empty() || !ctx->active || !running_; });
                if (!ctx->active || !running_) break;
                if (ctx->text_queue.empty()) continue;
                text = ctx->text_queue.front();
                ctx->text_queue.pop();
            }

            if (ctx->interrupted.load()) {
                ctx->interrupted = false;
                continue;
            }

            std::printf("[vits2] Synthesizing for call %u: %s\n", ctx->call_id, text.c_str());

            size_t chunks_produced = 0;
            auto start = std::chrono::steady_clock::now();

            pipeline_.synthesize_streaming(text, &ctx->interrupted,
                [&](std::vector<float> chunk) {
                    if (ctx->interrupted.load()) return;
                    {
                        std::lock_guard<std::mutex> alock(ctx->audio_mutex);
                        if (ctx->interrupted.load()) return;
                        ctx->audio_queue.push(std::move(chunk));
                    }
                    ctx->audio_cv.notify_one();
                    chunks_produced++;
                });

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (ctx->interrupted.load()) {
                ctx->interrupted = false;
                std::printf("[vits2] Synthesis interrupted for call %u\n", ctx->call_id);
                log_fwd_.forward(LogLevel::WARN, ctx->call_id, "Synthesis interrupted");
                continue;
            }

            if (chunks_produced == 0) {
                std::fprintf(stderr, "[vits2] No audio output for call %u\n", ctx->call_id);
                continue;
            }

            std::printf("[vits2] Synthesis complete for call %u in %lldms (%zu chunks)\n",
                        ctx->call_id, (long long)elapsed, chunks_produced);
            log_fwd_.forward(LogLevel::INFO, ctx->call_id, "Synthesis complete in %lldms (VITS2)",
                            (long long)elapsed);
        }
    }

    void audio_sender_loop(std::shared_ptr<CallContext> ctx) {
        while (true) {
            std::vector<float> chunk;
            {
                std::unique_lock<std::mutex> lock(ctx->audio_mutex);
                ctx->audio_cv.wait(lock, [&] {
                    return !ctx->audio_queue.empty() || !ctx->active.load();
                });
                if (!ctx->active.load() && ctx->audio_queue.empty()) break;
                if (ctx->audio_queue.empty()) continue;
                chunk = std::move(ctx->audio_queue.front());
                ctx->audio_queue.pop();
            }
            send_audio_to_downstream(ctx->call_id, chunk);
        }
    }

    void send_audio_to_downstream(uint32_t call_id, const std::vector<float>& samples) {
        if (!engine_.is_connected()) return;

        constexpr size_t HEADER_SIZE = whispertalk::tts::kTTSAudioHeaderBytes;

        for (size_t offset = 0; offset < samples.size(); offset += DOWNSTREAM_CHUNK_SAMPLES) {
            size_t count = std::min(DOWNSTREAM_CHUNK_SAMPLES, samples.size() - offset);

            Packet audio_pkt;
            audio_pkt.call_id = call_id;
            audio_pkt.payload_size = static_cast<uint32_t>(HEADER_SIZE + count * sizeof(float));
            audio_pkt.payload.resize(audio_pkt.payload_size);

            int32_t sr = VITS2_SAMPLE_RATE;
            std::memcpy(audio_pkt.payload.data(), &sr, sizeof(int32_t));
            uint64_t t_out_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            uint8_t ts_be[8];
            for (int i = 0; i < 8; ++i) ts_be[7 - i] = static_cast<uint8_t>((t_out_us >> (i * 8)) & 0xff);
            std::memcpy(audio_pkt.payload.data() + sizeof(int32_t), ts_be, sizeof(ts_be));
            std::memcpy(audio_pkt.payload.data() + HEADER_SIZE,
                       samples.data() + offset, count * sizeof(float));

            audio_pkt.trace.record(ServiceType::TTS_SERVICE, 0);
            if (!engine_.send_audio(audio_pkt)) {
                log_fwd_.forward(LogLevel::ERROR, call_id, "Failed to send audio chunk to TTS dock");
                break;
            }
        }
    }

    void handle_speech_active(uint32_t call_id) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto it = calls_.find(call_id);
        if (it == calls_.end()) return;
        auto& ctx = it->second;
        ctx->interrupted = true;
        {
            std::lock_guard<std::mutex> qlock(ctx->queue_mutex);
            std::queue<std::string> empty;
            std::swap(ctx->text_queue, empty);
        }
        {
            std::lock_guard<std::mutex> alock(ctx->audio_mutex);
            std::queue<std::vector<float>> empty;
            std::swap(ctx->audio_queue, empty);
        }
        ctx->audio_cv.notify_one();
        log_fwd_.forward(LogLevel::DEBUG, call_id, "SPEECH_ACTIVE — flushed TTS queue, interrupting synthesis");
    }

    void handle_call_end(uint32_t call_id) {
        log_fwd_.forward(LogLevel::INFO, call_id, "Call ended, cleaning up synthesis thread");
        std::shared_ptr<CallContext> ctx;
        {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            auto it = calls_.find(call_id);
            if (it == calls_.end()) return;
            ctx = it->second;
            calls_.erase(it);
        }
        ctx->interrupted = true;
        ctx->active = false;
        {
            std::lock_guard<std::mutex> qlock(ctx->queue_mutex);
            std::queue<std::string> empty_text;
            std::swap(ctx->text_queue, empty_text);
        }
        {
            std::lock_guard<std::mutex> alock(ctx->audio_mutex);
            std::queue<std::vector<float>> empty_audio;
            std::swap(ctx->audio_queue, empty_audio);
        }
        ctx->queue_cv.notify_one();
        ctx->audio_cv.notify_all();
        if (ctx->worker.joinable()) ctx->worker.join();
        if (ctx->audio_sender.joinable()) ctx->audio_sender.join();
    }

    void shutdown_all_calls() {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        for (auto& [id, ctx] : calls_) {
            ctx->interrupted = true;
            ctx->active = false;
            {
                std::lock_guard<std::mutex> qlock(ctx->queue_mutex);
                std::queue<std::string> empty_text;
                std::swap(ctx->text_queue, empty_text);
            }
            {
                std::lock_guard<std::mutex> alock(ctx->audio_mutex);
                std::queue<std::vector<float>> empty_audio;
                std::swap(ctx->audio_queue, empty_audio);
            }
            ctx->queue_cv.notify_one();
            ctx->audio_cv.notify_all();
        }
        for (auto& [id, ctx] : calls_) {
            if (ctx->worker.joinable()) ctx->worker.join();
            if (ctx->audio_sender.joinable()) ctx->audio_sender.join();
        }
        calls_.clear();
    }

    EngineClient engine_;
    LogForwarder log_fwd_;
    VITS2Pipeline pipeline_;
    std::atomic<bool> running_{true};
    std::atomic<int> cmd_sock_{-1};
    std::map<uint32_t, std::shared_ptr<CallContext>> calls_;
    std::mutex calls_mutex_;
};

static VITS2Service* g_service = nullptr;

void signal_handler(int) {
    if (g_service) {
        std::printf("\nShutting down VITS2 service\n");
        g_service->shutdown();
    }
}

int main(int argc, char* argv[]) {
    setlinebuf(stdout);
    setlinebuf(stderr);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::string log_level = "INFO";
    std::string model_dir;
    std::string voice_name = "de_DE-thorsten-high";
    G2PBackend g2p_backend = G2PBackend::AUTO;

    static struct option long_opts[] = {
        {"model-dir",  required_argument, 0, 'm'},
        {"voice",      required_argument, 0, 'v'},
        {"g2p",        required_argument, 0, 'g'},
        {"log-level",  required_argument, 0, 'L'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "m:v:g:L:h", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'm': model_dir = optarg; break;
            case 'v': voice_name = optarg; break;
            case 'g':
                if (std::string(optarg) == "neural") g2p_backend = G2PBackend::NEURAL;
                else if (std::string(optarg) == "espeak") g2p_backend = G2PBackend::ESPEAK;
                else g2p_backend = G2PBackend::AUTO;
                break;
            case 'L': log_level = optarg; break;
            case 'h':
                std::printf("Usage: vits2-service [OPTIONS]\n");
                std::printf("  --model-dir DIR    Models directory (default: $WHISPERTALK_MODELS_DIR)\n");
                std::printf("  --voice NAME       Voice name/filename without extension (default: de_DE-thorsten-high)\n");
                std::printf("  --g2p auto|neural|espeak  G2P backend (default: auto)\n");
                std::printf("  --log-level LEVEL  Log level: ERROR WARN INFO DEBUG TRACE (default: INFO)\n");
                std::printf("\nPiper VITS2 TTS engine. Connects to the TTS dock via EngineClient.\n");
                std::printf("Models: $WHISPERTALK_MODELS_DIR/vits2-german/<voice>.onnx + .onnx.json\n");
                return 0;
            default: break;
        }
    }

    std::printf("[vits2] Starting VITS2 Service (Piper, ONNX Runtime)\n");

    VITS2Service service;
    g_service = &service;

    if (!service.initialize(model_dir, voice_name, g2p_backend)) {
        return 1;
    }

    service.set_log_level(log_level.c_str());
    service.run();

    return 0;
}
