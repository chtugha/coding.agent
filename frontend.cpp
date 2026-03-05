// frontend.cpp — Web UI server, log aggregator, service manager, and test runner.
//
// The frontend is the central control plane for the WhisperTalk system. It:
//   - Serves a single-page web application (SPA) at http://0.0.0.0:8080/
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
#include "mongoose.h"
#include "sqlite3.h"
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

using namespace whispertalk;

static std::atomic<bool> s_sigint_received(false);
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
    FrontendServer(uint16_t http_port = 8080) 
        : http_port_(http_port),
          log_port_(0),
          interconnect_(ServiceType::FRONTEND),
          db_(nullptr) {
        
        init_database();
        discover_tests();
        load_services();
        scan_testfiles_directory();
    }

    ~FrontendServer() {
        if (db_) {
            sqlite3_close(db_);
        }
    }

    bool start() {
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
        
        std::string listen_addr = "http://127.0.0.1:" + std::to_string(http_port_);
        struct mg_connection *c = mg_http_listen(&mgr_, listen_addr.c_str(), http_handler_static, this);
        if (c) c->fn_data = this;
        
        std::cout << "Frontend web server started on " << listen_addr << "\n";
        std::cout << "Open http://localhost:" << http_port_ << " in your browser\n";

        auto last_flush = std::chrono::steady_clock::now();
        auto last_rotation = last_flush;
        auto last_svc_check = std::chrono::steady_clock::now();
        auto last_async_cleanup = std::chrono::steady_clock::now();
        while (!s_sigint_received) {
            mg_mgr_poll(&mgr_, 100);
            check_test_status();
            flush_sse_queue();

            auto now = std::chrono::steady_clock::now();
            if (now - last_flush >= std::chrono::milliseconds(500)) {
                flush_log_queue();
                last_flush = now;
            }
            if (now - last_svc_check >= std::chrono::seconds(2)) {
                check_service_status();
                last_svc_check = now;
            }
            if (now - last_async_cleanup >= std::chrono::seconds(30)) {
                cleanup_old_async_tasks();
                last_async_cleanup = now;
            }
            if (now - last_rotation >= std::chrono::hours(1)) {
                rotate_logs();
                last_rotation = now;
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
    bool db_write_mode_ = false;
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

    struct AsyncTask {
        int64_t id;
        std::string type;
        std::atomic<bool> running{true};
        std::atomic<bool> result_read{false};
        std::string result_json;
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

    int64_t create_async_task(const std::string& type) {
        int64_t id = ++async_id_counter_;
        auto task = std::make_shared<AsyncTask>();
        task->id = id;
        task->type = type;
        std::lock_guard<std::mutex> lock(async_mutex_);
        async_tasks_[id] = task;
        return id;
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

    void init_database() {
        int rc = sqlite3_open("frontend.db", &db_);
        if (rc != SQLITE_OK) {
            std::cerr << "Cannot open database: " << sqlite3_errmsg(db_) << "\n";
            db_ = nullptr;
            return;
        }

        const char* schema = R"(
            CREATE TABLE IF NOT EXISTS logs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TEXT NOT NULL,
                service TEXT NOT NULL,
                call_id INTEGER,
                level TEXT,
                message TEXT
            );
            CREATE INDEX IF NOT EXISTS idx_logs_timestamp ON logs(timestamp);
            CREATE INDEX IF NOT EXISTS idx_logs_service ON logs(service);
            CREATE INDEX IF NOT EXISTS idx_logs_service_ts ON logs(service, timestamp);
            
            CREATE TABLE IF NOT EXISTS test_runs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                test_name TEXT NOT NULL,
                start_time INTEGER,
                end_time INTEGER,
                exit_code INTEGER,
                arguments TEXT,
                log_file TEXT
            );
            
            CREATE TABLE IF NOT EXISTS service_status (
                service TEXT PRIMARY KEY,
                status TEXT,
                last_seen INTEGER,
                call_count INTEGER,
                ports TEXT
            );

            CREATE TABLE IF NOT EXISTS service_config (
                service TEXT PRIMARY KEY,
                binary_path TEXT NOT NULL,
                default_args TEXT DEFAULT '',
                description TEXT DEFAULT '',
                auto_start INTEGER DEFAULT 0
            );

            CREATE TABLE IF NOT EXISTS settings (
                key TEXT PRIMARY KEY,
                value TEXT
            );

            CREATE TABLE IF NOT EXISTS testfiles (
                name TEXT PRIMARY KEY,
                size_bytes INTEGER,
                duration_sec REAL,
                sample_rate INTEGER,
                channels INTEGER,
                ground_truth TEXT,
                last_modified INTEGER
            );

            CREATE TABLE IF NOT EXISTS whisper_accuracy_tests (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                test_run_id INTEGER,
                file_name TEXT,
                model_name TEXT,
                ground_truth TEXT,
                transcription TEXT,
                similarity_percent REAL,
                latency_ms INTEGER,
                status TEXT,
                timestamp INTEGER
            );

            CREATE TABLE IF NOT EXISTS iap_quality_tests (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                file_name TEXT,
                latency_ms REAL,
                snr_db REAL,
                rms_error_pct REAL,
                max_latency_ms REAL,
                status TEXT,
                timestamp INTEGER
            );

            CREATE TABLE IF NOT EXISTS models (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                service TEXT,
                name TEXT,
                path TEXT,
                backend TEXT,
                size_mb INTEGER,
                config_json TEXT,
                added_timestamp INTEGER
            );

            CREATE TABLE IF NOT EXISTS model_benchmark_runs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                model_id INTEGER,
                model_name TEXT,
                model_type TEXT DEFAULT 'whisper',
                backend TEXT,
                test_files TEXT,
                iterations INTEGER,
                files_tested INTEGER,
                avg_accuracy REAL,
                avg_latency_ms INTEGER,
                p50_latency_ms INTEGER,
                p95_latency_ms INTEGER,
                p99_latency_ms INTEGER,
                memory_mb INTEGER,
                pass_count INTEGER DEFAULT 0,
                fail_count INTEGER DEFAULT 0,
                avg_tokens REAL,
                interrupt_latency_ms REAL,
                german_pct REAL,
                timestamp INTEGER,
                FOREIGN KEY(model_id) REFERENCES models(id)
            );

            CREATE TABLE IF NOT EXISTS tts_validation_tests (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                line1_call_id INTEGER,
                line2_call_id INTEGER,
                original_text TEXT,
                tts_transcription TEXT,
                similarity_percent REAL,
                phoneme_errors TEXT,
                timestamp INTEGER
            );

            CREATE TABLE IF NOT EXISTS sip_lines (
                line_id INTEGER PRIMARY KEY,
                username TEXT,
                password TEXT,
                server TEXT,
                port INTEGER,
                status TEXT,
                last_registered INTEGER
            );

            CREATE TABLE IF NOT EXISTS service_test_runs (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                service TEXT,
                test_type TEXT,
                status TEXT,
                metrics_json TEXT,
                timestamp INTEGER
            );

            CREATE TABLE IF NOT EXISTS test_results (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                test_name TEXT,
                service TEXT,
                status TEXT,
                details TEXT,
                timestamp INTEGER
            );
        )";

        char* errmsg = nullptr;
        rc = sqlite3_exec(db_, schema, nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            std::cerr << "SQL error: " << errmsg << "\n";
            sqlite3_free(errmsg);
        }

        const char* migrations[] = {
            "ALTER TABLE model_benchmark_runs ADD COLUMN model_name TEXT",
            "ALTER TABLE model_benchmark_runs ADD COLUMN model_type TEXT DEFAULT 'whisper'",
            "ALTER TABLE model_benchmark_runs ADD COLUMN backend TEXT",
            "ALTER TABLE model_benchmark_runs ADD COLUMN files_tested INTEGER",
            "ALTER TABLE model_benchmark_runs ADD COLUMN pass_count INTEGER DEFAULT 0",
            "ALTER TABLE model_benchmark_runs ADD COLUMN fail_count INTEGER DEFAULT 0",
            "ALTER TABLE model_benchmark_runs ADD COLUMN avg_tokens REAL",
            "ALTER TABLE model_benchmark_runs ADD COLUMN interrupt_latency_ms REAL",
            "ALTER TABLE model_benchmark_runs ADD COLUMN german_pct REAL",
            "ALTER TABLE iap_quality_tests ADD COLUMN rms_error_pct REAL",
            "ALTER TABLE iap_quality_tests ADD COLUMN max_latency_ms REAL",
            "ALTER TABLE iap_quality_tests DROP COLUMN thd_percent",
            nullptr
        };
        for (int i = 0; migrations[i]; i++) {
            sqlite3_exec(db_, migrations[i], nullptr, nullptr, nullptr);
        }

        const char* seed = R"(
            INSERT OR IGNORE INTO service_config (service, binary_path, default_args, description) VALUES
                ('SIP_CLIENT', 'bin/sip-client', '--lines 2 alice 127.0.0.1 5060', 'SIP client / RTP gateway'),
                ('INBOUND_AUDIO_PROCESSOR', 'bin/inbound-audio-processor', '', 'G.711 decode + 8kHz to 16kHz resample'),
                ('VAD_SERVICE', 'bin/vad-service', '', 'Voice Activity Detection + speech segmentation'),
                ('WHISPER_SERVICE', 'bin/whisper-service', '--language de --model bin/models/ggml-large-v3-turbo-q5_0.bin', 'Whisper ASR (Metal)'),
                ('LLAMA_SERVICE', 'bin/llama-service', '', 'LLaMA 3.2-1B response generation'),
                ('KOKORO_SERVICE', 'bin/kokoro-service', '', 'Kokoro TTS (CoreML)'),
                ('OUTBOUND_AUDIO_PROCESSOR', 'bin/outbound-audio-processor', '', 'TTS audio to G.711 encode + RTP'),
                ('TEST_SIP_PROVIDER', 'bin/test_sip_provider', '--port 5060 --http-port 22011 --testfiles-dir Testfiles', 'SIP B2BUA test provider for audio injection');
            INSERT OR IGNORE INTO settings (key, value) VALUES ('theme', 'default');
            UPDATE service_config SET default_args='--language de --model bin/models/ggml-large-v3-turbo-q5_0.bin', description='Whisper ASR (Metal)' WHERE service='WHISPER_SERVICE' AND default_args LIKE '%models/ggml%' AND default_args NOT LIKE '%bin/models%';
            UPDATE service_config SET default_args='--lines 2 alice 127.0.0.1 5060' WHERE service='SIP_CLIENT' AND default_args='--lines 1 alice 127.0.0.1 5060';
        )";
        sqlite3_exec(db_, seed, nullptr, nullptr, nullptr);

        rotate_logs();
    }

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
            svc.name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            svc.binary_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
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

    bool is_service_running_unlocked(const std::string& name) {
        for (const auto& svc : services_) {
            if (svc.name == name) {
                return svc.managed && svc.pid > 0;
            }
        }
        return false;
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

    void kill_ghost_processes(const std::string& binary_name) {
        std::string cmd = "pgrep -f '" + binary_name + "' 2>/dev/null";
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
        if (!pids.empty()) usleep(500000);
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
                usleep(200000);
            }

            if (!is_allowed_binary(svc.binary_path)) return false;

            std::string use_args = args_override.empty() ? svc.default_args : args_override;

            if (name == "VAD_SERVICE" && args_override.empty()) {
                std::string vad_w = get_setting("vad_window_ms", "");
                std::string vad_t = get_setting("vad_threshold", "");
                std::string vad_s = get_setting("vad_silence_ms", "");
                std::string vad_c = get_setting("vad_max_chunk_ms", "");
                if (!vad_w.empty()) use_args += " --vad-window-ms " + vad_w;
                if (!vad_t.empty()) use_args += " --vad-threshold " + vad_t;
                if (!vad_s.empty()) use_args += " --vad-silence-ms " + vad_s;
                if (!vad_c.empty()) use_args += " --vad-max-chunk-ms " + vad_c;
            }

            if (args_override.empty()) {
                std::string ll_key = "log_level_" + name;
                std::string ll = get_setting(ll_key, "");
                if (!ll.empty()) use_args += " --log-level " + ll;
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
                for (int i = 3; i < 1024; ++i) close(i);

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
                usleep(100000);
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
        usleep(2000000);
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

    void log_receiver_loop() {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (sock < 0) {
            std::cerr << "Failed to create log socket\n";
            return;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(log_port_);

        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Failed to bind log socket to port " << log_port_ << "\n";
            close(sock);
            return;
        }

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        // recv buffer: 4096 bytes. Max inbound UDP datagram from LogForwarder is
        // 2303 bytes (buf[2304]-1), so 4096 provides ample headroom — no resize needed.
        char buffer[4096];
        while (!s_sigint_received) {
            ssize_t n = recv(sock, buffer, sizeof(buffer) - 1, 0);
            if (n > 0) {
                buffer[n] = '\0';
                process_log_message(std::string(buffer, n));
            }
        }

        close(sock);
    }

    static ServiceType parse_service_type(const std::string& name) {
        if (name == "SIP_CLIENT") return ServiceType::SIP_CLIENT;
        if (name == "INBOUND_AUDIO_PROCESSOR") return ServiceType::INBOUND_AUDIO_PROCESSOR;
        if (name == "VAD_SERVICE") return ServiceType::VAD_SERVICE;
        if (name == "WHISPER_SERVICE") return ServiceType::WHISPER_SERVICE;
        if (name == "LLAMA_SERVICE") return ServiceType::LLAMA_SERVICE;
        if (name == "KOKORO_SERVICE") return ServiceType::KOKORO_SERVICE;
        if (name == "OUTBOUND_AUDIO_PROCESSOR") return ServiceType::OUTBOUND_AUDIO_PROCESSOR;
        if (name == "FRONTEND") return ServiceType::FRONTEND;
        return ServiceType::SIP_CLIENT;
    }

    void process_log_message(const std::string& msg) {
        // Expected datagram format: "<SERVICE> <LEVEL> <CALL_ID> <message>"
        // Malformed datagrams (e.g. stray UDP traffic) are silently dropped here
        // to prevent garbage entries in the DB and UI.
        if (msg.empty()) return;

        size_t p1 = msg.find(' ');
        size_t p2 = (p1 != std::string::npos) ? msg.find(' ', p1 + 1) : std::string::npos;
        size_t p3 = (p2 != std::string::npos) ? msg.find(' ', p2 + 1) : std::string::npos;

        if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos) {
            return;
        }

        LogEntry entry;

        time_t now = time(nullptr);
        char timebuf[64];
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
        entry.timestamp = timebuf;

        entry.service = parse_service_type(msg.substr(0, p1));
        entry.level = msg.substr(p1 + 1, p2 - p1 - 1);
        try {
            entry.call_id = static_cast<uint32_t>(std::stoul(msg.substr(p2 + 1, p3 - p2 - 1)));
        } catch (const std::exception&) {
            entry.call_id = 0;
        }
        entry.message = msg.substr(p3 + 1);

        {
            std::lock_guard<std::mutex> lock(logs_mutex_);
            entry.seq = ++log_seq_;
            if (recent_logs_.size() >= MAX_RECENT_LOGS) {
                recent_logs_.pop_front();
            }
            recent_logs_.push_back(entry);
        }

        enqueue_log(entry);

        {
            std::lock_guard<std::mutex> lock(sse_queue_mutex_);
            sse_queue_.push_back(entry);
        }
    }

    std::mutex log_queue_mutex_;
    std::vector<LogEntry> log_queue_;

    // Async SQLite write design: enqueue_log() just appends to log_queue_ under a
    // mutex (O(1), <1µs). The main event loop calls flush_log_queue() every 500ms,
    // which batches all pending entries into a single BEGIN/COMMIT transaction —
    // typically < 1ms for hundreds of rows. No dedicated writer thread is needed.
    void enqueue_log(const LogEntry& entry) {
        std::lock_guard<std::mutex> lock(log_queue_mutex_);
        log_queue_.push_back(entry);
    }

    void flush_log_queue() {
        std::vector<LogEntry> batch;
        {
            std::lock_guard<std::mutex> lock(log_queue_mutex_);
            if (log_queue_.empty()) return;
            batch.swap(log_queue_);
        }

        if (!db_) return;

        if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
            std::cerr << "flush_log_queue: BEGIN failed: " << sqlite3_errmsg(db_) << "\n";
            return;
        }
        const char* sql = "INSERT INTO logs (timestamp, service, call_id, level, message) VALUES (?, ?, ?, ?, ?)";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "flush_log_queue: prepare failed: " << sqlite3_errmsg(db_) << "\n";
            sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
            return;
        }
        for (const auto& entry : batch) {
            sqlite3_bind_text(stmt, 1, entry.timestamp.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 2, service_type_to_string(entry.service), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 3, static_cast<int>(entry.call_id));
            sqlite3_bind_text(stmt, 4, entry.level.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(stmt, 5, entry.message.c_str(), -1, SQLITE_TRANSIENT);
            if (sqlite3_step(stmt) != SQLITE_DONE) {
                std::cerr << "flush_log_queue: insert failed: " << sqlite3_errmsg(db_) << "\n";
            }
            sqlite3_reset(stmt);
        }
        sqlite3_finalize(stmt);
        if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
            std::cerr << "flush_log_queue: COMMIT failed: " << sqlite3_errmsg(db_) << "\n";
        }
    }

    void rotate_logs() {
        if (!db_) return;
        const char* sql = "DELETE FROM logs WHERE timestamp < datetime('now', '-30 days')";
        sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
    }

    // GET /api/logs/stream — SSE live log stream. Registers the connection for
    // push-based log delivery. Max MAX_SSE_CONNECTIONS enforced (503 if exceeded).
    void handle_sse_stream(struct mg_connection *c, struct mg_http_message *hm) {
        {
            std::lock_guard<std::mutex> lock(sse_mutex_);
            if (sse_connections_.size() >= MAX_SSE_CONNECTIONS) {
                mg_http_reply(c, 503, "", "Too many SSE connections\n");
                return;
            }
        }

        char service_filter[30] = {0};
        mg_http_get_var(&hm->query, "service", service_filter, sizeof(service_filter));

        c->data[0] = 'S';
        memset(c->data + 1, 0, MG_DATA_SIZE - 1);
        if (service_filter[0]) {
            strncpy(c->data + 1, service_filter, MG_DATA_SIZE - 2);
        }

        mg_printf(c,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream; charset=utf-8\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Transfer-Encoding: chunked\r\n\r\n");

        {
            std::lock_guard<std::mutex> lock(sse_mutex_);
            sse_connections_.push_back(c);
        }
    }

    void remove_sse_connection(struct mg_connection *c) {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        sse_connections_.erase(
            std::remove(sse_connections_.begin(), sse_connections_.end(), c),
            sse_connections_.end());
    }

    void flush_sse_queue() {
        std::vector<LogEntry> batch;
        {
            std::lock_guard<std::mutex> lock(sse_queue_mutex_);
            if (sse_queue_.empty()) return;
            batch.swap(sse_queue_);
        }

        std::lock_guard<std::mutex> lock(sse_mutex_);
        if (sse_connections_.empty()) return;

        for (const auto& entry : batch) {
            std::string svc = service_type_to_string(entry.service);
            std::string json = "{\"timestamp\":\"" + escape_json(entry.timestamp) +
                "\",\"service\":\"" + escape_json(svc) +
                "\",\"level\":\"" + escape_json(entry.level) +
                "\",\"call_id\":" + std::to_string(entry.call_id) +
                ",\"message\":\"" + escape_json(entry.message) + "\"}";
            std::string sse_msg = "data: " + json + "\n\n";

            for (auto* c : sse_connections_) {
                std::string filter(c->data + 1, strnlen(c->data + 1, MG_DATA_SIZE - 1));
                if (!filter.empty() && svc != filter) continue;
                mg_http_printf_chunk(c, "%s", sse_msg.c_str());
            }
        }
    }

    static void http_handler_static(struct mg_connection *c, int ev, void *ev_data) {
        FrontendServer* self = static_cast<FrontendServer*>(c->fn_data);
        self->http_handler(c, ev, ev_data);
    }

    void http_handler(struct mg_connection *c, int ev, void *ev_data) {
        if (ev == MG_EV_CLOSE) {
            if (c->data[0] == 'S') {
                remove_sse_connection(c);
            }
            return;
        }
        if (ev == MG_EV_HTTP_MSG) {
            struct mg_http_message *hm = (struct mg_http_message *) ev_data;
            
            if (mg_strcmp(hm->uri, mg_str("/")) == 0) {
                serve_index(c);
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
            } else if (mg_match(hm->uri, mg_str("/css/theme/*"), NULL)) {
                serve_theme_css(c, hm);
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
            } else if (mg_strcmp(hm->uri, mg_str("/api/whisper/accuracy_test")) == 0) {
                handle_whisper_accuracy_test(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/whisper/hallucination_filter")) == 0) {
                handle_whisper_hallucination_filter(c, hm);
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
            } else if (mg_strcmp(hm->uri, mg_str("/api/models/download")) == 0) {
                handle_models_download(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/models/download/progress")) == 0) {
                handle_models_download_progress(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/whisper/benchmark")) == 0) {
                handle_whisper_benchmark(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/llama/prompts")) == 0) {
                handle_llama_prompts(c);
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
            } else {
                mg_http_reply(c, 404, "", "Not Found\n");
            }
        }
    }

    void serve_index(struct mg_connection *c) {
        std::string theme = get_setting("theme", "default");
        std::string html = build_ui_html(theme);
        mg_http_reply(c, 200, "Content-Type: text/html; charset=utf-8\r\n", "%s", html.c_str());
    }

    std::string build_ui_html(const std::string& theme) {
        std::string dark_attr;
        std::string theme_css_link;
        if (theme == "dark") {
            dark_attr = " data-bs-theme=\"dark\"";
        } else if (theme == "slate" || theme == "flatly" || theme == "cyborg") {
            theme_css_link = "<link rel=\"stylesheet\" href=\"/css/theme/" + theme + "\">";
        }

        std::string h;
        h += R"WT(<!DOCTYPE html><html lang="en")WT" + dark_attr + R"WT(><head><meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>WhisperTalk</title>
<link href="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css" rel="stylesheet">
)WT" + theme_css_link + R"WT(
<style>
:root{--wt-sidebar-width:240px;--wt-bg:#f5f5f7;--wt-sidebar-bg:rgba(255,255,255,0.72);--wt-card-bg:#fff;--wt-border:#d2d2d7;--wt-text:#1d1d1f;--wt-text-secondary:#86868b;--wt-accent:#0071e3;--wt-success:#34c759;--wt-danger:#ff3b30;--wt-warning:#ff9f0a;--wt-radius:12px;--wt-font:-apple-system,BlinkMacSystemFont,"SF Pro Display","SF Pro Text","Helvetica Neue",Helvetica,Arial,sans-serif;--wt-mono:"SF Mono",SFMono-Regular,ui-monospace,Menlo,monospace}
[data-bs-theme="dark"]{--wt-bg:#1c1c1e;--wt-sidebar-bg:rgba(44,44,46,0.72);--wt-card-bg:#2c2c2e;--wt-border:#38383a;--wt-text:#f5f5f7;--wt-text-secondary:#98989d}
*{box-sizing:border-box}
body{margin:0;font-family:var(--wt-font);background:var(--wt-bg);color:var(--wt-text);overflow:hidden;height:100vh}
.wt-app{display:flex;height:100vh}
.wt-sidebar{width:var(--wt-sidebar-width);min-width:var(--wt-sidebar-width);background:var(--wt-sidebar-bg);backdrop-filter:blur(20px);-webkit-backdrop-filter:blur(20px);border-right:0.5px solid var(--wt-border);display:flex;flex-direction:column;padding:0;overflow-y:auto;user-select:none}
.wt-sidebar-header{padding:20px 16px 8px;display:flex;align-items:center;gap:10px}
.wt-sidebar-header h1{font-size:13px;font-weight:700;letter-spacing:-0.01em;color:var(--wt-text-secondary);text-transform:uppercase;margin:0}
.wt-sidebar-section{padding:4px 8px;margin-bottom:4px}
.wt-sidebar-section-title{font-size:11px;font-weight:600;color:var(--wt-text-secondary);text-transform:uppercase;letter-spacing:0.5px;padding:6px 8px 2px;margin:0}
.wt-nav-item{display:flex;align-items:center;gap:8px;padding:7px 12px;margin:1px 0;border-radius:8px;cursor:pointer;font-size:13px;font-weight:400;color:var(--wt-text);text-decoration:none;transition:background 0.15s}
.wt-nav-item:hover{background:rgba(0,0,0,0.04)}
[data-bs-theme="dark"] .wt-nav-item:hover{background:rgba(255,255,255,0.06)}
.wt-nav-item.active{background:var(--wt-accent);color:#fff;font-weight:500}
.wt-nav-item .nav-icon{width:20px;text-align:center;font-size:15px}
.wt-nav-item .nav-badge{margin-left:auto;font-size:11px;font-weight:600;background:var(--wt-accent);color:#fff;border-radius:10px;padding:1px 7px;min-width:20px;text-align:center}
.wt-nav-item.active .nav-badge{background:rgba(255,255,255,0.25)}
.wt-main{flex:1;overflow-y:auto;padding:0}
.wt-content{max-width:960px;margin:0 auto;padding:24px 32px}
.wt-page-title{font-size:28px;font-weight:700;letter-spacing:-0.02em;margin:0 0 20px}
.wt-card{background:var(--wt-card-bg);border-radius:var(--wt-radius);border:0.5px solid var(--wt-border);padding:16px;margin-bottom:12px;transition:box-shadow 0.2s}
.wt-card:hover{box-shadow:0 2px 12px rgba(0,0,0,0.06)}
.wt-card-header{display:flex;align-items:center;justify-content:space-between;margin-bottom:10px}
.wt-card-title{font-size:15px;font-weight:600;margin:0}
.wt-status-dot{width:8px;height:8px;border-radius:50%;display:inline-block;margin-right:6px}
.wt-status-dot.online{background:var(--wt-success);box-shadow:0 0 6px var(--wt-success)}
.wt-status-dot.offline{background:var(--wt-text-secondary)}
.wt-status-dot.running{background:var(--wt-success);animation:pulse 2s infinite}
.wt-status-dot.failed{background:var(--wt-danger)}
@keyframes pulse{0%,100%{opacity:1}50%{opacity:0.4}}
.wt-btn{display:inline-flex;align-items:center;gap:5px;padding:5px 14px;border-radius:7px;border:none;font-size:13px;font-weight:500;cursor:pointer;transition:all 0.15s;font-family:var(--wt-font)}
.wt-btn-primary{background:var(--wt-accent);color:#fff}
.wt-btn-primary:hover{background:#005bb5}
.wt-btn-danger{background:var(--wt-danger);color:#fff}
.wt-btn-danger:hover{background:#d32f2f}
.wt-btn-secondary{background:var(--wt-border);color:var(--wt-text)}
.wt-btn-secondary:hover{background:#c0c0c5}
.wt-btn-sm{padding:3px 10px;font-size:12px}
.wt-input,.wt-textarea{width:100%;padding:8px 12px;border-radius:8px;border:0.5px solid var(--wt-border);background:var(--wt-bg);color:var(--wt-text);font-size:13px;font-family:var(--wt-font);outline:none;transition:border 0.15s}
.wt-input:focus,.wt-textarea:focus{border-color:var(--wt-accent);box-shadow:0 0 0 3px rgba(0,113,227,0.15)}
.wt-textarea{font-family:var(--wt-mono);resize:vertical;min-height:80px}
.wt-log-view{background:#1a1a1a;color:#e0e0e0;font-family:var(--wt-mono);font-size:12px;line-height:1.6;border-radius:var(--wt-radius);padding:12px 16px;max-height:500px;overflow-y:auto;white-space:pre-wrap;word-break:break-all}
.wt-log-entry{padding:2px 0}
.wt-log-entry .log-ts{color:#98989d}
.wt-log-entry .log-svc{color:#64d2ff;font-weight:500}
.wt-log-entry .log-lvl-INFO{color:#30d158}
.wt-log-entry .log-lvl-WARN{color:#ffd60a}
.wt-log-entry .log-lvl-ERROR{color:#ff453a;font-weight:600}
.wt-log-entry .log-lvl-DEBUG{color:#98989d}
.wt-log-entry .log-cid{color:#bf5af2;font-size:11px;font-weight:500;padding:0 4px;border-radius:3px;background:rgba(191,90,242,0.12)}
.wt-table{width:100%;border-collapse:separate;border-spacing:0;font-size:13px}
.wt-table th{font-weight:600;color:var(--wt-text-secondary);font-size:11px;text-transform:uppercase;letter-spacing:0.5px;padding:8px 12px;border-bottom:1px solid var(--wt-border);text-align:left}
.wt-table td{padding:10px 12px;border-bottom:0.5px solid var(--wt-border);vertical-align:middle}
.wt-table tr:last-child td{border-bottom:none}
.wt-badge{display:inline-flex;align-items:center;padding:2px 8px;border-radius:5px;font-size:11px;font-weight:600}
.wt-badge-success{background:rgba(52,199,89,0.12);color:var(--wt-success)}
.wt-badge-danger{background:rgba(255,59,48,0.12);color:var(--wt-danger)}
.wt-badge-secondary{background:rgba(142,142,147,0.12);color:var(--wt-text-secondary)}
.wt-badge-warning{background:rgba(255,159,10,0.12);color:var(--wt-warning)}
.wt-detail-back{font-size:13px;color:var(--wt-accent);cursor:pointer;margin-bottom:12px;display:inline-flex;align-items:center;gap:4px}
.wt-detail-back:hover{text-decoration:underline}
.wt-field{margin-bottom:12px}
.wt-field label{display:block;font-size:12px;font-weight:600;color:var(--wt-text-secondary);margin-bottom:4px}
.wt-theme-dropdown{position:relative;display:inline-block}
.wt-theme-menu{display:none;position:absolute;right:0;top:100%;background:var(--wt-card-bg);border:0.5px solid var(--wt-border);border-radius:8px;box-shadow:0 4px 16px rgba(0,0,0,0.12);min-width:140px;z-index:100;overflow:hidden}
.wt-theme-menu.open{display:block}
.wt-theme-opt{padding:8px 14px;font-size:13px;cursor:pointer;transition:background 0.1s}
.wt-theme-opt:hover{background:rgba(0,0,0,0.04)}
.wt-theme-opt.active{color:var(--wt-accent);font-weight:600}
.wt-status-bar{padding:8px 16px;border-top:0.5px solid var(--wt-border);font-size:11px;color:var(--wt-text-secondary);display:flex;gap:16px;margin-top:auto}
.wt-filter-bar{display:flex;gap:8px;margin-bottom:12px;flex-wrap:wrap;align-items:center}
.wt-select{padding:5px 10px;border-radius:7px;border:0.5px solid var(--wt-border);background:var(--wt-bg);color:var(--wt-text);font-size:12px;font-family:var(--wt-font)}
.wt-toggle{position:relative;width:42px;height:24px;background:var(--wt-border);border-radius:12px;cursor:pointer;transition:background 0.2s}
.wt-toggle.on{background:var(--wt-success)}
.wt-toggle::after{content:"";position:absolute;top:2px;left:2px;width:20px;height:20px;background:#fff;border-radius:50%;transition:transform 0.2s;box-shadow:0 1px 3px rgba(0,0,0,0.2)}
.wt-toggle.on::after{transform:translateX(18px)}
.hidden{display:none !important}
.wt-page{display:none}
.wt-page.active{display:block}
</style></head><body>
<div class="wt-app">
<aside class="wt-sidebar">
<div class="wt-sidebar-header"><h1>WhisperTalk</h1></div>
<div class="wt-sidebar-section">
<p class="wt-sidebar-section-title">Testing</p>
<a class="wt-nav-item active" data-page="tests" onclick="showPage('tests')">
<span class="nav-icon">&#x1F9EA;</span>Tests<span class="nav-badge" id="testsBadge">0</span></a>
<a class="wt-nav-item" data-page="beta-testing" onclick="showPage('beta-testing')">
<span class="nav-icon">&#x1F3AF;</span>Beta Testing</a>
<a class="wt-nav-item" data-page="models" onclick="showPage('models')">
<span class="nav-icon">&#x1F916;</span>Models</a>
</div>
<div class="wt-sidebar-section">
<p class="wt-sidebar-section-title">Services</p>
<a class="wt-nav-item" data-page="services" onclick="showPage('services')">
<span class="nav-icon">&#x2699;</span>Pipeline<span class="nav-badge" id="svcBadge">0/6</span></a>
</div>
<div class="wt-sidebar-section">
<p class="wt-sidebar-section-title">System</p>
<a class="wt-nav-item" data-page="logs" onclick="showPage('logs')">
<span class="nav-icon">&#x1F4CB;</span>Live Logs</a>
<a class="wt-nav-item" data-page="database" onclick="showPage('database')">
<span class="nav-icon">&#x1F5C4;</span>Database</a>
<a class="wt-nav-item" data-page="credentials" onclick="showPage('credentials')">
<span class="nav-icon">&#x1F511;</span>Credentials</a>
</div>
<div class="wt-status-bar" id="statusBar">
<span id="statusText">Connecting...</span>
</div>
<div style="padding:8px 12px;border-top:0.5px solid var(--wt-border)">
<div class="wt-theme-dropdown" style="width:100%">
<div class="wt-nav-item" onclick="toggleThemeMenu()" style="margin:0">
<span class="nav-icon">&#x1F3A8;</span>Theme<span style="margin-left:auto;font-size:11px;color:var(--wt-text-secondary)" id="currentThemeName">)WT" + theme + R"WT(</span>
</div>
<div class="wt-theme-menu" id="themeMenu">
<div class="wt-theme-opt)WT" + std::string(theme == "default" ? " active" : "") + R"WT(" onclick="setTheme('default')">Default</div>
<div class="wt-theme-opt)WT" + std::string(theme == "dark" ? " active" : "") + R"WT(" onclick="setTheme('dark')">Dark</div>
<div class="wt-theme-opt)WT" + std::string(theme == "slate" ? " active" : "") + R"WT(" onclick="setTheme('slate')">Slate</div>
<div class="wt-theme-opt)WT" + std::string(theme == "flatly" ? " active" : "") + R"WT(" onclick="setTheme('flatly')">Flatly</div>
<div class="wt-theme-opt)WT" + std::string(theme == "cyborg" ? " active" : "") + R"WT(" onclick="setTheme('cyborg')">Cyborg</div>
</div></div></div>
</aside>
<main class="wt-main">
)WT";
        h += build_ui_pages();
        h += R"WT(</main></div>
<script src="https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/js/bootstrap.bundle.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/chart.js@4.4.0/dist/chart.umd.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/chartjs-plugin-zoom@2.0.1/dist/chartjs-plugin-zoom.min.js"></script>
<script src="https://cdn.jsdelivr.net/npm/hammerjs@2.0.8/hammer.min.js"></script>
<script>
)WT" + build_ui_js() + R"WT(
</script></body></html>)WT";
        return h;
    }

    std::string build_ui_pages() {
        return R"PG(
<div class="wt-page active" id="page-tests">
<div class="wt-content">
<div id="tests-overview">
<h2 class="wt-page-title">Tests</h2>
<div id="testsContainer"></div>
</div>
<div id="tests-detail" class="hidden">
<div class="wt-detail-back" onclick="showTestsOverview()">&#x2190; All Tests</div>
<h2 class="wt-page-title" id="testDetailName"></h2>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Configuration</span>
<span id="testDetailStatus"></span></div>
<div class="wt-field"><label>Arguments</label>
<input class="wt-input" id="testDetailArgs" placeholder="e.g. --gtest_filter=*MyTest*"></div>
<div style="display:flex;gap:8px">
<button class="wt-btn wt-btn-primary" onclick="startTestDetail()">&#x25B6; Run</button>
<button class="wt-btn wt-btn-danger" id="testStopBtn" onclick="stopTestDetail()" style="display:none">&#x25A0; Stop</button>
</div></div>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Live Output</span>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="clearTestLog()">Clear</button></div>
<div class="wt-log-view" id="testDetailLog">Waiting for output...</div>
</div>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Run History</span></div>
<table class="wt-table"><thead><tr><th>Started</th><th>Duration</th><th>Exit Code</th><th>Arguments</th></tr></thead>
<tbody id="testHistoryBody"></tbody></table>
</div></div></div></div>

<div class="wt-page" id="page-services">
<div class="wt-content">
<div id="services-overview">
<h2 class="wt-page-title">Pipeline Services</h2>
<div id="servicesContainer"></div>
</div>
<div id="services-detail" class="hidden">
<div class="wt-detail-back" onclick="showServicesOverview()">&#x2190; All Services</div>
<h2 class="wt-page-title" id="svcDetailName"></h2>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Configuration</span>
<span id="svcDetailStatus"></span></div>
<div class="wt-field"><label>Binary Path</label>
<div style="font-size:13px;color:var(--wt-text-secondary);font-family:var(--wt-mono)" id="svcDetailPath"></div></div>
<div id="whisperConfig" class="hidden" style="border:1px solid var(--wt-border);border-radius:6px;padding:10px;margin-bottom:8px;background:var(--wt-bg-secondary)">
<div style="font-size:12px;font-weight:600;margin-bottom:6px">Whisper Configuration</div>
<div class="wt-field" style="margin-bottom:6px"><label style="font-size:12px">Language</label>
<select class="wt-select" id="whisperLang" onchange="updateWhisperArgs()" style="font-size:12px"></select></div>
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">Model</label>
<select class="wt-select" id="whisperModel" onchange="updateWhisperArgs()" style="font-size:12px"></select></div>
<div class="wt-field" style="margin-top:8px;margin-bottom:0;display:flex;align-items:center;gap:8px">
<label style="font-size:12px;margin:0;cursor:pointer;display:flex;align-items:center;gap:6px">
<input type="checkbox" id="whisperHallucinationFilter" onchange="toggleHallucinationFilter(this.checked)" style="width:16px;height:16px;cursor:pointer">
Hallucination Filter</label>
<span id="whisperHalluFilterStatus" style="font-size:11px;color:var(--wt-text-secondary)"></span>
</div>
</div>
<div id="sipClientConfig" class="hidden" style="border:1px solid var(--wt-border);border-radius:6px;padding:10px;margin-bottom:8px;background:var(--wt-bg-secondary)">
<div style="font-size:12px;font-weight:600;margin-bottom:6px">PBX Connection</div>
<div style="display:grid;grid-template-columns:1fr 1fr;gap:6px;margin-bottom:8px">
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">Server IP</label>
<input class="wt-input" id="sipPbxServer" placeholder="192.168.1.100" style="font-size:12px"></div>
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">Port</label>
<input class="wt-input" id="sipPbxPort" placeholder="5060" value="5060" style="font-size:12px"></div>
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">Username</label>
<input class="wt-input" id="sipPbxUser" placeholder="extension100" style="font-size:12px"></div>
<div class="wt-field" style="margin-bottom:0"><label style="font-size:12px">Password</label>
<input class="wt-input" id="sipPbxPassword" type="password" placeholder="password" style="font-size:12px"></div>
</div>
<div style="display:flex;gap:6px;align-items:center;margin-bottom:8px">
<button class="wt-btn wt-btn-primary" style="font-size:11px" onclick="sipConnectPbx()">Connect New Line</button>
<span id="sipPbxStatus" style="font-size:11px;color:var(--wt-text-secondary)"></span>
</div>
</div>
<div class="wt-field"><label>Arguments</label>
<input class="wt-input" id="svcDetailArgs" placeholder="Service arguments..."></div>
<div style="display:flex;gap:8px">
<button class="wt-btn wt-btn-primary" id="svcStartBtn" onclick="startSvcDetail()">&#x25B6; Start</button>
<button class="wt-btn wt-btn-danger" id="svcStopBtn" onclick="stopSvcDetail()">&#x25A0; Stop</button>
<button class="wt-btn wt-btn-secondary" id="svcRestartBtn" onclick="restartSvcDetail()">&#x21BB; Restart</button>
<button class="wt-btn wt-btn-secondary" id="svcSaveBtn" onclick="saveSvcConfig()">&#x1F4BE; Save Config</button>
</div></div>
<div id="sipActiveLinesCard" class="wt-card hidden">
<div class="wt-card-header"><span class="wt-card-title">Active Lines</span>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="sipRefreshActiveLines()">Refresh</button></div>
<div id="sipActiveLines" style="padding:8px;font-size:12px;color:var(--wt-text-secondary)">Loading...</div>
</div>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Live Logs</span>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="clearSvcLog()">Clear</button></div>
<div class="wt-log-view" id="svcDetailLog">Waiting for logs...</div>
</div></div></div></div>

<div class="wt-page" id="page-logs">
<div class="wt-content">
<h2 class="wt-page-title">Live Logs</h2>
<div class="wt-filter-bar">
<select class="wt-select" id="logServiceFilter" onchange="reconnectLogSSE()">
<option value="">All Services</option>
<option value="SIP_CLIENT">SIP Client</option>
<option value="INBOUND_AUDIO_PROCESSOR">Inbound Audio</option>
<option value="VAD_SERVICE">VAD</option>
<option value="WHISPER_SERVICE">Whisper ASR</option>
<option value="LLAMA_SERVICE">LLaMA LLM</option>
<option value="KOKORO_SERVICE">Kokoro TTS</option>
<option value="OUTBOUND_AUDIO_PROCESSOR">Outbound Audio</option>
<option value="FRONTEND">Frontend</option>
</select>
<select class="wt-select" id="logLevelFilter" onchange="reconnectLogSSE()">
<option value="">All Levels</option>
<option value="DEBUG">Debug</option>
<option value="INFO">Info</option>
<option value="WARN">Warn</option>
<option value="ERROR">Error</option>
</select>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="clearLiveLogs()">Clear</button>
<label style="font-size:12px;display:flex;align-items:center;gap:6px;margin-left:auto">
<span>Auto-scroll</span>
<div class="wt-toggle on" id="autoScrollToggle" onclick="this.classList.toggle('on')"></div></label>
</div>
<div class="wt-log-view" id="liveLogView" style="max-height:calc(100vh - 200px)"></div>
</div></div>

<div class="wt-page" id="page-database">
<div class="wt-content">
<h2 class="wt-page-title">Database Admin</h2>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">SQL Query</span>
<div style="display:flex;align-items:center;gap:8px">
<label style="font-size:12px;display:flex;align-items:center;gap:6px">
<span>Write Mode</span>
<div class="wt-toggle" id="dbWriteToggle" onclick="toggleDbWrite()"></div></label>
</div></div>
<textarea class="wt-textarea" id="sqlQuery" rows="3">SELECT * FROM logs ORDER BY id DESC LIMIT 50</textarea>
<div style="display:flex;gap:8px;margin-top:8px;flex-wrap:wrap">
<button class="wt-btn wt-btn-primary" onclick="runQuery()">&#x25B6; Execute</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="document.getElementById('sqlQuery').value='SELECT * FROM logs ORDER BY id DESC LIMIT 50'">Recent Logs</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="document.getElementById('sqlQuery').value='SELECT * FROM test_runs ORDER BY start_time DESC LIMIT 20'">Test History</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="document.getElementById('sqlQuery').value='SELECT service, COUNT(*) as count FROM logs GROUP BY service'">Log Stats</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="loadSchema()">Show Schema</button>
</div></div>
<div id="queryResults"></div>
<div id="schemaView" class="hidden"></div>
</div></div>

<div class="wt-page" id="page-credentials">
<div class="wt-content">
<h2 class="wt-page-title">Credentials</h2>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">HuggingFace</span></div>
<div class="wt-field">
<label>Access Token</label>
<div style="display:flex;gap:8px">
<input type="password" class="wt-input" id="credHfToken" placeholder="hf_..." style="flex:1" autocomplete="new-password">
<button class="wt-btn wt-btn-primary" onclick="saveCredential('hf_token','credHfToken','credHfStatus','credHfClear')">Save</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" id="credHfClear" style="display:none" onclick="clearCredential('hf_token','credHfToken','credHfStatus','credHfClear','hf_...')">Clear</button>
</div>
<div id="credHfStatus" style="font-size:12px;margin-top:4px"></div>
</div>
</div>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">GitHub</span></div>
<div class="wt-field">
<label>Access Token</label>
<div style="display:flex;gap:8px">
<input type="password" class="wt-input" id="credGhToken" placeholder="ghp_..." style="flex:1" autocomplete="new-password">
<button class="wt-btn wt-btn-primary" onclick="saveCredential('github_token','credGhToken','credGhStatus','credGhClear')">Save</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" id="credGhClear" style="display:none" onclick="clearCredential('github_token','credGhToken','credGhStatus','credGhClear','ghp_...')">Clear</button>
</div>
<div id="credGhStatus" style="font-size:12px;margin-top:4px"></div>
</div>
</div>
</div></div>

<div class="wt-page" id="page-beta-testing">
<div class="wt-content">
<h2 class="wt-page-title">Beta Testing & Optimization</h2>

<div class="wt-card">
<div class="wt-card-header">
<span class="wt-card-title">Test Audio Files</span>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="refreshTestFiles()">&#x21BB; Refresh</button>
</div>
<div id="testFilesContainer">Loading test files...</div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Audio Injection</span></div>
<div class="wt-field">
<label>Select Test File</label>
<select class="wt-select" id="injectFileSelect" style="width:100%;padding:8px">
<option value="">-- Select a test file --</option>
</select>
</div>
<div class="wt-field">
<label>Inject into Leg (username)</label>
<select class="wt-select" id="injectLeg" style="width:100%;padding:8px">
<option value="a">Leg A (first)</option>
<option value="b">Leg B (second)</option>
</select>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="refreshInjectLegs()" style="margin-top:4px">&#x21BB; Refresh Legs</button>
</div>
<div style="display:flex;gap:8px">
<button class="wt-btn wt-btn-primary" onclick="injectAudio()">&#x25B6; Inject Audio</button>
</div>
<div id="injectionStatus" style="margin-top:12px;font-size:13px"></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Test 1: SIP Client RTP Routing</span></div>
<p style="font-size:13px;color:var(--wt-text-secondary);margin-bottom:12px">Test SIP Client RTP packet routing and TCP connection handling with IAP service.</p>
<div style="display:flex;gap:8px;margin-bottom:12px">
<button class="wt-btn wt-btn-primary" onclick="startSipRtpTest()">&#x25B6; Start Test</button>
<button class="wt-btn wt-btn-danger" onclick="stopSipRtpTest()">&#x25A0; Stop Test</button>
<button class="wt-btn wt-btn-secondary" onclick="refreshSipStats()">&#x21BB; Refresh Stats</button>
</div>
<div id="sipRtpTestStatus" style="margin-bottom:12px;font-size:13px"></div>
<div id="sipRtpMetrics">
<h4 style="font-size:14px;font-weight:600;margin:12px 0 8px">RTP Packet Metrics</h4>
<table class="wt-table" style="width:100%">
<thead>
<tr>
<th>Call ID</th>
<th>Line</th>
<th>RX Packets</th>
<th>TX Packets</th>
<th>Forwarded</th>
<th>Discarded</th>
<th>Duration</th>
</tr>
</thead>
<tbody id="sipRtpStatsBody">
<tr><td colspan="7" style="text-align:center;color:var(--wt-text-secondary)">No active calls. Start SIP Client and inject audio to begin test.</td></tr>
</tbody>
</table>
<div style="margin-top:12px;font-size:13px">
<div><strong>TCP Connection Status:</strong> <span id="iapConnectionStatus">Unknown</span></div>
<div style="margin-top:4px"><strong>Test Instructions:</strong></div>
<ol style="margin:8px 0;padding-left:20px;font-size:12px;color:var(--wt-text-secondary)">
<li>Start SIP Client (without IAP) → Inject audio → Verify RTP packets received but discarded</li>
<li>Start IAP service → Verify TCP connection established</li>
<li>Re-inject audio → Verify packets forwarded to IAP</li>
<li>Stop/Start IAP multiple times to test reconnection handling</li>
</ol>
</div>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Test 2: IAP Codec Quality</span></div>
<p style="font-size:13px;color:var(--wt-text-secondary);margin-bottom:12px"><strong>Codec algorithm test</strong> (does not require IAP service). Runs the exact G.711 mu-law encode/decode + 15-tap FIR half-band 8kHz&#x2192;16kHz upsample pipeline offline, measuring SNR and RMS Error per-packet. Service connectivity is tested in Test 1 above.</p>
<div class="wt-field">
<label>Select Test File</label>
<select class="wt-select" id="iapTestFileSelect" style="width:100%;padding:8px">
<option value="">-- Select a test file --</option>
</select>
</div>
<div style="display:flex;gap:8px;margin-bottom:12px">
<button class="wt-btn wt-btn-primary" onclick="runIapQualityTest()">&#x25B6; Run Quality Test</button>
<button class="wt-btn wt-btn-success" onclick="runAllIapQualityTests()">&#x25B6; Run All Files</button>
</div>
<div id="iapTestStatus" style="margin-bottom:12px;font-size:13px"></div>
<div id="iapTestResults">
<h4 style="font-size:14px;font-weight:600;margin:12px 0 8px">Latest Test Results</h4>
<table class="wt-table" style="width:100%">
<thead>
<tr>
<th>File</th>
<th>Avg Pkt Latency (ms)</th>
<th>Max Pkt Latency (ms)</th>
<th>SNR (dB)</th>
<th>RMS Error (%)</th>
<th>Status</th>
<th>Timestamp</th>
</tr>
</thead>
<tbody id="iapResultsBody">
<tr><td colspan="7" style="text-align:center;color:var(--wt-text-secondary)">No test results yet. Run a test to see results here.</td></tr>
</tbody>
</table>
<div style="margin-top:12px;font-size:12px;color:var(--wt-text-secondary)">
<strong>Pass Criteria:</strong> SNR &#x2265; 25dB, RMS Error &#x2264; 10%, Per-Packet Latency &#x2264; 50ms. Uses shared IAP pipeline: G.711 &#x3BC;-law encode/decode + 15-tap FIR half-band upsample (from interconnect.h).
</div>
</div>
<div id="iapTestChart" style="margin-top:16px;display:none">
<canvas id="iapMetricsChart" style="max-height:250px"></canvas>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header">
<span class="wt-card-title">SIP Lines Management</span>
<div style="display:flex;gap:8px">
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="refreshSipPanel()">&#x21BB; Refresh</button>
</div>
</div>
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:12px">
  Enable lines (check-field 1) to register them with the SIP provider. Connect lines (check-field 2) to start a conference call between selected lines. Up to 20 lines supported.
</p>
<div style="display:flex;gap:8px;margin-bottom:12px;flex-wrap:wrap">
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="enableLinesPreset(1)">1 Line</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="enableLinesPreset(2)">2 Lines</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="enableLinesPreset(4)">4 Lines</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="enableLinesPreset(6)">6 Lines</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="enableLinesPreset(10)">10 Lines</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="enableLinesPreset(20)">20 Lines</button>
</div>
<div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(160px,1fr));gap:6px;margin-bottom:12px" id="sipLinesGrid"></div>
<div style="display:flex;gap:8px;margin-bottom:12px;flex-wrap:wrap">
<button class="wt-btn wt-btn-primary" onclick="applyEnabledLines()">&#x2705; Apply Enabled</button>
<button class="wt-btn wt-btn-success" onclick="startConference()">&#x260E; Start Conference</button>
<button class="wt-btn wt-btn-danger" onclick="hangupConference()">&#x1F4F5; Hangup</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="selectAllConnect()">Connect All</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="deselectAllConnect()">Disconnect All</button>
</div>
<div id="sipLinesStatus" style="margin-top:8px;font-size:13px"></div>
<div id="sipProviderUsers" style="margin-top:12px;font-size:12px;color:var(--wt-text-secondary)"></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Whisper Accuracy Test</span></div>
<div class="wt-field">
<label>Test Files (hold Ctrl/Cmd to select multiple)</label>
<select class="wt-select" id="accuracyTestFiles" multiple style="width:100%;padding:8px;height:120px">
</select>
</div>
<div class="wt-field">
<label>Model</label>
<input class="wt-input" id="accuracyModel" value="current" readonly>
</div>
<div style="padding:10px;background:var(--wt-card-hover);border-radius:6px;margin-top:8px;border-left:3px solid var(--wt-primary)">
<div style="font-size:12px;font-weight:600;color:var(--wt-text-secondary);margin-bottom:6px">&#x2699; VAD Service Settings (applied on next VAD restart)</div>
<div style="display:grid;grid-template-columns:1fr 1fr 1fr 1fr;gap:8px;font-size:12px">
<div><strong>Window:</strong> <span id="currentVadWindow" style="color:var(--wt-primary)">50</span> ms</div>
<div><strong>Threshold:</strong> <span id="currentVadThreshold" style="color:var(--wt-primary)">2.0</span></div>
<div><strong>Silence:</strong> <span id="currentVadSilence" style="color:var(--wt-primary)">400</span> ms</div>
<div><strong>Max Chunk:</strong> <span id="currentVadMaxChunk" style="color:var(--wt-primary)">8000</span> ms</div>
</div>
</div>
<div style="display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:8px">
<div class="wt-field">
<label>VAD Window (ms): <span id="vadWindowValue">50</span></label>
<input type="range" id="vadWindowSlider" min="10" max="200" value="50" step="10" style="width:100%" oninput="updateVadWindowDisplay(this.value)">
<div style="display:flex;justify-content:space-between;font-size:11px;color:var(--wt-text-secondary);margin-top:2px">
<span>10ms</span><span>200ms</span>
</div>
</div>
<div class="wt-field">
<label>VAD Threshold: <span id="vadThresholdValue">2.0</span></label>
<input type="range" id="vadThresholdSlider" min="1.0" max="4.0" value="2.0" step="0.1" style="width:100%" oninput="updateVadThresholdDisplay(this.value)">
<div style="display:flex;justify-content:space-between;font-size:11px;color:var(--wt-text-secondary);margin-top:2px">
<span>1.0</span><span>4.0</span>
</div>
</div>
<div class="wt-field">
<label>VAD Silence (ms): <span id="vadSilenceValue">400</span></label>
<input type="range" id="vadSilenceSlider" min="100" max="1500" value="400" step="50" style="width:100%" oninput="document.getElementById('vadSilenceValue').textContent=this.value">
<div style="display:flex;justify-content:space-between;font-size:11px;color:var(--wt-text-secondary);margin-top:2px">
<span>100ms</span><span>1500ms</span>
</div>
</div>
<div class="wt-field">
<label>Max Chunk (ms): <span id="vadMaxChunkValue">8000</span></label>
<input type="range" id="vadMaxChunkSlider" min="1000" max="10000" value="8000" step="500" style="width:100%" oninput="document.getElementById('vadMaxChunkValue').textContent=this.value">
<div style="display:flex;justify-content:space-between;font-size:11px;color:var(--wt-text-secondary);margin-top:2px">
<span>1000ms</span><span>10000ms</span>
</div>
</div>
</div>
<div style="display:flex;gap:8px;margin-top:8px">
<button class="wt-btn wt-btn-primary" onclick="runWhisperAccuracyTest()">&#x25B6; Run Accuracy Test</button>
<button class="wt-btn wt-btn-secondary" onclick="loadVadConfig()">&#x21BB; Load VAD</button>
<button class="wt-btn wt-btn-secondary" onclick="saveVadConfig()">&#x1F4BE; Save VAD</button>
</div>
<div id="accuracySummary" style="margin-top:12px;padding:12px;background:var(--wt-card-bg);border:1px solid var(--wt-border);border-radius:8px;display:none">
<h4 style="margin:0 0 8px 0;font-size:13px;font-weight:600">Test Summary</h4>
<div style="display:grid;grid-template-columns:repeat(auto-fit,minmax(100px,1fr));gap:8px;font-size:12px">
<div><strong>Total:</strong> <span id="summaryTotal">0</span></div>
<div><strong>PASS:</strong> <span id="summaryPass" style="color:var(--wt-success)">0</span></div>
<div><strong>WARN:</strong> <span id="summaryWarn" style="color:var(--wt-warning)">0</span></div>
<div><strong>FAIL:</strong> <span id="summaryFail" style="color:var(--wt-danger)">0</span></div>
<div><strong>Avg Accuracy:</strong> <span id="summaryAccuracy">0.0</span>%</div>
<div><strong>Avg Latency:</strong> <span id="summaryLatency">0</span>ms</div>
</div>
</div>
<div id="accuracyResults" style="margin-top:12px"></div>
<canvas id="accuracyTrendChart" style="margin-top:12px;display:none;max-height:200px"></canvas>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Test 4: LLaMA Response Quality</span></div>
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:10px">
  Send test prompts directly to LLaMA service and evaluate response quality.
  Requires: LLaMA service running. Does not require full pipeline.
</p>
<div class="wt-field">
<label>Test Prompts</label>
<select class="wt-select" id="llamaTestPrompts" multiple style="width:100%;padding:8px;height:100px">
</select>
</div>
<div class="wt-field">
<label>Custom Prompt (optional)</label>
<input class="wt-input" id="llamaCustomPrompt" placeholder="Type a custom German prompt...">
</div>
<div style="display:flex;gap:8px;margin-top:8px">
<button class="wt-btn wt-btn-primary" onclick="runLlamaQualityTest()">&#x25B6; Run Quality Test</button>
<button class="wt-btn wt-btn-secondary" onclick="runLlamaShutupTest()">&#x1F910; Shut-up Test</button>
</div>
<div id="llamaTestStatus" style="margin-top:8px;font-size:12px"></div>
<div id="llamaTestResults" style="margin-top:12px"></div>
<div id="llamaShutupResult" style="margin-top:8px"></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Test 4b: Shut-Up Mechanism (Pipeline)</span></div>
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:10px">
  Tests the LLaMA shut-up (interrupt / barge-in) mechanism via command port with configurable delays.
  Measures generation-interrupt latency across scenarios: immediate, standard (200ms), late (1s), and rapid successive.
  Also checks Kokoro and OAP status for signal propagation readiness.
  Requires: LLaMA service running. Kokoro + OAP optional (status-checked only).
</p>
<div class="wt-field">
<label>Scenarios</label>
<select class="wt-select" id="shutupScenarios" multiple style="width:100%;padding:8px;height:80px">
<option value="basic" selected>Basic: 200ms delay, interrupt mid-generation</option>
<option value="early" selected>Early: 0ms delay, interrupt immediately</option>
<option value="late" selected>Late: 1000ms delay, interrupt near end</option>
<option value="rapid" selected>Rapid: 3 successive interrupts (100ms delay each)</option>
</select>
</div>
<div style="display:flex;gap:8px;margin-top:8px">
<button class="wt-btn wt-btn-primary" onclick="runShutupPipelineTest()">&#x25B6; Run Pipeline Shut-Up Test</button>
</div>
<div id="shutupPipelineStatus" style="margin-top:8px;font-size:12px"></div>
<div id="shutupPipelineResults" style="margin-top:12px"></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Test 5: Kokoro TTS Quality</span></div>
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:10px">
  Synthesize German phrases via Kokoro TTS and measure latency, RTF, audio quality.
  Requires: Kokoro service running. Does not require full pipeline.
</p>
<div class="wt-field">
<label>Custom Phrase (optional)</label>
<input class="wt-input" id="kokoroCustomPhrase" placeholder="Type a German phrase to synthesize...">
</div>
<div style="display:flex;gap:8px;margin-top:8px">
<button class="wt-btn wt-btn-primary" onclick="runKokoroQualityTest()">&#x25B6; Run Quality Test</button>
<button class="wt-btn wt-btn-secondary" onclick="runKokoroBenchmark()">&#x23F1; Benchmark</button>
<select class="wt-select" id="kokoroBenchIter" style="width:80px">
<option value="3">3 iter</option>
<option value="5" selected>5 iter</option>
<option value="10">10 iter</option>
</select>
</div>
<div id="kokoroTestStatus" style="margin-top:8px;font-size:12px"></div>
<div id="kokoroTestResults" style="margin-top:12px"></div>
<div id="kokoroBenchResult" style="margin-top:8px"></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Test 6: Full Pipeline Round-Trip</span></div>
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:10px">
  Full pipeline loop: Phrase &#x2192; Kokoro WAV &#x2192; inject &#x2192; SIP(L1) &#x2192; IAP &#x2192; VAD &#x2192; Whisper &#x2192; LLaMA &#x2192; Kokoro &#x2192; OAP &#x2192; SIP &#x2192; relay &#x2192; SIP(L2) &#x2192; IAP &#x2192; VAD &#x2192; Whisper.
  Verifies transcription of injected phrase (Line 1) and LLaMA response (Line 2).
  Requires: All services running + active call on test_sip_provider.
</p>
<div class="wt-field">
<label>Custom Phrases (optional, comma-separated)</label>
<input class="wt-input" id="ttsRoundtripPhrases" placeholder="e.g. Hallo Welt, Guten Morgen">
</div>
<div style="display:flex;gap:8px;margin-top:8px">
<button class="wt-btn wt-btn-primary" id="ttsRoundtripBtn" onclick="runTtsRoundtrip()">&#x25B6; Run Round-Trip Test</button>
</div>
<div id="ttsRoundtripStatus" style="margin-top:8px;font-size:12px"></div>
<div id="ttsRoundtripResults" style="margin-top:12px"></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Test 6b: Full Loop File Test (WER)</span></div>
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:10px">
  Injects test audio files through full pipeline with 2 SIP lines. Measures Word Error Rate (WER)
  between LLaMA response text and Whisper Line 2 re-transcription of Kokoro/OAP output.
  Flow: TestFile &#x2192; SIP(L1) &#x2192; IAP &#x2192; VAD &#x2192; Whisper &#x2192; LLaMA &#x2192; Kokoro &#x2192; OAP &#x2192; SIP(L2) &#x2192; Whisper(L2).
  Requires: All services + 2 lines + active conference call.
</p>
<div class="wt-field">
<label>Select Test Files (hold Ctrl/Cmd for multiple)</label>
<select class="wt-select" id="fullLoopFiles" multiple style="width:100%;padding:8px;height:100px">
</select>
</div>
<div style="display:flex;gap:8px;margin-top:8px">
<button class="wt-btn wt-btn-primary" id="fullLoopBtn" onclick="runFullLoopTest()">&#x25B6; Run Full Loop Test</button>
</div>
<div id="fullLoopStatus" style="margin-top:8px;font-size:12px"></div>
<div id="fullLoopResults" style="margin-top:12px"></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Test 7: Pipeline Resilience Health Check</span></div>
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:10px">
  Pings all 7 pipeline services via their command ports and reports interconnect status.
  Use this to verify all services are running and interconnected correctly.
</p>
<div style="display:flex;gap:8px;margin-bottom:8px">
<button class="wt-btn wt-btn-primary" onclick="checkPipelineHealth(false)">&#x25B6; Check Now</button>
<button class="wt-btn wt-btn-secondary" id="pipelineHealthAutoBtn" onclick="startPipelineHealthAutoRefresh()">Auto-Refresh (10s)</button>
</div>
<div id="pipelineHealthStatus" style="font-size:12px;margin-bottom:8px"></div>
<div id="pipelineHealthResults"></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Test 8: Multi-Line Command Stress Test</span></div>
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:10px">
  Floods all 7 pipeline service command ports concurrently with PING requests from multiple simulated lines.
  Measures response success rate and latency under concurrent load.
</p>
<div style="display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin-bottom:10px">
  <label style="font-size:13px">Lines: <input id="stressLines" type="number" min="1" max="32" value="4" style="width:60px;margin-left:4px" class="wt-input"></label>
  <label style="font-size:13px">Duration (s): <input id="stressDuration" type="number" min="1" max="120" value="10" style="width:60px;margin-left:4px" class="wt-input"></label>
  <button class="wt-btn wt-btn-primary" id="stressRunBtn" onclick="runMultilineStress()">&#x25B6; Run Stress Test</button>
</div>
<div id="stressStatus" style="font-size:12px;margin-bottom:8px"></div>
<div id="stressResults"></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Test 9: Full Pipeline Stress Test</span></div>
<p style="font-size:12px;color:var(--wt-text-secondary);margin-bottom:10px">
  Continuously injects test audio through the full pipeline for a configurable duration.
  Measures end-to-end latency, per-service memory, health, and throughput under sustained load.
  Requires: All 7 services running + test_sip_provider with active call.
</p>
<div style="display:flex;gap:12px;align-items:center;flex-wrap:wrap;margin-bottom:10px">
  <label style="font-size:13px">Duration:
    <select class="wt-select" id="pstressDuration" style="width:100px;margin-left:4px">
      <option value="30">30s</option>
      <option value="60">60s</option>
      <option value="120" selected>2 min</option>
      <option value="300">5 min</option>
    </select>
  </label>
  <button class="wt-btn wt-btn-primary" id="pstressRunBtn" onclick="runPipelineStressTest()">&#x25B6; Start Stress Test</button>
  <button class="wt-btn wt-btn-danger" id="pstressStopBtn" onclick="stopPipelineStressTest()" style="display:none">&#x25A0; Stop</button>
</div>
<div id="pstressProgress" style="display:none;margin-bottom:10px">
  <div style="display:flex;align-items:center;gap:8px;margin-bottom:4px">
    <span id="pstressElapsed" style="font-size:13px;font-weight:600">0s / 120s</span>
    <span id="pstressCycles" style="font-size:12px;color:var(--wt-text-secondary)">0 cycles</span>
  </div>
  <div style="height:8px;background:var(--wt-border);border-radius:4px;overflow:hidden">
    <div id="pstressBar" style="height:100%;width:0%;background:var(--wt-accent);transition:width 0.5s"></div>
  </div>
</div>
<div id="pstressStatus" style="font-size:12px;margin-bottom:8px"></div>
<div id="pstressMetrics" style="display:none;margin-bottom:10px">
  <h4 style="font-size:14px;font-weight:600;margin:8px 0">Service Health &amp; Memory</h4>
  <table class="wt-table" style="width:100%">
    <thead><tr><th>Service</th><th>Status</th><th>Ping OK</th><th>Ping Fail</th><th>Avg Ping</th><th>Memory (MB)</th></tr></thead>
    <tbody id="pstressSvcBody"></tbody>
  </table>
  <h4 style="font-size:14px;font-weight:600;margin:12px 0 8px">Pipeline Throughput</h4>
  <div id="pstressThroughput" style="font-size:13px"></div>
</div>
<div id="pstressResults"></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Test SIP Provider Status</span></div>
<div id="sipProviderStatus">
<p style="font-size:13px;color:var(--wt-text-secondary)">Check if test_sip_provider is running...</p>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="checkSipProvider()">&#x21BB; Check Status</button>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Service Log Levels</span></div>
<p style="font-size:13px;color:var(--wt-text-secondary);margin-bottom:12px">Configure logging verbosity for each service (changes apply immediately)</p>
<div id="logLevelControls"></div>
<div style="margin-top:12px">
<button class="wt-btn wt-btn-sm wt-btn-primary" onclick="saveAllLogLevels()">&#x1F4BE; Save All</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="loadLogLevels()">&#x21BB; Reload</button>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header">
<span class="wt-card-title">Test Results</span>
<div style="display:flex;gap:8px">
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="refreshTestResults()">&#x21BB; Refresh</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="exportTestResults()">&#x1F4E5; Export JSON</button>
</div>
</div>
<div class="wt-filter-bar">
<select class="wt-select" id="testResultsService" onchange="filterTestResults()">
<option value="">All Services</option>
<option value="whisper">Whisper</option>
<option value="llama">LLaMA</option>
<option value="kokoro">Kokoro</option>
</select>
<select class="wt-select" id="testResultsStatus" onchange="filterTestResults()">
<option value="">All Status</option>
<option value="pass">Pass</option>
<option value="fail">Fail</option>
</select>
</div>
<div id="testResultsContainer">No test results yet. Run some tests to see results here.</div>
<div id="testResultsChart" style="margin-top:16px;display:none">
<canvas id="metricsChart" style="max-height:300px"></canvas>
</div>
</div>

</div></div>

<!-- ===== MODELS PAGE ===== -->
<div class="wt-page" id="page-models">
<div class="wt-content">
<h2 class="wt-page-title">Models & Benchmarking</h2>

<!-- Tab selector -->
<div style="display:flex;gap:8px;margin-bottom:16px">
  <button class="wt-btn wt-btn-primary" id="tabWhisper" onclick="switchModelTab('whisper')">Whisper Models</button>
  <button class="wt-btn wt-btn-secondary" id="tabLlama" onclick="switchModelTab('llama')">LLaMA Models</button>
  <button class="wt-btn wt-btn-secondary" id="tabCompare" onclick="switchModelTab('compare')">Comparison</button>
</div>

<!-- Whisper Models Panel -->
<div id="modelTabWhisper">

<div class="wt-card">
<div class="wt-card-header">
<span class="wt-card-title">Search HuggingFace Models</span>
</div>
<div style="display:grid;grid-template-columns:2fr 1fr 1fr auto;gap:8px;margin-bottom:8px;align-items:end">
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Search Query</label>
    <input class="wt-input" id="hfSearchQuery" placeholder="e.g. whisper german coreml ggml" value="whisper german">
  </div>
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Task</label>
    <select class="wt-select" id="hfSearchTask">
      <option value="automatic-speech-recognition">ASR (Speech-to-Text)</option>
      <option value="text-generation">Text Generation</option>
      <option value="">Any task</option>
    </select>
  </div>
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Sort by</label>
    <select class="wt-select" id="hfSearchSort">
      <option value="downloads">Downloads</option>
      <option value="likes">Likes</option>
      <option value="lastModified">Recently Updated</option>
    </select>
  </div>
  <button class="wt-btn wt-btn-primary" onclick="searchHuggingFace()" id="hfSearchBtn">&#x1F50D; Search</button>
</div>
<div id="hfSearchStatus" style="font-size:12px;margin-bottom:8px"></div>
<div id="hfSearchResults"></div>
</div>

<div class="wt-card">
<div class="wt-card-header">
<span class="wt-card-title">Registered Whisper Models</span>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="loadModels()">&#x21BB; Refresh</button>
</div>
<div id="whisperModelsTable"><em>Loading...</em></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Add Whisper Model Manually</span></div>
<div style="display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin-bottom:8px">
  <input class="wt-input" id="addModelName" placeholder="Name (e.g. large-v3-turbo-q5)">
  <input class="wt-input" id="addModelPath" placeholder="Full path to .bin file">
  <select class="wt-select" id="addModelBackend">
    <option value="coreml">CoreML (Apple Silicon)</option>
    <option value="metal">Metal GPU</option>
    <option value="cpu">CPU only</option>
  </select>
</div>
<button class="wt-btn wt-btn-primary" onclick="addWhisperModel()">+ Register Model</button>
<div id="addModelStatus" style="margin-top:8px;font-size:12px"></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Run Benchmark</span></div>
<div style="display:grid;grid-template-columns:1fr 1fr auto;gap:8px;align-items:end;margin-bottom:8px">
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Model</label>
    <select class="wt-select" id="benchmarkModelId"><option value="">-- select model --</option></select>
  </div>
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Iterations (per file)</label>
    <select class="wt-select" id="benchmarkIterations">
      <option value="1">1 pass</option>
      <option value="2">2 passes</option>
      <option value="3">3 passes</option>
    </select>
  </div>
  <button class="wt-btn wt-btn-primary" onclick="runBenchmark()" id="benchmarkRunBtn">&#x25B6; Run Benchmark</button>
</div>
<div style="font-size:12px;color:var(--wt-text-muted);margin-bottom:8px">
  Prerequisites: SIP Client, IAP, VAD, Whisper must be running with an active call via test_sip_provider.
  All Testfiles with ground truth will be used.
</div>
<div id="benchmarkStatus"></div>
<div id="benchmarkResults" style="margin-top:12px"></div>
</div>

</div><!-- end modelTabWhisper -->

<!-- LLaMA Models Panel -->
<div id="modelTabLlama" style="display:none">

<div class="wt-card">
<div class="wt-card-header">
<span class="wt-card-title">Search HuggingFace LLaMA Models</span>
</div>
<div style="display:grid;grid-template-columns:2fr 1fr auto;gap:8px;margin-bottom:8px;align-items:end">
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Search Query</label>
    <input class="wt-input" id="hfLlamaSearchQuery" placeholder="e.g. llama german gguf small" value="llama german gguf">
  </div>
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Sort by</label>
    <select class="wt-select" id="hfLlamaSearchSort">
      <option value="downloads">Downloads</option>
      <option value="likes">Likes</option>
      <option value="lastModified">Recently Updated</option>
    </select>
  </div>
  <button class="wt-btn wt-btn-primary" onclick="searchHuggingFaceLlama()" id="hfLlamaSearchBtn">&#x1F50D; Search</button>
</div>
<div id="hfLlamaSearchStatus" style="font-size:12px;margin-bottom:8px"></div>
<div id="hfLlamaSearchResults"></div>
</div>

<div class="wt-card">
<div class="wt-card-header">
<span class="wt-card-title">Registered LLaMA Models</span>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="loadModels()">&#x21BB; Refresh</button>
</div>
<div id="llamaModelsTable"><em>Loading...</em></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Add LLaMA Model</span></div>
<div style="display:grid;grid-template-columns:1fr 1fr 1fr;gap:8px;margin-bottom:8px">
  <input class="wt-input" id="addLlamaModelName" placeholder="Name (e.g. Llama-3.2-1B-Q8)">
  <input class="wt-input" id="addLlamaModelPath" placeholder="Full path to .gguf file">
  <select class="wt-select" id="addLlamaModelBackend">
    <option value="metal">Metal GPU</option>
    <option value="cpu">CPU only</option>
  </select>
</div>
<button class="wt-btn wt-btn-primary" onclick="addLlamaModel()">+ Register Model</button>
<div id="addLlamaModelStatus" style="margin-top:8px;font-size:12px"></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Run LLaMA Benchmark</span></div>
<div style="display:grid;grid-template-columns:1fr 1fr auto;gap:8px;align-items:end;margin-bottom:8px">
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Model</label>
    <select class="wt-select" id="llamaBenchmarkModelId"><option value="">-- select model --</option></select>
  </div>
  <div>
    <label style="font-size:12px;font-weight:600;display:block;margin-bottom:4px">Iterations (per prompt)</label>
    <select class="wt-select" id="llamaBenchmarkIterations">
      <option value="1">1 pass</option>
      <option value="2">2 passes</option>
      <option value="3">3 passes</option>
    </select>
  </div>
  <button class="wt-btn wt-btn-primary" onclick="runLlamaBenchmark()" id="llamaBenchmarkRunBtn">&#x25B6; Run Benchmark</button>
</div>
<div style="font-size:12px;color:var(--wt-text-muted);margin-bottom:8px">
  Sends all test prompts to LLaMA service and measures response quality, latency, and German compliance.
  Requires: LLaMA service running.
</div>
<div id="llamaBenchmarkStatus"></div>
<div id="llamaBenchmarkResults" style="margin-top:12px"></div>
</div>

</div><!-- end modelTabLlama -->

<!-- Comparison Panel -->
<div id="modelTabCompare" style="display:none">
<div class="wt-card">
<div class="wt-card-header">
<span class="wt-card-title">Model Benchmark Comparison</span>
<div style="display:flex;gap:8px;align-items:center">
  <select class="wt-select" id="compFilterType" style="width:auto;font-size:12px" onchange="loadModelComparison()">
    <option value="">All Types</option>
    <option value="whisper">Whisper Only</option>
    <option value="llama">LLaMA Only</option>
  </select>
  <button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="loadModelComparison()">&#x21BB; Refresh</button>
</div>
</div>
<div id="comparisonTable"><em>No benchmark runs yet. Run benchmarks on models to compare them.</em></div>
</div>

<div style="display:grid;grid-template-columns:1fr 1fr;gap:16px">
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Score Comparison</span></div>
<canvas id="compAccuracyChart" style="max-height:280px"></canvas>
</div>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Latency Comparison</span></div>
<canvas id="compLatencyChart" style="max-height:280px"></canvas>
</div>
</div>

<div style="display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-top:16px">
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Model Size (MB)</span></div>
<canvas id="compSizeChart" style="max-height:280px"></canvas>
</div>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Latency vs Accuracy Scatter</span></div>
<canvas id="compScatterChart" style="max-height:280px"></canvas>
</div>
</div>

<div id="compLlamaCharts" style="display:none">
<div style="display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-top:16px">
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">German Compliance (%)</span></div>
<canvas id="compGermanChart" style="max-height:280px"></canvas>
</div>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Interrupt Latency (ms)</span></div>
<canvas id="compInterruptChart" style="max-height:280px"></canvas>
</div>
</div>
<div style="display:grid;grid-template-columns:1fr 1fr;gap:16px;margin-top:16px">
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Avg Words per Response</span></div>
<canvas id="compTokensChart" style="max-height:280px"></canvas>
</div>
<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Quality Score vs Latency</span></div>
<canvas id="compQualityScatterChart" style="max-height:280px"></canvas>
</div>
</div>
</div>

</div><!-- end modelTabCompare -->

</div></div><!-- end page-models -->

)PG";
    }

    std::string build_ui_js() {
        std::string port_str = std::to_string(http_port_);
        std::string tsp_port_str = std::to_string(TEST_SIP_PROVIDER_PORT);
        std::string js = R"JS(
var currentPage='tests',currentTest=null,currentSvc=null;
var logSSE=null,svcLogSSE=null,testLogPoll=null;
var TSP_PORT=)JS" + tsp_port_str + R"JS(;

function showPage(p){
  document.querySelectorAll('.wt-page').forEach(e=>e.classList.remove('active'));
  document.getElementById('page-'+p).classList.add('active');
  document.querySelectorAll('.wt-nav-item').forEach(e=>{
    e.classList.toggle('active',e.dataset.page===p);
  });
  currentPage=p;
  if(p==='tests'){showTestsOverview();fetchTests();}
  if(p==='services'){showServicesOverview();fetchServices();}
  if(p==='beta-testing'){buildSipLinesGrid();refreshTestFiles();loadVadConfig();loadLlamaPrompts();}
  if(p==='models'){loadModels();loadModelComparison();}
  if(p==='logs'){reconnectLogSSE();}
  if(p==='database'){}
  if(p==='credentials'){loadCredentials();}
}

function fetchStatus(){
  fetch('/api/status').then(r=>r.json()).then(d=>{
    document.getElementById('statusText').textContent=
      d.services_online+' services \u2022 '+d.running_tests+' tests \u2022 '+d.sse_connections+' SSE';
    document.getElementById('svcBadge').textContent=d.services_online+'/6';
  }).catch(()=>{document.getElementById('statusText').textContent='Disconnected';});
}

function fetchTests(){
  fetch('/api/tests').then(r=>r.json()).then(d=>{
    var running=d.tests.filter(t=>t.is_running).length;
    document.getElementById('testsBadge').textContent=d.tests.length;
    var c=document.getElementById('testsContainer');
    c.innerHTML=d.tests.map(t=>{
      var status=t.is_running?'<span class="wt-badge wt-badge-success"><span class="wt-status-dot running"></span>Running</span>'
        :(t.exit_code===0&&t.end_time?'<span class="wt-badge wt-badge-secondary">Passed</span>'
        :(t.exit_code>0?'<span class="wt-badge wt-badge-danger">Failed ('+escapeHtml(String(t.exit_code))+')</span>'
        :'<span class="wt-badge wt-badge-secondary">Idle</span>'));
      var eName=escapeHtml(t.name),eDesc=escapeHtml(t.description),ePath=escapeHtml(t.binary_path);
      var safeAttr=t.name.replace(/\\/g,'\\\\').replace(/'/g,"\\'");
      return '<div class="wt-card" style="cursor:pointer" onclick="showTestDetail(\''+safeAttr+'\')">'
        +'<div class="wt-card-header"><span class="wt-card-title">'
        +'<span class="wt-status-dot '+(t.is_running?'running':(t.exit_code===0&&t.end_time?'online':'offline'))+'"></span>'
        +eName+'</span>'+status+'</div>'
        +'<div style="font-size:12px;color:var(--wt-text-secondary)">'+eDesc+'</div>'
        +'<div style="font-size:11px;color:var(--wt-text-secondary);margin-top:4px;font-family:var(--wt-mono)">'+ePath+'</div>'
        +'</div>';
    }).join('');
    if(currentTest){
      var t=d.tests.find(x=>x.name===currentTest);
      if(t)updateTestDetail(t);
    }
  });
}

function showTestDetail(name){
  currentTest=name;
  document.getElementById('tests-overview').classList.add('hidden');
  document.getElementById('tests-detail').classList.remove('hidden');
  document.getElementById('testDetailName').textContent=name;
  fetch('/api/tests').then(r=>r.json()).then(d=>{
    var t=d.tests.find(x=>x.name===name);
    if(t)updateTestDetail(t);
  });
  loadTestHistory(name);
  pollTestLog();
}

function updateTestDetail(t){
  var s=t.is_running?'<span class="wt-badge wt-badge-success">Running</span>'
    :(t.exit_code===0&&t.end_time?'<span class="wt-badge wt-badge-secondary">Passed</span>'
    :(t.exit_code>0?'<span class="wt-badge wt-badge-danger">Failed</span>':'<span class="wt-badge wt-badge-secondary">Idle</span>'));
  document.getElementById('testDetailStatus').innerHTML=s;
  document.getElementById('testStopBtn').style.display=t.is_running?'':'none';
  if(!document.getElementById('testDetailArgs').value&&t.default_args){
    document.getElementById('testDetailArgs').value=Array.isArray(t.default_args)?t.default_args.join(' '):t.default_args;
  }
}

function showTestsOverview(){
  currentTest=null;
  if(testLogPoll){clearInterval(testLogPoll);testLogPoll=null;}
  document.getElementById('tests-overview').classList.remove('hidden');
  document.getElementById('tests-detail').classList.add('hidden');
}

function startTestDetail(){
  if(!currentTest)return;
  var args=document.getElementById('testDetailArgs').value;
  fetch('/api/tests/start',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({test:currentTest,args:args})}).then(()=>{
    document.getElementById('testDetailLog').textContent='Starting...';
    setTimeout(fetchTests,500);pollTestLog();
  });
}

function stopTestDetail(){
  if(!currentTest)return;
  fetch('/api/tests/stop',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({test:currentTest})}).then(()=>setTimeout(fetchTests,500));
}

function clearTestLog(){document.getElementById('testDetailLog').textContent='';}

function pollTestLog(){
  if(testLogPoll)clearInterval(testLogPoll);
  testLogPoll=setInterval(()=>{
    if(!currentTest)return;
    fetch('/api/tests/'+encodeURIComponent(currentTest)+'/log').then(r=>r.json()).then(d=>{
      if(d.log){
        var el=document.getElementById('testDetailLog');
        el.textContent=d.log;
        el.scrollTop=el.scrollHeight;
      }
    }).catch(()=>{});
  },1500);
}

function loadTestHistory(name){
  fetch('/api/tests/'+encodeURIComponent(name)+'/history').then(r=>r.json()).then(d=>{
    var tb=document.getElementById('testHistoryBody');
    tb.innerHTML=d.runs.map(r=>{
      var started=r.start_time?new Date(r.start_time*1000).toLocaleString():'--';
      var dur=r.end_time&&r.start_time?(r.end_time-r.start_time)+'s':'--';
      var code=r.exit_code===0?'<span class="wt-badge wt-badge-success">0</span>'
        :'<span class="wt-badge wt-badge-danger">'+escapeHtml(String(r.exit_code))+'</span>';
      return '<tr><td>'+escapeHtml(started)+'</td><td>'+escapeHtml(dur)+'</td><td>'+code+'</td><td style="font-family:var(--wt-mono);font-size:12px">'+
        escapeHtml(r.arguments||'--')+'</td></tr>';
    }).join('')||'<tr><td colspan="4" style="text-align:center;color:var(--wt-text-secondary)">No history</td></tr>';
  });
}

function fetchServices(){
  fetch('/api/services').then(r=>r.json()).then(d=>{
    var online=d.services.filter(s=>s.online).length;
    document.getElementById('svcBadge').textContent=online+'/'+d.services.length;
    var c=document.getElementById('servicesContainer');
    c.innerHTML=d.services.map(s=>{
      var status=s.online?'<span class="wt-badge wt-badge-success"><span class="wt-status-dot online"></span>Online</span>'
        :'<span class="wt-badge wt-badge-secondary"><span class="wt-status-dot offline"></span>Offline</span>';
      var desc={'SIP_CLIENT':'SIP/RTP Gateway','INBOUND_AUDIO_PROCESSOR':'G.711 Decode & Resample',
        'VAD_SERVICE':'Voice Activity Detection','WHISPER_SERVICE':'Whisper ASR','LLAMA_SERVICE':'LLaMA LLM','KOKORO_SERVICE':'Kokoro TTS',
        'OUTBOUND_AUDIO_PROCESSOR':'Audio Encode & RTP'};
      var eName=escapeHtml(s.name),eDesc=escapeHtml(desc[s.name]||s.description),ePath=escapeHtml(s.binary_path);
      var safeAttr=s.name.replace(/\\/g,'\\\\').replace(/'/g,"\\'");
      var btns='<div style="margin-top:6px;display:flex;gap:6px;align-items:center" onclick="event.stopPropagation()">';
      if(!s.online) btns+='<button class="wt-btn wt-btn-primary" style="font-size:11px;padding:2px 8px" onclick="quickSvcStart(\''+safeAttr+'\')">&#x25B6; Start</button>';
      if(s.managed&&s.online) btns+='<button class="wt-btn wt-btn-danger" style="font-size:11px;padding:2px 8px" onclick="quickSvcStop(\''+safeAttr+'\')">&#x25A0; Stop</button>';
      if(s.managed&&s.online) btns+='<button class="wt-btn wt-btn-secondary" style="font-size:11px;padding:2px 8px" onclick="quickSvcRestart(\''+safeAttr+'\')">&#x21BB; Restart</button>';
      btns+='<button class="wt-btn wt-btn-secondary" style="font-size:11px;padding:2px 8px" onclick="showSvcDetail(\''+safeAttr+'\')">&#x2699; Config</button>';
      btns+='</div>';
      return '<div class="wt-card" style="cursor:pointer" onclick="showSvcDetail(\''+safeAttr+'\')">'
        +'<div class="wt-card-header"><span class="wt-card-title">'
        +'<span class="wt-status-dot '+(s.online?'online':'offline')+'"></span>'
        +eName+'</span>'+status+'</div>'
        +'<div style="font-size:12px;color:var(--wt-text-secondary)">'+eDesc+'</div>'
        +'<div style="font-size:11px;color:var(--wt-text-secondary);margin-top:4px;font-family:var(--wt-mono)">'+ePath+'</div>'
        +(s.managed?'<div style="font-size:11px;margin-top:4px"><span class="wt-badge wt-badge-warning">Managed by Frontend</span></div>':'')
        +(s.name==='SIP_CLIENT'?'<div id="sipOverviewLines" style="font-size:11px;margin-top:4px;color:var(--wt-text-secondary)"></div>':'')
        +btns+'</div>';
    }).join('');
    if(currentSvc){
      var s=d.services.find(x=>x.name===currentSvc);
      if(s)updateSvcDetail(s);
    }
    var sipSvc=d.services.find(x=>x.name==='SIP_CLIENT');
    if(sipSvc&&sipSvc.online){
      fetch('/api/sip/lines').then(r=>r.json()).then(ld=>{
        var el=document.getElementById('sipOverviewLines');
        if(!el)return;
        var lines=ld.lines||[];
        if(lines.length===0){el.innerHTML='No active lines';return;}
        var reg=lines.filter(l=>l.registered).length;
        el.innerHTML=lines.length+' line(s) ('+reg+' connected): '+lines.map(l=>l.user+'@'+l.server+':'+l.port).join(', ');
      }).catch(function(){});
    }
  });
}

function showSvcDetail(name){
  currentSvc=name;
  document.getElementById('services-overview').classList.add('hidden');
  document.getElementById('services-detail').classList.remove('hidden');
  document.getElementById('svcDetailName').textContent=name;
  fetch('/api/services').then(r=>r.json()).then(d=>{
    var s=d.services.find(x=>x.name===name);
    if(s)updateSvcDetail(s);
  });
  connectSvcSSE(name);
}

function updateSvcDetail(s){
  document.getElementById('svcDetailPath').textContent=s.binary_path;
  document.getElementById('svcDetailArgs').value=s.default_args||'';
  var online=s.online;
  document.getElementById('svcDetailStatus').innerHTML=online
    ?'<span class="wt-badge wt-badge-success">Online</span>'
    :'<span class="wt-badge wt-badge-secondary">Offline</span>';
  document.getElementById('svcStartBtn').style.display=online?'none':'';
  document.getElementById('svcStopBtn').style.display=(s.managed&&online)?'':'none';
  document.getElementById('svcRestartBtn').style.display=(s.managed&&online)?'':'none';
  var wc=document.getElementById('whisperConfig');
  if(s.name==='WHISPER_SERVICE'){
    wc.classList.remove('hidden');
    loadWhisperConfig(s.default_args||'');
    loadHallucinationFilterState();
  } else {
    wc.classList.add('hidden');
  }
  var sc=document.getElementById('sipClientConfig');
  var slc=document.getElementById('sipActiveLinesCard');
  if(s.name==='SIP_CLIENT'){
    sc.classList.remove('hidden');
    slc.classList.remove('hidden');
    sipRefreshActiveLines();
  } else {
    sc.classList.add('hidden');
    slc.classList.add('hidden');
  }
}
function loadWhisperConfig(args){
  fetch('/api/whisper/models').then(r=>r.json()).then(d=>{
    var langSel=document.getElementById('whisperLang');
    var modelSel=document.getElementById('whisperModel');
    langSel.innerHTML=d.languages.map(l=>'<option value="'+l+'">'+l+'</option>').join('');
    modelSel.innerHTML=d.models.map(m=>'<option value="'+m+'">'+m+'</option>').join('');
    var curLang='de',curModel='';
    var parts=args.split(/\s+/);
    for(var i=0;i<parts.length;i++){
      if((parts[i]==='--language'||parts[i]==='-l')&&i+1<parts.length){curLang=parts[i+1];i++;}
      else if((parts[i]==='--model'||parts[i]==='-m')&&i+1<parts.length){curModel=parts[i+1];i++;}
      else if(parts[i].indexOf('.bin')!==-1){curModel=parts[i];}
    }
    langSel.value=curLang;
    if(curModel)modelSel.value=curModel;
  });
}
function updateWhisperArgs(){
  var lang=document.getElementById('whisperLang').value;
  var model=document.getElementById('whisperModel').value;
  document.getElementById('svcDetailArgs').value='--language '+lang+' '+model;
}
function toggleHallucinationFilter(enabled){
  var statusEl=document.getElementById('whisperHalluFilterStatus');
  statusEl.textContent='...';
  fetch('/api/whisper/hallucination_filter',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({enabled:enabled?'true':'false'})})
  .then(r=>r.json()).then(d=>{
    if(d.error){statusEl.textContent='(offline)';document.getElementById('whisperHallucinationFilter').checked=false;return;}
    statusEl.textContent=d.enabled?'ON':'OFF';
  }).catch(()=>{statusEl.textContent='(error)';});
}
function loadHallucinationFilterState(){
  var cb=document.getElementById('whisperHallucinationFilter');
  var statusEl=document.getElementById('whisperHalluFilterStatus');
  fetch('/api/whisper/hallucination_filter').then(r=>r.json()).then(d=>{
    if(d.error){cb.checked=false;statusEl.textContent='(offline)';return;}
    cb.checked=d.enabled;statusEl.textContent=d.enabled?'ON':'OFF';
  }).catch(()=>{cb.checked=false;statusEl.textContent='(offline)';});
}

var sipLinesRefreshTimer=null;
function sipConnectPbx(){
  var server=document.getElementById('sipPbxServer').value.trim();
  var port=document.getElementById('sipPbxPort').value.trim()||'5060';
  var user=document.getElementById('sipPbxUser').value.trim();
  var password=document.getElementById('sipPbxPassword').value;
  var status=document.getElementById('sipPbxStatus');
  if(!server||!user){status.innerHTML='<span style="color:var(--wt-danger)">Server and Username required</span>';return;}
  var portNum=parseInt(port,10);
  if(isNaN(portNum)||portNum<1||portNum>65535){status.innerHTML='<span style="color:var(--wt-danger)">Port must be 1-65535</span>';return;}
  status.innerHTML='<span style="color:var(--wt-warning)">Connecting...</span>';
  fetch('/api/sip/add-line',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({user:user,server:server,password:password,port:port})
  }).then(r=>r.json()).then(d=>{
    if(d.success){
      status.innerHTML='<span style="color:var(--wt-success)">Line added</span>';
      document.getElementById('sipPbxUser').value='';
      document.getElementById('sipPbxPassword').value='';
      setTimeout(sipRefreshActiveLines,500);
    } else {
      status.innerHTML='<span style="color:var(--wt-danger)">'+(d.error||'Failed')+'</span>';
    }
  }).catch(function(e){status.innerHTML='<span style="color:var(--wt-danger)">SIP Client not reachable</span>';});
}
function sipRefreshActiveLines(){
  var container=document.getElementById('sipActiveLines');
  if(!container)return;
  fetch('/api/sip/lines').then(r=>r.json()).then(d=>{
    var lines=d.lines||[];
    if(lines.length===0){container.innerHTML='No active lines';return;}
    var html='<div style="display:flex;flex-direction:column;gap:4px">';
    lines.forEach(function(l){
      var regBadge=l.registered
        ?'<span class="wt-badge wt-badge-success" style="font-size:10px">connected</span>'
        :'<span class="wt-badge wt-badge-warning" style="font-size:10px">connecting</span>';
      var serverInfo=l.server?(l.server+':'+l.port):'local';
      html+='<div style="display:flex;align-items:center;gap:6px;padding:4px 6px;border-radius:4px;background:var(--wt-card-hover)">';
      html+='<span style="font-weight:600;min-width:60px">'+escapeHtml(l.user)+'</span>';
      html+='<span style="color:var(--wt-text-secondary);font-size:11px;font-family:var(--wt-mono)">'+escapeHtml(serverInfo)+'</span>';
      html+=regBadge;
      html+='<span style="flex:1"></span>';
      html+='<button class="wt-btn wt-btn-danger" style="font-size:10px;padding:1px 6px" onclick="sipHangupLine('+l.index+')">Hangup</button>';
      html+='</div>';
    });
    html+='</div>';
    container.innerHTML=html;
  }).catch(function(){container.innerHTML='<span style="color:var(--wt-danger)">SIP Client not reachable</span>';});
}
function sipHangupLine(index){
  fetch('/api/sip/remove-line',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({index:index.toString()})
  }).then(r=>r.json()).then(function(){
    setTimeout(sipRefreshActiveLines,300);
  }).catch(function(){});
}

function showServicesOverview(){
  currentSvc=null;
  if(svcLogSSE){svcLogSSE.close();svcLogSSE=null;}
  document.getElementById('services-overview').classList.remove('hidden');
  document.getElementById('services-detail').classList.add('hidden');
}

function startSvcDetail(){
  if(!currentSvc)return;
  var args=document.getElementById('svcDetailArgs').value;
  fetch('/api/services/start',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({service:currentSvc,args:args})}).then(()=>{
    setTimeout(fetchServices,1000);connectSvcSSE(currentSvc);
  });
}
function stopSvcDetail(){
  if(!currentSvc)return;
  fetch('/api/services/stop',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({service:currentSvc})}).then(()=>setTimeout(fetchServices,1000));
}
function restartSvcDetail(){
  if(!currentSvc)return;
  var args=document.getElementById('svcDetailArgs').value;
  fetch('/api/services/restart',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({service:currentSvc,args:args})}).then(()=>setTimeout(fetchServices,2000));
}
function saveSvcConfig(){
  if(!currentSvc)return;
  var args=document.getElementById('svcDetailArgs').value;
  fetch('/api/services/config',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({service:currentSvc,args:args})}).then(()=>{
    fetchServices();
    var btn=document.getElementById('svcSaveBtn');
    btn.textContent='Saved!';setTimeout(()=>{btn.textContent='Save Config';},1500);
  });
}
function quickSvcStart(name){
  fetch('/api/services/start',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({service:name})}).then(()=>setTimeout(fetchServices,1000));
}
function quickSvcStop(name){
  fetch('/api/services/stop',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({service:name})}).then(()=>setTimeout(fetchServices,1000));
}
function quickSvcRestart(name){
  fetch('/api/services/restart',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({service:name})}).then(()=>setTimeout(fetchServices,2000));
}
function clearSvcLog(){document.getElementById('svcDetailLog').textContent='';}

function connectSvcSSE(name){
  if(svcLogSSE){svcLogSSE.close();}
  var el=document.getElementById('svcDetailLog');
  el.innerHTML='';
  fetch('/api/logs/recent').then(function(r){return r.json();}).then(function(d){
    if(!d.logs||!d.logs.length)return;
    d.logs.slice().reverse().forEach(function(e){
      if(e.service!==name)return;
      var lc=/^[A-Z]+$/.test(e.level)?e.level:'INFO';
      el.innerHTML+='<div class="wt-log-entry"><span class="log-ts">'+escapeHtml(e.timestamp)+'</span> '
        +'<span class="log-lvl-'+lc+'">'+escapeHtml(e.level)+'</span>'+fmtCallBadge(e.call_id)+' '+escapeHtml(e.message)+'</div>';
    });
    el.scrollTop=el.scrollHeight;
  }).catch(function(){});
  svcLogSSE=new EventSource('/api/logs/stream?service='+encodeURIComponent(name));
  svcLogSSE.onmessage=function(e){
    try{
      var d=JSON.parse(e.data);
      var lc=/^[A-Z]+$/.test(d.level)?d.level:'INFO';
      el.innerHTML+='<div class="wt-log-entry"><span class="log-ts">'+escapeHtml(d.timestamp)+'</span> '
        +'<span class="log-lvl-'+lc+'">'+escapeHtml(d.level)+'</span>'+fmtCallBadge(d.call_id)+' '+escapeHtml(d.message)+'</div>';
      el.scrollTop=el.scrollHeight;
    }catch(x){}
  };
  svcLogSSE.onerror=function(){
    svcLogSSE.close();
    setTimeout(function(){if(currentSvc===name)connectSvcSSE(name);},3000);
  };
}

function reconnectLogSSE(){
  if(logSSE){logSSE.close();}
  var svc=document.getElementById('logServiceFilter').value;
  var lvl=document.getElementById('logLevelFilter').value;
  var el=document.getElementById('liveLogView');
  if(!el.children.length){
    fetch('/api/logs/recent').then(function(r){return r.json();}).then(function(d){
      if(!d.logs||!d.logs.length)return;
      var logs=d.logs.slice().reverse();
      logs.forEach(function(e){
        if(svc&&e.service!==svc)return;
        if(lvl&&e.level!==lvl)return;
        var lc=/^[A-Z]+$/.test(e.level)?e.level:'INFO';
        el.innerHTML+='<div class="wt-log-entry"><span class="log-ts">'+escapeHtml(e.timestamp)+'</span> '
          +'<span class="log-svc">'+escapeHtml(e.service)+'</span> '
          +'<span class="log-lvl-'+lc+'">'+escapeHtml(e.level)+'</span>'+fmtCallBadge(e.call_id)+' '+escapeHtml(e.message)+'</div>';
      });
      if(document.getElementById('autoScrollToggle').classList.contains('on')){el.scrollTop=el.scrollHeight;}
    }).catch(function(){});
  }
  var url='/api/logs/stream';
  if(svc)url+='?service='+encodeURIComponent(svc);
  logSSE=new EventSource(url);
  logSSE.onmessage=function(e){
    try{
      var d=JSON.parse(e.data);
      if(lvl&&d.level!==lvl)return;
      var lc=/^[A-Z]+$/.test(d.level)?d.level:'INFO';
      el.innerHTML+='<div class="wt-log-entry"><span class="log-ts">'+escapeHtml(d.timestamp)+'</span> '
        +'<span class="log-svc">'+escapeHtml(d.service)+'</span> '
        +'<span class="log-lvl-'+lc+'">'+escapeHtml(d.level)+'</span>'+fmtCallBadge(d.call_id)+' '+escapeHtml(d.message)+'</div>';
      if(el.children.length>2000){el.removeChild(el.firstChild);}
      if(document.getElementById('autoScrollToggle').classList.contains('on')){el.scrollTop=el.scrollHeight;}
    }catch(x){}
  };
  logSSE.onerror=function(){
    logSSE.close();
    setTimeout(reconnectLogSSE,3000);
  };
}

function clearLiveLogs(){document.getElementById('liveLogView').innerHTML='';}

function runQuery(){
  var q=document.getElementById('sqlQuery').value;
  if(!q)return;
  fetch('/api/db/query',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({query:q})}).then(r=>r.json()).then(d=>{
    var c=document.getElementById('queryResults');
    if(d.error){
      c.innerHTML='<div class="wt-card" style="border-color:var(--wt-danger)"><div style="color:var(--wt-danger);font-weight:500">Error</div><div style="font-size:13px;margin-top:4px">'+escapeHtml(d.error)+'</div></div>';
    }else if(d.rows&&d.rows.length>0){
      var cols=Object.keys(d.rows[0]);
      c.innerHTML='<div class="wt-card" style="padding:0;overflow:auto"><table class="wt-table"><thead><tr>'
        +cols.map(k=>'<th>'+escapeHtml(k)+'</th>').join('')+'</tr></thead><tbody>'
        +d.rows.map(r=>'<tr>'+cols.map(k=>'<td style="font-size:12px;font-family:var(--wt-mono)">'+escapeHtml(String(r[k]??'NULL'))+'</td>').join('')+'</tr>').join('')
        +'</tbody></table></div>'
        +(d.truncated?'<div style="font-size:12px;color:var(--wt-warning);margin-top:4px">Results truncated to 10,000 rows</div>':'')
        +'<div style="font-size:12px;color:var(--wt-text-secondary);margin-top:4px">'+escapeHtml(String(d.rows.length))+' rows returned</div>';
    }else{
      c.innerHTML='<div class="wt-card"><div style="color:var(--wt-success)">Query executed successfully</div>'
        +'<div style="font-size:13px;margin-top:4px">'+escapeHtml(String(d.affected||0))+' rows affected</div></div>';
    }
  });
}

function toggleDbWrite(){
  var el=document.getElementById('dbWriteToggle');
  var newMode=!el.classList.contains('on');
  if(newMode&&!confirm('Enable write mode? This allows INSERT, UPDATE, DELETE queries.')){return;}
  fetch('/api/db/write_mode',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({enabled:newMode?'true':'false'})}).then(r=>r.json()).then(d=>{
    if(d.write_mode)el.classList.add('on');else el.classList.remove('on');
  });
}

function loadSchema(){
  fetch('/api/db/schema').then(r=>r.json()).then(d=>{
    var v=document.getElementById('schemaView');
    v.classList.remove('hidden');
    v.innerHTML=d.tables.map(t=>
      '<div class="wt-card"><div class="wt-card-title" style="margin-bottom:8px">'+escapeHtml(t.name)+'</div>'
      +'<pre style="font-size:12px;font-family:var(--wt-mono);margin:0;white-space:pre-wrap;color:var(--wt-text-secondary)">'+escapeHtml(t.sql)+'</pre></div>'
    ).join('');
  });
}

function loadCredentials(){
  var credFields=[
    {key:'hf_token',inputId:'credHfToken',clearId:'credHfClear',statusId:'credHfStatus',ph:'hf_...'},
    {key:'github_token',inputId:'credGhToken',clearId:'credGhClear',statusId:'credGhStatus',ph:'ghp_...'}
  ];
  fetch('/api/settings').then(r=>{
    if(!r.ok)throw new Error('Server error '+r.status);
    return r.json();
  }).then(d=>{
    var s=d.settings||{};
    credFields.forEach(f=>{
      var inp=document.getElementById(f.inputId);
      var clr=document.getElementById(f.clearId);
      var saved=s[f.key]==='***';
      if(inp){inp.value='';inp.placeholder=saved?'Token saved (hidden)':f.ph;}
      if(clr){clr.style.display=saved?'':'none';}
    });
  }).catch(e=>{
    credFields.forEach(f=>{
      var el=document.getElementById(f.statusId);
      if(el){el.style.color='var(--wt-danger)';el.textContent='Failed to load: '+e.message;
        setTimeout(()=>{el.textContent='';},5000);}
    });
  });
}

function saveCredential(key,inputId,statusId,clearBtnId){
  var inp=document.getElementById(inputId);
  var el=document.getElementById(statusId);
  if(!inp||!el)return;
  var val=inp.value.trim();
  if(!val){el.style.color='var(--wt-danger)';el.textContent='Token cannot be empty';
    setTimeout(()=>{el.textContent='';},3000);return;}
  el.style.color='var(--wt-text-secondary)';el.textContent='Saving...';
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({key:key,value:val})}).then(r=>{
    if(!r.ok)return r.json().catch(()=>({error:'Server error '+r.status}));
    return r.json();
  }).then(d=>{
    if(d.status==='saved'){
      el.style.color='var(--wt-success)';el.textContent='Saved successfully';
      inp.value='';inp.placeholder='Token saved (hidden)';
      var clr=document.getElementById(clearBtnId);
      if(clr)clr.style.display='';
    }else{
      el.style.color='var(--wt-danger)';el.textContent='Error: '+(d.error||'Unknown');
    }
    setTimeout(()=>{el.textContent='';},3000);
  }).catch(()=>{
    el.style.color='var(--wt-danger)';el.textContent='Network error';
    setTimeout(()=>{el.textContent='';},3000);
  });
}

function clearCredential(key,inputId,statusId,clearBtnId,defaultPh){
  var el=document.getElementById(statusId);
  if(!el)return;
  if(!confirm('Remove saved token?'))return;
  el.style.color='var(--wt-text-secondary)';el.textContent='Removing...';
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({key:key,value:''})}).then(r=>{
    if(!r.ok)return r.json().catch(()=>({error:'Server error '+r.status}));
    return r.json();
  }).then(d=>{
    if(d.status==='saved'){
      el.style.color='var(--wt-success)';el.textContent='Token removed';
      var inp=document.getElementById(inputId);
      if(inp){inp.value='';inp.placeholder=defaultPh||'';}
      var clr=document.getElementById(clearBtnId);
      if(clr)clr.style.display='none';
    }else{
      el.style.color='var(--wt-danger)';el.textContent='Error: '+(d.error||'Unknown');
    }
    setTimeout(()=>{el.textContent='';},3000);
  }).catch(()=>{
    el.style.color='var(--wt-danger)';el.textContent='Network error';
    setTimeout(()=>{el.textContent='';},3000);
  });
}

function setTheme(t){
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({key:'theme',value:t})}).then(()=>location.reload());
}

function toggleThemeMenu(){
  document.getElementById('themeMenu').classList.toggle('open');
}

function escapeHtml(s){
  if(!s)return'';
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;').replace(/'/g,'&#39;');
}
function showToast(msg,type){
  var el=document.createElement('div');
  el.textContent=msg;
  el.style.cssText='position:fixed;top:20px;right:20px;padding:12px 20px;border-radius:6px;z-index:10000;font-size:14px;max-width:400px;box-shadow:0 4px 12px rgba(0,0,0,0.3);transition:opacity 0.3s;'
    +(type==='error'?'background:#dc3545;color:#fff;':'background:var(--wt-card-bg,#2a2a2a);color:var(--wt-text,#e0e0e0);border:1px solid var(--wt-border,#444);');
  document.body.appendChild(el);
  setTimeout(function(){el.style.opacity='0';setTimeout(function(){el.remove();},300);},3000);
}

var callLineMap={};
var _clmPending=null;
function refreshCallLineMap(){
  fetch('/api/sip/stats').then(function(r){return r.json();}).then(function(d){
    if(!d.calls)return;
    var m={};
    d.calls.forEach(function(c){m[c.call_id]='L'+c.line_index;});
    callLineMap=m;
    document.querySelectorAll('span.log-cid[data-cid]').forEach(function(el){
      var cid=parseInt(el.getAttribute('data-cid'),10);
      var lbl=m[cid];
      if(lbl){el.textContent=lbl+' C'+cid;}
    });
  }).catch(function(){});
}
setInterval(refreshCallLineMap,5000);
setTimeout(refreshCallLineMap,500);

function fmtCallBadge(cid){
  if(!cid)return'';
  var lbl=callLineMap[cid];
  if(!lbl&&!_clmPending){
    _clmPending=setTimeout(function(){_clmPending=null;refreshCallLineMap();},300);
  }
  var txt=lbl?(lbl+' C'+cid):('C'+cid);
  return ' <span class="log-cid" data-cid="'+cid+'">'+txt+'</span>';
}

function refreshTestFiles(){
  fetch('/api/testfiles').then(r=>r.json()).then(d=>{
    window._testFiles=d.files||[];
    var c=document.getElementById('testFilesContainer');
    if(!d.files||d.files.length===0){
      c.innerHTML='<p style="color:var(--wt-text-secondary);font-size:13px">No test files found in Testfiles/ directory</p>';
      return;
    }
    c.innerHTML='<table class="wt-table"><thead><tr><th>File</th><th>Duration</th><th>Sample Rate</th><th>Size</th><th>Ground Truth</th></tr></thead><tbody>'+
      d.files.map(f=>{
        var dur=(f.duration_sec||0).toFixed(2)+'s';
        var size=((f.size_bytes||0)/1024).toFixed(1)+' KB';
        return '<tr><td style="font-family:var(--wt-mono);font-size:12px">'+escapeHtml(f.name)+'</td>'+
          '<td>'+dur+'</td><td>'+f.sample_rate+' Hz</td><td>'+size+'</td>'+
          '<td style="font-size:12px;max-width:300px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">'+
          escapeHtml(f.ground_truth||'--')+'</td></tr>';
      }).join('')+'</tbody></table>';
    
    var sel1=document.getElementById('injectFileSelect');
    var sel2=document.getElementById('accuracyTestFiles');
    var sel3=document.getElementById('iapTestFileSelect');
    sel1.innerHTML='<option value="">-- Select a test file --</option>'+d.files.map(f=>'<option value="'+escapeHtml(f.name)+'">'+escapeHtml(f.name)+'</option>').join('');
    sel2.innerHTML=d.files.map(f=>'<option value="'+escapeHtml(f.name)+'">'+escapeHtml(f.name)+'</option>').join('');
    if(sel3)sel3.innerHTML='<option value="">-- Select a test file --</option>'+d.files.map(f=>'<option value="'+escapeHtml(f.name)+'">'+escapeHtml(f.name)+'</option>').join('');
    var sel4=document.getElementById('fullLoopFiles');
    if(sel4)sel4.innerHTML=d.files.map(f=>'<option value="'+escapeHtml(f.name)+'">'+escapeHtml(f.name)+'</option>').join('');
  }).catch(e=>console.error('Failed to load test files:',e));
  loadLogLevels();
}

function loadLogLevels(){
  fetch('/api/settings/log_level').then(r=>r.json()).then(d=>{
    var c=document.getElementById('logLevelControls');
    var services=['SIP_CLIENT','INBOUND_AUDIO_PROCESSOR','VAD_SERVICE','WHISPER_SERVICE','LLAMA_SERVICE','KOKORO_SERVICE','OUTBOUND_AUDIO_PROCESSOR'];
    var names={'SIP_CLIENT':'SIP Client','INBOUND_AUDIO_PROCESSOR':'Inbound Audio','VAD_SERVICE':'VAD','WHISPER_SERVICE':'Whisper','LLAMA_SERVICE':'LLaMA','KOKORO_SERVICE':'Kokoro','OUTBOUND_AUDIO_PROCESSOR':'Outbound Audio'};
    var levels=['ERROR','WARN','INFO','DEBUG','TRACE'];
    c.innerHTML=services.map(s=>{
      var current=d.log_levels&&d.log_levels[s]?d.log_levels[s]:'INFO';
      return '<div class="wt-field"><label>'+escapeHtml(names[s]||s)+'</label><select class="wt-select" id="loglevel_'+s+'" style="width:100%;padding:8px">'+
        levels.map(l=>'<option value="'+l+'"'+(l===current?' selected':'')+'>'+l+'</option>').join('')+'</select></div>';
    }).join('');
  });
}

function saveAllLogLevels(){
  var services=['SIP_CLIENT','INBOUND_AUDIO_PROCESSOR','VAD_SERVICE','WHISPER_SERVICE','LLAMA_SERVICE','KOKORO_SERVICE','OUTBOUND_AUDIO_PROCESSOR'];
  var promises=services.map(s=>{
    var level=document.getElementById('loglevel_'+s).value;
    return fetch('/api/settings/log_level',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({service:s,level:level})});
  });
  Promise.all(promises).then(responses=>Promise.all(responses.map(r=>r.json()))).then(results=>{
    var offline=results.map((r,i)=>r.live_update?null:services[i]).filter(Boolean);
    var msg='Log levels saved.';
    if(offline.length>0) msg+=' ('+offline.join(', ')+' offline — will apply on next start)';
    showToast(msg);
  }).catch(e=>showToast('Error saving log levels: '+e,'error'));
}

function refreshInjectLegs(){
  fetch('http://localhost:'+TSP_PORT+'/calls').then(r=>r.json()).then(d=>{
    var sel=document.getElementById('injectLeg');
    sel.innerHTML='<option value="a">Leg A (first)</option><option value="b">Leg B (second)</option>';
    if(d.calls&&d.calls.length>0&&d.calls[0].legs){
      sel.innerHTML='';
      d.calls[0].legs.forEach(function(l){
        sel.innerHTML+='<option value="'+escapeHtml(l.user)+'">'+escapeHtml(l.user)+(l.answered?' (connected)':' (pending)')+'</option>';
      });
    }
  }).catch(function(){});
}

function injectAudio(){
  var file=document.getElementById('injectFileSelect').value;
  var leg=document.getElementById('injectLeg').value;
  if(!file){alert('Please select a test file');return;}
  var status=document.getElementById('injectionStatus');
  status.innerHTML='<span style="color:var(--wt-accent)">Injecting audio...</span>';
  fetch('http://localhost:'+TSP_PORT+'/inject',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({file:file,leg:leg})})
    .then(r=>r.json()).then(d=>{
      if(d.success||d.injecting){
        status.innerHTML='<span style="color:var(--wt-success)">Injecting: '+escapeHtml(d.injecting||file)+' to leg '+escapeHtml(d.leg||leg)+'</span>';
      }else{
        status.innerHTML='<span style="color:var(--wt-danger)">Injection failed: '+escapeHtml(d.error||'Unknown error')+'</span>';
      }
    }).catch(e=>{
      status.innerHTML='<span style="color:var(--wt-danger)">Error: Test SIP Provider not reachable (is it running on port '+TSP_PORT+'?)</span>';
    });
}

var llamaPrompts=[];
function loadLlamaPrompts(){
  fetch('/api/llama/prompts').then(r=>r.json()).then(d=>{
    llamaPrompts=d.prompts||[];
    var sel=document.getElementById('llamaTestPrompts');
    if(!sel) return;
    sel.innerHTML='';
    llamaPrompts.forEach(function(p){
      var opt=document.createElement('option');
      opt.value=p.id;
      opt.textContent='['+p.category+'] '+p.prompt;
      opt.selected=true;
      sel.appendChild(opt);
    });
  }).catch(function(){});
}

var llamaQualityPoll=null;
var llamaShutupPoll=null;

function runLlamaQualityTest(){
  if(llamaQualityPoll){clearInterval(llamaQualityPoll);llamaQualityPoll=null;}
  var status=document.getElementById('llamaTestStatus');
  var results=document.getElementById('llamaTestResults');
  var sel=document.getElementById('llamaTestPrompts');
  var custom=document.getElementById('llamaCustomPrompt').value.trim();
  var selectedIds=Array.from(sel.selectedOptions).map(function(o){return parseInt(o.value);});
  var prompts=llamaPrompts.filter(function(p){return selectedIds.indexOf(p.id)>=0;});
  if(custom){prompts.push({id:0,prompt:custom,expected_keywords:[],category:'custom',max_words:30});}
  if(prompts.length===0){status.innerHTML='<span style="color:var(--wt-danger)">Select at least one prompt or enter a custom prompt.</span>';return;}
  status.innerHTML='<span style="color:var(--wt-accent)">Running quality test ('+prompts.length+' prompts)...</span>';
  results.innerHTML='';
  fetch('/api/llama/quality_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({prompts:prompts})})
    .then(r=>{
      if(r.status===202) return r.json();
      return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});
    }).then(d=>{
      status.innerHTML='<span style="color:var(--wt-accent)">Quality test running (task '+d.task_id+', '+prompts.length+' prompts)...</span>';
      llamaQualityPoll=setInterval(()=>pollLlamaQualityTask(d.task_id),2000);
    }).catch(e=>{
      if(llamaQualityPoll){clearInterval(llamaQualityPoll);llamaQualityPoll=null;}
      status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
    });
}

function pollLlamaQualityTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(r=>r.json()).then(d=>{
    if(d.status==='running') return;
    clearInterval(llamaQualityPoll);llamaQualityPoll=null;
    var status=document.getElementById('llamaTestStatus');
    var results=document.getElementById('llamaTestResults');
    if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error)+'</span>';return;}
    status.innerHTML='<span style="color:var(--wt-success)">Quality test complete — '+d.results.length+' prompts tested.</span>';
    var html='<table class="wt-table"><tr><th>Prompt</th><th>Response</th><th>Latency</th><th>Words</th><th>Keywords</th><th>German</th><th>Score</th></tr>';
    d.results.forEach(function(r){
      var scoreColor=r.score>=80?'var(--wt-success)':r.score>=50?'var(--wt-warning)':'var(--wt-danger)';
      html+='<tr><td style="max-width:200px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(r.prompt)+'</td>';
      html+='<td style="max-width:300px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(r.response)+'</td>';
      html+='<td>'+r.latency_ms+'ms</td>';
      html+='<td>'+r.word_count+(r.word_count>r.max_words?' <span style="color:var(--wt-danger)">!</span>':'')+'</td>';
      html+='<td>'+r.keywords_found+'/'+r.keywords_total+'</td>';
      html+='<td>'+(r.is_german?'<span style="color:var(--wt-success)">Ja</span>':'<span style="color:var(--wt-danger)">Nein</span>')+'</td>';
      html+='<td style="color:'+scoreColor+';font-weight:bold">'+r.score+'%</td></tr>';
    });
    html+='</table>';
    if(d.summary){
      html+='<div style="margin-top:10px;padding:10px;background:var(--wt-bg);border-radius:4px;font-size:12px">';
      html+='<strong>Summary:</strong> Avg Score: '+d.summary.avg_score+'% | Avg Latency: '+d.summary.avg_latency_ms+'ms | German: '+d.summary.german_pct+'%';
      html+='</div>';
    }
    results.innerHTML=html;
  }).catch(e=>console.error('pollLlamaQualityTask',e));
}

function runLlamaShutupTest(){
  if(llamaShutupPoll){clearInterval(llamaShutupPoll);llamaShutupPoll=null;}
  var status=document.getElementById('llamaTestStatus');
  var result=document.getElementById('llamaShutupResult');
  status.innerHTML='<span style="color:var(--wt-accent)">Running shut-up test...</span>';
  result.innerHTML='';
  fetch('/api/llama/shutup_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({prompt:'Erzähl mir eine lange Geschichte über einen Ritter.'})})
    .then(r=>{
      if(r.status===202) return r.json();
      return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});
    }).then(d=>{
      status.innerHTML='<span style="color:var(--wt-accent)">Shut-up test running (task '+d.task_id+')...</span>';
      llamaShutupPoll=setInterval(()=>pollLlamaShutupTask(d.task_id),1000);
    }).catch(e=>{
      if(llamaShutupPoll){clearInterval(llamaShutupPoll);llamaShutupPoll=null;}
      status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
    });
}

function pollLlamaShutupTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(r=>r.json()).then(d=>{
    if(d.status==='running') return;
    clearInterval(llamaShutupPoll);llamaShutupPoll=null;
    var status=document.getElementById('llamaTestStatus');
    var result=document.getElementById('llamaShutupResult');
    if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error)+'</span>';return;}
    status.innerHTML='<span style="color:var(--wt-success)">Shut-up test complete.</span>';
    var interruptColor=d.interrupt_latency_ms<=100?'var(--wt-success)':d.interrupt_latency_ms<=500?'var(--wt-warning)':'var(--wt-danger)';
    var html='<div class="wt-card" style="margin:0;padding:10px">';
    html+='<p><strong>Interrupt latency:</strong> <span style="color:'+interruptColor+';font-weight:bold">'+d.interrupt_latency_ms+'ms</span>';
    html+=' (target: &lt;500ms)</p>';
    html+='<p><strong>Total generation time:</strong> '+d.total_ms+'ms</p>';
    html+='<p><strong>Result:</strong> '+(d.interrupt_latency_ms<=500?'<span style="color:var(--wt-success)">PASS</span>':'<span style="color:var(--wt-danger)">FAIL — too slow</span>')+'</p>';
    html+='</div>';
    result.innerHTML=html;
  }).catch(e=>console.error('pollLlamaShutupTask',e));
}

var shutupPipelinePoll=null;

function runShutupPipelineTest(){
  if(shutupPipelinePoll){clearInterval(shutupPipelinePoll);shutupPipelinePoll=null;}
  var status=document.getElementById('shutupPipelineStatus');
  var results=document.getElementById('shutupPipelineResults');
  status.innerHTML='<span style="color:var(--wt-accent)">Running pipeline shut-up test...</span>';
  results.innerHTML='';
  var sel=document.getElementById('shutupScenarios');
  var scenarios=[];
  for(var i=0;i<sel.options.length;i++){if(sel.options[i].selected)scenarios.push(sel.options[i].value);}
  if(!scenarios.length)scenarios=['basic','early','late','rapid'];
  fetch('/api/shutup_pipeline_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({scenarios:scenarios})})
    .then(r=>{
      if(r.status===202) return r.json();
      return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});
    }).then(d=>{
      status.innerHTML='<span style="color:var(--wt-accent)">Pipeline shut-up test running (task '+d.task_id+')...</span>';
      shutupPipelinePoll=setInterval(()=>pollShutupPipelineTask(d.task_id),1500);
    }).catch(e=>{
      if(shutupPipelinePoll){clearInterval(shutupPipelinePoll);shutupPipelinePoll=null;}
      status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
    });
}

function pollShutupPipelineTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(r=>r.json()).then(d=>{
    if(d.status==='running') return;
    clearInterval(shutupPipelinePoll);shutupPipelinePoll=null;
    var status=document.getElementById('shutupPipelineStatus');
    var results=document.getElementById('shutupPipelineResults');
    if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error)+'</span>';return;}
    var sr=d.scenarios||[];
    var allPass=true;
    var html='';
    for(var i=0;i<sr.length;i++){
      var s=sr[i];
      var pass=s.pass;
      if(!pass) allPass=false;
      var col=pass?'var(--wt-success)':'var(--wt-danger)';
      html+='<div class="wt-card" style="margin:0 0 8px 0;padding:10px">';
      html+='<p><strong>'+escapeHtml(s.name)+'</strong> — <span style="color:'+col+';font-weight:bold">'+(pass?'PASS':'FAIL')+'</span></p>';
      html+='<p style="font-size:12px;color:var(--wt-text-secondary)">'+escapeHtml(s.description)+'</p>';
      if(s.interrupt_latency_ms!==undefined){
        var ic=s.interrupt_latency_ms<=100?'var(--wt-success)':s.interrupt_latency_ms<=500?'var(--wt-warning)':'var(--wt-danger)';
        html+='<p>Interrupt latency: <span style="color:'+ic+';font-weight:bold">'+s.interrupt_latency_ms.toFixed(1)+'ms</span> (target: &lt;500ms)</p>';
      }
      if(s.total_ms!==undefined) html+='<p>Total time: '+s.total_ms.toFixed(0)+'ms</p>';
      if(s.detail) html+='<p style="font-size:11px;color:var(--wt-text-secondary)">'+escapeHtml(s.detail)+'</p>';
      html+='</div>';
    }
    status.innerHTML='<span style="color:'+(allPass?'var(--wt-success)':'var(--wt-danger)')+'">Pipeline shut-up test '+(allPass?'PASSED':'FAILED')+'</span>';
    results.innerHTML=html;
  }).catch(e=>console.error('pollShutupPipelineTask',e));
}

var kokoroQualityPoll=null;
var kokoroBenchPoll=null;

function runKokoroQualityTest(){
  if(kokoroQualityPoll){clearInterval(kokoroQualityPoll);kokoroQualityPoll=null;}
  var status=document.getElementById('kokoroTestStatus');
  var results=document.getElementById('kokoroTestResults');
  var custom=document.getElementById('kokoroCustomPhrase').value.trim();
  var body={};
  if(custom) body.phrases=[custom];
  status.innerHTML='<span style="color:var(--wt-accent)">Running Kokoro quality test...</span>';
  results.innerHTML='';
  fetch('/api/kokoro/quality_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(r=>{
      if(r.status===202) return r.json();
      return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});
    }).then(d=>{
      status.innerHTML='<span style="color:var(--wt-accent)">Quality test running (task '+d.task_id+')...</span>';
      kokoroQualityPoll=setInterval(()=>pollKokoroQualityTask(d.task_id),2000);
    }).catch(e=>{
      if(kokoroQualityPoll){clearInterval(kokoroQualityPoll);kokoroQualityPoll=null;}
      status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
    });
}

function pollKokoroQualityTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(r=>r.json()).then(d=>{
    if(d.status==='running') return;
    clearInterval(kokoroQualityPoll);kokoroQualityPoll=null;
    var status=document.getElementById('kokoroTestStatus');
    var results=document.getElementById('kokoroTestResults');
    if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error)+'</span>';return;}
    status.innerHTML='<span style="color:var(--wt-success)">Quality test complete — '+d.results.length+' phrases tested.</span>';
    var html='<table class="wt-table"><tr><th>Phrase</th><th>Latency</th><th>Samples</th><th>Duration</th><th>RTF</th><th>Peak</th><th>RMS</th><th>Status</th></tr>';
    d.results.forEach(function(r){
      var color=r.status==='pass'?'var(--wt-success)':r.status==='warn'?'var(--wt-warning)':'var(--wt-danger)';
      html+='<tr><td style="max-width:250px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(r.phrase)+'</td>';
      html+='<td>'+r.latency_ms+'ms</td>';
      html+='<td>'+r.samples+'</td>';
      html+='<td>'+r.duration_s.toFixed(2)+'s</td>';
      html+='<td style="color:'+color+';font-weight:bold">'+r.rtf.toFixed(3)+'</td>';
      html+='<td>'+r.peak.toFixed(3)+'</td>';
      html+='<td>'+r.rms.toFixed(4)+'</td>';
      html+='<td style="color:'+color+'">'+r.status.toUpperCase()+'</td></tr>';
    });
    html+='</table>';
    if(d.summary){
      html+='<div style="margin-top:10px;padding:10px;background:var(--wt-bg);border-radius:4px;font-size:12px">';
      html+='<strong>Summary:</strong> Avg Latency: '+d.summary.avg_latency_ms+'ms | Avg RTF: '+d.summary.avg_rtf.toFixed(3);
      html+=' | Total Audio: '+d.summary.total_duration_s.toFixed(1)+'s | Success: '+d.summary.success_count+'/'+d.summary.total_count;
      html+='</div>';
    }
    results.innerHTML=html;
  }).catch(e=>console.error('pollKokoroQualityTask',e));
}

function runKokoroBenchmark(){
  if(kokoroBenchPoll){clearInterval(kokoroBenchPoll);kokoroBenchPoll=null;}
  var status=document.getElementById('kokoroTestStatus');
  var result=document.getElementById('kokoroBenchResult');
  var iterations=parseInt(document.getElementById('kokoroBenchIter').value)||5;
  var custom=document.getElementById('kokoroCustomPhrase').value.trim();
  var body={iterations:iterations};
  if(custom) body.phrase=custom;
  status.innerHTML='<span style="color:var(--wt-accent)">Running Kokoro benchmark ('+iterations+' iterations)...</span>';
  result.innerHTML='';
  fetch('/api/kokoro/benchmark',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(r=>{
      if(r.status===202) return r.json();
      return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});
    }).then(d=>{
      status.innerHTML='<span style="color:var(--wt-accent)">Benchmark running (task '+d.task_id+')...</span>';
      kokoroBenchPoll=setInterval(()=>pollKokoroBenchTask(d.task_id),2000);
    }).catch(e=>{
      if(kokoroBenchPoll){clearInterval(kokoroBenchPoll);kokoroBenchPoll=null;}
      status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
    });
}

function pollKokoroBenchTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(r=>r.json()).then(d=>{
    if(d.status==='running') return;
    clearInterval(kokoroBenchPoll);kokoroBenchPoll=null;
    var status=document.getElementById('kokoroTestStatus');
    var result=document.getElementById('kokoroBenchResult');
    if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error)+'</span>';return;}
    status.innerHTML='<span style="color:var(--wt-success)">Benchmark complete.</span>';
    var rtfColor=d.rtf<0.5?'var(--wt-success)':d.rtf<1.0?'var(--wt-warning)':'var(--wt-danger)';
    var html='<div class="wt-card" style="margin:0;padding:10px">';
    html+='<p><strong>Phrase:</strong> '+escapeHtml(d.phrase)+'</p>';
    html+='<p><strong>Avg latency:</strong> '+d.avg_ms+'ms | <strong>P50:</strong> '+d.p50_ms+'ms | <strong>P95:</strong> '+d.p95_ms+'ms</p>';
    html+='<p><strong>RTF:</strong> <span style="color:'+rtfColor+';font-weight:bold">'+d.rtf.toFixed(3)+'</span>';
    html+=' (target: &lt;1.0, ideal: &lt;0.5)</p>';
    html+='<p><strong>Audio:</strong> '+d.samples+' samples @ '+d.sample_rate+'Hz = '+d.duration_s.toFixed(2)+'s</p>';
    html+='<p><strong>Success:</strong> '+d.success+'/'+d.total+' iterations</p>';
    html+='<p><strong>Result:</strong> '+(d.rtf<1.0?'<span style="color:var(--wt-success)">PASS — real-time capable</span>':'<span style="color:var(--wt-danger)">FAIL — too slow for real-time</span>')+'</p>';
    html+='</div>';
    result.innerHTML=html;
  }).catch(e=>console.error('pollKokoroBenchTask',e));
}

var pipelineHealthInterval=null;
function checkPipelineHealth(auto_refresh){
  var status=document.getElementById('pipelineHealthStatus');
  var results=document.getElementById('pipelineHealthResults');
  if(status) status.innerHTML='<span style="color:var(--wt-accent)">Checking services...</span>';
  fetch('/api/pipeline/health').then(function(r){return r.json();}).then(function(d){
    var total=d.total||0,online=d.online||0;
    var allOk=online===total;
    var color=allOk?'var(--wt-success)':online===0?'var(--wt-danger)':'var(--wt-warning)';
    if(status) status.innerHTML='<span style="color:'+color+'">'+online+'/'+total+' services online</span>'
      +(auto_refresh?'<span style="color:var(--wt-text-secondary);font-size:11px"> &nbsp;(auto-refresh 10s)</span>':'');
    var html='<table class="wt-table"><tr><th>Service</th><th>Status</th><th>Details</th></tr>';
    (d.services||[]).forEach(function(s){
      var c=s.reachable?'var(--wt-success)':'var(--wt-danger)';
      var dot=s.reachable?'&#x25CF;':'&#x25CB;';
      html+='<tr><td>'+escapeHtml(s.name)+'</td>'
           +'<td style="color:'+c+';font-weight:bold">'+dot+' '+(s.reachable?'online':'offline')+'</td>'
           +'<td style="font-size:11px;color:var(--wt-text-secondary)">'+escapeHtml(s.details)+'</td></tr>';
    });
    html+='</table>';
    if(results) results.innerHTML=html;
  }).catch(function(e){
    if(status) status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
  });
}

function startPipelineHealthAutoRefresh(){
  if(pipelineHealthInterval){clearInterval(pipelineHealthInterval);pipelineHealthInterval=null;}
  checkPipelineHealth(true);
  pipelineHealthInterval=setInterval(function(){checkPipelineHealth(true);},10000);
  var btn=document.getElementById('pipelineHealthAutoBtn');
  if(btn) btn.textContent='Stop Auto-Refresh';
  btn.onclick=stopPipelineHealthAutoRefresh;
}

function stopPipelineHealthAutoRefresh(){
  if(pipelineHealthInterval){clearInterval(pipelineHealthInterval);pipelineHealthInterval=null;}
  var btn=document.getElementById('pipelineHealthAutoBtn');
  if(btn){btn.textContent='Auto-Refresh (10s)';btn.onclick=startPipelineHealthAutoRefresh;}
}

var stressPollInterval=null;
function runMultilineStress(){
  if(stressPollInterval){clearInterval(stressPollInterval);stressPollInterval=null;}
  var btn=document.getElementById('stressRunBtn');
  var status=document.getElementById('stressStatus');
  var results=document.getElementById('stressResults');
  var lines=parseInt(document.getElementById('stressLines').value)||4;
  var dur=parseInt(document.getElementById('stressDuration').value)||10;
  btn.disabled=true;
  status.innerHTML='<span style="color:var(--wt-accent)">Starting stress test ('+lines+' lines, '+dur+'s)...</span>';
  results.innerHTML='';
  fetch('/api/multiline_stress',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({lines:lines,duration_s:dur})})
  .then(function(r){return r.json();}).then(function(d){
    if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error)+'</span>';btn.disabled=false;return;}
    var task_id=d.task_id;
    status.innerHTML='<span style="color:var(--wt-accent)">Running... (task '+task_id+')</span>';
    stressPollInterval=setInterval(function(){
      fetch('/api/async/status?task_id='+task_id).then(function(r){return r.json();}).then(function(r){
        if(r.status==='running'){
          status.innerHTML='<span style="color:var(--wt-accent)">&#x23F3; Stress test in progress...</span>';
          return;
        }
        clearInterval(stressPollInterval);stressPollInterval=null;
        btn.disabled=false;
        if(r.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(r.error)+'</span>';return;}
        var overall_ok=(r.overall_success_pct||0)>=95;
        var col=overall_ok?'var(--wt-success)':r.overall_success_pct>=75?'var(--wt-warning)':'var(--wt-danger)';
        status.innerHTML='<span style="color:'+col+';font-weight:bold">'+r.overall_success_pct+'% success</span>'
          +' &nbsp;('+r.total_ok+'/'+r.total_pings+' pings OK, '+r.lines+' lines, '+r.duration_s+'s)';
        var html='<table class="wt-table"><tr><th>Service</th><th>OK</th><th>Fail</th><th>Success%</th><th>Avg latency</th></tr>';
        (r.services||[]).forEach(function(s){
          var c=s.success_pct>=95?'var(--wt-success)':s.success_pct>=75?'var(--wt-warning)':'var(--wt-danger)';
          html+='<tr><td>'+escapeHtml(s.name)+'</td><td>'+s.ok+'</td><td>'+s.fail+'</td>'
               +'<td style="color:'+c+';font-weight:bold">'+s.success_pct+'%</td>'
               +'<td>'+s.avg_ms+'ms</td></tr>';
        });
        html+='</table>';
        results.innerHTML=html;
      }).catch(function(e){
        clearInterval(stressPollInterval);stressPollInterval=null;
        btn.disabled=false;
        status.innerHTML='<span style="color:var(--wt-danger)">Poll error: '+escapeHtml(String(e))+'</span>';
      });
    },2000);
  }).catch(function(e){
    btn.disabled=false;
    status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
  });
}

var pstressPoll=null;
function runPipelineStressTest(){
  if(pstressPoll){clearInterval(pstressPoll);pstressPoll=null;}
  var btn=document.getElementById('pstressRunBtn');
  var stopBtn=document.getElementById('pstressStopBtn');
  var status=document.getElementById('pstressStatus');
  var progress=document.getElementById('pstressProgress');
  var metrics=document.getElementById('pstressMetrics');
  var results=document.getElementById('pstressResults');
  var dur=parseInt(document.getElementById('pstressDuration').value)||120;
  btn.disabled=true;stopBtn.style.display='inline-block';
  progress.style.display='block';metrics.style.display='block';
  results.innerHTML='';
  status.innerHTML='<span style="color:var(--wt-accent)">Starting full pipeline stress test ('+dur+'s)...</span>';
  document.getElementById('pstressElapsed').textContent='0s / '+dur+'s';
  document.getElementById('pstressCycles').textContent='0 cycles';
  document.getElementById('pstressBar').style.width='0%';
  fetch('/api/pipeline_stress_test',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({duration_s:dur})})
  .then(function(r){return r.json();}).then(function(d){
    if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">'+escapeHtml(d.error)+'</span>';btn.disabled=false;stopBtn.style.display='none';progress.style.display='none';return;}
    status.innerHTML='<span style="color:var(--wt-accent)">Running...</span>';
    pstressPoll=setInterval(function(){pollPipelineStress(dur);},2000);
  }).catch(function(e){
    btn.disabled=false;stopBtn.style.display='none';progress.style.display='none';
    status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
  });
}
function stopPipelineStressTest(){
  fetch('/api/pipeline_stress/stop',{method:'POST'}).then(function(){
    document.getElementById('pstressStatus').innerHTML='<span style="color:var(--wt-warning)">Stopping...</span>';
  });
}
var svcNames=['SIP','IAP','VAD','Whisper','LLaMA','Kokoro','OAP'];
function pollPipelineStress(dur){
  fetch('/api/pipeline_stress/progress').then(function(r){return r.json();}).then(function(d){
    if(d.error){return;}
    var elapsed=d.elapsed_s||0;
    var total=d.duration_s||dur;
    var pct=Math.min(100,Math.round(100*elapsed/total));
    document.getElementById('pstressBar').style.width=pct+'%';
    document.getElementById('pstressElapsed').textContent=elapsed+'s / '+total+'s';
    var cyc=d.cycles_completed||0;
    document.getElementById('pstressCycles').textContent=cyc+' cycles ('+
      (d.cycles_ok||0)+' ok, '+(d.cycles_fail||0)+' fail)';
    var svcs=d.services||[];
    var tbody=document.getElementById('pstressSvcBody');
    var html='';
    for(var i=0;i<svcs.length;i++){
      var s=svcs[i];
      var col=s.reachable?'var(--wt-success)':'var(--wt-danger)';
      html+='<tr><td>'+escapeHtml(s.name)+'</td>'
        +'<td style="color:'+col+';font-weight:bold">'+(s.reachable?'Online':'Offline')+'</td>'
        +'<td>'+s.ping_ok+'</td><td>'+s.ping_fail+'</td>'
        +'<td>'+s.avg_ping_ms+'ms</td><td>'+s.memory_mb+'</td></tr>';
    }
    tbody.innerHTML=html;
    var okCyc=d.cycles_ok||0;
    var avgLat=(okCyc>0)?Math.round((d.total_latency_ms||0)/okCyc):0;
    document.getElementById('pstressThroughput').innerHTML=
      '<strong>Avg E2E latency:</strong> '+avgLat+'ms &nbsp; '
      +'<strong>Min:</strong> '+(d.min_latency_ms>=999999?'-':d.min_latency_ms)+'ms &nbsp; '
      +'<strong>Max:</strong> '+(d.max_latency_ms||0)+'ms &nbsp; '
      +'<strong>Cycles/min:</strong> '+(elapsed>0?((cyc*60/elapsed).toFixed(1)):'0');
    if(!d.running){
      clearInterval(pstressPoll);pstressPoll=null;
      document.getElementById('pstressRunBtn').disabled=false;
      document.getElementById('pstressStopBtn').style.display='none';
      var ok_pct=cyc>0?Math.round(100*(d.cycles_ok||0)/cyc):0;
      var col2=ok_pct>=90?'var(--wt-success)':ok_pct>=70?'var(--wt-warning)':'var(--wt-danger)';
      document.getElementById('pstressStatus').innerHTML=
        '<span style="color:'+col2+';font-weight:bold">Completed: '+ok_pct+'% success</span>'
        +' ('+cyc+' cycles, '+(d.cycles_ok||0)+' ok, '+(d.cycles_fail||0)+' fail, '+elapsed+'s)';
      if(d.result){
        var r2=d.result;
        var rhtml='<h4 style="font-size:14px;font-weight:600;margin:8px 0">Final Summary</h4>'
          +'<table class="wt-table"><tr><th>Metric</th><th>Value</th></tr>'
          +'<tr><td>Total Cycles</td><td>'+cyc+'</td></tr>'
          +'<tr><td>Success Rate</td><td style="color:'+col2+';font-weight:bold">'+ok_pct+'%</td></tr>'
          +'<tr><td>Avg E2E Latency</td><td>'+avgLat+'ms</td></tr>'
          +'<tr><td>Min Latency</td><td>'+(d.min_latency_ms>=999999?'-':d.min_latency_ms)+'ms</td></tr>'
          +'<tr><td>Max Latency</td><td>'+(d.max_latency_ms||0)+'ms</td></tr>'
          +'<tr><td>Throughput</td><td>'+(elapsed>0?((cyc*60/elapsed).toFixed(1)):'0')+' cycles/min</td></tr>'
          +'<tr><td>Duration</td><td>'+elapsed+'s</td></tr>'
          +'<tr><td>Errors</td><td>'+(d.cycles_fail||0)+'</td></tr>'
          +'</table>';
        document.getElementById('pstressResults').innerHTML=rhtml;
      }
    }
  }).catch(function(){});
}

var ttsRoundtripPoll=null;
function runTtsRoundtrip(){
  if(ttsRoundtripPoll){clearInterval(ttsRoundtripPoll);ttsRoundtripPoll=null;}
  var status=document.getElementById('ttsRoundtripStatus');
  var results=document.getElementById('ttsRoundtripResults');
  var btn=document.getElementById('ttsRoundtripBtn');
  var customStr=document.getElementById('ttsRoundtripPhrases').value.trim();
  var body={};
  if(customStr){
    body.phrases=customStr.split(',').map(function(s){return s.trim();}).filter(function(s){return s.length>0;});
  }
  btn.disabled=true;
  status.innerHTML='<span style="color:var(--wt-accent)">Starting TTS round-trip test...</span>';
  results.innerHTML='';
  fetch('/api/tts_roundtrip',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(body)})
    .then(function(r){
      if(r.status===202) return r.json();
      return r.json().then(function(d){throw new Error(d.error||'HTTP '+r.status);});
    }).then(function(d){
      status.innerHTML='<span style="color:var(--wt-accent)">Round-trip test running (task '+d.task_id+')... This may take several minutes.</span>';
      ttsRoundtripPoll=setInterval(function(){pollTtsRoundtripTask(d.task_id);},3000);
    }).catch(function(e){
      btn.disabled=false;
      if(ttsRoundtripPoll){clearInterval(ttsRoundtripPoll);ttsRoundtripPoll=null;}
      status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
    });
}

function pollTtsRoundtripTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(function(r){return r.json();}).then(function(d){
    if(d.status==='running') return;
    clearInterval(ttsRoundtripPoll);ttsRoundtripPoll=null;
    document.getElementById('ttsRoundtripBtn').disabled=false;
    var status=document.getElementById('ttsRoundtripStatus');
    var results=document.getElementById('ttsRoundtripResults');
    if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error)+'</span>';return;}
    var s=d.summary;
    status.innerHTML='<span style="color:var(--wt-success)">Round-trip complete — '+s.pass+'/'+s.total+' passed (L1 avg: '+s.avg_similarity_in.toFixed(1)+'%, L2 avg: '+s.avg_similarity_out.toFixed(1)+'%)</span>';
    var html='<table class="wt-table"><tr><th>Injected Phrase</th><th>Whisper L1</th><th>L1 Sim%</th><th>LLaMA Response</th><th>Whisper L2 (Kokoro)</th><th>L2 Sim%</th><th>WER%</th><th>E2E</th><th>Status</th></tr>';
    d.results.forEach(function(r){
      var color=r.status==='PASS'?'var(--wt-success)':r.status==='WARN'?'var(--wt-warning)':'var(--wt-danger)';
      var inColor=r.similarity_in>=60?'var(--wt-success)':r.similarity_in>=40?'var(--wt-warning)':'var(--wt-danger)';
      var outColor=r.similarity_out>=50?'var(--wt-success)':r.similarity_out>=30?'var(--wt-warning)':'var(--wt-danger)';
      var werColor=(r.wer_out||100)<=10?'var(--wt-success)':(r.wer_out||100)<=30?'var(--wt-warning)':'var(--wt-danger)';
      html+='<tr>';
      html+='<td style="max-width:160px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(r.phrase)+'</td>';
      html+='<td style="max-width:160px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(r.transcription_in||'')+'</td>';
      html+='<td style="color:'+inColor+';font-weight:bold">'+(r.similarity_in||0).toFixed(1)+'%</td>';
      html+='<td style="max-width:180px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(r.llama_response||'')+'</td>';
      html+='<td style="max-width:180px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(r.transcription_out||'')+'</td>';
      html+='<td style="color:'+outColor+';font-weight:bold">'+(r.similarity_out||0).toFixed(1)+'%</td>';
      html+='<td style="color:'+werColor+';font-weight:bold">'+(r.wer_out!=null?r.wer_out.toFixed(1):'—')+'%</td>';
      html+='<td>'+(r.e2e_ms/1000).toFixed(1)+'s</td>';
      html+='<td style="color:'+color+'">'+r.status+'</td>';
      html+='</tr>';
    });
    html+='</table>';
    if(s){
      html+='<div style="margin-top:10px;padding:10px;background:var(--wt-bg);border-radius:4px;font-size:12px">';
      html+='<strong>Summary:</strong> L1 Avg Sim: '+s.avg_similarity_in.toFixed(1)+'%';
      html+=' | L2 Avg Sim (Kokoro quality): '+s.avg_similarity_out.toFixed(1)+'%';
      html+=' | Avg E2E: '+(s.avg_e2e_ms/1000).toFixed(1)+'s';
      html+=' | Pass: '+s.pass+' | Warn: '+s.warn+' | Fail: '+s.fail;
      html+='</div>';
    }
    results.innerHTML=html;
  }).catch(function(e){console.error('pollTtsRoundtripTask',e);});
}

var fullLoopPoll=null;
function runFullLoopTest(){
  if(fullLoopPoll){clearInterval(fullLoopPoll);fullLoopPoll=null;}
  var status=document.getElementById('fullLoopStatus');
  var results=document.getElementById('fullLoopResults');
  var btn=document.getElementById('fullLoopBtn');
  var sel=document.getElementById('fullLoopFiles');
  var files=[];
  for(var i=0;i<sel.options.length;i++){if(sel.options[i].selected)files.push(sel.options[i].value);}
  if(files.length===0){status.innerHTML='<span style="color:var(--wt-danger)">Select at least one test file</span>';return;}
  btn.disabled=true;
  status.innerHTML='<span style="color:var(--wt-accent)">Starting full loop test...</span>';
  results.innerHTML='';
  fetch('/api/full_loop_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({files:files})})
    .then(function(r){
      if(r.status===202) return r.json();
      return r.json().then(function(d){throw new Error(d.error||'HTTP '+r.status);});
    }).then(function(d){
      status.innerHTML='<span style="color:var(--wt-accent)">Full loop test running (task '+d.task_id+')... This may take several minutes.</span>';
      fullLoopPoll=setInterval(function(){pollFullLoopTask(d.task_id);},3000);
    }).catch(function(e){
      btn.disabled=false;
      status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
    });
}

function pollFullLoopTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(function(r){return r.json();}).then(function(d){
    if(d.status==='running') return;
    clearInterval(fullLoopPoll);fullLoopPoll=null;
    document.getElementById('fullLoopBtn').disabled=false;
    var status=document.getElementById('fullLoopStatus');
    var results=document.getElementById('fullLoopResults');
    if(d.error){status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error)+'</span>';return;}
    var s=d.summary;
    status.innerHTML='<span style="color:'+(s.avg_wer<=10?'var(--wt-success)':s.avg_wer<=30?'var(--wt-warning)':'var(--wt-danger)')+'">Full loop complete — '+s.pass+'/'+s.total+' passed | Avg WER: '+s.avg_wer.toFixed(1)+'% | Avg Sim: '+s.avg_similarity.toFixed(1)+'%</span>';
    var html='<table class="wt-table"><tr><th>File</th><th>Whisper L1</th><th>LLaMA Response</th><th>Whisper L2</th><th>WER%</th><th>Sim%</th><th>E2E</th><th>Status</th></tr>';
    d.results.forEach(function(r){
      var color=r.status==='PASS'?'var(--wt-success)':r.status==='WARN'?'var(--wt-warning)':'var(--wt-danger)';
      var werColor=(r.wer||100)<=10?'var(--wt-success)':(r.wer||100)<=30?'var(--wt-warning)':'var(--wt-danger)';
      var simColor=(r.similarity||0)>=70?'var(--wt-success)':(r.similarity||0)>=40?'var(--wt-warning)':'var(--wt-danger)';
      html+='<tr>';
      html+='<td style="font-size:11px">'+escapeHtml(r.file)+'</td>';
      html+='<td style="max-width:140px;overflow:hidden;text-overflow:ellipsis;font-size:11px">'+escapeHtml(r.whisper_l1||'')+'</td>';
      html+='<td style="max-width:160px;overflow:hidden;text-overflow:ellipsis;font-size:11px">'+escapeHtml(r.llama_response||r.error||'')+'</td>';
      html+='<td style="max-width:160px;overflow:hidden;text-overflow:ellipsis;font-size:11px">'+escapeHtml(r.whisper_l2||'')+'</td>';
      html+='<td style="color:'+werColor+';font-weight:bold">'+(r.wer!=null?r.wer.toFixed(1):'—')+'</td>';
      html+='<td style="color:'+simColor+';font-weight:bold">'+(r.similarity!=null?r.similarity.toFixed(1):'—')+'</td>';
      html+='<td>'+((r.e2e_ms||0)/1000).toFixed(1)+'s</td>';
      html+='<td style="color:'+color+'">'+r.status+'</td>';
      html+='</tr>';
    });
    html+='</table>';
    if(s){
      html+='<div style="margin-top:10px;padding:10px;background:var(--wt-bg);border-radius:4px;font-size:12px">';
      html+='<strong>Summary:</strong> Avg WER: '+s.avg_wer.toFixed(1)+'%';
      html+=' | Avg Similarity: '+s.avg_similarity.toFixed(1)+'%';
      html+=' | Avg E2E: '+(s.avg_e2e_ms/1000).toFixed(1)+'s';
      html+=' | Pass (WER&le;10%): '+s.pass+' | Warn: '+s.warn+' | Fail: '+s.fail;
      html+='</div>';
    }
    results.innerHTML=html;
  }).catch(function(e){console.error('pollFullLoopTask',e);});
}

function checkSipProvider(){
  var status=document.getElementById('sipProviderStatus');
  status.innerHTML='<p style="color:var(--wt-accent)">Checking...</p>';
  fetch('http://localhost:'+TSP_PORT+'/status').then(r=>r.json()).then(d=>{
    var html='<p style="color:var(--wt-success)">Test SIP Provider is running</p>';
    html+='<p style="font-size:12px;color:var(--wt-text-secondary)">Call active: '+(d.call_active?'Yes':'No');
    if(d.legs) html+=', Legs: '+d.legs;
    html+='</p>';
    if(d.relay_stats){html+='<p style="font-size:12px;color:var(--wt-text-secondary)">Total pkts: '+d.relay_stats.total_pkts+'</p>';}
    status.innerHTML=html;
  }).catch(e=>{
    status.innerHTML='<p style="color:var(--wt-danger)">Test SIP Provider is NOT running</p>'+
      '<p style="font-size:12px;color:var(--wt-text-secondary)">Start it from the Services page</p>';
  });
}

var testResultsCache=[];
var metricsChart=null;

function refreshTestResults(){
  var serviceFilter=document.getElementById('testResultsService').value;
  var statusFilter=document.getElementById('testResultsStatus').value;
  var url='/api/test_results?service='+encodeURIComponent(serviceFilter)+'&status='+encodeURIComponent(statusFilter);
  fetch(url).then(r=>r.json()).then(d=>{
    testResultsCache=d.results||[];
    displayTestResults(testResultsCache);
  }).catch(e=>console.error('Failed to load test results:',e));
}

function filterTestResults(){
  refreshTestResults();
}

function displayTestResults(results){
  var c=document.getElementById('testResultsContainer');
  if(!results||results.length===0){
    c.innerHTML='<p style="color:var(--wt-text-secondary);font-size:13px">No test results match the filters</p>';
    document.getElementById('testResultsChart').style.display='none';
    return;
  }
  c.innerHTML='<table class="wt-table"><thead><tr><th>Service</th><th>Test Type</th><th>Status</th><th>Timestamp</th><th>Metrics</th></tr></thead><tbody>'+
    results.map(r=>{
      var ts=new Date(r.timestamp*1000).toLocaleString();
      var statusBadge=r.status==='pass'?'<span class="wt-badge wt-badge-success">Pass</span>':'<span class="wt-badge wt-badge-danger">Fail</span>';
      var metricsStr=JSON.stringify(r.metrics).substring(0,100);
      return '<tr><td>'+escapeHtml(r.service)+'</td><td>'+escapeHtml(r.test_type)+'</td><td>'+statusBadge+'</td><td style="font-size:12px">'+ts+'</td>'+
        '<td style="font-family:var(--wt-mono);font-size:11px">'+escapeHtml(metricsStr)+'</td></tr>';
    }).join('')+'</tbody></table>';
  
  if(results.length>0){
    document.getElementById('testResultsChart').style.display='block';
    renderMetricsChart(results);
  }
}

function renderMetricsChart(results){
  var ctx=document.getElementById('metricsChart');
  if(!ctx)return;
  if(metricsChart){metricsChart.destroy();}
  
  var labels=results.map((r,i)=>'Test '+(i+1));
  var latencies=results.map(r=>r.metrics&&r.metrics.latency_ms?r.metrics.latency_ms:0);
  var accuracies=results.map(r=>r.metrics&&r.metrics.accuracy?r.metrics.accuracy:0);
  
  metricsChart=new Chart(ctx,{
    type:'line',
    data:{
      labels:labels,
      datasets:[{
        label:'Latency (ms)',
        data:latencies,
        borderColor:'rgb(0,113,227)',
        backgroundColor:'rgba(0,113,227,0.1)',
        yAxisID:'y'
      },{
        label:'Accuracy (%)',
        data:accuracies,
        borderColor:'rgb(52,199,89)',
        backgroundColor:'rgba(52,199,89,0.1)',
        yAxisID:'y1'
      }]
    },
    options:{
      responsive:true,
      interaction:{mode:'index',intersect:false},
      plugins:{
        tooltip:{
          enabled:true,
          mode:'index',
          intersect:false,
          backgroundColor:'rgba(0,0,0,0.8)',
          titleColor:'#fff',
          bodyColor:'#fff',
          borderColor:'rgba(0,113,227,0.5)',
          borderWidth:1,
          padding:12,
          displayColors:true,
          callbacks:{
            title:function(items){return 'Test: '+items[0].label;},
            label:function(ctx){
              var label=ctx.dataset.label||'';
              if(label)label+=': ';
              label+=ctx.parsed.y.toFixed(2);
              if(ctx.datasetIndex===0)label+=' ms';
              else label+=' %';
              return label;
            }
          }
        },
        zoom:{
          pan:{enabled:true,mode:'x',modifierKey:'shift'},
          zoom:{
            wheel:{enabled:true,speed:0.1},
            pinch:{enabled:true},
            mode:'x'
          },
          limits:{x:{min:'original',max:'original'}}
        }
      },
      scales:{
        y:{type:'linear',display:true,position:'left',title:{display:true,text:'Latency (ms)'}},
        y1:{type:'linear',display:true,position:'right',title:{display:true,text:'Accuracy (%)'},grid:{drawOnChartArea:false}}
      }
    }
  });
}

function exportTestResults(){
  if(testResultsCache.length===0){alert('No test results to export');return;}
  var json=JSON.stringify(testResultsCache,null,2);
  var blob=new Blob([json],{type:'application/json'});
  var url=URL.createObjectURL(blob);
  var a=document.createElement('a');
  a.href=url;
  a.download='test_results_'+new Date().toISOString().replace(/[:.]/g,'-')+'.json';
  a.click();
  URL.revokeObjectURL(url);
}

setInterval(fetchStatus,3000);
setInterval(fetchTests,3000);
setInterval(fetchServices,5000);
fetchStatus();fetchTests();fetchServices();
document.getElementById('statusText').textContent='Port )JS" + port_str + R"JS(';

document.addEventListener('click',function(e){
  if(!e.target.closest('.wt-theme-dropdown')){
    document.getElementById('themeMenu').classList.remove('open');
  }
});

document.getElementById('sqlQuery').addEventListener('keydown',function(e){
  if((e.metaKey||e.ctrlKey)&&e.key==='Enter'){e.preventDefault();runQuery();}
});

var sipLineNames=['alice','bob','charlie','david','eve','frank','george','helen','ivan','julia',
  'karl','laura','max','nina','oscar','petra','quinn','rosa','sam','tina'];

function buildSipLinesGrid(){
  var grid=document.getElementById('sipLinesGrid');
  if(!grid) return;
  var html='';
  html+='<div style="grid-column:1/-1;display:grid;grid-template-columns:60px 60px 1fr;gap:4px;font-size:11px;font-weight:600;color:var(--wt-text-secondary);padding:0 4px">';
  html+='<div>Enable</div><div>Connect</div><div>Line</div></div>';
  for(var i=0;i<20;i++){
    var name=sipLineNames[i];
    var num=i+1;
    html+='<div style="display:grid;grid-template-columns:60px 60px 1fr;gap:4px;align-items:center;padding:4px 4px;border-radius:4px;background:var(--wt-card-hover)" id="sipLine_'+i+'">';
    html+='<div style="text-align:center"><input type="checkbox" id="sipEnable_'+i+'" onchange="onEnableChange('+i+')" title="Enable line '+num+'"></div>';
    html+='<div style="text-align:center"><input type="checkbox" id="sipConnect_'+i+'" disabled title="Connect line '+num+' to conference"></div>';
    html+='<div style="font-size:12px"><span id="sipLineName_'+i+'">'+escapeHtml(name)+'</span> <span id="sipLineStatus_'+i+'" style="color:var(--wt-text-secondary);font-size:10px"></span></div>';
    html+='</div>';
  }
  grid.innerHTML=html;
}

function onEnableChange(idx){
  var en=document.getElementById('sipEnable_'+idx);
  var cn=document.getElementById('sipConnect_'+idx);
  if(en.checked){cn.disabled=false;}else{cn.checked=false;cn.disabled=true;}
}

function enableLinesPreset(count){
  for(var i=0;i<20;i++){
    var en=document.getElementById('sipEnable_'+i);
    var cn=document.getElementById('sipConnect_'+i);
    if(i<count){en.checked=true;cn.disabled=false;}else{en.checked=false;cn.checked=false;cn.disabled=true;}
  }
  applyEnabledLines();
}

function selectAllConnect(){
  for(var i=0;i<20;i++){
    var en=document.getElementById('sipEnable_'+i);
    var cn=document.getElementById('sipConnect_'+i);
    if(en.checked){cn.checked=true;}
  }
}

function deselectAllConnect(){
  for(var i=0;i<20;i++){
    document.getElementById('sipConnect_'+i).checked=false;
  }
}

function applyEnabledLines(){
  var statusDiv=document.getElementById('sipLinesStatus');
  var enabledNames=[];
  for(var i=0;i<20;i++){
    if(document.getElementById('sipEnable_'+i).checked){
      enabledNames.push(sipLineNames[i]);
    }
  }
  statusDiv.innerHTML='<span style="color:var(--wt-warning)">Configuring '+enabledNames.length+' line(s)...</span>';
  fetch('/api/sip/lines').then(r=>r.json()).then(d=>{
    var currentUsers=(d.lines||[]).map(function(l){return l.user;});
    var toAdd=enabledNames.filter(function(n){return currentUsers.indexOf(n)<0;});
    var toRemove=(d.lines||[]).filter(function(l){return enabledNames.indexOf(l.user)<0;});
    var ops=[];
    toRemove.forEach(function(l){
      ops.push(fetch('/api/sip/remove-line',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({index:l.index.toString()})}));
    });
    Promise.all(ops).then(function(){
      var addNext=function(i){
        if(i>=toAdd.length){
          statusDiv.innerHTML='<span style="color:var(--wt-success)">Applied '+enabledNames.length+' line(s)</span>';
          setTimeout(refreshSipPanel,1000);
          return;
        }
        fetch('/api/sip/add-line',{method:'POST',headers:{'Content-Type':'application/json'},
          body:JSON.stringify({user:toAdd[i],server:'127.0.0.1',password:''})
        }).then(function(){setTimeout(function(){addNext(i+1);},200);}).catch(function(){setTimeout(function(){addNext(i+1);},200);});
      };
      addNext(0);
    });
  }).catch(function(e){
    statusDiv.innerHTML='<span style="color:var(--wt-danger)">Error: '+e+'</span>';
  });
}

function refreshSipPanel(){
  var statusDiv=document.getElementById('sipLinesStatus');
  fetch('/api/sip/lines').then(r=>r.json()).then(d=>{
    var lines=d.lines||[];
    var lineUsers=lines.map(function(l){return l.user;});
    var regMap={};
    lines.forEach(function(l){regMap[l.user]=l.registered;});
    for(var i=0;i<20;i++){
      var name=sipLineNames[i];
      var en=document.getElementById('sipEnable_'+i);
      var cn=document.getElementById('sipConnect_'+i);
      var st=document.getElementById('sipLineStatus_'+i);
      if(lineUsers.indexOf(name)>=0){
        en.checked=true;cn.disabled=false;
        st.innerHTML=regMap[name]?'<span style="color:var(--wt-success)">registered</span>':'<span style="color:var(--wt-warning)">pending</span>';
      }else{
        en.checked=false;cn.checked=false;cn.disabled=true;
        st.innerHTML='';
      }
    }
    statusDiv.innerHTML='<span style="color:var(--wt-success)">'+lines.length+' line(s) active</span>';
  }).catch(function(e){
    statusDiv.innerHTML='<span style="color:var(--wt-danger)">SIP Client not reachable</span>';
  });
  fetch('http://localhost:'+TSP_PORT+'/users').then(r=>r.json()).then(d=>{
    var usersDiv=document.getElementById('sipProviderUsers');
    var users=d.users||[];
    if(users.length===0){usersDiv.innerHTML='No users registered at SIP provider';return;}
    usersDiv.innerHTML='SIP Provider: '+users.length+'/'+d.max_lines+' registered — '+users.map(function(u){return u.username;}).join(', ');
  }).catch(function(){
    document.getElementById('sipProviderUsers').innerHTML='SIP provider not reachable';
  });
}

function startConference(){
  var statusDiv=document.getElementById('sipLinesStatus');
  var users=[];
  for(var i=0;i<20;i++){
    if(document.getElementById('sipConnect_'+i).checked){
      users.push(sipLineNames[i]);
    }
  }
  if(users.length<2){statusDiv.innerHTML='<span style="color:var(--wt-danger)">Select at least 2 lines to connect</span>';return;}
  statusDiv.innerHTML='<span style="color:var(--wt-warning)">Starting conference with '+users.length+' lines...</span>';
  fetch('http://localhost:'+TSP_PORT+'/conference',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({users:users})
  }).then(r=>r.json()).then(d=>{
    if(d.success){
      statusDiv.innerHTML='<span style="color:var(--wt-success)">Conference started with '+d.legs+' legs</span>';
    }else{
      statusDiv.innerHTML='<span style="color:var(--wt-danger)">Error: '+(d.error||'Failed')+'</span>';
    }
  }).catch(function(e){
    statusDiv.innerHTML='<span style="color:var(--wt-danger)">SIP provider not reachable</span>';
  });
}

function hangupConference(){
  var statusDiv=document.getElementById('sipLinesStatus');
  fetch('http://localhost:'+TSP_PORT+'/hangup',{method:'POST'}).then(r=>r.json()).then(d=>{
    statusDiv.innerHTML='<span style="color:var(--wt-success)">'+(d.message||'Call ended')+'</span>';
  }).catch(function(e){
    statusDiv.innerHTML='<span style="color:var(--wt-danger)">SIP provider not reachable</span>';
  });
}

var sipRtpTestInterval=null;
function startSipRtpTest(){
  var statusDiv=document.getElementById('sipRtpTestStatus');
  statusDiv.innerHTML='<span style="color:var(--wt-success)">&#x25B6; Test running. Use Services page to start/stop SIP Client and IAP.</span>';
  refreshSipStats();
  if(sipRtpTestInterval)clearInterval(sipRtpTestInterval);
  sipRtpTestInterval=setInterval(refreshSipStats,2000);
}

function stopSipRtpTest(){
  if(sipRtpTestInterval){
    clearInterval(sipRtpTestInterval);
    sipRtpTestInterval=null;
  }
  var statusDiv=document.getElementById('sipRtpTestStatus');
  statusDiv.innerHTML='<span style="color:var(--wt-text-secondary)">&#x25A0; Test stopped</span>';
}

function refreshSipStats(){
  fetch('/api/sip/stats').then(r=>r.json()).then(d=>{
    var tbody=document.getElementById('sipRtpStatsBody');
    var iapStatus=document.getElementById('iapConnectionStatus');
    
    iapStatus.innerHTML=d.downstream_connected?
      '<span style="color:var(--wt-success)">&#x2713; Connected</span>':
      '<span style="color:var(--wt-danger)">&#x2717; Disconnected</span>';
    
    if(!d.calls||d.calls.length===0){
      tbody.innerHTML='<tr><td colspan="7" style="text-align:center;color:var(--wt-text-secondary)">No active calls</td></tr>';
      return;
    }
    
    var html='';
    d.calls.forEach(function(call){
      var fwd=call.rtp_fwd_count||0;
      var disc=call.rtp_discard_count||0;
      html+='<tr>';
      html+='<td>'+call.call_id+'</td>';
      html+='<td>'+call.line_index+'</td>';
      html+='<td>'+call.rtp_rx_count.toLocaleString()+'</td>';
      html+='<td>'+call.rtp_tx_count.toLocaleString()+'</td>';
      html+='<td style="color:var(--wt-success)">'+fwd.toLocaleString()+'</td>';
      html+='<td style="color:'+(disc>0?'var(--wt-danger)':'var(--wt-text-secondary)')+'">'+disc.toLocaleString()+'</td>';
      html+='<td>'+call.duration_sec+'s</td>';
      html+='</tr>';
    });
    tbody.innerHTML=html;
  }).catch(e=>{
    console.error('Failed to fetch SIP stats:',e);
    document.getElementById('iapConnectionStatus').innerHTML='<span style="color:var(--wt-danger)">Error</span>';
  });
}

function runIapQualityTest(){
  var file=document.getElementById('iapTestFileSelect').value;
  if(!file){alert('Please select a test file');return;}
  
  var statusDiv=document.getElementById('iapTestStatus');
  statusDiv.innerHTML='<span style="color:var(--wt-warning)">&#x23F3; Running IAP quality test on '+file+'...</span>';
  
  fetch('/api/iap/quality_test',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({file:file})
  }).then(r=>r.json()).then(d=>{
    if(d.error){
      statusDiv.innerHTML='<span style="color:var(--wt-danger)">&#x2717; Error: '+d.error+'</span>';
      return;
    }
    
    var sc=d.pkt_count?(' ('+d.pkt_count+' packets, '+d.samples_compared.toLocaleString()+' samples)'):'';
    statusDiv.innerHTML='<span style="color:var(--wt-success)">&#x2713; Test completed'+sc+'</span>';
    
    var tbody=document.getElementById('iapResultsBody');
    var statusColor=d.status==='PASS'?'var(--wt-success)':'var(--wt-danger)';
    
    var now=new Date().toLocaleString();
    var html='<tr>';
    html+='<td>'+escapeHtml(d.file)+'</td>';
    html+='<td>'+d.latency_ms.toFixed(4)+'</td>';
    html+='<td>'+(d.max_latency_ms||0).toFixed(4)+'</td>';
    html+='<td>'+d.snr.toFixed(2)+'</td>';
    html+='<td>'+d.rms_error.toFixed(2)+'</td>';
    html+='<td style="color:'+statusColor+';font-weight:600">'+d.status+'</td>';
    html+='<td style="font-size:11px">'+now+'</td>';
    html+='</tr>';
    tbody.innerHTML=html+tbody.innerHTML;
    
    if(!window.iapTestHistory)window.iapTestHistory=[];
    window.iapTestHistory.push({file:d.file,snr:d.snr,rmsError:d.rms_error,latency:d.latency_ms,maxLatency:d.max_latency_ms||0,status:d.status});
    renderIapChart();
    
  }).catch(e=>{
    statusDiv.innerHTML='<span style="color:var(--wt-danger)">&#x2717; Error: '+e+'</span>';
  });
}

function renderIapChart(){
  var container=document.getElementById('iapTestChart');
  if(!window.iapTestHistory||window.iapTestHistory.length===0){container.style.display='none';return;}
  container.style.display='block';
  var ctx=document.getElementById('iapMetricsChart');
  if(window.iapChart)window.iapChart.destroy();
  var labels=window.iapTestHistory.map(function(h){return h.file.replace('.wav','');});
  var snrData=window.iapTestHistory.map(function(h){return h.snr;});
  var rmsData=window.iapTestHistory.map(function(h){return h.rmsError;});
  var colors=window.iapTestHistory.map(function(h){return h.status==='PASS'?'rgba(34,197,94,0.7)':'rgba(239,68,68,0.7)';});
  window.iapChart=new Chart(ctx,{
    type:'bar',
    data:{labels:labels,datasets:[
      {label:'SNR (dB)',data:snrData,backgroundColor:'rgba(59,130,246,0.7)',yAxisID:'y'},
      {label:'RMS Error (%)',data:rmsData,backgroundColor:colors,yAxisID:'y1'}
    ]},
    options:{
      responsive:true,
      interaction:{mode:'index',intersect:false},
      scales:{
        y:{type:'linear',position:'left',title:{display:true,text:'SNR (dB)'}},
        y1:{type:'linear',position:'right',title:{display:true,text:'RMS Error (%)'},grid:{drawOnChartArea:false}}
      },
      plugins:{
        title:{display:true,text:'IAP Codec Quality - Historical Results'},
        legend:{position:'bottom'},
        tooltip:{
          enabled:true,
          mode:'index',
          intersect:false,
          backgroundColor:'rgba(0,0,0,0.8)',
          titleColor:'#fff',
          bodyColor:'#fff',
          borderColor:'rgba(59,130,246,0.5)',
          borderWidth:1,
          padding:12,
          displayColors:true,
          callbacks:{
            title:function(items){return 'File: '+items[0].label+'.wav';},
            label:function(ctx){
              var lbl=ctx.dataset.label||'';
              if(lbl)lbl+=': ';
              lbl+=ctx.parsed.y.toFixed(2);
              if(ctx.datasetIndex===0)lbl+=' dB';
              else lbl+=' %';
              return lbl;
            },
            afterBody:function(items){
              var idx=items[0].dataIndex;
              var h=window.iapTestHistory[idx];
              return ['Avg Latency: '+h.latency.toFixed(4)+' ms','Max Latency: '+h.maxLatency.toFixed(4)+' ms','Status: '+h.status];
            }
          }
        },
        zoom:{
          pan:{enabled:true,mode:'x',modifierKey:'shift'},
          zoom:{
            wheel:{enabled:true,speed:0.1},
            pinch:{enabled:true},
            mode:'x'
          },
          limits:{x:{min:'original',max:'original'}}
        }
      }
    }
  });
}

async function runAllIapQualityTests(){
  var sel=document.getElementById('iapTestFileSelect');
  var files=[];
  for(var i=0;i<sel.options.length;i++){
    if(sel.options[i].value)files.push(sel.options[i].value);
  }
  if(files.length===0){alert('No test files found');return;}
  var statusDiv=document.getElementById('iapTestStatus');
  var tbody=document.getElementById('iapResultsBody');
  tbody.innerHTML='';
  if(!window.iapTestHistory)window.iapTestHistory=[];
  var passed=0,failed=0;
  for(var fi=0;fi<files.length;fi++){
    var file=files[fi];
    statusDiv.innerHTML='<span style="color:var(--wt-warning)">&#x23F3; Testing '+(fi+1)+'/'+files.length+': '+file+'...</span>';
    try{
      let r=await fetch('/api/iap/quality_test',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({file:file})});
      let d=await r.json();
      if(d.error){
        failed++;
        tbody.innerHTML+='<tr><td>'+escapeHtml(file)+'</td><td>-</td><td>-</td><td>-</td><td>-</td><td style="color:var(--wt-danger)">ERROR</td><td>'+d.error+'</td></tr>';
        continue;
      }
      if(d.status==='PASS')passed++;else failed++;
      let sc=d.status==='PASS'?'var(--wt-success)':'var(--wt-danger)';
      let now=new Date().toLocaleString();
      tbody.innerHTML+='<tr><td>'+escapeHtml(d.file)+'</td><td>'+d.latency_ms.toFixed(4)+'</td><td>'+(d.max_latency_ms||0).toFixed(4)+'</td><td>'+d.snr.toFixed(2)+'</td><td>'+d.rms_error.toFixed(2)+'</td><td style="color:'+sc+';font-weight:600">'+d.status+'</td><td style="font-size:11px">'+now+'</td></tr>';
      window.iapTestHistory.push({file:d.file,snr:d.snr,rmsError:d.rms_error,latency:d.latency_ms,maxLatency:d.max_latency_ms||0,status:d.status});
    }catch(e){
      failed++;
      tbody.innerHTML+='<tr><td>'+escapeHtml(file)+'</td><td colspan="6" style="color:var(--wt-danger)">'+e+'</td></tr>';
    }
  }
  statusDiv.innerHTML='<span style="color:var(--wt-success)">&#x2713; All tests complete: '+passed+' passed, '+failed+' failed out of '+files.length+'</span>';
  renderIapChart();
}

function updateVadWindowDisplay(val){
  document.getElementById('vadWindowValue').textContent=val;
}

function updateVadThresholdDisplay(val){
  document.getElementById('vadThresholdValue').textContent=parseFloat(val).toFixed(1);
}

// ===== MODELS PAGE =====

function switchModelTab(tab){
  ['whisper','llama','compare'].forEach(t=>{
    document.getElementById('modelTab'+t.charAt(0).toUpperCase()+t.slice(1)).style.display=(t===tab)?'':'none';
    var btn=document.getElementById('tab'+t.charAt(0).toUpperCase()+t.slice(1));
    if(btn){btn.className='wt-btn '+(t===tab?'wt-btn-primary':'wt-btn-secondary');}
  });
  if(tab==='compare') loadModelComparison();
}

function loadModels(){
  fetch('/api/models').then(r=>r.json()).then(data=>{
    renderModelsTable('whisperModelsTable','whisper',data.whisper||[]);
    renderModelsTable('llamaModelsTable','llama',data.llama||[]);
    populateBenchmarkModelSelect(data.whisper||[]);
    var llamaModelsWithType=(data.llama||[]).map(function(m){m.type='llama';return m;});
    populateLlamaBenchmarkSelect(llamaModelsWithType);
  }).catch(e=>{ console.error('loadModels error',e); });
}

function renderModelsTable(containerId, service, models){
  var el=document.getElementById(containerId);
  if(!models.length){el.innerHTML='<em>No '+service+' models registered yet.</em>';return;}
  var html='<table class="wt-table"><thead><tr>'
    +'<th>Name</th><th>Path</th><th>Backend</th><th>Size (MB)</th><th>Added</th><th>Action</th>'
    +'</tr></thead><tbody>';
  models.forEach(m=>{
    var added=new Date(m.added_timestamp*1000).toLocaleString();
    html+='<tr>'
      +'<td><strong>'+escapeHtml(m.name)+'</strong></td>'
      +'<td style="font-size:11px;word-break:break-all">'+escapeHtml(m.path)+'</td>'
      +'<td>'+escapeHtml(m.backend)+'</td>'
      +'<td>'+m.size_mb+'</td>'
      +'<td style="font-size:11px">'+added+'</td>'
      +'<td><button class="wt-btn wt-btn-sm wt-btn-primary" data-model-id="'+m.id+'" data-model-name="'+escapeHtml(m.name)+'" onclick="selectModelForBenchmark(this.dataset.modelId,this.dataset.modelName)">Benchmark</button></td>'
      +'</tr>';
  });
  html+='</tbody></table>';
  el.innerHTML=html;
}

function populateBenchmarkModelSelect(whisperModels){
  var sel=document.getElementById('benchmarkModelId');
  var current=sel.value;
  sel.innerHTML='<option value="">-- select model --</option>';
  whisperModels.forEach(m=>{
    var opt=document.createElement('option');
    opt.value=m.id;
    opt.textContent=m.name+' ('+m.size_mb+'MB, '+m.backend+')';
    sel.appendChild(opt);
  });
  if(current) sel.value=current;
}

function selectModelForBenchmark(id,name){
  switchModelTab('whisper');
  var sel=document.getElementById('benchmarkModelId');
  sel.value=id;
  if(!sel.value){
    // model not in list yet, reload first
    loadModels();
    setTimeout(()=>{sel.value=id;},500);
  }
  document.getElementById('benchmarkResults').innerHTML='';
  document.getElementById('benchmarkStatus').innerHTML=
    '<span style="color:var(--wt-accent)">Selected: '+escapeHtml(name)+'</span>';
}

function addWhisperModel(){
  var name=document.getElementById('addModelName').value.trim();
  var path=document.getElementById('addModelPath').value.trim();
  var backend=document.getElementById('addModelBackend').value;
  var status=document.getElementById('addModelStatus');
  if(!name||!path){status.innerHTML='<span style="color:var(--wt-danger)">Name and path are required.</span>';return;}
  status.innerHTML='Adding...';
  fetch('/api/models/add',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({service:'whisper',name,path,backend,config:''})})
  .then(r=>r.json()).then(d=>{
    if(d.success){
      status.innerHTML='<span style="color:var(--wt-success)">Registered (id='+d.model_id+')</span>';
      document.getElementById('addModelName').value='';
      document.getElementById('addModelPath').value='';
      loadModels();
    } else {
      status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error||'unknown')+'</span>';
    }
  }).catch(e=>{status.innerHTML='<span style="color:var(--wt-danger)">Request failed: '+e+'</span>';});
}

function addLlamaModel(){
  var name=document.getElementById('addLlamaModelName').value.trim();
  var path=document.getElementById('addLlamaModelPath').value.trim();
  var backend=document.getElementById('addLlamaModelBackend').value;
  var status=document.getElementById('addLlamaModelStatus');
  if(!name||!path){status.innerHTML='<span style="color:var(--wt-danger)">Name and path are required.</span>';return;}
  status.innerHTML='Adding...';
  fetch('/api/models/add',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({service:'llama',name,path,backend,config:''})})
  .then(r=>r.json()).then(d=>{
    if(d.success){
      status.innerHTML='<span style="color:var(--wt-success)">Registered (id='+d.model_id+')</span>';
      document.getElementById('addLlamaModelName').value='';
      document.getElementById('addLlamaModelPath').value='';
      loadModels();
    } else {
      status.innerHTML='<span style="color:var(--wt-danger)">Error: '+escapeHtml(d.error||'unknown')+'</span>';
    }
  }).catch(e=>{status.innerHTML='<span style="color:var(--wt-danger)">Request failed: '+e+'</span>';});
}

var benchmarkPollInterval=null;

function runBenchmark(){
  if(benchmarkPollInterval){clearInterval(benchmarkPollInterval);benchmarkPollInterval=null;}
  var modelId=document.getElementById('benchmarkModelId').value;
  var iterations=parseInt(document.getElementById('benchmarkIterations').value)||1;
  if(!modelId){alert('Please select a model first.');return;}

  // Load test files first if not yet cached, then proceed
  if(!window._testFiles){
    document.getElementById('benchmarkStatus').innerHTML='<span style="color:var(--wt-accent)">Loading test files...</span>';
    fetch('/api/testfiles').then(r=>r.json()).then(d=>{
      window._testFiles=d.files||[];
      runBenchmark();
    });
    return;
  }

  // Collect all test files with ground truth
  var testFiles=[];
  window._testFiles.forEach(f=>{if(f.ground_truth&&f.ground_truth.length>0) testFiles.push(f.name);});
  if(!testFiles.length){
    document.getElementById('benchmarkStatus').innerHTML=
      '<span style="color:var(--wt-danger)">No test files with ground truth found. Check the Beta Testing page.</span>';
    return;
  }

  var btn=document.getElementById('benchmarkRunBtn');
  btn.disabled=true;
  btn.textContent='Running...';
  document.getElementById('benchmarkStatus').innerHTML='<span style="color:var(--wt-accent)">Starting benchmark...</span>';
  document.getElementById('benchmarkResults').innerHTML='';

  fetch('/api/whisper/benchmark',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({model_id:parseInt(modelId),test_files:testFiles,iterations})})
  .then(r=>{
    if(r.status===202) return r.json();
    return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});
  }).then(d=>{
    document.getElementById('benchmarkStatus').innerHTML=
      '<span style="color:var(--wt-accent)">Benchmark running (task '+d.task_id+', '+testFiles.length+' files × '+iterations+' iterations)...</span>';
    benchmarkPollInterval=setInterval(()=>pollBenchmarkTask(d.task_id),2000);
  }).catch(e=>{
    if(benchmarkPollInterval){clearInterval(benchmarkPollInterval);benchmarkPollInterval=null;}
    btn.disabled=false;btn.textContent='▶ Run Benchmark';
    document.getElementById('benchmarkStatus').innerHTML=
      '<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
  });
}

function pollBenchmarkTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(r=>r.json()).then(d=>{
    if(d.status==='running') return;
    clearInterval(benchmarkPollInterval);
    var btn=document.getElementById('benchmarkRunBtn');
    btn.disabled=false;btn.textContent='▶ Run Benchmark';
    if(d.error){
      document.getElementById('benchmarkStatus').innerHTML=
        '<span style="color:var(--wt-danger)">Benchmark failed: '+escapeHtml(d.error)+'</span>';
      return;
    }
    document.getElementById('benchmarkStatus').innerHTML=
      '<span style="color:var(--wt-success)">&#x2713; Benchmark complete</span>';
    renderBenchmarkResults(d);
    loadModelComparison();
  }).catch(e=>console.error('pollBenchmarkTask',e));
}

function renderBenchmarkResults(r){
  var passColor=r.pass_count>0?'var(--wt-success)':'var(--wt-text-muted)';
  var failColor=r.fail_count>0?'var(--wt-danger)':'var(--wt-text-muted)';
  var html='<div style="display:grid;grid-template-columns:repeat(4,1fr);gap:8px">'
    +'<div class="wt-card" style="padding:12px;text-align:center">'
    +'<div style="font-size:24px;font-weight:700">'+r.avg_accuracy.toFixed(1)+'%</div>'
    +'<div style="font-size:11px;color:var(--wt-text-muted)">Avg Accuracy</div></div>'
    +'<div class="wt-card" style="padding:12px;text-align:center">'
    +'<div style="font-size:24px;font-weight:700">'+r.avg_latency_ms.toFixed(0)+'ms</div>'
    +'<div style="font-size:11px;color:var(--wt-text-muted)">Avg Latency</div></div>'
    +'<div class="wt-card" style="padding:12px;text-align:center">'
    +'<div style="font-size:24px;font-weight:700;color:'+passColor+'">'+r.pass_count+'</div>'
    +'<div style="font-size:11px;color:var(--wt-text-muted)">PASS (≥95%)</div></div>'
    +'<div class="wt-card" style="padding:12px;text-align:center">'
    +'<div style="font-size:24px;font-weight:700;color:'+failColor+'">'+r.fail_count+'</div>'
    +'<div style="font-size:11px;color:var(--wt-text-muted)">FAIL (<95%)</div></div>'
    +'</div>'
    +'<div style="font-size:12px;color:var(--wt-text-muted);margin-top:8px">'
    +'P50: '+r.p50_latency_ms+'ms &nbsp; P95: '+r.p95_latency_ms+'ms &nbsp; P99: '+r.p99_latency_ms+'ms'
    +' &nbsp;|&nbsp; Memory: '+r.memory_mb+'MB &nbsp;|&nbsp; Files: '+r.files_tested
    +'</div>';
  document.getElementById('benchmarkResults').innerHTML=html;
}

var _hfSearchGen=0;
function searchHuggingFace(){
  var btn=document.getElementById('hfSearchBtn');
  var statusEl=document.getElementById('hfSearchStatus');
  var resultsEl=document.getElementById('hfSearchResults');
  btn.disabled=true;
  statusEl.innerHTML='<span style="color:var(--wt-accent)">Searching HuggingFace...</span>';
  resultsEl.innerHTML='';
  var query=document.getElementById('hfSearchQuery').value.trim();
  var task=document.getElementById('hfSearchTask').value;
  var sort=document.getElementById('hfSearchSort').value;
  var gen=++_hfSearchGen;
  fetch('/api/models/search',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({query:query,task:task,sort:sort,limit:20})})
  .then(r=>r.json()).then(data=>{
    if(gen!==_hfSearchGen) return;
    btn.disabled=false;
    if(data.error){
      statusEl.innerHTML='<span style="color:var(--wt-danger)">'+escapeHtml(data.error)+'</span>'
        +(data.has_token?'':' <em>(No HF token set - go to Credentials page)</em>');
      return;
    }
    var models=data.models||[];
    statusEl.innerHTML='<span style="color:var(--wt-success)">Found '+models.length+' models</span>'
      +(data.has_token?'':' <em style="color:var(--wt-warning)">(No HF token - some gated models may be inaccessible)</em>');
    if(!models.length){resultsEl.innerHTML='<em>No models found.</em>';return;}
    var html='<table class="wt-table"><thead><tr>'
      +'<th>Model</th><th>Downloads</th><th>Likes</th><th>Tags</th><th>Updated</th><th>Action</th>'
      +'</tr></thead><tbody>';
    window._hfSearchModels=models;
    models.forEach(function(m,idx){
      var id=m.modelId||m.id||'';
      var dl=m.downloads||0;
      var likes=m.likes||0;
      var tags=(m.tags||[]).slice(0,5).join(', ');
      var updated=m.lastModified?(new Date(m.lastModified)).toLocaleDateString():'';
      html+='<tr>'
        +'<td><a href="https://huggingface.co/'+escapeHtml(id)+'" target="_blank" style="color:var(--wt-accent)"><strong>'+escapeHtml(id)+'</strong></a></td>'
        +'<td>'+formatNumber(dl)+'</td>'
        +'<td>'+likes+'</td>'
        +'<td style="font-size:11px;max-width:200px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(tags)+'</td>'
        +'<td style="font-size:11px">'+updated+'</td>'
        +'<td><button class="wt-btn wt-btn-sm wt-btn-primary" data-idx="'+idx+'" onclick="showDownloadDialog(parseInt(this.dataset.idx))">Download</button></td>'
        +'</tr>';
    });
    html+='</tbody></table>';
    resultsEl.innerHTML=html;
  }).catch(e=>{
    if(gen!==_hfSearchGen) return;
    btn.disabled=false;
    statusEl.innerHTML='<span style="color:var(--wt-danger)">Search failed: '+escapeHtml(String(e))+'</span>';
  });
}

function formatNumber(n){
  if(n>=1000000) return (n/1000000).toFixed(1)+'M';
  if(n>=1000) return (n/1000).toFixed(1)+'K';
  return String(n);
}

function showDownloadDialog(idx,serviceType){
  serviceType=serviceType||'whisper';
  var models=serviceType==='llama'?(window._hfLlamaSearchModels||[]):(window._hfSearchModels||[]);
  var m=models[idx];
  if(!m) return;
  var repoId=m.modelId||m.id||'';
  var existing=document.getElementById('dlModal');
  if(existing) existing.remove();
  var modal=document.createElement('div');
  modal.id='dlModal';
  modal.style.cssText='position:fixed;top:0;left:0;width:100%;height:100%;background:rgba(0,0,0,0.5);z-index:9999;display:flex;align-items:center;justify-content:center';
  modal.dataset.repoId=repoId;
  modal.dataset.serviceType=serviceType;
  var backendOpts=serviceType==='llama'
    ?'<option value="metal">Metal GPU</option><option value="cpu">CPU only</option>'
    :'<option value="coreml">CoreML (Apple Silicon)</option><option value="metal">Metal GPU</option><option value="cpu">CPU only</option>';
  var fileHint=serviceType==='llama'?'e.g. model-q8_0.gguf':'e.g. ggml-model.bin';
  modal.innerHTML='<div style="background:var(--wt-card-bg);border-radius:var(--wt-radius);padding:24px;width:480px;max-width:90vw;box-shadow:0 8px 32px rgba(0,0,0,0.3)">'
    +'<h3 style="margin:0 0 16px">Download '+serviceType.toUpperCase()+' model from '+escapeHtml(repoId)+'</h3>'
    +'<div class="wt-field"><label>Filename</label>'
    +'<input class="wt-input" id="dlFilename" placeholder="'+fileHint+'" value=""></div>'
    +'<div class="wt-field"><label>Display Name</label>'
    +'<input class="wt-input" id="dlModelName" placeholder="Model display name" value=""></div>'
    +'<div class="wt-field"><label>Backend</label>'
    +'<select class="wt-select" id="dlBackend">'+backendOpts+'</select></div>'
    +'<div id="dlModalError" style="color:var(--wt-danger);font-size:12px;margin-bottom:8px"></div>'
    +'<div style="display:flex;gap:8px;justify-content:flex-end">'
    +'<button class="wt-btn wt-btn-secondary" onclick="document.getElementById(\'dlModal\').remove()">Cancel</button>'
    +'<button class="wt-btn wt-btn-primary" onclick="submitDownload()">Download</button>'
    +'</div></div>';
  document.body.appendChild(modal);
  modal.addEventListener('click',function(e){if(e.target===modal)modal.remove();});
  document.getElementById('dlFilename').focus();
}

function submitDownload(){
  var modal=document.getElementById('dlModal');
  if(!modal) return;
  var repoId=modal.dataset.repoId||'';
  var serviceType=modal.dataset.serviceType||'whisper';
  var filename=(document.getElementById('dlFilename').value||'').trim();
  var modelName=(document.getElementById('dlModelName').value||'').trim();
  var backend=document.getElementById('dlBackend').value;
  var errEl=document.getElementById('dlModalError');
  if(!filename){errEl.textContent='Filename is required.';return;}
  if(/[^A-Za-z0-9._-]/.test(filename)){errEl.textContent='Filename must only contain alphanumeric, dash, underscore, dot.';return;}
  if(!modelName) modelName=filename.replace(/\\.bin$/,'').replace(/\\.gguf$/,'');
  modal.remove();
  startModelDownload(repoId,filename,modelName,backend,serviceType);
}

var activeDownloads={};

function startModelDownload(repoId,filename,modelName,backend,serviceType){
  serviceType=serviceType||'whisper';
  var statusEl=document.getElementById(serviceType==='llama'?'hfLlamaSearchStatus':'hfSearchStatus');
  statusEl.innerHTML='<span style="color:var(--wt-accent)">Starting download of '+escapeHtml(filename)+'...</span>';
  fetch('/api/models/download',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({repo_id:repoId,filename:filename,model_name:modelName,backend:backend,service:serviceType})})
  .then(r=>r.json()).then(data=>{
    if(data.error){
      statusEl.innerHTML='<span style="color:var(--wt-danger)">'+escapeHtml(data.error)+'</span>';
      return;
    }
    var dlId=data.download_id;
    activeDownloads[dlId]={filename:filename,repoId:repoId,serviceType:serviceType};
    statusEl.innerHTML='<span style="color:var(--wt-accent)">Downloading '+escapeHtml(filename)+' (ID: '+dlId+')...</span>'
      +'<div id="dlProgress_'+dlId+'" style="margin-top:4px"><div class="progress" style="height:20px;background:var(--wt-border);border-radius:4px;overflow:hidden">'
      +'<div id="dlBar_'+dlId+'" style="height:100%;background:var(--wt-accent);transition:width 0.5s;width:0%"></div>'
      +'</div><span id="dlPctText_'+dlId+'" style="font-size:11px">0%</span></div>';
    pollDownloadProgress(dlId);
  }).catch(e=>{
    statusEl.innerHTML='<span style="color:var(--wt-danger)">Download failed: '+escapeHtml(String(e))+'</span>';
  });
}

function pollDownloadProgress(dlId){
  var iv=setInterval(function(){
    fetch('/api/models/download/progress?id='+dlId).then(r=>r.json()).then(data=>{
      var bar=document.getElementById('dlBar_'+dlId);
      var pctText=document.getElementById('dlPctText_'+dlId);
      if(!bar) {clearInterval(iv);return;}
      var pct=0;
      if(data.total_bytes>0){
        pct=Math.min(100,Math.round(data.bytes_downloaded/data.total_bytes*100));
      } else if(data.bytes_downloaded>0){
        pct=50;
      }
      bar.style.width=pct+'%';
      var mbDl=(data.bytes_downloaded/1048576).toFixed(1);
      var mbTotal=data.total_bytes>0?((data.total_bytes/1048576).toFixed(1)+'MB'):'?';
      pctText.textContent=mbDl+'MB / '+mbTotal+(data.total_bytes>0?' ('+pct+'%)':'');
      if(data.complete||data.failed){
        clearInterval(iv);
        var svcType=(activeDownloads[dlId]||{}).serviceType||'whisper';
        var statusEl=document.getElementById(svcType==='llama'?'hfLlamaSearchStatus':'hfSearchStatus');
        if(data.failed){
          statusEl.innerHTML='<span style="color:var(--wt-danger)">Download failed: '+escapeHtml(data.error||'Unknown error')+'</span>';
        } else {
          bar.style.width='100%';
          pctText.textContent=mbDl+'MB - Complete!';
          statusEl.innerHTML='<span style="color:var(--wt-success)">Downloaded and registered: '+escapeHtml(data.filename)+'</span>';
          loadModels();
        }
        delete activeDownloads[dlId];
      }
    }).catch(function(){});
  },1000);
}

function loadModelComparison(){
  var filterType=(document.getElementById('compFilterType')||{}).value||'';
  fetch('/api/models/benchmarks').then(r=>r.json()).then(data=>{
    var runs=data.runs||[];
    if(filterType) runs=runs.filter(r=>(r.model_type||'whisper')===filterType);
    renderComparisonTable(runs);
    renderComparisonCharts(runs);
  }).catch(e=>console.error('loadModelComparison',e));
}

function renderComparisonTable(runs){
  var el=document.getElementById('comparisonTable');
  if(!runs.length){el.innerHTML='<em>No benchmark runs yet.</em>';return;}
  var html='<table class="wt-table"><thead><tr>'
    +'<th>Model</th><th>Type</th><th>Backend</th><th>Score %</th>'
    +'<th>Avg Latency</th><th>P50</th><th>P95</th><th>Memory</th><th>Extra</th><th>Date</th>'
    +'</tr></thead><tbody>';
  runs.forEach(r=>{
    var accColor=r.avg_accuracy>=95?'var(--wt-success)':r.avg_accuracy>=80?'var(--wt-warning)':'var(--wt-danger)';
    var date=new Date(r.timestamp*1000).toLocaleString();
    var typeLabel=(r.model_type||'whisper').toUpperCase();
    var extra='';
    if(r.model_type==='llama'){
      extra='DE:'+((r.german_pct||0).toFixed(0))+'% Int:'+(r.interrupt_latency_ms||0).toFixed(0)+'ms';
    } else {
      extra='P:'+(r.pass_count||0)+' F:'+(r.fail_count||0);
    }
    html+='<tr>'
      +'<td><strong>'+escapeHtml(r.model_name)+'</strong></td>'
      +'<td><span style="font-size:10px;padding:2px 6px;border-radius:3px;background:'+(r.model_type==='llama'?'#7c3aed':'#2563eb')+';color:#fff">'+typeLabel+'</span></td>'
      +'<td>'+escapeHtml(r.backend)+'</td>'
      +'<td style="color:'+accColor+';font-weight:700">'+r.avg_accuracy.toFixed(1)+'%</td>'
      +'<td>'+r.avg_latency_ms+'ms</td>'
      +'<td>'+r.p50_latency_ms+'ms</td>'
      +'<td>'+r.p95_latency_ms+'ms</td>'
      +'<td>'+r.memory_mb+'MB</td>'
      +'<td style="font-size:11px">'+extra+'</td>'
      +'<td style="font-size:11px">'+date+'</td>'
      +'</tr>';
  });
  html+='</tbody></table>';
  el.innerHTML=html;
}

var compCharts={accuracy:null,latency:null,size:null,scatter:null,german:null,interrupt:null,tokens:null,qualityScatter:null};

function destroyCompCharts(){
  Object.keys(compCharts).forEach(k=>{
    if(compCharts[k]){compCharts[k].destroy();compCharts[k]=null;}
  });
}

function renderComparisonCharts(runs){
  destroyCompCharts();
  if(!runs.length) return;
  var byModel={};
  runs.forEach(r=>{if(!byModel[r.model_name]) byModel[r.model_name]=r;});
  var labels=Object.keys(byModel);
  var whisperColors=['rgba(59,130,246,0.7)','rgba(34,197,94,0.7)','rgba(14,165,233,0.7)','rgba(6,182,212,0.7)'];
  var llamaColors_=['rgba(168,85,247,0.7)','rgba(124,58,237,0.7)','rgba(192,132,252,0.7)','rgba(139,92,246,0.7)'];
  var bgColors=labels.map(function(_,i){
    var r=byModel[labels[i]];
    if((r.model_type||'whisper')==='llama') return llamaColors_[i%llamaColors_.length];
    return whisperColors[i%whisperColors.length];
  });

  var accCanvas=document.getElementById('compAccuracyChart');
  if(accCanvas){
    compCharts.accuracy=new Chart(accCanvas,{
      type:'bar',
      data:{labels:labels,datasets:[{
        label:'Score (%)',
        data:labels.map(n=>byModel[n].avg_accuracy),
        backgroundColor:bgColors,
        borderRadius:4
      }]},
      options:{responsive:true,plugins:{legend:{display:false},
        tooltip:{callbacks:{label:function(ctx){
          var n=labels[ctx.dataIndex];var r=byModel[n];
          var t=(r.model_type||'whisper')==='llama'?'Quality':'Accuracy';
          return n+': '+ctx.raw.toFixed(1)+'% ('+t+')';
        }}}},
        scales:{y:{beginAtZero:true,max:100,title:{display:true,text:'Score (%)'}}}}
    });
  }

  var latCanvas=document.getElementById('compLatencyChart');
  if(latCanvas){
    compCharts.latency=new Chart(latCanvas,{
      type:'bar',
      data:{labels:labels,datasets:[
        {label:'P50 (ms)',data:labels.map(n=>byModel[n].p50_latency_ms),backgroundColor:'rgba(59,130,246,0.7)',borderRadius:4},
        {label:'P95 (ms)',data:labels.map(n=>byModel[n].p95_latency_ms),backgroundColor:'rgba(251,146,60,0.7)',borderRadius:4},
        {label:'P99 (ms)',data:labels.map(n=>byModel[n].p99_latency_ms),backgroundColor:'rgba(239,68,68,0.7)',borderRadius:4}
      ]},
      options:{responsive:true,scales:{y:{beginAtZero:true,title:{display:true,text:'Latency (ms)'}}}}
    });
  }

  var sizeCanvas=document.getElementById('compSizeChart');
  if(sizeCanvas){
    compCharts.size=new Chart(sizeCanvas,{
      type:'bar',
      data:{labels:labels,datasets:[{
        label:'Size (MB)',
        data:labels.map(n=>byModel[n].memory_mb),
        backgroundColor:bgColors,
        borderRadius:4
      }]},
      options:{responsive:true,plugins:{legend:{display:false}},
        indexAxis:'y',
        scales:{x:{beginAtZero:true,title:{display:true,text:'Size (MB)'}}}}
    });
  }

  var scatterCanvas=document.getElementById('compScatterChart');
  if(scatterCanvas){
    var scatterData=labels.map((n,i)=>({
      x:byModel[n].p50_latency_ms,
      y:byModel[n].avg_accuracy,
      label:n
    }));
    compCharts.scatter=new Chart(scatterCanvas,{
      type:'scatter',
      data:{datasets:[{
        label:'Models',
        data:scatterData,
        backgroundColor:bgColors,
        pointRadius:8,
        pointHoverRadius:12
      }]},
      options:{responsive:true,
        plugins:{tooltip:{callbacks:{
          label:function(ctx){return ctx.raw.label+': '+ctx.raw.x+'ms, '+ctx.raw.y.toFixed(1)+'%';}
        }}},
        scales:{
          x:{title:{display:true,text:'P50 Latency (ms)'},beginAtZero:true},
          y:{title:{display:true,text:'Score (%)'},min:0,max:100}
        }
      }
    });
  }

  var llamaRuns=runs.filter(r=>(r.model_type||'whisper')==='llama');
  var llamaChartsEl=document.getElementById('compLlamaCharts');
  if(llamaChartsEl) llamaChartsEl.style.display=llamaRuns.length>0?'block':'none';
  if(llamaRuns.length===0) return;

  var llamaByModel={};
  llamaRuns.forEach(r=>{if(!llamaByModel[r.model_name]) llamaByModel[r.model_name]=r;});
  var llamaLabels=Object.keys(llamaByModel);
  var llamaColors=llamaLabels.map((_,i)=>llamaColors_[i%llamaColors_.length]);

  var germanCanvas=document.getElementById('compGermanChart');
  if(germanCanvas){
    compCharts.german=new Chart(germanCanvas,{
      type:'bar',
      data:{labels:llamaLabels,datasets:[{
        label:'German %',
        data:llamaLabels.map(n=>(llamaByModel[n].german_pct||0)),
        backgroundColor:llamaColors,
        borderRadius:4
      }]},
      options:{responsive:true,plugins:{legend:{display:false}},
        scales:{y:{beginAtZero:true,max:100,title:{display:true,text:'German Compliance (%)'}}}}
    });
  }

  var intCanvas=document.getElementById('compInterruptChart');
  if(intCanvas){
    compCharts.interrupt=new Chart(intCanvas,{
      type:'bar',
      data:{labels:llamaLabels,datasets:[{
        label:'Interrupt (ms)',
        data:llamaLabels.map(n=>(llamaByModel[n].interrupt_latency_ms||0)),
        backgroundColor:llamaColors,
        borderRadius:4
      }]},
      options:{responsive:true,plugins:{legend:{display:false}},
        scales:{y:{beginAtZero:true,title:{display:true,text:'Interrupt Latency (ms)'}}}}
    });
  }

  var tokCanvas=document.getElementById('compTokensChart');
  if(tokCanvas){
    compCharts.tokens=new Chart(tokCanvas,{
      type:'bar',
      data:{labels:llamaLabels,datasets:[{
        label:'Avg Words',
        data:llamaLabels.map(n=>(llamaByModel[n].avg_tokens||0)),
        backgroundColor:llamaColors,
        borderRadius:4
      }]},
      options:{responsive:true,plugins:{legend:{display:false}},
        scales:{y:{beginAtZero:true,title:{display:true,text:'Avg Words / Response'}}}}
    });
  }

  var qsCanvas=document.getElementById('compQualityScatterChart');
  if(qsCanvas){
    var qsData=llamaLabels.map((n,i)=>({
      x:llamaByModel[n].avg_latency_ms||llamaByModel[n].p50_latency_ms,
      y:llamaByModel[n].avg_accuracy,
      label:n
    }));
    compCharts.qualityScatter=new Chart(qsCanvas,{
      type:'scatter',
      data:{datasets:[{
        label:'LLaMA Models',
        data:qsData,
        backgroundColor:llamaColors,
        pointRadius:8,
        pointHoverRadius:12
      }]},
      options:{responsive:true,
        plugins:{tooltip:{callbacks:{
          label:function(ctx){return ctx.raw.label+': '+ctx.raw.x+'ms, '+ctx.raw.y.toFixed(1)+'%';}
        }}},
        scales:{
          x:{title:{display:true,text:'Avg Latency (ms)'},beginAtZero:true},
          y:{title:{display:true,text:'Quality Score (%)'},min:0,max:100}
        }
      }
    });
  }
}

var llamaBenchmarkPollInterval=null;

function populateLlamaBenchmarkSelect(models){
  var sel=document.getElementById('llamaBenchmarkModelId');
  if(!sel) return;
  var cur=sel.value;
  sel.innerHTML='<option value="">-- select model --</option>';
  models.filter(function(m){return m.type==='llama';}).forEach(function(m){
    var opt=document.createElement('option');
    opt.value=m.id;
    opt.textContent=m.name+' ('+m.backend+')';
    sel.appendChild(opt);
  });
  if(cur) sel.value=cur;
}

function runLlamaBenchmark(){
  if(llamaBenchmarkPollInterval){clearInterval(llamaBenchmarkPollInterval);llamaBenchmarkPollInterval=null;}
  var modelId=document.getElementById('llamaBenchmarkModelId').value;
  var iterations=parseInt(document.getElementById('llamaBenchmarkIterations').value)||1;
  if(!modelId){alert('Please select a LLaMA model first.');return;}
  var btn=document.getElementById('llamaBenchmarkRunBtn');
  btn.disabled=true;btn.textContent='Running...';
  document.getElementById('llamaBenchmarkStatus').innerHTML='<span style="color:var(--wt-accent)">Starting LLaMA benchmark...</span>';
  document.getElementById('llamaBenchmarkResults').innerHTML='';
  fetch('/api/llama/benchmark',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({model_id:parseInt(modelId),iterations:iterations})})
  .then(r=>{
    if(r.status===202) return r.json();
    return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});
  }).then(d=>{
    document.getElementById('llamaBenchmarkStatus').innerHTML=
      '<span style="color:var(--wt-accent)">Benchmark running (task '+d.task_id+')...</span>';
    llamaBenchmarkPollInterval=setInterval(()=>pollLlamaBenchmarkTask(d.task_id),2000);
  }).catch(e=>{
    if(llamaBenchmarkPollInterval){clearInterval(llamaBenchmarkPollInterval);llamaBenchmarkPollInterval=null;}
    btn.disabled=false;btn.textContent='\u25B6 Run Benchmark';
    document.getElementById('llamaBenchmarkStatus').innerHTML=
      '<span style="color:var(--wt-danger)">Error: '+escapeHtml(String(e))+'</span>';
  });
}

function pollLlamaBenchmarkTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(r=>r.json()).then(d=>{
    if(d.status==='running') return;
    clearInterval(llamaBenchmarkPollInterval);
    var btn=document.getElementById('llamaBenchmarkRunBtn');
    btn.disabled=false;btn.textContent='\u25B6 Run Benchmark';
    if(d.error){
      document.getElementById('llamaBenchmarkStatus').innerHTML=
        '<span style="color:var(--wt-danger)">Benchmark failed: '+escapeHtml(d.error)+'</span>';
      return;
    }
    document.getElementById('llamaBenchmarkStatus').innerHTML=
      '<span style="color:var(--wt-success)">\u2713 LLaMA Benchmark complete</span>';
    renderLlamaBenchmarkResults(d);
    loadModelComparison();
  }).catch(e=>console.error('pollLlamaBenchmarkTask',e));
}

function renderLlamaBenchmarkResults(r){
  var html='<div style="display:grid;grid-template-columns:repeat(4,1fr);gap:8px">'
    +'<div class="wt-card" style="padding:12px;text-align:center">'
    +'<div style="font-size:24px;font-weight:700">'+r.avg_score.toFixed(1)+'%</div>'
    +'<div style="font-size:11px;color:var(--wt-text-muted)">Avg Quality Score</div></div>'
    +'<div class="wt-card" style="padding:12px;text-align:center">'
    +'<div style="font-size:24px;font-weight:700">'+r.avg_latency_ms.toFixed(0)+'ms</div>'
    +'<div style="font-size:11px;color:var(--wt-text-muted)">Avg Latency</div></div>'
    +'<div class="wt-card" style="padding:12px;text-align:center">'
    +'<div style="font-size:24px;font-weight:700">'+r.german_pct.toFixed(0)+'%</div>'
    +'<div style="font-size:11px;color:var(--wt-text-muted)">German Compliance</div></div>'
    +'<div class="wt-card" style="padding:12px;text-align:center">'
    +'<div style="font-size:24px;font-weight:700">'+r.avg_tokens.toFixed(1)+'</div>'
    +'<div style="font-size:11px;color:var(--wt-text-muted)">Avg Words</div></div>'
    +'</div>'
    +'<div style="font-size:12px;color:var(--wt-text-muted);margin-top:8px">'
    +'P50: '+r.p50_latency_ms+'ms &nbsp; P95: '+r.p95_latency_ms+'ms'
    +' &nbsp;|&nbsp; Interrupt: '+r.interrupt_latency_ms+'ms &nbsp;|&nbsp; Prompts: '+r.prompts_tested
    +'</div>';
  document.getElementById('llamaBenchmarkResults').innerHTML=html;
}

var _hfLlamaSearchGen=0;
function searchHuggingFaceLlama(){
  var btn=document.getElementById('hfLlamaSearchBtn');
  var statusEl=document.getElementById('hfLlamaSearchStatus');
  var resultsEl=document.getElementById('hfLlamaSearchResults');
  btn.disabled=true;
  statusEl.innerHTML='<span style="color:var(--wt-accent)">Searching HuggingFace for LLaMA models...</span>';
  resultsEl.innerHTML='';
  var query=document.getElementById('hfLlamaSearchQuery').value.trim();
  var sort=document.getElementById('hfLlamaSearchSort').value;
  var gen=++_hfLlamaSearchGen;
  fetch('/api/models/search',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({query:query,task:'text-generation',sort:sort,limit:20})})
  .then(r=>r.json()).then(data=>{
    if(gen!==_hfLlamaSearchGen) return;
    btn.disabled=false;
    if(data.error){
      statusEl.innerHTML='<span style="color:var(--wt-danger)">'+escapeHtml(data.error)+'</span>'
        +(data.has_token?'':' <em>(No HF token set - go to Credentials page)</em>');
      return;
    }
    var models=data.models||[];
    statusEl.innerHTML='<span style="color:var(--wt-success)">Found '+models.length+' models</span>'
      +(data.has_token?'':' <em style="color:var(--wt-warning)">(No HF token - some gated models may be inaccessible)</em>');
    if(!models.length){resultsEl.innerHTML='<em>No models found.</em>';return;}
    var html='<table class="wt-table"><thead><tr>'
      +'<th>Model</th><th>Downloads</th><th>Likes</th><th>Tags</th><th>Updated</th><th>Action</th>'
      +'</tr></thead><tbody>';
    window._hfLlamaSearchModels=models;
    models.forEach(function(m,idx){
      var id=m.modelId||m.id||'';
      var dl=m.downloads||0;
      var likes=m.likes||0;
      var tags=(m.tags||[]).slice(0,5).join(', ');
      var updated=m.lastModified?(new Date(m.lastModified)).toLocaleDateString():'';
      html+='<tr>'
        +'<td><a href="https://huggingface.co/'+escapeHtml(id)+'" target="_blank" style="color:var(--wt-accent)"><strong>'+escapeHtml(id)+'</strong></a></td>'
        +'<td>'+formatNumber(dl)+'</td>'
        +'<td>'+likes+'</td>'
        +'<td style="font-size:11px;max-width:200px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(tags)+'</td>'
        +'<td style="font-size:11px">'+updated+'</td>'
        +'<td><button class="wt-btn wt-btn-sm wt-btn-primary" data-idx="'+idx+'" onclick="showDownloadDialog(parseInt(this.dataset.idx),\'llama\')">Download</button></td>'
        +'</tr>';
    });
    html+='</tbody></table>';
    resultsEl.innerHTML=html;
  }).catch(e=>{
    if(gen!==_hfLlamaSearchGen) return;
    btn.disabled=false;
    statusEl.innerHTML='<span style="color:var(--wt-danger)">Search failed: '+escapeHtml(String(e))+'</span>';
  });
}

// ===== END MODELS PAGE =====

function loadVadConfig(){
  fetch('/api/vad/config').then(r=>r.json()).then(d=>{
    document.getElementById('vadWindowSlider').value=d.window_ms;
    document.getElementById('vadThresholdSlider').value=d.threshold;
    document.getElementById('vadSilenceSlider').value=d.silence_ms||400;
    document.getElementById('vadMaxChunkSlider').value=d.max_chunk_ms||8000;
    updateVadWindowDisplay(d.window_ms);
    updateVadThresholdDisplay(d.threshold);
    document.getElementById('vadSilenceValue').textContent=d.silence_ms||400;
    document.getElementById('vadMaxChunkValue').textContent=d.max_chunk_ms||8000;
    document.getElementById('currentVadWindow').textContent=d.window_ms;
    document.getElementById('currentVadThreshold').textContent=d.threshold;
    document.getElementById('currentVadSilence').textContent=d.silence_ms||400;
    document.getElementById('currentVadMaxChunk').textContent=d.max_chunk_ms||8000;
  }).catch(e=>console.error('Failed to load VAD config:',e));
}

function saveVadConfig(){
  var window_ms=document.getElementById('vadWindowSlider').value;
  var threshold=document.getElementById('vadThresholdSlider').value;
  var silence_ms=document.getElementById('vadSilenceSlider').value;
  var max_chunk_ms=document.getElementById('vadMaxChunkSlider').value;
  
  fetch('/api/vad/config',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({window_ms:window_ms,threshold:threshold,silence_ms:silence_ms,max_chunk_ms:max_chunk_ms})
  }).then(r=>r.json()).then(d=>{
    if(d.success){
      document.getElementById('currentVadWindow').textContent=d.window_ms;
      document.getElementById('currentVadThreshold').textContent=d.threshold;
      document.getElementById('currentVadSilence').textContent=d.silence_ms;
      document.getElementById('currentVadMaxChunk').textContent=d.max_chunk_ms;
      alert('VAD configuration saved successfully!');
    }
  }).catch(e=>console.error('Failed to save VAD config:',e));
}

var accuracyPollInterval=null;
var accuracyTestRunning=false;

function runWhisperAccuracyTest(){
  if(accuracyTestRunning) return;
  if(accuracyPollInterval){clearInterval(accuracyPollInterval);accuracyPollInterval=null;}
  var select=document.getElementById('accuracyTestFiles');
  var selected=Array.from(select.selectedOptions).map(o=>o.value);
  
  if(selected.length===0){
    alert('Please select at least one test file');
    return;
  }
  
  accuracyTestRunning=true;
  var btn=document.querySelector('[onclick*="runWhisperAccuracyTest"]');
  if(btn){btn.disabled=true;btn.textContent='Running...';}
  var resultsDiv=document.getElementById('accuracyResults');
  var summaryDiv=document.getElementById('accuracySummary');
  resultsDiv.innerHTML='<p style="color:var(--wt-warning)">&#x23F3; Running accuracy test on '+selected.length+' file(s)... This may take several minutes.</p>';
  summaryDiv.style.display='none';
  
  fetch('/api/whisper/accuracy_test',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({files:selected})
  }).then(r=>{
    if(r.status===202) return r.json();
    return r.json().then(d=>{throw new Error(d.error||'HTTP '+r.status);});
  }).then(d=>{
    resultsDiv.innerHTML='<p style="color:var(--wt-warning)">&#x23F3; Accuracy test running (task '+d.task_id+', '+selected.length+' files)...</p>';
    accuracyPollInterval=setInterval(()=>pollAccuracyTask(d.task_id),3000);
  }).catch(e=>{
    accuracyTestRunning=false;
    if(btn){btn.disabled=false;btn.textContent='Run Accuracy Test';}
    if(accuracyPollInterval){clearInterval(accuracyPollInterval);accuracyPollInterval=null;}
    resultsDiv.innerHTML='<p style="color:var(--wt-danger)">&#x2717; Error: '+escapeHtml(String(e))+'</p>';
  });
}

function pollAccuracyTask(taskId){
  fetch('/api/async/status?task_id='+taskId).then(r=>r.json()).then(d=>{
    if(d.status==='running') return;
    clearInterval(accuracyPollInterval);accuracyPollInterval=null;
    var resultsDiv=document.getElementById('accuracyResults');
    var summaryDiv=document.getElementById('accuracySummary');
    if(d.error){
      accuracyTestRunning=false;
      var btn=document.querySelector('[onclick*="runWhisperAccuracyTest"]');
      if(btn){btn.disabled=false;btn.textContent='Run Accuracy Test';}
      resultsDiv.innerHTML='<p style="color:var(--wt-danger)">&#x2717; Error: '+escapeHtml(d.error)+'</p>';
      return;
    }
    
    document.getElementById('summaryTotal').textContent=d.total||0;
    document.getElementById('summaryPass').textContent=d.pass_count||0;
    document.getElementById('summaryWarn').textContent=d.warn_count||0;
    document.getElementById('summaryFail').textContent=d.fail_count||0;
    document.getElementById('summaryAccuracy').textContent=(d.avg_accuracy||0).toFixed(2);
    document.getElementById('summaryLatency').textContent=Math.round(d.avg_latency_ms||0);
    summaryDiv.style.display='block';
    
    var html='<div style="overflow-x:auto"><table class="wt-table" style="width:100%;font-size:12px">';
    html+='<thead><tr>';
    html+='<th>File</th>';
    html+='<th>Ground Truth</th>';
    html+='<th>Transcription</th>';
    html+='<th>Similarity</th>';
    html+='<th>Latency (ms)</th>';
    html+='<th>Status</th>';
    html+='</tr></thead><tbody>';
    
    (d.results||[]).forEach(function(r){
      var statusColor='var(--wt-text)';
      if(r.status==='PASS')statusColor='var(--wt-success)';
      else if(r.status==='WARN')statusColor='var(--wt-warning)';
      else if(r.status==='FAIL')statusColor='var(--wt-danger)';
      
      html+='<tr>';
      html+='<td style="max-width:150px;overflow:hidden;text-overflow:ellipsis">'+escapeHtml(r.file)+'</td>';
      html+='<td style="max-width:200px;overflow:hidden;text-overflow:ellipsis" title="'+escapeHtml(r.ground_truth)+'">'+escapeHtml(r.ground_truth)+'</td>';
      html+='<td style="max-width:200px;overflow:hidden;text-overflow:ellipsis" title="'+escapeHtml(r.transcription)+'">'+escapeHtml(r.transcription)+'</td>';
      html+='<td style="font-weight:600">'+r.similarity.toFixed(2)+'%</td>';
      html+='<td>'+Math.round(r.latency_ms)+'</td>';
      html+='<td style="color:'+statusColor+';font-weight:600">'+r.status+'</td>';
      html+='</tr>';
    });
    
    html+='</tbody></table></div>';
    resultsDiv.innerHTML=html;
    
    loadAccuracyTrendChart();
    accuracyTestRunning=false;
    var btn=document.querySelector('[onclick*="runWhisperAccuracyTest"]');
    if(btn){btn.disabled=false;btn.textContent='Run Accuracy Test';}
  }).catch(function(e){
    clearInterval(accuracyPollInterval);accuracyPollInterval=null;
    accuracyTestRunning=false;
    var btn=document.querySelector('[onclick*="runWhisperAccuracyTest"]');
    if(btn){btn.disabled=false;btn.textContent='Run Accuracy Test';}
    var resultsDiv=document.getElementById('accuracyResults');
    resultsDiv.innerHTML='<p style="color:var(--wt-danger)">&#x2717; Poll error: '+escapeHtml(String(e))+'</p>';
  });
}

function loadAccuracyTrendChart(){
  fetch('/api/whisper/accuracy_results?limit=10').then(r=>r.json()).then(d=>{
    if(!d.results||d.results.length===0)return;
    
    var canvas=document.getElementById('accuracyTrendChart');
    canvas.style.display='block';
    
    var labels=d.results.reverse().map((r,i)=>'Run '+(i+1));
    var accuracyData=d.results.map(r=>r.avg_similarity);
    var latencyData=d.results.map(r=>r.avg_latency_ms);
    
    if(window.accuracyChart){
      window.accuracyChart.destroy();
    }
    
    var ctx=canvas.getContext('2d');
    window.accuracyChart=new Chart(ctx,{
      type:'line',
      data:{
        labels:labels,
        datasets:[
          {
            label:'Avg Accuracy (%)',
            data:accuracyData,
            borderColor:'rgb(52,199,89)',
            backgroundColor:'rgba(52,199,89,0.1)',
            yAxisID:'y'
          },
          {
            label:'Avg Latency (ms)',
            data:latencyData,
            borderColor:'rgb(0,113,227)',
            backgroundColor:'rgba(0,113,227,0.1)',
            yAxisID:'y1'
          }
        ]
      },
      options:{
        responsive:true,
        maintainAspectRatio:false,
        interaction:{mode:'index',intersect:false},
        plugins:{
          legend:{position:'top'},
          tooltip:{
            enabled:true,
            mode:'index',
            intersect:false,
            backgroundColor:'rgba(0,0,0,0.8)',
            titleColor:'#fff',
            bodyColor:'#fff',
            borderColor:'rgba(52,199,89,0.5)',
            borderWidth:1,
            padding:12,
            displayColors:true,
            callbacks:{
              title:function(items){return items[0].label;},
              label:function(ctx){
                var label=ctx.dataset.label||'';
                if(label)label+=': ';
                label+=ctx.parsed.y.toFixed(2);
                if(ctx.datasetIndex===0)label+=' %';
                else label+=' ms';
                return label;
              }
            }
          },
          zoom:{
            pan:{enabled:true,mode:'x',modifierKey:'shift'},
            zoom:{
              wheel:{enabled:true,speed:0.1},
              pinch:{enabled:true},
              mode:'x'
            },
            limits:{x:{min:'original',max:'original'}}
          }
        },
        scales:{
          y:{
            type:'linear',
            display:true,
            position:'left',
            title:{display:true,text:'Accuracy (%)'},
            min:0,
            max:100
          },
          y1:{
            type:'linear',
            display:true,
            position:'right',
            title:{display:true,text:'Latency (ms)'},
            grid:{drawOnChartArea:false}
          }
        }
      }
    });
  }).catch(e=>console.error('Failed to load accuracy trend:',e));
}

if(currentPage==='beta-testing'){buildSipLinesGrid();refreshTestFiles();loadVadConfig();}
)JS";
        return js;
    }

    void serve_theme_css(struct mg_connection *c, struct mg_http_message *hm) {
        std::string uri(hm->uri.buf, hm->uri.len);
        std::string name = uri.substr(strlen("/css/theme/"));

        static const char* slate_css = R"CSS(
:root{--wt-bg:#272b30;--wt-sidebar-bg:rgba(39,43,48,0.85);--wt-card-bg:#32363b;--wt-border:#43474c;--wt-text:#c8c8c8;--wt-text-secondary:#999;--wt-accent:#5bc0de;--wt-success:#62c462;--wt-danger:#ee5f5b;--wt-warning:#f89406}
body{background:var(--wt-bg) !important;color:var(--wt-text) !important}
.wt-sidebar{background:var(--wt-sidebar-bg) !important;border-color:var(--wt-border) !important}
.wt-card{background:var(--wt-card-bg) !important;border-color:var(--wt-border) !important}
.wt-card:hover{box-shadow:0 2px 12px rgba(0,0,0,0.2) !important}
.wt-input,.wt-textarea,.wt-select{background:#3a3f44 !important;color:var(--wt-text) !important;border-color:var(--wt-border) !important}
.wt-btn-secondary{background:#43474c !important;color:#c8c8c8 !important}
.wt-nav-item{color:var(--wt-text) !important}
.wt-nav-item:hover{background:rgba(255,255,255,0.05) !important}
.wt-nav-item.active{background:var(--wt-accent) !important;color:#fff !important}
.wt-table th{color:var(--wt-text-secondary) !important;border-color:var(--wt-border) !important}
.wt-table td{border-color:var(--wt-border) !important}
.wt-page-title{color:#fff !important}
.wt-status-bar{border-color:var(--wt-border) !important}
.wt-sidebar-header h1{color:#999 !important}
.wt-sidebar-section-title{color:#777 !important}
.wt-theme-menu{background:var(--wt-card-bg) !important;border-color:var(--wt-border) !important}
.wt-theme-opt:hover{background:rgba(255,255,255,0.05) !important}
)CSS";

        static const char* flatly_css = R"CSS(
:root{--wt-bg:#ecf0f1;--wt-sidebar-bg:rgba(255,255,255,0.88);--wt-card-bg:#fff;--wt-border:#dce4ec;--wt-text:#2c3e50;--wt-text-secondary:#7b8a8b;--wt-accent:#18bc9c;--wt-success:#18bc9c;--wt-danger:#e74c3c;--wt-warning:#f39c12;--wt-radius:4px}
body{background:var(--wt-bg) !important;color:var(--wt-text) !important}
.wt-card{border-radius:var(--wt-radius) !important;border-color:var(--wt-border) !important}
.wt-btn{border-radius:4px !important}
.wt-input,.wt-textarea,.wt-select{border-radius:4px !important}
.wt-nav-item{border-radius:4px !important}
.wt-nav-item.active{background:var(--wt-accent) !important}
.wt-badge{border-radius:3px !important}
.wt-page-title{color:#2c3e50 !important;font-weight:800 !important}
.wt-sidebar-header h1{color:#2c3e50 !important}
.wt-btn-primary{background:var(--wt-accent) !important}
.wt-btn-primary:hover{background:#15a589 !important}
.wt-nav-item .nav-badge{background:var(--wt-accent) !important}
.wt-log-view{border-radius:var(--wt-radius) !important}
)CSS";

        static const char* cyborg_css = R"CSS(
:root{--wt-bg:#060606;--wt-sidebar-bg:rgba(17,17,17,0.9);--wt-card-bg:#111;--wt-border:#282828;--wt-text:#ddd;--wt-text-secondary:#888;--wt-accent:#2a9fd6;--wt-success:#77b300;--wt-danger:#cc0000;--wt-warning:#ff8800;--wt-radius:0px}
body{background:var(--wt-bg) !important;color:var(--wt-text) !important}
.wt-sidebar{background:var(--wt-sidebar-bg) !important;border-color:var(--wt-border) !important}
.wt-card{background:var(--wt-card-bg) !important;border-color:var(--wt-border) !important;border-radius:0 !important}
.wt-card:hover{box-shadow:0 0 10px rgba(42,159,214,0.15) !important}
.wt-input,.wt-textarea,.wt-select{background:#1a1a1a !important;color:var(--wt-text) !important;border-color:var(--wt-border) !important;border-radius:0 !important}
.wt-btn{border-radius:0 !important;text-transform:uppercase;font-size:11px !important;letter-spacing:1px}
.wt-btn-primary{background:var(--wt-accent) !important}
.wt-btn-primary:hover{background:#2186b4 !important}
.wt-btn-secondary{background:#282828 !important;color:#aaa !important}
.wt-nav-item{color:var(--wt-text) !important;border-radius:0 !important}
.wt-nav-item:hover{background:rgba(42,159,214,0.1) !important}
.wt-nav-item.active{background:var(--wt-accent) !important;color:#fff !important;border-radius:0 !important}
.wt-nav-item .nav-badge{background:var(--wt-accent) !important;border-radius:0 !important}
.wt-table th{color:var(--wt-accent) !important;border-color:var(--wt-border) !important;text-transform:uppercase}
.wt-table td{border-color:var(--wt-border) !important}
.wt-page-title{color:var(--wt-accent) !important;text-transform:uppercase;letter-spacing:2px}
.wt-status-bar{border-color:var(--wt-border) !important}
.wt-sidebar-header h1{color:var(--wt-accent) !important;letter-spacing:2px}
.wt-sidebar-section-title{color:var(--wt-accent) !important;letter-spacing:1px}
.wt-badge{border-radius:0 !important}
.wt-log-view{border-radius:0 !important}
.wt-theme-menu{background:#111 !important;border-color:var(--wt-border) !important;border-radius:0 !important}
.wt-theme-opt:hover{background:rgba(42,159,214,0.1) !important}
.wt-toggle{border-radius:2px !important}
.wt-toggle::after{border-radius:2px !important}
.wt-status-dot.online{box-shadow:0 0 6px var(--wt-success) !important}
)CSS";

        const char* css = nullptr;
        if (name == "slate") css = slate_css;
        else if (name == "flatly") css = flatly_css;
        else if (name == "cyborg") css = cyborg_css;

        if (css) {
            mg_http_reply(c, 200, "Content-Type: text/css\r\nCache-Control: no-cache\r\n", "%s", css);
        } else {
            mg_http_reply(c, 404, "", "Theme not found\n");
        }
    }

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
        for (auto it = recent_logs_.rbegin(); it != recent_logs_.rend() && count < 100; ++it, ++count) {
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

    static std::string extract_json_string(const std::string& json, const std::string& key) {
        std::string needle = "\"" + key + "\"";
        size_t pos = json.find(needle);
        if (pos == std::string::npos) return "";
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
        
        std::lock_guard<std::mutex> lock(tests_mutex_);
        for (auto& test : tests_) {
            if (test.name == test_name && !test.is_running) {
                std::string bin_name = test.binary_path;
                size_t slash = bin_name.rfind('/');
                if (slash != std::string::npos) bin_name = bin_name.substr(slash + 1);
                kill_ghost_processes(bin_name);

                std::vector<std::string> use_args;
                if (!custom_args.empty()) {
                    use_args = split_args(custom_args);
                } else {
                    use_args = test.default_args;
                }

                if (!is_allowed_binary(test.binary_path)) {
                    std::cerr << "Invalid test binary path: " << test.binary_path << "\n";
                    break;
                }

                mkdir("logs", 0755);
                std::string log_path = "logs/" + test.name + "_" + std::to_string(time(nullptr)) + ".log";

                pid_t pid = fork();
                if (pid < 0) {
                    std::cerr << "fork() failed for test " << test.name << ": " << strerror(errno) << "\n";
                    break;
                }
                if (pid == 0) {
                    for (int i = 3; i < 1024; ++i) close(i);

                    int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd >= 0) {
                        dup2(fd, STDOUT_FILENO);
                        dup2(fd, STDERR_FILENO);
                        close(fd);
                    }
                    
                    std::vector<char*> argv;
                    argv.push_back(const_cast<char*>(test.binary_path.c_str()));
                    for (auto& a : use_args) {
                        argv.push_back(const_cast<char*>(a.c_str()));
                    }
                    argv.push_back(nullptr);
                    
                    execv(test.binary_path.c_str(), argv.data());
                    _exit(1);
                }
                test.is_running = true;
                test.pid = pid;
                test.start_time = time(nullptr);
                test.log_file = log_path;
                test.default_args = use_args;
                break;
            }
        }
        
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
                    test.is_running = false;
                    test.end_time = time(nullptr);
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

        usleep(500000);

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

        ssize_t sent = ::send(sock, cmd.c_str(), cmd.size(), 0);
        if (sent <= 0) {
            close(sock);
            return "";
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

        std::string cmd = "ADD_LINE " + user + " " + (server.empty() ? "127.0.0.1" : server);
        cmd += " " + std::to_string(port_val);
        cmd += " " + (password.empty() ? "-" : password);

        std::string resp = send_negotiation_command(whispertalk::ServiceType::SIP_CLIENT, cmd);
        if (resp.empty()) {
            mg_http_reply(c, 502, "Content-Type: application/json\r\n", "{\"error\":\"SIP Client not reachable\"}");
            return;
        }

        if (resp.find("LINE_ADDED") == 0) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"success\":true,\"response\":\"%s\"}", resp.c_str());
        } else {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"%s\"}", resp.c_str());
        }
    }

    // POST /api/sip/remove-line — Remove a SIP registration via REMOVE_LINE command.
    void handle_sip_remove_line(struct mg_connection *c, struct mg_http_message *hm) {
        std::string body(hm->body.buf, hm->body.len);
        std::string idx_str = extract_json_string(body, "index");
        if (idx_str.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing 'index'\"}");
            return;
        }

        std::string cmd = "REMOVE_LINE " + idx_str;
        std::string resp = send_negotiation_command(whispertalk::ServiceType::SIP_CLIENT, cmd);
        if (resp.empty()) {
            mg_http_reply(c, 502, "Content-Type: application/json\r\n", "{\"error\":\"SIP Client not reachable\"}");
            return;
        }

        if (resp.find("LINE_REMOVED") == 0) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"success\":true,\"response\":\"%s\"}", resp.c_str());
        } else {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"%s\"}", resp.c_str());
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
            size_t p1 = token.find(':');
            size_t p2 = token.find(':', p1 + 1);
            if (p1 == std::string::npos || p2 == std::string::npos) continue;
            std::string idx_s = token.substr(0, p1);
            std::string user_s = token.substr(p1 + 1, p2 - p1 - 1);
            size_t p3 = token.find(':', p2 + 1);
            std::string status_s, server_s, port_s;
            if (p3 != std::string::npos) {
                status_s = token.substr(p2 + 1, p3 - p2 - 1);
                size_t p4 = token.find(':', p3 + 1);
                if (p4 != std::string::npos) {
                    server_s = token.substr(p3 + 1, p4 - p3 - 1);
                    port_s = token.substr(p4 + 1);
                } else {
                    server_s = token.substr(p3 + 1);
                }
            } else {
                status_s = token.substr(p2 + 1);
            }
            if (!first) json << ",";
            json << "{\"index\":" << idx_s
                 << ",\"user\":\"" << user_s << "\""
                 << ",\"registered\":" << (status_s == "registered" ? "true" : "false")
                 << ",\"server\":\"" << server_s << "\""
                 << ",\"port\":" << (port_s.empty() ? "5060" : port_s) << "}";
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

        std::string wav_path = "Testfiles/" + file;
        IapWavData wav = load_wav_for_iap(wav_path);
        if (!wav.error.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                "{\"error\":\"WAV load failed: %s\"}", wav.error.c_str());
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

    // GET /api/whisper/models — Lists .bin GGML model files in models/ directory.
    void handle_whisper_models(struct mg_connection *c) {
        std::stringstream json;
        json << "{\"models\":[";
        DIR* dir = opendir("models");
        bool first = true;
        if (dir) {
            struct dirent* entry;
            while ((entry = readdir(dir)) != nullptr) {
                std::string name = entry->d_name;
                if (name.size() > 4 && name.substr(name.size() - 4) == ".bin") {
                    if (!first) json << ",";
                    json << "\"models/" << escape_json(name) << "\"";
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
                    size_t ms_end = msg.find("ms)", ms_start);
                    if (ms_end == std::string::npos) continue;

                    size_t text_start = ms_end + 5;
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
                settle_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);
                if (settle_deadline > deadline) settle_deadline = deadline;
            } else if (!found_any && std::chrono::steady_clock::now() >= deadline) {
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }

        result.found = found_any;
        if (!found_any) {
            std::cerr << "DEBUG: No Whisper transcription received within " 
                      << timeout_ms << "ms for seq > " << after_seq 
                      << " (current seq: " << log_seq_.load() << ")" << std::endl;
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
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
        uint16_t kokoro_cmd_port = whispertalk::service_cmd_port(whispertalk::ServiceType::KOKORO_SERVICE);
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
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
            std::string kokoro_status = tcp_command(kokoro_cmd_port, "STATUS", err, 3);
            std::string oap_status = tcp_command(oap_cmd_port, "STATUS", err, 3);
            if (!r.detail.empty()) r.detail += " | ";
            r.detail += "Kokoro: " + (kokoro_status.empty() ? "unreachable" : kokoro_status);
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
                    model_name = (const char*)sqlite3_column_text(stmt, 0);
                    model_path = (const char*)sqlite3_column_text(stmt, 1);
                    model_backend = (const char*)sqlite3_column_text(stmt, 2);
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
        uint16_t kokoro_cmd_port = whispertalk::service_cmd_port(whispertalk::ServiceType::KOKORO_SERVICE);

        std::string ping_err;
        if (tcp_command(kokoro_cmd_port, "PING", ping_err, 3) != "PONG") {
            finish_async_task(task_id, "{\"error\":\"Kokoro service not reachable (port "
                + std::to_string(kokoro_cmd_port) + "): " + escape_json(ping_err) + "\"}");
            return;
        }

        std::string results_json = "[";
        double total_latency = 0;
        int success_count = 0;
        double total_rtf = 0;
        double total_duration = 0;

        for (size_t i = 0; i < phrases.size(); i++) {
            std::string err;
            std::string resp = tcp_command(kokoro_cmd_port, "TEST_SYNTH:" + phrases[i], err, 15);

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
        uint16_t kokoro_cmd_port = whispertalk::service_cmd_port(whispertalk::ServiceType::KOKORO_SERVICE);

        std::string ping_err;
        if (tcp_command(kokoro_cmd_port, "PING", ping_err, 3) != "PONG") {
            finish_async_task(task_id, "{\"error\":\"Kokoro service not reachable (port "
                + std::to_string(kokoro_cmd_port) + "): " + escape_json(ping_err) + "\"}");
            return;
        }

        std::string bench_cmd = "BENCHMARK:" + phrase + "|" + std::to_string(iterations);
        std::string err;
        std::string resp = tcp_command(kokoro_cmd_port, bench_cmd, err, 60);

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
            {"kokoro-service",            whispertalk::ServiceType::KOKORO_SERVICE},
            {"outbound-audio-processor",  whispertalk::ServiceType::OUTBOUND_AUDIO_PROCESSOR},
        };
        constexpr int N = 7;

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
        uint16_t kokoro_cmd_port = whispertalk::service_cmd_port(whispertalk::ServiceType::KOKORO_SERVICE);

        std::string ping_err;
        if (tcp_command(kokoro_cmd_port, "PING", ping_err, 3) != "PONG") {
            finish_async_task(task_id, "{\"error\":\"Kokoro service not reachable\"}");
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
            std::string synth_resp = tcp_command(kokoro_cmd_port, synth_cmd, synth_err, 30);

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

            TranscriptionResult tr1 = wait_for_whisper_transcription(seq_before, 30000);
            std::string transcription_in = tr1.found ? tr1.text : "";
            double similarity_in = tr1.found ? calculate_levenshtein_similarity(phrase, transcription_in) : 0;

            uint64_t seq_after_l1 = current_log_seq();

            LlamaResponseResult llama_res = wait_for_llama_response(seq_before, 30000);
            std::string llama_text = llama_res.found ? llama_res.text : "";

            TranscriptionResult tr2 = wait_for_whisper_transcription(seq_after_l1, 60000);

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

        static const std::pair<whispertalk::ServiceType, const char*> required_services[] = {
            {whispertalk::ServiceType::SIP_CLIENT, "SIP Client"},
            {whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR, "IAP"},
            {whispertalk::ServiceType::VAD_SERVICE, "VAD"},
            {whispertalk::ServiceType::WHISPER_SERVICE, "Whisper"},
            {whispertalk::ServiceType::LLAMA_SERVICE, "LLaMA"},
            {whispertalk::ServiceType::KOKORO_SERVICE, "Kokoro"},
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

        for (size_t fi = 0; fi < files.size(); fi++) {
            const auto& file = files[fi];
            uint64_t seq_before = current_log_seq();
            auto e2e_start = std::chrono::steady_clock::now();

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

            TranscriptionResult tr1 = wait_for_whisper_transcription(seq_before, 30000, l1_call_id);
            std::string whisper_l1 = tr1.found ? tr1.text : "";

            LlamaResponseResult llama_res = wait_for_llama_response(seq_before, 30000);
            std::string llama_text = llama_res.found ? llama_res.text : "";

            uint64_t seq_after_llama = current_log_seq();

            TranscriptionResult tr2 = wait_for_whisper_transcription(seq_after_llama, 60000, l2_call_id);

            auto e2e_end = std::chrono::steady_clock::now();
            double e2e_ms = std::chrono::duration_cast<std::chrono::milliseconds>(e2e_end - e2e_start).count();

            std::string whisper_l2 = tr2.found ? tr2.text : "";
            double wer = 100.0;
            double similarity = 0.0;
            if (!llama_text.empty() && !whisper_l2.empty()) {
                wer = calculate_word_error_rate(llama_text, whisper_l2);
                similarity = calculate_levenshtein_similarity(llama_text, whisper_l2);
            }

            std::string status_str;
            bool llama_ok = llama_res.found && !llama_text.empty();
            bool l2_ok = tr2.found && !whisper_l2.empty();

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
                    std::string details = file + " -> L1:" + whisper_l1
                        + " -> LLaMA:" + llama_text
                        + " -> L2:" + whisper_l2
                        + " (WER=" + std::to_string((int)wer) + "%"
                        + " sim=" + std::to_string((int)similarity) + "%)"
                        + " e2e=" + std::to_string((int)e2e_ms) + "ms";
                    sqlite3_bind_text(stmt, 3, details.c_str(), -1, SQLITE_TRANSIENT);
                    sqlite3_step(stmt);
                    sqlite3_finalize(stmt);
                }
            }

            if (!first) json << ",";
            json << "{\"file\":\"" << escape_json(file) << "\""
                 << ",\"whisper_l1\":\"" << escape_json(whisper_l1) << "\""
                 << ",\"llama_response\":\"" << escape_json(llama_text) << "\""
                 << ",\"whisper_l2\":\"" << escape_json(whisper_l2) << "\""
                 << ",\"wer\":" << wer
                 << ",\"similarity\":" << similarity
                 << ",\"llama_ms\":" << (int)llama_res.gen_ms
                 << ",\"e2e_ms\":" << (int)e2e_ms
                 << ",\"status\":\"" << status_str << "\"}";
            first = false;

            if (fi + 1 < files.size()) {
                uint64_t drain_seq = current_log_seq();
                std::this_thread::sleep_for(std::chrono::seconds(4));
                for (int drain = 0; drain < 5; drain++) {
                    uint64_t cur = current_log_seq();
                    if (cur == drain_seq) break;
                    drain_seq = cur;
                    std::this_thread::sleep_for(std::chrono::seconds(2));
                }
            }
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
            {"kokoro", whispertalk::ServiceType::KOKORO_SERVICE},
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
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
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
            {ServiceType::KOKORO_SERVICE, "Kokoro"},
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

        static const char* svc_names[] = {"SIP","IAP","VAD","Whisper","LLaMA","Kokoro","OAP"};
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
            ServiceType::KOKORO_SERVICE,
            ServiceType::OUTBOUND_AUDIO_PROCESSOR,
        };
        static const char* svc_service_names[] = {
            "SIP_CLIENT", "INBOUND_AUDIO_PROCESSOR", "VAD_SERVICE",
            "WHISPER_SERVICE", "LLAMA_SERVICE", "KOKORO_SERVICE",
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

            std::this_thread::sleep_for(std::chrono::milliseconds(500));
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
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"status\":\"running\"}");
        } else {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", it->second->result_json.c_str());
            it->second->result_read = true;
        }
    }

    // POST /api/whisper/accuracy_test — Run offline Whisper accuracy test on selected
    // WAV files. Async: returns task_id immediately, test runs in background thread.
    // Each file is decoded, upsampled, and fed directly to whisper_full() for WER scoring.
    void handle_whisper_accuracy_test(struct mg_connection *c, struct mg_http_message *hm) {
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

            std::this_thread::sleep_for(std::chrono::milliseconds(2000));

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

    // GET/POST /api/vad/config — GET: return current VAD params from running service
    // (via STATUS cmd). POST: send SET_VAD_* commands to the running VAD service.
    void handle_vad_config(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) == 0) {
            std::string body(hm->body.buf, hm->body.len);
            
            std::string window_ms_str = extract_json_string(body, "window_ms");
            std::string threshold_str = extract_json_string(body, "threshold");
            std::string silence_ms_str = extract_json_string(body, "silence_ms");
            std::string max_chunk_ms_str = extract_json_string(body, "max_chunk_ms");
            
            if (!window_ms_str.empty()) set_setting("vad_window_ms", window_ms_str);
            if (!threshold_str.empty()) set_setting("vad_threshold", threshold_str);
            if (!silence_ms_str.empty()) set_setting("vad_silence_ms", silence_ms_str);
            if (!max_chunk_ms_str.empty()) set_setting("vad_max_chunk_ms", max_chunk_ms_str);
            
            std::string w = window_ms_str.empty() ? get_setting("vad_window_ms", "50") : window_ms_str;
            std::string t = threshold_str.empty() ? get_setting("vad_threshold", "2.0") : threshold_str;
            std::string s = silence_ms_str.empty() ? get_setting("vad_silence_ms", "400") : silence_ms_str;
            std::string m = max_chunk_ms_str.empty() ? get_setting("vad_max_chunk_ms", "8000") : max_chunk_ms_str;
            
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", 
                "{\"success\":true,\"window_ms\":%s,\"threshold\":%s,\"silence_ms\":%s,\"max_chunk_ms\":%s}",
                w.c_str(), t.c_str(), s.c_str(), m.c_str());
        } else {
            std::string window_ms = get_setting("vad_window_ms", "50");
            std::string threshold = get_setting("vad_threshold", "2.0");
            std::string silence_ms = get_setting("vad_silence_ms", "400");
            std::string max_chunk_ms = get_setting("vad_max_chunk_ms", "8000");
            
            std::stringstream json;
            json << "{"
                 << "\"window_ms\":" << window_ms << ","
                 << "\"threshold\":" << threshold << ","
                 << "\"silence_ms\":" << silence_ms << ","
                 << "\"max_chunk_ms\":" << max_chunk_ms
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

    // GET /api/whisper/accuracy_results — Returns paginated Whisper accuracy test
    // results from SQLite, grouped by test_run_id with PASS/WARN/FAIL counts.
    void handle_whisper_accuracy_results(struct mg_connection *c, struct mg_http_message *hm) {
        std::string limit_str = "20";
        if (hm->query.len > 0) {
            std::string query(hm->query.buf, hm->query.len);
            size_t limit_pos = query.find("limit=");
            if (limit_pos != std::string::npos) {
                limit_str = query.substr(limit_pos + 6);
                size_t amp = limit_str.find('&');
                if (amp != std::string::npos) limit_str = limit_str.substr(0, amp);
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
                         "LIMIT " + limit_str;
        
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
                "LLAMA_SERVICE", "KOKORO_SERVICE", "OUTBOUND_AUDIO_PROCESSOR", nullptr
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
                static const struct { const char* name; whispertalk::ServiceType type; } svc_map[] = {
                    {"SIP_CLIENT",               whispertalk::ServiceType::SIP_CLIENT},
                    {"INBOUND_AUDIO_PROCESSOR",  whispertalk::ServiceType::INBOUND_AUDIO_PROCESSOR},
                    {"VAD_SERVICE",              whispertalk::ServiceType::VAD_SERVICE},
                    {"WHISPER_SERVICE",          whispertalk::ServiceType::WHISPER_SERVICE},
                    {"LLAMA_SERVICE",            whispertalk::ServiceType::LLAMA_SERVICE},
                    {"KOKORO_SERVICE",           whispertalk::ServiceType::KOKORO_SERVICE},
                    {"OUTBOUND_AUDIO_PROCESSOR", whispertalk::ServiceType::OUTBOUND_AUDIO_PROCESSOR},
                };
                for (const auto& m : svc_map) {
                    if (service == m.name) {
                        std::string resp = send_negotiation_command(m.type, "SET_LOG_LEVEL:" + level);
                        live_update = (resp.find("OK") != std::string::npos);
                        break;
                    }
                }
            }

            mg_http_reply(c, 200, "Content-Type: application/json\r\n", 
                "{\"success\":true,\"service\":\"%s\",\"level\":\"%s\",\"live_update\":%s}", 
                service.c_str(), level.c_str(), live_update ? "true" : "false");
        }
    }

    // GET /api/test_results — Returns pipeline WER test results from
    // /tmp/pipeline_results_*.json files (written by run_pipeline_test.py).
    void handle_test_results(struct mg_connection *c, struct mg_http_message *hm) {
        if (!db_) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database not available\"}");
            return;
        }

        std::string query_str(hm->query.buf, hm->query.len);
        std::string service_filter;
        std::string status_filter;

        size_t service_pos = query_str.find("service=");
        if (service_pos != std::string::npos) {
            size_t end = query_str.find('&', service_pos);
            service_filter = query_str.substr(service_pos + 8, 
                end == std::string::npos ? std::string::npos : end - service_pos - 8);
        }

        size_t status_pos = query_str.find("status=");
        if (status_pos != std::string::npos) {
            size_t end = query_str.find('&', status_pos);
            status_filter = query_str.substr(status_pos + 7, 
                end == std::string::npos ? std::string::npos : end - status_pos - 7);
        }

        std::stringstream sql_query;
        sql_query << "SELECT id, service, test_type, status, metrics_json, timestamp FROM service_test_runs WHERE 1=1";
        
        if (!service_filter.empty()) {
            sql_query << " AND service = '" << service_filter << "'";
        }
        if (!status_filter.empty()) {
            sql_query << " AND status = '" << status_filter << "'";
        }
        sql_query << " ORDER BY timestamp DESC LIMIT 100";

        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql_query.str().c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Query failed\"}");
            return;
        }

        std::stringstream json;
        json << "{\"results\":[";
        int count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (count > 0) json << ",";
            json << "{";
            json << "\"id\":" << sqlite3_column_int(stmt, 0) << ",";
            json << "\"service\":\"" << escape_json(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1))) << "\",";
            json << "\"test_type\":\"" << escape_json(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))) << "\",";
            json << "\"status\":\"" << escape_json(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3))) << "\",";
            const char* metrics = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4));
            json << "\"metrics\":" << (metrics ? metrics : "{}") << ",";
            json << "\"timestamp\":" << sqlite3_column_int64(stmt, 5);
            json << "}";
            count++;
        }
        sqlite3_finalize(stmt);
        json << "]}";

        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    // GET /api/db/schema — Returns all SQLite table names and their CREATE statements.
    void handle_db_schema(struct mg_connection *c) {
        if (!db_) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database not available\"}");
            return;
        }

        sqlite3_stmt* stmt;
        const char* sql = "SELECT name, sql FROM sqlite_master WHERE type='table' ORDER BY name";
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"DB error\"}");
            return;
        }

        std::stringstream json;
        json << "{\"tables\":[";
        int count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            if (count > 0) json << ",";
            const char* name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            const char* ddl = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            json << "{\"name\":\"" << escape_json(name ? name : "") << "\","
                 << "\"sql\":\"" << escape_json(ddl ? ddl : "") << "\"}";
            count++;
        }
        sqlite3_finalize(stmt);
        json << "]}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

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
        
        std::map<std::string, std::vector<std::string>> models_by_type;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string service = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            std::stringstream model_json;
            model_json << "{\"id\":" << sqlite3_column_int(stmt, 0)
                      << ",\"service\":\"" << escape_json(service) << "\""
                      << ",\"name\":\"" << escape_json(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2))) << "\""
                      << ",\"path\":\"" << escape_json(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3))) << "\""
                      << ",\"backend\":\"" << escape_json(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 4))) << "\""
                      << ",\"size_mb\":" << sqlite3_column_int(stmt, 5)
                      << ",\"config_json\":\"" << escape_json(reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6))) << "\""
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

    // POST /api/models/download — Async download of a model file from HuggingFace.
    // Spawns a background curl process; progress available via handle_models_download_progress.
    void handle_models_download(struct mg_connection *c, struct mg_http_message *hm) {
        std::string body(hm->body.buf, hm->body.len);
        std::string repo_id = extract_json_string(body, "repo_id");
        std::string filename = extract_json_string(body, "filename");
        std::string model_name = extract_json_string(body, "model_name");
        std::string backend = extract_json_string(body, "backend");
        std::string service = extract_json_string(body, "service");
        if (service != "whisper" && service != "llama") service = "whisper";

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

        if (!is_safe_filename(filename)) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n",
                "{\"error\":\"Invalid filename. Use alphanumeric, dash, underscore, dot only. No path separators.\"}");
            return;
        }

        if (model_name.empty()) model_name = filename;
        if (backend.empty()) backend = "coreml";

        std::string hf_token = get_setting("hf_token", "");

        std::string models_dir = "models";
        mkdir(models_dir.c_str(), 0755);

        std::string local_path = models_dir + "/" + filename;

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
        progress->filename = filename;
        progress->local_path = local_path;
        {
            std::lock_guard<std::mutex> lock(downloads_mutex_);
            downloads_[dl_id] = progress;
        }

        std::string abs_models_str(abs_models_dir);

        std::thread([this, repo_id, filename, local_path, hf_token, model_name, backend, service, progress, abs_models_str]() {
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

            std::vector<std::string> dl_args = {"-s", "-S", "-L", "--max-time", "3600", "-o", tmp_path};
            if (!header_file.empty()) {
                dl_args.push_back("-H");
                dl_args.push_back("@" + header_file);
            }
            dl_args.push_back(url);

            std::thread size_tracker([&tmp_path, &progress](){
                while (!progress->complete.load() && !progress->failed.load()) {
                    struct stat st;
                    if (stat(tmp_path.c_str(), &st) == 0) {
                        progress->bytes_downloaded.store(st.st_size);
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            });

            std::string curl_stderr;
            int ret = run_curl_to_file(dl_args, &curl_stderr);
            progress->complete.store(true);
            size_tracker.join();

            if (!header_file.empty()) unlink(header_file.c_str());

            struct stat fst;
            if (ret != 0 || stat(tmp_path.c_str(), &fst) != 0 || fst.st_size < 1024) {
                progress->failed.store(true);
                {
                    std::lock_guard<std::mutex> lock(progress->mu);
                    std::string detail = "Download failed (curl exit " + std::to_string(ret) + ")";
                    if (!curl_stderr.empty()) {
                        detail += ": " + curl_stderr.substr(0, 512);
                    }
                    progress->error = detail;
                }
                unlink(tmp_path.c_str());
                return;
            }

            if (rename(tmp_path.c_str(), local_path.c_str()) != 0) {
                progress->failed.store(true);
                {
                    std::lock_guard<std::mutex> lock(progress->mu);
                    progress->error = "Failed to move file to final path";
                }
                unlink(tmp_path.c_str());
                return;
            }

            char abs_path[PATH_MAX];
            if (!realpath(local_path.c_str(), abs_path) ||
                std::string(abs_path).find(abs_models_str) != 0) {
                progress->failed.store(true);
                {
                    std::lock_guard<std::mutex> lock(progress->mu);
                    progress->error = "Path traversal detected — file removed";
                }
                unlink(local_path.c_str());
                return;
            }

            progress->bytes_downloaded.store(fst.st_size);

            if (db_) {
                int size_mb = static_cast<int>(fst.st_size / (1024 * 1024));

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
        std::string snap_error, snap_filename, snap_local_path;
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
            ",\"error\":\"%s\",\"filename\":\"%s\",\"path\":\"%s\"}",
            (long long)snap_bytes,
            (long long)snap_total,
            snap_complete ? "true" : "false",
            snap_failed ? "true" : "false",
            escape_json(snap_error).c_str(),
            escape_json(snap_filename).c_str(),
            escape_json(snap_local_path).c_str());
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
        
        std::string model_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        std::string model_path = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
        std::string backend = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        std::string config = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
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

    static bool is_read_only_query(const std::string& query) {
        std::string trimmed = query;
        size_t start = trimmed.find_first_not_of(" \t\r\n");
        if (start == std::string::npos) return false;
        trimmed = trimmed.substr(start);
        if (trimmed.size() < 6) return false;
        std::string upper;
        for (size_t i = 0; i < trimmed.size(); i++) {
            upper += static_cast<char>(toupper(static_cast<unsigned char>(trimmed[i])));
        }
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

    // POST /api/db/write_mode — Toggle write mode for the SQL query endpoint.
    // When disabled (default), only SELECT/EXPLAIN/PRAGMA queries are allowed.
    void handle_db_write_mode(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("GET")) == 0) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                         "{\"write_mode\":%s}", db_write_mode_ ? "true" : "false");
        } else {
            std::string body(hm->body.buf, hm->body.len);
            std::string enabled = extract_json_string(body, "enabled");
            db_write_mode_ = (enabled == "true" || enabled == "1");
            mg_http_reply(c, 200, "Content-Type: application/json\r\n",
                         "{\"write_mode\":%s}", db_write_mode_ ? "true" : "false");
        }
    }

    // POST /api/db/query — Execute a SQL query against the SQLite database.
    // Read-only by default; write queries require db_write_mode_ to be enabled.
    // Returns rows as JSON array of column-name→value objects.
    void handle_db_query(struct mg_connection *c, struct mg_http_message *hm) {
        std::string body(hm->body.buf, hm->body.len);
        std::string query = extract_json_string(body, "query");
        
        if (!db_) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database not available\"}");
            return;
        }

        if (query.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Empty query\"}");
            return;
        }

        if (!db_write_mode_ && !is_read_only_query(query)) {
            mg_http_reply(c, 403, "Content-Type: application/json\r\n",
                         "{\"error\":\"Write mode is disabled. Only SELECT, EXPLAIN, and PRAGMA queries are allowed. Enable write mode via POST /api/db/write_mode.\"}");
            return;
        }

        sqlite3_stmt* stmt;
        int rc = sqlite3_prepare_v2(db_, query.c_str(), -1, &stmt, nullptr);
        if (rc != SQLITE_OK) {
            std::string error = sqlite3_errmsg(db_);
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", 
                         "{\"error\":\"%s\"}", escape_json(error).c_str());
            return;
        }

        std::stringstream json;
        json << "{\"rows\":[";
        
        int row_count = 0;
        while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
            if (row_count >= 10000) break;
            if (row_count > 0) json << ",";
            json << "{";
            
            int col_count = sqlite3_column_count(stmt);
            for (int i = 0; i < col_count; i++) {
                if (i > 0) json << ",";
                const char* col_name = sqlite3_column_name(stmt, i);
                json << "\"" << escape_json(col_name ? col_name : "") << "\":";
                
                const char* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, i));
                if (text) {
                    json << "\"" << escape_json(text) << "\"";
                } else {
                    json << "null";
                }
            }
            
            json << "}";
            row_count++;
        }
        
        json << "],\"affected\":" << sqlite3_changes(db_)
             << ",\"truncated\":" << (row_count >= 10000 ? "true" : "false") << "}";
        sqlite3_finalize(stmt);
        
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }
};

int main(int argc, char* argv[]) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    {
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
        char cwd[1024];
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

    uint16_t port = 8080;
    if (argc > 2 && strcmp(argv[1], "--port") == 0) {
        port = static_cast<uint16_t>(atoi(argv[2]));
    }

    mkdir("logs", 0755);

    std::cout << "WhisperTalk Frontend Server\n";
    std::cout << "============================\n\n";

    FrontendServer server(port);
    if (!server.start()) {
        return 1;
    }

    return 0;
}
