#include <torch/script.h>
#include <torch/torch.h>
#include <espeak-ng/speak_lib.h>
#include "interconnect.h"
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
#include <unordered_map>
#include <vector>
#include <sys/stat.h>

using namespace whispertalk;

static const int KOKORO_SAMPLE_RATE = 24000;
static const size_t MAX_AUDIO_SAMPLES = 10 * KOKORO_SAMPLE_RATE;
static const size_t PHONEME_CACHE_MAX = 10000;

#ifndef ESPEAK_NG_DATA_DIR
#define ESPEAK_NG_DATA_DIR ""
#endif

struct KokoroVocab {
    std::map<std::string, int64_t> phoneme_to_id;

    bool load(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return false;
        std::string content((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
        size_t pos = 0;
        while ((pos = content.find("\"", pos)) != std::string::npos) {
            pos++;
            size_t end = content.find("\"", pos);
            if (end == std::string::npos) break;
            std::string key = content.substr(pos, end - pos);
            pos = end + 1;
            size_t colon = content.find(":", pos);
            if (colon == std::string::npos) break;
            size_t num_start = colon + 1;
            while (num_start < content.size() && (content[num_start] == ' ' || content[num_start] == '\n'))
                num_start++;
            size_t num_end = num_start;
            while (num_end < content.size() && (std::isdigit(content[num_end]) || content[num_end] == '-'))
                num_end++;
            if (num_end > num_start) {
                int64_t val = std::stoll(content.substr(num_start, num_end - num_start));
                phoneme_to_id[key] = val;
            }
            pos = num_end;
        }
        return !phoneme_to_id.empty();
    }

    std::vector<int64_t> encode(const std::string& phonemes) const {
        std::vector<int64_t> ids;
        ids.push_back(0);
        size_t i = 0;
        while (i < phonemes.size()) {
            bool found = false;
            for (int len = 4; len >= 1; len--) {
                if (i + len > phonemes.size()) continue;
                std::string key = phonemes.substr(i, len);
                auto it = phoneme_to_id.find(key);
                if (it != phoneme_to_id.end()) {
                    ids.push_back(it->second);
                    i += len;
                    found = true;
                    break;
                }
            }
            if (!found) {
                unsigned char c = static_cast<unsigned char>(phonemes[i]);
                if ((c & 0x80) != 0) {
                    int bytes = 1;
                    if ((c & 0xE0) == 0xC0) bytes = 2;
                    else if ((c & 0xF0) == 0xE0) bytes = 3;
                    else if ((c & 0xF8) == 0xF0) bytes = 4;
                    if (i + bytes <= phonemes.size()) {
                        std::string utf8char = phonemes.substr(i, bytes);
                        auto it = phoneme_to_id.find(utf8char);
                        if (it != phoneme_to_id.end()) {
                            ids.push_back(it->second);
                        }
                    }
                    i += bytes;
                } else {
                    i++;
                }
            }
        }
        ids.push_back(0);
        return ids;
    }
};

static std::string resolve_espeak_data_dir() {
    const char* env = std::getenv("ESPEAK_NG_DATA");
    if (env && env[0] != '\0') return env;

    const char* compiled = ESPEAK_NG_DATA_DIR;
    if (compiled[0] != '\0') {
        struct stat st;
        if (stat(compiled, &st) == 0) return compiled;
    }

    const char* candidates[] = {
        "./espeak-ng-data",
        "/opt/homebrew/share/espeak-ng-data",
        "/usr/local/share/espeak-ng-data",
        "/usr/share/espeak-ng-data",
        nullptr
    };
    for (int i = 0; candidates[i]; i++) {
        struct stat st;
        if (stat(candidates[i], &st) == 0) return candidates[i];
    }
    return "";
}

class KokoroPipeline {
public:
    bool initialize(const std::string& models_dir, const std::string& voice_name) {
        std::string model_path = models_dir + "/kokoro-german/kokoro_german.pt";
        std::string voice_path = models_dir + "/kokoro-german/" + voice_name + "_embedding.pt";
        std::string vocab_path = models_dir + "/kokoro-german/vocab.json";

        if (!vocab_.load(vocab_path)) {
            std::fprintf(stderr, "Failed to load vocab from %s\n", vocab_path.c_str());
            return false;
        }
        std::printf("Loaded vocab: %zu entries\n", vocab_.phoneme_to_id.size());

        try {
            model_ = torch::jit::load(model_path);
            model_.eval();
        } catch (const c10::Error& e) {
            std::fprintf(stderr, "Failed to load model: %s\n", e.what());
            return false;
        }
        std::printf("Loaded TorchScript model from %s\n", model_path.c_str());

        try {
            std::ifstream f(voice_path, std::ios::binary);
            if (!f.is_open()) {
                std::fprintf(stderr, "Failed to open voice file: %s\n", voice_path.c_str());
                return false;
            }
            std::vector<char> data((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
            voice_pack_ = torch::jit::pickle_load(data).toTensor().to(torch::kFloat32);
        } catch (const c10::Error& e) {
            std::fprintf(stderr, "Failed to load voice: %s\n", e.what());
            return false;
        }
        std::printf("Loaded voice pack '%s' shape: [%lld, %lld, %lld]\n",
                   voice_name.c_str(), voice_pack_.size(0), voice_pack_.size(1), voice_pack_.size(2));

        std::string espeak_data = resolve_espeak_data_dir();
        if (espeak_data.empty()) {
            std::fprintf(stderr, "Cannot find espeak-ng-data directory. "
                "Set ESPEAK_NG_DATA env var or install espeak-ng.\n");
            return false;
        }
        std::printf("Using espeak-ng data: %s\n", espeak_data.c_str());

        int result = espeak_Initialize(AUDIO_OUTPUT_RETRIEVAL, 0,
                                      espeak_data.c_str(), 0);
        if (result == -1) {
            std::fprintf(stderr, "Failed to initialize espeak-ng\n");
            return false;
        }
        espeak_SetVoiceByName("de");
        std::printf("espeak-ng initialized (German)\n");

        return true;
    }

    std::string phonemize(const std::string& text) {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = phoneme_cache_.find(text);
            if (it != phoneme_cache_.end()) return it->second;
        }

        std::string result;
        {
            std::lock_guard<std::mutex> lock(espeak_mutex_);
            const char* ptr = text.c_str();
            while (ptr && *ptr) {
                const char* ph = espeak_TextToPhonemes(
                    (const void**)&ptr, espeakCHARS_UTF8, espeakPHONEMES_IPA);
                if (ph) result += ph;
            }
        }

        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (phoneme_cache_.size() >= PHONEME_CACHE_MAX) {
                phoneme_cache_.clear();
            }
            phoneme_cache_[text] = result;
        }

        return result;
    }

    std::vector<float> synthesize(const std::string& text, float speed = 1.0f) {
        std::string phonemes = phonemize(text);
        if (phonemes.empty()) return {};

        auto ids = vocab_.encode(phonemes);
        if (ids.size() <= 2) return {};

        int phoneme_count = static_cast<int>(ids.size()) - 2;
        int voice_idx = std::min(phoneme_count - 1,
                                static_cast<int>(voice_pack_.size(0)) - 1);
        voice_idx = std::max(0, voice_idx);

        auto ref_s = voice_pack_[voice_idx];
        auto input_ids = torch::from_blob(ids.data(),
                                         {1, static_cast<int64_t>(ids.size())},
                                         torch::kLong).clone();
        auto speed_tensor = torch::tensor(speed);

        torch::Tensor audio;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            torch::NoGradGuard no_grad;
            std::vector<torch::jit::IValue> inputs;
            inputs.push_back(input_ids);
            inputs.push_back(ref_s);
            inputs.push_back(speed_tensor);
            audio = model_.forward(inputs).toTensor();
        }

        audio = audio.contiguous().cpu();
        auto accessor = audio.accessor<float, 1>();
        std::vector<float> samples(accessor.size(0));
        for (int64_t i = 0; i < accessor.size(0); i++) {
            samples[i] = accessor[i];
        }
        return samples;
    }

private:
    torch::jit::script::Module model_;
    torch::Tensor voice_pack_;
    KokoroVocab vocab_;
    std::mutex model_mutex_;
    std::mutex espeak_mutex_;
    std::unordered_map<std::string, std::string> phoneme_cache_;
    std::mutex cache_mutex_;
};

struct CallContext {
    uint32_t call_id;
    std::queue<std::string> text_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::thread worker;
    std::atomic<bool> active{true};
};

class KokoroService {
public:
    KokoroService() : node_(ServiceType::KOKORO_SERVICE) {}

    bool initialize(const std::string& voice_name) {
        if (!node_.initialize()) {
            std::fprintf(stderr, "Failed to initialize interconnect node\n");
            return false;
        }

        std::string models_dir = WHISPERTALK_MODELS_DIR;
        if (!pipeline_.initialize(models_dir, voice_name)) {
            std::fprintf(stderr, "Failed to initialize Kokoro pipeline\n");
            return false;
        }

        std::printf("Kokoro TTS Service initialized (German, libtorch + espeak-ng, voice=%s)\n",
                   voice_name.c_str());
        std::printf("  Negotiation ports: IN=%u OUT=%u\n", node_.ports().neg_in, node_.ports().neg_out);
        std::printf("  Is master: %s\n", node_.is_master() ? "yes" : "no");

        node_.register_call_end_handler([this](uint32_t call_id) {
            handle_call_end(call_id);
        });

        return true;
    }

    void run() {
        if (!node_.connect_to_downstream()) {
            std::printf("Downstream (OAP) not available yet - will auto-reconnect\n");
        }

        std::printf("Kokoro service ready - waiting for text from LLaMA\n");

        while (running_) {
            Packet pkt;
            if (node_.recv_from_upstream(pkt, 100)) {
                dispatch_text_packet(pkt);
            }

            if (node_.upstream_state() == ConnectionState::FAILED) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }

        shutdown_all_calls();
    }

    void shutdown() {
        running_ = false;
        shutdown_all_calls();
        node_.shutdown();
    }

private:
    void dispatch_text_packet(const Packet& pkt) {
        std::string text(reinterpret_cast<const char*>(pkt.payload.data()), pkt.payload.size());

        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto it = calls_.find(pkt.call_id);
        if (it == calls_.end()) {
            auto ctx = std::make_shared<CallContext>();
            ctx->call_id = pkt.call_id;
            ctx->worker = std::thread(&KokoroService::call_worker, this, ctx);
            calls_[pkt.call_id] = ctx;
            std::printf("Started synthesis thread for call %u\n", pkt.call_id);
            it = calls_.find(pkt.call_id);
        }

        auto& ctx = it->second;
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
                ctx->queue_cv.wait_for(lock, std::chrono::milliseconds(500),
                    [&]{ return !ctx->text_queue.empty() || !ctx->active || !running_; });
                if (!ctx->active || !running_) break;
                if (ctx->text_queue.empty()) continue;
                text = ctx->text_queue.front();
                ctx->text_queue.pop();
            }

            std::printf("Synthesizing for call %u: %s\n", ctx->call_id, text.c_str());

            auto start = std::chrono::steady_clock::now();
            auto samples = pipeline_.synthesize(text);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (samples.empty() || samples.size() > MAX_AUDIO_SAMPLES) {
                std::fprintf(stderr, "Invalid audio output for call %u: %zu samples\n",
                            ctx->call_id, samples.size());
                continue;
            }

            std::printf("Synthesized %zu samples in %lldms for call %u\n",
                        samples.size(), elapsed, ctx->call_id);

            send_audio_to_downstream(ctx->call_id, samples);
        }
    }

    void send_audio_to_downstream(uint32_t call_id, const std::vector<float>& samples) {
        if (node_.downstream_state() != ConnectionState::CONNECTED) {
            std::printf("Downstream (OAP) not connected - discarding audio for call %u\n", call_id);
            return;
        }

        Packet audio_pkt;
        audio_pkt.call_id = call_id;

        size_t header_size = sizeof(int32_t);
        audio_pkt.payload_size = static_cast<uint32_t>(header_size + samples.size() * sizeof(float));
        audio_pkt.payload.resize(audio_pkt.payload_size);

        int32_t sr = KOKORO_SAMPLE_RATE;
        std::memcpy(audio_pkt.payload.data(), &sr, sizeof(int32_t));
        std::memcpy(audio_pkt.payload.data() + header_size, samples.data(),
                   samples.size() * sizeof(float));

        if (node_.send_to_downstream(audio_pkt)) {
            std::printf("Sent %zu samples @ %d Hz for call %u to OAP\n",
                       samples.size(), KOKORO_SAMPLE_RATE, call_id);
        } else {
            std::fprintf(stderr, "Failed to send audio for call %u\n", call_id);
        }
    }

    void handle_call_end(uint32_t call_id) {
        std::printf("Call %u ended - cleaning up synthesis thread\n", call_id);
        std::shared_ptr<CallContext> ctx;
        {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            auto it = calls_.find(call_id);
            if (it == calls_.end()) return;
            ctx = it->second;
            calls_.erase(it);
        }
        ctx->active = false;
        ctx->queue_cv.notify_one();
        if (ctx->worker.joinable()) ctx->worker.join();
    }

    void shutdown_all_calls() {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        for (auto& [id, ctx] : calls_) {
            ctx->active = false;
            ctx->queue_cv.notify_one();
        }
        for (auto& [id, ctx] : calls_) {
            if (ctx->worker.joinable()) ctx->worker.join();
        }
        calls_.clear();
    }

    InterconnectNode node_;
    KokoroPipeline pipeline_;
    std::atomic<bool> running_{true};
    std::map<uint32_t, std::shared_ptr<CallContext>> calls_;
    std::mutex calls_mutex_;
};

static KokoroService* g_service = nullptr;

void signal_handler(int) {
    if (g_service) {
        std::printf("\nShutting down Kokoro service\n");
        g_service->shutdown();
    }
}

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::string voice = "df_eva";
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.rfind("--voice=", 0) == 0) {
            voice = arg.substr(8);
        } else if (arg == "--voice" && i + 1 < argc) {
            voice = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            std::printf("Usage: kokoro-service [--voice=NAME]\n");
            std::printf("  --voice=NAME   Voice to use (default: df_eva, also: dm_bernd)\n");
            return 0;
        }
    }

    std::printf("Starting Kokoro TTS Service (libtorch + espeak-ng, voice=%s)\n", voice.c_str());

    KokoroService service;
    g_service = &service;

    if (!service.initialize(voice)) {
        return 1;
    }

    service.run();

    return 0;
}
