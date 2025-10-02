#pragma once

#include "database.h"
#include <string>
#include <memory>
#include <unordered_map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <queue>
#include <vector>
#include <cstdint>

// Forward declarations for Piper C API
struct piper_synthesizer;
struct piper_audio_chunk;
struct piper_synthesize_options;

// Piper session configuration
struct PiperSessionConfig {
    std::string model_path = "models/voice.onnx";
    std::string config_path = "";  // Auto-generated from model_path + .json
    std::string espeak_data_path = "espeak-ng-data";
    int speaker_id = 0;
    float length_scale = 0.90f; // slightly faster speech for low latency
    float noise_scale = 0.667f;
    float noise_w_scale = 0.8f;
    bool verbose = false;
};

// Individual Piper session for a call
class PiperSession {
public:
    PiperSession(const std::string& call_id, const PiperSessionConfig& config);
    ~PiperSession();

    // Text-to-speech processing
    bool synthesize_text(const std::string& text);
    bool get_next_audio_chunk(std::vector<float>& audio_samples, int& sample_rate, bool& is_last);

    // Session management
    bool is_active() const { return active_; }
    void set_active(bool active) { active_ = active; }
    std::string get_call_id() const { return call_id_; }

    // Statistics
    size_t get_total_text_processed() const { return total_text_processed_; }
    size_t get_total_audio_generated() const { return total_audio_generated_; }

private:
    std::string call_id_;
    PiperSessionConfig config_;
    piper_synthesizer* synthesizer_;
    std::atomic<bool> active_;
    std::atomic<bool> synthesis_in_progress_;

    // Statistics
    size_t total_text_processed_;
    size_t total_audio_generated_;

    // Thread safety
    std::mutex synthesis_mutex_;

    // Internal methods
    bool initialize_synthesizer();
    void cleanup_synthesizer();
};

// Standalone Piper Service
class StandalonePiperService {
public:
    StandalonePiperService(const PiperSessionConfig& default_config);
    ~StandalonePiperService();

    // Service lifecycle
    bool start(int tcp_port = 8090);
    void stop();
    bool is_running() const { return running_.load(); }

    // Database integration
    bool init_database(const std::string& db_path);

    // Configuration
    void set_output_endpoint(const std::string& host, int port);
    void set_max_concurrency(size_t n);
    PiperSessionConfig get_default_config() const { return default_config_; }
    void set_default_config(const PiperSessionConfig& config) { default_config_ = config; }

    // Session management
    bool create_session(const std::string& call_id);
    bool destroy_session(const std::string& call_id);
    PiperSession* get_session(const std::string& call_id);

    // Text processing
    std::string process_text_for_call(const std::string& call_id, const std::string& text);

    // Statistics
    struct ServiceStats {
        size_t active_sessions;
        size_t total_sessions_created;
        size_t total_text_processed;
        size_t total_audio_generated;
        bool is_running;
    };
    ServiceStats get_stats() const;

private:
    PiperSessionConfig default_config_;

    // Database connection
    std::unique_ptr<Database> database_;

    // Eager warm preload to ensure synthesizer is ready on startup
    piper_synthesizer* warm_synth_ = nullptr;
    bool warm_loaded_ = false;

    // Global concurrency gate for synthesis (throughput control)
    size_t max_concurrent_synthesis_ = std::max<size_t>(1, std::min<size_t>(4, std::thread::hardware_concurrency()));
    size_t current_synthesis_ = 0;
    mutable std::mutex synth_gate_mutex_;
    std::condition_variable synth_gate_cv_;

    // Session management
    std::unordered_map<std::string, std::unique_ptr<PiperSession>> sessions_;
    mutable std::mutex sessions_mutex_;

    // TCP server (input from LLaMA)
    int server_socket_;
    std::atomic<bool> running_;
    std::thread server_thread_;
    std::unordered_map<std::string, std::thread> call_tcp_threads_;
    std::mutex call_threads_mutex_;

    // TCP output (to audio processor) per call
    std::string output_host_ = "127.0.0.1";
    int output_port_ = 8091;  // Base port for audio processor connections
    std::unordered_map<std::string, int> output_sockets_;
    // Per-call monotonic chunk counters (to deduplicate on reconnect)
    std::unordered_map<std::string, uint32_t> chunk_counters_;
    std::mutex output_sockets_mutex_;

    // Statistics
    std::atomic<size_t> total_sessions_created_;
    std::atomic<size_t> total_text_processed_;
    std::atomic<size_t> total_audio_generated_;

    // Internal methods
    void run_tcp_server(int port);
    void handle_tcp_text_stream(const std::string& call_id, int socket);
    bool read_tcp_hello(int socket, std::string& call_id);
    bool read_tcp_text_chunk(int socket, std::string& text);
    bool send_tcp_response(int socket, const std::string& response);
    bool send_tcp_bye(int socket);

    // Audio output helpers (resilient)
    bool connect_audio_output_for_call(const std::string& call_id);
    bool try_connect_audio_output_for_call(const std::string& call_id);  // Non-blocking version
    bool send_audio_to_processor(const std::string& call_id, const std::vector<float>& audio_samples, int sample_rate);
    void close_audio_output_for_call(const std::string& call_id);
    int calculate_audio_processor_port(const std::string& call_id);

    void cleanup_inactive_sessions();
    void cleanup_tcp_threads();
};

// Note: Argument parsing is handled in piper-service-main.cpp
