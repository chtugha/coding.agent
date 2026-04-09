#pragma once
#include <string>

inline bool FrontendServer::init_database() {
    int rc = sqlite3_open(db_path_.c_str(), &db_);
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

    const char* seed = R"(
        INSERT OR IGNORE INTO service_config (service, binary_path, default_args, description) VALUES
            ('SIP_CLIENT', 'bin/sip-client', '', 'SIP client / RTP gateway'),
            ('INBOUND_AUDIO_PROCESSOR', 'bin/inbound-audio-processor', '', 'G.711 decode + 8kHz to 16kHz resample'),
            ('VAD_SERVICE', 'bin/vad-service', '', 'Voice Activity Detection + speech segmentation'),
            ('WHISPER_SERVICE', 'bin/whisper-service', '--language de --model bin/models/ggml-large-v3-turbo-q5_0.bin', 'Whisper ASR (Metal)'),
            ('LLAMA_SERVICE', 'bin/llama-service', '', 'LLaMA 3.2-1B response generation'),
            ('KOKORO_SERVICE', 'bin/kokoro-service', '', 'Kokoro TTS (CoreML)'),
            ('NEUTTS_SERVICE', 'bin/neutts-service', '', 'NeuTTS Nano German TTS (CoreML)'),
            ('OUTBOUND_AUDIO_PROCESSOR', 'bin/outbound-audio-processor', '', 'TTS audio to G.711 encode + RTP'),
            ('TEST_SIP_PROVIDER', 'bin/test_sip_provider', '--port 5060 --http-port 22011 --testfiles-dir Testfiles', 'SIP B2BUA test provider for audio injection');
        UPDATE service_config SET default_args='--language de --model bin/models/ggml-large-v3-turbo-q5_0.bin', description='Whisper ASR (Metal)' WHERE service='WHISPER_SERVICE' AND default_args LIKE '%models/ggml%' AND default_args NOT LIKE '%bin/models%';
        UPDATE service_config SET default_args='' WHERE service='SIP_CLIENT' AND (default_args='--lines 1 alice 127.0.0.1 5060' OR default_args='--lines 2 alice 127.0.0.1 5060');
    )";
    sqlite3_exec(db_, seed, nullptr, nullptr, nullptr);

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
        std::string upper_query = query;
        for (auto& ch : upper_query) ch = toupper(ch);
        if (upper_query.find("DROP TABLE") != std::string::npos ||
            upper_query.find("DROP INDEX") != std::string::npos ||
            upper_query.find("TRUNCATE") != std::string::npos) {
            mg_http_reply(c, 403, "Content-Type: application/json\r\n",
                         "{\"error\":\"DROP TABLE, DROP INDEX, and TRUNCATE are blocked for safety.\"}");
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
