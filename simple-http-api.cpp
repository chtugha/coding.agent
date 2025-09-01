#include "simple-http-api.h"
#include "database.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <algorithm>
#include <vector>
#include <cstdlib>
#include <sys/stat.h>
#include <ctime>
#include <atomic>
#include <mutex>
#include <map>
#include <chrono>
#include <thread>
#include <sys/wait.h>

// Chunked upload tracking for large files
struct ChunkedUpload {
    std::string filename;
    size_t total_size;
    size_t received_size;
    std::ofstream file_stream;
    std::mutex mutex;
    std::chrono::steady_clock::time_point last_activity;

    ChunkedUpload(const std::string& fname, size_t size)
        : filename(fname), total_size(size), received_size(0),
          last_activity(std::chrono::steady_clock::now()) {}
};

// Global upload tracking with cleanup
static std::map<std::string, std::unique_ptr<ChunkedUpload>> active_uploads;
static std::mutex uploads_mutex;
static std::atomic<bool> cleanup_running{false};

// Cleanup function for abandoned uploads
void cleanup_abandoned_uploads() {
    std::lock_guard<std::mutex> lock(uploads_mutex);
    auto now = std::chrono::steady_clock::now();

    for (auto it = active_uploads.begin(); it != active_uploads.end();) {
        auto elapsed = std::chrono::duration_cast<std::chrono::minutes>(now - it->second->last_activity);
        if (elapsed.count() > 30) { // 30 minutes timeout
            std::cout << "🎤 Cleaning up abandoned upload: " << it->second->filename << std::endl;
            if (it->second->file_stream.is_open()) {
                it->second->file_stream.close();
            }
            std::string filepath = "models/" + it->second->filename;
            std::remove(filepath.c_str());
            it = active_uploads.erase(it);
        } else {
            ++it;
        }
    }
}

SimpleHttpServer::SimpleHttpServer(int port, Database* database)
    : port_(port), server_socket_(-1), running_(false), database_(database) {}

SimpleHttpServer::~SimpleHttpServer() {
    stop();
}

bool SimpleHttpServer::start() {
    server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_ < 0) {
        std::cerr << "Failed to create socket" << std::endl;
        return false;
    }
    
    // Allow socket reuse
    int opt = 1;
    setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port_);
    
    if (bind(server_socket_, (struct sockaddr*)&address, sizeof(address)) < 0) {
        std::cerr << "Failed to bind to port " << port_ << std::endl;
        close(server_socket_);
        return false;
    }
    
    if (listen(server_socket_, 10) < 0) {
        std::cerr << "Failed to listen on socket" << std::endl;
        close(server_socket_);
        return false;
    }
    
    running_ = true;
    server_thread_ = std::thread(&SimpleHttpServer::server_loop, this);
    
    return true;
}

void SimpleHttpServer::stop() {
    running_ = false;
    if (server_socket_ >= 0) {
        close(server_socket_);
        server_socket_ = -1;
    }
    if (server_thread_.joinable()) {
        server_thread_.join();
    }
}

void SimpleHttpServer::server_loop() {
    while (running_) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_socket = accept(server_socket_, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (running_) {
                std::cerr << "Failed to accept client connection" << std::endl;
            }
            continue;
        }
        
        // Handle client in separate thread for simplicity
        std::thread client_thread(&SimpleHttpServer::handle_client, this, client_socket);
        client_thread.detach();
    }
}

void SimpleHttpServer::handle_client(int client_socket) {
    // Set socket timeout
    struct timeval timeout;
    timeout.tv_sec = 300; // 5 minutes for large uploads
    timeout.tv_usec = 0;
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    try {
        // Use larger buffer for reading request
        char buffer[65536]; // 64KB buffer
        ssize_t bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);

        if (bytes_read <= 0) {
            close(client_socket);
            return;
        }

        buffer[bytes_read] = '\0';
        std::string raw_request(buffer, bytes_read); // Use binary-safe constructor

        // For large uploads, we need to read more data if Content-Length indicates it
        std::string content_length_header = "Content-Length: ";
        size_t cl_pos = raw_request.find(content_length_header);
        if (cl_pos != std::string::npos) {
            size_t cl_start = cl_pos + content_length_header.length();
            size_t cl_end = raw_request.find("\r\n", cl_start);
            if (cl_end != std::string::npos) {
                std::string cl_str = raw_request.substr(cl_start, cl_end - cl_start);
                size_t content_length = std::stoull(cl_str);

                // Find where headers end
                size_t headers_end = raw_request.find("\r\n\r\n");
                if (headers_end != std::string::npos) {
                    size_t body_start = headers_end + 4;
                    size_t current_body_size = bytes_read - body_start;

                    // Read remaining body if needed
                    while (current_body_size < content_length) {
                        size_t to_read = std::min(content_length - current_body_size, sizeof(buffer) - 1);
                        ssize_t more_bytes = recv(client_socket, buffer, to_read, 0);

                        if (more_bytes <= 0) break;

                        raw_request.append(buffer, more_bytes);
                        current_body_size += more_bytes;
                    }
                }
            }
        }

        HttpRequest request = parse_request(raw_request);
        HttpResponse response = handle_request(request);
        std::string response_str = create_response(response);

        send(client_socket, response_str.c_str(), response_str.length(), 0);
    } catch (const std::exception& e) {
        std::cerr << "Client handling error: " << e.what() << std::endl;
        // Send error response
        std::string error_response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        send(client_socket, error_response.c_str(), error_response.length(), 0);
    }

    close(client_socket);
}

HttpRequest SimpleHttpServer::parse_request_streaming(int client_socket) {
    HttpRequest request;
    std::string headers_buffer;
    char buffer[8192]; // Increased buffer size

    // Read headers first
    while (true) {
        ssize_t bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
        if (bytes_read <= 0) {
            throw std::runtime_error("Failed to read request headers");
        }

        buffer[bytes_read] = '\0';
        headers_buffer += buffer;

        // Check if we have complete headers (double CRLF)
        size_t headers_end = headers_buffer.find("\r\n\r\n");
        if (headers_end != std::string::npos) {
            // Parse headers
            std::string headers_only = headers_buffer.substr(0, headers_end);
            std::istringstream stream(headers_only);
            std::string line;

            // Parse request line
            if (std::getline(stream, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                std::istringstream request_line(line);
                request_line >> request.method >> request.path;

                // Parse query parameters
                size_t query_pos = request.path.find('?');
                if (query_pos != std::string::npos) {
                    std::string query = request.path.substr(query_pos + 1);
                    request.path = request.path.substr(0, query_pos);

                    // Simple query parsing
                    size_t pos = 0;
                    while (pos < query.length()) {
                        size_t eq_pos = query.find('=', pos);
                        size_t amp_pos = query.find('&', pos);
                        if (amp_pos == std::string::npos) amp_pos = query.length();

                        if (eq_pos != std::string::npos && eq_pos < amp_pos) {
                            std::string key = query.substr(pos, eq_pos - pos);
                            std::string value = query.substr(eq_pos + 1, amp_pos - eq_pos - 1);
                            request.query_params[key] = value;
                        }
                        pos = amp_pos + 1;
                    }
                }
            }

            // Parse headers
            while (std::getline(stream, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) break;

                size_t colon_pos = line.find(':');
                if (colon_pos != std::string::npos) {
                    std::string key = line.substr(0, colon_pos);
                    std::string value = line.substr(colon_pos + 1);
                    // Trim whitespace
                    value.erase(0, value.find_first_not_of(" \t"));
                    value.erase(value.find_last_not_of(" \t") + 1);
                    request.headers[key] = value;
                }
            }

            // Handle body if Content-Length is specified
            auto content_length_it = request.headers.find("Content-Length");
            if (content_length_it != request.headers.end()) {
                size_t content_length = std::stoull(content_length_it->second);

                // Get any body data already read
                size_t body_start = headers_end + 4;
                if (body_start < headers_buffer.length()) {
                    request.body = headers_buffer.substr(body_start);
                }

                // Read remaining body data
                while (request.body.length() < content_length) {
                    size_t remaining = content_length - request.body.length();
                    size_t to_read = std::min(remaining, sizeof(buffer) - 1);

                    ssize_t bytes_read = recv(client_socket, buffer, to_read, 0);
                    if (bytes_read <= 0) {
                        std::cout << "❌ Failed to read body: bytes_read=" << bytes_read
                                  << ", remaining=" << remaining << ", errno=" << errno << std::endl;
                        throw std::runtime_error("Failed to read request body");
                    }

                    request.body.append(buffer, bytes_read);
                    std::cout << "📥 Read " << bytes_read << " bytes, total body: " << request.body.length()
                              << "/" << content_length << std::endl;
                }
            }

            break;
        }
    }

    return request;
}

HttpRequest SimpleHttpServer::parse_request(const std::string& raw_request) {
    HttpRequest request;
    std::istringstream stream(raw_request);
    std::string line;

    // Parse request line
    if (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream request_line(line);
        request_line >> request.method >> request.path;

        // Parse query parameters
        size_t query_pos = request.path.find('?');
        if (query_pos != std::string::npos) {
            std::string query = request.path.substr(query_pos + 1);
            request.path = request.path.substr(0, query_pos);

            // Simple query parsing
            size_t pos = 0;
            while (pos < query.length()) {
                size_t eq_pos = query.find('=', pos);
                size_t amp_pos = query.find('&', pos);
                if (amp_pos == std::string::npos) amp_pos = query.length();

                if (eq_pos != std::string::npos && eq_pos < amp_pos) {
                    std::string key = query.substr(pos, eq_pos - pos);
                    std::string value = query.substr(eq_pos + 1, amp_pos - eq_pos - 1);
                    request.query_params[key] = value;
                }
                pos = amp_pos + 1;
            }
        }
    }

    // Parse headers
    while (std::getline(stream, line) && line != "\r") {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) break;

        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 1);
            // Trim whitespace
            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t") + 1);
            request.headers[key] = value;
        }
    }

    // Parse body (binary safe)
    size_t headers_end = raw_request.find("\r\n\r\n");
    if (headers_end != std::string::npos) {
        request.body = raw_request.substr(headers_end + 4);
    }

    return request;
}

std::string SimpleHttpServer::create_response(const HttpResponse& response) {
    std::ostringstream stream;
    stream << "HTTP/1.1 " << response.status_code << " " << response.status_text << "\r\n";
    
    for (const auto& header : response.headers) {
        stream << header.first << ": " << header.second << "\r\n";
    }
    
    stream << "Content-Length: " << response.body.length() << "\r\n";
    stream << "\r\n";
    stream << response.body;
    
    return stream.str();
}

HttpResponse SimpleHttpServer::handle_request(const HttpRequest& request) {
    // Silence all HTTP request logging - only show business logic messages

    // API routes
    if (request.path.substr(0, 5) == "/api/") {
        return handle_api_request(request);
    }
    
    // Static file serving
    std::string file_path = request.path;
    if (file_path == "/") {
        file_path = "/index.html";
    }
    
    return serve_static_file(file_path);
}

HttpResponse SimpleHttpServer::serve_static_file(const std::string& path) {
    HttpResponse response;

    // Serve embedded HTML for main page
    if (path == "/index.html" || path == "/") {
        response.status_code = 200;
        response.status_text = "OK";
        response.headers["Content-Type"] = "text/html";
        response.body = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>🐱 Whisper Talk LLaMA - Status</title>
    <style>
        body { font-family: -apple-system, BlinkMacSystemFont, sans-serif; margin: 0; padding: 20px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; }
        .container { max-width: 800px; margin: 0 auto; }
        .card { background: rgba(255,255,255,0.95); border-radius: 15px; padding: 25px; margin-bottom: 20px; box-shadow: 0 8px 32px rgba(0,0,0,0.1); }
        .header { text-align: center; margin-bottom: 30px; }
        .logo { font-size: 3em; margin-bottom: 10px; }
        .status-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr)); gap: 15px; }
        .status-item { padding: 15px; background: #f8f9fa; border-radius: 10px; text-align: center; }
        .status-online { color: #28a745; }
        .status-offline { color: #dc3545; }
        .status-warning { color: #ffc107; }
        .status-error { color: #dc3545; }
        .status-disabled { color: #6c757d; }

        .model-item {
            padding: 8px 12px;
            margin: 2px 0;
            border-radius: 4px;
            cursor: pointer;
            transition: background-color 0.2s;
            border: 1px solid transparent;
        }

        .model-item:hover {
            background-color: #f8f9fa;
        }

        .model-item.current {
            background-color: #007bff;
            color: white;
            font-weight: bold;
        }

        .model-item.selected {
            background-color: #ffc107;
            color: #000;
            border: 2px solid #ff6b35;
            font-weight: bold;
        }
        .refresh-btn { background: #667eea; color: white; border: none; padding: 10px 20px; border-radius: 8px; cursor: pointer; }
        .refresh-btn:hover { background: #5a6fd8; }
        .form-group { margin-bottom: 15px; }
        .form-group label { display: block; margin-bottom: 5px; font-weight: bold; }
        .form-group input { width: 100%; padding: 8px; border: 1px solid #ddd; border-radius: 4px; box-sizing: border-box; }
        .form-row { display: flex; gap: 15px; }
        .form-row .form-group { flex: 1; }
        .sip-form { background: #f8f9fa; padding: 20px; border-radius: 10px; margin-bottom: 20px; }
        .sip-form h3 { margin-top: 0; color: #333; }
    </style>
</head>
<body>
    <div class="container">
        <div class="card">
            <div class="header">
                <div class="logo">🐱</div>
                <h1>Whisper Talk LLaMA</h1>
                <p>AI Phone System Status Dashboard</p>
            </div>
        </div>

        <div class="card">
            <h2>System Status</h2>
            <div class="status-grid" id="statusGrid">
                <div class="status-item">
                    <h3>HTTP Server</h3>
                    <div class="status-online">● Online</div>
                </div>
                <div class="status-item">
                    <h3>Database</h3>
                    <div class="status-online">● Online</div>
                </div>
                <div class="status-item">
                    <h3>SIP Client</h3>
                    <div class="status-offline">● Offline</div>
                </div>
                <div class="status-item">
                    <h3>Whisper</h3>
                    <div class="status-offline">● Offline</div>
                </div>
                <div class="status-item">
                    <h3>LLaMA</h3>
                    <div class="status-offline">● Offline</div>
                </div>
                <div class="status-item">
                    <h3>Piper TTS</h3>
                    <div class="status-offline">● Offline</div>
                </div>
            </div>
            <br>
            <button class="refresh-btn" onclick="refreshStatus()">Refresh Status</button>
        </div>

        <div class="card">
            <h2>SIP Lines</h2>

            <!-- Add New SIP Line Form -->
            <div class="sip-form">
                <h3>Add New SIP Line</h3>
                <form id="sipLineForm">
                    <div class="form-row">
                        <div class="form-group">
                            <label for="serverIp">Server IP:</label>
                            <input type="text" id="serverIp" name="serverIp" value="192.168.1.100" required>
                        </div>
                        <div class="form-group">
                            <label for="serverPort">Port:</label>
                            <input type="number" id="serverPort" name="serverPort" value="5060" required>
                        </div>
                    </div>
                    <div class="form-row">
                        <div class="form-group">
                            <label for="username">Username:</label>
                            <input type="text" id="username" name="username" placeholder="e.g. 1002" required>
                        </div>
                        <div class="form-group">
                            <label for="password">Password:</label>
                            <input type="password" id="password" name="password" placeholder="SIP password">
                        </div>
                    </div>

                    <button type="button" class="refresh-btn" onclick="addSipLine()">Add SIP Line</button>
                </form>
            </div>

            <!-- Existing SIP Lines -->
            <h3>Configured SIP Lines</h3>
            <div id="sipLinesContainer">
                <p>Loading SIP lines...</p>
            </div>
        </div>

        <div class="card">
            <h2>🎤 Whisper Service</h2>
            <div id="whisperServiceContainer">
                <div class="status-grid">
                    <div class="status-item">
                        <h3>Service Status</h3>
                        <div id="whisperStatus" class="status-offline">● Stopped</div>
                    </div>
                    <div class="status-item">
                        <h3>Available Models</h3>
                        <div id="modelList" style="max-height: 150px; overflow-y: auto; border: 1px solid #ddd; border-radius: 4px; padding: 5px;">
                            Loading models...
                        </div>
                    </div>
                </div>

                <div style="margin: 20px 0;">
                    <button id="whisperToggleBtn" class="refresh-btn" onclick="toggleWhisperService()">
                        Start Service
                    </button>
                    <button id="restartBtn" class="refresh-btn" onclick="restartWithSelectedModel()" style="margin-left: 10px; background: #ffc107; color: #000;" disabled>
                        Restart with Selected Model
                    </button>
                    <button class="refresh-btn" onclick="showUploadArea()" style="margin-left: 10px; background: #28a745;">
                        Upload a new model
                    </button>
                </div>

                <!-- Upload Area (hidden by default) -->
                <div id="uploadArea" style="display: none; margin-top: 20px; padding: 20px; border: 2px dashed #ccc; border-radius: 10px; text-align: center; background: #f9f9f9;">
                    <h4>Upload Whisper Model</h4>
                    <p>Drop both files here:</p>
                    <ul style="text-align: left; display: inline-block;">
                        <li><strong>.bin file</strong> - The main model file</li>
                        <li><strong>.mlmodelc file</strong> - CoreML acceleration</li>
                    </ul>
                    <div id="dropZone" style="margin: 20px 0; padding: 40px; border: 2px dashed #007bff; border-radius: 8px; background: #f0f8ff;">
                        <p style="margin: 0; color: #007bff; font-weight: bold;">Drag and drop files here</p>
                        <p style="margin: 5px 0 0 0; font-size: 14px; color: #666;">or click to select files</p>
                        <input type="file" id="fileInput" multiple style="display: none;">
                    </div>
                    <div id="uploadStatus" style="margin-top: 15px;"></div>
                    <div id="progressContainer" style="margin-top: 15px; display: none;">
                        <div style="background: #f0f0f0; border-radius: 10px; overflow: hidden; margin-bottom: 10px;">
                            <div id="progressBar" style="height: 20px; background: #007bff; width: 0%; transition: width 0.3s ease;"></div>
                        </div>
                        <div id="progressText" style="font-size: 14px; color: #666;">Preparing upload...</div>
                    </div>
                    <div style="margin-top: 15px;">
                        <button class="refresh-btn" onclick="clearUploadFiles()" style="background: #6c757d;">
                            Clear Files
                        </button>
                        <button class="refresh-btn" onclick="hideUploadArea()" style="background: #dc3545; margin-left: 10px;">
                            Cancel
                        </button>
                        <button id="uploadBtn" class="refresh-btn" onclick="uploadModel()" style="background: #28a745; margin-left: 10px;" disabled>
                            Upload Model
                        </button>
                    </div>
                </div>
            </div>
        </div>

        <div class="card">
            <h2>API Endpoints</h2>
            <ul>
                <li><a href="/api/status">/api/status</a> - System status</li>
                <li><a href="/api/callers">/api/callers</a> - Caller list</li>
                <li><a href="/api/sip-lines">/api/sip-lines</a> - SIP lines</li>
                <li><a href="/api/whisper/service">/api/whisper/service</a> - Whisper service info</li>
                <li><strong>POST</strong> /api/whisper/service/toggle - Start/stop service</li>
            </ul>
        </div>
    </div>

    <script>
        // Cache buster: v2.0 - Force browser to reload JavaScript
        console.log('JavaScript loaded - version 2.0');

        async function refreshStatus() {
            try {
                const response = await fetch('/api/status');
                const data = await response.json();
                console.log('Status:', data);
                // Update UI based on API response
                updateStatusDisplay(data);
            } catch (error) {
                console.error('Failed to fetch status:', error);
            }
        }

        function updateStatusDisplay(data) {
            // Simple status update - could be enhanced
            if (data.modules) {
                const items = document.querySelectorAll('.status-item');
                items.forEach(item => {
                    const title = item.querySelector('h3').textContent.toLowerCase().replace(' ', '_');
                    const statusDiv = item.querySelector('div:last-child');
                    if (data.modules[title] === 'online') {
                        statusDiv.className = 'status-online';
                        statusDiv.textContent = '● Online';
                    } else {
                        statusDiv.className = 'status-offline';
                        statusDiv.textContent = '● Offline';
                    }
                });
            }
        }

        // Load SIP lines on page load
        loadSipLines();

        // Simple function to add SIP line
        window.addSipLine = function() {
            console.log('=== ADD SIP LINE FUNCTION CALLED ===');

            const serverIp = document.getElementById('serverIp').value;
            const serverPort = document.getElementById('serverPort').value;
            const username = document.getElementById('username').value;
            const password = document.getElementById('password').value;

            console.log('Form values:', {
                serverIp, serverPort, username, password
            });

            if (!username) {
                alert('Username is required!');
                return;
            }

            const sipLineData = {
                server_ip: serverIp || '192.168.1.100',
                server_port: parseInt(serverPort) || 5060,
                username: username,
                password: password
            };

            console.log('Sending data:', sipLineData);

            fetch('/api/sip-lines', {
                method: 'POST',
                headers: {
                    'Content-Type': 'application/json'
                },
                body: JSON.stringify(sipLineData)
            })
            .then(response => {
                console.log('Response status:', response.status);
                return response.text();
            })
            .then(data => {
                console.log('Response data:', data);
                alert('SIP line added successfully!');
                // Clear form
                document.getElementById('username').value = '';
                document.getElementById('password').value = '';
                // Refresh list
                loadSipLines();
            })
            .catch(error => {
                console.error('Error:', error);
                alert('Error adding SIP line: ' + error.message);
            });
        };

        // Auto-refresh every 5 seconds
        setInterval(refreshStatus, 5000);
        setInterval(loadSipLines, 3000); // Refresh SIP lines every 3 seconds for better status updates
        setInterval(loadWhisperService, 5000); // Refresh Whisper service every 5 seconds

        async function loadSipLines() {
            try {
                const response = await fetch('/api/sip-lines');
                const data = await response.json();
                displaySipLines(data.sip_lines);
            } catch (error) {
                console.error('Failed to load SIP lines:', error);
            }
        }

        function displaySipLines(sipLines) {
            const container = document.getElementById('sipLinesContainer');
            if (!sipLines || sipLines.length === 0) {
                container.innerHTML = '<p>No SIP lines configured</p>';
                return;
            }

            let html = '<div class="status-grid">';
            sipLines.forEach(line => {
                // Status color based on actual connection status, not enabled/disabled
                let statusClass = 'status-offline'; // default
                let statusText = line.status || 'unknown';

                if (line.status === 'connected') {
                    statusClass = 'status-online';
                    statusText = 'Connected';
                } else if (line.status === 'connecting') {
                    statusClass = 'status-warning';
                    statusText = 'Connecting...';
                } else if (line.status === 'error') {
                    statusClass = 'status-error';
                    statusText = 'Connection Error';
                } else if (line.status === 'disabled') {
                    statusClass = 'status-disabled';
                    statusText = 'Disabled';
                } else {
                    statusClass = 'status-offline';
                    statusText = 'Disconnected';
                }

                const hasPassword = line.password && line.password.length > 0;
                html += `
                    <div class="status-item">
                        <h4>Line ${line.line_id}: ${line.username}</h4>
                        <p><strong>Server:</strong> ${line.server_ip}:${line.server_port}</p>
                        <p><strong>Username:</strong> ${line.username}</p>
                        <p><strong>Password:</strong> ${hasPassword ? '●●●●●●' : 'Not set'}</p>
                        <div class="${statusClass}">● ${statusText}</div>
                        <div style="margin-top: 10px;">
                            <button onclick="toggleSipLine(${line.line_id})" class="refresh-btn" style="font-size: 12px; margin-right: 5px;">
                                ${line.enabled ? 'Disable' : 'Enable'}
                            </button>
                            <button onclick="deleteSipLine(${line.line_id})" class="refresh-btn" style="font-size: 12px; background: #dc3545;">
                                Delete
                            </button>
                        </div>
                    </div>
                `;
            });
            html += '</div>';
            container.innerHTML = html;
        }

        // Old form handler removed - using simple addSipLine function instead

        async function toggleSipLine(lineId) {
            try {
                const response = await fetch(`/api/sip-lines/${lineId}/toggle`, {
                    method: 'PUT'
                });

                const result = await response.json();

                if (response.ok) {
                    console.log('SIP line toggled successfully');
                    loadSipLines(); // Refresh the list
                } else {
                    alert(`Failed to toggle SIP line: ${result.error}`);
                }
            } catch (error) {
                console.error('Error toggling SIP line:', error);
                alert('Failed to toggle SIP line');
            }
        }

        async function deleteSipLine(lineId) {
            if (confirm('Are you sure you want to delete this SIP line?')) {
                try {
                    const response = await fetch(`/api/sip-lines/${lineId}`, {
                        method: 'DELETE'
                    });

                    const result = await response.json();

                    if (response.ok) {
                        alert('SIP line deleted successfully');
                        loadSipLines(); // Refresh the list
                    } else {
                        alert(`Failed to delete SIP line: ${result.error}`);
                    }
                } catch (error) {
                    console.error('Error deleting SIP line:', error);
                    alert('Failed to delete SIP line');
                }
            }
        }

        // Model Management
        let uploadedFiles = [];
        let selectedModel = null;
        let currentModel = null;

        function showUploadArea() {
            document.getElementById('uploadArea').style.display = 'block';
            setupDragAndDrop();
        }

        function clearUploadFiles() {
            uploadedFiles = [];
            updateUploadStatus();
            hideProgressBar();
        }

        function hideUploadArea() {
            document.getElementById('uploadArea').style.display = 'none';
            uploadedFiles = [];
            updateUploadStatus();
            hideProgressBar();
        }

        function showProgressBar() {
            document.getElementById('progressContainer').style.display = 'block';
        }

        function hideProgressBar() {
            document.getElementById('progressContainer').style.display = 'none';
            document.getElementById('progressBar').style.width = '0%';
            document.getElementById('progressText').textContent = 'Preparing upload...';
        }

        function updateProgress(percentage, text) {
            document.getElementById('progressBar').style.width = percentage + '%';
            document.getElementById('progressText').textContent = text || `${Math.round(percentage)}% complete`;
        }

        function setupDragAndDrop() {
            const dropZone = document.getElementById('dropZone');
            const fileInput = document.getElementById('fileInput');

            // Click to select files
            dropZone.addEventListener('click', () => fileInput.click());

            // Handle file selection
            fileInput.addEventListener('change', handleFiles);

            // Drag and drop events
            dropZone.addEventListener('dragover', (e) => {
                e.preventDefault();
                dropZone.style.borderColor = '#007bff';
                dropZone.style.backgroundColor = '#e3f2fd';
            });

            dropZone.addEventListener('dragleave', (e) => {
                e.preventDefault();
                dropZone.style.borderColor = '#007bff';
                dropZone.style.backgroundColor = '#f0f8ff';
            });

            dropZone.addEventListener('drop', (e) => {
                e.preventDefault();
                dropZone.style.borderColor = '#007bff';
                dropZone.style.backgroundColor = '#f0f8ff';

                const files = Array.from(e.dataTransfer.files);
                processFiles(files);
            });
        }

        function handleFiles(e) {
            const files = Array.from(e.target.files);
            processFiles(files);
        }

        function processFiles(files) {
            // Don't reset uploadedFiles - accumulate files instead

            files.forEach(file => {
                if (file.name.endsWith('.bin') || file.name.endsWith('.mlmodelc') || file.name.endsWith('.mlmodelc.zip')) {
                    uploadedFiles.push(file);
                }
            });

            updateUploadStatus();
        }

        function updateUploadStatus() {
            const statusDiv = document.getElementById('uploadStatus');
            const uploadBtn = document.getElementById('uploadBtn');

            const binFile = uploadedFiles.find(f => f.name.endsWith('.bin'));
            const mlmodelcFiles = uploadedFiles.filter(f => f.name.endsWith('.mlmodelc') || f.name.endsWith('.mlmodelc.zip'));

            let status = '<div style="text-align: left;">';

            if (binFile) {
                status += '<p style="color: #28a745;">✅ .bin file: ' + binFile.name + '</p>';
            } else {
                status += '<p style="color: #dc3545;">❌ .bin file: Not found</p>';
            }

            if (mlmodelcFiles.length > 0) {
                const fileName = mlmodelcFiles[0].name;
                status += '<p style="color: #28a745;">✅ .mlmodelc file: ' + fileName + '</p>';
            } else {
                status += '<p style="color: #dc3545;">❌ .mlmodelc file: Not found</p>';
            }

            status += '</div>';
            statusDiv.innerHTML = status;

            // Enable upload button only if both files are present
            uploadBtn.disabled = !(binFile && mlmodelcFiles.length > 0);
        }

        async function uploadModel() {
            const binFile = uploadedFiles.find(f => f.name.endsWith('.bin'));
            const mlmodelcFiles = uploadedFiles.filter(f => f.name.endsWith('.mlmodelc') || f.name.endsWith('.mlmodelc.zip'));

            if (!binFile || mlmodelcFiles.length === 0) {
                alert('Both .bin file and .mlmodelc/.mlmodelc.zip file are required');
                return;
            }

            const uploadBtn = document.getElementById('uploadBtn');
            uploadBtn.disabled = true;
            uploadBtn.textContent = 'Preparing upload...';
            showProgressBar();

            try {
                const allFiles = [binFile, ...mlmodelcFiles];

                for (let i = 0; i < allFiles.length; i++) {
                    const file = allFiles[i];
                    const fileProgress = (i / allFiles.length) * 100;

                    uploadBtn.textContent = `Uploading ${file.name} (${i + 1}/${allFiles.length})...`;
                    updateProgress(fileProgress, `Uploading ${file.name} (${i + 1}/${allFiles.length})`);

                    await uploadFileChunked(file, (progress) => {
                        const totalProgress = fileProgress + (progress / allFiles.length);
                        updateProgress(totalProgress, `Uploading ${file.name}: ${Math.round(progress)}%`);
                        uploadBtn.textContent = `Uploading ${file.name}: ${Math.round(progress)}%`;
                    });
                }

                updateProgress(100, 'Upload completed successfully!');
                uploadBtn.textContent = 'Upload Complete!';

                setTimeout(() => {
                    alert('All files uploaded successfully!');
                    hideUploadArea();
                    loadWhisperService();
                }, 1000);

            } catch (error) {
                console.error('Upload error:', error);
                alert('Upload failed: ' + error.message);
                uploadBtn.disabled = false;
                uploadBtn.textContent = 'Upload Model';
                hideProgressBar();
            }
        }

        async function uploadFileChunked(file, progressCallback) {
            const CHUNK_SIZE = 1024 * 1024; // 1MB chunks
            const totalChunks = Math.ceil(file.size / CHUNK_SIZE);

            for (let chunkIndex = 0; chunkIndex < totalChunks; chunkIndex++) {
                const start = chunkIndex * CHUNK_SIZE;
                const end = Math.min(start + CHUNK_SIZE, file.size);
                const chunk = file.slice(start, end);

                const contentRange = `bytes ${start}-${end - 1}/${file.size}`;

                let retries = 0;
                const maxRetries = 3;
                let success = false;

                while (retries < maxRetries && !success) {
                    try {
                        const response = await fetch('/api/whisper/upload', {
                            method: 'POST',
                            headers: {
                                'Content-Type': 'application/octet-stream',
                                'Content-Range': contentRange,
                                'X-File-Name': file.name,
                                'X-File-Size': file.size.toString()
                            },
                            body: chunk
                        });

                        const result = await response.json();

                        if (!response.ok) {
                            throw new Error(result.error || 'Upload failed');
                        }

                        if (progressCallback && result.progress !== undefined) {
                            progressCallback(result.progress);
                        }

                        success = true;

                        // If this was the final chunk, we're done
                        if (response.status === 200) {
                            return; // Upload completed
                        }

                    } catch (error) {
                        retries++;
                        if (retries >= maxRetries) {
                            throw new Error(`Chunk ${chunkIndex + 1} failed after ${maxRetries} retries: ${error.message}`);
                        }
                        // Exponential backoff
                        await new Promise(resolve => setTimeout(resolve, Math.pow(2, retries) * 1000));
                    }
                }
            }
        }

        // Whisper Service Management
        function extractModelName(modelPath) {
            if (!modelPath) return 'Not set';

            // Extract filename from path
            const filename = modelPath.split('/').pop();

            // Remove extension (.bin)
            const nameWithoutExt = filename.replace(/\.[^/.]+$/, "");

            // Clean up common prefixes
            return nameWithoutExt
                .replace(/^ggml-/, '')  // Remove ggml- prefix
                .replace(/-q[0-9]_[0-9]$/, '')  // Remove quantization suffix like -q5_0
                .replace(/-encoder$/, '');  // Remove -encoder suffix
        }

        async function loadWhisperService() {
            try {
                const [serviceResponse, modelsResponse] = await Promise.all([
                    fetch('/api/whisper/service'),
                    fetch('/api/whisper/models')
                ]);

                const serviceData = await serviceResponse.json();
                const modelsData = await modelsResponse.json();

                updateWhisperServiceDisplay(serviceData);
                updateModelList(modelsData, serviceData.model_path);
            } catch (error) {
                console.error('Failed to load whisper service:', error);
            }
        }

        function updateWhisperServiceDisplay(data) {
            const statusDiv = document.getElementById('whisperStatus');
            const toggleBtn = document.getElementById('whisperToggleBtn');

            // Update status display
            if (data.status === 'running') {
                statusDiv.className = 'status-online';
                statusDiv.textContent = '● Running';
                toggleBtn.textContent = 'Stop Service';
            } else if (data.status === 'starting') {
                statusDiv.className = 'status-warning';
                statusDiv.textContent = '● Starting...';
                toggleBtn.textContent = 'Stop Service';
            } else if (data.status === 'error') {
                statusDiv.className = 'status-error';
                statusDiv.textContent = '● Error';
                toggleBtn.textContent = 'Start Service';
            } else {
                statusDiv.className = 'status-offline';
                statusDiv.textContent = '● Stopped';
                toggleBtn.textContent = 'Start Service';
            }

            // Store current model
            currentModel = data.model_path;
        }

        function updateModelList(modelsData, currentModelPath) {
            const modelListDiv = document.getElementById('modelList');
            const restartBtn = document.getElementById('restartBtn');

            // Store models data for re-rendering
            window.lastModelsData = modelsData.models || [];

            if (!modelsData.models || modelsData.models.length === 0) {
                modelListDiv.innerHTML = '<div style="padding: 10px; color: #666;">No models found</div>';
                return;
            }

            let html = '';
            modelsData.models.forEach(model => {
                const modelName = extractModelName(model.path);
                const isCurrent = model.path === currentModelPath;
                const isSelected = model.path === selectedModel;

                let className = 'model-item';
                if (isCurrent) className += ' current';
                if (isSelected) className += ' selected';

                html += `<div class="${className}" onclick="selectModel('${model.path}')">${modelName}</div>`;
            });

            modelListDiv.innerHTML = html;

            // Enable restart button if a different model is selected
            restartBtn.disabled = !selectedModel || selectedModel === currentModelPath;
        }

        function selectModel(modelPath) {
            selectedModel = modelPath;
            // Re-render the model list to update highlighting
            updateModelList({ models: window.lastModelsData || [] }, currentModel);
        }

        async function restartWithSelectedModel() {
            if (!selectedModel) {
                alert('Please select a model first');
                return;
            }

            try {
                const response = await fetch('/api/whisper/restart', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({ model_path: selectedModel })
                });

                const result = await response.json();

                if (response.ok) {
                    alert('Whisper service restarted with new model!');
                    selectedModel = null; // Reset selection
                    loadWhisperService(); // Refresh display
                } else {
                    alert(`Failed to restart service: ${result.error}`);
                }
            } catch (error) {
                console.error('Error restarting service:', error);
                alert('Failed to restart service');
            }
        }

        async function toggleWhisperService() {
            try {
                const response = await fetch('/api/whisper/service/toggle', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    }
                });

                const result = await response.json();

                if (response.ok) {
                    console.log('Whisper service toggled:', result);
                    loadWhisperService(); // Refresh display
                } else {
                    alert(`Failed to toggle whisper service: ${result.error}`);
                }
            } catch (error) {
                console.error('Error toggling whisper service:', error);
                alert('Failed to toggle whisper service');
            }
        }

        // updateModelPath function removed - replaced with model selection list

        // Load initial data
        loadWhisperService();
    </script>
</body>
</html>)HTML";
        return response;
    }

    // For other files, return 404
    response.status_code = 404;
    response.status_text = "Not Found";
    response.body = "File not found";
    return response;
}

HttpResponse SimpleHttpServer::handle_api_request(const HttpRequest& request) {
    // Silence all API request logging - only show business logic messages

    if (request.path == "/api/status") {
        return api_status(request);
    } else if (request.path == "/api/callers") {
        return api_callers(request);
    } else if (request.path.length() > 15 && request.path.substr(0, 15) == "/api/sip-lines/" && request.method == "DELETE") {
        // Extract line_id from path like /api/sip-lines/1
        std::string path_suffix = request.path.substr(15); // Remove "/api/sip-lines/"
        int line_id = std::atoi(path_suffix.c_str());

        // Validate line_id
        if (line_id <= 0) {
            HttpResponse error_response;
            error_response.status_code = 400;
            error_response.status_text = "Bad Request";
            error_response.body = R"({"error": "Invalid line ID"})";
            error_response.headers["Content-Type"] = "application/json";
            return error_response;
        }

        std::cout << "Extracted line_id: " << line_id << std::endl;
        return api_sip_lines_delete(request, line_id);
    } else if (request.path.find("/api/sip-lines/") == 0 && request.path.find("/toggle") != std::string::npos && request.method == "PUT") {
        // Extract line_id from path like /api/sip-lines/1/toggle
        std::cout << "Matched TOGGLE sip-lines endpoint: " << request.path << std::endl;
        size_t start = 15; // After "/api/sip-lines/"
        size_t end = request.path.find("/toggle");
        if (end != std::string::npos) {
            std::string line_id_str = request.path.substr(start, end - start);
            int line_id = std::atoi(line_id_str.c_str());

            // Validate line_id
            if (line_id <= 0) {
                HttpResponse error_response;
                error_response.status_code = 400;
                error_response.status_text = "Bad Request";
                error_response.body = R"({"error": "Invalid line ID"})";
                error_response.headers["Content-Type"] = "application/json";
                return error_response;
            }

            // Toggle SIP line
            return api_sip_lines_toggle(request, line_id);
        }
    } else if (request.path == "/api/sip-lines") {
        if (request.method == "GET") {
            return api_sip_lines(request);
        } else if (request.method == "POST") {
            return api_sip_lines_post(request);
        }
    } else if (request.path == "/api/system/speed") {
        if (request.method == "GET") {
            return api_system_speed_get(request);
        } else if (request.method == "POST") {
            return api_system_speed_post(request);
        }
    } else if (request.path == "/api/whisper/service") {
        if (request.method == "GET") {
            return api_whisper_service_get(request);
        } else if (request.method == "POST") {
            return api_whisper_service_post(request);
        }
    } else if (request.path == "/api/whisper/service/toggle") {
        if (request.method == "POST") {
            return api_whisper_service_toggle(request);
        }
    } else if (request.path == "/api/whisper/upload") {
        if (request.method == "POST") {
            return api_whisper_upload(request);
        }
    } else if (request.path == "/api/whisper/models") {
        if (request.method == "GET") {
            return api_whisper_models_get(request);
        }
    } else if (request.path == "/api/whisper/restart") {
        if (request.method == "POST") {
            return api_whisper_restart(request);
        }
    // Session and Whisper endpoints removed
    }
    // Note: /api/whisper/transcribe removed - using direct interface now

    // Silently handle unmatched requests

    HttpResponse response;
    response.status_code = 404;
    response.status_text = "Not Found";
    response.body = R"({"error": "API endpoint not found"})";
    response.headers["Content-Type"] = "application/json";
    return response;
}

HttpResponse SimpleHttpServer::api_status(const HttpRequest& request) {
    HttpResponse response;
    response.status_code = 200;
    response.status_text = "OK";
    response.headers["Content-Type"] = "application/json";

    // Check actual module status
    std::string db_status = database_ ? "online" : "offline";

    // Check if whisper service is running
    std::string whisper_status = "offline";
    if (database_) {
        std::string status = database_->get_whisper_service_status();
        whisper_status = (status == "running") ? "online" : "offline";
    }

    // Check SIP client processes
    std::string sip_status = "offline";
    int sip_result = system("pgrep -f whisper-sip-client > /dev/null 2>&1");
    if (sip_result == 0) {
        sip_status = "online";
    }

    response.body = "{\"status\": \"online\", \"modules\": {" +
                   std::string("\"http_server\": \"online\", ") +
                   "\"database\": \"" + db_status + "\", " +
                   "\"sip_client\": \"" + sip_status + "\", " +
                   "\"whisper\": \"" + whisper_status + "\", " +
                   "\"llama\": \"offline\", " +
                   "\"piper\": \"offline\"}}";

    return response;
}

HttpResponse SimpleHttpServer::api_callers(const HttpRequest& request) {
    HttpResponse response;
    response.status_code = 200;
    response.status_text = "OK";
    response.body = R"({"callers": []})";
    response.headers["Content-Type"] = "application/json";
    return response;
}



std::string SimpleHttpServer::get_mime_type(const std::string& extension) {
    if (extension == ".html") return "text/html";
    if (extension == ".css") return "text/css";
    if (extension == ".js") return "application/javascript";
    if (extension == ".json") return "application/json";
    if (extension == ".png") return "image/png";
    if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
    if (extension == ".gif") return "image/gif";
    if (extension == ".svg") return "image/svg+xml";
    return "text/plain";
}

// SIP Line Management now uses database directly

HttpResponse SimpleHttpServer::api_sip_lines(const HttpRequest& request) {
    HttpResponse response;
    response.status_code = 200;
    response.status_text = "OK";
    response.headers["Content-Type"] = "application/json";

    if (!database_) {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Database not available"})";
        return response;
    }

    // Get SIP lines from database
    auto sip_lines = database_->get_all_sip_lines();

    // Convert to JSON
    std::ostringstream json;
    json << "{\"sip_lines\":[";

    for (size_t i = 0; i < sip_lines.size(); ++i) {
        const auto& line = sip_lines[i];
        if (i > 0) json << ",";

        json << "{"
             << "\"line_id\":" << line.line_id << ","
             << "\"username\":\"" << line.username << "\","
             << "\"password\":\"" << line.password << "\","
             << "\"server_ip\":\"" << line.server_ip << "\","
             << "\"server_port\":" << line.server_port << ","
             << "\"enabled\":" << (line.enabled ? "true" : "false") << ","
             << "\"status\":\"" << line.status << "\""
             << "}";
    }

    json << "]}";
    response.body = json.str();

    return response;
}

HttpResponse SimpleHttpServer::api_sip_lines_post(const HttpRequest& request) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";

    if (!database_) {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Database not available"})";
        return response;
    }

    // Extract values from request body (JSON or form data)
    std::string server_ip = "localhost";
    int server_port = 5060;
    std::string username = "";
    std::string password = "";

    std::cout << "POST body: " << request.body << std::endl;

    // Simple JSON value extraction
    if (!request.body.empty()) {
        std::string body = request.body;

        // Helper function to extract JSON string value
        auto extract_json_string = [&](const std::string& key) -> std::string {
            std::string search = "\"" + key + "\":\"";
            size_t start = body.find(search);
            if (start != std::string::npos) {
                start += search.length();
                size_t end = body.find("\"", start);
                if (end != std::string::npos) {
                    return body.substr(start, end - start);
                }
            }
            return "";
        };

        // Helper function to extract JSON number value
        auto extract_json_number = [&](const std::string& key) -> int {
            std::string search = "\"" + key + "\":";
            size_t start = body.find(search);
            if (start != std::string::npos) {
                start += search.length();
                size_t end = body.find_first_of(",}", start);
                if (end != std::string::npos) {
                    std::string num_str = body.substr(start, end - start);
                    return std::atoi(num_str.c_str());
                }
            }
            return 0;
        };

        // Extract all values
        server_ip = extract_json_string("server_ip");
        username = extract_json_string("username");
        password = extract_json_string("password");

        int port = extract_json_number("server_port");
        if (port > 0) server_port = port;

        // Debug output
        std::cout << "Extracted values:" << std::endl;
        std::cout << "  server_ip: '" << server_ip << "'" << std::endl;
        std::cout << "  server_port: " << server_port << std::endl;
        std::cout << "  username: '" << username << "'" << std::endl;
        std::cout << "  password: '" << password << "'" << std::endl;
    }

    // Validate required parameters
    if (username.empty()) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Username is required"})";
        return response;
    }

    if (password.empty()) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Password is required"})";
        return response;
    }

    // Validate server_port range
    if (server_port < 1 || server_port > 65535) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = "{\"error\": \"Invalid server port (must be 1-65535)\"}";
        return response;
    }

    // Create new SIP line in database
    int line_id = database_->create_sip_line(username, password, server_ip, server_port);

    if (line_id > 0) {
        response.status_code = 201;
        response.status_text = "Created";
        response.body = R"({"success": true, "message": "SIP line created", "line_id": )" + std::to_string(line_id) + "}";
    } else {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Failed to create SIP line"})";
    }

    return response;
}

HttpResponse SimpleHttpServer::api_sip_lines_delete(const HttpRequest& request, int line_id) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";

    if (!database_) {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Database not available"})";
        return response;
    }

    if (line_id <= 0) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Invalid line ID"})";
        return response;
    }

    // Delete the SIP line from database
    bool success = database_->delete_sip_line(line_id);

    if (success) {
        response.status_code = 200;
        response.status_text = "OK";
        response.body = R"({"success": true, "message": "SIP line deleted"})";
    } else {
        response.status_code = 404;
        response.status_text = "Not Found";
        response.body = R"({"error": "SIP line not found"})";
    }

    return response;
}

HttpResponse SimpleHttpServer::api_sip_lines_toggle(const HttpRequest& request, int line_id) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";

    if (!database_) {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Database not available"})";
        return response;
    }

    if (line_id <= 0) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Invalid line ID"})";
        return response;
    }

    // Toggle the SIP line enabled status
    bool success = database_->toggle_sip_line(line_id);

    if (success) {
        // Get the updated line status to check if it's now enabled
        auto lines = database_->get_all_sip_lines();
        bool line_enabled = false;
        std::string line_info = "";

        for (const auto& line : lines) {
            if (line.line_id == line_id) {
                line_enabled = line.enabled;
                line_info = "Line " + std::to_string(line.line_id) + " (" + line.username + " @ " + line.server_ip + ":" + std::to_string(line.server_port) + ")";
                break;
            }
        }

        if (line_enabled) {
            // Line was enabled - start SIP client for this line
            std::cout << "🚀 Starting SIP client for enabled " << line_info << std::endl;

            // Start SIP client in background with specific line ID
            // Use relative path to call SIP client from same directory as HTTP server
            std::string command = "./whisper-sip-client --line-id " + std::to_string(line_id) + " &";
            int result = system(command.c_str());

            // system() returns -1 on error, or the exit status of the command
            if (result != -1 && WIFEXITED(result) && WEXITSTATUS(result) == 0) {
                std::cout << "✅ SIP client started successfully for " << line_info << std::endl;
                response.body = R"({"success": true, "message": "SIP line enabled and client started"})";
            } else {
                std::cout << "⚠️ SIP client start failed for " << line_info << " (result: " << result << ")" << std::endl;
                response.body = R"({"success": true, "message": "SIP line enabled but client start failed"})";
            }
        } else {
            // Line was disabled - stop SIP client for this line
            std::cout << "🛑 SIP line disabled: " << line_info << std::endl;

            // Kill any existing SIP client processes for this line
            std::cout << "🛑 Sending SIGTERM to SIP client processes..." << std::endl;
            std::string kill_command = "pkill -TERM -f 'whisper-sip-client.*--line-id " + std::to_string(line_id) + "'";
            int kill_result = system(kill_command.c_str());
            std::cout << "🛑 Kill command result: " << kill_result << std::endl;

            // Give the process time to shut down gracefully
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            std::cout << "🛑 SIP client shutdown complete" << std::endl;

            response.body = R"({"success": true, "message": "SIP line disabled and client stopped"})";
        }

        response.status_code = 200;
        response.status_text = "OK";
    } else {
        response.status_code = 404;
        response.status_text = "Not Found";
        response.body = R"({"error": "SIP line not found"})";
    }

    return response;
}

HttpResponse SimpleHttpServer::api_system_speed_get(const HttpRequest& request) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";

    if (!database_) {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Database not available"})";
        return response;
    }

    int speed = database_->get_system_speed();

    response.status_code = 200;
    response.status_text = "OK";
    response.body = "{\"system_speed\": " + std::to_string(speed) + "}";

    return response;
}

HttpResponse SimpleHttpServer::api_system_speed_post(const HttpRequest& request) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";

    if (!database_) {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Database not available"})";
        return response;
    }

    // Parse speed from request body (JSON: {"system_speed": 3})
    int speed = 3; // Default

    // Simple JSON parsing for speed value
    size_t speed_pos = request.body.find("\"system_speed\"");
    if (speed_pos != std::string::npos) {
        size_t colon_pos = request.body.find(":", speed_pos);
        if (colon_pos != std::string::npos) {
            size_t start = colon_pos + 1;
            while (start < request.body.length() && (request.body[start] == ' ' || request.body[start] == '\t')) start++;
            size_t end = start;
            while (end < request.body.length() && std::isdigit(request.body[end])) end++;
            if (end > start) {
                speed = std::stoi(request.body.substr(start, end - start));
            }
        }
    }

    // Validate speed range (1-5)
    if (speed < 1 || speed > 5) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "System speed must be between 1 and 5"})";
        return response;
    }

    bool success = database_->set_system_speed(speed);

    if (success) {
        response.status_code = 200;
        response.status_text = "OK";
        response.body = "{\"success\": true, \"system_speed\": " + std::to_string(speed) + "}";
        std::cout << "🎛️ System speed updated to: " << speed << std::endl;
    } else {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Failed to update system speed"})";
    }

    return response;
}

// api_sessions_get method removed - no session management

// All whisper API methods removed - WhisperService deleted

// api_whisper_models method removed

// All remaining whisper API methods removed - WhisperService deleted

// All remaining whisper API methods removed - WhisperService deleted

// Whisper service management endpoints
HttpResponse SimpleHttpServer::api_whisper_service_get(const HttpRequest& request) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";

    if (!database_) {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Database not available"})";
        return response;
    }

    bool enabled = database_->get_whisper_service_enabled();
    std::string model_path = database_->get_whisper_model_path();
    std::string status = database_->get_whisper_service_status();

    response.status_code = 200;
    response.status_text = "OK";
    response.body = "{\"enabled\": " + std::string(enabled ? "true" : "false") +
                   ", \"model_path\": \"" + model_path +
                   "\", \"status\": \"" + status + "\"}";

    return response;
}

HttpResponse SimpleHttpServer::api_whisper_service_post(const HttpRequest& request) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";

    if (!database_) {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Database not available"})";
        return response;
    }

    // Parse JSON body to update model path
    std::string model_path;
    size_t model_pos = request.body.find("\"model_path\":");
    if (model_pos != std::string::npos) {
        size_t start = request.body.find("\"", model_pos + 13);
        if (start != std::string::npos) {
            start++; // Skip opening quote
            size_t end = request.body.find("\"", start);
            if (end != std::string::npos) {
                model_path = request.body.substr(start, end - start);
            }
        }
    }

    if (model_path.empty()) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Model path is required"})";
        return response;
    }

    bool success = database_->set_whisper_model_path(model_path);

    if (success) {
        response.status_code = 200;
        response.status_text = "OK";
        response.body = "{\"success\": true, \"model_path\": \"" + model_path + "\"}";
        // Model path updated silently to reduce console spam
    } else {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Failed to update model path"})";
    }

    return response;
}

HttpResponse SimpleHttpServer::api_whisper_service_toggle(const HttpRequest& request) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";

    if (!database_) {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Database not available"})";
        return response;
    }

    bool current_enabled = database_->get_whisper_service_enabled();
    bool new_enabled = !current_enabled;

    bool success = database_->set_whisper_service_enabled(new_enabled);

    if (success) {
        // Update status based on enabled state
        std::string new_status = new_enabled ? "starting" : "stopped";
        database_->set_whisper_service_status(new_status);

        response.status_code = 200;
        response.status_text = "OK";
        response.body = "{\"success\": true, \"enabled\": " +
                       std::string(new_enabled ? "true" : "false") +
                       ", \"status\": \"" + new_status + "\"}";

        if (new_enabled) {
            std::cout << "🎤 Whisper service enabled" << std::endl;
        } else {
            std::cout << "🎤 Whisper service disabled" << std::endl;
        }

        // TODO: Actually start/stop the whisper service process here
        // For now, we just update the database state

    } else {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Failed to toggle whisper service"})";
    }

    return response;
}

HttpResponse SimpleHttpServer::api_whisper_upload(const HttpRequest& request) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";
    response.headers["Access-Control-Allow-Origin"] = "*";
    response.headers["Access-Control-Allow-Headers"] = "Content-Type, Content-Range, X-File-Name, X-File-Size";

    if (!database_) {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Database not available"})";
        return response;
    }

    // Create models directory if it doesn't exist
    system("mkdir -p models");

    // Handle chunked upload using Content-Range header
    std::string content_range = request.headers.count("Content-Range") ? request.headers.at("Content-Range") : "";
    std::string filename = request.headers.count("X-File-Name") ? request.headers.at("X-File-Name") : "";
    std::string file_size_str = request.headers.count("X-File-Size") ? request.headers.at("X-File-Size") : "";

    if (filename.empty()) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Missing X-File-Name header"})";
        return response;
    }

    // Validate filename for security
    if (filename.find("..") != std::string::npos ||
        filename.find("/") != std::string::npos ||
        filename.find("\\") != std::string::npos) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Invalid filename"})";
        return response;
    }

    // Validate file type
    bool is_bin = filename.length() >= 4 && filename.substr(filename.length() - 4) == ".bin";
    bool is_mlmodelc = filename.length() >= 9 && filename.substr(filename.length() - 9) == ".mlmodelc";
    bool is_mlmodelc_zip = filename.length() >= 13 && filename.substr(filename.length() - 13) == ".mlmodelc.zip";

    if (!is_bin && !is_mlmodelc && !is_mlmodelc_zip) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Invalid file type. Only .bin, .mlmodelc, and .mlmodelc.zip files are allowed"})";
        return response;
    }

    size_t total_file_size = 0;
    if (!file_size_str.empty()) {
        try {
            total_file_size = std::stoull(file_size_str);
        } catch (const std::exception& e) {
            response.status_code = 400;
            response.status_text = "Bad Request";
            response.body = R"({"error": "Invalid file size"})";
            return response;
        }
    }

    // Validate file size (min 1KB, max 50GB)
    if (total_file_size > 0) {
        if (total_file_size < 1024) {
            response.status_code = 400;
            response.status_text = "Bad Request";
            response.body = "{\"error\": \"File too small (minimum 1KB)\"}";
            return response;
        }
        if (total_file_size > 50ULL * 1024 * 1024 * 1024) {
            response.status_code = 413;
            response.status_text = "Payload Too Large";
            response.body = "{\"error\": \"File too large (maximum 50GB)\"}";
            return response;
        }
    }

    std::cout << "🎤 Chunked upload: " << filename << " (" << request.body.length() << " bytes)" << std::endl;

    return handle_chunked_upload(request, filename, total_file_size, content_range);
}

HttpResponse SimpleHttpServer::api_whisper_models_get(const HttpRequest& request) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";

    if (!database_) {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Database not available"})";
        return response;
    }

    // Scan models directory for .bin files
    std::string json_response = R"({"models": [)";
    bool first = true;

    // Scan the models directory for actual .bin files
    std::string models_dir = "models";
    DIR* dir = opendir(models_dir.c_str());

    if (dir != nullptr) {
        struct dirent* entry;
        std::vector<std::string> model_files;

        // Collect all .bin files
        while ((entry = readdir(dir)) != nullptr) {
            std::string filename = entry->d_name;
            if (filename.length() > 4 && filename.substr(filename.length() - 4) == ".bin") {
                model_files.push_back(models_dir + "/" + filename);
            }
        }
        closedir(dir);

        // Sort the files for consistent ordering
        std::sort(model_files.begin(), model_files.end());

        // Add to JSON response
        for (const auto& model_path : model_files) {
            if (!first) json_response += ",";
            json_response += R"({"path": ")" + model_path + R"("})";
            first = false;
        }
    } else {
        // Directory doesn't exist or can't be opened
        std::cout << "⚠️ Models directory not accessible: " << models_dir << std::endl;
        // Create directory if it doesn't exist
        system("mkdir -p models");
    }

    json_response += "]}";

    response.status_code = 200;
    response.status_text = "OK";
    response.body = json_response;

    return response;
}

HttpResponse SimpleHttpServer::api_whisper_restart(const HttpRequest& request) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";

    if (!database_) {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Database not available"})";
        return response;
    }

    // Parse JSON body to get model path
    std::string model_path;
    size_t model_pos = request.body.find("\"model_path\":");
    if (model_pos != std::string::npos) {
        size_t start = request.body.find("\"", model_pos + 13);
        if (start != std::string::npos) {
            start++; // Skip opening quote
            size_t end = request.body.find("\"", start);
            if (end != std::string::npos) {
                model_path = request.body.substr(start, end - start);
            }
        }
    }

    if (model_path.empty()) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Model path is required"})";
        return response;
    }

    // Update model path and restart service
    bool model_updated = database_->set_whisper_model_path(model_path);
    bool service_restarted = database_->set_whisper_service_status("starting");

    if (model_updated && service_restarted) {
        response.status_code = 200;
        response.status_text = "OK";
        response.body = "{\"success\": true, \"model_path\": \"" + model_path + "\", \"status\": \"starting\"}";

        // Extract model name for logging
        std::string model_name = model_path;
        size_t slash_pos = model_name.find_last_of('/');
        if (slash_pos != std::string::npos) {
            model_name = model_name.substr(slash_pos + 1);
        }
        size_t dot_pos = model_name.find_last_of('.');
        if (dot_pos != std::string::npos) {
            model_name = model_name.substr(0, dot_pos);
        }
        if (model_name.substr(0, 5) == "ggml-") {
            model_name = model_name.substr(5);
        }

        std::cout << "🎤 Whisper service restarting with model: " << model_name << std::endl;

        // Validate model file exists
        struct stat file_stat;
        if (stat(model_path.c_str(), &file_stat) != 0) {
            response.status_code = 404;
            response.status_text = "Not Found";
            response.body = R"({"error": "Model file not found"})";
            return response;
        }

        // Kill existing whisper service processes
        std::cout << "🎤 Stopping existing whisper service..." << std::endl;
        system("pkill -TERM -f whisper-service");
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        // Start new whisper service with selected model
        std::string start_command = "./whisper-service --model \"" + model_path + "\" &";
        int start_result = system(start_command.c_str());

        if (start_result == 0) {
            std::cout << "✅ Whisper service started with model: " << model_name << std::endl;
            database_->set_whisper_service_status("running");
        } else {
            std::cout << "❌ Failed to start whisper service" << std::endl;
            database_->set_whisper_service_status("error");
            response.status_code = 500;
            response.status_text = "Internal Server Error";
            response.body = R"({"error": "Failed to start whisper service"})";
            return response;
        }

    } else {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Failed to restart service with new model"})";
    }

    return response;
}

HttpResponse SimpleHttpServer::handle_chunked_upload(const HttpRequest& request, const std::string& filename, size_t total_size, const std::string& content_range) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";
    response.headers["Access-Control-Allow-Origin"] = "*";

    // Disable cleanup thread for now to avoid race conditions
    // TODO: Re-enable cleanup with proper synchronization

    // Validate max concurrent uploads (limit to 10)
    {
        std::lock_guard<std::mutex> lock(uploads_mutex);
        if (active_uploads.size() >= 10) {
            response.status_code = 429;
            response.status_text = "Too Many Requests";
            response.body = R"({"error": "Too many concurrent uploads"})";
            return response;
        }
    }

    // Parse Content-Range header: "bytes start-end/total" or "bytes start-end/*"
    size_t range_start = 0, range_end = 0;
    bool is_final_chunk = false;

    if (!content_range.empty()) {
        // Parse "bytes 0-1023/2048" format
        size_t bytes_pos = content_range.find("bytes ");
        if (bytes_pos == std::string::npos) {
            response.status_code = 400;
            response.status_text = "Bad Request";
            response.body = R"({"error": "Invalid Content-Range format"})";
            return response;
        }

        size_t dash_pos = content_range.find("-", bytes_pos + 6);
        size_t slash_pos = content_range.find("/", dash_pos);

        if (dash_pos == std::string::npos || slash_pos == std::string::npos ||
            dash_pos <= bytes_pos + 6 || slash_pos <= dash_pos + 1) {
            response.status_code = 400;
            response.status_text = "Bad Request";
            response.body = R"({"error": "Malformed Content-Range header"})";
            return response;
        }

        try {
            std::string start_str = content_range.substr(bytes_pos + 6, dash_pos - bytes_pos - 6);
            std::string end_str = content_range.substr(dash_pos + 1, slash_pos - dash_pos - 1);
            std::string total_str = content_range.substr(slash_pos + 1);

            // Trim whitespace
            start_str.erase(0, start_str.find_first_not_of(" \t"));
            end_str.erase(0, end_str.find_first_not_of(" \t"));
            total_str.erase(0, total_str.find_first_not_of(" \t"));

            range_start = std::stoull(start_str);
            range_end = std::stoull(end_str);

            if (total_str != "*") {
                total_size = std::stoull(total_str);
            }

            // Validate range
            if (range_start > range_end) {
                response.status_code = 400;
                response.status_text = "Bad Request";
                response.body = R"({"error": "Invalid range: start > end"})";
                return response;
            }

        } catch (const std::exception& e) {
            response.status_code = 400;
            response.status_text = "Bad Request";
            response.body = R"({"error": "Invalid Content-Range values"})";
            return response;
        }
    }

    // Validate chunk size matches request body
    size_t expected_chunk_size = range_end - range_start + 1;
    if (expected_chunk_size != request.body.length()) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Chunk size mismatch"})";
        return response;
    }

    // Validate chunk size limits (max 10MB per chunk)
    if (request.body.length() > 10 * 1024 * 1024) {
        response.status_code = 413;
        response.status_text = "Payload Too Large";
        response.body = "{\"error\": \"Chunk too large (max 10MB)\"}";
        return response;
    }

    // Use filename as key - each file upload is tracked separately
    std::string upload_key = filename;

    std::lock_guard<std::mutex> global_lock(uploads_mutex);
    auto it = active_uploads.find(upload_key);

    // First chunk - create new upload
    if (it == active_uploads.end()) {
        if (range_start != 0) {
            response.status_code = 400;
            response.status_text = "Bad Request";
            response.body = R"({"error": "First chunk must start at byte 0"})";
            return response;
        }

        auto upload = std::make_unique<ChunkedUpload>(filename, total_size);
        std::string filepath = "models/" + filename;

        // Check if file already exists and warn
        struct stat file_stat;
        if (stat(filepath.c_str(), &file_stat) == 0) {
            std::cout << "🎤 Warning: Overwriting existing file: " << filename << std::endl;
        }

        upload->file_stream.open(filepath, std::ios::binary | std::ios::trunc);
        if (!upload->file_stream.is_open()) {
            response.status_code = 500;
            response.status_text = "Internal Server Error";
            response.body = R"({"error": "Failed to create file"})";
            return response;
        }

        active_uploads[upload_key] = std::move(upload);
        it = active_uploads.find(upload_key);
        std::cout << "🎤 Started chunked upload: " << filename << " (total: " << total_size << " bytes)" << std::endl;
    }

    // Get upload reference (already have global lock)
    auto& upload = active_uploads[upload_key];

    // Scope the upload lock to avoid use-after-free when erasing
    bool upload_completed = false;
    size_t final_received_size = 0;
    {
        std::lock_guard<std::mutex> upload_lock(upload->mutex);

    // Validate chunk sequence
    if (range_start != upload->received_size) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Chunk out of sequence"})";
        return response;
    }

        // Write chunk to file
        upload->file_stream.write(request.body.c_str(), request.body.length());
        if (!upload->file_stream.good()) {
            upload->file_stream.close();

            // Mark for cleanup - will be done outside lock scope
            upload_completed = true; // Use this flag to indicate failure cleanup needed
            final_received_size = 0; // Use 0 to indicate failure
        } else {
            // Update progress
            upload->received_size += request.body.length();
            upload->last_activity = std::chrono::steady_clock::now();

            // Check if upload is complete
            upload_completed = (upload->received_size >= upload->total_size) ||
                              (range_end + 1 >= total_size && total_size > 0);

            if (upload_completed) {
                upload->file_stream.close();
                final_received_size = upload->received_size;
            } else {
                // Store values for partial response before leaving lock scope
                final_received_size = upload->received_size;
            }
        }
    } // upload_lock destructor runs here - SAFE because upload object still exists

    // Handle file write failure outside lock scope
    if (upload_completed && final_received_size == 0) {
        // Clean up failed upload (already have global lock)
        active_uploads.erase(upload_key);

        std::string filepath = "models/" + filename;
        std::remove(filepath.c_str());

        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Failed to write chunk"})";
        return response;
    }



    // Handle completion outside the upload lock scope
    if (upload_completed) {
        std::cout << "🎤 Completed chunked upload: " << filename << " (" << final_received_size << " bytes)" << std::endl;

        // Validate final file size
        if (total_size > 0 && final_received_size != total_size) {
            std::string filepath = "models/" + filename;
            std::remove(filepath.c_str());

            // Clean up incomplete upload (already have global lock)
            active_uploads.erase(upload_key);

            response.status_code = 400;
            response.status_text = "Bad Request";
            response.body = R"({"error": "Incomplete upload"})";
            return response;
        }

        // Clean up completed upload (already have global lock) - NOW SAFE
        active_uploads.erase(upload_key);

        response.status_code = 200;
        response.status_text = "OK";
        response.body = R"({"success": true, "message": "Upload completed", "filename": ")" + filename + R"("})";
        return response;
    } else {
        // Partial content response (using stored values, not accessing upload object)
        response.status_code = 206;
        response.status_text = "Partial Content";
        response.headers["Range"] = "bytes=0-" + std::to_string(final_received_size - 1);

        double progress = total_size > 0 ? (double)final_received_size / total_size * 100.0 : 0.0;
        response.body = R"({"success": true, "progress": )" + std::to_string(progress) +
                       R"(, "received": )" + std::to_string(final_received_size) + "}";
    }

    return response;
}
