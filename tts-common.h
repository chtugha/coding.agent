#pragma once

#include <vector>
#include <string>
#include <cmath>
#include <cstdlib>
#include <sys/stat.h>

#ifndef ESPEAK_NG_DATA_DIR
#define ESPEAK_NG_DATA_DIR ""
#endif

namespace whispertalk {
namespace tts {

inline float normalize_audio(std::vector<float>& samples, float ceiling = 0.90f) {
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

inline void apply_fade_in(std::vector<float>& samples, int fade_samples = 48) {
    int n = std::min(fade_samples, (int)samples.size());
    for (int i = 0; i < n; i++) {
        samples[i] *= static_cast<float>(i) / static_cast<float>(n);
    }
}

inline std::string resolve_espeak_data_dir() {
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

}
}
