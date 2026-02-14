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
#include <string>
#include <thread>
#include <vector>

using namespace whispertalk;

static const int KOKORO_SAMPLE_RATE = 24000;

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
                if ((c & 0xC0) == 0xC0) {
                    int bytes = 1;
                    if ((c & 0xE0) == 0xC0) bytes = 2;
                    else if ((c & 0xF0) == 0xE0) bytes = 3;
                    else if ((c & 0xF8) == 0xF0) bytes = 4;
                    std::string utf8char = phonemes.substr(i, bytes);
                    auto it = phoneme_to_id.find(utf8char);
                    if (it != phoneme_to_id.end()) {
                        ids.push_back(it->second);
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

class KokoroPipeline {
public:
    bool initialize(const std::string& models_dir) {
        std::string model_path = models_dir + "/kokoro-german/kokoro_german.pt";
        std::string voice_path = models_dir + "/kokoro-german/df_eva_embedding.pt";
        std::string vocab_path = models_dir + "/kokoro-german/vocab.json";
        std::string espeak_data = "/opt/homebrew/share/espeak-ng-data";

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
            std::vector<char> data((std::istreambuf_iterator<char>(f)),
                                  std::istreambuf_iterator<char>());
            voice_pack_ = torch::jit::pickle_load(data).toTensor().to(torch::kFloat32);
        } catch (const c10::Error& e) {
            std::fprintf(stderr, "Failed to load voice: %s\n", e.what());
            return false;
        }
        std::printf("Loaded voice pack shape: [%lld, %lld, %lld]\n",
                   voice_pack_.size(0), voice_pack_.size(1), voice_pack_.size(2));

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
        std::lock_guard<std::mutex> lock(espeak_mutex_);
        std::string result;
        const char* ptr = text.c_str();
        while (ptr && *ptr) {
            const char* ph = espeak_TextToPhonemes(
                (const void**)&ptr, espeakCHARS_UTF8, espeakPHONEMES_IPA);
            if (ph) result += ph;
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
};

class KokoroService {
public:
    KokoroService() : node_(ServiceType::KOKORO_SERVICE) {}

    bool initialize() {
        if (!node_.initialize()) {
            std::fprintf(stderr, "Failed to initialize interconnect node\n");
            return false;
        }

        std::string models_dir = WHISPERTALK_MODELS_DIR;
        if (!pipeline_.initialize(models_dir)) {
            std::fprintf(stderr, "Failed to initialize Kokoro pipeline\n");
            return false;
        }

        std::printf("Kokoro TTS Service initialized (German, libtorch + espeak-ng)\n");
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
                handle_text_packet(pkt);
            }

            if (node_.upstream_state() == ConnectionState::FAILED) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    void shutdown() {
        running_ = false;
        node_.shutdown();
    }

private:
    void handle_text_packet(const Packet& pkt) {
        std::string text(reinterpret_cast<const char*>(pkt.payload.data()), pkt.payload.size());

        std::printf("Synthesizing for call %u: %s\n", pkt.call_id, text.c_str());

        auto start = std::chrono::steady_clock::now();
        auto samples = pipeline_.synthesize(text);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start).count();

        if (samples.empty()) {
            std::fprintf(stderr, "No audio generated for call %u\n", pkt.call_id);
            return;
        }

        std::printf("Synthesized %zu samples in %lldms for call %u\n",
                    samples.size(), elapsed, pkt.call_id);

        send_audio_to_downstream(pkt.call_id, samples);
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
        std::printf("Call %u ended - cleaning up\n", call_id);
    }

    InterconnectNode node_;
    KokoroPipeline pipeline_;
    std::atomic<bool> running_{true};
};

static KokoroService* g_service = nullptr;

void signal_handler(int) {
    if (g_service) {
        std::printf("\nShutting down Kokoro service\n");
        g_service->shutdown();
    }
}

int main() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::printf("Starting Kokoro TTS Service (libtorch + espeak-ng)\n");

    KokoroService service;
    g_service = &service;

    if (!service.initialize()) {
        return 1;
    }

    service.run();

    return 0;
}
