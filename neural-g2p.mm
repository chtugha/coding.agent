#ifdef __APPLE__

#include "neural-g2p.h"
#include <cctype>
#include <cfloat>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#import <Foundation/Foundation.h>

static constexpr int kMaxCharLen = 128;

static bool parse_char_vocab(const std::string& path,
                              std::unordered_map<std::string, int>& vocab) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(f)), {});

    size_t pos = 0;
    while (pos < content.size()) {
        size_t q1 = content.find('"', pos);
        if (q1 == std::string::npos) break;
        size_t q2 = q1 + 1;
        while (q2 < content.size()) {
            if (content[q2] == '\\') { q2 += 2; continue; }
            if (content[q2] == '"') break;
            q2++;
        }
        if (q2 >= content.size()) break;
        std::string key = content.substr(q1 + 1, q2 - q1 - 1);
        pos = q2 + 1;

        size_t colon = content.find(':', pos);
        if (colon == std::string::npos) break;
        size_t num_start = colon + 1;
        while (num_start < content.size() && std::isspace((unsigned char)content[num_start]))
            num_start++;
        if (num_start >= content.size()) break;
        if (content[num_start] == '"') {
            pos = num_start + 1;
            continue;
        }
        char* end_ptr = nullptr;
        long val = std::strtol(content.c_str() + num_start, &end_ptr, 10);
        if (end_ptr == content.c_str() + num_start) {
            pos = num_start + 1;
            continue;
        }
        vocab[key] = static_cast<int>(val);
        pos = static_cast<size_t>(end_ptr - content.c_str());
    }
    return !vocab.empty();
}

static bool parse_phoneme_vocab(const std::string& path,
                                 std::vector<std::string>& vocab) {
    std::ifstream f(path);
    if (!f.is_open()) return false;
    std::string content((std::istreambuf_iterator<char>(f)), {});

    size_t pos = content.find('[');
    if (pos == std::string::npos) return false;
    pos++;

    while (pos < content.size()) {
        size_t q1 = content.find('"', pos);
        if (q1 == std::string::npos) break;
        size_t q2 = q1 + 1;
        while (q2 < content.size()) {
            if (content[q2] == '\\') { q2 += 2; continue; }
            if (content[q2] == '"') break;
            q2++;
        }
        if (q2 >= content.size()) break;
        vocab.push_back(content.substr(q1 + 1, q2 - q1 - 1));
        pos = q2 + 1;

        size_t next = pos;
        while (next < content.size() &&
               content[next] != ',' && content[next] != ']' && content[next] != '"')
            next++;
        if (next < content.size() && content[next] == ']') break;
        pos = next;
    }
    return !vocab.empty();
}

NeuralG2P::~NeuralG2P() {
    if (model_) {
        [model_ release];
        model_ = nil;
    }
}

bool NeuralG2P::load(const std::string& mlmodelc_path) {
    size_t slash = mlmodelc_path.rfind('/');
    std::string g2p_dir = (slash != std::string::npos)
        ? mlmodelc_path.substr(0, slash)
        : ".";

    std::string char_vocab_path = g2p_dir + "/char_vocab.json";
    std::string phoneme_vocab_path = g2p_dir + "/phoneme_vocab.json";

    if (!parse_char_vocab(char_vocab_path, char_vocab_)) {
        std::fprintf(stderr, "NeuralG2P: failed to load char_vocab from %s\n",
                     char_vocab_path.c_str());
        return false;
    }
    std::printf("NeuralG2P: char_vocab loaded (%zu entries)\n", char_vocab_.size());

    if (!parse_phoneme_vocab(phoneme_vocab_path, phoneme_vocab_)) {
        std::fprintf(stderr, "NeuralG2P: failed to load phoneme_vocab from %s\n",
                     phoneme_vocab_path.c_str());
        return false;
    }
    std::printf("NeuralG2P: phoneme_vocab loaded (%zu entries)\n", phoneme_vocab_.size());

    auto it_pad = char_vocab_.find("<pad>");
    auto it_unk = char_vocab_.find("<unk>");
    pad_idx_ = (it_pad != char_vocab_.end()) ? it_pad->second : 0;
    unk_idx_ = (it_unk != char_vocab_.end()) ? it_unk->second : 1;

    phoneme_pad_idx_ = 0;
    eos_idx_ = -1;
    for (size_t i = 0; i < phoneme_vocab_.size(); i++) {
        if (phoneme_vocab_[i] == "<pad>") phoneme_pad_idx_ = static_cast<int>(i);
        if (phoneme_vocab_[i] == "<eos>") eos_idx_ = static_cast<int>(i);
    }
    if (eos_idx_ < 0) {
        std::fprintf(stderr, "NeuralG2P: WARNING — <eos> not found in phoneme_vocab; "
                     "output decoding may not terminate correctly\n");
    }

    @autoreleasepool {
        NSString* path = [NSString stringWithUTF8String:mlmodelc_path.c_str()];
        NSURL* url = [NSURL fileURLWithPath:path];

        MLModelConfiguration* config = [[[MLModelConfiguration alloc] init] autorelease];
        config.computeUnits = MLComputeUnitsAll;

        NSError* error = nil;
        model_ = [MLModel modelWithContentsOfURL:url configuration:config error:&error];
        if (error || !model_) {
            std::fprintf(stderr, "NeuralG2P: failed to load CoreML model from %s: %s\n",
                         mlmodelc_path.c_str(),
                         error ? [[error description] UTF8String] : "unknown");
            return false;
        }
        [model_ retain];

        MLModelDescription* desc = model_.modelDescription;
        needs_lengths_ = ([desc.inputDescriptionsByName objectForKey:@"lengths"] != nil);

        std::printf("NeuralG2P: CoreML model loaded (needs_lengths=%s)\n",
                    needs_lengths_ ? "yes" : "no");
    }

    available_ = true;
    return true;
}

std::string NeuralG2P::phonemize_word(const std::string& word) {
    if (word.empty() || !available_ || !model_) return "";

    std::vector<int32_t> ids;
    ids.reserve(kMaxCharLen);

    for (size_t i = 0; i < word.size() && (int)ids.size() < kMaxCharLen; ) {
        unsigned char c = static_cast<unsigned char>(word[i]);
        int char_len = 1;
        if ((c & 0x80) == 0)       char_len = 1;
        else if ((c & 0xE0) == 0xC0) char_len = 2;
        else if ((c & 0xF0) == 0xE0) char_len = 3;
        else if ((c & 0xF8) == 0xF0) char_len = 4;

        if (i + (size_t)char_len > word.size()) break;
        std::string ch = word.substr(i, char_len);
        auto it = char_vocab_.find(ch);
        ids.push_back((it != char_vocab_.end()) ? it->second : unk_idx_);
        i += char_len;
    }

    int actual_len = static_cast<int>(ids.size());
    while ((int)ids.size() < kMaxCharLen) ids.push_back(pad_idx_);

    std::string result;

    @autoreleasepool {
        NSError* error = nil;

        MLMultiArray* x_arr = [[[MLMultiArray alloc]
            initWithShape:@[@1, @(kMaxCharLen)]
            dataType:MLMultiArrayDataTypeInt32
            error:&error] autorelease];
        if (error || !x_arr) return "";
        int32_t* x_ptr = (int32_t*)x_arr.dataPointer;
        for (int i = 0; i < kMaxCharLen; i++) x_ptr[i] = ids[i];

        NSDictionary* input_dict;
        if (needs_lengths_) {
            MLMultiArray* len_arr = [[[MLMultiArray alloc]
                initWithShape:@[@1]
                dataType:MLMultiArrayDataTypeInt32
                error:&error] autorelease];
            if (error || !len_arr) return "";
            ((int32_t*)len_arr.dataPointer)[0] = actual_len;
            input_dict = @{@"x": x_arr, @"lengths": len_arr};
        } else {
            input_dict = @{@"x": x_arr};
        }

        MLDictionaryFeatureProvider* features = [[[MLDictionaryFeatureProvider alloc]
            initWithDictionary:input_dict error:&error] autorelease];
        if (error || !features) return "";

        id<MLFeatureProvider> out;
        {
            std::lock_guard<std::mutex> lock(predict_mutex_);
            out = [model_ predictionFromFeatures:features error:&error];
        }
        if (error || !out) {
            std::fprintf(stderr, "NeuralG2P: prediction failed: %s\n",
                         error ? [[error description] UTF8String] : "unknown");
            return "";
        }

        MLMultiArray* logits_arr = [out featureValueForName:@"logits"].multiArrayValue;
        if (!logits_arr || logits_arr.shape.count < 3) {
            std::fprintf(stderr, "NeuralG2P: unexpected output shape (count=%zu) for word '%s'\n",
                         logits_arr ? (size_t)logits_arr.shape.count : 0, word.c_str());
            return "";
        }
        if (logits_arr.dataType != MLMultiArrayDataTypeFloat32) {
            std::fprintf(stderr, "NeuralG2P: unexpected logits dtype %ld for word '%s'\n",
                         (long)logits_arr.dataType, word.c_str());
            return "";
        }

        NSInteger seq_len = [logits_arr.shape[1] integerValue];
        NSInteger num_ph  = [logits_arr.shape[2] integerValue];
        float* logits_ptr = (float*)logits_arr.dataPointer;

        for (NSInteger i = 0; i < seq_len; i++) {
            float* row = logits_ptr + i * num_ph;
            int best_idx = 0;
            float best_val = -FLT_MAX;
            for (NSInteger j = 0; j < num_ph; j++) {
                if (row[j] > best_val) {
                    best_val = row[j];
                    best_idx = (int)j;
                }
            }
            if (best_idx == phoneme_pad_idx_ || best_idx == eos_idx_) break;
            if (best_idx >= 0 && best_idx < (int)phoneme_vocab_.size()) {
                const std::string& ph = phoneme_vocab_[best_idx];
                if (ph.empty() || ph == "<pad>" || ph == "<sos>" || ph == "<eos>") break;
                result += ph;
            }
        }
    }

    return result;
}

std::string NeuralG2P::phonemize(const std::string& text) {
    if (!available_ || !model_) return "";

    std::string result;
    std::string current_word;

    auto is_punct = [](unsigned char c) -> bool {
        return c == '.' || c == ',' || c == ';' || c == ':' ||
               c == '!' || c == '?' || c == '"' || c == '\'' ||
               c == '(' || c == ')' || c == '[' || c == ']' ||
               c == '{' || c == '}' || c == '-';
    };

    auto strip_punct = [&](const std::string& w) -> std::string {
        size_t start = 0;
        while (start < w.size() && is_punct(static_cast<unsigned char>(w[start])))
            start++;
        size_t end = w.size();
        while (end > start && is_punct(static_cast<unsigned char>(w[end - 1])))
            end--;
        return (start < end) ? w.substr(start, end - start) : "";
    };

    auto flush_word = [&]() {
        if (current_word.empty()) return;
        std::string cleaned = strip_punct(current_word);
        if (cleaned.empty()) { current_word.clear(); return; }
        std::string ph = phonemize_word(cleaned);
        if (!ph.empty()) {
            if (!result.empty()) result += ' ';
            result += ph;
        }
        current_word.clear();
    };

    for (size_t i = 0; i < text.size(); ) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        int char_len = 1;
        if ((c & 0x80) == 0)       char_len = 1;
        else if ((c & 0xE0) == 0xC0) char_len = 2;
        else if ((c & 0xF0) == 0xE0) char_len = 3;
        else if ((c & 0xF8) == 0xF0) char_len = 4;

        if (i + (size_t)char_len > text.size()) break;

        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            flush_word();
        } else {
            current_word += text.substr(i, char_len);
        }
        i += char_len;
    }
    flush_word();

    return result;
}

#endif // __APPLE__
