#include <torch/script.h>
#include <torch/torch.h>
#include <espeak-ng/speak_lib.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>
#include <chrono>

#ifndef WHISPERTALK_MODELS_DIR
#define WHISPERTALK_MODELS_DIR "bin/models"
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
    std::string model_path = models_dir + "/kokoro-german/kokoro_german.pt";
    std::string voice_path = models_dir + "/kokoro-german/df_eva_embedding.pt";
    std::string vocab_path = models_dir + "/kokoro-german/vocab.json";
    
    int passed = 0;
    int failed = 0;

    std::printf("=== Kokoro C++ TTS Test Suite ===\n\n");

    std::printf("[TEST 1] espeak-ng initialization and phonemization\n");
    int result = espeak_Initialize(AUDIO_OUTPUT_RETRIEVAL, 0,
                                  "/opt/homebrew/share/espeak-ng-data", 0);
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

    torch::jit::script::Module model;
    torch::Tensor voice_pack;

    std::printf("\n[TEST 4] Model loading\n");
    try {
        model = torch::jit::load(model_path);
        model.eval();
        passed++;
        std::printf("  PASS: Model loaded from %s\n", model_path.c_str());
    } catch (const c10::Error& e) {
        failed++;
        std::printf("  FAIL: %s\n", e.what());
        goto done;
    }

    std::printf("\n[TEST 5] Voice embedding loading\n");
    try {
        std::ifstream f(voice_path, std::ios::binary);
        std::vector<char> data((std::istreambuf_iterator<char>(f)),
                              std::istreambuf_iterator<char>());
        voice_pack = torch::jit::pickle_load(data).toTensor().to(torch::kFloat32);
        std::printf("  Voice pack shape: [%lld, %lld, %lld]\n",
                   voice_pack.size(0), voice_pack.size(1), voice_pack.size(2));
        if (voice_pack.size(0) > 100 && voice_pack.size(2) == 256) {
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

    std::printf("\n[TEST 6] End-to-end synthesis (German)\n");
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
            
            int phoneme_count = static_cast<int>(ids.size()) - 2;
            int voice_idx = std::min(phoneme_count - 1, static_cast<int>(voice_pack.size(0)) - 1);
            voice_idx = std::max(0, voice_idx);
            auto ref_s = voice_pack[voice_idx];
            
            auto input_ids = torch::from_blob(ids.data(),
                                             {1, static_cast<int64_t>(ids.size())},
                                             torch::kLong).clone();
            auto speed = torch::tensor(1.0f);
            
            torch::Tensor audio;
            {
                torch::NoGradGuard no_grad;
                std::vector<torch::jit::IValue> inputs;
                inputs.push_back(input_ids);
                inputs.push_back(ref_s);
                inputs.push_back(speed);
                audio = model.forward(inputs).toTensor();
            }
            
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();
            
            float duration_sec = static_cast<float>(audio.numel()) / 24000.0f;
            
            std::printf("  \"%s\"\n", text.c_str());
            std::printf("    phonemes: %s\n", phonemes.c_str());
            std::printf("    tokens: %zu, audio: %lld samples (%.2fs @ 24kHz)\n",
                       ids.size(), audio.numel(), duration_sec);
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

done:
    espeak_Terminate();
    
    std::printf("\n=== Results: %d passed, %d failed ===\n", passed, failed);
    return failed > 0 ? 1 : 0;
}
