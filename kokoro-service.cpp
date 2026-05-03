// kokoro-service.cpp — Text-to-Speech (TTS) synthesis stage using Kokoro + espeak-ng.
//
// Pipeline position: LLaMA → [Kokoro] → OAP
//
// Receives response text from LLaMA and synthesizes 24kHz float32 PCM audio using
// the Kokoro TTS model (CoreML). Streams audio chunks to the
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
//   MLComputeUnitsAll (ANE + GPU + CPU). Data is bridged to CoreML
//   via MLMultiArray memory copies. This path provides ~2-4× speedup on M-series chips
//   compared to CPU-only inference for the decoder stage alone.
//
// Audio output normalization:
//   normalize_audio() scales audio to 0.90 peak ceiling to prevent clipping distortion
//   in the G.711 encoder. Skips near-silent (peak < 0.01) and already-normalized audio.
//
// SPEECH_ACTIVE handling:
//   If a SPEECH_ACTIVE signal arrives during synthesis (caller starts speaking),
//   the current synthesis is abandoned immediately and the output buffer is cleared.
//   This prevents stale TTS audio from playing over the caller's speech.
//
// CMD port (Kokoro diagnostic port 13144): PING, STATUS, SET_LOG_LEVEL commands.
//   STATUS returns: active call count, dock connection state, speed, G2P backend.
#include "ktensor.h"
#include "har_source.h"
#include "neural-g2p.h"
#include <espeak-ng/speak_lib.h>
#include "interconnect.h"
#include "tts-engine-client.h"
#include "tts-common.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <future>
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

// Shared TTS audio constants (single source of truth: tts-common.h).
static constexpr int KOKORO_SAMPLE_RATE = static_cast<int>(whispertalk::tts::kTTSSampleRate);
// Hard upper bound on a single synthesis output. Long LLaMA responses can
// easily exceed 10 s after multi-chunk merging in pipeline_.synthesize();
// 120 s is the realistic ceiling for a single conversational turn and
// covers the worst-case sentence we have observed in production.
static constexpr size_t MAX_AUDIO_SAMPLES = 120 * static_cast<size_t>(KOKORO_SAMPLE_RATE);
static constexpr size_t PHONEME_CACHE_MAX = 10000;
// Diagnostic cmd port for the Kokoro engine (see spec §4.2). Separate from
// the TTS dock's own cmd port (13142) so operators can query the engine
// process directly without going through the dock.
static constexpr uint16_t KOKORO_ENGINE_CMD_PORT = whispertalk::tts::kKokoroEngineCmdPort;

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
        std::vector<int64_t> pred_dur;
        KTensor d;
        KTensor t_en;
        KTensor s;
        KTensor ref_s_out;
    };

    bool predict(const std::vector<int32_t>& input_ids_vec,
                 const KTensor& ref_s_tensor,
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
            std::memcpy((float*)ref_s_arr.dataPointer, ref_s_tensor.ptr(), 256 * sizeof(float));

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

            auto extract = [&](NSString* name, std::initializer_list<int64_t> shape) -> KTensor {
                MLMultiArray* arr = [result featureValueForName:name].multiArrayValue;
                if (!arr) return {};
                auto t = KTensor::zeros(shape);
                std::memcpy(t.ptr(), (float*)arr.dataPointer, t.numel() * sizeof(float));
                return t;
            };

            {
                MLMultiArray* arr = [result featureValueForName:@"pred_dur"].multiArrayValue;
                if (!arr) return false;
                output.pred_dur.resize(512);
                int32_t* src = (int32_t*)arr.dataPointer;
                for (int i = 0; i < 512; i++) output.pred_dur[i] = src[i];
            }

            output.d = extract(@"d", {1, 512, 640});
            output.t_en = extract(@"t_en", {1, 512, 512});
            output.s = extract(@"s", {1, 128});
            output.ref_s_out = extract(@"ref_s_out", {1, 256});

            return !output.pred_dur.empty() && output.t_en.defined() && output.d.defined();
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

            std::string har_path = variants_dir + "/har_weights.bin";
            if (!har_source_.load(har_path)) {
                std::fprintf(stderr, "HAR weights not found: %s\n", har_path.c_str());
                return false;
            }
            std::printf("HAR source loaded from %s\n", har_path.c_str());

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

    int max_asr_frames() const {
        if (buckets_.empty()) return 0;
        return buckets_.back().asr_frames;
    }

    std::vector<float> decode(const BucketInfo& bucket,
                               const KTensor& asr,
                               const KTensor& f0_pred,
                               const KTensor& n_pred,
                               const KTensor& ref_s,
                               const std::vector<float>& har) {
        @autoreleasepool {
            NSError* error = nil;

            auto make_array = [&](NSArray<NSNumber*>* shape, const float* src, int64_t count) -> MLMultiArray* {
                MLMultiArray* arr = [[MLMultiArray alloc] initWithShape:shape
                    dataType:MLMultiArrayDataTypeFloat32 error:&error];
                if (error) return nil;
                std::memcpy((float*)arr.dataPointer, src, count * sizeof(float));
                return arr;
            };

            auto asr_arr = make_array(@[@1, @512, @(bucket.asr_frames)], asr.ptr(), asr.numel());
            auto f0_arr = make_array(@[@1, @(bucket.f0_frames)], f0_pred.ptr(), f0_pred.numel());
            auto n_arr = make_array(@[@1, @(bucket.f0_frames)], n_pred.ptr(), n_pred.numel());
            auto refs_arr = make_array(@[@1, @256], ref_s.ptr(), ref_s.numel());
            int64_t har_total = (int64_t)(bucket.har_channels * 2) * bucket.har_time;
            auto har_arr = make_array(@[@1, @(bucket.har_channels * 2), @(bucket.har_time)], har.data(), har_total);

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

    std::vector<float> compute_har(const float* f0, int f0_frames) {
        return har_source_.compute(f0, f0_frames);
    }

    bool is_available() const { return available_; }

    ~CoreMLSplitDecoder() {
        for (auto& b : buckets_) {
            if (b.model) [b.model release];
        }
    }

private:
    std::vector<BucketInfo> buckets_;
    HarSource har_source_;
    bool available_ = false;
};

class CoreMLF0NPredictor {
public:
    struct BucketInfo {
        std::string name;
        int asr_frames;
        int f0_frames;
        MLModel* model = nil;
    };

    bool load(const std::string& coreml_dir) {
        @autoreleasepool {
            struct { const char* name; int asr; int f0; } buckets[] = {
                {"3s", 72, 144},
                {"5s", 120, 240},
                {"10s", 240, 480},
            };

            MLModelConfiguration* config = [[MLModelConfiguration alloc] init];
            config.computeUnits = MLComputeUnitsAll;

            for (auto& b : buckets) {
                std::string path = coreml_dir + "/kokoro_f0n_" + b.name + ".mlmodelc";
                struct stat st;
                if (stat(path.c_str(), &st) != 0) {
                    path = coreml_dir + "/kokoro_f0n_" + b.name + ".mlpackage";
                    if (stat(path.c_str(), &st) != 0) continue;
                }

                NSString* ns_path = [NSString stringWithUTF8String:path.c_str()];
                NSURL* url = [NSURL fileURLWithPath:ns_path];
                NSError* error = nil;
                MLModel* model = [MLModel modelWithContentsOfURL:url configuration:config error:&error];
                if (error || !model) {
                    std::fprintf(stderr, "CoreML: Failed to load F0N predictor %s: %s\n",
                        b.name, error ? [[error description] UTF8String] : "unknown");
                    continue;
                }

                BucketInfo info;
                info.name = b.name;
                info.asr_frames = b.asr;
                info.f0_frames = b.f0;
                info.model = model;
                [model retain];
                buckets_.push_back(info);
                std::printf("CoreML F0/N predictor loaded: %s (asr=%d, f0=%d)\n", b.name, b.asr, b.f0);
            }

            available_ = !buckets_.empty();
            return available_;
        }
    }

    const BucketInfo* select_bucket(int asr_frames) const {
        for (auto& b : buckets_) {
            if (b.asr_frames >= asr_frames) return &b;
        }
        return buckets_.empty() ? nullptr : &buckets_.back();
    }

    bool predict(const BucketInfo& bucket,
                 const float* en_data, int en_dim, int en_frames,
                 const float* s_data,
                 KTensor& f0_out, KTensor& n_out) {
        @autoreleasepool {
            NSError* error = nil;

            MLMultiArray* en_arr = [[MLMultiArray alloc]
                initWithShape:@[@1, @(en_dim), @(bucket.asr_frames)]
                dataType:MLMultiArrayDataTypeFloat32 error:&error];
            if (error) return false;
            float* en_ptr = (float*)en_arr.dataPointer;
            std::memset(en_ptr, 0, sizeof(float) * en_dim * bucket.asr_frames);
            int actual = std::min(en_frames, bucket.asr_frames);
            for (int ch = 0; ch < en_dim; ch++) {
                std::memcpy(en_ptr + ch * bucket.asr_frames,
                           en_data + ch * en_frames,
                           actual * sizeof(float));
            }

            MLMultiArray* s_arr = [[MLMultiArray alloc]
                initWithShape:@[@1, @128]
                dataType:MLMultiArrayDataTypeFloat32 error:&error];
            if (error) return false;
            std::memcpy((float*)s_arr.dataPointer, s_data, 128 * sizeof(float));

            NSDictionary* input_dict = @{@"en": en_arr, @"s": s_arr};
            auto features = [[MLDictionaryFeatureProvider alloc] initWithDictionary:input_dict error:&error];
            if (error) return false;

            auto result = [bucket.model predictionFromFeatures:features error:&error];
            if (error || !result) {
                std::fprintf(stderr, "CoreML F0N predict failed: %s\n",
                    error ? [[error description] UTF8String] : "unknown");
                return false;
            }

            MLMultiArray* f0_arr = [result featureValueForName:@"F0_pred"].multiArrayValue;
            MLMultiArray* n_arr_out = [result featureValueForName:@"N_pred"].multiArrayValue;
            if (!f0_arr || !n_arr_out) return false;

            f0_out = KTensor::zeros({1, (int64_t)bucket.f0_frames});
            n_out = KTensor::zeros({1, (int64_t)bucket.f0_frames});
            std::memcpy(f0_out.ptr(), (float*)f0_arr.dataPointer, bucket.f0_frames * sizeof(float));
            std::memcpy(n_out.ptr(), (float*)n_arr_out.dataPointer, bucket.f0_frames * sizeof(float));
            return true;
        }
    }

    bool is_available() const { return available_; }

    ~CoreMLF0NPredictor() {
        for (auto& b : buckets_) {
            if (b.model) [b.model release];
        }
    }

private:
    std::vector<BucketInfo> buckets_;
    bool available_ = false;
};
#endif

struct AlignedIntermediates {
    KTensor asr;
    KTensor f0_pred;
    KTensor n_pred;
    KTensor ref_s_dec;
    KTensor ref_s_full;
    bool valid = false;
    std::vector<int64_t> pred_dur;
    int actual_len = 0;
    int64_t total_frames = 0;
    KTensor d_enc;
    KTensor s_style;
};

class KokoroPipeline {
public:
    bool initialize(const std::string& models_dir, const std::string& voice_name, const std::string& variant) {
        std::string base_dir = models_dir + "/" + variant;
        std::string vocab_path = base_dir + "/vocab.json";
        std::string voice_path = base_dir + "/" + voice_name + "_voice.bin";
        std::string variants_dir = base_dir + "/decoder_variants";

        if (!vocab_.load(vocab_path)) {
            std::fprintf(stderr, "Failed to load vocab from %s\n", vocab_path.c_str());
            return false;
        }
        std::printf("Loaded vocab: %zu entries\n", vocab_.phoneme_to_id.size());

        if (!load_voice_pack(voice_path, voice_name)) {
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

        std::string coreml_dir = base_dir + "/coreml";
        coreml_f0n_ = std::make_unique<CoreMLF0NPredictor>();
        if (coreml_f0n_->load(coreml_dir)) {
            std::printf("CoreML F0/N predictor ENABLED (ANE)\n");
        } else {
            coreml_f0n_.reset();
            std::printf("CoreML F0/N predictor not available — using zero F0/N fallback\n");
        }
#else
        std::fprintf(stderr, "FATAL: kokoro-service requires CoreML (macOS). Build with KOKORO_COREML=ON\n");
        return false;
#endif

        std::string espeak_data = tts::resolve_espeak_data_dir();
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

#ifdef __APPLE__
        if (g2p_backend_ != G2PBackend::ESPEAK) {
            std::string g2p_path = models_dir + "/g2p/de_g2p.mlmodelc";
            neural_g2p_ = std::make_unique<NeuralG2P>();
            if (!neural_g2p_->load(g2p_path)) {
                neural_g2p_.reset();
                std::printf("Neural G2P not available — using espeak-ng for German\n");
            } else {
                std::printf("Neural G2P loaded (German)\n");
            }
        } else {
            std::printf("Neural G2P skipped (--g2p espeak)\n");
        }
#endif

        warmup();

        return true;
    }

    void warmup() {
        std::printf("Warming up CoreML models...\n");
        auto t0 = std::chrono::steady_clock::now();
        auto samples = synthesize("Hallo.", 1.0f);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        std::printf("CoreML warmup done: %lldms (%zu samples)\n", (long long)ms, samples.size());
    }

    static bool detect_german(const std::string& text) {
        static const char* umlaut_markers[] = {
            "ä", "ö", "ü", "Ä", "Ö", "Ü", "ß",
            "möchte", "könnte", "würde", "für",
            nullptr
        };
        for (int i = 0; umlaut_markers[i]; i++) {
            if (text.find(umlaut_markers[i]) != std::string::npos) return true;
        }
        static const char* de_words[] = {
            "ich", "und", "nicht", "haben", "werden",
            "bitte", "danke", "gerne",
            nullptr
        };
        for (int i = 0; de_words[i]; i++) {
            std::string word = de_words[i];
            size_t pos = 0;
            while ((pos = text.find(word, pos)) != std::string::npos) {
                bool left_ok = (pos == 0 || !std::isalpha(static_cast<unsigned char>(text[pos - 1])));
                bool right_ok = (pos + word.size() >= text.size() ||
                    !std::isalpha(static_cast<unsigned char>(text[pos + word.size()])));
                if (left_ok && right_ok) return true;
                pos += word.size();
            }
        }
        int ascii_alpha = 0;
        for (unsigned char c : text) {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) ascii_alpha++;
        }
        return ascii_alpha < (int)text.size() / 2;
    }

    static std::string clean_phonemes(const std::string& ph) {
        std::string out;
        out.reserve(ph.size());
        for (size_t i = 0; i < ph.size(); i++) {
            unsigned char c = static_cast<unsigned char>(ph[i]);
            if (c == '\n' || c == '\r') continue;
            if (c == '(' || c == ')') continue;
            out.push_back(ph[i]);
        }
        while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
        return out;
    }

    std::string phonemize(const std::string& text) {
        bool is_de = detect_german(text);
        std::string cache_key = text + (is_de ? "|de" : "|en");
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            auto it = phoneme_cache_.find(cache_key);
            if (it != phoneme_cache_.end()) return it->second;
        }

        std::string result;
#ifdef __APPLE__
        bool use_neural_g2p = is_de &&
            neural_g2p_ && neural_g2p_->is_available() &&
            g2p_backend_ != G2PBackend::ESPEAK;
        if (use_neural_g2p) {
            result = neural_g2p_->phonemize(text);
        } else {
#endif
        {
            std::lock_guard<std::mutex> lock(espeak_mutex_);
            espeak_SetVoiceByName(is_de ? "de" : "en-us");
            const char* ptr = text.c_str();
            while (ptr && *ptr) {
                const char* ph = espeak_TextToPhonemes(
                    (const void**)&ptr, espeakCHARS_UTF8, espeakPHONEMES_IPA);
                if (ph) result += ph;
            }
        }
#ifdef __APPLE__
        }
#endif

        result = clean_phonemes(result);

        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            if (phoneme_cache_.size() >= PHONEME_CACHE_MAX) {
                phoneme_cache_.clear();
            }
            phoneme_cache_[cache_key] = result;
        }

        return result;
    }

    std::vector<float> synthesize(const std::string& text, float speed = 1.0f,
                                   const KTensor* prev_ref_s = nullptr,
                                   KTensor* ref_s_out = nullptr) {
        std::string phonemes = phonemize(text);
        if (phonemes.empty()) return {};

        auto ids = vocab_.encode(phonemes);
        if (ids.size() <= 2) return {};

        KTensor ref_s;
        if (prev_ref_s && prev_ref_s->defined() && prev_ref_s->numel() == 256) {
            ref_s = KTensor::from_data(prev_ref_s->ptr(), {1, 256});
        } else {
            int phoneme_count = static_cast<int>(ids.size()) - 2;
            int voice_idx = std::min(phoneme_count - 1, voice_entries_ - 1);
            voice_idx = std::max(0, voice_idx);
            ref_s = KTensor::from_data(voice_pack_.ptr() + voice_idx * 256, {1, 256});
        }

#ifdef KOKORO_COREML
        return synthesize_coreml(ids, ref_s, speed, ref_s_out);
#else
        (void)speed;
        (void)ref_s_out;
        return {};
#endif
    }

#ifdef KOKORO_COREML
    std::vector<float> decode_part(const AlignedIntermediates& inter,
                                    int phon_start, int phon_end,
                                    int64_t frame_start, int64_t num_frames) {
        if (num_frames <= 0) return {};

        auto asr_slice = KTensor::zeros({1, 512, num_frames});
        int64_t src_time = inter.asr.size(2);
        for (int ch = 0; ch < 512; ch++) {
            std::memcpy(asr_slice.ptr() + (int64_t)ch * num_frames,
                        inter.asr.ptr() + (int64_t)ch * src_time + frame_start,
                        num_frames * sizeof(float));
        }

        KTensor f0_pred, n_pred;
        bool f0n_ok = false;
        if (coreml_f0n_ && coreml_f0n_->is_available() && inter.d_enc.defined() && inter.s_style.defined()) {
            int d_dim = 640;
            auto en = KTensor::zeros({1, (int64_t)d_dim, num_frames});
            {
                int64_t pos = 0;
                for (int i = phon_start; i < phon_end; i++) {
                    int64_t dur = inter.pred_dur[i];
                    if (dur <= 0) continue;
                    int64_t frames = std::min(dur, num_frames - pos);
                    for (int ch = 0; ch < d_dim; ch++) {
                        float val = inter.d_enc.at3(0, i, ch);
                        std::fill_n(en.ptr() + (int64_t)ch * num_frames + pos, frames, val);
                    }
                    pos += dur;
                }
            }
            auto* f0n_bucket = coreml_f0n_->select_bucket((int)num_frames);
            if (f0n_bucket) {
                f0n_ok = coreml_f0n_->predict(*f0n_bucket, en.ptr(), d_dim, (int)num_frames,
                                               inter.s_style.ptr(), f0_pred, n_pred);
                if (f0n_ok) {
                    int64_t f0n_actual = num_frames * 2;
                    if (f0n_actual < (int64_t)f0_pred.size(1))
                        f0_pred.truncate_last_dim(f0n_actual);
                    if (f0n_actual < (int64_t)n_pred.size(1))
                        n_pred.truncate_last_dim(f0n_actual);
                }
            }
        }
        if (!f0n_ok) {
            int64_t f0_len = num_frames * 2;
            f0_pred = KTensor::zeros({1, f0_len});
            n_pred = KTensor::zeros({1, f0_len});
        }

        int f0_len = static_cast<int>(f0_pred.size(1));
        auto* sb = coreml_split_decoder_->select_bucket(f0_len);
        if (!sb) return {};

        int asr_frames = sb->asr_frames;
        int f0_frames = sb->f0_frames;

        auto f0_padded = KTensor::zeros({1, (int64_t)f0_frames});
        auto n_padded = KTensor::zeros({1, (int64_t)f0_frames});
        int f0_actual = std::min(f0_len, f0_frames);
        std::memcpy(f0_padded.ptr(), f0_pred.ptr(), f0_actual * sizeof(float));
        std::memcpy(n_padded.ptr(), n_pred.ptr(), f0_actual * sizeof(float));

        float* f0_ptr = f0_padded.ptr();
        int f0_frames_cap = f0_frames;
        auto har_future = std::async(std::launch::async,
            [this, f0_ptr, f0_frames_cap]() {
                return coreml_split_decoder_->compute_har(f0_ptr, f0_frames_cap);
            });

        auto asr_padded = KTensor::zeros({1, 512, (int64_t)asr_frames});
        int asr_actual = std::min((int)num_frames, asr_frames);
        for (int ch = 0; ch < 512; ch++) {
            std::memcpy(asr_padded.ptr() + (int64_t)ch * asr_frames,
                        asr_slice.ptr() + (int64_t)ch * num_frames,
                        asr_actual * sizeof(float));
        }

        auto har = har_future.get();
        if (har.empty()) return {};

        int har_time = sb->har_time;
        int har_channels = sb->har_channels * 2;
        int64_t har_expected = (int64_t)har_channels * har_time;
        std::vector<float> har_padded(har_expected, 0.0f);
        int har_actual_frames = std::min((int)(har.size() / har_channels), har_time);
        for (int c = 0; c < har_channels; c++) {
            std::memcpy(har_padded.data() + c * har_time,
                       har.data() + c * har_actual_frames,
                       har_actual_frames * sizeof(float));
        }

        auto audio = coreml_split_decoder_->decode(*sb, asr_padded, f0_padded, n_padded,
                                                     inter.ref_s_dec, har_padded);
        if (!audio.empty() && num_frames < asr_frames) {
            int64_t samples_per_frame = (int64_t)audio.size() / asr_frames;
            int64_t trim_to = num_frames * samples_per_frame;
            if (trim_to > 0 && (int64_t)audio.size() > trim_to)
                audio.resize(trim_to);
        }
        return audio;
    }

    std::vector<float> synthesize_coreml(const std::vector<int64_t>& ids,
                                          const KTensor& ref_s,
                                          float speed,
                                          KTensor* ref_s_out = nullptr) {
        if (!coreml_available_ || !coreml_split_decoder_) return {};

        auto intermediates = run_duration_and_align(ids, ref_s, speed);
        if (!intermediates.valid) return {};

        if (ref_s_out && intermediates.ref_s_full.defined() && intermediates.ref_s_full.numel() == 256) {
            *ref_s_out = intermediates.ref_s_full;
        }

        const int max_asr = coreml_split_decoder_->max_asr_frames();
        if (intermediates.total_frames > max_asr && intermediates.actual_len > 2) {
            struct ChunkRange { int phon_start; int phon_end; int64_t frame_start; int64_t num_frames; };
            std::vector<ChunkRange> chunks;
            int64_t cum = 0;
            int chunk_start = 0;
            int64_t chunk_frame_start = 0;
            for (int i = 0; i < intermediates.actual_len; i++) {
                int64_t next = cum + intermediates.pred_dur[i];
                if (next > max_asr && i > chunk_start) {
                    chunks.push_back({chunk_start, i, chunk_frame_start, cum});
                    chunk_frame_start += cum;
                    chunk_start = i;
                    cum = intermediates.pred_dur[i];
                } else {
                    cum = next;
                }
            }
            if (chunk_start < intermediates.actual_len) {
                chunks.push_back({chunk_start, intermediates.actual_len, chunk_frame_start, cum});
            }

            std::fprintf(stderr, "Splitting long synthesis (total_frames=%lld > max_asr=%d) into %zu chunks\n",
                        (long long)intermediates.total_frames, max_asr, chunks.size());

            std::vector<float> result;
            // Equal-power crossfade at chunk seams masks F0/N discontinuities and
            // zero-padded edges between independently-decoded segments. 8ms @24kHz
            // = 192 samples — short enough to be inaudible musically, long enough
            // to suppress the click/pop and brief energy gap that otherwise harms
            // Whisper re-transcription.
            constexpr int kCrossfadeSamples = 192;
            for (auto& c : chunks) {
                auto part = decode_part(intermediates, c.phon_start, c.phon_end,
                                         c.frame_start, c.num_frames);
                if (part.empty()) continue;
                if (result.empty()) {
                    result = std::move(part);
                    continue;
                }
                int xf = std::min<int>({kCrossfadeSamples,
                                         (int)result.size(),
                                         (int)part.size()});
                if (xf <= 0) {
                    result.insert(result.end(), part.begin(), part.end());
                    continue;
                }
                int tail = (int)result.size() - xf;
                for (int i = 0; i < xf; i++) {
                    float t = (float)(i + 1) / (float)(xf + 1);
                    // Equal-power (constant energy) crossfade weights.
                    float w_out = std::cos(t * 1.5707963267948966f);
                    float w_in  = std::sin(t * 1.5707963267948966f);
                    result[tail + i] = result[tail + i] * w_out + part[i] * w_in;
                }
                result.insert(result.end(), part.begin() + xf, part.end());
            }
            return result;
        }

        int f0_len = static_cast<int>(intermediates.f0_pred.size(1));
        auto* sb = coreml_split_decoder_->select_bucket(f0_len);
        if (!sb) {
            std::fprintf(stderr, "No decoder bucket for f0_len=%d\n", f0_len);
            return {};
        }

        int asr_frames = sb->asr_frames;
        int f0_frames = sb->f0_frames;

        auto f0_padded = KTensor::zeros({1, (int64_t)f0_frames});
        auto n_padded = KTensor::zeros({1, (int64_t)f0_frames});
        int f0_actual = std::min(f0_len, f0_frames);
        std::memcpy(f0_padded.ptr(), intermediates.f0_pred.ptr(), f0_actual * sizeof(float));
        std::memcpy(n_padded.ptr(), intermediates.n_pred.ptr(), f0_actual * sizeof(float));

        float* f0_ptr = f0_padded.ptr();
        int f0_frames_cap = f0_frames;
        auto har_future = std::async(std::launch::async,
            [this, f0_ptr, f0_frames_cap]() {
                return coreml_split_decoder_->compute_har(f0_ptr, f0_frames_cap);
            });

        auto asr_padded = KTensor::zeros({1, 512, (int64_t)asr_frames});
        int asr_actual = std::min((int)intermediates.asr.size(2), asr_frames);
        int64_t src_time = intermediates.asr.size(2);
        for (int ch = 0; ch < 512; ch++) {
            std::memcpy(asr_padded.ptr() + (int64_t)ch * asr_frames,
                        intermediates.asr.ptr() + (int64_t)ch * src_time,
                        asr_actual * sizeof(float));
        }

        auto har = har_future.get();
        if (har.empty()) {
            std::fprintf(stderr, "HAR computation failed for bucket %s\n", sb->name.c_str());
            return {};
        }

        int har_time = sb->har_time;
        int har_channels = sb->har_channels * 2;
        int64_t har_expected = (int64_t)har_channels * har_time;
        std::vector<float> har_padded(har_expected, 0.0f);
        int har_actual_frames = std::min((int)(har.size() / har_channels), har_time);
        for (int c = 0; c < har_channels; c++) {
            std::memcpy(har_padded.data() + c * har_time,
                       har.data() + c * har_actual_frames,
                       har_actual_frames * sizeof(float));
        }

        auto audio = coreml_split_decoder_->decode(*sb, asr_padded, f0_padded, n_padded,
                                              intermediates.ref_s_dec, har_padded);
        int actual_asr = static_cast<int>(intermediates.asr.size(2));
        if (!audio.empty() && actual_asr < asr_frames) {
            int64_t samples_per_frame = (int64_t)audio.size() / asr_frames;
            int64_t trim_to = (int64_t)actual_asr * samples_per_frame;
            if (trim_to > 0 && (int64_t)audio.size() > trim_to)
                audio.resize(trim_to);
        }
        return audio;
    }

    AlignedIntermediates run_duration_and_align(const std::vector<int64_t>& ids,
                                                 const KTensor& ref_s,
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
        KTensor ref_s_flat = KTensor::from_data(ref_s.ptr(), {256});
        if (!coreml_duration_->predict(ids_vec, ref_s_flat, speed, mask_vec, dur_out)) {
            return result;
        }

        int64_t total_frames = 0;
        for (int i = 0; i < actual_len && i < 512; i++) {
            total_frames += dur_out.pred_dur[i];
        }
        if (total_frames <= 0) return result;

        result.pred_dur.resize(actual_len);
        for (int i = 0; i < actual_len; i++) result.pred_dur[i] = dur_out.pred_dur[i];
        result.actual_len = actual_len;
        result.total_frames = total_frames;
        result.d_enc = std::move(dur_out.d);
        result.s_style = std::move(dur_out.s);

        int t_en_dim = 512;
        auto asr = KTensor::zeros({1, (int64_t)t_en_dim, total_frames});

        {
            int64_t pos = 0;
            for (int i = 0; i < actual_len && i < 512; i++) {
                int64_t dur = dur_out.pred_dur[i];
                if (dur <= 0) continue;
                int64_t frames = std::min(dur, total_frames - pos);
                for (int d = 0; d < t_en_dim; d++) {
                    float val = dur_out.t_en.at3(0, d, i);
                    std::fill_n(asr.ptr() + (int64_t)d * total_frames + pos, frames, val);
                }
                pos += dur;
            }
        }

        int64_t f0_frames = total_frames * 2;
        result.asr = std::move(asr);

        bool f0n_ok = false;
#ifdef KOKORO_COREML
        if (coreml_f0n_ && coreml_f0n_->is_available() && result.d_enc.defined() && result.s_style.defined()) {
            int d_dim = 640;
            auto en = KTensor::zeros({1, (int64_t)d_dim, total_frames});
            {
                int64_t pos = 0;
                for (int i = 0; i < actual_len && i < 512; i++) {
                    int64_t dur = result.pred_dur[i];
                    if (dur <= 0) continue;
                    int64_t frames = std::min(dur, total_frames - pos);
                    for (int ch = 0; ch < d_dim; ch++) {
                        float val = result.d_enc.at3(0, i, ch);
                        std::fill_n(en.ptr() + (int64_t)ch * total_frames + pos, frames, val);
                    }
                    pos += dur;
                }
            }

            auto* f0n_bucket = coreml_f0n_->select_bucket((int)total_frames);
            if (f0n_bucket) {
                f0n_ok = coreml_f0n_->predict(*f0n_bucket, en.ptr(), d_dim, (int)total_frames,
                                               result.s_style.ptr(), result.f0_pred, result.n_pred);
                if (f0n_ok) {
                    int64_t f0n_actual = total_frames * 2;
                    if (f0n_actual < (int64_t)result.f0_pred.size(1))
                        result.f0_pred.truncate_last_dim(f0n_actual);
                    if (f0n_actual < (int64_t)result.n_pred.size(1))
                        result.n_pred.truncate_last_dim(f0n_actual);
                    std::fprintf(stderr, "F0/N predicted via CoreML (bucket %s, frames=%lld)\n",
                               f0n_bucket->name.c_str(), (long long)total_frames);
                }
            }
        }
#endif
        if (!f0n_ok) {
            result.f0_pred = KTensor::zeros({1, f0_frames});
            result.n_pred = KTensor::zeros({1, f0_frames});
        }

        result.ref_s_dec = KTensor::from_data(dur_out.ref_s_out.ptr(), {1, 128});
        if (!result.ref_s_dec.defined() || result.ref_s_dec.size(1) < 128) {
            result.ref_s_dec = KTensor::from_data(ref_s.ptr(), {1, 128});
        }
        if (dur_out.ref_s_out.defined() && dur_out.ref_s_out.numel() == 256) {
            result.ref_s_full = dur_out.ref_s_out;
        }
        result.valid = true;
        return result;
    }
#endif

    bool has_coreml() const { return coreml_available_; }

#ifdef __APPLE__
    void set_g2p_backend(G2PBackend backend) { g2p_backend_ = backend; }
    bool has_neural_g2p() const { return neural_g2p_ && neural_g2p_->is_available(); }
#endif

private:
    bool load_voice_pack(const std::string& bin_path, const std::string& voice_name) {
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
            voice_pack_ = KTensor::zeros({(int64_t)voice_entries_, 256});
            f.read(reinterpret_cast<char*>(voice_pack_.ptr()), file_size);
            std::printf("Loaded voice '%s' from bin: [%d, 256]\n", voice_name.c_str(), voice_entries_);
            return true;
        }

        std::fprintf(stderr, "Voice file not found: %s\n", bin_path.c_str());
        return false;
    }

    KTensor voice_pack_;
    int voice_entries_ = 0;
    KokoroVocab vocab_;
    std::mutex espeak_mutex_;
    std::unordered_map<std::string, std::string> phoneme_cache_;
    std::mutex cache_mutex_;
    bool coreml_available_ = false;
#ifdef KOKORO_COREML
    std::unique_ptr<CoreMLDurationModel> coreml_duration_;
    std::unique_ptr<CoreMLSplitDecoder> coreml_split_decoder_;
    std::unique_ptr<CoreMLF0NPredictor> coreml_f0n_;
#endif
#ifdef __APPLE__
    std::unique_ptr<NeuralG2P> neural_g2p_;
    G2PBackend g2p_backend_ = G2PBackend::AUTO;
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
    KTensor last_ref_s;
};

class KokoroService {
public:
    KokoroService() = default;

    bool initialize(const std::string& voice_name, const std::string& variant) {
        const char* env_models = std::getenv("WHISPERTALK_MODELS_DIR");
        std::string models_dir = env_models ? env_models :
#ifdef WHISPERTALK_MODELS_DIR
            WHISPERTALK_MODELS_DIR;
#else
            "models";
#endif
        if (!pipeline_.initialize(models_dir, voice_name, variant)) {
            std::fprintf(stderr, "Failed to initialize Kokoro pipeline\n");
            return false;
        }

        log_fwd_.init(FRONTEND_LOG_PORT, "KOKORO_ENGINE");

        std::printf("Kokoro TTS Service initialized (German, variant=%s, voice=%s, decoder=coreml-split)\n",
                   variant.c_str(), voice_name.c_str());
        std::printf("  CoreML duration: %s\n", pipeline_.has_coreml() ? "ENABLED (ANE)" : "DISABLED");

        engine_.set_name("kokoro");
        EngineAudioFormat fmt;
        fmt.sample_rate = KOKORO_SAMPLE_RATE;
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
            std::fprintf(stderr, "[kokoro] received SHUTDOWN from TTS dock — signalling exit\n");
            // SHUTDOWN is dispatched from the EngineClient's own recv thread.
            // Calling engine_.shutdown() here would self-join and deadlock.
            // Instead, just clear running_ so the main recv loop in run()
            // exits, which then performs an orderly shutdown (joins worker
            // threads, calls engine_.shutdown() from outside the recv thread,
            // flushes LogForwarder, and lets main() return normally).
            running_.store(false);
        });

        if (!engine_.start()) {
            std::fprintf(stderr, "Failed to start TTS engine client\n");
            return false;
        }

        return true;
    }

    void run() {
        std::thread cmd_thread(&KokoroService::command_listener_loop, this);

        std::printf("Kokoro service ready - connecting to TTS dock at 127.0.0.1:%u\n",
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

#ifdef __APPLE__
    void set_g2p_backend(G2PBackend backend) {
        pipeline_.set_g2p_backend(backend);
    }
    bool has_neural_g2p() const {
        return pipeline_.has_neural_g2p();
    }
#endif

private:
    void command_listener_loop() {
        uint16_t port = KOKORO_ENGINE_CMD_PORT;
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
            samples = pipeline_.synthesize(text, speed_.load());
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (samples.empty()) {
                return "ERROR:synthesis failed\n";
            }

            double duration_s = (double)samples.size() / KOKORO_SAMPLE_RATE;
            double rtf = (elapsed / 1000.0) / duration_s;

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
                samples = pipeline_.synthesize(text, speed_.load());
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
            samples = pipeline_.synthesize(text, speed_.load());
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            if (samples.empty()) return "ERROR:synthesis failed\n";

            tts::normalize_audio(samples);
            tts::apply_fade_in(samples);

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
            std::string status = "ACTIVE_CALLS:" + std::to_string(calls_.size())
                + ":DOCK:" + (engine_.is_connected() ? "connected" : "disconnected")
                + ":SPEED:" + spd;
#ifdef __APPLE__
            status += ":G2P:" + std::string(pipeline_.has_neural_g2p() ? "neural" : "espeak");
#endif
            return status + "\n";
        }
        return "ERROR:Unknown command\n";
    }

    void prewarm_call(uint32_t call_id) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto it = calls_.find(call_id);
        if (it == calls_.end()) {
            auto ctx = std::make_shared<CallContext>();
            ctx->call_id = call_id;
            ctx->worker = std::thread(&KokoroService::call_worker, this, ctx);
            it = calls_.emplace(call_id, std::move(ctx)).first;
            log_fwd_.forward(LogLevel::DEBUG, call_id, "Prewarmed synthesis thread on SPEECH_IDLE");
        }
        it->second->interrupted = false;
    }

    void dispatch_text_packet(const Packet& pkt) {
        std::string text(reinterpret_cast<const char*>(pkt.payload.data()), pkt.payload.size());

        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto it = calls_.find(pkt.call_id);
        if (it == calls_.end()) {
            auto ctx = std::make_shared<CallContext>();
            ctx->call_id = pkt.call_id;
            ctx->worker = std::thread(&KokoroService::call_worker, this, ctx);
            it = calls_.emplace(pkt.call_id, std::move(ctx)).first;
            std::printf("Started synthesis thread for call %u\n", pkt.call_id);
            log_fwd_.forward(LogLevel::INFO, pkt.call_id, "Started synthesis thread");
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
            KTensor ref_s_result;
            auto start = std::chrono::steady_clock::now();
            samples = pipeline_.synthesize(text, speed_.load(), &ctx->last_ref_s, &ref_s_result);
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

            if (ref_s_result.defined() && ref_s_result.numel() == 256) {
                ctx->last_ref_s = std::move(ref_s_result);
            }

            float raw_peak = tts::normalize_audio(samples);
            tts::apply_fade_in(samples);

            const char* norm_tag = (raw_peak > 0.01f && std::abs(raw_peak - 0.90f) > 0.001f)
                                   ? " -> normalized" : "";
            std::printf("Synthesized %zu samples in %lldms for call %u (raw_peak=%.3f%s)\n",
                        samples.size(), (long long)elapsed, ctx->call_id, raw_peak, norm_tag);
            log_fwd_.forward(LogLevel::INFO, ctx->call_id, "Synthesized %zu samples in %lldms (raw_peak=%.3f%s)",
                             samples.size(), (long long)elapsed, raw_peak, norm_tag);

            send_audio_to_downstream(ctx->call_id, samples, &ctx->interrupted);
        }
    }

    void send_audio_to_downstream(uint32_t call_id, const std::vector<float>& samples,
                                   std::atomic<bool>* interrupted = nullptr) {
        if (!engine_.is_connected()) {
            std::printf("TTS dock not connected - discarding audio for call %u\n", call_id);
            return;
        }

        static constexpr size_t CHUNK_SAMPLES = whispertalk::tts::kTTSMaxFrameSamples;
        constexpr size_t header_size = whispertalk::tts::kTTSAudioHeaderBytes;
        size_t total_sent = 0;

        for (size_t offset = 0; offset < samples.size(); offset += CHUNK_SAMPLES) {
            if (interrupted && interrupted->load()) {
                log_fwd_.forward(LogLevel::DEBUG, call_id, "Audio send interrupted at chunk %zu/%zu",
                                 offset / CHUNK_SAMPLES, (samples.size() + CHUNK_SAMPLES - 1) / CHUNK_SAMPLES);
                return;
            }
            size_t count = std::min(CHUNK_SAMPLES, samples.size() - offset);

            Packet audio_pkt;
            audio_pkt.call_id = call_id;
            audio_pkt.payload_size = static_cast<uint32_t>(header_size + count * sizeof(float));
            audio_pkt.payload.resize(audio_pkt.payload_size);

            int32_t sr = KOKORO_SAMPLE_RATE;
            std::memcpy(audio_pkt.payload.data(), &sr, sizeof(int32_t));
            // t_engine_out_us (big-endian uint64) for per-frame latency.
            uint64_t t_out_us = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count());
            uint8_t ts_be[8];
            for (int i = 0; i < 8; ++i) ts_be[7 - i] = static_cast<uint8_t>((t_out_us >> (i * 8)) & 0xff);
            std::memcpy(audio_pkt.payload.data() + sizeof(int32_t), ts_be, sizeof(ts_be));
            std::memcpy(audio_pkt.payload.data() + header_size,
                       samples.data() + offset, count * sizeof(float));

            audio_pkt.trace.record(whispertalk::ServiceType::TTS_SERVICE, 0);
            if (engine_.send_audio(audio_pkt)) {
                total_sent += count;
            } else {
                std::fprintf(stderr, "Failed to send audio chunk for call %u at offset %zu\n", call_id, offset);
                log_fwd_.forward(LogLevel::ERROR, call_id, "Failed to send audio chunk to TTS dock");
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

    EngineClient engine_;
    LogForwarder log_fwd_;
    KokoroPipeline pipeline_;
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

    std::string variant = "kokoro-german";
    std::string voice = "df_eva";
    std::string log_level = "INFO";
    std::string g2p_str = "auto";

    if (const char* env_variant = std::getenv("KOKORO_VARIANT")) {
        if (env_variant[0] != '\0') variant = env_variant;
    }
    if (const char* env_voice = std::getenv("KOKORO_VOICE")) {
        if (env_voice[0] != '\0') voice = env_voice;
    }

    static struct option long_opts[] = {
        {"voice",     required_argument, 0, 'v'},
        {"variant",   required_argument, 0, 'V'},
        {"log-level", required_argument, 0, 'L'},
        {"g2p",       required_argument, 0, 'g'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "v:V:L:g:h", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'v': voice = optarg; break;
            case 'V': variant = optarg; break;
            case 'L': log_level = optarg; break;
            case 'g': g2p_str = optarg; break;
            case 'h':
                std::printf("Usage: kokoro-service [OPTIONS]\n");
                std::printf("  --variant NAME    Model variant subdir under models/ (default: kokoro-german).\n");
                std::printf("                    also: kokoro-german-martin for Kikiri Martin. Env: KOKORO_VARIANT\n");
                std::printf("  --voice NAME      Voice to use (default: df_eva; also dm_bernd). Env: KOKORO_VOICE\n");
                std::printf("  --log-level LEVEL Log level: ERROR WARN INFO DEBUG TRACE (default: INFO)\n");
                std::printf("  --g2p BACKEND     G2P backend: auto|neural|espeak (default: auto)\n");
                return 0;
            default: break;
        }
    }

    std::printf("Starting Kokoro TTS Service (variant=%s, voice=%s, decoder=coreml-split, g2p=%s)\n",
                variant.c_str(), voice.c_str(), g2p_str.c_str());

    KokoroService service;
    g_service = &service;

#ifdef __APPLE__
    {
        G2PBackend g2p_backend = G2PBackend::AUTO;
        if (g2p_str == "neural") g2p_backend = G2PBackend::NEURAL;
        else if (g2p_str == "espeak") g2p_backend = G2PBackend::ESPEAK;
        service.set_g2p_backend(g2p_backend);
    }
#endif

    if (!service.initialize(voice, variant)) {
        return 1;
    }

#ifdef __APPLE__
    if (g2p_str == "neural" && !service.has_neural_g2p()) {
        std::fprintf(stderr, "[WARN] --g2p neural requested but neural G2P model failed to load;"
                             " falling back to espeak-ng\n");
    }
#endif

    service.set_log_level(log_level.c_str());
    service.run();

    return 0;
}
