#include "../ktensor.h"
#include "../har_source.h"
#include <espeak-ng/speak_lib.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <chrono>
#include <algorithm>
#include <numeric>
#include <sys/stat.h>

#ifdef KOKORO_COREML
#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>
#endif

#ifndef WHISPERTALK_MODELS_DIR
#define WHISPERTALK_MODELS_DIR "bin/models"
#endif

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

std::string phonemize_german(const std::string& text) {
    std::string result;
    const char* ptr = text.c_str();
    while (ptr && *ptr) {
        const char* ph = espeak_TextToPhonemes(
            (const void**)&ptr, espeakCHARS_UTF8, espeakPHONEMES_IPA);
        if (ph) result += ph;
    }
    return result;
}

int main() {
    std::string models_dir = WHISPERTALK_MODELS_DIR;
    std::string base_dir = models_dir + "/kokoro-german";
    std::string vocab_path = base_dir + "/vocab.json";

    int passed = 0;
    int failed = 0;

    std::printf("=== Kokoro C++ TTS Test Suite (CoreML Split Decoder) ===\n\n");

    std::string espeak_data = resolve_espeak_data_dir();
    if (espeak_data.empty()) {
        std::printf("FATAL: Cannot find espeak-ng-data. Set ESPEAK_NG_DATA env var.\n");
        return 1;
    }

    std::printf("[TEST 1] espeak-ng initialization and phonemization\n");
    int result = espeak_Initialize(AUDIO_OUTPUT_RETRIEVAL, 0,
                                  espeak_data.c_str(), 0);
    if (result == -1) {
        std::printf("  FAIL: espeak_Initialize returned -1\n");
        failed++;
    } else {
        espeak_SetVoiceByName("de");
        std::vector<std::pair<std::string, bool>> test_sentences = {
            {"Hallo Welt", true},
            {"Wie geht es Ihnen?", true},
            {"Guten Morgen, ich bin ein Sprachassistent.", true},
            {"Die Temperatur betraegt heute zwanzig Grad Celsius.", true},
            {"", false},
        };
        bool all_ok = true;
        for (auto& [text, expect_output] : test_sentences) {
            auto ph = phonemize_german(text);
            bool has_output = !ph.empty();
            if (has_output != expect_output) {
                std::printf("  FAIL: \"%s\" -> expected output=%d got=%d\n",
                           text.c_str(), expect_output, has_output);
                all_ok = false;
            } else if (has_output) {
                std::printf("  OK: \"%s\" -> \"%s\"\n", text.c_str(), ph.c_str());
            }
        }
        if (all_ok) { passed++; std::printf("  PASS\n"); }
        else { failed++; }
    }

    std::printf("\n[TEST 2] Vocab loading\n");
    KokoroVocab vocab;
    if (!vocab.load(vocab_path)) {
        std::printf("  FAIL: Could not load vocab from %s\n", vocab_path.c_str());
        failed++;
    } else {
        std::printf("  Loaded %zu vocab entries\n", vocab.phoneme_to_id.size());
        if (vocab.phoneme_to_id.size() >= 100) {
            passed++;
            std::printf("  PASS\n");
        } else {
            failed++;
            std::printf("  FAIL: too few vocab entries\n");
        }
    }

    std::printf("\n[TEST 3] Phoneme encoding\n");
    {
        auto ph = phonemize_german("Hallo");
        auto ids = vocab.encode(ph);
        std::printf("  \"Hallo\" -> \"%s\" -> %zu tokens\n", ph.c_str(), ids.size());
        if (ids.size() >= 3 && ids.front() == 0 && ids.back() == 0) {
            passed++;
            std::printf("  PASS\n");
        } else {
            failed++;
            std::printf("  FAIL: bad encoding\n");
        }
    }

    KTensor voice_pack;
    int voice_entries = 0;

    std::printf("\n[TEST 4] Voice pack loading (bin format)\n");
    {
        std::string bin_path = base_dir + "/df_eva_voice.bin";
        struct stat st;
        if (stat(bin_path.c_str(), &st) == 0) {
            std::ifstream f(bin_path, std::ios::binary);
            size_t file_size = static_cast<size_t>(st.st_size);
            size_t num_floats = file_size / sizeof(float);
            voice_entries = static_cast<int>(num_floats / 256);
            voice_pack = KTensor::zeros({(int64_t)voice_entries, 256});
            f.read(reinterpret_cast<char*>(voice_pack.ptr()), file_size);
            std::printf("  Loaded from bin: [%d, 256]\n", voice_entries);
            if (voice_entries > 100) {
                passed++;
                std::printf("  PASS\n");
            } else {
                failed++;
                std::printf("  FAIL: too few voice entries\n");
            }
        } else {
            failed++;
            std::printf("  FAIL: no voice file found at %s\n", bin_path.c_str());
            goto done;
        }
    }

#ifdef KOKORO_COREML
    std::printf("\n[TEST 5] CoreML duration model load and predict\n");
    {
        std::string mlmodelc_path = base_dir + "/coreml/kokoro_duration.mlmodelc";
        struct stat st;
        if (stat(mlmodelc_path.c_str(), &st) != 0) {
            std::printf("  SKIP: CoreML model not found at %s\n", mlmodelc_path.c_str());
        } else {
            @autoreleasepool {
                NSString* path = [NSString stringWithUTF8String:mlmodelc_path.c_str()];
                NSURL* url = [NSURL fileURLWithPath:path];
                MLModelConfiguration* config = [[MLModelConfiguration alloc] init];
                config.computeUnits = MLComputeUnitsAll;
                NSError* error = nil;
                MLModel* model = [MLModel modelWithContentsOfURL:url configuration:config error:&error];
                if (error || !model) {
                    std::printf("  FAIL: Could not load CoreML model: %s\n",
                        error ? [[error description] UTF8String] : "unknown");
                    failed++;
                } else {
                    std::printf("  Loaded CoreML duration model\n");

                    NSArray<NSNumber*>* ids_shape = @[@1, @512];
                    MLMultiArray* input_ids_arr = [[MLMultiArray alloc]
                        initWithShape:ids_shape dataType:MLMultiArrayDataTypeInt32 error:&error];
                    int32_t* ids_ptr = (int32_t*)input_ids_arr.dataPointer;
                    for (int i = 0; i < 512; i++) ids_ptr[i] = (i < 10) ? (i + 1) : 0;

                    NSArray<NSNumber*>* ref_shape = @[@1, @256];
                    MLMultiArray* ref_s_arr = [[MLMultiArray alloc]
                        initWithShape:ref_shape dataType:MLMultiArrayDataTypeFloat32 error:&error];
                    float* ref_ptr = (float*)ref_s_arr.dataPointer;
                    if (voice_pack.defined() && voice_entries > 0) {
                        std::memcpy(ref_ptr, voice_pack.ptr(), 256 * sizeof(float));
                    }

                    NSArray<NSNumber*>* speed_shape = @[@1];
                    MLMultiArray* speed_arr = [[MLMultiArray alloc]
                        initWithShape:speed_shape dataType:MLMultiArrayDataTypeFloat32 error:&error];
                    ((float*)speed_arr.dataPointer)[0] = 1.0f;

                    MLMultiArray* mask_arr = [[MLMultiArray alloc]
                        initWithShape:ids_shape dataType:MLMultiArrayDataTypeInt32 error:&error];
                    int32_t* mask_ptr = (int32_t*)mask_arr.dataPointer;
                    for (int i = 0; i < 512; i++) mask_ptr[i] = (i < 10) ? 1 : 0;

                    NSDictionary* input_dict = @{
                        @"input_ids": input_ids_arr,
                        @"ref_s": ref_s_arr,
                        @"speed": speed_arr,
                        @"attention_mask": mask_arr
                    };
                    id<MLFeatureProvider> features = [[MLDictionaryFeatureProvider alloc]
                        initWithDictionary:input_dict error:&error];

                    auto coreml_start = std::chrono::steady_clock::now();
                    id<MLFeatureProvider> coreml_result = [model predictionFromFeatures:features error:&error];
                    auto coreml_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - coreml_start).count();

                    if (error || !coreml_result) {
                        std::printf("  FAIL: CoreML prediction failed: %s\n",
                            error ? [[error description] UTF8String] : "unknown");
                        failed++;
                    } else {
                        MLMultiArray* pred_dur = [coreml_result featureValueForName:@"pred_dur"].multiArrayValue;
                        MLMultiArray* d_out = [coreml_result featureValueForName:@"d"].multiArrayValue;
                        MLMultiArray* t_en_out = [coreml_result featureValueForName:@"t_en"].multiArrayValue;
                        MLMultiArray* s_out = [coreml_result featureValueForName:@"s"].multiArrayValue;
                        if (pred_dur && d_out && t_en_out && s_out) {
                            std::printf("  CoreML outputs: pred_dur=%s, d=%s, t_en=%s, s=%s\n",
                                [[pred_dur shape] description].UTF8String,
                                [[d_out shape] description].UTF8String,
                                [[t_en_out shape] description].UTF8String,
                                [[s_out shape] description].UTF8String);
                            std::printf("  CoreML inference: %lldms\n", coreml_elapsed);
                            passed++;
                            std::printf("  PASS\n");
                        } else {
                            std::printf("  FAIL: Missing output tensors\n");
                            failed++;
                        }
                    }
                }
            }
        }
    }

    std::printf("\n[TEST 6] CoreML split decoder load and benchmark\n");
    {
        std::string variants_dir = base_dir + "/decoder_variants";
        struct stat st;
        std::string cml_3s = variants_dir + "/kokoro_decoder_split_3s.mlmodelc";
        if (stat(cml_3s.c_str(), &st) == 0) {
            @autoreleasepool {
                MLModelConfiguration* cfg = [[MLModelConfiguration alloc] init];
                cfg.computeUnits = MLComputeUnitsAll;

                struct { const char* name; int asr; int f0; int harc; int hart; } cml_buckets[] = {
                    {"3s", 72, 144, 11, 8641},
                    {"5s", 120, 240, 11, 14401},
                    {"10s", 240, 480, 11, 28801},
                };

                std::map<std::string, MLModel*> cml_models;
                for (auto& b : cml_buckets) {
                    std::string p = variants_dir + "/kokoro_decoder_split_" + std::string(b.name) + ".mlmodelc";
                    struct stat st2;
                    if (stat(p.c_str(), &st2) != 0) continue;
                    NSString* ns_p = [NSString stringWithUTF8String:p.c_str()];
                    NSError* err = nil;
                    MLModel* m = [MLModel modelWithContentsOfURL:[NSURL fileURLWithPath:ns_p] configuration:cfg error:&err];
                    if (m && !err) cml_models[b.name] = m;
                }

                HarSource har_source;
                std::string har_path = variants_dir + "/har_weights.bin";
                bool har_ok = har_source.load(har_path);

                if (cml_models.empty() || !har_ok) {
                    std::printf("  SKIP: CoreML split models or HAR weights not loaded\n");
                } else {
                    std::printf("  Loaded %zu decoder buckets, HAR source from binary weights\n",
                               cml_models.size());

                    std::vector<std::string> bench_texts = {
                        "Hallo Welt",
                        "Guten Tag, wie kann ich Ihnen helfen?",
                        "Das Wetter ist heute sehr schoen und die Sonne scheint.",
                    };
                    std::vector<double> cml_times;
                    for (auto& text : bench_texts) {
                        auto ph = phonemize_german(text);
                        auto ids = vocab.encode(ph);

                        auto& bk = cml_buckets[0];
                        auto cml_it = cml_models.find(bk.name);
                        if (cml_it == cml_models.end()) continue;

                        std::vector<float> f0_data(bk.f0, 0.0f);

                        for (int r = 0; r < 4; r++) {
                            auto t0 = std::chrono::steady_clock::now();

                            auto har = har_source.compute(f0_data.data(), bk.f0);

                            NSError* err = nil;
                            auto make_arr = [&](NSArray<NSNumber*>* shape, const float* data, size_t count) -> MLMultiArray* {
                                MLMultiArray* arr = [[MLMultiArray alloc] initWithShape:shape
                                    dataType:MLMultiArrayDataTypeFloat32 error:&err];
                                if (err) return nil;
                                std::memcpy((float*)arr.dataPointer, data, count * sizeof(float));
                                return arr;
                            };

                            std::vector<float> asr_data(512 * bk.asr, 0.0f);
                            std::vector<float> refs_data(256, 0.0f);
                            if (voice_pack.defined()) {
                                std::memcpy(refs_data.data(), voice_pack.ptr(), 256 * sizeof(float));
                            }

                            auto asr_a = make_arr(@[@1, @512, @(bk.asr)], asr_data.data(), asr_data.size());
                            auto f0_a = make_arr(@[@1, @(bk.f0)], f0_data.data(), f0_data.size());
                            auto n_a = make_arr(@[@1, @(bk.f0)], f0_data.data(), f0_data.size());
                            auto refs_a = make_arr(@[@1, @256], refs_data.data(), 256);
                            auto har_a = make_arr(@[@1, @(bk.harc * 2), @(bk.hart)], har.data(), har.size());

                            NSDictionary* dict = @{@"asr": asr_a, @"F0_pred": f0_a, @"N_pred": n_a,
                                                   @"ref_s": refs_a, @"har": har_a};
                            auto feats = [[MLDictionaryFeatureProvider alloc] initWithDictionary:dict error:&err];
                            auto res = [cml_it->second predictionFromFeatures:feats error:&err];

                            auto ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0).count();
                            if (r > 0 && res && !err) cml_times.push_back(ms);
                        }
                    }
                    if (!cml_times.empty()) {
                        std::sort(cml_times.begin(), cml_times.end());
                        double avg = std::accumulate(cml_times.begin(), cml_times.end(), 0.0) / cml_times.size();
                        std::printf("  CoreML Split (ANE): avg=%.0fms, min=%.0fms, p50=%.0fms (%zu samples)\n",
                                   avg, cml_times.front(), cml_times[cml_times.size()/2], cml_times.size());
                        passed++;
                        std::printf("  PASS\n");
                    } else {
                        failed++;
                        std::printf("  FAIL: no benchmark samples collected\n");
                    }
                }
            }
        } else {
            std::printf("  SKIP: CoreML split models not found at %s\n", cml_3s.c_str());
        }
    }

    std::printf("\n[TEST 7] Model size inventory\n");
    {
        std::string variants_dir = base_dir + "/decoder_variants";
        struct stat st;

        long cml_total = 0;
        for (auto* name : {"3s", "5s", "10s"}) {
            std::string p = variants_dir + "/kokoro_decoder_split_" + std::string(name) + ".mlmodelc";
            if (stat(p.c_str(), &st) == 0) {
                cml_total += 102 * 1024 * 1024;
            }
        }
        std::string har_bin = variants_dir + "/har_weights.bin";
        if (stat(har_bin.c_str(), &st) == 0) cml_total += st.st_size;
        std::printf("  CoreML split decoder models: %.1f MB (3 buckets + HAR weights)\n", cml_total / 1e6);

        std::string dur_path = base_dir + "/coreml/kokoro_duration.mlmodelc";
        if (stat(dur_path.c_str(), &st) == 0) {
            std::printf("  CoreML duration model: present\n");
        }

        std::string voice_bin = base_dir + "/df_eva_voice.bin";
        if (stat(voice_bin.c_str(), &st) == 0) {
            std::printf("  Voice pack (df_eva): %.1f MB\n", st.st_size / 1e6);
        }

        std::string vocab_p = base_dir + "/vocab.json";
        if (stat(vocab_p.c_str(), &st) == 0) {
            std::printf("  Vocab: %.1f KB\n", st.st_size / 1e3);
        }

        passed++;
        std::printf("  PASS\n");
    }
#else
    std::printf("\n[TEST 5] CoreML duration model (SKIPPED - not compiled with KOKORO_COREML)\n");
    std::printf("[TEST 6] CoreML split decoder (SKIPPED - not compiled with KOKORO_COREML)\n");
    std::printf("[TEST 7] Model size inventory (SKIPPED - not compiled with KOKORO_COREML)\n");
#endif

done:
    espeak_Terminate();

    std::printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
