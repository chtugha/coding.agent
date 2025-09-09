#include "piper-service.h"
#include "database.h"
#include <iostream>
#include <sstream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <random>
#include <cmath>
#include <fstream>

// Include Piper C API
extern "C" {
#include "piper.h"
}

// PiperSession Implementation
PiperSession::PiperSession(const std::string& call_id, const PiperSessionConfig& config)
    : call_id_(call_id), config_(config), synthesizer_(nullptr), active_(false), 
      synthesis_in_progress_(false), total_text_processed_(0), total_audio_generated_(0) {
    
    if (!initialize_synthesizer()) {
        std::cout << "❌ Failed to initialize Piper synthesizer for call " << call_id << std::endl;
    } else {
        std::cout << "✅ Piper session created for call " << call_id << std::endl;
    }
}

PiperSession::~PiperSession() {
    cleanup_synthesizer();
    std::cout << "🗑️ Piper session destroyed for call " << call_id_ << std::endl;
}

bool PiperSession::initialize_synthesizer() {
    std::string config_path = config_.config_path;
    if (config_path.empty()) {
        config_path = config_.model_path + ".json";
    }

    synthesizer_ = piper_create(
        config_.model_path.c_str(),
        config_path.c_str(),
        config_.espeak_data_path.c_str()
    );

    if (!synthesizer_) {
        std::cout << "❌ Failed to create Piper synthesizer" << std::endl;
        return false;
    }

    active_ = true;
    std::cout << "🎤 Piper synthesizer initialized for call " << call_id_ << std::endl;
    return true;
}

void PiperSession::cleanup_synthesizer() {
    if (synthesizer_) {
        piper_free(synthesizer_);
        synthesizer_ = nullptr;
    }
    active_ = false;
}

bool PiperSession::synthesize_text(const std::string& text) {
    if (!synthesizer_ || !active_ || text.empty()) {
        return false;
    }

    std::lock_guard<std::mutex> lock(synthesis_mutex_);
    
    // Set up synthesis options
    piper_synthesize_options options = piper_default_synthesize_options(synthesizer_);
    options.speaker_id = config_.speaker_id;
    options.length_scale = config_.length_scale;
    options.noise_scale = config_.noise_scale;
    options.noise_w_scale = config_.noise_w_scale;

    // Start synthesis
    int result = piper_synthesize_start(synthesizer_, text.c_str(), &options);
    if (result != PIPER_OK) {
        std::cout << "❌ Failed to start Piper synthesis for call " << call_id_ << std::endl;
        return false;
    }

    synthesis_in_progress_ = true;
    total_text_processed_ += text.length();
    
    if (config_.verbose) {
        std::cout << "🎤 Started synthesis for call " << call_id_ << ": \"" << text << "\"" << std::endl;
    }
    
    return true;
}

bool PiperSession::get_next_audio_chunk(std::vector<float>& audio_samples, int& sample_rate, bool& is_last) {
    if (!synthesizer_ || !synthesis_in_progress_) {
        return false;
    }

    std::lock_guard<std::mutex> lock(synthesis_mutex_);
    
    piper_audio_chunk chunk;
    int result = piper_synthesize_next(synthesizer_, &chunk);
    
    if (result == PIPER_DONE) {
        synthesis_in_progress_ = false;
        is_last = true;
        return chunk.num_samples > 0;
    } else if (result != PIPER_OK) {
        synthesis_in_progress_ = false;
        return false;
    }

    // Copy audio data
    audio_samples.resize(chunk.num_samples);
    std::copy(chunk.samples, chunk.samples + chunk.num_samples, audio_samples.begin());
    sample_rate = chunk.sample_rate;
    is_last = chunk.is_last;
    
    total_audio_generated_ += chunk.num_samples;
    
    if (chunk.is_last) {
        synthesis_in_progress_ = false;
    }
    
    return true;
}

// StandalonePiperService Implementation
StandalonePiperService::StandalonePiperService(const PiperSessionConfig& default_config)
    : default_config_(default_config), server_socket_(-1), running_(false),
      total_sessions_created_(0), total_text_processed_(0), total_audio_generated_(0) {
    std::cout << "🎤 Piper service initialized" << std::endl;
}

StandalonePiperService::~StandalonePiperService() {
    stop();
    std::cout << "🎤 Piper service destroyed" << std::endl;
}

bool StandalonePiperService::start(int tcp_port) {
    if (running_.load()) {
        std::cout << "⚠️ Piper service already running" << std::endl;
        return false;
    }

    std::cout << "🚀 Starting Piper service on TCP port " << tcp_port << std::endl;
    std::cout << "📁 Model: " << default_config_.model_path << std::endl;
    std::cout << "📁 eSpeak data: " << default_config_.espeak_data_path << std::endl;

    running_.store(true);
    server_thread_ = std::thread(&StandalonePiperService::run_tcp_server, this, tcp_port);

    return true;
}

void StandalonePiperService::stop() {
    if (!running_.load()) return;

    std::cout << "🛑 Stopping Piper service..." << std::endl;
    running_.store(false);

    // Close server socket
    if (server_socket_ >= 0) {
        close(server_socket_);
        server_socket_ = -1;
    }

    // Wait for server thread
    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    // Clean up sessions and threads
    cleanup_tcp_threads();
    cleanup_inactive_sessions();

    std::cout << "✅ Piper service stopped" << std::endl;
}

bool StandalonePiperService::init_database(const std::string& db_path) {
    database_ = std::make_unique<Database>();
    if (!database_->init(db_path)) {
        std::cout << "❌ Failed to initialize database at " << db_path << std::endl;
        database_.reset();
        return false;
    }
    std::cout << "💾 Piper service connected to DB: " << db_path << std::endl;
    return true;
}

void StandalonePiperService::set_output_endpoint(const std::string& host, int port) {
    output_host_ = host;
    output_port_ = port;
    std::cout << "🔌 Piper output endpoint set to " << host << ":" << port << std::endl;
}

bool StandalonePiperService::create_session(const std::string& call_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    if (sessions_.find(call_id) != sessions_.end()) {
        std::cout << "⚠️ Piper session already exists for call " << call_id << std::endl;
        return true;
    }

    auto session = std::make_unique<PiperSession>(call_id, default_config_);
    if (!session->is_active()) {
        return false;
    }

    sessions_[call_id] = std::move(session);
    total_sessions_created_++;
    
    std::cout << "✅ Created Piper session for call " << call_id << std::endl;
    return true;
}

bool StandalonePiperService::destroy_session(const std::string& call_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto it = sessions_.find(call_id);
    if (it != sessions_.end()) {
        sessions_.erase(it);
        std::cout << "🗑️ Destroyed Piper session for call " << call_id << std::endl;
        return true;
    }
    
    return false;
}

PiperSession* StandalonePiperService::get_session(const std::string& call_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto it = sessions_.find(call_id);
    return (it != sessions_.end()) ? it->second.get() : nullptr;
}

std::string StandalonePiperService::process_text_for_call(const std::string& call_id, const std::string& text) {
    PiperSession* session = get_session(call_id);
    if (!session) {
        std::cout << "❌ No Piper session found for call " << call_id << std::endl;
        return "";
    }

    if (!session->synthesize_text(text)) {
        std::cout << "❌ Failed to synthesize text for call " << call_id << std::endl;
        return "";
    }

    // Process all audio chunks and send to audio processor
    std::vector<float> audio_samples;
    int sample_rate;
    bool is_last = false;
    size_t total_samples = 0;

    while (session->get_next_audio_chunk(audio_samples, sample_rate, is_last)) {
        if (!audio_samples.empty()) {
            if (connect_audio_output_for_call(call_id)) {
                send_audio_to_processor(call_id, audio_samples, sample_rate);
                total_samples += audio_samples.size();
            }
        }
        
        if (is_last) break;
    }

    total_text_processed_ += text.length();
    total_audio_generated_ += total_samples;

    std::ostringstream response;
    response << "Synthesized " << total_samples << " audio samples at " << sample_rate << "Hz for: " << text;
    
    if (default_config_.verbose) {
        std::cout << "🎤 " << response.str() << std::endl;
    }
    
    return response.str();
}

StandalonePiperService::ServiceStats StandalonePiperService::get_stats() const {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    ServiceStats stats;
    stats.active_sessions = sessions_.size();
    stats.total_sessions_created = total_sessions_created_.load();
    stats.total_text_processed = total_text_processed_.load();
    stats.total_audio_generated = total_audio_generated_.load();
    stats.is_running = running_.load();
    return stats;
}

void StandalonePiperService::run_tcp_server(int port) {
    server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_ < 0) {
        std::cout << "❌ Failed to create TCP server socket" << std::endl;
        return;
    }

    int opt = 1;
    setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_socket_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cout << "❌ Failed to bind TCP server socket to port " << port << std::endl;
        close(server_socket_);
        server_socket_ = -1;
        return;
    }

    if (listen(server_socket_, 16) < 0) {
        std::cout << "❌ Failed to listen on TCP server socket" << std::endl;
        close(server_socket_);
        server_socket_ = -1;
        return;
    }

    std::cout << "🎤 Piper service listening on TCP port " << port << std::endl;

    while (running_.load()) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_socket = accept(server_socket_, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (running_.load()) {
                std::cout << "⚠️ Failed to accept TCP connection" << std::endl;
            }
            continue;
        }

        std::string call_id;
        if (!read_tcp_hello(client_socket, call_id)) {
            std::cout << "❌ Failed to read TCP HELLO" << std::endl;
            close(client_socket);
            continue;
        }

        create_session(call_id);

        call_tcp_threads_[call_id] = std::thread(&StandalonePiperService::handle_tcp_text_stream, this, call_id, client_socket);
    }
}

void StandalonePiperService::handle_tcp_text_stream(const std::string& call_id, int socket) {
    std::cout << "🎤 Starting TCP text handler for call " << call_id << std::endl;

    while (running_.load()) {
        std::string text;
        if (!read_tcp_text_chunk(socket, text)) {
            break;
        }

        if (text == "BYE") {
            break;
        }

        if (text.empty()) {
            continue;
        }

        std::string response = process_text_for_call(call_id, text);

        if (!response.empty()) {
            // 1) Write to DB if available
            if (database_) {
                // Store Piper response in database (could extend database schema)
                // For now, we'll just log it
                std::cout << "💾 Piper response for call " << call_id << ": " << response << std::endl;
            }

            // 2) Send response back to LLaMA service (optional)
            if (!send_tcp_response(socket, response)) {
                std::cout << "⚠️ Failed to send response back on inbound socket for call " << call_id << std::endl;
            }
        }
    }

    send_tcp_bye(socket);
    close(socket);
    destroy_session(call_id);
    close_audio_output_for_call(call_id);
    std::cout << "📤 Ended Piper text handler for call " << call_id << std::endl;
}

bool StandalonePiperService::read_tcp_hello(int socket, std::string& call_id) {
    uint32_t length = 0;
    if (recv(socket, &length, 4, 0) != 4) return false;
    length = ntohl(length);
    if (length == 0 || length > 4096) return false;
    std::string buf(length, '\0');
    if (recv(socket, buf.data(), length, 0) != (ssize_t)length) return false;
    call_id = buf;
    std::cout << "👋 HELLO from LLaMA for call_id=" << call_id << std::endl;
    return true;
}

bool StandalonePiperService::read_tcp_text_chunk(int socket, std::string& text) {
    uint32_t length = 0;
    if (recv(socket, &length, 4, 0) != 4) return false;
    length = ntohl(length);
    if (length == 0xFFFFFFFF) { text = "BYE"; return true; }
    if (length == 0 || length > 10*1024*1024) return false;
    text.resize(length);
    if (recv(socket, text.data(), length, 0) != (ssize_t)length) return false;
    return true;
}

bool StandalonePiperService::send_tcp_response(int socket, const std::string& response) {
    uint32_t l = htonl((uint32_t)response.size());
    if (send(socket, &l, 4, 0) != 4) return false;
    if (!response.empty() && send(socket, response.data(), response.size(), 0) != (ssize_t)response.size()) return false;
    return true;
}

bool StandalonePiperService::send_tcp_bye(int socket) {
    uint32_t bye = 0xFFFFFFFF;
    return send(socket, &bye, 4, 0) == 4;
}

bool StandalonePiperService::connect_audio_output_for_call(const std::string& call_id) {
    if (output_sockets_.count(call_id)) return true;

    int port = calculate_audio_processor_port(call_id);
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = inet_addr(output_host_.c_str());

    if (connect(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(s);
        std::cout << "⚠️ Failed to connect to audio processor on port " << port << " for call " << call_id << std::endl;
        return false;
    }

    // Send HELLO(call_id)
    uint32_t n = htonl((uint32_t)call_id.size());
    if (send(s, &n, 4, 0) != 4) { close(s); return false; }
    if (send(s, call_id.data(), call_id.size(), 0) != (ssize_t)call_id.size()) { close(s); return false; }

    output_sockets_[call_id] = s;
    std::cout << "🔗 Connected audio output for call " << call_id << " to " << output_host_ << ":" << port << std::endl;
    return true;
}

bool StandalonePiperService::send_audio_to_processor(const std::string& call_id, const std::vector<float>& audio_samples, int sample_rate) {
    auto it = output_sockets_.find(call_id);
    if (it == output_sockets_.end()) return false;

    int s = it->second;

    // Convert float samples to bytes for transmission
    size_t byte_count = audio_samples.size() * sizeof(float);
    uint32_t l = htonl((uint32_t)byte_count);

    if (send(s, &l, 4, 0) != 4) return false;
    if (!audio_samples.empty() && send(s, audio_samples.data(), byte_count, 0) != (ssize_t)byte_count) return false;

    if (default_config_.verbose) {
        std::cout << "🔊 Sent " << audio_samples.size() << " audio samples (" << sample_rate << "Hz) to audio processor for call " << call_id << std::endl;
    }

    return true;
}

void StandalonePiperService::close_audio_output_for_call(const std::string& call_id) {
    auto it = output_sockets_.find(call_id);
    if (it != output_sockets_.end()) {
        send_tcp_bye(it->second);
        close(it->second);
        output_sockets_.erase(it);
        std::cout << "🔌 Closed audio output for call " << call_id << std::endl;
    }
}

int StandalonePiperService::calculate_audio_processor_port(const std::string& call_id) {
    // Calculate port based on call_id hash, similar to audio processor logic
    // This should match the audio processor's incoming port calculation
    std::hash<std::string> hasher;
    size_t hash = hasher(call_id);
    return output_port_ + (hash % 1000);  // Spread across 1000 ports
}

void StandalonePiperService::cleanup_inactive_sessions() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    auto it = sessions_.begin();
    while (it != sessions_.end()) {
        if (!it->second->is_active()) {
            std::cout << "🧹 Cleaning up inactive session for call " << it->first << std::endl;
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

void StandalonePiperService::cleanup_tcp_threads() {
    for (auto& [call_id, thread] : call_tcp_threads_) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    call_tcp_threads_.clear();

    // Close all output sockets
    for (auto& [call_id, socket] : output_sockets_) {
        send_tcp_bye(socket);
        close(socket);
    }
    output_sockets_.clear();
}
