#pragma once
#include <vector>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <random>
#include <string>
#include <sys/stat.h>
#ifdef __APPLE__
#include <Accelerate/Accelerate.h>
#endif

static constexpr int HAR_HARMONICS = 9;
static constexpr int HAR_SAMPLE_RATE = 24000;
static constexpr int HAR_UPSAMPLE_FACTOR = 300;
static constexpr float HAR_SINE_AMP = 0.1f;
static constexpr float HAR_VOICED_NOISE = 0.003f;
static constexpr float HAR_UV_THRESHOLD = 10.0f;
static constexpr int HAR_STFT_NFFT = 20;
static constexpr int HAR_STFT_HOP = 5;
static constexpr int HAR_STFT_PAD = 10;
static constexpr int HAR_STFT_BINS = 11;

struct HarWeights {
    float l_linear_weight[HAR_HARMONICS];
    float l_linear_bias;
    float stft_real[HAR_STFT_BINS * HAR_STFT_NFFT];
    float stft_imag[HAR_STFT_BINS * HAR_STFT_NFFT];
};

class HarSource {
public:
    bool load(const std::string& weights_path) {
        struct stat st;
        if (stat(weights_path.c_str(), &st) != 0) {
            std::fprintf(stderr, "HAR weights not found: %s\n", weights_path.c_str());
            return false;
        }

        std::ifstream f(weights_path, std::ios::binary);
        if (!f.is_open()) return false;

        char magic[4];
        f.read(magic, 4);
        if (std::memcmp(magic, "HAR1", 4) != 0) {
            std::fprintf(stderr, "HAR: invalid magic in %s\n", weights_path.c_str());
            return false;
        }

        f.read(reinterpret_cast<char*>(w_.l_linear_weight), HAR_HARMONICS * sizeof(float));
        f.read(reinterpret_cast<char*>(&w_.l_linear_bias), sizeof(float));
        f.read(reinterpret_cast<char*>(w_.stft_real), HAR_STFT_BINS * HAR_STFT_NFFT * sizeof(float));
        f.read(reinterpret_cast<char*>(w_.stft_imag), HAR_STFT_BINS * HAR_STFT_NFFT * sizeof(float));

        if (!f.good()) {
            std::fprintf(stderr, "HAR: truncated weights file %s\n", weights_path.c_str());
            return false;
        }

        loaded_ = true;
        return true;
    }

    std::vector<float> compute(const float* f0, int f0_frames) const {
        if (!loaded_) return {};

        thread_local std::mt19937 tl_rng{std::random_device{}()};

        int audio_frames = f0_frames * HAR_UPSAMPLE_FACTOR;
        int padded_len = audio_frames + 2 * HAR_STFT_PAD;
        int stft_frames = (padded_len - HAR_STFT_NFFT) / HAR_STFT_HOP + 1;

        std::vector<float> rad_values(f0_frames * HAR_HARMONICS);
        for (int t = 0; t < f0_frames; t++) {
            for (int h = 0; h < HAR_HARMONICS; h++) {
                float freq = f0[t] * (h + 1);
                rad_values[t * HAR_HARMONICS + h] = std::fmod(freq / HAR_SAMPLE_RATE, 1.0f);
            }
        }

        std::uniform_real_distribution<float> uniform(0.0f, 1.0f);
        for (int h = 0; h < HAR_HARMONICS; h++) {
            float rand_phase = (h == 0) ? 0.0f : uniform(tl_rng);
            rad_values[0 * HAR_HARMONICS + h] += rand_phase;
        }

        std::vector<float> phase_cumsum(f0_frames * HAR_HARMONICS);
        for (int h = 0; h < HAR_HARMONICS; h++) {
            float cum = 0.0f;
            for (int t = 0; t < f0_frames; t++) {
                cum += rad_values[t * HAR_HARMONICS + h];
                phase_cumsum[t * HAR_HARMONICS + h] = cum * 2.0f * static_cast<float>(M_PI) * HAR_UPSAMPLE_FACTOR;
            }
        }

        std::vector<float> phase_upsampled(audio_frames * HAR_HARMONICS);
        for (int h = 0; h < HAR_HARMONICS; h++) {
            for (int t = 0; t < audio_frames; t++) {
                float src_pos = static_cast<float>(t) / HAR_UPSAMPLE_FACTOR;
                int idx0 = static_cast<int>(src_pos);
                float frac = src_pos - idx0;
                int idx1 = std::min(idx0 + 1, f0_frames - 1);
                idx0 = std::min(idx0, f0_frames - 1);
                float v0 = phase_cumsum[idx0 * HAR_HARMONICS + h];
                float v1 = phase_cumsum[idx1 * HAR_HARMONICS + h];
                phase_upsampled[t * HAR_HARMONICS + h] = v0 + frac * (v1 - v0);
            }
        }

        int total_phases = audio_frames * HAR_HARMONICS;
        std::vector<float> sine_waves(total_phases);
#ifdef __APPLE__
        vvsinf(sine_waves.data(), phase_upsampled.data(), &total_phases);
        float amp = HAR_SINE_AMP;
        vDSP_vsmul(sine_waves.data(), 1, &amp, sine_waves.data(), 1, static_cast<vDSP_Length>(total_phases));
#else
        for (int i = 0; i < total_phases; i++) {
            sine_waves[i] = std::sin(phase_upsampled[i]) * HAR_SINE_AMP;
        }
#endif

        std::vector<float> f0_up(audio_frames);
        for (int t = 0; t < audio_frames; t++) {
            f0_up[t] = f0[t / HAR_UPSAMPLE_FACTOR];
        }

        std::normal_distribution<float> normal(0.0f, 1.0f);
        std::vector<float> source(audio_frames);
        for (int t = 0; t < audio_frames; t++) {
            float uv = (f0_up[t] > HAR_UV_THRESHOLD) ? 1.0f : 0.0f;
            float noise_amp = uv * HAR_VOICED_NOISE + (1.0f - uv) * HAR_SINE_AMP / 3.0f;

            float val = 0.0f;
            for (int h = 0; h < HAR_HARMONICS; h++) {
                float s = sine_waves[t * HAR_HARMONICS + h] * uv + noise_amp * normal(tl_rng);
                val += s * w_.l_linear_weight[h];
            }
            source[t] = std::tanh(val + w_.l_linear_bias);
        }

        std::vector<float> padded(padded_len);
        for (int i = 0; i < HAR_STFT_PAD; i++) padded[i] = source[0];
        std::memcpy(padded.data() + HAR_STFT_PAD, source.data(), audio_frames * sizeof(float));
        for (int i = 0; i < HAR_STFT_PAD; i++) padded[audio_frames + HAR_STFT_PAD + i] = source[audio_frames - 1];

        std::vector<float> output(HAR_STFT_BINS * 2 * stft_frames);
        float* spec_out = output.data();
        float* phase_out = output.data() + HAR_STFT_BINS * stft_frames;

#ifdef __APPLE__
        for (int f = 0; f < stft_frames; f++) {
            int offset = f * HAR_STFT_HOP;
            for (int b = 0; b < HAR_STFT_BINS; b++) {
                float re = 0.0f, im = 0.0f;
                vDSP_dotpr(padded.data() + offset, 1,
                           w_.stft_real + b * HAR_STFT_NFFT, 1, &re, HAR_STFT_NFFT);
                vDSP_dotpr(padded.data() + offset, 1,
                           w_.stft_imag + b * HAR_STFT_NFFT, 1, &im, HAR_STFT_NFFT);
                float mag = std::sqrt(re * re + im * im + 1e-14f);
                float ph = std::atan2(im, re);
                if (im == 0.0f && re < 0.0f) ph = static_cast<float>(M_PI);
                spec_out[b * stft_frames + f] = mag;
                phase_out[b * stft_frames + f] = ph;
            }
        }
#else
        for (int f = 0; f < stft_frames; f++) {
            int offset = f * HAR_STFT_HOP;
            for (int b = 0; b < HAR_STFT_BINS; b++) {
                float re = 0.0f, im = 0.0f;
                for (int k = 0; k < HAR_STFT_NFFT; k++) {
                    float x = padded[offset + k];
                    re += x * w_.stft_real[b * HAR_STFT_NFFT + k];
                    im += x * w_.stft_imag[b * HAR_STFT_NFFT + k];
                }
                float mag = std::sqrt(re * re + im * im + 1e-14f);
                float ph = std::atan2(im, re);
                if (im == 0.0f && re < 0.0f) ph = static_cast<float>(M_PI);
                spec_out[b * stft_frames + f] = mag;
                phase_out[b * stft_frames + f] = ph;
            }
        }
#endif

        return output;
    }

    bool is_loaded() const { return loaded_; }

private:
    HarWeights w_{};
    bool loaded_ = false;
};
