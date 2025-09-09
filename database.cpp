#include "database.h"
#include <iostream>
#include <chrono>
#include <random>
#include <sstream>
#include <iomanip>

Database::Database() : db_(nullptr) {}

Database::~Database() {
    close();
}

bool Database::init(const std::string& db_path) {
    int rc = sqlite3_open(db_path.c_str(), &db_);
    if (rc != SQLITE_OK) {
        std::cerr << "Cannot open database: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    // Enable WAL mode for better performance
    sqlite3_exec(db_, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
    sqlite3_exec(db_, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);

    return create_tables();
}

void Database::close() {
    if (db_) {
        sqlite3_close(db_);
        db_ = nullptr;
    }
}

bool Database::create_tables() {
    const char* callers_sql = R"(
        CREATE TABLE IF NOT EXISTS callers (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            phone_number TEXT UNIQUE,
            created_at TEXT NOT NULL,
            last_call TEXT NOT NULL
        );
        CREATE INDEX IF NOT EXISTS idx_phone_number ON callers(phone_number);
    )";

    const char* calls_sql = R"(
        CREATE TABLE IF NOT EXISTS calls (
            id INTEGER PRIMARY KEY AUTOINCREMENT,
            call_id TEXT UNIQUE NOT NULL,
            caller_id INTEGER,
            line_id INTEGER,
            phone_number TEXT,
            start_time TEXT NOT NULL,
            end_time TEXT,
            transcription TEXT DEFAULT '',
            llama_response TEXT DEFAULT '',
            status TEXT DEFAULT 'active',
            FOREIGN KEY (caller_id) REFERENCES callers(id)
        );
        CREATE INDEX IF NOT EXISTS idx_call_id ON calls(call_id);
        CREATE INDEX IF NOT EXISTS idx_caller_id ON calls(caller_id);
    )";

    char* err_msg = nullptr;
    int rc = sqlite3_exec(db_, callers_sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "SQL error creating callers table: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    rc = sqlite3_exec(db_, calls_sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "SQL error creating calls table: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    // Try to add llama_response column for existing databases (ignore error if it already exists)
    const char* alter_calls_add_llama = "ALTER TABLE calls ADD COLUMN llama_response TEXT DEFAULT ''";
    sqlite3_exec(db_, alter_calls_add_llama, nullptr, nullptr, nullptr);

    // Create SIP lines table
    const char* sip_lines_sql = R"(
        CREATE TABLE IF NOT EXISTS sip_lines (
            line_id INTEGER PRIMARY KEY AUTOINCREMENT,
            username TEXT NOT NULL,
            password TEXT,
            server_ip TEXT NOT NULL,
            server_port INTEGER NOT NULL DEFAULT 5060,
            enabled BOOLEAN DEFAULT 0,
            status TEXT DEFAULT 'disconnected',
            created_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );
        CREATE INDEX IF NOT EXISTS idx_username ON sip_lines(username);
    )";

    // Create system configuration table
    const char* system_config_sql = R"(
        CREATE TABLE IF NOT EXISTS system_config (
            key TEXT PRIMARY KEY,
            value TEXT NOT NULL,
            updated_at DATETIME DEFAULT CURRENT_TIMESTAMP
        );
        INSERT OR IGNORE INTO system_config (key, value) VALUES ('system_speed', '3');
        INSERT OR IGNORE INTO system_config (key, value) VALUES ('whisper_service_enabled', 'false');
        INSERT OR IGNORE INTO system_config (key, value) VALUES ('whisper_model_path', 'models/ggml-small.en.bin');
        INSERT OR IGNORE INTO system_config (key, value) VALUES ('whisper_service_status', 'stopped');
        INSERT OR IGNORE INTO system_config (key, value) VALUES ('llama_service_enabled', 'false');
        INSERT OR IGNORE INTO system_config (key, value) VALUES ('llama_model_path', 'models/llama-7b-q4_0.gguf');
        INSERT OR IGNORE INTO system_config (key, value) VALUES ('llama_service_status', 'stopped');
        INSERT OR IGNORE INTO system_config (key, value) VALUES ('piper_service_enabled', 'false');
        INSERT OR IGNORE INTO system_config (key, value) VALUES ('piper_model_path', 'models/voice.onnx');
        INSERT OR IGNORE INTO system_config (key, value) VALUES ('piper_espeak_data_path', 'espeak-ng-data');
        INSERT OR IGNORE INTO system_config (key, value) VALUES ('piper_service_status', 'stopped');
    )";

    rc = sqlite3_exec(db_, sip_lines_sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "SQL error: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    rc = sqlite3_exec(db_, system_config_sql, nullptr, nullptr, &err_msg);
    if (rc != SQLITE_OK) {
        std::cerr << "SQL error: " << err_msg << std::endl;
        sqlite3_free(err_msg);
        return false;
    }

    return true;
}

int Database::get_or_create_caller(const std::string& phone_number) {
    if (phone_number.empty()) {
        // Create anonymous caller
        const char* sql = "INSERT INTO callers (phone_number, created_at, last_call) VALUES (NULL, ?, ?)";
        sqlite3_stmt* stmt;

        if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
            std::string timestamp = get_current_timestamp();
            sqlite3_bind_text(stmt, 1, timestamp.c_str(), -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, timestamp.c_str(), -1, SQLITE_STATIC);

            if (sqlite3_step(stmt) == SQLITE_DONE) {
                int caller_id = sqlite3_last_insert_rowid(db_);
                sqlite3_finalize(stmt);
                return caller_id;
            }
        }
        sqlite3_finalize(stmt);
        return -1;
    }

    // Check if caller exists
    const char* select_sql = "SELECT id FROM callers WHERE phone_number = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, select_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, phone_number.c_str(), -1, SQLITE_STATIC);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int caller_id = sqlite3_column_int(stmt, 0);
            sqlite3_finalize(stmt);
            update_caller_last_call(caller_id);
            return caller_id;
        }
    }
    sqlite3_finalize(stmt);

    // Create new caller
    const char* insert_sql = "INSERT INTO callers (phone_number, created_at, last_call) VALUES (?, ?, ?)";
    if (sqlite3_prepare_v2(db_, insert_sql, -1, &stmt, nullptr) == SQLITE_OK) {
        std::string timestamp = get_current_timestamp();
        sqlite3_bind_text(stmt, 1, phone_number.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, timestamp.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, timestamp.c_str(), -1, SQLITE_STATIC);

        if (sqlite3_step(stmt) == SQLITE_DONE) {
            int caller_id = sqlite3_last_insert_rowid(db_);
            sqlite3_finalize(stmt);
            return caller_id;
        }
    }
    sqlite3_finalize(stmt);
    return -1;
}

bool Database::update_caller_last_call(int caller_id) {
    const char* sql = "UPDATE callers SET last_call = ? WHERE id = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        std::string timestamp = get_current_timestamp();
        sqlite3_bind_text(stmt, 1, timestamp.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, caller_id);

        bool success = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return success;
    }
    sqlite3_finalize(stmt);
    return false;
}

// create_session method removed

// get_session method removed

// All session update methods removed

std::string Database::generate_uuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static std::uniform_int_distribution<> dis(0, 15);
    static std::uniform_int_distribution<> dis2(8, 11);

    std::stringstream ss;
    ss << std::hex;
    for (int i = 0; i < 8; i++) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 4; i++) ss << dis(gen);
    ss << "-4";
    for (int i = 0; i < 3; i++) ss << dis(gen);
    ss << "-";
    ss << dis2(gen);
    for (int i = 0; i < 3; i++) ss << dis(gen);
    ss << "-";
    for (int i = 0; i < 12; i++) ss << dis(gen);

    return ss.str();
}

std::string Database::get_current_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    std::stringstream ss;
    ss << std::put_time(std::gmtime(&time_t), "%Y-%m-%d %H:%M:%S");
    return ss.str();
}

// SIP Line Management
int Database::create_sip_line(const std::string& username, const std::string& password,
                             const std::string& server_ip, int server_port) {
    const char* sql = R"(
        INSERT INTO sip_lines (username, password, server_ip, server_port)
        VALUES (?, ?, ?, ?)
    )";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db_) << std::endl;
        return -1;
    }

    sqlite3_bind_text(stmt, 1, username.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, password.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, server_ip.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, server_port);

    rc = sqlite3_step(stmt);
    int line_id = -1;
    if (rc == SQLITE_DONE) {
        line_id = static_cast<int>(sqlite3_last_insert_rowid(db_));
    } else {
        std::cerr << "Failed to insert SIP line: " << sqlite3_errmsg(db_) << std::endl;
    }

    sqlite3_finalize(stmt);
    return line_id;
}

std::vector<SipLineConfig> Database::get_all_sip_lines() {
    std::vector<SipLineConfig> lines;

    const char* sql = R"(
        SELECT line_id, username, password, server_ip, server_port, enabled, status
        FROM sip_lines
        ORDER BY line_id
    )";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db_) << std::endl;
        return lines;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        SipLineConfig line;
        line.line_id = sqlite3_column_int(stmt, 0);
        line.username = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));

        const char* password = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
        line.password = password ? password : "";

        line.server_ip = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 3));
        line.server_port = sqlite3_column_int(stmt, 4);
        line.enabled = sqlite3_column_int(stmt, 5) != 0;

        const char* status = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 6));
        line.status = status ? status : "disconnected";

        lines.push_back(line);
    }

    sqlite3_finalize(stmt);
    return lines;
}

bool Database::update_sip_line_status(int line_id, const std::string& status) {
    const char* sql = "UPDATE sip_lines SET status = ? WHERE line_id = ?";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, line_id);

    rc = sqlite3_step(stmt);
    bool success = (rc == SQLITE_DONE);

    sqlite3_finalize(stmt);
    return success;
}

bool Database::toggle_sip_line(int line_id) {
    const char* sql = "UPDATE sip_lines SET enabled = NOT enabled WHERE line_id = ?";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    sqlite3_bind_int(stmt, 1, line_id);

    rc = sqlite3_step(stmt);
    bool success = (rc == SQLITE_DONE);

    sqlite3_finalize(stmt);
    return success;
}

bool Database::delete_sip_line(int line_id) {
    const char* sql = "DELETE FROM sip_lines WHERE line_id = ?";

    sqlite3_stmt* stmt;
    int rc = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (rc != SQLITE_OK) {
        std::cerr << "Failed to prepare statement: " << sqlite3_errmsg(db_) << std::endl;
        return false;
    }

    sqlite3_bind_int(stmt, 1, line_id);

    rc = sqlite3_step(stmt);
    bool success = (rc == SQLITE_DONE);

    sqlite3_finalize(stmt);
    return success;
}

int Database::get_system_speed() {
    const char* sql = "SELECT value FROM system_config WHERE key = 'system_speed'";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* value = (char*)sqlite3_column_text(stmt, 0);
            int speed = value ? std::stoi(value) : 3;
            sqlite3_finalize(stmt);
            return speed;
        }
    }
    sqlite3_finalize(stmt);
    return 3; // Default speed
}

bool Database::set_system_speed(int speed) {
    const char* sql = "UPDATE system_config SET value = ?, updated_at = ? WHERE key = 'system_speed'";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        std::string timestamp = get_current_timestamp();
        sqlite3_bind_text(stmt, 1, std::to_string(speed).c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, timestamp.c_str(), -1, SQLITE_STATIC);

        bool success = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
        return success;
    }
    sqlite3_finalize(stmt);
    return false;
}

std::vector<Caller> Database::get_all_callers() {
    std::vector<Caller> callers;
    const char* sql = "SELECT id, phone_number, created_at, last_call FROM callers ORDER BY last_call DESC";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            Caller caller;
            caller.id = sqlite3_column_int(stmt, 0);

            const char* phone = (char*)sqlite3_column_text(stmt, 1);
            caller.phone_number = phone ? phone : "";

            const char* created = (char*)sqlite3_column_text(stmt, 2);
            caller.created_at = created ? created : "";

            const char* last_call = (char*)sqlite3_column_text(stmt, 3);
            caller.last_call = last_call ? last_call : "";

            callers.push_back(caller);
        }
    }
    sqlite3_finalize(stmt);
    return callers;
}

// Call Management Implementation
bool Database::create_call(const std::string& call_id, int caller_id, int line_id, const std::string& phone_number) {
    const char* sql = "INSERT INTO calls (call_id, caller_id, line_id, phone_number, start_time, status) VALUES (?, ?, ?, ?, ?, 'active')";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        std::string timestamp = get_current_timestamp();
        sqlite3_bind_text(stmt, 1, call_id.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_int(stmt, 2, caller_id);
        sqlite3_bind_int(stmt, 3, line_id);
        sqlite3_bind_text(stmt, 4, phone_number.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, timestamp.c_str(), -1, SQLITE_STATIC);

        bool success = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);

        if (success) {
            std::cout << "📞 Call record created: " << call_id << " (caller: " << phone_number << ")" << std::endl;
        }

        return success;
    }

    sqlite3_finalize(stmt);
    return false;
}

bool Database::end_call(const std::string& call_id) {
    const char* sql = "UPDATE calls SET end_time = ?, status = 'ended' WHERE call_id = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        std::string timestamp = get_current_timestamp();
        sqlite3_bind_text(stmt, 1, timestamp.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, call_id.c_str(), -1, SQLITE_STATIC);

        bool success = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);

        if (success) {
            std::cout << "📞 Call ended: " << call_id << std::endl;
        }

        return success;
    }

    sqlite3_finalize(stmt);
    return false;
}

bool Database::append_transcription(const std::string& call_id, const std::string& text) {
    const char* sql = "UPDATE calls SET transcription = transcription || ? WHERE call_id = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        std::string text_with_space = " " + text; // Add space before appending
        sqlite3_bind_text(stmt, 1, text_with_space.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, call_id.c_str(), -1, SQLITE_STATIC);

        bool success = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);

        if (success) {
            std::cout << "📝 Transcription appended to call " << call_id << ": " << text << std::endl;
        }

        return success;
    }

    sqlite3_finalize(stmt);
    return false;
}

bool Database::append_llama_response(const std::string& call_id, const std::string& text) {
    const char* sql = "UPDATE calls SET llama_response = llama_response || ? WHERE call_id = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        std::string text_with_space = " " + text; // Add space before appending
        sqlite3_bind_text(stmt, 1, text_with_space.c_str(), -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, call_id.c_str(), -1, SQLITE_STATIC);

        bool success = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);

        if (success) {
            std::cout << "🦙 LLaMA response appended to call " << call_id << ": " << text << std::endl;
        }

        return success;
    }

    sqlite3_finalize(stmt);
    return false;
}


Call Database::get_call(const std::string& call_id) {
    Call call;
    const char* sql = "SELECT id, call_id, caller_id, line_id, phone_number, start_time, end_time, transcription, llama_response, status FROM calls WHERE call_id = ?";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, call_id.c_str(), -1, SQLITE_STATIC);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            call.id = sqlite3_column_int(stmt, 0);
            call.call_id = (char*)sqlite3_column_text(stmt, 1);
            call.caller_id = sqlite3_column_int(stmt, 2);
            call.line_id = sqlite3_column_int(stmt, 3);
            call.phone_number = (char*)sqlite3_column_text(stmt, 4);
            call.start_time = (char*)sqlite3_column_text(stmt, 5);

            const char* end_time = (char*)sqlite3_column_text(stmt, 6);
            call.end_time = end_time ? end_time : "";

            const char* transcription = (char*)sqlite3_column_text(stmt, 7);
            call.transcription = transcription ? transcription : "";

            const char* llama_resp = (char*)sqlite3_column_text(stmt, 8);
            call.llama_response = llama_resp ? llama_resp : "";

            call.status = (char*)sqlite3_column_text(stmt, 9);
        }

        sqlite3_finalize(stmt);
    }

    return call;
}

// get_caller_sessions method removed

// Whisper service management methods
bool Database::get_whisper_service_enabled() {
    const char* sql = "SELECT value FROM system_config WHERE key = 'whisper_service_enabled'";
    sqlite3_stmt* stmt;
    bool enabled = false;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* value = (char*)sqlite3_column_text(stmt, 0);
            enabled = (value && std::string(value) == "true");
        }
        sqlite3_finalize(stmt);
    }

    return enabled;
}

bool Database::set_whisper_service_enabled(bool enabled) {
    const char* sql = "INSERT OR REPLACE INTO system_config (key, value, updated_at) VALUES ('whisper_service_enabled', ?, CURRENT_TIMESTAMP)";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, enabled ? "true" : "false", -1, SQLITE_STATIC);
        int result = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return result == SQLITE_DONE;
    }

    return false;
}

std::string Database::get_whisper_model_path() {
    // Safety check for database connection
    if (!db_) {
        std::cerr << "❌ Database connection is null in get_whisper_model_path()" << std::endl;
        return "models/ggml-small.en.bin"; // default fallback
    }

    const char* sql = "SELECT value FROM system_config WHERE key = 'whisper_model_path'";
    sqlite3_stmt* stmt = nullptr;
    std::string model_path = "models/ggml-small.en.bin"; // default

    int prepare_result = sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr);
    if (prepare_result == SQLITE_OK) {
        int step_result = sqlite3_step(stmt);
        if (step_result == SQLITE_ROW) {
            const char* value = (char*)sqlite3_column_text(stmt, 0);
            if (value) {
                model_path = value;
            }
        } else if (step_result != SQLITE_DONE) {
            std::cerr << "❌ SQLite step error in get_whisper_model_path(): " << sqlite3_errmsg(db_) << std::endl;
        }
        sqlite3_finalize(stmt);
    } else {
        std::cerr << "❌ SQLite prepare error in get_whisper_model_path(): " << sqlite3_errmsg(db_) << std::endl;
    }

    return model_path;
}

bool Database::set_whisper_model_path(const std::string& model_path) {
    const char* sql = "INSERT OR REPLACE INTO system_config (key, value, updated_at) VALUES ('whisper_model_path', ?, CURRENT_TIMESTAMP)";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, model_path.c_str(), -1, SQLITE_STATIC);
        int result = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return result == SQLITE_DONE;
    }

    return false;
}

std::string Database::get_whisper_service_status() {
    const char* sql = "SELECT value FROM system_config WHERE key = 'whisper_service_status'";
    sqlite3_stmt* stmt;
    std::string status = "stopped"; // default

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* value = (char*)sqlite3_column_text(stmt, 0);
            if (value) {
                status = value;
            }
        }
        sqlite3_finalize(stmt);
    }

    return status;
}

bool Database::set_whisper_service_status(const std::string& status) {
    const char* sql = "INSERT OR REPLACE INTO system_config (key, value, updated_at) VALUES ('whisper_service_status', ?, CURRENT_TIMESTAMP)";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_STATIC);
        int result = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return result == SQLITE_DONE;
    }

    return false;
}

// LLaMA service management methods
bool Database::get_llama_service_enabled() {
    const char* sql = "SELECT value FROM system_config WHERE key = 'llama_service_enabled'";
    sqlite3_stmt* stmt;
    bool enabled = false;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* value = (char*)sqlite3_column_text(stmt, 0);
            enabled = (value && std::string(value) == "true");
        }
        sqlite3_finalize(stmt);
    }

    return enabled;
}

bool Database::set_llama_service_enabled(bool enabled) {
    const char* sql = "INSERT OR REPLACE INTO system_config (key, value, updated_at) VALUES ('llama_service_enabled', ?, CURRENT_TIMESTAMP)";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, enabled ? "true" : "false", -1, SQLITE_STATIC);
        int result = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return result == SQLITE_DONE;
    }

    return false;
}

std::string Database::get_llama_model_path() {
    if (!db_) {
        std::cerr << "❌ Database connection is null in get_llama_model_path()" << std::endl;
        return "models/llama-7b-q4_0.gguf";
    }

    const char* sql = "SELECT value FROM system_config WHERE key = 'llama_model_path'";
    sqlite3_stmt* stmt;
    std::string model_path = "models/llama-7b-q4_0.gguf";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* value = (char*)sqlite3_column_text(stmt, 0);
            if (value) {
                model_path = std::string(value);
            }
        }
        sqlite3_finalize(stmt);
    }

    return model_path;
}

bool Database::set_llama_model_path(const std::string& model_path) {
    const char* sql = "INSERT OR REPLACE INTO system_config (key, value, updated_at) VALUES ('llama_model_path', ?, CURRENT_TIMESTAMP)";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, model_path.c_str(), -1, SQLITE_STATIC);
        int result = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return result == SQLITE_DONE;
    }

    return false;
}

std::string Database::get_llama_service_status() {
    const char* sql = "SELECT value FROM system_config WHERE key = 'llama_service_status'";
    sqlite3_stmt* stmt;
    std::string status = "stopped";

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char* value = (char*)sqlite3_column_text(stmt, 0);
            if (value) {
                status = std::string(value);
            }
        }
        sqlite3_finalize(stmt);
    }

    return status;
}

bool Database::set_llama_service_status(const std::string& status) {
    const char* sql = "INSERT OR REPLACE INTO system_config (key, value, updated_at) VALUES ('llama_service_status', ?, CURRENT_TIMESTAMP)";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_STATIC);
        int result = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return result == SQLITE_DONE;
    }

    return false;
}

// Piper service management methods
bool Database::get_piper_service_enabled() {
    const char* sql = "SELECT value FROM system_config WHERE key = 'piper_service_enabled'";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            sqlite3_finalize(stmt);
            return value == "true";
        }
        sqlite3_finalize(stmt);
    }

    return false;
}

bool Database::set_piper_service_enabled(bool enabled) {
    const char* sql = "UPDATE system_config SET value = ?, updated_at = CURRENT_TIMESTAMP WHERE key = 'piper_service_enabled'";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, enabled ? "true" : "false", -1, SQLITE_STATIC);
        int result = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return result == SQLITE_DONE;
    }

    return false;
}

std::string Database::get_piper_model_path() {
    const char* sql = "SELECT value FROM system_config WHERE key = 'piper_model_path'";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            sqlite3_finalize(stmt);
            return value;
        }
        sqlite3_finalize(stmt);
    }

    return "models/voice.onnx";
}

bool Database::set_piper_model_path(const std::string& model_path) {
    const char* sql = "UPDATE system_config SET value = ?, updated_at = CURRENT_TIMESTAMP WHERE key = 'piper_model_path'";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, model_path.c_str(), -1, SQLITE_STATIC);
        int result = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return result == SQLITE_DONE;
    }

    return false;
}

std::string Database::get_piper_espeak_data_path() {
    const char* sql = "SELECT value FROM system_config WHERE key = 'piper_espeak_data_path'";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            sqlite3_finalize(stmt);
            return value;
        }
        sqlite3_finalize(stmt);
    }

    return "espeak-ng-data";
}

bool Database::set_piper_espeak_data_path(const std::string& espeak_data_path) {
    const char* sql = "UPDATE system_config SET value = ?, updated_at = CURRENT_TIMESTAMP WHERE key = 'piper_espeak_data_path'";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, espeak_data_path.c_str(), -1, SQLITE_STATIC);
        int result = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return result == SQLITE_DONE;
    }

    return false;
}

std::string Database::get_piper_service_status() {
    const char* sql = "SELECT value FROM system_config WHERE key = 'piper_service_status'";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            std::string value = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
            sqlite3_finalize(stmt);
            return value;
        }
        sqlite3_finalize(stmt);
    }

    return "stopped";
}

bool Database::set_piper_service_status(const std::string& status) {
    const char* sql = "UPDATE system_config SET value = ?, updated_at = CURRENT_TIMESTAMP WHERE key = 'piper_service_status'";
    sqlite3_stmt* stmt;

    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, status.c_str(), -1, SQLITE_STATIC);
        int result = sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        return result == SQLITE_DONE;
    }

    return false;
}
