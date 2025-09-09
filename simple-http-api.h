#pragma once

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>

// Forward declarations
class Database;
struct SipLineConfig;

// Simple HTTP server for web interface only
// No dependencies on SIP client or AI models

struct HttpRequest {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::map<std::string, std::string> query_params;
    std::string body;
};

struct HttpResponse {
    int status_code;
    std::string status_text;
    std::map<std::string, std::string> headers;
    std::string body;
};

class SimpleHttpServer {
public:
    SimpleHttpServer(int port, Database* database = nullptr);
    ~SimpleHttpServer();

    bool start();
    void stop();
    bool is_running() const { return running_; }
    
private:
    int port_;
    int server_socket_;
    std::atomic<bool> running_;
    std::thread server_thread_;
    
    void server_loop();
    void handle_client(int client_socket);
    HttpRequest parse_request(const std::string& raw_request);
    HttpRequest parse_request_streaming(int client_socket);
    HttpResponse handle_streaming_upload(int client_socket, const std::string& headers_data);
    std::string create_response(const HttpResponse& response);
    
    // Route handlers
    HttpResponse handle_request(const HttpRequest& request);
    HttpResponse serve_static_file(const std::string& path);
    HttpResponse handle_api_request(const HttpRequest& request);
    
    // API endpoints (cleaned up - removed session and whisper endpoints)
    HttpResponse api_status(const HttpRequest& request);
    HttpResponse api_callers(const HttpRequest& request);
    HttpResponse api_sip_lines(const HttpRequest& request);
    HttpResponse api_sip_lines_post(const HttpRequest& request);
    HttpResponse api_sip_lines_delete(const HttpRequest& request, int line_id);
    HttpResponse api_sip_lines_toggle(const HttpRequest& request, int line_id);

    // System configuration endpoints
    HttpResponse api_system_speed_get(const HttpRequest& request);
    HttpResponse api_system_speed_post(const HttpRequest& request);

    // Whisper service endpoints
    HttpResponse api_whisper_service_get(const HttpRequest& request);
    HttpResponse api_whisper_service_post(const HttpRequest& request);
    HttpResponse api_whisper_service_toggle(const HttpRequest& request);
    HttpResponse api_upload_stream(const HttpRequest& request);
    HttpResponse api_whisper_check_files(const HttpRequest& request);
    HttpResponse api_whisper_models_get(const HttpRequest& request);
    HttpResponse api_whisper_restart(const HttpRequest& request);

    // LLaMA service endpoints
    HttpResponse api_llama_service_get(const HttpRequest& request);
    HttpResponse api_llama_service_post(const HttpRequest& request);
    HttpResponse api_llama_service_toggle(const HttpRequest& request);
    HttpResponse api_llama_models_get(const HttpRequest& request);
    HttpResponse api_llama_restart(const HttpRequest& request);

    // Piper service endpoints
    HttpResponse api_piper_service_get(const HttpRequest& request);
    HttpResponse api_piper_service_post(const HttpRequest& request);
    HttpResponse api_piper_service_toggle(const HttpRequest& request);
    HttpResponse api_piper_models_get(const HttpRequest& request);
    HttpResponse api_piper_restart(const HttpRequest& request);

    std::string get_mime_type(const std::string& extension);

    // Database connection
    Database* database_;
};
