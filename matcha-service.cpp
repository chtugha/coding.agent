// matcha-service.cpp
//
// Pipeline position: LLaMA → [TTS dock] → (engine: Matcha-TTS) → [TTS dock] → OAP
//
// TTS engine using Matcha-TTS CoreML models. Connects to the generic TTS dock
// (`tts-service`) via the EngineClient hotplug protocol.
//
// Inference pipeline:
//   1. Phonemize text via espeak-ng (language-aware) + optional NeuralG2P for German
//   2. Tokenize IPA phonemes using vocab.json
//   3. Run matcha_encoder.mlmodelc → mu (mean), mask
//   4. Sample Gaussian noise z_0 with per-call deterministic seed (std::mt19937)
//   5. Run matcha_flow_{3s,5s,10s}.mlmodelc (baked 10-step Euler ODE) → mel spectrogram
//   6. Run matcha_vocoder.mlmodelc (HiFi-GAN) → waveform
//   7. Normalize, chunk to ≤ kTTSMaxFrameSamples, send via EngineClient → OAP
//
// Bucket selection: based on phoneme token count
//   < 50 tokens  → 3s bucket
//   < 100 tokens → 5s bucket
//   ≥ 100 tokens → 10s bucket
//
// Model layout: $WHISPERTALK_MODELS_DIR/matcha-german/coreml/
//   matcha_encoder.mlmodelc  — text encoder
//   matcha_flow_3s.mlmodelc  — baked ODE flow (3s bucket)
//   matcha_flow_5s.mlmodelc  — baked ODE flow (5s bucket)
//   matcha_flow_10s.mlmodelc — baked ODE flow (10s bucket)
//   matcha_vocoder.mlmodelc  — HiFi-GAN vocoder (mel → waveform)
//   vocab.json               — phoneme-to-ID mapping
//
// CMD port (Matcha engine diagnostic port 13176): PING, STATUS, SET_LOG_LEVEL,
//   TEST_SYNTH, SYNTH_WAV. Separate from the TTS dock's cmd port (13142).

#include <espeak-ng/speak_lib.h>
#include "interconnect.h"
#include "tts-engine-client.h"
#include "tts-common.h"
#include "neural-g2p.h"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <queue>
#include <random>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#include <getopt.h>
#include <cmath>

#ifdef __APPLE__
#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>
#endif

using namespace whispertalk;

static constexpr int MATCHA_SAMPLE_RATE = static_cast<int>(whispertalk::tts::kTTSSampleRate);
static constexpr uint16_t MATCHA_ENGINE_CMD_PORT = whispertalk::tts::kMatchaEngineCmdPort;
static constexpr size_t DOWNSTREAM_CHUNK_SAMPLES = whispertalk::tts::kTTSMaxFrameSamples;
static constexpr size_t PHONEME_CACHE_MAX = 10000;
static constexpr int CMD_RECV_TIMEOUT_SEC = 30;
static constexpr int CMD_POLL_TIMEOUT_MS = 200;
static constexpr int WORKER_WAIT_TIMEOUT_MS = 500;
static constexpr size_t CMD_BUF_SIZE = 4096;

// Mel spectrogram constants (standard Matcha-TTS / HiFi-GAN config)
static constexpr int MEL_BINS = 80;
// Bucket frame counts (T_mel frames per bucket)
// At hop_size=256 and 24kHz: 3s→281 frames, 5s→469 frames, 10s→938 frames
static constexpr int BUCKET_3S_FRAMES  = 281;
static constexpr int BUCKET_5S_FRAMES  = 469;
static constexpr int BUCKET_10S_FRAMES = 938;

// Token count thresholds for bucket selection
static constexpr int BUCKET_3S_MAX_TOKENS  = 50;
static constexpr int BUCKET_5S_MAX_TOKENS  = 100;

#ifdef __APPLE__

// Helper: load a CoreML model from path, configured to use all compute units.
static MLModel* load_coreml_model(const std::string& path) {
    @autoreleasepool {
        NSError* err = nil;
        NSString* ns_path = [NSString stringWithUTF8String:path.c_str()];
        NSURL* url = [NSURL fileURLWithPath:ns_path];
        MLModelConfiguration* cfg = [[MLModelConfiguration alloc] init];
        cfg.computeUnits = MLComputeUnitsAll;
        MLModel* model = [MLModel modelWithContentsOfURL:url configuration:cfg error:&err];
        if (!model) {
            std::fprintf(stderr, "[matcha] CoreML load failed: %s — %s\n",
                path.c_str(), [[err localizedDescription] UTF8String]);
            return nil;
        }
        [model retain];
        return model;
    }
}

// Helper: create an int32 MLMultiArray from a vector.
static MLMultiArray* make_int32_array(const std::vector<int32_t>& data,
                                      NSArray<NSNumber*>* shape) {
    NSError* err = nil;
    MLMultiArray* arr = [[MLMultiArray alloc]
        initWithShape:shape dataType:MLMultiArrayDataTypeInt32 error:&err];
    if (!arr) return nil;
    int32_t* dst = (int32_t*)arr.dataPointer;
    std::memcpy(dst, data.data(), data.size() * sizeof(int32_t));
    return arr;
}

// Helper: create a float32 MLMultiArray from raw pointer + element count.
static MLMultiArray* make_float32_array(const float* src, int64_t count,
                                        NSArray<NSNumber*>* shape) {
    NSError* err = nil;
    MLMultiArray* arr = [[MLMultiArray alloc]
        initWithShape:shape dataType:MLMultiArrayDataTypeFloat32 error:&err];
    if (!arr) return nil;
    float* dst = (float*)arr.dataPointer;
    std::memcpy(dst, src, count * sizeof(float));
    return arr;
}

#endif // __APPLE__

// Load vocab.json: maps phoneme symbol strings to integer token IDs.
// Expected format: {"<pad>": 0, "a": 1, "b": 2, ...}
static std::unordered_map<std::string, int32_t> load_vocab_json(const std::string& path) {
    std::unordered_map<std::string, int32_t> vocab;
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "[matcha] Cannot open vocab.json: %s\n", path.c_str());
        return vocab;
    }

    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
    f.close();

    // Minimal JSON parser for {"key": int, ...}
    size_t pos = 0;
    while (pos < content.size()) {
        // Find opening quote of key
        size_t key_start = content.find('"', pos);
        if (key_start == std::string::npos) break;
        size_t key_end = content.find('"', key_start + 1);
        if (key_end == std::string::npos) break;
        std::string key = content.substr(key_start + 1, key_end - key_start - 1);

        // Find colon
        size_t colon = content.find(':', key_end + 1);
        if (colon == std::string::npos) break;

        // Parse integer value
        size_t val_pos = colon + 1;
        while (val_pos < content.size() && (content[val_pos] == ' ' || content[val_pos] == '\t'))
            val_pos++;

        if (val_pos < content.size() && (std::isdigit(content[val_pos]) || content[val_pos] == '-')) {
            try {
                size_t consumed = 0;
                int32_t val = (int32_t)std::stol(content.substr(val_pos), &consumed);
                vocab[key] = val;
                pos = val_pos + consumed;
            } catch (...) {
                pos = val_pos + 1;
            }
        } else {
            pos = key_end + 1;
        }
    }
    return vocab;
}

// Tokenize an IPA phoneme string into a vector of token IDs.
// Tries multi-character tokens first (greedy longest match), then single chars.
// Silently skips symbols not in vocab.
static std::vector<int32_t> phonemes_to_ids(
    const std::string& phonemes,
    const std::unordered_map<std::string, int32_t>& vocab,
    int32_t pad_id = 0)
{
    std::vector<int32_t> ids;
    size_t i = 0;
    while (i < phonemes.size()) {
        // Try lengths 4, 3, 2, 1 bytes (greedy)
        bool matched = false;
        for (int len = 4; len >= 1 && !matched; len--) {
            if (i + len <= phonemes.size()) {
                std::string sym = phonemes.substr(i, len);
                auto it = vocab.find(sym);
                if (it != vocab.end()) {
                    ids.push_back(it->second);
                    i += len;
                    matched = true;
                }
            }
        }
        if (!matched) {
            // Advance by the full UTF-8 codepoint width to avoid slicing
            // multi-byte sequences and misaligning subsequent lookups.
            unsigned char lead = (unsigned char)phonemes[i];
            i += (lead < 0x80u) ? 1u : (lead < 0xE0u) ? 2u : (lead < 0xF0u) ? 3u : 4u;
        }
    }
    (void)pad_id;
    return ids;
}

struct FlowBucket {
    std::string name;
    int frames;
#ifdef __APPLE__
    MLModel* model = nil;
#endif
};

class MatchaPipeline {
public:
    ~MatchaPipeline() {
#ifdef __APPLE__
        if (encoder_model_) { [encoder_model_ release]; encoder_model_ = nil; }
        if (vocoder_model_) { [vocoder_model_ release]; vocoder_model_ = nil; }
        for (auto& b : flow_buckets_) {
            if (b.model) { [b.model release]; b.model = nil; }
        }
#endif
    }

    bool initialize(const std::string& models_dir, const std::string& voice_name,
                    G2PBackend g2p_backend) {
        g2p_backend_ = g2p_backend;

        std::string matcha_dir = models_dir + "/matcha-german";
        std::string coreml_dir = matcha_dir + "/coreml";
        coreml_dir_ = coreml_dir;
        std::string vocab_path = matcha_dir + "/vocab.json";
        (void)voice_name;

        struct stat st;
        if (stat(coreml_dir.c_str(), &st) != 0) {
            std::fprintf(stderr, "[matcha] Model directory not found: %s\n", coreml_dir.c_str());
            std::fprintf(stderr, "[matcha] Run scripts/export_matcha_models.py to export models\n");
            return false;
        }

        vocab_ = load_vocab_json(vocab_path);
        if (vocab_.empty()) {
            std::fprintf(stderr, "[matcha] Vocab empty or not found: %s\n", vocab_path.c_str());
            return false;
        }
        pad_id_ = 0;
        auto it = vocab_.find("<pad>");
        if (it != vocab_.end()) pad_id_ = it->second;
        std::printf("[matcha] Vocab loaded: %zu entries\n", vocab_.size());

#ifdef __APPLE__
        std::string enc_path = coreml_dir + "/matcha_encoder.mlmodelc";
        encoder_model_ = load_coreml_model(enc_path);
        if (!encoder_model_) {
            std::fprintf(stderr, "[matcha] Failed to load encoder: %s\n", enc_path.c_str());
            return false;
        }
        std::printf("[matcha] Encoder loaded: %s\n", enc_path.c_str());

        std::string voc_path = coreml_dir + "/matcha_vocoder.mlmodelc";
        vocoder_model_ = load_coreml_model(voc_path);
        if (!vocoder_model_) {
            std::fprintf(stderr, "[matcha] Failed to load vocoder: %s\n", voc_path.c_str());
            return false;
        }
        std::printf("[matcha] Vocoder loaded: %s\n", voc_path.c_str());

        flow_buckets_ = {
            {"3s",  BUCKET_3S_FRAMES,  nil},
            {"5s",  BUCKET_5S_FRAMES,  nil},
            {"10s", BUCKET_10S_FRAMES, nil},
        };
        for (auto& b : flow_buckets_) {
            std::string flow_path = coreml_dir + "/matcha_flow_" + b.name + ".mlmodelc";
            b.model = load_coreml_model(flow_path);
            if (!b.model) {
                std::fprintf(stderr, "[matcha] Failed to load flow model: %s\n", flow_path.c_str());
                return false;
            }
            std::printf("[matcha] Flow model loaded: %s\n", flow_path.c_str());
        }
#else
        std::fprintf(stderr, "[matcha] CoreML not available on non-Apple platform\n");
        return false;
#endif

        std::string espeak_data = tts::resolve_espeak_data_dir();
        if (espeak_data.empty()) {
            std::fprintf(stderr, "[matcha] Cannot find espeak-ng-data directory\n");
            return false;
        }
        std::printf("[matcha] Using espeak-ng data: %s\n", espeak_data.c_str());

        int rc = espeak_Initialize(AUDIO_OUTPUT_RETRIEVAL, 0, espeak_data.c_str(), 0);
        if (rc == -1) {
            std::fprintf(stderr, "[matcha] Failed to initialize espeak-ng\n");
            return false;
        }
        espeak_SetVoiceByName("de");
        std::printf("[matcha] espeak-ng initialized (German)\n");

#ifdef __APPLE__
        const char* env_models = std::getenv("WHISPERTALK_MODELS_DIR");
        std::string g2p_dir = (env_models ? std::string(env_models) : models_dir) + "/g2p";
        std::string g2p_model = g2p_dir + "/de_g2p.mlmodelc";
        struct stat g2p_st;
        if (stat(g2p_model.c_str(), &g2p_st) == 0) {
            neural_g2p_ = std::make_unique<NeuralG2P>();
            if (!neural_g2p_->load(g2p_model)) {
                std::fprintf(stderr, "[matcha] Neural G2P load failed, falling back to espeak-ng\n");
                neural_g2p_.reset();
            } else {
                std::printf("[matcha] Neural G2P loaded: %s\n", g2p_model.c_str());
            }
        }
#endif

        std::printf("[matcha] Warming up CoreML models...\n");
        auto warmup = synthesize("Hallo.", 1.0f, 0);
        if (warmup.empty()) {
            std::fprintf(stderr, "[matcha] Warmup synthesis failed — first real call may be slow\n");
        } else {
            std::printf("[matcha] Warmup done (%zu samples)\n", warmup.size());
        }

        return true;
    }

    std::string phonemize(const std::string& text) {
        // The matcha-german model is German-only. Default is_de=true so that
        // German text without umlauts (the common case) is phonemized correctly.
        // detect_german() is only used as a fast confirmation; absence of umlauts
        // does not imply the text is English.
        bool is_de = language_ == "de" || (language_ == "auto" && !detect_english(text));

        std::string cache_key = text + (is_de ? "|de" : "|en");
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = phoneme_cache_.find(cache_key);
            if (it != phoneme_cache_.end()) return it->second;
        }

        std::string result;

#ifdef __APPLE__
        bool use_neural = is_de && neural_g2p_ && neural_g2p_->is_available() &&
                          g2p_backend_ != G2PBackend::ESPEAK;
        if (use_neural) {
            result = neural_g2p_->phonemize(text);
        } else
#endif
        {
            std::lock_guard<std::mutex> lock(espeak_mutex_);
            espeak_SetVoiceByName(is_de ? "de" : "en-us");
            const char* ptr = text.c_str();
            result.reserve(text.size() * 2);
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
                for (size_t i = 0; i < PHONEME_CACHE_MAX / 2 && it != phoneme_cache_.end(); ++i)
                    it = phoneme_cache_.erase(it);
            }
            phoneme_cache_[cache_key] = result;
        }
        return result;
    }

    std::vector<float> synthesize(const std::string& text, float speed, uint32_t call_id) {
        std::string phonemes = phonemize(text);
        if (phonemes.empty()) return {};

        std::vector<int32_t> token_ids = phonemes_to_ids(phonemes, vocab_, pad_id_);
        if (token_ids.empty()) {
            std::fprintf(stderr, "[matcha] No token IDs for phonemes: %s\n", phonemes.c_str());
            return {};
        }

#ifdef __APPLE__
        int token_count = (int)token_ids.size();
        const FlowBucket* bucket = &flow_buckets_[2];
        if (token_count < BUCKET_3S_MAX_TOKENS)      bucket = &flow_buckets_[0];
        else if (token_count < BUCKET_5S_MAX_TOKENS) bucket = &flow_buckets_[1];

        auto mu_mask = run_encoder(token_ids, speed, bucket->frames);
        if (mu_mask.empty()) {
            std::fprintf(stderr, "[matcha] Encoder failed for: %.40s\n", text.c_str());
            return {};
        }

        int T_mel = bucket->frames;
        std::vector<float> z_0 = sample_noise(T_mel, MEL_BINS, call_id);

        const float* mu_ptr   = mu_mask.data();
        const float* mask_ptr = mu_mask.data() + MEL_BINS * T_mel;

        std::vector<float> mel = run_flow(*bucket, z_0, mu_ptr, mask_ptr, T_mel);
        if (mel.empty()) {
            std::fprintf(stderr, "[matcha] Flow model failed for: %.40s\n", text.c_str());
            return {};
        }

        std::vector<float> waveform = run_vocoder(mel, T_mel);
        if (waveform.empty()) {
            std::fprintf(stderr, "[matcha] Vocoder failed for: %.40s\n", text.c_str());
            return {};
        }

        // Trim trailing silence: find last sample with |x| > 1e-4
        size_t last_nonzero = 0;
        for (size_t i = 0; i < waveform.size(); i++) {
            if (std::abs(waveform[i]) > 1e-4f) last_nonzero = i;
        }
        if (last_nonzero + 1 < waveform.size()) {
            size_t trim_end = std::min(waveform.size(), last_nonzero + (size_t)MATCHA_SAMPLE_RATE / 8);
            waveform.resize(trim_end);
        }

        tts::normalize_audio(waveform);
        tts::apply_fade_in(waveform);
        return waveform;
#else
        (void)speed; (void)call_id;
        return {};
#endif
    }

    void set_g2p_backend(G2PBackend backend) { g2p_backend_ = backend; }

    std::string coreml_dir() const { return coreml_dir_; }

private:
    // Returns true only when the text is confidently English (ASCII-only and
    // contains common English function words). Used to avoid switching the
    // German-model espeak voice for the rare case when an English phrase is
    // mixed in. When uncertain, we treat text as German (the model's language).
    static bool detect_english(const std::string& text) {
        for (unsigned char c : text) {
            if (c > 0x7F) return false;  // non-ASCII → definitely not plain English
        }
        // Common English short words that would be absent in German
        static const char* EN_MARKERS[] = {
            " the ", " and ", " you ", " are ", " is ", " was ", " have ",
            " will ", " can ", " not ", " this ", " that ", " with ", nullptr
        };
        std::string lower = text;
        for (char& c : lower) c = (char)std::tolower((unsigned char)c);
        for (int i = 0; EN_MARKERS[i]; i++) {
            if (lower.find(EN_MARKERS[i]) != std::string::npos) return true;
        }
        return false;
    }

    // Sample Gaussian noise z_0 ~ N(0,I) with shape [1, MEL_BINS, T_mel].
    // Uses per-call deterministic seed for reproducibility.
    std::vector<float> sample_noise(int T_mel, int mel_bins, uint32_t call_id) {
        std::mt19937 rng(static_cast<uint32_t>(call_id ^ 0xDEADBEEFu));
        std::normal_distribution<float> dist(0.0f, 1.0f);
        int count = mel_bins * T_mel;
        std::vector<float> noise(count);
        for (auto& v : noise) v = dist(rng);
        return noise;
    }

#ifdef __APPLE__
    // Run matcha_encoder.mlmodelc.
    // Input: input_ids [1, T] int32, x_lengths [1] int32, speed [1] float32
    // Output: mu [1, MEL_BINS, T_mel], mask [1, 1, T_mel] (T_mel = bucket frames)
    // Returns concatenated [mu_flat; mask_flat] or empty on failure.
    std::vector<float> run_encoder(const std::vector<int32_t>& token_ids, float speed, int T_mel) {
        @autoreleasepool {
            NSError* err = nil;
            int T = (int)token_ids.size();

            MLMultiArray* ids_arr = make_int32_array(
                token_ids, @[@1, @(T)]);
            if (!ids_arr) return {};

            std::vector<int32_t> lengths = {T};
            MLMultiArray* len_arr = make_int32_array(lengths, @[@1]);
            if (!len_arr) return {};

            std::vector<float> speed_vec = {speed};
            MLMultiArray* spd_arr = make_float32_array(
                speed_vec.data(), 1, @[@1]);
            if (!spd_arr) return {};

            std::vector<float> t_mel_vec = {(float)T_mel};
            MLMultiArray* tmel_arr = make_float32_array(
                t_mel_vec.data(), 1, @[@1]);
            if (!tmel_arr) return {};

            NSDictionary* features = @{
                @"input_ids":  [MLFeatureValue featureValueWithMultiArray:ids_arr],
                @"x_lengths":  [MLFeatureValue featureValueWithMultiArray:len_arr],
                @"speed":      [MLFeatureValue featureValueWithMultiArray:spd_arr],
                @"n_timesteps":[MLFeatureValue featureValueWithMultiArray:tmel_arr],
            };
            id<MLFeatureProvider> provider = [[MLDictionaryFeatureProvider alloc]
                initWithDictionary:features error:&err];
            if (!provider) return {};

            id<MLFeatureProvider> result;
            {
                std::lock_guard<std::mutex> lock(encoder_mutex_);
                result = [encoder_model_ predictionFromFeatures:provider error:&err];
            }
            if (!result) {
                std::fprintf(stderr, "[matcha] Encoder prediction failed: %s\n",
                    [[err localizedDescription] UTF8String]);
                return {};
            }

            MLMultiArray* mu_arr   = [[result featureValueForName:@"mu"] multiArrayValue];
            MLMultiArray* mask_arr = [[result featureValueForName:@"mask"] multiArrayValue];
            if (!mu_arr || !mask_arr) {
                std::fprintf(stderr, "[matcha] Encoder output missing mu or mask\n");
                return {};
            }

            size_t mu_count   = (size_t)mu_arr.count;
            size_t mask_count = (size_t)mask_arr.count;
            std::vector<float> out(mu_count + mask_count);
            std::memcpy(out.data(), mu_arr.dataPointer, mu_count * sizeof(float));
            std::memcpy(out.data() + mu_count, mask_arr.dataPointer, mask_count * sizeof(float));
            return out;
        }
    }

    // Run matcha_flow_{bucket}.mlmodelc.
    // Input: z [1, MEL_BINS, T_mel], mu [1, MEL_BINS, T_mel], mask [1, 1, T_mel]
    // Output: mel [1, MEL_BINS, T_mel]
    std::vector<float> run_flow(const FlowBucket& bucket,
                                const std::vector<float>& z,
                                const float* mu, const float* mask,
                                int T_mel) {
        @autoreleasepool {
            NSError* err = nil;
            int mel_flat = MEL_BINS * T_mel;
            int mask_flat = T_mel;

            MLMultiArray* z_arr = make_float32_array(
                z.data(), mel_flat, @[@1, @(MEL_BINS), @(T_mel)]);
            if (!z_arr) return {};

            MLMultiArray* mu_arr = make_float32_array(
                mu, mel_flat, @[@1, @(MEL_BINS), @(T_mel)]);
            if (!mu_arr) return {};

            MLMultiArray* mask_arr = make_float32_array(
                mask, mask_flat, @[@1, @1, @(T_mel)]);
            if (!mask_arr) return {};

            NSDictionary* features = @{
                @"z":    [MLFeatureValue featureValueWithMultiArray:z_arr],
                @"mu":   [MLFeatureValue featureValueWithMultiArray:mu_arr],
                @"mask": [MLFeatureValue featureValueWithMultiArray:mask_arr],
            };
            id<MLFeatureProvider> provider = [[MLDictionaryFeatureProvider alloc]
                initWithDictionary:features error:&err];
            if (!provider) return {};

            id<MLFeatureProvider> result;
            {
                std::lock_guard<std::mutex> lock(flow_mutex_);
                if (!bucket.model) return {};
                result = [bucket.model predictionFromFeatures:provider error:&err];
            }
            if (!result) {
                std::fprintf(stderr, "[matcha] Flow prediction failed: %s\n",
                    [[err localizedDescription] UTF8String]);
                return {};
            }

            MLMultiArray* mel_arr = [[result featureValueForName:@"mel"] multiArrayValue];
            if (!mel_arr) {
                std::fprintf(stderr, "[matcha] Flow output missing mel\n");
                return {};
            }
            size_t n = (size_t)mel_arr.count;
            const float* src = (const float*)mel_arr.dataPointer;
            return std::vector<float>(src, src + n);
        }
    }

    // Run matcha_vocoder.mlmodelc (HiFi-GAN).
    // Input: mel [1, MEL_BINS, T_mel]
    // Output: waveform [1, 1, T_audio]  where T_audio = T_mel * MEL_HOP_SIZE
    std::vector<float> run_vocoder(const std::vector<float>& mel, int T_mel) {
        @autoreleasepool {
            NSError* err = nil;
            int mel_flat = MEL_BINS * T_mel;

            MLMultiArray* mel_arr = make_float32_array(
                mel.data(), mel_flat, @[@1, @(MEL_BINS), @(T_mel)]);
            if (!mel_arr) return {};

            NSDictionary* features = @{
                @"mel": [MLFeatureValue featureValueWithMultiArray:mel_arr],
            };
            id<MLFeatureProvider> provider = [[MLDictionaryFeatureProvider alloc]
                initWithDictionary:features error:&err];
            if (!provider) return {};

            id<MLFeatureProvider> result;
            {
                std::lock_guard<std::mutex> lock(vocoder_mutex_);
                result = [vocoder_model_ predictionFromFeatures:provider error:&err];
            }
            if (!result) {
                std::fprintf(stderr, "[matcha] Vocoder prediction failed: %s\n",
                    [[err localizedDescription] UTF8String]);
                return {};
            }

            MLMultiArray* wav_arr = [[result featureValueForName:@"waveform"] multiArrayValue];
            if (!wav_arr) {
                std::fprintf(stderr, "[matcha] Vocoder output missing waveform\n");
                return {};
            }
            size_t n = (size_t)wav_arr.count;
            const float* src = (const float*)wav_arr.dataPointer;
            return std::vector<float>(src, src + n);
        }
    }

    MLModel* encoder_model_ = nil;
    MLModel* vocoder_model_ = nil;
    std::vector<FlowBucket> flow_buckets_;
    std::mutex encoder_mutex_;
    std::mutex vocoder_mutex_;
    std::mutex flow_mutex_;
#endif // __APPLE__

    std::unordered_map<std::string, int32_t> vocab_;
    int32_t pad_id_ = 0;
    std::string coreml_dir_;
    std::string language_ = "de";
    G2PBackend g2p_backend_ = G2PBackend::AUTO;

    std::mutex espeak_mutex_;
    std::mutex cache_mutex_;
    std::unordered_map<std::string, std::string> phoneme_cache_;

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

class MatchaService {
public:
    MatchaService() = default;

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

        if (!pipeline_.initialize(models_dir, voice_name, g2p_backend)) {
            std::fprintf(stderr, "[matcha] Failed to initialize Matcha pipeline\n");
            return false;
        }

        log_fwd_.init(FRONTEND_LOG_PORT, ServiceType::TTS_SERVICE);

        std::printf("[matcha] Service initialized (Matcha-TTS, CoreML, espeak-ng)\n");

        engine_.set_name("matcha");
        EngineAudioFormat fmt;
        fmt.sample_rate = MATCHA_SAMPLE_RATE;
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
            std::fprintf(stderr, "[matcha] received SHUTDOWN from TTS dock — signalling exit\n");
            running_.store(false);
        });

        if (!engine_.start()) {
            std::fprintf(stderr, "[matcha] Failed to start TTS engine client\n");
            return false;
        }

        return true;
    }

    void run() {
        std::thread cmd_thread(&MatchaService::command_listener_loop, this);

        std::printf("[matcha] Service ready - connecting to TTS dock at 127.0.0.1:%u\n",
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
        uint16_t port = MATCHA_ENGINE_CMD_PORT;
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return;

        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);

        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::fprintf(stderr, "[matcha] cmd: bind port %d failed\n", port);
            ::close(sock);
            return;
        }
        listen(sock, 4);
        cmd_sock_.store(sock);
        std::printf("[matcha] Command listener on port %d\n", port);

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
            samples = pipeline_.synthesize(text, 1.0f, 0);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (samples.empty()) return "ERROR:synthesis failed\n";

            double duration_s = (double)samples.size() / MATCHA_SAMPLE_RATE;
            double rtf = elapsed > 0 ? (elapsed / 1000.0) / duration_s : 0.0;

            float raw_peak = 0.0f;
            for (float s : samples) {
                float a = std::abs(s);
                if (a > raw_peak) raw_peak = a;
            }

            return "SYNTH_RESULT:" + std::to_string(elapsed) + "ms:"
                + std::to_string(samples.size()) + ":" + std::to_string(MATCHA_SAMPLE_RATE) + ":"
                + std::to_string(duration_s) + "s:rtf=" + std::to_string(rtf)
                + ":peak=" + std::to_string(raw_peak)
                + ":engine=matcha\n";
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
            samples = pipeline_.synthesize(text, 1.0f, 0);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (samples.empty()) return "ERROR:synthesis failed\n";

            std::ofstream out(path, std::ios::binary);
            if (!out.is_open()) return "ERROR:cannot open " + path + "\n";

            uint32_t data_size = (uint32_t)(samples.size() * sizeof(int16_t));
            uint32_t file_size = 36 + data_size;
            int32_t sr = MATCHA_SAMPLE_RATE;
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
                int16_t pcm = static_cast<int16_t>(
                    std::max(-1.0f, std::min(1.0f, s)) * 32767.0f);
                out.write(reinterpret_cast<char*>(&pcm), 2);
            }
            out.flush();
            if (!out.good()) {
                out.close();
                std::remove(path.c_str());
                return "ERROR:write failed\n";
            }
            out.close();

            double duration_s = (double)samples.size() / MATCHA_SAMPLE_RATE;
            double rtf = elapsed > 0 ? (elapsed / 1000.0) / duration_s : 0.0;

            return "WAV_RESULT:" + std::to_string(elapsed) + "ms:"
                + std::to_string(samples.size()) + ":" + std::to_string(MATCHA_SAMPLE_RATE) + ":"
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
                + ":MODEL:" + pipeline_.coreml_dir()
                + ":ENGINE:matcha\n";
        }
        return "ERROR:Unknown command\n";
    }

    void prewarm_call(uint32_t call_id) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto [it, inserted] = calls_.try_emplace(call_id, nullptr);
        if (inserted) {
            auto ctx = std::make_shared<CallContext>();
            ctx->call_id = call_id;
            ctx->worker = std::thread(&MatchaService::call_worker, this, ctx);
            ctx->audio_sender = std::thread(&MatchaService::audio_sender_loop, this, ctx);
            it->second = ctx;
            log_fwd_.forward(LogLevel::DEBUG, call_id, "Prewarmed Matcha synthesis thread on SPEECH_IDLE");
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
            ctx->worker = std::thread(&MatchaService::call_worker, this, ctx);
            ctx->audio_sender = std::thread(&MatchaService::audio_sender_loop, this, ctx);
            it->second = ctx;
            std::printf("[matcha] Started synthesis thread for call %u\n", pkt.call_id);
            log_fwd_.forward(LogLevel::INFO, pkt.call_id, "Started Matcha synthesis thread");
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

            std::printf("[matcha] Synthesizing for call %u: %.60s\n", ctx->call_id, text.c_str());

            auto start = std::chrono::steady_clock::now();
            std::vector<float> audio = pipeline_.synthesize(text, 1.0f, ctx->call_id);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (ctx->interrupted.load()) {
                ctx->interrupted = false;
                std::printf("[matcha] Synthesis interrupted for call %u\n", ctx->call_id);
                log_fwd_.forward(LogLevel::WARN, ctx->call_id, "Synthesis interrupted");
                continue;
            }

            if (audio.empty()) {
                std::fprintf(stderr, "[matcha] No audio output for call %u\n", ctx->call_id);
                continue;
            }

            {
                std::lock_guard<std::mutex> alock(ctx->audio_mutex);
                if (!ctx->interrupted.load()) {
                    ctx->audio_queue.push(std::move(audio));
                    ctx->audio_cv.notify_one();
                }
            }

            std::printf("[matcha] Synthesis complete for call %u in %lldms\n",
                        ctx->call_id, (long long)elapsed);
            log_fwd_.forward(LogLevel::INFO, ctx->call_id,
                            "Synthesis complete in %lldms (Matcha-TTS)", (long long)elapsed);
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

            int32_t sr = MATCHA_SAMPLE_RATE;
            std::memcpy(audio_pkt.payload.data(), &sr, sizeof(int32_t));
            uint64_t t_out_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            uint8_t ts_be[8];
            for (int i = 0; i < 8; ++i)
                ts_be[7 - i] = static_cast<uint8_t>((t_out_us >> (i * 8)) & 0xff);
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
        log_fwd_.forward(LogLevel::DEBUG, call_id,
            "SPEECH_ACTIVE — flushed TTS queue, interrupting synthesis");
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
    MatchaPipeline pipeline_;
    std::atomic<bool> running_{true};
    std::atomic<int> cmd_sock_{-1};
    std::map<uint32_t, std::shared_ptr<CallContext>> calls_;
    std::mutex calls_mutex_;
};

static MatchaService* g_service = nullptr;

void signal_handler(int) {
    if (g_service) {
        std::printf("\nShutting down Matcha service\n");
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
    std::string voice_name = "default";
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
                if (std::string(optarg) == "neural")  g2p_backend = G2PBackend::NEURAL;
                else if (std::string(optarg) == "espeak") g2p_backend = G2PBackend::ESPEAK;
                else g2p_backend = G2PBackend::AUTO;
                break;
            case 'L': log_level = optarg; break;
            case 'h':
                std::printf("Usage: matcha-service [OPTIONS]\n");
                std::printf("  --model-dir DIR    Models directory (default: $WHISPERTALK_MODELS_DIR)\n");
                std::printf("  --voice NAME       Voice name (default: default)\n");
                std::printf("  --g2p auto|neural|espeak  G2P backend (default: auto)\n");
                std::printf("  --log-level LEVEL  Log level: ERROR WARN INFO DEBUG TRACE (default: INFO)\n");
                std::printf("\nMatcha-TTS engine. Connects to the TTS dock via EngineClient.\n");
                std::printf("Models: $WHISPERTALK_MODELS_DIR/matcha-german/coreml/\n");
                std::printf("  matcha_encoder.mlmodelc, matcha_flow_{3s,5s,10s}.mlmodelc,\n");
                std::printf("  matcha_vocoder.mlmodelc, vocab.json\n");
                std::printf("Export: python scripts/export_matcha_models.py\n");
                return 0;
            default: break;
        }
    }

    std::printf("[matcha] Starting Matcha-TTS Service (CoreML, HiFi-GAN vocoder)\n");

    MatchaService service;
    g_service = &service;

    if (!service.initialize(model_dir, voice_name, g2p_backend)) {
        return 1;
    }

    service.set_log_level(log_level.c_str());
    service.run();

    return 0;
}
