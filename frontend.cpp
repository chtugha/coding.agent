#include "interconnect.h"
#include "mongoose.h"
#include "sqlite3.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <map>
#include <vector>
#include <mutex>
#include <atomic>
#include <queue>
#include <ctime>
#include <chrono>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
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
    
    std::mutex logs_mutex_;
    std::deque<LogEntry> recent_logs_;
    static constexpr size_t MAX_RECENT_LOGS = 1000;

    std::mutex sse_mutex_;
    std::vector<struct mg_connection*> sse_connections_;
    static constexpr size_t MAX_SSE_CONNECTIONS = 20;

    std::mutex sse_queue_mutex_;
    std::vector<LogEntry> sse_queue_;

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
                ('WHISPER_SERVICE', 'bin/whisper-service', '--language de models/ggml-large-v3-turbo-q5_0.bin', 'Whisper ASR (CoreML/Metal)'),
                ('LLAMA_SERVICE', 'bin/llama-service', '', 'LLaMA 3.2-1B response generation'),
                ('KOKORO_SERVICE', 'bin/kokoro-service', '', 'Kokoro TTS (CoreML)'),
                ('OUTBOUND_AUDIO_PROCESSOR', 'bin/outbound-audio-processor', '', 'TTS audio to G.711 encode + RTP');
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
                    info.default_args = {"--port", "5060", "--http-port", "22011", "--testfiles-dir", "Testfiles"};
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

            ServiceType st = parse_service_type(name);
            if (interconnect_.is_service_alive(st)) {
                std::string bin_name = svc.binary_path;
                size_t slash = bin_name.rfind('/');
                if (slash != std::string::npos) bin_name = bin_name.substr(slash + 1);
                kill_ghost_processes(bin_name);
                usleep(500000);
                if (interconnect_.is_service_alive(st)) return false;
            }

            if (!is_allowed_binary(svc.binary_path)) return false;

            std::string use_args = args_override.empty() ? svc.default_args : args_override;
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
                    interconnect_.unregister_service(parse_service_type(name));
                    return true;
                }
                usleep(100000);
            }
            kill(svc.pid, SIGKILL);
            waitpid(svc.pid, nullptr, 0);
            svc.managed = false;
            svc.pid = 0;
            interconnect_.unregister_service(parse_service_type(name));
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
)PG";
    }

    std::string build_ui_js() {
        std::string port_str = std::to_string(http_port_);
        return R"JS(
var currentPage='tests',currentTest=null,currentSvc=null;
var logSSE=null,svcLogSSE=null,testLogPoll=null;

function showPage(p){
  document.querySelectorAll('.wt-page').forEach(e=>e.classList.remove('active'));
  document.getElementById('page-'+p).classList.add('active');
  document.querySelectorAll('.wt-nav-item').forEach(e=>{
    e.classList.toggle('active',e.dataset.page===p);
  });
  currentPage=p;
  if(p==='tests'){showTestsOverview();fetchTests();}
  if(p==='services'){showServicesOverview();fetchServices();}
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
        'WHISPER_SERVICE':'Whisper ASR','LLAMA_SERVICE':'LLaMA LLM','KOKORO_SERVICE':'Kokoro TTS',
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
)JS";
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
            ServiceType st = parse_service_type(svc.name);
            bool alive = interconnect_.is_service_alive(st);
            std::string status = "offline";
            if (svc.managed && svc.pid > 0) {
                status = alive ? "running" : "starting";
            } else if (alive) {
                status = "running (external)";
            }

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

        for (int i = 0; i < 20; i++) {
            ServiceType st = parse_service_type(name);
            if (!interconnect_.is_service_alive(st)) break;
            usleep(100000);
        }

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
        whispertalk::PortConfig ports = interconnect_.query_service_ports(target);
        if (ports.neg_in == 0) return "";

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return "";

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        addr.sin_port = htons(ports.neg_in);

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
                ServiceType st = parse_service_type(svc.name);
                if (interconnect_.is_service_alive(st)) svc_count++;
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
             << ",\"is_master\":" << (interconnect_.is_master() ? "true" : "false")
             << "}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
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
