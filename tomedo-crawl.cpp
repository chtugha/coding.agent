// ============================================================
// tomedo-crawl  —  RAG sidecar for Prodigy
// ============================================================
//
// TOMEDO API DISCOVERY  (probed live 2026-04-11, server 192.168.10.9)
// ============================================================
//
// LIVE PROBE EVIDENCE (redacted PII — field shapes confirmed):
//
//   $ curl -sk --cert client.pem --key client.pem \
//       https://192.168.10.9:8443/tomedo_live/serverstatus
//   → {"status":"OK","softwareVersion":"2026-03-30 15:45","revision":121155207,...}
//
//   $ curl -sk ... https://192.168.10.9:8443/tomedo_live/patient?flach=true \
//       | python3 -c "import json,sys; d=json.load(sys.stdin); print(len(d))"
//   → 15421
//
//   $ curl -sk ... https://192.168.10.9:8443/tomedo_live/patient?flach=true \
//       | python3 -c "import json,sys; print(json.dumps(json.load(sys.stdin)[0],indent=2))"
//   → {
//       "ident": 1,
//       "nachname": "Arnold",        ← confirmed field name
//       "vorname": "Herbert",        ← confirmed field name
//       "titel": null,
//       "geburtsDatum": -530802000000,  ← epoch ms (negative = before 1970)
//       "ort": "Pfronstetten",
//       "zuletztAufgerufen": 1775631991660,
//       "patientenDetails": {"ident": 1},  ← flat list has NO phone data
//       "nachname_phonetic": "_ernolc_",
//       "vorname_phonetic": "_erferc_",
//       "revision": null,
//       "geburtsname": null
//     }
//
//   $ curl -sk ... https://192.168.10.9:8443/tomedo_live/patient/776
//   → {
//       "nachname": "Kunsch",
//       "vorname": "Lothar",
//       "patientenDetails": {
//         "kontaktdaten": {
//           "telefon":      "07383-942735",  ← confirmed field name
//           "telefon2":     null,
//           "handyNummer":  null,             ← confirmed field name
//           "telefon3":     null,
//           "weitereTelefonummern": [],
//           "fax": null,
//           ...
//         },
//         ...
//       }
//     }
//
//   PHONE SEARCH — CONFIRMED DOES NOT WORK BY PHONE DIGITS:
//   $ curl -sk ... ".../patient/searchByAttributes?query=942735&telefonNummern=true"
//   → {}     ← empty dict, not an array — name-only search confirmed
//
//   $ curl -sk ... ".../patient/1403/patientenDetailsRelationen/medikamentenPlan"
//   → 12 entries, e.g.:
//     { "nameBeiVerordnung": "AMLODIPIN/Valsartan/HCT Heumann 10/160/12,5mg FTA 98 ST",
//       "dosierungFrueh": null, "dosierungMittag": null, "dosierungAbend": null,
//       "wirkstaerkeBeiVerordnung": "10 mg / 160 mg / 12,5 mg",
//       "darreichungBeiVerordnung": "FITBL", ... }
//
//   $ curl -sk ... ".../patient/3892/patientenDetailsRelationen?limitScheine=true&limitMedikamentenPlan=50"
//   → diagnosen: 101 entries, e.g.:
//     { "freitext": "Sinusitis",   ← human-readable ICD description confirmed
//       "typ": null,               ← some entries have null typ, use freitext as primary
//       "icdKatalogEintrag": {"ident": 12345}, ... }
//     other entries: {"freitext": "lokal allergische Reaktion auf Wespenstich", "typ": "G"}
//
// ============================================================
//
// BASE URL:    https://192.168.10.9:8443/tomedo_live/
// AUTH:        Mutual TLS — macOS Keychain identity "tomedoClientCert"
//              (self-signed RSA-4096; the Tomedo macOS client installs
//               this certificate pair automatically on first server
//               connection). Export once:
//                 security export -k ~/Library/Keychains/login.keychain-db \
//                   -t identities -f pkcs12 -P "" \
//                   -o /tmp/tomedo_client.p12
//                 openssl pkcs12 -legacy -in /tmp/tomedo_client.p12 -nodes \
//                   -passin pass:"" -out /etc/tomedo-crawl/client.pem
//              PEM contains both cert and private key (no password).
//              Use OpenSSL: SSL_CTX_use_certificate_file +
//                           SSL_CTX_use_PrivateKey_file.
//              NO HTTP Authorization header required.
//
// PATIENT LIST (flat, no phone data):
//   GET /patient?flach=true
//   → JSON array, 15 421 records (confirmed 2026-04-11)
//   Fields: ident, nachname, vorname, titel, geburtsDatum (epoch ms),
//           ort, zuletztAufgerufen — phone NOT included.
//
// PATIENT FULL RECORD (includes phone fields):
//   GET /patient/{id}
//   Phone fields inside patientenDetails.kontaktdaten (confirmed names):
//     telefon         — main phone (may contain \n-separated entries)
//     telefon2        — secondary phone
//     handyNummer     — mobile
//     telefon3        — tertiary phone
//     weitereTelefonummern[] — additional numbers
//
// PHONE-BASED CALLER LOOKUP:
//   No server-side phone-search endpoint exists (confirmed).
//   searchByAttributes?query={digits}&telefonNummern=true → {} (empty).
//   → Build local phone_index SQLite table during background crawl.
//   → Lookup at call time: query local SQLite by phone digits (LIKE match).
//
// PATIENT DETAILS WITH CLINICAL RELATIONS:
//   GET /patient/{id}/patientenDetailsRelationen
//       ?limitScheine=true&limitKartei=50&limitFormulare=10
//       &limitVerordnungen=50&limitMedikamentenPlan=50
//       &limitZeiterfassungen=true&limitBehandlungsfaelle=true
//   → JSON object; key arrays:
//     diagnosen[]  — { freitext: "human-readable text", typ: "G"|"V"|null,
//                      icdKatalogEintrag.ident }
//     (other arrays: karteiEintraege, behandlungsfaelle, ...)
//
// MEDICATIONS (separate endpoint — not in patientenDetailsRelationen body):
//   GET /patient/{id}/patientenDetailsRelationen/medikamentenPlan
//   → JSON array, e.g. 12 entries:
//     { nameBeiVerordnung, wirkstaerkeBeiVerordnung,
//       darreichungBeiVerordnung,
//       dosierungFrueh, dosierungMittag, dosierungAbend, dosierungNacht }
//
// APPOINTMENTS:
//   GET /patient/{id}/termine?flach=true
//   → JSON array: { ident, beginn: epoch_ms, ende: epoch_ms, info }
//
// VISITS (Besuch):
//   GET /besuch/{patient_id}/besucheForPatient
//
// SERVER HEALTH:
//   GET /serverstatus → { status: "OK", softwareVersion, revision }
//
// NO BRIEFKOMMANDO API:
//   Briefkommando ($[d:...]$, $[&p.name]$, etc.) is client-side only.
//   Context documents are composed directly from the JSON fields above.
//
// NO STATISTICS SQL API:
//   GET /statistik/ → "RESTEASY003210: Could not find resource"
//   Custom SQL queries are not supported via REST.
//
// RAG CONTEXT DOCUMENT FORMAT (produced per patient):
//   Patient: {vorname} {nachname} (ID {ident}), geb. {geburtsDatum}
//   Diagnosen: {diagnosen[].freitext} [max 20 with non-null freitext]
//   Medikamente: {nameBeiVerordnung} {dosierungFrueh}-{mittag}-{abend}
//   Nächster Termin: {beginn_formatted} ({info})
//   Telefon: {telefon}
//
// PAGINATION:
//   Flat patient list returns all ~15k records in one HTTP response.
//   No server-side pagination parameter found.
//   Background crawl processes patients in batches of 100 with 10ms
//   sleep between batches to avoid hammering the server.
//
// ============================================================

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <sstream>
#include <fstream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <memory>
#include <vector>
#include <algorithm>
#include <ctime>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include "mongoose.h"
#include "sqlite3.h"
#include "interconnect.h"
#include "hnswlib.h"

namespace {

std::atomic<bool> s_quit{false};

void sig_handler(int) { s_quit.store(true); }

// ============================================================
// Config
// ============================================================

struct TomedoConfig {
    std::string tomedo_host      = "192.168.10.9";
    int         tomedo_port      = 8443;
    std::string tomedo_db        = "tomedo_live";
    std::string tomedo_user      = "";
    std::string tomedo_pass      = "";
    std::string tomedo_cert_pem  = "/etc/tomedo-crawl/client.pem";
    int         crawl_interval_sec = 3600;
    std::string ollama_url       = "http://127.0.0.1:11434";
    std::string ollama_model     = "nomic-embed-text";
    std::string api_host         = "127.0.0.1";
    int         api_port         = 13181;
    int         log_port         = 22022;
    std::string db_path          = "tomedo-crawl.db";
    size_t      hnsw_max_elements = 500000;
};

static int parse_int(const std::string& val, int fallback) {
    try { return std::stoi(val); }
    catch (...) { return fallback; }
}

static int parse_port(const std::string& val, int fallback) {
    int v = parse_int(val, fallback);
    return (v > 0 && v <= 65535) ? v : fallback;
}

static TomedoConfig parse_config(const std::string& path) {
    TomedoConfig cfg;
    std::ifstream f(path);
    if (!f.is_open()) {
        std::fprintf(stderr, "tomedo-crawl: config file '%s' not found, using defaults\n", path.c_str());
        return cfg;
    }
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty() || line[0] == '#') continue;
        auto eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string key = line.substr(0, eq);
        std::string val = line.substr(eq + 1);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(val.begin());
        while (!val.empty() && (val.back() == '\r' || val.back() == '\n')) val.pop_back();
        if (key == "tomedo_host")             cfg.tomedo_host = val;
        else if (key == "tomedo_port")        cfg.tomedo_port = parse_port(val, cfg.tomedo_port);
        else if (key == "tomedo_db")          cfg.tomedo_db = val;
        else if (key == "tomedo_user")        cfg.tomedo_user = val;
        else if (key == "tomedo_pass")        cfg.tomedo_pass = val;
        else if (key == "tomedo_cert_pem")    cfg.tomedo_cert_pem = val;
        else if (key == "crawl_interval_sec") {
            int v = parse_int(val, cfg.crawl_interval_sec);
            cfg.crawl_interval_sec = (v > 0) ? v : cfg.crawl_interval_sec;
        }
        else if (key == "ollama_url")         cfg.ollama_url = val;
        else if (key == "ollama_model")       cfg.ollama_model = val;
        else if (key == "api_host")           cfg.api_host = val;
        else if (key == "api_port")           cfg.api_port = parse_port(val, cfg.api_port);
        else if (key == "log_port")           cfg.log_port = parse_port(val, cfg.log_port);
        else if (key == "db_path")            cfg.db_path = val;
        else if (key == "hnsw_max_elements") {
            try {
                size_t v = std::stoul(val);
                if (v > 0) cfg.hnsw_max_elements = v;
            } catch (...) {}
        }
    }
    return cfg;
}

// ============================================================
// LogForwarder (re-use from interconnect.h via ServiceType)
// ============================================================

whispertalk::LogForwarder g_log;

#define LOG_INFO(fmt, ...)  g_log.forward(whispertalk::LogLevel::INFO,  0, fmt, ##__VA_ARGS__)
#define LOG_WARN(fmt, ...)  g_log.forward(whispertalk::LogLevel::WARN,  0, fmt, ##__VA_ARGS__)
#define LOG_ERROR(fmt, ...) g_log.forward(whispertalk::LogLevel::ERROR, 0, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...) g_log.forward(whispertalk::LogLevel::DEBUG, 0, fmt, ##__VA_ARGS__)

// ============================================================
// State (stubs — filled in later steps)
// ============================================================


std::atomic<long> g_last_crawl_time{0};
std::atomic<bool> g_crawl_requested{false};

constexpr int MG_POLL_TIMEOUT_MS = 100;

// ============================================================
// Ollama embedding stub (completed in Step 6)
// ============================================================

static std::vector<float> embed_text(const std::string& /*text*/, const TomedoConfig& /*cfg*/) {
    return {};
}

// ============================================================
// VectorStore — hnswlib (ANN) + SQLite (persistence + text)
// ============================================================

struct QueryResult {
    std::string text;
    float       score;
    std::string source;
    int         patient_id;
};

static constexpr int HNSW_M        = 16;
static constexpr int HNSW_EF_BUILD = 200;
static constexpr int HNSW_EF_QUERY = 50;

class VectorStore {
    sqlite3*                                        db_           = nullptr;
    std::unique_ptr<hnswlib::L2Space>                space_;
    std::unique_ptr<hnswlib::HierarchicalNSW<float>> hnsw_;
    int                                              dim_          = 0;
    size_t                                           max_elements_ = 500000;
    mutable std::mutex                               mutex_;

    bool ensure_index(int dim) {
        if (hnsw_) {
            if (dim_ != dim) {
                LOG_ERROR("VectorStore: dim mismatch (stored=%d, new=%d)", dim_, dim);
                return false;
            }
            return true;
        }
        dim_   = dim;
        space_ = std::make_unique<hnswlib::L2Space>(static_cast<size_t>(dim));
        hnsw_  = std::make_unique<hnswlib::HierarchicalNSW<float>>(
                     space_.get(), max_elements_, HNSW_M, HNSW_EF_BUILD);
        hnsw_->setEf(HNSW_EF_QUERY);
        return true;
    }

public:
    void set_max_elements(size_t n) { max_elements_ = n; }

    bool open(const std::string& db_path) {
        int rc = sqlite3_open(db_path.c_str(), &db_);
        if (rc != SQLITE_OK) {
            LOG_ERROR("VectorStore: sqlite3_open(%s): %s", db_path.c_str(), sqlite3_errmsg(db_));
            return false;
        }
        sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(db_, "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);
        const char* schema =
            "CREATE TABLE IF NOT EXISTS chunks ("
            "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
            "  source     TEXT    NOT NULL,"
            "  patient_id INTEGER,"
            "  text       TEXT    NOT NULL,"
            "  embedding  BLOB    NOT NULL,"
            "  updated_at INTEGER NOT NULL"
            ");"
            "CREATE INDEX IF NOT EXISTS idx_patient ON chunks(patient_id);"
            "CREATE UNIQUE INDEX IF NOT EXISTS idx_source_patient ON chunks(source, patient_id);";
        char* errmsg = nullptr;
        rc = sqlite3_exec(db_, schema, nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            LOG_ERROR("VectorStore: schema: %s", errmsg ? errmsg : "unknown");
            sqlite3_free(errmsg);
            return false;
        }
        return true;
    }

    // Must be called from main() before the HTTP server thread starts.
    // No mutex needed — no other threads access the store at this point.
    void rebuild_index() {
        if (!db_) return;
        sqlite3_stmt* stmt = nullptr;
        int rc = sqlite3_prepare_v2(db_,
            "SELECT id, embedding FROM chunks", -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            LOG_ERROR("VectorStore: rebuild_index prepare: %s", sqlite3_errmsg(db_));
            return;
        }

        int count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            sqlite3_int64 id         = sqlite3_column_int64(stmt, 0);
            const void*   blob       = sqlite3_column_blob(stmt,  1);
            int           blob_bytes = sqlite3_column_bytes(stmt, 1);
            if (!blob || blob_bytes < static_cast<int>(sizeof(float))) continue;
            int emb_dim = blob_bytes / static_cast<int>(sizeof(float));
            if (!ensure_index(emb_dim)) continue;
            try {
                hnsw_->addPoint(blob, static_cast<size_t>(id));
                ++count;
            } catch (const std::exception& e) {
                LOG_ERROR("VectorStore: rebuild addPoint id=%lld: %s",
                          (long long)id, e.what());
            } catch (...) {
                LOG_ERROR("VectorStore: rebuild addPoint id=%lld: unknown exception",
                          (long long)id);
            }
        }
        sqlite3_finalize(stmt);
        LOG_INFO("VectorStore: rebuilt index with %d vectors", count);
    }

    void upsert(const std::string& source, int patient_id,
                const std::string& text, const std::vector<float>& embedding) {
        if (!db_ || embedding.empty()) return;
        int emb_dim = static_cast<int>(embedding.size());
        int blob_bytes = emb_dim * static_cast<int>(sizeof(float));
        sqlite3_int64 now_ts = static_cast<sqlite3_int64>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        std::lock_guard<std::mutex> lk(mutex_);
        if (!ensure_index(emb_dim)) return;

        sqlite3_stmt* sel = nullptr;
        int rc = sqlite3_prepare_v2(db_,
            "SELECT id FROM chunks WHERE source=? AND patient_id=?",
            -1, &sel, nullptr);
        if (rc != SQLITE_OK) return;
        sqlite3_bind_text(sel, 1, source.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(sel, 2, patient_id);

        sqlite3_int64 existing_id = -1;
        if (sqlite3_step(sel) == SQLITE_ROW)
            existing_id = sqlite3_column_int64(sel, 0);
        sqlite3_finalize(sel);

        if (existing_id >= 0) {
            sqlite3_stmt* upd = nullptr;
            rc = sqlite3_prepare_v2(db_,
                "UPDATE chunks SET text=?, embedding=?, updated_at=? WHERE id=?",
                -1, &upd, nullptr);
            if (rc != SQLITE_OK) return;
            sqlite3_bind_text(upd, 1, text.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_blob(upd, 2, embedding.data(), blob_bytes, SQLITE_TRANSIENT);
            sqlite3_bind_int64(upd, 3, now_ts);
            sqlite3_bind_int64(upd, 4, existing_id);
            rc = sqlite3_step(upd);
            sqlite3_finalize(upd);
            if (rc != SQLITE_DONE) return;
            try {
                hnsw_->addPoint(embedding.data(), static_cast<size_t>(existing_id));
            } catch (const std::exception& e) {
                LOG_ERROR("VectorStore: upsert updatePoint id=%lld: %s — HNSW/SQLite diverged",
                          (long long)existing_id, e.what());
            } catch (...) {
                LOG_ERROR("VectorStore: upsert updatePoint id=%lld: unknown — HNSW/SQLite diverged",
                          (long long)existing_id);
            }
        } else {
            sqlite3_stmt* ins = nullptr;
            rc = sqlite3_prepare_v2(db_,
                "INSERT INTO chunks(source,patient_id,text,embedding,updated_at)"
                " VALUES(?,?,?,?,?)",
                -1, &ins, nullptr);
            if (rc != SQLITE_OK) return;
            sqlite3_bind_text(ins, 1, source.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(ins, 2, patient_id);
            sqlite3_bind_text(ins, 3, text.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_blob(ins, 4, embedding.data(), blob_bytes, SQLITE_TRANSIENT);
            sqlite3_bind_int64(ins, 5, now_ts);
            rc = sqlite3_step(ins);
            sqlite3_int64 rowid = sqlite3_last_insert_rowid(db_);
            sqlite3_finalize(ins);
            if (rc != SQLITE_DONE) return;

            auto rollback_row = [&](const char* reason) {
                LOG_ERROR("VectorStore: upsert addPoint rowid=%lld: %s — rolling back",
                          (long long)rowid, reason);
                sqlite3_stmt* del = nullptr;
                if (sqlite3_prepare_v2(db_, "DELETE FROM chunks WHERE id=?",
                                       -1, &del, nullptr) == SQLITE_OK) {
                    sqlite3_bind_int64(del, 1, rowid);
                    sqlite3_step(del);
                    sqlite3_finalize(del);
                }
            };
            try {
                hnsw_->addPoint(embedding.data(), static_cast<size_t>(rowid));
            } catch (const std::exception& e) {
                rollback_row(e.what());
            } catch (...) {
                rollback_row("unknown exception");
            }
        }
    }

    std::vector<QueryResult> query(const std::vector<float>& query_vec,
                                   int top_k, int patient_id_filter = -1) {
        if (!db_ || query_vec.empty()) return {};

        int fetch_k = (patient_id_filter >= 0) ? top_k * 4 : top_k;

        // Phase 1: ANN search — lock held only for HNSW, released before Phase 2.
        std::vector<std::pair<float, size_t>> candidates;
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (!hnsw_ || hnsw_->getCurrentElementCount() == 0) return {};
            if (static_cast<int>(query_vec.size()) != dim_) return {};
            int actual_k = std::min(fetch_k,
                static_cast<int>(hnsw_->getCurrentElementCount()));
            if (actual_k <= 0) return {};
            auto knn = hnsw_->searchKnn(query_vec.data(), static_cast<size_t>(actual_k));
            candidates.reserve(knn.size());
            while (!knn.empty()) {
                candidates.push_back(knn.top());
                knn.pop();
            }
        }

        // Phase 2: SQLite text fetch — safe without mutex_ because sqlite3.c is
        // compiled with SQLITE_THREADSAFE=1 (serialized mode), which serialises
        // all operations on this handle internally.
        std::vector<QueryResult> results;
        results.reserve(static_cast<size_t>(top_k));
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_,
                "SELECT text, source, patient_id FROM chunks WHERE id=?",
                -1, &stmt, nullptr) != SQLITE_OK)
            return results;
        for (auto& [dist, label] : candidates) {
            if (static_cast<int>(results.size()) >= top_k) break;
            sqlite3_reset(stmt);
            sqlite3_bind_int64(stmt, 1, static_cast<sqlite3_int64>(label));
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* t   = (const char*)sqlite3_column_text(stmt, 0);
                const char* s   = (const char*)sqlite3_column_text(stmt, 1);
                int         pid = sqlite3_column_int(stmt, 2);
                if (patient_id_filter < 0 || pid == patient_id_filter) {
                    QueryResult qr;
                    qr.text       = t ? t : "";
                    qr.source     = s ? s : "";
                    qr.patient_id = pid;
                    qr.score      = dist;
                    results.push_back(std::move(qr));
                }
            }
        }
        sqlite3_finalize(stmt);
        return results;
    }

    int doc_count() {
        if (!db_) return 0;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, "SELECT COUNT(*) FROM chunks",
                               -1, &stmt, nullptr) != SQLITE_OK)
            return 0;
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW)
            count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        return count;
    }

    int index_usage_pct() {
        std::lock_guard<std::mutex> lk(mutex_);
        if (!hnsw_ || max_elements_ == 0) return 0;
        return static_cast<int>(hnsw_->getCurrentElementCount() * 100 / max_elements_);
    }

    ~VectorStore() {
        hnsw_.reset();
        space_.reset();
        if (db_) sqlite3_close(db_);
    }
};

VectorStore g_vector_store;

// ============================================================
// CallerStore — thread-safe in-memory caller identity tracking
// ============================================================

enum class LookupStatus { PENDING, FOUND, NOT_FOUND, ERROR };

static const char* lookup_status_str(LookupStatus s) {
    switch (s) {
        case LookupStatus::PENDING:   return "pending";
        case LookupStatus::FOUND:     return "found";
        case LookupStatus::NOT_FOUND: return "not_found";
        case LookupStatus::ERROR:     return "error";
    }
    return "error";
}

struct CallerRecord {
    int call_id = 0;
    std::string phone_number;
    LookupStatus status = LookupStatus::PENDING;
    int patient_id = -1;
    std::string name;
    std::string vorname;
    std::chrono::steady_clock::time_point created_at;
};

class CallerStore {
    mutable std::mutex mutex_;
    std::unordered_map<int, CallerRecord> map_;
    static constexpr int TTL_SECONDS = 3600;
public:
    void register_caller(int call_id, const std::string& phone) {
        CallerRecord rec;
        rec.call_id = call_id;
        rec.phone_number = phone;
        rec.status = LookupStatus::PENDING;
        rec.created_at = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(mutex_);
        map_[call_id] = std::move(rec);
    }

    void update(int call_id, LookupStatus st, int patient_id,
                const std::string& name, const std::string& vorname) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = map_.find(call_id);
        if (it == map_.end()) return;
        it->second.status = st;
        it->second.patient_id = patient_id;
        it->second.name = name;
        it->second.vorname = vorname;
    }

    bool get(int call_id, CallerRecord& out) const {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = map_.find(call_id);
        if (it == map_.end()) return false;
        out = it->second;
        return true;
    }

    bool remove(int call_id) {
        std::lock_guard<std::mutex> lk(mutex_);
        return map_.erase(call_id) > 0;
    }

    void expire_old() {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> lk(mutex_);
        for (auto it = map_.begin(); it != map_.end(); ) {
            auto age = std::chrono::duration_cast<std::chrono::seconds>(
                now - it->second.created_at).count();
            if (age > TTL_SECONDS)
                it = map_.erase(it);
            else
                ++it;
        }
    }
};

CallerStore g_caller_store;

// ============================================================
// JSON helpers (hand-rolled, no external library)
// ============================================================

static std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 4);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[7];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
                break;
        }
    }
    return out;
}

static bool is_json_key_position(const std::string& body, size_t pos) {
    if (pos == 0) return true;
    for (size_t i = pos; i > 0; --i) {
        char ch = body[i - 1];
        if (ch == ' ' || ch == '\t' || ch == '\n' || ch == '\r') continue;
        return ch == '{' || ch == ',';
    }
    return true;
}

static long long json_get_int64(const std::string& body, const std::string& key, long long fallback) {
    std::string needle = "\"" + key + "\"";
    size_t search_from = 0;
    while (true) {
        auto pos = body.find(needle, search_from);
        if (pos == std::string::npos) return fallback;
        if (!is_json_key_position(body, pos)) {
            search_from = pos + 1;
            continue;
        }
        pos += needle.size();
        while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
        if (pos >= body.size() || body[pos] != ':') {
            search_from = pos;
            continue;
        }
        ++pos;
        while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
        if (pos >= body.size()) return fallback;
        if (body[pos] != '-' && (body[pos] < '0' || body[pos] > '9')) return fallback;
        return std::atoll(body.c_str() + pos);
    }
}

static std::vector<std::string> json_get_array_strings(const std::string& body, const std::string& key) {
    std::vector<std::string> result;
    std::string needle = "\"" + key + "\"";
    size_t search_from = 0;
    size_t pos;
    while (true) {
        pos = body.find(needle, search_from);
        if (pos == std::string::npos) return result;
        if (is_json_key_position(body, pos)) break;
        search_from = pos + 1;
    }
    pos += needle.size();
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == ':' || body[pos] == '\t')) ++pos;
    if (pos >= body.size() || body[pos] != '[') return result;
    ++pos;
    while (pos < body.size()) {
        while (pos < body.size() && body[pos] != '"' && body[pos] != ']') ++pos;
        if (pos >= body.size() || body[pos] == ']') break;
        ++pos;
        std::string item;
        while (pos < body.size() && body[pos] != '"') {
            if (body[pos] == '\\' && pos + 1 < body.size()) {
                ++pos;
                if (body[pos] == '"') item += '"';
                else if (body[pos] == '\\') item += '\\';
                else if (body[pos] == 'n') item += '\n';
                else item += body[pos];
            } else {
                item += body[pos];
            }
            ++pos;
        }
        if (pos < body.size()) ++pos;
        result.push_back(std::move(item));
    }
    return result;
}

static std::string json_get_string(const std::string& body, const std::string& key);

static std::string json_get_nested_string(const std::string& body,
                                           const std::string& outer_key,
                                           const std::string& inner_key) {
    std::string needle = "\"" + outer_key + "\"";
    size_t search_from = 0;
    size_t pos;
    while (true) {
        pos = body.find(needle, search_from);
        if (pos == std::string::npos) return {};
        if (is_json_key_position(body, pos)) break;
        search_from = pos + 1;
    }
    pos += needle.size();
    while (pos < body.size() && body[pos] != '{') ++pos;
    if (pos >= body.size()) return {};
    int depth = 1;
    size_t obj_start = pos;
    ++pos;
    while (pos < body.size() && depth > 0) {
        if (body[pos] == '{') ++depth;
        else if (body[pos] == '}') --depth;
        else if (body[pos] == '"') {
            ++pos;
            while (pos < body.size() && body[pos] != '"') {
                if (body[pos] == '\\') ++pos;
                ++pos;
            }
        }
        ++pos;
    }
    std::string sub = body.substr(obj_start, pos - obj_start);
    return json_get_string(sub, inner_key);
}

static std::string json_get_deep_string(const std::string& body,
                                         const std::string& k1,
                                         const std::string& k2,
                                         const std::string& k3) {
    std::string needle = "\"" + k1 + "\"";
    size_t search_from = 0;
    size_t pos;
    while (true) {
        pos = body.find(needle, search_from);
        if (pos == std::string::npos) return {};
        if (is_json_key_position(body, pos)) break;
        search_from = pos + 1;
    }
    pos += needle.size();
    while (pos < body.size() && body[pos] != '{') ++pos;
    if (pos >= body.size()) return {};
    int depth = 1;
    size_t obj_start = pos;
    ++pos;
    while (pos < body.size() && depth > 0) {
        if (body[pos] == '{') ++depth;
        else if (body[pos] == '}') --depth;
        else if (body[pos] == '"') {
            ++pos;
            while (pos < body.size() && body[pos] != '"') {
                if (body[pos] == '\\') ++pos;
                ++pos;
            }
        }
        ++pos;
    }
    std::string outer_sub = body.substr(obj_start, pos - obj_start);
    return json_get_nested_string(outer_sub, k2, k3);
}

static std::string json_get_string(const std::string& body, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    size_t search_from = 0;
    while (true) {
        auto pos = body.find(needle, search_from);
        if (pos == std::string::npos) return {};
        if (!is_json_key_position(body, pos)) {
            search_from = pos + 1;
            continue;
        }
        pos += needle.size();
        while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
        if (pos >= body.size() || body[pos] != ':') {
            search_from = pos;
            continue;
        }
        ++pos;
        while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
        if (pos >= body.size() || body[pos] != '"') return {};
        ++pos;
        std::string result;
        while (pos < body.size() && body[pos] != '"') {
            if (body[pos] == '\\' && pos + 1 < body.size()) {
                ++pos;
                switch (body[pos]) {
                    case '"':  result += '"';  break;
                    case '\\': result += '\\'; break;
                    case '/':  result += '/';  break;
                    case 'n':  result += '\n'; break;
                    case 'r':  result += '\r'; break;
                    case 't':  result += '\t'; break;
                    case 'b':  result += '\b'; break;
                    case 'f':  result += '\f'; break;
                    case 'u': {
                        if (pos + 4 < body.size()) {
                            unsigned cp = 0;
                            bool ok = true;
                            for (int d = 1; d <= 4 && ok; ++d) {
                                char h = body[pos + d];
                                cp <<= 4;
                                if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                                else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                                else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                                else ok = false;
                            }
                            if (ok) {
                                pos += 4;
                                if (cp >= 0xD800 && cp <= 0xDBFF &&
                                    pos + 6 < body.size() && body[pos + 1] == '\\' && body[pos + 2] == 'u') {
                                    unsigned lo = 0;
                                    bool ok2 = true;
                                    for (int d = 3; d <= 6 && ok2; ++d) {
                                        char h = body[pos + d];
                                        lo <<= 4;
                                        if (h >= '0' && h <= '9') lo |= (unsigned)(h - '0');
                                        else if (h >= 'a' && h <= 'f') lo |= (unsigned)(h - 'a' + 10);
                                        else if (h >= 'A' && h <= 'F') lo |= (unsigned)(h - 'A' + 10);
                                        else ok2 = false;
                                    }
                                    if (ok2 && lo >= 0xDC00 && lo <= 0xDFFF) {
                                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                        pos += 6;
                                    }
                                }
                                if (cp < 0x80) {
                                    result += (char)cp;
                                } else if (cp < 0x800) {
                                    result += (char)(0xC0 | (cp >> 6));
                                    result += (char)(0x80 | (cp & 0x3F));
                                } else if (cp < 0x10000) {
                                    result += (char)(0xE0 | (cp >> 12));
                                    result += (char)(0x80 | ((cp >> 6) & 0x3F));
                                    result += (char)(0x80 | (cp & 0x3F));
                                } else {
                                    result += (char)(0xF0 | (cp >> 18));
                                    result += (char)(0x80 | ((cp >> 12) & 0x3F));
                                    result += (char)(0x80 | ((cp >> 6) & 0x3F));
                                    result += (char)(0x80 | (cp & 0x3F));
                                }
                            } else {
                                result += 'u';
                            }
                        } else {
                            result += 'u';
                        }
                        break;
                    }
                    default:   result += body[pos]; break;
                }
            } else {
                result += body[pos];
            }
            ++pos;
        }
        return result;
    }
}

static int json_get_int(const std::string& body, const std::string& key, int fallback) {
    std::string needle = "\"" + key + "\"";
    size_t search_from = 0;
    while (true) {
        auto pos = body.find(needle, search_from);
        if (pos == std::string::npos) return fallback;
        if (!is_json_key_position(body, pos)) {
            search_from = pos + 1;
            continue;
        }
        pos += needle.size();
        while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
        if (pos >= body.size() || body[pos] != ':') {
            search_from = pos;
            continue;
        }
        ++pos;
        while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
        if (pos >= body.size()) return fallback;
        if (body[pos] != '-' && (body[pos] < '0' || body[pos] > '9')) return fallback;
        return std::atoi(body.c_str() + pos);
    }
}

// ============================================================
// HTTPS client (OpenSSL, statically linked — no runtime dependency)
// ============================================================

struct HttpResponse {
    int status = 0;
    std::string body;
};

static int tcp_connect(const std::string& host, int port, int timeout_ms) {
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), nullptr, &hints, &res) != 0 || !res)
            return -1;
        std::memcpy(&addr.sin_addr,
            &reinterpret_cast<struct sockaddr_in*>(res->ai_addr)->sin_addr,
            sizeof(addr.sin_addr));
        freeaddrinfo(res);
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = connect(fd, (struct sockaddr*)&addr, sizeof(addr));
    if (rc < 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (rc < 0) {
        struct pollfd pfd{fd, POLLOUT, 0};
        if (poll(&pfd, 1, timeout_ms) <= 0) { close(fd); return -1; }
        int err = 0; socklen_t len = sizeof(err);
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
        if (err != 0) { close(fd); return -1; }
    }

    fcntl(fd, F_SETFL, flags);
    return fd;
}

static std::string ssl_read_all(SSL* ssl, int timeout_ms) {
    std::string result;
    char buf[8192];
    int fd = SSL_get_fd(ssl);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;
        struct pollfd pfd{fd, POLLIN, 0};
        int pr = poll(&pfd, 1, static_cast<int>(std::min(remaining, (long long)1000)));
        if (pr <= 0) break;
        int n = SSL_read(ssl, buf, sizeof(buf));
        if (n <= 0) {
            int err = SSL_get_error(ssl, n);
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) continue;
            break;
        }
        result.append(buf, static_cast<size_t>(n));
    }
    return result;
}

static std::string decode_chunked(const std::string& body) {
    std::string result;
    size_t pos = 0;
    while (pos < body.size()) {
        auto crlf = body.find("\r\n", pos);
        if (crlf == std::string::npos || crlf == pos) break;
        size_t chunk_size = 0;
        for (size_t i = pos; i < crlf; ++i) {
            char c = body[i];
            chunk_size <<= 4;
            if (c >= '0' && c <= '9')      chunk_size |= static_cast<size_t>(c - '0');
            else if (c >= 'a' && c <= 'f') chunk_size |= static_cast<size_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') chunk_size |= static_cast<size_t>(c - 'A' + 10);
            else if (c == ';') break;
            else break;
        }
        if (chunk_size == 0) break;
        pos = crlf + 2;
        if (pos + chunk_size > body.size()) {
            result.append(body, pos, body.size() - pos);
            break;
        }
        result.append(body, pos, chunk_size);
        pos += chunk_size + 2;
    }
    return result;
}

static bool header_contains(const std::string& headers, const std::string& name,
                             const std::string& value) {
    std::string lname;
    lname.reserve(name.size());
    for (char c : name) lname += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    std::string lval;
    lval.reserve(value.size());
    for (char c : value) lval += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    size_t pos = 0;
    while (pos < headers.size()) {
        auto line_end = headers.find('\n', pos);
        if (line_end == std::string::npos) line_end = headers.size();
        auto colon = headers.find(':', pos);
        if (colon != std::string::npos && colon < line_end) {
            std::string hdr_name;
            for (size_t i = pos; i < colon; ++i)
                hdr_name += static_cast<char>(std::tolower(static_cast<unsigned char>(headers[i])));
            while (!hdr_name.empty() && hdr_name.back() == ' ') hdr_name.pop_back();
            if (hdr_name == lname) {
                size_t vstart = colon + 1;
                while (vstart < line_end && headers[vstart] == ' ') ++vstart;
                std::string hdr_val;
                for (size_t i = vstart; i < line_end; ++i) {
                    char c = headers[i];
                    if (c == '\r') continue;
                    hdr_val += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                }
                if (hdr_val.find(lval) != std::string::npos) return true;
            }
        }
        pos = line_end + 1;
    }
    return false;
}

static HttpResponse parse_http_response(const std::string& raw) {
    HttpResponse resp;
    if (raw.size() < 12 || raw.substr(0, 5) != "HTTP/") return resp;
    auto sp = raw.find(' ', 5);
    if (sp == std::string::npos) return resp;
    resp.status = std::atoi(raw.c_str() + sp + 1);
    auto hdr_end = raw.find("\r\n\r\n");
    size_t body_start;
    std::string headers;
    if (hdr_end == std::string::npos) {
        hdr_end = raw.find("\n\n");
        if (hdr_end == std::string::npos) return resp;
        headers = raw.substr(0, hdr_end);
        body_start = hdr_end + 2;
    } else {
        headers = raw.substr(0, hdr_end);
        body_start = hdr_end + 4;
    }
    resp.body = raw.substr(body_start);
    if (header_contains(headers, "Transfer-Encoding", "chunked"))
        resp.body = decode_chunked(resp.body);
    return resp;
}

static SSL_CTX* g_ssl_ctx = nullptr;
static std::mutex g_ssl_ctx_mutex;
static std::string g_ssl_ctx_pem;

static SSL_CTX* get_shared_ssl_ctx(const std::string& pem_path) {
    std::lock_guard<std::mutex> lk(g_ssl_ctx_mutex);
    if (g_ssl_ctx && g_ssl_ctx_pem == pem_path) return g_ssl_ctx;
    if (g_ssl_ctx) { SSL_CTX_free(g_ssl_ctx); g_ssl_ctx = nullptr; }
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return nullptr;
    SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
    if (!pem_path.empty()) {
        if (SSL_CTX_use_certificate_file(ctx, pem_path.c_str(), SSL_FILETYPE_PEM) != 1 ||
            SSL_CTX_use_PrivateKey_file(ctx, pem_path.c_str(), SSL_FILETYPE_PEM) != 1) {
            unsigned long err = ERR_get_error();
            char err_buf[256];
            ERR_error_string_n(err, err_buf, sizeof(err_buf));
            LOG_ERROR("HTTPS: failed to load client cert/key from %s: %s", pem_path.c_str(), err_buf);
            SSL_CTX_free(ctx);
            return nullptr;
        }
    }
    g_ssl_ctx = ctx;
    g_ssl_ctx_pem = pem_path;
    return ctx;
}

static void cleanup_shared_ssl_ctx() {
    std::lock_guard<std::mutex> lk(g_ssl_ctx_mutex);
    if (g_ssl_ctx) { SSL_CTX_free(g_ssl_ctx); g_ssl_ctx = nullptr; }
}

static HttpResponse https_request(const std::string& method,
                                   const std::string& host, int port,
                                   const std::string& path,
                                   const std::string& req_body,
                                   const std::string& pem_path,
                                   int timeout_ms) {
    HttpResponse fail;
    SSL_CTX* ctx = get_shared_ssl_ctx(pem_path);
    if (!ctx) return fail;

    int fd = tcp_connect(host, port, timeout_ms);
    if (fd < 0) return fail;

    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host.c_str());
    if (SSL_connect(ssl) != 1) {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        LOG_ERROR("HTTPS: SSL_connect to %s:%d failed: %s", host.c_str(), port, err_buf);
        SSL_free(ssl); close(fd);
        return fail;
    }

    std::ostringstream req;
    req << method << " " << path << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Connection: close\r\n";
    if (!req_body.empty()) {
        req << "Content-Type: application/json\r\n"
            << "Content-Length: " << req_body.size() << "\r\n";
    }
    req << "\r\n" << req_body;

    std::string raw_req = req.str();
    int written = SSL_write(ssl, raw_req.c_str(), static_cast<int>(raw_req.size()));
    if (written <= 0) {
        LOG_ERROR("HTTPS: SSL_write failed for %s %s", method.c_str(), path.c_str());
        SSL_free(ssl); close(fd);
        return fail;
    }

    std::string raw_resp = ssl_read_all(ssl, timeout_ms);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);

    return parse_http_response(raw_resp);
}

static HttpResponse https_get(const std::string& host, int port,
                               const std::string& path,
                               const std::string& pem_path,
                               int timeout_ms = 10000) {
    return https_request("GET", host, port, path, "", pem_path, timeout_ms);
}

static HttpResponse https_post(const std::string& host, int port,
                                const std::string& path,
                                const std::string& body,
                                const std::string& pem_path,
                                int timeout_ms = 10000) {
    return https_request("POST", host, port, path, body, pem_path, timeout_ms);
}

// ============================================================
// Phone index (local SQLite table for phone -> patient mapping)
// ============================================================

static bool phone_index_init(sqlite3* db) {
    const char* sql =
        "CREATE TABLE IF NOT EXISTS phone_index ("
        "  id         INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  phone      TEXT NOT NULL,"
        "  patient_id INTEGER NOT NULL,"
        "  name       TEXT,"
        "  vorname    TEXT"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_phone_digits ON phone_index(phone);"
        "CREATE UNIQUE INDEX IF NOT EXISTS idx_phone_patient ON phone_index(phone, patient_id);";
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        LOG_ERROR("phone_index_init: %s", errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        return false;
    }
    return true;
}

static void phone_index_upsert(sqlite3* db, const std::string& phone,
                                int patient_id, const std::string& name,
                                const std::string& vorname) {
    if (phone.empty()) return;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO phone_index(phone, patient_id, name, vorname)"
        " VALUES(?,?,?,?)", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, phone.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 2, patient_id);
    sqlite3_bind_text(stmt, 3, name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, vorname.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);
}

static std::string normalize_phone(const std::string& raw) {
    std::string digits;
    for (char c : raw) {
        if (c >= '0' && c <= '9') digits += c;
    }
    return digits;
}

struct PhoneLookupResult {
    bool found = false;
    int patient_id = -1;
    std::string name;
    std::string vorname;
};

static PhoneLookupResult phone_index_lookup(sqlite3* db, const std::string& phone) {
    PhoneLookupResult r;
    std::string digits = normalize_phone(phone);
    if (digits.size() < 4) return r;
    std::string pattern = "%" + digits.substr(digits.size() > 6 ? digits.size() - 6 : 0) + "%";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "SELECT patient_id, name, vorname FROM phone_index WHERE phone LIKE ? LIMIT 1",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return r;
    sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        r.found = true;
        r.patient_id = sqlite3_column_int(stmt, 0);
        const char* n = (const char*)sqlite3_column_text(stmt, 1);
        const char* v = (const char*)sqlite3_column_text(stmt, 2);
        r.name = n ? n : "";
        r.vorname = v ? v : "";
    }
    sqlite3_finalize(stmt);
    return r;
}

// ============================================================
// Tomedo patient enumeration + context fetch
// ============================================================

struct PatientRef {
    int ident = 0;
    std::string nachname;
    std::string vorname;
    long long geburts_datum = 0;
};

static std::vector<PatientRef> parse_patient_array(const std::string& json) {
    std::vector<PatientRef> result;
    size_t pos = 0;
    while (pos < json.size()) {
        auto obj_start = json.find('{', pos);
        if (obj_start == std::string::npos) break;
        int depth = 1;
        size_t obj_end = obj_start + 1;
        while (obj_end < json.size() && depth > 0) {
            if (json[obj_end] == '{') ++depth;
            else if (json[obj_end] == '}') --depth;
            else if (json[obj_end] == '"') {
                ++obj_end;
                while (obj_end < json.size() && json[obj_end] != '"') {
                    if (json[obj_end] == '\\') ++obj_end;
                    ++obj_end;
                }
            }
            ++obj_end;
        }
        std::string obj = json.substr(obj_start, obj_end - obj_start);
        PatientRef p;
        p.ident = json_get_int(obj, "ident", 0);
        p.nachname = json_get_string(obj, "nachname");
        p.vorname = json_get_string(obj, "vorname");
        p.geburts_datum = json_get_int64(obj, "geburtsDatum", 0);
        if (p.ident > 0)
            result.push_back(std::move(p));
        pos = obj_end;
    }
    return result;
}

static std::vector<PatientRef> enumerate_patients(const TomedoConfig& cfg) {
    std::string path = "/" + cfg.tomedo_db + "/patient?flach=true";
    auto resp = https_get(cfg.tomedo_host, cfg.tomedo_port, path, cfg.tomedo_cert_pem, 60000);
    if (resp.status != 200) {
        LOG_ERROR("enumerate_patients: HTTP %d", resp.status);
        return {};
    }
    return parse_patient_array(resp.body);
}

static std::string format_epoch_ms(long long epoch_ms) {
    if (epoch_ms == 0) return "unbekannt";
    time_t secs = static_cast<time_t>(epoch_ms / 1000);
    struct tm tm{};
    localtime_r(&secs, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%d.%m.%Y", &tm);
    return buf;
}

static std::string format_epoch_ms_datetime(long long epoch_ms) {
    if (epoch_ms == 0) return "unbekannt";
    time_t secs = static_cast<time_t>(epoch_ms / 1000);
    struct tm tm{};
    localtime_r(&secs, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%d.%m.%Y, %H:%M Uhr", &tm);
    return buf;
}

struct PatientPhones {
    std::string telefon;
    std::string telefon2;
    std::string handy;
    std::string telefon3;
};

static PatientPhones fetch_patient_phones(const std::string& body) {
    PatientPhones ph;
    ph.telefon  = json_get_deep_string(body, "patientenDetails", "kontaktdaten", "telefon");
    ph.telefon2 = json_get_deep_string(body, "patientenDetails", "kontaktdaten", "telefon2");
    ph.handy    = json_get_deep_string(body, "patientenDetails", "kontaktdaten", "handyNummer");
    ph.telefon3 = json_get_deep_string(body, "patientenDetails", "kontaktdaten", "telefon3");
    return ph;
}

static std::string extract_diagnosen(const std::string& body) {
    std::string result;
    std::string needle = "\"diagnosen\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return result;
    pos += needle.size();
    while (pos < body.size() && body[pos] != '[') ++pos;
    if (pos >= body.size()) return result;
    ++pos;
    int count = 0;
    while (pos < body.size() && count < 20) {
        auto obj_start = body.find('{', pos);
        if (obj_start == std::string::npos) break;
        auto arr_end = body.find(']', pos);
        if (arr_end != std::string::npos && arr_end < obj_start) break;
        int depth = 1;
        size_t obj_end = obj_start + 1;
        while (obj_end < body.size() && depth > 0) {
            if (body[obj_end] == '{') ++depth;
            else if (body[obj_end] == '}') --depth;
            else if (body[obj_end] == '"') {
                ++obj_end;
                while (obj_end < body.size() && body[obj_end] != '"') {
                    if (body[obj_end] == '\\') ++obj_end;
                    ++obj_end;
                }
            }
            ++obj_end;
        }
        std::string obj = body.substr(obj_start, obj_end - obj_start);
        std::string freitext = json_get_string(obj, "freitext");
        std::string typ = json_get_string(obj, "typ");
        if (!freitext.empty()) {
            if (!result.empty()) result += ", ";
            result += freitext;
            if (!typ.empty()) result += " (" + typ + ")";
            ++count;
        }
        pos = obj_end;
    }
    return result;
}

static std::string extract_medications(const std::string& body) {
    std::string result;
    size_t pos = 0;
    int count = 0;
    while (pos < body.size() && count < 20) {
        auto obj_start = body.find('{', pos);
        if (obj_start == std::string::npos) break;
        int depth = 1;
        size_t obj_end = obj_start + 1;
        while (obj_end < body.size() && depth > 0) {
            if (body[obj_end] == '{') ++depth;
            else if (body[obj_end] == '}') --depth;
            else if (body[obj_end] == '"') {
                ++obj_end;
                while (obj_end < body.size() && body[obj_end] != '"') {
                    if (body[obj_end] == '\\') ++obj_end;
                    ++obj_end;
                }
            }
            ++obj_end;
        }
        std::string obj = body.substr(obj_start, obj_end - obj_start);
        std::string name = json_get_string(obj, "nameBeiVerordnung");
        if (!name.empty()) {
            std::string df = json_get_string(obj, "dosierungFrueh");
            std::string dm = json_get_string(obj, "dosierungMittag");
            std::string da = json_get_string(obj, "dosierungAbend");
            if (!result.empty()) result += "; ";
            result += name;
            if (!df.empty() || !dm.empty() || !da.empty()) {
                result += " " + (df.empty() ? "0" : df) + "-"
                              + (dm.empty() ? "0" : dm) + "-"
                              + (da.empty() ? "0" : da);
            }
            ++count;
        }
        pos = obj_end;
    }
    return result;
}

static std::string extract_next_appointment(const std::string& body) {
    long long now_ms = static_cast<long long>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    long long best_begin = 0;
    std::string best_info;
    size_t pos = 0;
    while (pos < body.size()) {
        auto obj_start = body.find('{', pos);
        if (obj_start == std::string::npos) break;
        int depth = 1;
        size_t obj_end = obj_start + 1;
        while (obj_end < body.size() && depth > 0) {
            if (body[obj_end] == '{') ++depth;
            else if (body[obj_end] == '}') --depth;
            else if (body[obj_end] == '"') {
                ++obj_end;
                while (obj_end < body.size() && body[obj_end] != '"') {
                    if (body[obj_end] == '\\') ++obj_end;
                    ++obj_end;
                }
            }
            ++obj_end;
        }
        std::string obj = body.substr(obj_start, obj_end - obj_start);
        long long beginn = json_get_int64(obj, "beginn", 0);
        if (beginn > now_ms) {
            if (best_begin == 0 || beginn < best_begin) {
                best_begin = beginn;
                best_info = json_get_string(obj, "info");
            }
        }
        pos = obj_end;
    }
    if (best_begin == 0) return {};
    std::string result = format_epoch_ms_datetime(best_begin);
    if (!best_info.empty()) result += " (" + best_info + ")";
    return result;
}

static std::string fetch_patient_context(int patient_id, const TomedoConfig& cfg) {
    std::string base = "/" + cfg.tomedo_db;
    std::string pid = std::to_string(patient_id);

    auto detail_resp = https_get(cfg.tomedo_host, cfg.tomedo_port,
        base + "/patient/" + pid, cfg.tomedo_cert_pem, 15000);
    if (detail_resp.status != 200) return {};

    std::string nachname = json_get_string(detail_resp.body, "nachname");
    std::string vorname = json_get_string(detail_resp.body, "vorname");
    long long geb = json_get_int64(detail_resp.body, "geburtsDatum", 0);
    PatientPhones phones = fetch_patient_phones(detail_resp.body);

    auto rel_resp = https_get(cfg.tomedo_host, cfg.tomedo_port,
        base + "/patient/" + pid + "/patientenDetailsRelationen"
        "?limitScheine=true&limitKartei=50&limitMedikamentenPlan=50"
        "&limitVerordnungen=50&limitZeiterfassungen=true&limitBehandlungsfaelle=true",
        cfg.tomedo_cert_pem, 15000);
    std::string diagnosen;
    if (rel_resp.status == 200)
        diagnosen = extract_diagnosen(rel_resp.body);

    auto med_resp = https_get(cfg.tomedo_host, cfg.tomedo_port,
        base + "/patient/" + pid + "/patientenDetailsRelationen/medikamentenPlan",
        cfg.tomedo_cert_pem, 15000);
    std::string medikamente;
    if (med_resp.status == 200)
        medikamente = extract_medications(med_resp.body);

    auto termin_resp = https_get(cfg.tomedo_host, cfg.tomedo_port,
        base + "/patient/" + pid + "/termine?flach=true",
        cfg.tomedo_cert_pem, 15000);
    std::string termin;
    if (termin_resp.status == 200)
        termin = extract_next_appointment(termin_resp.body);

    std::ostringstream doc;
    doc << "Patient: " << vorname << " " << nachname
        << " (ID " << patient_id << "), geb. " << format_epoch_ms(geb) << "\n";
    if (!diagnosen.empty())
        doc << "Diagnosen: " << diagnosen << "\n";
    if (!medikamente.empty())
        doc << "Medikamente: " << medikamente << "\n";
    if (!termin.empty())
        doc << "Naechster Termin: " << termin << "\n";
    if (!phones.telefon.empty())
        doc << "Telefon: " << phones.telefon << "\n";
    if (!phones.handy.empty())
        doc << "Handy: " << phones.handy << "\n";

    return doc.str();
}

// ============================================================
// Background phone crawl (populates phone_index from Tomedo)
// ============================================================

static int run_phone_crawl(const TomedoConfig& cfg, sqlite3* db) {
    LOG_INFO("Phone crawl starting...");
    auto patients = enumerate_patients(cfg);
    if (patients.empty()) {
        LOG_WARN("Phone crawl: no patients returned");
        return 0;
    }
    LOG_INFO("Phone crawl: %d patients to process", (int)patients.size());

    int phone_count = 0;
    std::string base = "/" + cfg.tomedo_db;
    for (size_t i = 0; i < patients.size(); ++i) {
        if (s_quit.load()) break;
        int pid = patients[i].ident;
        auto resp = https_get(cfg.tomedo_host, cfg.tomedo_port,
            base + "/patient/" + std::to_string(pid),
            cfg.tomedo_cert_pem, 15000);
        if (resp.status != 200) continue;

        std::string nachname = json_get_string(resp.body, "nachname");
        std::string vorname_val = json_get_string(resp.body, "vorname");
        PatientPhones ph = fetch_patient_phones(resp.body);

        auto store_phone = [&](const std::string& raw) {
            if (raw.empty()) return;
            std::string digits = normalize_phone(raw);
            if (digits.size() >= 4) {
                phone_index_upsert(db, digits, pid, nachname, vorname_val);
                ++phone_count;
            }
        };
        store_phone(ph.telefon);
        store_phone(ph.telefon2);
        store_phone(ph.handy);
        store_phone(ph.telefon3);

        if ((i + 1) % 100 == 0) {
            LOG_INFO("Phone crawl: %d/%d patients processed, %d phones",
                     (int)(i + 1), (int)patients.size(), phone_count);
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    LOG_INFO("Phone crawl complete: %d phones indexed from %d patients",
             phone_count, (int)patients.size());
    return phone_count;
}

// ============================================================
// Caller resolution (uses local phone_index)
// ============================================================

static sqlite3* g_phone_db = nullptr;

static void resolve_caller(int call_id, const std::string& phone) {
    if (!g_phone_db || phone.empty()) {
        g_caller_store.update(call_id, LookupStatus::NOT_FOUND, -1, "", "");
        return;
    }
    auto result = phone_index_lookup(g_phone_db, phone);
    if (result.found) {
        LOG_INFO("Caller resolved: call_id=%d -> patient=%d %s %s",
                 call_id, result.patient_id, result.vorname.c_str(), result.name.c_str());
        g_caller_store.update(call_id, LookupStatus::FOUND,
                              result.patient_id, result.name, result.vorname);
    } else {
        LOG_DEBUG("Caller not found in phone_index: call_id=%d phone=%s", call_id, phone.c_str());
        g_caller_store.update(call_id, LookupStatus::NOT_FOUND, -1, "", "");
    }
}

// ============================================================
// ResolveQueue — bounded single-background-thread worker queue
// ============================================================

class ResolveQueue {
    struct Job { int call_id; std::string phone; };
    std::queue<Job>          queue_;
    std::mutex               mutex_;
    std::condition_variable  cv_;
    bool                     stop_ = false;
    std::thread              worker_;

public:
    void start() {
        worker_ = std::thread([this]() {
            while (true) {
                Job job;
                {
                    std::unique_lock<std::mutex> lk(mutex_);
                    cv_.wait(lk, [this]{ return stop_ || !queue_.empty(); });
                    // Drain remaining jobs after stop_ is set so in-flight
                    // caller registrations complete. NOTE (Step 5): when
                    // resolve_caller makes a live TLS/HTTP call (~1-5 s each),
                    // any jobs queued before SIGINT will run to completion.
                    // If that is unacceptable, change this to
                    //   if (stop_) return;
                    // to abandon pending jobs on shutdown.
                    if (stop_ && queue_.empty()) return;
                    job = std::move(queue_.front());
                    queue_.pop();
                }
                resolve_caller(job.call_id, job.phone);
            }
        });
    }

    void enqueue(int call_id, const std::string& phone) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            queue_.push({call_id, phone});
        }
        cv_.notify_one();
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stop_ = true;
        }
        cv_.notify_one();
        if (worker_.joinable()) worker_.join();
    }

    ~ResolveQueue() { shutdown(); }
};

ResolveQueue g_resolve_queue;

// ============================================================
// Mongoose HTTP server
// ============================================================

void http_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev != MG_EV_HTTP_MSG) return;
    struct mg_http_message *hm = (struct mg_http_message *)ev_data;

    if (mg_strcmp(hm->uri, mg_str("/health")) == 0) {
        long lc = g_last_crawl_time.load();
        int docs = g_vector_store.doc_count();
        int idx_pct = g_vector_store.index_usage_pct();
        std::ostringstream j;
        j << "{\"status\":\"ok\","
          << "\"indexed_docs\":" << docs << ","
          << "\"index_usage_pct\":" << idx_pct << ","
          << "\"last_crawl\":";
        if (lc == 0) j << "null";
        else         j << lc;
        j << "}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", j.str().c_str());

    } else if (mg_strcmp(hm->uri, mg_str("/query")) == 0 &&
               mg_strcmp(hm->method, mg_str("POST")) == 0) {
        std::string body(hm->body.buf, hm->body.len);
        std::string query_text = json_get_string(body, "text");
        int top_k = json_get_int(body, "top_k", 3);
        if (top_k <= 0) top_k = 3;
        if (top_k > 20) top_k = 20;
        int patient_id_filter = json_get_int(body, "patient_id", -1);

        if (query_text.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                          "{\"error\":\"text parameter required\"}");
            return;
        }

        auto emb = embed_text(query_text, *static_cast<const TomedoConfig*>(c->fn_data));
        if (emb.empty()) {
            mg_http_reply(c, 503, "Content-Type: application/json\r\n",
                          "{\"error\":\"embedding_unavailable\"}");
            return;
        }

        auto results = g_vector_store.query(emb, top_k, patient_id_filter);

        std::ostringstream j;
        j << "{\"results\":[";
        for (size_t i = 0; i < results.size(); ++i) {
            if (i) j << ",";
            float safe_score = std::isfinite(results[i].score) ? results[i].score : 0.0f;
            j << "{"
              << "\"text\":\""    << json_escape(results[i].text)   << "\","
              << "\"source\":\""  << json_escape(results[i].source) << "\","
              << "\"patient_id\":" << results[i].patient_id          << ","
              << "\"score\":"      << safe_score
              << "}";
        }
        j << "]}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", j.str().c_str());

    } else if (mg_strcmp(hm->uri, mg_str("/caller")) == 0 &&
               mg_strcmp(hm->method, mg_str("POST")) == 0) {
        std::string body(hm->body.buf, hm->body.len);
        int call_id = json_get_int(body, "call_id", -1);
        std::string phone = json_get_string(body, "phone_number");
        if (call_id < 0) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                          "{\"error\":\"missing call_id\"}");
            return;
        }
        g_caller_store.register_caller(call_id, phone);
        g_resolve_queue.enqueue(call_id, phone);
        mg_http_reply(c, 202, "Content-Type: application/json\r\n",
                      "{\"status\":\"pending\"}");

    } else if (mg_strcmp(hm->uri, mg_str("/crawl/trigger")) == 0 &&
               mg_strcmp(hm->method, mg_str("POST")) == 0) {
        g_crawl_requested.store(true);
        mg_http_reply(c, 202, "Content-Type: application/json\r\n",
                      "{\"status\":\"crawl_triggered\"}");

    } else {
        struct mg_str caps[1] = {};
        bool is_caller_wildcard = mg_match(hm->uri, mg_str("/caller/*"), caps);
        if (is_caller_wildcard && mg_strcmp(hm->method, mg_str("GET")) == 0) {
            int call_id = std::atoi(std::string(caps[0].buf, caps[0].len).c_str());
            CallerRecord rec;
            if (!g_caller_store.get(call_id, rec)) {
                mg_http_reply(c, 404, "Content-Type: application/json\r\n",
                              "{\"error\":\"not found\"}");
                return;
            }
            std::ostringstream j;
            j << "{\"status\":\"" << lookup_status_str(rec.status) << "\","
              << "\"call_id\":" << rec.call_id << ","
              << "\"phone_number\":\"" << json_escape(rec.phone_number) << "\","
              << "\"patient_id\":" << rec.patient_id << ",";
            if (rec.name.empty())
                j << "\"name\":null,";
            else
                j << "\"name\":\"" << json_escape(rec.name) << "\",";
            if (rec.vorname.empty())
                j << "\"vorname\":null";
            else
                j << "\"vorname\":\"" << json_escape(rec.vorname) << "\"";
            j << "}";
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", j.str().c_str());
        } else if (is_caller_wildcard && mg_strcmp(hm->method, mg_str("DELETE")) == 0) {
            int call_id = std::atoi(std::string(caps[0].buf, caps[0].len).c_str());
            g_caller_store.remove(call_id);
            mg_http_reply(c, 204, "", "");
        } else {
            mg_http_reply(c, 404, "", "Not Found\n");
        }
        return;
    }
}

// mgr is initialized in start() and owned exclusively by the event-loop thread
// after start() returns. Do not touch mgr from any other thread. mg_mgr_free()
// is called inside the event-loop thread on shutdown.
struct HttpServer {
    struct mg_mgr mgr;
    std::thread   thread;
    std::string   listen_addr;

    bool start(const TomedoConfig& cfg) {
        listen_addr = "http://" + cfg.api_host + ":" + std::to_string(cfg.api_port);
        mg_mgr_init(&mgr);
        struct mg_connection *c = mg_http_listen(&mgr, listen_addr.c_str(), http_handler,
                                                 const_cast<TomedoConfig*>(&cfg));
        if (!c) {
            LOG_ERROR("Failed to listen on %s", listen_addr.c_str());
            mg_mgr_free(&mgr);
            return false;
        }
        thread = std::thread([this]() {
            while (!s_quit.load()) {
                mg_mgr_poll(&mgr, MG_POLL_TIMEOUT_MS);
            }
            mg_mgr_free(&mgr);
        });
        return true;
    }

    ~HttpServer() {
        if (thread.joinable()) thread.join();
    }

    void join() {
        if (thread.joinable()) thread.join();
    }
};

} // namespace

int main(int argc, char** argv) {
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    std::string config_path = "tomedo-crawl.ini";
    if (argc >= 2) config_path = argv[1];

    TomedoConfig cfg = parse_config(config_path);

    g_log.init(static_cast<uint16_t>(cfg.log_port), whispertalk::ServiceType::TOMEDO_CRAWL_SERVICE);

    LOG_INFO("tomedo-crawl starting (api=%s:%d, db=%s)",
             cfg.api_host.c_str(), cfg.api_port, cfg.db_path.c_str());

    if (sqlite3_threadsafe() == 0) {
        LOG_ERROR("tomedo-crawl requires SQLite compiled with SQLITE_THREADSAFE>=1");
        return 1;
    }

    g_vector_store.set_max_elements(cfg.hnsw_max_elements);
    if (!g_vector_store.open(cfg.db_path)) {
        LOG_ERROR("Failed to open vector store at '%s'", cfg.db_path.c_str());
        return 1;
    }
    g_vector_store.rebuild_index();

    sqlite3* phone_db = nullptr;
    if (sqlite3_open(cfg.db_path.c_str(), &phone_db) != SQLITE_OK) {
        LOG_ERROR("Failed to open phone_index DB");
        return 1;
    }
    sqlite3_exec(phone_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(phone_db, "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);
    if (!phone_index_init(phone_db)) {
        LOG_ERROR("Failed to init phone_index table");
        sqlite3_close(phone_db);
        return 1;
    }
    g_phone_db = phone_db;

    g_resolve_queue.start();

    std::thread expiry_thread([]() {
        while (!s_quit.load()) {
            g_caller_store.expire_old();
            for (int i = 0; i < 300 && !s_quit.load(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    std::thread crawl_thread([&cfg, phone_db]() {
        run_phone_crawl(cfg, phone_db);
        g_last_crawl_time.store(static_cast<long>(
            std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count()));
        while (!s_quit.load()) {
            for (int i = 0; i < cfg.crawl_interval_sec && !s_quit.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (g_crawl_requested.load()) break;
            }
            if (s_quit.load()) break;
            g_crawl_requested.store(false);
            run_phone_crawl(cfg, phone_db);
            g_last_crawl_time.store(static_cast<long>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));
        }
    });

    HttpServer srv;
    if (!srv.start(cfg)) {
        s_quit.store(true);
        g_resolve_queue.shutdown();
        crawl_thread.join();
        expiry_thread.join();
        sqlite3_close(phone_db);
        return 1;
    }

    LOG_INFO("HTTP server listening on %s", srv.listen_addr.c_str());

    srv.join();
    g_resolve_queue.shutdown();
    crawl_thread.join();
    expiry_thread.join();
    sqlite3_close(phone_db);
    g_phone_db = nullptr;
    cleanup_shared_ssl_ctx();

    LOG_INFO("tomedo-crawl stopped");
    return 0;
}
