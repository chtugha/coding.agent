// neutts-service.cpp
//
// Pipeline position: LLaMA → [NeuTTS] → OAP
//
// Alternative TTS service using NeuTTS Nano German model.
// Uses llama.cpp for backbone inference and CoreML NeuCodec for audio decoding.
// Occupies the KOKORO_SERVICE pipeline slot (ports 13140-13142).
// Only one TTS service (kokoro-service OR neutts-service) can run at a time.
//
// Inference pipeline:
//   1. espeak-ng converts text → IPA phonemes (language="de", with stress)
//   2. Build NeuTTS prompt:
//        "user: Convert the text to speech:<|TEXT_PROMPT_START|>{ref_phones} {input_phones}<|TEXT_PROMPT_END|>\nassistant:<|SPEECH_GENERATION_START|>{ref_codes_str}"
//   3. Tokenize prompt, feed to NeuTTS backbone (llama.cpp, Q4_0 GGUF)
//   4. Sample autoregressively: temperature=1.0, top_k=50
//   5. Extract speech codes from <|speech_N|> tokens
//   6. Stop at <|SPEECH_GENERATION_END|> or EOS
//   7. Decode speech codes through NeuCodec (CoreML mlmodelc) → 24kHz float32 PCM
//   8. Normalize + fade-in, send to OAP
//
// Reference voice:
//   Pre-computed codec codes (ref_codes.bin) and phonemized text (ref_text.txt)
//   are loaded at startup. These define the voice timbre and speaking style.
//
// CMD port (Kokoro base+2 = 13142): PING, STATUS, SET_LOG_LEVEL, TEST_SYNTH.
#include <espeak-ng/speak_lib.h>
#include "interconnect.h"
#include "llama.h"
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

#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>

using namespace whispertalk;

static const int NEUTTS_SAMPLE_RATE = 24000;
static const size_t MAX_AUDIO_SAMPLES = 30 * NEUTTS_SAMPLE_RATE;
static const size_t PHONEME_CACHE_MAX = 10000;

static float normalize_audio(std::vector<float>& samples, float ceiling = 0.90f) {
    float peak = 0.0f;
    for (float s : samples) {
        float a = std::abs(s);
        if (a > peak) peak = a;
    }
    if (peak > 0.01f && std::abs(peak - ceiling) > 0.001f) {
        float scale = ceiling / peak;
        for (float& s : samples) s *= scale;
    }
    return peak;
}

static void apply_fade_in(std::vector<float>& samples, int fade_samples = 48) {
    int n = std::min(fade_samples, (int)samples.size());
    for (int i = 0; i < n; i++) {
        samples[i] *= static_cast<float>(i) / static_cast<float>(n);
    }
}

#ifndef ESPEAK_NG_DATA_DIR
#define ESPEAK_NG_DATA_DIR ""
#endif

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

struct ReferenceVoice {
    std::vector<int32_t> codes;
    std::string phonemes;

    bool load(const std::string& codes_path, const std::string& text_path) {
        std::ifstream cf(codes_path, std::ios::binary);
        if (!cf.is_open()) {
            std::fprintf(stderr, "Failed to open reference codes: %s\n", codes_path.c_str());
            return false;
        }
        cf.seekg(0, std::ios::end);
        size_t file_size = cf.tellg();
        cf.seekg(0, std::ios::beg);
        size_t num_codes = file_size / sizeof(int32_t);
        codes.resize(num_codes);
        cf.read(reinterpret_cast<char*>(codes.data()), file_size);
        if (!cf.good()) {
            std::fprintf(stderr, "Failed to read reference codes\n");
            return false;
        }

        std::ifstream tf(text_path);
        if (!tf.is_open()) {
            std::fprintf(stderr, "Failed to open reference text: %s\n", text_path.c_str());
            return false;
        }
        std::getline(tf, phonemes);

        std::printf("Reference voice: %zu codes, phonemes=%s\n", codes.size(), phonemes.c_str());
        return true;
    }

    std::string codes_prompt_str() const {
        std::string result;
        for (int32_t c : codes) {
            result += "<|speech_" + std::to_string(c) + "|>";
        }
        return result;
    }
};

class CoreMLNeuCodecDecoder {
public:
    bool load(const std::string& mlmodelc_path) {
        @autoreleasepool {
            NSError *error = nil;
            NSString *path = [NSString stringWithUTF8String:mlmodelc_path.c_str()];
            NSURL *url = [NSURL fileURLWithPath:path];

            MLModelConfiguration *config = [[MLModelConfiguration alloc] init];
            config.computeUnits = MLComputeUnitsAll;

            model_ = [MLModel modelWithContentsOfURL:url configuration:config error:&error];
            if (!model_) {
                std::fprintf(stderr, "NeuCodec CoreML: Failed to load %s: %s\n",
                    mlmodelc_path.c_str(),
                    [[error localizedDescription] UTF8String]);
                return false;
            }
            [model_ retain];

            available_ = true;
            std::printf("NeuCodec CoreML loaded: %s\n", mlmodelc_path.c_str());
            return true;
        }
    }

    std::vector<float> decode(const std::vector<int32_t>& codes) {
        if (!available_ || codes.empty()) return {};

        @autoreleasepool {
            NSError *error = nil;
            int64_t T = (int64_t)codes.size();

            MLMultiArray *input = [[MLMultiArray alloc]
                initWithShape:@[@1, @1, @(T)]
                dataType:MLMultiArrayDataTypeInt32
                error:&error];
            if (!input) {
                std::fprintf(stderr, "NeuCodec: Failed to create input array: %s\n",
                    [[error localizedDescription] UTF8String]);
                return {};
            }
            std::memcpy((int32_t *)input.dataPointer, codes.data(), codes.size() * sizeof(int32_t));

            NSDictionary *feature_dict = @{@"codes": [MLFeatureValue featureValueWithMultiArray:input]};
            id<MLFeatureProvider> provider = [[MLDictionaryFeatureProvider alloc]
                initWithDictionary:feature_dict error:&error];
            if (!provider) {
                std::fprintf(stderr, "NeuCodec: Failed to create feature provider\n");
                return {};
            }

            id<MLFeatureProvider> result = [model_ predictionFromFeatures:provider error:&error];
            if (!result) {
                std::fprintf(stderr, "NeuCodec: Prediction failed: %s\n",
                    [[error localizedDescription] UTF8String]);
                return {};
            }

            MLMultiArray *output = [[result featureValueForName:@"audio"] multiArrayValue];
            if (!output) {
                std::fprintf(stderr, "NeuCodec: No output tensor\n");
                return {};
            }

            size_t n = (size_t)output.count;
            const float *src = (const float *)output.dataPointer;
            return std::vector<float>(src, src + n);
        }
    }

    bool is_available() const { return available_; }

    ~CoreMLNeuCodecDecoder() {
        if (model_) { [model_ release]; model_ = nil; }
    }

private:
    MLModel *model_ = nil;
    bool available_ = false;
};

class NeuTTSPipeline {
public:
    bool initialize(const std::string& models_dir) {
        std::string neutts_dir = models_dir + "/neutts-nano-german";
        std::string gguf_path = neutts_dir + "/neutts-nano-german-Q4_0.gguf";
        std::string codec_path = neutts_dir + "/neucodec_decoder.mlmodelc";
        std::string ref_codes_path = neutts_dir + "/ref_codes.bin";
        std::string ref_text_path = neutts_dir + "/ref_text.txt";

        if (!ref_voice_.load(ref_codes_path, ref_text_path)) {
            std::fprintf(stderr, "Failed to load reference voice\n");
            return false;
        }
        ref_codes_prompt_ = ref_voice_.codes_prompt_str();

        llama_backend_init();

        llama_model_params mparams = llama_model_default_params();
        mparams.n_gpu_layers = -1;
        model_ = llama_model_load_from_file(gguf_path.c_str(), mparams);
        if (!model_) {
            std::fprintf(stderr, "Failed to load NeuTTS backbone: %s\n", gguf_path.c_str());
            return false;
        }

        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx = 2048;
        cparams.n_threads = 4;
        cparams.n_threads_batch = 4;
        ctx_ = llama_init_from_model(model_, cparams);
        if (!ctx_) {
            std::fprintf(stderr, "Failed to initialize NeuTTS context\n");
            return false;
        }

        vocab_ = llama_model_get_vocab(model_);

        sampler_ = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(sampler_, llama_sampler_init_top_k(50));
        llama_sampler_chain_add(sampler_, llama_sampler_init_temp(1.0f));
        llama_sampler_chain_add(sampler_, llama_sampler_init_dist(42));

        std::printf("NeuTTS backbone loaded: %s\n", gguf_path.c_str());

        std::vector<llama_token> end_tokens(8);
        int n = llama_tokenize(vocab_, "<|SPEECH_GENERATION_END|>", 25,
                               end_tokens.data(), end_tokens.size(), false, true);
        if (n == 1) {
            speech_end_token_ = end_tokens[0];
            std::printf("Speech end token ID: %d\n", speech_end_token_);
        } else {
            speech_end_token_ = llama_vocab_eos(vocab_);
            std::printf("Using EOS as speech end: %d\n", speech_end_token_);
        }

        codec_decoder_ = std::make_unique<CoreMLNeuCodecDecoder>();
        if (!codec_decoder_->load(codec_path)) {
            std::fprintf(stderr, "Failed to load NeuCodec decoder: %s\n", codec_path.c_str());
            return false;
        }

        std::string espeak_data = resolve_espeak_data_dir();
        if (espeak_data.empty()) {
            std::fprintf(stderr, "Cannot find espeak-ng-data directory\n");
            return false;
        }
        std::printf("Using espeak-ng data: %s\n", espeak_data.c_str());

        int result = espeak_Initialize(AUDIO_OUTPUT_RETRIEVAL, 0, espeak_data.c_str(), 0);
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

    std::vector<float> synthesize(const std::string& text, std::atomic<bool>* interrupted = nullptr) {
        std::string input_phones = phonemize(text);
        if (input_phones.empty()) return {};

        std::string prompt = build_prompt(input_phones);

        auto tokens = tokenize(prompt, false);
        if (tokens.empty()) return {};

        llama_memory_t mem = llama_get_memory(ctx_);
        llama_memory_seq_rm(mem, 0, -1, -1);

        llama_batch batch = llama_batch_init(tokens.size(), 0, 1);
        batch.n_tokens = tokens.size();
        for (size_t i = 0; i < tokens.size(); i++) {
            batch.token[i] = tokens[i];
            batch.pos[i] = i;
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i] = (i == tokens.size() - 1);
        }

        if (llama_decode(ctx_, batch) != 0) {
            llama_batch_free(batch);
            return {};
        }
        int n_past = static_cast<int>(tokens.size());
        llama_batch_free(batch);

        std::vector<int32_t> speech_codes;
        llama_batch single = llama_batch_init(1, 0, 1);

        for (int i = 0; i < 1500; i++) {
            if (interrupted && interrupted->load()) break;

            llama_token id = llama_sampler_sample(sampler_, ctx_, -1);
            if (id == speech_end_token_ || id == llama_vocab_eos(vocab_)) break;

            int32_t code = extract_speech_code(id);
            if (code >= 0) {
                speech_codes.push_back(code);
            }

            single.n_tokens = 1;
            single.token[0] = id;
            single.pos[0] = n_past;
            single.n_seq_id[0] = 1;
            single.seq_id[0][0] = 0;
            single.logits[0] = true;

            if (llama_decode(ctx_, single) != 0) break;
            n_past++;
        }
        llama_batch_free(single);

        if (speech_codes.empty()) return {};

        std::printf("Generated %zu speech codes\n", speech_codes.size());

        auto samples = codec_decoder_->decode(speech_codes);
        if (samples.size() > MAX_AUDIO_SAMPLES) {
            samples.resize(MAX_AUDIO_SAMPLES);
        }
        return samples;
    }

    ~NeuTTSPipeline() {
        if (sampler_) llama_sampler_free(sampler_);
        if (ctx_) llama_free(ctx_);
        if (model_) llama_model_free(model_);
        llama_backend_free();
    }

private:
    std::string build_prompt(const std::string& input_phones) {
        return "user: Convert the text to speech:<|TEXT_PROMPT_START|>"
               + ref_voice_.phonemes + " " + input_phones
               + "<|TEXT_PROMPT_END|>\nassistant:<|SPEECH_GENERATION_START|>"
               + ref_codes_prompt_;
    }

    std::vector<llama_token> tokenize(const std::string& text, bool bos) {
        std::vector<llama_token> res(text.size() + 128);
        int n = llama_tokenize(vocab_, text.c_str(), text.size(),
                               res.data(), res.size(), bos, true);
        if (n < 0) {
            res.resize(-n);
            n = llama_tokenize(vocab_, text.c_str(), text.size(),
                               res.data(), res.size(), bos, true);
        }
        if (n <= 0) return {};
        res.resize(n);
        return res;
    }

    int32_t extract_speech_code(llama_token id) {
        char piece[128];
        int n = llama_token_to_piece(vocab_, id, piece, sizeof(piece), 0, true);
        if (n <= 0) return -1;

        std::string token_str(piece, n);
        if (token_str.size() > 11 &&
            token_str.substr(0, 9) == "<|speech_" &&
            token_str.back() == '>') {
            size_t end = token_str.find("|>", 9);
            if (end != std::string::npos) {
                std::string num_str = token_str.substr(9, end - 9);
                try {
                    return std::stoi(num_str);
                } catch (...) {}
            }
        }
        return -1;
    }

    struct llama_model* model_ = nullptr;
    struct llama_context* ctx_ = nullptr;
    const struct llama_vocab* vocab_ = nullptr;
    struct llama_sampler* sampler_ = nullptr;
    std::mutex espeak_mutex_;
    std::mutex cache_mutex_;
    std::unordered_map<std::string, std::string> phoneme_cache_;
    ReferenceVoice ref_voice_;
    std::string ref_codes_prompt_;
    llama_token speech_end_token_ = -1;
    std::unique_ptr<CoreMLNeuCodecDecoder> codec_decoder_;
};

struct CallContext {
    uint32_t call_id;
    std::queue<std::string> text_queue;
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::thread worker;
    std::atomic<bool> active{true};
    std::atomic<bool> interrupted{false};
};

class NeuTTSService {
public:
    NeuTTSService() : node_(ServiceType::KOKORO_SERVICE) {}

    bool initialize() {
        if (!check_tts_exclusion()) {
            std::fprintf(stderr, "Another TTS service is already running on port %d\n",
                        service_cmd_port(ServiceType::KOKORO_SERVICE));
            return false;
        }

        if (!node_.initialize()) {
            std::fprintf(stderr, "Failed to initialize interconnect node\n");
            return false;
        }

        const char* env_models = std::getenv("WHISPERTALK_MODELS_DIR");
        std::string models_dir = env_models ? env_models :
#ifdef WHISPERTALK_MODELS_DIR
            WHISPERTALK_MODELS_DIR;
#else
            "models";
#endif
        if (!pipeline_.initialize(models_dir)) {
            std::fprintf(stderr, "Failed to initialize NeuTTS pipeline\n");
            return false;
        }

        log_fwd_.init(FRONTEND_LOG_PORT, ServiceType::KOKORO_SERVICE);

        std::printf("NeuTTS Service initialized (German, NeuTTS Nano, NeuCodec CoreML)\n");

        node_.register_call_end_handler([this](uint32_t call_id) {
            handle_call_end(call_id);
        });

        node_.register_speech_signal_handler([this](uint32_t call_id, bool active) {
            if (active) {
                handle_speech_active(call_id);
            }
        });

        return true;
    }

    void run() {
        if (!node_.connect_to_downstream()) {
            std::printf("Downstream (OAP) not available yet - will auto-reconnect\n");
        }

        std::thread cmd_thread(&NeuTTSService::command_listener_loop, this);

        std::printf("NeuTTS service ready - waiting for text from LLaMA\n");

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

        int s1 = cmd_sock_.exchange(-1);
        if (s1 >= 0) ::close(s1);
        if (cmd_thread.joinable()) cmd_thread.join();
    }

    void shutdown() {
        running_ = false;
        int s2 = cmd_sock_.exchange(-1);
        if (s2 >= 0) ::close(s2);
        shutdown_all_calls();
        node_.shutdown();
    }

    void set_log_level(const char* level) {
        log_fwd_.set_level(level);
    }

private:
    bool check_tts_exclusion() {
        uint16_t cmd_port = service_cmd_port(ServiceType::KOKORO_SERVICE);
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return true;

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(cmd_port);

        struct timeval tv{1, 0};
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            const char* ping = "PING\n";
            send(sock, ping, strlen(ping), 0);
            char buf[64];
            int n = (int)recv(sock, buf, sizeof(buf) - 1, 0);
            ::close(sock);
            if (n > 0) {
                buf[n] = '\0';
                if (strstr(buf, "PONG")) {
                    return false;
                }
            }
            return false;
        }
        ::close(sock);
        return true;
    }

    void command_listener_loop() {
        uint16_t port = service_cmd_port(ServiceType::KOKORO_SERVICE);
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return;

        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);

        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::fprintf(stderr, "NeuTTS cmd: bind port %d failed\n", port);
            ::close(sock);
            return;
        }
        listen(sock, 4);
        cmd_sock_.store(sock);
        std::printf("NeuTTS command listener on port %d\n", port);

        while (running_) {
            struct pollfd pfd{sock, POLLIN, 0};
            int r = poll(&pfd, 1, 200);
            if (r <= 0) continue;

            int csock = accept(sock, nullptr, nullptr);
            if (csock < 0) continue;

            struct timeval tv{30, 0};
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
        if (cmd.rfind("TEST_SYNTH:", 0) == 0) {
            std::string text = cmd.substr(11);
            std::vector<float> samples;
            auto start = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(pipeline_mutex_);
                samples = pipeline_.synthesize(text);
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (samples.empty()) {
                return "ERROR:synthesis failed\n";
            }

            double duration_s = (double)samples.size() / NEUTTS_SAMPLE_RATE;
            double rtf = (elapsed / 1000.0) / duration_s;

            float raw_peak = 0.0f;
            for (float s : samples) {
                float a = std::abs(s);
                if (a > raw_peak) raw_peak = a;
            }

            return "SYNTH_RESULT:" + std::to_string(elapsed) + "ms:"
                + std::to_string(samples.size()) + ":" + std::to_string(NEUTTS_SAMPLE_RATE) + ":"
                + std::to_string(duration_s) + "s:rtf=" + std::to_string(rtf)
                + ":peak=" + std::to_string(raw_peak)
                + ":engine=neutts\n";
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
            {
                std::lock_guard<std::mutex> lock(pipeline_mutex_);
                samples = pipeline_.synthesize(text);
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (samples.empty()) return "ERROR:synthesis failed\n";

            normalize_audio(samples);
            apply_fade_in(samples);

            std::ofstream out(path, std::ios::binary);
            if (!out.is_open()) return "ERROR:cannot open " + path + "\n";

            uint32_t data_size = (uint32_t)(samples.size() * sizeof(int16_t));
            uint32_t file_size = 36 + data_size;
            int32_t sr = NEUTTS_SAMPLE_RATE;
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

            double duration_s = (double)samples.size() / NEUTTS_SAMPLE_RATE;
            double rtf = (elapsed / 1000.0) / duration_s;

            return "WAV_RESULT:" + std::to_string(elapsed) + "ms:"
                + std::to_string(samples.size()) + ":" + std::to_string(NEUTTS_SAMPLE_RATE) + ":"
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
                + ":UPSTREAM:" + (node_.upstream_state() == ConnectionState::CONNECTED ? "connected" : "disconnected")
                + ":DOWNSTREAM:" + (node_.downstream_state() == ConnectionState::CONNECTED ? "connected" : "disconnected")
                + ":ENGINE:neutts-nano-german"
                + "\n";
        }
        return "ERROR:Unknown command\n";
    }

    void dispatch_text_packet(const Packet& pkt) {
        std::string text(reinterpret_cast<const char*>(pkt.payload.data()), pkt.payload.size());

        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto it = calls_.find(pkt.call_id);
        if (it == calls_.end()) {
            auto ctx = std::make_shared<CallContext>();
            ctx->call_id = pkt.call_id;
            ctx->worker = std::thread(&NeuTTSService::call_worker, this, ctx);
            calls_[pkt.call_id] = ctx;
            std::printf("Started synthesis thread for call %u\n", pkt.call_id);
            log_fwd_.forward(LogLevel::INFO, pkt.call_id, "Started NeuTTS synthesis thread");
            it = calls_.find(pkt.call_id);
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
                ctx->queue_cv.wait_for(lock, std::chrono::milliseconds(500),
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

            std::printf("Synthesizing for call %u: %s\n", ctx->call_id, text.c_str());

            std::vector<float> samples;
            auto start = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(pipeline_mutex_);
                samples = pipeline_.synthesize(text, &ctx->interrupted);
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (ctx->interrupted.load()) {
                ctx->interrupted = false;
                std::printf("Synthesis interrupted for call %u — discarding %zu samples\n",
                           ctx->call_id, samples.size());
                log_fwd_.forward(LogLevel::WARN, ctx->call_id, "Synthesis interrupted — discarding audio");
                continue;
            }

            if (samples.empty() || samples.size() > MAX_AUDIO_SAMPLES) {
                std::fprintf(stderr, "Invalid audio output for call %u: %zu samples\n",
                            ctx->call_id, samples.size());
                continue;
            }

            float raw_peak = normalize_audio(samples);
            apply_fade_in(samples);

            const char* norm_tag = (raw_peak > 0.01f && std::abs(raw_peak - 0.90f) > 0.001f)
                                   ? " -> normalized" : "";
            std::printf("Synthesized %zu samples in %lldms for call %u (raw_peak=%.3f%s)\n",
                        samples.size(), (long long)elapsed, ctx->call_id, raw_peak, norm_tag);
            log_fwd_.forward(LogLevel::INFO, ctx->call_id, "Synthesized %zu samples in %lldms (NeuTTS)",
                            samples.size(), (long long)elapsed);

            send_audio_to_downstream(ctx->call_id, samples);
        }
    }

    void send_audio_to_downstream(uint32_t call_id, const std::vector<float>& samples) {
        if (node_.downstream_state() != ConnectionState::CONNECTED) {
            std::printf("Downstream (OAP) not connected - discarding audio for call %u\n", call_id);
            return;
        }

        static constexpr size_t CHUNK_SAMPLES = 4800;
        size_t header_size = sizeof(int32_t);
        size_t total_sent = 0;

        for (size_t offset = 0; offset < samples.size(); offset += CHUNK_SAMPLES) {
            size_t count = std::min(CHUNK_SAMPLES, samples.size() - offset);

            Packet audio_pkt;
            audio_pkt.call_id = call_id;
            audio_pkt.payload_size = static_cast<uint32_t>(header_size + count * sizeof(float));
            audio_pkt.payload.resize(audio_pkt.payload_size);

            int32_t sr = NEUTTS_SAMPLE_RATE;
            std::memcpy(audio_pkt.payload.data(), &sr, sizeof(int32_t));
            std::memcpy(audio_pkt.payload.data() + header_size,
                       samples.data() + offset, count * sizeof(float));

            audio_pkt.trace.record(ServiceType::KOKORO_SERVICE, 0);
            audio_pkt.trace.record(ServiceType::KOKORO_SERVICE, 1);
            if (node_.send_to_downstream(audio_pkt)) {
                total_sent += count;
            } else {
                std::fprintf(stderr, "Failed to send audio chunk for call %u at offset %zu\n", call_id, offset);
                log_fwd_.forward(LogLevel::ERROR, call_id, "Failed to send audio chunk to OAP");
                break;
            }
        }

        if (total_sent > 0) {
            std::printf("Sent %zu samples @ %d Hz for call %u to OAP (%zu chunks)\n",
                       total_sent, NEUTTS_SAMPLE_RATE, call_id,
                       (total_sent + CHUNK_SAMPLES - 1) / CHUNK_SAMPLES);
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
    LogForwarder log_fwd_;
    NeuTTSPipeline pipeline_;
    std::mutex pipeline_mutex_;
    std::atomic<bool> running_{true};
    std::atomic<int> cmd_sock_{-1};
    std::map<uint32_t, std::shared_ptr<CallContext>> calls_;
    std::mutex calls_mutex_;
};

static NeuTTSService* g_service = nullptr;

void signal_handler(int) {
    if (g_service) {
        std::printf("\nShutting down NeuTTS service\n");
        g_service->shutdown();
    }
}

int main(int argc, char* argv[]) {
    setlinebuf(stdout);
    setlinebuf(stderr);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::string log_level = "INFO";

    static struct option long_opts[] = {
        {"log-level", required_argument, 0, 'L'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "L:h", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'L': log_level = optarg; break;
            case 'h':
                std::printf("Usage: neutts-service [OPTIONS]\n");
                std::printf("  --log-level LEVEL Log level: ERROR WARN INFO DEBUG TRACE (default: INFO)\n");
                std::printf("\nAlternative TTS engine using NeuTTS Nano German + NeuCodec\n");
                std::printf("Occupies the same pipeline slot as kokoro-service (mutually exclusive)\n");
                return 0;
            default: break;
        }
    }

    std::printf("Starting NeuTTS Service (NeuTTS Nano German, NeuCodec CoreML)\n");

    NeuTTSService service;
    g_service = &service;

    if (!service.initialize()) {
        return 1;
    }

    service.set_log_level(log_level.c_str());
    service.run();

    return 0;
}
