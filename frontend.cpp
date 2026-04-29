// frontend.cpp — Web UI server, log aggregator, service manager, and test runner.
//
// The frontend is the central control plane for the Prodigy system. It:
//   - Serves a single-page web application (SPA) at http://127.0.0.1:8080/ (loopback only)
//   - Manages the lifecycle of all 7 pipeline services (start/stop/restart/config)
//   - Aggregates structured log entries from all services via UDP on port 22022
//   - Stores logs in SQLite and exposes them via REST API and SSE stream
//   - Provides test infrastructure for Whisper ASR accuracy and pipeline WER testing
//   - Provides the IAP audio quality test (offline G.711 codec round-trip check)
//
// HTTP API index (all endpoints on port 8080):
//
//   Service management:
//     GET  /api/services                    — list all managed services + status
//     POST /api/services/start              — start a service {name, args}
//     POST /api/services/stop               — stop a service {name}
//     POST /api/services/restart            — restart a service {name}
//     GET/POST /api/services/config         — read/write per-service config in SQLite
//
//   Logging:
//     GET  /api/logs                        — paginated log query {limit, offset, service, level}
//     GET  /api/logs/recent                 — last N log entries from in-memory ring buffer
//     GET  /api/logs/stream                 — Server-Sent Events (SSE) live log stream
//     POST /api/settings/log_level          — set per-service log level; propagates to running service
//
//   Database:
//     POST /api/db/query                    — execute arbitrary SELECT query (read-only guard)
//     POST /api/db/write_mode               — toggle write mode for unsafe queries
//     GET  /api/db/schema                   — return SQLite schema
//
//   Whisper / ASR:
//     GET  /api/whisper/models              — list available GGML model files in models/
//     POST /api/whisper/accuracy_test       — run offline Whisper accuracy test on a WAV file
//     POST /api/whisper/hallucination_filter — enable/disable hallucination filter on running service
//
//   VAD:
//     GET/POST /api/vad/config              — read/write VAD parameters; propagates to running service
//
//   SIP:
//     POST /api/sip/add-line                — register a new SIP account (calls ADD_LINE on service)
//     POST /api/sip/remove-line             — remove a SIP account
//     GET  /api/sip/lines                   — list registered SIP lines
//     GET  /api/sip/stats                   — RTP counters per active call
//
//   IAP:
//     POST /api/iap/quality_test            — offline G.711 round-trip codec quality test
//
//   Test files:
//     GET  /api/testfiles                   — list WAV+TXT sample pairs in Testfiles/
//     POST /api/testfiles/scan              — rescan Testfiles/ directory
//
//   Test infrastructure:
//     GET  /api/tests                       — list available test binaries
//     POST /api/tests/start                 — run a test binary
//     POST /api/tests/stop                  — kill a running test
//     GET  /api/tests/*/history             — test run history
//     GET  /api/tests/*/log                 — test stdout/stderr log
//     GET  /api/test_results                — pipeline WER test results from /tmp/pipeline_results_*.json
//
//   Dashboard / aggregated:
//     GET  /api/dashboard                   — service statuses, recent logs, test summary, uptime, pipeline topology
//     GET  /api/test_results_summary        — aggregated test results (service_test_runs, whisper_accuracy_tests, ...)
//
//   Misc:
//     GET  /api/status                      — system uptime, service health summary
//
// Log processing flow:
//   1. UDP recv on port 22022: run_log_server() reads 4096-byte datagrams.
//   2. process_log_message() parses "<SERVICE> <LEVEL> <CALL_ID> <message>".
//      Malformed datagrams are silently dropped (no crash).
//   3. LogEntry is enqueued to the async SQLite writer thread (enqueue_log()).
//   4. Writer thread batch-INSERTs into the `logs` table at high throughput.
//   5. In-memory ring buffer (recent_logs_, MAX_RECENT_LOGS entries) is updated.
//   6. SSE broadcast notifies all open /api/logs/stream connections.
//
// Service start / log-level persistence:
//   Service configs (args, log level) are stored in SQLite table `service_config`.
//   start_service() reads log_level_<NAME> from DB and appends --log-level to args.
//   handle_log_level_settings() writes to DB then sends SET_LOG_LEVEL:<LEVEL> to
//   the service's cmd port (if running) — no restart needed.
//
// LLaMA quality test (score_llama_response):
//   Scores generated responses by keyword match%, brevity (vs. max_words), and German
//   language detection. Used by the frontend test panel to evaluate LLM quality.
#include "interconnect.h"
#include "tts-common.h"
#include "mongoose.h"
#include "sqlite3.h"
#include "db_key.h"
#include "tls_cert.h"
#include "css.h"
#include "fonts.h"
#include "vendors.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <cmath>
#include <cstring>
#include <map>
#include <vector>
#include <deque>
#include <mutex>
#include <atomic>
#include <queue>
#include <ctime>
#include <chrono>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <dirent.h>
#include <algorithm>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <climits>
#include <regex>
#include <iomanip>
#include <unordered_set>
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

// Named constants — all timing, buffer, and limit values formerly scattered as
// magic numbers throughout the event loop, log infrastructure, and test runners.
// Units are indicated by the suffix: _MS (milliseconds), _S (seconds),
// _US (microseconds), _DAYS (days). Buffer/count limits have no time suffix.
static constexpr int LOG_FLUSH_INTERVAL_MS = 500;       // batch-INSERT cadence for log writer
static constexpr int UDP_BUFFER_SIZE = 4096;             // max datagram size for log receiver
static constexpr int DB_QUERY_ROW_LIMIT = 10000;         // max rows returned by /api/db/query
static constexpr int MG_POLL_TIMEOUT_MS = 100;           // mongoose event-loop poll timeout
static constexpr int LOG_RETENTION_DAYS = 30;             // log rotation: delete entries older than this
static constexpr int SERVICE_CHECK_INTERVAL_S = 2;       // how often to reap dead child processes
static constexpr int ASYNC_CLEANUP_INTERVAL_S = 30;      // how often to clean up finished async tasks
static constexpr int RECENT_LOGS_API_LIMIT = 100;        // /api/logs/recent returns at most this many
static constexpr int DASHBOARD_RECENT_LOGS_LIMIT = 10;   // /api/dashboard activity feed entry count
static constexpr useconds_t SIGTERM_GRACE_US = 500000;   // 500ms grace after SIGTERM before SIGKILL
static constexpr useconds_t SERVICE_STARTUP_WAIT_US = 200000;  // 200ms delay after killing ghosts
static constexpr useconds_t STOP_POLL_INTERVAL_US = 100000;    // 100ms between stop-poll iterations
static constexpr useconds_t SHUTDOWN_GRACE_US = 2000000;       // 2s shutdown grace period
static constexpr useconds_t RESTART_WAIT_US = 500000;          // 500ms wait between stop and start
static constexpr int TRANSCRIPTION_SETTLE_MS = 2000;     // settle time before reading transcription (reduced from 5s)
static constexpr int TRANSCRIPTION_POLL_MS = 100;        // poll interval for transcription log check (reduced from 150ms)
static constexpr int LLAMA_RESPONSE_POLL_MS = 100;       // poll interval for LLaMA response check (reduced from 200ms)
static constexpr int SHUTUP_INTER_ROUND_MS = 100;        // pause between shut-up test rounds
static constexpr int STRESS_POLL_MS = 100;               // poll interval in pipeline stress loop
static constexpr int PIPELINE_ROUND_POLL_MS = 250;       // poll interval for pipeline round-trip test (reduced from 500ms)
static constexpr int ACCURACY_INTER_FILE_MS = 2000;      // pause between accuracy test files
static constexpr int DOWNLOAD_PROGRESS_POLL_MS = 500;    // poll interval for download progress

// TTS engine cmd-ports (engines dock into TTS_SERVICE but keep a private
// diagnostic cmd-port for quality tests / benchmarks). Single source of
// truth is `whispertalk::tts::kKokoroEngineCmdPort` /
// `kNeuTTSEngineCmdPort` in `tts-common.h` — shared with the engine
// processes so these values cannot drift.
static constexpr uint16_t KOKORO_ENGINE_CMD_PORT = whispertalk::tts::kKokoroEngineCmdPort;
static constexpr uint16_t NEUTTS_ENGINE_CMD_PORT = whispertalk::tts::kNeuTTSEngineCmdPort;

using namespace whispertalk;

static std::atomic<bool> s_sigint_received(false);
static SSL_CTX* s_rag_ssl_ctx = nullptr;
static void cleanup_rag_ssl_ctx() {
    if (s_rag_ssl_ctx) { SSL_CTX_free(s_rag_ssl_ctx); s_rag_ssl_ctx = nullptr; }
}
static void signal_handler(int signal) {
    if (signal == SIGINT || signal == SIGTERM) {
        s_sigint_received = true;
    }
}

struct LogEntry {
    std::string timestamp;
    ServiceType service;
    uint32_t call_id;
    std::string level;
    std::string message;
    uint64_t seq = 0;
};

struct TestInfo {
    std::string name;
    std::string binary_path;
    std::string description;
    std::vector<std::string> default_args;
    bool is_running;
    pid_t pid;
    std::string log_file;
    time_t start_time;
    time_t end_time;
    int exit_code;
};

struct ServiceInfo {
    std::string name;
    std::string binary_path;
    std::string default_args;
    std::string description;
    bool managed;
    pid_t pid;
    std::string log_file;
    time_t start_time;
};

struct TestFileInfo {
    std::string name;
    size_t size_bytes;
    double duration_sec;
    uint32_t sample_rate;
    uint16_t channels;
    std::string ground_truth;
    time_t last_modified;
};

static std::string escape_json(const std::string& s) {
    std::string result;
    for (char c : s) {
        switch (c) {
            case '"':  result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\b': result += "\\b"; break;
            case '\f': result += "\\f"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 32) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(c));
                    result += buf;
                } else {
                    result += c;
                }
        }
    }
    return result;
}

static bool contains_whole_word(const std::string& text, const std::string& word) {
    size_t pos = 0;
    while ((pos = text.find(word, pos)) != std::string::npos) {
        bool pre_ok  = (pos == 0 || !isalpha((unsigned char)text[pos - 1]));
        bool post_ok = (pos + word.size() >= text.size() ||
                        !isalpha((unsigned char)text[pos + word.size()]));
        if (pre_ok && post_ok) return true;
        pos++;
    }
    return false;
}

static bool detect_german(const std::string& text) {
    static const char* word_markers[] = {
        "ich", "der", "die", "das", "ist", "ein", "und",
        "den", "dem", "des", "von", "als", "auch", "nicht",
        "sich", "wie", "kann", "gerne", "bitte", "danke", "nein",
        "es", "zu", "er", "sie", "wir", "ihr",
        "mir", "dir", "uns", "ihm", "mich", "dich",
        "sind", "oder", "aber", "haben", "werden",
        "schon", "sehr", "noch", "hier", "dort", "diese",
        "einen", "einer", "gute", "guten"
    };
    static const char* substr_markers[] = {"ü", "ö", "ä", "ß"};
    std::string lower = text;
    for (auto& ch : lower) ch = tolower((unsigned char)ch);
    int hits = 0;
    for (const auto* m : word_markers) {
        if (contains_whole_word(lower, m)) hits++;
    }
    for (const auto* m : substr_markers) {
        if (lower.find(m) != std::string::npos) hits++;
    }
    int word_count = 0;
    bool in_w = false;
    for (char ch : text) {
        if (ch == ' ' || ch == '\n' || ch == '\t') in_w = false;
        else if (!in_w) { in_w = true; word_count++; }
    }
    if (word_count <= 3) return hits >= 1;
    return hits >= 2;
}

static int count_words(const std::string& text) {
    int count = 0;
    bool in_word = false;
    for (char ch : text) {
        if (ch == ' ' || ch == '\n' || ch == '\t') { in_word = false; }
        else if (!in_word) { in_word = true; count++; }
    }
    return count;
}

static int count_keyword_matches(const std::string& text, const std::vector<std::string>& keywords) {
    std::string lower = text;
    for (auto& ch : lower) ch = tolower((unsigned char)ch);
    int found = 0;
    for (const auto& kw : keywords) {
        std::string lk = kw;
        for (auto& ch : lk) ch = tolower((unsigned char)ch);
        if (lower.find(lk) != std::string::npos) found++;
    }
    return found;
}

struct LlamaScoreResult {
    double score;
    int word_count;
    int keywords_found;
    bool is_german;
};

// Scores a LLaMA response on three axes, returning a weighted composite 0-100:
//   1. Keyword coverage (40%): % of expected keywords found (case-insensitive substring).
//   2. Brevity (30%): 100 if word_count ≤ max_words, else penalized −5 per excess word (floor 0).
//   3. German language (30%): binary — 100 if German detected, 0 otherwise.
// Used by the LLaMA quality test panel to evaluate response quality without human review.
static LlamaScoreResult score_llama_response(const std::string& response,
        const std::vector<std::string>& keywords, int max_words) {
    LlamaScoreResult r;
    r.word_count = count_words(response);
    r.keywords_found = count_keyword_matches(response, keywords);
    r.is_german = detect_german(response);
    double kw_pct = keywords.empty() ? 100.0 : (r.keywords_found * 100.0 / keywords.size());
    // Brevity penalty: −5 points per word over max_words, clamped to [0, 100].
    double brevity = (r.word_count <= max_words) ? 100.0 :
        std::max(0.0, 100.0 - (r.word_count - max_words) * 5.0);
    double german = r.is_german ? 100.0 : 0.0;
    // Weighted composite: keyword relevance (40%) + brevity (30%) + language (30%).
    r.score = kw_pct * 0.4 + brevity * 0.3 + german * 0.3;
    return r;
}

class FrontendServer {
public:
    FrontendServer(uint16_t http_port, const std::string& project_root) 
        : http_port_(http_port),
          log_port_(0),
          interconnect_(ServiceType::FRONTEND),
          db_(nullptr),
          db_ok_(false),
          project_root_(project_root),
          db_path_(project_root + "/frontend.db"),
          rag_db_path_(project_root + "/tomedo-crawl.db") {
        
        db_ok_ = init_database();
        if (db_ok_) {
            discover_tests();
            load_services();
            scan_testfiles_directory();
            check_and_refresh_cert();
        }
    }

    ~FrontendServer() {
        cleanup_rag_ssl_ctx();
        if (db_) {
            sqlite3_close(db_);
        }
    }

    bool validate_schema();

    bool start() {
        if (!db_ok_) {
            std::cerr << "ERROR: Database initialization failed. Cannot start frontend server.\n";
            std::cerr << "Check that the database path is writable: " << db_path_ << "\n";
            return false;
        }

        if (!interconnect_.initialize()) {
            std::cerr << "Failed to initialize interconnect\n";
            return false;
        }

        log_port_ = whispertalk::FRONTEND_LOG_PORT;

        int probe = socket(AF_INET, SOCK_DGRAM, 0);
        if (probe >= 0) {
            struct sockaddr_in pa{};
            pa.sin_family = AF_INET;
            pa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            pa.sin_port = htons(log_port_);
            if (bind(probe, (struct sockaddr*)&pa, sizeof(pa)) < 0) {
                std::cerr << "FATAL: Log port " << log_port_ << " is already in use (another frontend running?)\n";
                close(probe);
                interconnect_.shutdown();
                return false;
            }
            close(probe);
        }

        std::cout << "Frontend logging port: " << log_port_ << "\n";
        std::cout << "Frontend HTTP port: " << http_port_ << "\n";

        log_thread_ = std::thread(&FrontendServer::log_receiver_loop, this);

        mg_mgr_init(&mgr_);
        
        // Security: binds to loopback only. TLS enabled via self-signed cert (tls_cert.h).
        std::string listen_addr = "https://127.0.0.1:" + std::to_string(http_port_);
        struct mg_connection *c = mg_http_listen(&mgr_, listen_addr.c_str(), http_handler_static, this);
        if (c) c->fn_data = this;
        
        // Also listen on plain HTTP (http_port + 1) for tools/browsers that reject self-signed certs.
        uint16_t http_plain_port = http_port_ + 1;
        std::string http_listen = "http://127.0.0.1:" + std::to_string(http_plain_port);
        struct mg_connection *ch = mg_http_listen(&mgr_, http_listen.c_str(), http_handler_plain_static, this);
        if (ch) ch->fn_data = this;

        std::cout << "Frontend web server started on " << listen_addr << "\n";
        std::cout << "Also listening on " << http_listen << " (plain HTTP, loopback only)\n";
        std::cout << "Open https://localhost:" << http_port_ << " in your browser\n";

        {
            std::string active_cert = get_setting("active_cert_name", "");
            std::string active_key  = get_setting("active_key_name",  "");
            std::string dir = prodigy_tls::tls_dir();
            if (!active_cert.empty() && !active_key.empty()) {
                prodigy_tls::reload_certs(dir + "/" + active_cert, dir + "/" + active_key);
            } else {
                prodigy_tls::ensure_certs();
            }
        }

        auto last_flush = std::chrono::steady_clock::now();
        auto last_rotation = last_flush;
        auto last_svc_check = std::chrono::steady_clock::now();
        auto last_async_cleanup = std::chrono::steady_clock::now();
        auto last_cert_check = std::chrono::steady_clock::now();
        auto last_session_cleanup = std::chrono::steady_clock::now();
        while (!s_sigint_received) {
            mg_mgr_poll(&mgr_, MG_POLL_TIMEOUT_MS);
            check_test_status();
            flush_sse_queue();

            auto now = std::chrono::steady_clock::now();
            if (now - last_flush >= std::chrono::milliseconds(LOG_FLUSH_INTERVAL_MS)) {
                flush_log_queue();
                last_flush = now;
            }
            if (now - last_svc_check >= std::chrono::seconds(SERVICE_CHECK_INTERVAL_S)) {
                check_service_status();
                last_svc_check = now;
            }
            if (now - last_async_cleanup >= std::chrono::seconds(ASYNC_CLEANUP_INTERVAL_S)) {
                cleanup_old_async_tasks();
                last_async_cleanup = now;
            }
            if (now - last_rotation >= std::chrono::hours(1)) {
                rotate_logs();
                last_rotation = now;
            }
            if (now - last_cert_check >= std::chrono::hours(24)) {
                check_and_refresh_cert();
                last_cert_check = now;
            }
            if (now - last_session_cleanup >= std::chrono::hours(1)) {
                cleanup_expired_sessions();
                last_session_cleanup = now;
            }
        }
        flush_log_queue();
        shutdown_managed_processes();

        mg_mgr_free(&mgr_);
        interconnect_.shutdown();
        
        if (log_thread_.joinable()) {
            log_thread_.join();
        }

        return true;
    }

private:
    uint16_t http_port_;
    uint16_t log_port_;
    InterconnectNode interconnect_;
    sqlite3* db_;
    bool db_ok_ = false;
    bool db_write_mode_ = false;
    std::string project_root_;
    std::string db_path_;
    std::string rag_db_path_;
    std::atomic<pid_t> ollama_pid_{0};
    std::atomic<bool> ollama_pulling_{false};
    struct mg_mgr mgr_;
    std::thread log_thread_;
    
    std::mutex tests_mutex_;
    std::vector<TestInfo> tests_;

    std::mutex services_mutex_;
    std::vector<ServiceInfo> services_;

    std::mutex testfiles_mutex_;
    std::vector<TestFileInfo> testfiles_;
    
    std::mutex logs_mutex_;
    std::deque<LogEntry> recent_logs_;
    std::atomic<uint64_t> log_seq_{0};
    static constexpr size_t MAX_RECENT_LOGS = 1000;
    static constexpr int TEST_SIP_PROVIDER_PORT = 22011;

    std::mutex sse_mutex_;
    std::vector<struct mg_connection*> sse_connections_;
    static constexpr size_t MAX_SSE_CONNECTIONS = 20;

    std::mutex sse_queue_mutex_;
    std::vector<LogEntry> sse_queue_;

    std::chrono::steady_clock::time_point start_time_ = std::chrono::steady_clock::now();

    struct AsyncTask {
        int64_t id;
        std::string type;
        std::atomic<bool> running{true};
        std::atomic<bool> result_read{false};
        std::string result_json;
        std::string progress_json;
        std::thread worker;
    };
    std::mutex async_mutex_;
    std::map<int64_t, std::shared_ptr<AsyncTask>> async_tasks_;
    std::atomic<int64_t> async_id_counter_{0};

    struct DownloadProgress {
        std::atomic<int64_t> bytes_downloaded{0};
        std::atomic<int64_t> total_bytes{0};
        std::atomic<bool> complete{false};
        std::atomic<bool> failed{false};
        std::string error;
        std::string filename;
        std::string local_path;
        std::string service;
        std::mutex mu;
    };
    std::mutex downloads_mutex_;
    std::map<int64_t, std::shared_ptr<DownloadProgress>> downloads_;

    struct PipelineStressProgress {
        std::atomic<bool> running{true};
        std::atomic<bool> stop_requested{false};
        std::atomic<int> elapsed_s{0};
        std::atomic<int> duration_s{120};
        std::atomic<int> cycles_completed{0};
        std::atomic<int> cycles_ok{0};
        std::atomic<int> cycles_fail{0};
        std::atomic<int> total_latency_ms{0};
        std::atomic<int> min_latency_ms{999999};
        std::atomic<int> max_latency_ms{0};
        struct SvcSnap {
            std::atomic<int> memory_mb{0};
            std::atomic<bool> reachable{true};
            std::atomic<int> ping_ok{0};
            std::atomic<int> ping_fail{0};
            std::atomic<int> total_ping_ms{0};
        };
        SvcSnap svcs[7];
        std::mutex result_mutex;
        std::string result_json;
    };
    std::shared_ptr<PipelineStressProgress> pipeline_stress_;
    std::mutex pipeline_stress_mutex_;

    struct FailedAttempts {
        int count = 0;
        time_t window_start = 0;
    };
    std::map<std::string, FailedAttempts> login_failures_;

    int64_t create_async_task(const std::string& type) {
        int64_t id = ++async_id_counter_;
        auto task = std::make_shared<AsyncTask>();
        task->id = id;
        task->type = type;
        std::lock_guard<std::mutex> lock(async_mutex_);
        async_tasks_[id] = task;
        return id;
    }

    void update_async_task_progress(int64_t id, const std::string& progress) {
        std::lock_guard<std::mutex> lock(async_mutex_);
        auto it = async_tasks_.find(id);
        if (it != async_tasks_.end()) {
            it->second->progress_json = progress;
        }
    }

    void finish_async_task(int64_t id, const std::string& result) {
        std::lock_guard<std::mutex> lock(async_mutex_);
        auto it = async_tasks_.find(id);
        if (it != async_tasks_.end()) {
            it->second->result_json = result;
            it->second->running = false;
        }
    }

    void cleanup_old_async_tasks() {
        std::lock_guard<std::mutex> lock(async_mutex_);
        for (auto it = async_tasks_.begin(); it != async_tasks_.end(); ) {
            if (!it->second->running && it->second->result_read) {
                if (it->second->worker.joinable()) it->second->worker.join();
                it = async_tasks_.erase(it);
            } else {
                ++it;
            }
        }
    }

    bool init_database();

    void discover_tests() {
        std::lock_guard<std::mutex> lock(tests_mutex_);
        
        std::vector<std::pair<std::string, std::string>> test_files = {
            {"test_sanity", "bin/test_sanity"},
            {"test_interconnect", "bin/test_interconnect"},
            {"test_sip_provider_unit", "bin/test_sip_provider_unit"},
            {"test_kokoro_cpp", "bin/test_kokoro_cpp"},
            {"test_integration", "bin/test_integration"},
            {"test_sip_provider", "bin/test_sip_provider"},
        };

        for (const auto& [name, path] : test_files) {
            struct stat st;
            if (stat(path.c_str(), &st) == 0 && (st.st_mode & S_IXUSR)) {
                TestInfo info;
                info.name = name;
                info.binary_path = path;
                info.is_running = false;
                info.pid = 0;
                info.start_time = 0;
                info.end_time = 0;
                info.exit_code = -1;
                
                if (name == "test_integration") {
                    info.description = "Full pipeline integration test with real services";
                } else if (name == "test_interconnect") {
                    info.description = "Interconnect protocol tests (master/slave, heartbeat, crash recovery)";
                } else if (name == "test_sip_provider") {
                    info.description = "SIP B2BUA test provider";
                    info.default_args = {"--port", "5060", "--http-port", std::to_string(TEST_SIP_PROVIDER_PORT), "--testfiles-dir", "Testfiles"};
                } else if (name == "test_kokoro_cpp") {
                    info.description = "Kokoro TTS C++ tests (phonemization, CoreML inference)";
                } else {
                    info.description = "Unit test: " + name;
                }
                
                tests_.push_back(info);
            }
        }

        std::cout << "Discovered " << tests_.size() << " tests\n";
    }

    void check_test_status() {
        std::lock_guard<std::mutex> lock(tests_mutex_);
        for (auto& test : tests_) {
            if (test.is_running && test.pid > 0) {
                int status;
                pid_t result = waitpid(test.pid, &status, WNOHANG);
                if (result == test.pid) {
                    test.is_running = false;
                    test.end_time = time(nullptr);
                    if (WIFEXITED(status)) {
                        test.exit_code = WEXITSTATUS(status);
                    } else {
                        test.exit_code = -1;
                    }
                    save_test_run(test);
                }
            }
        }
    }

    void load_services() {
        std::lock_guard<std::mutex> lock(services_mutex_);
        if (!db_) return;

        sqlite3_stmt* stmt;
        const char* sql = "SELECT service, binary_path, default_args, description FROM service_config";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return;

        while (sqlite3_step(stmt) == SQLITE_ROW) {
            ServiceInfo svc;
            const char* name_col = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* path_col = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (!name_col || !path_col) continue;
            svc.name = name_col;
            svc.binary_path = path_col;
            const char* args = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            svc.default_args = args ? args : "";
            const char* desc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            svc.description = desc ? desc : "";
            svc.managed = false;
            svc.pid = 0;
            svc.start_time = 0;
            services_.push_back(svc);
        }
        sqlite3_finalize(stmt);
        std::cout << "Loaded " << services_.size() << " service configs\n";
    }

    void check_service_status() {
        std::lock_guard<std::mutex> lock(services_mutex_);
        for (auto& svc : services_) {
            if (svc.managed && svc.pid > 0) {
                int status;
                pid_t result = waitpid(svc.pid, &status, WNOHANG);
                if (result == svc.pid) {
                    svc.managed = false;
                    svc.pid = 0;
                }
            }
        }
    }

    static std::vector<std::string> split_args(const std::string& s) {
        std::vector<std::string> result;
        std::istringstream iss(s);
        std::string token;
        while (iss >> token) {
            result.push_back(token);
        }
        return result;
    }

    static bool is_allowed_binary(const std::string& path) {
        if (path.empty()) return false;
        if (path.find("..") != std::string::npos) return false;
        if (path[0] == '/') return false;
        if (path.substr(0, 4) != "bin/") return false;
        struct stat st;
        if (stat(path.c_str(), &st) != 0) return false;
        if (!S_ISREG(st.st_mode)) return false;
        if (!(st.st_mode & S_IXUSR)) return false;
        return true;
    }

    // kill_ghost_processes() — SIGTERM then SIGKILL any existing processes matching
    // binary_name. Input sanitized: only bare names matching [a-zA-Z0-9_.-] are
    // accepted (no paths, no shell metacharacters) to prevent popen() command injection.
    void kill_ghost_processes(const std::string& binary_name) {
        static const std::regex valid_name("^[a-zA-Z0-9_.-]+$");
        if (!std::regex_match(binary_name, valid_name)) return;
        std::string escaped_name;
        for (char ch : binary_name) {
            if (ch == '.') escaped_name += "[.]";
            else escaped_name += ch;
        }
        std::string cmd = "pgrep -f '" + escaped_name + "' 2>/dev/null";
        FILE* fp = popen(cmd.c_str(), "r");
        if (!fp) return;
        char buf[64];
        std::vector<pid_t> pids;
        while (fgets(buf, sizeof(buf), fp)) {
            pid_t p = atoi(buf);
            if (p > 0 && p != getpid()) pids.push_back(p);
        }
        pclose(fp);
        for (pid_t p : pids) {
            std::cerr << "Killing ghost process " << p << " for " << binary_name << "\n";
            kill(p, SIGTERM);
        }
        if (!pids.empty()) usleep(SIGTERM_GRACE_US);
        for (pid_t p : pids) {
            if (kill(p, 0) == 0) {
                kill(p, SIGKILL);
                waitpid(p, nullptr, WNOHANG);
            }
        }
    }

    bool start_service(const std::string& name, const std::string& args_override) {
        std::lock_guard<std::mutex> lock(services_mutex_);
        for (auto& svc : services_) {
            if (svc.name != name) continue;
            if (svc.managed && svc.pid > 0) return false;

            {
                std::string bin_name = svc.binary_path;
                size_t slash = bin_name.rfind('/');
                if (slash != std::string::npos) bin_name = bin_name.substr(slash + 1);
                kill_ghost_processes(bin_name);
                usleep(SERVICE_STARTUP_WAIT_US);
            }

            if (!is_allowed_binary(svc.binary_path)) return false;

            std::string use_args = args_override.empty() ? svc.default_args : args_override;

            auto is_numeric = [](const std::string& s) {
                if (s.empty()) return false;
                bool has_dot = false;
                for (size_t i = 0; i < s.size(); i++) {
                    if (s[i] == '.' && !has_dot) { has_dot = true; continue; }
                    if (!isdigit(s[i])) return false;
                }
                return true;
            };

            if (name == "WHISPER_SERVICE" && args_override.empty()) {
                std::string lang = get_setting("pipeline_language", "");
                if (!lang.empty()) {
                    auto valid_lang = [](const std::string& s) {
                        if (s.size() < 2 || s.size() > 5) return false;
                        for (char ch : s) {
                            if (!(isalpha((unsigned char)ch) || ch == '-')) return false;
                        }
                        return true;
                    };
                    if (valid_lang(lang)) {
                        size_t lpos = use_args.find("--language");
                        if (lpos != std::string::npos) {
                            size_t end = use_args.find(' ', lpos);
                            if (end == std::string::npos) {
                                use_args.erase(lpos);
                            } else {
                                size_t arg_end = use_args.find_first_not_of(' ', end);
                                if (arg_end == std::string::npos) {
                                    use_args.erase(lpos);
                                } else {
                                    size_t val_end = use_args.find(' ', arg_end);
                                    use_args.erase(lpos, (val_end == std::string::npos ? use_args.size() : val_end) - lpos);
                                }
                            }
                        }
                        if (!use_args.empty() && use_args.back() != ' ') use_args += " ";
                        use_args += "--language " + lang;
                    }
                }
            }

            if (name == "VAD_SERVICE" && args_override.empty()) {
                std::string vad_w = get_setting("vad_window_ms", "");
                std::string vad_t = get_setting("vad_threshold", "");
                std::string vad_s = get_setting("vad_silence_ms", "");
                std::string vad_c = get_setting("vad_max_chunk_ms", "");
                std::string vad_g = get_setting("vad_onset_gap", "");
                std::string vad_pic = get_setting("vad_post_idle_cooldown_ms", "");
                std::string vad_rg = get_setting("vad_rms_gate", "");
                if (!vad_w.empty() && is_numeric(vad_w)) use_args += " --vad-window-ms " + vad_w;
                if (!vad_t.empty() && is_numeric(vad_t)) use_args += " --vad-threshold " + vad_t;
                if (!vad_s.empty() && is_numeric(vad_s)) use_args += " --vad-silence-ms " + vad_s;
                if (!vad_c.empty() && is_numeric(vad_c)) use_args += " --vad-max-chunk-ms " + vad_c;
                if (!vad_g.empty() && is_numeric(vad_g)) use_args += " --vad-onset-gap " + vad_g;
                if (!vad_pic.empty() && is_numeric(vad_pic)) use_args += " --post-idle-cooldown-ms " + vad_pic;
                if (!vad_rg.empty() && is_numeric(vad_rg)) use_args += " --rms-gate " + vad_rg;
            }

            if (args_override.empty()) {
                std::string ll_key = "log_level_" + name;
                std::string ll = get_setting(ll_key, "");
                if (!ll.empty() && ll.find(' ') == std::string::npos) use_args += " --log-level " + ll;
            }

            if (name == "TOMEDO_CRAWL_SERVICE" && args_override.empty()) {
                rag_db_sync_all_config();
                std::string db_args = rag_db_path_;
                if (!use_args.empty()) db_args += " " + use_args;
                use_args = db_args;
            }

            auto argv_strings = split_args(use_args);

            mkdir("logs", 0755);
            svc.log_file = "logs/" + name + ".log";

            pid_t pid = fork();
            if (pid < 0) {
                std::cerr << "fork() failed for service " << name << ": " << strerror(errno) << "\n";
                return false;
            }
            if (pid == 0) {
                long max_fd = sysconf(_SC_OPEN_MAX);
                if (max_fd < 0) max_fd = 1024;
                for (int i = 3; i < (int)max_fd; ++i) close(i);

                int fd = open(svc.log_file.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) {
                    dup2(fd, STDOUT_FILENO);
                    dup2(fd, STDERR_FILENO);
                    close(fd);
                }

                std::vector<char*> argv;
                argv.push_back(const_cast<char*>(svc.binary_path.c_str()));
                for (auto& a : argv_strings) {
                    argv.push_back(const_cast<char*>(a.c_str()));
                }
                argv.push_back(nullptr);
                execv(svc.binary_path.c_str(), argv.data());
                _exit(1);
            }
            svc.managed = true;
            svc.pid = pid;
            svc.start_time = time(nullptr);

            if (!args_override.empty()) {
                svc.default_args = args_override;
                save_service_config(name, args_override);
            }
            return true;
        }
        return false;
    }

    bool stop_service(const std::string& name) {
        std::lock_guard<std::mutex> lock(services_mutex_);
        for (auto& svc : services_) {
            if (svc.name != name) continue;
            if (!svc.managed || svc.pid <= 0) return false;

            kill(svc.pid, SIGTERM);
            for (int i = 0; i < 50; i++) {
                int status;
                if (waitpid(svc.pid, &status, WNOHANG) == svc.pid) {
                    svc.managed = false;
                    svc.pid = 0;
                    return true;
                }
                usleep(STOP_POLL_INTERVAL_US);
            }
            kill(svc.pid, SIGKILL);
            waitpid(svc.pid, nullptr, 0);
            svc.managed = false;
            svc.pid = 0;
            return true;
        }
        return false;
    }

    void save_service_config(const std::string& name, const std::string& args) {
        if (!db_) return;
        sqlite3_stmt* stmt;
        const char* sql = "UPDATE service_config SET default_args = ? WHERE service = ?";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, args.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    std::string get_setting(const std::string& key, const std::string& default_val = "") {
        if (!db_) return default_val;
        sqlite3_stmt* stmt;
        const char* sql = "SELECT value FROM settings WHERE key = ?";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return default_val;
        sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
        std::string result = default_val;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            if (v) result = v;
        }
        sqlite3_finalize(stmt);
        return result;
    }

    void set_setting(const std::string& key, const std::string& value) {
        if (!db_) return;
        sqlite3_stmt* stmt;
        const char* sql = "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    void save_test_run(const TestInfo& test) {
        if (!db_) return;
        sqlite3_stmt* stmt;
        const char* sql = "INSERT INTO test_runs (test_name, start_time, end_time, exit_code, arguments, log_file) VALUES (?, ?, ?, ?, ?, ?)";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, test.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 2, test.start_time);
            sqlite3_bind_int64(stmt, 3, test.end_time);
            sqlite3_bind_int(stmt, 4, test.exit_code);
            std::string args_str;
            for (size_t i = 0; i < test.default_args.size(); i++) {
                if (i > 0) args_str += " ";
                args_str += test.default_args[i];
            }
            sqlite3_bind_text(stmt, 5, args_str.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 6, test.log_file.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    void shutdown_managed_processes() {
        {
            std::lock_guard<std::mutex> lock(services_mutex_);
            for (auto& svc : services_) {
                if (svc.managed && svc.pid > 0) {
                    kill(svc.pid, SIGTERM);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(tests_mutex_);
            for (auto& test : tests_) {
                if (test.is_running && test.pid > 0) {
                    kill(test.pid, SIGTERM);
                }
            }
        }
        usleep(SHUTDOWN_GRACE_US);
        {
            std::lock_guard<std::mutex> lock(services_mutex_);
            for (auto& svc : services_) {
                if (svc.managed && svc.pid > 0) {
                    kill(svc.pid, SIGKILL);
                    waitpid(svc.pid, nullptr, 0);
                    svc.pid = 0;
                    svc.managed = false;
                }
            }
        }
    }

    void log_receiver_loop();

    static ServiceType parse_service_type(const std::string& name) {
        if (name == "SIP_CLIENT") return ServiceType::SIP_CLIENT;
        if (name == "INBOUND_AUDIO_PROCESSOR") return ServiceType::INBOUND_AUDIO_PROCESSOR;
        if (name == "VAD_SERVICE") return ServiceType::VAD_SERVICE;
        if (name == "WHISPER_SERVICE") return ServiceType::WHISPER_SERVICE;
        if (name == "LLAMA_SERVICE") return ServiceType::LLAMA_SERVICE;
        if (name == "TTS_SERVICE") return ServiceType::TTS_SERVICE;
        if (name == "OUTBOUND_AUDIO_PROCESSOR") return ServiceType::OUTBOUND_AUDIO_PROCESSOR;
        if (name == "TOMEDO_CRAWL_SERVICE" || name == "TOMEDO_CRAWL") return ServiceType::TOMEDO_CRAWL_SERVICE;
        if (name == "MOSHI_SERVICE") return ServiceType::MOSHI_SERVICE;
        if (name == "FRONTEND") return ServiceType::FRONTEND;
        return ServiceType::SIP_CLIENT;
    }

    void process_log_message(const std::string& msg);

    std::mutex log_queue_mutex_;
    std::vector<LogEntry> log_queue_;

    void enqueue_log(const LogEntry& entry);
    void flush_log_queue();
    void rotate_logs();
    void handle_sse_stream(struct mg_connection *c, struct mg_http_message *hm);
    void remove_sse_connection(struct mg_connection *c);
    void flush_sse_queue();

    static void http_handler_plain_static(struct mg_connection *c, int ev, void *ev_data) {
        FrontendServer* self = static_cast<FrontendServer*>(c->fn_data);
        self->http_handler_plain(c, ev, ev_data);
    }

    void http_handler_plain(struct mg_connection *c, int ev, void *ev_data) {
        if (ev == MG_EV_CLOSE) {
            if (c->data[0] == 'S') {
                remove_sse_connection(c);
            }
            return;
        }
        if (ev == MG_EV_HTTP_MSG) {
            struct mg_http_message *hm = (struct mg_http_message *) ev_data;
            if (get_setting("http_redirect_to_https", "0") == "1") {
                char loc[1024];
                if (hm->query.len > 0) {
                    snprintf(loc, sizeof(loc), "https://127.0.0.1:%u%.*s?%.*s",
                             (unsigned)http_port_,
                             (int)hm->uri.len, hm->uri.buf,
                             (int)hm->query.len, hm->query.buf);
                } else {
                    snprintf(loc, sizeof(loc), "https://127.0.0.1:%u%.*s",
                             (unsigned)http_port_,
                             (int)hm->uri.len, hm->uri.buf);
                }
                mg_printf(c,
                    "HTTP/1.1 301 Moved Permanently\r\n"
                    "Location: %s\r\n"
                    "Content-Length: 0\r\n"
                    "\r\n", loc);
                return;
            }
            handle_http_request(c, hm);
        }
    }

    static void http_handler_static(struct mg_connection *c, int ev, void *ev_data) {
        FrontendServer* self = static_cast<FrontendServer*>(c->fn_data);
        self->http_handler(c, ev, ev_data);
    }

    void http_handler(struct mg_connection *c, int ev, void *ev_data) {
        if (ev == MG_EV_ACCEPT) {
            const auto certs = prodigy_tls::get_certs();
            if (!certs.cert_pem.empty() && !certs.key_pem.empty()) {
                struct mg_tls_opts opts{};
                opts.cert = mg_str(certs.cert_pem.c_str());
                opts.key  = mg_str(certs.key_pem.c_str());
                mg_tls_init(c, &opts);
            }
            return;
        }
        if (ev == MG_EV_CLOSE) {
            if (c->data[0] == 'S') {
                remove_sse_connection(c);
            }
            return;
        }
        if (ev == MG_EV_HTTP_MSG) {
            struct mg_http_message *hm = (struct mg_http_message *) ev_data;
            handle_http_request(c, hm);
        }
    }

    void handle_http_request(struct mg_connection *c, struct mg_http_message *hm) {
            if (get_setting("auth_enabled", "0") == "1") {
                bool is_public = (mg_strcmp(hm->uri, mg_str("/login")) == 0 ||
                                  mg_strcmp(hm->uri, mg_str("/api/auth/login")) == 0);
                if (!is_public) {
                    std::string token = extract_session_cookie(hm);
                    std::string username;
                    if (token.empty() || !validate_session(token, username)) {
                        if (mg_strcmp(hm->uri, mg_str("/")) == 0) {
                            serve_login_page(c);
                        } else {
                            mg_http_reply(c, 401, "Content-Type: application/json\r\n",
                                          "{\"error\":\"Unauthorized\"}");
                        }
                        return;
                    }
                }
            }
            if (mg_strcmp(hm->uri, mg_str("/login")) == 0) {
                serve_login_page(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/auth/login")) == 0) {
                handle_api_auth_login(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/auth/logout")) == 0) {
                handle_api_auth_logout(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/auth/users")) == 0) {
                handle_api_auth_users(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/auth/users/add")) == 0) {
                handle_api_auth_users_add(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/auth/users/change_password")) == 0) {
                handle_api_auth_users_change_password(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/auth/users/delete")) == 0) {
                handle_api_auth_users_delete(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/auth/settings")) == 0) {
                handle_api_auth_settings(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/certs/list")) == 0) {
                handle_api_certs_list(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/certs/generate")) == 0) {
                handle_api_certs_generate(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/certs/upload")) == 0) {
                handle_api_certs_upload(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/certs/select")) == 0) {
                handle_api_certs_select(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/certs/settings")) == 0) {
                handle_api_certs_settings(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/")) == 0) {
                serve_index(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/dashboard")) == 0) {
                handle_dashboard(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/tests")) == 0) {
                serve_tests_api(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/tests/start")) == 0) {
                handle_test_start(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/tests/stop")) == 0) {
                handle_test_stop(c, hm);
            } else if (mg_match(hm->uri, mg_str("/api/tests/*/history"), NULL)) {
                handle_test_history(c, hm);
            } else if (mg_match(hm->uri, mg_str("/api/tests/*/log"), NULL)) {
                handle_test_log(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/services")) == 0) {
                serve_services_api(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/services/start")) == 0) {
                handle_service_start(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/services/stop")) == 0) {
                handle_service_stop(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/services/restart")) == 0) {
                handle_service_restart(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/tts/status")) == 0) {
                handle_tts_status(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/services/config")) == 0) {
                handle_service_config(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/logs")) == 0) {
                serve_logs_api(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/logs/stream")) == 0) {
                handle_sse_stream(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/logs/recent")) == 0) {
                serve_logs_recent(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/db/query")) == 0) {
                handle_db_query(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/db/write_mode")) == 0) {
                handle_db_write_mode(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/db/schema")) == 0) {
                handle_db_schema(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/settings")) == 0) {
                handle_settings(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/status")) == 0) {
                handle_status(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/whisper/models")) == 0) {
                handle_whisper_models(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/sip/add-line")) == 0) {
                handle_sip_add_line(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/sip/remove-line")) == 0) {
                handle_sip_remove_line(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/sip/lines")) == 0) {
                handle_sip_lines(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/sip/stats")) == 0) {
                handle_sip_stats(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/iap/quality_test")) == 0) {
                handle_iap_quality_test(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/testfiles")) == 0) {
                serve_testfiles_api(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/testfiles/scan")) == 0) {
                handle_testfiles_scan(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/settings/log_level")) == 0) {
                handle_log_level_settings(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/test_results")) == 0) {
                handle_test_results(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/test_results_summary")) == 0) {
                handle_test_results_summary(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/whisper/accuracy_test")) == 0) {
                handle_whisper_accuracy_test(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/whisper/hallucination_filter")) == 0) {
                handle_whisper_hallucination_filter(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/oap/wav_recording")) == 0) {
                handle_oap_wav_recording(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/vad/config")) == 0) {
                handle_vad_config(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/whisper/accuracy_results")) == 0) {
                handle_whisper_accuracy_results(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/models")) == 0) {
                handle_models_get(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/models/benchmarks")) == 0) {
                handle_models_benchmarks_get(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/models/add")) == 0) {
                handle_models_add(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/models/search")) == 0) {
                handle_models_search(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/models/hf-files")) == 0) {
                handle_models_hf_files(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/models/local")) == 0) {
                handle_models_local(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/models/llama")) == 0) {
                handle_models_llama(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/models/kokoro")) == 0) {
                handle_models_kokoro(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/models/neutts")) == 0) {
                handle_models_neutts(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/models/upload")) == 0) {
                handle_models_upload(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/models/convert")) == 0) {
                handle_models_convert(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/models/download")) == 0) {
                handle_models_download(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/models/download/progress")) == 0) {
                handle_models_download_progress(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/whisper/benchmark")) == 0) {
                handle_whisper_benchmark(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/llama/prompts")) == 0) {
                handle_llama_prompts(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/llama/set_sampling")) == 0) {
                handle_llama_set_sampling(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/llama/quality_test")) == 0) {
                handle_llama_quality_test(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/llama/shutup_test")) == 0) {
                handle_llama_shutup_test(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/shutup_pipeline_test")) == 0) {
                handle_shutup_pipeline_test(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/llama/benchmark")) == 0) {
                handle_llama_benchmark(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/kokoro/quality_test")) == 0) {
                handle_kokoro_quality_test(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/kokoro/benchmark")) == 0) {
                handle_kokoro_benchmark(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/tts_roundtrip")) == 0) {
                handle_tts_roundtrip(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/pipeline/mode")) == 0) {
                handle_pipeline_mode(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/pipeline/health")) == 0) {
                handle_pipeline_health(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/full_loop_test")) == 0) {
                handle_full_loop_test(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/multiline_stress")) == 0) {
                handle_multiline_stress(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/pipeline_stress_test")) == 0) {
                handle_pipeline_stress_test(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/pipeline_stress/progress")) == 0) {
                handle_pipeline_stress_progress(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/pipeline_stress/stop")) == 0) {
                handle_pipeline_stress_stop(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/async/status")) == 0) {
                handle_async_status(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/rag/health")) == 0) {
                handle_rag_health(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/rag/config")) == 0) {
                handle_rag_config(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/rag/cert_upload")) == 0) {
                handle_rag_cert_upload(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/rag/trigger_crawl")) == 0) {
                handle_rag_trigger_crawl(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/ollama/status")) == 0) {
                handle_ollama_status(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/ollama/start")) == 0) {
                handle_ollama_start(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/ollama/stop")) == 0) {
                handle_ollama_stop(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/ollama/restart")) == 0) {
                handle_ollama_restart(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/ollama/models")) == 0) {
                handle_ollama_models(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/ollama/pull")) == 0) {
                handle_ollama_pull(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/ollama/install")) == 0) {
                handle_ollama_install(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/rag/wipe_vectors")) == 0) {
                handle_rag_wipe_vectors(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/tests/setup/start")) == 0) {
                handle_test_setup_start(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/tests/teardown")) == 0) {
                handle_test_teardown(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/tests/tts_preference")) == 0) {
                handle_test_tts_preference(c, hm);
            } else {
                mg_http_reply(c, 404, "", "Not Found\n");
            }
    }

    void serve_index(struct mg_connection *c) {
        std::string html = build_ui_html();
        mg_http_reply(c, 200, "Content-Type: text/html; charset=utf-8\r\nCache-Control: no-cache, no-store, must-revalidate\r\nPragma: no-cache\r\n", "%s", html.c_str());
    }

    std::string build_ui_html() {
        std::string h;
        h += R"WT(<!DOCTYPE html><html lang="en"><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Prodigy</title>
)WT";
        h += "<style>";
        h += get_embedded_fonts_css();
        h += get_fontawesome_css();
        h += get_frontend_css();
        h += "</style>";
        h += R"WT(</head><body>
<div class="wt-app">
<aside class="wt-sidebar">
<div class="wt-sidebar-header">
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" width="28" height="28">
  <defs>
    <linearGradient id="neon-grad" x1="0%" y1="0%" x2="100%" y2="100%">
      <stop offset="0%" stop-color="#ff2d95"/>
      <stop offset="50%" stop-color="#b026ff"/>
      <stop offset="100%" stop-color="#00fff5"/>
    </linearGradient>
    <filter id="neon-glow">
      <feDropShadow dx="0" dy="0" stdDeviation="2" flood-color="#ff2d95" flood-opacity="0.6"/>
      <feDropShadow dx="0" dy="0" stdDeviation="1" flood-color="#00fff5" flood-opacity="0.3"/>
    </filter>
  </defs>
  <g filter="url(#neon-glow)">
    <polygon points="16,1 29.86,9 29.86,23 16,31 2.14,23 2.14,9" fill="none" stroke="url(#neon-grad)" stroke-width="1.5"/>
    <text x="16" y="20" text-anchor="middle" font-family="Orbitron,monospace" font-size="15" font-weight="700" fill="url(#neon-grad)">P</text>
    <line x1="6" y1="6" x2="3" y2="3" stroke="#ff2d95" stroke-width="0.8" opacity="0.5"/>
    <line x1="26" y1="6" x2="29" y2="3" stroke="#00fff5" stroke-width="0.8" opacity="0.5"/>
    <line x1="6" y1="26" x2="3" y2="29" stroke="#b026ff" stroke-width="0.8" opacity="0.5"/>
    <line x1="26" y1="26" x2="29" y2="29" stroke="#00fff5" stroke-width="0.8" opacity="0.5"/>
    <circle cx="3" cy="3" r="1" fill="#ff2d95" opacity="0.6"/>
    <circle cx="29" cy="3" r="1" fill="#00fff5" opacity="0.6"/>
    <circle cx="3" cy="29" r="1" fill="#b026ff" opacity="0.6"/>
    <circle cx="29" cy="29" r="1" fill="#00fff5" opacity="0.6"/>
  </g>
</svg>
<span class="header-text">PRODIGY</span>
</div>
<div class="wt-sidebar-section">
<a class="wt-nav-item active" data-page="dashboard" onclick="showPage('dashboard')">
<i class="nav-icon fas fa-tachometer-alt" aria-hidden="true"></i><span class="nav-text">Dashboard</span></a>
</div>
<div class="wt-sidebar-section">
<p class="wt-sidebar-section-title">Pipeline</p>
<a class="wt-nav-item" data-page="services" onclick="showPage('services')">
<i class="nav-icon fas fa-cogs" aria-hidden="true"></i><span class="nav-text">Services</span><span class="nav-badge" id="svcBadge">0/6</span></a>
<a class="wt-nav-item" data-page="logs" onclick="showPage('logs')">
<i class="nav-icon fas fa-list-alt" aria-hidden="true"></i><span class="nav-text">Live Logs</span></a>
</div>
<div class="wt-sidebar-section">
<p class="wt-sidebar-section-title">Testing</p>
<a class="wt-nav-item" data-page="beta-testing" onclick="showPage('beta-testing')">
<i class="nav-icon fas fa-crosshairs" aria-hidden="true"></i><span class="nav-text">Tests</span></a>
</div>
<div class="wt-sidebar-section">
<p class="wt-sidebar-section-title">Configuration</p>
<a class="wt-nav-item" data-page="models" onclick="showPage('models')">
<i class="nav-icon fas fa-robot" aria-hidden="true"></i><span class="nav-text">Models</span></a>
<a class="wt-nav-item" data-page="database" onclick="showPage('database')">
<i class="nav-icon fas fa-database" aria-hidden="true"></i><span class="nav-text">Database</span></a>
<a class="wt-nav-item" data-page="credentials" onclick="showPage('credentials')">
<i class="nav-icon fas fa-key" aria-hidden="true"></i><span class="nav-text">Credentials</span></a>
<a class="wt-nav-item" data-page="certificates" onclick="showPage('certificates')">
<i class="nav-icon fas fa-certificate" aria-hidden="true"></i><span class="nav-text">Certificates</span></a>
<a class="wt-nav-item" data-page="login" onclick="showPage('login')">
<i class="nav-icon fas fa-sign-in-alt" aria-hidden="true"></i><span class="nav-text">Login</span></a>
</div>
<div class="wt-status-bar" id="statusBar">
<span id="statusText">Connecting...</span>
</div>
</aside>
<main class="wt-main">
)WT";
        h += build_ui_pages();
        h += R"WT(</main></div>)WT";
        h += "<script>" + get_chartjs() + "</script>";
        h += "<script>" + get_hammerjs() + "</script>";
        h += "<script>" + get_chartjs_zoom_plugin() + "</script>";
        h += "<script>" + build_ui_js() + "</script>";
        h += R"WT(</body></html>)WT";
        return h;
    }

    std::string build_ui_pages();

    std::string build_ui_js();

    void serve_tests_api(struct mg_connection *c) {
        std::lock_guard<std::mutex> lock(tests_mutex_);
        
        std::stringstream json;
        json << "{\"tests\":[";
        for (size_t i = 0; i < tests_.size(); i++) {
            if (i > 0) json << ",";
            const auto& t = tests_[i];
            json << "{"
                 << "\"name\":\"" << escape_json(t.name) << "\","
                 << "\"description\":\"" << escape_json(t.description) << "\","
                 << "\"binary_path\":\"" << escape_json(t.binary_path) << "\","
                 << "\"is_running\":" << (t.is_running ? "true" : "false") << ","
                 << "\"pid\":" << t.pid << ","
                 << "\"exit_code\":" << t.exit_code << ","
                 << "\"start_time\":" << t.start_time << ","
                 << "\"end_time\":" << t.end_time << ","
                 << "\"log_file\":\"" << escape_json(t.log_file) << "\","
                 << "\"default_args\":\"";
            for (size_t j = 0; j < t.default_args.size(); j++) {
                if (j > 0) json << " ";
                json << escape_json(t.default_args[j]);
            }
            json << "\"}";
        }
        json << "]}";
        
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    void serve_services_api(struct mg_connection *c) {
        std::lock_guard<std::mutex> lock(services_mutex_);
        std::stringstream json;
        json << "{\"services\":[";

        for (size_t i = 0; i < services_.size(); i++) {
            if (i > 0) json << ",";
            const auto& svc = services_[i];
            bool alive = svc.managed && svc.pid > 0;
            std::string status = alive ? "running" : "offline";

            json << "{"
                 << "\"name\":\"" << escape_json(svc.name) << "\","
                 << "\"description\":\"" << escape_json(svc.description) << "\","
                 << "\"binary_path\":\"" << escape_json(svc.binary_path) << "\","
                 << "\"status\":\"" << status << "\","
                 << "\"online\":" << (alive ? "true" : "false") << ","
                 << "\"managed\":" << (svc.managed ? "true" : "false") << ","
                 << "\"pid\":" << svc.pid << ","
                 << "\"default_args\":\"" << escape_json(svc.default_args) << "\","
                 << "\"start_time\":" << svc.start_time
                 << "}";
        }

        json << "]}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    void serve_logs_api(struct mg_connection *c, struct mg_http_message *hm) {
        if (!db_) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database not available\"}");
            return;
        }

        char svc_filter[64] = {0}, level_filter[16] = {0}, limit_str[16] = {0}, offset_str[16] = {0};
        mg_http_get_var(&hm->query, "service", svc_filter, sizeof(svc_filter));
        mg_http_get_var(&hm->query, "level", level_filter, sizeof(level_filter));
        mg_http_get_var(&hm->query, "limit", limit_str, sizeof(limit_str));
        mg_http_get_var(&hm->query, "offset", offset_str, sizeof(offset_str));

        int limit = limit_str[0] ? atoi(limit_str) : 100;
        int offset = offset_str[0] ? atoi(offset_str) : 0;
        if (limit < 1) limit = 1;
        if (limit > 1000) limit = 1000;
        if (offset < 0) offset = 0;

        std::string sql = "SELECT timestamp, service, call_id, level, message FROM logs";
        std::vector<std::string> conditions;
        int bind_idx = 1;
        bool has_svc = svc_filter[0] != 0;
        bool has_lvl = level_filter[0] != 0;
        if (has_svc) conditions.push_back("service = ?");
        if (has_lvl) conditions.push_back("level = ?");
        if (!conditions.empty()) {
            sql += " WHERE ";
            for (size_t i = 0; i < conditions.size(); i++) {
                if (i > 0) sql += " AND ";
                sql += conditions[i];
            }
        }
        sql += " ORDER BY id DESC LIMIT ? OFFSET ?";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                         "{\"error\":\"%s\"}", escape_json(sqlite3_errmsg(db_)).c_str());
            return;
        }
        if (has_svc) sqlite3_bind_text(stmt, bind_idx++, svc_filter, -1, SQLITE_TRANSIENT);
        if (has_lvl) sqlite3_bind_text(stmt, bind_idx++, level_filter, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, bind_idx++, limit);
        sqlite3_bind_int(stmt, bind_idx++, offset);

        std::stringstream json;
        json << "{\"logs\":[";
        int row_count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (row_count > 0) json << ",";
            const char* ts = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* svc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            int call_id = sqlite3_column_int(stmt, 2);
            const char* lvl = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            const char* msg = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            json << "{\"timestamp\":\"" << escape_json(ts ? ts : "") << "\","
                 << "\"service\":\"" << escape_json(svc ? svc : "") << "\","
                 << "\"call_id\":" << call_id << ","
                 << "\"level\":\"" << escape_json(lvl ? lvl : "") << "\","
                 << "\"message\":\"" << escape_json(msg ? msg : "") << "\"}";
            row_count++;
        }
        sqlite3_finalize(stmt);
        json << "]}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    void serve_logs_recent(struct mg_connection *c) {
        std::lock_guard<std::mutex> lock(logs_mutex_);

        std::stringstream json;
        json << "{\"logs\":[";
        size_t count = 0;
        for (auto it = recent_logs_.rbegin(); it != recent_logs_.rend() && count < RECENT_LOGS_API_LIMIT; ++it, ++count) {
            if (count > 0) json << ",";
            json << "{"
                 << "\"timestamp\":\"" << escape_json(it->timestamp) << "\","
                 << "\"service\":\"" << service_type_to_string(it->service) << "\","
                 << "\"level\":\"" << escape_json(it->level) << "\","
                 << "\"call_id\":" << it->call_id << ","
                 << "\"message\":\"" << escape_json(it->message) << "\""
                 << "}";
        }
        json << "]}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    // extract_json_string() — Hand-rolled JSON string extractor.
    // Returns the string value for a given key from a JSON object, handling
    // escape sequences: \", \\, \n, \t, \r, \/, \b, \f. No external JSON
    // library dependency. Validates key is preceded by '{' or ',' to avoid
    // false matches on keys that appear inside string values.
    static std::string extract_json_string(const std::string& json, const std::string& key) {
        std::string needle = "\"" + key + "\"";
        size_t pos = 0;
        while (true) {
            pos = json.find(needle, pos);
            if (pos == std::string::npos) return "";
            // Verify the character before the key (skipping whitespace) is '{' or ','
            bool valid = false;
            if (pos == 0) { valid = true; }
            else {
                size_t pre = pos - 1;
                while (pre > 0 && (json[pre] == ' ' || json[pre] == '\t' || json[pre] == '\n' || json[pre] == '\r')) pre--;
                // re-check index 0 after the loop (loop exits before testing it)
                char c = json[pre];
                valid = (c == '{' || c == ',' || (pre == 0 && (c == ' ' || c == '\t' || c == '\n' || c == '\r')));
            }
            if (valid) break;
            pos += needle.size();
        }
        pos += needle.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == ':')) pos++;
        if (pos >= json.size() || json[pos] != '"') return "";
        pos++;
        std::string result;
        while (pos < json.size()) {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                char next = json[pos + 1];
                if (next == '"') { result += '"'; pos += 2; }
                else if (next == '\\') { result += '\\'; pos += 2; }
                else if (next == 'n') { result += '\n'; pos += 2; }
                else if (next == 't') { result += '\t'; pos += 2; }
                else if (next == 'r') { result += '\r'; pos += 2; }
                else if (next == '/') { result += '/'; pos += 2; }
                else if (next == 'b') { result += '\b'; pos += 2; }
                else if (next == 'f') { result += '\f'; pos += 2; }
                else if (next == 'u' && pos + 5 < json.size()) {
                    auto is_hex = [](char c) { return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'); };
                    char h0=json[pos+2], h1=json[pos+3], h2=json[pos+4], h3=json[pos+5];
                    if (!is_hex(h0) || !is_hex(h1) || !is_hex(h2) || !is_hex(h3)) {
                        result += "\xEF\xBF\xBD";
                        pos += 6;
                    } else {
                        char hex[5] = {h0, h1, h2, h3, '\0'};
                        uint32_t cp = static_cast<uint32_t>(std::strtoul(hex, nullptr, 16));
                        pos += 6;
                        if (cp >= 0xD800 && cp <= 0xDBFF) {
                            if (pos + 5 < json.size() && json[pos] == '\\' && json[pos+1] == 'u' &&
                                is_hex(json[pos+2]) && is_hex(json[pos+3]) && is_hex(json[pos+4]) && is_hex(json[pos+5])) {
                                char hex2[5] = {json[pos+2], json[pos+3], json[pos+4], json[pos+5], '\0'};
                                uint32_t low = static_cast<uint32_t>(std::strtoul(hex2, nullptr, 16));
                                if (low >= 0xDC00 && low <= 0xDFFF) {
                                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                                    pos += 6;
                                } else {
                                    result += "\xEF\xBF\xBD";
                                    continue;
                                }
                            } else {
                                result += "\xEF\xBF\xBD";
                                continue;
                            }
                        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                            result += "\xEF\xBF\xBD";
                            continue;
                        }
                        if (cp == 0) {
                            result += "\xEF\xBF\xBD";
                        } else if (cp < 0x80) {
                            result += static_cast<char>(cp);
                        } else if (cp < 0x800) {
                            result += static_cast<char>(0xC0 | (cp >> 6));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else if (cp < 0x10000) {
                            result += static_cast<char>(0xE0 | (cp >> 12));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        } else {
                            result += static_cast<char>(0xF0 | (cp >> 18));
                            result += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                            result += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                            result += static_cast<char>(0x80 | (cp & 0x3F));
                        }
                    }
                }
                else { result += json[pos]; pos++; }
            } else if (json[pos] == '"') {
                break;
            } else {
                result += json[pos];
                pos++;
            }
        }
        return result;
    }

    // POST /api/tests/start — Start a test binary as a child process. Captures
    // stdout/stderr to a log file for later retrieval via handle_test_log.
    void handle_test_start(struct mg_connection *c, struct mg_http_message *hm) {
        std::string body(hm->body.buf, hm->body.len);
        std::string test_name = extract_json_string(body, "test");
        std::string custom_args = extract_json_string(body, "args");

        if (test_name.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing test name\"}");
            return;
        }

        std::lock_guard<std::mutex> lock(tests_mutex_);

        TestInfo* found = nullptr;
        for (auto& test : tests_) {
            if (test.name == test_name) { found = &test; break; }
        }

        if (!found) {
            mg_http_reply(c, 404, "Content-Type: application/json\r\n", "{\"error\":\"Test not found\"}");
            return;
        }

        if (found->is_running) {
            mg_http_reply(c, 409, "Content-Type: application/json\r\n", "{\"error\":\"Test is already running\"}");
            return;
        }

        if (!is_allowed_binary(found->binary_path)) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Invalid test binary path\"}");
            return;
        }

        std::string bin_name = found->binary_path;
        size_t slash = bin_name.rfind('/');
        if (slash != std::string::npos) bin_name = bin_name.substr(slash + 1);
        kill_ghost_processes(bin_name);

        std::vector<std::string> use_args;
        if (!custom_args.empty()) {
            use_args = split_args(custom_args);
        } else {
            use_args = found->default_args;
        }

        mkdir("logs", 0755);
        std::string log_path = "logs/" + found->name + "_" + std::to_string(time(nullptr)) + ".log";

        pid_t pid = fork();
        if (pid < 0) {
            std::string err_msg = std::string("{\"error\":\"Failed to start test: ") + escape_json(strerror(errno)) + "\"}";
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "%s", err_msg.c_str());
            return;
        }
        if (pid == 0) {
            long max_fd = sysconf(_SC_OPEN_MAX);
            if (max_fd < 0) max_fd = 1024;
            for (int i = 3; i < (int)max_fd; ++i) close(i);

            int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) {
                dup2(fd, STDOUT_FILENO);
                dup2(fd, STDERR_FILENO);
                close(fd);
            }

            std::vector<char*> argv;
            argv.push_back(const_cast<char*>(found->binary_path.c_str()));
            for (auto& a : use_args) {
                argv.push_back(const_cast<char*>(a.c_str()));
            }
            argv.push_back(nullptr);

            execv(found->binary_path.c_str(), argv.data());
            _exit(1);
        }
        found->is_running = true;
        found->pid = pid;
        found->start_time = time(nullptr);
        found->log_file = log_path;
        found->default_args = use_args;

        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\":\"ok\"}");
    }

    // POST /api/tests/stop — Kill a running test process by name (SIGTERM).
    void handle_test_stop(struct mg_connection *c, struct mg_http_message *hm) {
        std::string body(hm->body.buf, hm->body.len);
        std::string test_name = extract_json_string(body, "test");
        
        std::lock_guard<std::mutex> lock(tests_mutex_);
        for (auto& test : tests_) {
            if (test.name == test_name && test.is_running) {
                if (test.pid > 0) {
                    kill(test.pid, SIGTERM);
                }
                break;
            }
        }
        
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\":\"ok\"}");
    }

    // GET /api/tests/*/history — Returns last 20 test runs from SQLite for a named test.
    void handle_test_history(struct mg_connection *c, struct mg_http_message *hm) {
        std::string uri(hm->uri.buf, hm->uri.len);
        size_t start = strlen("/api/tests/");
        size_t end = uri.find("/history");
        std::string test_name = (end != std::string::npos) ? uri.substr(start, end - start) : "";

        if (!db_ || test_name.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Invalid request\"}");
            return;
        }

        sqlite3_stmt* stmt;
        const char* sql = "SELECT id, start_time, end_time, exit_code, arguments, log_file FROM test_runs WHERE test_name = ? ORDER BY start_time DESC LIMIT 20";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"DB error\"}");
            return;
        }
        sqlite3_bind_text(stmt, 1, test_name.c_str(), -1, SQLITE_TRANSIENT);

        std::stringstream json;
        json << "{\"test\":\"" << escape_json(test_name) << "\",\"runs\":[";
        int count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (count > 0) json << ",";
            json << "{\"id\":" << sqlite3_column_int(stmt, 0)
                 << ",\"start_time\":" << sqlite3_column_int64(stmt, 1)
                 << ",\"end_time\":" << sqlite3_column_int64(stmt, 2)
                 << ",\"exit_code\":" << sqlite3_column_int(stmt, 3);
            const char* args = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            const char* log = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 5));
            json << ",\"arguments\":\"" << escape_json(args ? args : "") << "\""
                 << ",\"log_file\":\"" << escape_json(log ? log : "") << "\"}";
            count++;
        }
        sqlite3_finalize(stmt);
        json << "]}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    // GET /api/tests/*/log — Returns stdout/stderr log content of a test's child process.
    void handle_test_log(struct mg_connection *c, struct mg_http_message *hm) {
        std::string uri(hm->uri.buf, hm->uri.len);
        size_t start = strlen("/api/tests/");
        size_t end = uri.find("/log");
        std::string test_name = (end != std::string::npos) ? uri.substr(start, end - start) : "";

        std::string log_file;
        {
            std::lock_guard<std::mutex> lock(tests_mutex_);
            for (const auto& t : tests_) {
                if (t.name == test_name && !t.log_file.empty()) {
                    log_file = t.log_file;
                    break;
                }
            }
        }

        if (log_file.empty()) {
            mg_http_reply(c, 404, "Content-Type: application/json\r\n", "{\"error\":\"No log file\"}");
            return;
        }

        std::ifstream f(log_file);
        if (!f.is_open()) {
            mg_http_reply(c, 404, "Content-Type: application/json\r\n", "{\"error\":\"Log file not found\"}");
            return;
        }

        std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (content.size() > 65536) {
            content = content.substr(content.size() - 65536);
        }

        std::stringstream json;
        json << "{\"test\":\"" << escape_json(test_name) << "\",\"log\":\"" << escape_json(content) << "\"}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    // POST /api/services/start — Start a pipeline service by name. Reads persisted
    // log level from SQLite and appends --log-level to args. Side effects: spawns
    // child process, updates service status in services_ vector.
    void handle_service_start(struct mg_connection *c, struct mg_http_message *hm) {
        std::string body(hm->body.buf, hm->body.len);
        std::string name = extract_json_string(body, "service");
        std::string args = extract_json_string(body, "args");

        if (name.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing service name\"}");
            return;
        }

        if (start_service(name, args)) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\":\"started\"}");
        } else {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Cannot start service (already running or binary not found)\"}");
        }
    }

    // Engine cmd-port lookup by engine name (dock-local diagnostic ports).
    // Used by quality_test / benchmark handlers that talk directly to the
    // engine's private cmd-port (TEST_SYNTH, BENCHMARK, SYNTH_WAV, etc.).
    static uint16_t tts_engine_cmd_port_for(const std::string& engine_name) {
        if (engine_name == "kokoro") return KOKORO_ENGINE_CMD_PORT;
        if (engine_name == "neutts") return NEUTTS_ENGINE_CMD_PORT;
        // Unknown engine name: return 0 so the caller can surface a
        // diagnostic error instead of silently misrouting to kokoro.
        return 0;
    }

    // Query the TTS dock's cmd-port for the currently docked engine name.
    // Returns the engine name (e.g. "kokoro", "neutts") or "" if no engine
    // is docked / the dock is unreachable.
    std::string query_tts_active_engine() {
        uint16_t port = whispertalk::service_cmd_port(whispertalk::ServiceType::TTS_SERVICE);
        std::string err;
        std::string resp = tcp_command(port, "STATUS", err, 2);
        if (resp.empty()) return "";
        while (!resp.empty() && (resp.back() == '\n' || resp.back() == '\r')) resp.pop_back();
        // Dock protocol: "ACTIVE <name>" or "NONE"
        const std::string prefix = "ACTIVE ";
        if (resp.compare(0, prefix.size(), prefix) == 0) {
            return resp.substr(prefix.size());
        }
        return "";
    }

    // GET /api/tts/status — Returns the currently docked TTS engine (or null).
    // {"engine":"kokoro"} | {"engine":"neutts"} | {"engine":null}
    void handle_tts_status(struct mg_connection *c, struct mg_http_message *hm) {
        (void)hm;
        std::string engine = query_tts_active_engine();
        if (engine.empty()) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                "{\"engine\":null}");
        } else {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                "{\"engine\":\"%s\"}", escape_json(engine).c_str());
        }
    }

    // -------------------------------------------------------------------------
    // Test Setup / Teardown
    // -------------------------------------------------------------------------

    // POST /api/tests/setup/start — Start the async pre-test setup procedure.
    // Body: {"tts":"auto"|"kokoro"|"neutts"}
    // Returns: 202 {"task_id":N}
    void handle_test_setup_start(struct mg_connection *c, struct mg_http_message *hm) {
        std::string body(hm->body.buf, hm->body.len);
        std::string tts = extract_json_string(body, "tts");
        if (tts != "kokoro" && tts != "neutts") tts = "auto";

        int64_t task_id = create_async_task("test_setup");
        std::thread([this, task_id, tts]() {
            run_test_setup_async(task_id, tts);
        }).detach();
        mg_http_reply(c, 202, "Content-Type: application/json\r\n",
            "{\"task_id\":%lld}", (long long)task_id);
    }

    // Helper: update a task's progress JSON while it is still running.
    void set_setup_progress(int64_t task_id, const std::string& step, const std::string& detail) {
        std::string json = "{\"status\":\"running\",\"step\":\"" + step
            + "\",\"detail\":\"" + escape_json(detail) + "\"}";
        update_async_task_progress(task_id, json);
    }

    // Core async setup logic: Steps A–D.
    void run_test_setup_async(int64_t task_id, const std::string& tts_choice) {
        // ---- Step A: check which of the 9 services are running ----
        set_setup_progress(task_id, "A", "Checking service state...");

        const std::vector<std::string> all_svcs = {
            "SIP_CLIENT", "INBOUND_AUDIO_PROCESSOR", "VAD_SERVICE",
            "WHISPER_SERVICE", "LLAMA_SERVICE", "TTS_SERVICE",
            "KOKORO_ENGINE", "NEUTTS_ENGINE",
            "OUTBOUND_AUDIO_PROCESSOR", "TOMEDO_CRAWL_SERVICE"
        };
        const std::vector<std::string> core_svcs = {
            "SIP_CLIENT", "INBOUND_AUDIO_PROCESSOR", "VAD_SERVICE",
            "WHISPER_SERVICE", "LLAMA_SERVICE", "OUTBOUND_AUDIO_PROCESSOR"
        };
        const std::vector<std::string> tts_svcs  = {"TTS_SERVICE", "KOKORO_ENGINE", "NEUTTS_ENGINE"};
        const std::vector<std::string> rag_svcs   = {"TOMEDO_CRAWL_SERVICE"};

        int running_count = 0;
        for (const auto& s : all_svcs) {
            if (is_service_running(s)) running_count++;
        }
        bool all_running = (running_count == (int)all_svcs.size());

        // ---- Step B: normalise service state ----
        set_setup_progress(task_id, "B", all_running
            ? "All services running — stopping TTS and RAG for clean restart..."
            : "Stopping all services and starting core pipeline...");

        if (all_running) {
            // Case 1: hang up any active call, remove SIP lines, stop TTS engines+dock+RAG
            std::string sip_err;
            http_post_localhost(TEST_SIP_PROVIDER_PORT, "/hangup", "{}", sip_err);
            send_negotiation_command(whispertalk::ServiceType::SIP_CLIENT, "REMOVE_ALL_LINES");
            stop_service("KOKORO_ENGINE");
            stop_service("NEUTTS_ENGINE");
            stop_service("TTS_SERVICE");
            stop_service("TOMEDO_CRAWL_SERVICE");
            usleep(1000000); // 1s settling
        } else {
            // Case 2: stop everything, start core pipeline in order
            static const std::vector<std::string> stop_order = {
                "TOMEDO_CRAWL_SERVICE", "NEUTTS_ENGINE", "KOKORO_ENGINE", "TTS_SERVICE",
                "OUTBOUND_AUDIO_PROCESSOR", "LLAMA_SERVICE", "WHISPER_SERVICE",
                "VAD_SERVICE", "INBOUND_AUDIO_PROCESSOR", "SIP_CLIENT"
            };
            for (const auto& s : stop_order) {
                if (is_service_running(s)) {
                    stop_service(s);
                    usleep(500000);
                }
            }

            // Hang up and clear lines too, in case TEST_SIP_PROVIDER is still running
            std::string sip_err;
            http_post_localhost(TEST_SIP_PROVIDER_PORT, "/hangup", "{}", sip_err);

            // Start core pipeline with mandatory delays
            struct StartStep { std::string name; int sleep_s; };
            static const std::vector<StartStep> start_order = {
                {"SIP_CLIENT",               3},
                {"INBOUND_AUDIO_PROCESSOR",  3},
                {"VAD_SERVICE",              3},
                {"WHISPER_SERVICE",         10},
                {"LLAMA_SERVICE",           10},
                {"TTS_SERVICE",              2},
                {"OUTBOUND_AUDIO_PROCESSOR", 3},
            };
            for (const auto& step : start_order) {
                set_setup_progress(task_id, "B", "Starting " + step.name + "...");
                if (!start_service(step.name, "")) {
                    finish_async_task(task_id,
                        "{\"error\":\"Failed to start " + step.name + "\"}");
                    return;
                }
                for (int i = 0; i < step.sleep_s; i++) {
                    usleep(1000000);
                }
            }
        }

        // ---- Step C: ensure TTS dock is up, then start the chosen engine ----
        std::string engine_to_start = (tts_choice == "neutts") ? "NEUTTS_ENGINE" : "KOKORO_ENGINE";
        std::string engine_label    = (tts_choice == "neutts") ? "NeuTTS" : "Kokoro";
        std::string engine_name     = (tts_choice == "neutts") ? "neutts" : "kokoro";

        // Make sure the generic TTS dock is running (it may have been skipped in Case 1 above).
        if (!is_service_running("TTS_SERVICE")) {
            set_setup_progress(task_id, "C", "Starting generic TTS dock (TTS_SERVICE)...");
            if (!start_service("TTS_SERVICE", "")) {
                finish_async_task(task_id, "{\"error\":\"Failed to start TTS_SERVICE (dock)\"}");
                return;
            }
            // Wait up to 10s for dock cmd-port to respond.
            uint16_t dock_cmd_port = whispertalk::service_cmd_port(whispertalk::ServiceType::TTS_SERVICE);
            bool dock_ready = false;
            for (int i = 0; i < 20; i++) {
                std::string err;
                std::string resp = tcp_command(dock_cmd_port, "PING", err, 1);
                if (resp.find("PONG") != std::string::npos) { dock_ready = true; break; }
                usleep(500000);
            }
            if (!dock_ready) {
                finish_async_task(task_id, "{\"error\":\"TTS dock did not become reachable within 10s\"}");
                return;
            }
        }

        // Stop any previously docked engine and start the requested one.
        set_setup_progress(task_id, "C", "Starting " + engine_label + " engine...");
        stop_service("KOKORO_ENGINE");
        stop_service("NEUTTS_ENGINE");
        usleep(500000);

        if (!start_service(engine_to_start, "")) {
            finish_async_task(task_id, "{\"error\":\"Failed to start TTS engine: " + engine_to_start + "\"}");
            return;
        }

        // Poll the dock's /api/tts/status equivalent (STATUS cmd) until the
        // chosen engine shows up as ACTIVE. Engines take a while to load
        // their models before they send HELLO.
        {
            bool tts_ready = false;
            for (int i = 0; i < 720; i++) {
                set_setup_progress(task_id, "C",
                    "Waiting for " + engine_label + " engine to dock (" + std::to_string(i) + "s)...");
                std::string active = query_tts_active_engine();
                if (active == engine_name) {
                    tts_ready = true;
                    break;
                }
                usleep(1000000);
            }
            if (!tts_ready) {
                finish_async_task(task_id,
                    "{\"error\":\"" + engine_label + " engine did not dock within 12 minutes\"}");
                return;
            }
        }

        // Record active TTS so teardown knows what to stop
        set_setting("test_active_tts", engine_name);

        // ---- Step D: test SIP provider + conference ----
        set_setup_progress(task_id, "D", "Starting Test SIP Provider...");

        if (!is_service_running("TEST_SIP_PROVIDER")) {
            if (!start_service("TEST_SIP_PROVIDER", "")) {
                finish_async_task(task_id, "{\"error\":\"Failed to start TEST_SIP_PROVIDER\"}");
                return;
            }
        }

        // Wait up to 5s for provider HTTP to be reachable
        bool provider_up = false;
        for (int i = 0; i < 10; i++) {
            std::string err;
            std::string resp = http_get_localhost(TEST_SIP_PROVIDER_PORT, "/status", err);
            if (!resp.empty()) { provider_up = true; break; }
            usleep(500000);
        }
        if (!provider_up) {
            finish_async_task(task_id, "{\"error\":\"Test SIP provider did not become reachable within 5s\"}");
            return;
        }

        // Register Alice and Bob
        set_setup_progress(task_id, "D", "Registering SIP lines (Alice + Bob)...");
        send_negotiation_command(whispertalk::ServiceType::SIP_CLIENT, "ADD_LINE alice 127.0.0.1");
        usleep(500000);
        send_negotiation_command(whispertalk::ServiceType::SIP_CLIENT, "ADD_LINE bob 127.0.0.1");

        // Wait up to 10s for both lines to appear in /users
        bool lines_ready = false;
        for (int i = 0; i < 20; i++) {
            std::string err;
            std::string resp = http_get_localhost(TEST_SIP_PROVIDER_PORT, "/users", err);
            if (resp.find("\"alice\"") != std::string::npos
                && resp.find("\"bob\"") != std::string::npos) {
                lines_ready = true;
                break;
            }
            usleep(500000);
        }
        if (!lines_ready) {
            finish_async_task(task_id, "{\"error\":\"SIP lines did not register within 10s\"}");
            return;
        }

        // Start conference
        set_setup_progress(task_id, "D", "Starting Alice+Bob conference...");
        std::string conf_err;
        std::string conf_resp = http_post_localhost(TEST_SIP_PROVIDER_PORT, "/conference",
            "{\"users\":[\"alice\",\"bob\"]}", conf_err);
        if (conf_resp.find("\"success\":true") == std::string::npos) {
            finish_async_task(task_id, "{\"error\":\"Failed to start conference: "
                + escape_json(conf_err.empty() ? conf_resp : conf_err) + "\"}");
            return;
        }

        // Wait up to 10s for call to be active
        bool call_active = false;
        for (int i = 0; i < 20; i++) {
            std::string err;
            std::string resp = http_get_localhost(TEST_SIP_PROVIDER_PORT, "/status", err);
            if (resp.find("\"call_active\":true") != std::string::npos) {
                call_active = true;
                break;
            }
            usleep(500000);
        }
        if (!call_active) {
            finish_async_task(task_id, "{\"error\":\"Conference call did not become active within 10s\"}");
            return;
        }

        std::string active_tts = (tts_choice == "neutts") ? "neutts" : "kokoro";
        finish_async_task(task_id,
            "{\"status\":\"done\",\"tts\":\"" + active_tts + "\"}");
    }

    // POST /api/tests/teardown — Hang up conference, remove SIP lines, stop active TTS.
    void handle_test_teardown(struct mg_connection *c, struct mg_http_message *hm) {
        (void)hm;
        // Hang up any active conference
        std::string err;
        http_post_localhost(TEST_SIP_PROVIDER_PORT, "/hangup", "{}", err);

        // Remove all SIP lines
        send_negotiation_command(whispertalk::ServiceType::SIP_CLIENT, "REMOVE_ALL_LINES");

        // Stop the TTS that was active for this run
        std::string active_tts = get_setting("test_active_tts", "");
        if (active_tts == "neutts") {
            stop_service("NEUTTS_ENGINE");
        } else if (active_tts == "kokoro") {
            stop_service("KOKORO_ENGINE");
        }
        set_setting("test_active_tts", "");

        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"ok\":true}");
    }

    // GET/POST /api/tests/tts_preference — Get or set the TTS preference for test runs.
    // GET:  returns {"preference":"auto"|"kokoro"|"neutts"}
    // POST: {"preference":"..."} — persists to settings
    void handle_test_tts_preference(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("GET")) == 0) {
            std::string pref = get_setting("test_tts_preference", "auto");
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                "{\"preference\":\"%s\"}", pref.c_str());
        } else {
            std::string body(hm->body.buf, hm->body.len);
            std::string pref = extract_json_string(body, "preference");
            if (pref != "kokoro" && pref != "neutts") pref = "auto";
            set_setting("test_tts_preference", pref);
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"ok\":true}");
        }
    }

    // POST /api/services/stop — Stop a running service by sending SIGTERM.
    void handle_service_stop(struct mg_connection *c, struct mg_http_message *hm) {
        std::string body(hm->body.buf, hm->body.len);
        std::string name = extract_json_string(body, "service");

        if (name.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing service name\"}");
            return;
        }

        if (stop_service(name)) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\":\"stopped\"}");
        } else {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Cannot stop service (not managed by frontend)\"}");
        }
    }

    // POST /api/services/restart — Stop then start a service (500ms sleep between).
    void handle_service_restart(struct mg_connection *c, struct mg_http_message *hm) {
        std::string body(hm->body.buf, hm->body.len);
        std::string name = extract_json_string(body, "service");
        std::string args = extract_json_string(body, "args");

        if (name.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing service name\"}");
            return;
        }

        stop_service(name);

        usleep(RESTART_WAIT_US);

        if (start_service(name, args)) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\":\"restarted\"}");
        } else {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Restart failed\"}");
        }
    }

    // GET/POST /api/services/config — GET: return all service configs (name, binary, args).
    // POST: update default_args for a service in memory and persist to SQLite.
    void handle_service_config(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("GET")) == 0) {
            std::lock_guard<std::mutex> lock(services_mutex_);
            std::stringstream json;
            json << "{\"configs\":[";
            for (size_t i = 0; i < services_.size(); i++) {
                if (i > 0) json << ",";
                const auto& svc = services_[i];
                json << "{\"service\":\"" << escape_json(svc.name) << "\","
                     << "\"binary_path\":\"" << escape_json(svc.binary_path) << "\","
                     << "\"default_args\":\"" << escape_json(svc.default_args) << "\","
                     << "\"description\":\"" << escape_json(svc.description) << "\"}";
            }
            json << "]}";
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
        } else {
            std::string body(hm->body.buf, hm->body.len);
            std::string name = extract_json_string(body, "service");
            std::string args = extract_json_string(body, "args");

            if (name.empty()) {
                mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing service name\"}");
                return;
            }

            {
                std::lock_guard<std::mutex> lock(services_mutex_);
                for (auto& svc : services_) {
                    if (svc.name == name) {
                        svc.default_args = args;
                        break;
                    }
                }
            }
            save_service_config(name, args);
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\":\"saved\"}");
        }
    }

    void handle_pipeline_mode(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("GET")) == 0) {
            std::string mode = get_setting("pipeline_mode", "classic");
            std::string json = "{\"mode\":\"" + escape_json(mode) + "\"}";
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.c_str());
        } else if (mg_strcmp(hm->method, mg_str("POST")) == 0) {
            std::string body(hm->body.buf, hm->body.len);
            std::string mode = extract_json_string(body, "mode");

            if (mode != "classic" && mode != "moshi") {
                mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Invalid mode. Must be 'classic' or 'moshi'\"}");
                return;
            }

            set_setting("pipeline_mode", mode);

            std::string iap_args;
            bool iap_found = false;
            {
                std::lock_guard<std::mutex> lock(services_mutex_);
                for (auto& svc : services_) {
                    if (svc.name == "INBOUND_AUDIO_PROCESSOR") {
                        std::string args = svc.default_args;
                        const std::string flag_sp = " --moshi-mode";
                        const std::string flag_bare = "--moshi-mode";
                        if (mode == "moshi") {
                            if (args.find(flag_bare) == std::string::npos) {
                                args += flag_sp;
                            }
                        } else {
                            size_t pos;
                            while ((pos = args.find(flag_sp)) != std::string::npos) {
                                args.erase(pos, flag_sp.size());
                            }
                            if (args.find(flag_bare) == 0) {
                                args.erase(0, flag_bare.size());
                            }
                        }
                        svc.default_args = args;
                        iap_args = args;
                        iap_found = true;
                        break;
                    }
                }
            }
            if (iap_found) {
                save_service_config("INBOUND_AUDIO_PROCESSOR", iap_args);
            }

            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\":\"saved\"}");
        } else {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"Method not allowed\"}");
        }
    }

    std::string send_negotiation_command(whispertalk::ServiceType target, const std::string& cmd) {
        uint16_t port = whispertalk::service_cmd_port(target);
        if (port == 0) return "";

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return "";

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(port);

        struct timeval tv{2, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            return "";
        }

        size_t total_sent = 0;
        while (total_sent < cmd.size()) {
            ssize_t sent = ::send(sock, cmd.c_str() + total_sent, cmd.size() - total_sent, 0);
            if (sent <= 0) {
                close(sock);
                return "";
            }
            total_sent += static_cast<size_t>(sent);
        }

        char buf[4096];
        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
        close(sock);
        if (n <= 0) return "";
        buf[n] = '\0';
        return std::string(buf, n);
    }

    // POST /api/sip/add-line — Register a new SIP account. Sends ADD_LINE command
    // to the SIP Client's cmd port (13102). Returns LINE_ADDED on success.
    void handle_sip_add_line(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        std::string body(hm->body.buf, hm->body.len);
        std::string user = extract_json_string(body, "user");
        std::string server = extract_json_string(body, "server");
        std::string password = extract_json_string(body, "password");
        std::string port_str = extract_json_string(body, "port");

        if (user.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing 'user'\"}");
            return;
        }

        int port_val = 5060;
        if (!port_str.empty()) {
            try { port_val = std::stoi(port_str); } catch (...) { port_val = 5060; }
            if (port_val < 1 || port_val > 65535) port_val = 5060;
        }

        auto has_space_or_ctrl = [](const std::string& s) {
            for (char ch : s) if (ch <= ' ' || ch == 127) return true;
            return false;
        };
        if (has_space_or_ctrl(user) || has_space_or_ctrl(server) || has_space_or_ctrl(password)) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Fields must not contain spaces or control characters\"}");
            return;
        }

        std::string cmd = "ADD_LINE " + user + " " + (server.empty() ? "127.0.0.1" : server);
        cmd += " " + std::to_string(port_val);
        cmd += " " + (password.empty() ? "-" : password);

        std::string resp = send_negotiation_command(whispertalk::ServiceType::SIP_CLIENT, cmd);
        if (resp.empty()) {
            mg_http_reply(c, 502, "Content-Type: application/json\r\n", "{\"error\":\"SIP Client not reachable\"}");
            return;
        }

        if (resp.find("LINE_ADDED") == 0) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"success\":true,\"response\":\"%s\"}", escape_json(resp).c_str());
        } else {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"%s\"}", escape_json(resp).c_str());
        }
    }

    // POST /api/sip/remove-line — Remove a SIP registration via REMOVE_LINE command.
    void handle_sip_remove_line(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        std::string body(hm->body.buf, hm->body.len);
        std::string idx_str = extract_json_string(body, "index");
        if (idx_str.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing 'index'\"}");
            return;
        }
        for (char ch : idx_str) {
            if (!isdigit(ch)) {
                mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Index must be numeric\"}");
                return;
            }
        }

        std::string cmd = "REMOVE_LINE " + idx_str;
        std::string resp = send_negotiation_command(whispertalk::ServiceType::SIP_CLIENT, cmd);
        if (resp.empty()) {
            mg_http_reply(c, 502, "Content-Type: application/json\r\n", "{\"error\":\"SIP Client not reachable\"}");
            return;
        }

        if (resp.find("LINE_REMOVED") == 0) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"success\":true,\"response\":\"%s\"}", escape_json(resp).c_str());
        } else {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"%s\"}", escape_json(resp).c_str());
        }
    }

    // GET /api/sip/lines — Returns registered SIP lines via LIST_LINES cmd to SIP client.
    void handle_sip_lines(struct mg_connection *c) {
        std::string resp = send_negotiation_command(whispertalk::ServiceType::SIP_CLIENT, "LIST_LINES");
        if (resp.empty()) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"lines\":[],\"error\":\"SIP Client not reachable\"}");
            return;
        }

        std::stringstream json;
        json << "{\"lines\":[";

        std::istringstream iss(resp);
        std::string token;
        iss >> token;
        bool first = true;
        while (iss >> token) {
            std::vector<std::string> fields;
            std::string field;
            std::istringstream fs(token);
            while (std::getline(fs, field, ':')) fields.push_back(field);
            if (fields.size() < 3) continue;
            std::string idx_s = fields[0];
            std::string user_s = fields[1];
            std::string status_s = fields[2];
            std::string server_s = fields.size() > 3 ? fields[3] : "";
            std::string port_s = fields.size() > 4 ? fields[4] : "5060";
            std::string local_ip_s = fields.size() > 5 ? fields[5] : "";
            int idx_val = atoi(idx_s.c_str());
            int port_val = atoi(port_s.c_str());
            if (port_val < 1 || port_val > 65535) port_val = 5060;
            if (!first) json << ",";
            json << "{\"index\":" << idx_val
                 << ",\"user\":\"" << escape_json(user_s) << "\""
                 << ",\"registered\":" << (status_s == "registered" ? "true" : "false")
                 << ",\"server\":\"" << escape_json(server_s) << "\""
                 << ",\"port\":" << port_val
                 << ",\"local_ip\":\"" << escape_json(local_ip_s) << "\"}";
            first = false;
        }

        json << "]}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    struct SipCallInfo {
        uint32_t call_id = 0;
        int line_index = -1;
        uint64_t rtp_rx = 0, rtp_tx = 0, rx_bytes = 0, tx_bytes = 0;
        uint64_t duration = 0, fwd = 0, discard = 0;
    };

    struct SipStatsResult {
        bool valid = false;
        bool ds_connected = false;
        std::vector<SipCallInfo> calls;
    };

    SipStatsResult parse_sip_stats() {
        SipStatsResult result;
        std::string resp = send_negotiation_command(whispertalk::ServiceType::SIP_CLIENT, "GET_STATS");
        if (resp.empty()) return result;

        std::istringstream iss(resp);
        std::string token;
        iss >> token;
        if (token != "STATS") return result;

        int count = 0;
        iss >> count;
        result.valid = true;

        while (iss >> token) {
            if (token.size() >= 3 && token.substr(0, 3) == "DS:") {
                result.ds_connected = (token == "DS:1");
                continue;
            }
            std::vector<std::string> parts;
            size_t pos = 0;
            while (pos < token.size()) {
                size_t next = token.find(':', pos);
                if (next == std::string::npos) { parts.push_back(token.substr(pos)); break; }
                parts.push_back(token.substr(pos, next - pos));
                pos = next + 1;
            }
            if (parts.size() >= 7) {
                SipCallInfo ci;
                try {
                    ci.call_id = static_cast<uint32_t>(std::stoul(parts[0]));
                    ci.line_index = std::stoi(parts[1]);
                    ci.rtp_rx = std::stoull(parts[2]);
                    ci.rtp_tx = std::stoull(parts[3]);
                    ci.rx_bytes = std::stoull(parts[4]);
                    ci.tx_bytes = std::stoull(parts[5]);
                    ci.duration = std::stoull(parts[6]);
                    if (parts.size() >= 9) {
                        ci.fwd = std::stoull(parts[7]);
                        ci.discard = std::stoull(parts[8]);
                    }
                    result.calls.push_back(ci);
                } catch (...) {}
            }
        }
        return result;
    }

    // GET /api/sip/stats — Returns per-call RTP counters via GET_STATS cmd to SIP client.
    void handle_sip_stats(struct mg_connection *c) {
        auto stats = parse_sip_stats();
        if (!stats.valid) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"calls\":[],\"error\":\"SIP Client not reachable\"}");
            return;
        }

        std::stringstream json;
        json << "{\"calls\":[";
        bool first = true;
        for (const auto& ci : stats.calls) {
            if (!first) json << ",";
            json << "{"
                 << "\"call_id\":" << ci.call_id
                 << ",\"line_index\":" << ci.line_index
                 << ",\"rtp_rx_count\":" << ci.rtp_rx
                 << ",\"rtp_tx_count\":" << ci.rtp_tx
                 << ",\"rtp_rx_bytes\":" << ci.rx_bytes
                 << ",\"rtp_tx_bytes\":" << ci.tx_bytes
                 << ",\"duration_sec\":" << ci.duration
                 << ",\"rtp_fwd_count\":" << ci.fwd
                 << ",\"rtp_discard_count\":" << ci.discard
                 << "}";
            first = false;
        }
        json << "],\"downstream_connected\":" << (stats.ds_connected ? "true" : "false") << "}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    // Container for WAV file data loaded for IAP quality testing.
    // Stores mono int16 samples regardless of source format.
    struct IapWavData {
        std::vector<int16_t> samples;   // Mono 16-bit PCM samples
        uint32_t sample_rate = 0;       // Original sample rate (e.g. 44100, 16000)
        uint16_t channels = 0;          // Original channel count (downmixed to mono)
        std::string error;              // Non-empty if loading failed
    };

    // Loads a WAV file and converts it to mono int16 PCM samples.
    // Supports: PCM 16-bit, PCM 24-bit (downscaled to 16-bit), IEEE float 32-bit.
    // Multi-channel files are downmixed to mono by averaging all channels.
    // Returns IapWavData with .error set on failure.
    IapWavData load_wav_for_iap(const std::string& path) {
        IapWavData result;
        std::ifstream f(path, std::ios::binary);
        if (!f) { result.error = "Cannot open file"; return result; }

        char riff[12];
        f.read(riff, 12);
        if (!f || std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(riff + 8, "WAVE", 4) != 0) {
            result.error = "Invalid WAV header"; return result;
        }

        uint16_t audio_format = 0, num_channels = 0, bits_per_sample = 0;
        uint32_t sample_rate = 0;
        std::vector<uint8_t> raw_data;
        bool got_fmt = false, got_data = false;

        while (f && !got_data) {
            char chunk_id[4]; uint32_t chunk_size;
            f.read(chunk_id, 4);
            f.read(reinterpret_cast<char*>(&chunk_size), 4);
            if (!f) break;

            if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
                if (chunk_size < 16) { result.error = "fmt too small"; return result; }
                f.read(reinterpret_cast<char*>(&audio_format), 2);
                f.read(reinterpret_cast<char*>(&num_channels), 2);
                f.read(reinterpret_cast<char*>(&sample_rate), 4);
                f.seekg(6, std::ios::cur);
                f.read(reinterpret_cast<char*>(&bits_per_sample), 2);
                if (chunk_size > 16) f.seekg(chunk_size - 16, std::ios::cur);
                got_fmt = true;
            } else if (std::memcmp(chunk_id, "data", 4) == 0) {
                if (!got_fmt) { result.error = "data before fmt"; return result; }
                raw_data.resize(chunk_size);
                f.read(reinterpret_cast<char*>(raw_data.data()), chunk_size);
                got_data = true;
            } else {
                f.seekg(chunk_size, std::ios::cur);
            }
        }
        if (!got_fmt || !got_data) { result.error = "Missing fmt/data"; return result; }
        if (audio_format != 1 && audio_format != 3) { result.error = "Unsupported format"; return result; }

        result.sample_rate = sample_rate;
        result.channels = num_channels;
        size_t frame_size = num_channels * (bits_per_sample / 8);
        if (frame_size == 0) { result.error = "Invalid bits"; return result; }
        size_t num_frames = raw_data.size() / frame_size;
        result.samples.reserve(num_frames);

        for (size_t i = 0; i < num_frames; i++) {
            const uint8_t* frame = raw_data.data() + i * frame_size;
            int32_t mono_sum = 0;
            for (uint16_t ch = 0; ch < num_channels; ch++) {
                const uint8_t* s = frame + ch * (bits_per_sample / 8);
                int32_t val = 0;
                if (audio_format == 1 && bits_per_sample == 16) {
                    val = static_cast<int16_t>(s[0] | (s[1] << 8));
                } else if (audio_format == 1 && bits_per_sample == 24) {
                    val = static_cast<int32_t>((s[0] << 8) | (s[1] << 16) | (s[2] << 24)) >> 16;
                } else if (audio_format == 3 && bits_per_sample == 32) {
                    float fval; std::memcpy(&fval, s, 4);
                    val = static_cast<int32_t>(fval * 32767.0f);
                    if (val > 32767) val = 32767; if (val < -32768) val = -32768;
                } else {
                    result.error = "Unsupported bits"; result.samples.clear(); return result;
                }
                mono_sum += val;
            }
            result.samples.push_back(static_cast<int16_t>(mono_sum / num_channels));
        }
        return result;
    }

    // Encode a 16-bit linear PCM sample to 8-bit G.711 mu-law (ITU-T G.711).
    // Measured encode/decode roundtrip SNR: ~36-38 dB for speech signals.
    static uint8_t linear_to_ulaw(int16_t sample) {
        const int BIAS = 0x84;
        const int CLIP = 32635;
        int sign = (sample >> 8) & 0x80;
        if (sign) sample = static_cast<int16_t>(-(int)sample);
        if (sample > CLIP) sample = CLIP;
        sample += BIAS;
        int exponent = 7;
        for (int mask = 0x4000; mask > 0; mask >>= 1, exponent--) {
            if (sample & mask) break;
        }
        int mantissa = (sample >> (exponent + 3)) & 0x0F;
        return ~(sign | (exponent << 4) | mantissa);
    }

    // Decode an 8-bit G.711 mu-law byte to a float32 sample in [-1.0, 1.0].
    // ITU-T G.711 compliant: invert all bits, extract sign (bit 7),
    // exponent/segment (bits 6-4), mantissa/quantization (bits 3-0).
    // Reconstruct magnitude: ((2*mantissa + 33) << (segment + 2)) - 132.
    // Same formula as inbound-audio-processor.cpp init_g711_tables().
    static float ulaw_to_float(uint8_t byte) {
        int mu = ~byte;
        int sign = mu & 0x80;
        int segment = (mu >> 4) & 0x07;
        int quantization = mu & 0x0F;
        int magnitude = ((quantization << 1) + 33) << (segment + 2);
        magnitude -= 132;
        return (sign ? -magnitude : magnitude) / 32768.0f;
    }

    // Resample int16 audio using linear interpolation.
    // Used to convert from source sample rate (e.g. 44100Hz) to target (e.g. 8000Hz).
    static std::vector<int16_t> resample_linear(const std::vector<int16_t>& src, uint32_t src_rate, uint32_t dst_rate) {
        if (src_rate == dst_rate) return src;
        double ratio = static_cast<double>(src_rate) / dst_rate;
        size_t out_len = static_cast<size_t>(src.size() / ratio);
        std::vector<int16_t> out(out_len);
        for (size_t i = 0; i < out_len; i++) {
            double pos = i * ratio;
            size_t idx = static_cast<size_t>(pos);
            double frac = pos - idx;
            int32_t s0 = (idx < src.size()) ? src[idx] : 0;
            int32_t s1 = (idx + 1 < src.size()) ? src[idx + 1] : s0;
            int32_t val = static_cast<int32_t>(s0 + frac * (s1 - s0));
            if (val > 32767) val = 32767; if (val < -32768) val = -32768;
            out[i] = static_cast<int16_t>(val);
        }
        return out;
    }

    // IAP Codec Quality Test endpoint (POST /api/iap/quality_test).
    //
    // Tests the G.711 mu-law encode/decode pipeline used by the Inbound Audio
    // Processor (IAP). Runs the exact same algorithm offline:
    //   1. Load WAV file and resample to 8kHz (telephony sample rate)
    //   2. Encode each sample to G.711 mu-law (simulating RTP payload encoding)
    //   3. Decode mu-law to float32 (using ITU-T G.711 compliant lookup)
    //   4. Upsample 8kHz to 16kHz via shared 15-tap FIR half-band filter
    //      (whispertalk::iap_fir_upsample_frame from interconnect.h — same
    //       code path used by the real IAP service)
    //   5. Build a reference 16kHz signal by FIR-upsampling the original 8kHz
    //   6. Compare codec output vs reference to measure distortion
    //
    // NOTE: This is a codec algorithm quality test, NOT an IAP service
    // integration test. It validates the conversion algorithm quality without
    // requiring the IAP service to be running. Service integration (TCP,
    // reconnection, RTP forwarding) is tested separately via Test 1.
    //
    // Metrics:
    //   SNR (Signal-to-Noise Ratio): 10*log10(signal_power/noise_power) in dB.
    //     Noise is the quantization error from the mu-law encode/decode
    //     roundtrip. Expected: ~36-38 dB for speech through G.711 mu-law.
    //   RMS Error %: 100 * sqrt(noise_power/signal_power). Normalized root-
    //     mean-square error between reference and codec output. Expected: ~1-2%.
    //   Per-Packet Latency: Time to process a single 160-sample (20ms) frame
    //     through G.711 decode + FIR upsample, measured per-frame and averaged.
    //     Expected: < 50us on Apple Silicon (target < 15ms, max 50ms).
    //
    // Pass criteria: SNR >= 25 dB, RMS Error <= 10%, Per-Pkt Latency <= 50ms
    void handle_iap_quality_test(struct mg_connection *c, struct mg_http_message *hm) {
        std::string body(hm->body.buf, hm->body.len);
        std::string file = extract_json_string(body, "file");

        if (file.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing file parameter\"}");
            return;
        }

        if (file.find("..") != std::string::npos || file[0] == '/') {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Invalid file path\"}");
            return;
        }

        std::string wav_path = "Testfiles/" + file;
        IapWavData wav = load_wav_for_iap(wav_path);
        if (!wav.error.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                "{\"error\":\"WAV load failed: %s\"}", escape_json(wav.error).c_str());
            return;
        }
        if (wav.samples.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"WAV file is empty\"}");
            return;
        }

        std::vector<int16_t> samples_8k = resample_linear(wav.samples, wav.sample_rate, 8000);

        std::vector<uint8_t> ulaw_data(samples_8k.size());
        for (size_t i = 0; i < samples_8k.size(); i++) {
            ulaw_data[i] = linear_to_ulaw(samples_8k[i]);
        }

        float ulaw_table[256];
        for (int i = 0; i < 256; i++) ulaw_table[i] = ulaw_to_float((uint8_t)i);

        size_t n_samples = ulaw_data.size();
        std::vector<float> decoded(n_samples);

        constexpr size_t frame_size = whispertalk::IAP_ULAW_FRAME;
        std::vector<float> iap_output(n_samples * 2, 0.0f);
        float fir_history[whispertalk::IAP_FIR_CENTER] = {};

        double pkt_latency_sum = 0.0;
        double pkt_latency_max = 0.0;
        size_t pkt_count = 0;

        for (size_t offset = 0; offset < n_samples; offset += frame_size) {
            size_t chunk = std::min(frame_size, n_samples - offset);

            auto t0 = std::chrono::steady_clock::now();
            for (size_t i = 0; i < chunk; i++)
                decoded[offset + i] = ulaw_table[ulaw_data[offset + i]];
            whispertalk::iap_fir_upsample_frame(
                &decoded[offset], chunk, &iap_output[offset * 2], fir_history);
            auto t1 = std::chrono::steady_clock::now();

            double pkt_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            pkt_latency_sum += pkt_us;
            if (pkt_us > pkt_latency_max) pkt_latency_max = pkt_us;
            pkt_count++;
        }

        double avg_pkt_latency_us = pkt_count > 0 ? pkt_latency_sum / pkt_count : 0.0;
        double avg_pkt_latency_ms = avg_pkt_latency_us / 1000.0;
        double max_pkt_latency_ms = pkt_latency_max / 1000.0;

        std::vector<float> ref_8k_float(n_samples);
        for (size_t i = 0; i < n_samples; i++) {
            ref_8k_float[i] = samples_8k[i] / 32768.0f;
        }
        std::vector<float> ref_16k(n_samples * 2, 0.0f);
        float ref_hist[whispertalk::IAP_FIR_CENTER] = {};
        for (size_t offset = 0; offset < n_samples; offset += frame_size) {
            size_t chunk = std::min(frame_size, n_samples - offset);
            whispertalk::iap_fir_upsample_frame(
                &ref_8k_float[offset], chunk, &ref_16k[offset * 2], ref_hist);
        }

        size_t compare_len = std::min(ref_16k.size(), iap_output.size());
        if (compare_len < 2) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"No samples to compare\"}");
            return;
        }

        double signal_power = 0.0, noise_power = 0.0;
        for (size_t i = 0; i < compare_len; i++) {
            double orig = ref_16k[i];
            double proc = iap_output[i];
            signal_power += orig * orig;
            noise_power += (orig - proc) * (orig - proc);
        }
        signal_power /= compare_len;
        noise_power /= compare_len;

        double snr_db = (noise_power > 1e-15) ? 10.0 * log10(signal_power / noise_power) : 99.0;

        double rms_error_pct = 0.0;
        if (signal_power > 1e-15) {
            rms_error_pct = 100.0 * sqrt(noise_power / signal_power);
            if (rms_error_pct > 100.0) rms_error_pct = 100.0;
        }

        std::string status = (snr_db >= 25.0 && rms_error_pct <= 10.0 && max_pkt_latency_ms <= 50.0) ? "PASS" : "FAIL";

        std::string sql = "INSERT INTO iap_quality_tests (file_name, latency_ms, snr_db, rms_error_pct, max_latency_ms, status, timestamp) "
                         "VALUES (?, ?, ?, ?, ?, ?, ?)";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, file.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 2, avg_pkt_latency_ms);
            sqlite3_bind_double(stmt, 3, snr_db);
            sqlite3_bind_double(stmt, 4, rms_error_pct);
            sqlite3_bind_double(stmt, 5, max_pkt_latency_ms);
            sqlite3_bind_text(stmt, 6, status.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 7, time(nullptr));
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        std::stringstream json;
        json << std::fixed;
        json << "{"
             << "\"success\":true,"
             << "\"file\":\"" << escape_json(file) << "\","
             << "\"latency_ms\":" << avg_pkt_latency_ms << ","
             << "\"max_latency_ms\":" << max_pkt_latency_ms << ","
             << "\"snr\":" << snr_db << ","
             << "\"rms_error\":" << rms_error_pct << ","
             << "\"pkt_count\":" << pkt_count << ","
             << "\"samples_compared\":" << compare_len << ","
             << "\"status\":\"" << status << "\""
             << "}";

        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    // GET /api/whisper/models — Lists .bin GGML model files in bin/models/ directory.
    void handle_whisper_models(struct mg_connection *c) {
        std::stringstream json;
        json << "{\"models\":[";
        DIR* dir = opendir("bin/models");
        bool first = true;
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string name = entry->d_name;
                if (name.size() > 4 && name.substr(name.size() - 4) == ".bin") {
                    if (!first) json << ",";
                    json << "\"bin/models/" << escape_json(name) << "\"";
                    first = false;
                }
            }
            closedir(dir);
        }
        std::string current_args;
        {
            std::lock_guard<std::mutex> lock(services_mutex_);
            for (auto& svc : services_) {
                if (svc.name == "WHISPER_SERVICE") { current_args = svc.default_args; break; }
            }
        }
        json << "],\"current_args\":\"" << escape_json(current_args) << "\"";
        json << ",\"languages\":[\"de\",\"en\",\"fr\",\"es\",\"it\",\"pt\",\"nl\",\"pl\",\"ru\",\"ja\",\"zh\",\"auto\"]";
        json << "}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    // Normalizes text for Levenshtein comparison:
    //  1. Decompose German UTF-8 umlauts: ä→ae, ö→oe, ü→ue, ß→ss, Ä→ae, Ö→oe, Ü→ue
    //  2. Lowercase all ASCII characters
    //  3. Replace German number words with digit equivalents (longest-first to prevent partial matches)
    //  4. Collapse whitespace, strip punctuation
    // This ensures Whisper's digit output ("2023") matches ground truth number words ("zweitausenddreiundzwanzig").
    std::string normalize_for_comparison(const std::string& input) {
        // Step 1: UTF-8 umlaut decomposition + lowercase + strip punctuation
        std::string s;
        for (size_t i = 0; i < input.size(); i++) {
            unsigned char c = input[i];
            if (c == 0xC3 && i + 1 < input.size()) {
                unsigned char c2 = input[i + 1];
                switch (c2) {
                    case 0xA4: case 0x84: s += "ae"; break;  // ä, Ä
                    case 0xB6: case 0x96: s += "oe"; break;  // ö, Ö
                    case 0xBC: case 0x9C: s += "ue"; break;  // ü, Ü
                    case 0x9F:            s += "ss"; break;  // ß
                    default: break;
                }
                i++;
            } else if (c == 0xE2 && i + 2 < input.size()) {
                i += 2;
                s += ' ';
            } else if (c < 0x80) {
                char lc = std::tolower(c);
                if (std::isalnum(lc) || lc == ' ') {
                    s += lc;
                } else {
                    s += ' ';
                }
            }
        }

        // Step 2: Replace German number words with digits (longest first)
        static const std::pair<std::string, std::string> number_map[] = {
            {"zweitausendfuenfzehn", "2015"},
            {"zweitausendneunzehn", "2019"},
            {"zweitausenddreiundzwanzig", "2023"},
            {"zweitausendsiebenundachtzig", "2087"},
            {"vierhundertfuenfundneunzig", "495"},
            {"dreihundertfuenfzig", "350"},
            {"neunundzwanzig", "29"},
            {"siebenundsechzig", "67"},
            {"neunundvierzig", "49"},
            {"fuenfzehn", "15"},
            {"neunzehn", "19"},
            {"dreiundzwanzig", "23"},
            {"siebenundachtzig", "87"},
            {"hundert", "100"},
            {"tausend", "1000"},
        };
        for (const auto& [word, digit] : number_map) {
            size_t pos;
            while ((pos = s.find(word)) != std::string::npos) {
                s.replace(pos, word.length(), digit);
            }
        }

        // Step 3: Normalize age patterns (e.g., "49er" → "49", "49 jaehrige" → "49 jaehrige")
        {
            std::string tmp;
            for (size_t i = 0; i < s.size(); i++) {
                if (i > 0 && std::isdigit(s[i-1]) && s[i] == 'e' && i+1 < s.size() && s[i+1] == 'r') {
                    if (i+2 >= s.size() || s[i+2] == ' ') {
                        i += 1;
                        continue;
                    }
                }
                tmp += s[i];
            }
            s = tmp;
        }

        // Step 4: Collapse whitespace
        std::string result;
        bool last_space = false;
        for (char c : s) {
            if (c == ' ') { if (!last_space && !result.empty()) { result += ' '; last_space = true; } }
            else { result += c; last_space = false; }
        }
        while (!result.empty() && result.back() == ' ') result.pop_back();
        return result;
    }

    double calculate_levenshtein_similarity(const std::string& s1, const std::string& s2) {
        std::string n1 = normalize_for_comparison(s1);
        std::string n2 = normalize_for_comparison(s2);

        if (n1.empty() && n2.empty()) return 100.0;
        if (n1.empty() || n2.empty()) return 0.0;

        size_t len1 = n1.length(), len2 = n2.length();
        if (len1 < len2) { std::swap(n1, n2); std::swap(len1, len2); }
        std::vector<size_t> prev(len2 + 1), curr(len2 + 1);

        for (size_t j = 0; j <= len2; j++) prev[j] = j;

        for (size_t i = 1; i <= len1; i++) {
            curr[0] = i;
            for (size_t j = 1; j <= len2; j++) {
                size_t cost = (n1[i - 1] == n2[j - 1]) ? 0 : 1;
                curr[j] = std::min({
                    prev[j] + 1,
                    curr[j - 1] + 1,
                    prev[j - 1] + cost
                });
            }
            std::swap(prev, curr);
        }

        size_t distance = prev[len2];
        size_t max_len = std::max(len1, len2);
        double similarity = (1.0 - (double)distance / max_len) * 100.0;
        return similarity;
    }

    double calculate_word_error_rate(const std::string& reference, const std::string& hypothesis) {
        std::string n_ref = normalize_for_comparison(reference);
        std::string n_hyp = normalize_for_comparison(hypothesis);

        auto split_words = [](const std::string& s) -> std::vector<std::string> {
            std::vector<std::string> words;
            std::istringstream iss(s);
            std::string w;
            while (iss >> w) { if (!w.empty()) words.push_back(w); }
            return words;
        };

        std::vector<std::string> ref_words = split_words(n_ref);
        std::vector<std::string> hyp_words = split_words(n_hyp);

        if (ref_words.empty() && hyp_words.empty()) return 0.0;
        if (ref_words.empty()) return 100.0;

        size_t m = ref_words.size(), n = hyp_words.size();
        std::vector<size_t> prev(n + 1), curr(n + 1);
        for (size_t j = 0; j <= n; j++) prev[j] = j;

        for (size_t i = 1; i <= m; i++) {
            curr[0] = i;
            for (size_t j = 1; j <= n; j++) {
                size_t cost = (ref_words[i - 1] == hyp_words[j - 1]) ? 0 : 1;
                curr[j] = std::min({prev[j] + 1, curr[j - 1] + 1, prev[j - 1] + cost});
            }
            std::swap(prev, curr);
        }

        return (double)prev[n] / (double)m * 100.0;
    }

    // Makes a simple HTTP POST request to localhost:<port><path> with JSON body.
    // Returns the response body string, or sets error on failure.
    // Used to communicate with test_sip_provider for audio injection.
    std::string http_post_localhost(int port, const std::string& path,
                                   const std::string& json_body, std::string& error) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) { error = "socket() failed"; return ""; }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        struct timeval tv{5, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            error = "connect() failed — is test_sip_provider running on port " + std::to_string(port) + "?";
            return "";
        }

        std::string req = "POST " + path + " HTTP/1.1\r\n"
                          "Host: localhost:" + std::to_string(port) + "\r\n"
                          "Content-Type: application/json\r\n"
                          "Content-Length: " + std::to_string(json_body.size()) + "\r\n"
                          "Connection: close\r\n\r\n" + json_body;
        if (send(sock, req.c_str(), req.size(), 0) < 0) {
            close(sock);
            error = "send() failed";
            return "";
        }

        std::string response;
        char buf[4096];
        while (true) {
            ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            response.append(buf, n);
        }
        close(sock);

        size_t body_start = response.find("\r\n\r\n");
        if (body_start == std::string::npos) {
            error = "Malformed HTTP response";
            return "";
        }
        return response.substr(body_start + 4);
    }

    // Makes a simple HTTP GET request to localhost:<port><path>.
    std::string http_get_localhost(int port, const std::string& path, std::string& error) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) { error = "socket() failed"; return ""; }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        struct timeval tv{5, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            error = "connect() failed — is test_sip_provider running on port " + std::to_string(port) + "?";
            return "";
        }

        std::string req = "GET " + path + " HTTP/1.1\r\n"
                          "Host: localhost:" + std::to_string(port) + "\r\n"
                          "Connection: close\r\n\r\n";
        if (send(sock, req.c_str(), req.size(), 0) < 0) {
            close(sock);
            error = "send() failed";
            return "";
        }

        std::string response;
        char buf[4096];
        while (true) {
            ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            response.append(buf, n);
        }
        close(sock);

        size_t body_start = response.find("\r\n\r\n");
        if (body_start == std::string::npos) {
            error = "Malformed HTTP response";
            return "";
        }
        return response.substr(body_start + 4);
    }

    std::string tcp_command(int port, const std::string& cmd, std::string& error, int timeout_s = 15) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) { error = "socket() failed"; return ""; }

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        struct timeval tv{timeout_s, 0};
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            error = "connect() failed — is service running on port " + std::to_string(port) + "?";
            return "";
        }

        if (send(sock, cmd.c_str(), cmd.size(), 0) < 0) {
            close(sock);
            error = "send() failed";
            return "";
        }

        std::string response;
        char buf[4096];
        while (true) {
            ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
            if (n <= 0) break;
            response.append(buf, n);
        }
        close(sock);
        while (!response.empty() && (response.back() == '\n' || response.back() == '\r'))
            response.pop_back();
        return response;
    }

    struct TranscriptionResult {
        std::string text;
        double whisper_latency_ms = 0.0;
        bool found = false;
    };

    // Waits for Whisper transcription(s) after the given log sequence number.
    // With chunked VAD, a single audio file may produce multiple transcription chunks.
    // This function collects all chunks by waiting for transcription activity to settle:
    //   1. Wait until the first transcription appears (up to timeout_ms)
    //   2. After finding a transcription, keep waiting for 5s of inactivity
    //   3. Concatenate all transcription chunks in chronological order
    // Returns the combined text and total Whisper inference latency.
    TranscriptionResult wait_for_whisper_transcription(
            uint64_t after_seq, int timeout_ms = 30000, uint32_t filter_call_id = 0) {
        TranscriptionResult result;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        auto settle_deadline = deadline;
        bool found_any = false;
        uint64_t last_found_seq = after_seq;

        while (std::chrono::steady_clock::now() < settle_deadline) {
            bool found_new = false;
            {
                std::lock_guard<std::mutex> lock(logs_mutex_);
                for (const auto& entry : recent_logs_) {
                    if (entry.seq <= last_found_seq) continue;
                    if (entry.service != ServiceType::WHISPER_SERVICE) continue;
                    if (filter_call_id > 0 && entry.call_id != filter_call_id) continue;

                    const std::string& msg = entry.message;
                    size_t tpos = msg.find("Transcription (");
                    if (tpos == std::string::npos) continue;

                    size_t ms_start = tpos + 15;
                    size_t ms_end = msg.find("ms", ms_start);
                    if (ms_end == std::string::npos) continue;

                    size_t paren_close = msg.find("):", ms_end);
                    if (paren_close == std::string::npos) continue;

                    size_t text_start = paren_close + 2;
                    if (text_start >= msg.size()) continue;

                    double chunk_latency = std::stod(msg.substr(ms_start, ms_end - ms_start));
                    std::string chunk_text = msg.substr(text_start);
                    while (!chunk_text.empty() && (chunk_text.front() == ' ' || chunk_text.front() == ':'))
                        chunk_text.erase(chunk_text.begin());
                    while (!chunk_text.empty() && (chunk_text.back() == ' ' || chunk_text.back() == '\n'))
                        chunk_text.pop_back();

                    if (!chunk_text.empty()) {
                        if (!result.text.empty()) result.text += " ";
                        result.text += chunk_text;
                        result.whisper_latency_ms += chunk_latency;
                        last_found_seq = entry.seq;
                        found_new = true;
                        found_any = true;
                    }
                }
            }

            if (found_new) {
                settle_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(TRANSCRIPTION_SETTLE_MS);
                if (settle_deadline > deadline) settle_deadline = deadline;
            } else if (!found_any && std::chrono::steady_clock::now() >= deadline) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(TRANSCRIPTION_POLL_MS));
        }

        result.found = found_any;
        if (!found_any) {
            std::cerr << "No Whisper transcription received within " 
                      << timeout_ms << "ms for seq > " << after_seq 
                      << " (current seq: " << log_seq_.load() << ")\n";
        }
        return result;
    }

    uint64_t current_log_seq() {
        return log_seq_.load();
    }

    struct LlamaResponseResult {
        std::string text;
        double gen_ms = 0.0;
        bool found = false;
    };

    LlamaResponseResult wait_for_llama_response(uint64_t after_seq, int timeout_ms = 30000) {
        LlamaResponseResult result;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

        while (std::chrono::steady_clock::now() < deadline) {
            {
                std::lock_guard<std::mutex> lock(logs_mutex_);
                for (const auto& entry : recent_logs_) {
                    if (entry.seq <= after_seq) continue;
                    if (entry.service != ServiceType::LLAMA_SERVICE) continue;
                    if (entry.level != "INFO") continue;

                    const std::string& msg = entry.message;
                    size_t rpos = msg.find("Response (");
                    if (rpos == std::string::npos) continue;

                    size_t ms_start = rpos + 10;
                    size_t ms_end = msg.find("ms)", ms_start);
                    if (ms_end == std::string::npos) continue;

                    try {
                        result.gen_ms = std::stod(msg.substr(ms_start, ms_end - ms_start));
                    } catch (...) {}

                    size_t text_start = ms_end + 5;
                    if (text_start < msg.size()) {
                        result.text = msg.substr(text_start);
                        while (!result.text.empty() && (result.text.back() == ' ' || result.text.back() == '\n'))
                            result.text.pop_back();
                    }
                    result.found = true;
                    return result;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(LLAMA_RESPONSE_POLL_MS));
        }
        return result;
    }

    // Helper function to check if a service is currently running.
    // Returns true if the service exists in services_ list and has a valid PID.
    bool is_service_running(const std::string& service_name) {
        std::lock_guard<std::mutex> lock(services_mutex_);
        for (const auto& svc : services_) {
            if (svc.name == service_name && svc.pid > 0) {
                // Verify PID is still alive
                if (kill(svc.pid, 0) == 0) {
                    return true;
                }
            }
        }
        return false;
    }

    // Validates that all required pipeline services are running.
    // Returns error message if any service is down, empty string if all OK.
    std::string validate_pipeline_services() {
        if (!is_service_running("SIP_CLIENT")) {
            return "SIP Client (sip-client-main) is not running. Start it first.";
        }
        if (!is_service_running("INBOUND_AUDIO_PROCESSOR")) {
            return "Inbound Audio Processor (inbound-audio-processor) is not running. Start it first.";
        }
        if (!is_service_running("VAD_SERVICE")) {
            return "VAD Service (vad-service) is not running. Start it first.";
        }
        if (!is_service_running("WHISPER_SERVICE")) {
            return "Whisper Service (whisper-service) is not running. Start it first.";
        }
        return "";
    }

    // Gets the actual RSS (Resident Set Size) memory usage in MB for a running service.
    // Returns 0 if the service is not running or memory cannot be determined.
    // Uses ps command on macOS to get RSS memory.
    int get_service_memory_mb(const std::string& service_name) {
        pid_t pid = 0;
        {
            std::lock_guard<std::mutex> lock(services_mutex_);
            for (const auto& svc : services_) {
                if (svc.name == service_name && svc.pid > 0) {
                    if (kill(svc.pid, 0) == 0) {
                        pid = svc.pid;
                        break;
                    }
                }
            }
        }
        
        if (pid == 0) return 0;
        
        // Use ps to get RSS in KB, then convert to MB
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "ps -o rss= -p %d 2>/dev/null", pid);
        FILE* fp = popen(cmd, "r");
        if (!fp) return 0;
        
        char buf[128];
        int rss_kb = 0;
        if (fgets(buf, sizeof(buf), fp)) {
            rss_kb = atoi(buf);
        }
        pclose(fp);
        
        return rss_kb / 1024;  // Convert KB to MB
    }

    // GET /api/llama/prompts — Returns test prompts from Testfiles/llama_prompts.json.
    void handle_llama_prompts(struct mg_connection *c) {
        std::string prompts_path = "Testfiles/llama_prompts.json";
        FILE* fp = fopen(prompts_path.c_str(), "r");
        if (!fp) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"prompts\":[]}");
            return;
        }
        std::string content;
        char buf[4096];
        while (size_t n = fread(buf, 1, sizeof(buf), fp)) content.append(buf, n);
        fclose(fp);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"prompts\":%s}", content.c_str());
    }

    struct QualityPrompt {
        int id;
        std::string prompt;
        std::vector<std::string> expected_keywords;
        std::string category;
        int max_words;
    };

    std::vector<QualityPrompt> parse_llama_prompts(struct mg_str json_body) {
        std::vector<QualityPrompt> prompts;
        int prompts_len = 0;
        int prompts_ofs = mg_json_get(json_body, "$.prompts", &prompts_len);
        if (prompts_ofs < 0) return prompts;
        struct mg_str arr = mg_str_n(json_body.buf + prompts_ofs, (size_t)prompts_len);
        struct mg_str key, val;
        size_t ofs = 0;
        while ((ofs = mg_json_next(arr, ofs, &key, &val)) > 0) {
            if (val.len < 2 || val.buf[0] != '{') continue;
            QualityPrompt pi;
            pi.id = (int)mg_json_get_long(val, "$.id", 0);
            int plen = 0;
            int pofs = mg_json_get(val, "$.prompt", &plen);
            if (pofs >= 0 && plen >= 2) {
                char pbuf[1024];
                if (mg_json_unescape(mg_str_n(val.buf + pofs + 1, plen - 2), pbuf, sizeof(pbuf)))
                    pi.prompt = pbuf;
            }
            pi.max_words = (int)mg_json_get_long(val, "$.max_words", 30);
            int clen = 0;
            int cofs = mg_json_get(val, "$.category", &clen);
            if (cofs >= 0 && clen >= 2) {
                char cbuf[64];
                if (mg_json_unescape(mg_str_n(val.buf + cofs + 1, clen - 2), cbuf, sizeof(cbuf)))
                    pi.category = cbuf;
            }
            int klen = 0;
            int kofs = mg_json_get(val, "$.expected_keywords", &klen);
            if (kofs >= 0) {
                struct mg_str karr = mg_str_n(val.buf + kofs, (size_t)klen);
                struct mg_str kk, kv;
                size_t ko = 0;
                while ((ko = mg_json_next(karr, ko, &kk, &kv)) > 0) {
                    if (kv.len >= 2 && kv.buf[0] == '"') {
                        char kbuf[256];
                        if (mg_json_unescape(mg_str_n(kv.buf + 1, kv.len - 2), kbuf, sizeof(kbuf)))
                            pi.expected_keywords.push_back(kbuf);
                    }
                }
            }
            if (!pi.prompt.empty()) prompts.push_back(pi);
        }
        return prompts;
    }

    void handle_llama_set_sampling(struct mg_connection *c, struct mg_http_message *hm) {
        double temp = 0.3, top_p = 0.95;
        mg_json_get_num(hm->body, "$.temperature", &temp);
        mg_json_get_num(hm->body, "$.top_p", &top_p);
        uint16_t llama_cmd_port = whispertalk::service_cmd_port(whispertalk::ServiceType::LLAMA_SERVICE);
        std::string err;
        std::string cmd = "SET_SAMPLING:temp=" + std::to_string((float)temp) + ",top_p=" + std::to_string((float)top_p);
        std::string resp = tcp_command(llama_cmd_port, cmd, err, 3);
        if (resp.rfind("OK", 0) == 0) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                "{\"status\":\"ok\",\"temperature\":%.2f,\"top_p\":%.2f}", temp, top_p);
        } else {
            mg_http_reply(c, 502, "Content-Type: application/json\r\n",
                "{\"error\":\"LLaMA service unreachable: %s\"}", escape_json(err.empty() ? resp : err).c_str());
        }
    }

    // POST /api/llama/quality_test — Async LLaMA quality test. Sends each prompt to
    // LLaMA's data port and scores the response via score_llama_response(). Returns task_id.
    void handle_llama_quality_test(struct mg_connection *c, struct mg_http_message *hm) {
        std::vector<QualityPrompt> prompts = parse_llama_prompts(hm->body);
        if (prompts.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"No prompts provided\"}");
            return;
        }

        int64_t task_id = create_async_task("llama_quality_test");
        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            auto& task = async_tasks_[task_id];
            task->worker = std::thread(&FrontendServer::run_llama_quality_test_async, this,
                task_id, std::move(prompts));
        }
        mg_http_reply(c, 202, "Content-Type: application/json\r\n", "{\"task_id\":%lld}", (long long)task_id);
    }

    void run_llama_quality_test_async(int64_t task_id, std::vector<QualityPrompt> prompts) {
        uint16_t llama_cmd_port = whispertalk::service_cmd_port(whispertalk::ServiceType::LLAMA_SERVICE);

        std::string ping_err;
        if (tcp_command(llama_cmd_port, "PING", ping_err, 3) != "PONG") {
            finish_async_task(task_id, "{\"error\":\"LLaMA service not reachable (port "
                + std::to_string(llama_cmd_port) + "): " + escape_json(ping_err) + "\"}");
            return;
        }

        std::string results_json = "[";
        double total_score = 0, total_latency = 0;
        int german_count = 0;

        for (size_t i = 0; i < prompts.size(); i++) {
            const auto& p = prompts[i];
            std::string err;
            std::string resp = tcp_command(llama_cmd_port, "TEST_PROMPT:" + p.prompt, err, 15);

            std::string response_text;
            double latency_ms = 0;
            if (resp.rfind("RESPONSE:", 0) == 0) {
                size_t ms_end = resp.find("ms:", 9);
                if (ms_end != std::string::npos) {
                    latency_ms = std::stod(resp.substr(9, ms_end - 9));
                    response_text = resp.substr(ms_end + 3);
                }
            } else {
                response_text = "(no response)";
            }

            LlamaScoreResult sr = score_llama_response(response_text, p.expected_keywords, p.max_words);
            total_score += sr.score;
            total_latency += latency_ms;
            if (sr.is_german) german_count++;

            if (i > 0) results_json += ",";
            results_json += "{\"prompt\":\"" + escape_json(p.prompt)
                + "\",\"response\":\"" + escape_json(response_text)
                + "\",\"latency_ms\":" + std::to_string((int)latency_ms)
                + ",\"word_count\":" + std::to_string(sr.word_count)
                + ",\"max_words\":" + std::to_string(p.max_words)
                + ",\"keywords_found\":" + std::to_string(sr.keywords_found)
                + ",\"keywords_total\":" + std::to_string((int)p.expected_keywords.size())
                + ",\"is_german\":" + (sr.is_german ? "true" : "false")
                + ",\"score\":" + std::to_string((int)sr.score) + "}";

            if (db_) {
                const char* sql = "INSERT INTO test_results (test_name,service,status,details,timestamp) "
                    "VALUES ('llama_quality',?1,?2,?3,strftime('%s','now'))";
                sqlite3_stmt* stmt = nullptr;
                if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, "llama", -1, SQLITE_STATIC);
                    std::string status_str = sr.score >= 80 ? "pass" : sr.score >= 50 ? "warn" : "fail";
                    sqlite3_bind_text(stmt, 2, status_str.c_str(), -1, SQLITE_TRANSIENT);
                    std::string details = p.prompt + " -> " + response_text + " (" + std::to_string((int)latency_ms) + "ms, score:" + std::to_string((int)sr.score) + "%)";
                    sqlite3_bind_text(stmt, 3, details.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
            }
        }
        results_json += "]";

        double avg_score = prompts.empty() ? 0 : total_score / prompts.size();
        double avg_latency = prompts.empty() ? 0 : total_latency / prompts.size();
        double german_pct = prompts.empty() ? 0 : (german_count * 100.0 / prompts.size());

        std::string result = "{\"results\":" + results_json
            + ",\"summary\":{\"avg_score\":" + std::to_string(avg_score)
            + ",\"avg_latency_ms\":" + std::to_string((int)avg_latency)
            + ",\"german_pct\":" + std::to_string((int)german_pct) + "}}";
        finish_async_task(task_id, result);
    }

    // POST /api/llama/shutup_test — Async test: sends a long prompt, then fires
    // SPEECH_ACTIVE to test interrupt latency. Measures time to abort generation.
    void handle_llama_shutup_test(struct mg_connection *c, struct mg_http_message *hm) {
        struct mg_str json_body = hm->body;
        int plen = 0;
        int pofs = mg_json_get(json_body, "$.prompt", &plen);
        std::string prompt = "Erzähl mir eine lange Geschichte über einen Ritter.";
        if (pofs >= 0 && plen >= 2) {
            char pbuf[1024];
            if (mg_json_unescape(mg_str_n(json_body.buf + pofs + 1, plen - 2), pbuf, sizeof(pbuf)))
                prompt = pbuf;
        }

        int64_t task_id = create_async_task("llama_shutup_test");
        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            auto& task = async_tasks_[task_id];
            task->worker = std::thread(&FrontendServer::run_llama_shutup_test_async, this,
                task_id, prompt);
        }
        mg_http_reply(c, 202, "Content-Type: application/json\r\n", "{\"task_id\":%lld}", (long long)task_id);
    }

    void run_llama_shutup_test_async(int64_t task_id, std::string prompt) {
        uint16_t llama_cmd_port = whispertalk::service_cmd_port(whispertalk::ServiceType::LLAMA_SERVICE);

        std::string ping_err;
        if (tcp_command(llama_cmd_port, "PING", ping_err, 3) != "PONG") {
            finish_async_task(task_id, "{\"error\":\"LLaMA service not reachable\"}");
            return;
        }

        std::string err;
        std::string resp = tcp_command(llama_cmd_port, "SHUTUP_TEST:" + prompt, err, 15);

        if (resp.empty()) {
            finish_async_task(task_id, "{\"error\":\"LLaMA service not reachable\"}");
            return;
        }

        double interrupt_ms = 0, total_ms = 0;
        if (resp.rfind("SHUTUP_RESULT:", 0) == 0) {
            size_t p1 = resp.find("ms:", 14);
            if (p1 != std::string::npos) {
                interrupt_ms = std::stod(resp.substr(14, p1 - 14));
                size_t p2 = resp.find("ms", p1 + 3);
                if (p2 != std::string::npos) {
                    total_ms = std::stod(resp.substr(p1 + 3, p2 - (p1 + 3)));
                }
            }
        }

        char result[256];
        snprintf(result, sizeof(result),
            "{\"interrupt_latency_ms\":%.0f,\"total_ms\":%.0f}", interrupt_ms, total_ms);
        finish_async_task(task_id, result);
    }

    // POST /api/tests/shutup_pipeline — Full pipeline shut-up test with configurable
    // scenarios (basic, early, late, rapid). Tests SPEECH_ACTIVE propagation end-to-end.
    void handle_shutup_pipeline_test(struct mg_connection *c, struct mg_http_message *hm) {
        struct mg_str json_body = hm->body;
        std::vector<std::string> scenarios;
        for (int i = 0; ; i++) {
            char path[64];
            snprintf(path, sizeof(path), "$.scenarios[%d]", i);
            int vlen = 0;
            int vofs = mg_json_get(json_body, path, &vlen);
            if (vofs < 0) break;
            if (vlen >= 2) {
                std::string s(json_body.buf + vofs + 1, vlen - 2);
                scenarios.push_back(s);
            }
        }
        if (scenarios.empty()) scenarios = {"basic", "early", "late", "rapid"};

        int64_t task_id = create_async_task("shutup_pipeline_test");
        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            auto& task = async_tasks_[task_id];
            task->worker = std::thread(&FrontendServer::run_shutup_pipeline_test_async, this,
                task_id, scenarios);
        }
        mg_http_reply(c, 202, "Content-Type: application/json\r\n", "{\"task_id\":%lld}", (long long)task_id);
    }

    struct ShutupScenarioResult {
        std::string name;
        std::string description;
        bool pass = false;
        double interrupt_latency_ms = 0;
        double total_ms = 0;
        std::string detail;
    };

    ShutupScenarioResult run_shutup_scenario(const std::string& scenario) {
        ShutupScenarioResult r;
        uint16_t llama_cmd_port = whispertalk::service_cmd_port(whispertalk::ServiceType::LLAMA_SERVICE);
        std::string active_tts_s = get_setting("test_active_tts", "kokoro");
        uint16_t engine_cmd_port = tts_engine_cmd_port_for(active_tts_s);
        uint16_t oap_cmd_port = whispertalk::service_cmd_port(whispertalk::ServiceType::OUTBOUND_AUDIO_PROCESSOR);

        std::string prompt = "Erzähl mir eine sehr lange und detaillierte Geschichte über einen Ritter, der durch dunkle Wälder reist.";

        if (scenario == "basic") {
            r.name = "Basic Interrupt";
            r.description = "Trigger response, wait 200ms, then interrupt mid-generation";

            std::string err;
            std::string resp = tcp_command(llama_cmd_port, "SHUTUP_TEST:" + prompt + "|200", err, 15);
            if (resp.empty() || resp.rfind("SHUTUP_RESULT:", 0) != 0) {
                r.detail = "LLaMA command failed: " + (err.empty() ? resp : err);
                return r;
            }
            parse_shutup_result(resp, r.interrupt_latency_ms, r.total_ms);
            r.pass = (r.interrupt_latency_ms <= 500);
            r.detail = r.pass ? "Interrupt within target" : "Interrupt too slow";

        } else if (scenario == "early") {
            r.name = "Early Interrupt";
            r.description = "Interrupt immediately after generation starts (0ms delay)";

            std::string err;
            std::string resp = tcp_command(llama_cmd_port, "SHUTUP_TEST:" + prompt + "|0", err, 15);
            if (resp.empty() || resp.rfind("SHUTUP_RESULT:", 0) != 0) {
                r.detail = "LLaMA command failed: " + (err.empty() ? resp : err);
                return r;
            }
            parse_shutup_result(resp, r.interrupt_latency_ms, r.total_ms);
            r.pass = (r.interrupt_latency_ms <= 500);
            r.detail = r.pass ? "Early interrupt successful" : "Early interrupt too slow";

        } else if (scenario == "late") {
            r.name = "Late Interrupt";
            r.description = "Wait 1000ms before interrupt (may catch end of generation)";

            std::string err;
            std::string resp = tcp_command(llama_cmd_port, "SHUTUP_TEST:" + prompt + "|1000", err, 15);

            if (resp.empty() || resp.rfind("SHUTUP_RESULT:", 0) != 0) {
                r.detail = "LLaMA command failed: " + (err.empty() ? resp : err);
                return r;
            }
            parse_shutup_result(resp, r.interrupt_latency_ms, r.total_ms);
            r.pass = (r.interrupt_latency_ms <= 500);
            r.detail = r.pass ? "Late interrupt successful" : "Late interrupt too slow";

        } else if (scenario == "rapid") {
            r.name = "Rapid Successive Interrupts";
            r.description = "3 consecutive interrupt cycles with minimal delay between them";

            double max_latency = 0;
            double total_time = 0;
            int success_count = 0;

            for (int i = 0; i < 3; i++) {
                std::string err;
                std::string resp = tcp_command(llama_cmd_port, "SHUTUP_TEST:" + prompt + "|100", err, 15);
                if (!resp.empty() && resp.rfind("SHUTUP_RESULT:", 0) == 0) {
                    double il = 0, tm = 0;
                    parse_shutup_result(resp, il, tm);
                    if (il > max_latency) max_latency = il;
                    total_time += tm;
                    if (il <= 500) success_count++;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(SHUTUP_INTER_ROUND_MS));
            }

            r.interrupt_latency_ms = max_latency;
            r.total_ms = total_time;
            r.pass = (success_count == 3);
            char detail_buf[128];
            snprintf(detail_buf, sizeof(detail_buf), "%d/3 interrupts within 500ms target, max=%.1fms", success_count, max_latency);
            r.detail = detail_buf;
        }

        {
            std::string err;
            // Unknown-engine guard: port 0 means tts_engine_cmd_port_for
            // didn't recognise the configured engine. Surface that
            // explicitly instead of an "unreachable" false negative.
            std::string engine_status;
            if (engine_cmd_port == 0) {
                engine_status = "unknown engine (" + active_tts_s + ")";
            } else {
                std::string s = tcp_command(engine_cmd_port, "STATUS", err, 3);
                engine_status = s.empty() ? std::string("unreachable") : s;
            }
            std::string oap_status = tcp_command(oap_cmd_port, "STATUS", err, 3);
            if (!r.detail.empty()) r.detail += " | ";
            r.detail += active_tts_s + ": " + engine_status;
            r.detail += " OAP: " + (oap_status.empty() ? "unreachable" : oap_status);
        }

        return r;
    }

    void parse_shutup_result(const std::string& resp, double& interrupt_ms, double& total_ms) {
        if (resp.rfind("SHUTUP_RESULT:", 0) != 0) return;
        try {
            size_t p1 = resp.find("ms:", 14);
            if (p1 != std::string::npos) {
                interrupt_ms = std::stod(resp.substr(14, p1 - 14));
                size_t p2 = resp.find("ms", p1 + 3);
                if (p2 != std::string::npos) {
                    total_ms = std::stod(resp.substr(p1 + 3, p2 - (p1 + 3)));
                }
            }
        } catch (...) {}
    }

    void run_shutup_pipeline_test_async(int64_t task_id, std::vector<std::string> scenarios) {
        try {
            uint16_t llama_cmd_port = whispertalk::service_cmd_port(whispertalk::ServiceType::LLAMA_SERVICE);
            std::string ping_err;
            if (tcp_command(llama_cmd_port, "PING", ping_err, 3) != "PONG") {
                finish_async_task(task_id, "{\"error\":\"LLaMA service not reachable\"}");
                return;
            }

            std::string json = "{\"scenarios\":[";
            for (size_t i = 0; i < scenarios.size(); i++) {
                auto r = run_shutup_scenario(scenarios[i]);
                if (i > 0) json += ",";
                json += "{\"name\":\"" + escape_json(r.name) + "\"";
                json += ",\"description\":\"" + escape_json(r.description) + "\"";
                json += ",\"pass\":" + std::string(r.pass ? "true" : "false");
                json += ",\"interrupt_latency_ms\":" + std::to_string(r.interrupt_latency_ms);
                json += ",\"total_ms\":" + std::to_string(r.total_ms);
                json += ",\"detail\":\"" + escape_json(r.detail) + "\"}";
            }
            json += "]}";
            finish_async_task(task_id, json);
        } catch (const std::exception& e) {
            finish_async_task(task_id, std::string("{\"error\":\"") + escape_json(e.what()) + "\"}");
        } catch (...) {
            finish_async_task(task_id, "{\"error\":\"Unknown error in shut-up pipeline test\"}");
        }
    }

    // POST /api/llama/benchmark — Async LLaMA benchmark: runs N iterations of prompt
    // generation, measures avg/p50/p95/p99 latency and memory. Stores results in SQLite.
    void handle_llama_benchmark(struct mg_connection *c, struct mg_http_message *hm) {
        if (!db_) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database not available\"}");
            return;
        }

        struct mg_str json_body = hm->body;
        int model_id = (int)mg_json_get_long(json_body, "$.model_id", 0);
        int iterations = (int)mg_json_get_long(json_body, "$.iterations", 1);
        if (model_id == 0) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing model_id\"}");
            return;
        }
        if (iterations < 1) iterations = 1;
        if (iterations > 10) iterations = 10;

        std::string model_name, model_path, model_backend;
        {
            const char* sql = "SELECT name, path, backend FROM models WHERE id=? AND service='llama'";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, model_id);
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char* mn = (const char*)sqlite3_column_text(stmt, 0);
                    const char* mp = (const char*)sqlite3_column_text(stmt, 1);
                    const char* mb = (const char*)sqlite3_column_text(stmt, 2);
                    model_name = mn ? mn : "";
                    model_path = mp ? mp : "";
                    model_backend = mb ? mb : "";
                }
                sqlite3_finalize(stmt);
            }
        }
        if (model_name.empty()) {
            mg_http_reply(c, 404, "Content-Type: application/json\r\n", "{\"error\":\"LLaMA model not found\"}");
            return;
        }

        int64_t task_id = create_async_task("llama_benchmark");

        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            auto& task = async_tasks_[task_id];
            task->worker = std::thread(&FrontendServer::run_llama_benchmark_async_wrapper, this,
                task_id, model_id, model_name, model_path, model_backend, iterations);
        }

        mg_http_reply(c, 202, "Content-Type: application/json\r\n", "{\"task_id\":%lld}", (long long)task_id);
    }

    void run_llama_benchmark_async_wrapper(int64_t task_id, int model_id,
            std::string model_name, std::string model_path, std::string model_backend, int iterations) {
        std::string result = run_llama_benchmark_async(model_id, model_name, model_path, model_backend, iterations);
        finish_async_task(task_id, result);
    }

    std::string run_llama_benchmark_async(int model_id, const std::string& model_name,
            const std::string& model_path, const std::string& model_backend, int iterations) {
        uint16_t llama_cmd_port = whispertalk::service_cmd_port(whispertalk::ServiceType::LLAMA_SERVICE);

        std::string ping_err;
        std::string ping_resp = tcp_command(llama_cmd_port, "PING", ping_err, 3);
        if (ping_resp != "PONG") {
            return "{\"error\":\"LLaMA service not reachable\"}";
        }

        std::string prompts_path = "Testfiles/llama_prompts.json";
        FILE* fp = fopen(prompts_path.c_str(), "r");
        if (!fp) return "{\"error\":\"Test prompts file not found\"}";
        std::string content;
        char buf[4096];
        while (size_t n = fread(buf, 1, sizeof(buf), fp)) content.append(buf, n);
        fclose(fp);

        struct PromptEntry {
            std::string prompt;
            std::vector<std::string> keywords;
            int max_words;
        };
        std::vector<PromptEntry> prompts;

        struct mg_str jbody = mg_str(content.c_str());
        struct mg_str key, val;
        size_t ofs = 0;
        while ((ofs = mg_json_next(jbody, ofs, &key, &val)) > 0) {
            if (val.len < 2 || val.buf[0] != '{') continue;
            PromptEntry pe;
            int plen = 0;
            int pofs = mg_json_get(val, "$.prompt", &plen);
            if (pofs >= 0 && plen >= 2) {
                char pbuf[1024];
                if (mg_json_unescape(mg_str_n(val.buf + pofs + 1, plen - 2), pbuf, sizeof(pbuf)))
                    pe.prompt = pbuf;
            }
            pe.max_words = (int)mg_json_get_long(val, "$.max_words", 30);
            int klen = 0;
            int kofs = mg_json_get(val, "$.expected_keywords", &klen);
            if (kofs >= 0) {
                struct mg_str karr = mg_str_n(val.buf + kofs, (size_t)klen);
                struct mg_str kk, kv;
                size_t ko = 0;
                while ((ko = mg_json_next(karr, ko, &kk, &kv)) > 0) {
                    if (kv.len >= 2 && kv.buf[0] == '"') {
                        char kbuf[256];
                        if (mg_json_unescape(mg_str_n(kv.buf + 1, kv.len - 2), kbuf, sizeof(kbuf)))
                            pe.keywords.push_back(kbuf);
                    }
                }
            }
            if (!pe.prompt.empty()) prompts.push_back(pe);
        }

        if (prompts.empty()) return "{\"error\":\"No test prompts found\"}";

        std::vector<double> latencies;
        std::vector<double> scores;
        int german_count = 0;
        int total_words = 0;
        double interrupt_latency = 0;

        for (int iter = 0; iter < iterations; iter++) {
            for (const auto& p : prompts) {
                std::string cmd = "TEST_PROMPT:" + p.prompt;
                std::string err;
                std::string resp = tcp_command(llama_cmd_port, cmd, err, 15);

                std::string response_text;
                double latency_ms = 0;
                if (resp.rfind("RESPONSE:", 0) == 0) {
                    size_t ms_end = resp.find("ms:", 9);
                    if (ms_end != std::string::npos) {
                        latency_ms = std::stod(resp.substr(9, ms_end - 9));
                        response_text = resp.substr(ms_end + 3);
                    }
                }
                latencies.push_back(latency_ms);

                LlamaScoreResult sr = score_llama_response(response_text, p.keywords, p.max_words);
                total_words += sr.word_count;
                if (sr.is_german) german_count++;
                scores.push_back(sr.score);
            }
        }

        {
            std::string shutup_cmd = "SHUTUP_TEST:Erzähl mir eine sehr lange und detaillierte Geschichte.";
            std::string err;
            std::string resp = tcp_command(llama_cmd_port, shutup_cmd, err, 15);
            if (resp.rfind("SHUTUP_RESULT:", 0) == 0) {
                size_t p1 = resp.find("ms:", 14);
                if (p1 != std::string::npos) {
                    interrupt_latency = std::stod(resp.substr(14, p1 - 14));
                }
            }
        }

        std::sort(latencies.begin(), latencies.end());
        int n = (int)latencies.size();
        double avg_latency = 0, avg_score = 0;
        for (double v : latencies) avg_latency += v;
        for (double v : scores) avg_score += v;
        avg_latency = n > 0 ? avg_latency / n : 0;
        avg_score = n > 0 ? avg_score / scores.size() : 0;
        double p50 = n > 0 ? latencies[n / 2] : 0;
        double p95 = n > 0 ? latencies[(int)(n * 0.95)] : 0;
        double p99 = n > 0 ? latencies[(int)(n * 0.99)] : 0;
        int total_runs = n;
        double german_pct = total_runs > 0 ? (german_count * 100.0 / total_runs) : 0;
        double avg_words = total_runs > 0 ? (double)total_words / total_runs : 0;

        struct stat st;
        double memory_mb = 0;
        if (stat(model_path.c_str(), &st) == 0) {
            memory_mb = st.st_size / (1024.0 * 1024.0);
        }

        if (db_) {
            const char* sql = "INSERT INTO model_benchmark_runs "
                "(model_id, model_name, model_type, backend, avg_accuracy, avg_latency_ms, "
                "p50_latency_ms, p95_latency_ms, p99_latency_ms, memory_mb, files_tested, "
                "pass_count, fail_count, timestamp, avg_tokens, interrupt_latency_ms, german_pct) "
                "VALUES (?1,?2,'llama',?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,strftime('%s','now'),?13,?14,?15) "
                "RETURNING id";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int(stmt, 1, model_id);
                sqlite3_bind_text(stmt, 2, model_name.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, model_backend.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(stmt, 4, avg_score);
                sqlite3_bind_double(stmt, 5, avg_latency);
                sqlite3_bind_int(stmt, 6, (int)p50);
                sqlite3_bind_int(stmt, 7, (int)p95);
                sqlite3_bind_int(stmt, 8, (int)p99);
                sqlite3_bind_double(stmt, 9, memory_mb);
                sqlite3_bind_int(stmt, 10, (int)prompts.size());
                int pass_count = 0, fail_count = 0;
                for (double s : scores) { if (s >= 80) pass_count++; else fail_count++; }
                sqlite3_bind_int(stmt, 11, pass_count);
                sqlite3_bind_int(stmt, 12, fail_count);
                sqlite3_bind_double(stmt, 13, avg_words);
                sqlite3_bind_double(stmt, 14, interrupt_latency);
                sqlite3_bind_double(stmt, 15, german_pct);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }

        char result[1024];
        snprintf(result, sizeof(result),
            "{\"avg_score\":%.1f,\"avg_latency_ms\":%.0f,\"p50_latency_ms\":%d,\"p95_latency_ms\":%d,"
            "\"p99_latency_ms\":%d,\"memory_mb\":%.0f,\"prompts_tested\":%d,"
            "\"german_pct\":%.0f,\"avg_tokens\":%.1f,\"interrupt_latency_ms\":%.0f,"
            "\"pass_count\":%d,\"fail_count\":%d}",
            avg_score, avg_latency, (int)p50, (int)p95, (int)p99, memory_mb,
            (int)prompts.size(), german_pct, avg_words, interrupt_latency,
            (int)std::count_if(scores.begin(), scores.end(), [](double s){ return s >= 80; }),
            (int)std::count_if(scores.begin(), scores.end(), [](double s){ return s < 80; }));
        return result;
    }

    // POST /api/kokoro/quality_test — Async Kokoro TTS quality test. Sends text to
    // Kokoro, records synthesized audio, measures latency and audio quality metrics.
    void handle_kokoro_quality_test(struct mg_connection *c, struct mg_http_message *hm) {
        struct mg_str json_body = hm->body;
        std::vector<std::string> phrases;
        int plen = 0;
        int pofs = mg_json_get(json_body, "$.phrases", &plen);
        if (pofs >= 0) {
            struct mg_str arr = mg_str_n(json_body.buf + pofs, (size_t)plen);
            size_t ofs = 0;
            struct mg_str key, val;
            while ((ofs = mg_json_next(arr, ofs, &key, &val)) > 0) {
                if (val.len >= 2 && val.buf[0] == '"') {
                    char buf[1024];
                    if (mg_json_unescape(mg_str_n(val.buf + 1, val.len - 2), buf, sizeof(buf)))
                        phrases.push_back(buf);
                }
            }
        }

        if (phrases.empty()) {
            phrases = {
                "Hallo, wie kann ich Ihnen helfen?",
                "Die Hauptstadt von Deutschland ist Berlin.",
                "Guten Morgen, ich bin Ihr Sprachassistent.",
                "Das Wetter ist heute sehr schoen und die Sonne scheint.",
                "Vielen Dank fuer Ihren Anruf, auf Wiedersehen!"
            };
        }

        int64_t task_id = create_async_task("kokoro_quality_test");
        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            auto& task = async_tasks_[task_id];
            task->worker = std::thread(&FrontendServer::run_kokoro_quality_test_async, this,
                task_id, std::move(phrases));
        }
        mg_http_reply(c, 202, "Content-Type: application/json\r\n", "{\"task_id\":%lld}", (long long)task_id);
    }

    void run_kokoro_quality_test_async(int64_t task_id, std::vector<std::string> phrases) {
        std::string active_tts_q = get_setting("test_active_tts", "kokoro");
        uint16_t engine_cmd_port = tts_engine_cmd_port_for(active_tts_q);
        if (engine_cmd_port == 0) {
            finish_async_task(task_id, "{\"error\":\"unknown TTS engine: " + escape_json(active_tts_q) + "\"}");
            return;
        }

        std::string ping_err;
        if (tcp_command(engine_cmd_port, "PING", ping_err, 3) != "PONG") {
            finish_async_task(task_id, "{\"error\":\"TTS service not reachable (port "
                + std::to_string(engine_cmd_port) + "): " + escape_json(ping_err) + "\"}");
            return;
        }

        std::string results_json = "[";
        double total_latency = 0;
        int success_count = 0;
        double total_rtf = 0;
        double total_duration = 0;

        for (size_t i = 0; i < phrases.size(); i++) {
            std::string err;
            std::string resp = tcp_command(engine_cmd_port, "TEST_SYNTH:" + phrases[i], err, 15);

            double latency_ms = 0;
            long samples = 0;
            int sample_rate = 0;
            double duration_s = 0;
            double rtf = 0;
            float peak = 0;
            double rms = 0;
            bool ok = false;

            if (resp.rfind("SYNTH_RESULT:", 0) == 0) {
                ok = true;
                size_t p = 13;
                size_t ms_end = resp.find("ms:", p);
                if (ms_end != std::string::npos) {
                    latency_ms = std::stod(resp.substr(p, ms_end - p));
                    p = ms_end + 3;
                }
                size_t c1 = resp.find(':', p);
                if (c1 != std::string::npos) { samples = std::stol(resp.substr(p, c1 - p)); p = c1 + 1; }
                size_t c2 = resp.find(':', p);
                if (c2 != std::string::npos) { sample_rate = std::stoi(resp.substr(p, c2 - p)); p = c2 + 1; }
                size_t c3 = resp.find("s:", p);
                if (c3 != std::string::npos) { duration_s = std::stod(resp.substr(p, c3 - p)); p = c3 + 2; }
                size_t rtf_pos = resp.find("rtf=", p);
                if (rtf_pos != std::string::npos) {
                    size_t rtf_end = resp.find(':', rtf_pos + 4);
                    rtf = std::stod(resp.substr(rtf_pos + 4, rtf_end - rtf_pos - 4));
                }
                size_t peak_pos = resp.find("peak=", p);
                if (peak_pos != std::string::npos) {
                    size_t peak_end = resp.find(':', peak_pos + 5);
                    peak = std::stof(resp.substr(peak_pos + 5, peak_end - peak_pos - 5));
                }
                size_t rms_pos = resp.find("rms=", p);
                if (rms_pos != std::string::npos) {
                    rms = std::stod(resp.substr(rms_pos + 4));
                }
            }

            std::string status_str = "fail";
            if (ok) {
                success_count++;
                total_latency += latency_ms;
                total_rtf += rtf;
                total_duration += duration_s;
                status_str = (rtf < 1.0) ? "pass" : "warn";
            }

            if (i > 0) results_json += ",";
            results_json += "{\"phrase\":\"" + escape_json(phrases[i])
                + "\",\"latency_ms\":" + std::to_string((int)latency_ms)
                + ",\"samples\":" + std::to_string(samples)
                + ",\"sample_rate\":" + std::to_string(sample_rate)
                + ",\"duration_s\":" + std::to_string(duration_s)
                + ",\"rtf\":" + std::to_string(rtf)
                + ",\"peak\":" + std::to_string(peak)
                + ",\"rms\":" + std::to_string(rms)
                + ",\"status\":\"" + status_str
                + "\",\"ok\":" + (ok ? "true" : "false") + "}";

            if (db_) {
                const char* sql = "INSERT INTO test_results (test_name,service,status,details,timestamp) "
                    "VALUES ('kokoro_quality',?1,?2,?3,strftime('%s','now'))";
                sqlite3_stmt* stmt = nullptr;
                if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, "kokoro", -1, SQLITE_STATIC);
                    sqlite3_bind_text(stmt, 2, status_str.c_str(), -1, SQLITE_TRANSIENT);
                    std::string details = phrases[i] + " -> " + std::to_string((int)latency_ms) + "ms, "
                        + std::to_string(samples) + " samples, RTF=" + std::to_string(rtf);
                    sqlite3_bind_text(stmt, 3, details.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
            }
        }
        results_json += "]";

        double avg_latency = success_count > 0 ? total_latency / success_count : 0;
        double avg_rtf = success_count > 0 ? total_rtf / success_count : 0;

        std::string result = "{\"results\":" + results_json
            + ",\"summary\":{\"avg_latency_ms\":" + std::to_string((int)avg_latency)
            + ",\"avg_rtf\":" + std::to_string(avg_rtf)
            + ",\"total_duration_s\":" + std::to_string(total_duration)
            + ",\"success_count\":" + std::to_string(success_count)
            + ",\"total_count\":" + std::to_string((int)phrases.size()) + "}}";
        finish_async_task(task_id, result);
    }

    // POST /api/kokoro/benchmark — Async Kokoro TTS benchmark: runs N iterations
    // of synthesis, measures latency percentiles and memory. Stores results in SQLite.
    void handle_kokoro_benchmark(struct mg_connection *c, struct mg_http_message *hm) {
        struct mg_str json_body = hm->body;
        int iterations = (int)mg_json_get_long(json_body, "$.iterations", 5);
        if (iterations < 1) iterations = 1;
        if (iterations > 20) iterations = 20;

        int plen = 0;
        std::string phrase = "Guten Tag, wie kann ich Ihnen helfen?";
        int pofs = mg_json_get(json_body, "$.phrase", &plen);
        if (pofs >= 0 && plen >= 2) {
            char pbuf[1024];
            if (mg_json_unescape(mg_str_n(json_body.buf + pofs + 1, plen - 2), pbuf, sizeof(pbuf)))
                phrase = pbuf;
        }

        int64_t task_id = create_async_task("kokoro_benchmark");
        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            auto& task = async_tasks_[task_id];
            task->worker = std::thread(&FrontendServer::run_kokoro_benchmark_async, this,
                task_id, phrase, iterations);
        }
        mg_http_reply(c, 202, "Content-Type: application/json\r\n", "{\"task_id\":%lld}", (long long)task_id);
    }

    void run_kokoro_benchmark_async(int64_t task_id, std::string phrase, int iterations) {
        std::string active_tts_b = get_setting("test_active_tts", "kokoro");
        uint16_t engine_cmd_port = tts_engine_cmd_port_for(active_tts_b);
        if (engine_cmd_port == 0) {
            finish_async_task(task_id, "{\"error\":\"unknown TTS engine: " + escape_json(active_tts_b) + "\"}");
            return;
        }

        std::string ping_err;
        if (tcp_command(engine_cmd_port, "PING", ping_err, 3) != "PONG") {
            finish_async_task(task_id, "{\"error\":\"TTS service not reachable (port "
                + std::to_string(engine_cmd_port) + "): " + escape_json(ping_err) + "\"}");
            return;
        }

        std::string bench_cmd = "BENCHMARK:" + phrase + "|" + std::to_string(iterations);
        std::string err;
        std::string resp = tcp_command(engine_cmd_port, bench_cmd, err, 60);

        if (resp.rfind("BENCH_RESULT:", 0) != 0) {
            finish_async_task(task_id, "{\"error\":\"Benchmark failed: " + escape_json(err.empty() ? resp : err) + "\"}");
            return;
        }

        int avg_ms = 0, p50_ms = 0, p95_ms = 0;
        int success = 0, total = 0;
        long total_samples = 0;
        int sample_rate = 0;
        double rtf = 0;

        size_t p = 13;
        size_t c1 = resp.find("ms:", p);
        if (c1 != std::string::npos) { avg_ms = std::stoi(resp.substr(p, c1 - p)); p = c1 + 3; }
        size_t c2 = resp.find("ms:", p);
        if (c2 != std::string::npos) { p50_ms = std::stoi(resp.substr(p, c2 - p)); p = c2 + 3; }
        size_t c3 = resp.find("ms:", p);
        if (c3 != std::string::npos) { p95_ms = std::stoi(resp.substr(p, c3 - p)); p = c3 + 3; }
        size_t slash = resp.find('/', p);
        if (slash != std::string::npos) {
            success = std::stoi(resp.substr(p, slash - p));
            size_t c4 = resp.find(':', slash + 1);
            if (c4 != std::string::npos) { total = std::stoi(resp.substr(slash + 1, c4 - slash - 1)); p = c4 + 1; }
        }
        size_t at = resp.find('@', p);
        if (at != std::string::npos) {
            total_samples = std::stol(resp.substr(p, at - p));
            size_t c5 = resp.find(':', at + 1);
            if (c5 != std::string::npos) { sample_rate = std::stoi(resp.substr(at + 1, c5 - at - 1)); p = c5 + 1; }
        }
        size_t rtf_pos = resp.find("rtf=", p);
        if (rtf_pos != std::string::npos) {
            rtf = std::stod(resp.substr(rtf_pos + 4));
        }

        double duration_s = sample_rate > 0 ? (double)total_samples / sample_rate : 0;

        if (db_) {
            const char* sql = "INSERT INTO test_results (test_name,service,status,details,timestamp) "
                "VALUES ('kokoro_benchmark','kokoro',?1,?2,strftime('%s','now'))";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                std::string status = rtf < 1.0 ? "pass" : "warn";
                sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_TRANSIENT);
                std::string details = "avg=" + std::to_string(avg_ms) + "ms p50=" + std::to_string(p50_ms)
                    + "ms p95=" + std::to_string(p95_ms) + "ms RTF=" + std::to_string(rtf)
                    + " (" + std::to_string(success) + "/" + std::to_string(total) + " ok)";
                sqlite3_bind_text(stmt, 2, details.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }

        std::string result = "{\"avg_ms\":" + std::to_string(avg_ms)
            + ",\"p50_ms\":" + std::to_string(p50_ms)
            + ",\"p95_ms\":" + std::to_string(p95_ms)
            + ",\"success\":" + std::to_string(success)
            + ",\"total\":" + std::to_string(total)
            + ",\"samples\":" + std::to_string(total_samples)
            + ",\"sample_rate\":" + std::to_string(sample_rate)
            + ",\"duration_s\":" + std::to_string(duration_s)
            + ",\"rtf\":" + std::to_string(rtf)
            + ",\"phrase\":\"" + escape_json(phrase) + "\"}";
        finish_async_task(task_id, result);
    }

    // POST /api/pipeline/health — Check pipeline connectivity by sending PING to each
    // service's cmd port and measuring round-trip latency (ms).
    void handle_pipeline_health(struct mg_connection *c, struct mg_http_message *hm) {
        (void)hm;

        struct ServiceDef {
            const char* name;
            whispertalk::ServiceType type;
        };
        const ServiceDef defs[] = {
            {"sip-client",                whispertalk::ServiceType::SIP_CLIENT},
            {"inbound-audio-processor",   whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR},
            {"vad-service",               whispertalk::ServiceType::VAD_SERVICE},
            {"whisper-service",           whispertalk::ServiceType::WHISPER_SERVICE},
            {"llama-service",             whispertalk::ServiceType::LLAMA_SERVICE},
            {"tts-service",               whispertalk::ServiceType::TTS_SERVICE},
            {"outbound-audio-processor",  whispertalk::ServiceType::OUTBOUND_AUDIO_PROCESSOR},
            {"tomedo-crawl",              whispertalk::ServiceType::TOMEDO_CRAWL_SERVICE},
        };
        constexpr int N = 8;

        struct Result { bool reachable; std::string details; };
        std::vector<Result> results(N);

        std::vector<std::thread> threads;
        for (int i = 0; i < N; ++i) {
            threads.emplace_back([i, &results, &defs]() {
                std::string err;
                uint16_t port = whispertalk::service_cmd_port(defs[i].type);
                int sock = socket(AF_INET, SOCK_STREAM, 0);
                if (sock < 0) { results[i] = {false, "socket error"}; return; }
                struct timeval tv{1, 0};
                setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                struct sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(port);
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                    ::close(sock);
                    results[i] = {false, "not reachable"};
                    return;
                }
                const char* cmd = "PING";
                send(sock, cmd, strlen(cmd), 0);
                char buf[64] = {};
                ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
                ::close(sock);
                if (n > 0) {
                    std::string resp(buf, n);
                    while (!resp.empty() && (resp.back() == '\n' || resp.back() == '\r')) resp.pop_back();
                    bool pong = (resp.find("PONG") != std::string::npos);
                    results[i] = {pong, pong ? "online" : resp};
                } else {
                    results[i] = {false, "connected but no response"};
                }
            });
        }
        for (auto& t : threads) t.join();

        std::stringstream json;
        json << "{\"services\":[";
        for (int i = 0; i < N; ++i) {
            if (i > 0) json << ",";
            json << "{\"name\":\"" << defs[i].name << "\""
                 << ",\"reachable\":" << (results[i].reachable ? "true" : "false")
                 << ",\"details\":\"" << escape_json(results[i].details) << "\"}";
        }
        int online = 0;
        for (auto& r : results) if (r.reachable) online++;
        json << "],\"online\":" << online << ",\"total\":" << N << "}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    // POST /api/tests/tts_roundtrip — End-to-end TTS test: sends text through
    // LLaMA→Kokoro→OAP→SIP and measures audio output latency and quality.
    void handle_tts_roundtrip(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }

        struct mg_str json_body = hm->body;
        std::vector<std::string> phrases;
        int plen = 0;
        int pofs = mg_json_get(json_body, "$.phrases", &plen);
        if (pofs >= 0) {
            struct mg_str arr = mg_str_n(json_body.buf + pofs, (size_t)plen);
            size_t ofs = 0;
            struct mg_str key, val;
            while ((ofs = mg_json_next(arr, ofs, &key, &val)) > 0) {
                if (val.len >= 2 && val.buf[0] == '"') {
                    char buf[1024];
                    if (mg_json_unescape(mg_str_n(val.buf + 1, val.len - 2), buf, sizeof(buf)))
                        phrases.push_back(buf);
                }
            }
        }

        if (phrases.empty()) {
            phrases = {
                "Hallo, wie kann ich Ihnen helfen?",
                "Die Hauptstadt von Deutschland ist Berlin.",
                "Guten Morgen, ich bin Ihr Sprachassistent."
            };
        }

        int64_t task_id = create_async_task("tts_roundtrip");
        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            auto& task = async_tasks_[task_id];
            task->worker = std::thread(&FrontendServer::run_tts_roundtrip_async, this,
                task_id, std::move(phrases));
        }
        mg_http_reply(c, 202, "Content-Type: application/json\r\n", "{\"task_id\":%lld}", (long long)task_id);
    }

    void run_tts_roundtrip_async(int64_t task_id, std::vector<std::string> phrases) {
        std::string active_tts = get_setting("test_active_tts", "kokoro");
        uint16_t engine_cmd_port = tts_engine_cmd_port_for(active_tts);
        if (engine_cmd_port == 0) {
            finish_async_task(task_id, "{\"error\":\"unknown TTS engine: " + escape_json(active_tts) + "\"}");
            return;
        }

        std::string ping_err;
        if (tcp_command(engine_cmd_port, "PING", ping_err, 3) != "PONG") {
            finish_async_task(task_id, "{\"error\":\"TTS service not reachable\"}");
            return;
        }

        std::string status_err;
        std::string status_resp = http_get_localhost(TEST_SIP_PROVIDER_PORT, "/status", status_err);
        if (!status_err.empty() || status_resp.find("\"call_active\":true") == std::string::npos) {
            finish_async_task(task_id, "{\"error\":\"No active call on test_sip_provider — start a call first\"}");
            return;
        }

        std::stringstream json;
        json << "{\"results\":[";
        bool first = true;
        int pass_count = 0, warn_count = 0, fail_count = 0;
        double total_in_sim = 0.0;
        double total_out_sim = 0.0;
        double total_e2e_ms = 0.0;
        int processed = 0;

        for (const auto& phrase : phrases) {
            std::string tmp_filename = "_tts_roundtrip_" + std::to_string(task_id) + "_" + std::to_string(processed) + ".wav";
            std::string tmp_path = "Testfiles/" + tmp_filename;

            std::string synth_err;
            std::string synth_cmd = "SYNTH_WAV:" + tmp_path + "|" + phrase;
            std::string synth_resp = tcp_command(engine_cmd_port, synth_cmd, synth_err, 30);

            bool synth_ok = (synth_resp.rfind("WAV_RESULT:", 0) == 0);
            if (!synth_ok) {
                if (!first) json << ",";
                json << "{\"phrase\":\"" << escape_json(phrase) << "\""
                     << ",\"transcription_in\":\"\",\"similarity_in\":0"
                     << ",\"llama_response\":\"\",\"transcription_out\":\"\",\"similarity_out\":0"
                     << ",\"e2e_ms\":0,\"status\":\"FAIL\""
                     << ",\"error\":\"" << escape_json(synth_err.empty() ? synth_resp : synth_err) << "\"}";
                first = false;
                fail_count++;
                continue;
            }

            uint64_t seq_before = current_log_seq();
            auto e2e_start = std::chrono::steady_clock::now();

            std::string inject_body = "{\"file\":\"" + escape_json(tmp_filename) + "\",\"leg\":\"a\",\"no_silence\":true}";
            std::string inject_err;
            std::string inject_resp = http_post_localhost(TEST_SIP_PROVIDER_PORT, "/inject", inject_body, inject_err);

            if (!inject_err.empty() || inject_resp.find("\"success\":true") == std::string::npos) {
                std::remove(tmp_path.c_str());
                if (!first) json << ",";
                json << "{\"phrase\":\"" << escape_json(phrase) << "\""
                     << ",\"transcription_in\":\"\",\"similarity_in\":0"
                     << ",\"llama_response\":\"\",\"transcription_out\":\"\",\"similarity_out\":0"
                     << ",\"e2e_ms\":0,\"status\":\"FAIL\""
                     << ",\"error\":\"" << escape_json(inject_err.empty() ? inject_resp : inject_err) << "\"}";
                first = false;
                fail_count++;
                continue;
            }

            TranscriptionResult tr1 = wait_for_whisper_transcription(seq_before, 60000);
            std::string transcription_in = tr1.found ? tr1.text : "";
            double similarity_in = tr1.found ? calculate_levenshtein_similarity(phrase, transcription_in) : 0;

            uint64_t seq_after_l1 = current_log_seq();

            LlamaResponseResult llama_res = wait_for_llama_response(seq_before, 60000);
            std::string llama_text = llama_res.found ? llama_res.text : "";

            TranscriptionResult tr2 = wait_for_whisper_transcription(seq_after_l1, 120000);

            auto e2e_end = std::chrono::steady_clock::now();
            double e2e_ms = std::chrono::duration_cast<std::chrono::milliseconds>(e2e_end - e2e_start).count();

            std::string transcription_out = tr2.found ? tr2.text : "";
            double similarity_out = 0;
            double wer_out = 100.0;
            if (!llama_text.empty() && !transcription_out.empty()) {
                similarity_out = calculate_levenshtein_similarity(llama_text, transcription_out);
                wer_out = calculate_word_error_rate(llama_text, transcription_out);
            }

            std::string status_str;
            bool in_ok = tr1.found && similarity_in >= 40.0;
            bool llama_ok = llama_res.found && !llama_text.empty();
            bool out_ok = tr2.found && !transcription_out.empty();
            bool kokoro_ok = similarity_out >= 30.0;

            if (in_ok && llama_ok && out_ok && kokoro_ok && similarity_in >= 60.0) {
                status_str = "PASS"; pass_count++;
            } else if (in_ok && llama_ok && out_ok) {
                status_str = "WARN"; warn_count++;
            } else {
                status_str = "FAIL"; fail_count++;
            }

            total_in_sim += similarity_in;
            total_out_sim += similarity_out;
            total_e2e_ms += e2e_ms;
            processed++;

            if (db_) {
                const char* sql = "INSERT INTO test_results (test_name,service,status,details,timestamp) "
                    "VALUES ('tts_roundtrip',?1,?2,?3,strftime('%s','now'))";
                sqlite3_stmt* stmt = nullptr;
                if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, "full_pipeline", -1, SQLITE_STATIC);
                    sqlite3_bind_text(stmt, 2, status_str.c_str(), -1, SQLITE_TRANSIENT);
                    std::string details = phrase + " -> L1:" + transcription_in
                        + " (in_sim=" + std::to_string((int)similarity_in) + "%)"
                        + " -> LLaMA:" + llama_text
                        + " -> L2:" + transcription_out
                        + " (out_sim=" + std::to_string((int)similarity_out) + "%)"
                        + " e2e=" + std::to_string((int)e2e_ms) + "ms";
                    sqlite3_bind_text(stmt, 3, details.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
            }

            if (!first) json << ",";
            json << "{\"phrase\":\"" << escape_json(phrase) << "\""
                 << ",\"transcription_in\":\"" << escape_json(transcription_in) << "\""
                 << ",\"similarity_in\":" << similarity_in
                 << ",\"llama_response\":\"" << escape_json(llama_text) << "\""
                 << ",\"transcription_out\":\"" << escape_json(transcription_out) << "\""
                 << ",\"similarity_out\":" << similarity_out
                 << ",\"wer_out\":" << wer_out
                 << ",\"llama_ms\":" << (int)llama_res.gen_ms
                 << ",\"e2e_ms\":" << (int)e2e_ms
                 << ",\"status\":\"" << status_str << "\"}";
            first = false;

            std::remove(tmp_path.c_str());

            if (&phrase != &phrases.back()) {
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        }

        json << "]";

        double avg_in_sim = processed > 0 ? total_in_sim / processed : 0;
        double avg_out_sim = processed > 0 ? total_out_sim / processed : 0;
        double avg_e2e = processed > 0 ? total_e2e_ms / processed : 0;

        json << ",\"summary\":{"
             << "\"avg_similarity_in\":" << avg_in_sim
             << ",\"avg_similarity_out\":" << avg_out_sim
             << ",\"avg_e2e_ms\":" << (int)avg_e2e
             << ",\"pass\":" << pass_count
             << ",\"warn\":" << warn_count
             << ",\"fail\":" << fail_count
             << ",\"total\":" << (int)phrases.size()
             << "}}";

        finish_async_task(task_id, json.str());
    }

    // POST /api/tests/full_loop — Full pipeline loop test: injects audio via SIP
    // provider, captures Whisper transcription from logs, computes WER similarity.
    void handle_full_loop_test(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }

        struct mg_str body = hm->body;
        std::vector<std::string> files;
        int plen = 0;
        int pofs = mg_json_get(body, "$.files", &plen);
        if (pofs >= 0) {
            struct mg_str arr = mg_str_n(body.buf + pofs, (size_t)plen);
            size_t ofs = 0;
            struct mg_str key, val;
            while ((ofs = mg_json_next(arr, ofs, &key, &val)) > 0) {
                if (val.len >= 2 && val.buf[0] == '"') {
                    char buf[1024];
                    if (mg_json_unescape(mg_str_n(val.buf + 1, val.len - 2), buf, sizeof(buf)))
                        files.push_back(buf);
                }
            }
        }

        if (files.empty()) {
            files = {"sample_01.wav"};
        }

        int64_t task_id = create_async_task("full_loop_test");
        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            auto& task = async_tasks_[task_id];
            task->worker = std::thread(&FrontendServer::run_full_loop_test_async, this,
                task_id, std::move(files));
        }
        mg_http_reply(c, 202, "Content-Type: application/json\r\n", "{\"task_id\":%lld}", (long long)task_id);
    }

    void run_full_loop_test_async(int64_t task_id, std::vector<std::string> files) {
        std::string sip_err;
        std::string status_resp = http_get_localhost(TEST_SIP_PROVIDER_PORT, "/status", sip_err);
        if (!sip_err.empty() || status_resp.find("\"call_active\":true") == std::string::npos) {
            finish_async_task(task_id, "{\"error\":\"No active call on test_sip_provider — start a conference call with 2 lines first\"}");
            return;
        }

        std::string active_tts = get_setting("test_active_tts", "kokoro");
        (void)active_tts;

        std::vector<std::pair<whispertalk::ServiceType, const char*>> required_services = {
            {whispertalk::ServiceType::SIP_CLIENT, "SIP Client"},
            {whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR, "IAP"},
            {whispertalk::ServiceType::VAD_SERVICE, "VAD"},
            {whispertalk::ServiceType::WHISPER_SERVICE, "Whisper"},
            {whispertalk::ServiceType::LLAMA_SERVICE, "LLaMA"},
            {whispertalk::ServiceType::TTS_SERVICE, "TTS"},
            {whispertalk::ServiceType::OUTBOUND_AUDIO_PROCESSOR, "OAP"},
        };
        for (const auto& [svc, name] : required_services) {
            uint16_t cmd_port = whispertalk::service_cmd_port(svc);
            std::string err;
            std::string resp = tcp_command(cmd_port, "PING", err, 3);
            if (resp.find("PONG") == std::string::npos) {
                std::string msg = "{\"error\":\"" + std::string(name) + " service not reachable — start all pipeline services first\"}";
                finish_async_task(task_id, msg);
                return;
            }
        }

        std::stringstream json;
        json << "{\"results\":[";
        bool first = true;
        int pass_count = 0, warn_count = 0, fail_count = 0;
        double total_wer = 0.0;
        double total_sim = 0.0;
        double total_e2e = 0.0;
        int processed = 0;

        static constexpr int FULL_LOOP_CONV_DURATION_MS = 180000;

        for (size_t fi = 0; fi < files.size(); fi++) {
            const auto& file = files[fi];
            uint64_t seq_before = current_log_seq();
            auto e2e_start = std::chrono::steady_clock::now();
            auto conv_deadline = e2e_start + std::chrono::milliseconds(FULL_LOOP_CONV_DURATION_MS);

            std::string inject_body = "{\"file\":\"" + escape_json(file) + "\",\"leg\":\"a\",\"no_silence\":true}";
            std::string inject_err;
            std::string inject_resp = http_post_localhost(TEST_SIP_PROVIDER_PORT, "/inject", inject_body, inject_err);

            if (!inject_err.empty() || inject_resp.find("\"success\":true") == std::string::npos) {
                if (!first) json << ",";
                json << "{\"file\":\"" << escape_json(file) << "\""
                     << ",\"error\":\"" << escape_json(inject_err.empty() ? inject_resp : inject_err) << "\""
                     << ",\"status\":\"FAIL\"}";
                first = false;
                fail_count++;
                continue;
            }

            uint32_t l1_call_id = 0;
            uint32_t l2_call_id = 0;
            {
                auto stats = parse_sip_stats();
                for (const auto& ci : stats.calls) {
                    if (ci.line_index == 0) l1_call_id = ci.call_id;
                    if (ci.line_index == 1) l2_call_id = ci.call_id;
                }
            }
            if (l1_call_id == 0) {
                std::cerr << "WARNING: WER test: could not resolve L1 call_id from SIP stats — LLaMA response filter disabled, WER may be inflated\n";
            }

            // Collect data for 180 seconds: let the loopback conversation run multiple round-trips.
            // L1 Whisper: transcription of injected audio (input side, call_id=l1)
            // LLaMA: AI responses triggered by L1 transcriptions
            // L2 Whisper: re-transcription of TTS output (quality measure, call_id=l2)
            std::string whisper_l1_all, llama_text_all, whisper_l2_all;
            std::string last_llama;
            double total_llama_ms = 0.0;
            int llama_count = 0;
            uint64_t scan_seq = seq_before;

            while (std::chrono::steady_clock::now() < conv_deadline) {
                {
                    std::lock_guard<std::mutex> lock(logs_mutex_);
                    for (const auto& entry : recent_logs_) {
                        if (entry.seq <= scan_seq) continue;
                        scan_seq = entry.seq;

                        const std::string& msg = entry.message;

                        if (entry.service == ServiceType::WHISPER_SERVICE) {
                            size_t tpos = msg.find("Transcription (");
                            if (tpos != std::string::npos) {
                                size_t ms_end = msg.find("ms", tpos + 15);
                                if (ms_end != std::string::npos) {
                                    size_t paren_close = msg.find("):", ms_end);
                                    if (paren_close != std::string::npos) {
                                        size_t text_start = paren_close + 2;
                                        if (text_start < msg.size()) {
                                            std::string chunk = msg.substr(text_start);
                                            while (!chunk.empty() && (chunk.front() == ' ' || chunk.front() == ':'))
                                                chunk.erase(chunk.begin());
                                            while (!chunk.empty() && (chunk.back() == ' ' || chunk.back() == '\n'))
                                                chunk.pop_back();
                                            if (!chunk.empty()) {
                                                bool is_l1 = (l1_call_id == 0 || entry.call_id == l1_call_id);
                                                bool is_l2 = (l2_call_id != 0 && entry.call_id == l2_call_id);
                                                if (is_l2) {
                                                    if (!whisper_l2_all.empty()) whisper_l2_all += " ";
                                                    whisper_l2_all += chunk;
                                                } else if (is_l1) {
                                                    if (!whisper_l1_all.empty()) whisper_l1_all += " ";
                                                    whisper_l1_all += chunk;
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                        } else if (entry.service == ServiceType::LLAMA_SERVICE && entry.level == "INFO"
                                   && (l1_call_id == 0 || entry.call_id == l1_call_id)) {
                            size_t rpos = msg.find("Response (");
                            if (rpos != std::string::npos) {
                                size_t ms_end = msg.find("ms)", rpos + 10);
                                if (ms_end != std::string::npos) {
                                    try {
                                        double ms_val = std::stod(msg.substr(rpos + 10, ms_end - (rpos + 10)));
                                        total_llama_ms += ms_val;
                                        llama_count++;
                                    } catch (...) {}
                                    size_t text_start = ms_end + 5;
                                    if (text_start < msg.size()) {
                                        std::string resp = msg.substr(text_start);
                                        while (!resp.empty() && (resp.back() == ' ' || resp.back() == '\n'))
                                            resp.pop_back();
                                        if (!resp.empty()) {
                                            last_llama = resp;
                                            if (!llama_text_all.empty()) llama_text_all += " ";
                                            llama_text_all += resp;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }

            auto e2e_end = std::chrono::steady_clock::now();
            double e2e_ms = std::chrono::duration_cast<std::chrono::milliseconds>(e2e_end - e2e_start).count();
            double avg_llama_ms = llama_count > 0 ? total_llama_ms / llama_count : 0.0;

            double wer = 100.0;
            double similarity = 0.0;
            bool llama_ok = !llama_text_all.empty();
            bool l2_ok = !whisper_l2_all.empty();
            if (llama_ok && l2_ok) {
                wer = calculate_word_error_rate(llama_text_all, whisper_l2_all);
                similarity = calculate_levenshtein_similarity(llama_text_all, whisper_l2_all);
            }

            std::string status_str;
            if (llama_ok && l2_ok && wer <= 10.0) {
                status_str = "PASS"; pass_count++;
            } else if (llama_ok && l2_ok && wer <= 30.0) {
                status_str = "WARN"; warn_count++;
            } else {
                status_str = "FAIL"; fail_count++;
            }

            total_wer += wer;
            total_sim += similarity;
            total_e2e += e2e_ms;
            processed++;

            if (db_) {
                const char* sql = "INSERT INTO test_results (test_name,service,status,details,timestamp) "
                    "VALUES ('full_loop',?1,?2,?3,strftime('%s','now'))";
                sqlite3_stmt* stmt = nullptr;
                if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, "full_pipeline", -1, SQLITE_STATIC);
                    sqlite3_bind_text(stmt, 2, status_str.c_str(), -1, SQLITE_TRANSIENT);
                    std::string details = file + " -> L1:" + whisper_l1_all
                        + " -> LLaMA[" + std::to_string(llama_count) + "]:" + last_llama
                        + " -> L2:" + whisper_l2_all
                        + " (WER=" + std::to_string((int)wer) + "%"
                        + " sim=" + std::to_string((int)similarity) + "%)"
                        + " conv=" + std::to_string((int)e2e_ms) + "ms";
                    sqlite3_bind_text(stmt, 3, details.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
            }

            if (!first) json << ",";
            json << "{\"file\":\"" << escape_json(file) << "\""
                 << ",\"whisper_l1\":\"" << escape_json(whisper_l1_all) << "\""
                 << ",\"llama_response\":\"" << escape_json(last_llama) << "\""
                 << ",\"llama_turns\":" << llama_count
                 << ",\"whisper_l2\":\"" << escape_json(whisper_l2_all) << "\""
                 << ",\"wer\":" << wer
                 << ",\"similarity\":" << similarity
                 << ",\"llama_ms\":" << (int)avg_llama_ms
                 << ",\"e2e_ms\":" << (int)e2e_ms
                 << ",\"status\":\"" << status_str << "\"}";
            first = false;
        }

        json << "]";

        double avg_wer = processed > 0 ? total_wer / processed : 100.0;
        double avg_sim = processed > 0 ? total_sim / processed : 0.0;
        double avg_e2e = processed > 0 ? total_e2e / processed : 0.0;

        json << ",\"summary\":{"
             << "\"avg_wer\":" << avg_wer
             << ",\"avg_similarity\":" << avg_sim
             << ",\"avg_e2e_ms\":" << (int)avg_e2e
             << ",\"pass\":" << pass_count
             << ",\"warn\":" << warn_count
             << ",\"fail\":" << fail_count
             << ",\"total\":" << (int)files.size()
             << "}}";

        finish_async_task(task_id, json.str());
    }

    // POST /api/tests/multiline_stress — Async multi-line SIP stress test: registers
    // N concurrent SIP lines and drives calls for duration_s seconds.
    void handle_multiline_stress(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        struct mg_str body = hm->body;
        int lines = (int)mg_json_get_long(body, "$.lines", 4);
        int duration_s = (int)mg_json_get_long(body, "$.duration_s", 10);
        if (lines < 1) lines = 1;
        if (lines > 32) lines = 32;
        if (duration_s < 1) duration_s = 1;
        if (duration_s > 120) duration_s = 120;

        int64_t task_id = create_async_task("multiline_stress");
        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            auto& task = async_tasks_[task_id];
            task->worker = std::thread(&FrontendServer::run_multiline_stress_async, this,
                task_id, lines, duration_s);
        }
        mg_http_reply(c, 202, "Content-Type: application/json\r\n", "{\"task_id\":%lld}", (long long)task_id);
    }

    void run_multiline_stress_async(int64_t task_id, int lines, int duration_s) {
        struct ServicePing {
            const char* name;
            whispertalk::ServiceType type;
        };
        const ServicePing services[] = {
            {"sip",    whispertalk::ServiceType::SIP_CLIENT},
            {"iap",    whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR},
            {"vad",    whispertalk::ServiceType::VAD_SERVICE},
            {"whisper",whispertalk::ServiceType::WHISPER_SERVICE},
            {"llama",  whispertalk::ServiceType::LLAMA_SERVICE},
            {"tts",    whispertalk::ServiceType::TTS_SERVICE},
            {"oap",    whispertalk::ServiceType::OUTBOUND_AUDIO_PROCESSOR},
        };
        constexpr int NSVC = 7;

        struct ServiceStats {
            std::atomic<long> ok{0};
            std::atomic<long> fail{0};
            std::atomic<long> total_ms{0};
        };
        std::vector<ServiceStats> stats(NSVC);

        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_s);

        std::vector<std::thread> workers;
        for (int line = 0; line < lines; ++line) {
            workers.emplace_back([&, line]() {
                (void)line;
                while (std::chrono::steady_clock::now() < deadline) {
                    for (int s = 0; s < NSVC; ++s) {
                        uint16_t port = whispertalk::service_cmd_port(services[s].type);
                        auto t0 = std::chrono::steady_clock::now();
                        int sock = socket(AF_INET, SOCK_STREAM, 0);
                        if (sock < 0) { stats[s].fail++; continue; }
                        struct timeval tv{1, 0};
                        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
                        struct sockaddr_in addr{};
                        addr.sin_family = AF_INET;
                        addr.sin_port = htons(port);
                        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
                            ::close(sock);
                            stats[s].fail++;
                            continue;
                        }
                        send(sock, "PING", 4, 0);
                        char buf[16] = {};
                        ssize_t n = recv(sock, buf, sizeof(buf) - 1, 0);
                        ::close(sock);
                        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0).count();
                        if (n > 0 && std::string(buf, n).find("PONG") != std::string::npos) {
                            stats[s].ok++;
                            stats[s].total_ms += ms;
                        } else {
                            stats[s].fail++;
                        }
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(STRESS_POLL_MS));
                }
            });
        }
        for (auto& w : workers) w.join();

        std::stringstream json;
        json << "{\"lines\":" << lines
             << ",\"duration_s\":" << duration_s
             << ",\"services\":[";
        long grand_ok = 0, grand_fail = 0;
        for (int s = 0; s < NSVC; ++s) {
            long ok = stats[s].ok.load(), fail = stats[s].fail.load();
            long total = ok + fail;
            double avg_ms = (ok > 0) ? (double)stats[s].total_ms.load() / ok : 0;
            if (s > 0) json << ",";
            json << "{\"name\":\"" << services[s].name << "\""
                 << ",\"ok\":" << ok
                 << ",\"fail\":" << fail
                 << ",\"success_pct\":" << (total > 0 ? (int)(100.0 * ok / total) : 0)
                 << ",\"avg_ms\":" << (int)avg_ms << "}";
            grand_ok += ok; grand_fail += fail;
        }
        long grand_total = grand_ok + grand_fail;
        json << "],\"total_pings\":" << grand_total
             << ",\"total_ok\":" << grand_ok
             << ",\"total_fail\":" << grand_fail
             << ",\"overall_success_pct\":" << (grand_total > 0 ? (int)(100.0 * grand_ok / grand_total) : 0)
             << "}";
        finish_async_task(task_id, json.str());
    }

    // POST /api/tests/pipeline_stress — Start a multi-sample pipeline stress test.
    // Injects multiple audio samples sequentially and collects WER results.
    void handle_pipeline_stress_test(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(pipeline_stress_mutex_);
            if (pipeline_stress_ && pipeline_stress_->running.load()) {
                mg_http_reply(c, 409, "Content-Type: application/json\r\n",
                    "{\"error\":\"Pipeline stress test already running\"}");
                return;
            }
        }

        struct mg_str body = hm->body;
        int duration_s = (int)mg_json_get_long(body, "$.duration_s", 120);
        if (duration_s < 10) duration_s = 10;
        if (duration_s > 600) duration_s = 600;

        static const std::pair<ServiceType, const char*> required_svc[] = {
            {ServiceType::SIP_CLIENT, "SIP Client"},
            {ServiceType::INBOUND_AUDIO_PROCESSOR, "IAP"},
            {ServiceType::VAD_SERVICE, "VAD"},
            {ServiceType::WHISPER_SERVICE, "Whisper"},
            {ServiceType::LLAMA_SERVICE, "LLaMA"},
            {ServiceType::TTS_SERVICE, "TTS"},
            {ServiceType::OUTBOUND_AUDIO_PROCESSOR, "OAP"},
        };
        for (const auto& [svc, name] : required_svc) {
            std::string err;
            std::string resp = tcp_command(service_cmd_port(svc), "PING", err, 3);
            if (resp.find("PONG") == std::string::npos) {
                mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                    "{\"error\":\"%s not reachable — start all 7 pipeline services first\"}", name);
                return;
            }
        }

        std::string sip_err;
        std::string sip_status = http_get_localhost(TEST_SIP_PROVIDER_PORT, "/status", sip_err);
        if (!sip_err.empty() || sip_status.find("\"call_active\":true") == std::string::npos) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                "{\"error\":\"No active call on test_sip_provider — start SIP client and establish a call first\"}");
            return;
        }

        std::vector<std::string> files;
        {
            std::lock_guard<std::mutex> lock(testfiles_mutex_);
            for (const auto& tf : testfiles_) files.push_back(tf.name);
        }
        if (files.empty()) files.push_back("sample_01.wav");

        auto progress = std::make_shared<PipelineStressProgress>();
        progress->duration_s.store(duration_s);
        {
            std::lock_guard<std::mutex> lock(pipeline_stress_mutex_);
            pipeline_stress_ = progress;
        }

        std::thread([this, duration_s, files, progress]() {
            run_pipeline_stress_async(duration_s, files, progress);
        }).detach();

        mg_http_reply(c, 202, "Content-Type: application/json\r\n",
            "{\"status\":\"started\",\"duration_s\":%d}", duration_s);
    }

    // GET /api/tests/pipeline_stress/progress — Returns running state and per-service
    // restart counts for the active pipeline stress test.
    void handle_pipeline_stress_progress(struct mg_connection *c, struct mg_http_message *hm) {
        (void)hm;
        std::shared_ptr<PipelineStressProgress> p;
        {
            std::lock_guard<std::mutex> lock(pipeline_stress_mutex_);
            p = pipeline_stress_;
        }
        if (!p) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                "{\"running\":false,\"error\":\"No stress test has been run\"}");
            return;
        }

        static const char* svc_names[] = {"SIP","IAP","VAD","Whisper","LLaMA","TTS","OAP"};
        std::stringstream json;
        json << "{\"running\":" << (p->running.load() ? "true" : "false")
             << ",\"elapsed_s\":" << p->elapsed_s.load()
             << ",\"duration_s\":" << p->duration_s.load()
             << ",\"cycles_completed\":" << p->cycles_completed.load()
             << ",\"cycles_ok\":" << p->cycles_ok.load()
             << ",\"cycles_fail\":" << p->cycles_fail.load()
             << ",\"total_latency_ms\":" << p->total_latency_ms.load()
             << ",\"min_latency_ms\":" << p->min_latency_ms.load()
             << ",\"max_latency_ms\":" << p->max_latency_ms.load()
             << ",\"services\":[";
        for (int i = 0; i < 7; i++) {
            if (i > 0) json << ",";
            int ok = p->svcs[i].ping_ok.load();
            int fail = p->svcs[i].ping_fail.load();
            int avg_ms = ok > 0 ? p->svcs[i].total_ping_ms.load() / ok : 0;
            json << "{\"name\":\"" << svc_names[i] << "\""
                 << ",\"reachable\":" << (p->svcs[i].reachable.load() ? "true" : "false")
                 << ",\"ping_ok\":" << ok
                 << ",\"ping_fail\":" << fail
                 << ",\"avg_ping_ms\":" << avg_ms
                 << ",\"memory_mb\":" << p->svcs[i].memory_mb.load() << "}";
        }
        json << "]";
        if (!p->running.load()) {
            std::lock_guard<std::mutex> lock(p->result_mutex);
            if (!p->result_json.empty()) {
                json << ",\"result\":" << p->result_json;
            }
        }
        json << "}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    // POST /api/tests/pipeline_stress/stop — Signals the running stress test to stop.
    void handle_pipeline_stress_stop(struct mg_connection *c, struct mg_http_message *hm) {
        (void)hm;
        std::shared_ptr<PipelineStressProgress> p;
        {
            std::lock_guard<std::mutex> lock(pipeline_stress_mutex_);
            p = pipeline_stress_;
        }
        if (p && p->running.load()) {
            p->stop_requested.store(true);
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\":\"stopping\"}");
        } else {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\":\"not_running\"}");
        }
    }

    void run_pipeline_stress_async(int duration_s, std::vector<std::string> files,
                                    std::shared_ptr<PipelineStressProgress> progress) {
        static const ServiceType svc_types[] = {
            ServiceType::SIP_CLIENT,
            ServiceType::INBOUND_AUDIO_PROCESSOR,
            ServiceType::VAD_SERVICE,
            ServiceType::WHISPER_SERVICE,
            ServiceType::LLAMA_SERVICE,
            ServiceType::TTS_SERVICE,
            ServiceType::OUTBOUND_AUDIO_PROCESSOR,
        };
        static const char* svc_service_names[] = {
            "SIP_CLIENT", "INBOUND_AUDIO_PROCESSOR", "VAD_SERVICE",
            "WHISPER_SERVICE", "LLAMA_SERVICE", "TTS_SERVICE",
            "OUTBOUND_AUDIO_PROCESSOR"
        };

        auto start_time = std::chrono::steady_clock::now();
        auto deadline = start_time + std::chrono::seconds(duration_s);
        int file_idx = 0;

        std::thread health_thread([this, start_time, progress]() {
            while (progress->running.load() && !progress->stop_requested.load()) {
                for (int i = 0; i < 7; i++) {
                    uint16_t port = service_cmd_port(svc_types[i]);
                    auto t0 = std::chrono::steady_clock::now();
                    std::string err;
                    std::string resp = tcp_command(port, "PING", err, 2);
                    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - t0).count();
                    if (resp.find("PONG") != std::string::npos) {
                        progress->svcs[i].reachable.store(true);
                        progress->svcs[i].ping_ok++;
                        progress->svcs[i].total_ping_ms += (int)ms;
                    } else {
                        progress->svcs[i].reachable.store(false);
                        progress->svcs[i].ping_fail++;
                    }
                    progress->svcs[i].memory_mb.store(get_service_memory_mb(svc_service_names[i]));
                }
                auto now = std::chrono::steady_clock::now();
                progress->elapsed_s.store((int)std::chrono::duration_cast<std::chrono::seconds>(
                    now - start_time).count());
                std::this_thread::sleep_for(std::chrono::seconds(5));
            }
        });

        while (std::chrono::steady_clock::now() < deadline && !progress->stop_requested.load()) {
            const auto& file = files[file_idx % files.size()];
            file_idx++;

            uint64_t seq_before = current_log_seq();
            auto cycle_start = std::chrono::steady_clock::now();

            std::string inject_body = "{\"file\":\"" + escape_json(file) + "\",\"leg\":\"a\"}";
            std::string inject_err;
            std::string inject_resp = http_post_localhost(TEST_SIP_PROVIDER_PORT, "/inject", inject_body, inject_err);

            bool inject_failed = (!inject_err.empty() ||
                inject_resp.find("\"success\":true") == std::string::npos);
            bool cycle_ok = false;
            if (inject_failed) {
                progress->cycles_fail++;
            } else {
                TranscriptionResult tr = wait_for_whisper_transcription(seq_before, 30000);
                if (tr.found && !tr.text.empty()) {
                    cycle_ok = true;
                }
            }

            auto cycle_end = std::chrono::steady_clock::now();
            int cycle_ms = (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                cycle_end - cycle_start).count();

            if (cycle_ok) {
                progress->cycles_ok++;
                progress->total_latency_ms += cycle_ms;
                int cur_min = progress->min_latency_ms.load();
                while (cycle_ms < cur_min && !progress->min_latency_ms.compare_exchange_weak(cur_min, cycle_ms));
                int cur_max = progress->max_latency_ms.load();
                while (cycle_ms > cur_max && !progress->max_latency_ms.compare_exchange_weak(cur_max, cycle_ms));
            } else if (!inject_failed) {
                progress->cycles_fail++;
            }
            progress->cycles_completed++;

            auto now = std::chrono::steady_clock::now();
            progress->elapsed_s.store((int)std::chrono::duration_cast<std::chrono::seconds>(
                now - start_time).count());

            std::this_thread::sleep_for(std::chrono::milliseconds(PIPELINE_ROUND_POLL_MS));
        }

        progress->running.store(false);
        if (health_thread.joinable()) health_thread.join();

        auto final_elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - start_time).count();
        progress->elapsed_s.store((int)final_elapsed);

        int cyc = progress->cycles_completed.load();
        int ok = progress->cycles_ok.load();
        int fail = progress->cycles_fail.load();
        int total_lat = progress->total_latency_ms.load();
        int avg_lat = ok > 0 ? total_lat / ok : 0;

        std::stringstream result;
        result << "{\"total_cycles\":" << cyc
               << ",\"cycles_ok\":" << ok
               << ",\"cycles_fail\":" << fail
               << ",\"success_pct\":" << (cyc > 0 ? (int)(100.0 * ok / cyc) : 0)
               << ",\"avg_latency_ms\":" << avg_lat
               << ",\"duration_s\":" << (int)final_elapsed
               << "}";

        {
            std::lock_guard<std::mutex> lock(progress->result_mutex);
            progress->result_json = result.str();
        }

        if (db_) {
            std::string status_str = (cyc > 0 && ok * 100 / cyc >= 90) ? "PASS" : "FAIL";
            std::string metrics = result.str();
            std::string details = std::to_string(cyc) + " cycles, " + std::to_string(ok)
                + " ok, " + std::to_string(fail) + " fail, avg_lat=" + std::to_string(avg_lat)
                + "ms, " + std::to_string((int)final_elapsed) + "s";
            const char* sql1 = "INSERT INTO service_test_runs (service,test_type,status,metrics_json,timestamp) "
                "VALUES ('full_pipeline','pipeline_stress',?1,?2,strftime('%s','now'))";
            sqlite3_stmt* stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql1, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, status_str.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, metrics.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
            const char* sql2 = "INSERT INTO test_results (test_name,service,status,details,timestamp) "
                "VALUES ('pipeline_stress','full_pipeline',?1,?2,strftime('%s','now'))";
            stmt = nullptr;
            if (sqlite3_prepare_v2(db_, sql2, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, status_str.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, details.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }
    }

    // GET /api/async/status?task_id=N — Poll any async task by ID. Returns
    // "running" or the final result JSON when complete.
    void handle_async_status(struct mg_connection *c, struct mg_http_message *hm) {
        char id_buf[32] = {0};
        mg_http_get_var(&hm->query, "task_id", id_buf, sizeof(id_buf));
        int64_t task_id = atoll(id_buf);
        if (task_id <= 0) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing task_id\"}");
            return;
        }
        std::lock_guard<std::mutex> lock(async_mutex_);
        auto it = async_tasks_.find(task_id);
        if (it == async_tasks_.end()) {
            mg_http_reply(c, 404, "Content-Type: application/json\r\n", "{\"error\":\"Unknown task_id\"}");
            return;
        }
        if (it->second->running.load()) {
            std::string prog = it->second->progress_json;
            if (prog.empty()) {
                mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\":\"running\"}");
            } else {
                mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", prog.c_str());
            }
        } else {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", it->second->result_json.c_str());
            it->second->result_read = true;
        }
    }

    // POST /api/whisper/accuracy_test — Run offline Whisper accuracy test on selected
    // WAV files. Async: returns task_id immediately, test runs in background thread.
    // Each file is decoded, upsampled, and fed directly to whisper_full() for WER scoring.
    void handle_whisper_accuracy_test(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        std::string body(hm->body.buf, hm->body.len);

        size_t files_start = body.find("\"files\":");
        if (files_start == std::string::npos) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing files parameter\"}");
            return;
        }

        std::vector<std::string> test_files;
        size_t arr_start = body.find('[', files_start);
        size_t arr_end = body.find(']', arr_start);
        if (arr_start != std::string::npos && arr_end != std::string::npos) {
            std::string arr_content = body.substr(arr_start + 1, arr_end - arr_start - 1);
            size_t pos = 0;
            while (pos < arr_content.length()) {
                size_t quote1 = arr_content.find('"', pos);
                if (quote1 == std::string::npos) break;
                size_t quote2 = arr_content.find('"', quote1 + 1);
                if (quote2 == std::string::npos) break;
                test_files.push_back(arr_content.substr(quote1 + 1, quote2 - quote1 - 1));
                pos = quote2 + 1;
            }
        }

        if (test_files.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"No test files specified\"}");
            return;
        }

        int64_t task_id = create_async_task("accuracy_test");

        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            auto& task = async_tasks_[task_id];
            task->worker = std::thread(&FrontendServer::run_accuracy_test_async, this, task_id, test_files);
        }

        mg_http_reply(c, 202, "Content-Type: application/json\r\n",
            "{\"status\":\"started\",\"task_id\":%lld}", (long long)task_id);
    }

    void run_accuracy_test_async(int64_t task_id, std::vector<std::string> test_files) {
        std::string pipeline_err = validate_pipeline_services();
        if (!pipeline_err.empty()) {
            finish_async_task(task_id, "{\"error\":\"" + escape_json(pipeline_err) + "\"}");
            return;
        }

        std::string sip_err;
        std::string status_body = http_get_localhost(TEST_SIP_PROVIDER_PORT, "/status", sip_err);
        if (!sip_err.empty()) {
            finish_async_task(task_id, "{\"error\":\"test_sip_provider not reachable: " + escape_json(sip_err) + "\"}");
            return;
        }
        if (status_body.find("\"call_active\":true") == std::string::npos) {
            finish_async_task(task_id, "{\"error\":\"No active call in test_sip_provider. Start SIP client and establish a call first.\"}");
            return;
        }

        int64_t test_run_id = time(nullptr);
        std::stringstream json;
        json << "{\"status\":\"done\",\"success\":true,\"test_run_id\":" << test_run_id << ",\"results\":[";

        bool first = true;
        int pass_count = 0, warn_count = 0, fail_count = 0;
        double total_similarity = 0.0;
        double total_latency = 0.0;
        int processed = 0;

        for (const auto& file : test_files) {
            std::string ground_truth;
            {
                std::lock_guard<std::mutex> lock(testfiles_mutex_);
                for (const auto& tf : testfiles_) {
                    if (tf.name == file) {
                        ground_truth = tf.ground_truth;
                        break;
                    }
                }
            }

            if (ground_truth.empty()) {
                continue;
            }

            uint64_t seq_before = current_log_seq();
            auto inject_start = std::chrono::steady_clock::now();

            std::string inject_body = "{\"file\":\"" + escape_json(file) + "\",\"leg\":\"a\"}";
            std::string inject_err;
            std::string inject_resp = http_post_localhost(TEST_SIP_PROVIDER_PORT, "/inject", inject_body, inject_err);

            if (!inject_err.empty() || inject_resp.find("\"success\":true") == std::string::npos) {
                if (!first) json << ",";
                json << "{"
                     << "\"file\":\"" << escape_json(file) << "\","
                     << "\"ground_truth\":\"" << escape_json(ground_truth) << "\","
                     << "\"transcription\":\"" << escape_json("[injection failed]") << "\","
                     << "\"similarity\":0,"
                     << "\"latency_ms\":0,"
                     << "\"status\":\"FAIL\""
                     << "}";
                first = false;
                fail_count++;
                continue;
            }

            TranscriptionResult tr = wait_for_whisper_transcription(seq_before, 30000);

            auto inject_end = std::chrono::steady_clock::now();
            double total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                inject_end - inject_start).count();

            std::string transcription;
            double latency_ms;
            if (tr.found) {
                transcription = tr.text;
                latency_ms = tr.whisper_latency_ms > 0 ? tr.whisper_latency_ms : total_ms;
            } else {
                transcription = "[no transcription received within 30s]";
                latency_ms = total_ms;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(ACCURACY_INTER_FILE_MS));

            double similarity = calculate_levenshtein_similarity(ground_truth, transcription);

            std::string file_status;
            if (similarity >= 95.0) {
                file_status = "PASS";
                pass_count++;
            } else if (similarity >= 80.0) {
                file_status = "WARN";
                warn_count++;
            } else {
                file_status = "FAIL";
                fail_count++;
            }

            total_similarity += similarity;
            total_latency += latency_ms;
            processed++;

            const char* sql = "INSERT INTO whisper_accuracy_tests (test_run_id, file_name, model_name, ground_truth, transcription, similarity_percent, latency_ms, status, timestamp) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, test_run_id);
                sqlite3_bind_text(stmt, 2, file.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 3, "current", -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 4, ground_truth.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 5, transcription.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_double(stmt, 6, similarity);
                sqlite3_bind_int(stmt, 7, (int)latency_ms);
                sqlite3_bind_text(stmt, 8, file_status.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int64(stmt, 9, test_run_id);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }

            if (!first) json << ",";
            json << "{"
                 << "\"file\":\"" << escape_json(file) << "\","
                 << "\"ground_truth\":\"" << escape_json(ground_truth) << "\","
                 << "\"transcription\":\"" << escape_json(transcription) << "\","
                 << "\"similarity\":" << similarity << ","
                 << "\"latency_ms\":" << latency_ms << ","
                 << "\"status\":\"" << file_status << "\""
                 << "}";
            first = false;
        }

        int total_files = processed;
        double avg_similarity = total_files > 0 ? total_similarity / total_files : 0.0;
        double avg_latency = total_files > 0 ? total_latency / total_files : 0.0;

        json << "],"
             << "\"total\":" << total_files << ","
             << "\"pass_count\":" << pass_count << ","
             << "\"warn_count\":" << warn_count << ","
             << "\"fail_count\":" << fail_count << ","
             << "\"avg_accuracy\":" << avg_similarity << ","
             << "\"avg_latency_ms\":" << avg_latency
             << "}";

        finish_async_task(task_id, json.str());
    }

    // GET/POST /api/vad/config — GET: query running VAD service for live params via
    // STATUS cmd, falling back to saved settings if service is down. POST: persist
    // settings AND send SET_VAD_* commands to the running service for live tuning.
    // window_ms is startup-only (no runtime command) — saved for next restart.
    void handle_vad_config(struct mg_connection *c, struct mg_http_message *hm) {
        int vad_cmd_port = whispertalk::service_cmd_port(whispertalk::ServiceType::VAD_SERVICE);

        if (mg_strcmp(hm->method, mg_str("POST")) == 0) {
            std::string body(hm->body.buf, hm->body.len);

            std::string window_ms_str = extract_json_string(body, "window_ms");
            std::string threshold_str = extract_json_string(body, "threshold");
            std::string silence_ms_str = extract_json_string(body, "silence_ms");
            std::string max_chunk_ms_str = extract_json_string(body, "max_chunk_ms");
            std::string onset_gap_str = extract_json_string(body, "onset_gap");
            std::string post_idle_cooldown_str = extract_json_string(body, "post_idle_cooldown_ms");
            std::string rms_gate_str = extract_json_string(body, "rms_gate");

            if (!window_ms_str.empty()) set_setting("vad_window_ms", window_ms_str);
            if (!threshold_str.empty()) set_setting("vad_threshold", threshold_str);
            if (!silence_ms_str.empty()) set_setting("vad_silence_ms", silence_ms_str);
            if (!max_chunk_ms_str.empty()) set_setting("vad_max_chunk_ms", max_chunk_ms_str);
            if (!onset_gap_str.empty()) set_setting("vad_onset_gap", onset_gap_str);
            if (!post_idle_cooldown_str.empty()) set_setting("vad_post_idle_cooldown_ms", post_idle_cooldown_str);
            if (!rms_gate_str.empty()) set_setting("vad_rms_gate", rms_gate_str);

            bool live = is_service_running("VAD_SERVICE");
            bool all_ok = true;
            std::string actual_max_chunk;
            if (live) {
                std::string err, resp;
                if (!threshold_str.empty()) {
                    resp = tcp_command(vad_cmd_port, "SET_VAD_THRESHOLD:" + threshold_str + "\n", err, 3);
                    if (resp.find("OK") == std::string::npos) all_ok = false;
                }
                if (!silence_ms_str.empty()) {
                    resp = tcp_command(vad_cmd_port, "SET_VAD_SILENCE_MS:" + silence_ms_str + "\n", err, 3);
                    if (resp.find("OK") == std::string::npos) all_ok = false;
                }
                if (!max_chunk_ms_str.empty()) {
                    resp = tcp_command(vad_cmd_port, "SET_VAD_MAX_CHUNK_MS:" + max_chunk_ms_str + "\n", err, 3);
                    if (resp.find("OK") == std::string::npos) {
                        all_ok = false;
                    } else {
                        size_t colon = resp.find(':');
                        size_t ms_pos = resp.find("ms");
                        if (colon != std::string::npos && ms_pos != std::string::npos && ms_pos > colon + 1)
                            actual_max_chunk = resp.substr(colon + 1, ms_pos - colon - 1);
                    }
                }
                if (!onset_gap_str.empty()) {
                    resp = tcp_command(vad_cmd_port, "SET_VAD_ONSET_GAP:" + onset_gap_str + "\n", err, 3);
                    if (resp.find("OK") == std::string::npos) all_ok = false;
                }
                if (!post_idle_cooldown_str.empty()) {
                    resp = tcp_command(vad_cmd_port, "SET_POST_IDLE_COOLDOWN_MS:" + post_idle_cooldown_str + "\n", err, 3);
                    if (resp.find("OK") == std::string::npos) all_ok = false;
                }
                if (!rms_gate_str.empty()) {
                    resp = tcp_command(vad_cmd_port, "SET_RMS_GATE:" + rms_gate_str + "\n", err, 3);
                    if (resp.find("OK") == std::string::npos) all_ok = false;
                }
                if (!all_ok) live = false;
            }

            std::string w = window_ms_str.empty() ? get_setting("vad_window_ms", "50") : window_ms_str;
            std::string t = threshold_str.empty() ? get_setting("vad_threshold", "2.0") : threshold_str;
            std::string s = silence_ms_str.empty() ? get_setting("vad_silence_ms", "400") : silence_ms_str;
            std::string m = max_chunk_ms_str.empty() ? get_setting("vad_max_chunk_ms", "8000") : max_chunk_ms_str;
            if (!actual_max_chunk.empty()) m = actual_max_chunk;
            std::string g = onset_gap_str.empty() ? get_setting("vad_onset_gap", "1") : onset_gap_str;
            std::string pic = post_idle_cooldown_str.empty() ? get_setting("vad_post_idle_cooldown_ms", "1200") : post_idle_cooldown_str;
            std::string rg = rms_gate_str.empty() ? get_setting("vad_rms_gate", "0.04") : rms_gate_str;

            auto safe_num = [](const std::string& v, const char* fallback) -> const char* {
                for (char ch : v) if (!isdigit(ch) && ch != '.') return fallback;
                return v.c_str();
            };
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                "{\"success\":true,\"live\":%s,\"window_ms\":%s,\"threshold\":%s,\"silence_ms\":%s,\"max_chunk_ms\":%s,\"onset_gap\":%s,\"post_idle_cooldown_ms\":%s,\"rms_gate\":%s}",
                live ? "true" : "false", safe_num(w,"50"), safe_num(t,"2.0"), safe_num(s,"400"), safe_num(m,"8000"), safe_num(g,"1"), safe_num(pic,"1200"), safe_num(rg,"0.04"));
        } else {
            std::string err;
            std::string status = tcp_command(vad_cmd_port, "STATUS\n", err, 3);
            bool live = err.empty() && !status.empty() && status.find("ACTIVE_CALLS:") != std::string::npos;

            std::string window_ms, threshold, silence_ms, max_chunk_ms, onset_gap, post_idle_cooldown, rms_gate;
            if (live) {
                auto extract_field = [&](const std::string& key) -> std::string {
                    size_t pos = status.find(key + ":");
                    if (pos == std::string::npos) return "";
                    pos += key.size() + 1;
                    size_t end = status.find(':', pos);
                    if (end == std::string::npos) {
                        end = status.find('\n', pos);
                        if (end == std::string::npos) end = status.size();
                    }
                    return status.substr(pos, end - pos);
                };
                window_ms = extract_field("WINDOW_MS");
                threshold = extract_field("THRESHOLD");
                silence_ms = extract_field("SILENCE_MS");
                max_chunk_ms = extract_field("MAX_CHUNK_MS");
                onset_gap = extract_field("ONSET_GAP");
                post_idle_cooldown = extract_field("POST_IDLE_COOLDOWN_MS");
                rms_gate = extract_field("RMS_GATE");
            }

            if (window_ms.empty()) window_ms = get_setting("vad_window_ms", "50");
            if (threshold.empty()) threshold = get_setting("vad_threshold", "2.0");
            if (silence_ms.empty()) silence_ms = get_setting("vad_silence_ms", "400");
            if (max_chunk_ms.empty()) max_chunk_ms = get_setting("vad_max_chunk_ms", "8000");
            if (onset_gap.empty()) onset_gap = get_setting("vad_onset_gap", "1");
            if (post_idle_cooldown.empty()) post_idle_cooldown = get_setting("vad_post_idle_cooldown_ms", "1200");
            if (rms_gate.empty()) rms_gate = get_setting("vad_rms_gate", "0.04");

            auto safe_num = [](const std::string& s, const std::string& fallback) {
                if (s.empty()) return fallback;
                size_t i = 0;
                if (s[0] == '-' || s[0] == '+') i = 1;
                bool seen_digit = false, seen_dot = false;
                for (; i < s.size(); i++) {
                    if (isdigit((unsigned char)s[i])) { seen_digit = true; }
                    else if (s[i] == '.' && !seen_dot) { seen_dot = true; }
                    else return fallback;
                }
                return seen_digit ? s : fallback;
            };

            std::stringstream json;
            json << "{"
                 << "\"live\":" << (live ? "true" : "false") << ","
                 << "\"window_ms\":" << safe_num(window_ms, "50") << ","
                 << "\"threshold\":" << safe_num(threshold, "2.0") << ","
                 << "\"silence_ms\":" << safe_num(silence_ms, "400") << ","
                 << "\"max_chunk_ms\":" << safe_num(max_chunk_ms, "8000") << ","
                 << "\"onset_gap\":" << safe_num(onset_gap, "1") << ","
                 << "\"post_idle_cooldown_ms\":" << safe_num(post_idle_cooldown, "1200") << ","
                 << "\"rms_gate\":" << safe_num(rms_gate, "0.04")
                 << "}";
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
        }
    }

    // POST /api/whisper/hallucination_filter — Toggle hallucination filter on/off
    // by sending HALLUCINATION_FILTER:ON/OFF to Whisper's cmd port (13122).
    void handle_whisper_hallucination_filter(struct mg_connection *c, struct mg_http_message *hm) {
        int whisper_cmd_port = whispertalk::service_cmd_port(whispertalk::ServiceType::WHISPER_SERVICE);
        std::string err;

        if (mg_strcmp(hm->method, mg_str("POST")) == 0) {
            std::string body(hm->body.buf, hm->body.len);
            std::string enabled_str = extract_json_string(body, "enabled");
            bool enable = (enabled_str == "true" || enabled_str == "1");
            std::string cmd = enable ? "HALLUCINATION_FILTER:ON\n" : "HALLUCINATION_FILTER:OFF\n";
            std::string resp = tcp_command(whisper_cmd_port, cmd, err, 3);
            if (!err.empty()) {
                mg_http_reply(c, 503, "Content-Type: application/json\r\n",
                    "{\"error\":\"%s\"}", err.c_str());
                return;
            }
            bool confirmed = resp.find(":ON") != std::string::npos;
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                "{\"success\":true,\"enabled\":%s}", confirmed ? "true" : "false");
        } else {
            std::string resp = tcp_command(whisper_cmd_port, "HALLUCINATION_FILTER:STATUS\n", err, 3);
            if (!err.empty()) {
                mg_http_reply(c, 503, "Content-Type: application/json\r\n",
                    "{\"error\":\"%s\",\"enabled\":false}", err.c_str());
                return;
            }
            bool enabled = resp.find(":ON") != std::string::npos;
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                "{\"enabled\":%s}", enabled ? "true" : "false");
        }
    }

    // GET /api/oap/wav_recording — Query WAV recording state of OAP.
    // POST /api/oap/wav_recording — Enable/disable WAV recording and set save directory.
    void handle_oap_wav_recording(struct mg_connection *c, struct mg_http_message *hm) {
        uint16_t oap_cmd_port = whispertalk::service_cmd_port(whispertalk::ServiceType::OUTBOUND_AUDIO_PROCESSOR);
        std::string err;

        if (mg_strcmp(hm->method, mg_str("POST")) == 0) {
            std::string body(hm->body.buf, hm->body.len);
            std::string enabled_str = extract_json_string(body, "enabled");
            std::string dir = extract_json_string(body, "dir");
            bool enable;
            if (!enabled_str.empty()) {
                enable = (enabled_str == "true" || enabled_str == "1");
            } else {
                auto pos = body.find("\"enabled\"");
                if (pos != std::string::npos) {
                    pos = body.find_first_not_of(" \t\r\n:", pos + 9);
                    enable = (pos != std::string::npos && body.substr(pos, 4) == "true");
                } else {
                    enable = false;
                }
            }

            if (!dir.empty()) {
                if (dir.find("..") != std::string::npos || dir[0] == '/') {
                    mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Invalid directory path\"}");
                    return;
                }
                std::string dir_cmd = "SET_SAVE_WAV_DIR:" + dir + "\n";
                tcp_command(oap_cmd_port, dir_cmd, err, 3);
                if (!err.empty()) {
                    mg_http_reply(c, 503, "Content-Type: application/json\r\n",
                        "{\"error\":\"%s\"}", err.c_str());
                    return;
                }
            }

            std::string cmd = enable ? "SAVE_WAV:ON\n" : "SAVE_WAV:OFF\n";
            std::string resp = tcp_command(oap_cmd_port, cmd, err, 3);
            if (!err.empty()) {
                mg_http_reply(c, 503, "Content-Type: application/json\r\n",
                    "{\"error\":\"%s\"}", err.c_str());
                return;
            }
            bool confirmed = resp.find("SAVE_WAV:ON") != std::string::npos;
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                "{\"success\":true,\"enabled\":%s}", confirmed ? "true" : "false");
        } else {
            std::string resp = tcp_command(oap_cmd_port, "SAVE_WAV:STATUS\n", err, 3);
            if (!err.empty()) {
                mg_http_reply(c, 503, "Content-Type: application/json\r\n",
                    "{\"error\":\"%s\",\"enabled\":false,\"dir\":\"\"}", err.c_str());
                return;
            }
            bool enabled = resp.find("SAVE_WAV:ON") != std::string::npos;
            std::string dir;
            size_t dir_pos = resp.find(":DIR:");
            if (dir_pos != std::string::npos) {
                dir = resp.substr(dir_pos + 5);
                while (!dir.empty() && (dir.back() == '\n' || dir.back() == '\r'))
                    dir.pop_back();
            }
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                "{\"enabled\":%s,\"dir\":\"%s\"}", enabled ? "true" : "false",
                escape_json(dir).c_str());
        }
    }

    // GET /api/whisper/accuracy_results — Returns paginated Whisper accuracy test
    // results from SQLite, grouped by test_run_id with PASS/WARN/FAIL counts.
    void handle_whisper_accuracy_results(struct mg_connection *c, struct mg_http_message *hm) {
        int limit_val = 20;
        if (hm->query.len > 0) {
            std::string query(hm->query.buf, hm->query.len);
            size_t limit_pos = query.find("limit=");
            if (limit_pos != std::string::npos) {
                std::string limit_str = query.substr(limit_pos + 6);
                size_t amp = limit_str.find('&');
                if (amp != std::string::npos) limit_str = limit_str.substr(0, amp);
                int parsed = atoi(limit_str.c_str());
                if (parsed > 0 && parsed <= 1000) limit_val = parsed;
            }
        }
        
        std::string sql = "SELECT test_run_id, COUNT(*) as total, "
                         "SUM(CASE WHEN status='PASS' THEN 1 ELSE 0 END) as pass_count, "
                         "SUM(CASE WHEN status='WARN' THEN 1 ELSE 0 END) as warn_count, "
                         "SUM(CASE WHEN status='FAIL' THEN 1 ELSE 0 END) as fail_count, "
                         "AVG(similarity_percent) as avg_similarity, "
                         "AVG(latency_ms) as avg_latency, "
                         "timestamp "
                         "FROM whisper_accuracy_tests "
                         "GROUP BY test_run_id "
                         "ORDER BY timestamp DESC "
                         "LIMIT " + std::to_string(limit_val);
        
        sqlite3_stmt* stmt;
        std::stringstream json;
        json << "{\"results\":[";
        
        bool first = true;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (!first) json << ",";
                json << "{"
                     << "\"test_run_id\":" << sqlite3_column_int64(stmt, 0) << ","
                     << "\"total\":" << sqlite3_column_int(stmt, 1) << ","
                     << "\"pass\":" << sqlite3_column_int(stmt, 2) << ","
                     << "\"warn\":" << sqlite3_column_int(stmt, 3) << ","
                     << "\"fail\":" << sqlite3_column_int(stmt, 4) << ","
                     << "\"avg_similarity\":" << sqlite3_column_double(stmt, 5) << ","
                     << "\"avg_latency_ms\":" << sqlite3_column_double(stmt, 6) << ","
                     << "\"timestamp\":" << sqlite3_column_int64(stmt, 7)
                     << "}";
                first = false;
            }
            sqlite3_finalize(stmt);
        }
        
        json << "]}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    void scan_testfiles_directory() {
        std::lock_guard<std::mutex> lock(testfiles_mutex_);
        testfiles_.clear();

        DIR* dir = opendir("Testfiles");
        if (!dir) {
            std::cerr << "Warning: Testfiles directory not found\n";
            return;
        }

        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name.size() <= 4 || name.substr(name.size() - 4) != ".wav") continue;

            std::string wav_path = "Testfiles/" + name;
            std::string txt_name = name.substr(0, name.size() - 4) + ".txt";
            std::string txt_path = "Testfiles/" + txt_name;

            struct stat st;
            if (stat(wav_path.c_str(), &st) != 0) continue;

            TestFileInfo info;
            info.name = name;
            info.size_bytes = st.st_size;
            info.last_modified = st.st_mtime;
            info.sample_rate = 0;
            info.channels = 0;
            info.duration_sec = 0.0;

            std::ifstream wav_file(wav_path, std::ios::binary);
            if (wav_file) {
                char riff_header[12];
                wav_file.read(riff_header, 12);
                if (wav_file && std::memcmp(riff_header, "RIFF", 4) == 0 && 
                    std::memcmp(riff_header + 8, "WAVE", 4) == 0) {
                    
                    while (wav_file) {
                        char chunk_id[4];
                        uint32_t chunk_size;
                        wav_file.read(chunk_id, 4);
                        wav_file.read(reinterpret_cast<char*>(&chunk_size), 4);
                        if (!wav_file) break;

                        if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
                            if (chunk_size >= 16) {
                                uint16_t audio_format;
                                wav_file.read(reinterpret_cast<char*>(&audio_format), 2);
                                wav_file.read(reinterpret_cast<char*>(&info.channels), 2);
                                wav_file.read(reinterpret_cast<char*>(&info.sample_rate), 4);
                                if (chunk_size > 16) {
                                    wav_file.seekg(chunk_size - 16, std::ios::cur);
                                }
                            }
                        } else if (std::memcmp(chunk_id, "data", 4) == 0) {
                            if (info.sample_rate > 0 && info.channels > 0) {
                                info.duration_sec = (double)chunk_size / (info.sample_rate * info.channels * 2);
                            }
                            break;
                        } else {
                            wav_file.seekg(chunk_size, std::ios::cur);
                        }
                    }
                }
            }

            std::ifstream txt_file(txt_path);
            if (txt_file) {
                std::getline(txt_file, info.ground_truth);
                size_t last_char = info.ground_truth.find_last_not_of(" \t\r\n");
                if (last_char != std::string::npos) {
                    info.ground_truth = info.ground_truth.substr(0, last_char + 1);
                }
            }

            testfiles_.push_back(info);

            if (db_) {
                const char* sql = "INSERT OR REPLACE INTO testfiles (name, size_bytes, duration_sec, sample_rate, channels, ground_truth, last_modified) VALUES (?, ?, ?, ?, ?, ?, ?)";
                sqlite3_stmt* stmt;
                if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, info.name.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(stmt, 2, info.size_bytes);
                    sqlite3_bind_double(stmt, 3, info.duration_sec);
                    sqlite3_bind_int(stmt, 4, info.sample_rate);
                    sqlite3_bind_int(stmt, 5, info.channels);
                    sqlite3_bind_text(stmt, 6, info.ground_truth.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int64(stmt, 7, info.last_modified);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
            }
        }
        closedir(dir);

        std::cout << "Scanned " << testfiles_.size() << " test files\n";
    }

    void serve_testfiles_api(struct mg_connection *c) {
        std::lock_guard<std::mutex> lock(testfiles_mutex_);
        
        std::stringstream json;
        json << "{\"files\":[";
        bool first = true;
        for (const auto& file : testfiles_) {
            if (!first) json << ",";
            json << "{"
                 << "\"name\":\"" << escape_json(file.name) << "\","
                 << "\"size_bytes\":" << file.size_bytes << ","
                 << "\"duration_sec\":" << file.duration_sec << ","
                 << "\"sample_rate\":" << file.sample_rate << ","
                 << "\"channels\":" << file.channels << ","
                 << "\"ground_truth\":\"" << escape_json(file.ground_truth) << "\","
                 << "\"last_modified\":" << file.last_modified
                 << "}";
            first = false;
        }
        json << "]}";
        
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    // POST /api/testfiles/scan — Rescan Testfiles/ directory for WAV+TXT sample pairs.
    void handle_testfiles_scan(struct mg_connection *c) {
        scan_testfiles_directory();
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", 
            "{\"success\":true,\"scanned\":%zu}", testfiles_.size());
    }

    // GET/POST /api/settings/log_level — GET: return per-service log levels from SQLite.
    // POST: persist level to DB, then send SET_LOG_LEVEL:<LEVEL> to the service's cmd
    // port if it's running (graceful — no error if service is offline).
    void handle_log_level_settings(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("GET")) == 0) {
            if (!db_) {
                mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database not available\"}");
                return;
            }

            const char* services[] = {
                "SIP_CLIENT", "INBOUND_AUDIO_PROCESSOR", "VAD_SERVICE", "WHISPER_SERVICE",
                "LLAMA_SERVICE", "TTS_SERVICE", "KOKORO_ENGINE", "NEUTTS_ENGINE",
                "OUTBOUND_AUDIO_PROCESSOR", "TOMEDO_CRAWL_SERVICE", nullptr
            };

            std::stringstream json;
            json << "{\"log_levels\":{";
            bool first = true;
            for (int i = 0; services[i]; i++) {
                std::string key = std::string("log_level_") + services[i];
                std::string level = get_setting(key, "INFO");
                if (!first) json << ",";
                json << "\"" << services[i] << "\":\"" << level << "\"";
                first = false;
            }
            json << "}}";
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
        } else {
            std::string body(hm->body.buf, hm->body.len);
            std::string service = extract_json_string(body, "service");
            std::string level = extract_json_string(body, "level");

            if (service.empty() || level.empty()) {
                mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing service or level\"}");
                return;
            }

            const char* valid_levels[] = {"ERROR", "WARN", "INFO", "DEBUG", "TRACE", nullptr};
            bool valid = false;
            for (int i = 0; valid_levels[i]; i++) {
                if (level == valid_levels[i]) {
                    valid = true;
                    break;
                }
            }

            if (!valid) {
                mg_http_reply(c, 400, "Content-Type: application/json\r\n", 
                    "{\"error\":\"Invalid log level. Must be ERROR, WARN, INFO, DEBUG, or TRACE\"}");
                return;
            }

            std::string key = "log_level_" + service;
            if (db_) {
                sqlite3_stmt* stmt;
                const char* sql = "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)";
                if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 2, level.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
            }

            bool live_update = false;
            {
                // Map by ServiceType for pipeline services that go through
                // send_negotiation_command (uses service_cmd_port()), and
                // by raw cmd-port for TTS engines (which dock into TTS_SERVICE
                // but keep their own private diagnostic cmd-port).
                static const struct { const char* name; whispertalk::ServiceType type; } svc_map[] = {
                    {"SIP_CLIENT",               whispertalk::ServiceType::SIP_CLIENT},
                    {"INBOUND_AUDIO_PROCESSOR",  whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR},
                    {"VAD_SERVICE",              whispertalk::ServiceType::VAD_SERVICE},
                    {"WHISPER_SERVICE",          whispertalk::ServiceType::WHISPER_SERVICE},
                    {"LLAMA_SERVICE",            whispertalk::ServiceType::LLAMA_SERVICE},
                    {"TTS_SERVICE",              whispertalk::ServiceType::TTS_SERVICE},
                    {"OUTBOUND_AUDIO_PROCESSOR", whispertalk::ServiceType::OUTBOUND_AUDIO_PROCESSOR},
                    {"TOMEDO_CRAWL_SERVICE",     whispertalk::ServiceType::TOMEDO_CRAWL_SERVICE},
                };
                bool handled = false;
                for (const auto& m : svc_map) {
                    if (service == m.name) {
                        std::string resp = send_negotiation_command(m.type, "SET_LOG_LEVEL:" + level);
                        live_update = (resp.find("OK") != std::string::npos);
                        handled = true;
                        break;
                    }
                }
                if (!handled && (service == "KOKORO_ENGINE" || service == "NEUTTS_ENGINE")) {
                    uint16_t port = (service == "NEUTTS_ENGINE")
                        ? NEUTTS_ENGINE_CMD_PORT
                        : KOKORO_ENGINE_CMD_PORT;
                    std::string err;
                    std::string resp = tcp_command(port, "SET_LOG_LEVEL:" + level, err, 2);
                    live_update = (resp.find("OK") != std::string::npos);
                }
            }

            mg_http_reply(c, 200, "Content-Type: application/json\r\n", 
                "{\"success\":true,\"service\":\"%s\",\"level\":\"%s\",\"live_update\":%s}", 
                escape_json(service).c_str(), escape_json(level).c_str(), live_update ? "true" : "false");
        }
    }

    // GET /api/test_results — Returns pipeline WER test results from
    // /tmp/pipeline_results_*.json files (written by run_pipeline_test.py).
    void handle_test_results(struct mg_connection *c, struct mg_http_message *hm) {
        if (!db_) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database not available\"}");
            return;
        }

        char svc_buf[256] = {}, st_buf[256] = {};
        mg_http_get_var(&hm->query, "service", svc_buf, sizeof(svc_buf));
        mg_http_get_var(&hm->query, "status", st_buf, sizeof(st_buf));
        std::string service_filter(svc_buf);
        std::string status_filter(st_buf);

        std::string sql = "SELECT id, service, test_type, status, metrics_json, timestamp FROM service_test_runs WHERE 1=1";
        if (!service_filter.empty()) sql += " AND service = ?";
        if (!status_filter.empty()) sql += " AND status = ?";
        sql += " ORDER BY timestamp DESC LIMIT 100";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Query failed\"}");
            return;
        }
        int bind_idx = 1;
        if (!service_filter.empty()) sqlite3_bind_text(stmt, bind_idx++, service_filter.c_str(), -1, SQLITE_TRANSIENT);
        if (!status_filter.empty()) sqlite3_bind_text(stmt, bind_idx++, status_filter.c_str(), -1, SQLITE_TRANSIENT);

        std::stringstream json;
        json << "{\"results\":[";
        int count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (count > 0) json << ",";
            json << "{";
            json << "\"id\":" << sqlite3_column_int(stmt, 0) << ",";
            const char* svc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            const char* tt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
            const char* st = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
            const char* metrics = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            json << "\"service\":\"" << escape_json(svc ? svc : "") << "\",";
            json << "\"test_type\":\"" << escape_json(tt ? tt : "") << "\",";
            json << "\"status\":\"" << escape_json(st ? st : "") << "\",";
            std::string metrics_safe = "{}";
            if (metrics && metrics[0] == '{') {
                std::string ms(metrics);
                auto last = ms.find_last_not_of(" \t\r\n");
                if (last != std::string::npos && ms[last] == '}') metrics_safe = ms;
            }
            json << "\"metrics\":" << metrics_safe << ",";
            json << "\"timestamp\":" << sqlite3_column_int64(stmt, 5);
            json << "}";
            count++;
        }
        sqlite3_finalize(stmt);
        json << "]}";

        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    // GET /api/test_results_summary — Aggregated test results for the Test Results page.
    //
    // Query params: type (filter by test type), status (filter by status), from/to (timestamp range).
    // Response JSON:
    //   { results: [{type,id,service,test_type,status,metrics,timestamp},...],
    //     summary: {total,pass,fail,warn,avg_latency_ms} }
    //
    // Queries multiple DB tables: service_test_runs, whisper_accuracy_tests,
    // model_benchmark_runs, iap_quality_tests. Results are unioned into a single array.
    // Used by the Test Results page chart and table, polled when that page is active.
    void handle_test_results_summary(struct mg_connection *c, struct mg_http_message *hm) {
        if (!db_) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database not available\"}");
            return;
        }

        std::string query_str(hm->query.buf, hm->query.len);
        std::string type_filter, status_filter;
        long from_ts = 0, to_ts = 0;

        auto extract_param = [&](const std::string& key) -> std::string {
            std::string needle = key + "=";
            size_t pos = 0;
            while ((pos = query_str.find(needle, pos)) != std::string::npos) {
                if (pos == 0 || query_str[pos - 1] == '&') {
                    size_t start = pos + needle.size();
                    size_t end = query_str.find('&', start);
                    return query_str.substr(start, end == std::string::npos ? std::string::npos : end - start);
                }
                pos += needle.size();
            }
            return "";
        };

        type_filter = extract_param("type");
        status_filter = extract_param("status");
        if (!status_filter.empty()) {
            static const std::unordered_set<std::string> allowed_statuses = {
                "pass", "fail", "warn", "PASS", "FAIL", "WARN", "passed", "failed", "success", "error"
            };
            if (allowed_statuses.find(status_filter) == allowed_statuses.end()) status_filter.clear();
        }
        std::string from_str = extract_param("from");
        std::string to_str = extract_param("to");
        if (!from_str.empty()) from_ts = std::atol(from_str.c_str());
        if (!to_str.empty()) to_ts = std::atol(to_str.c_str());

        std::stringstream json;
        json << "{";

        int total = 0, pass_count = 0, fail_count = 0, warn_count = 0;
        double total_latency = 0.0;
        int latency_count = 0;

        auto build_where = [&](const std::string& ts_col, const std::string& status_col,
                               const std::string& extra_filter = "") -> std::string {
            std::string w = " WHERE 1=1";
            if (!extra_filter.empty()) w += extra_filter;
            if (!status_filter.empty()) w += " AND " + status_col + " = ?";
            if (from_ts > 0) w += " AND " + ts_col + " >= " + std::to_string(from_ts);
            if (to_ts > 0) w += " AND " + ts_col + " <= " + std::to_string(to_ts);
            return w;
        };
        auto bind_status = [&](sqlite3_stmt* s) {
            if (!status_filter.empty()) sqlite3_bind_text(s, 1, status_filter.c_str(), -1, SQLITE_TRANSIENT);
        };

        json << "\"results\":[";
        bool first = true;

        if (type_filter.empty() || type_filter == "service_test") {
            std::string sql = "SELECT id, service, test_type, status, metrics_json, timestamp FROM service_test_runs"
                + build_where("timestamp", "status") + " ORDER BY timestamp DESC LIMIT 200";
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                bind_status(stmt);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    if (!first) json << ",";
                    first = false;
                    const char* st = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                    const char* svc = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    const char* tt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                    const char* metrics = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
                    std::string status_str = st ? st : "";
                    std::string metrics_safe = "{}";
                    if (metrics && metrics[0] == '{') {
                        std::string ms(metrics);
                        auto last = ms.find_last_not_of(" \t\r\n");
                        if (last != std::string::npos && ms[last] == '}') metrics_safe = ms;
                    }
                    json << "{\"type\":\"service_test\""
                         << ",\"id\":" << sqlite3_column_int(stmt, 0)
                         << ",\"service\":\"" << escape_json(svc ? svc : "") << "\""
                         << ",\"test_type\":\"" << escape_json(tt ? tt : "") << "\""
                         << ",\"status\":\"" << escape_json(status_str) << "\""
                         << ",\"metrics\":" << metrics_safe
                         << ",\"timestamp\":" << sqlite3_column_int64(stmt, 5)
                         << "}";
                    total++;
                    if (status_str == "pass" || status_str == "PASS" || status_str == "passed" || status_str == "success") pass_count++;
                    else if (status_str == "fail" || status_str == "FAIL" || status_str == "failed" || status_str == "error") fail_count++;
                    else if (status_str == "warn" || status_str == "WARN") warn_count++;
                    if (metrics_safe.size() > 2) {
                        size_t lp = metrics_safe.find("\"latency_ms\":");
                        if (lp != std::string::npos) {
                            double lat = std::atof(metrics_safe.c_str() + lp + 13);
                            if (lat > 0) { total_latency += lat; latency_count++; }
                        }
                    }
                }
                sqlite3_finalize(stmt);
            }
        }

        if (type_filter.empty() || type_filter == "whisper_accuracy") {
            std::string sql = "SELECT id, file_name, model_name, status, similarity_percent, latency_ms, timestamp FROM whisper_accuracy_tests"
                + build_where("timestamp", "status") + " ORDER BY timestamp DESC LIMIT 200";
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                bind_status(stmt);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    if (!first) json << ",";
                    first = false;
                    const char* st = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
                    std::string status_str = st ? st : "";
                    double lat = sqlite3_column_double(stmt, 5);
                    json << "{\"type\":\"whisper_accuracy\""
                         << ",\"id\":" << sqlite3_column_int(stmt, 0)
                         << ",\"service\":\"whisper\""
                         << ",\"test_type\":\"accuracy\""
                         << ",\"status\":\"" << escape_json(status_str) << "\""
                         << ",\"metrics\":{\"similarity_percent\":" << sqlite3_column_double(stmt, 4)
                         << ",\"latency_ms\":" << lat << "}"
                         << ",\"timestamp\":" << sqlite3_column_int64(stmt, 6)
                         << "}";
                    total++;
                    if (status_str == "PASS" || status_str == "pass") pass_count++;
                    else if (status_str == "FAIL" || status_str == "fail") fail_count++;
                    else if (status_str == "WARN" || status_str == "warn") warn_count++;
                    if (lat > 0) { total_latency += lat; latency_count++; }
                }
                sqlite3_finalize(stmt);
            }
        }

        if (type_filter.empty() || type_filter == "iap_quality") {
            std::string sql = "SELECT id, file_name, status, latency_ms, snr_db, timestamp FROM iap_quality_tests"
                + build_where("timestamp", "status") + " ORDER BY timestamp DESC LIMIT 200";
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                bind_status(stmt);
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    if (!first) json << ",";
                    first = false;
                    const char* st = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                    std::string status_str = st ? st : "";
                    double lat = sqlite3_column_double(stmt, 3);
                    json << "{\"type\":\"iap_quality\""
                         << ",\"id\":" << sqlite3_column_int(stmt, 0)
                         << ",\"service\":\"iap\""
                         << ",\"test_type\":\"quality\""
                         << ",\"status\":\"" << escape_json(status_str) << "\""
                         << ",\"metrics\":{\"latency_ms\":" << lat
                         << ",\"snr_db\":" << sqlite3_column_double(stmt, 4) << "}"
                         << ",\"timestamp\":" << sqlite3_column_int64(stmt, 5)
                         << "}";
                    total++;
                    if (status_str == "PASS" || status_str == "pass") pass_count++;
                    else if (status_str == "FAIL" || status_str == "fail") fail_count++;
                    else if (status_str == "WARN" || status_str == "warn") warn_count++;
                    if (lat > 0) { total_latency += lat; latency_count++; }
                }
                sqlite3_finalize(stmt);
            }
        }

        if (type_filter.empty() || type_filter == "model_benchmark") {
            std::string bm_where = " WHERE 1=1";
            if (from_ts > 0) bm_where += " AND timestamp >= " + std::to_string(from_ts);
            if (to_ts > 0) bm_where += " AND timestamp <= " + std::to_string(to_ts);
            std::string sql = "SELECT id, model_name, model_type, avg_accuracy, avg_latency_ms, pass_count, fail_count, timestamp FROM model_benchmark_runs"
                + bm_where + " ORDER BY timestamp DESC LIMIT 200";
            sqlite3_stmt* stmt;
            if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    int pc = sqlite3_column_int(stmt, 5);
                    int fc = sqlite3_column_int(stmt, 6);
                    std::string status_str = (fc > 0) ? "fail" : "pass";
                    if (!status_filter.empty()) {
                        std::string sf_lower = status_filter;
                        for (auto& ch : sf_lower) ch = std::tolower(ch);
                        if (sf_lower != status_str) continue;
                    }
                    if (!first) json << ",";
                    first = false;
                    double lat = sqlite3_column_double(stmt, 4);
                    const char* mn = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                    const char* mt = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
                    json << "{\"type\":\"model_benchmark\""
                         << ",\"id\":" << sqlite3_column_int(stmt, 0)
                         << ",\"service\":\"" << escape_json(mt ? mt : "whisper") << "\""
                         << ",\"test_type\":\"benchmark\""
                         << ",\"status\":\"" << status_str << "\""
                         << ",\"metrics\":{\"avg_accuracy\":" << sqlite3_column_double(stmt, 3)
                         << ",\"latency_ms\":" << lat
                         << ",\"model_name\":\"" << escape_json(mn ? mn : "") << "\""
                         << ",\"pass_count\":" << pc
                         << ",\"fail_count\":" << fc << "}"
                         << ",\"timestamp\":" << sqlite3_column_int64(stmt, 7)
                         << "}";
                    total++;
                    if (fc > 0) fail_count++;
                    else pass_count++;
                    if (lat > 0) { total_latency += lat; latency_count++; }
                }
                sqlite3_finalize(stmt);
            }
        }

        json << "]";

        double avg_latency = latency_count > 0 ? total_latency / latency_count : 0;
        double pass_rate = total > 0 ? 100.0 * pass_count / total : 0;

        json << ",\"summary\":{\"total\":" << total
             << ",\"pass\":" << pass_count
             << ",\"fail\":" << fail_count
             << ",\"warn\":" << warn_count
             << ",\"pass_rate\":" << std::fixed << std::setprecision(1) << pass_rate
             << ",\"avg_latency_ms\":" << std::fixed << std::setprecision(1) << avg_latency
             << "}}";

        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    void handle_db_schema(struct mg_connection *c);

    // GET/POST /api/settings — GET: return all key-value pairs from settings table.
    // POST: upsert a key-value pair into settings table.
    void handle_settings(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("GET")) == 0) {
            if (!db_) {
                mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database not available\"}");
                return;
            }
            sqlite3_stmt* stmt;
            const char* sql = "SELECT key, value FROM settings";
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
                mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"DB error\"}");
                return;
            }
            std::stringstream json;
            json << "{\"settings\":{";
            int count = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                if (count > 0) json << ",";
                const char* k = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                const char* v = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
                std::string key_str = k ? k : "";
                std::string val_str = v ? v : "";
                if ((key_str == "hf_token" || key_str == "github_token") && !val_str.empty()) {
                    val_str = "***";
                }
                json << "\"" << escape_json(key_str) << "\":\"" << escape_json(val_str) << "\"";
                count++;
            }
            sqlite3_finalize(stmt);
            json << "}}";
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
        } else {
            std::string body(hm->body.buf, hm->body.len);
            std::string key = extract_json_string(body, "key");
            std::string value = extract_json_string(body, "value");

            if (key.empty()) {
                mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing key\"}");
                return;
            }

            if (key == "hf_token" || key == "github_token") {
                auto trimmed = value;
                trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
                if (!trimmed.empty()) trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);
                if (trimmed.empty()) {
                    if (db_) {
                        sqlite3_stmt* del_stmt;
                        const char* del_sql = "DELETE FROM settings WHERE key = ?";
                        if (sqlite3_prepare_v2(db_, del_sql, -1, &del_stmt, nullptr) == SQLITE_OK) {
                            sqlite3_bind_text(del_stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
                            sqlite3_step(del_stmt);
                            sqlite3_finalize(del_stmt);
                        }
                    }
                    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\":\"saved\"}");
                    return;
                }
                if (trimmed.size() > 4096) {
                    mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Token too long (max 4096)\"}");
                    return;
                }
                value = trimmed;
            }

            if (db_) {
                sqlite3_stmt* stmt;
                const char* sql = "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)";
                if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, key.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 2, value.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
            }
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\":\"saved\"}");
        }
    }

    bool rag_db_set_config(const std::string& key, const std::string& value) {
        sqlite3* rag_db = nullptr;
        if (prodigy_db::db_open_encrypted(rag_db_path_.c_str(), &rag_db) != SQLITE_OK) {
            std::cerr << "WARNING: cannot open RAG config DB " << rag_db_path_ << "\n";
            if (rag_db) sqlite3_close(rag_db);
            return false;
        }
        sqlite3_exec(rag_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(rag_db, "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);
        sqlite3_exec(rag_db,
            "CREATE TABLE IF NOT EXISTS config ("
            "  key   TEXT PRIMARY KEY NOT NULL,"
            "  value TEXT NOT NULL);",
            nullptr, nullptr, nullptr);
        sqlite3_stmt* st = nullptr;
        bool ok = false;
        if (sqlite3_prepare_v2(rag_db,
                "INSERT OR REPLACE INTO config (key, value) VALUES (?, ?)",
                -1, &st, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(st, 1, key.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(st, 2, value.c_str(), -1, SQLITE_TRANSIENT);
            ok = (sqlite3_step(st) == SQLITE_DONE);
            sqlite3_finalize(st);
        }
        sqlite3_close(rag_db);
        if (!ok)
            std::cerr << "WARNING: failed to write RAG config key '" << key << "'\n";
        return ok;
    }

    void rag_db_sync_all_config() {
        sqlite3* rag_db = nullptr;
        if (prodigy_db::db_open_encrypted(rag_db_path_.c_str(), &rag_db) != SQLITE_OK) {
            std::cerr << "WARNING: cannot open RAG config DB " << rag_db_path_ << "\n";
            if (rag_db) sqlite3_close(rag_db);
            return;
        }
        sqlite3_exec(rag_db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
        sqlite3_exec(rag_db, "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);
        sqlite3_exec(rag_db,
            "CREATE TABLE IF NOT EXISTS config ("
            "  key   TEXT PRIMARY KEY NOT NULL,"
            "  value TEXT NOT NULL);",
            nullptr, nullptr, nullptr);

        std::string cp = get_setting("rag_cert_uploaded", "");
        if (cp.empty()) cp = "/etc/tomedo-crawl/client.pem";
        const std::pair<const char*, std::string> entries[] = {
            {"tomedo_host",       get_setting("rag_tomedo_host", "192.168.10.9")},
            {"tomedo_port",       get_setting("rag_tomedo_port", "8443")},
            {"tomedo_cert_pem",   cp},
            {"ollama_url",        get_setting("rag_ollama_url", "http://127.0.0.1:11434")},
            {"ollama_model",      get_setting("rag_ollama_model", "embeddinggemma:300m")},
            {"crawl_interval_sec",get_setting("rag_crawl_interval_sec", "3600")},
        };
        sqlite3_exec(rag_db, "BEGIN", nullptr, nullptr, nullptr);
        for (auto& [key, val] : entries) {
            sqlite3_stmt* st = nullptr;
            if (sqlite3_prepare_v2(rag_db,
                    "INSERT OR REPLACE INTO config (key, value) VALUES (?, ?)",
                    -1, &st, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, key, -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(st, 2, val.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(st);
                sqlite3_finalize(st);
            }
        }
        sqlite3_exec(rag_db, "COMMIT", nullptr, nullptr, nullptr);
        sqlite3_close(rag_db);
    }

    static std::string rag_http_request(const std::string& method, const std::string& path,
                                       const std::string& body = "", int timeout_ms = 2000) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return "";
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(whispertalk::service_data_port(whispertalk::ServiceType::TOMEDO_CRAWL_SERVICE));
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(sock);
            return "";
        }

        // Wrap in TLS — tomedo-crawl serves HTTPS with the shared prodigy self-signed cert.
        // Use SSL_VERIFY_PEER with the prodigy CA so we authenticate the loopback service.
        static std::once_flag s_rag_ctx_once;
        std::call_once(s_rag_ctx_once, []() {
            prodigy_tls::ensure_certs();
            s_rag_ssl_ctx = SSL_CTX_new(TLS_client_method());
            if (s_rag_ssl_ctx) {
                std::string ca = prodigy_tls::cert_file_path();
                if (SSL_CTX_load_verify_locations(s_rag_ssl_ctx, ca.c_str(), nullptr) == 1) {
                    SSL_CTX_set_verify(s_rag_ssl_ctx, SSL_VERIFY_PEER, nullptr);
                } else {
                    SSL_CTX_set_verify(s_rag_ssl_ctx, SSL_VERIFY_NONE, nullptr);
                }
            }
        });
        if (!s_rag_ssl_ctx) { ::close(sock); return ""; }

        SSL* ssl = SSL_new(s_rag_ssl_ctx);
        if (!ssl) { ::close(sock); return ""; }
        SSL_set_fd(ssl, sock);
        SSL_set_tlsext_host_name(ssl, "127.0.0.1");
        if (SSL_connect(ssl) != 1) {
            SSL_free(ssl); ::close(sock);
            return "";
        }

        // POST requests always need Content-Length (even for empty body) so
        // mongoose doesn't reject with 411.
        bool is_post = (method == "POST" || method == "PUT" || method == "PATCH");
        std::string req = method + " " + path + " HTTP/1.0\r\nHost: 127.0.0.1\r\n";
        if (!body.empty()) {
            req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
        } else if (is_post) {
            req += "Content-Length: 0\r\n";
        }
        req += "Connection: close\r\n\r\n";
        if (!body.empty()) req += body;

        if (SSL_write(ssl, req.c_str(), static_cast<int>(req.size())) <= 0) {
            SSL_shutdown(ssl); SSL_free(ssl); ::close(sock);
            return "";
        }

        std::string response;
        char buf[4096];
        int n;
        while ((n = SSL_read(ssl, buf, sizeof(buf))) > 0) {
            response.append(buf, static_cast<size_t>(n));
        }
        SSL_shutdown(ssl);
        SSL_free(ssl);
        ::close(sock);
        auto hdr_end = response.find("\r\n\r\n");
        if (hdr_end != std::string::npos) return response.substr(hdr_end + 4);
        return response;
    }

    void handle_rag_health(struct mg_connection *c) {
        std::string body = rag_http_request("GET", "/health");
        if (body.empty()) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                "{\"status\":\"offline\",\"indexed_docs\":0,\"last_crawl\":null}");
        } else {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body.c_str());
        }
    }

    void handle_rag_config(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("GET")) == 0) {
            std::string tomedo_host = get_setting("rag_tomedo_host", "192.168.10.9");
            std::string tomedo_port = get_setting("rag_tomedo_port", "8443");
            std::string ollama_url = get_setting("rag_ollama_url", "http://127.0.0.1:11434");
            std::string ollama_model = get_setting("rag_ollama_model", "embeddinggemma:300m");
            std::string crawl_interval = get_setting("rag_crawl_interval_sec", "3600");
            std::string crawl_time = get_setting("rag_crawl_time", "02:00");
            std::string crawl_repeat_min = get_setting("rag_crawl_repeat_minutes", "0");
            std::string cert_status = get_setting("rag_cert_uploaded", "");
            std::stringstream json;
            json << "{\"tomedo_host\":\"" << escape_json(tomedo_host)
                 << "\",\"tomedo_port\":\"" << escape_json(tomedo_port)
                 << "\",\"ollama_url\":\"" << escape_json(ollama_url)
                 << "\",\"ollama_model\":\"" << escape_json(ollama_model)
                 << "\",\"crawl_interval_sec\":\"" << escape_json(crawl_interval)
                 << "\",\"crawl_time\":\"" << escape_json(crawl_time)
                 << "\",\"crawl_repeat_minutes\":\"" << escape_json(crawl_repeat_min)
                 << "\",\"cert_uploaded\":" << (cert_status.empty() ? "false" : "true")
                 << "}";
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
        } else {
            std::string body(hm->body.buf, hm->body.len);
            auto save = [&](const std::string& json_key, const std::string& setting_key) {
                std::string val = extract_json_string(body, json_key);
                if (!val.empty() && db_) {
                    sqlite3_stmt* stmt;
                    const char* sql = "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)";
                    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                        sqlite3_bind_text(stmt, 1, setting_key.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_bind_text(stmt, 2, val.c_str(), -1, SQLITE_TRANSIENT);
                        sqlite3_step(stmt);
                        sqlite3_finalize(stmt);
                    }
                }
            };
            save("tomedo_host", "rag_tomedo_host");
            save("tomedo_port", "rag_tomedo_port");
            save("ollama_url", "rag_ollama_url");
            save("ollama_model", "rag_ollama_model");
            save("crawl_interval_sec", "rag_crawl_interval_sec");
            save("crawl_time", "rag_crawl_time");
            save("crawl_repeat_minutes", "rag_crawl_repeat_minutes");

            auto sync = [&](const std::string& json_key, const char* cfg_key) {
                std::string val = extract_json_string(body, json_key);
                rag_db_set_config(cfg_key, val);
            };
            sync("tomedo_host", "tomedo_host");
            sync("tomedo_port", "tomedo_port");
            sync("ollama_url", "ollama_url");
            sync("ollama_model", "ollama_model");
            sync("crawl_interval_sec", "crawl_interval_sec");
            sync("crawl_time", "crawl_time");
            sync("crawl_repeat_minutes", "crawl_repeat_minutes");

            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\":\"saved\"}");
        }
    }

    void handle_rag_cert_upload(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        struct mg_http_part part;
        size_t pos = 0;
        std::string pem_data;
        while ((pos = mg_http_next_multipart(hm->body, pos, &part)) > 0) {
            if (part.body.len > 0) {
                pem_data.assign(part.body.buf, part.body.len);
                break;
            }
        }
        if (pem_data.empty()) {
            std::string raw(hm->body.buf, hm->body.len);
            if (!raw.empty() && raw.find("-----BEGIN") != std::string::npos) {
                pem_data = raw;
            }
        }
        if (pem_data.empty() || pem_data.size() > 65536) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                "{\"error\":\"No PEM data or file too large (max 64KB)\"}");
            return;
        }
        mkdir("certs", 0700);
        std::string cert_path = "certs/tomedo-client.pem";
        std::ofstream out(cert_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                "{\"error\":\"Cannot write certificate file\"}");
            return;
        }
        out.write(pem_data.data(), pem_data.size());
        out.close();
        chmod(cert_path.c_str(), 0600);
        if (db_) {
            sqlite3_stmt* stmt;
            const char* sql = "INSERT OR REPLACE INTO settings (key, value) VALUES (?, ?)";
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, "rag_cert_uploaded", -1, SQLITE_STATIC);
                sqlite3_bind_text(stmt, 2, cert_path.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
        }
        rag_db_set_config("tomedo_cert_pem", cert_path);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
            "{\"status\":\"uploaded\",\"path\":\"%s\"}", escape_json(cert_path).c_str());
    }

    void handle_rag_trigger_crawl(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        std::string body = rag_http_request("POST", "/crawl/trigger");
        if (body.empty()) {
            mg_http_reply(c, 503, "Content-Type: application/json\r\n",
                "{\"error\":\"RAG service not reachable\"}");
        } else {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", body.c_str());
        }
    }

    void handle_rag_wipe_vectors(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        std::string resp = rag_http_request("POST", "/vectors/wipe");
        if (resp.empty()) {
            mg_http_reply(c, 503, "Content-Type: application/json\r\n",
                "{\"error\":\"RAG service not reachable\"}");
        } else {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", resp.c_str());
        }
    }

    static std::string ollama_http_request(const std::string& method, const std::string& url_base,
                                           const std::string& path, const std::string& body = "",
                                           int timeout_ms = 10000) {
        std::string host = "127.0.0.1";
        std::string port_str = "11434";
        std::string trimmed = url_base;
        auto proto = trimmed.find("://");
        if (proto != std::string::npos) trimmed = trimmed.substr(proto + 3);
        while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
        auto colon = trimmed.find(':');
        if (colon != std::string::npos) {
            host = trimmed.substr(0, colon);
            port_str = trimmed.substr(colon + 1);
        } else if (!trimmed.empty()) {
            host = trimmed;
        }
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(host.c_str(), port_str.c_str(), &hints, &res) != 0 || !res) return "";
        int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (sock < 0) { freeaddrinfo(res); return ""; }
        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        if (connect(sock, res->ai_addr, res->ai_addrlen) < 0) {
            ::close(sock);
            freeaddrinfo(res);
            return "";
        }
        freeaddrinfo(res);
        std::string req = method + " " + path + " HTTP/1.0\r\nHost: " + host + "\r\n";
        if (!body.empty()) {
            req += "Content-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
        }
        req += "Connection: close\r\n\r\n";
        if (!body.empty()) req += body;
        send(sock, req.c_str(), req.size(), 0);
        std::string response;
        char buf[4096];
        ssize_t n;
        while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) {
            response.append(buf, n);
        }
        ::close(sock);
        auto hdr_end = response.find("\r\n\r\n");
        if (hdr_end != std::string::npos) return response.substr(hdr_end + 4);
        return response;
    }

    void handle_ollama_status(struct mg_connection *c) {
        std::string ollama_url = get_setting("rag_ollama_url", "http://127.0.0.1:11434");
        std::string resp = ollama_http_request("GET", ollama_url, "/api/tags");
        bool running = !resp.empty();
        bool pulling = ollama_pulling_.load();
        bool installed = true;
        std::string rag_health = rag_http_request("GET", "/ollama/status");
        if (!rag_health.empty()) {
            installed = rag_health.find("\"installed\":true") != std::string::npos;
            if (!running) running = rag_health.find("\"running\":true") != std::string::npos;
        }
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
            "{\"running\":%s,\"pulling\":%s,\"installed\":%s}",
            running ? "true" : "false", pulling ? "true" : "false",
            installed ? "true" : "false");
    }

    pid_t spawn_ollama_serve() {
        int pidpipe[2];
        if (pipe(pidpipe) < 0) return -1;
        int execpipe[2];
        if (pipe(execpipe) < 0) { ::close(pidpipe[0]); ::close(pidpipe[1]); return -1; }
        fcntl(execpipe[1], F_SETFD, FD_CLOEXEC);
        pid_t mid = fork();
        if (mid < 0) { ::close(pidpipe[0]); ::close(pidpipe[1]); ::close(execpipe[0]); ::close(execpipe[1]); return -1; }
        if (mid == 0) {
            ::close(pidpipe[0]);
            ::close(execpipe[0]);
            pid_t child = fork();
            if (child < 0) { pid_t z = 0; (void)write(pidpipe[1], &z, sizeof(z)); ::close(pidpipe[1]); ::close(execpipe[1]); _exit(127); }
            if (child > 0) { (void)write(pidpipe[1], &child, sizeof(child)); ::close(pidpipe[1]); ::close(execpipe[1]); _exit(0); }
            ::close(pidpipe[1]);
            setsid();
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) { dup2(devnull, STDIN_FILENO); dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); ::close(devnull); }
            execlp("ollama", "ollama", "serve", nullptr);
            char err = 1;
            (void)write(execpipe[1], &err, 1);
            ::close(execpipe[1]);
            _exit(127);
        }
        ::close(pidpipe[1]);
        ::close(execpipe[1]);
        pid_t grandchild = 0;
        (void)read(pidpipe[0], &grandchild, sizeof(grandchild));
        ::close(pidpipe[0]);
        int status;
        waitpid(mid, &status, 0);
        if (grandchild < 1) { ::close(execpipe[0]); return -1; }
        char err = 0;
        struct pollfd pfd = {execpipe[0], POLLIN, 0};
        if (poll(&pfd, 1, 200) > 0 && read(execpipe[0], &err, 1) == 1) {
            ::close(execpipe[0]);
            return -1;
        }
        ::close(execpipe[0]);
        ollama_pid_.store(grandchild);
        return 0;
    }

    void kill_ollama_serve() {
        pid_t p = ollama_pid_.exchange(0);
        if (p > 1) {
            kill(p, SIGTERM);
            return;
        }
        std::thread([](){
            FILE* fp = popen("pgrep -f 'ollama serve'", "r");
            if (fp) {
                char buf[64];
                while (fgets(buf, sizeof(buf), fp)) {
                    pid_t kp = static_cast<pid_t>(atoi(buf));
                    if (kp > 1) kill(kp, SIGTERM);
                }
                pclose(fp);
            }
        }).detach();
    }

    void handle_ollama_start(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        std::string resp = rag_http_request("POST", "/ollama/start");
        if (resp.empty()) {
            if (spawn_ollama_serve() >= 0) {
                mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                    "{\"status\":\"started\"}");
            } else {
                mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                    "{\"error\":\"failed to start ollama (not installed or not in PATH?)\"}");
            }
        } else {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", resp.c_str());
        }
    }

    void handle_ollama_stop(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        std::string resp = rag_http_request("POST", "/ollama/stop");
        if (resp.empty()) {
            kill_ollama_serve();
            mg_http_reply(c, 202, "Content-Type: application/json\r\n",
                "{\"status\":\"stopping\"}");
        } else {
            mg_http_reply(c, 202, "Content-Type: application/json\r\n", "%s", resp.c_str());
        }
    }

    void handle_ollama_restart(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        std::string stop_resp = rag_http_request("POST", "/ollama/stop");
        if (!stop_resp.empty()) {
            std::thread([](){
                usleep(500000);
                rag_http_request("POST", "/ollama/start");
            }).detach();
        } else {
            std::thread([this](){
                kill_ollama_serve();
                usleep(500000);
                spawn_ollama_serve();
            }).detach();
        }
        mg_http_reply(c, 202, "Content-Type: application/json\r\n",
            "{\"status\":\"restarting\"}");
    }

    void handle_ollama_models(struct mg_connection *c) {
        std::string ollama_url = get_setting("rag_ollama_url", "http://127.0.0.1:11434");
        std::string resp = ollama_http_request("GET", ollama_url, "/api/tags");
        if (resp.empty()) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                "{\"models\":[],\"error\":\"ollama not reachable\"}");
        } else {
            std::vector<std::string> models;
            size_t pos = 0;
            while (true) {
                auto name_key = resp.find("\"name\"", pos);
                if (name_key == std::string::npos) break;
                auto colon = resp.find(':', name_key);
                if (colon == std::string::npos) break;
                auto q1 = resp.find('"', colon + 1);
                if (q1 == std::string::npos) break;
                auto q2 = resp.find('"', q1 + 1);
                if (q2 == std::string::npos) break;
                models.push_back(resp.substr(q1 + 1, q2 - q1 - 1));
                pos = q2 + 1;
            }
            std::string active_model = get_setting("rag_ollama_model", "embeddinggemma:300m");
            std::stringstream json;
            json << "{\"models\":[";
            for (size_t i = 0; i < models.size(); i++) {
                if (i > 0) json << ",";
                json << "{\"name\":\"" << escape_json(models[i])
                     << "\",\"active\":" << (models[i] == active_model ? "true" : "false") << "}";
            }
            json << "],\"active_model\":\"" << escape_json(active_model) << "\"}";
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
        }
    }

    void handle_ollama_pull(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        if (ollama_pulling_.load()) {
            mg_http_reply(c, 409, "Content-Type: application/json\r\n",
                "{\"error\":\"a pull is already in progress\"}");
            return;
        }
        std::string body(hm->body.buf, hm->body.len);
        std::string model = extract_json_string(body, "model");
        if (model.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                "{\"error\":\"model field required\"}");
            return;
        }
        for (char ch : model) {
            if (!std::isalnum(ch) && ch != '-' && ch != '_' && ch != '.' && ch != ':' && ch != '/') {
                mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                    "{\"error\":\"invalid model name\"}");
                return;
            }
        }
        std::string ollama_url = get_setting("rag_ollama_url", "http://127.0.0.1:11434");
        ollama_pulling_.store(true);
        std::thread([this, model, ollama_url]() {
            std::string pull_body = "{\"name\":\"" + model + "\",\"stream\":false}";
            ollama_http_request("POST", ollama_url, "/api/pull", pull_body, 600000);
            ollama_pulling_.store(false);
        }).detach();
        mg_http_reply(c, 202, "Content-Type: application/json\r\n",
            "{\"status\":\"pulling\",\"model\":\"%s\"}", escape_json(model).c_str());
    }

    void handle_ollama_install(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        std::string resp = rag_http_request("POST", "/ollama/install");
        if (resp.empty()) {
            mg_http_reply(c, 502, "Content-Type: application/json\r\n",
                "{\"error\":\"tomedo-crawl not reachable\"}");
        } else {
            mg_http_reply(c, 202, "Content-Type: application/json\r\n", "%s", resp.c_str());
        }
    }

    // GET /api/dashboard — Aggregated dashboard data for the main overview page.
    //
    // Response JSON:
    //   { services_online, services_total, running_tests, test_pass, test_fail,
    //     uptime_seconds, services: [{name,online,managed},...],
    //     recent_logs: [{timestamp,service,level,message},...],
    //     pipeline: ["SIP_CLIENT",...,"OUTBOUND_AUDIO_PROCESSOR"] }
    //
    // Data sources:
    //   - services_online/total: counted from in-memory services_ vector (pid > 0)
    //   - running_tests: counted from in-memory tests_ vector (is_running flag)
    //   - test_pass/fail: aggregated from service_test_runs DB table (GROUP BY status)
    //   - uptime_seconds: elapsed since start_time_ (set in constructor)
    //   - recent_logs: last DASHBOARD_RECENT_LOGS_LIMIT entries from recent_logs_ ring buffer
    //   - pipeline: static list defining the node order for the dashboard visualization
    //
    // Polled by the frontend JS every POLL_STATUS_MS (3s) when dashboard page is active.
    void handle_dashboard(struct mg_connection *c) {
        int svc_online = 0;
        int svc_total = 0;
        std::stringstream svc_json;
        svc_json << "[";
        {
            std::lock_guard<std::mutex> lock(services_mutex_);
            svc_total = static_cast<int>(services_.size());
            for (size_t i = 0; i < services_.size(); i++) {
                if (i > 0) svc_json << ",";
                const auto& s = services_[i];
                bool alive = s.managed && s.pid > 0;
                if (alive) svc_online++;
                svc_json << "{\"name\":\"" << escape_json(s.name)
                         << "\",\"online\":" << (alive ? "true" : "false")
                         << ",\"managed\":" << (s.managed ? "true" : "false") << "}";
            }
        }
        svc_json << "]";

        int running_tests = 0;
        {
            std::lock_guard<std::mutex> lock(tests_mutex_);
            for (const auto& t : tests_) {
                if (t.is_running) running_tests++;
            }
        }

        std::stringstream logs_json;
        logs_json << "[";
        {
            std::lock_guard<std::mutex> lock(logs_mutex_);
            size_t count = 0;
            for (auto it = recent_logs_.rbegin(); it != recent_logs_.rend() && count < DASHBOARD_RECENT_LOGS_LIMIT; ++it, ++count) {
                if (count > 0) logs_json << ",";
                logs_json << "{\"timestamp\":\"" << escape_json(it->timestamp)
                          << "\",\"service\":\"" << service_type_to_string(it->service)
                          << "\",\"level\":\"" << escape_json(it->level)
                          << "\",\"message\":\"" << escape_json(it->message) << "\"}";
            }
        }
        logs_json << "]";

        int test_pass = 0, test_fail = 0;
        if (db_) {
            sqlite3_stmt* stmt;
            const char* sql = "SELECT status, COUNT(*) FROM service_test_runs GROUP BY status";
            if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char* st = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
                    int cnt = sqlite3_column_int(stmt, 1);
                    if (st) {
                        std::string status_str(st);
                        if (status_str == "pass" || status_str == "PASS" || status_str == "passed" || status_str == "success") test_pass += cnt;
                        else if (status_str == "fail" || status_str == "FAIL" || status_str == "failed" || status_str == "error") test_fail += cnt;
                    }
                }
                sqlite3_finalize(stmt);
            }
        }

        auto now = std::chrono::steady_clock::now();
        auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(now - start_time_).count();

        bool ollama_installed = true, ollama_running = false;
        {
            std::string rag_ollama = rag_http_request("GET", "/ollama/status", "", 200);
            if (!rag_ollama.empty()) {
                ollama_installed = rag_ollama.find("\"installed\":true") != std::string::npos;
                ollama_running = rag_ollama.find("\"running\":true") != std::string::npos;
            } else {
                std::string ollama_url = get_setting("rag_ollama_url", "http://127.0.0.1:11434");
                std::string resp = ollama_http_request("GET", ollama_url, "/api/tags");
                ollama_running = !resp.empty();
            }
        }

        std::stringstream json;
        json << "{\"services_online\":" << svc_online
             << ",\"services_total\":" << svc_total
             << ",\"running_tests\":" << running_tests
             << ",\"test_pass\":" << test_pass
             << ",\"test_fail\":" << test_fail
             << ",\"uptime_seconds\":" << uptime_s
             << ",\"ollama_installed\":" << (ollama_installed ? "true" : "false")
             << ",\"ollama_running\":" << (ollama_running ? "true" : "false")
             << ",\"services\":" << svc_json.str()
             << ",\"recent_logs\":" << logs_json.str()
             << ",\"pipeline\":" << (get_setting("pipeline_mode", "classic") == "moshi"
                 ? "[\"SIP_CLIENT\",\"INBOUND_AUDIO_PROCESSOR\",\"MOSHI_SERVICE\",\"OUTBOUND_AUDIO_PROCESSOR\"]"
                 : "[\"SIP_CLIENT\",\"INBOUND_AUDIO_PROCESSOR\",\"VAD_SERVICE\",\"WHISPER_SERVICE\",\"LLAMA_SERVICE\",\"TTS_SERVICE\",\"OUTBOUND_AUDIO_PROCESSOR\"]")
             << "}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    // GET /api/status — System health summary: uptime, service count, memory usage.
    void handle_status(struct mg_connection *c) {
        int svc_count = 0;
        {
            std::lock_guard<std::mutex> lock(services_mutex_);
            for (const auto& svc : services_) {
                if (svc.managed && svc.pid > 0) svc_count++;
            }
        }

        int running_tests = 0;
        {
            std::lock_guard<std::mutex> lock(tests_mutex_);
            for (const auto& t : tests_) {
                if (t.is_running) running_tests++;
            }
        }

        size_t log_count;
        {
            std::lock_guard<std::mutex> lock(logs_mutex_);
            log_count = recent_logs_.size();
        }

        size_t sse_count;
        {
            std::lock_guard<std::mutex> lock(sse_mutex_);
            sse_count = sse_connections_.size();
        }

        std::stringstream json;
        json << "{\"http_port\":" << http_port_
             << ",\"log_port\":" << log_port_
             << ",\"services_online\":" << svc_count
             << ",\"running_tests\":" << running_tests
             << ",\"log_buffer_size\":" << log_count
             << ",\"sse_connections\":" << sse_count
             << ",\"architecture\":\"peer-to-peer\""
             << "}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    // GET /api/models — Returns all registered models from SQLite, grouped by service type.
    void handle_models_get(struct mg_connection *c) {
        if (!db_) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database not available\"}");
            return;
        }
        
        sqlite3_stmt* stmt;
        const char* query = "SELECT id, service, name, path, backend, size_mb, config_json, added_timestamp FROM models ORDER BY service, name";
        int rc = sqlite3_prepare_v2(db_, query, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database query failed\"}");
            return;
        }
        
        auto safe_col = [&](int col) -> std::string {
            const char* t = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
            return t ? t : "";
        };
        std::map<std::string, std::vector<std::string>> models_by_type;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string service = safe_col(1);
            std::stringstream model_json;
            model_json << "{\"id\":" << sqlite3_column_int(stmt, 0)
                      << ",\"service\":\"" << escape_json(service) << "\""
                      << ",\"name\":\"" << escape_json(safe_col(2)) << "\""
                      << ",\"path\":\"" << escape_json(safe_col(3)) << "\""
                      << ",\"backend\":\"" << escape_json(safe_col(4)) << "\""
                      << ",\"size_mb\":" << sqlite3_column_int(stmt, 5)
                      << ",\"config_json\":\"" << escape_json(safe_col(6)) << "\""
                      << ",\"added_timestamp\":" << sqlite3_column_int(stmt, 7) << "}";
            models_by_type[service].push_back(model_json.str());
        }
        sqlite3_finalize(stmt);
        
        std::stringstream json;
        json << "{";
        bool first_type = true;
        for (const auto& [service, models] : models_by_type) {
            if (!first_type) json << ",";
            json << "\"" << escape_json(service) << "\":[";
            bool first_model = true;
            for (const auto& model : models) {
                if (!first_model) json << ",";
                json << model;
                first_model = false;
            }
            json << "]";
            first_type = false;
        }
        json << "}";
        
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }
    
    // Returns all model benchmark runs joined with model info, ordered by timestamp descending.
    // Used by the Model Comparison view to render the side-by-side table and Chart.js bar chart.
    // Response: { "runs": [ { run_id, model_id, model_name, service, backend, avg_accuracy,
    //             avg_latency_ms, p50_latency_ms, p95_latency_ms, p99_latency_ms,
    //             memory_mb, timestamp }, ... ] }
    void handle_models_benchmarks_get(struct mg_connection *c) {
        if (!db_) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database not available\"}");
            return;
        }

        const char* query =
            "SELECT r.id, r.model_id, "
            "COALESCE(r.model_name, m.name) as name, "
            "COALESCE(r.model_type, m.service) as type, "
            "COALESCE(r.backend, m.backend) as backend, "
            "r.avg_accuracy, r.avg_latency_ms, r.p50_latency_ms, r.p95_latency_ms, "
            "r.p99_latency_ms, r.memory_mb, r.timestamp, "
            "r.pass_count, r.fail_count, r.files_tested, "
            "r.avg_tokens, r.interrupt_latency_ms, r.german_pct "
            "FROM model_benchmark_runs r "
            "LEFT JOIN models m ON m.id = r.model_id "
            "ORDER BY r.timestamp DESC LIMIT 100";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, query, -1, &stmt, nullptr) != SQLITE_OK) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database query failed\"}");
            return;
        }

        std::stringstream json;
        json << "{\"runs\":[";
        bool first = true;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (!first) json << ",";
            first = false;
            auto col_str = [&](int col) -> std::string {
                const unsigned char* t = sqlite3_column_text(stmt, col);
                return t ? reinterpret_cast<const char*>(t) : "";
            };
            json << "{"
                 << "\"run_id\":" << sqlite3_column_int(stmt, 0)
                 << ",\"model_id\":" << sqlite3_column_int(stmt, 1)
                 << ",\"model_name\":\"" << escape_json(col_str(2)) << "\""
                 << ",\"model_type\":\"" << escape_json(col_str(3)) << "\""
                 << ",\"backend\":\"" << escape_json(col_str(4)) << "\""
                 << ",\"avg_accuracy\":" << sqlite3_column_double(stmt, 5)
                 << ",\"avg_latency_ms\":" << sqlite3_column_int(stmt, 6)
                 << ",\"p50_latency_ms\":" << sqlite3_column_int(stmt, 7)
                 << ",\"p95_latency_ms\":" << sqlite3_column_int(stmt, 8)
                 << ",\"p99_latency_ms\":" << sqlite3_column_int(stmt, 9)
                 << ",\"memory_mb\":" << sqlite3_column_int(stmt, 10)
                 << ",\"timestamp\":" << sqlite3_column_int64(stmt, 11)
                 << ",\"pass_count\":" << sqlite3_column_int(stmt, 12)
                 << ",\"fail_count\":" << sqlite3_column_int(stmt, 13)
                 << ",\"files_tested\":" << sqlite3_column_int(stmt, 14)
                 << ",\"avg_tokens\":" << sqlite3_column_double(stmt, 15)
                 << ",\"interrupt_latency_ms\":" << sqlite3_column_double(stmt, 16)
                 << ",\"german_pct\":" << sqlite3_column_double(stmt, 17)
                 << "}";
        }
        sqlite3_finalize(stmt);
        json << "]}";

        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    static bool is_safe_repo_id(const std::string& s) {
        if (s.empty() || s.size() > 256) return false;
        int slash_count = 0;
        for (unsigned char ch : s) {
            if (ch == '/') { slash_count++; continue; }
            if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') continue;
            return false;
        }
        return slash_count == 1 && s.front() != '/' && s.back() != '/';
    }

    static bool is_safe_filename(const std::string& s) {
        if (s.empty() || s.size() > 256) return false;
        if (s.find("..") != std::string::npos) return false;
        for (unsigned char ch : s) {
            if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.') continue;
            return false;
        }
        return s.front() != '.' && s.back() != '.';
    }

    static std::string url_encode(const std::string& s) {
        std::string result;
        for (unsigned char ch : s) {
            if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
                result += ch;
            } else {
                char hex[4];
                snprintf(hex, sizeof(hex), "%%%02X", ch);
                result += hex;
            }
        }
        return result;
    }

    std::string find_hf_cli() {
        FILE* fp = popen("which huggingface-cli 2>/dev/null", "r");
        if (fp) {
            char buf[512] = {0};
            if (fgets(buf, sizeof(buf), fp)) {
                std::string path(buf);
                while (!path.empty() && (path.back() == '\n' || path.back() == '\r' || path.back() == ' '))
                    path.pop_back();
                pclose(fp);
                if (!path.empty()) {
                    struct stat st;
                    if (stat(path.c_str(), &st) == 0 && (st.st_mode & S_IXUSR))
                        return path;
                }
            } else {
                pclose(fp);
            }
        }

        static const char* candidates[] = {
            "/opt/homebrew/Caskroom/miniconda/base/bin/huggingface-cli",
            "/opt/homebrew/bin/huggingface-cli",
            "/usr/local/bin/huggingface-cli",
            "/usr/bin/huggingface-cli",
            nullptr
        };
        for (int i = 0; candidates[i]; i++) {
            struct stat st;
            if (stat(candidates[i], &st) == 0 && (st.st_mode & S_IXUSR))
                return candidates[i];
        }
        return "";
    }

    int run_hf_cli_download(const std::string& hf_cli, const std::string& repo_id,
                             const std::string& filename, const std::string& local_dir,
                             const std::string& hf_token, std::string* err_out) {
        int errpipe[2] = {-1, -1};
        if (err_out && pipe(errpipe) != 0) err_out = nullptr;

        pid_t pid = fork();
        if (pid < 0) {
            if (errpipe[0] >= 0) { close(errpipe[0]); close(errpipe[1]); }
            return -1;
        }

        if (pid == 0) {
            if (!hf_token.empty()) {
                setenv("HF_TOKEN", hf_token.c_str(), 1);
                setenv("HUGGING_FACE_HUB_TOKEN", hf_token.c_str(), 1);
            }
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                dup2(devnull, STDOUT_FILENO);
                close(devnull);
            }
            if (err_out && errpipe[1] >= 0) {
                close(errpipe[0]);
                dup2(errpipe[1], STDERR_FILENO);
                close(errpipe[1]);
            } else {
                int dn = open("/dev/null", O_WRONLY);
                if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
            }
            const char* argv[] = {
                hf_cli.c_str(), "download",
                repo_id.c_str(), filename.c_str(),
                "--local-dir", local_dir.c_str(),
                "--local-dir-use-symlinks", "False",
                nullptr
            };
            execvp(hf_cli.c_str(), const_cast<char* const*>(argv));
            _exit(127);
        }

        if (err_out && errpipe[0] >= 0) {
            close(errpipe[1]);
            std::string captured;
            char buf[1024];
            ssize_t n;
            while ((n = read(errpipe[0], buf, sizeof(buf))) > 0) {
                if (captured.size() < 4096)
                    captured.append(buf, std::min((size_t)n, 4096 - captured.size()));
            }
            close(errpipe[0]);
            *err_out = captured;
        }

        int status = 0;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    std::string write_curl_header_file(const std::string& token) {
        if (token.empty()) return "";
        char tmpname[] = "/tmp/wt_hdr_XXXXXX";
        int fd = mkstemp(tmpname);
        if (fd < 0) return "";
        std::string header = "Authorization: Bearer " + token + "\n";
        ssize_t written = write(fd, header.c_str(), header.size());
        close(fd);
        if (written != static_cast<ssize_t>(header.size())) {
            unlink(tmpname);
            return "";
        }
        return std::string(tmpname);
    }

    std::string run_curl_safe(const std::vector<std::string>& args) {
        int pipefd[2];
        if (pipe(pipefd) != 0) return "";

        pid_t pid = fork();
        if (pid < 0) {
            close(pipefd[0]);
            close(pipefd[1]);
            return "";
        }

        if (pid == 0) {
            close(pipefd[0]);
            dup2(pipefd[1], STDOUT_FILENO);
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[1]);

            std::vector<const char*> argv;
            argv.push_back("curl");
            for (const auto& a : args) argv.push_back(a.c_str());
            argv.push_back(nullptr);

            execvp("curl", const_cast<char* const*>(argv.data()));
            _exit(127);
        }

        close(pipefd[1]);
        std::string result;
        char buf[4096];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            result.append(buf, n);
        }
        close(pipefd[0]);
        int status = 0;
        waitpid(pid, &status, 0);
        return result;
    }

    int run_curl_to_file(const std::vector<std::string>& args, std::string* err_out = nullptr) {
        int errpipe[2] = {-1, -1};
        if (err_out && pipe(errpipe) != 0) err_out = nullptr;

        pid_t pid = fork();
        if (pid < 0) {
            if (errpipe[0] >= 0) { close(errpipe[0]); close(errpipe[1]); }
            return -1;
        }
        if (pid == 0) {
            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDOUT_FILENO);
                close(devnull);
            }
            if (err_out && errpipe[1] >= 0) {
                close(errpipe[0]);
                dup2(errpipe[1], STDERR_FILENO);
                close(errpipe[1]);
            } else {
                int dn = open("/dev/null", O_WRONLY);
                if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
            }
            std::vector<const char*> argv;
            argv.push_back("curl");
            for (const auto& a : args) argv.push_back(a.c_str());
            argv.push_back(nullptr);
            execvp("curl", const_cast<char* const*>(argv.data()));
            _exit(127);
        }
        if (err_out && errpipe[0] >= 0) {
            close(errpipe[1]);
            std::string captured;
            char buf[1024];
            ssize_t n;
            while ((n = read(errpipe[0], buf, sizeof(buf))) > 0) {
                if (captured.size() < 2048) captured.append(buf, std::min((size_t)n, 2048 - captured.size()));
            }
            close(errpipe[0]);
            *err_out = captured;
        }
        int status = 0;
        waitpid(pid, &status, 0);
        return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }

    // POST /api/models/search — Search HuggingFace Hub for GGML/GGUF model files.
    // Uses HF API token from settings if available. Returns model metadata.
    void handle_models_search(struct mg_connection *c, struct mg_http_message *hm) {
        std::string body(hm->body.buf, hm->body.len);
        std::string query = extract_json_string(body, "query");
        std::string task_filter = extract_json_string(body, "task");
        std::string sort_by = extract_json_string(body, "sort");
        int limit_val = 20;

        if (query.empty()) query = "whisper";
        if (sort_by.empty()) sort_by = "downloads";

        {
            std::string body_str(body);
            size_t lp = body_str.find("\"limit\":");
            if (lp != std::string::npos) {
                int lv = atoi(body_str.c_str() + lp + 8);
                if (lv > 0 && lv <= 100) limit_val = lv;
            }
        }

        std::string hf_token = get_setting("hf_token", "");

        std::string api_url = "https://huggingface.co/api/models?search=" + url_encode(query)
            + "&sort=" + url_encode(sort_by)
            + "&direction=-1"
            + "&limit=" + std::to_string(limit_val);
        if (!task_filter.empty()) {
            api_url += "&pipeline_tag=" + url_encode(task_filter);
        }

        std::string header_file = write_curl_header_file(hf_token);

        std::vector<std::string> args = {"-s", "-S", "--max-time", "20", "-L"};
        if (!header_file.empty()) {
            args.push_back("-H");
            args.push_back("@" + header_file);
        }
        args.push_back(api_url);

        std::string raw = run_curl_safe(args);

        if (!header_file.empty()) unlink(header_file.c_str());

        if (raw.empty() || raw[0] != '[') {
            std::string err_msg;
            if (raw.empty()) {
                err_msg = "No response from HuggingFace API";
            } else if (raw[0] == '{') {
                err_msg = "HuggingFace API error: " + raw.substr(0, 200);
            } else {
                err_msg = "Invalid response from HuggingFace API";
            }
            mg_http_reply(c, 502, "Content-Type: application/json\r\n",
                "{\"error\":\"%s\",\"has_token\":%s}",
                escape_json(err_msg).c_str(), hf_token.empty() ? "false" : "true");
            return;
        }

        std::string resp = "{\"models\":" + raw + ",\"has_token\":" +
            std::string(hf_token.empty() ? "false" : "true") + "}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", resp.c_str());
    }

    // GET /api/models/hf-files?repo_id=owner/repo — List files in a HuggingFace repo.
    // Returns { files: ["filename1", "filename2", ...] }
    void handle_models_hf_files(struct mg_connection *c, struct mg_http_message *hm) {
        char repo_id_buf[300] = {0};
        mg_http_get_var(&hm->query, "repo_id", repo_id_buf, sizeof(repo_id_buf));
        std::string repo_id(repo_id_buf);

        if (repo_id.empty() || !is_safe_repo_id(repo_id)) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Invalid repo_id\"}");
            return;
        }

        std::string hf_token = get_setting("hf_token", "");
        std::string api_url = "https://huggingface.co/api/models/" + repo_id;

        std::string header_file = write_curl_header_file(hf_token);
        std::vector<std::string> args = {"-s", "-S", "--max-time", "20", "-L"};
        if (!header_file.empty()) {
            args.push_back("-H");
            args.push_back("@" + header_file);
        }
        args.push_back(api_url);

        std::string raw = run_curl_safe(args);
        if (!header_file.empty()) unlink(header_file.c_str());

        if (raw.empty() || raw[0] != '{') {
            mg_http_reply(c, 502, "Content-Type: application/json\r\n",
                "{\"error\":\"Failed to fetch repo info from HuggingFace\"}");
            return;
        }

        std::stringstream result;
        result << "{\"files\":[";
        bool first_file = true;

        size_t sib_pos = raw.find("\"siblings\":");
        if (sib_pos != std::string::npos) {
            size_t arr_start = raw.find('[', sib_pos + 11);
            if (arr_start != std::string::npos) {
                size_t pos = arr_start + 1;
                while (pos < raw.size()) {
                    size_t rf_pos = raw.find("\"rfilename\":", pos);
                    if (rf_pos == std::string::npos) break;
                    rf_pos += 12;
                    while (rf_pos < raw.size() && (raw[rf_pos] == ' ' || raw[rf_pos] == '\t')) rf_pos++;
                    if (rf_pos >= raw.size() || raw[rf_pos] != '"') { pos = rf_pos + 1; continue; }
                    rf_pos++;
                    size_t end_pos = raw.find('"', rf_pos);
                    if (end_pos == std::string::npos) break;
                    std::string fname = raw.substr(rf_pos, end_pos - rf_pos);
                    if (!first_file) result << ",";
                    result << "\"" << escape_json(fname) << "\"";
                    first_file = false;
                    pos = end_pos + 1;
                }
            }
        }

        result << "]}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", result.str().c_str());
    }

    // GET /api/models/local — Disk scan for all locally available model files.
    void handle_models_local(struct mg_connection *c) {
        auto scan_models = [](auto callback) {
            DIR* dir = opendir("bin/models");
            if (dir) {
                struct dirent* entry;
                while ((entry = readdir(dir)) != nullptr)
                    callback(std::string(entry->d_name));
                closedir(dir);
            }
        };

        std::stringstream json;
        json << "{";

        json << "\"whisper\":[";
        {
            bool first = true;
            scan_models([&](const std::string& name) {
                if (name.size() <= 4 || name.substr(name.size() - 4) != ".bin") return;
                std::string path = "bin/models/" + name;
                struct stat st;
                int64_t size_mb = 0;
                if (stat(path.c_str(), &st) == 0) size_mb = st.st_size / (1024 * 1024);
                std::string stem = name.substr(0, name.size() - 4);
                struct stat cst;
                std::string prefix = stem;
                {
                    auto qp = stem.rfind("-q");
                    if (qp != std::string::npos) {
                        bool ok = true;
                        for (size_t i = qp + 2; i < stem.size() && ok; i++)
                            if (!isdigit((unsigned char)stem[i]) && stem[i] != '_') ok = false;
                        if (ok) prefix = stem.substr(0, qp);
                    }
                }
                bool coreml = (stat(("bin/models/" + stem + ".mlpackage").c_str(), &cst) == 0)
                           || (stat(("bin/models/" + stem + "_coreml").c_str(), &cst) == 0)
                           || (stat(("bin/models/" + prefix + "-encoder.mlmodelc").c_str(), &cst) == 0)
                           || (stat(("bin/models/" + prefix + "-encoder.mlpackage").c_str(), &cst) == 0);
                if (!first) json << ",";
                json << "{\"filename\":\"" << escape_json(name) << "\""
                     << ",\"path\":\"" << escape_json(path) << "\""
                     << ",\"size_mb\":" << size_mb
                     << ",\"coreml\":" << (coreml ? "true" : "false") << "}";
                first = false;
            });
        }
        json << "],";

        json << "\"llama\":[";
        {
            bool first = true;
            scan_models([&](const std::string& name) {
                if (name.size() <= 5 || name.substr(name.size() - 5) != ".gguf") return;
                std::string path = "bin/models/" + name;
                struct stat st;
                int64_t size_mb = 0;
                if (stat(path.c_str(), &st) == 0) size_mb = st.st_size / (1024 * 1024);
                if (!first) json << ",";
                json << "{\"filename\":\"" << escape_json(name) << "\""
                     << ",\"path\":\"" << escape_json(path) << "\""
                     << ",\"size_mb\":" << size_mb << "}";
                first = false;
            });
        }
        json << "],";

        json << "\"kokoro\":[";
        {
            bool first = true;
            scan_models([&](const std::string& name) {
                if (name == "." || name == ".." || name == "neutts-nano-german") return;
                std::string variant_path = "bin/models/" + name;
                struct stat st;
                if (stat(variant_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return;
                if (stat((variant_path + "/coreml/kokoro_duration.mlmodelc").c_str(), &st) != 0) return;
                if (stat((variant_path + "/decoder_variants").c_str(), &st) != 0) return;
                std::vector<std::string> voices;
                // Voices are stored as <name>_voice.bin in the variant ROOT directory
                // (decoder_variants/ holds decoder mlmodelc files, not voice tensors).
                DIR* vdir = opendir(variant_path.c_str());
                if (vdir) {
                    struct dirent* ve;
                    while ((ve = readdir(vdir)) != nullptr) {
                        std::string vname = ve->d_name;
                        const std::string vsuf = "_voice.bin";
                        if (vname.size() > vsuf.size() &&
                            vname.substr(vname.size() - vsuf.size()) == vsuf)
                            voices.push_back(vname.substr(0, vname.size() - vsuf.size()));
                    }
                    closedir(vdir);
                }
                if (!first) json << ",";
                json << "{\"variant\":\"" << escape_json(name) << "\""
                     << ",\"path\":\"" << escape_json(variant_path) << "\""
                     << ",\"voices\":[";
                bool fv = true;
                for (auto& v : voices) {
                    if (!fv) json << ",";
                    json << "\"" << escape_json(v) << "\"";
                    fv = false;
                }
                json << "],\"coreml\":true,\"size_mb\":0}";
                first = false;
            });
        }
        json << "],";

        {
            std::string neutts_path = "bin/models/neutts-nano-german";
            std::string coreml_path = neutts_path + "/neucodec_decoder.mlpackage";
            struct stat st;
            bool exists = (stat(neutts_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
            bool coreml = exists && (stat(coreml_path.c_str(), &st) == 0);
            json << "\"neutts\":{\"exists\":" << (exists ? "true" : "false")
                 << ",\"coreml\":" << (coreml ? "true" : "false")
                 << ",\"path\":\"" << escape_json(neutts_path) << "\""
                 << ",\"coreml_path\":\"" << escape_json(coreml_path) << "\"}";
        }

        json << "}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    // GET /api/models/llama — Scan bin/models/*.gguf for LLaMA service config dropdown.
    void handle_models_llama(struct mg_connection *c) {
        std::stringstream json;
        json << "{\"models\":[";
        DIR* dir = opendir("bin/models");
        bool first = true;
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string name = entry->d_name;
                if (name.size() > 5 && name.substr(name.size() - 5) == ".gguf") {
                    std::string path = "bin/models/" + name;
                    struct stat st;
                    int64_t size_mb = 0;
                    if (stat(path.c_str(), &st) == 0) size_mb = st.st_size / (1024 * 1024);
                    if (!first) json << ",";
                    json << "{\"filename\":\"" << escape_json(name) << "\""
                         << ",\"path\":\"" << escape_json(path) << "\""
                         << ",\"size_mb\":" << size_mb << "}";
                    first = false;
                }
            }
            closedir(dir);
        }
        std::string current_args;
        {
            std::lock_guard<std::mutex> lock(services_mutex_);
            for (auto& svc : services_) {
                if (svc.name == "LLAMA_SERVICE") { current_args = svc.default_args; break; }
            }
        }
        json << "],\"current_args\":\"" << escape_json(current_args) << "\"}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    // GET /api/models/kokoro — Scan bin/models/*/ for Kokoro variants/voices for service config dropdown.
    void handle_models_kokoro(struct mg_connection *c) {
        std::stringstream json;
        json << "{\"variants\":[";
        DIR* dir = opendir("bin/models");
        bool first = true;
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string name = entry->d_name;
                if (name == "." || name == ".." || name == "neutts-nano-german") continue;
                std::string variant_path = "bin/models/" + name;
                struct stat st;
                if (stat(variant_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                if (stat((variant_path + "/coreml/kokoro_duration.mlmodelc").c_str(), &st) != 0) continue;
                if (stat((variant_path + "/decoder_variants").c_str(), &st) != 0) continue;
                std::vector<std::string> voices;
                // Voices are stored as <name>_voice.bin in the variant ROOT directory
                // (decoder_variants/ holds decoder mlmodelc files, not voice tensors).
                DIR* vdir = opendir(variant_path.c_str());
                if (vdir) {
                    struct dirent* ve;
                    while ((ve = readdir(vdir)) != nullptr) {
                        std::string vname = ve->d_name;
                        const std::string vsuf = "_voice.bin";
                        if (vname.size() > vsuf.size() &&
                            vname.substr(vname.size() - vsuf.size()) == vsuf)
                            voices.push_back(vname.substr(0, vname.size() - vsuf.size()));
                    }
                    closedir(vdir);
                }
                if (!first) json << ",";
                json << "{\"name\":\"" << escape_json(name) << "\""
                     << ",\"path\":\"" << escape_json(variant_path) << "\""
                     << ",\"voices\":[";
                bool fv = true;
                for (auto& v : voices) {
                    if (!fv) json << ",";
                    json << "\"" << escape_json(v) << "\"";
                    fv = false;
                }
                json << "],\"coreml\":true}";
                first = false;
            }
            closedir(dir);
        }
        std::string current_args;
        {
            std::lock_guard<std::mutex> lock(services_mutex_);
            for (auto& svc : services_) {
                if (svc.name == "KOKORO_ENGINE") { current_args = svc.default_args; break; }
            }
        }
        json << "],\"current_args\":\"" << escape_json(current_args) << "\"}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    // GET /api/models/neutts — NeuTTS model presence check for service config info panel.
    void handle_models_neutts(struct mg_connection *c) {
        std::string neutts_path = "bin/models/neutts-nano-german";
        std::string coreml_path = neutts_path + "/neucodec_decoder.mlpackage";
        struct stat st;
        bool exists = (stat(neutts_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode));
        bool coreml = exists && (stat(coreml_path.c_str(), &st) == 0);
        std::stringstream json;
        json << "{\"exists\":" << (exists ? "true" : "false")
             << ",\"coreml\":" << (coreml ? "true" : "false")
             << ",\"path\":\"" << escape_json(neutts_path) << "\""
             << ",\"coreml_path\":\"" << escape_json(coreml_path) << "\"}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    // POST /api/models/upload — Chunked model file upload.
    // Query params: service, filename, chunk_idx, total_chunks
    // Body: raw binary chunk data (keep <= 512KB to stay within MG_MAX_RECV_SIZE)
    void handle_models_upload(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }

        char service_buf[32] = {0}, filename_buf[256] = {0};
        char chunk_idx_buf[16] = {0}, total_chunks_buf[16] = {0};
        mg_http_get_var(&hm->query, "service", service_buf, sizeof(service_buf));
        mg_http_get_var(&hm->query, "filename", filename_buf, sizeof(filename_buf));
        mg_http_get_var(&hm->query, "chunk_idx", chunk_idx_buf, sizeof(chunk_idx_buf));
        mg_http_get_var(&hm->query, "total_chunks", total_chunks_buf, sizeof(total_chunks_buf));

        std::string service(service_buf);
        std::string filename(filename_buf);
        auto parse_uint = [](const char* s, int def) {
            if (!s || !*s) return def;
            for (const char* p = s; *p; ++p) if (!isdigit((unsigned char)*p)) return -1;
            long v = atol(s);
            if (v < 0 || v > 1000000) return -1;
            return static_cast<int>(v);
        };
        int chunk_idx = parse_uint(chunk_idx_buf, 0);
        int total_chunks = parse_uint(total_chunks_buf, 1);
        if (chunk_idx < 0 || total_chunks < 1 || chunk_idx >= total_chunks) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                "{\"error\":\"Invalid chunk_idx/total_chunks\"}");
            return;
        }

        if (service != "whisper" && service != "llama" && service != "kokoro" && service != "neutts") {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Invalid service\"}");
            return;
        }

        if (filename.empty() || filename.find('/') != std::string::npos ||
            filename.find('\\') != std::string::npos ||
            filename.find("..") != std::string::npos ||
            filename.size() > 200) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Invalid filename\"}");
            return;
        }
        for (unsigned char ch : filename) {
            if (!(isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '+')) {
                mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                    "{\"error\":\"Invalid filename character\"}");
                return;
            }
        }

        auto has_suffix = [&](const std::string& suf) {
            return filename.size() > suf.size() &&
                   filename.substr(filename.size() - suf.size()) == suf;
        };
        bool valid_ext = false;
        if (service == "whisper") valid_ext = has_suffix(".bin");
        if (service == "llama")   valid_ext = has_suffix(".gguf");
        if (service == "kokoro" || service == "neutts") {
            valid_ext = has_suffix(".bin") || has_suffix(".gguf") || has_suffix(".pth") ||
                        has_suffix(".tar.gz") || has_suffix(".tgz") || has_suffix(".zip") ||
                        has_suffix(".tar.bz2") || has_suffix(".tar.xz");
        }
        if (!valid_ext) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                "{\"error\":\"File extension not valid for this service type\"}");
            return;
        }

        mkdir("bin/models", 0755);
        std::string temp_path  = "bin/models/.upload_" + filename;
        std::string final_path = "bin/models/" + filename;

        if (hm->body.len > 0) {
            int flags = (chunk_idx == 0) ? (O_WRONLY | O_CREAT | O_TRUNC) : (O_WRONLY | O_APPEND);
            int fd = open(temp_path.c_str(), flags, 0644);
            if (fd < 0) {
                mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                    "{\"error\":\"Cannot write to models directory\"}");
                return;
            }
            ssize_t written = write(fd, hm->body.buf, hm->body.len);
            close(fd);
            if (written != (ssize_t)hm->body.len) {
                unlink(temp_path.c_str());
                mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Write error\"}");
                return;
            }
        }

        bool is_last = (chunk_idx >= total_chunks - 1);
        if (!is_last) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                "{\"received\":%d,\"total\":%d}", chunk_idx + 1, total_chunks);
            return;
        }

        char magic[16] = {0};
        FILE* mf = fopen(temp_path.c_str(), "rb");
        if (mf) { fread(magic, 1, 15, mf); fclose(mf); }
        std::string magic_str(magic);
        if (magic_str.find("<!DOCTYPE") != std::string::npos ||
            magic_str.find("<html") != std::string::npos) {
            unlink(temp_path.c_str());
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                "{\"error\":\"Uploaded file appears to be an HTML page, not a model file.\"}");
            return;
        }

        if (rename(temp_path.c_str(), final_path.c_str()) != 0) {
            unlink(temp_path.c_str());
            mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                "{\"error\":\"Failed to finalise uploaded file\"}");
            return;
        }

        bool needs_convert = (service == "whisper" || service == "kokoro" || service == "neutts");
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
            "{\"complete\":true,\"path\":\"%s\",\"filename\":\"%s\","
            "\"service\":\"%s\",\"needs_convert\":%s}",
            escape_json(final_path).c_str(),
            escape_json(filename).c_str(),
            escape_json(service).c_str(),
            needs_convert ? "true" : "false");
    }

    // POST /api/models/convert — Trigger async CoreML conversion for a model.
    void handle_models_convert(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        std::string body(hm->body.buf, hm->body.len);
        std::string service = extract_json_string(body, "service");
        std::string path = extract_json_string(body, "path");

        if (service.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing service\"}");
            return;
        }
        if (service != "whisper" && service != "kokoro" && service != "neutts") {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Invalid service. Use whisper, kokoro, or neutts\"}");
            return;
        }

        if (!path.empty()) {
            if (path.find("..") != std::string::npos ||
                path.find('\0') != std::string::npos ||
                path.find("bin/models/") != 0 ||
                path.size() > 512) {
                mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                    "{\"error\":\"Invalid path. Must start with 'bin/models/' and contain no '..'\"}");
                return;
            }
            for (unsigned char ch : path) {
                if (!(isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == '/')) {
                    mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                        "{\"error\":\"Invalid path character\"}");
                    return;
                }
            }
        }

        int64_t task_id = create_async_task("model_convert");

        std::thread([this, task_id, service, path]() {
            auto resolve_cmd = [](const std::string& cmd) -> std::string {
                int pipefd[2];
                if (pipe(pipefd) != 0) return "";
                pid_t pid = fork();
                if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return ""; }
                if (pid == 0) {
                    close(pipefd[0]);
                    dup2(pipefd[1], STDOUT_FILENO);
                    int dn = open("/dev/null", O_WRONLY);
                    if (dn >= 0) { dup2(dn, STDERR_FILENO); close(dn); }
                    close(pipefd[1]);
                    execlp("which", "which", cmd.c_str(), nullptr);
                    _exit(127);
                }
                close(pipefd[1]);
                char buf[512] = {0};
                ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
                close(pipefd[0]);
                int status = 0;
                waitpid(pid, &status, 0);
                if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || n <= 0) return "";
                std::string result(buf, n);
                while (!result.empty() && (result.back() == '\n' || result.back() == '\r' || result.back() == ' '))
                    result.pop_back();
                return result;
            };

            std::string python_path = resolve_cmd("python3");
            if (python_path.empty()) python_path = resolve_cmd("python");
            if (python_path.empty()) {
                finish_async_task(task_id, "{\"error\":\"Python not found. Install Python 3 and required dependencies.\"}");
                return;
            }

            std::string script_path;
            std::vector<std::string> extra_args;
            if (service == "whisper") {
                script_path = "scripts/export_coreml.py";
                if (!path.empty()) extra_args.push_back(path);
            } else if (service == "kokoro") {
                script_path = "scripts/export_kokoro_models.py";
                if (!path.empty()) {
                    extra_args.push_back("--variant");
                    extra_args.push_back(path);
                }
            } else if (service == "neutts") {
                script_path = "scripts/convert_neucodec_to_coreml.py";
            }

            struct stat st;
            if (stat(script_path.c_str(), &st) != 0) {
                finish_async_task(task_id, "{\"error\":\"Conversion script not found: " + escape_json(script_path) + "\"}");
                return;
            }

            update_async_task_progress(task_id, "{\"status\":\"running\",\"message\":\"Starting conversion...\"}");

            std::vector<const char*> argv;
            argv.push_back(python_path.c_str());
            argv.push_back(script_path.c_str());
            for (auto& a : extra_args) argv.push_back(a.c_str());
            argv.push_back(nullptr);

            int pipefd[2];
            if (pipe(pipefd) != 0) {
                finish_async_task(task_id, "{\"error\":\"Failed to create pipe\"}");
                return;
            }

            pid_t pid = fork();
            if (pid < 0) {
                close(pipefd[0]); close(pipefd[1]);
                finish_async_task(task_id, "{\"error\":\"Failed to fork process\"}");
                return;
            }

            if (pid == 0) {
                close(pipefd[0]);
                dup2(pipefd[1], STDOUT_FILENO);
                dup2(pipefd[1], STDERR_FILENO);
                close(pipefd[1]);
                execvp(python_path.c_str(), const_cast<char* const*>(argv.data()));
                _exit(127);
            }

            close(pipefd[1]);
            std::string output;
            char buf[4096];
            ssize_t n;
            while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                output.append(buf, n);
                if (output.size() > 16384) output = output.substr(output.size() - 16384);
            }
            close(pipefd[0]);

            int status = 0;
            waitpid(pid, &status, 0);
            int exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

            if (exit_code == 0) {
                finish_async_task(task_id, "{\"status\":\"done\",\"message\":\"Conversion completed successfully\"}");
            } else {
                std::string out_snippet = output.size() > 512 ? output.substr(output.size() - 512) : output;
                finish_async_task(task_id, "{\"error\":\"Conversion failed (exit " + std::to_string(exit_code) + "): " + escape_json(out_snippet) + "\"}");
            }
        }).detach();

        mg_http_reply(c, 202, "Content-Type: application/json\r\n", "{\"task_id\":%lld}", (long long)task_id);
    }

    // POST /api/models/download — Async download of a model file from HuggingFace.
    // Spawns a background curl process; progress available via handle_models_download_progress.
    void handle_models_download(struct mg_connection *c, struct mg_http_message *hm) {
        std::string body(hm->body.buf, hm->body.len);
        std::string repo_id = extract_json_string(body, "repo_id");
        std::string filename = extract_json_string(body, "filename");
        std::string model_name = extract_json_string(body, "model_name");
        std::string backend = extract_json_string(body, "backend");
        std::string service = extract_json_string(body, "service");
        if (service != "whisper" && service != "llama" && service != "kokoro" && service != "neutts") service = "whisper";

        if (repo_id.empty() || filename.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                "{\"error\":\"Missing repo_id or filename\"}");
            return;
        }

        if (!is_safe_repo_id(repo_id)) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                "{\"error\":\"Invalid repo_id. Use format: owner/model with alphanumeric, dash, underscore, dot only.\"}");
            return;
        }

        if (filename.find("..") != std::string::npos || filename.empty() || filename.front() == '/' || filename.size() > 512) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                "{\"error\":\"Invalid filename.\"}");
            return;
        }
        for (unsigned char ch : filename) {
            if (!isalnum(ch) && ch != '-' && ch != '_' && ch != '.' && ch != '/') {
                mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                    "{\"error\":\"Invalid filename. Use alphanumeric, dash, underscore, dot, slash only.\"}");
                return;
            }
        }

        std::string local_filename = filename;
        {
            auto slash = filename.rfind('/');
            if (slash != std::string::npos) local_filename = filename.substr(slash + 1);
        }
        if (local_filename.empty() || local_filename.front() == '.') {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                "{\"error\":\"Invalid filename after path extraction.\"}");
            return;
        }

        if (model_name.empty()) model_name = local_filename;
        if (backend.empty()) backend = "coreml";

        std::string hf_token = get_setting("hf_token", "");

        std::string models_dir = "bin/models";
        mkdir(models_dir.c_str(), 0755);

        std::string local_path = models_dir + "/" + local_filename;

        char abs_models_dir[PATH_MAX];
        if (!realpath(models_dir.c_str(), abs_models_dir)) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                "{\"error\":\"Cannot resolve models directory\"}");
            return;
        }

        struct stat st;
        if (stat(local_path.c_str(), &st) == 0) {
            mg_http_reply(c, 409, "Content-Type: application/json\r\n",
                "{\"error\":\"File already exists\",\"path\":\"%s\"}", escape_json(local_path).c_str());
            return;
        }

        int64_t dl_id = ++async_id_counter_;
        auto progress = std::make_shared<DownloadProgress>();
        progress->filename = local_filename;
        progress->local_path = local_path;
        progress->service = service;
        {
            std::lock_guard<std::mutex> lock(downloads_mutex_);
            downloads_[dl_id] = progress;
        }

        std::string abs_models_str(abs_models_dir);

        std::thread([this, repo_id, filename, local_filename, local_path, hf_token, model_name, backend, service, progress, abs_models_str]() {
            auto fail = [&progress](const std::string& msg) {
                {
                    std::lock_guard<std::mutex> lock(progress->mu);
                    progress->error = msg;
                }
                progress->failed.store(true);
                progress->complete.store(true);
            };

            std::string hf_cli = find_hf_cli();

            if (!hf_cli.empty()) {
                std::string tmp_dir = local_path + ".hf_tmp";
                mkdir(tmp_dir.c_str(), 0755);
                std::string tmp_file = tmp_dir + "/" + filename;

                auto tracker_stop = std::make_shared<std::atomic<bool>>(false);
                std::thread size_tracker([tmp_file, progress, tracker_stop](){
                    while (!tracker_stop->load()) {
                        struct stat st;
                        if (stat(tmp_file.c_str(), &st) == 0)
                            progress->bytes_downloaded.store(st.st_size);
                        std::this_thread::sleep_for(std::chrono::milliseconds(DOWNLOAD_PROGRESS_POLL_MS));
                    }
                });

                std::string cli_stderr;
                int ret = run_hf_cli_download(hf_cli, repo_id, filename, tmp_dir, hf_token, &cli_stderr);
                tracker_stop->store(true);
                size_tracker.join();

                if (ret != 0) {
                    unlink(tmp_file.c_str());
                    rmdir(tmp_dir.c_str());
                    std::string detail = "Download failed (huggingface-cli exit " + std::to_string(ret) + ")";
                    if (!cli_stderr.empty()) {
                        std::string clipped = cli_stderr.substr(0, 512);
                        for (auto& ch : clipped) if (ch == '\n' || ch == '\r') ch = ' ';
                        detail += ": " + clipped;
                    }
                    fail(detail);
                    return;
                }

                struct stat fst;
                if (stat(tmp_file.c_str(), &fst) != 0 || fst.st_size < 1024) {
                    unlink(tmp_file.c_str());
                    rmdir(tmp_dir.c_str());
                    fail("Downloaded file is missing or too small. Check the filename and repo ID.");
                    return;
                }

                if (rename(tmp_file.c_str(), local_path.c_str()) != 0) {
                    fail("Failed to move downloaded file to final location");
                    unlink(tmp_file.c_str());
                    rmdir(tmp_dir.c_str());
                    return;
                }
                rmdir(tmp_dir.c_str());

            } else {
                std::string url = "https://huggingface.co/" + repo_id + "/resolve/main/" + filename;
                std::string tmp_path = local_path + ".downloading";

                std::string header_file = write_curl_header_file(hf_token);

                std::vector<std::string> head_args = {"-s", "-S", "-L", "-I", "--max-time", "10"};
                if (!header_file.empty()) {
                    head_args.push_back("-H");
                    head_args.push_back("@" + header_file);
                }
                head_args.push_back(url);

                std::string head_resp = run_curl_safe(head_args);
                {
                    std::string lower_head;
                    for (char ch : head_resp) lower_head += tolower(ch);
                    size_t cl_pos = lower_head.find("content-length:");
                    if (cl_pos != std::string::npos) {
                        int64_t cl = atoll(head_resp.c_str() + cl_pos + 15);
                        if (cl > 0) progress->total_bytes.store(cl);
                    }
                }

                std::vector<std::string> dl_args = {"-s", "-S", "-L", "-f", "--max-time", "3600", "-o", tmp_path};
                if (!header_file.empty()) {
                    dl_args.push_back("-H");
                    dl_args.push_back("@" + header_file);
                }
                dl_args.push_back(url);

                auto tracker_stop = std::make_shared<std::atomic<bool>>(false);
                std::thread size_tracker([tmp_path, progress, tracker_stop](){
                    while (!tracker_stop->load()) {
                        struct stat st;
                        if (stat(tmp_path.c_str(), &st) == 0)
                            progress->bytes_downloaded.store(st.st_size);
                        std::this_thread::sleep_for(std::chrono::milliseconds(DOWNLOAD_PROGRESS_POLL_MS));
                    }
                });

                std::string curl_stderr;
                int ret = run_curl_to_file(dl_args, &curl_stderr);
                tracker_stop->store(true);
                size_tracker.join();

                if (!header_file.empty()) unlink(header_file.c_str());

                struct stat fst;
                bool stat_ok = (stat(tmp_path.c_str(), &fst) == 0);
                if (ret != 0 || !stat_ok || fst.st_size < 1024) {
                    std::string detail;
                    std::string body_snippet;
                    if (stat_ok && fst.st_size > 0) {
                        FILE* ef = fopen(tmp_path.c_str(), "rb");
                        if (ef) {
                            char ebuf[1024] = {0};
                            size_t en = fread(ebuf, 1, sizeof(ebuf) - 1, ef);
                            fclose(ef);
                            body_snippet.assign(ebuf, en);
                        }
                    }
                    std::string body_lower;
                    for (unsigned char ch : body_snippet) body_lower += tolower(ch);
                    bool looks_html = body_lower.find("<!doctype") != std::string::npos ||
                                      body_lower.find("<html") != std::string::npos ||
                                      body_lower.find("error code") != std::string::npos ||
                                      body_lower.find("unauthor") != std::string::npos ||
                                      body_lower.find("forbidden") != std::string::npos ||
                                      body_lower.find("not found") != std::string::npos;
                    if (looks_html || (stat_ok && fst.st_size > 0 && fst.st_size < 1024)) {
                        detail = "HuggingFace returned an error page instead of the model file. "
                                 "The repo may be gated/private, the filename may be wrong, "
                                 "or your HF token may be missing/invalid. ";
                        if (!body_snippet.empty()) {
                            std::string clipped = body_snippet.substr(0, 240);
                            for (auto& ch : clipped) if (ch == '\n' || ch == '\r' || ch == '\t') ch = ' ';
                            detail += "Server response: " + clipped;
                        }
                    } else {
                        detail = "Download failed (curl exit " + std::to_string(ret) + ")";
                        if (!curl_stderr.empty()) {
                            detail += ": " + curl_stderr.substr(0, 512);
                        } else if (!stat_ok) {
                            detail += ": curl produced no output file";
                        }
                    }
                    fail(detail);
                    unlink(tmp_path.c_str());
                    return;
                }

                if (rename(tmp_path.c_str(), local_path.c_str()) != 0) {
                    fail("Failed to move file to final path");
                    unlink(tmp_path.c_str());
                    return;
                }
            }

            char abs_path[PATH_MAX];
            if (!realpath(local_path.c_str(), abs_path) ||
                std::string(abs_path).find(abs_models_str) != 0) {
                fail("Path traversal detected — file removed");
                unlink(local_path.c_str());
                return;
            }

            {
                char magic[16] = {0};
                size_t magic_n = 0;
                FILE* mf = fopen(local_path.c_str(), "rb");
                if (mf) { magic_n = fread(magic, 1, 15, mf); fclose(mf); }
                std::string magic_str(magic, magic_n);
                if (magic_str.find("<!DOCTYPE") != std::string::npos ||
                    magic_str.find("<html") != std::string::npos) {
                    fail("Downloaded file appears to be an HTML error page "
                         "(HuggingFace auth error or 404). Check your HF token.");
                    unlink(local_path.c_str());
                    return;
                }
            }

            struct stat fst_final;
            stat(local_path.c_str(), &fst_final);
            progress->bytes_downloaded.store(fst_final.st_size);

            if (db_) {
                int size_mb = static_cast<int>(fst_final.st_size / (1024 * 1024));

                sqlite3_stmt* stmt;
                const char* sql = "INSERT INTO models (service, name, path, backend, size_mb, config_json, added_timestamp) VALUES (?, ?, ?, ?, ?, '{}', ?)";
                if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
                    sqlite3_bind_text(stmt, 1, service.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 2, model_name.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 3, abs_path, -1, SQLITE_TRANSIENT);
                    sqlite3_bind_text(stmt, 4, backend.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_bind_int(stmt, 5, size_mb);
                    sqlite3_bind_int64(stmt, 6, static_cast<sqlite3_int64>(time(nullptr)));
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
            }

            progress->complete.store(true);
        }).detach();

        mg_http_reply(c, 202, "Content-Type: application/json\r\n",
            "{\"download_id\":%lld,\"filename\":\"%s\",\"path\":\"%s\"}",
            (long long)dl_id, escape_json(filename).c_str(), escape_json(local_path).c_str());
    }

    // GET /api/models/download/progress?id=N — Poll download progress for a model download.
    // Returns bytes downloaded, total size, completion/failure state.
    void handle_models_download_progress(struct mg_connection *c, struct mg_http_message *hm) {
        char id_buf[32] = {0};
        mg_http_get_var(&hm->query, "id", id_buf, sizeof(id_buf));
        int64_t dl_id = atoll(id_buf);
        if (dl_id <= 0) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing id\"}");
            return;
        }

        int64_t snap_bytes = 0, snap_total = 0;
        bool snap_complete = false, snap_failed = false;
        std::string snap_error, snap_filename, snap_local_path, snap_service;
        bool found = false, is_done = false;

        {
            std::lock_guard<std::mutex> lock(downloads_mutex_);
            auto it = downloads_.find(dl_id);
            if (it != downloads_.end()) {
                found = true;
                auto& p = it->second;
                snap_bytes = p->bytes_downloaded.load();
                snap_total = p->total_bytes.load();
                snap_complete = p->complete.load();
                snap_failed = p->failed.load();
                {
                    std::lock_guard<std::mutex> plock(p->mu);
                    snap_error = p->error;
                }
                snap_filename = p->filename;
                snap_local_path = p->local_path;
                snap_service = p->service;
                is_done = snap_complete || snap_failed;
                if (is_done) downloads_.erase(it);
            }
        }

        if (!found) {
            mg_http_reply(c, 404, "Content-Type: application/json\r\n", "{\"error\":\"Unknown download id\"}");
            return;
        }

        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
            "{\"bytes_downloaded\":%lld,\"total_bytes\":%lld,\"complete\":%s,\"failed\":%s"
            ",\"error\":\"%s\",\"filename\":\"%s\",\"path\":\"%s\",\"service\":\"%s\"}",
            (long long)snap_bytes,
            (long long)snap_total,
            snap_complete ? "true" : "false",
            snap_failed ? "true" : "false",
            escape_json(snap_error).c_str(),
            escape_json(snap_filename).c_str(),
            escape_json(snap_local_path).c_str(),
            escape_json(snap_service).c_str());
    }

    // POST /api/models/add — Register a locally-present model file in SQLite.
    // Verifies file exists on disk before inserting.
    void handle_models_add(struct mg_connection *c, struct mg_http_message *hm) {
        if (!db_) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database not available\"}");
            return;
        }
        
        std::string body(hm->body.buf, hm->body.len);
        std::string service = extract_json_string(body, "service");
        std::string name = extract_json_string(body, "name");
        std::string path = extract_json_string(body, "path");
        std::string backend = extract_json_string(body, "backend");
        std::string config = extract_json_string(body, "config");
        
        if (service.empty() || name.empty() || path.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing required fields: service, name, path\"}");
            return;
        }
        
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Model file not found at specified path\"}");
            return;
        }
        int size_mb = static_cast<int>(st.st_size / (1024 * 1024));
        
        sqlite3_stmt* stmt;
        const char* insert_sql = "INSERT INTO models (service, name, path, backend, size_mb, config_json, added_timestamp) VALUES (?, ?, ?, ?, ?, ?, ?) RETURNING id";
        int rc = sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database prepare failed\"}");
            return;
        }
        
        sqlite3_bind_text(stmt, 1, service.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, path.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 4, backend.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 5, size_mb);
        sqlite3_bind_text(stmt, 6, config.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 7, static_cast<sqlite3_int64>(time(nullptr)));
        
        rc = sqlite3_step(stmt);
        sqlite3_int64 model_id = 0;
        if (rc == SQLITE_ROW) {
            model_id = sqlite3_column_int64(stmt, 0);
        }
        sqlite3_finalize(stmt);
        
        if (model_id == 0) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Failed to insert model\"}");
            return;
        }
        
        std::stringstream response;
        response << "{\"success\":true,\"model_id\":" << model_id << "}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", response.str().c_str());
    }
    
    // Whisper Model Benchmark endpoint (POST /api/whisper/benchmark).
    //
    // Runs a performance benchmark for a registered Whisper model by injecting
    // test audio through the full pipeline and measuring real metrics:
    //   1. Validates test_sip_provider is running with an active call
    //   2. Looks up the model in the database by model_id
    //   3. For each iteration × test file: injects audio via test_sip_provider,
    //      captures Whisper transcription from the log stream, and measures
    //      accuracy (Levenshtein similarity vs ground truth) and latency
    //   4. Computes aggregate statistics: avg accuracy, avg/p50/p95/p99 latency
    //   5. Uses the model's file size on disk as memory estimate (MB)
    //   6. Stores results in model_benchmark_runs table for later comparison
    //
    // Requires: Full pipeline running — test_sip_provider, sip-client,
    //           inbound-audio-processor, vad-service, and whisper-service with active call.
    //
    // Parameters (JSON body):
    //   model_id   — registered model ID from the models table
    //   test_files — array of WAV filenames from Testfiles/ directory
    //   iterations — number of passes over all files (1–10, default 1)
    //
    // Returns: Aggregated benchmark metrics for model comparison charts.
    void handle_whisper_benchmark(struct mg_connection *c, struct mg_http_message *hm) {
        if (!db_) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database not available\"}");
            return;
        }
        
        struct mg_str json_body = hm->body;

        int model_id = (int)mg_json_get_long(json_body, "$.model_id", 0);
        if (model_id == 0) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing or invalid model_id\"}");
            return;
        }

        std::vector<std::string> test_files;
        {
            struct mg_str key, val;
            size_t ofs = 0;
            int arr_len = 0;
            int arr_ofs = mg_json_get(json_body, "$.test_files", &arr_len);
            if (arr_ofs >= 0) {
                struct mg_str arr = mg_str_n(json_body.buf + arr_ofs, (size_t)arr_len);
                while ((ofs = mg_json_next(arr, ofs, &key, &val)) > 0) {
                    if (val.len >= 2 && val.buf[0] == '"') {
                        char buf[512];
                        if (mg_json_unescape(mg_str_n(val.buf + 1, val.len - 2), buf, sizeof(buf))) {
                            test_files.push_back(buf);
                        }
                    }
                }
            }
        }

        if (test_files.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"No test files specified\"}");
            return;
        }

        int iterations = (int)mg_json_get_long(json_body, "$.iterations", 1);
        if (iterations < 1) iterations = 1;
        if (iterations > 10) iterations = 10;
        
        sqlite3_stmt* stmt;
        const char* model_query = "SELECT name, path, backend, config_json FROM models WHERE id = ?";
        int rc = sqlite3_prepare_v2(db_, model_query, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database query failed\"}");
            return;
        }
        
        sqlite3_bind_int(stmt, 1, model_id);
        if (sqlite3_step(stmt) != SQLITE_ROW) {
            sqlite3_finalize(stmt);
            mg_http_reply(c, 404, "Content-Type: application/json\r\n", "{\"error\":\"Model not found\"}");
            return;
        }
        
        const char* mn_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        const char* mp_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        const char* be_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        const char* cf_raw = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        std::string model_name = mn_raw ? mn_raw : "";
        std::string model_path = mp_raw ? mp_raw : "";
        std::string backend = be_raw ? be_raw : "";
        std::string config = cf_raw ? cf_raw : "";
        sqlite3_finalize(stmt);
        
        std::stringstream files_json;
        files_json << "[";
        for (size_t i = 0; i < test_files.size(); i++) {
            if (i > 0) files_json << ",";
            files_json << "\"" << escape_json(test_files[i]) << "\"";
        }
        files_json << "]";

        int memory_mb = get_service_memory_mb("WHISPER_SERVICE");
        if (memory_mb == 0) {
            struct stat mst;
            if (stat(model_path.c_str(), &mst) == 0) {
                memory_mb = static_cast<int>(mst.st_size / (1024 * 1024));
            }
        }

        int64_t task_id = create_async_task("benchmark");
        std::string files_json_str = files_json.str();

        {
            std::lock_guard<std::mutex> lock(async_mutex_);
            auto& task = async_tasks_[task_id];
            task->worker = std::thread(&FrontendServer::run_benchmark_async, this,
                task_id, test_files, iterations, model_id, model_name, backend, files_json_str, memory_mb);
        }

        mg_http_reply(c, 202, "Content-Type: application/json\r\n",
            "{\"status\":\"started\",\"task_id\":%lld}", (long long)task_id);
    }

    void run_benchmark_async(int64_t task_id, std::vector<std::string> test_files,
            int iterations, int model_id, std::string model_name, std::string model_backend,
            std::string files_json_str, int memory_mb) {
        std::string pipeline_err = validate_pipeline_services();
        if (!pipeline_err.empty()) {
            finish_async_task(task_id, "{\"error\":\"" + escape_json(pipeline_err) + "\"}");
            return;
        }

        std::string sip_err;
        std::string sip_status = http_get_localhost(TEST_SIP_PROVIDER_PORT, "/status", sip_err);
        if (!sip_err.empty()) {
            finish_async_task(task_id, "{\"error\":\"test_sip_provider not reachable: " + escape_json(sip_err) + "\"}");
            return;
        }
        if (sip_status.find("\"call_active\":true") == std::string::npos) {
            finish_async_task(task_id, "{\"error\":\"No active call. Start SIP client + IAP + VAD + Whisper and establish a call first.\"}");
            return;
        }

        std::vector<double> latencies;
        std::vector<double> accuracies;
        int pass_count = 0;
        int fail_count = 0;

        for (int iter = 0; iter < iterations; iter++) {
            for (size_t fi = 0; fi < test_files.size(); fi++) {
                const auto& file = test_files[fi];

                std::string ground_truth;
                {
                    std::lock_guard<std::mutex> lock(testfiles_mutex_);
                    for (const auto& tf : testfiles_) {
                        if (tf.name == file) {
                            ground_truth = tf.ground_truth;
                            break;
                        }
                    }
                }

                uint64_t seq_before = current_log_seq();
                auto t0 = std::chrono::steady_clock::now();

                std::string inject_body = "{\"file\":\"" + escape_json(file) + "\",\"leg\":\"a\"}";
                std::string inject_err;
                http_post_localhost(TEST_SIP_PROVIDER_PORT, "/inject", inject_body, inject_err);

                TranscriptionResult tr = wait_for_whisper_transcription(seq_before, 30000);
                auto t1 = std::chrono::steady_clock::now();
                double elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();

                double latency = tr.found ? (tr.whisper_latency_ms > 0 ? tr.whisper_latency_ms : elapsed_ms) : elapsed_ms;
                double accuracy = 0.0;
                if (tr.found && !ground_truth.empty()) {
                    accuracy = calculate_levenshtein_similarity(ground_truth, tr.text);
                }

                std::this_thread::sleep_for(std::chrono::seconds(3));

                latencies.push_back(latency);
                accuracies.push_back(accuracy);

                if (accuracy >= 95.0) {
                    pass_count++;
                } else {
                    fail_count++;
                }
            }
        }

        if (latencies.empty()) {
            finish_async_task(task_id, "{\"status\":\"done\",\"error\":\"No benchmark results collected\"}");
            return;
        }

        std::sort(latencies.begin(), latencies.end());
        double avg_accuracy = 0;
        for (double acc : accuracies) avg_accuracy += acc;
        avg_accuracy /= accuracies.size();

        size_t n = latencies.size();
        int p50_latency = static_cast<int>(latencies[n * 50 / 100]);
        int p95_latency = static_cast<int>(latencies[std::min(n - 1, n * 95 / 100)]);
        int p99_latency = static_cast<int>(latencies[std::min(n - 1, n * 99 / 100)]);

        double avg_latency = 0;
        for (double lat : latencies) avg_latency += lat;
        avg_latency /= n;

        sqlite3_int64 run_id = 0;
        sqlite3_stmt* ins_stmt;
        const char* insert_sql = "INSERT INTO model_benchmark_runs "
            "(model_id, model_name, model_type, backend, test_files, iterations, files_tested, "
            "avg_accuracy, avg_latency_ms, p50_latency_ms, p95_latency_ms, p99_latency_ms, "
            "memory_mb, pass_count, fail_count, timestamp) "
            "VALUES (?1,?2,'whisper',?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,strftime('%s','now')) RETURNING id";
        int ins_rc = sqlite3_prepare_v2(db_, insert_sql, -1, &ins_stmt, nullptr);
        if (ins_rc == SQLITE_OK) {
            sqlite3_bind_int(ins_stmt, 1, model_id);
            sqlite3_bind_text(ins_stmt, 2, model_name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins_stmt, 3, model_backend.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins_stmt, 4, files_json_str.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(ins_stmt, 5, iterations);
            sqlite3_bind_int(ins_stmt, 6, (int)test_files.size());
            sqlite3_bind_double(ins_stmt, 7, avg_accuracy);
            sqlite3_bind_int(ins_stmt, 8, static_cast<int>(avg_latency));
            sqlite3_bind_int(ins_stmt, 9, p50_latency);
            sqlite3_bind_int(ins_stmt, 10, p95_latency);
            sqlite3_bind_int(ins_stmt, 11, p99_latency);
            sqlite3_bind_int(ins_stmt, 12, memory_mb);
            sqlite3_bind_int(ins_stmt, 13, pass_count);
            sqlite3_bind_int(ins_stmt, 14, fail_count);
            if (sqlite3_step(ins_stmt) == SQLITE_ROW) {
                run_id = sqlite3_column_int64(ins_stmt, 0);
            }
            sqlite3_finalize(ins_stmt);
        }

        std::stringstream response;
        response << "{\"status\":\"done\",\"success\":true"
                << ",\"run_id\":" << run_id
                << ",\"model_name\":\"" << escape_json(model_name) << "\""
                << ",\"files_tested\":" << (test_files.size() * iterations)
                << ",\"avg_accuracy\":" << avg_accuracy
                << ",\"avg_latency_ms\":" << avg_latency
                << ",\"p50_latency_ms\":" << p50_latency
                << ",\"p95_latency_ms\":" << p95_latency
                << ",\"p99_latency_ms\":" << p99_latency
                << ",\"memory_mb\":" << memory_mb
                << ",\"pass_count\":" << pass_count
                << ",\"fail_count\":" << fail_count
                << "}";

        finish_async_task(task_id, response.str());
    }

    // strip_sql_comments() — Remove SQL comments (-- to EOL, /* ... */) while
    // preserving string literal content. Used by is_read_only_query() to prevent
    // comment-based bypass of the keyword guard (e.g., "SELECT --\nDROP TABLE...").
    static std::string strip_sql_comments(const std::string& sql) {
        std::string result;
        result.reserve(sql.size());
        size_t i = 0;
        while (i < sql.size()) {
            if (sql[i] == '\'' ) {
                result += sql[i++];
                while (i < sql.size()) {
                    if (sql[i] == '\'' && i + 1 < sql.size() && sql[i + 1] == '\'') {
                        result += sql[i]; result += sql[i + 1];
                        i += 2;
                    } else if (sql[i] == '\'') {
                        result += sql[i++];
                        break;
                    } else {
                        result += sql[i++];
                    }
                }
            } else if (i + 1 < sql.size() && sql[i] == '-' && sql[i + 1] == '-') {
                while (i < sql.size() && sql[i] != '\n') i++;
            } else if (i + 1 < sql.size() && sql[i] == '/' && sql[i + 1] == '*') {
                i += 2;
                while (i + 1 < sql.size() && !(sql[i] == '*' && sql[i + 1] == '/')) i++;
                if (i + 1 < sql.size()) i += 2;
                else i = sql.size();
            } else {
                result += sql[i];
                i++;
            }
        }
        return result;
    }

    // is_read_only_query() — SQL guard for /api/db/query when write mode is off.
    //
    // Security layers (defense-in-depth):
    //   1. sqlite3_prepare_v2 only compiles one statement → multi-statement injection
    //      like "SELECT 1; DROP TABLE x" is impossible.
    //   2. strip_sql_comments() removes --, /* */ before keyword check.
    //   3. LOAD_EXTENSION substring check → blocks SELECT load_extension('...').
    //   4. sqlite3_db_config(SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0) in
    //      init_database() disables extension loading at runtime (belt-and-suspenders).
    //   5. PRAGMA whitelist: only read-only PRAGMAs are allowed; any PRAGMA with
    //      '=' (setting a value) is rejected.
    static bool is_read_only_query(const std::string& query) {
        std::string stripped = strip_sql_comments(query);
        size_t start = stripped.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return false;
        stripped = stripped.substr(start);
        if (stripped.size() < 6) return false;
        std::string upper;
        for (size_t i = 0; i < stripped.size(); i++) {
            upper += static_cast<char>(toupper(static_cast<unsigned char>(stripped[i])));
        }
        if (upper.find("LOAD_EXTENSION") != std::string::npos) return false;
        if (upper.substr(0, 6) == "SELECT" || upper.substr(0, 7) == "EXPLAIN") return true;
        if (upper.substr(0, 6) == "PRAGMA") {
            if (upper.find('=') != std::string::npos) return false;
            static const char* safe[] = {
                "PRAGMA TABLE_INFO", "PRAGMA TABLE_LIST", "PRAGMA TABLE_XINFO",
                "PRAGMA DATABASE_LIST", "PRAGMA INDEX_LIST", "PRAGMA INDEX_INFO",
                "PRAGMA SCHEMA_VERSION", "PRAGMA PAGE_COUNT", "PRAGMA PAGE_SIZE",
                "PRAGMA FREELIST_COUNT", "PRAGMA INTEGRITY_CHECK", "PRAGMA QUICK_CHECK",
                "PRAGMA COMPILE_OPTIONS", "PRAGMA FOREIGN_KEY_LIST", nullptr
            };
            for (int i = 0; safe[i]; i++) {
                if (upper.find(safe[i]) == 0) return true;
            }
            return false;
        }
        return false;
    }

    void handle_db_write_mode(struct mg_connection *c, struct mg_http_message *hm);
    void handle_db_query(struct mg_connection *c, struct mg_http_message *hm);

    // -------------------------------------------------------------------------
    // Password hashing (PBKDF2-SHA256 via OpenSSL)
    // -------------------------------------------------------------------------

    static std::string bytes_to_hex(const unsigned char* data, size_t len) {
        std::string out;
        out.reserve(len * 2);
        for (size_t i = 0; i < len; i++) {
            char buf[3];
            snprintf(buf, sizeof(buf), "%02x", data[i]);
            out += buf;
        }
        return out;
    }

    static std::string generate_salt() {
        unsigned char raw[16];
        RAND_bytes(raw, sizeof(raw));
        return bytes_to_hex(raw, sizeof(raw));
    }

    static std::string hash_password(const std::string& password, const std::string& salt) {
        unsigned char out[32];
        PKCS5_PBKDF2_HMAC(password.c_str(), (int)password.size(),
                           (const unsigned char*)salt.c_str(), (int)salt.size(),
                           100000, EVP_sha256(), 32, out);
        return bytes_to_hex(out, 32);
    }

    static bool verify_password(const std::string& password,
                                const std::string& salt,
                                const std::string& stored_hash) {
        return hash_password(password, salt) == stored_hash;
    }

    // -------------------------------------------------------------------------
    // Session management
    // -------------------------------------------------------------------------

    std::string create_session(const std::string& username) {
        unsigned char raw[32];
        RAND_bytes(raw, sizeof(raw));
        std::string token = bytes_to_hex(raw, 32);

        if (!db_) return "";
        sqlite3_stmt* stmt;
        const char* sql = "INSERT INTO sessions (token, username, created_at, last_seen) VALUES (?, ?, ?, ?)";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            time_t now = time(nullptr);
            sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, username.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 3, (int64_t)now);
            sqlite3_bind_int64(stmt, 4, (int64_t)now);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        return token;
    }

    bool validate_session(const std::string& token, std::string& out_username) {
        if (!db_ || token.empty() || token.size() != 64) return false;

        sqlite3_stmt* stmt;
        const char* sql = "SELECT username, last_seen FROM sessions WHERE token = ?";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);

        bool valid = false;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* uname = (const char*)sqlite3_column_text(stmt, 0);
            int64_t last_seen = sqlite3_column_int64(stmt, 1);
            time_t now = time(nullptr);
            if (uname && (now - last_seen) < 24 * 3600) {
                out_username = uname;
                valid = true;
            }
        }
        sqlite3_finalize(stmt);

        if (valid) {
            sqlite3_stmt* upd;
            const char* usql = "UPDATE sessions SET last_seen = ? WHERE token = ?";
            if (sqlite3_prepare_v2(db_, usql, -1, &upd, nullptr) == SQLITE_OK) {
                sqlite3_bind_int64(upd, 1, (int64_t)time(nullptr));
                sqlite3_bind_text(upd, 2, token.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(upd);
                sqlite3_finalize(upd);
            }
        }
        return valid;
    }

    void invalidate_session(const std::string& token) {
        if (!db_ || token.empty()) return;
        sqlite3_stmt* stmt;
        const char* sql = "DELETE FROM sessions WHERE token = ?";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, token.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    void cleanup_expired_sessions() {
        if (!db_) return;
        sqlite3_stmt* stmt;
        const char* sql = "DELETE FROM sessions WHERE last_seen < ?";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, (int64_t)(time(nullptr) - 24 * 3600));
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        time_t now = time(nullptr);
        for (auto it = login_failures_.begin(); it != login_failures_.end(); ) {
            if (now - it->second.window_start > 60) it = login_failures_.erase(it);
            else ++it;
        }
    }

    static std::string extract_session_cookie(struct mg_http_message *hm) {
        struct mg_str* cookie_ptr = mg_http_get_header(hm, "Cookie");
        if (!cookie_ptr || !cookie_ptr->buf || cookie_ptr->len == 0) return "";
        struct mg_str cookie_hdr = *cookie_ptr;

        std::string cookies(cookie_hdr.buf, cookie_hdr.len);
        const std::string key = "prodigy_session=";
        size_t pos = cookies.find(key);
        if (pos == std::string::npos) return "";
        pos += key.size();
        size_t end = cookies.find(';', pos);
        if (end == std::string::npos) end = cookies.size();
        std::string token = cookies.substr(pos, end - pos);
        while (!token.empty() && token.front() == ' ') token.erase(token.begin());
        while (!token.empty() && token.back() == ' ') token.pop_back();
        return token;
    }

    // -------------------------------------------------------------------------
    // Seed default admin user
    // -------------------------------------------------------------------------

    void seed_default_admin_if_empty() {
        if (!db_) return;
        sqlite3_stmt* stmt;
        const char* check = "SELECT COUNT(*) FROM users";
        if (sqlite3_prepare_v2(db_, check, -1, &stmt, nullptr) != SQLITE_OK) return;
        int count = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) count = sqlite3_column_int(stmt, 0);
        sqlite3_finalize(stmt);
        if (count > 0) return;

        std::string salt = generate_salt();
        std::string phash = hash_password("admin", salt);
        time_t now = time(nullptr);

        sqlite3_stmt* ins;
        const char* sql = "INSERT INTO users (username, password_hash, salt, created_at) VALUES (?, ?, ?, ?)";
        if (sqlite3_prepare_v2(db_, sql, -1, &ins, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(ins, 1, "admin", -1, SQLITE_STATIC);
            sqlite3_bind_text(ins, 2, phash.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins, 3, salt.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(ins, 4, (int64_t)now);
            sqlite3_step(ins);
            sqlite3_finalize(ins);
        }
        std::fprintf(stderr, "[auth] Seeded default admin user\n");
    }

    // -------------------------------------------------------------------------
    // Login page
    // -------------------------------------------------------------------------

    void serve_login_page(struct mg_connection *c) {
        const char* html = R"HTML(<!DOCTYPE html><html lang="en"><head>
<meta charset="UTF-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Prodigy Login</title>
<style>
*{box-sizing:border-box;margin:0;padding:0}
body{background:#0d0d14;color:#e0e0e0;font-family:system-ui,sans-serif;
  display:flex;align-items:center;justify-content:center;min-height:100vh}
.login-box{background:#1a1a2e;border:1px solid #2a2a4a;border-radius:12px;
  padding:2.5rem;width:100%;max-width:360px;box-shadow:0 8px 32px rgba(0,0,0,.5)}
h1{font-size:1.5rem;text-align:center;margin-bottom:1.8rem;
  background:linear-gradient(135deg,#ff2d95,#b026ff,#00fff5);
  -webkit-background-clip:text;-webkit-text-fill-color:transparent}
label{display:block;font-size:.8rem;color:#aaa;margin-bottom:.35rem;margin-top:1rem}
input{width:100%;padding:.65rem .9rem;background:#0d0d1a;border:1px solid #333;
  border-radius:6px;color:#e0e0e0;font-size:.95rem;outline:none}
input:focus{border-color:#b026ff}
button{margin-top:1.5rem;width:100%;padding:.75rem;background:#b026ff;
  color:#fff;border:none;border-radius:6px;font-size:1rem;cursor:pointer;
  transition:background .2s}
button:hover{background:#9000e0}
#err{margin-top:1rem;color:#ff4d6d;font-size:.85rem;text-align:center;min-height:1.2rem}
</style></head><body>
<div class="login-box">
<h1>PRODIGY</h1>
<form id="f">
<label>Username</label>
<input type="text" id="u" autocomplete="username" required>
<label>Password</label>
<input type="password" id="p" autocomplete="current-password" required>
<button type="submit">Login</button>
</form>
<div id="err"></div>
</div>
<script>
document.getElementById('f').onsubmit=async function(e){
  e.preventDefault();
  document.getElementById('err').textContent='';
  try{
    const r=await fetch('/api/auth/login',{method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({username:document.getElementById('u').value,
                           password:document.getElementById('p').value})});
    if(r.ok){location.href='/';}
    else{const d=await r.json();document.getElementById('err').textContent=d.error||'Login failed';}
  }catch(ex){document.getElementById('err').textContent='Network error';}
};
</script>
</body></html>)HTML";
        mg_http_reply(c, 200, "Content-Type: text/html; charset=utf-8\r\n", "%s", html);
    }

    // -------------------------------------------------------------------------
    // Auth API handlers
    // -------------------------------------------------------------------------

    void handle_api_auth_login(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        std::string body(hm->body.buf, hm->body.len);
        std::string username = extract_json_string(body, "username");
        std::string password = extract_json_string(body, "password");

        if (username.empty() || password.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                          "{\"error\":\"username and password required\"}");
            return;
        }

        char ip_buf[48] = {};
        if (!c->rem.is_ip6) {
            snprintf(ip_buf, sizeof(ip_buf), "%u.%u.%u.%u",
                     c->rem.ip[0], c->rem.ip[1], c->rem.ip[2], c->rem.ip[3]);
        } else {
            snprintf(ip_buf, sizeof(ip_buf),
                     "%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x:%02x%02x",
                     c->rem.ip[0],c->rem.ip[1],c->rem.ip[2],c->rem.ip[3],
                     c->rem.ip[4],c->rem.ip[5],c->rem.ip[6],c->rem.ip[7],
                     c->rem.ip[8],c->rem.ip[9],c->rem.ip[10],c->rem.ip[11],
                     c->rem.ip[12],c->rem.ip[13],c->rem.ip[14],c->rem.ip[15]);
        }
        std::string ip(ip_buf);

        time_t now = time(nullptr);
        auto& fa = login_failures_[ip];
        if (fa.count >= 5 && (now - fa.window_start) < 60) {
            mg_http_reply(c, 429, "Content-Type: application/json\r\n",
                          "{\"error\":\"Too many failed attempts. Try again later.\"}");
            return;
        }
        if ((now - fa.window_start) >= 60) {
            fa.count = 0;
            fa.window_start = now;
        }

        if (!db_) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"DB unavailable\"}");
            return;
        }

        sqlite3_stmt* stmt;
        const char* sql = "SELECT password_hash, salt FROM users WHERE username = ?";
        bool auth_ok = false;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                const char* phash = (const char*)sqlite3_column_text(stmt, 0);
                const char* salt  = (const char*)sqlite3_column_text(stmt, 1);
                if (phash && salt) {
                    auth_ok = verify_password(password, std::string(salt), std::string(phash));
                }
            }
            sqlite3_finalize(stmt);
        }

        if (!auth_ok) {
            fa.count++;
            fa.window_start = now;
            mg_http_reply(c, 401, "Content-Type: application/json\r\n",
                          "{\"error\":\"Invalid credentials\"}");
            return;
        }

        fa.count = 0;
        std::string token = create_session(username);
        mg_printf(c,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Set-Cookie: prodigy_session=%s; HttpOnly; Secure; SameSite=Strict; Path=/\r\n"
            "Content-Length: 12\r\n"
            "\r\n"
            "{\"ok\":true}", token.c_str());
    }

    void handle_api_auth_logout(struct mg_connection *c, struct mg_http_message *hm) {
        std::string token = extract_session_cookie(hm);
        if (!token.empty()) invalidate_session(token);
        mg_printf(c,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Set-Cookie: prodigy_session=; HttpOnly; Secure; SameSite=Strict; Path=/; Max-Age=0\r\n"
            "Content-Length: 12\r\n"
            "\r\n"
            "{\"ok\":true}");
    }

    void handle_api_auth_users(struct mg_connection *c, struct mg_http_message *hm) {
        (void)hm;
        if (!db_) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"DB unavailable\"}");
            return;
        }
        sqlite3_stmt* stmt;
        const char* sql = "SELECT id, username, created_at FROM users ORDER BY id";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"DB error\"}");
            return;
        }
        std::stringstream json;
        json << "{\"users\":[";
        int i = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (i++ > 0) json << ",";
            int id = sqlite3_column_int(stmt, 0);
            const char* uname = (const char*)sqlite3_column_text(stmt, 1);
            int64_t ts = sqlite3_column_int64(stmt, 2);
            json << "{\"id\":" << id
                 << ",\"username\":\"" << escape_json(uname ? uname : "") << "\""
                 << ",\"created_at\":" << ts << "}";
        }
        sqlite3_finalize(stmt);
        json << "]}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    void handle_api_auth_users_add(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        std::string body(hm->body.buf, hm->body.len);
        std::string username = extract_json_string(body, "username");
        std::string password = extract_json_string(body, "password");

        if (username.empty() || password.size() < 4) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                          "{\"error\":\"username required and password must be at least 4 characters\"}");
            return;
        }
        if (!db_) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"DB unavailable\"}");
            return;
        }
        std::string salt = generate_salt();
        std::string phash = hash_password(password, salt);
        sqlite3_stmt* stmt;
        const char* sql = "INSERT INTO users (username, password_hash, salt, created_at) VALUES (?, ?, ?, ?)";
        int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"DB error\"}");
            return;
        }
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, phash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 3, salt.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(stmt, 4, (int64_t)time(nullptr));
        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        if (rc == SQLITE_CONSTRAINT) {
            mg_http_reply(c, 409, "Content-Type: application/json\r\n",
                          "{\"error\":\"Username already exists\"}");
        } else if (rc != SQLITE_DONE) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Insert failed\"}");
        } else {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"ok\":true}");
        }
    }

    void handle_api_auth_users_change_password(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        std::string body(hm->body.buf, hm->body.len);
        std::string username = extract_json_string(body, "username");
        std::string current_pw = extract_json_string(body, "current_password");
        std::string new_pw = extract_json_string(body, "new_password");

        if (username.empty() || new_pw.size() < 4) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                          "{\"error\":\"username required and new password must be at least 4 characters\"}");
            return;
        }
        if (!db_) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"DB unavailable\"}");
            return;
        }

        sqlite3_stmt* stmt;
        const char* sel = "SELECT password_hash, salt FROM users WHERE username = ?";
        if (sqlite3_prepare_v2(db_, sel, -1, &stmt, nullptr) != SQLITE_OK) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"DB error\"}");
            return;
        }
        sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        bool found = false;
        std::string old_hash, old_salt;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* ph = (const char*)sqlite3_column_text(stmt, 0);
            const char* s  = (const char*)sqlite3_column_text(stmt, 1);
            if (ph && s) { old_hash = ph; old_salt = s; found = true; }
        }
        sqlite3_finalize(stmt);

        if (!found) {
            mg_http_reply(c, 404, "Content-Type: application/json\r\n", "{\"error\":\"User not found\"}");
            return;
        }
        if (!verify_password(current_pw, old_salt, old_hash)) {
            mg_http_reply(c, 401, "Content-Type: application/json\r\n",
                          "{\"error\":\"Current password is incorrect\"}");
            return;
        }

        std::string new_salt = generate_salt();
        std::string new_hash = hash_password(new_pw, new_salt);
        sqlite3_stmt* upd;
        const char* usql = "UPDATE users SET password_hash=?, salt=? WHERE username=?";
        if (sqlite3_prepare_v2(db_, usql, -1, &upd, nullptr) != SQLITE_OK) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"DB error\"}");
            return;
        }
        sqlite3_bind_text(upd, 1, new_hash.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(upd, 2, new_salt.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(upd, 3, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(upd);
        sqlite3_finalize(upd);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"ok\":true}");
    }

    void handle_api_auth_users_delete(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        std::string body(hm->body.buf, hm->body.len);
        std::string username = extract_json_string(body, "username");
        if (username.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"username required\"}");
            return;
        }
        if (!db_) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"DB unavailable\"}");
            return;
        }
        sqlite3_stmt* cnt;
        const char* csql = "SELECT COUNT(*) FROM users";
        int total = 0;
        if (sqlite3_prepare_v2(db_, csql, -1, &cnt, nullptr) == SQLITE_OK) {
            if (sqlite3_step(cnt) == SQLITE_ROW) total = sqlite3_column_int(cnt, 0);
            sqlite3_finalize(cnt);
        }
        if (total <= 1) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                          "{\"error\":\"Cannot delete the last user\"}");
            return;
        }
        sqlite3_stmt* del;
        const char* dsql = "DELETE FROM users WHERE username = ?";
        if (sqlite3_prepare_v2(db_, dsql, -1, &del, nullptr) != SQLITE_OK) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"DB error\"}");
            return;
        }
        sqlite3_bind_text(del, 1, username.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(del);
        sqlite3_finalize(del);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"ok\":true}");
    }

    void handle_api_auth_settings(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("GET")) == 0) {
            std::string auth_enabled = get_setting("auth_enabled", "0");
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                          "{\"auth_enabled\":\"%s\"}", auth_enabled.c_str());
        } else {
            std::string body(hm->body.buf, hm->body.len);
            std::string val = extract_json_string(body, "auth_enabled");
            if (!val.empty()) set_setting("auth_enabled", val);
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"ok\":true}");
        }
    }

    // -------------------------------------------------------------------------
    // Certificate management
    // -------------------------------------------------------------------------

    void check_and_refresh_cert() {
        if (get_setting("cert_self_refresh", "1") != "1") return;

        std::string active_cert = get_setting("active_cert_name", "server.crt");
        bool is_selfsigned = (active_cert == "server.crt" ||
                              active_cert.substr(0, 11) == "selfsigned_");
        if (!is_selfsigned) return;

        std::string dir = prodigy_tls::tls_dir();
        std::string cert_path = dir + "/" + active_cert;

        time_t expiry = prodigy_tls::get_cert_expiry(cert_path);
        time_t now = time(nullptr);
        bool needs_refresh = (expiry == 0 || expiry < now + 7 * 24 * 3600);
        if (!needs_refresh) return;

        char ts[16];
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        strftime(ts, sizeof(ts), "%Y%m%d", &tm_buf);

        std::string new_name = std::string("selfsigned_") + ts + ".crt";
        std::string new_key_name = std::string("selfsigned_") + ts + ".key";
        std::string new_cert_path = dir + "/" + new_name;
        std::string new_key_path  = dir + "/" + new_key_name;

        if (prodigy_tls::generate_self_signed_cert_90d(new_cert_path, new_key_path)) {
            set_setting("active_cert_name", new_name);
            set_setting("active_key_name", new_key_name);
            prodigy_tls::reload_certs(new_cert_path, new_key_path);
            std::fprintf(stderr, "[cert] Auto-refreshed TLS certificate: %s\n", new_name.c_str());
        }
    }

    void handle_api_certs_list(struct mg_connection *c) {
        std::string dir = prodigy_tls::tls_dir();
        auto certs = prodigy_tls::list_certs_in_dir(dir);
        std::string active = get_setting("active_cert_name", "server.crt");
        std::string self_refresh = get_setting("cert_self_refresh", "1");
        std::string http_redirect = get_setting("http_redirect_to_https", "0");
        std::string ic_encryption = get_setting("ic_encryption_enabled", "0");

        time_t now = time(nullptr);
        std::stringstream json;
        json << "{\"certs\":[";
        for (size_t i = 0; i < certs.size(); i++) {
            if (i > 0) json << ",";
            const std::string& name = certs[i];
            std::string cert_path = dir + "/" + name;
            time_t expiry = prodigy_tls::get_cert_expiry(cert_path);
            long days = (expiry > now) ? (long)((expiry - now) / 86400) : -1;
            bool is_active = (name == active);
            std::string type = "custom";
            if (name.substr(0, 11) == "selfsigned_" || name == "server.crt") type = "selfsigned";
            else if (name.substr(0, 9) == "uploaded_") type = "uploaded";
            json << "{\"name\":\"" << escape_json(name) << "\""
                 << ",\"expiry\":" << (int64_t)expiry
                 << ",\"days_remaining\":" << days
                 << ",\"active\":" << (is_active ? "true" : "false")
                 << ",\"type\":\"" << type << "\"}";
        }
        json << "]"
             << ",\"active_cert\":\"" << escape_json(active) << "\""
             << ",\"self_refresh\":\"" << self_refresh << "\""
             << ",\"http_redirect\":\"" << http_redirect << "\""
             << ",\"ic_encryption\":\"" << ic_encryption << "\"}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    void handle_api_certs_generate(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        std::string dir = prodigy_tls::tls_dir();
        mkdir(dir.c_str(), 0700);

        time_t now = time(nullptr);
        char ts[16];
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        strftime(ts, sizeof(ts), "%Y%m%d", &tm_buf);

        std::string new_name = std::string("selfsigned_") + ts + ".crt";
        std::string new_key_name = std::string("selfsigned_") + ts + ".key";
        std::string cert_path = dir + "/" + new_name;
        std::string key_path  = dir + "/" + new_key_name;

        if (!prodigy_tls::generate_self_signed_cert_90d(cert_path, key_path)) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                          "{\"error\":\"Certificate generation failed\"}");
            return;
        }
        set_setting("active_cert_name", new_name);
        set_setting("active_key_name", new_key_name);
        prodigy_tls::reload_certs(cert_path, key_path);

        time_t expiry = prodigy_tls::get_cert_expiry(cert_path);
        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"ok\":true,\"name\":\"%s\",\"expiry\":%lld}",
                      escape_json(new_name).c_str(), (long long)expiry);
    }

    void handle_api_certs_upload(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }

        std::string cert_data, key_data;
        struct mg_http_part part;
        size_t pos = 0;
        while ((pos = mg_http_next_multipart(hm->body, pos, &part)) > 0) {
            if (part.name.len > 0 && part.body.len > 0) {
                std::string name(part.name.buf, part.name.len);
                if (name == "cert") {
                    cert_data.assign(part.body.buf, part.body.len);
                } else if (name == "key") {
                    key_data.assign(part.body.buf, part.body.len);
                }
            }
        }

        if (cert_data.empty() || key_data.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                          "{\"error\":\"Both cert and key fields are required\"}");
            return;
        }
        if (cert_data.size() > 65536 || key_data.size() > 65536) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                          "{\"error\":\"File too large (max 64KB)\"}");
            return;
        }
        if (!prodigy_tls::validate_pem_cert(cert_data)) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                          "{\"error\":\"Invalid PEM certificate\"}");
            return;
        }
        if (!prodigy_tls::validate_pem_key(key_data)) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                          "{\"error\":\"Invalid PEM private key\"}");
            return;
        }

        std::string dir = prodigy_tls::tls_dir();
        mkdir(dir.c_str(), 0700);

        time_t now = time(nullptr);
        char ts[16];
        struct tm tm_buf;
        localtime_r(&now, &tm_buf);
        strftime(ts, sizeof(ts), "%Y%m%d", &tm_buf);

        std::string new_name = std::string("uploaded_") + ts + ".crt";
        std::string new_key_name = std::string("uploaded_") + ts + ".key";
        std::string cert_path = dir + "/" + new_name;
        std::string key_path  = dir + "/" + new_key_name;

        {
            std::ofstream f(cert_path, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) {
                mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                              "{\"error\":\"Cannot write certificate\"}");
                return;
            }
            f.write(cert_data.data(), cert_data.size());
        }
        chmod(cert_path.c_str(), 0600);

        {
            std::ofstream f(key_path, std::ios::binary | std::ios::trunc);
            if (!f.is_open()) {
                std::remove(cert_path.c_str());
                mg_http_reply(c, 500, "Content-Type: application/json\r\n",
                              "{\"error\":\"Cannot write key\"}");
                return;
            }
            f.write(key_data.data(), key_data.size());
        }
        chmod(key_path.c_str(), 0600);

        set_setting("active_cert_name", new_name);
        set_setting("active_key_name", new_key_name);
        prodigy_tls::reload_certs(cert_path, key_path);

        mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                      "{\"ok\":true,\"name\":\"%s\"}", escape_json(new_name).c_str());
    }

    void handle_api_certs_select(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 405, "Content-Type: application/json\r\n", "{\"error\":\"POST required\"}");
            return;
        }
        std::string body(hm->body.buf, hm->body.len);
        std::string name = extract_json_string(body, "name");

        if (name.empty() || name.find('/') != std::string::npos ||
            name.find("..") != std::string::npos ||
            name.size() < 5 || name.substr(name.size() - 4) != ".crt") {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                          "{\"error\":\"Invalid certificate name\"}");
            return;
        }

        std::string dir = prodigy_tls::tls_dir();
        std::string cert_path = dir + "/" + name;
        std::string key_name = name.substr(0, name.size() - 4) + ".key";
        std::string key_path = dir + "/" + key_name;

        if (!prodigy_tls::file_exists(cert_path) || !prodigy_tls::file_exists(key_path)) {
            mg_http_reply(c, 404, "Content-Type: application/json\r\n",
                          "{\"error\":\"Certificate or key file not found\"}");
            return;
        }

        set_setting("active_cert_name", name);
        set_setting("active_key_name", key_name);
        prodigy_tls::reload_certs(cert_path, key_path);

        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"ok\":true}");
    }

    void handle_api_certs_settings(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("GET")) == 0) {
            std::string self_refresh  = get_setting("cert_self_refresh", "1");
            std::string http_redirect = get_setting("http_redirect_to_https", "0");
            std::string ic_encryption = get_setting("ic_encryption_enabled", "0");
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                          "{\"self_refresh\":\"%s\",\"http_redirect\":\"%s\",\"ic_encryption\":\"%s\"}",
                          self_refresh.c_str(), http_redirect.c_str(), ic_encryption.c_str());
        } else {
            std::string body(hm->body.buf, hm->body.len);
            std::string sr = extract_json_string(body, "self_refresh");
            std::string hr = extract_json_string(body, "http_redirect");
            std::string ic = extract_json_string(body, "ic_encryption");
            if (!sr.empty()) set_setting("cert_self_refresh", sr);
            if (!hr.empty()) set_setting("http_redirect_to_https", hr);
            if (!ic.empty()) {
                set_setting("ic_encryption_enabled", ic);
                prodigy_tls::ic_encryption_set_persistent(ic == "1");
            }
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"ok\":true}");
        }
    }

};

#include "log-server.h"
#include "database.h"
#include "javascript.h"
#include "frontend-ui.h"

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    {
        char resolved[PATH_MAX];
        if (realpath(argv[0], resolved)) {
            std::string exe_real(resolved);
            size_t slash = exe_real.rfind('/');
            if (slash != std::string::npos) {
                std::string exe_dir = exe_real.substr(0, slash);
                size_t ps = exe_dir.rfind('/');
                if (ps != std::string::npos && exe_dir.substr(ps + 1) == "bin") {
                    std::string parent = exe_dir.substr(0, ps);
                    if (chdir(parent.c_str()) == 0) {
                        std::cout << "Working directory: " << parent << "\n";
                    } else {
                        std::cerr << "Warning: chdir to " << parent << " failed: " << strerror(errno) << "\n";
                    }
                } else {
                    if (chdir(exe_dir.c_str()) == 0) {
                        std::cout << "Working directory: " << exe_dir << "\n";
                    } else {
                        std::cerr << "Warning: chdir to " << exe_dir << " failed: " << strerror(errno) << "\n";
                    }
                }
            }
        } else {
            std::string exe_path = argv[0];
            size_t slash = exe_path.rfind('/');
            if (slash != std::string::npos) {
                std::string exe_dir = exe_path.substr(0, slash);
                if (!exe_dir.empty() && exe_dir != ".") {
                    std::string parent = exe_dir;
                    size_t ps = parent.rfind('/');
                    if (ps != std::string::npos && parent.substr(ps + 1) == "bin") {
                        parent = parent.substr(0, ps);
                        if (chdir(parent.c_str()) == 0) {
                            std::cout << "Working directory: " << parent << "\n";
                        }
                    } else {
                        if (chdir(exe_dir.c_str()) == 0) {
                            std::cout << "Working directory: " << exe_dir << "\n";
                        }
                    }
                }
            }
        }
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            struct stat st;
            if (stat("bin/frontend", &st) != 0) {
                std::string try_parent = std::string(cwd);
                size_t ps = try_parent.rfind('/');
                if (ps != std::string::npos && try_parent.substr(ps + 1) == "bin") {
                    std::string p = try_parent.substr(0, ps);
                    if (chdir(p.c_str()) == 0) {
                        std::cout << "Working directory (corrected): " << p << "\n";
                    }
                }
            }
        }
    }

    std::string project_root;
    {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd))) {
            project_root = cwd;
        } else {
            std::cerr << "Fatal: cannot determine working directory\n";
            return 1;
        }
    }

    struct stat st;
    if (stat((project_root + "/bin/frontend").c_str(), &st) != 0) {
        std::cerr << "Fatal: bin/frontend not found in project root: " << project_root << "\n";
        return 1;
    }

    uint16_t port = 8080;
    std::string db_mode;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = static_cast<uint16_t>(atoi(argv[++i]));
        } else if (strcmp(argv[i], "--db") == 0 && i + 1 < argc) {
            db_mode = argv[++i];
        }
    }

    std::string db_path = project_root + "/frontend.db";

    if (db_mode == "new") {
        if (database_exists(db_path)) {
            if (!backup_database(db_path)) {
                std::cerr << "Error: failed to backup existing database. Aborting.\n";
                return 1;
            }
        }
        std::cout << "Creating new database.\n";
    } else if (db_mode == "reuse") {
        if (!database_exists(db_path)) {
            std::cerr << "Error: --db reuse specified but no database found at " << db_path << "\n";
            return 1;
        }
        std::cout << "Reusing existing database.\n";
    } else if (db_mode.empty()) {
        if (!database_exists(db_path)) {
            std::cout << "No existing database found. Creating new database...\n";
        } else {
            char choice = prompt_database_action(db_path);
            if (choice == 'N') {
                if (!backup_database(db_path)) {
                    std::cerr << "Error: failed to backup existing database. Aborting.\n";
                    return 1;
                }
                std::cout << "Creating new database.\n";
            } else {
                std::cout << "Reusing existing database.\n";
                db_mode = "reuse";
            }
        }
    } else {
        std::cerr << "Error: unknown --db mode '" << db_mode << "'. Use 'new' or 'reuse'.\n";
        return 1;
    }

    mkdir((project_root + "/logs").c_str(), 0755);

    std::cout << "Prodigy Frontend Server\n";
    std::cout << "=======================\n\n";

    FrontendServer server(port, project_root);

    if (db_mode == "reuse") {
        if (!server.validate_schema()) {
            std::cerr << "Error: database schema validation failed. Aborting.\n";
            return 1;
        }
    }

    if (!server.start()) {
        return 1;
    }

    return 0;
}
