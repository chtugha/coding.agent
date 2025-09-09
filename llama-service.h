#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

// Forward declarations for LLaMA
struct llama_model;
struct llama_context;
struct llama_sampler;
struct llama_batch;
struct llama_vocab;

// Forward declaration for Database
class Database;

// LLaMA session configuration
struct LlamaSessionConfig {
    std::string model_path = "models/llama-7b-q4_0.gguf";
    std::string language = "en";
    int n_threads = 4;
    int n_ctx = 2048;
    int n_gpu_layers = 999;
    int max_tokens = 512;
    float temperature = 0.3f;
    float top_p = 0.8f;
    int top_k = 5;
    bool use_gpu = true;
    bool flash_attn = false;
    std::string person_name = "User";
    std::string bot_name = "Assistant";
};

// Individual LLaMA session for each call
class LlamaSession {
public:
    LlamaSession(const std::string& call_id, const LlamaSessionConfig& config);
    ~LlamaSession();
    
    bool initialize();
    std::string process_text(const std::string& input_text);
    std::string get_latest_response() const { return latest_response_; }
    
    void mark_activity() { last_activity_ = std::chrono::steady_clock::now(); }
    std::chrono::steady_clock::time_point get_last_activity() const { return last_activity_; }
    
    bool is_active() const { return is_active_.load(); }
    void set_active(bool active) { is_active_.store(active); }

private:
    std::string call_id_;
    LlamaSessionConfig config_;
    
    // LLaMA context and model
    llama_model* model_;
    llama_context* ctx_;
    llama_sampler* sampler_;
    llama_batch* batch_;
    const llama_vocab* vocab_;

    // Session state
    std::vector<int> session_tokens_;
    std::string conversation_history_;
    std::string latest_response_;
    std::atomic<bool> is_active_;
    std::chrono::steady_clock::time_point last_activity_;
    std::mutex session_mutex_;
    
    // Internal methods
    bool initialize_llama_context();
    void cleanup_llama_context();
    std::string generate_response(const std::string& prompt);
    std::string format_conversation_prompt(const std::string& user_input);
};

// Main LLaMA service
class StandaloneLlamaService {
public:
    StandaloneLlamaService(const LlamaSessionConfig& default_config);
    ~StandaloneLlamaService();

    bool start(int tcp_port = 8083);
    void stop();

    // Configuration
    bool init_database(const std::string& db_path);
    void set_output_endpoint(const std::string& host, int port);

    // Session management
    bool create_session(const std::string& call_id);
    bool destroy_session(const std::string& call_id);
    std::string process_text_for_call(const std::string& call_id, const std::string& text);

private:
    LlamaSessionConfig default_config_;

    // Database connection
    std::unique_ptr<Database> database_;

    // Session management
    std::unordered_map<std::string, std::unique_ptr<LlamaSession>> sessions_;
    std::mutex sessions_mutex_;

    // TCP server (input from Whisper)
    int server_socket_;
    std::atomic<bool> running_;
    std::thread server_thread_;
    std::unordered_map<std::string, std::thread> call_tcp_threads_;

    // TCP output (to downstream, e.g., Piper) per call
    std::string output_host_ = "127.0.0.1"; // default output host
    int output_port_ = 8090;
    std::unordered_map<std::string, int> output_sockets_;


    // Internal methods
    void run_tcp_server(int port);
    void handle_tcp_text_stream(const std::string& call_id, int socket);
    bool read_tcp_hello(int socket, std::string& call_id);
    bool read_tcp_text_chunk(int socket, std::string& text);
    bool send_tcp_response(int socket, const std::string& response);
    bool send_tcp_bye(int socket);

    // Output side helpers
    bool connect_output_for_call(const std::string& call_id);
    bool send_output_text(const std::string& call_id, const std::string& text);
    void close_output_for_call(const std::string& call_id);

    void cleanup_inactive_sessions();
    void cleanup_tcp_threads();
};

// Command line argument parsing
struct LlamaServiceArgs {
    std::string model_path = "models/llama-7b-q4_0.gguf";
    int tcp_port = 8083;
    int n_threads = 4;
    int n_ctx = 2048;
    int n_gpu_layers = 999;
    float temperature = 0.3f;
    bool use_gpu = true;
    bool verbose = false;
};

bool parse_llama_service_args(int argc, char** argv, LlamaServiceArgs& args);
void print_llama_service_usage(int argc, char** argv, const LlamaServiceArgs& args);
