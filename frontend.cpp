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
                if (c < 32) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
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
        std::cout << "Frontend logging port: " << log_port_ << "\n";
        std::cout << "Frontend HTTP port: " << http_port_ << "\n";

        log_thread_ = std::thread(&FrontendServer::log_receiver_loop, this);

        mg_mgr_init(&mgr_);
        
        std::string listen_addr = "http://0.0.0.0:" + std::to_string(http_port_);
        struct mg_connection *c = mg_http_listen(&mgr_, listen_addr.c_str(), http_handler_static, this);
        if (c) c->fn_data = this;
        
        std::cout << "Frontend web server started on " << listen_addr << "\n";
        std::cout << "Open http://localhost:" << http_port_ << " in your browser\n";

        auto last_flush = std::chrono::steady_clock::now();
        auto last_rotation = last_flush;
        while (!s_sigint_received) {
            mg_mgr_poll(&mgr_, 500);
            check_test_status();

            auto now = std::chrono::steady_clock::now();
            if (now - last_flush >= std::chrono::milliseconds(500)) {
                flush_log_queue();
                last_flush = now;
            }
            if (now - last_rotation >= std::chrono::hours(1)) {
                rotate_logs();
                last_rotation = now;
            }
        }
        flush_log_queue();

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
    struct mg_mgr mgr_;
    std::thread log_thread_;
    
    std::mutex tests_mutex_;
    std::vector<TestInfo> tests_;
    
    std::mutex logs_mutex_;
    std::deque<LogEntry> recent_logs_;
    static constexpr size_t MAX_RECENT_LOGS = 1000;

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
                ('SIP_CLIENT', 'bin/sip-client', '--lines 2 test 127.0.0.1 5060', 'SIP client / RTP gateway'),
                ('INBOUND_AUDIO_PROCESSOR', 'bin/inbound-audio-processor', '', 'G.711 decode + 8kHz→16kHz resample'),
                ('WHISPER_SERVICE', 'bin/whisper-service', '', 'Whisper ASR (CoreML/Metal)'),
                ('LLAMA_SERVICE', 'bin/llama-service', '', 'LLaMA 3.2-1B response generation'),
                ('KOKORO_SERVICE', 'bin/kokoro-service', '', 'Kokoro TTS (CoreML)'),
                ('OUTBOUND_AUDIO_PROCESSOR', 'bin/outbound-audio-processor', '', 'TTS audio → G.711 encode + RTP');
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
                    info.default_args = {"--port", "5060", "--duration", "60"};
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
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
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

        sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr);
        const char* sql = "INSERT INTO logs (timestamp, service, call_id, level, message) VALUES (?, ?, ?, ?, ?)";
        sqlite3_stmt* stmt;
        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            for (const auto& entry : batch) {
                sqlite3_bind_text(stmt, 1, entry.timestamp.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 2, service_type_to_string(entry.service), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(stmt, 3, static_cast<int>(entry.call_id));
                sqlite3_bind_text(stmt, 4, entry.level.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(stmt, 5, entry.message.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_step(stmt);
                sqlite3_reset(stmt);
            }
            sqlite3_finalize(stmt);
        }
        sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr);
    }

    void rotate_logs() {
        if (!db_) return;
        const char* sql = "DELETE FROM logs WHERE timestamp < datetime('now', '-30 days')";
        sqlite3_exec(db_, sql, nullptr, nullptr, nullptr);
    }

    static void http_handler_static(struct mg_connection *c, int ev, void *ev_data) {
        FrontendServer* self = static_cast<FrontendServer*>(c->fn_data);
        self->http_handler(c, ev, ev_data);
    }

    void http_handler(struct mg_connection *c, int ev, void *ev_data) {
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
            } else if (mg_strcmp(hm->uri, mg_str("/api/services")) == 0) {
                serve_services_api(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/logs")) == 0) {
                serve_logs_api(c);
            } else if (mg_strcmp(hm->uri, mg_str("/api/db/query")) == 0) {
                handle_db_query(c, hm);
            } else {
                mg_http_reply(c, 404, "", "Not Found\n");
            }
        }
    }

    void serve_index(struct mg_connection *c) {
        const char* html_part1 = 
            "<!DOCTYPE html><html lang=\"en\"><head><meta charset=\"UTF-8\">"
            "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">"
            "<title>WhisperTalk Frontend</title>"
            "<link href=\"https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/css/bootstrap.min.css\" rel=\"stylesheet\">"
            "<style>.test-card{margin-bottom:1rem}.test-running{border-left:4px solid #28a745}"
            ".test-idle{border-left:4px solid #6c757d}.service-online{color:#28a745}"
            ".service-offline{color:#dc3545}.log-entry{font-family:monospace;font-size:0.85rem;padding:0.25rem;border-bottom:1px solid #eee}"
            ".log-container{max-height:400px;overflow-y:auto;background:#f8f9fa;padding:1rem}"
            "#liveLogs{max-height:600px;overflow-y:auto}</style></head><body>"
            "<nav class=\"navbar navbar-dark bg-dark\"><div class=\"container-fluid\">"
            "<span class=\"navbar-brand mb-0 h1\">WhisperTalk Frontend</span>"
            "<span class=\"text-light\" id=\"status\">Loading...</span></div></nav>"
            "<div class=\"container-fluid mt-4\"><ul class=\"nav nav-tabs\" id=\"mainTabs\" role=\"tablist\">"
            "<li class=\"nav-item\" role=\"presentation\"><button class=\"nav-link active\" id=\"tests-tab\" data-bs-toggle=\"tab\" data-bs-target=\"#tests\" type=\"button\">Tests</button></li>"
            "<li class=\"nav-item\" role=\"presentation\"><button class=\"nav-link\" id=\"services-tab\" data-bs-toggle=\"tab\" data-bs-target=\"#services\" type=\"button\">Services</button></li>"
            "<li class=\"nav-item\" role=\"presentation\"><button class=\"nav-link\" id=\"logs-tab\" data-bs-toggle=\"tab\" data-bs-target=\"#logs\" type=\"button\">Logs</button></li>"
            "<li class=\"nav-item\" role=\"presentation\"><button class=\"nav-link\" id=\"database-tab\" data-bs-toggle=\"tab\" data-bs-target=\"#database\" type=\"button\">Database</button></li></ul>"
            "<div class=\"tab-content\" id=\"mainTabContent\">"
            "<div class=\"tab-pane fade show active\" id=\"tests\" role=\"tabpanel\"><div class=\"row mt-3\"><div class=\"col-md-12\"><h4>Available Tests</h4><div id=\"testsContainer\"></div></div></div></div>"
            "<div class=\"tab-pane fade\" id=\"services\" role=\"tabpanel\"><div class=\"row mt-3\"><div class=\"col-md-12\"><h4>Service Status</h4><table class=\"table table-striped\"><thead><tr><th>Service</th><th>Status</th><th>Calls</th><th>Ports</th><th>Last Seen</th></tr></thead><tbody id=\"servicesTable\"></tbody></table></div></div></div>"
            "<div class=\"tab-pane fade\" id=\"logs\" role=\"tabpanel\"><div class=\"row mt-3\"><div class=\"col-md-12\"><h4>Live Logs</h4><div class=\"log-container\" id=\"liveLogs\"></div></div></div></div>"
            "<div class=\"tab-pane fade\" id=\"database\" role=\"tabpanel\"><div class=\"row mt-3\"><div class=\"col-md-12\"><h4>Database Query</h4><form id=\"queryForm\">"
            "<div class=\"mb-3\"><label for=\"sqlQuery\" class=\"form-label\">SQL Query</label><textarea class=\"form-control\" id=\"sqlQuery\" rows=\"3\">SELECT * FROM logs ORDER BY timestamp DESC LIMIT 50</textarea></div>"
            "<button type=\"submit\" class=\"btn btn-primary\">Execute Query</button></form><div class=\"mt-3\" id=\"queryResults\"></div></div></div></div></div></div>"
            "<script src=\"https://cdn.jsdelivr.net/npm/bootstrap@5.3.0/dist/js/bootstrap.bundle.min.js\"></script><script>";
        
        const char* html_part2 = 
            "function fetchTests(){fetch('/api/tests').then(r=>r.json()).then(data=>{"
            "document.getElementById('testsContainer').innerHTML=data.tests.map(test=>"
            "'<div class=\"card test-card '+(test.is_running?'test-running':'test-idle')+'\">"
            "<div class=\"card-body\"><h5 class=\"card-title\">'+test.name+(test.is_running?' <span class=\"badge bg-success\">Running</span>':'')+' </h5>"
            "<p class=\"card-text\">'+test.description+'</p><p class=\"text-muted small\">'+test.binary_path+'</p>"
            "'+(test.is_running?'<button class=\"btn btn-sm btn-danger\" onclick=\"stopTest(\\''+test.name+'\\')\" >Stop</button>':'<button class=\"btn btn-sm btn-primary\" onclick=\"startTest(\\''+test.name+'\\')\" >Start</button>')"
            "+(test.log_file?'<a href=\"/logs/'+test.log_file+'\" class=\"btn btn-sm btn-secondary\">View Log</a>':'')"
            "'</div></div>').join('')})}"
            "function fetchServices(){fetch('/api/services').then(r=>r.json()).then(data=>{"
            "document.getElementById('servicesTable').innerHTML=data.services.map(svc=>'<tr><td>'+svc.name+'</td>"
            "<td><span class=\"'+(svc.online?'service-online':'service-offline')+'\"> '+(svc.online?'● Online':'○ Offline')+'</span></td>"
            "<td>'+(svc.calls||0)+'</td><td>'+(svc.ports||'N/A')+'</td><td>'+(svc.last_seen||'Never')+'</td></tr>').join('')})}"
            "function fetchLogs(){fetch('/api/logs').then(r=>r.json()).then(data=>{"
            "var container=document.getElementById('liveLogs');container.innerHTML=data.logs.map(log=>"
            "'<div class=\"log-entry\"><span class=\"text-muted\">'+log.timestamp+'</span> "
            "<span class=\"badge bg-secondary\">'+log.service+'</span> <span>'+log.message+'</span></div>').join('');"
            "container.scrollTop=container.scrollHeight})}"
            "function startTest(name){fetch('/api/tests/start',{method:'POST',headers:{'Content-Type':'application/json'},"
            "body:JSON.stringify({test:name})}).then(()=>setTimeout(fetchTests,500))}"
            "function stopTest(name){fetch('/api/tests/stop',{method:'POST',headers:{'Content-Type':'application/json'},"
            "body:JSON.stringify({test:name})}).then(()=>setTimeout(fetchTests,500))}"
            "document.getElementById('queryForm').addEventListener('submit',(e)=>{e.preventDefault();"
            "var query=document.getElementById('sqlQuery').value;fetch('/api/db/query',{method:'POST',headers:{'Content-Type':'application/json'},"
            "body:JSON.stringify({query:query})}).then(r=>r.json()).then(data=>{"
            "var container=document.getElementById('queryResults');if(data.error){container.innerHTML='<div class=\"alert alert-danger\">'+data.error+'</div>'}"
            "else if(data.rows&&data.rows.length>0){var cols=Object.keys(data.rows[0]);container.innerHTML='<table class=\"table table-sm table-bordered\"><thead><tr>'"
            "+cols.map(c=>'<th>'+c+'</th>').join('')+'</tr></thead><tbody>'+data.rows.map(row=>'<tr>'+cols.map(c=>'<td>'+row[c]+'</td>').join('')+'</tr>').join('')+'</tbody></table>'}"
            "else{container.innerHTML='<div class=\"alert alert-info\">Query executed successfully. '+(data.affected||0)+' rows affected.</div>'}})});"
            "setInterval(fetchTests,2000);setInterval(fetchServices,3000);setInterval(fetchLogs,1000);"
            "fetchTests();fetchServices();fetchLogs();document.getElementById('status').textContent='Frontend running on port ";
        
        std::string port_str = std::to_string(http_port_);
        const char* html_part3 = "';</script></body></html>";
        
        mg_http_reply(c, 200, "Content-Type: text/html\r\n", "%s%s%s%s", html_part1, html_part2, port_str.c_str(), html_part3);
    }

    void serve_tests_api(struct mg_connection *c) {
        std::lock_guard<std::mutex> lock(tests_mutex_);
        
        std::stringstream json;
        json << "{\"tests\":[";
        for (size_t i = 0; i < tests_.size(); i++) {
            if (i > 0) json << ",";
            const auto& t = tests_[i];
            json << "{"
                 << "\"name\":\"" << t.name << "\","
                 << "\"description\":\"" << t.description << "\","
                 << "\"binary_path\":\"" << t.binary_path << "\","
                 << "\"is_running\":" << (t.is_running ? "true" : "false") << ","
                 << "\"pid\":" << t.pid << ","
                 << "\"log_file\":\"" << t.log_file << "\""
                 << "}";
        }
        json << "]}";
        
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    void serve_services_api(struct mg_connection *c) {
        std::stringstream json;
        json << "{\"services\":[";
        
        const char* services[] = {"SIP_CLIENT", "INBOUND_AUDIO_PROCESSOR", "WHISPER_SERVICE", 
                                   "LLAMA_SERVICE", "KOKORO_SERVICE", "OUTBOUND_AUDIO_PROCESSOR"};
        
        for (size_t i = 0; i < 6; i++) {
            if (i > 0) json << ",";
            json << "{"
                 << "\"name\":\"" << services[i] << "\","
                 << "\"online\":false,"
                 << "\"calls\":0,"
                 << "\"ports\":\"N/A\","
                 << "\"last_seen\":\"Never\""
                 << "}";
        }
        
        json << "]}";
        mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
    }

    void serve_logs_api(struct mg_connection *c) {
        std::lock_guard<std::mutex> lock(logs_mutex_);
        
        std::stringstream json;
        json << "{\"logs\":[";
        size_t count = 0;
        for (auto it = recent_logs_.rbegin(); it != recent_logs_.rend() && count < 100; ++it, ++count) {
            if (count > 0) json << ",";
            json << "{"
                 << "\"timestamp\":\"" << it->timestamp << "\","
                 << "\"service\":\"" << service_type_to_string(it->service) << "\","
                 << "\"level\":\"" << it->level << "\","
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
        pos = json.find('"', pos + needle.size() + 1);
        if (pos == std::string::npos) return "";
        size_t end = json.find('"', pos + 1);
        if (end == std::string::npos) return "";
        return json.substr(pos + 1, end - pos - 1);
    }

    void handle_test_start(struct mg_connection *c, struct mg_http_message *hm) {
        std::string body(hm->body.buf, hm->body.len);
        std::string test_name = extract_json_string(body, "test");
        
        std::lock_guard<std::mutex> lock(tests_mutex_);
        for (auto& test : tests_) {
            if (test.name == test_name && !test.is_running) {
                pid_t pid = fork();
                if (pid == 0) {
                    mkdir("logs", 0755);
                    std::string log = "logs/" + test.name + "_" + std::to_string(time(nullptr)) + ".log";
                    int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                    if (fd >= 0) {
                        dup2(fd, STDOUT_FILENO);
                        dup2(fd, STDERR_FILENO);
                        close(fd);
                    }
                    
                    std::vector<char*> args;
                    args.push_back(const_cast<char*>(test.binary_path.c_str()));
                    for (const auto& arg : test.default_args) {
                        args.push_back(const_cast<char*>(arg.c_str()));
                    }
                    args.push_back(nullptr);
                    
                    execv(test.binary_path.c_str(), args.data());
                    exit(1);
                } else if (pid > 0) {
                    test.is_running = true;
                    test.pid = pid;
                    test.start_time = time(nullptr);
                    test.log_file = "logs/" + test.name + "_" + std::to_string(test.start_time) + ".log";
                }
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

    void handle_db_query(struct mg_connection *c, struct mg_http_message *hm) {
        std::string body(hm->body.buf, hm->body.len);
        std::string query = extract_json_string(body, "query");
        
        if (!db_) {
            mg_http_reply(c, 500, "Content-Type: application/json\r\n", "{\"error\":\"Database not available\"}");
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
            if (row_count > 0) json << ",";
            json << "{";
            
            int col_count = sqlite3_column_count(stmt);
            for (int i = 0; i < col_count; i++) {
                if (i > 0) json << ",";
                json << "\"" << sqlite3_column_name(stmt, i) << "\":";
                
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
        
        json << "],\"affected\":" << sqlite3_changes(db_) << "}";
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
