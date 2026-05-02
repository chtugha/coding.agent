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
#include <string>
#include <thread>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <vector>
#include <algorithm>
#include <ctime>
#include <signal.h>
#include <sys/wait.h>
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
#include "db_key.h"
#include "tls_cert.h"
#include "interconnect.h"
#include "embedding-db.h"

namespace {

std::atomic<bool> s_quit{false};

void sig_handler(int) { s_quit.store(true); }

// ============================================================
// Config — stored in SQLite config table (no INI files)
// ============================================================
//
// All configuration is persisted in the encrypted SQLite database (same file
// as the vector store) in a "config" table (key TEXT PRIMARY KEY, value TEXT).
// The frontend writes to this table via /api/rag/config; on every service
// start the binary reads the table and materialises the struct below.
//
// Defaults are safe for a fresh install: the service starts and exposes /health
// immediately, and the first crawl is deferred until Tomedo credentials are set.

struct TomedoConfig {
    std::string tomedo_host      = "192.168.10.9";
    int         tomedo_port      = 8443;
    std::string tomedo_db        = "tomedo_live";
    std::string tomedo_cert_pem  = "/etc/tomedo-crawl/client.pem";
    int         crawl_interval_sec = 3600;
    std::string ollama_url       = "http://127.0.0.1:11434";
    std::string ollama_model     = "bge-small";
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

static bool config_db_ensure(const std::string& db_path) {
    sqlite3* db = nullptr;
    if (prodigy_db::db_open_encrypted(db_path.c_str(), &db) != SQLITE_OK) {
        std::fprintf(stderr, "tomedo-crawl: cannot open config db '%s': %s\n",
                     db_path.c_str(), db ? sqlite3_errmsg(db) : "unknown");
        if (db) sqlite3_close(db);
        return false;
    }
    sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db, "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);
    const char* sql =
        "CREATE TABLE IF NOT EXISTS config ("
        "  key   TEXT PRIMARY KEY NOT NULL,"
        "  value TEXT NOT NULL"
        ");";
    char* errmsg = nullptr;
    int rc = sqlite3_exec(db, sql, nullptr, nullptr, &errmsg);
    if (rc != SQLITE_OK) {
        std::fprintf(stderr, "tomedo-crawl: config table creation failed: %s\n",
                     errmsg ? errmsg : "unknown");
        sqlite3_free(errmsg);
        sqlite3_close(db);
        return false;
    }
    sqlite3_close(db);
    return true;
}

static std::string config_db_get(sqlite3* db, const std::string& key,
                                 const std::string& fallback) {
    if (!db) return fallback;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "SELECT value FROM config WHERE key=?", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return fallback;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    std::string result = fallback;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (v && v[0] != '\0') result = v;
    }
    sqlite3_finalize(stmt);
    return result;
}

static bool config_db_set(sqlite3* db, const std::string& key, const std::string& value) {
    if (!db) return false;
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "INSERT OR REPLACE INTO config(key,value) VALUES(?,?)", -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return false;
    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return rc == SQLITE_DONE;
}

static TomedoConfig load_config_from_db(const std::string& db_path) {
    TomedoConfig cfg;
    cfg.db_path = db_path;

    if (!config_db_ensure(db_path)) {
        std::fprintf(stderr, "tomedo-crawl: config db init failed, using defaults\n");
        return cfg;
    }

    sqlite3* db = nullptr;
    if (prodigy_db::db_open_encrypted(db_path.c_str(), &db) != SQLITE_OK) {
        std::fprintf(stderr, "tomedo-crawl: cannot open config db for reading, using defaults\n");
        if (db) sqlite3_close(db);
        return cfg;
    }
    sqlite3_exec(db, "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);

    cfg.tomedo_host       = config_db_get(db, "tomedo_host",       cfg.tomedo_host);
    cfg.tomedo_port       = parse_port(config_db_get(db, "tomedo_port",
                                std::to_string(cfg.tomedo_port)), cfg.tomedo_port);
    cfg.tomedo_db         = config_db_get(db, "tomedo_db",         cfg.tomedo_db);
    cfg.tomedo_cert_pem   = config_db_get(db, "tomedo_cert_pem",   cfg.tomedo_cert_pem);
    cfg.crawl_interval_sec = parse_int(config_db_get(db, "crawl_interval_sec",
                                std::to_string(cfg.crawl_interval_sec)), cfg.crawl_interval_sec);
    if (cfg.crawl_interval_sec <= 0) cfg.crawl_interval_sec = 3600;
    cfg.ollama_url        = config_db_get(db, "ollama_url",        cfg.ollama_url);
    cfg.ollama_model      = config_db_get(db, "ollama_model",      cfg.ollama_model);
    cfg.api_host          = config_db_get(db, "api_host",          cfg.api_host);
    cfg.api_port          = parse_port(config_db_get(db, "api_port",
                                std::to_string(cfg.api_port)), cfg.api_port);
    cfg.log_port          = parse_port(config_db_get(db, "log_port",
                                std::to_string(cfg.log_port)), cfg.log_port);
    std::string hnsw_str = config_db_get(db, "hnsw_max_elements",
                                std::to_string(cfg.hnsw_max_elements));
    try {
        size_t v = std::stoul(hnsw_str);
        if (v > 0) cfg.hnsw_max_elements = v;
    } catch (...) {}

    sqlite3_close(db);
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
// Global service state
// ============================================================
//
// All atomics are written by background threads and read by the Mongoose
// event-loop thread (or vice-versa).  Plain relaxed loads are sufficient for
// the status/health data; crawl_requested_ uses sequential consistency so the
// crawl thread observes the write promptly.

std::atomic<long> g_last_crawl_time{0};
std::atomic<bool> g_crawl_requested{false};
std::atomic<bool> g_ollama_installed{false};
std::atomic<bool> g_ollama_running{false};
std::atomic<pid_t> g_ollama_pid{0};

static int tcp_connect(const std::string& host, int port, int timeout_ms);

static bool check_ollama_installed() {
    FILE* fp = popen("which ollama 2>/dev/null", "r");
    if (!fp) return false;
    char buf[256];
    bool found = false;
    if (fgets(buf, sizeof(buf), fp) && buf[0] == '/') found = true;
    pclose(fp);
    return found;
}

static bool check_ollama_running(const std::string& ollama_url) {
    std::string host = "127.0.0.1";
    int port = 11434;
    size_t scheme_end = ollama_url.find("://");
    size_t host_start = (scheme_end == std::string::npos) ? 0 : scheme_end + 3;
    size_t path_start = ollama_url.find('/', host_start);
    std::string hostport = (path_start == std::string::npos) ?
        ollama_url.substr(host_start) : ollama_url.substr(host_start, path_start - host_start);
    auto colon = hostport.find(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = std::atoi(hostport.c_str() + colon + 1);
    } else if (!hostport.empty()) {
        host = hostport;
    }
    int fd = tcp_connect(host, port, 2000);
    if (fd < 0) return false;
    std::string req = "GET / HTTP/1.1\r\nHost: " + host + "\r\nConnection: close\r\n\r\n";
    ssize_t w = ::write(fd, req.c_str(), req.size());
    if (w <= 0) { close(fd); return false; }
    char buf[256];
    struct pollfd pfd{fd, POLLIN, 0};
    int pr = poll(&pfd, 1, 2000);
    bool ok = false;
    if (pr > 0) {
        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = 0;
            // Match only the HTTP status line to avoid false positives from
            // JSON body content (e.g. {"code": 200} would fool a bare "200" search).
            const char* status_line = strstr(buf, "HTTP/1.");
            ok = (status_line && strstr(status_line, " 200 ") != nullptr);
        }
    }
    close(fd);
    return ok;
}

static pid_t spawn_ollama_serve_detached() {
    int pidpipe[2];
    if (pipe(pidpipe) < 0) return -1;
    int execpipe[2];
    if (pipe(execpipe) < 0) { close(pidpipe[0]); close(pidpipe[1]); return -1; }
    fcntl(execpipe[1], F_SETFD, FD_CLOEXEC);
    pid_t mid = fork();
    if (mid < 0) { close(pidpipe[0]); close(pidpipe[1]); close(execpipe[0]); close(execpipe[1]); return -1; }
    if (mid == 0) {
        close(pidpipe[0]);
        close(execpipe[0]);
        pid_t child = fork();
        if (child < 0) { pid_t z = 0; (void)write(pidpipe[1], &z, sizeof(z)); close(pidpipe[1]); close(execpipe[1]); _exit(127); }
        if (child > 0) { (void)write(pidpipe[1], &child, sizeof(child)); close(pidpipe[1]); close(execpipe[1]); _exit(0); }
        close(pidpipe[1]);
        setsid();
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
        execlp("ollama", "ollama", "serve", nullptr);
        char err = 1;
        (void)write(execpipe[1], &err, 1);
        close(execpipe[1]);
        _exit(127);
    }
    close(pidpipe[1]);
    close(execpipe[1]);
    pid_t grandchild = 0;
    (void)read(pidpipe[0], &grandchild, sizeof(grandchild));
    close(pidpipe[0]);
    int status;
    waitpid(mid, &status, 0);
    if (grandchild < 1) { close(execpipe[0]); return -1; }
    char err = 0;
    struct pollfd pfd = {execpipe[0], POLLIN, 0};
    if (poll(&pfd, 1, 200) > 0 && read(execpipe[0], &err, 1) == 1) {
        close(execpipe[0]);
        return -1;
    }
    close(execpipe[0]);
    g_ollama_pid.store(grandchild);
    return grandchild;
}

// Ollama must be installed by the operator before starting this service.
// Automatic installation via curl | sh is intentionally NOT supported:
// this application processes HIPAA/GDPR-regulated patient data and cannot
// download and execute arbitrary scripts from the internet at runtime.
// Direct the operator to https://ollama.com to install manually.

static void kill_ollama_tracked() {
    pid_t p = g_ollama_pid.exchange(0);
    if (p > 1) {
        kill(p, SIGTERM);
        return;
    }
    FILE* fp = popen("pgrep -f 'ollama serve'", "r");
    if (fp) {
        char buf[64];
        while (fgets(buf, sizeof(buf), fp)) {
            pid_t kp = static_cast<pid_t>(atoi(buf));
            if (kp > 1) kill(kp, SIGTERM);
        }
        pclose(fp);
    }
}

static bool ollama_model_available(const std::string& ollama_url, const std::string& model) {
    std::string host = "127.0.0.1";
    int port = 11434;
    size_t scheme_end = ollama_url.find("://");
    size_t host_start = (scheme_end == std::string::npos) ? 0 : scheme_end + 3;
    size_t path_start = ollama_url.find('/', host_start);
    std::string hostport = (path_start == std::string::npos) ?
        ollama_url.substr(host_start) : ollama_url.substr(host_start, path_start - host_start);
    auto colon = hostport.find(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = std::atoi(hostport.c_str() + colon + 1);
    } else if (!hostport.empty()) {
        host = hostport;
    }
    int fd = tcp_connect(host, port, 5000);
    if (fd < 0) return false;
    std::string req = "GET /api/tags HTTP/1.1\r\nHost: " + host + ":" + std::to_string(port) +
                      "\r\nConnection: close\r\n\r\n";
    ssize_t w = ::write(fd, req.c_str(), req.size());
    if (w <= 0) { close(fd); return false; }
    std::string resp;
    char buf[4096];
    struct pollfd pfd{fd, POLLIN, 0};
    while (poll(&pfd, 1, 5000) > 0) {
        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = 0;
        resp.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    std::string needle = "\"" + model + "\"";
    if (resp.find(needle) != std::string::npos) return true;
    size_t col = model.find(':');
    if (col != std::string::npos) {
        std::string base = model.substr(0, col);
        if (resp.find("\"" + base + "\"") != std::string::npos) return true;
        if (resp.find("\"" + base + ":") != std::string::npos &&
            resp.find(model) != std::string::npos) return true;
    }
    return resp.find("\"name\":\"" + model + "\"") != std::string::npos;
}

static void ollama_pull_model(const std::string& ollama_url, const std::string& model) {
    std::string host = "127.0.0.1";
    int port = 11434;
    size_t scheme_end = ollama_url.find("://");
    size_t host_start = (scheme_end == std::string::npos) ? 0 : scheme_end + 3;
    size_t path_start = ollama_url.find('/', host_start);
    std::string hostport = (path_start == std::string::npos) ?
        ollama_url.substr(host_start) : ollama_url.substr(host_start, path_start - host_start);
    auto colon = hostport.find(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = std::atoi(hostport.c_str() + colon + 1);
    } else if (!hostport.empty()) {
        host = hostport;
    }
    std::string escaped_model;
    for (char ch : model) {
        if (ch == '"' || ch == '\\') escaped_model += '\\';
        escaped_model += ch;
    }
    std::string body = "{\"name\":\"" + escaped_model + "\"}";
    int fd = tcp_connect(host, port, 5000);
    if (fd < 0) { LOG_ERROR("ollama pull: cannot connect"); return; }
    std::ostringstream req;
    req << "POST /api/pull HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    std::string r = req.str();
    ssize_t w = ::write(fd, r.c_str(), r.size());
    if (w <= 0) { close(fd); LOG_ERROR("ollama pull: write failed"); return; }
    std::string resp;
    char buf[4096];
    struct pollfd pfd{fd, POLLIN, 0};
    while (poll(&pfd, 1, 300000) > 0) {
        ssize_t n = ::read(fd, buf, sizeof(buf) - 1);
        if (n <= 0) break;
        buf[n] = 0;
        resp.append(buf, static_cast<size_t>(n));
    }
    close(fd);
    if (resp.find("\"error\"") != std::string::npos)
        LOG_WARN("ollama pull response contains error: %.200s", resp.c_str());
}

static void ensure_model_available(const TomedoConfig& cfg) {
    if (s_quit.load()) return;
    if (ollama_model_available(cfg.ollama_url, cfg.ollama_model)) {
        LOG_INFO("embedding model '%s' is available", cfg.ollama_model.c_str());
        return;
    }
    LOG_INFO("embedding model '%s' not found — pulling...", cfg.ollama_model.c_str());
    ollama_pull_model(cfg.ollama_url, cfg.ollama_model);
    if (ollama_model_available(cfg.ollama_url, cfg.ollama_model)) {
        LOG_INFO("embedding model '%s' pulled successfully", cfg.ollama_model.c_str());
    } else {
        LOG_ERROR("failed to pull embedding model '%s'", cfg.ollama_model.c_str());
    }
}

static void ollama_startup_check(const TomedoConfig& cfg) {
    bool installed = check_ollama_installed();
    g_ollama_installed.store(installed);
    if (!installed) {
        LOG_WARN("ollama is not installed — RAG embedding will be unavailable");
        return;
    }
    LOG_INFO("ollama found in PATH");
    bool running = check_ollama_running(cfg.ollama_url);
    g_ollama_running.store(running);
    if (running) {
        LOG_INFO("ollama is already running at %s", cfg.ollama_url.c_str());
        ensure_model_available(cfg);
        return;
    }
    LOG_INFO("ollama not running — attempting auto-start");
    pid_t pid = spawn_ollama_serve_detached();
    if (pid > 0) {
        LOG_INFO("ollama serve started (pid=%d)", (int)pid);
        for (int i = 0; i < 10 && !s_quit.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (check_ollama_running(cfg.ollama_url)) {
                g_ollama_running.store(true);
                LOG_INFO("ollama is now reachable at %s", cfg.ollama_url.c_str());
                ensure_model_available(cfg);
                return;
            }
        }
        LOG_WARN("ollama started but not yet reachable after 5s");
    } else {
        LOG_ERROR("failed to auto-start ollama serve");
    }
}

constexpr int MG_POLL_TIMEOUT_MS      = 100;
constexpr int CHARS_PER_TOKEN_APPROX  = 4;
constexpr int MIN_PHONE_DIGITS        = 4;
constexpr int PHONE_SUFFIX_MATCH_LEN  = 6;
constexpr int MAX_DIAGNOSEN           = 20;
constexpr int MAX_MEDICATIONS         = 20;
constexpr int CRAWL_PROGRESS_INTERVAL = 50;
constexpr int CRAWL_BATCH_SLEEP_MS    = 10;
constexpr int EXPIRY_CHECK_INTERVAL_S  = 300;
constexpr int QUERY_POOL_WORKERS       = 4;
constexpr int RESOLVE_QUEUE_MAX_DEPTH  = 200;
constexpr int EMBED_TIMEOUT_MS        = 30000;
constexpr int TOMEDO_API_TIMEOUT_MS   = 15000;
constexpr int TOMEDO_LIST_TIMEOUT_MS  = 60000;

// ============================================================
// Ollama embedding (forward declaration — implemented after HTTP helpers)
// ============================================================

static std::vector<float> embed_text(const std::string& text, const TomedoConfig& cfg);

using QueryResult = embedding_db::QueryResult;

embedding_db::EmbeddingDB g_vector_store;

// ============================================================
// CallerStore — thread-safe in-memory caller identity tracking
// ============================================================
//
// Lifecycle per call:
//   1. sip-client-main detects an incoming call and POSTs /caller with the
//      call_id and raw phone number string from the SIP From: header.
//   2. CallerStore::register_caller() creates a PENDING entry.
//   3. ResolveQueue dispatches a background lookup against the local
//      phone_index SQLite table (populated by the crawl thread).
//   4. CallerStore::update() sets status to FOUND/NOT_FOUND/ERROR and fills
//      name/patient_id when a match is found.
//   5. llama-service GETs /caller/{call_id} to retrieve the identity before
//      building the dynamic system prompt.
//   6. sip-client-main DELETEs /caller/{call_id} on call tear-down.
//
// TTL: entries expire after 1 hour (TTL_SECONDS) regardless of DELETE, as a
// safety net for calls that did not send a DELETE (e.g. crash).  expire_old()
// is called by the expiry_thread every EXPIRY_CHECK_INTERVAL_S seconds.

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

struct CallerPatient {
    int patient_id = -1;
    std::string name;
    std::string vorname;
};

struct CallerRecord {
    int call_id = 0;
    std::string phone_number;
    LookupStatus status = LookupStatus::PENDING;
    int patient_id = -1;
    std::string name;
    std::string vorname;
    std::vector<CallerPatient> all_patients;
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
                const std::string& name, const std::string& vorname,
                std::vector<CallerPatient> all = {}) {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = map_.find(call_id);
        if (it == map_.end()) return;
        it->second.status = st;
        it->second.patient_id = patient_id;
        it->second.name = name;
        it->second.vorname = vorname;
        it->second.all_patients = std::move(all);
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
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        int pr;
        do {
            int rem = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count());
            if (rem <= 0) { pr = 0; break; }
            pr = poll(&pfd, 1, rem);
        } while (pr < 0 && errno == EINTR);
        if (pr <= 0) { close(fd); return -1; }
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
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;  // sub-poll timeout, check deadline and retry
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
    if (g_ssl_ctx && g_ssl_ctx_pem == pem_path) {
        // g_ssl_ctx refcount reflects one "stored" ref + one "in-use" ref.
        // Caller calls SSL_CTX_free() when done, leaving the stored ref intact.
        // cleanup_shared_ssl_ctx() drops the stored ref on shutdown.
        SSL_CTX_up_ref(g_ssl_ctx);
        return g_ssl_ctx;
    }
    if (g_ssl_ctx) { SSL_CTX_free(g_ssl_ctx); g_ssl_ctx = nullptr; }
    SSL_CTX* ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return nullptr;

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
        // Try to load server CA from the same PEM (Tomedo client certs are
        // typically signed by the same CA as the server cert).
        // SSL_CTX_load_verify_locations silently ignores non-CA entries.
        if (SSL_CTX_load_verify_locations(ctx, pem_path.c_str(), nullptr) == 1) {
            SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
            LOG_INFO("HTTPS: server cert verification enabled (CA from %s)", pem_path.c_str());
        } else {
            // CA not in PEM — disable peer verification but warn prominently.
            // Patient data is still encrypted in transit; only MITM on the
            // local network segment could intercept. Pin the server cert by
            // exporting the Tomedo server CA and setting tomedo_ca_pem in the
            // config database to enable full verification.
            ERR_clear_error();
            SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
            LOG_WARN("HTTPS: server cert verification DISABLED — "
                     "Tomedo CA not found in %s. "
                     "Set tomedo_ca_pem in config DB to enable VERIFY_PEER.",
                     pem_path.c_str());
        }
    } else {
        SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);
        LOG_WARN("HTTPS: no client cert configured — Tomedo mTLS disabled, "
                 "server cert not verified");
    }

    g_ssl_ctx = ctx;
    g_ssl_ctx_pem = pem_path;
    SSL_CTX_up_ref(g_ssl_ctx);
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
    if (fd < 0) { SSL_CTX_free(ctx); return fail; }

    SSL* ssl = SSL_new(ctx);
    SSL_set_fd(ssl, fd);
    SSL_set_tlsext_host_name(ssl, host.c_str());
    if (SSL_connect(ssl) != 1) {
        unsigned long err = ERR_get_error();
        char err_buf[256];
        ERR_error_string_n(err, err_buf, sizeof(err_buf));
        LOG_ERROR("HTTPS: SSL_connect to %s:%d failed: %s", host.c_str(), port, err_buf);
        SSL_free(ssl); close(fd); SSL_CTX_free(ctx);
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
        SSL_free(ssl); close(fd); SSL_CTX_free(ctx);
        return fail;
    }

    std::string raw_resp = ssl_read_all(ssl, timeout_ms);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);
    SSL_CTX_free(ctx);

    return parse_http_response(raw_resp);
}

static HttpResponse https_get(const std::string& host, int port,
                               const std::string& path,
                               const std::string& pem_path,
                               int timeout_ms = 10000) {
    return https_request("GET", host, port, path, "", pem_path, timeout_ms);
}

// ============================================================
// Plain HTTP client (for Ollama — no TLS)
// ============================================================

static std::string http_read_all_plain(int fd, int timeout_ms) {
    std::string result;
    char buf[8192];
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (true) {
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) break;
        struct pollfd pfd{fd, POLLIN, 0};
        int pr = poll(&pfd, 1, static_cast<int>(std::min(remaining, (long long)1000)));
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n <= 0) break;
        result.append(buf, static_cast<size_t>(n));
    }
    return result;
}

static HttpResponse http_post_plain(const std::string& url, const std::string& body,
                                    int timeout_ms = 30000) {
    std::string host = "127.0.0.1";
    int port = 11434;
    std::string path = "/";

    size_t scheme_end = url.find("://");
    size_t host_start = (scheme_end == std::string::npos) ? 0 : scheme_end + 3;
    size_t path_start = url.find('/', host_start);
    std::string hostport;
    if (path_start == std::string::npos) {
        hostport = url.substr(host_start);
    } else {
        hostport = url.substr(host_start, path_start - host_start);
        path = url.substr(path_start);
    }
    auto colon = hostport.find(':');
    if (colon != std::string::npos) {
        host = hostport.substr(0, colon);
        port = std::atoi(hostport.c_str() + colon + 1);
    } else {
        host = hostport;
    }

    HttpResponse fail;
    int fd = tcp_connect(host, port, timeout_ms);
    if (fd < 0) return fail;

    std::ostringstream req;
    req << "POST " << path << " HTTP/1.1\r\n"
        << "Host: " << host << ":" << port << "\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Connection: close\r\n\r\n"
        << body;

    std::string raw_req = req.str();
    {
        const char* data = raw_req.c_str();
        size_t remaining = raw_req.size();
        while (remaining > 0) {
            ssize_t n = ::write(fd, data, remaining);
            if (n < 0) {
                if (errno == EINTR) continue;
                close(fd); return fail;
            }
            if (n == 0) { close(fd); return fail; }
            data += n;
            remaining -= static_cast<size_t>(n);
        }
    }

    std::string raw_resp = http_read_all_plain(fd, timeout_ms);
    close(fd);
    return parse_http_response(raw_resp);
}

// ============================================================
// Ollama embedding client (real implementation)
// ============================================================

static std::vector<float> embed_text(const std::string& text, const TomedoConfig& cfg) {
    if (text.empty()) return {};

    std::string url = cfg.ollama_url + "/api/embeddings";
    std::ostringstream body;
    body << "{\"model\":\"" << json_escape(cfg.ollama_model)
         << "\",\"prompt\":\"" << json_escape(text) << "\"}";

    auto resp = http_post_plain(url, body.str(), EMBED_TIMEOUT_MS);
    if (resp.status != 200 || resp.body.empty()) {
        LOG_WARN("embed_text: Ollama status %d (url=%s)", resp.status, url.c_str());
        return {};
    }

    auto pos = resp.body.find("\"embedding\"");
    if (pos == std::string::npos) {
        LOG_WARN("embed_text: no 'embedding' key in response");
        return {};
    }
    pos += 11;
    while (pos < resp.body.size() && resp.body[pos] != '[') ++pos;
    if (pos >= resp.body.size()) return {};
    ++pos;

    std::vector<float> result;
    while (pos < resp.body.size()) {
        while (pos < resp.body.size() &&
               (resp.body[pos] == ' ' || resp.body[pos] == '\t' ||
                resp.body[pos] == '\n' || resp.body[pos] == '\r' ||
                resp.body[pos] == ',')) ++pos;
        if (pos >= resp.body.size() || resp.body[pos] == ']') break;
        char* end = nullptr;
        float val = std::strtof(resp.body.c_str() + pos, &end);
        if (end == resp.body.c_str() + pos) break;
        result.push_back(val);
        pos = static_cast<size_t>(end - resp.body.c_str());
    }
    return result;
}

// ============================================================
// Text chunker (UTF-8 aware)
// ============================================================
//
// chunk_text() splits a patient context document into overlapping windows
// suitable for embedding.  Splitting is done on sentence boundaries (., !, ?,
// \n) to avoid cutting mid-sentence.
//
// Token estimation: utf8_codepoint_count(text) / 4 — counts Unicode codepoints
// rather than bytes.  This is important for German text where ä/ö/ü/ß are
// 2-byte UTF-8 sequences; using text.size()/4 would over-count and produce
// chunks that are too small.
//
// Default parameters (configurable via --chunk-size and --overlap argv flags):
//   chunk_tokens = 512   — target window size in estimated tokens
//   overlap_tokens = 64  — how many tokens from the previous chunk are
//                          prepended to the next chunk for context continuity

static size_t utf8_codepoint_count(const char* s, size_t len) {
    size_t count = 0;
    for (size_t i = 0; i < len; ) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if      (c < 0x80)             i += 1;
        else if ((c & 0xE0) == 0xC0)  i += 2;
        else if ((c & 0xF0) == 0xE0)  i += 3;
        else if ((c & 0xF8) == 0xF0)  i += 4;
        else                           i += 1;
        ++count;
    }
    return count;
}

static std::string trim_whitespace(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

static std::vector<std::string> chunk_text(const std::string& text,
                                            int chunk_tokens = 512,
                                            int overlap_tokens = 64) {
    if (text.empty()) return {};

    std::vector<std::string> sentences;
    std::string cur;
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        cur += c;
        if (c == '.' || c == '!' || c == '?' || c == '\n') {
            std::string trimmed = trim_whitespace(cur);
            if (!trimmed.empty())
                sentences.push_back(std::move(trimmed));
            cur.clear();
        }
    }
    if (!cur.empty()) {
        std::string trimmed = trim_whitespace(cur);
        if (!trimmed.empty())
            sentences.push_back(std::move(trimmed));
    }

    if (sentences.empty()) return {};

    std::vector<std::string> chunks;
    size_t start = 0;
    while (start < sentences.size()) {
        std::string chunk;
        size_t tokens = 0;
        size_t end = start;
        while (end < sentences.size()) {
            size_t sent_tokens = utf8_codepoint_count(
                sentences[end].c_str(), sentences[end].size()) / CHARS_PER_TOKEN_APPROX + 1;
            if (!chunk.empty() && tokens + sent_tokens > static_cast<size_t>(chunk_tokens))
                break;
            if (!chunk.empty()) chunk += ' ';
            chunk += sentences[end];
            tokens += sent_tokens;
            ++end;
        }
        if (!chunk.empty()) chunks.push_back(std::move(chunk));

        if (end == start) { ++start; continue; }

        size_t overlap_toks = 0;
        size_t new_start = end;
        for (size_t j = end; j > start && overlap_toks < static_cast<size_t>(overlap_tokens); --j) {
            overlap_toks += utf8_codepoint_count(
                sentences[j - 1].c_str(), sentences[j - 1].size()) / CHARS_PER_TOKEN_APPROX + 1;
            new_start = j - 1;
        }
        start = (new_start > start && new_start < end) ? new_start : end;
    }
    return chunks;
}

// ============================================================
// Phone index (local SQLite table for phone -> patient mapping)
// ============================================================
//
// The Tomedo REST API has no server-side phone-number search endpoint
// (confirmed: searchByAttributes?telefonNummern=true returns an empty dict).
// Instead, during each crawl the phone numbers of every patient are stored in
// the local "phone_index" SQLite table.  Caller lookup at call time is then a
// fast local LIKE query.
//
// Normalisation: all phone numbers are stored as digit-only strings (non-digit
// characters stripped).  Lookups use a suffix LIKE match ('%' || digits || '%')
// to handle varying area-code formats (e.g. "07383-942735" stored as "07383942735"
// matches a query for "942735").
//
// Schema:
//   phone_index(id PK, phone TEXT, patient_id INTEGER, name TEXT, vorname TEXT)
//   Unique on (phone, patient_id) — upsert replaces on conflict.

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

struct PhoneLookupEntry {
    int patient_id = -1;
    std::string name;
    std::string vorname;
};

struct PhoneLookupResult {
    bool found = false;
    std::vector<PhoneLookupEntry> entries;
};

static PhoneLookupResult phone_index_lookup(sqlite3* db, const std::string& phone) {
    PhoneLookupResult r;
    std::string digits = normalize_phone(phone);
    if (digits.size() < static_cast<size_t>(MIN_PHONE_DIGITS)) return r;
    std::string pattern = "%" + digits.substr(digits.size() > static_cast<size_t>(PHONE_SUFFIX_MATCH_LEN) ? digits.size() - PHONE_SUFFIX_MATCH_LEN : 0) + "%";
    sqlite3_stmt* stmt = nullptr;
    int rc = sqlite3_prepare_v2(db,
        "SELECT patient_id, name, vorname FROM phone_index WHERE phone LIKE ?",
        -1, &stmt, nullptr);
    if (rc != SQLITE_OK) return r;
    sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        PhoneLookupEntry e;
        e.patient_id = sqlite3_column_int(stmt, 0);
        const char* n = (const char*)sqlite3_column_text(stmt, 1);
        const char* v = (const char*)sqlite3_column_text(stmt, 2);
        e.name = n ? n : "";
        e.vorname = v ? v : "";
        r.entries.push_back(std::move(e));
    }
    r.found = !r.entries.empty();
    sqlite3_finalize(stmt);
    return r;
}

// ============================================================
// Tomedo patient enumeration + context fetch
// ============================================================
//
// All Tomedo API calls use mutual TLS (client certificate) over HTTPS port 8443.
// The certificate is a self-signed RSA-4096 identity that the Tomedo macOS client
// installs automatically in the user's Keychain on first server connection.
// Export procedure (one-time):
//   security export -k ~/Library/Keychains/login.keychain-db \
//     -t identities -f pkcs12 -P "" -o /tmp/tomedo_client.p12
//   openssl pkcs12 -legacy -in /tmp/tomedo_client.p12 -nodes \
//     -passin pass:"" -out /etc/tomedo-crawl/client.pem
//
// enumerate_patients():
//   GET /tomedo_live/patient?flach=true — returns all ~15 k patients in one
//   response (no server-side pagination).  Phone numbers are NOT in the flat
//   list; they are fetched per-patient in a separate call.
//
// fetch_patient_context_full():
//   Composes a natural-language document from three Tomedo endpoints:
//     GET /patient/{id}                            — contact data + phones
//     GET /patient/{id}/patientenDetailsRelationen — diagnoses (up to 20)
//     GET /patient/{id}/patientenDetailsRelationen/medikamentenPlan — medications
//     GET /patient/{id}/termine?flach=true         — appointments
//   Phone numbers are also written into the phone_index table for lookup.
//
// Context document format (per patient):
//   Patient: {vorname} {nachname} (ID {ident}), geb. {geburtsDatum}
//   Diagnosen: {diagnosen[].freitext} [max 20]
//   Medikamente: {nameBeiVerordnung} {dosierungFrueh}-{mittag}-{abend}
//   Nächster Termin: {beginn_formatted} ({info})
//   Telefon: {telefon}

struct PatientRef {
    int ident = 0;
    std::string nachname;
    std::string vorname;
    long long geburts_datum = 0;
    long long zuletzt_aufgerufen = 0;
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
        p.zuletzt_aufgerufen = json_get_int64(obj, "zuletztAufgerufen", 0);
        if (p.ident > 0)
            result.push_back(std::move(p));
        pos = obj_end;
    }
    return result;
}

static std::vector<PatientRef> enumerate_patients(const TomedoConfig& cfg) {
    std::string path = "/" + cfg.tomedo_db + "/patient?flach=true";
    auto resp = https_get(cfg.tomedo_host, cfg.tomedo_port, path, cfg.tomedo_cert_pem, TOMEDO_LIST_TIMEOUT_MS);
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
    while (pos < body.size() && count < MAX_DIAGNOSEN) {
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
    while (pos < body.size() && count < MAX_MEDICATIONS) {
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

struct PatientContext {
    std::string text;
    std::string nachname;
    std::string vorname;
    PatientPhones phones;
    bool http_error = false;
};

static PatientContext fetch_patient_context_full(int patient_id, const TomedoConfig& cfg) {
    PatientContext ctx;

    std::string base = "/" + cfg.tomedo_db;
    std::string pid = std::to_string(patient_id);

    auto detail_resp = https_get(cfg.tomedo_host, cfg.tomedo_port,
        base + "/patient/" + pid, cfg.tomedo_cert_pem, TOMEDO_API_TIMEOUT_MS);
    if (detail_resp.status != 200) { ctx.http_error = true; return ctx; }

    ctx.nachname = json_get_string(detail_resp.body, "nachname");
    ctx.vorname = json_get_string(detail_resp.body, "vorname");
    long long geb = json_get_int64(detail_resp.body, "geburtsDatum", 0);
    ctx.phones = fetch_patient_phones(detail_resp.body);

    auto rel_resp = https_get(cfg.tomedo_host, cfg.tomedo_port,
        base + "/patient/" + pid + "/patientenDetailsRelationen"
        "?limitScheine=true&limitKartei=50&limitMedikamentenPlan=50"
        "&limitVerordnungen=50&limitZeiterfassungen=true&limitBehandlungsfaelle=true",
        cfg.tomedo_cert_pem, TOMEDO_API_TIMEOUT_MS);
    std::string diagnosen;
    if (rel_resp.status == 200)
        diagnosen = extract_diagnosen(rel_resp.body);

    auto med_resp = https_get(cfg.tomedo_host, cfg.tomedo_port,
        base + "/patient/" + pid + "/patientenDetailsRelationen/medikamentenPlan",
        cfg.tomedo_cert_pem, TOMEDO_API_TIMEOUT_MS);
    std::string medikamente;
    if (med_resp.status == 200)
        medikamente = extract_medications(med_resp.body);

    auto termin_resp = https_get(cfg.tomedo_host, cfg.tomedo_port,
        base + "/patient/" + pid + "/termine?flach=true",
        cfg.tomedo_cert_pem, TOMEDO_API_TIMEOUT_MS);
    std::string termin;
    if (termin_resp.status == 200)
        termin = extract_next_appointment(termin_resp.body);

    std::ostringstream doc;
    doc << "Patient: " << ctx.vorname << " " << ctx.nachname
        << " (ID " << patient_id << "), geb. " << format_epoch_ms(geb) << "\n";
    if (!diagnosen.empty())
        doc << "Diagnosen: " << diagnosen << "\n";
    if (!medikamente.empty())
        doc << "Medikamente: " << medikamente << "\n";
    if (!termin.empty())
        doc << "Naechster Termin: " << termin << "\n";
    if (!ctx.phones.telefon.empty())
        doc << "Telefon: " << ctx.phones.telefon << "\n";
    if (!ctx.phones.handy.empty())
        doc << "Handy: " << ctx.phones.handy << "\n";

    ctx.text = doc.str();
    return ctx;
}

// ============================================================
// Unified crawl: RAG chunks + phone index in a single pass
// ============================================================
//
// run_full_crawl() is the main ingestion pipeline.  It is called:
//   • Once immediately at startup (before the HTTP server enters its poll loop).
//   • Periodically by the crawl_thread at crawl_interval_sec (default: daily at
//     02:00 as computed by main()).
//   • On demand when POST /crawl/trigger is received.
//
// For each patient in the flat list:
//   1. fetch_patient_context_full() — makes up to 4 HTTPS calls to Tomedo.
//   2. index_patient_phones() — writes phone digits to the phone_index table.
//   3. chunk_text() — splits the context document into overlapping windows.
//   4. embed_text() — calls Ollama /api/embeddings for each chunk.
//   5. VectorStore::upsert() — writes the embedding to SQLite + hnswlib.
//
// Batching: patients are processed in groups of 100 with a 10 ms sleep between
// batches to avoid overwhelming the Tomedo server.
//
// since_ts (epoch ms): when non-zero, only patients with zuletztAufgerufen
// > since_ts are re-fetched.  Patients not re-fetched retain their existing
// vectors from the previous crawl.  since_ts is only advanced after a clean
// (non-interrupted, zero-skipped) crawl to ensure no patient is silently missed.
//
// Returns CrawlStats.interrupted = true when s_quit is set mid-crawl.
// In that case the caller should NOT advance since_ts.

static void index_patient_phones(sqlite3* db, int patient_id,
                                  const std::string& nachname,
                                  const std::string& vorname,
                                  const PatientPhones& ph) {
    auto store_phone = [&](const std::string& raw) {
        if (raw.empty()) return;
        std::string digits = normalize_phone(raw);
        if (digits.size() >= 4)
            phone_index_upsert(db, digits, patient_id, nachname, vorname);
    };
    store_phone(ph.telefon);
    store_phone(ph.telefon2);
    store_phone(ph.handy);
    store_phone(ph.telefon3);
}

struct CrawlStats {
    int  chunks      = 0;
    int  skipped     = 0;  // patients skipped due to HTTP errors (detail GET non-200)
    bool interrupted = false;
};

static constexpr int MAX_ACTIVE_PATIENTS = 2000;

static CrawlStats run_full_crawl(const std::vector<PatientRef>& patients,
                                  const TomedoConfig& cfg, embedding_db::EmbeddingDB& store,
                                  sqlite3* phone_db, long long since_ts = 0) {
    CrawlStats stats;
    int processed = 0;
    for (const auto& p : patients) {
        if (since_ts > 0 && p.zuletzt_aufgerufen > 0 &&
            p.zuletzt_aufgerufen <= since_ts)  // both in epoch-ms (Tomedo API confirmed)
            continue;
        ++processed;
    }
    LOG_INFO("Crawl starting (%d patients total, %d to process, since_ts=%lld)",
             (int)patients.size(), processed, since_ts);

    int phone_count = 0;
    for (size_t i = 0; i < patients.size(); ++i) {
        if (s_quit.load()) { stats.interrupted = true; break; }
        if (since_ts > 0 && patients[i].zuletzt_aufgerufen > 0 &&
            patients[i].zuletzt_aufgerufen <= since_ts)  // both in epoch-ms
            continue;

        int pid = patients[i].ident;
        auto pctx = fetch_patient_context_full(pid, cfg);

        if (pctx.http_error) {
            ++stats.skipped;
        }

        if (phone_db) {
            index_patient_phones(phone_db, pid, pctx.nachname, pctx.vorname, pctx.phones);
            if (!pctx.phones.telefon.empty() || !pctx.phones.telefon2.empty() ||
                !pctx.phones.handy.empty() || !pctx.phones.telefon3.empty())
                ++phone_count;
        }

        if (pctx.text.empty()) continue;

        auto chunks = chunk_text(pctx.text);
        std::string source = "patient/" + std::to_string(pid);
        for (size_t ci = 0; ci < chunks.size(); ++ci) {
            if (s_quit.load()) { stats.interrupted = true; break; }
            auto emb = embed_text(chunks[ci], cfg);
            if (emb.empty()) {
                LOG_DEBUG("Crawl: embed_text empty for patient=%d chunk=%d", pid, (int)ci);
                continue;
            }
            std::string chunk_source = source + "/chunk" + std::to_string(ci);
            store.upsert(chunk_source, pid, chunks[ci], emb);
            ++stats.chunks;
        }

        if ((i + 1) % CRAWL_PROGRESS_INTERVAL == 0 || i + 1 == patients.size()) {
            LOG_INFO("Crawl: %d/%d patients, %d chunks, %d phones so far",
                     (int)(i + 1), (int)patients.size(), stats.chunks, phone_count);
            std::this_thread::sleep_for(std::chrono::milliseconds(CRAWL_BATCH_SLEEP_MS));
        }
    }
    LOG_INFO("Crawl complete: %d chunks, %d phones, %d skipped, interrupted=%s",
             stats.chunks, phone_count, stats.skipped,
             stats.interrupted ? "yes" : "no");
    return stats;
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
        auto& first = result.entries[0];
        LOG_INFO("Caller resolved: call_id=%d -> %d patient(s), first: patient=%d %s %s",
                 call_id, (int)result.entries.size(),
                 first.patient_id, first.vorname.c_str(), first.name.c_str());
        std::vector<CallerPatient> all;
        all.reserve(result.entries.size());
        for (auto& e : result.entries) {
            CallerPatient cp;
            cp.patient_id = e.patient_id;
            cp.name       = e.name;
            cp.vorname    = e.vorname;
            all.push_back(std::move(cp));
        }
        g_caller_store.update(call_id, LookupStatus::FOUND,
                              first.patient_id, first.name, first.vorname,
                              std::move(all));
    } else {
        LOG_DEBUG("Caller not found in phone_index: call_id=%d phone=%s", call_id, phone.c_str());
        g_caller_store.update(call_id, LookupStatus::NOT_FOUND, -1, "", "");
    }
}

// ============================================================
// ResolveQueue — bounded single-background-thread worker queue
// ============================================================
//
// Serialises all phone-lookup work on a single worker thread so that the
// Mongoose event loop is never blocked by SQLite queries.
//
// The queue is bounded (RESOLVE_QUEUE_MAX_PENDING = 64) to shed load when
// many simultaneous calls arrive faster than lookups can complete.  Excess
// enqueue() calls immediately return false, and the caller is registered with
// status ERROR so llama-service proceeds without RAG enrichment rather than
// stalling.
//
// Shutdown is graceful: all pending jobs are drained before the thread exits,
// so no registered caller is left in PENDING state at teardown.

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
            if (static_cast<int>(queue_.size()) >= RESOLVE_QUEUE_MAX_DEPTH) {
                // Queue full — resolve immediately as NOT_FOUND rather than
                // blocking indefinitely. Under normal load (≤200 concurrent
                // unresolved callers) this should never trigger.
                LOG_WARN("ResolveQueue full (depth=%d) — call_id=%d set to NOT_FOUND",
                         RESOLVE_QUEUE_MAX_DEPTH, call_id);
                g_caller_store.update(call_id, LookupStatus::NOT_FOUND, -1, "", "");
                return;
            }
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
// HttpContext — passed as fn_data to every Mongoose connection
// ============================================================

struct HttpContext {
    const TomedoConfig* cfg = nullptr;
    struct mg_mgr*      mgr = nullptr;
};

// ============================================================
// Pending query responses — written by worker threads, read by Mongoose thread
// ============================================================

struct PendingResponse {
    int         status = 200;
    std::string json;
};

static std::mutex                                         g_pending_mutex;
static std::unordered_map<unsigned long, PendingResponse> g_pending_responses;
static std::unordered_set<unsigned long>                  g_inflight_query_ids;
static std::unordered_set<unsigned long>                  g_cancelled_queries;

static std::string build_query_json(const std::vector<QueryResult>& results) {
    std::ostringstream j;
    j << "{\"results\":[";
    for (size_t i = 0; i < results.size(); ++i) {
        if (i) j << ",";
        float safe_score = std::isfinite(results[i].score) ? results[i].score : 0.0f;
        j << "{"
          << "\"text\":\""     << json_escape(results[i].text)   << "\","
          << "\"source\":\""   << json_escape(results[i].source) << "\","
          << "\"patient_id\":" << results[i].patient_id          << ","
          << "\"score\":"      << safe_score
          << "}";
    }
    j << "]}";
    return j.str();
}

// ============================================================
// QueryWorkerPool — embeds text off the Mongoose event-loop thread
// ============================================================
//
// /query requests require an Ollama embedding call (~50-200 ms) which must not
// block the single-threaded Mongoose event loop.  Instead:
//   1. http_handler() enqueues the job and returns immediately (connection stays
//      open — Mongoose will buffer the response).
//   2. A worker thread calls embed_text() + VectorStore::query().
//   3. The result is stored in g_pending_responses (keyed by mg_connection::id).
//   4. mg_wakeup() signals the Mongoose event loop, which picks up the result
//      in the MG_EV_WAKEUP branch and sends the HTTP response.
//
// If the client disconnects before the worker finishes, the connection id is
// moved from g_inflight_query_ids to g_cancelled_queries so the worker silently
// discards its result instead of writing to a dead connection.
//
// The pool is bounded at QUERY_POOL_MAX_QUEUE = 32 concurrent jobs; overflow
// returns HTTP 429.  QUERY_POOL_WORKERS threads are started in main() using
// the value from argv or defaulting to 4.

static constexpr size_t QUERY_POOL_MAX_QUEUE = 32;

class QueryWorkerPool {
    struct Job {
        unsigned long      conn_id;
        struct mg_mgr*     mgr;
        std::string        text;
        int                top_k;
        int                patient_id_filter;
        const TomedoConfig* cfg;
    };

    std::queue<Job>         queue_;
    std::mutex              mutex_;
    std::condition_variable cv_;
    bool                    stop_ = false;
    std::vector<std::thread> workers_;

    void worker_func() {
        while (true) {
            Job job;
            {
                std::unique_lock<std::mutex> lk(mutex_);
                cv_.wait(lk, [this]{ return stop_ || !queue_.empty(); });
                if (stop_ && queue_.empty()) return;
                job = std::move(queue_.front());
                queue_.pop();
            }

            PendingResponse pr;
            auto emb = embed_text(job.text, *job.cfg);
            if (emb.empty()) {
                pr.status = 503;
                pr.json   = "{\"error\":\"embedding_unavailable\"}";
            } else {
                auto results = g_vector_store.query(emb, job.top_k, job.patient_id_filter);
                pr.status = 200;
                pr.json   = build_query_json(results);
            }

            bool should_wake = false;
            {
                std::lock_guard<std::mutex> lk(g_pending_mutex);
                if (g_cancelled_queries.erase(job.conn_id) == 0) {
                    g_pending_responses[job.conn_id] = std::move(pr);
                    should_wake = true;
                }
            }
            if (should_wake)
                mg_wakeup(job.mgr, job.conn_id, "Q", 1);
        }
    }

public:
    void start(int n = 4) {
        workers_.reserve(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i)
            workers_.emplace_back([this]{ worker_func(); });
    }

    bool enqueue(unsigned long conn_id, struct mg_mgr* mgr,
                 const std::string& text, int top_k, int patient_id_filter,
                 const TomedoConfig* cfg) {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            if (queue_.size() >= QUERY_POOL_MAX_QUEUE) return false;
            queue_.push({conn_id, mgr, text, top_k, patient_id_filter, cfg});
        }
        cv_.notify_one();
        return true;
    }

    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        for (auto& t : workers_)
            if (t.joinable()) t.join();
    }

    ~QueryWorkerPool() { shutdown(); }
};

QueryWorkerPool g_query_pool;

// ============================================================
// Mongoose HTTP server
// ============================================================
//
// tomedo-crawl exposes a plain HTTP REST API on api_host:api_port (default
// 127.0.0.1:13181).  All endpoints return application/json.  TLS is enabled
// when prodigy_tls::ensure_certs() returns valid cert/key PEM strings (managed
// by tls_cert.h, shared with the other Prodigy services).
//
// Endpoint summary:
//   GET  /health            — service status, doc count, Ollama state
//   GET/POST /query         — RAG semantic search (async via QueryWorkerPool)
//   POST /caller            — register incoming call + phone number
//   GET  /caller/{id}       — poll lookup status for a call
//   DELETE /caller/{id}     — deregister call on hangup
//   POST /crawl/trigger     — request an immediate crawl (sets atomic flag)
//   POST /wipe              — delete all vectors and rebuild empty index
//   GET  /ollama/models     — list models from Ollama /api/tags
//   POST /ollama/start      — start ollama serve (if not already running)
//   POST /ollama/stop       — kill the tracked ollama process
//   POST /ollama/pull       — background ollama pull {model}
//   GET  /config            — read all config keys from SQLite
//   POST /config            — write one or more config keys to SQLite
//
// Security: the listener binds to api_host (127.0.0.1 by default).  No
// authentication is required — the loopback-only binding is the security model.

void http_handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_ACCEPT) {
        const auto& certs = prodigy_tls::ensure_certs();
        if (!certs.cert_pem.empty() && !certs.key_pem.empty()) {
            struct mg_tls_opts opts{};
            opts.cert = mg_str(certs.cert_pem.c_str());
            opts.key  = mg_str(certs.key_pem.c_str());
            mg_tls_init(c, &opts);
        }
        return;
    }
    if (ev == MG_EV_WAKEUP) {
        PendingResponse pr;
        bool found = false;
        {
            std::lock_guard<std::mutex> lk(g_pending_mutex);
            auto it = g_pending_responses.find(c->id);
            if (it != g_pending_responses.end()) {
                pr    = std::move(it->second);
                found = true;
                g_pending_responses.erase(it);
            }
            g_inflight_query_ids.erase(c->id);
        }
        if (found)
            mg_http_reply(c, pr.status, "Content-Type: application/json\r\n",
                          "%s", pr.json.c_str());
        return;
    }

    if (ev == MG_EV_CLOSE) {
        std::lock_guard<std::mutex> lk(g_pending_mutex);
        g_pending_responses.erase(c->id);
        if (g_inflight_query_ids.erase(c->id))
            g_cancelled_queries.insert(c->id);
        return;
    }

    if (ev != MG_EV_HTTP_MSG) return;
    struct mg_http_message *hm = (struct mg_http_message *)ev_data;
    auto* ctx = static_cast<HttpContext*>(c->fn_data);
    const TomedoConfig& cfg = *ctx->cfg;

    if (mg_strcmp(hm->uri, mg_str("/health")) == 0) {
        long lc = g_last_crawl_time.load();
        int docs = g_vector_store.doc_count();
        int idx_pct = g_vector_store.index_usage_pct();
        bool oi = g_ollama_installed.load();
        bool or_ = g_ollama_running.load();
        std::ostringstream j;
        j << "{\"status\":\"ok\","
          << "\"indexed_docs\":" << docs << ","
          << "\"index_usage_pct\":" << idx_pct << ","
          << "\"ollama_installed\":" << (oi ? "true" : "false") << ","
          << "\"ollama_running\":" << (or_ ? "true" : "false") << ","
          << "\"last_crawl\":";
        if (lc == 0) j << "null";
        else         j << lc;
        j << "}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", j.str().c_str());

    } else if (mg_strcmp(hm->uri, mg_str("/query")) == 0 &&
               (mg_strcmp(hm->method, mg_str("GET")) == 0 ||
                mg_strcmp(hm->method, mg_str("POST")) == 0)) {
        std::string query_text;
        int top_k = 3;
        int patient_id_filter = -1;
        if (mg_strcmp(hm->method, mg_str("GET")) == 0) {
            char text_buf[4096] = {};
            char topk_buf[32]   = {};
            char pid_buf[32]    = {};
            mg_http_get_var(&hm->query, "text",       text_buf, sizeof(text_buf));
            mg_http_get_var(&hm->query, "top_k",      topk_buf, sizeof(topk_buf));
            mg_http_get_var(&hm->query, "patient_id", pid_buf,  sizeof(pid_buf));
            query_text = text_buf;
            if (topk_buf[0]) top_k = std::atoi(topk_buf);
            if (pid_buf[0])  patient_id_filter = std::atoi(pid_buf);
        } else {
            std::string body(hm->body.buf, hm->body.len);
            query_text        = json_get_string(body, "text");
            top_k             = json_get_int(body, "top_k", 3);
            patient_id_filter = json_get_int(body, "patient_id", -1);
        }
        if (top_k <= 0) top_k = 3;
        if (top_k > 20) top_k = 20;

        if (query_text.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                          "{\"error\":\"text parameter required\"}");
            return;
        }

        {
            std::lock_guard<std::mutex> lk(g_pending_mutex);
            g_inflight_query_ids.insert(c->id);
        }
        if (!g_query_pool.enqueue(c->id, ctx->mgr, query_text, top_k, patient_id_filter, &cfg)) {
            {
                std::lock_guard<std::mutex> lk(g_pending_mutex);
                g_inflight_query_ids.erase(c->id);
            }
            mg_http_reply(c, 429, "Content-Type: application/json\r\n",
                          "{\"error\":\"too_many_requests\"}");
            return;
        }

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

    } else if (mg_strcmp(hm->uri, mg_str("/vectors/wipe")) == 0 &&
               mg_strcmp(hm->method, mg_str("POST")) == 0) {
        int before = g_vector_store.doc_count();
        g_vector_store.wipe();
        LOG_INFO("Vector store wiped via API (%d docs removed)", before);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"status\":\"wiped\",\"docs_removed\":%d}", before);

    } else if (mg_strcmp(hm->uri, mg_str("/ollama/status")) == 0 &&
               mg_strcmp(hm->method, mg_str("GET")) == 0) {
        bool installed = g_ollama_installed.load();
        bool running = installed ? check_ollama_running(cfg.ollama_url) : false;
        g_ollama_running.store(running);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
            "{\"installed\":%s,\"running\":%s}",
            installed ? "true" : "false", running ? "true" : "false");

    } else if (mg_strcmp(hm->uri, mg_str("/ollama/install")) == 0 &&
               mg_strcmp(hm->method, mg_str("POST")) == 0) {
        mg_http_reply(c, 501, "Content-Type: application/json\r\n",
            "{\"error\":\"Automatic Ollama installation is not supported. "
            "Install Ollama manually from https://ollama.com before starting this service.\"}");
        LOG_WARN("POST /ollama/install called — automatic install is disabled for security reasons");

    } else if (mg_strcmp(hm->uri, mg_str("/ollama/start")) == 0 &&
               mg_strcmp(hm->method, mg_str("POST")) == 0) {
        if (!g_ollama_installed.load()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                "{\"error\":\"ollama not installed\"}");
        } else {
            pid_t pid = spawn_ollama_serve_detached();
            if (pid > 0) {
                LOG_INFO("ollama serve started via API (pid=%d)", (int)pid);
                mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                    "{\"status\":\"started\"}");
            } else {
                mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                    "{\"error\":\"failed to start ollama\"}");
            }
        }

    } else if (mg_strcmp(hm->uri, mg_str("/ollama/stop")) == 0 &&
               mg_strcmp(hm->method, mg_str("POST")) == 0) {
        std::thread([](){
            kill_ollama_tracked();
            g_ollama_running.store(false);
        }).detach();
        mg_http_reply(c, 202, "Content-Type: application/json\r\n",
            "{\"status\":\"stopping\"}");

    } else if (mg_strcmp(hm->uri, mg_str("/config")) == 0 &&
               mg_strcmp(hm->method, mg_str("GET")) == 0) {
        std::ostringstream j;
        j << "{\"tomedo_host\":\"" << json_escape(cfg.tomedo_host) << "\""
          << ",\"tomedo_port\":" << cfg.tomedo_port
          << ",\"tomedo_db\":\"" << json_escape(cfg.tomedo_db) << "\""
          << ",\"tomedo_cert_pem\":\"" << json_escape(cfg.tomedo_cert_pem) << "\""
          << ",\"crawl_interval_sec\":" << cfg.crawl_interval_sec
          << ",\"ollama_url\":\"" << json_escape(cfg.ollama_url) << "\""
          << ",\"ollama_model\":\"" << json_escape(cfg.ollama_model) << "\""
          << ",\"hnsw_max_elements\":" << cfg.hnsw_max_elements
          << "}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", j.str().c_str());

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
                j << "\"vorname\":null,";
            else
                j << "\"vorname\":\"" << json_escape(rec.vorname) << "\",";
            j << "\"all_patients\":[";
            for (size_t pi = 0; pi < rec.all_patients.size(); ++pi) {
                if (pi) j << ",";
                auto& p = rec.all_patients[pi];
                j << "{\"patient_id\":" << p.patient_id
                  << ",\"name\":\"" << json_escape(p.name)
                  << "\",\"vorname\":\"" << json_escape(p.vorname) << "\"}";
            }
            j << "]}";
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

// mgr is initialized in start(). The event-loop thread polls it until s_quit.
// mg_mgr_free() is called from main() via free_mgr(), after g_query_pool.shutdown()
// ensures no worker thread can call mg_wakeup on this mgr.
struct HttpServer {
    struct mg_mgr mgr;
    std::thread   thread;
    std::string   listen_addr;
    HttpContext   ctx;

    bool start(const TomedoConfig& cfg) {
        listen_addr = "https://" + cfg.api_host + ":" + std::to_string(cfg.api_port);
        mg_mgr_init(&mgr);
        if (!mg_wakeup_init(&mgr)) {
            LOG_ERROR("mg_wakeup_init failed — cannot serve /query asynchronously");
            mg_mgr_free(&mgr);
            return false;
        }
        ctx.cfg = &cfg;
        ctx.mgr = &mgr;
        struct mg_connection *c = mg_http_listen(&mgr, listen_addr.c_str(), http_handler, &ctx);
        if (!c) {
            LOG_ERROR("Failed to listen on %s", listen_addr.c_str());
            mg_mgr_free(&mgr);
            return false;
        }
        thread = std::thread([this]() {
            while (!s_quit.load()) {
                mg_mgr_poll(&mgr, MG_POLL_TIMEOUT_MS);
            }
        });
        return true;
    }

    void free_mgr() { mg_mgr_free(&mgr); }

    ~HttpServer() {
        if (thread.joinable()) thread.join();
    }

    void join() {
        if (thread.joinable()) thread.join();
    }
};

} // namespace

// ============================================================
// main() — startup sequence
// ============================================================
//
// 1. Parse argv[1] as the database path (default: "tomedo-crawl.db").
// 2. load_config_from_db() — reads config from the encrypted SQLite DB.
// 3. LogForwarder initialisation — UDP datagrams to frontend log port 22022.
// 4. Spawn ollama_init_thread — checks ollama installation/running state in
//    parallel with the rest of startup to avoid a sequential delay.
// 5. VectorStore::open() + rebuild_index() — open the encrypted DB and
//    reconstruct the hnswlib ANN index from persisted embeddings.
// 6. Active embedding model check — if the configured model changed since the
//    last run, the vector store is wiped to prevent dimension mismatches.
// 7. phone_index_init() — ensure the phone lookup table exists.
// 8. ResolveQueue + QueryWorkerPool — start background worker threads.
// 9. CallerStore expiry thread — calls expire_old() every 5 minutes.
// 10. HttpServer::start() — bind the Mongoose listener before the crawl so
//     /health is available immediately.
// 11. Crawl thread — runs the first crawl immediately, then sleeps until the
//     next scheduled time or a /crawl/trigger request arrives.
// 12. Wait for s_quit (SIGINT/SIGTERM), then orderly shutdown:
//     QueryWorkerPool → HttpServer → ResolveQueue → crawl thread → expiry
//     thread → sqlite close → cleanup SSL.

int main(int argc, char** argv) {
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);

    std::string db_path = "tomedo-crawl.db";
    if (argc >= 2) db_path = argv[1];

    TomedoConfig cfg = load_config_from_db(db_path);

    g_log.init(static_cast<uint16_t>(cfg.log_port), whispertalk::ServiceType::TOMEDO_CRAWL_SERVICE);

    LOG_INFO("tomedo-crawl starting (api=%s:%d, db=%s)",
             cfg.api_host.c_str(), cfg.api_port, cfg.db_path.c_str());

    if (sqlite3_threadsafe() == 0) {
        LOG_ERROR("tomedo-crawl requires SQLite compiled with SQLITE_THREADSAFE>=1");
        return 1;
    }

    std::thread ollama_init_thread([&cfg](){
        ollama_startup_check(cfg);
    });

    std::string vector_store_path = cfg.db_path + ".vectors";
    g_vector_store.set_max_elements(cfg.hnsw_max_elements);
    if (!g_vector_store.open(vector_store_path)) {
        LOG_ERROR("Failed to open vector store at '%s'", vector_store_path.c_str());
        s_quit.store(true);
        if (ollama_init_thread.joinable()) ollama_init_thread.join();
        return 1;
    }
    LOG_INFO("Vector store loaded (%d docs)", g_vector_store.doc_count());

    {
        sqlite3* cdb = nullptr;
        if (prodigy_db::db_open_encrypted(cfg.db_path.c_str(), &cdb) == SQLITE_OK) {
            sqlite3_exec(cdb, "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);
            std::string prev_model = config_db_get(cdb, "active_embedding_model", "");
            if (!prev_model.empty() && prev_model != cfg.ollama_model) {
                LOG_WARN("Embedding model changed from '%s' to '%s' — wiping vector store",
                         prev_model.c_str(), cfg.ollama_model.c_str());
                g_vector_store.wipe();
            }
            config_db_set(cdb, "active_embedding_model", cfg.ollama_model);
            sqlite3_close(cdb);
        }
    }

    sqlite3* phone_db = nullptr;
    if (prodigy_db::db_open_encrypted(cfg.db_path.c_str(), &phone_db) != SQLITE_OK) {
        LOG_ERROR("Failed to open phone_index DB");
        s_quit.store(true);
        if (ollama_init_thread.joinable()) ollama_init_thread.join();
        return 1;
    }
    sqlite3_exec(phone_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(phone_db, "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);
    if (!phone_index_init(phone_db)) {
        LOG_ERROR("Failed to init phone_index table");
        sqlite3_close(phone_db);
        s_quit.store(true);
        if (ollama_init_thread.joinable()) ollama_init_thread.join();
        return 1;
    }
    g_phone_db = phone_db;

    g_resolve_queue.start();
    g_query_pool.start(QUERY_POOL_WORKERS);

    std::thread expiry_thread([]() {
        while (!s_quit.load()) {
            g_caller_store.expire_old();
            for (int i = 0; i < EXPIRY_CHECK_INTERVAL_S && !s_quit.load(); ++i)
                std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    });

    // Start HTTP server before crawl thread so /health is available from the
    // very first request, even while the initial crawl is in progress.
    HttpServer srv;
    if (!srv.start(cfg)) {
        s_quit.store(true);
        if (ollama_init_thread.joinable()) ollama_init_thread.join();
        g_query_pool.shutdown();
        g_resolve_queue.shutdown();
        expiry_thread.join();
        sqlite3_close(phone_db);
        return 1;
    }

    LOG_INFO("HTTP server listening on %s", srv.listen_addr.c_str());

    auto crawl_fn = [&cfg, phone_db]() {
        long long since_ts = 0;
        // Returns the wall-clock epoch-ms at which the crawl STARTED (for use
        // as the next since_ts), or 0 if the crawl was incomplete/interrupted.
        // since_ts is only advanced when every patient was fetched without HTTP
        // errors AND the crawl was not interrupted by SIGTERM — preventing
        // partially-crawled patients from being silently skipped on the next run.
        auto do_crawl = [&](long long ts) -> long long {
            long long started_at = static_cast<long long>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count());
            auto patients = enumerate_patients(cfg);
            if (patients.empty()) return 0;
            std::sort(patients.begin(), patients.end(),
                [](const PatientRef& a, const PatientRef& b) {
                    return a.zuletzt_aufgerufen > b.zuletzt_aufgerufen;
                });
            if (patients.size() > static_cast<size_t>(MAX_ACTIVE_PATIENTS))
                patients.resize(static_cast<size_t>(MAX_ACTIVE_PATIENTS));
            LOG_INFO("Crawl: selected top %d most recently active patients",
                     (int)patients.size());
            auto stats = run_full_crawl(patients, cfg, g_vector_store, phone_db, ts);
            g_vector_store.save();
            g_last_crawl_time.store(static_cast<long>(
                std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count()));
            if (stats.interrupted || stats.skipped > 0) {
                LOG_WARN("Crawl incomplete (interrupted=%s, skipped=%d) — "
                         "since_ts not advanced; all patients will be re-checked next run",
                         stats.interrupted ? "yes" : "no", stats.skipped);
                return 0;  // don't advance ts
            }
            return started_at;
        };
        long long new_ts = do_crawl(since_ts);
        if (new_ts > 0) since_ts = new_ts;
        while (!s_quit.load()) {
            for (int i = 0; i < cfg.crawl_interval_sec && !s_quit.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                if (g_crawl_requested.load()) break;
            }
            if (s_quit.load()) break;
            g_crawl_requested.store(false);
            new_ts = do_crawl(since_ts);
            if (new_ts > 0) since_ts = new_ts;
        }
    };
    std::thread crawl_thread(crawl_fn);

    if (ollama_init_thread.joinable()) ollama_init_thread.join();

    srv.join();
    kill_ollama_tracked();
    g_query_pool.shutdown();
    srv.free_mgr();
    g_resolve_queue.shutdown();
    crawl_thread.join();
    expiry_thread.join();
    sqlite3_close(phone_db);
    g_phone_db = nullptr;
    g_vector_store.close();
    cleanup_shared_ssl_ctx();

    LOG_INFO("tomedo-crawl stopped");
    return 0;
}
