#include <torch/torch.h>
#include <torch/script.h>
#ifdef __APPLE__
#include <torch/mps.h>
#endif
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

#ifdef KOKORO_COREML
#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>
#endif

#ifdef KOKORO_ONNX
#include <onnxruntime_cxx_api.h>
#endif

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

            std::printf("CoreML duration model loaded from %s\n", mlmodelc_path.c_str());
            available_ = true;
            return true;
        }
    }

    struct DurationOutput {
        torch::Tensor pred_dur;  // [1, 512] int32
        torch::Tensor d;         // [1, 512, 640]
        torch::Tensor t_en;      // [1, 512, 512]
        torch::Tensor s;         // [1, 128]
        torch::Tensor ref_s_out; // [1, 256]
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

            id<MLFeatureProvider> input_features;
            NSDictionary* input_dict = @{
                @"input_ids": input_ids_arr,
                @"ref_s": ref_s_arr,
                @"speed": speed_arr,
                @"attention_mask": mask_arr
            };
            input_features = [[MLDictionaryFeatureProvider alloc]
                initWithDictionary:input_dict error:&error];
            if (error) return false;

            id<MLFeatureProvider> result = [model_ predictionFromFeatures:input_features error:&error];
            if (error || !result) {
                std::fprintf(stderr, "CoreML prediction failed: %s\n",
                    error ? [[error description] UTF8String] : "unknown");
                return false;
            }

            MLMultiArray* pred_dur_ml = [result featureValueForName:@"pred_dur"].multiArrayValue;
            MLMultiArray* d_ml = [result featureValueForName:@"d"].multiArrayValue;
            MLMultiArray* t_en_ml = [result featureValueForName:@"t_en"].multiArrayValue;
            MLMultiArray* s_ml = [result featureValueForName:@"s"].multiArrayValue;
            MLMultiArray* ref_s_out_ml = [result featureValueForName:@"ref_s_out"].multiArrayValue;

            if (!pred_dur_ml || !d_ml || !t_en_ml || !s_ml || !ref_s_out_ml) return false;

            output.pred_dur = torch::from_blob(
                (int32_t*)pred_dur_ml.dataPointer, {1, 512}, torch::kInt32).clone();
            output.d = torch::from_blob(
                (float*)d_ml.dataPointer, {1, 512, 640}, torch::kFloat32).clone();
            output.t_en = torch::from_blob(
                (float*)t_en_ml.dataPointer, {1, 512, 512}, torch::kFloat32).clone();
            output.s = torch::from_blob(
                (float*)s_ml.dataPointer, {1, 128}, torch::kFloat32).clone();
            output.ref_s_out = torch::from_blob(
                (float*)ref_s_out_ml.dataPointer, {1, 256}, torch::kFloat32).clone();

            return true;
        }
    }

    bool is_available() const { return available_; }

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

private:
    std::vector<BucketInfo> buckets_;
    std::map<std::string, torch::jit::script::Module> har_models_;
    bool available_ = false;
};
#endif

#ifdef KOKORO_ONNX
class OnnxDecoder {
public:
    struct BucketInfo {
        std::string name;
        int asr_frames;
        int f0_frames;
        std::unique_ptr<Ort::Session> session;
    };

    bool load(const std::string& variants_dir) {
        env_ = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "kokoro_decoder");
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(4);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

        struct { const char* name; int asr; int f0; } sizes[] = {
            {"3s", 72, 144}, {"5s", 120, 240}, {"10s", 240, 480},
        };

        for (auto& s : sizes) {
            std::string path = variants_dir + "/kokoro_decoder_" + s.name + ".onnx";
            struct stat st;
            if (stat(path.c_str(), &st) != 0) continue;
            try {
                auto session = std::make_unique<Ort::Session>(*env_, path.c_str(), opts);
                BucketInfo info;
                info.name = s.name;
                info.asr_frames = s.asr;
                info.f0_frames = s.f0;
                info.session = std::move(session);
                buckets_.push_back(std::move(info));
                std::printf("ONNX decoder loaded: %s (asr=%d, f0=%d)\n", s.name, s.asr, s.f0);
            } catch (const Ort::Exception& e) {
                std::fprintf(stderr, "Failed to load ONNX decoder %s: %s\n", s.name, e.what());
            }
        }

        available_ = !buckets_.empty();
        return available_;
    }

    BucketInfo* select_bucket(int f0_frames) {
        for (auto& b : buckets_) {
            if (b.f0_frames >= f0_frames) return &b;
        }
        return buckets_.empty() ? nullptr : &buckets_.back();
    }

    std::vector<float> decode(BucketInfo& bucket,
                               const std::vector<float>& asr_data,
                               const std::vector<float>& f0_data,
                               const std::vector<float>& n_data,
                               const std::vector<float>& ref_s_data) {
        auto alloc = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        std::array<int64_t, 3> asr_shape = {1, 512, (int64_t)bucket.asr_frames};
        std::array<int64_t, 2> f0_shape = {1, (int64_t)bucket.f0_frames};
        std::array<int64_t, 2> refs_shape = {1, 256};

        auto asr_val = Ort::Value::CreateTensor<float>(alloc, (float*)asr_data.data(), asr_data.size(), asr_shape.data(), 3);
        auto f0_val = Ort::Value::CreateTensor<float>(alloc, (float*)f0_data.data(), f0_data.size(), f0_shape.data(), 2);
        auto n_val = Ort::Value::CreateTensor<float>(alloc, (float*)n_data.data(), n_data.size(), f0_shape.data(), 2);
        auto refs_val = Ort::Value::CreateTensor<float>(alloc, (float*)ref_s_data.data(), ref_s_data.size(), refs_shape.data(), 2);

        const char* input_names[] = {"asr", "F0_pred", "N_pred", "ref_s"};
        const char* output_names[] = {"waveform"};
        std::array<Ort::Value, 4> inputs;
        inputs[0] = std::move(asr_val);
        inputs[1] = std::move(f0_val);
        inputs[2] = std::move(n_val);
        inputs[3] = std::move(refs_val);

        try {
            auto outputs = bucket.session->Run(Ort::RunOptions{nullptr},
                input_names, inputs.data(), 4, output_names, 1);
            auto& out_tensor = outputs[0];
            auto* data = out_tensor.GetTensorData<float>();
            auto shape = out_tensor.GetTensorTypeAndShapeInfo().GetShape();
            int64_t n = 1;
            for (auto s : shape) n *= s;
            return std::vector<float>(data, data + n);
        } catch (const Ort::Exception& e) {
            std::fprintf(stderr, "ONNX decode failed: %s\n", e.what());
            return {};
        }
    }

    bool is_available() const { return available_; }

private:
    std::unique_ptr<Ort::Env> env_;
    std::vector<BucketInfo> buckets_;
    bool available_ = false;
};
#endif

enum class DecoderBackend { TORCHSCRIPT, ONNX, COREML_SPLIT };

static const char* backend_name(DecoderBackend b) {
    switch (b) {
        case DecoderBackend::TORCHSCRIPT: return "torchscript";
        case DecoderBackend::ONNX: return "onnx";
        case DecoderBackend::COREML_SPLIT: return "coreml-split";
    }
    return "unknown";
}

class KokoroPipeline {
public:
    bool initialize(const std::string& models_dir, const std::string& voice_name,
                    bool enable_coreml, DecoderBackend requested_backend = DecoderBackend::TORCHSCRIPT) {
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

        if (!load_bucket_models(base_dir)) {
            std::fprintf(stderr, "Failed to load any bucket models from %s\n", base_dir.c_str());
            return false;
        }

        if (!load_voice_pack(voice_path, voice_fallback, voice_name)) {
            return false;
        }

#ifdef KOKORO_COREML
        if (enable_coreml) {
            std::string coreml_path = base_dir + "/coreml/kokoro_duration.mlmodelc";
            struct stat st;
            if (stat(coreml_path.c_str(), &st) == 0) {
                coreml_duration_ = std::make_unique<CoreMLDurationModel>();
                if (coreml_duration_->load(coreml_path)) {
                    std::printf("CoreML acceleration ENABLED for duration model (ANE)\n");
                    coreml_available_ = true;
                } else {
                    coreml_duration_.reset();
                    std::printf("CoreML load failed, using TorchScript fallback\n");
                }
            } else {
                std::printf("CoreML model not found at %s, using TorchScript\n", coreml_path.c_str());
            }

            if (requested_backend == DecoderBackend::COREML_SPLIT) {
                coreml_split_decoder_ = std::make_unique<CoreMLSplitDecoder>();
                if (coreml_split_decoder_->load(variants_dir)) {
                    active_backend_ = DecoderBackend::COREML_SPLIT;
                    std::printf("CoreML split decoder ENABLED (ANE)\n");
                } else {
                    coreml_split_decoder_.reset();
                    std::printf("CoreML split decoder load failed, using TorchScript\n");
                }
            }
        }
#else
        (void)enable_coreml;
#endif

#ifdef KOKORO_ONNX
        if (requested_backend == DecoderBackend::ONNX) {
            onnx_decoder_ = std::make_unique<OnnxDecoder>();
            if (onnx_decoder_->load(variants_dir)) {
                active_backend_ = DecoderBackend::ONNX;
                std::printf("ONNX decoder ENABLED\n");
            } else {
                onnx_decoder_.reset();
                std::printf("ONNX decoder load failed, using TorchScript\n");
            }
        }
#endif

        if (active_backend_ == DecoderBackend::TORCHSCRIPT) {
            std::printf("Using TorchScript decoder (baseline)\n");
        }

        try_mps_acceleration();

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

    DecoderBackend active_backend() const { return active_backend_; }

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

        int64_t input_len = static_cast<int64_t>(ids.size());
        int bucket = select_bucket(input_len);
        if (bucket < 0) {
            std::fprintf(stderr, "Input too long (%lld tokens), max bucket=%d\n",
                        input_len, bucket_sizes_.empty() ? 0 : bucket_sizes_.back());
            return {};
        }

        std::vector<int64_t> padded_ids(bucket, 0);
        std::copy(ids.begin(), ids.end(), padded_ids.begin());

        int phoneme_count = static_cast<int>(ids.size()) - 2;
        int voice_idx = std::min(phoneme_count - 1, voice_entries_ - 1);
        voice_idx = std::max(0, voice_idx);

        auto ref_s = voice_pack_.index({voice_idx}).unsqueeze(0).to(device_);
        auto input_ids = torch::from_blob(padded_ids.data(),
                                         {1, static_cast<int64_t>(bucket)},
                                         torch::kLong).clone().to(device_);
        auto speed_tensor = torch::tensor(speed).to(device_);

        return synthesize_with_backend(input_ids, ref_s, speed_tensor, bucket);
    }

    std::vector<float> synthesize_with_backend(const torch::Tensor& input_ids,
                                                const torch::Tensor& ref_s,
                                                const torch::Tensor& speed_tensor,
                                                int bucket) {
        if (active_backend_ == DecoderBackend::TORCHSCRIPT) {
            return synthesize_torchscript(input_ids, ref_s, speed_tensor, bucket);
        }
#ifdef KOKORO_ONNX
        if (active_backend_ == DecoderBackend::ONNX && onnx_decoder_) {
            return synthesize_onnx(input_ids, ref_s, speed_tensor, bucket);
        }
#endif
#ifdef KOKORO_COREML
        if (active_backend_ == DecoderBackend::COREML_SPLIT && coreml_split_decoder_) {
            return synthesize_coreml_split(input_ids, ref_s, speed_tensor, bucket);
        }
#endif
        return synthesize_torchscript(input_ids, ref_s, speed_tensor, bucket);
    }

    std::vector<float> synthesize_torchscript(const torch::Tensor& input_ids,
                                               const torch::Tensor& ref_s,
                                               const torch::Tensor& speed_tensor,
                                               int bucket) {
        torch::Tensor audio;
        {
            std::lock_guard<std::mutex> lock(model_mutex_);
            torch::NoGradGuard no_grad;
            std::vector<torch::jit::IValue> inputs;
            inputs.push_back(input_ids);
            inputs.push_back(ref_s);
            inputs.push_back(speed_tensor);
            auto& model = bucket_models_[bucket];
            audio = model.forward(inputs).toTensor();
        }
        return tensor_to_vector(audio);
    }

#ifdef KOKORO_ONNX
    std::vector<float> synthesize_onnx(const torch::Tensor& input_ids,
                                        const torch::Tensor& ref_s,
                                        const torch::Tensor& speed_tensor,
                                        int bucket) {
        if (!coreml_available_ || !onnx_decoder_) {
            return synthesize_torchscript(input_ids, ref_s, speed_tensor, bucket);
        }
#ifdef KOKORO_COREML
        auto intermediates = run_duration_and_align(input_ids, ref_s, speed_tensor);
        if (!intermediates.valid) {
            return synthesize_torchscript(input_ids, ref_s, speed_tensor, bucket);
        }

        int f0_len = static_cast<int>(intermediates.f0_pred.size(1));
        auto* ob = onnx_decoder_->select_bucket(f0_len);
        if (!ob) {
            return synthesize_torchscript(input_ids, ref_s, speed_tensor, bucket);
        }

        auto asr_cpu = intermediates.asr.contiguous().cpu();
        auto f0_cpu = intermediates.f0_pred.contiguous().cpu();
        auto n_cpu = intermediates.n_pred.contiguous().cpu();
        auto refs_cpu = intermediates.ref_s_dec.contiguous().cpu();

        int asr_frames = ob->asr_frames;
        int f0_frames = ob->f0_frames;
        std::vector<float> asr_data(512 * asr_frames, 0.0f);
        std::vector<float> f0_data(f0_frames, 0.0f);
        std::vector<float> n_data(f0_frames, 0.0f);
        std::vector<float> refs_data(256, 0.0f);

        int asr_actual = std::min((int)asr_cpu.size(2), asr_frames);
        for (int c = 0; c < 512; c++) {
            std::memcpy(asr_data.data() + c * asr_frames,
                       asr_cpu.data_ptr<float>() + c * asr_cpu.size(2),
                       asr_actual * sizeof(float));
        }
        int f0_actual = std::min((int)f0_cpu.size(1), f0_frames);
        std::memcpy(f0_data.data(), f0_cpu.data_ptr<float>(), f0_actual * sizeof(float));
        std::memcpy(n_data.data(), n_cpu.data_ptr<float>(), f0_actual * sizeof(float));
        std::memcpy(refs_data.data(), refs_cpu.data_ptr<float>(), 256 * sizeof(float));

        return onnx_decoder_->decode(*ob, asr_data, f0_data, n_data, refs_data);
#else
        return synthesize_torchscript(input_ids, ref_s, speed_tensor, bucket);
#endif
    }
#endif

#ifdef KOKORO_COREML
    std::vector<float> synthesize_coreml_split(const torch::Tensor& input_ids,
                                                const torch::Tensor& ref_s,
                                                const torch::Tensor& speed_tensor,
                                                int bucket) {
        if (!coreml_available_ || !coreml_split_decoder_) {
            return synthesize_torchscript(input_ids, ref_s, speed_tensor, bucket);
        }

        auto intermediates = run_duration_and_align(input_ids, ref_s, speed_tensor);
        if (!intermediates.valid) {
            return synthesize_torchscript(input_ids, ref_s, speed_tensor, bucket);
        }

        int f0_len = static_cast<int>(intermediates.f0_pred.size(1));
        auto* sb = coreml_split_decoder_->select_bucket(f0_len);
        if (!sb) {
            return synthesize_torchscript(input_ids, ref_s, speed_tensor, bucket);
        }

        auto f0_for_har = intermediates.f0_pred.unsqueeze(0);
        auto har = coreml_split_decoder_->compute_har(sb->name, f0_for_har);
        if (!har.defined() || har.numel() == 0) {
            return synthesize_torchscript(input_ids, ref_s, speed_tensor, bucket);
        }

        int asr_frames = sb->asr_frames;
        int f0_frames = sb->f0_frames;
        auto asr_padded = torch::zeros({1, 512, asr_frames});
        int asr_actual = std::min((int)intermediates.asr.size(2), asr_frames);
        asr_padded.slice(2, 0, asr_actual) = intermediates.asr.slice(2, 0, asr_actual);

        auto f0_padded = torch::zeros({1, f0_frames});
        auto n_padded = torch::zeros({1, f0_frames});
        int f0_actual = std::min(f0_len, f0_frames);
        f0_padded.slice(1, 0, f0_actual) = intermediates.f0_pred.slice(1, 0, f0_actual);
        n_padded.slice(1, 0, f0_actual) = intermediates.n_pred.slice(1, 0, f0_actual);

        int har_time = sb->har_time;
        int har_channels = sb->har_channels * 2;
        auto har_padded = torch::zeros({1, har_channels, har_time});
        int har_actual_t = std::min((int)har.size(2), har_time);
        int har_actual_c = std::min((int)har.size(1), har_channels);
        har_padded.slice(1, 0, har_actual_c).slice(2, 0, har_actual_t) =
            har.slice(1, 0, har_actual_c).slice(2, 0, har_actual_t);

        return coreml_split_decoder_->decode(*sb, asr_padded, f0_padded, n_padded,
                                              intermediates.ref_s_dec, har_padded);
    }
#endif

    struct AlignedIntermediates {
        torch::Tensor asr;        // [1, 512, T_aligned]
        torch::Tensor f0_pred;    // [1, T_f0]
        torch::Tensor n_pred;     // [1, T_f0]
        torch::Tensor ref_s_dec;  // [1, 128] (decoder style)
        bool valid = false;
    };

#ifdef KOKORO_COREML
    AlignedIntermediates run_duration_and_align(const torch::Tensor& input_ids,
                                                 const torch::Tensor& ref_s,
                                                 const torch::Tensor& speed_tensor) {
        AlignedIntermediates result;
        if (!coreml_duration_ || !coreml_duration_->is_available()) return result;

        auto ids_cpu = input_ids.contiguous().cpu();
        int64_t seq_len = ids_cpu.size(1);
        std::vector<int32_t> ids_vec(seq_len);
        auto ids_ptr = ids_cpu.data_ptr<int64_t>();
        for (int64_t i = 0; i < seq_len; i++) ids_vec[i] = static_cast<int32_t>(ids_ptr[i]);

        int actual_len = 0;
        for (int i = 0; i < (int)ids_vec.size(); i++) {
            if (ids_vec[i] != 0) actual_len = i + 1;
        }
        std::vector<int32_t> mask_vec(seq_len);
        for (int i = 0; i < actual_len; i++) mask_vec[i] = 1;

        float speed_val = speed_tensor.item<float>();

        CoreMLDurationModel::DurationOutput dur_out;
        if (!coreml_duration_->predict(ids_vec, ref_s.squeeze(0), speed_val, mask_vec, dur_out)) {
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
        int64_t pos = 0;
        for (int i = 0; i < actual_len && i < pred_dur.size(0); i++) {
            int64_t dur = dur_acc[i];
            if (dur <= 0) continue;
            auto col = t_en_cpu.select(1, i);
            for (int64_t j = 0; j < dur && (pos + j) < total_frames; j++) {
                asr[0].select(1, pos + j) = col;
            }
            pos += dur;
        }

        int64_t f0_frames = total_frames * 2;
        result.asr = asr;
        result.f0_pred = torch::zeros({1, f0_frames});
        result.n_pred = torch::zeros({1, f0_frames});
        result.ref_s_dec = dur_out.ref_s_out.slice(1, 0, 128);
        if (!result.ref_s_dec.defined() || result.ref_s_dec.size(1) < 128) {
            result.ref_s_dec = ref_s.slice(1, 0, 128);
        }
        result.valid = true;
        return result;
    }
#else
    AlignedIntermediates run_duration_and_align(const torch::Tensor&,
                                                 const torch::Tensor&,
                                                 const torch::Tensor&) {
        return {};
    }
#endif

    static std::vector<float> tensor_to_vector(const torch::Tensor& audio) {
        auto t = audio.contiguous().cpu();
        auto accessor = t.accessor<float, 1>();
        std::vector<float> samples(accessor.size(0));
        for (int64_t i = 0; i < accessor.size(0); i++) {
            samples[i] = accessor[i];
        }
        return samples;
    }

    bool has_coreml() const { return coreml_available_; }

private:
    void try_mps_acceleration() {
#ifdef __APPLE__
        if (torch::mps::is_available()) {
            std::printf("MPS hardware available but TorchScript has placeholder storage incompatibility\n");
            std::printf("GPU acceleration via CoreML (ANE) for duration model instead\n");
        }
#endif
        device_ = torch::kCPU;
        std::printf("Using CPU inference for TorchScript decoder\n");
    }

    bool load_bucket_models(const std::string& base_dir) {
        const int candidates[] = {8, 16, 32, 64, 128, 256, 512};
        for (int sz : candidates) {
            std::string path = base_dir + "/kokoro_german_L" + std::to_string(sz) + ".pt";
            struct stat st;
            if (stat(path.c_str(), &st) != 0) continue;
            try {
                auto model = torch::jit::load(path);
                model.eval();
                bucket_models_[sz] = std::move(model);
                bucket_sizes_.push_back(sz);
                std::printf("Loaded bucket model L=%d from %s\n", sz, path.c_str());
            } catch (const c10::Error& e) {
                std::fprintf(stderr, "Failed to load bucket L=%d: %s\n", sz, e.what());
            }
        }
        if (bucket_sizes_.empty()) {
            std::string single = base_dir + "/kokoro_german.pt";
            struct stat st;
            if (stat(single.c_str(), &st) == 0) {
                try {
                    auto model = torch::jit::load(single);
                    model.eval();
                    bucket_models_[0] = std::move(model);
                    bucket_sizes_.push_back(0);
                    std::printf("Loaded single model (no buckets) from %s\n", single.c_str());
                } catch (const c10::Error& e) {
                    std::fprintf(stderr, "Failed to load single model: %s\n", e.what());
                    return false;
                }
            }
        }
        std::sort(bucket_sizes_.begin(), bucket_sizes_.end());
        std::printf("Loaded %zu bucket models: ", bucket_sizes_.size());
        for (int sz : bucket_sizes_) std::printf("%d ", sz);
        std::printf("\n");
        return !bucket_sizes_.empty();
    }

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
                auto loaded = torch::pickle_load(data).toTensor().to(torch::kFloat32);
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

    int select_bucket(int64_t input_len) const {
        if (bucket_sizes_.size() == 1 && bucket_sizes_[0] == 0) {
            return 0;
        }
        for (int sz : bucket_sizes_) {
            if (sz >= input_len) return sz;
        }
        return -1;
    }

    std::map<int, torch::jit::script::Module> bucket_models_;
    std::vector<int> bucket_sizes_;
    torch::Tensor voice_pack_;
    int voice_entries_ = 0;
    torch::Device device_{torch::kCPU};
    KokoroVocab vocab_;
    std::mutex model_mutex_;
    std::mutex espeak_mutex_;
    std::unordered_map<std::string, std::string> phoneme_cache_;
    std::mutex cache_mutex_;
    bool coreml_available_ = false;
    DecoderBackend active_backend_ = DecoderBackend::TORCHSCRIPT;
#ifdef KOKORO_COREML
    std::unique_ptr<CoreMLDurationModel> coreml_duration_;
    std::unique_ptr<CoreMLSplitDecoder> coreml_split_decoder_;
#endif
#ifdef KOKORO_ONNX
    std::unique_ptr<OnnxDecoder> onnx_decoder_;
#endif
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

    bool initialize(const std::string& voice_name, bool enable_coreml, DecoderBackend backend) {
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
        if (!pipeline_.initialize(models_dir, voice_name, enable_coreml, backend)) {
            std::fprintf(stderr, "Failed to initialize Kokoro pipeline\n");
            return false;
        }

        std::printf("Kokoro TTS Service initialized (German, voice=%s, decoder=%s)\n",
                   voice_name.c_str(), backend_name(pipeline_.active_backend()));
        std::printf("  Negotiation ports: IN=%u OUT=%u\n", node_.ports().neg_in, node_.ports().neg_out);
        std::printf("  Is master: %s\n", node_.is_master() ? "yes" : "no");
        std::printf("  CoreML duration: %s\n", pipeline_.has_coreml() ? "ENABLED (ANE)" : "DISABLED");

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
    bool enable_coreml = true;
    DecoderBackend backend = DecoderBackend::TORCHSCRIPT;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg.rfind("--voice=", 0) == 0) {
            voice = arg.substr(8);
        } else if (arg == "--voice" && i + 1 < argc) {
            voice = argv[++i];
        } else if (arg == "--no-coreml") {
            enable_coreml = false;
        } else if (arg.rfind("--decoder=", 0) == 0) {
            std::string dec = arg.substr(10);
            if (dec == "torchscript" || dec == "ts") backend = DecoderBackend::TORCHSCRIPT;
            else if (dec == "onnx") backend = DecoderBackend::ONNX;
            else if (dec == "coreml-split" || dec == "coreml") backend = DecoderBackend::COREML_SPLIT;
            else {
                std::fprintf(stderr, "Unknown decoder backend: %s\n", dec.c_str());
                return 1;
            }
        } else if (arg == "--help" || arg == "-h") {
            std::printf("Usage: kokoro-service [OPTIONS]\n");
            std::printf("  --voice=NAME      Voice to use (default: df_eva, also: dm_bernd)\n");
            std::printf("  --decoder=BACKEND  Decoder backend: torchscript, onnx, coreml-split (default: torchscript)\n");
            std::printf("  --no-coreml       Disable CoreML acceleration for duration model\n");
            return 0;
        }
    }

    std::printf("Starting Kokoro TTS Service (voice=%s, decoder=%s, coreml=%s)\n",
               voice.c_str(), backend_name(backend), enable_coreml ? "auto" : "disabled");

    KokoroService service;
    g_service = &service;

    if (!service.initialize(voice, enable_coreml, backend)) {
        return 1;
    }

    service.run();

    return 0;
}
