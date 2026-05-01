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

// Diagnostic cmd ports exposed by each TTS engine process. These are
// *not* the dock's cmd port (13142) — they are the engines' own
// per-process diagnostic / quality-test endpoints. Defined here so the
// frontend and tests share a single source of truth with the engines.
constexpr uint16_t kKokoroEngineCmdPort = 13144;
constexpr uint16_t kNeuTTSEngineCmdPort = 13174;
constexpr uint16_t kVITS2EngineCmdPort  = 13175;
constexpr uint16_t kMatchaEngineCmdPort = 13176;

// Audio payload header emitted by every TTS engine and parsed by OAP.
// Wire layout:
//   [0..4)   int32   sample_rate (Hz) — INTENTIONALLY HOST BYTE ORDER.
//                    Engines always emit kTTSSampleRate (24000); OAP does
//                    not read this field on the hot path (it uses the
//                    global constant) and therefore tolerates either
//                    endianness. The field is reserved for future format
//                    negotiation. New engine implementations MUST emit
//                    host byte order here for parity with existing
//                    engines until/unless the negotiation protocol is
//                    introduced; do NOT byte-swap. The deliberate
//                    deviation from the t_engine_out_us big-endian
//                    convention below is documented here so future
//                    implementors do not "fix" it.
//   [4..12)  uint64  t_engine_out_us — BIG-ENDIAN. Steady-clock microseconds at
//                    the moment the engine hands the chunk to its
//                    EngineClient::send_audio(). Used by OAP to measure
//                    engine→OAP hop latency for the histogram dumped to
//                    test-results/tts_latency_<ts>.json.
//   [12..)   float32 PCM samples (native host endianness, count =
//                    (payload_size - 12) / sizeof(float))
constexpr size_t   kTTSAudioHeaderBytes = sizeof(int32_t) + sizeof(uint64_t);

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
