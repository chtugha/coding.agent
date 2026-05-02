#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <fstream>
#include <cstring>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <memory>
#include <cmath>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "hnswlib.h"
#pragma GCC diagnostic pop

namespace embedding_db {

struct QueryResult {
    std::string text;
    float       score;
    std::string source;
    int         patient_id;
};

struct ChunkMeta {
    std::string source;
    int         patient_id = 0;
    std::string text;
};

class EmbeddingDB {
    static inline constexpr int HNSW_M        = 16;
    static inline constexpr int HNSW_EF_BUILD = 200;
    static inline constexpr int HNSW_EF_QUERY = 50;
    static inline constexpr uint32_t META_VERSION = 1;

    std::unique_ptr<hnswlib::L2Space>                space_;
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> hnsw_;
    std::unordered_map<size_t, ChunkMeta>            meta_;
    std::unordered_map<std::string, size_t>          source_index_;
    int                                              dim_          = 0;
    size_t                                           max_elements_ = 500000;
    size_t                                           next_id_      = 1;
    mutable std::mutex                               mutex_;
    std::string                                      base_path_;

    bool ensure_index(int dim) {
        if (hnsw_) {
            if (dim_ != dim) return false;
            return true;
        }
        dim_   = dim;
        space_ = std::make_unique<hnswlib::L2Space>(static_cast<size_t>(dim));
        hnsw_  = std::make_unique<hnswlib::HierarchicalNSW<float>>(
                     space_.get(), max_elements_, HNSW_M, HNSW_EF_BUILD);
        hnsw_->setEf(HNSW_EF_QUERY);
        return true;
    }

    static void write_string(std::ofstream& f, const std::string& s) {
        uint32_t len = static_cast<uint32_t>(s.size());
        f.write(reinterpret_cast<const char*>(&len), sizeof(len));
        if (len > 0) f.write(s.data(), len);
    }

    static std::string read_string(std::ifstream& f) {
        uint32_t len = 0;
        f.read(reinterpret_cast<char*>(&len), sizeof(len));
        if (len == 0 || !f.good()) return {};
        std::string s(len, '\0');
        f.read(&s[0], len);
        return s;
    }

    static std::string make_source_key(const std::string& source, int patient_id) {
        return source + "\0" + std::to_string(patient_id);
    }

    void rebuild_source_index() {
        source_index_.clear();
        source_index_.reserve(meta_.size());
        for (const auto& [id, cm] : meta_)
            source_index_[make_source_key(cm.source, cm.patient_id)] = id;
    }

    bool save_locked() {
        if (base_path_.empty()) return false;

        std::string meta_path = base_path_ + ".meta";
        std::string hnsw_path = base_path_ + ".hnsw";
        std::string meta_tmp  = meta_path + ".tmp";

        if (hnsw_) {
            try { hnsw_->saveIndex(hnsw_path); } catch (...) { return false; }
        }

        std::ofstream mf(meta_tmp, std::ios::binary | std::ios::trunc);
        if (!mf.good()) return false;

        uint32_t version = META_VERSION;
        mf.write(reinterpret_cast<const char*>(&version), sizeof(version));

        int32_t dim = static_cast<int32_t>(dim_);
        mf.write(reinterpret_cast<const char*>(&dim), sizeof(dim));

        uint64_t count = static_cast<uint64_t>(meta_.size());
        mf.write(reinterpret_cast<const char*>(&count), sizeof(count));

        uint64_t next = static_cast<uint64_t>(next_id_);
        mf.write(reinterpret_cast<const char*>(&next), sizeof(next));

        for (const auto& [id, cm] : meta_) {
            uint64_t uid = static_cast<uint64_t>(id);
            mf.write(reinterpret_cast<const char*>(&uid), sizeof(uid));
            write_string(mf, cm.source);
            int32_t pid = static_cast<int32_t>(cm.patient_id);
            mf.write(reinterpret_cast<const char*>(&pid), sizeof(pid));
            write_string(mf, cm.text);
        }
        mf.flush();
        if (!mf.good()) return false;
        mf.close();

        return std::rename(meta_tmp.c_str(), meta_path.c_str()) == 0;
    }

public:
    void set_max_elements(size_t n) { max_elements_ = n; }

    bool open(const std::string& base_path) {
        std::lock_guard<std::mutex> lk(mutex_);
        base_path_ = base_path;
        std::string meta_path = base_path_ + ".meta";
        std::string hnsw_path = base_path_ + ".hnsw";

        std::ifstream mf(meta_path, std::ios::binary);
        if (mf.good()) {
            uint32_t version = 0;
            mf.read(reinterpret_cast<char*>(&version), sizeof(version));
            if (version != META_VERSION) return false;

            int32_t dim = 0;
            mf.read(reinterpret_cast<char*>(&dim), sizeof(dim));

            uint64_t count = 0;
            mf.read(reinterpret_cast<char*>(&count), sizeof(count));

            uint64_t next = 0;
            mf.read(reinterpret_cast<char*>(&next), sizeof(next));
            next_id_ = static_cast<size_t>(next);

            for (uint64_t i = 0; i < count && mf.good(); ++i) {
                uint64_t id = 0;
                mf.read(reinterpret_cast<char*>(&id), sizeof(id));
                ChunkMeta cm;
                cm.source = read_string(mf);
                int32_t pid = 0;
                mf.read(reinterpret_cast<char*>(&pid), sizeof(pid));
                cm.patient_id = pid;
                cm.text = read_string(mf);
                meta_[static_cast<size_t>(id)] = std::move(cm);
            }
            mf.close();

            rebuild_source_index();

            if (dim > 0) {
                dim_ = dim;
                space_ = std::make_unique<hnswlib::L2Space>(static_cast<size_t>(dim));
                try {
                    hnsw_ = std::make_unique<hnswlib::HierarchicalNSW<float>>(
                        space_.get(), hnsw_path, false, max_elements_);
                    hnsw_->setEf(HNSW_EF_QUERY);
                } catch (...) {
                    hnsw_.reset();
                    space_.reset();
                    dim_ = 0;
                    meta_.clear();
                    source_index_.clear();
                    next_id_ = 1;
                }
            }
        }
        return true;
    }

    bool save() {
        std::lock_guard<std::mutex> lk(mutex_);
        return save_locked();
    }

    void upsert(const std::string& source, int patient_id,
                const std::string& text, const std::vector<float>& embedding) {
        if (embedding.empty()) return;
        int emb_dim = static_cast<int>(embedding.size());

        std::lock_guard<std::mutex> lk(mutex_);
        if (!ensure_index(emb_dim)) return;

        std::string key = make_source_key(source, patient_id);
        auto sit = source_index_.find(key);

        if (sit != source_index_.end()) {
            size_t existing_id = sit->second;
            meta_[existing_id].text = text;
            try {
                hnsw_->addPoint(embedding.data(), existing_id);
            } catch (...) {}
        } else {
            size_t new_id = next_id_++;
            ChunkMeta cm;
            cm.source     = source;
            cm.patient_id = patient_id;
            cm.text       = text;
            meta_[new_id] = std::move(cm);
            source_index_[key] = new_id;
            try {
                hnsw_->addPoint(embedding.data(), new_id);
            } catch (...) {
                meta_.erase(new_id);
                source_index_.erase(key);
                --next_id_;
            }
        }
    }

    std::vector<QueryResult> query(const std::vector<float>& query_vec,
                                   int top_k, int patient_id_filter = -1) {
        if (query_vec.empty()) return {};

        int fetch_k = (patient_id_filter >= 0) ? top_k * 4 : top_k;

        std::lock_guard<std::mutex> lk(mutex_);
        if (!hnsw_ || hnsw_->getCurrentElementCount() == 0) return {};
        if (static_cast<int>(query_vec.size()) != dim_) return {};
        int actual_k = std::min(fetch_k,
            static_cast<int>(hnsw_->getCurrentElementCount()));
        if (actual_k <= 0) return {};
        auto knn = hnsw_->searchKnn(query_vec.data(), static_cast<size_t>(actual_k));

        std::vector<QueryResult> results;
        results.reserve(static_cast<size_t>(top_k));
        while (!knn.empty()) {
            auto [dist, label] = knn.top();
            knn.pop();
            if (static_cast<int>(results.size()) >= top_k) continue;
            auto it = meta_.find(label);
            if (it == meta_.end()) continue;
            if (patient_id_filter >= 0 && it->second.patient_id != patient_id_filter)
                continue;
            QueryResult qr;
            qr.text       = it->second.text;
            qr.source     = it->second.source;
            qr.patient_id = it->second.patient_id;
            qr.score      = dist;
            results.push_back(std::move(qr));
        }
        return results;
    }

    int doc_count() {
        std::lock_guard<std::mutex> lk(mutex_);
        return static_cast<int>(meta_.size());
    }

    void wipe() {
        std::lock_guard<std::mutex> lk(mutex_);
        meta_.clear();
        source_index_.clear();
        hnsw_.reset();
        space_.reset();
        dim_ = 0;
        next_id_ = 1;
    }

    int index_usage_pct() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!hnsw_ || max_elements_ == 0) return 0;
        return static_cast<int>(hnsw_->getCurrentElementCount() * 100 / max_elements_);
    }

    void close() {
        std::lock_guard<std::mutex> lk(mutex_);
        save_locked();
        hnsw_.reset();
        space_.reset();
        meta_.clear();
        source_index_.clear();
    }

    ~EmbeddingDB() {
        close();
    }
};

} // namespace embedding_db
