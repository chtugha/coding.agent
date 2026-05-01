#pragma once

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

enum class G2PBackend { AUTO, NEURAL, ESPEAK };

#ifdef __APPLE__
#import <CoreML/CoreML.h>

class NeuralG2P {
public:
    ~NeuralG2P();
    bool load(const std::string& mlmodelc_path);
    std::string phonemize(const std::string& text);
    bool is_available() const { return available_; }

private:
    std::string phonemize_word(const std::string& word);

    MLModel* model_ = nil;
    bool available_ = false;
    bool needs_lengths_ = true;
    int pad_idx_ = 0;
    int unk_idx_ = 1;
    int eos_idx_ = 2;
    int phoneme_pad_idx_ = 0;
    mutable std::mutex predict_mutex_;
    std::unordered_map<std::string, int> char_vocab_;
    std::vector<std::string> phoneme_vocab_;
};

#endif // __APPLE__
