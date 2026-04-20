// neutts-service.cpp
//
// Pipeline position: LLaMA → [TTS dock] → (engine: NeuTTS) → [TTS dock] → OAP
//
// Alternative TTS engine using NeuTTS Nano German model. Connects to the
// generic TTS dock (`tts-service`) via the EngineClient hotplug protocol
// instead of being a pipeline node itself. Last engine to dock wins; the
// dock arbitrates between kokoro/neutts/future engines.
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
//   8. Normalize + fade-in, send via EngineClient to TTS dock → OAP
//
// Reference voice:
//   Pre-computed codec codes (ref_codes.bin) and phonemized text (ref_text.txt)
//   are loaded at startup. These define the voice timbre and speaking style.
//
// CMD port (NeuTTS engine diagnostic port 13174): PING, STATUS, SET_LOG_LEVEL,
//   TEST_SYNTH, SYNTH_WAV. Separate from the TTS dock's cmd port (13142).
#include <espeak-ng/speak_lib.h>
#include "interconnect.h"
#include "tts-engine-client.h"
#include "tts-common.h"
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
#include <unordered_map>
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

// Shared TTS audio constants (single source of truth: tts-common.h).
static constexpr int NEUTTS_SAMPLE_RATE = static_cast<int>(whispertalk::tts::kTTSSampleRate);
// Diagnostic cmd port for the NeuTTS engine (see spec §4.2). Separate from
// the TTS dock's own cmd port (13142) so operators can query the engine
// process directly without going through the dock.
static constexpr uint16_t NEUTTS_ENGINE_CMD_PORT = whispertalk::tts::kNeuTTSEngineCmdPort;
// Hard upper bound on a single synthesis output (per call, per text). 120 s
// covers worst-case long LLaMA responses; the synthesis loop truncates
// gracefully when it would exceed this ceiling.
static constexpr size_t MAX_AUDIO_SAMPLES = 120 * NEUTTS_SAMPLE_RATE;
static constexpr size_t PHONEME_CACHE_MAX = 10000;

static constexpr int MODEL_CONTEXT_SIZE = 2048;
static constexpr int MODEL_N_THREADS = 4;
static constexpr int MODEL_N_THREADS_BATCH = 8;
static constexpr int SAMPLER_TOP_K = 50;
static constexpr float SAMPLER_TEMPERATURE = 1.0f;
static constexpr uint32_t SAMPLER_SEED = 42;
static constexpr int MAX_GENERATION_TOKENS = 1500;
static constexpr int FIRST_BATCH_CODES = 16;
static constexpr int STREAM_BATCH_CODES = 64;
static constexpr size_t DOWNSTREAM_CHUNK_SAMPLES = whispertalk::tts::kTTSMaxFrameSamples;
static constexpr float AUDIO_CEILING = 0.90f;
static constexpr int CMD_RECV_TIMEOUT_SEC = 30;
static constexpr int CMD_POLL_TIMEOUT_MS = 200;
static constexpr int WORKER_WAIT_TIMEOUT_MS = 500;
static constexpr size_t CMD_BUF_SIZE = 4096;

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
        result.reserve(codes.size() * 16);
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

        static constexpr int64_t COMPILED_T       = 256;     // only input shape that works (mlmodelc output has no shape flexibility)
        static constexpr int64_t COMPILED_SAMPLES = 122400;  // model output shape [1,1,122400] = 480 * (256-1)
        static constexpr int64_t SAMPLES_PER_CODE = COMPILED_SAMPLES / (COMPILED_T - 1);  // = 480

        int64_t actual_T = (int64_t)codes.size();
        if (actual_T > COMPILED_T) {
            std::fprintf(stderr, "NeuCodec: input codes %lld exceed compiled limit %lld, truncating\n",
                (long long)codes.size(), (long long)COMPILED_T);
            actual_T = COMPILED_T;
        }

        @autoreleasepool {
            NSError *error = nil;

            MLMultiArray *input = [[MLMultiArray alloc]
                initWithShape:@[@1, @1, @(COMPILED_T)]
                dataType:MLMultiArrayDataTypeInt32
                error:&error];
            if (!input) {
                std::fprintf(stderr, "NeuCodec: Failed to create input array: %s\n",
                    [[error localizedDescription] UTF8String]);
                return {};
            }
            int32_t *dst = (int32_t *)input.dataPointer;
            std::memcpy(dst, codes.data(), actual_T * sizeof(int32_t));
            std::memset(dst + actual_T, 0, (COMPILED_T - actual_T) * sizeof(int32_t));

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
            size_t actual_samples = (size_t)(SAMPLES_PER_CODE * (actual_T - 1));
            if (actual_samples < n) n = actual_samples;
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
        cparams.n_ctx = MODEL_CONTEXT_SIZE;
        cparams.n_threads = MODEL_N_THREADS;
        cparams.n_threads_batch = MODEL_N_THREADS_BATCH;
        ctx_ = llama_init_from_model(model_, cparams);
        if (!ctx_) {
            std::fprintf(stderr, "Failed to initialize NeuTTS context\n");
            return false;
        }

        vocab_ = llama_model_get_vocab(model_);

        sampler_ = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(sampler_, llama_sampler_init_top_k(SAMPLER_TOP_K));
        llama_sampler_chain_add(sampler_, llama_sampler_init_temp(SAMPLER_TEMPERATURE));
        llama_sampler_chain_add(sampler_, llama_sampler_init_dist(SAMPLER_SEED));

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

        std::string espeak_data = tts::resolve_espeak_data_dir();
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

        build_speech_code_lut();

        std::printf("Warming up Metal shaders and CoreML (first synthesis)...\n");
        auto warmup_start = std::chrono::steady_clock::now();
        auto warmup = synthesize("Hallo.");
        auto warmup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - warmup_start).count();
        if (warmup.empty()) {
            std::fprintf(stderr, "Warmup synthesis failed — first real call may be slow\n");
        } else {
            std::printf("Warmup done in %lldms (%zu samples)\n", (long long)warmup_ms, warmup.size());
        }

        pin_prefix();

        return true;
    }

    std::string phonemize(const std::string& text) {
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = phoneme_cache_.find(text);
            if (it != phoneme_cache_.end()) return it->second;
        }

        std::string result;
        result.reserve(text.size() * 2);
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
                auto it = phoneme_cache_.begin();
                for (size_t i = 0; i < PHONEME_CACHE_MAX / 2 && it != phoneme_cache_.end(); ++i) {
                    it = phoneme_cache_.erase(it);
                }
            }
            phoneme_cache_[text] = result;
        }

        return result;
    }

    std::vector<float> synthesize(const std::string& text, std::atomic<bool>* interrupted = nullptr) {
        std::vector<float> result;
        result.reserve(NEUTTS_SAMPLE_RATE * 4);
        synthesize_streaming(text, interrupted, [&](std::vector<float> chunk) {
            result.insert(result.end(), chunk.begin(), chunk.end());
        });
        return result;
    }

    template<typename Callback>
    void synthesize_streaming(const std::string& text, std::atomic<bool>* interrupted,
                              Callback&& callback) {
        std::string input_phones = phonemize(text);
        if (input_phones.empty()) return;

        llama_memory_t mem = llama_get_memory(ctx_);
        std::vector<llama_token> decode_tokens;
        int n_past;

        if (prefix_n_past_ > 0 && !suffix_delim_tokens_.empty()) {
            auto phone_tokens = tokenize(input_phones, false);
            if (phone_tokens.empty()) return;
            decode_tokens.reserve(phone_tokens.size() + suffix_delim_tokens_.size() + ref_codes_tokens_.size());
            decode_tokens.insert(decode_tokens.end(), phone_tokens.begin(), phone_tokens.end());
            decode_tokens.insert(decode_tokens.end(), suffix_delim_tokens_.begin(), suffix_delim_tokens_.end());
            decode_tokens.insert(decode_tokens.end(), ref_codes_tokens_.begin(), ref_codes_tokens_.end());
            llama_memory_seq_rm(mem, 0, prefix_n_past_, -1);
            n_past = prefix_n_past_;
        } else {
            std::string prompt = build_prompt(input_phones);
            decode_tokens = tokenize(prompt, false);
            if (decode_tokens.empty()) return;
            llama_memory_seq_rm(mem, 0, -1, -1);
            n_past = 0;
        }

        llama_batch batch = llama_batch_init(decode_tokens.size(), 0, 1);
        batch.n_tokens = decode_tokens.size();
        for (size_t i = 0; i < decode_tokens.size(); i++) {
            batch.token[i] = decode_tokens[i];
            batch.pos[i] = n_past + i;
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i] = (i == decode_tokens.size() - 1);
        }
        if (llama_decode(ctx_, batch) != 0) {
            llama_batch_free(batch);
            return;
        }
        n_past += static_cast<int>(decode_tokens.size());
        llama_batch_free(batch);

        llama_batch single = llama_batch_init(1, 0, 1);
        std::vector<int32_t> pending_codes;
        pending_codes.reserve(STREAM_BATCH_CODES + 1);
        size_t total_samples = 0;
        int32_t context_code = -1;
        int batch_target = FIRST_BATCH_CODES;

        std::vector<int32_t> input_codes;
        input_codes.reserve(STREAM_BATCH_CODES + 1);

        auto flush_batch = [&]() {
            if (pending_codes.empty()) return;

            input_codes.clear();
            if (context_code >= 0) {
                input_codes.push_back(context_code);
            }
            input_codes.insert(input_codes.end(), pending_codes.begin(), pending_codes.end());

            context_code = pending_codes.back();
            pending_codes.clear();

            auto chunk = codec_decoder_->decode(input_codes);
            if (chunk.empty()) return;
            if (total_samples + chunk.size() > MAX_AUDIO_SAMPLES) {
                size_t allowed = MAX_AUDIO_SAMPLES - total_samples;
                if (allowed == 0) return;
                chunk.resize(allowed);
            }
            total_samples += chunk.size();
            callback(std::move(chunk));
            batch_target = STREAM_BATCH_CODES;
        };

        const llama_token eos = llama_vocab_eos(vocab_);
        int gen_limit = std::min(MAX_GENERATION_TOKENS, MODEL_CONTEXT_SIZE - n_past);
        if (gen_limit <= 0) {
            std::fprintf(stderr, "NeuTTS: context window exhausted (n_past=%d/%d), cannot generate\n",
                n_past, MODEL_CONTEXT_SIZE);
            llama_batch_free(single);
            return;
        }
        for (int i = 0; i < gen_limit; i++) {
            if (interrupted && interrupted->load()) break;
            if (total_samples >= MAX_AUDIO_SAMPLES) break;

            llama_token id = llama_sampler_sample(sampler_, ctx_, -1);
            if (id == speech_end_token_ || id == eos) break;

            int32_t code = lookup_speech_code(id);
            if (code >= 0) {
                pending_codes.push_back(code);
                if ((int)pending_codes.size() >= batch_target) {
                    flush_batch();
                    if (interrupted && interrupted->load()) break;
                }
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

        if (!(interrupted && interrupted->load())) {
            flush_batch();
        }
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

    int32_t lookup_speech_code(llama_token id) const {
        auto it = speech_code_lut_.find(id);
        return it != speech_code_lut_.end() ? it->second : -1;
    }

    void build_speech_code_lut() {
        int n_vocab = llama_vocab_n_tokens(vocab_);
        for (llama_token id = 0; id < n_vocab; id++) {
            int32_t code = extract_speech_code(id);
            if (code >= 0) speech_code_lut_[id] = code;
        }
        std::printf("Built speech code LUT: %zu entries\n", speech_code_lut_.size());
    }

    bool pin_prefix() {
        std::string prefix_str = "user: Convert the text to speech:<|TEXT_PROMPT_START|>"
                                + ref_voice_.phonemes + " ";
        prefix_tokens_ = tokenize(prefix_str, false);
        if (prefix_tokens_.empty()) {
            std::fprintf(stderr, "Failed to tokenize prefix for KV cache pinning\n");
            return false;
        }

        suffix_delim_tokens_ = tokenize("<|TEXT_PROMPT_END|>\nassistant:<|SPEECH_GENERATION_START|>", false);
        ref_codes_tokens_ = tokenize(ref_codes_prompt_, false);
        std::printf("Pre-tokenized suffix: delim=%zu tokens, ref_codes=%zu tokens\n",
                    suffix_delim_tokens_.size(), ref_codes_tokens_.size());

        llama_memory_t mem = llama_get_memory(ctx_);
        llama_memory_seq_rm(mem, 0, -1, -1);

        llama_batch batch = llama_batch_init(prefix_tokens_.size(), 0, 1);
        batch.n_tokens = prefix_tokens_.size();
        for (size_t i = 0; i < prefix_tokens_.size(); i++) {
            batch.token[i] = prefix_tokens_[i];
            batch.pos[i] = i;
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = 0;
            batch.logits[i] = (i == prefix_tokens_.size() - 1);
        }
        if (llama_decode(ctx_, batch) != 0) {
            llama_batch_free(batch);
            std::fprintf(stderr, "Failed to decode prefix for KV cache pinning\n");
            prefix_tokens_.clear();
            prefix_n_past_ = 0;
            return false;
        }
        prefix_n_past_ = static_cast<int>(prefix_tokens_.size());
        llama_batch_free(batch);
        std::printf("Pinned KV cache prefix: %d tokens\n", prefix_n_past_);
        return true;
    }

    struct llama_model* model_ = nullptr;
    struct llama_context* ctx_ = nullptr;
    const struct llama_vocab* vocab_ = nullptr;
    struct llama_sampler* sampler_ = nullptr;
    std::mutex espeak_mutex_;
    std::mutex cache_mutex_;
    std::unordered_map<std::string, std::string> phoneme_cache_;
    std::unordered_map<llama_token, int32_t> speech_code_lut_;
    std::vector<llama_token> prefix_tokens_;
    std::vector<llama_token> suffix_delim_tokens_;
    std::vector<llama_token> ref_codes_tokens_;
    int prefix_n_past_ = 0;
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

    std::queue<std::vector<float>> audio_queue;
    std::mutex audio_mutex;
    std::condition_variable audio_cv;
    std::thread audio_sender;
};

class NeuTTSService {
public:
    NeuTTSService() = default;

    bool initialize() {
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

        log_fwd_.init(FRONTEND_LOG_PORT, ServiceType::TTS_SERVICE);

        std::printf("NeuTTS Service initialized (German, NeuTTS Nano, NeuCodec CoreML)\n");

        engine_.set_name("neutts");
        EngineAudioFormat fmt;
        fmt.sample_rate = NEUTTS_SAMPLE_RATE;
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
            std::fprintf(stderr, "[neutts] received SHUTDOWN from TTS dock — signalling exit\n");
            // SHUTDOWN is dispatched from the EngineClient's own recv thread.
            // Calling engine_.shutdown() here would self-join and deadlock.
            // Just clear running_; the main recv loop in run() will exit and
            // perform an orderly shutdown (flush LogForwarder, join workers,
            // call engine_.shutdown() from outside the recv thread).
            running_.store(false);
        });

        if (!engine_.start()) {
            std::fprintf(stderr, "Failed to start TTS engine client\n");
            return false;
        }

        return true;
    }

    void run() {
        std::thread cmd_thread(&NeuTTSService::command_listener_loop, this);

        std::printf("NeuTTS service ready - connecting to TTS dock at 127.0.0.1:%u\n",
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
        uint16_t port = NEUTTS_ENGINE_CMD_PORT;
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

            tts::normalize_audio(samples);
            tts::apply_fade_in(samples);

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
                + ":DOCK:" + (engine_.is_connected() ? "connected" : "disconnected")
                + ":ENGINE:neutts-nano-german"
                + "\n";
        }
        return "ERROR:Unknown command\n";
    }

    void prewarm_call(uint32_t call_id) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto [it, inserted] = calls_.try_emplace(call_id, nullptr);
        if (inserted) {
            auto ctx = std::make_shared<CallContext>();
            ctx->call_id = call_id;
            ctx->worker = std::thread(&NeuTTSService::call_worker, this, ctx);
            ctx->audio_sender = std::thread(&NeuTTSService::audio_sender_loop, this, ctx);
            it->second = ctx;
            log_fwd_.forward(LogLevel::DEBUG, call_id, "Prewarmed NeuTTS synthesis thread on SPEECH_IDLE");
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
            ctx->worker = std::thread(&NeuTTSService::call_worker, this, ctx);
            ctx->audio_sender = std::thread(&NeuTTSService::audio_sender_loop, this, ctx);
            it->second = ctx;
            std::printf("Started synthesis thread for call %u\n", pkt.call_id);
            log_fwd_.forward(LogLevel::INFO, pkt.call_id, "Started NeuTTS synthesis thread");
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

            std::printf("Synthesizing for call %u: %s\n", ctx->call_id, text.c_str());

            size_t chunks_produced = 0;
            auto start = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(pipeline_mutex_);
                bool first_chunk = true;
                pipeline_.synthesize_streaming(text, &ctx->interrupted,
                    [&](std::vector<float> chunk) {
                        if (ctx->interrupted.load()) return;
                        float peak = 0.0f;
                        for (float s : chunk) {
                            float a = std::abs(s);
                            if (a > peak) peak = a;
                        }
                        if (peak > AUDIO_CEILING) {
                            float scale = AUDIO_CEILING / peak;
                            for (float& s : chunk) s *= scale;
                        }
                        if (first_chunk) {
                            tts::apply_fade_in(chunk);
                            first_chunk = false;
                        }
                        {
                            std::lock_guard<std::mutex> alock(ctx->audio_mutex);
                            if (ctx->interrupted.load()) return;
                            ctx->audio_queue.push(std::move(chunk));
                        }
                        ctx->audio_cv.notify_one();
                        chunks_produced++;
                    });
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (ctx->interrupted.load()) {
                ctx->interrupted = false;
                std::printf("Synthesis interrupted for call %u\n", ctx->call_id);
                log_fwd_.forward(LogLevel::WARN, ctx->call_id, "Synthesis interrupted");
                continue;
            }

            if (chunks_produced == 0) {
                std::fprintf(stderr, "No audio output for call %u\n", ctx->call_id);
                continue;
            }

            std::printf("Synthesis complete for call %u in %lldms (%zu chunks)\n",
                        ctx->call_id, (long long)elapsed, chunks_produced);
            log_fwd_.forward(LogLevel::INFO, ctx->call_id, "Synthesis complete in %lldms (NeuTTS streaming)",
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

            int32_t sr = NEUTTS_SAMPLE_RATE;
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
                std::printf("Connects to the TTS dock via EngineClient (last engine to dock wins)\n");
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
