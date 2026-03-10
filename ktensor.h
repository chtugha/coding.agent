#pragma once
#include <vector>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <algorithm>

struct KTensor {
    std::vector<float> data;
    int64_t shape[4] = {0, 0, 0, 0};
    int ndim = 0;

    KTensor() = default;

    static KTensor zeros(std::initializer_list<int64_t> dims) {
        KTensor t;
        t.ndim = static_cast<int>(dims.size());
        int i = 0;
        int64_t n = 1;
        for (auto d : dims) { t.shape[i++] = d; n *= d; }
        t.data.assign(n, 0.0f);
        return t;
    }

    static KTensor from_data(const float* src, std::initializer_list<int64_t> dims) {
        KTensor t;
        t.ndim = static_cast<int>(dims.size());
        int i = 0;
        int64_t n = 1;
        for (auto d : dims) { t.shape[i++] = d; n *= d; }
        t.data.assign(src, src + n);
        return t;
    }

    bool defined() const { return !data.empty(); }
    float* ptr() { return data.data(); }
    const float* ptr() const { return data.data(); }
    int64_t size(int dim) const { return shape[dim]; }
    int64_t numel() const { return static_cast<int64_t>(data.size()); }

    float& at(int64_t i) { return data[i]; }
    const float& at(int64_t i) const { return data[i]; }

    float& at2(int64_t r, int64_t c) { return data[r * shape[1] + c]; }
    const float& at2(int64_t r, int64_t c) const { return data[r * shape[1] + c]; }

    float& at3(int64_t d0, int64_t d1, int64_t d2) {
        return data[(d0 * shape[1] + d1) * shape[2] + d2];
    }
    const float& at3(int64_t d0, int64_t d1, int64_t d2) const {
        return data[(d0 * shape[1] + d1) * shape[2] + d2];
    }

    void copy_slice1(const KTensor& src, int64_t count) {
        int64_t n = std::min(count, std::min(shape[1], src.shape[1]));
        std::memcpy(data.data(), src.data.data(), n * sizeof(float));
    }

    void copy_row(int64_t dst_col, const float* src, int64_t dim0, int64_t src_stride) {
        for (int64_t d = 0; d < dim0; d++) {
            data[d * shape[ndim - 1] + dst_col] = src[d * src_stride];
        }
    }

    void copy_col_3d(int64_t dst_col, const float* col_data, int64_t dim1) {
        for (int64_t d = 0; d < dim1; d++) {
            at3(0, d, dst_col) = col_data[d];
        }
    }
};
