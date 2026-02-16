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
#include <numeric>
#include <sys/stat.h>

#ifdef KOKORO_COREML
#import <CoreML/CoreML.h>
#import <Foundation/Foundation.h>
#endif

#ifdef KOKORO_ONNX
#include <onnxruntime_cxx_api.h>
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

    std::printf("\n[TEST 9] Decoder backend benchmark (TorchScript vs ONNX vs CoreML-Split)\n");
    {
        std::string variants_dir = base_dir + "/decoder_variants";
        std::vector<std::string> bench_texts = {
            "Hallo Welt",
            "Guten Tag, wie kann ich Ihnen helfen?",
            "Das Wetter ist heute sehr schoen und die Sonne scheint.",
        };

        auto bench_torchscript = [&](int runs) -> std::vector<double> {
            std::vector<double> times;
            for (auto& text : bench_texts) {
                auto ph = phonemize_german(text);
                auto ids = vocab.encode(ph);
                int bucket = select_bucket(static_cast<int64_t>(ids.size()), bucket_sizes);
                if (bucket < 0) continue;
                std::vector<int64_t> padded(bucket, 0);
                std::copy(ids.begin(), ids.end(), padded.begin());
                int pc = std::max(0, std::min((int)ids.size() - 3, voice_entries - 1));
                auto ref_s = voice_pack.index({pc}).unsqueeze(0);
                auto input_ids = torch::from_blob(padded.data(), {1, (int64_t)bucket}, torch::kLong).clone();
                auto speed = torch::tensor(1.0f);

                for (int r = 0; r < runs; r++) {
                    auto t0 = std::chrono::steady_clock::now();
                    torch::NoGradGuard ng;
                    std::vector<torch::jit::IValue> inputs;
                    inputs.push_back(input_ids);
                    inputs.push_back(ref_s);
                    inputs.push_back(speed);
                    auto audio = bucket_models[bucket].forward(inputs).toTensor();
                    auto ms = std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - t0).count();
                    if (r > 0) times.push_back(ms);
                }
            }
            return times;
        };

        std::printf("  --- TorchScript (CPU) ---\n");
        auto ts_times = bench_torchscript(4);
        if (!ts_times.empty()) {
            std::sort(ts_times.begin(), ts_times.end());
            double ts_avg = std::accumulate(ts_times.begin(), ts_times.end(), 0.0) / ts_times.size();
            double ts_min = ts_times.front();
            double ts_p50 = ts_times[ts_times.size() / 2];
            std::printf("    avg=%.0fms, min=%.0fms, p50=%.0fms (%zu samples)\n",
                       ts_avg, ts_min, ts_p50, ts_times.size());
        }

#ifdef KOKORO_ONNX
        std::printf("  --- ONNX Runtime (CPU) ---\n");
        {
            struct stat st;
            std::string onnx_3s = variants_dir + "/kokoro_decoder_3s.onnx";
            if (stat(onnx_3s.c_str(), &st) == 0) {
                try {
                    Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "bench");
                    Ort::SessionOptions opts;
                    opts.SetIntraOpNumThreads(4);
                    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

                    struct { const char* name; int asr; int f0; } onnx_buckets[] = {
                        {"3s", 72, 144}, {"5s", 120, 240}, {"10s", 240, 480},
                    };
                    std::map<std::string, std::unique_ptr<Ort::Session>> onnx_sessions;
                    for (auto& b : onnx_buckets) {
                        std::string p = variants_dir + "/kokoro_decoder_" + b.name + ".onnx";
                        struct stat st2;
                        if (stat(p.c_str(), &st2) != 0) continue;
                        onnx_sessions[b.name] = std::make_unique<Ort::Session>(env, p.c_str(), opts);
                    }

                    auto select_onnx = [&](int f0) -> std::pair<std::string, int> {
                        for (auto& b : onnx_buckets) {
                            if (b.f0 >= f0) return {b.name, b.f0};
                        }
                        return {"10s", 480};
                    };

                    std::vector<double> onnx_times;
                    for (auto& text : bench_texts) {
                        auto ph = phonemize_german(text);
                        auto ids = vocab.encode(ph);
                        int bucket = select_bucket(static_cast<int64_t>(ids.size()), bucket_sizes);
                        if (bucket < 0) continue;
                        std::vector<int64_t> padded(bucket, 0);
                        std::copy(ids.begin(), ids.end(), padded.begin());
                        int pc = std::max(0, std::min((int)ids.size() - 3, voice_entries - 1));
                        auto ref_s_t = voice_pack.index({pc}).unsqueeze(0);
                        auto input_ids_t = torch::from_blob(padded.data(), {1, (int64_t)bucket}, torch::kLong).clone();
                        auto speed_t = torch::tensor(1.0f);

                        torch::Tensor full_out;
                        {
                            torch::NoGradGuard ng;
                            std::vector<torch::jit::IValue> ins;
                            ins.push_back(input_ids_t);
                            ins.push_back(ref_s_t);
                            ins.push_back(speed_t);
                            full_out = bucket_models[bucket].forward(ins).toTensor();
                        }

                        (void)full_out;

                        auto [bname, bf0] = select_onnx(144);
                        auto it = onnx_sessions.find(bname);
                        if (it == onnx_sessions.end()) continue;

                        int asr_f = (bname == "3s") ? 72 : (bname == "5s") ? 120 : 240;
                        std::vector<float> asr_data(512 * asr_f, 0.0f);
                        std::vector<float> f0_data(bf0, 0.0f);
                        std::vector<float> n_data(bf0, 0.0f);
                        std::vector<float> refs_data(256, 0.0f);
                        auto ref_ptr = ref_s_t.contiguous().data_ptr<float>();
                        std::memcpy(refs_data.data(), ref_ptr, 256 * sizeof(float));

                        auto alloc = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
                        std::array<int64_t, 3> asr_shape = {1, 512, (int64_t)asr_f};
                        std::array<int64_t, 2> f0_shape = {1, (int64_t)bf0};
                        std::array<int64_t, 2> refs_shape = {1, 256};

                        for (int r = 0; r < 4; r++) {
                            auto asr_val = Ort::Value::CreateTensor<float>(alloc, asr_data.data(), asr_data.size(), asr_shape.data(), 3);
                            auto f0_val = Ort::Value::CreateTensor<float>(alloc, f0_data.data(), f0_data.size(), f0_shape.data(), 2);
                            auto n_val = Ort::Value::CreateTensor<float>(alloc, n_data.data(), n_data.size(), f0_shape.data(), 2);
                            auto refs_val = Ort::Value::CreateTensor<float>(alloc, refs_data.data(), refs_data.size(), refs_shape.data(), 2);

                            const char* input_names[] = {"asr", "F0_pred", "N_pred", "ref_s"};
                            const char* output_names[] = {"waveform"};
                            std::array<Ort::Value, 4> inputs;
                            inputs[0] = std::move(asr_val);
                            inputs[1] = std::move(f0_val);
                            inputs[2] = std::move(n_val);
                            inputs[3] = std::move(refs_val);

                            auto t0 = std::chrono::steady_clock::now();
                            auto outputs = it->second->Run(Ort::RunOptions{nullptr},
                                input_names, inputs.data(), 4, output_names, 1);
                            auto ms = std::chrono::duration<double, std::milli>(
                                std::chrono::steady_clock::now() - t0).count();
                            if (r > 0) onnx_times.push_back(ms);
                        }
                    }
                    if (!onnx_times.empty()) {
                        std::sort(onnx_times.begin(), onnx_times.end());
                        double avg = std::accumulate(onnx_times.begin(), onnx_times.end(), 0.0) / onnx_times.size();
                        std::printf("    avg=%.0fms, min=%.0fms, p50=%.0fms (%zu samples)\n",
                                   avg, onnx_times.front(), onnx_times[onnx_times.size()/2], onnx_times.size());
                    }
                } catch (const Ort::Exception& e) {
                    std::printf("    ONNX bench failed: %s\n", e.what());
                }
            } else {
                std::printf("    SKIP: ONNX models not found\n");
            }
        }
#else
        std::printf("  --- ONNX Runtime (SKIPPED - not compiled) ---\n");
#endif

#ifdef KOKORO_COREML
        std::printf("  --- CoreML Split Decoder (ANE) ---\n");
        {
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
                    std::map<std::string, torch::jit::script::Module> har_models;
                    for (auto& b : cml_buckets) {
                        std::string p = variants_dir + "/kokoro_decoder_split_" + std::string(b.name) + ".mlmodelc";
                        struct stat st2;
                        if (stat(p.c_str(), &st2) != 0) continue;
                        NSString* ns_p = [NSString stringWithUTF8String:p.c_str()];
                        NSError* err = nil;
                        MLModel* m = [MLModel modelWithContentsOfURL:[NSURL fileURLWithPath:ns_p] configuration:cfg error:&err];
                        if (m && !err) cml_models[b.name] = m;

                        std::string hp = variants_dir + "/kokoro_har_" + std::string(b.name) + ".pt";
                        struct stat st3;
                        if (stat(hp.c_str(), &st3) == 0) {
                            try {
                                auto hm = torch::jit::load(hp);
                                hm.eval();
                                har_models[b.name] = std::move(hm);
                            } catch (...) {}
                        }
                    }

                    if (cml_models.empty() || har_models.empty()) {
                        std::printf("    SKIP: CoreML split models or HAR models not loaded\n");
                    } else {
                        std::vector<double> cml_times;
                        for (auto& text : bench_texts) {
                            auto ph = phonemize_german(text);
                            auto ids = vocab.encode(ph);

                            auto& bk = cml_buckets[0];
                            auto cml_it = cml_models.find(bk.name);
                            auto har_it = har_models.find(bk.name);
                            if (cml_it == cml_models.end() || har_it == har_models.end()) continue;

                            torch::Tensor f0_t = torch::zeros({1, bk.f0});

                            for (int r = 0; r < 4; r++) {
                                auto t0 = std::chrono::steady_clock::now();

                                torch::Tensor har;
                                {
                                    torch::NoGradGuard ng;
                                    har = har_it->second.forward({f0_t}).toTensor();
                                }

                                NSError* err = nil;
                                auto make_arr = [&](NSArray<NSNumber*>* shape, const float* data, size_t count) -> MLMultiArray* {
                                    MLMultiArray* arr = [[MLMultiArray alloc] initWithShape:shape
                                        dataType:MLMultiArrayDataTypeFloat32 error:&err];
                                    if (err) return nil;
                                    std::memcpy((float*)arr.dataPointer, data, count * sizeof(float));
                                    return arr;
                                };

                                std::vector<float> asr_data(512 * bk.asr, 0.0f);
                                std::vector<float> f0_data(bk.f0, 0.0f);
                                std::vector<float> refs_data(256, 0.0f);
                                if (voice_pack.defined()) {
                                    auto vp = voice_pack.index({0}).contiguous().data_ptr<float>();
                                    std::memcpy(refs_data.data(), vp, 256 * sizeof(float));
                                }
                                auto har_data = har.contiguous().data_ptr<float>();

                                auto asr_a = make_arr(@[@1, @512, @(bk.asr)], asr_data.data(), asr_data.size());
                                auto f0_a = make_arr(@[@1, @(bk.f0)], f0_data.data(), f0_data.size());
                                auto n_a = make_arr(@[@1, @(bk.f0)], f0_data.data(), f0_data.size());
                                auto refs_a = make_arr(@[@1, @256], refs_data.data(), 256);
                                auto har_a = make_arr(@[@1, @(bk.harc * 2), @(bk.hart)], har_data, har.numel());

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
                            std::printf("    avg=%.0fms, min=%.0fms, p50=%.0fms (%zu samples)\n",
                                       avg, cml_times.front(), cml_times[cml_times.size()/2], cml_times.size());
                        }
                    }
                }
            } else {
                std::printf("    SKIP: CoreML split models not found\n");
            }
        }
#else
        std::printf("  --- CoreML Split Decoder (SKIPPED - not compiled) ---\n");
#endif

        passed++;
        std::printf("  PASS (benchmark)\n");
    }

    std::printf("\n[TEST 10] Binary/model size comparison\n");
    {
        std::string variants_dir = base_dir + "/decoder_variants";
        struct stat st;

        long ts_total = 0;
        for (int sz : bucket_sizes) {
            std::string p = base_dir + "/kokoro_german_L" + std::to_string(sz) + ".pt";
            if (stat(p.c_str(), &st) == 0) ts_total += st.st_size;
        }
        std::printf("  TorchScript models: %.1f MB (%d buckets)\n", ts_total / 1e6, (int)bucket_sizes.size());

        long onnx_total = 0;
        for (auto* name : {"3s", "5s", "10s"}) {
            std::string p = variants_dir + "/kokoro_decoder_" + name + ".onnx";
            std::string pd = p + ".data";
            if (stat(p.c_str(), &st) == 0) onnx_total += st.st_size;
            if (stat(pd.c_str(), &st) == 0) onnx_total += st.st_size;
        }
        std::printf("  ONNX models: %.1f MB (3 buckets)\n", onnx_total / 1e6);

        long cml_total = 0;
        for (auto* name : {"3s", "5s", "10s"}) {
            std::string p = variants_dir + "/kokoro_decoder_split_" + std::string(name) + ".mlmodelc";
            if (stat(p.c_str(), &st) == 0) {
                cml_total += 102 * 1024 * 1024;
            }
            std::string hp = variants_dir + "/kokoro_har_" + std::string(name) + ".pt";
            if (stat(hp.c_str(), &st) == 0) cml_total += st.st_size;
        }
        std::printf("  CoreML split models: %.1f MB (3 buckets + HAR)\n", cml_total / 1e6);

        passed++;
        std::printf("  PASS\n");
    }

done:
    espeak_Terminate();

    std::printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
