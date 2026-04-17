#pragma once

#include <vector>
#include <string>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <sys/stat.h>

#ifndef ESPEAK_NG_DATA_DIR
#define ESPEAK_NG_DATA_DIR ""
#endif

namespace whispertalk {
namespace tts {

// Shared TTS audio format — the single source of truth for every
// engine and the TTS dock. The dock's HELLO contract requires these
// exact values (see `tts-service.cpp`).
constexpr uint32_t kTTSSampleRate     = 24000;  // Hz
constexpr uint16_t kTTSChannels       = 1;      // mono
// Max samples per streamed chunk (200 ms at 24 kHz). Engines MUST NOT
// emit chunks larger than this; OAP sizes its per-frame buffers from
// this constant.
constexpr size_t   kTTSMaxFrameSamples = 4800;

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
