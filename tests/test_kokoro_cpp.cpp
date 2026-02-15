#include <torch/torch.h>
#include <torch/script.h>
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

static int select_bucket(int64_t input_len, const std::vector<int>& bucket_sizes) {
    for (int sz : bucket_sizes) {
        if (sz >= input_len) return sz;
    }
    return -1;
}

int main() {
    std::string models_dir = WHISPERTALK_MODELS_DIR;
    std::string base_dir = models_dir + "/kokoro-german";
    std::string vocab_path = base_dir + "/vocab.json";

    int passed = 0;
    int failed = 0;

    std::printf("=== Kokoro C++ TTS Test Suite (Bucketed Models) ===\n\n");

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

    std::map<int, torch::jit::script::Module> bucket_models;
    std::vector<int> bucket_sizes;
    torch::Tensor voice_pack;
    int voice_entries = 0;

    std::printf("\n[TEST 4] Bucket model loading\n");
    {
        const int candidates[] = {8, 16, 32, 64, 128, 256, 512};
        for (int sz : candidates) {
            std::string path = base_dir + "/kokoro_german_L" + std::to_string(sz) + ".pt";
            struct stat st;
            if (stat(path.c_str(), &st) != 0) continue;
            try {
                auto model = torch::jit::load(path);
                model.eval();
                bucket_models[sz] = std::move(model);
                bucket_sizes.push_back(sz);
                std::printf("  Loaded L=%d\n", sz);
            } catch (const c10::Error& e) {
                std::printf("  FAIL loading L=%d: %s\n", sz, e.what());
            }
        }
        std::sort(bucket_sizes.begin(), bucket_sizes.end());
        if (bucket_sizes.size() >= 3) {
            passed++;
            std::printf("  PASS: %zu bucket models loaded\n", bucket_sizes.size());
        } else {
            std::string single = base_dir + "/kokoro_german.pt";
            struct stat st;
            if (stat(single.c_str(), &st) == 0) {
                try {
                    auto model = torch::jit::load(single);
                    model.eval();
                    bucket_models[0] = std::move(model);
                    bucket_sizes.push_back(0);
                    passed++;
                    std::printf("  PASS: single model loaded (no buckets)\n");
                } catch (const c10::Error& e) {
                    failed++;
                    std::printf("  FAIL: %s\n", e.what());
                    goto done;
                }
            } else {
                failed++;
                std::printf("  FAIL: no models found\n");
                goto done;
            }
        }
    }

    std::printf("\n[TEST 5] Voice pack loading (bin format)\n");
    {
        std::string bin_path = base_dir + "/df_eva_voice.bin";
        std::string pt_path = base_dir + "/df_eva_embedding.pt";
        struct stat st;
        if (stat(bin_path.c_str(), &st) == 0) {
            std::ifstream f(bin_path, std::ios::binary);
            size_t file_size = static_cast<size_t>(st.st_size);
            size_t num_floats = file_size / sizeof(float);
            voice_entries = static_cast<int>(num_floats / 256);
            std::vector<float> raw(num_floats);
            f.read(reinterpret_cast<char*>(raw.data()), file_size);
            voice_pack = torch::from_blob(raw.data(),
                                          {voice_entries, 256}, torch::kFloat32).clone();
            std::printf("  Loaded from bin: [%d, 256]\n", voice_entries);
            if (voice_entries > 100) {
                passed++;
                std::printf("  PASS\n");
            } else {
                failed++;
                std::printf("  FAIL: too few voice entries\n");
            }
        } else if (stat(pt_path.c_str(), &st) == 0) {
            try {
                std::ifstream f(pt_path, std::ios::binary);
                std::vector<char> data((std::istreambuf_iterator<char>(f)),
                                      std::istreambuf_iterator<char>());
                auto loaded = torch::pickle_load(data).toTensor().to(torch::kFloat32);
                voice_pack = loaded.squeeze(1);
                voice_entries = static_cast<int>(voice_pack.size(0));
                std::printf("  Loaded from pt (fallback): [%d, %lld]\n",
                           voice_entries, voice_pack.size(1));
                if (voice_entries > 100) {
                    passed++;
                    std::printf("  PASS\n");
                } else {
                    failed++;
                    std::printf("  FAIL: unexpected shape\n");
                }
            } catch (const c10::Error& e) {
                failed++;
                std::printf("  FAIL: %s\n", e.what());
                goto done;
            }
        } else {
            failed++;
            std::printf("  FAIL: no voice file found\n");
            goto done;
        }
    }

    std::printf("\n[TEST 6] End-to-end synthesis with bucket selection (German)\n");
    {
        std::vector<std::string> test_texts = {
            "Hallo Welt",
            "Guten Tag, wie kann ich Ihnen helfen?",
            "Das Wetter ist heute sehr schoen.",
        };
        bool all_ok = true;
        for (auto& text : test_texts) {
            auto start = std::chrono::steady_clock::now();

            auto phonemes = phonemize_german(text);
            auto ids = vocab.encode(phonemes);

            int64_t input_len = static_cast<int64_t>(ids.size());
            int bucket = select_bucket(input_len, bucket_sizes);
            if (bucket < 0) {
                std::printf("  FAIL: no bucket for %lld tokens\n", input_len);
                all_ok = false;
                continue;
            }

            std::vector<int64_t> padded_ids(bucket, 0);
            std::copy(ids.begin(), ids.end(), padded_ids.begin());

            int phoneme_count = static_cast<int>(ids.size()) - 2;
            int voice_idx = std::min(phoneme_count - 1, voice_entries - 1);
            voice_idx = std::max(0, voice_idx);
            auto ref_s = voice_pack.index({voice_idx}).unsqueeze(0);

            auto input_ids = torch::from_blob(padded_ids.data(),
                                             {1, static_cast<int64_t>(bucket)},
                                             torch::kLong).clone();
            auto speed = torch::tensor(1.0f);

            torch::Tensor audio;
            {
                torch::NoGradGuard no_grad;
                std::vector<torch::jit::IValue> inputs;
                inputs.push_back(input_ids);
                inputs.push_back(ref_s);
                inputs.push_back(speed);
                audio = bucket_models[bucket].forward(inputs).toTensor();
            }

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            float duration_sec = static_cast<float>(audio.numel()) / 24000.0f;

            std::printf("  \"%s\"\n", text.c_str());
            std::printf("    phonemes: %s\n", phonemes.c_str());
            std::printf("    tokens: %zu (padded to bucket %d), audio: %lld samples (%.2fs @ 24kHz)\n",
                       ids.size(), bucket, audio.numel(), duration_sec);
            std::printf("    range: [%.4f, %.4f], latency: %lldms\n",
                       audio.min().item<float>(), audio.max().item<float>(), elapsed);

            if (audio.numel() < 1000 || audio.max().item<float>() < 0.01f) {
                std::printf("    FAIL: audio seems empty\n");
                all_ok = false;
            }
        }
        if (all_ok) { passed++; std::printf("  PASS\n"); }
        else { failed++; }
    }

#ifdef __APPLE__
    std::printf("\n[TEST 8] MPS acceleration probe\n");
    {
        if (!torch::mps::is_available()) {
            std::printf("  MPS hardware not available\n");
        } else {
            setenv("PYTORCH_ENABLE_MPS_FALLBACK", "1", 0);
            std::printf("  MPS available, PYTORCH_ENABLE_MPS_FALLBACK=1 set\n");
            std::printf("  NOTE: TorchScript models have placeholder storage incompatible with MPS\n");
            std::printf("  GPU acceleration uses CoreML (ANE) for duration model instead\n");
        }
        passed++;
        std::printf("  PASS (informational)\n");
    }
#endif

#ifdef KOKORO_COREML
    std::printf("\n[TEST 7] CoreML duration model load and predict\n");
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
                        auto accessor = voice_pack.index({0}).contiguous().data_ptr<float>();
                        std::memcpy(ref_ptr, accessor, 256 * sizeof(float));
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
                    id<MLFeatureProvider> result = [model predictionFromFeatures:features error:&error];
                    auto coreml_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - coreml_start).count();

                    if (error || !result) {
                        std::printf("  FAIL: CoreML prediction failed: %s\n",
                            error ? [[error description] UTF8String] : "unknown");
                        failed++;
                    } else {
                        MLMultiArray* pred_dur = [result featureValueForName:@"pred_dur"].multiArrayValue;
                        MLMultiArray* d_out = [result featureValueForName:@"d"].multiArrayValue;
                        MLMultiArray* t_en_out = [result featureValueForName:@"t_en"].multiArrayValue;
                        MLMultiArray* s_out = [result featureValueForName:@"s"].multiArrayValue;
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
#else
    std::printf("\n[TEST 7] CoreML duration model (SKIPPED - not compiled with KOKORO_COREML)\n");
#endif

done:
    espeak_Terminate();

    std::printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
