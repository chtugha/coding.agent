#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <queue>
#include <memory>
#include <optional>
#include <cstdint>

// Third-party includes
#include <nlohmann/json.hpp>
#include <onnxruntime/onnxruntime_cxx_api.h>

// Unicode normalization and ranges (simplified for this implementation)
namespace una {
    namespace norm {
        inline std::string to_nfd_utf8(const std::string& str) { return str; }
    }
    namespace ranges {
        struct utf8_view {
            std::string str;
            utf8_view(const std::string& s) : str(s) {}
            auto begin() const { return str.begin(); }
            auto end() const { return str.end(); }
        };
    }
}

// Type definitions
using Phoneme = char32_t;
using PhonemeId = int64_t;
using SpeakerId = int64_t;

// Constants
constexpr Phoneme PHONEME_BOS = U'^';
constexpr Phoneme PHONEME_EOS = U'$';
constexpr Phoneme PHONEME_PAD = U'_';
constexpr Phoneme PHONEME_SEPARATOR = U'|';

constexpr PhonemeId ID_BOS = 1;
constexpr PhonemeId ID_EOS = 2;
constexpr PhonemeId ID_PAD = 0;

constexpr float DEFAULT_LENGTH_SCALE = 1.0f;
constexpr float DEFAULT_NOISE_SCALE = 0.667f;
constexpr float DEFAULT_NOISE_W_SCALE = 0.8f;

// eSpeak-ng constants (from speak_lib.h)
constexpr int CLAUSE_PERIOD = 0x00000001;
constexpr int CLAUSE_QUESTION = 0x00000002;
constexpr int CLAUSE_EXCLAMATION = 0x00000004;
constexpr int CLAUSE_COMMA = 0x00000008;
constexpr int CLAUSE_COLON = 0x00000010;
constexpr int CLAUSE_SEMICOLON = 0x00000020;
constexpr int CLAUSE_TYPE_SENTENCE = 0x00000100;

// Global ONNX Runtime environment
static Ort::Env ort_env(ORT_LOGGING_LEVEL_WARNING, "piper");

// Piper synthesizer implementation
struct piper_synthesizer {
    // Model configuration
    std::string espeak_voice = "en-us";
    int sample_rate = 22050;
    int hop_length = 256;
    SpeakerId num_speakers = 1;
    
    // Phoneme mapping
    std::unordered_map<Phoneme, std::vector<PhonemeId>> phoneme_id_map;
    
    // Synthesis parameters
    float synth_length_scale = DEFAULT_LENGTH_SCALE;
    float synth_noise_scale = DEFAULT_NOISE_SCALE;
    float synth_noise_w_scale = DEFAULT_NOISE_W_SCALE;
    
    // Current synthesis state
    float length_scale = DEFAULT_LENGTH_SCALE;
    float noise_scale = DEFAULT_NOISE_SCALE;
    float noise_w_scale = DEFAULT_NOISE_W_SCALE;
    SpeakerId speaker_id = 0;
    
    // ONNX Runtime
    Ort::SessionOptions session_options;
    std::unique_ptr<Ort::Session> session;
    
    // Synthesis queue and buffers
    std::queue<std::pair<std::vector<Phoneme>, std::vector<PhonemeId>>> phoneme_id_queue;
    std::vector<float> chunk_samples;
    std::vector<Phoneme> chunk_phonemes;
    std::vector<int> chunk_phoneme_ids;
    std::vector<int> chunk_alignments;
};

// Utility functions
inline std::optional<Phoneme> get_codepoint(const std::string& phoneme_str) {
    if (phoneme_str.empty()) return std::nullopt;
    
    // Simple UTF-8 to UTF-32 conversion for single characters
    // This is a simplified implementation - in practice you'd use proper Unicode libraries
    if (phoneme_str.size() == 1) {
        return static_cast<Phoneme>(phoneme_str[0]);
    }
    
    // For multi-byte UTF-8, return first byte as approximation
    return static_cast<Phoneme>(phoneme_str[0]);
}
