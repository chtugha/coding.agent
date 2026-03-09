// kokoro-service.cpp — Text-to-Speech (TTS) synthesis stage using Kokoro + espeak-ng.
//
// Pipeline position: LLaMA → [Kokoro] → OAP
//
// Receives response text from LLaMA and synthesizes 24kHz float32 PCM audio using
// the Kokoro TTS model (PyTorch TorchScript). Streams audio chunks to the
// Outbound Audio Processor (OAP) for downsampling and G.711 encoding.
//
// Phonemization pipeline:
//   1. espeak-ng (via libespeak-ng) converts text → IPA phoneme string.
//      Language selection: German ("de") or English ("en-us") based on detect_german().
//      A phoneme cache (PHONEME_CACHE_MAX=10000 entries, LRU eviction) avoids re-running
//      espeak-ng for repeated phrases.
//   2. KokoroVocab maps phoneme strings → int64 token IDs using a greedy longest-match
//      scan (up to 4 chars per token, UTF-8 aware) over the vocab.json lookup table.
//   3. Input is padded to 512 tokens and passed to the duration model.
//
// Model architecture (two-stage):
//   Stage 1 — Duration model: predicts phoneme durations and generates alignment tensors
//     (pred_dur, d, t_en, s, ref_s). Runs the style encoder over a reference style
//     embedding from voice.bin.
//   Stage 2 — Decoder model: generates the audio waveform from the alignment tensors.
//     On Apple Silicon: uses CoreML ANE (Apple Neural Engine) split decoder when
//     compiled with -DKOKORO_COREML. Falls back to TorchScript GPU path otherwise.
//
// CoreML split decoder (KOKORO_COREML):
//   CoreMLDurationModel and CoreMLDecoderModel wrap MLModel instances configured with
//   MLComputeUnitsAll (ANE + GPU + CPU). Tensors are bridged between PyTorch and CoreML
//   via MLMultiArray memory copies. This path provides ~2-4× speedup on M-series chips
//   compared to the PyTorch Metal backend for the decoder stage alone.
//
// Audio output normalization:
//   normalize_audio() clips peaks above 0.95 to prevent clipping distortion in the
//   G.711 encoder. Only scales down — never amplifies — to preserve dynamic range.
//
// SPEECH_ACTIVE handling:
//   If a SPEECH_ACTIVE signal arrives during synthesis (caller starts speaking),
//   the current synthesis is abandoned immediately and the output buffer is cleared.
//   This prevents stale TTS audio from playing over the caller's speech.
//
// CMD port (Kokoro base+2 = 13142): PING, STATUS, SET_LOG_LEVEL commands.
//   STATUS returns: model path, upstream/downstream state, active call count.
#include <torch/script.h>
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
#include <sys/socket.h>
#include <netinet/in.h>
#include <poll.h>
#include <unistd.h>
#include <getopt.h>
#include <cmath>

#ifdef KOKORO_COREML
#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>
#endif

using namespace whispertalk;

static const int KOKORO_SAMPLE_RATE = 24000;
static const size_t MAX_AUDIO_SAMPLES = 10 * KOKORO_SAMPLE_RATE;
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

#ifdef KOKORO_COREML
class CoreMLDurationModel {
public:
    bool load(const std::string& mlmodelc_path) {
        @autoreleasepool {
            NSString* path = [NSString stringWithUTF8String:mlmodelc_path.c_str()];
            NSURL* url = [NSURL fileURLWithPath:path];

            MLModelConfiguration* config = [[MLModelConfiguration alloc] init];
            config.computeUnits = MLComputeUnitsAll;

            NSError* error = nil;
            model_ = [MLModel modelWithContentsOfURL:url configuration:config error:&error];
            if (error || !model_) {
                std::fprintf(stderr, "CoreML: Failed to load duration model: %s\n",
                    error ? [[error description] UTF8String] : "unknown error");
                return false;
            }
            [model_ retain];

            std::printf("CoreML duration model loaded from %s\n", mlmodelc_path.c_str());
            available_ = true;
            return true;
        }
    }

    struct DurationOutput {
        torch::Tensor pred_dur;
        torch::Tensor d;
        torch::Tensor t_en;
        torch::Tensor s;
        torch::Tensor ref_s_out;
    };

    bool predict(const std::vector<int32_t>& input_ids_vec,
                 const torch::Tensor& ref_s_tensor,
                 float speed_val,
                 const std::vector<int32_t>& attention_mask_vec,
                 DurationOutput& output) {
        if (!available_) return false;

        @autoreleasepool {
            NSError* error = nil;

            NSArray<NSNumber*>* ids_shape = @[@1, @512];
            MLMultiArray* input_ids_arr = [[MLMultiArray alloc]
                initWithShape:ids_shape dataType:MLMultiArrayDataTypeInt32 error:&error];
            if (error) return false;
            int32_t* ids_ptr = (int32_t*)input_ids_arr.dataPointer;
            for (int i = 0; i < 512; i++) {
                ids_ptr[i] = (i < (int)input_ids_vec.size()) ? input_ids_vec[i] : 0;
            }

            NSArray<NSNumber*>* ref_shape = @[@1, @256];
            MLMultiArray* ref_s_arr = [[MLMultiArray alloc]
                initWithShape:ref_shape dataType:MLMultiArrayDataTypeFloat32 error:&error];
            if (error) return false;
            float* ref_ptr = (float*)ref_s_arr.dataPointer;
            auto ref_accessor = ref_s_tensor.contiguous().cpu().data_ptr<float>();
            std::memcpy(ref_ptr, ref_accessor, 256 * sizeof(float));

            NSArray<NSNumber*>* speed_shape = @[@1];
            MLMultiArray* speed_arr = [[MLMultiArray alloc]
                initWithShape:speed_shape dataType:MLMultiArrayDataTypeFloat32 error:&error];
            if (error) return false;
            ((float*)speed_arr.dataPointer)[0] = speed_val;

            MLMultiArray* mask_arr = [[MLMultiArray alloc]
                initWithShape:ids_shape dataType:MLMultiArrayDataTypeInt32 error:&error];
            if (error) return false;
            int32_t* mask_ptr = (int32_t*)mask_arr.dataPointer;
            for (int i = 0; i < 512; i++) {
                mask_ptr[i] = (i < (int)attention_mask_vec.size()) ? attention_mask_vec[i] : 0;
            }

            NSDictionary* input_dict = @{
                @"input_ids": input_ids_arr,
                @"ref_s": ref_s_arr,
                @"speed": speed_arr,
                @"attention_mask": mask_arr
            };
            auto features = [[MLDictionaryFeatureProvider alloc] initWithDictionary:input_dict error:&error];
            if (error) return false;

            auto result = [model_ predictionFromFeatures:features error:&error];
            if (error || !result) {
                std::fprintf(stderr, "CoreML duration predict failed: %s\n",
                    error ? [[error description] UTF8String] : "unknown");
                return false;
            }

            auto extract = [&](NSString* name, std::vector<int64_t> shape) -> torch::Tensor {
                MLMultiArray* arr = [result featureValueForName:name].multiArrayValue;
                if (!arr) return {};
                auto t = torch::zeros(shape);
                int64_t n = 1;
                for (auto s : shape) n *= s;
                std::memcpy(t.data_ptr<float>(), (float*)arr.dataPointer, n * sizeof(float));
                return t;
            };

            auto extract_int = [&](NSString* name, std::vector<int64_t> shape) -> torch::Tensor {
                MLMultiArray* arr = [result featureValueForName:name].multiArrayValue;
                if (!arr) return {};
                auto t = torch::zeros(shape, torch::kInt32);
                int64_t n = 1;
                for (auto s : shape) n *= s;
                std::memcpy(t.data_ptr<int32_t>(), (int32_t*)arr.dataPointer, n * sizeof(int32_t));
                return t.to(torch::kLong);
            };

            output.pred_dur = extract_int(@"pred_dur", {1, 512});
            output.d = extract(@"d", {1, 512, 640});
            output.t_en = extract(@"t_en", {1, 512, 512});
            output.s = extract(@"s", {1, 128});
            output.ref_s_out = extract(@"ref_s_out", {1, 256});

            return output.pred_dur.defined() && output.t_en.defined();
        }
    }

    bool is_available() const { return available_; }

    ~CoreMLDurationModel() {
        if (model_) [model_ release];
    }

private:
    MLModel* model_ = nil;
    bool available_ = false;
};

class CoreMLSplitDecoder {
public:
    struct BucketInfo {
        std::string name;
        int asr_frames;
        int f0_frames;
        int har_channels;
        int har_time;
        MLModel* model = nil;
    };

    bool load(const std::string& variants_dir) {
        @autoreleasepool {
            struct { const char* name; int asr; int f0; int harc; int hart; } buckets[] = {
                {"3s", 72, 144, 11, 8641},
                {"5s", 120, 240, 11, 14401},
                {"10s", 240, 480, 11, 28801},
            };

            MLModelConfiguration* config = [[MLModelConfiguration alloc] init];
            config.computeUnits = MLComputeUnitsAll;

            for (auto& b : buckets) {
                std::string path = variants_dir + "/kokoro_decoder_split_" + b.name + ".mlmodelc";
                struct stat st;
                if (stat(path.c_str(), &st) != 0) continue;

                NSString* ns_path = [NSString stringWithUTF8String:path.c_str()];
                NSURL* url = [NSURL fileURLWithPath:ns_path];
                NSError* error = nil;
                MLModel* model = [MLModel modelWithContentsOfURL:url configuration:config error:&error];
                if (error || !model) {
                    std::fprintf(stderr, "CoreML: Failed to load split decoder %s: %s\n",
                        b.name, error ? [[error description] UTF8String] : "unknown");
                    continue;
                }

                BucketInfo info;
                info.name = b.name;
                info.asr_frames = b.asr;
                info.f0_frames = b.f0;
                info.har_channels = b.harc;
                info.har_time = b.hart;
                info.model = model;
                [model retain];
                buckets_.push_back(info);
                std::printf("CoreML split decoder loaded: %s (asr=%d, f0=%d)\n", b.name, b.asr, b.f0);
            }

            if (buckets_.empty()) return false;

            for (auto& b : buckets_) {
                std::string har_path = variants_dir + "/kokoro_har_" + b.name + ".pt";
                struct stat st2;
                if (stat(har_path.c_str(), &st2) != 0) {
                    std::fprintf(stderr, "HAR model not found: %s\n", har_path.c_str());
                    continue;
                }
                try {
                    auto har = torch::jit::load(har_path);
                    har.eval();
                    har_models_[b.name] = std::move(har);
                    std::printf("HAR model loaded: %s\n", b.name.c_str());
                } catch (const c10::Error& e) {
                    std::fprintf(stderr, "Failed to load HAR model %s: %s\n", b.name.c_str(), e.what());
                }
            }

            available_ = true;
            return true;
        }
    }

    const BucketInfo* select_bucket(int f0_frames) const {
        for (auto& b : buckets_) {
            if (b.f0_frames >= f0_frames) return &b;
        }
        return buckets_.empty() ? nullptr : &buckets_.back();
    }

    std::vector<float> decode(const BucketInfo& bucket,
                               const torch::Tensor& asr,
                               const torch::Tensor& f0_pred,
                               const torch::Tensor& n_pred,
                               const torch::Tensor& ref_s,
                               const torch::Tensor& har) {
        @autoreleasepool {
            NSError* error = nil;

            auto make_array = [&](NSArray<NSNumber*>* shape, const torch::Tensor& t) -> MLMultiArray* {
                MLMultiArray* arr = [[MLMultiArray alloc] initWithShape:shape
                    dataType:MLMultiArrayDataTypeFloat32 error:&error];
                if (error) return nil;
                auto src = t.contiguous().cpu().data_ptr<float>();
                std::memcpy((float*)arr.dataPointer, src, t.numel() * sizeof(float));
                return arr;
            };

            auto asr_arr = make_array(@[@1, @512, @(bucket.asr_frames)], asr);
            auto f0_arr = make_array(@[@1, @(bucket.f0_frames)], f0_pred);
            auto n_arr = make_array(@[@1, @(bucket.f0_frames)], n_pred);
            auto refs_arr = make_array(@[@1, @256], ref_s);
            auto har_arr = make_array(@[@1, @(bucket.har_channels * 2), @(bucket.har_time)], har);

            if (!asr_arr || !f0_arr || !n_arr || !refs_arr || !har_arr) return {};

            NSDictionary* input_dict = @{
                @"asr": asr_arr, @"F0_pred": f0_arr, @"N_pred": n_arr,
                @"ref_s": refs_arr, @"har": har_arr
            };
            auto features = [[MLDictionaryFeatureProvider alloc] initWithDictionary:input_dict error:&error];
            if (error) return {};

            auto result = [bucket.model predictionFromFeatures:features error:&error];
            if (error || !result) {
                std::fprintf(stderr, "CoreML split decoder predict failed: %s\n",
                    error ? [[error description] UTF8String] : "unknown");
                return {};
            }

            MLMultiArray* wav = [result featureValueForName:@"waveform"].multiArrayValue;
            if (!wav) return {};

            int64_t n_samples = wav.count;
            std::vector<float> samples(n_samples);
            std::memcpy(samples.data(), (float*)wav.dataPointer, n_samples * sizeof(float));
            return samples;
        }
    }

    torch::Tensor compute_har(const std::string& bucket_name, const torch::Tensor& f0) {
        auto it = har_models_.find(bucket_name);
        if (it == har_models_.end()) return {};
        torch::NoGradGuard no_grad;
        return it->second.forward({f0}).toTensor();
    }

    bool is_available() const { return available_; }

    ~CoreMLSplitDecoder() {
        for (auto& b : buckets_) {
            if (b.model) [b.model release];
        }
    }

private:
    std::vector<BucketInfo> buckets_;
    std::map<std::string, torch::jit::script::Module> har_models_;
    bool available_ = false;
};
#endif

struct AlignedIntermediates {
    torch::Tensor asr;
    torch::Tensor f0_pred;
    torch::Tensor n_pred;
    torch::Tensor ref_s_dec;
    bool valid = false;
};

class KokoroPipeline {
public:
    bool initialize(const std::string& models_dir, const std::string& voice_name) {
        std::string base_dir = models_dir + "/kokoro-german";
        std::string vocab_path = base_dir + "/vocab.json";
        std::string voice_path = base_dir + "/" + voice_name + "_voice.bin";
        std::string voice_fallback = base_dir + "/" + voice_name + "_embedding.pt";
        std::string variants_dir = base_dir + "/decoder_variants";

        if (!vocab_.load(vocab_path)) {
            std::fprintf(stderr, "Failed to load vocab from %s\n", vocab_path.c_str());
            return false;
        }
        std::printf("Loaded vocab: %zu entries\n", vocab_.phoneme_to_id.size());

        if (!load_voice_pack(voice_path, voice_fallback, voice_name)) {
            return false;
        }

#ifdef KOKORO_COREML
        std::string coreml_path = base_dir + "/coreml/kokoro_duration.mlmodelc";
        struct stat st;
        if (stat(coreml_path.c_str(), &st) == 0) {
            coreml_duration_ = std::make_unique<CoreMLDurationModel>();
            if (coreml_duration_->load(coreml_path)) {
                std::printf("CoreML duration model ENABLED (ANE)\n");
                coreml_available_ = true;
            } else {
                coreml_duration_.reset();
                std::fprintf(stderr, "CoreML duration model load failed\n");
                return false;
            }
        } else {
            std::fprintf(stderr, "CoreML duration model not found at %s\n", coreml_path.c_str());
            return false;
        }

        coreml_split_decoder_ = std::make_unique<CoreMLSplitDecoder>();
        if (coreml_split_decoder_->load(variants_dir)) {
            std::printf("CoreML split decoder ENABLED (ANE)\n");
        } else {
            coreml_split_decoder_.reset();
            std::fprintf(stderr, "CoreML split decoder load failed from %s\n", variants_dir.c_str());
            return false;
        }
#else
        std::fprintf(stderr, "FATAL: kokoro-service requires CoreML (macOS). Build with KOKORO_COREML=ON\n");
        return false;
#endif

        std::string f0n_path = base_dir + "/kokoro_f0n_predictor.pt";
        struct stat st_f0n;
        if (stat(f0n_path.c_str(), &st_f0n) == 0) {
            try {
                f0n_predictor_ = torch::jit::load(f0n_path);
                f0n_predictor_.eval();
                f0n_available_ = true;
                std::printf("F0/N predictor loaded from %s (%.1f MB)\n",
                           f0n_path.c_str(), st_f0n.st_size / 1e6);
            } catch (const c10::Error& e) {
                std::fprintf(stderr, "Failed to load F0/N predictor: %s\n", e.what());
            }
        } else {
            std::fprintf(stderr, "WARNING: F0/N predictor not found at %s — F0/noise will be zero (degraded quality)\n", f0n_path.c_str());
        }

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
        int voice_idx = std::min(phoneme_count - 1, voice_entries_ - 1);
        voice_idx = std::max(0, voice_idx);

        auto ref_s = voice_pack_.index({voice_idx}).unsqueeze(0);

#ifdef KOKORO_COREML
        return synthesize_coreml(ids, ref_s, speed);
#else
        (void)speed;
        return {};
#endif
    }

#ifdef KOKORO_COREML
    std::vector<float> synthesize_coreml(const std::vector<int64_t>& ids,
                                          const torch::Tensor& ref_s,
                                          float speed) {
        if (!coreml_available_ || !coreml_split_decoder_) return {};

        auto intermediates = run_duration_and_align(ids, ref_s, speed);
        if (!intermediates.valid) return {};

        int f0_len = static_cast<int>(intermediates.f0_pred.size(1));
        auto* sb = coreml_split_decoder_->select_bucket(f0_len);
        if (!sb) {
            std::fprintf(stderr, "No decoder bucket for f0_len=%d\n", f0_len);
            return {};
        }

        int asr_frames = sb->asr_frames;
        int f0_frames = sb->f0_frames;

        auto f0_padded = torch::zeros({1, f0_frames});
        auto n_padded = torch::zeros({1, f0_frames});
        int f0_actual = std::min(f0_len, f0_frames);
        f0_padded.slice(1, 0, f0_actual) = intermediates.f0_pred.slice(1, 0, f0_actual);
        n_padded.slice(1, 0, f0_actual) = intermediates.n_pred.slice(1, 0, f0_actual);

        torch::Tensor har;
        try {
            har = coreml_split_decoder_->compute_har(sb->name, f0_padded);
        } catch (const c10::Error& e) {
            std::fprintf(stderr, "HAR computation exception for bucket %s: %s\n", sb->name.c_str(), e.what());
            return {};
        }
        if (!har.defined() || har.numel() == 0) {
            std::fprintf(stderr, "HAR computation failed for bucket %s\n", sb->name.c_str());
            return {};
        }

        auto asr_padded = torch::zeros({1, 512, asr_frames});
        int asr_actual = std::min((int)intermediates.asr.size(2), asr_frames);
        asr_padded.slice(2, 0, asr_actual) = intermediates.asr.slice(2, 0, asr_actual);

        int har_time = sb->har_time;
        int har_channels = sb->har_channels * 2;
        auto har_padded = torch::zeros({1, har_channels, har_time});
        int har_actual_t = std::min((int)har.size(2), har_time);
        int har_actual_c = std::min((int)har.size(1), har_channels);
        har_padded.slice(1, 0, har_actual_c).slice(2, 0, har_actual_t) =
            har.slice(1, 0, har_actual_c).slice(2, 0, har_actual_t);

        try {
            return coreml_split_decoder_->decode(*sb, asr_padded, f0_padded, n_padded,
                                                  intermediates.ref_s_dec, har_padded);
        } catch (const c10::Error& e) {
            std::fprintf(stderr, "Decoder exception for bucket %s: %s\n", sb->name.c_str(), e.what());
            return {};
        }
    }

    AlignedIntermediates run_duration_and_align(const std::vector<int64_t>& ids,
                                                 const torch::Tensor& ref_s,
                                                 float speed) {
        AlignedIntermediates result;
        if (!coreml_duration_ || !coreml_duration_->is_available()) return result;

        int64_t seq_len = 512;
        std::vector<int32_t> ids_vec(seq_len, 0);
        for (int64_t i = 0; i < (int64_t)ids.size() && i < seq_len; i++)
            ids_vec[i] = static_cast<int32_t>(ids[i]);

        int actual_len = 0;
        for (int i = 0; i < (int)ids.size() && i < (int)seq_len; i++) {
            if (ids_vec[i] != 0) actual_len = i + 1;
        }
        std::vector<int32_t> mask_vec(seq_len, 0);
        for (int i = 0; i < actual_len; i++) mask_vec[i] = 1;

        CoreMLDurationModel::DurationOutput dur_out;
        if (!coreml_duration_->predict(ids_vec, ref_s.squeeze(0), speed, mask_vec, dur_out)) {
            return result;
        }

        auto pred_dur = dur_out.pred_dur.squeeze(0).to(torch::kLong);
        auto t_en = dur_out.t_en;

        int64_t total_frames = 0;
        auto dur_acc = pred_dur.accessor<int64_t, 1>();
        for (int i = 0; i < actual_len && i < pred_dur.size(0); i++) {
            total_frames += dur_acc[i];
        }
        if (total_frames <= 0) return result;

        auto t_en_cpu = t_en.squeeze(0).contiguous().cpu();
        int t_en_dim = static_cast<int>(t_en_cpu.size(0));
        auto asr = torch::zeros({1, t_en_dim, total_frames});

        auto d_cpu = dur_out.d.squeeze(0).contiguous().cpu();
        int d_dim = static_cast<int>(d_cpu.size(1));
        auto d_aligned = torch::zeros({1, d_dim, total_frames});

        int64_t pos = 0;
        for (int i = 0; i < actual_len && i < pred_dur.size(0); i++) {
            int64_t dur = dur_acc[i];
            if (dur <= 0) continue;
            auto t_col = t_en_cpu.select(1, i);
            auto d_col = d_cpu.select(0, i);
            for (int64_t j = 0; j < dur && (pos + j) < total_frames; j++) {
                asr[0].select(1, pos + j) = t_col;
                d_aligned[0].select(1, pos + j) = d_col;
            }
            pos += dur;
        }

        int64_t f0_frames = total_frames * 2;
        result.asr = asr;

        if (f0n_available_) {
            try {
                torch::NoGradGuard no_grad;
                auto f0n_out = f0n_predictor_.forward({d_aligned, dur_out.s}).toTuple();
                result.f0_pred = f0n_out->elements()[0].toTensor();
                result.n_pred = f0n_out->elements()[1].toTensor();
            } catch (const c10::Error& e) {
                std::fprintf(stderr, "F0/N prediction failed: %s — falling back to zeros\n", e.what());
                result.f0_pred = torch::zeros({1, f0_frames});
                result.n_pred = torch::zeros({1, f0_frames});
            }
        } else {
            result.f0_pred = torch::zeros({1, f0_frames});
            result.n_pred = torch::zeros({1, f0_frames});
        }
        result.ref_s_dec = dur_out.ref_s_out.slice(1, 0, 128);
        if (!result.ref_s_dec.defined() || result.ref_s_dec.size(1) < 128) {
            result.ref_s_dec = ref_s.slice(1, 0, 128);
        }
        result.valid = true;
        return result;
    }
#endif

    bool has_coreml() const { return coreml_available_; }

private:
    bool load_voice_pack(const std::string& bin_path, const std::string& pt_fallback,
                          const std::string& voice_name) {
        struct stat st;
        if (stat(bin_path.c_str(), &st) == 0) {
            std::ifstream f(bin_path, std::ios::binary);
            if (!f.is_open()) {
                std::fprintf(stderr, "Failed to open voice bin: %s\n", bin_path.c_str());
                return false;
            }
            size_t file_size = static_cast<size_t>(st.st_size);
            size_t num_floats = file_size / sizeof(float);
            voice_entries_ = static_cast<int>(num_floats / 256);
            std::vector<float> raw(num_floats);
            f.read(reinterpret_cast<char*>(raw.data()), file_size);
            voice_pack_ = torch::from_blob(raw.data(),
                                           {voice_entries_, 256}, torch::kFloat32).clone();
            std::printf("Loaded voice '%s' from bin: [%d, 256]\n", voice_name.c_str(), voice_entries_);
            return true;
        }

        if (stat(pt_fallback.c_str(), &st) == 0) {
            try {
                std::ifstream f(pt_fallback, std::ios::binary);
                if (!f.is_open()) {
                    std::fprintf(stderr, "Failed to open voice: %s\n", pt_fallback.c_str());
                    return false;
                }
                std::vector<char> data((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
                auto loaded = torch::jit::pickle_load(data).toTensor().to(torch::kFloat32);
                voice_pack_ = loaded.squeeze(1);
                voice_entries_ = static_cast<int>(voice_pack_.size(0));
                std::printf("Loaded voice '%s' from pt: [%d, %lld]\n",
                           voice_name.c_str(), voice_entries_, voice_pack_.size(1));
                return true;
            } catch (const c10::Error& e) {
                std::fprintf(stderr, "Failed to load voice pt: %s\n", e.what());
                return false;
            }
        }

        std::fprintf(stderr, "No voice file found: %s or %s\n",
                    bin_path.c_str(), pt_fallback.c_str());
        return false;
    }

    torch::Tensor voice_pack_;
    int voice_entries_ = 0;
    KokoroVocab vocab_;
    std::mutex espeak_mutex_;
    std::unordered_map<std::string, std::string> phoneme_cache_;
    std::mutex cache_mutex_;
    bool coreml_available_ = false;
    torch::jit::script::Module f0n_predictor_;
    bool f0n_available_ = false;
#ifdef KOKORO_COREML
    std::unique_ptr<CoreMLDurationModel> coreml_duration_;
    std::unique_ptr<CoreMLSplitDecoder> coreml_split_decoder_;
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
};

class KokoroService {
public:
    KokoroService() : node_(ServiceType::KOKORO_SERVICE) {}

    bool initialize(const std::string& voice_name) {
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
        if (!pipeline_.initialize(models_dir, voice_name)) {
            std::fprintf(stderr, "Failed to initialize Kokoro pipeline\n");
            return false;
        }

        log_fwd_.init(FRONTEND_LOG_PORT, ServiceType::KOKORO_SERVICE);

        std::printf("Kokoro TTS Service initialized (German, voice=%s, decoder=coreml-split)\n",
                   voice_name.c_str());
        std::printf("  CoreML duration: %s\n", pipeline_.has_coreml() ? "ENABLED (ANE)" : "DISABLED");

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

        std::thread cmd_thread(&KokoroService::command_listener_loop, this);

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
    void command_listener_loop() {
        uint16_t port = whispertalk::service_cmd_port(ServiceType::KOKORO_SERVICE);
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return;

        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);

        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::fprintf(stderr, "Kokoro cmd: bind port %d failed\n", port);
            ::close(sock);
            return;
        }
        listen(sock, 4);
        cmd_sock_.store(sock);
        std::printf("Kokoro command listener on port %d\n", port);

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
        if (cmd.rfind("TEST_SYNTH:", 0) == 0) {
            std::string text = cmd.substr(11);
            std::vector<float> samples;
            auto start = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(pipeline_mutex_);
                samples = pipeline_.synthesize(text, speed_.load());
            }
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (samples.empty()) {
                return "ERROR:synthesis failed\n";
            }

            double duration_s = (double)samples.size() / KOKORO_SAMPLE_RATE;
            double rtf = (elapsed / 1000.0) / duration_s;

            // Report raw (pre-normalization) peak for diagnostics.
            // Production path (call_worker) applies normalize_audio() before OAP.
            float raw_peak = 0.0f;
            double sum_sq = 0.0;
            for (float s : samples) {
                float a = std::abs(s);
                if (a > raw_peak) raw_peak = a;
                sum_sq += s * s;
            }
            double rms = std::sqrt(sum_sq / samples.size());

            return "SYNTH_RESULT:" + std::to_string(elapsed) + "ms:"
                + std::to_string(samples.size()) + ":" + std::to_string(KOKORO_SAMPLE_RATE) + ":"
                + std::to_string(duration_s) + "s:rtf=" + std::to_string(rtf)
                + ":peak=" + std::to_string(raw_peak) + ":rms=" + std::to_string(rms) + "\n";
        }
        if (cmd.rfind("BENCHMARK:", 0) == 0) {
            std::string text = cmd.substr(10);
            int iterations = 5;
            size_t sep = text.find('|');
            if (sep != std::string::npos) {
                iterations = std::max(1, std::atoi(text.substr(sep + 1).c_str()));
                text = text.substr(0, sep);
            }

            std::vector<double> rtfs;
            std::vector<double> latencies;
            size_t last_samples = 0;
            for (int i = 0; i < iterations; i++) {
                std::vector<float> samples;
                auto t0 = std::chrono::steady_clock::now();
                {
                    std::lock_guard<std::mutex> lock(pipeline_mutex_);
                    samples = pipeline_.synthesize(text, speed_.load());
                }
                auto ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - t0).count();
                if (!samples.empty()) {
                    latencies.push_back(ms);
                    last_samples = samples.size();
                    double dur = (double)samples.size() / KOKORO_SAMPLE_RATE;
                    rtfs.push_back((ms / 1000.0) / dur);
                }
            }

            if (latencies.empty()) {
                return "ERROR:all iterations failed\n";
            }

            std::sort(latencies.begin(), latencies.end());
            double sum = 0;
            for (double v : latencies) sum += v;
            double avg = sum / latencies.size();
            double p50 = latencies[latencies.size() / 2];
            double p95 = latencies[std::min(latencies.size() - 1, (size_t)(latencies.size() * 0.95))];
            double rtf_sum = 0;
            for (double v : rtfs) rtf_sum += v;
            double rtf_avg = rtf_sum / rtfs.size();

            return "BENCH_RESULT:" + std::to_string((int)avg) + "ms:"
                + std::to_string((int)p50) + "ms:" + std::to_string((int)p95) + "ms:"
                + std::to_string((int)latencies.size()) + "/" + std::to_string(iterations) + ":"
                + std::to_string(last_samples) + "@" + std::to_string(KOKORO_SAMPLE_RATE) + ":"
                + "rtf=" + std::to_string(rtf_avg) + "\n";
        }
        if (cmd.rfind("SYNTH_WAV:", 0) == 0) {
            std::string rest = cmd.substr(10);
            size_t sep = rest.find('|');
            if (sep == std::string::npos) return "ERROR:format SYNTH_WAV:<path>|<text>\n";
            std::string path = rest.substr(0, sep);
            std::string text = rest.substr(sep + 1);
            if (path.empty() || text.empty()) return "ERROR:empty path or text\n";
            if (path.find("..") != std::string::npos || path[0] == '/')
                return "ERROR:invalid path (no traversal or absolute paths)\n";

            std::vector<float> samples;
            auto start = std::chrono::steady_clock::now();
            {
                std::lock_guard<std::mutex> lock(pipeline_mutex_);
                samples = pipeline_.synthesize(text, speed_.load());
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
            int32_t sr = KOKORO_SAMPLE_RATE;
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
                return "ERROR:write failed (disk full?)\n";
            }
            out.close();

            double duration_s = (double)samples.size() / KOKORO_SAMPLE_RATE;
            double rtf = (elapsed / 1000.0) / duration_s;

            return "WAV_RESULT:" + std::to_string(elapsed) + "ms:"
                + std::to_string(samples.size()) + ":" + std::to_string(KOKORO_SAMPLE_RATE) + ":"
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
        if (cmd.rfind("SET_SPEED:", 0) == 0) {
            try {
                float v = std::stof(cmd.substr(10));
                if (v < 0.5f) v = 0.5f;
                if (v > 2.0f) v = 2.0f;
                speed_.store(v);
                char buf[64];
                std::snprintf(buf, sizeof(buf), "SPEED:%.2f\n", v);
                return buf;
            } catch (...) { return "ERROR:Invalid speed value\n"; }
        }
        if (cmd == "GET_SPEED") {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "SPEED:%.2f\n", speed_.load());
            return buf;
        }
        if (cmd == "STATUS") {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            char spd[16];
            std::snprintf(spd, sizeof(spd), "%.2f", speed_.load());
            return "ACTIVE_CALLS:" + std::to_string(calls_.size())
                + ":UPSTREAM:" + (node_.upstream_state() == ConnectionState::CONNECTED ? "connected" : "disconnected")
                + ":DOWNSTREAM:" + (node_.downstream_state() == ConnectionState::CONNECTED ? "connected" : "disconnected")
                + ":SPEED:" + spd
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
            ctx->worker = std::thread(&KokoroService::call_worker, this, ctx);
            calls_[pkt.call_id] = ctx;
            std::printf("Started synthesis thread for call %u\n", pkt.call_id);
            log_fwd_.forward(LogLevel::INFO, pkt.call_id, "Started synthesis thread");
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
                samples = pipeline_.synthesize(text, speed_.load());
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
            log_fwd_.forward(LogLevel::INFO, ctx->call_id, "Synthesized %zu samples in %lldms (raw_peak=%.3f%s)",
                             samples.size(), (long long)elapsed, raw_peak, norm_tag);

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

            int32_t sr = KOKORO_SAMPLE_RATE;
            std::memcpy(audio_pkt.payload.data(), &sr, sizeof(int32_t));
            std::memcpy(audio_pkt.payload.data() + header_size,
                       samples.data() + offset, count * sizeof(float));

            audio_pkt.trace.record(whispertalk::ServiceType::KOKORO_SERVICE, 0);
            audio_pkt.trace.record(whispertalk::ServiceType::KOKORO_SERVICE, 1);
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
                       total_sent, KOKORO_SAMPLE_RATE, call_id,
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
    KokoroPipeline pipeline_;
    std::mutex pipeline_mutex_;
    std::atomic<bool> running_{true};
    std::atomic<int> cmd_sock_{-1};
    std::map<uint32_t, std::shared_ptr<CallContext>> calls_;
    std::mutex calls_mutex_;
    std::atomic<float> speed_{1.0f};
};

static KokoroService* g_service = nullptr;

void signal_handler(int) {
    if (g_service) {
        std::printf("\nShutting down Kokoro service\n");
        g_service->shutdown();
    }
}

int main(int argc, char* argv[]) {
    setlinebuf(stdout);
    setlinebuf(stderr);

    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    std::string voice = "df_eva";
    std::string log_level = "INFO";

    static struct option long_opts[] = {
        {"voice",     required_argument, 0, 'v'},
        {"log-level", required_argument, 0, 'L'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "v:L:h", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'v': voice = optarg; break;
            case 'L': log_level = optarg; break;
            case 'h':
                std::printf("Usage: kokoro-service [OPTIONS]\n");
                std::printf("  --voice NAME      Voice to use (default: df_eva, also: dm_bernd)\n");
                std::printf("  --log-level LEVEL Log level: ERROR WARN INFO DEBUG TRACE (default: INFO)\n");
                return 0;
            default: break;
        }
    }

    std::printf("Starting Kokoro TTS Service (voice=%s, decoder=coreml-split)\n", voice.c_str());

    KokoroService service;
    g_service = &service;

    if (!service.initialize(voice)) {
        return 1;
    }

    service.set_log_level(log_level.c_str());
    service.run();

    return 0;
}
