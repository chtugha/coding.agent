#pragma once

#include <string>
#include <memory>
#include <vector>
#include <mutex>
#include <sqlite3.h>

struct Caller {
    int id;
    std::string phone_number;
    std::string created_at;
    std::string last_call;
};

struct Call {
    int id;
    std::string call_id;
    int caller_id;
    int line_id;
    std::string phone_number;  // For quick lookup
    std::string start_time;
    std::string end_time;
    std::string transcription; // Accumulated whisper output
    std::string llama_response; // Accumulated llama output
    std::string status;        // 'active', 'ended', 'missed'
};

struct SipLineConfig {
    int line_id;
    std::string username;
    std::string password;
    std::string server_ip;
    int server_port;
    bool enabled;
    std::string status;
};

class Database {
public:
    Database();
    ~Database();

    bool init(const std::string& db_path = "whisper_talk.db");
    void close();

    // Caller management
    int get_or_create_caller(const std::string& phone_number);
    bool update_caller_last_call(int caller_id);
    std::vector<Caller> get_all_callers();

    // Call management
    bool create_call(const std::string& call_id, int caller_id, int line_id, const std::string& phone_number);
    bool end_call(const std::string& call_id);
    bool append_transcription(const std::string& call_id, const std::string& text);
    bool append_llama_response(const std::string& call_id, const std::string& text);

    Call get_call(const std::string& call_id);

    // Session management removed

    // SIP line management
    int create_sip_line(const std::string& username, const std::string& password,
                       const std::string& server_ip, int server_port);
    std::vector<SipLineConfig> get_all_sip_lines();
    bool update_sip_line_status(int line_id, const std::string& status);
    bool toggle_sip_line(int line_id);
    bool delete_sip_line(int line_id);
    SipLineConfig get_sip_line(int line_id);
    // get_caller_sessions removed

    // System configuration
    int get_system_speed(); // 1-5 scale (1=slow, 5=fast)
    bool set_system_speed(int speed);

    // Whisper service management
    bool get_whisper_service_enabled();
    bool set_whisper_service_enabled(bool enabled);
    std::string get_whisper_model_path();
    bool set_whisper_model_path(const std::string& model_path);
    std::string get_whisper_service_status(); // "running", "stopped", "error"
    bool set_whisper_service_status(const std::string& status);

    // LLaMA service management
    bool get_llama_service_enabled();
    bool set_llama_service_enabled(bool enabled);
    std::string get_llama_model_path();
    bool set_llama_model_path(const std::string& model_path);
    std::string get_llama_service_status(); // "running", "stopped", "error"
    bool set_llama_service_status(const std::string& status);

    // Piper service management
    bool get_piper_service_enabled();
    bool set_piper_service_enabled(bool enabled);
    std::string get_piper_model_path();
    bool set_piper_model_path(const std::string& model_path);
    std::string get_piper_espeak_data_path();
    bool set_piper_espeak_data_path(const std::string& espeak_data_path);
    std::string get_piper_service_status(); // "running", "stopped", "error"
    bool set_piper_service_status(const std::string& status);

    // Atomic Piper configuration update (transaction-safe)
    bool set_piper_service_config_atomic(bool enabled, const std::string& model_path,
                                        const std::string& espeak_path, const std::string& status);

private:
    sqlite3* db_;
    mutable std::mutex db_mutex_;  // Thread safety for database operations
    bool create_tables();
    std::string generate_uuid();
    std::string get_current_timestamp();
};
