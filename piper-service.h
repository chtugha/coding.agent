#pragma once

#include "database.h"
#include <string>
#include <memory>
#include <unordered_map>
#include <thread>
#include <mutex>
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
    float length_scale = 1.0f;
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

    // Session management
    std::unordered_map<std::string, std::unique_ptr<PiperSession>> sessions_;
    mutable std::mutex sessions_mutex_;

    // TCP server (input from LLaMA)
    int server_socket_;
    std::atomic<bool> running_;
    std::thread server_thread_;
    std::unordered_map<std::string, std::thread> call_tcp_threads_;

    // TCP output (to audio processor) per call
    std::string output_host_ = "127.0.0.1";
    int output_port_ = 8091;  // Base port for audio processor connections
    std::unordered_map<std::string, int> output_sockets_;

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

    // Audio output helpers
    bool connect_audio_output_for_call(const std::string& call_id);
    bool send_audio_to_processor(const std::string& call_id, const std::vector<float>& audio_samples, int sample_rate);
    void close_audio_output_for_call(const std::string& call_id);
    int calculate_audio_processor_port(const std::string& call_id);

    void cleanup_inactive_sessions();
    void cleanup_tcp_threads();
};

// Command line argument parsing
struct PiperServiceArgs {
    std::string model_path = "models/voice.onnx";
    std::string config_path = "";
    std::string espeak_data_path = "espeak-ng-data";
    int tcp_port = 8090;
    int speaker_id = 0;
    float length_scale = 1.0f;
    float noise_scale = 0.667f;
    float noise_w_scale = 0.8f;
    std::string output_host = "127.0.0.1";
    int output_port = 8091;
    bool verbose = false;
};

bool parse_piper_service_args(int argc, char** argv, PiperServiceArgs& args);
void print_piper_service_usage(int argc, char** argv, const PiperServiceArgs& args);
