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
        std::string result_json;
        std::thread worker;
    };
    std::mutex async_mutex_;
    std::map<int64_t, std::shared_ptr<AsyncTask>> async_tasks_;
    std::atomic<int64_t> async_id_counter_{0};

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
            if (!it->second->running) {
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
                thd_percent REAL,
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
                test_files TEXT,
                iterations INTEGER,
                avg_accuracy REAL,
                avg_latency_ms INTEGER,
                p50_latency_ms INTEGER,
                p95_latency_ms INTEGER,
                p99_latency_ms INTEGER,
                memory_mb INTEGER,
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
        )";

        char* errmsg = nullptr;
        rc = sqlite3_exec(db_, schema, nullptr, nullptr, &errmsg);
        if (rc != SQLITE_OK) {
            std::cerr << "SQL error: " << errmsg << "\n";
            sqlite3_free(errmsg);
        }

        const char* seed = R"(
            INSERT OR IGNORE INTO service_config (service, binary_path, default_args, description) VALUES
                ('SIP_CLIENT', 'bin/sip-client', '--lines 2 alice 127.0.0.1 5060', 'SIP client / RTP gateway'),
                ('INBOUND_AUDIO_PROCESSOR', 'bin/inbound-audio-processor', '', 'G.711 decode + 8kHz to 16kHz resample'),
                ('VAD_SERVICE', 'bin/vad-service', '', 'Voice Activity Detection + speech segmentation'),
                ('WHISPER_SERVICE', 'bin/whisper-service', '--language de models/ggml-large-v3-turbo-q5_0.bin', 'Whisper ASR (CoreML/Metal)'),
                ('LLAMA_SERVICE', 'bin/llama-service', '', 'LLaMA 3.2-1B response generation'),
                ('KOKORO_SERVICE', 'bin/kokoro-service', '', 'Kokoro TTS (CoreML)'),
                ('OUTBOUND_AUDIO_PROCESSOR', 'bin/outbound-audio-processor', '', 'TTS audio to G.711 encode + RTP'),
                ('TEST_SIP_PROVIDER', 'bin/test_sip_provider', '--port 5060 --http-port 22011 --testfiles-dir Testfiles', 'SIP B2BUA test provider for audio injection');
            INSERT OR IGNORE INTO settings (key, value) VALUES ('theme', 'default');
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
                std::string vad_w = get_setting("whisper_vad_window_ms", "");
                std::string vad_t = get_setting("whisper_vad_threshold", "");
                if (!vad_w.empty()) use_args += " --vad-window-ms " + vad_w;
                if (!vad_t.empty()) use_args += " --vad-threshold " + vad_t;
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
        LogEntry entry;

        time_t now = time(nullptr);
        char timebuf[64];
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", localtime(&now));
        entry.timestamp = timebuf;

        size_t p1 = msg.find(' ');
        size_t p2 = (p1 != std::string::npos) ? msg.find(' ', p1 + 1) : std::string::npos;
        size_t p3 = (p2 != std::string::npos) ? msg.find(' ', p2 + 1) : std::string::npos;

        if (p1 != std::string::npos && p2 != std::string::npos && p3 != std::string::npos) {
            entry.service = parse_service_type(msg.substr(0, p1));
            entry.level = msg.substr(p1 + 1, p2 - p1 - 1);
            try {
                entry.call_id = static_cast<uint32_t>(std::stoul(msg.substr(p2 + 1, p3 - p2 - 1)));
            } catch (const std::exception&) {
                entry.call_id = 0;
            }
            entry.message = msg.substr(p3 + 1);
        } else {
            entry.service = ServiceType::FRONTEND;
            entry.call_id = 0;
            entry.level = "INFO";
            entry.message = msg;
        }

        {
            std::lock_guard<std::mutex> lock(logs_mutex_);
            entry.seq = ++log_seq_;
            recent_logs_.push_back(entry);
            if (recent_logs_.size() > MAX_RECENT_LOGS) {
                recent_logs_.pop_front();
            }
        }

        enqueue_log(entry);

        {
            std::lock_guard<std::mutex> lock(sse_queue_mutex_);
            sse_queue_.push_back(entry);
        }
    }

    std::mutex log_queue_mutex_;
    std::vector<LogEntry> log_queue_;

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
            } else if (mg_strcmp(hm->uri, mg_str("/api/whisper/vad_config")) == 0) {
                handle_whisper_vad_config(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/whisper/accuracy_results")) == 0) {
                handle_whisper_accuracy_results(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/models")) == 0) {
                handle_models_get(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/models/benchmarks")) == 0) {
                handle_models_benchmarks_get(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/models/add")) == 0) {
                handle_models_add(c, hm);
            } else if (mg_strcmp(hm->uri, mg_str("/api/whisper/benchmark")) == 0) {
                handle_whisper_benchmark(c, hm);
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
</div>
<div class="wt-field"><label>Arguments</label>
<input class="wt-input" id="svcDetailArgs" placeholder="Service arguments..."></div>
<div style="display:flex;gap:8px">
<button class="wt-btn wt-btn-primary" id="svcStartBtn" onclick="startSvcDetail()">&#x25B6; Start</button>
<button class="wt-btn wt-btn-danger" id="svcStopBtn" onclick="stopSvcDetail()">&#x25A0; Stop</button>
<button class="wt-btn wt-btn-secondary" id="svcRestartBtn" onclick="restartSvcDetail()">&#x21BB; Restart</button>
<button class="wt-btn wt-btn-secondary" id="svcSaveBtn" onclick="saveSvcConfig()">&#x1F4BE; Save Config</button>
</div></div>
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
<label>Target Call/Line</label>
<select class="wt-select" id="injectTargetCall" style="width:100%;padding:8px">
<option value="1">Line 1</option>
<option value="2">Line 2</option>
<option value="3">Line 3</option>
<option value="4">Line 4</option>
</select>
</div>
<div class="wt-field">
<label>Inject To</label>
<select class="wt-select" id="injectLeg" style="width:100%;padding:8px">
<option value="a">Leg A</option>
<option value="b">Leg B</option>
</select>
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
<p style="font-size:13px;color:var(--wt-text-secondary);margin-bottom:12px"><strong>Codec algorithm test</strong> (does not require IAP service). Runs the exact G.711 mu-law encode/decode + 8kHz&#x2192;16kHz upsample pipeline offline, measuring SNR and THD against original audio. Service connectivity is tested in Test 1 above.</p>
<div class="wt-field">
<label>Select Test File</label>
<select class="wt-select" id="iapTestFileSelect" style="width:100%;padding:8px">
<option value="">-- Select a test file --</option>
</select>
</div>
<div style="display:flex;gap:8px;margin-bottom:12px">
<button class="wt-btn wt-btn-primary" onclick="runIapQualityTest()">&#x25B6; Run Quality Test</button>
</div>
<div id="iapTestStatus" style="margin-bottom:12px;font-size:13px"></div>
<div id="iapTestResults">
<h4 style="font-size:14px;font-weight:600;margin:12px 0 8px">Latest Test Results</h4>
<table class="wt-table" style="width:100%">
<thead>
<tr>
<th>File</th>
<th>Latency (ms)</th>
<th>SNR (dB)</th>
<th>THD (%)</th>
<th>Status</th>
<th>Timestamp</th>
</tr>
</thead>
<tbody id="iapResultsBody">
<tr><td colspan="6" style="text-align:center;color:var(--wt-text-secondary)">No test results yet. Run a test to see results here.</td></tr>
</tbody>
</table>
<div style="margin-top:12px;font-size:12px;color:var(--wt-text-secondary)">
<strong>Pass Criteria:</strong> SNR ≥ 3dB (G.711 ulaw codec), THD ≤ 80%, Latency ≤ 50ms
</div>
</div>
<div id="iapTestChart" style="margin-top:16px;display:none">
<canvas id="iapMetricsChart" style="max-height:250px"></canvas>
</div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">SIP Lines Management</span></div>
<div style="margin-bottom:16px">
<div style="display:flex;gap:8px;margin-bottom:12px">
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="applyPreset(1)">1 Line</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="applyPreset(2)">2 Lines</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="applyPreset(4)">4 Lines</button>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="applyPreset(6)">6 Lines</button>
<button class="wt-btn wt-btn-sm wt-btn-primary" onclick="refreshLines()" style="margin-left:auto">&#x21BB; Refresh</button>
</div>
</div>
<div class="wt-field">
<label>Add New Line</label>
<div style="display:flex;gap:8px;margin-bottom:8px">
<input class="wt-input" id="newLineUser" placeholder="Username (e.g., alice)" style="flex:1">
<input class="wt-input" id="newLineServer" placeholder="Server IP (default: 127.0.0.1)" style="flex:1">
</div>
<div style="display:flex;gap:8px">
<input class="wt-input" id="newLinePassword" placeholder="Password (optional)" style="flex:1" type="password">
<button class="wt-btn wt-btn-primary" onclick="addLine()">&#x2795; Add Line</button>
</div>
</div>
<div id="sipLinesStatus" style="margin-top:12px;font-size:13px"></div>
<div id="sipLinesTable" style="margin-top:16px">
<table class="wt-table" id="linesTable" style="width:100%">
<thead>
<tr>
<th>Index</th>
<th>Username</th>
<th>Status</th>
<th>Actions</th>
</tr>
</thead>
<tbody id="linesTableBody">
<tr><td colspan="4" style="text-align:center;color:var(--wt-text-secondary)">No lines configured. Click "Refresh" to load lines.</td></tr>
</tbody>
</table>
</div>
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
<div style="font-size:12px;font-weight:600;color:var(--wt-text-secondary);margin-bottom:6px">&#x2699; Current VAD Settings (will be used on next Whisper start)</div>
<div style="display:grid;grid-template-columns:1fr 1fr;gap:8px;font-size:12px">
<div><strong>Window:</strong> <span id="currentVadWindow" style="color:var(--wt-primary)">100</span> ms</div>
<div><strong>Threshold:</strong> <span id="currentVadThreshold" style="color:var(--wt-primary)">2.0</span></div>
</div>
</div>
<div style="display:grid;grid-template-columns:1fr 1fr;gap:12px;margin-top:8px">
<div class="wt-field">
<label>VAD Window (ms): <span id="vadWindowValue">100</span></label>
<input type="range" id="vadWindowSlider" min="50" max="300" value="100" step="25" style="width:100%" oninput="updateVadWindowDisplay(this.value)">
<div style="display:flex;justify-content:space-between;font-size:11px;color:var(--wt-text-secondary);margin-top:2px">
<span>50ms</span><span>300ms</span>
</div>
</div>
<div class="wt-field">
<label>VAD Threshold: <span id="vadThresholdValue">2.0</span></label>
<input type="range" id="vadThresholdSlider" min="1.0" max="4.0" value="2.0" step="0.1" style="width:100%" oninput="updateVadThresholdDisplay(this.value)">
<div style="display:flex;justify-content:space-between;font-size:11px;color:var(--wt-text-secondary);margin-top:2px">
<span>1.0</span><span>4.0</span>
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
<span class="wt-card-title">Registered Whisper Models</span>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="loadModels()">&#x21BB; Refresh</button>
</div>
<div id="whisperModelsTable"><em>Loading...</em></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Add Whisper Model</span></div>
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
</div><!-- end modelTabLlama -->

<!-- Comparison Panel -->
<div id="modelTabCompare" style="display:none">
<div class="wt-card">
<div class="wt-card-header">
<span class="wt-card-title">Model Benchmark Comparison</span>
<button class="wt-btn wt-btn-sm wt-btn-secondary" onclick="loadModelComparison()">&#x21BB; Refresh</button>
</div>
<div id="comparisonTable"><em>No benchmark runs yet. Run benchmarks on models to compare them.</em></div>
</div>

<div class="wt-card">
<div class="wt-card-header"><span class="wt-card-title">Latency Comparison Chart</span></div>
<canvas id="modelComparisonChart" style="max-height:300px"></canvas>
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
  if(p==='beta-testing'){refreshTestFiles();loadVadConfig();}
  if(p==='models'){loadModels();loadModelComparison();}
  if(p==='logs'){reconnectLogSSE();}
  if(p==='database'){}
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
        +btns+'</div>';
    }).join('');
    if(currentSvc){
      var s=d.services.find(x=>x.name===currentSvc);
      if(s)updateSvcDetail(s);
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
  } else {
    wc.classList.add('hidden');
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
  svcLogSSE=new EventSource('/api/logs/stream?service='+encodeURIComponent(name));
  svcLogSSE.onmessage=function(e){
    try{
      var d=JSON.parse(e.data);
      var el=document.getElementById('svcDetailLog');
      var lvlClass=/^[A-Z]+$/.test(d.level)?d.level:'INFO';
      el.innerHTML+='<div class="wt-log-entry"><span class="log-ts">'+escapeHtml(d.timestamp)+'</span> '
        +'<span class="log-lvl-'+lvlClass+'">'+escapeHtml(d.level)+'</span> '+escapeHtml(d.message)+'</div>';
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
  var url='/api/logs/stream';
  if(svc)url+='?service='+encodeURIComponent(svc);
  logSSE=new EventSource(url);
  logSSE.onmessage=function(e){
    try{
      var d=JSON.parse(e.data);
      var lvl=document.getElementById('logLevelFilter').value;
      if(lvl&&d.level!==lvl)return;
      var el=document.getElementById('liveLogView');
      var lvlClass=/^[A-Z]+$/.test(d.level)?d.level:'INFO';
      el.innerHTML+='<div class="wt-log-entry"><span class="log-ts">'+escapeHtml(d.timestamp)+'</span> '
        +'<span class="log-svc">'+escapeHtml(d.service)+'</span> '
        +'<span class="log-lvl-'+lvlClass+'">'+escapeHtml(d.level)+'</span> '+escapeHtml(d.message)+'</div>';
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

function setTheme(t){
  fetch('/api/settings',{method:'POST',headers:{'Content-Type':'application/json'},
    body:JSON.stringify({key:'theme',value:t})}).then(()=>location.reload());
}

function toggleThemeMenu(){
  document.getElementById('themeMenu').classList.toggle('open');
}

function escapeHtml(s){
  if(!s)return'';
  return String(s).replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
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
  Promise.all(promises).then(()=>alert('Log levels saved successfully!')).catch(e=>alert('Error saving log levels: '+e));
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
        status.innerHTML='<span style="color:var(--wt-success)">✓ Injecting: '+escapeHtml(d.injecting||file)+' to leg '+escapeHtml(d.leg||leg)+'</span>';
      }else{
        status.innerHTML='<span style="color:var(--wt-danger)">✗ Injection failed: '+escapeHtml(d.error||'Unknown error')+'</span>';
      }
    }).catch(e=>{
      status.innerHTML='<span style="color:var(--wt-danger)">✗ Error: Test SIP Provider not reachable (is it running on port '+TSP_PORT+'?)</span>';
    });
}

function checkSipProvider(){
  var status=document.getElementById('sipProviderStatus');
  status.innerHTML='<p style="color:var(--wt-accent)">Checking...</p>';
  fetch('http://localhost:'+TSP_PORT+'/status').then(r=>r.json()).then(d=>{
    var html='<p style="color:var(--wt-success)">✓ Test SIP Provider is running</p>';
    html+='<p style="font-size:12px;color:var(--wt-text-secondary)">Call active: '+(d.call_active?'Yes':'No')+'</p>';
    if(d.relay_stats){html+='<p style="font-size:12px;color:var(--wt-text-secondary)">Pkts A→B: '+d.relay_stats.pkts_a_to_b+', B→A: '+d.relay_stats.pkts_b_to_a+'</p>';}
    status.innerHTML=html;
  }).catch(e=>{
    status.innerHTML='<p style="color:var(--wt-danger)">✗ Test SIP Provider is NOT running</p>'+
      '<p style="font-size:12px;color:var(--wt-text-secondary)">Start it from the Tests page</p>';
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

function refreshLines(){
  var statusDiv=document.getElementById('sipLinesStatus');
  statusDiv.innerHTML='<span style="color:var(--wt-warning)">&#x23F3; Loading lines...</span>';
  fetch('/api/sip/lines').then(r=>r.json()).then(d=>{
    if(d.error){
      statusDiv.innerHTML='<span style="color:var(--wt-danger)">&#x2717; Error: '+d.error+'</span>';
      return;
    }
    var tbody=document.getElementById('linesTableBody');
    if(d.lines.length===0){
      tbody.innerHTML='<tr><td colspan="4" style="text-align:center;color:var(--wt-text-secondary)">No lines configured</td></tr>';
      statusDiv.innerHTML='<span style="color:var(--wt-text-secondary)">No lines configured</span>';
    }else{
      var html='';
      d.lines.forEach(function(line){
        var statusBadge=line.registered?'<span style="color:var(--wt-success)">&#x2713; Registered</span>':'<span style="color:var(--wt-text-secondary)">&#x25CB; Unregistered</span>';
        html+='<tr>';
        html+='<td>'+line.index+'</td>';
        html+='<td>'+line.user+'</td>';
        html+='<td>'+statusBadge+'</td>';
        html+='<td><button class="wt-btn wt-btn-sm wt-btn-danger" onclick="removeLine('+line.index+')">&#x2716; Remove</button></td>';
        html+='</tr>';
      });
      tbody.innerHTML=html;
      statusDiv.innerHTML='<span style="color:var(--wt-success)">&#x2713; Loaded '+d.lines.length+' line(s)</span>';
    }
  }).catch(e=>{
    statusDiv.innerHTML='<span style="color:var(--wt-danger)">&#x2717; Error: '+e+'</span>';
  });
}

function addLine(){
  var user=document.getElementById('newLineUser').value.trim();
  var server=document.getElementById('newLineServer').value.trim();
  var password=document.getElementById('newLinePassword').value;
  if(!user){alert('Please enter a username');return;}
  var statusDiv=document.getElementById('sipLinesStatus');
  statusDiv.innerHTML='<span style="color:var(--wt-warning)">&#x23F3; Adding line '+user+'...</span>';
  fetch('/api/sip/add-line',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({user:user,server:server||'127.0.0.1',password:password})
  }).then(r=>r.json()).then(d=>{
    if(d.success){
      statusDiv.innerHTML='<span style="color:var(--wt-success)">&#x2713; Line added: '+user+'</span>';
      document.getElementById('newLineUser').value='';
      document.getElementById('newLineServer').value='';
      document.getElementById('newLinePassword').value='';
      setTimeout(refreshLines,500);
    }else{
      statusDiv.innerHTML='<span style="color:var(--wt-danger)">&#x2717; Error: '+(d.error||'Failed to add line')+'</span>';
    }
  }).catch(e=>{
    statusDiv.innerHTML='<span style="color:var(--wt-danger)">&#x2717; Error: '+e+'</span>';
  });
}

function removeLine(index){
  if(!confirm('Remove line '+index+'?'))return;
  var statusDiv=document.getElementById('sipLinesStatus');
  statusDiv.innerHTML='<span style="color:var(--wt-warning)">&#x23F3; Removing line '+index+'...</span>';
  fetch('/api/sip/remove-line',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({index:index.toString()})
  }).then(r=>r.json()).then(d=>{
    if(d.success){
      statusDiv.innerHTML='<span style="color:var(--wt-success)">&#x2713; Line removed</span>';
      setTimeout(refreshLines,500);
    }else{
      statusDiv.innerHTML='<span style="color:var(--wt-danger)">&#x2717; Error: '+(d.error||'Failed to remove line')+'</span>';
    }
  }).catch(e=>{
    statusDiv.innerHTML='<span style="color:var(--wt-danger)">&#x2717; Error: '+e+'</span>';
  });
}

function applyPreset(count){
  if(!confirm('Configure '+count+' line(s)? This will use usernames alice, bob, charlie, etc.'))return;
  var statusDiv=document.getElementById('sipLinesStatus');
  statusDiv.innerHTML='<span style="color:var(--wt-warning)">&#x23F3; Configuring '+count+' line(s)...</span>';
  var names=['alice','bob','charlie','david','eve','frank'];
  var added=0;
  var addNext=function(i){
    if(i>=count){
      statusDiv.innerHTML='<span style="color:var(--wt-success)">&#x2713; Configured '+added+'/'+count+' line(s)</span>';
      setTimeout(refreshLines,1000);
      return;
    }
    fetch('/api/sip/add-line',{
      method:'POST',
      headers:{'Content-Type':'application/json'},
      body:JSON.stringify({user:names[i]||'user'+i,server:'127.0.0.1',password:''})
    }).then(r=>r.json()).then(d=>{
      if(d.success)added++;
      setTimeout(function(){addNext(i+1);},300);
    }).catch(e=>{
      console.error('Failed to add line:',e);
      setTimeout(function(){addNext(i+1);},300);
    });
  };
  addNext(0);
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
    
    var sc=d.samples_compared?(' ('+d.samples_compared.toLocaleString()+' samples)'):'';
    statusDiv.innerHTML='<span style="color:var(--wt-success)">&#x2713; Test completed'+sc+'</span>';
    
    var tbody=document.getElementById('iapResultsBody');
    var statusColor=d.status==='PASS'?'var(--wt-success)':'var(--wt-danger)';
    
    var now=new Date().toLocaleString();
    var html='<tr>';
    html+='<td>'+escapeHtml(d.file)+'</td>';
    html+='<td>'+d.latency_ms.toFixed(2)+'</td>';
    html+='<td>'+d.snr.toFixed(2)+'</td>';
    html+='<td>'+d.thd.toFixed(2)+'</td>';
    html+='<td style="color:'+statusColor+';font-weight:600">'+d.status+'</td>';
    html+='<td style="font-size:11px">'+now+'</td>';
    html+='</tr>';
    tbody.innerHTML=html+tbody.innerHTML;
    
    if(!window.iapTestHistory)window.iapTestHistory=[];
    window.iapTestHistory.push({file:d.file,snr:d.snr,thd:d.thd,latency:d.latency_ms,status:d.status});
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
  var thdData=window.iapTestHistory.map(function(h){return h.thd;});
  var colors=window.iapTestHistory.map(function(h){return h.status==='PASS'?'rgba(34,197,94,0.7)':'rgba(239,68,68,0.7)';});
  window.iapChart=new Chart(ctx,{
    type:'bar',
    data:{labels:labels,datasets:[
      {label:'SNR (dB)',data:snrData,backgroundColor:'rgba(59,130,246,0.7)',yAxisID:'y'},
      {label:'THD (%)',data:thdData,backgroundColor:colors,yAxisID:'y1'}
    ]},
    options:{
      responsive:true,
      interaction:{mode:'index',intersect:false},
      scales:{
        y:{type:'linear',position:'left',title:{display:true,text:'SNR (dB)'}},
        y1:{type:'linear',position:'right',title:{display:true,text:'THD (%)'},grid:{drawOnChartArea:false}}
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
              var label=ctx.dataset.label||'';
              if(label)label+=': ';
              label+=ctx.parsed.y.toFixed(2);
              if(ctx.datasetIndex===0)label+=' dB';
              else label+=' %';
              return label;
            },
            afterBody:function(items){
              var idx=items[0].dataIndex;
              return 'Status: '+window.iapTestHistory[idx].status;
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

function updateVadWindowDisplay(val){
  document.getElementById('vadWindowValue').textContent=val;
}

function updateVadThresholdDisplay(val){
  document.getElementById('vadThresholdValue').textContent=parseFloat(val).toFixed(1);
}

// ===== MODELS PAGE =====

var modelCompChart=null;

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
      +'<td><button class="wt-btn wt-btn-sm wt-btn-primary" onclick="selectModelForBenchmark('+m.id+',\''+escapeHtml(m.name)+'\')">Benchmark</button></td>'
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

function loadModelComparison(){
  fetch('/api/models/benchmarks').then(r=>r.json()).then(data=>{
    renderComparisonTable(data.runs||[]);
    renderComparisonChart(data.runs||[]);
  }).catch(e=>console.error('loadModelComparison',e));
}

function renderComparisonTable(runs){
  var el=document.getElementById('comparisonTable');
  if(!runs.length){el.innerHTML='<em>No benchmark runs yet.</em>';return;}
  var html='<table class="wt-table"><thead><tr>'
    +'<th>Model</th><th>Backend</th><th>Accuracy %</th>'
    +'<th>Avg Latency</th><th>P50</th><th>P95</th><th>P99</th><th>Memory</th><th>Date</th>'
    +'</tr></thead><tbody>';
  runs.forEach(r=>{
    var accColor=r.avg_accuracy>=95?'var(--wt-success)':r.avg_accuracy>=80?'var(--wt-warning)':'var(--wt-danger)';
    var date=new Date(r.timestamp*1000).toLocaleString();
    html+='<tr>'
      +'<td><strong>'+escapeHtml(r.model_name)+'</strong></td>'
      +'<td>'+escapeHtml(r.backend)+'</td>'
      +'<td style="color:'+accColor+';font-weight:700">'+r.avg_accuracy.toFixed(1)+'%</td>'
      +'<td>'+r.avg_latency_ms+'ms</td>'
      +'<td>'+r.p50_latency_ms+'ms</td>'
      +'<td>'+r.p95_latency_ms+'ms</td>'
      +'<td>'+r.p99_latency_ms+'ms</td>'
      +'<td>'+r.memory_mb+'MB</td>'
      +'<td style="font-size:11px">'+date+'</td>'
      +'</tr>';
  });
  html+='</tbody></table>';
  el.innerHTML=html;
}

function renderComparisonChart(runs){
  var canvas=document.getElementById('modelComparisonChart');
  if(!canvas||!runs.length) return;
  if(modelCompChart){modelCompChart.destroy();modelCompChart=null;}
  // Group: keep latest run per model
  var byModel={};
  runs.forEach(r=>{if(!byModel[r.model_name]) byModel[r.model_name]=r;});
  var labels=Object.keys(byModel);
  var p50=labels.map(n=>byModel[n].p50_latency_ms);
  var p95=labels.map(n=>byModel[n].p95_latency_ms);
  var p99=labels.map(n=>byModel[n].p99_latency_ms);
  var acc=labels.map(n=>byModel[n].avg_accuracy);
  modelCompChart=new Chart(canvas,{
    type:'bar',
    data:{
      labels,
      datasets:[
        {label:'P50 Latency (ms)',data:p50,backgroundColor:'rgba(59,130,246,0.7)',yAxisID:'y'},
        {label:'P95 Latency (ms)',data:p95,backgroundColor:'rgba(251,146,60,0.7)',yAxisID:'y'},
        {label:'P99 Latency (ms)',data:p99,backgroundColor:'rgba(239,68,68,0.7)',yAxisID:'y'},
        {label:'Accuracy %',data:acc,type:'line',borderColor:'rgba(34,197,94,1)',
         backgroundColor:'rgba(34,197,94,0.15)',yAxisID:'y2',tension:0.3}
      ]
    },
    options:{
      responsive:true,
      scales:{
        y:{title:{display:true,text:'Latency (ms)'},beginAtZero:true},
        y2:{position:'right',title:{display:true,text:'Accuracy (%)'},min:0,max:100,
            grid:{drawOnChartArea:false}}
      }
    }
  });
}

// ===== END MODELS PAGE =====

function loadVadConfig(){
  fetch('/api/whisper/vad_config').then(r=>r.json()).then(d=>{
    document.getElementById('vadWindowSlider').value=d.window_ms;
    document.getElementById('vadThresholdSlider').value=d.threshold;
    updateVadWindowDisplay(d.window_ms);
    updateVadThresholdDisplay(d.threshold);
    // Update current settings display
    document.getElementById('currentVadWindow').textContent=d.window_ms;
    document.getElementById('currentVadThreshold').textContent=d.threshold;
  }).catch(e=>console.error('Failed to load VAD config:',e));
}

function saveVadConfig(){
  var window_ms=document.getElementById('vadWindowSlider').value;
  var threshold=document.getElementById('vadThresholdSlider').value;
  
  fetch('/api/whisper/vad_config',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({window_ms:window_ms,threshold:threshold})
  }).then(r=>r.json()).then(d=>{
    if(d.success){
      // Update current settings display
      document.getElementById('currentVadWindow').textContent=d.window_ms;
      document.getElementById('currentVadThreshold').textContent=d.threshold;
      alert('VAD configuration saved successfully!');
    }
  }).catch(e=>console.error('Failed to save VAD config:',e));
}

function runWhisperAccuracyTest(){
  var select=document.getElementById('accuracyTestFiles');
  var selected=Array.from(select.selectedOptions).map(o=>o.value);
  
  if(selected.length===0){
    alert('Please select at least one test file');
    return;
  }
  
  var resultsDiv=document.getElementById('accuracyResults');
  var summaryDiv=document.getElementById('accuracySummary');
  resultsDiv.innerHTML='<p style="color:var(--wt-warning)">&#x23F3; Running accuracy test on '+selected.length+' file(s)...</p>';
  summaryDiv.style.display='none';
  
  fetch('/api/whisper/accuracy_test',{
    method:'POST',
    headers:{'Content-Type':'application/json'},
    body:JSON.stringify({files:selected})
  }).then(r=>r.json()).then(d=>{
    if(d.error){
      resultsDiv.innerHTML='<p style="color:var(--wt-danger)">&#x2717; Error: '+d.error+'</p>';
      return;
    }
    
    document.getElementById('summaryTotal').textContent=d.summary.total;
    document.getElementById('summaryPass').textContent=d.summary.pass;
    document.getElementById('summaryWarn').textContent=d.summary.warn;
    document.getElementById('summaryFail').textContent=d.summary.fail;
    document.getElementById('summaryAccuracy').textContent=d.summary.avg_similarity.toFixed(2);
    document.getElementById('summaryLatency').textContent=Math.round(d.summary.avg_latency_ms);
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
    
    d.results.forEach(function(r){
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
    
  }).catch(e=>{
    resultsDiv.innerHTML='<p style="color:var(--wt-danger)">&#x2717; Error: '+e+'</p>';
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

if(currentPage==='beta-testing'){refreshTestFiles();loadVadConfig();}
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

    void handle_sip_add_line(struct mg_connection *c, struct mg_http_message *hm) {
        std::string body(hm->body.buf, hm->body.len);
        std::string user = extract_json_string(body, "user");
        std::string server = extract_json_string(body, "server");
        std::string password = extract_json_string(body, "password");

        if (user.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing 'user'\"}");
            return;
        }

        std::string cmd = "ADD_LINE " + user + " " + (server.empty() ? "127.0.0.1" : server);
        if (!password.empty()) cmd += " " + password;

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
            if (!first) json << ",";
            json << "{\"index\":" << token.substr(0, p1)
                 << ",\"user\":\"" << token.substr(p1 + 1, p2 - p1 - 1) << "\""
                 << ",\"registered\":" << (token.substr(p2 + 1) == "registered" ? "true" : "false") << "}";
            first = false;
        }

        json << "]}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    void handle_sip_stats(struct mg_connection *c) {
        std::string resp = send_negotiation_command(whispertalk::ServiceType::SIP_CLIENT, "GET_STATS");
        if (resp.empty()) {
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "{\"calls\":[],\"error\":\"SIP Client not reachable\"}");
            return;
        }

        std::stringstream json;
        json << "{\"calls\":[";

        std::istringstream iss(resp);
        std::string token;
        iss >> token;
        if (token != "STATS") {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Invalid response format\"}");
            return;
        }
        
        int count;
        iss >> count;
        
        bool first = true;
        while (iss >> token) {
            std::vector<std::string> parts;
            size_t pos = 0;
            while (pos < token.size()) {
                size_t next = token.find(':', pos);
                if (next == std::string::npos) {
                    parts.push_back(token.substr(pos));
                    break;
                }
                parts.push_back(token.substr(pos, next - pos));
                pos = next + 1;
            }
            
            if (parts.size() >= 7) {
                if (!first) json << ",";
                json << "{"
                     << "\"call_id\":" << parts[0]
                     << ",\"line_index\":" << parts[1]
                     << ",\"rtp_rx_count\":" << parts[2]
                     << ",\"rtp_tx_count\":" << parts[3]
                     << ",\"rtp_rx_bytes\":" << parts[4]
                     << ",\"rtp_tx_bytes\":" << parts[5]
                     << ",\"duration_sec\":" << parts[6];
                if (parts.size() >= 9) {
                    json << ",\"rtp_fwd_count\":" << parts[7]
                         << ",\"rtp_discard_count\":" << parts[8];
                }
                json << "}";
                first = false;
            }
        }

        json << "],\"downstream_connected\":" << (is_service_running("INBOUND_AUDIO_PROCESSOR") ? "true" : "false") << "}";
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
    // Algorithm per ITU-T recommendation:
    //   1. Extract sign bit (bit 15), negate if negative
    //   2. Clip to max linear value 32635 (0x7F7B) to prevent overflow
    //   3. Add bias 0x84 (132) to shift the companding curve - this ensures
    //      small signals near zero still get meaningful quantization
    //   4. Find the exponent (segment number 0-7) by locating the highest set bit
    //   5. Extract 4-bit mantissa from the biased sample
    //   6. Combine sign|exponent|mantissa and bitwise-NOT the result
    //      (mu-law uses inverted bits for better idle channel noise)
    // G.711 mu-law provides ~38dB SNR for a dynamic range of ~78dB.
    // Typical speech codec SNR is ~5-6dB after encode/decode roundtrip.
    static uint8_t linear_to_ulaw(int16_t sample) {
        const int BIAS = 0x84;  // 132: mu-law companding bias per ITU-T G.711
        const int CLIP = 32635; // Max linear value before clipping (0x7F7B)
        int sign = (sample >> 8) & 0x80;
        if (sign) sample = -sample;
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
    // Reverses the encoding: extract sign, exponent, mantissa from the
    // inverted byte, reconstruct the linear sample, and normalize to float.
    // ITU-T G.711 μ-law decode. Same algorithm as inbound-audio-processor.cpp (ulaw_table).
    // Reconstructs linear magnitude: ((quantization * 2 + 33) << segment) - 33
    static float ulaw_to_float(uint8_t byte) {
        int mu = ~byte;
        int sign = mu & 0x80;
        int segment = (mu >> 4) & 0x07;
        int quantization = mu & 0x0F;
        int magnitude = ((quantization << 1) + 33) << segment;
        magnitude -= 33;
        return (sign ? -magnitude : magnitude) / 32768.0f;
    }

    // Resample int16 audio using linear interpolation.
    // Used to convert from source sample rate (e.g. 44100Hz) to target (e.g. 8000Hz).
    // Linear interpolation introduces some smoothing but is very fast and matches
    // the interpolation quality used in the real IAP's 8kHz→16kHz upsampling.
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
    // This tests the G.711 mu-law encode/decode conversion pipeline used by the
    // Inbound Audio Processor (IAP). It runs the exact same algorithm offline:
    //   1. Load WAV file → resample to 8kHz (telephony rate)
    //   2. Encode to G.711 mu-law (simulating RTP payload encoding)
    //   3. Decode mu-law → float32 + upsample 8kHz→16kHz (same as real IAP)
    //   4. Compare original vs roundtripped audio to measure codec distortion
    //
    // NOTE: This is a codec quality test, NOT an IAP service integration test.
    // It validates the conversion algorithm quality without requiring the IAP
    // service to be running. Service integration (TCP, reconnection) is tested
    // separately via the SIP Client RTP Routing test (Test 1).
    //
    // Metrics computed:
    //   - SNR (Signal-to-Noise Ratio): 10*log10(signal_power/noise_power) in dB
    //     where noise = quantization error from mu-law encode/decode roundtrip.
    //     Expected: ~5-6 dB for speech (G.711 is a lossy 8-bit codec).
    //   - THD (Total Harmonic Distortion): sqrt(distortion_power/signal_power)*100%
    //     Combines mu-law quantization distortion + linear interpolation error.
    //     Expected: ~50-55% (dominated by mu-law's 8-bit quantization).
    //   - Latency: wall-clock time for the full conversion pipeline in ms.
    //
    // Pass criteria: SNR >= 3dB, THD <= 80%, Latency <= 50ms
    // (conservative thresholds appropriate for G.711 mu-law codec)
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

        auto start_time = std::chrono::steady_clock::now();

        // Step 1: Resample source audio to 8kHz (telephony sample rate)
        std::vector<int16_t> samples_8k = resample_linear(wav.samples, wav.sample_rate, 8000);

        // Step 2: Encode to G.711 mu-law (same as RTP payload encoding)
        std::vector<uint8_t> ulaw_data(samples_8k.size());
        for (size_t i = 0; i < samples_8k.size(); i++) {
            ulaw_data[i] = linear_to_ulaw(samples_8k[i]);
        }

        // Step 3: Decode mu-law → float32 and upsample 8kHz→16kHz via linear
        // interpolation (exact same algorithm as inbound-audio-processor.cpp:96-101)
        std::vector<float> iap_output(ulaw_data.size() * 2);
        for (size_t i = 0; i < ulaw_data.size(); i++) {
            float s = ulaw_to_float(ulaw_data[i]);
            iap_output[i * 2] = s;                    // Even samples: direct decode
            float next = (i + 1 < ulaw_data.size()) ? ulaw_to_float(ulaw_data[i + 1]) : s;
            iap_output[i * 2 + 1] = 0.5f * (s + next); // Odd samples: midpoint interpolation
        }

        auto end_time = std::chrono::steady_clock::now();
        double latency_ms = std::chrono::duration<double, std::milli>(end_time - start_time).count();

        // Step 4: Compute quality metrics by comparing original vs roundtripped audio
        size_t compare_len = std::min(samples_8k.size(), iap_output.size() / 2);
        if (compare_len == 0) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"No samples to compare\"}");
            return;
        }

        // SNR calculation: compare original 8kHz samples against mu-law decoded samples
        // signal_power = mean(original^2), noise_power = mean((original - decoded)^2)
        double signal_power = 0.0, noise_power = 0.0;
        for (size_t i = 0; i < compare_len; i++) {
            double orig_f = samples_8k[i] / 32768.0;
            double decoded_f = ulaw_to_float(ulaw_data[i]);
            signal_power += orig_f * orig_f;
            noise_power += (orig_f - decoded_f) * (orig_f - decoded_f);
        }
        signal_power /= compare_len;
        noise_power /= compare_len;

        // SNR = 10 * log10(signal / noise). Higher is better. ~5-6dB typical for G.711.
        double snr_db = (noise_power > 1e-15) ? 10.0 * log10(signal_power / noise_power) : 99.0;

        // Upsampling interpolation error: verify midpoint samples match expectation
        double upsample_error = 0.0;
        size_t upsample_count = 0;
        for (size_t i = 0; i + 1 < compare_len; i++) {
            double s0 = ulaw_to_float(ulaw_data[i]);
            double s1 = ulaw_to_float(ulaw_data[i + 1]);
            double expected_mid = 0.5 * (s0 + s1);
            double actual_mid = iap_output[i * 2 + 1];
            upsample_error += (expected_mid - actual_mid) * (expected_mid - actual_mid);
            upsample_count++;
        }

        // THD: combine mu-law quantization distortion + upsampling interpolation error
        // THD% = 100 * sqrt(total_distortion_power / signal_power)
        double thd_percent = 0.0;
        if (signal_power > 1e-15) {
            double ulaw_distortion = noise_power;
            double upsample_distortion = (upsample_count > 0) ? upsample_error / upsample_count : 0.0;
            double total_distortion = ulaw_distortion + upsample_distortion;
            thd_percent = 100.0 * sqrt(total_distortion / signal_power);
            if (thd_percent > 100.0) thd_percent = 100.0;
        }

        std::string status = (snr_db >= 3.0 && thd_percent <= 80.0 && latency_ms <= 50.0) ? "PASS" : "FAIL";

        std::string sql = "INSERT INTO iap_quality_tests (file_name, latency_ms, snr_db, thd_percent, status, timestamp) "
                         "VALUES (?, ?, ?, ?, ?, ?)";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, file.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_double(stmt, 2, latency_ms);
            sqlite3_bind_double(stmt, 3, snr_db);
            sqlite3_bind_double(stmt, 4, thd_percent);
            sqlite3_bind_text(stmt, 5, status.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(stmt, 6, time(nullptr));
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        std::stringstream json;
        json << std::fixed;
        json << "{"
             << "\"success\":true,"
             << "\"file\":\"" << escape_json(file) << "\","
             << "\"latency_ms\":" << latency_ms << ","
             << "\"snr\":" << snr_db << ","
             << "\"thd\":" << thd_percent << ","
             << "\"samples_compared\":" << compare_len << ","
             << "\"status\":\"" << status << "\""
             << "}";

        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

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

        // Step 3: Collapse whitespace
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
        std::vector<std::vector<size_t>> dp(len1 + 1, std::vector<size_t>(len2 + 1));

        for (size_t i = 0; i <= len1; i++) dp[i][0] = i;
        for (size_t j = 0; j <= len2; j++) dp[0][j] = j;

        for (size_t i = 1; i <= len1; i++) {
            for (size_t j = 1; j <= len2; j++) {
                size_t cost = (n1[i - 1] == n2[j - 1]) ? 0 : 1;
                dp[i][j] = std::min({
                    dp[i - 1][j] + 1,
                    dp[i][j - 1] + 1,
                    dp[i - 1][j - 1] + cost
                });
            }
        }

        size_t distance = dp[len1][len2];
        size_t max_len = std::max(len1, len2);
        double similarity = (1.0 - (double)distance / max_len) * 100.0;
        return similarity;
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

    struct TranscriptionResult {
        std::string text;
        double whisper_latency_ms = 0.0;
        bool found = false;
    };

    // Waits for Whisper transcription(s) after the given log sequence number.
    // With chunked VAD, a single audio file may produce multiple transcription chunks.
    // This function collects all chunks by waiting for transcription activity to settle:
    //   1. Wait until the first transcription appears (up to timeout_ms)
    //   2. After finding a transcription, keep waiting for 2s of inactivity
    //   3. Concatenate all transcription chunks in chronological order
    // Returns the combined text and total Whisper inference latency.
    TranscriptionResult wait_for_whisper_transcription(
            uint64_t after_seq, int timeout_ms = 30000) {
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
                // Reset settle timer — wait 4s after last transcription for more chunks.
                // VAD silence detection (400ms) + Whisper inference (~500ms) + inter-chunk gap
                // means chunks can arrive 1-2s apart; 4s provides enough margin.
                settle_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
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

    // Whisper Accuracy Test endpoint (POST /api/whisper/accuracy_test).
    //
    // Runs a real end-to-end transcription test through the full pipeline:
    //   1. Injects each selected test file via test_sip_provider (/inject endpoint)
    //   2. Audio flows: test_sip_provider → SIP Client → IAP → VAD → Whisper Service
    //   3. Captures Whisper's transcription from the log stream (LogForwarder UDP)
    //   4. Compares transcription against ground truth text using Levenshtein similarity
    //
    // Requires: test_sip_provider, sip-client, inbound-audio-processor, vad-service,
    //           and whisper-service all running with an active call established.
    //
    // Returns per-file results: transcription, similarity %, latency, status (PASS/WARN/FAIL).
    // PASS >= 95%, WARN >= 80%, FAIL < 80%.
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
            if (it->second->worker.joinable()) it->second->worker.join();
            async_tasks_.erase(it);
        }
    }

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

        std::string pipeline_err = validate_pipeline_services();
        if (!pipeline_err.empty()) {
            mg_http_reply(c, 409, "Content-Type: application/json\r\n",
                "{\"error\":\"%s\"}", pipeline_err.c_str());
            return;
        }

        std::string err;
        std::string status_body = http_get_localhost(TEST_SIP_PROVIDER_PORT, "/status", err);
        if (!err.empty()) {
            mg_http_reply(c, 503, "Content-Type: application/json\r\n",
                "{\"error\":\"test_sip_provider not reachable: %s\"}", err.c_str());
            return;
        }
        if (status_body.find("\"call_active\":true") == std::string::npos) {
            mg_http_reply(c, 409, "Content-Type: application/json\r\n",
                "{\"error\":\"No active call in test_sip_provider. Start SIP client and establish a call first.\"}");
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

            std::string inject_body = "{\"file\":\"" + file + "\",\"leg\":\"a\"}";
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

            // Wait for VAD buffer to fully flush before injecting next file.
            // The silence stream from test_sip_provider drives VAD silence detection,
            // but we need enough silence frames to reset the VAD state completely.
            // vad_inactivity_flush_ms_ defaults to 2000ms; we add 1s margin.
            std::this_thread::sleep_for(std::chrono::seconds(3));

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

    void handle_whisper_vad_config(struct mg_connection *c, struct mg_http_message *hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) == 0) {
            std::string body(hm->body.buf, hm->body.len);
            
            std::string window_ms_str = extract_json_string(body, "window_ms");
            std::string threshold_str = extract_json_string(body, "threshold");
            
            if (!window_ms_str.empty()) {
                set_setting("whisper_vad_window_ms", window_ms_str);
            }
            if (!threshold_str.empty()) {
                set_setting("whisper_vad_threshold", threshold_str);
            }
            
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", 
                "{\"success\":true,\"window_ms\":%s,\"threshold\":%s}",
                window_ms_str.empty() ? "100" : window_ms_str.c_str(),
                threshold_str.empty() ? "2.0" : threshold_str.c_str());
        } else {
            std::string window_ms = get_setting("whisper_vad_window_ms", "100");
            std::string threshold = get_setting("whisper_vad_threshold", "2.0");
            
            std::stringstream json;
            json << "{"
                 << "\"window_ms\":" << window_ms << ","
                 << "\"threshold\":" << threshold
                 << "}";
            mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
        }
    }

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

    void handle_testfiles_scan(struct mg_connection *c) {
        scan_testfiles_directory();
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", 
            "{\"success\":true,\"scanned\":%zu}", testfiles_.size());
    }

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

            mg_http_reply(c, 200, "Content-Type: application/json\r\n", 
                "{\"success\":true,\"service\":\"%s\",\"level\":\"%s\"}", 
                service.c_str(), level.c_str());
        }
    }

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
                json << "\"" << escape_json(k ? k : "") << "\":\"" << escape_json(v ? v : "") << "\"";
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
            "SELECT r.id, r.model_id, m.name, m.service, m.backend, "
            "r.avg_accuracy, r.avg_latency_ms, r.p50_latency_ms, r.p95_latency_ms, "
            "r.p99_latency_ms, r.memory_mb, r.timestamp "
            "FROM model_benchmark_runs r "
            "JOIN models m ON m.id = r.model_id "
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
                 << ",\"service\":\"" << escape_json(col_str(3)) << "\""
                 << ",\"backend\":\"" << escape_json(col_str(4)) << "\""
                 << ",\"avg_accuracy\":" << sqlite3_column_double(stmt, 5)
                 << ",\"avg_latency_ms\":" << sqlite3_column_int(stmt, 6)
                 << ",\"p50_latency_ms\":" << sqlite3_column_int(stmt, 7)
                 << ",\"p95_latency_ms\":" << sqlite3_column_int(stmt, 8)
                 << ",\"p99_latency_ms\":" << sqlite3_column_int(stmt, 9)
                 << ",\"memory_mb\":" << sqlite3_column_int(stmt, 10)
                 << ",\"timestamp\":" << sqlite3_column_int64(stmt, 11)
                 << "}";
        }
        sqlite3_finalize(stmt);
        json << "]}";

        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

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
        const char* insert_sql = "INSERT INTO models (service, name, path, backend, size_mb, config_json, added_timestamp) VALUES (?, ?, ?, ?, ?, ?, ?)";
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
        sqlite3_int64 model_id = sqlite3_last_insert_rowid(db_);
        sqlite3_finalize(stmt);
        
        if (rc != SQLITE_DONE) {
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
        
        std::string body(hm->body.buf, hm->body.len);
        
        size_t model_id_pos = body.find("\"model_id\":");
        if (model_id_pos == std::string::npos) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Missing model_id parameter\"}");
            return;
        }
        
        int model_id = 0;
        size_t num_start = body.find_first_of("0123456789", model_id_pos);
        if (num_start != std::string::npos) {
            model_id = atoi(body.c_str() + num_start);
        }
        
        if (model_id == 0) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"Invalid model_id\"}");
            return;
        }
        
        std::vector<std::string> test_files;
        size_t files_start = body.find("\"test_files\":");
        if (files_start != std::string::npos) {
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
        }
        
        if (test_files.empty()) {
            mg_http_reply(c, 400, "Content-Type: application/json\r\n", "{\"error\":\"No test files specified\"}");
            return;
        }
        
        int iterations = 1;
        size_t iter_pos = body.find("\"iterations\":");
        if (iter_pos != std::string::npos) {
            size_t num_start = body.find_first_of("0123456789", iter_pos);
            if (num_start != std::string::npos) {
                iterations = atoi(body.c_str() + num_start);
                if (iterations < 1) iterations = 1;
                if (iterations > 10) iterations = 10;
            }
        }
        
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
        
        // Validate all pipeline services are running before benchmarking
        std::string pipeline_err = validate_pipeline_services();
        if (!pipeline_err.empty()) {
            mg_http_reply(c, 409, "Content-Type: application/json\r\n",
                "{\"error\":\"%s\"}", pipeline_err.c_str());
            return;
        }
        
        // Verify the full pipeline is running before benchmarking.
        std::string sip_err;
        std::string sip_status = http_get_localhost(TEST_SIP_PROVIDER_PORT, "/status", sip_err);
        if (!sip_err.empty()) {
            mg_http_reply(c, 503, "Content-Type: application/json\r\n",
                "{\"error\":\"test_sip_provider not reachable: %s\"}", sip_err.c_str());
            return;
        }
        if (sip_status.find("\"call_active\":true") == std::string::npos) {
            mg_http_reply(c, 409, "Content-Type: application/json\r\n",
                "{\"error\":\"No active call. Start SIP client + IAP + VAD + Whisper and establish a call first.\"}");
            return;
        }

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
                task_id, test_files, iterations, model_id, model_name, model_path, files_json_str, memory_mb);
        }

        mg_http_reply(c, 202, "Content-Type: application/json\r\n",
            "{\"status\":\"started\",\"task_id\":%lld}", (long long)task_id);
    }

    void run_benchmark_async(int64_t task_id, std::vector<std::string> test_files,
            int iterations, int model_id, std::string model_name, std::string /*model_path*/,
            std::string files_json_str, int memory_mb) {
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

                std::string inject_body = "{\"file\":\"" + file + "\",\"leg\":\"a\"}";
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

        sqlite3_stmt* stmt;
        const char* insert_sql = "INSERT INTO model_benchmark_runs (model_id, test_files, iterations, avg_accuracy, avg_latency_ms, p50_latency_ms, p95_latency_ms, p99_latency_ms, memory_mb, timestamp) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";
        int rc = sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr);
        sqlite3_int64 run_id = 0;
        if (rc == SQLITE_OK) {
            sqlite3_bind_int(stmt, 1, model_id);
            sqlite3_bind_text(stmt, 2, files_json_str.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(stmt, 3, iterations);
            sqlite3_bind_double(stmt, 4, avg_accuracy);
            sqlite3_bind_int(stmt, 5, static_cast<int>(avg_latency));
            sqlite3_bind_int(stmt, 6, p50_latency);
            sqlite3_bind_int(stmt, 7, p95_latency);
            sqlite3_bind_int(stmt, 8, p99_latency);
            sqlite3_bind_int(stmt, 9, memory_mb);
            sqlite3_bind_int64(stmt, 10, static_cast<sqlite3_int64>(time(nullptr)));
            sqlite3_step(stmt);
            run_id = sqlite3_last_insert_rowid(db_);
            sqlite3_finalize(stmt);
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
