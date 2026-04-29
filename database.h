#pragma once
#include <string>
#include <vector>
#include <sys/stat.h>
#include <cstdio>
#include <ctime>
#include <cerrno>
#include <cctype>
#include <iostream>
#include "db_key.h"

inline bool database_exists(const std::string& db_path) {
    struct stat st;
    return stat(db_path.c_str(), &st) == 0;
}

inline bool backup_database(const std::string& db_path) {
    char timestamp[20];
    time_t now = time(nullptr);
    struct tm tm_buf;
    localtime_r(&now, &tm_buf);
    strftime(timestamp, sizeof(timestamp), "%Y%m%dT%H%M%S", &tm_buf);
    std::string backup_path = db_path + ".bak." + timestamp;
    if (rename(db_path.c_str(), backup_path.c_str()) != 0) {
        std::cerr << "Error: failed to backup database to " << backup_path << ": " << strerror(errno) << "\n";
        return false;
    }
    std::cout << "Database backed up to: " << backup_path << "\n";
    return true;
}

inline char prompt_database_action(const std::string& db_path) {
    std::cout << "\nExisting database found: " << db_path << "\n";
    std::cout << "  [R] Reuse existing database\n";
    std::cout << "  [N] Create new database (backs up existing)\n";
    while (true) {
        std::cout << "Choice [R/N]: ";
        std::cout.flush();
        std::string line;
        if (!std::getline(std::cin, line)) return 'R';
        if (line.empty()) continue;
        char ch = static_cast<char>(toupper(static_cast<unsigned char>(line[0])));
        if (ch == 'R' || ch == 'N') return ch;
        std::cout << "Invalid choice. Please enter R or N.\n";
    }
}

inline bool FrontendServer::validate_schema() {
    if (!db_) {
        std::cerr << "Error: database not initialized.\n";
        return false;
    }

    struct CanonicalTable {
        const char* name;
        std::vector<const char*> columns;
    };

    std::vector<CanonicalTable> canonical = {
        {"logs", {"id", "timestamp", "service", "call_id", "level", "message"}},
        {"test_runs", {"id", "test_name", "start_time", "end_time", "exit_code", "arguments", "log_file"}},
        {"service_status", {"service", "status", "last_seen", "call_count", "ports"}},
        {"service_config", {"service", "binary_path", "default_args", "description", "auto_start"}},
        {"settings", {"key", "value"}},
        {"testfiles", {"name", "size_bytes", "duration_sec", "sample_rate", "channels", "ground_truth", "last_modified"}},
        {"whisper_accuracy_tests", {"id", "test_run_id", "file_name", "model_name", "ground_truth", "transcription", "similarity_percent", "latency_ms", "status", "timestamp"}},
        {"iap_quality_tests", {"id", "file_name", "latency_ms", "snr_db", "rms_error_pct", "max_latency_ms", "status", "timestamp"}},
        {"models", {"id", "service", "name", "path", "backend", "size_mb", "config_json", "added_timestamp"}},
        {"model_benchmark_runs", {"id", "model_id", "model_name", "model_type", "backend", "test_files", "iterations", "files_tested", "avg_accuracy", "avg_latency_ms", "p50_latency_ms", "p95_latency_ms", "p99_latency_ms", "memory_mb", "pass_count", "fail_count", "avg_tokens", "interrupt_latency_ms", "german_pct", "timestamp"}},
        {"tts_validation_tests", {"id", "line1_call_id", "line2_call_id", "original_text", "tts_transcription", "similarity_percent", "phoneme_errors", "timestamp"}},
        {"sip_lines", {"line_id", "username", "password", "server", "port", "status", "last_registered"}},
        {"service_test_runs", {"id", "service", "test_type", "status", "metrics_json", "timestamp"}},
        {"test_results", {"id", "test_name", "service", "status", "details", "timestamp"}},
        {"users", {"id", "username", "password_hash", "salt", "created_at"}},
        {"sessions", {"token", "username", "created_at", "last_seen"}},
    };

    auto get_columns = [this](const char* table_name) -> std::vector<std::string> {
        std::vector<std::string> cols;
        std::string pragma = "PRAGMA table_info(" + std::string(table_name) + ")";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, pragma.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "  Error: could not query PRAGMA table_info for " << table_name
                      << ": " << sqlite3_errmsg(db_) << "\n";
            return cols;
        }
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* col_name = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
            if (col_name) cols.emplace_back(col_name);
        }
        sqlite3_finalize(stmt);
        return cols;
    };

    auto table_exists = [this](const char* table_name) -> bool {
        std::string sql = "SELECT name FROM sqlite_master WHERE type='table' AND name=?";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &stmt, nullptr) != SQLITE_OK) {
            std::cerr << "  Error: could not query sqlite_master for " << table_name
                      << ": " << sqlite3_errmsg(db_) << "\n";
            return false;
        }
        sqlite3_bind_text(stmt, 1, table_name, -1, SQLITE_STATIC);
        bool exists = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return exists;
    };

    bool has_errors = false;

    for (const auto& table : canonical) {
        auto existing = get_columns(table.name);

        if (existing.empty()) {
            if (table_exists(table.name)) {
                std::cerr << "  Error: table " << table.name << " exists but has no columns\n";
            } else {
                std::cerr << "  Error: table missing after init: " << table.name << "\n";
            }
            has_errors = true;
            continue;
        }

        for (const auto& col : table.columns) {
            bool found = false;
            for (const auto& ec : existing) {
                if (ec == col) { found = true; break; }
            }
            if (!found) {
                std::cerr << "  Error: column missing after init: " << table.name << "." << col << "\n";
                has_errors = true;
            }
        }
    }

    if (has_errors) {
        std::cerr << "Database schema has unresolved issues.\n";
    } else {
        std::cout << "Database schema is up to date.\n";
    }

    return !has_errors;
}

inline bool FrontendServer::init_database() {
    int rc = prodigy_db::db_open_encrypted(db_path_.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "Cannot open database: " << sqlite3_errmsg(db_) << "\n";
        db_ = nullptr;
        return false;
    }

    if (sqlite3_db_readonly(db_, "main") == 1) {
        std::cerr << "Fatal: database is read-only: " << db_path_ << "\n";
        sqlite3_close(db_);
        db_ = nullptr;
        return false;
    }

    int cfg_rc = sqlite3_db_config(db_, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 0, nullptr);
    if (cfg_rc != SQLITE_OK) {
        std::cerr << "Warning: could not disable SQLite extension loading (rc="
                  << cfg_rc << ")\n";
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

        CREATE TABLE IF NOT EXISTS users (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            username TEXT UNIQUE NOT NULL,
            password_hash TEXT NOT NULL,
            salt TEXT NOT NULL,
            created_at INTEGER NOT NULL
        );

        CREATE TABLE IF NOT EXISTS sessions (
            token TEXT PRIMARY KEY,
            username TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            last_seen INTEGER NOT NULL
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
    if (sqlite3_libversion_number() < 3035000) {
        std::cerr << "Warning: SQLite " << sqlite3_libversion()
                  << " lacks DROP COLUMN support (requires >=3.35.0); some migrations will be skipped\n";
    }
    for (int i = 0; migrations[i]; i++) {
        sqlite3_exec(db_, migrations[i], nullptr, nullptr, nullptr);
    }

    // Order matters: run the KOKORO_SERVICE→KOKORO_ENGINE / NEUTTS_SERVICE→NEUTTS_ENGINE
    // renames BEFORE any INSERT OR IGNORE that would create the target rows. If the INSERTs
    // ran first, the subsequent UPDATE … WHERE service='KOKORO_SERVICE' would hit a PRIMARY
    // KEY conflict on 'KOKORO_ENGINE', sqlite3_exec would silently skip it, and the old
    // orphan rows would linger and show up in the service manager as ghosts. On a fresh DB
    // the UPDATEs affect 0 rows and the INSERTs then populate the table; on an upgrade the
    // UPDATEs rename the old rows and INSERT OR IGNORE is a no-op for them and fills any
    // missing services.
    const char* seed = R"(
        UPDATE service_config SET service='KOKORO_ENGINE', description='Kokoro TTS engine (CoreML) — docks into TTS_SERVICE' WHERE service='KOKORO_SERVICE';
        UPDATE service_config SET service='NEUTTS_ENGINE', description='NeuTTS Nano German TTS engine (CoreML) — docks into TTS_SERVICE' WHERE service='NEUTTS_SERVICE';
        DELETE FROM service_config WHERE service IN ('KOKORO_SERVICE','NEUTTS_SERVICE');
        INSERT OR IGNORE INTO service_config (service, binary_path, default_args, description) VALUES
            ('SIP_CLIENT', 'bin/sip-client', '', 'SIP client / RTP gateway'),
            ('INBOUND_AUDIO_PROCESSOR', 'bin/inbound-audio-processor', '', 'G.711 decode + 8kHz to 16kHz resample'),
            ('VAD_SERVICE', 'bin/vad-service', '', 'Voice Activity Detection + speech segmentation'),
            ('WHISPER_SERVICE', 'bin/whisper-service', '--language de --model bin/models/ggml-large-v3-turbo-q5_0.bin', 'Whisper ASR (Metal)'),
            ('LLAMA_SERVICE', 'bin/llama-service', '', 'LLaMA 3.2-1B response generation'),
            ('TTS_SERVICE', 'bin/tts-service', '', 'Generic TTS stage/dock (engine hotplug)'),
            ('KOKORO_ENGINE', 'bin/kokoro-service', '', 'Kokoro TTS engine (CoreML) — docks into TTS_SERVICE'),
            ('NEUTTS_ENGINE', 'bin/neutts-service', '', 'NeuTTS Nano German TTS engine (CoreML) — docks into TTS_SERVICE'),
            ('OUTBOUND_AUDIO_PROCESSOR', 'bin/outbound-audio-processor', '', 'TTS audio to G.711 encode + RTP'),
            ('TEST_SIP_PROVIDER', 'bin/test_sip_provider', '--port 5060 --http-port 22011 --testfiles-dir Testfiles', 'SIP B2BUA test provider for audio injection'),
            ('TOMEDO_CRAWL_SERVICE', 'bin/tomedo-crawl', '', 'Tomedo RAG — patient context & caller ID'),
            ('MOSHI_SERVICE', 'bin/moshi-service', '--log-level INFO', 'Moshi full-duplex neural voice service');
        UPDATE service_config SET default_args='--language de --model bin/models/ggml-large-v3-turbo-q5_0.bin', description='Whisper ASR (Metal)' WHERE service='WHISPER_SERVICE' AND default_args LIKE '%models/ggml%' AND default_args NOT LIKE '%bin/models%';
        UPDATE service_config SET default_args='' WHERE service='SIP_CLIENT' AND (default_args='--lines 1 alice 127.0.0.1 5060' OR default_args='--lines 2 alice 127.0.0.1 5060');
    )";
    sqlite3_exec(db_, seed, nullptr, nullptr, nullptr);

    seed_default_admin_if_empty();

    rotate_logs();
    return true;
}

inline void FrontendServer::handle_db_schema(struct mg_connection *c) {
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

inline void FrontendServer::handle_db_write_mode(struct mg_connection *c, struct mg_http_message *hm) {
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

inline void FrontendServer::handle_db_query(struct mg_connection *c, struct mg_http_message *hm) {
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

    {
        std::string stripped = strip_sql_comments(query);
        std::string normalized;
        bool in_space = false;
        for (char c : stripped) {
            if (std::isspace((unsigned char)c)) {
                if (!in_space) { normalized += ' '; in_space = true; }
            } else { normalized += c; in_space = false; }
        }
        std::string upper_query = normalized;
        for (auto& ch : upper_query) ch = toupper(ch);
        if (upper_query.find("DROP TABLE") != std::string::npos ||
            upper_query.find("DROP INDEX") != std::string::npos ||
            upper_query.find("DROP VIEW") != std::string::npos ||
            upper_query.find("DROP TRIGGER") != std::string::npos ||
            upper_query.find("TRUNCATE") != std::string::npos) {
            mg_http_reply(c, 403, "Content-Type: application/json\r\n",
                         "{\"error\":\"DROP TABLE, DROP INDEX, DROP VIEW, DROP TRIGGER, and TRUNCATE are blocked for safety.\"}");
            return;
        }
        if (upper_query.find("ATTACH") != std::string::npos ||
            upper_query.find("DETACH") != std::string::npos) {
            mg_http_reply(c, 403, "Content-Type: application/json\r\n",
                         "{\"error\":\"ATTACH and DETACH are not allowed.\"}");
            return;
        }
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
        if (row_count >= DB_QUERY_ROW_LIMIT) break;
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
         << ",\"truncated\":" << (row_count >= DB_QUERY_ROW_LIMIT ? "true" : "false") << "}";
    sqlite3_finalize(stmt);
    
    mg_http_reply(c, 200, "Content-Type: application/json\r\n", "%s", json.str().c_str());
}
