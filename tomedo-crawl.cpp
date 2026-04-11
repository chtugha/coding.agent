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
#include <sstream>
#include <fstream>
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <signal.h>

#include "mongoose.h"
#include "sqlite3.h"
#include "interconnect.h"

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

std::atomic<int>  g_indexed_docs{0};
std::atomic<long> g_last_crawl_time{0};

constexpr int MG_POLL_TIMEOUT_MS = 100;

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
            default:   out += c;
        }
    }
    return out;
}

static std::string json_get_string(const std::string& body, const std::string& key) {
    std::string needle = "\"" + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
    if (pos >= body.size() || body[pos] != ':') return {};
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
                case 'n':  result += '\n'; break;
                case 'r':  result += '\r'; break;
                case 't':  result += '\t'; break;
                default:   result += body[pos]; break;
            }
        } else {
            result += body[pos];
        }
        ++pos;
    }
    return result;
}

static int json_get_int(const std::string& body, const std::string& key, int fallback) {
    std::string needle = "\"" + key + "\"";
    auto pos = body.find(needle);
    if (pos == std::string::npos) return fallback;
    pos += needle.size();
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
    if (pos >= body.size() || body[pos] != ':') return fallback;
    ++pos;
    while (pos < body.size() && (body[pos] == ' ' || body[pos] == '\t')) ++pos;
    if (pos >= body.size()) return fallback;
    if (body[pos] != '-' && (body[pos] < '0' || body[pos] > '9')) return fallback;
    return std::atoi(body.c_str() + pos);
}

// ============================================================
// Caller resolution (stub — real lookup added in Step 5)
// ============================================================

static void resolve_caller(int call_id, const std::string& /*phone*/) {
    g_caller_store.update(call_id, LookupStatus::NOT_FOUND, -1, "", "");
}

// ============================================================
// Mongoose HTTP server
// ============================================================

void http_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev != MG_EV_HTTP_MSG) return;
    struct mg_http_message *hm = (struct mg_http_message *)ev_data;

    if (mg_strcmp(hm->uri, mg_str("/health")) == 0) {
        long lc = g_last_crawl_time.load();
        std::ostringstream j;
        j << "{\"status\":\"ok\","
          << "\"indexed_docs\":" << g_indexed_docs.load() << ","
          << "\"last_crawl\":";
        if (lc == 0) j << "null";
        else         j << lc;
        j << "}";
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
        std::thread([call_id, phone]() {
            resolve_caller(call_id, phone);
        }).detach();
        mg_http_reply(c, 202, "Content-Type: application/json\r\n",
                      "{\"status\":\"pending\"}");

    } else {
        struct mg_str caps[1] = {};
        if (mg_match(hm->uri, mg_str("/caller/*"), caps) &&
            mg_strcmp(hm->method, mg_str("GET")) == 0) {
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
        } else if (mg_match(hm->uri, mg_str("/caller/*"), caps) &&
                   mg_strcmp(hm->method, mg_str("DELETE")) == 0) {
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
        struct mg_connection *c = mg_http_listen(&mgr, listen_addr.c_str(), http_handler, nullptr);
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

    std::thread expiry_thread([]() {
        while (!s_quit.load()) {
            for (int i = 0; i < 300 && !s_quit.load(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
            g_caller_store.expire_old();
        }
    });

    HttpServer srv;
    if (!srv.start(cfg)) {
        s_quit.store(true);
        expiry_thread.join();
        return 1;
    }

    LOG_INFO("HTTP server listening on %s", srv.listen_addr.c_str());

    srv.join();
    expiry_thread.join();

    LOG_INFO("tomedo-crawl stopped");
    return 0;
}
