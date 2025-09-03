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

// ADDED: Server-side logging for debugging crashes
static std::mutex log_mutex;
static void write_server_log(const std::string& message) {
    std::lock_guard<std::mutex> lock(log_mutex);
    std::ofstream logfile("server_debug.log", std::ios::app);
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    logfile << "[" << std::put_time(std::localtime(&time_t), "%Y-%m-%d %H:%M:%S") << "] " << message << std::endl;
    logfile.close();
}

// Simplified chunked upload tracking - removed complex global state
struct ChunkedUpload {
    std::string filename;
    size_t total_size;
    size_t received_size;
    std::ofstream file_stream;
    std::chrono::steady_clock::time_point last_activity;

    ChunkedUpload(const std::string& fname, size_t size)
        : filename(fname), total_size(size), received_size(0),
          last_activity(std::chrono::steady_clock::now()) {}
};

// Simplified upload tracking - removed global mutex issues
static std::map<std::string, std::unique_ptr<ChunkedUpload>> active_uploads;
static std::mutex uploads_mutex;

// Removed complex cleanup function - simplified approach

SimpleHttpServer::SimpleHttpServer(int port, Database* database)
    : port_(port), server_socket_(-1), running_(false), database_(database) {}

SimpleHttpServer::~SimpleHttpServer() {
    stop();
}

bool SimpleHttpServer::start() {
    write_server_log("SERVER: Starting HTTP server...");
    server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_ < 0) {
        write_server_log("SERVER: Failed to create socket");
        std::cerr << "Failed to create socket" << std::endl;
        return false;
    }
    write_server_log("SERVER: Socket created successfully");
    
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
    // write_server_log("SERVER: Handling new client connection"); // SILENCED: Too verbose
    // Set socket timeout
    struct timeval timeout;
    timeout.tv_sec = 300; // 5 minutes for large uploads
    timeout.tv_usec = 0;
    setsockopt(client_socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(client_socket, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

    try {
        // write_server_log("SERVER: About to receive request data"); // SILENCED: Too verbose
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

        // write_server_log("SERVER: About to parse request"); // SILENCED: Too verbose
        HttpRequest request = parse_request(raw_request);
        // write_server_log("SERVER: Request parsed, method: " + request.method + " path: " + request.path); // SILENCED

        // write_server_log("SERVER: About to handle request"); // SILENCED
        HttpResponse response = handle_request(request);
        // write_server_log("SERVER: Request handled, status: " + std::to_string(response.status_code)); // SILENCED

        // write_server_log("SERVER: About to create response string"); // SILENCED
        std::string response_str = create_response(response);
        // write_server_log("SERVER: Response string created, length: " + std::to_string(response_str.length())); // SILENCED

        // write_server_log("SERVER: About to send response"); // SILENCED
        send(client_socket, response_str.c_str(), response_str.length(), 0);
        // write_server_log("SERVER: Response sent successfully"); // SILENCED
    } catch (const std::exception& e) {
        std::cerr << "Client handling error: " << e.what() << std::endl;
        // Send error response
        std::string error_response = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
        send(client_socket, error_response.c_str(), error_response.length(), 0);
    }

    close(client_socket);
}

// Removed complex streaming parser - using simpler approach only

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
                    <p>Upload both files:</p>
                    <ul style="text-align: left; display: inline-block;">
                        <li><strong>.bin file</strong> - The main model file</li>
                        <li><strong>.mlmodelc file/folder</strong> - CoreML acceleration (can be a directory)</li>
                    </ul>
                    <p style="font-size: 12px; color: #666; margin: 10px 0;">
                        <strong>Note:</strong> Click the drop zone to choose between selecting files or folders.
                    </p>
                    <div id="dropZone" style="margin: 20px 0; padding: 40px; border: 2px dashed #007bff; border-radius: 8px; background: #f0f8ff;">
                        <p style="margin: 0; color: #007bff; font-weight: bold;">Drag and drop files here</p>
                        <p style="margin: 5px 0 0 0; font-size: 14px; color: #666;">or click to select files</p>
                        <input type="file" id="fileInput" multiple style="display: none;">
                        <input type="file" id="directoryInput" webkitdirectory style="display: none;">
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

    <script src="https://cdnjs.cloudflare.com/ajax/libs/jszip/3.10.1/jszip.min.js"></script>
    <script>
        // Cache buster: v3.0 - Force browser to reload JavaScript with JSZip support
        console.log('JavaScript loaded - version 3.0 with JSZip support');

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

        // ADDED: JavaScript logging to server
        function logToServer(message) {
            try {
                fetch('/api/log', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json' },
                    body: JSON.stringify({ message: message })
                }).catch(err => {
                    // Fallback to console if server logging fails
                    console.log('SERVER-LOG-FAILED:', message);
                });
                // Also log to browser console
                console.log(message);
            } catch (error) {
                console.log('LOG-ERROR:', message);
            }
        }

        // TEST: Log when page loads to verify logging system
        console.log('🔥 CONSOLE-TEST: JavaScript is working');
        alert('🔥 ALERT-TEST: JavaScript is working');
        logToServer('🔥 PAGE-LOADED: JavaScript logging system initialized');

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
            const directoryInput = document.getElementById('directoryInput');

            // Click to select files - show options
            dropZone.addEventListener('click', () => {
                const choice = confirm('Select FILES (.bin) or FOLDERS (.mlmodelc)?\n\nOK = Files\nCancel = Folders');
                if (choice) {
                    fileInput.click();
                } else {
                    directoryInput.click();
                }
            });

            // Handle file selection
            fileInput.addEventListener('change', handleFiles);
            directoryInput.addEventListener('change', handleFiles);

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

            dropZone.addEventListener('drop', async (e) => {
                e.preventDefault();
                dropZone.style.borderColor = '#007bff';
                dropZone.style.backgroundColor = '#f0f8ff';

                logToServer('🎯 DROP-EVENT: Drop event detected');
                console.log('🎯 Drop event detected');

                // CLEAN ARCHITECTURE: Handle files and directories as single units
                if (e.dataTransfer.items) {
                    const droppedItems = [];

                    for (let i = 0; i < e.dataTransfer.items.length; i++) {
                        const item = e.dataTransfer.items[i];

                        if (item.kind === 'file') {
                            const entry = item.webkitGetAsEntry();

                            if (entry) {
                                if (entry.isFile) {
                                    // Handle individual files (.bin files)
                                    const file = item.getAsFile();
                                    console.log('📄 Dropped file:', file.name);
                                    droppedItems.push(file);
                                } else if (entry.isDirectory) {
                                    // Handle directories as single bundles (.mlmodelc)
                                    console.log('📁 MLMODELC-LOG: Dropped directory bundle:', entry.name);
                                    console.log('📁 MLMODELC-LOG: Directory entry object:', entry);
                                    droppedItems.push({
                                        name: entry.name,
                                        type: 'directory-bundle',
                                        directoryEntry: entry
                                    });
                                    console.log('📁 MLMODELC-LOG: Added directory bundle to droppedItems');
                                }
                            }
                        }
                    }

                    console.log('🔍 Total items from drop:', droppedItems.length);
                    processDroppedItems(droppedItems);
                } else {
                    // Fallback for older browsers - only handles files
                    const files = Array.from(e.dataTransfer.files);
                    processDroppedItems(files);
                }
            });
        }

        // FIXED: TRUE STREAMING - Upload chunks as they're created
        async function createAndUploadZipFromDirectory(directoryEntry, uploadCallback, maxChunkSize = 100 * 1024 * 1024) {
            logToServer('📦 MLMODELC-LOG: === STARTING ZIP CREATION ===');
            logToServer('📦 MLMODELC-LOG: Directory name: ' + directoryEntry.name);
            logToServer('📦 MLMODELC-LOG: Max chunk size: ' + maxChunkSize);
            console.log('📦 MLMODELC-LOG: === STARTING ZIP CREATION ===');
            console.log('📦 MLMODELC-LOG: Directory name:', directoryEntry.name);
            console.log('📦 MLMODELC-LOG: Directory entry object:', directoryEntry);
            console.log('📦 MLMODELC-LOG: Max chunk size:', maxChunkSize);

            console.log('📦 MLMODELC-LOG: Creating new JSZip instance...');
            const zip = new JSZip();
            console.log('📦 MLMODELC-LOG: JSZip instance created successfully');

            // Read directory contents and add to ZIP
            console.log('📦 MLMODELC-LOG: About to call addDirectoryToZip...');
            await addDirectoryToZip(zip, directoryEntry, '');
            console.log('📦 MLMODELC-LOG: addDirectoryToZip completed successfully');

            console.log('📦 MLMODELC-LOG: ZIP created, streaming to chunks...');

            // TRUE STREAMING: Upload chunks immediately as they're created
            let chunkCount = 0;
            let totalSize = 0;

            try {
                console.log('📦 MLMODELC-LOG: Starting streaming ZIP generation...');

                // Use streaming generation with chunked output
                console.log('📦 MLMODELC-JS-LOG: About to call zip.generateInternalStream...');
                console.log('📦 MLMODELC-JS-LOG: zip object:', zip);
                console.log('📦 MLMODELC-JS-LOG: Checking if zip.generateInternalStream exists:', typeof zip.generateInternalStream);

                if (typeof zip.generateInternalStream !== 'function') {
                    console.error('❌ MLMODELC-JS-LOG: zip.generateInternalStream is NOT a function!');
                    console.error('❌ MLMODELC-JS-LOG: Available zip methods:', Object.getOwnPropertyNames(zip));
                    throw new Error('zip.generateInternalStream is not a function');
                }

                console.log('📦 MLMODELC-JS-LOG: zip.generateInternalStream exists, calling it...');
                const zipStream = zip.generateInternalStream({
                    type: 'uint8array',
                    compression: 'DEFLATE',
                    compressionOptions: { level: 1 }, // Minimal compression
                    streamFiles: true
                });
                console.log('📦 MLMODELC-JS-LOG: generateInternalStream returned successfully');
                console.log('📦 MLMODELC-JS-LOG: zipStream object:', zipStream);
                console.log('📦 MLMODELC-JS-LOG: zipStream type:', typeof zipStream);

                console.log('📦 MLMODELC-JS-LOG: About to initialize efficient buffer');
                let currentChunkParts = [];
                let currentChunkSize = 0;

                console.log('📦 MLMODELC-JS-LOG: About to set up zipStream.on("data") handler');
                zipStream.on('data', (data, metadata) => {
                    console.log('📦 MLMODELC-JS-LOG: zipStream data event fired, length:', data.length);

                    // EFFICIENT: Just store the data parts, don't copy everything
                    currentChunkParts.push(data);
                    currentChunkSize += data.length;
                    totalSize += data.length;

                    // If chunk is large enough, upload it immediately and start new chunk
                    if (currentChunkSize >= maxChunkSize) {
                        console.log('📦 MLMODELC-JS-LOG: Chunk ready, size:', currentChunkSize);
                        chunkCount++;
                        const chunkBlob = new Blob(currentChunkParts);
                        console.log('📦 Created chunk:', chunkCount, '(' + (maxChunkSize / 1024 / 1024).toFixed(1) + ' MB)');

                        // Upload chunk immediately
                        uploadCallback(chunkBlob, chunkCount).catch(error => {
                            console.error('❌ Chunk upload failed:', error);
                        });

                        currentChunkParts = [];
                        currentChunkSize = 0;
                    }
                });
                console.log('📦 MLMODELC-JS-LOG: zipStream.on("data") handler set up');

                // Wait for streaming to complete
                await new Promise((resolve, reject) => {
                    zipStream.on('end', () => {
                        // Upload final chunk if any data remains
                        if (currentChunkParts.length > 0) {
                            chunkCount++;
                            const finalChunkBlob = new Blob(currentChunkParts);
                            console.log('📦 Created final chunk:', chunkCount, '(' + (currentChunkSize / 1024 / 1024).toFixed(1) + ' MB)');

                            // Upload final chunk immediately
                            uploadCallback(finalChunkBlob, chunkCount).catch(error => {
                                console.error('❌ Final chunk upload failed:', error);
                            });
                        }
                        resolve();
                    });
                    zipStream.on('error', reject);
                    zipStream.resume();
                });

                console.log('✅ Streaming ZIP generation and upload successful:', (totalSize / 1024 / 1024).toFixed(1), 'MB in', chunkCount, 'chunks');

            } catch (error) {
                console.error('❌ Streaming ZIP generation failed:', error);
                throw new Error(`ZIP generation failed: ${error.message}. Try uploading smaller files.`);
            }

            return {
                name: directoryEntry.name + '.zip',
                chunkCount: chunkCount,
                totalSize: totalSize
            };
        }

        async function addDirectoryToZip(zip, directoryEntry, path) {
            console.log('📁 MLMODELC-LOG: === STARTING addDirectoryToZip ===');
            console.log('📁 MLMODELC-LOG: Directory entry:', directoryEntry.name);
            console.log('📁 MLMODELC-LOG: Path:', path);

            return new Promise((resolve, reject) => {
                console.log('📁 MLMODELC-LOG: Creating directory reader...');
                const reader = directoryEntry.createReader();
                console.log('📁 MLMODELC-LOG: Directory reader created successfully');

                function readEntries() {
                    console.log('📁 MLMODELC-LOG: Calling reader.readEntries...');
                    reader.readEntries(async (entries) => {
                        console.log('📁 MLMODELC-LOG: readEntries callback - entries count:', entries.length);
                        if (entries.length === 0) {
                            console.log('📁 MLMODELC-LOG: No more entries, resolving');
                            resolve();
                            return;
                        }

                        console.log('📁 MLMODELC-LOG: Processing', entries.length, 'entries...');
                        for (let i = 0; i < entries.length; i++) {
                            const entry = entries[i];
                            const entryPath = path ? path + '/' + entry.name : entry.name;
                            console.log('📁 MLMODELC-LOG: Processing entry', i + 1, '/', entries.length, ':', entry.name);
                            console.log('📁 MLMODELC-LOG: Entry path:', entryPath);
                            console.log('📁 MLMODELC-LOG: Entry isFile:', entry.isFile, 'isDirectory:', entry.isDirectory);

                            if (entry.isFile) {
                                console.log('📄 MLMODELC-LOG: Processing FILE:', entryPath);
                                try {
                                    console.log('📄 MLMODELC-LOG: Getting file object...');
                                    const file = await new Promise((res, rej) => {
                                        entry.file(res, rej);
                                    });
                                    console.log('📄 MLMODELC-LOG: File object obtained, size:', file.size);

                                    // FIXED: Always use File object directly - JSZip streaming handles memory efficiently
                                    console.log('📄 MLMODELC-LOG: Adding file to ZIP:', entryPath, '(' + (file.size / 1024 / 1024).toFixed(1) + ' MB)');
                                    zip.file(entryPath, file);
                                    console.log('✅ MLMODELC-LOG: Successfully added to ZIP:', entryPath);
                                } catch (error) {
                                    console.error('❌ MLMODELC-LOG: Error adding file to ZIP:', entryPath, error);
                                }
                            } else if (entry.isDirectory) {
                                console.log('📁 MLMODELC-LOG: Processing SUBDIRECTORY:', entryPath);
                                console.log('📁 MLMODELC-LOG: Recursively calling addDirectoryToZip...');
                                await addDirectoryToZip(zip, entry, entryPath);
                                console.log('📁 MLMODELC-LOG: Recursive call completed for:', entryPath);
                            }
                        }
                        console.log('📁 MLMODELC-LOG: Finished processing all entries');

                        console.log('📁 MLMODELC-LOG: Calling readEntries again to continue...');
                        readEntries(); // Continue reading
                    }, (error) => {
                        console.error('❌ MLMODELC-LOG: readEntries error:', error);
                        reject(error);
                    });
                }

                console.log('📁 MLMODELC-LOG: Starting initial readEntries call...');
                readEntries();
            });
        }

        function handleFiles(e) {
            const files = Array.from(e.target.files);
            processDroppedItems(files);
        }

        function processDroppedItems(items) {
            console.log('🔍 Processing', items.length, 'dropped items');

            items.forEach(item => {
                if (item.type === 'directory-bundle') {
                    // Handle .mlmodelc directory bundle
                    console.log('📁 MLMODELC-LOG: Processing directory bundle:', item.name);
                    console.log('📁 MLMODELC-LOG: Item object:', item);
                    console.log('📁 MLMODELC-LOG: DirectoryEntry:', item.directoryEntry);

                    // Remove existing .mlmodelc files
                    console.log('📁 MLMODELC-LOG: Before filter - uploadedFiles count:', uploadedFiles.length);
                    uploadedFiles = uploadedFiles.filter(f =>
                        !f.name.endsWith('.mlmodelc') &&
                        !f.name.endsWith('.mlmodelc.zip') &&
                        f.type !== 'mlmodelc-bundle'
                    );
                    console.log('📁 MLMODELC-LOG: After filter - uploadedFiles count:', uploadedFiles.length);

                    // Add as bundle
                    const bundleObject = {
                        name: item.name,
                        type: 'mlmodelc-bundle',
                        directoryEntry: item.directoryEntry,
                        size: 0 // Will be calculated during ZIP
                    };
                    console.log('📁 MLMODELC-LOG: Created bundle object:', bundleObject);

                    uploadedFiles.push(bundleObject);
                    console.log('📁 MLMODELC-LOG: Added bundle to uploadedFiles, new count:', uploadedFiles.length);

                    console.log('✅ MLMODELC-LOG: Successfully added .mlmodelc bundle:', item.name);

                } else if (item.name && item.name.endsWith('.bin')) {
                    // Handle .bin file
                    console.log('📄 Processing .bin file:', item.name);

                    // Remove existing .bin files
                    uploadedFiles = uploadedFiles.filter(f => !f.name.endsWith('.bin'));

                    // Add new .bin file
                    uploadedFiles.push(item);
                    console.log('✅ Added .bin file:', item.name);

                } else if (item.name && item.name.endsWith('.mlmodelc.zip')) {
                    // Handle pre-zipped .mlmodelc file
                    console.log('📦 Processing .mlmodelc.zip file:', item.name);

                    // Remove existing .mlmodelc files
                    uploadedFiles = uploadedFiles.filter(f =>
                        !f.name.endsWith('.mlmodelc') &&
                        !f.name.endsWith('.mlmodelc.zip') &&
                        f.type !== 'mlmodelc-bundle'
                    );

                    // Add ZIP file
                    uploadedFiles.push(item);
                    console.log('✅ Added .mlmodelc.zip file:', item.name);

                } else {
                    console.log('⏭️ Skipping unsupported item:', item.name || 'unknown');
                }
            });

            updateUploadStatus();
        }

        function updateUploadStatus() {
            const statusDiv = document.getElementById('uploadStatus');
            const uploadBtn = document.getElementById('uploadBtn');

            const binFile = uploadedFiles.find(f => f.name.endsWith('.bin'));
            const mlmodelcFiles = uploadedFiles.filter(f =>
                f.name.endsWith('.mlmodelc') ||
                f.name.endsWith('.mlmodelc.zip') ||
                f.type === 'mlmodelc-bundle'
            );

            let status = '<div style="text-align: left;">';

            if (binFile) {
                status += '<p style="color: #28a745;">✅ .bin file: ' + binFile.name + ' (' + (binFile.size / 1024 / 1024).toFixed(1) + ' MB)</p>';
            } else {
                status += '<p style="color: #dc3545;">❌ .bin file: Not found</p>';
            }

            if (mlmodelcFiles.length > 0) {
                const file = mlmodelcFiles[0];
                let sizeText = '';
                let typeText = '';

                if (file.type === 'mlmodelc-bundle') {
                    typeText = ' (directory bundle)';
                    sizeText = ' (will be zipped)';
                } else if (file.size > 0) {
                    sizeText = ' (' + (file.size / 1024 / 1024).toFixed(1) + ' MB)';
                }

                status += '<p style="color: #28a745;">✅ .mlmodelc file: ' + file.name + sizeText + typeText + '</p>';
            } else {
                status += '<p style="color: #dc3545;">❌ .mlmodelc file: Not found</p>';
            }

            status += '</div>';
            statusDiv.innerHTML = status;

            // Enable upload button only if both files are present
            uploadBtn.disabled = !(binFile && mlmodelcFiles.length > 0);
        }

        async function uploadModel() {
            logToServer('🔥 BUTTON-CLICKED: Upload button was clicked - uploadModel() called');
            logToServer('🔥 UPLOAD-START: === UPLOAD MODEL FUNCTION CALLED ===');
            logToServer('🔥 UPLOAD-START: uploadedFiles array length: ' + uploadedFiles.length);
            logToServer('=== CLEAN UPLOAD STARTED ===');

            logToServer('🔥 UPLOAD-START: Looking for .bin file...');
            const binFile = uploadedFiles.find(f => f.name.endsWith('.bin'));
            logToServer('🔥 UPLOAD-START: binFile found: ' + (binFile ? binFile.name : 'NONE'));

            logToServer('🔥 UPLOAD-START: Looking for .mlmodelc files...');
            const mlmodelcFiles = uploadedFiles.filter(f =>
                f.name.endsWith('.mlmodelc') ||
                f.name.endsWith('.mlmodelc.zip') ||
                f.type === 'mlmodelc-bundle'
            );
            logToServer('🔥 UPLOAD-START: mlmodelcFiles found count: ' + mlmodelcFiles.length);

            console.log('📋 Upload Summary:');
            console.log('  .bin file:', binFile ? binFile.name : 'MISSING');
            console.log('  .mlmodelc file:', mlmodelcFiles.length > 0 ? mlmodelcFiles[0].name : 'MISSING');

            if (!binFile || mlmodelcFiles.length === 0) {
                alert('Both .bin file and .mlmodelc file/directory are required');
                return;
            }

            const uploadBtn = document.getElementById('uploadBtn');
            uploadBtn.disabled = true;
            uploadBtn.textContent = 'Preparing upload...';
            showProgressBar();

            try {
                // Check for existing files
                console.log('🔍 Checking for existing files...');
                const filesToCheck = [];
                filesToCheck.push(binFile.name);

                if (mlmodelcFiles[0].type === 'mlmodelc-bundle') {
                    filesToCheck.push(mlmodelcFiles[0].name + '.zip');
                } else {
                    filesToCheck.push(mlmodelcFiles[0].name);
                }

                const existingFiles = await checkExistingFiles(filesToCheck);
                if (existingFiles.length > 0) {
                    const fileList = existingFiles.map(f => '• ' + f).join('\n');
                    const overwrite = confirm(
                        `The following files already exist and will be overwritten:\n\n${fileList}\n\nDo you want to continue?`
                    );

                    if (!overwrite) {
                        uploadBtn.disabled = false;
                        uploadBtn.textContent = 'Upload Model';
                        hideProgressBar();
                        return;
                    }
                }

                // CLEAN ARCHITECTURE: Process .mlmodelc FIRST, then .bin
                console.log('🚀 Starting clean upload process...');

                // Process .mlmodelc file/directory FIRST
                const mlmodelcFile = mlmodelcFiles[0];
                logToServer('📦 MLMODELC-JS-LOG: === STARTING MLMODELC PROCESSING ===');
                logToServer('📦 MLMODELC-JS-LOG: mlmodelcFile.name: ' + mlmodelcFile.name);
                logToServer('📦 MLMODELC-JS-LOG: mlmodelcFile.type: ' + mlmodelcFile.type);
                logToServer('📦 MLMODELC-JS-LOG: Processing .mlmodelc FIRST: ' + mlmodelcFile.name + ' type: ' + mlmodelcFile.type);

                logToServer('📦 MLMODELC-JS-LOG: Checking if mlmodelcFile.type === "mlmodelc-bundle"');
                logToServer('📦 MLMODELC-JS-LOG: mlmodelcFile.type value: ' + mlmodelcFile.type);
                logToServer('📦 MLMODELC-JS-LOG: Comparison result: ' + (mlmodelcFile.type === 'mlmodelc-bundle'));

                if (mlmodelcFile.type === 'mlmodelc-bundle') {
                    logToServer('📦 MLMODELC-JS-LOG: === ENTERING MLMODELC-BUNDLE PROCESSING ===');
                    // CLEAN ARCHITECTURE: Create ZIP from directory and upload in chunks
                    logToServer('📦 MLMODELC-JS-LOG: About to log "Creating ZIP from .mlmodelc directory bundle..."');
                    logToServer('📦 Creating ZIP from .mlmodelc directory bundle...');
                    logToServer('📦 MLMODELC-JS-LOG: About to call updateProgress(10, ...)');
                    updateProgress(10, 'Creating ZIP from directory...');
                    logToServer('📦 MLMODELC-JS-LOG: About to set uploadBtn.textContent');
                    uploadBtn.textContent = 'Creating ZIP from directory...';
                    logToServer('📦 MLMODELC-JS-LOG: UI updated, about to call createZipFromDirectory()');
                    logToServer('📦 MLMODELC-JS-LOG: mlmodelcFile.directoryEntry exists: ' + !!mlmodelcFile.directoryEntry);

                    logToServer('📦 MLMODELC-JS-LOG: === CALLING createAndUploadZipFromDirectory() ===');

                    // Create upload callback for streaming
                    let totalChunks = 0;
                    const zipName = mlmodelcFile.directoryEntry.name + '.zip';

                    const uploadCallback = async (chunkBlob, chunkNumber) => {
                        totalChunks = Math.max(totalChunks, chunkNumber);
                        const chunkName = `${zipName}.part${chunkNumber}`;

                        console.log(`📤 Streaming upload chunk ${chunkNumber}:`, chunkName);
                        logToServer(`📤 Streaming upload chunk ${chunkNumber}: ${chunkName}`);

                        const chunkFile = new File([chunkBlob], chunkName, { type: 'application/zip' });

                        await uploadFileChunked(chunkFile, (progress) => {
                            const totalProgress = 10 + (chunkNumber / (totalChunks || chunkNumber)) * 40;
                            updateProgress(totalProgress, `Uploading ${chunkName}: ${Math.round(progress)}%`);
                            uploadBtn.textContent = `Uploading ${chunkName}: ${Math.round(progress)}%`;
                        });

                        console.log(`✅ Chunk ${chunkNumber} uploaded successfully`);
                        logToServer(`✅ Chunk ${chunkNumber} uploaded successfully`);
                    };

                    const zipResult = await createAndUploadZipFromDirectory(mlmodelcFile.directoryEntry, uploadCallback);
                    logToServer('📦 MLMODELC-JS-LOG: === createAndUploadZipFromDirectory() RETURNED ===');
                    logToServer('📦 MLMODELC-JS-LOG: zipResult.name: ' + zipResult.name);
                    logToServer('📦 ZIP created and uploaded: ' + zipResult.name + ' chunks: ' + zipResult.chunkCount);

                } else {
                    // Upload pre-zipped .mlmodelc file
                    console.log('📤 Uploading .mlmodelc.zip file:', mlmodelcFile.name);
                    updateProgress(10, 'Uploading .mlmodelc.zip file...');
                    uploadBtn.textContent = 'Uploading .mlmodelc.zip file...';

                    await uploadFileChunked(mlmodelcFile, (progress) => {
                        updateProgress(10 + (progress * 0.4), `Uploading ${mlmodelcFile.name}: ${Math.round(progress)}%`);
                        uploadBtn.textContent = `Uploading ${mlmodelcFile.name}: ${Math.round(progress)}%`;
                    });
                }

                console.log('✅ .mlmodelc file/bundle uploaded successfully');

                // Upload .bin file LAST
                console.log('📤 Uploading .bin file:', binFile.name);
                updateProgress(50, 'Uploading .bin file...');
                uploadBtn.textContent = 'Uploading .bin file...';

                await uploadFileChunked(binFile, (progress) => {
                    updateProgress(50 + (progress * 0.5), `Uploading ${binFile.name}: ${Math.round(progress)}%`);
                    uploadBtn.textContent = `Uploading ${binFile.name}: ${Math.round(progress)}%`;
                });

                console.log('✅ .bin file uploaded successfully');

                console.log('✅ All uploads completed successfully!');
                updateProgress(100, 'Upload completed successfully!');
                uploadBtn.textContent = 'Upload Complete!';

                setTimeout(() => {
                    alert('All files uploaded successfully!');
                    hideUploadArea();
                    loadWhisperService();
                }, 1000);

            } catch (error) {
                console.error('❌ Upload error:', error);
                alert('Upload failed: ' + error.message);
                uploadBtn.disabled = false;
                uploadBtn.textContent = 'Upload Model';
                hideProgressBar();
            }
        }

        async function checkExistingFiles(filenames) {
            try {
                console.log('Checking existing files:', filenames);
                const response = await fetch('/api/whisper/check-files', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({ filenames: filenames })
                });

                if (!response.ok) {
                    console.error('Failed to check existing files:', response.statusText);
                    return []; // If check fails, assume no existing files
                }

                const result = await response.json();
                return result.existing_files || [];
            } catch (error) {
                console.error('Error checking existing files:', error);
                return []; // If check fails, assume no existing files
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

                        // Calculate progress based on chunks completed
                        const progress = ((chunkIndex + 1) / totalChunks) * 100;
                        if (progressCallback) {
                            progressCallback(progress);
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

    // ADDED: JavaScript logging endpoint
    if (request.path == "/api/log" && request.method == "POST") {
        // Extract message from JSON body
        std::string message = "JS-LOG: Unknown message";
        try {
            // Simple JSON parsing for {"message": "..."} - handle both with and without spaces
            size_t start = request.body.find("\"message\"");
            if (start != std::string::npos) {
                // Find the opening quote after the colon
                start = request.body.find("\"", start + 9); // Skip past "message"
                if (start != std::string::npos) {
                    start++; // Skip the opening quote
                    size_t end = request.body.find("\"", start);
                    if (end != std::string::npos) {
                        message = "JS-LOG: " + request.body.substr(start, end - start);
                    }
                }
            }
        } catch (...) {
            message = "JS-LOG: Failed to parse log message";
        }

        // Write to server log
        write_server_log(message);

        HttpResponse response;
        response.status_code = 200;
        response.status_text = "OK";
        response.headers["Content-Type"] = "application/json";
        response.body = R"({"success": true})";
        return response;
    }

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
    } else if (request.path == "/api/whisper/check-files") {
        if (request.method == "POST") {
            return api_whisper_check_files(request);
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

    // Check for detailed status request
    bool detailed = false;
    auto it = request.query_params.find("detailed");
    if (it != request.query_params.end() && it->second == "true") {
        detailed = true;
    }

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

    std::string body = "{\"status\": \"online\", \"modules\": {" +
                      std::string("\"http_server\": \"online\", ") +
                      "\"database\": \"" + db_status + "\", " +
                      "\"sip_client\": \"" + sip_status + "\", " +
                      "\"whisper\": \"" + whisper_status + "\", " +
                      "\"llama\": \"offline\", " +
                      "\"piper\": \"offline\"}";

    // Add detailed info if requested
    if (detailed) {
        body += ", \"uptime\": " + std::to_string(time(nullptr)) +
                ", \"version\": \"1.0.0\"";
    }

    body += "}";
    response.body = body;

    return response;
}

HttpResponse SimpleHttpServer::api_callers(const HttpRequest& request) {
    HttpResponse response;
    response.status_code = 200;
    response.status_text = "OK";
    response.headers["Content-Type"] = "application/json";

    // Check for active filter
    bool active_only = false;
    auto it = request.query_params.find("active");
    if (it != request.query_params.end() && it->second == "true") {
        active_only = true;
    }

    // Get callers from database
    std::ostringstream json;
    json << "[";

    if (database_) {
        try {
            auto callers = database_->get_all_callers();
            bool first = true;
            for (const auto& caller : callers) {
                // Apply active filter if requested (check if they have recent calls)
                if (active_only && caller.last_call.empty()) {
                    continue;
                }

                if (!first) json << ",";
                first = false;

                json << "{"
                     << "\"id\":" << caller.id << ","
                     << "\"phone_number\":\"" << caller.phone_number << "\","
                     << "\"created_at\":\"" << caller.created_at << "\","
                     << "\"last_call\":\"" << caller.last_call << "\""
                     << "}";
            }
        } catch (const std::exception& e) {
            std::cout << "❌ Error getting callers: " << e.what() << std::endl;
        }
    }

    json << "]";
    response.body = "{\"callers\": " + json.str() + "}";
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

    // Check for enabled filter
    bool enabled_only = false;
    auto it = request.query_params.find("enabled");
    if (it != request.query_params.end() && it->second == "true") {
        enabled_only = true;
    }

    // Get SIP lines from database
    auto sip_lines = database_->get_all_sip_lines();

    // Convert to JSON with filtering
    std::ostringstream json;
    json << "{\"sip_lines\":[";

    bool first = true;
    for (const auto& line : sip_lines) {
        // Apply filter if requested
        if (enabled_only && !line.enabled) {
            continue;
        }

        if (!first) json << ",";
        first = false;

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

    // Check for force delete parameter
    bool force = false;
    auto it = request.query_params.find("force");
    if (it != request.query_params.end() && it->second == "true") {
        force = true;
    }

    // If not force delete, check if line is currently active
    if (!force) {
        auto sip_lines = database_->get_all_sip_lines();
        for (const auto& line : sip_lines) {
            if (line.line_id == line_id && line.enabled && line.status == "active") {
                response.status_code = 409;
                response.status_text = "Conflict";
                response.body = R"({"error": "Cannot delete active SIP line. Use force=true to override."})";
                return response;
            }
        }
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

    // Check for specific enable/disable action
    std::string action = "";
    auto it = request.query_params.find("action");
    if (it != request.query_params.end()) {
        action = it->second;
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

    // Check for format parameter
    std::string format = "json";
    auto it = request.query_params.find("format");
    if (it != request.query_params.end()) {
        format = it->second;
    }

    response.status_code = 200;
    response.status_text = "OK";

    if (format == "text") {
        response.headers["Content-Type"] = "text/plain";
        response.body = std::to_string(speed);
    } else {
        response.body = "{\"system_speed\": " + std::to_string(speed) + "}";
    }

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

    // Check for detailed info request
    bool include_stats = false;
    auto it = request.query_params.find("stats");
    if (it != request.query_params.end() && it->second == "true") {
        include_stats = true;
    }

    bool enabled = database_->get_whisper_service_enabled();
    std::string model_path = database_->get_whisper_model_path();
    std::string status = database_->get_whisper_service_status();

    response.status_code = 200;
    response.status_text = "OK";
    std::string body = "{\"enabled\": " + std::string(enabled ? "true" : "false") +
                      ", \"model_path\": \"" + model_path +
                      "\", \"status\": \"" + status + "\"";

    if (include_stats) {
        body += ", \"uptime\": " + std::to_string(time(nullptr)) +
                ", \"memory_usage\": \"unknown\"";
    }

    body += "}";
    response.body = body;

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

    // Check for specific enable/disable action
    bool current_enabled = database_->get_whisper_service_enabled();
    bool new_enabled = !current_enabled;

    auto it = request.query_params.find("enable");
    if (it != request.query_params.end()) {
        new_enabled = (it->second == "true");
    }

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

    // Check for chunked files (.part1, .part2, etc.)
    bool is_chunked = false;
    if (filename.find(".part") != std::string::npos) {
        // Extract base filename before .partN
        size_t part_pos = filename.find(".part");
        std::string base_filename = filename.substr(0, part_pos);
        // Check if base filename is valid
        bool base_is_bin = base_filename.length() >= 4 && base_filename.substr(base_filename.length() - 4) == ".bin";
        bool base_is_mlmodelc = base_filename.length() >= 9 && base_filename.substr(base_filename.length() - 9) == ".mlmodelc";
        bool base_is_mlmodelc_zip = base_filename.length() >= 13 && base_filename.substr(base_filename.length() - 13) == ".mlmodelc.zip";
        is_chunked = base_is_bin || base_is_mlmodelc || base_is_mlmodelc_zip;
    }

    if (!is_bin && !is_mlmodelc && !is_mlmodelc_zip && !is_chunked) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Invalid file type. Only .bin, .mlmodelc, .mlmodelc.zip files and their chunks (.partN) are allowed"})";
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

HttpResponse SimpleHttpServer::api_whisper_check_files(const HttpRequest& request) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";
    response.headers["Access-Control-Allow-Origin"] = "*";
    response.headers["Access-Control-Allow-Headers"] = "Content-Type";

    if (!database_) {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Database not available"})";
        return response;
    }

    // Parse JSON body to get list of filenames to check
    std::string json_response = "{\"existing_files\": [";
    bool first = true;

    // Simple JSON parsing to extract filenames
    size_t pos = 0;
    while ((pos = request.body.find("\"", pos)) != std::string::npos) {
        pos++; // Skip opening quote
        size_t end = request.body.find("\"", pos);
        if (end != std::string::npos) {
            std::string filename = request.body.substr(pos, end - pos);

            // Check if this looks like a filename (has extension)
            if (filename.find('.') != std::string::npos &&
                (filename.find('/') == std::string::npos || filename.find("models/") == 0)) {

                // Remove models/ prefix if present
                if (filename.find("models/") == 0) {
                    filename = filename.substr(7);
                }

                // Check if file exists in models directory
                std::string filepath = "models/" + filename;
                struct stat file_stat;
                if (stat(filepath.c_str(), &file_stat) == 0) {
                    if (!first) json_response += ", ";
                    json_response += "\"" + filename + "\"";
                    first = false;
                }
            }
            pos = end + 1;
        } else {
            break;
        }
    }

    json_response += "]}";

    response.status_code = 200;
    response.status_text = "OK";
    response.body = json_response;
    return response;
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

    // Check for file type filter
    std::string file_type = "bin";
    auto it = request.query_params.find("type");
    if (it != request.query_params.end()) {
        file_type = it->second;
    }

    // Scan models directory for model files
    std::string json_response = R"({"models": [)";
    bool first = true;

    // Scan the models directory for model files
    std::string models_dir = "models";
    DIR* dir = opendir(models_dir.c_str());

    if (dir != nullptr) {
        struct dirent* entry;
        std::vector<std::string> model_files;

        // Collect model files based on type filter
        while ((entry = readdir(dir)) != nullptr) {
            std::string filename = entry->d_name;
            std::string extension = "." + file_type;
            if (filename.length() > extension.length() &&
                filename.substr(filename.length() - extension.length()) == extension) {
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

    // Simplified upload handling - removed complex concurrent limits
    std::lock_guard<std::mutex> lock(uploads_mutex);

    // Parse Content-Range header: "bytes start-end/total" or "bytes start-end/*"
    size_t range_start = 0, range_end = 0;

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

    // Validate chunk size matches request body (only if we have content range)
    if (!content_range.empty()) {
        size_t expected_chunk_size = range_end - range_start + 1;
        if (expected_chunk_size != request.body.length()) {
            response.status_code = 400;
            response.status_text = "Bad Request";
            response.body = R"({"error": "Chunk size mismatch"})";
            return response;
        }
    }

    // Validate chunk size limits (max 10MB per chunk)
    if (request.body.length() > 10 * 1024 * 1024) {
        response.status_code = 413;
        response.status_text = "Payload Too Large";
        response.body = "{\"error\": \"Chunk too large (max 10MB)\"}";
        return response;
    }

    // Simplified filename handling - use filename directly as key
    std::string upload_key = filename;
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
        std::cout << "🎤 Started chunked upload: " << filename << " (total: " << total_size << " bytes)" << std::endl;
    }

    // Get upload reference (we know it exists now)
    auto& upload = active_uploads[upload_key];

    // Validate chunk sequence (only if we have content range)
    if (!content_range.empty() && range_start != upload->received_size) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Chunk out of sequence"})";
        return response;
    }

    // Write chunk to file
    upload->file_stream.write(request.body.c_str(), request.body.length());
    if (!upload->file_stream.good()) {
        upload->file_stream.close();
        active_uploads.erase(upload_key);
        std::string filepath = "models/" + filename;
        std::remove(filepath.c_str());
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Failed to write chunk"})";
        return response;
    }

    // Update progress
    upload->received_size += request.body.length();
    upload->last_activity = std::chrono::steady_clock::now();

    // Check if upload is complete
    bool upload_completed = (upload->received_size >= upload->total_size) ||
                          (range_end + 1 >= total_size && total_size > 0);

    if (upload_completed) {
        // FIXED: Store all values BEFORE accessing upload members to prevent use-after-free
        size_t received_size = upload->received_size;

        // Close file stream
        upload->file_stream.close();

        // Clean up completed upload ONCE - do this early to prevent any use-after-free
        active_uploads.erase(upload_key);

        std::cout << "🎤 Completed chunked upload: " << filename << " (" << received_size << " bytes)" << std::endl;

        // DEBUG: Add safety logging
        std::cout << "DEBUG: About to validate file size" << std::endl;

        // Validate final file size
        if (total_size > 0 && received_size != total_size) {
            std::cout << "DEBUG: File size validation failed" << std::endl;
            std::string filepath = "models/" + filename;
            std::remove(filepath.c_str());
            response.status_code = 400;
            response.status_text = "Bad Request";
            response.body = R"({"error": "Incomplete upload"})";
            return response;
        }

        std::cout << "DEBUG: File size validation passed" << std::endl;
        std::cout << "DEBUG: Checking if file is .mlmodelc.zip" << std::endl;

        // Handle .zip files - extract them after upload
        if (filename.length() >= 4 && filename.substr(filename.length() - 4) == ".zip") {
            std::cout << "DEBUG: File IS .zip, starting extraction" << std::endl;
            std::string zip_path = "models/" + filename;
            std::string extract_dir = "models/" + filename.substr(0, filename.length() - 4); // Remove .zip extension

            std::cout << "🎤 Extracting .zip: " << filename << " to " << extract_dir << std::endl;

            // Create extraction directory
            std::string mkdir_cmd = "mkdir -p \"" + extract_dir + "\"";
            system(mkdir_cmd.c_str());

            // Extract zip file using unzip command - simple approach
            std::string unzip_cmd = "cd models && unzip -o \"" + filename + "\"";
            int unzip_result = system(unzip_cmd.c_str());

            if (unzip_result == 0) {
                std::cout << "✅ Successfully extracted " << filename << std::endl;

                // Remove the zip file after extraction
                std::remove(zip_path.c_str());
                std::cout << "🗑️ Removed zip file: " << filename << std::endl;

                // FIXED: Safe JSON construction
                std::string extracted_name = filename.substr(0, filename.length() - 4);
                response.body = R"({"success": true, "message": "Upload completed and extracted", "filename": ")" + extracted_name + R"("})";
            } else {
                std::cout << "❌ Failed to extract " << filename << " (exit code: " << unzip_result << ")" << std::endl;
                // FIXED: Safe JSON construction
                response.body = R"({"success": true, "message": "Upload completed but extraction failed", "filename": ")" + filename + R"("})";
            }
        } else {
            std::cout << "DEBUG: File is NOT .mlmodelc.zip, creating success response" << std::endl;

            // FIXED: Safe JSON response - escape filename to prevent segfault
            std::cout << "DEBUG: About to escape filename: " << filename << std::endl;
            std::string escaped_filename = filename;

            std::cout << "DEBUG: Starting JSON escaping" << std::endl;
            // Simple JSON escaping for common problematic characters
            size_t pos = 0;
            while ((pos = escaped_filename.find("\"", pos)) != std::string::npos) {
                escaped_filename.replace(pos, 1, "\\\"");
                pos += 2;
            }
            pos = 0;
            while ((pos = escaped_filename.find("\\", pos)) != std::string::npos) {
                escaped_filename.replace(pos, 1, "\\\\");
                pos += 2;
            }

            std::cout << "DEBUG: JSON escaping completed, escaped filename: " << escaped_filename << std::endl;
            std::cout << "DEBUG: About to create response body" << std::endl;

            response.body = R"({"success": true, "message": "Upload completed", "filename": ")" + escaped_filename + R"("})";

            std::cout << "DEBUG: Response body created successfully" << std::endl;
        }

        std::cout << "DEBUG: About to set response status" << std::endl;
        response.status_code = 200;
        response.status_text = "OK";
        std::cout << "DEBUG: About to return response" << std::endl;
        return response;
    } else {
        // Partial content response
        response.status_code = 206;
        response.status_text = "Partial Content";
        response.headers["Range"] = "bytes=0-" + std::to_string(upload->received_size - 1);

        double progress = total_size > 0 ? (double)upload->received_size / total_size * 100.0 : 0.0;
        response.body = R"({"success": true, "progress": )" + std::to_string(progress) +
                       R"(, "received": )" + std::to_string(upload->received_size) + "}";
    }

    return response;
}
