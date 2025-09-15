#include "service-advertisement.h"
#include <iostream>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>

// ServiceAdvertiser Implementation
ServiceAdvertiser::ServiceAdvertiser() 
    : running_(false), advertisement_port_(13000), server_socket_(-1) {
}

ServiceAdvertiser::~ServiceAdvertiser() {
    stop();
}

bool ServiceAdvertiser::start(int advertisement_port) {
    if (running_.load()) return true;
    
    advertisement_port_ = advertisement_port;
    
    // Create server socket
    server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_ < 0) {
        std::cout << "❌ Failed to create advertisement server socket" << std::endl;
        return false;
    }
    
    // Set socket options
    int opt = 1;
    setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    // Bind to advertisement port
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(advertisement_port_);
    
    if (bind(server_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cout << "❌ Failed to bind advertisement server to port " << advertisement_port_ << std::endl;
        close(server_socket_);
        server_socket_ = -1;
        return false;
    }
    
    if (listen(server_socket_, 5) < 0) {
        std::cout << "❌ Failed to listen on advertisement server" << std::endl;
        close(server_socket_);
        server_socket_ = -1;
        return false;
    }
    
    running_.store(true);
    
    // Start server thread
    server_thread_ = std::thread(&ServiceAdvertiser::run_advertisement_server, this);
    
    std::cout << "📢 Service advertisement server started on port " << advertisement_port_ << std::endl;
    return true;
}

void ServiceAdvertiser::stop() {
    if (!running_.load()) return;
    
    running_.store(false);
    
    if (server_socket_ >= 0) {
        close(server_socket_);
        server_socket_ = -1;
    }
    
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
    
    std::cout << "📢 Service advertisement server stopped" << std::endl;
}

bool ServiceAdvertiser::advertise_stream(const std::string& call_id, int tcp_port,
                                        const std::string& stream_type) {
    std::lock_guard<std::mutex> lock(streams_mutex_);

    // Sanitize call_id to avoid ':' tokenization issues in legacy clients
    std::string sanitized_id = call_id;
    for (char &ch : sanitized_id) { if (ch == ':') ch = '_'; }

    AudioStreamInfo stream_info;
    stream_info.call_id = sanitized_id;
    stream_info.tcp_port = tcp_port;
    stream_info.stream_type = stream_type;
    stream_info.sample_rate = 8000;
    stream_info.channels = 1;
    stream_info.created_time = std::chrono::steady_clock::now();
    stream_info.last_activity = std::chrono::steady_clock::now();
    stream_info.is_active = true;

    active_streams_[sanitized_id] = stream_info;

    std::cout << "📢 Advertising audio stream: call_id=" << sanitized_id
              << ", port=" << tcp_port << ", type=" << stream_type << std::endl;

    return true;
}

bool ServiceAdvertiser::remove_stream_advertisement(const std::string& call_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    auto it = active_streams_.find(call_id);
    if (it != active_streams_.end()) {
        active_streams_.erase(it);
        std::cout << "📢 Removed stream advertisement for call_id: " << call_id << std::endl;
        return true;
    }
    
    return false;
}

void ServiceAdvertiser::update_stream_activity(const std::string& call_id) {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    auto it = active_streams_.find(call_id);
    if (it != active_streams_.end()) {
        it->second.last_activity = std::chrono::steady_clock::now();
    }
}

std::vector<AudioStreamInfo> ServiceAdvertiser::get_active_streams() const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    std::vector<AudioStreamInfo> streams;
    for (const auto& pair : active_streams_) {
        if (pair.second.is_active) {
            streams.push_back(pair.second);
        }
    }
    
    return streams;
}

void ServiceAdvertiser::run_advertisement_server() {
    std::cout << "📢 Advertisement server listening on port " << advertisement_port_ << std::endl;
    
    while (running_.load()) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_socket = accept(server_socket_, (struct sockaddr*)&client_addr, &client_len);
        
        if (client_socket < 0) {
            if (running_.load()) {
                std::cout << "❌ Failed to accept advertisement client" << std::endl;
            }
            continue;
        }
        
        // Handle discovery request in separate thread
        std::thread([this, client_socket]() {
            handle_discovery_request(client_socket);
            close(client_socket);
        }).detach();
        
        // Cleanup inactive streams periodically
        cleanup_inactive_streams();
    }
}

void ServiceAdvertiser::handle_discovery_request(int client_socket) {
    char buffer[1024];
    ssize_t received = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    
    if (received > 0) {
        buffer[received] = '\0';
        std::string request(buffer);
        
        std::cout << "📢 Discovery request: " << request << std::endl;
        
        // Send advertisement response
        std::string response = create_advertisement_response();
        send(client_socket, response.c_str(), response.length(), 0);
    }
}

std::string ServiceAdvertiser::create_advertisement_response() const {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    std::stringstream response;
    response << "AUDIO_STREAMS\n";
    
    for (const auto& pair : active_streams_) {
        const auto& stream = pair.second;
        if (stream.is_active) {
            // Full advertisement line expected by parsers:
            // STREAM:call_id:port:type:sample_rate:channels
            response << "STREAM:"
                     << stream.call_id << ":"
                     << stream.tcp_port << ":"
                     << stream.stream_type << ":"
                     << stream.sample_rate << ":"
                     << stream.channels << "\n";
        }
    }

    response << "END\n";
    return response.str();
}

void ServiceAdvertiser::cleanup_inactive_streams() {
    std::lock_guard<std::mutex> lock(streams_mutex_);
    
    auto now = std::chrono::steady_clock::now();
    auto it = active_streams_.begin();
    
    while (it != active_streams_.end()) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.last_activity);
        
        if (age.count() > 300) { // 5 minutes timeout
            std::cout << "📢 Removing inactive stream: " << it->first << std::endl;
            it = active_streams_.erase(it);
        } else {
            ++it;
        }
    }
}

// ServiceDiscovery Implementation
ServiceDiscovery::ServiceDiscovery() {
}

ServiceDiscovery::~ServiceDiscovery() {
}

std::vector<AudioStreamInfo> ServiceDiscovery::discover_streams(const std::string& server_host, 
                                                               int advertisement_port) {
    std::string response = query_advertisement_server(server_host, advertisement_port);
    return parse_advertisement_response(response);
}

bool ServiceDiscovery::find_stream(const std::string& call_id, AudioStreamInfo& stream_info,
                                  const std::string& server_host, int advertisement_port) {
    auto streams = discover_streams(server_host, advertisement_port);
    
    for (const auto& stream : streams) {
        if (stream.call_id == call_id) {
            stream_info = stream;
            return true;
        }
    }
    
    return false;
}

std::string ServiceDiscovery::query_advertisement_server(const std::string& host, int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return "";
    }
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr(host.c_str());
    server_addr.sin_port = htons(port);
    
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        close(sock);
        return "";
    }
    
    // Send discovery request
    std::string request = "DISCOVER_STREAMS\n";
    send(sock, request.c_str(), request.length(), 0);
    
    // Receive response
    char buffer[4096];
    ssize_t received = recv(sock, buffer, sizeof(buffer) - 1, 0);
    
    close(sock);
    
    if (received > 0) {
        buffer[received] = '\0';
        return std::string(buffer);
    }
    
    return "";
}

std::vector<AudioStreamInfo> ServiceDiscovery::parse_advertisement_response(const std::string& response) {
    std::vector<AudioStreamInfo> streams;
    std::istringstream iss(response);
    std::string line;

    while (std::getline(iss, line)) {
        if (line.rfind("STREAM:", 0) == 0) {
            // Robust parse allowing ':' inside call_id by parsing from the end
            // Format: STREAM:call_id:port:type:sample_rate:channels
            std::istringstream stream_iss(line.substr(7));
            std::string token;
            std::vector<std::string> tokens;

            while (std::getline(stream_iss, token, ':')) {
                tokens.push_back(token);
            }

            if (tokens.size() >= 5) {
                try {
                    const int n = static_cast<int>(tokens.size());
                    AudioStreamInfo stream;
                    // Join call_id tokens back together with ':' in case call_id contains ':' (e.g., host:port)
                    std::ostringstream call_id_oss;
                    for (int i = 0; i < n - 4; ++i) {
                        if (i > 0) call_id_oss << ":";
                        call_id_oss << tokens[i];
                    }
                    stream.call_id = call_id_oss.str();
                    stream.tcp_port = std::stoi(tokens[n - 4]);
                    stream.stream_type = tokens[n - 3];
                    stream.sample_rate = std::stoi(tokens[n - 2]);
                    stream.channels = std::stoi(tokens[n - 1]);
                    stream.is_active = true;
                    stream.created_time = std::chrono::steady_clock::now();
                    stream.last_activity = std::chrono::steady_clock::now();
                    streams.push_back(stream);
                } catch (const std::exception& e) {
                    std::cout << "⚠️  Failed to parse stream line: '" << line << "' (" << e.what() << ")" << std::endl;
                }
            } else {
                std::cout << "⚠️  Malformed stream line (too few tokens): '" << line << "'" << std::endl;
            }
        }
    }

    return streams;
}
