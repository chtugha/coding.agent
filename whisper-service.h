#pragma once

#include "service-advertisement.h"
#include "database.h"
#include <string>
#include <vector>
#include <unordered_map>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>

// Forward declarations for whisper.cpp
struct whisper_context;
struct whisper_full_params;

// Standalone Whisper Service - completely independent from SIP client
// Discovers audio streams via service advertisement and connects to process them

struct WhisperSessionConfig {
    std::string model_path;
    int n_threads;
    bool use_gpu;
    std::string language;
    float temperature;
    bool no_timestamps;
    bool translate;
};

class WhisperSession {
public:
    WhisperSession(const std::string& call_id, const WhisperSessionConfig& config);
    ~WhisperSession();

    // Audio processing
    bool process_audio_chunk(const std::vector<float>& audio_samples);
    std::string get_latest_transcription();

    // Session management
    bool is_active() const { return is_active_; }
    void mark_activity() { last_activity_ = std::chrono::steady_clock::now(); }
    std::chrono::steady_clock::time_point get_last_activity() const { return last_activity_; }

    const std::string& get_call_id() const { return call_id_; }

private:
    std::string call_id_;
    whisper_context* ctx_;
    std::string latest_transcription_;
    std::atomic<bool> is_active_;
    std::chrono::steady_clock::time_point last_activity_;
    std::mutex session_mutex_;

    WhisperSessionConfig config_;

    bool initialize_whisper_context();
    void cleanup_whisper_context();
};

class StandaloneWhisperService {
public:
    StandaloneWhisperService();
    ~StandaloneWhisperService();

    // Service lifecycle
    bool start(const WhisperSessionConfig& config, const std::string& db_path = "whisper_talk.db");
    void stop();
    bool is_running() const { return running_.load(); }

    // LLaMA endpoint config
    void set_llama_endpoint(const std::string& host, int port) { llama_host_ = host; llama_port_ = port; }

    // Stream discovery and connection
    void discover_and_connect_streams();
    bool connect_to_audio_stream(const AudioStreamInfo& stream_info);

    // Session management
    bool create_session(const std::string& call_id);
    bool destroy_session(const std::string& call_id);
    void cleanup_inactive_sessions();

private:
    std::atomic<bool> running_;
    WhisperSessionConfig config_;

    // Database connection
    std::unique_ptr<Database> database_;


    // Eager model preload to avoid lazy loading on first TCP connection
    whisper_context* warm_ctx_ = nullptr;
    bool warm_loaded_ = false;

    // Session management
    std::unordered_map<std::string, std::unique_ptr<WhisperSession>> sessions_;
    std::mutex sessions_mutex_;

    // Service discovery
    std::unique_ptr<ServiceDiscovery> service_discovery_;
    std::thread discovery_thread_;
    std::chrono::steady_clock::time_point last_discovery_;

    // TCP connection management (audio input)
    std::unordered_map<std::string, int> call_tcp_sockets_;
    std::unordered_map<std::string, std::thread> call_tcp_threads_;

    // TCP connection management (to LLaMA per call)
    std::string llama_host_ = "127.0.0.1";
    int llama_port_ = 8083;
    std::unordered_map<std::string, int> llama_sockets_;

    std::mutex tcp_mutex_;

    // Main service loop
    void run_service_loop();
    void handle_tcp_audio_stream(const std::string& call_id, int socket);

    // TCP protocol methods
    bool read_tcp_hello(int socket, std::string& call_id);
    bool read_tcp_audio_chunk(int socket, std::vector<float>& audio_samples);
    bool send_tcp_transcription(int socket, const std::string& transcription);
    void send_tcp_bye(int socket);

    // LLaMA client helpers
    bool connect_llama_for_call(const std::string& call_id);
    bool send_llama_text(const std::string& call_id, const std::string& text);

    // Utility methods
    void log_session_stats() const;
};

// Configuration and main entry point
struct WhisperServiceArgs {
    std::string model_path = "models/ggml-base.en.bin";
    std::string database_path = "whisper_talk.db";
    std::string discovery_host = "127.0.0.1";
    int discovery_port = 13000;
    int n_threads = 4;
    bool use_gpu = true;
    std::string language = "en";
    float temperature = 0.0f;
    bool no_timestamps = false;
    bool translate = false;
    int discovery_interval_ms = 5000;
    bool verbose = false;
    // LLaMA endpoint
    std::string llama_host = "127.0.0.1";
    int llama_port = 8083;

};

bool parse_whisper_service_args(int argc, char** argv, WhisperServiceArgs& args);
void print_whisper_service_usage(const char* program_name);
