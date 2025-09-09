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
#include <iomanip>

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

// Chunked upload code removed - using streaming approach only

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
    bool socket_closed = false;
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

        // Simple request handling - streaming uploads use separate handler

        // Check if this is a streaming upload request
        if (raw_request.find("POST /api/upload-stream") == 0) {
            try {
                // Handle streaming upload directly from socket
                HttpResponse response = handle_streaming_upload(client_socket, raw_request);
                std::string response_str = create_response(response);
                send(client_socket, response_str.c_str(), response_str.length(), 0);
                close(client_socket);
                socket_closed = true; // Mark as closed
                return; // Socket closed, exit function
            } catch (const std::exception& e) {
                std::cout << "❌ STREAM: Exception in streaming upload: " << e.what() << std::endl;
                close(client_socket);
                socket_closed = true; // Mark as closed
                return; // Socket closed, exit function
            } catch (...) {
                std::cout << "❌ STREAM: Unknown exception in streaming upload" << std::endl;
                close(client_socket);
                socket_closed = true; // Mark as closed
                return; // Socket closed, exit function
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

    // Safe socket close - only if not already closed
    if (!socket_closed) {
        close(client_socket);
    }
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

HttpResponse SimpleHttpServer::handle_streaming_upload(int client_socket, const std::string& headers_data) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";
    response.headers["Access-Control-Allow-Origin"] = "*";

    // Parse headers from the initial request
    std::istringstream stream(headers_data);
    std::string line;
    std::map<std::string, std::string> headers;

    // Skip request line
    std::getline(stream, line);

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
            headers[key] = value;
        }
    }

    // Get filename from header
    std::string filename;
    auto filename_it = headers.find("X-File-Name");
    if (filename_it != headers.end()) {
        filename = filename_it->second;
    } else {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Missing X-File-Name header"})";
        return response;
    }

    // Get content length
    size_t content_length = 0;
    auto length_it = headers.find("Content-Length");
    if (length_it != headers.end()) {
        content_length = std::stoull(length_it->second);
    } else {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Missing Content-Length header"})";
        return response;
    }

    std::cout << "📤 STREAM: Receiving file: " << filename << " (" << content_length << " bytes)" << std::endl;

    // Validate filename for security
    if (filename.find("..") != std::string::npos ||
        filename.find("\\") != std::string::npos) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Invalid filename"})";
        return response;
    }

    // Extract basename for validation
    std::string basename = filename;
    size_t last_slash = filename.find_last_of('/');
    if (last_slash != std::string::npos) {
        basename = filename.substr(last_slash + 1);
    }

    // Validate file type
    bool is_bin = basename.length() >= 4 && basename.substr(basename.length() - 4) == ".bin";
    bool is_mlmodelc = basename.length() >= 9 && basename.substr(basename.length() - 9) == ".mlmodelc";
    bool is_mlmodelc_zip = basename.length() >= 13 && basename.substr(basename.length() - 13) == ".mlmodelc.zip";
    bool is_json = basename.length() >= 5 && basename.substr(basename.length() - 5) == ".json";
    bool is_mil = basename.length() >= 4 && basename.substr(basename.length() - 4) == ".mil";

    if (!is_bin && !is_mlmodelc && !is_mlmodelc_zip && !is_json && !is_mil) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Invalid file type"})";
        return response;
    }

    // Create directory structure if needed
    std::string filepath = "models/" + filename;
    if (last_slash != std::string::npos) {
        std::string dir_path = "models/" + filename.substr(0, last_slash);
        std::string mkdir_cmd = "mkdir -p \"" + dir_path + "\"";
        system(mkdir_cmd.c_str());
    }

    // Open file for writing
    std::ofstream file(filepath, std::ios::binary);
    if (!file) {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Failed to create file"})";
        return response;
    }

    // Stream data directly from socket to file
    const size_t BUFFER_SIZE = 64 * 1024; // 64KB buffer
    char buffer[BUFFER_SIZE];
    size_t total_received = 0;
    size_t remaining = content_length;

    // Check if there's already some body data in the headers_data
    size_t headers_end = headers_data.find("\r\n\r\n");
    if (headers_end != std::string::npos && headers_end + 4 < headers_data.length()) {
        // Write existing body data
        std::string existing_body = headers_data.substr(headers_end + 4);
        file.write(existing_body.c_str(), existing_body.length());
        total_received += existing_body.length();
        remaining -= existing_body.length();
    }

    // Stream remaining data
    while (remaining > 0) {
        size_t to_read = std::min(remaining, BUFFER_SIZE);
        ssize_t bytes_read = recv(client_socket, buffer, to_read, 0);

        if (bytes_read <= 0) {
            file.close();
            response.status_code = 500;
            response.status_text = "Internal Server Error";
            response.body = R"({"error": "Connection error during upload"})";
            return response;
        }

        file.write(buffer, bytes_read);
        total_received += bytes_read;
        remaining -= bytes_read;
    }

    file.close();

    std::cout << "✅ STREAM: File saved: " << filepath << " (" << total_received << " bytes)" << std::endl;

    response.status_code = 200;
    response.status_text = "OK";
    response.body = R"({"status": "success", "message": "File uploaded successfully"})";
    return response;
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
            <h2>🦙 LLaMA Service</h2>
            <div id="llamaServiceContainer">
                <div class="status-grid">
                    <div class="status-item">
                        <h3>Service Status</h3>
                        <div id="llamaStatus" class="status-offline">● Stopped</div>
                    </div>
                    <div class="status-item">
                        <h3>Available Models</h3>
                        <div id="llamaModelList" style="max-height: 150px; overflow-y: auto; border: 1px solid #ddd; border-radius: 4px; padding: 5px;">
                            Loading models...
                        </div>
                    </div>
                </div>

                <div style="margin: 20px 0;">
                    <button id="llamaToggleBtn" class="refresh-btn" onclick="toggleLlamaService()">
                        Start Service
                    </button>
                    <button id="llamaRestartBtn" class="refresh-btn" onclick="restartLlamaWithSelectedModel()" style="margin-left: 10px; background: #ffc107; color: #000;" disabled>
                        Restart with Selected Model
                    </button>
                </div>
            </div>
        </div>

        <div class="card">
            <h2>🎤 Piper TTS Service</h2>
            <div id="piperServiceContainer">
                <div class="status-grid">
                    <div class="status-item">
                        <h3>Service Status</h3>
                        <div id="piperStatus" class="status-offline">● Stopped</div>
                    </div>
                    <div class="status-item">
                        <h3>Available Models</h3>
                        <div id="piperModelList" style="max-height: 150px; overflow-y: auto; border: 1px solid #ddd; border-radius: 4px; padding: 5px;">
                            Loading models...
                        </div>
                    </div>
                </div>

                <div style="margin: 20px 0;">
                    <button id="piperToggleBtn" class="refresh-btn" onclick="togglePiperService()">
                        Start Service
                    </button>
                    <button id="piperRestartBtn" class="refresh-btn" onclick="restartPiperWithSelectedModel()" style="margin-left: 10px; background: #ffc107; color: #000;" disabled>
                        Restart with Selected Model
                    </button>
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
                <li><a href="/api/llama/service">/api/llama/service</a> - LLaMA service info</li>
                <li><strong>POST</strong> /api/llama/service/toggle - Start/stop service</li>
                <li><a href="/api/llama/models">/api/llama/models</a> - Available LLaMA models</li>
                <li><a href="/api/piper/service">/api/piper/service</a> - Piper TTS service info</li>
                <li><strong>POST</strong> /api/piper/service/toggle - Start/stop service</li>
                <li><a href="/api/piper/models">/api/piper/models</a> - Available Piper models</li>
            </ul>
        </div>
    </div>

    <script src="/jszip.min.js"></script>
    <script>
        // Cache buster: v3.0 - Force browser to reload JavaScript with JSZip support
        console.log('JavaScript loaded - version 3.0 with JSZip support');

        async function refreshStatus() {
            console.log('🔄 refreshStatus() called');
            try {
                console.log('📡 Fetching /api/status...');

                // Add timeout to prevent hanging fetch requests
                const controller = new AbortController();
                const timeoutId = setTimeout(() => controller.abort(), 3000); // 3 second timeout

                const response = await fetch('/api/status', {
                    signal: controller.signal
                });
                clearTimeout(timeoutId);

                console.log('📡 Status response received:', response.status, response.statusText);

                if (!response.ok) {
                    throw new Error(`HTTP ${response.status}: ${response.statusText}`);
                }
                const data = await response.json();
                console.log('Status:', data);
                // Update UI based on API response
                const safeData = data || { status: 'unknown', modules: {} };
                updateStatusDisplay(safeData);
            } catch (error) {
                if (error.name === 'AbortError') {
                    console.error('❌ Status request timed out after 3 seconds');
                    updateStatusDisplay({ status: 'timeout', modules: {} });
                } else {
                    console.error('❌ Failed to fetch status:', error);
                    updateStatusDisplay({ status: 'error', modules: {} });
                }
            }
        }

        function updateStatusDisplay(data) {
            // FIXED: Only update the main status grid, NOT the Whisper service section
            if (data && data.modules) {
                // Only target status items in the main status grid, not the Whisper service section
                const mainStatusGrid = document.querySelector('.status-grid');
                if (mainStatusGrid) {
                    const items = mainStatusGrid.querySelectorAll('.status-item');
                    items.forEach(item => {
                        try {
                            const titleElement = item.querySelector('h3');
                            const statusDiv = item.querySelector('div:last-child');
                            if (titleElement && statusDiv && statusDiv.id !== 'modelList') {
                                const title = titleElement.textContent.toLowerCase().replace(' ', '_');
                                if (data.modules[title] === 'online') {
                                    statusDiv.className = 'status-online';
                                    statusDiv.textContent = '● Online';
                                } else {
                                    statusDiv.className = 'status-offline';
                                    statusDiv.textContent = '● Offline';
                                }
                            }
                        } catch (error) {
                            console.error('Error updating status item:', error);
                        }
                    });
                }
            }
        }

        // Load SIP lines on page load - use direct call for initial load
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
                // Refresh list using safe version
                safeLoadSipLines();
            })
            .catch(error => {
                console.error('Error:', error);
                alert('Error adding SIP line: ' + error.message);
            });
        };

        // Auto-refresh with proper cleanup and race condition prevention
        let refreshIntervals = [];
        let isRefreshing = { status: false, sip: false, whisper: false };

        // Reset all refresh flags on page load to prevent stuck states
        function resetRefreshFlags() {
            isRefreshing.status = false;
            isRefreshing.sip = false;
            isRefreshing.whisper = false;
            console.log('🔄 Refresh flags reset');
        }

        // Call reset immediately
        resetRefreshFlags();

        function safeRefreshStatus() {
            if (isRefreshing.status) {
                console.log('Status already refreshing, skipping...');
                return;
            }
            isRefreshing.status = true;
            console.log('Starting status refresh...');

            // Set timeout to force reset if stuck (last resort)
            const safetyTimeoutId = setTimeout(() => {
                console.warn('⚠️ Status refresh safety timeout - forcing reset');
                isRefreshing.status = false;
            }, 3000); // 3 second safety timeout

            refreshStatus()
                .then(() => {
                    console.log('Status refresh completed successfully');
                })
                .catch((error) => {
                    console.error('Status refresh failed:', error);
                })
                .finally(() => {
                    clearTimeout(safetyTimeoutId);
                    console.log('Resetting status refresh flag');
                    isRefreshing.status = false;
                });
        }

        function safeLoadSipLines() {
            if (isRefreshing.sip) {
                console.log('SIP lines already refreshing, skipping...');
                return;
            }
            isRefreshing.sip = true;
            console.log('Starting SIP lines refresh...');

            // Set timeout to force reset if stuck (last resort)
            const safetyTimeoutId = setTimeout(() => {
                console.warn('⚠️ SIP refresh safety timeout - forcing reset');
                isRefreshing.sip = false;
            }, 3000); // 3 second safety timeout

            loadSipLines()
                .then(() => {
                    console.log('SIP lines refresh completed successfully');
                })
                .catch((error) => {
                    console.error('SIP lines refresh failed:', error);
                })
                .finally(() => {
                    clearTimeout(safetyTimeoutId);
                    console.log('Resetting SIP refresh flag');
                    isRefreshing.sip = false;
                });
        }

        function safeLoadWhisperService() {
            if (isRefreshing.whisper) {
                console.log('Whisper service already refreshing, skipping...');
                return;
            }
            isRefreshing.whisper = true;
            console.log('Starting Whisper service refresh...');

            // Set timeout to force reset if stuck (last resort)
            const safetyTimeoutId = setTimeout(() => {
                console.warn('⚠️ Whisper refresh safety timeout - forcing reset');
                isRefreshing.whisper = false;
            }, 3000); // 3 second safety timeout

            loadWhisperService()
                .then(() => {
                    console.log('Whisper service refresh completed successfully');
                })
                .catch((error) => {
                    console.error('Whisper service refresh failed:', error);
                })
                .finally(() => {
                    clearTimeout(safetyTimeoutId);
                    console.log('Resetting Whisper refresh flag');
                    isRefreshing.whisper = false;
                });
        }

        refreshIntervals.push(setInterval(safeRefreshStatus, 5000));
        refreshIntervals.push(setInterval(safeLoadSipLines, 3000));
        refreshIntervals.push(setInterval(safeLoadWhisperService, 5000));

        // Cleanup intervals on page unload
        window.addEventListener('beforeunload', () => {
            refreshIntervals.forEach(interval => clearInterval(interval));
        });

        async function loadSipLines() {
            console.log('🔄 loadSipLines() called');

            // ALWAYS clear the loading message first
            const container = document.getElementById('sipLinesContainer');
            if (!container) {
                console.error('❌ sipLinesContainer element not found!');
                return;
            }

            try {
                console.log('📡 Fetching /api/sip-lines...');

                // Add timeout to prevent hanging fetch requests
                const controller = new AbortController();
                const timeoutId = setTimeout(() => controller.abort(), 3000); // 3 second timeout

                const response = await fetch('/api/sip-lines', {
                    signal: controller.signal
                });
                clearTimeout(timeoutId);

                console.log('📡 Response received:', response.status, response.statusText);

                if (!response.ok) {
                    throw new Error(`HTTP ${response.status}: ${response.statusText}`);
                }

                const data = await response.json();
                console.log('📡 Data received:', data);

                const sipLines = data && data.sip_lines ? data.sip_lines : [];
                console.log('📡 SIP lines extracted:', sipLines.length, 'lines');

                displaySipLines(sipLines);
                console.log('✅ SIP lines displayed successfully');
            } catch (error) {
                if (error.name === 'AbortError') {
                    console.error('❌ SIP lines request timed out after 3 seconds');
                    container.innerHTML = '<p>Request timed out - server may be slow</p>';
                } else {
                    console.error('❌ Failed to load SIP lines:', error);
                    container.innerHTML = '<p>Unable to load SIP lines</p>';
                }
                console.log('❌ Fallback UI displayed');
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
                    safeLoadSipLines(); // Refresh the list safely
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
                        safeLoadSipLines(); // Refresh the list safely
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

        // Simple console logging only
        console.log('🔥 CONSOLE-TEST: JavaScript is working');
        console.log('🔥 PAGE-LOADED: JavaScript logging system initialized');

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

                console.log('🎯 DROP-EVENT: Drop event detected');

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

        // STREAMING: Upload files directly to backend
        async function streamFilesToBackend(droppedItems) {
            console.log('📤 STREAM: === STARTING DIRECT STREAMING TO BACKEND ===');
            console.log('📤 STREAM: Total items to stream:', droppedItems.length);

            for (let i = 0; i < droppedItems.length; i++) {
                const item = droppedItems[i];
                console.log('📤 STREAM: Processing item', i + 1, '/', droppedItems.length, ':', item.name);
                console.log('📤 STREAM: Item type:', item.type, 'Has file:', !!item.file);

                if (item.type === 'directory-bundle' || item.type === 'mlmodelc-bundle') {
                    console.log('📁 STREAM: Streaming directory bundle:', item.name);
                    await streamDirectoryToBackend(item.directoryEntry, item.name, i, droppedItems.length);
                } else if (item.file) {
                    console.log('📄 STREAM: Streaming file:', item.name);
                    await streamFileToBackend(item.file, i, droppedItems.length);
                } else {
                    console.log('⚠️ STREAM: Unknown item type or missing file:', item);
                }
            }

            console.log('✅ STREAM: All items streamed successfully');
        }

        // Stream individual file to backend (TRUE STREAMING - no chunking)
        async function streamFileToBackend(file, fileIndex = 0, totalFiles = 1) {
            console.log('📄 STREAM-FILE: Uploading:', file.name, '(' + (file.size / 1024 / 1024).toFixed(1) + ' MB)');

            const xhr = new XMLHttpRequest();

            return new Promise((resolve, reject) => {
                xhr.upload.addEventListener('progress', (e) => {
                    if (e.lengthComputable) {
                        // Calculate overall progress across all files
                        const fileProgress = (e.loaded / e.total) * 100;
                        const overallProgress = ((fileIndex * 100) + fileProgress) / totalFiles;

                        updateProgress(overallProgress, `Uploading ${file.name}: ${Math.round(fileProgress)}% (File ${fileIndex + 1}/${totalFiles})`);
                        const uploadBtn = document.getElementById('uploadBtn');
                        if (uploadBtn) {
                            uploadBtn.textContent = `Uploading ${file.name}: ${Math.round(fileProgress)}% (${fileIndex + 1}/${totalFiles})`;
                        }
                    }
                });

                xhr.addEventListener('load', () => {
                    if (xhr.status === 200) {
                        console.log('✅ STREAM-FILE: Upload completed:', file.name);
                        resolve();
                    } else {
                        console.error('❌ STREAM-FILE: Upload failed:', xhr.status, xhr.responseText);
                        reject(new Error(`Upload failed: ${xhr.status} ${xhr.responseText}`));
                    }
                });

                xhr.addEventListener('error', () => {
                    console.error('❌ STREAM-FILE: Network error during upload:', file.name);
                    reject(new Error('Network error during upload'));
                });

                xhr.open('POST', '/api/upload-stream');
                xhr.setRequestHeader('X-File-Name', file.name);
                xhr.setRequestHeader('Content-Type', 'application/octet-stream');
                xhr.send(file);
            });
        }

        // Stream directory to backend (as individual files)
        async function streamDirectoryToBackend(directoryEntry, baseName, fileIndex = 0, totalFiles = 1) {
            console.log('📁 STREAM-DIR: Streaming directory contents as individual files:', baseName);

            // Stream each file in the directory individually
            await streamDirectoryContents(directoryEntry, baseName, fileIndex, totalFiles);
        }

        // Stream directory contents as individual files (no ZIP creation)
        async function streamDirectoryContents(directoryEntry, pathPrefix, fileIndex = 0, totalFiles = 1) {
            return new Promise((resolve, reject) => {
                const reader = directoryEntry.createReader();

                function readEntries() {
                    reader.readEntries(async (entries) => {
                        if (entries.length === 0) {
                            resolve();
                            return;
                        }

                        for (const entry of entries) {
                            const entryPath = pathPrefix ? pathPrefix + '/' + entry.name : entry.name;

                            if (entry.isFile) {
                                console.log('📄 STREAM-DIR: Streaming file:', entryPath);
                                const file = await new Promise((res, rej) => entry.file(res, rej));
                                const renamedFile = new File([file], entryPath, { type: file.type });
                                await streamFileToBackend(renamedFile, fileIndex, totalFiles);
                            } else if (entry.isDirectory) {
                                console.log('📁 STREAM-DIR: Streaming subdirectory:', entryPath);
                                await streamDirectoryContents(entry, entryPath, fileIndex, totalFiles);
                            }
                        }
                        readEntries(); // Continue reading
                    }, reject);
                }
                readEntries();
            });
        }

        // Dead code removed: addDirectoryToZip() - not used with streaming approach

        function handleFiles(e) {
            const files = Array.from(e.target.files);
            processDroppedItems(files);
        }

        function processDroppedItems(items) {
            console.log('🔍 Processing', items.length, 'dropped items');

            items.forEach(item => {
                if (item.type === 'directory-bundle') {
                    // Handle .mlmodelc directory bundle
                    console.log('📁 Processing directory bundle:', item.name);

                    // Remove existing .mlmodelc files
                    uploadedFiles = uploadedFiles.filter(f =>
                        !f.name.endsWith('.mlmodelc') &&
                        !f.name.endsWith('.mlmodelc.zip') &&
                        f.type !== 'mlmodelc-bundle'
                    );

                    // Add as bundle
                    uploadedFiles.push({
                        name: item.name,
                        type: 'mlmodelc-bundle',
                        directoryEntry: item.directoryEntry,
                        size: 0
                    });

                    console.log('✅ Added .mlmodelc bundle:', item.name);

                } else if (item.name && item.name.endsWith('.bin')) {
                    // Handle .bin file
                    console.log('📄 Processing .bin file:', item.name);

                    // Remove existing .bin files
                    uploadedFiles = uploadedFiles.filter(f => !f.name.endsWith('.bin'));

                    // Add new .bin file with proper structure
                    uploadedFiles.push({
                        name: item.name,
                        type: 'file',
                        file: item.file || item // Ensure file property exists
                    });
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
            console.log('🔥 BUTTON-CLICKED: Upload button was clicked - uploadModel() called');
            console.log('🔥 UPLOAD-START: === UPLOAD MODEL FUNCTION CALLED ===');
            console.log('🔥 UPLOAD-START: uploadedFiles array length: ' + uploadedFiles.length);
            console.log('=== CLEAN UPLOAD STARTED ===');

            console.log('🔥 UPLOAD-START: Looking for .bin file...');
            const binFile = uploadedFiles.find(f => f.name.endsWith('.bin'));
            console.log('🔥 UPLOAD-START: binFile found: ' + (binFile ? binFile.name : 'NONE'));

            console.log('🔥 UPLOAD-START: Looking for .mlmodelc files...');
            const mlmodelcFiles = uploadedFiles.filter(f =>
                f.name.endsWith('.mlmodelc') ||
                f.name.endsWith('.mlmodelc.zip') ||
                f.type === 'mlmodelc-bundle'
            );
            console.log('🔥 UPLOAD-START: mlmodelcFiles found count: ' + mlmodelcFiles.length);

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

                // SIMPLIFIED: Create one ZIP from ALL dropped items
                console.log('🚀 Starting simplified ZIP-ALL upload process...');
                console.log('📦 ZIP-ALL: Total files to zip:', uploadedFiles.length);
                // Stream all files directly to backend
                console.log('📤 STREAM: Starting direct streaming to backend...');
                updateProgress(10, 'Streaming files to server...');
                uploadBtn.textContent = 'Streaming files to server...';

                await streamFilesToBackend(uploadedFiles);

                console.log('✅ .bin file uploaded successfully');

                console.log('✅ All uploads completed successfully!');
                updateProgress(100, 'Upload completed successfully!');
                uploadBtn.textContent = 'Upload Complete!';

                setTimeout(() => {
                    alert('All files uploaded successfully!');
                    hideUploadArea();
                    // Don't call loadWhisperService() here - let the 5-second interval handle it
                    console.log('Upload completed, letting interval refresh handle model list update');
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

        // Dead code removed: uploadFileChunked() - replaced by streaming approach

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
            console.log('🔄 loadWhisperService() called');
            try {
                console.log('📡 Fetching whisper service and models...');

                // Create abort controllers for both requests
                const serviceController = new AbortController();
                const modelsController = new AbortController();

                // Set timeouts for both requests
                const serviceTimeoutId = setTimeout(() => serviceController.abort(), 3000);
                const modelsTimeoutId = setTimeout(() => modelsController.abort(), 3000);

                const [serviceResponse, modelsResponse] = await Promise.all([
                    fetch('/api/whisper/service', { signal: serviceController.signal }),
                    fetch('/api/whisper/models', { signal: modelsController.signal })
                ]);

                // Clear timeouts on success
                clearTimeout(serviceTimeoutId);
                clearTimeout(modelsTimeoutId);

                console.log('📡 Both responses received');

                if (!serviceResponse.ok) {
                    throw new Error(`Service API HTTP ${serviceResponse.status}: ${serviceResponse.statusText}`);
                }
                if (!modelsResponse.ok) {
                    throw new Error(`Models API HTTP ${modelsResponse.status}: ${modelsResponse.statusText}`);
                }

                const serviceData = await serviceResponse.json();
                const modelsData = await modelsResponse.json();

                // Ensure data has expected structure
                const safeServiceData = serviceData || { enabled: false, model_path: '', status: 'unknown' };
                const safeModelsData = modelsData && modelsData.models ? modelsData : { models: [] };

                updateWhisperServiceDisplay(safeServiceData);
                updateModelList(safeModelsData, safeServiceData.model_path);
            } catch (error) {
                if (error.name === 'AbortError') {
                    console.error('❌ Whisper service requests timed out after 3 seconds');
                } else {
                    console.error('❌ Failed to load whisper service:', error);
                }

                // Show fallback UI for SERVICE STATUS only - keep model list intact
                const statusDiv = document.getElementById('whisperStatus');
                if (statusDiv) {
                    const errorMsg = error.name === 'AbortError' ?
                        '<span class="status-error">● Request Timeout</span>' :
                        '<span class="status-error">● Service Unavailable</span>';
                    statusDiv.innerHTML = errorMsg;
                }

                // Show timeout message for models if needed
                const modelListDiv = document.getElementById('modelList');
                if (modelListDiv && error.name === 'AbortError') {
                    modelListDiv.innerHTML = '<div style="padding: 10px; color: #666;">Request timed out - server may be slow</div>';
                }

                console.log('Keeping existing data during API error');
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

        // Store models data locally to avoid memory leaks
        let cachedModelsData = [];

        function updateModelList(modelsData, currentModelPath) {
            const modelListDiv = document.getElementById('modelList');
            const restartBtn = document.getElementById('restartBtn');

            if (!modelListDiv || !restartBtn) {
                console.error('Model list elements not found');
                return;
            }

            // Only update if we have valid new data, otherwise keep existing
            if (modelsData && modelsData.models && Array.isArray(modelsData.models)) {
                cachedModelsData = modelsData.models;
                console.log('Updated model list with', cachedModelsData.length, 'models');
            } else {
                console.log('Invalid models data, keeping existing list with', cachedModelsData.length, 'models');
            }

            if (cachedModelsData.length === 0) {
                // Only show "No models found" if we've never had models
                if (!modelListDiv.innerHTML || modelListDiv.innerHTML.includes('No models found')) {
                    modelListDiv.innerHTML = '<div style="padding: 10px; color: #666;">No models found</div>';
                }
                if (restartBtn) restartBtn.disabled = true;
                return;
            }

            let html = '';
            cachedModelsData.forEach(model => {
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
            if (restartBtn) {
                restartBtn.disabled = !selectedModel || selectedModel === currentModelPath;
            }
        }

        function selectModel(modelPath) {
            selectedModel = modelPath;
            // Re-render the model list to update highlighting
            updateModelList({ models: cachedModelsData }, currentModel);
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

        // LLaMA Service Functions
        async function loadLlamaService() {
            try {
                const serviceController = new AbortController();
                const modelsController = new AbortController();

                // Set timeouts for both requests
                const serviceTimeoutId = setTimeout(() => serviceController.abort(), 3000);
                const modelsTimeoutId = setTimeout(() => modelsController.abort(), 3000);

                const [serviceResponse, modelsResponse] = await Promise.all([
                    fetch('/api/llama/service', { signal: serviceController.signal }),
                    fetch('/api/llama/models', { signal: modelsController.signal })
                ]);

                // Clear timeouts on success
                clearTimeout(serviceTimeoutId);
                clearTimeout(modelsTimeoutId);

                if (serviceResponse.ok && modelsResponse.ok) {
                    const serviceData = await serviceResponse.json();
                    const modelsData = await modelsResponse.json();

                    // Update service status
                    const statusElement = document.getElementById('llamaStatus');
                    const toggleBtn = document.getElementById('llamaToggleBtn');
                    const restartBtn = document.getElementById('llamaRestartBtn');

                    if (statusElement && toggleBtn) {
                        if (serviceData.enabled && serviceData.status === 'running') {
                            statusElement.textContent = '● Running';
                            statusElement.className = 'status-online';
                            toggleBtn.textContent = 'Stop Service';
                            if (restartBtn) restartBtn.disabled = false;
                        } else if (serviceData.enabled && serviceData.status === 'starting') {
                            statusElement.textContent = '● Starting...';
                            statusElement.className = 'status-warning';
                            toggleBtn.textContent = 'Stop Service';
                            if (restartBtn) restartBtn.disabled = true;
                        } else if (serviceData.enabled && serviceData.status === 'error') {
                            statusElement.textContent = '● Error';
                            statusElement.className = 'status-error';
                            toggleBtn.textContent = 'Start Service';
                            if (restartBtn) restartBtn.disabled = true;
                        } else {
                            statusElement.textContent = '● Stopped';
                            statusElement.className = 'status-offline';
                            toggleBtn.textContent = 'Start Service';
                            if (restartBtn) restartBtn.disabled = true;
                        }
                    }

                    // Update models list
                    const modelListElement = document.getElementById('llamaModelList');
                    if (modelListElement && modelsData.models) {
                        let modelHtml = '';
                        modelsData.models.forEach(model => {
                            const isSelected = model.path === serviceData.model_path;
                            modelHtml += `<div style="padding: 5px; border-bottom: 1px solid #eee; cursor: pointer; ${isSelected ? 'background: #e3f2fd; font-weight: bold;' : ''}"
                                         onclick="selectLlamaModel('${model.path}')"
                                         data-model-path="${model.path}">
                                         ${model.path.split('/').pop()} (${model.size})
                                         ${isSelected ? ' ✓' : ''}
                                      </div>`;
                        });
                        modelListElement.innerHTML = modelHtml || '<div style="padding: 5px; color: #666;">No .gguf models found</div>';
                    }
                } else {
                    console.error('Failed to load LLaMA service data');
                }
            } catch (error) {
                if (error.name === 'AbortError') {
                    console.error('LLaMA service request timed out');
                } else {
                    console.error('Error loading LLaMA service:', error);
                }
            }
        }

        async function selectLlamaModel(modelPath) {
            try {
                const response = await fetch('/api/llama/service', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({ model_path: modelPath })
                });

                const result = await response.json();

                if (response.ok) {
                    console.log('LLaMA model updated:', result);
                    loadLlamaService(); // Refresh display

                    // Enable restart button if service is running
                    const restartBtn = document.getElementById('llamaRestartBtn');
                    const statusElement = document.getElementById('llamaStatus');
                    if (restartBtn && statusElement && statusElement.textContent.includes('Running')) {
                        restartBtn.disabled = false;
                    }
                } else {
                    alert(`Failed to update model: ${result.error}`);
                }
            } catch (error) {
                console.error('Error updating model:', error);
                alert('Failed to update model');
            }
        }

        async function restartLlamaWithSelectedModel() {
            if (!confirm('Restart LLaMA service with the selected model?')) {
                return;
            }

            try {
                const response = await fetch('/api/llama/restart', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    }
                });

                const result = await response.json();

                if (response.ok) {
                    console.log('LLaMA service restarted:', result);
                    loadLlamaService(); // Refresh display
                } else {
                    alert(`Failed to restart service: ${result.error}`);
                }
            } catch (error) {
                console.error('Error restarting service:', error);
                alert('Failed to restart service');
            }
        }

        async function toggleLlamaService() {
            try {
                const response = await fetch('/api/llama/service/toggle', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    }
                });

                const result = await response.json();

                if (response.ok) {
                    console.log('LLaMA service toggled:', result);
                    loadLlamaService(); // Refresh display
                } else {
                    alert(`Failed to toggle llama service: ${result.error}`);
                }
            } catch (error) {
                console.error('Error toggling llama service:', error);
                alert('Failed to toggle llama service');
            }
        }

        // Piper TTS service functions
        async function loadPiperService() {
            try {
                const response = await fetch('/api/piper/service');
                const data = await response.json();

                const statusDiv = document.getElementById('piperStatus');
                const toggleBtn = document.getElementById('piperToggleBtn');
                const restartBtn = document.getElementById('piperRestartBtn');

                if (data.enabled && data.status === 'running') {
                    statusDiv.className = 'status-online';
                    statusDiv.textContent = '● Running';
                    toggleBtn.textContent = 'Stop Service';
                    restartBtn.disabled = false;
                } else if (data.enabled && data.status === 'error') {
                    statusDiv.className = 'status-error';
                    statusDiv.textContent = '● Error';
                    toggleBtn.textContent = 'Start Service';
                    restartBtn.disabled = true;
                } else {
                    statusDiv.className = 'status-offline';
                    statusDiv.textContent = '● Stopped';
                    toggleBtn.textContent = 'Start Service';
                    restartBtn.disabled = true;
                }

                // Load available models
                loadPiperModels();

            } catch (error) {
                console.error('Error loading piper service:', error);
                const statusDiv = document.getElementById('piperStatus');
                statusDiv.className = 'status-error';
                statusDiv.textContent = '● Error';
            }
        }

        async function loadPiperModels() {
            try {
                const response = await fetch('/api/piper/models');
                const data = await response.json();

                const modelList = document.getElementById('piperModelList');
                if (data.models && data.models.length > 0) {
                    modelList.innerHTML = data.models.map(model =>
                        `<div style="padding: 5px; border-bottom: 1px solid #eee; cursor: pointer;"
                              onclick="selectPiperModel('${model.path}')"
                              data-model-path="${model.path}">
                            <strong>${model.name}</strong><br>
                            <small>Size: ${(model.size / (1024*1024)).toFixed(1)} MB</small>
                        </div>`
                    ).join('');
                } else {
                    modelList.innerHTML = '<div style="padding: 10px; color: #666;">No .onnx models found in models/ directory</div>';
                }
            } catch (error) {
                console.error('Error loading piper models:', error);
                document.getElementById('piperModelList').innerHTML = '<div style="padding: 10px; color: #f00;">Error loading models</div>';
            }
        }

        function selectPiperModel(modelPath) {
            // Remove previous selection
            document.querySelectorAll('#piperModelList div').forEach(div => {
                div.style.backgroundColor = '';
                div.style.color = '';
            });

            // Highlight selected model
            const selectedDiv = document.querySelector(`#piperModelList div[data-model-path="${modelPath}"]`);
            if (selectedDiv) {
                selectedDiv.style.backgroundColor = '#007bff';
                selectedDiv.style.color = 'white';
            }

            // Store selected model
            window.selectedPiperModel = modelPath;

            // Enable restart button if service is running
            const restartBtn = document.getElementById('piperRestartBtn');
            const statusDiv = document.getElementById('piperStatus');
            if (statusDiv.textContent.includes('Running')) {
                restartBtn.disabled = false;
            }
        }

        async function restartPiperWithSelectedModel() {
            if (!window.selectedPiperModel) {
                alert('Please select a model first');
                return;
            }

            try {
                // Update model configuration
                const configResponse = await fetch('/api/piper/service', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    },
                    body: JSON.stringify({
                        model_path: window.selectedPiperModel,
                        enabled: true
                    })
                });

                if (!configResponse.ok) {
                    throw new Error('Failed to update configuration');
                }

                const response = await fetch('/api/piper/restart', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    }
                });

                const result = await response.json();

                if (response.ok) {
                    console.log('Piper service restarted:', result);
                    loadPiperService(); // Refresh display
                } else {
                    alert(`Failed to restart piper service: ${result.error}`);
                }
            } catch (error) {
                console.error('Error restarting service:', error);
                alert('Failed to restart service');
            }
        }

        async function togglePiperService() {
            try {
                const response = await fetch('/api/piper/service/toggle', {
                    method: 'POST',
                    headers: {
                        'Content-Type': 'application/json'
                    }
                });

                const result = await response.json();

                if (response.ok) {
                    console.log('Piper service toggled:', result);
                    loadPiperService(); // Refresh display
                } else {
                    alert(`Failed to toggle piper service: ${result.error}`);
                }
            } catch (error) {
                console.error('Error toggling piper service:', error);
                alert('Failed to toggle piper service');
            }
        }

        // Load initial data - use direct call for initial load
        loadWhisperService();
        loadLlamaService();
        loadPiperService();
    </script>
</body>
</html>)HTML";
        return response;
    }

    // Serve JSZip library locally
    if (path == "/jszip.min.js") {
        std::ifstream file("jszip.min.js", std::ios::binary);
        if (file.good()) {
            response.status_code = 200;
            response.status_text = "OK";
            response.headers["Content-Type"] = "application/javascript";

            // Safe file reading - get file size first
            file.seekg(0, std::ios::end);
            size_t file_size = file.tellg();
            file.seekg(0, std::ios::beg);

            // Read file content safely
            response.body.resize(file_size);
            file.read(&response.body[0], file_size);
            file.close();
            return response;
        }
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
    } else if (request.path == "/api/llama/service") {
        if (request.method == "GET") {
            return api_llama_service_get(request);
        } else if (request.method == "POST") {
            return api_llama_service_post(request);
        }
    } else if (request.path == "/api/llama/service/toggle") {
        if (request.method == "POST") {
            return api_llama_service_toggle(request);
        }
    } else if (request.path == "/api/llama/models") {
        if (request.method == "GET") {
            return api_llama_models_get(request);
        }
    } else if (request.path == "/api/llama/restart") {
        if (request.method == "POST") {
            return api_llama_restart(request);
        }
    } else if (request.path == "/api/piper/service") {
        if (request.method == "GET") {
            return api_piper_service_get(request);
        } else if (request.method == "POST") {
            return api_piper_service_post(request);
        }
    } else if (request.path == "/api/piper/service/toggle") {
        if (request.method == "POST") {
            return api_piper_service_toggle(request);
        }
    } else if (request.path == "/api/piper/models") {
        if (request.method == "GET") {
            return api_piper_models_get(request);
        }
    } else if (request.path == "/api/piper/restart") {
        if (request.method == "POST") {
            return api_piper_restart(request);
        }
    } else if (request.path == "/api/upload-stream") {
        if (request.method == "POST") {
            return api_upload_stream(request);
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

    // Safe JSON value extraction
    if (!request.body.empty()) {
        const std::string& body = request.body;

        // Safe helper function to extract JSON string value
        auto extract_json_string = [](const std::string& body, const std::string& key) -> std::string {
            try {
                std::string search = "\"" + key + "\":\"";
                size_t start = body.find(search);
                if (start != std::string::npos) {
                    start += search.length();
                    size_t end = body.find("\"", start);
                    if (end != std::string::npos && end > start) {
                        return body.substr(start, end - start);
                    }
                }
            } catch (const std::exception& e) {
                std::cout << "❌ Error extracting JSON string for key '" << key << "': " << e.what() << std::endl;
            }
            return "";
        };

        // Safe helper function to extract JSON number value
        auto extract_json_number = [](const std::string& body, const std::string& key) -> int {
            try {
                std::string search = "\"" + key + "\":";
                size_t start = body.find(search);
                if (start != std::string::npos) {
                    start += search.length();
                    size_t end = body.find_first_of(",}", start);
                    if (end != std::string::npos && end > start) {
                        std::string num_str = body.substr(start, end - start);
                        return std::atoi(num_str.c_str());
                    }
                }
            } catch (const std::exception& e) {
                std::cout << "❌ Error extracting JSON number for key '" << key << "': " << e.what() << std::endl;
            }
            return 0;
        };

        // Extract all values safely
        server_ip = extract_json_string(body, "server_ip");
        username = extract_json_string(body, "username");
        password = extract_json_string(body, "password");

        int port = extract_json_number(body, "server_port");
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

    bool enabled = false;
    std::string model_path = "models/ggml-small.en.bin";
    std::string status = "unknown";

    try {
        enabled = database_->get_whisper_service_enabled();
        model_path = database_->get_whisper_model_path();
        status = database_->get_whisper_service_status();
    } catch (const std::exception& e) {
        std::cout << "❌ Database error in api_whisper_service_get: " << e.what() << std::endl;
        // Use default values set above
    } catch (...) {
        std::cout << "❌ Unknown database error in api_whisper_service_get" << std::endl;
        // Use default values set above
    }

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
            std::cout << "🎤 Whisper service enabled - starting service..." << std::endl;

            // Get the configured model path from database
            std::string model_path;
            try {
                model_path = database_->get_whisper_model_path();
                std::cout << "🎤 Got model path: " << model_path << std::endl;
            } catch (const std::exception& e) {
                std::cout << "❌ Database error getting model path: " << e.what() << std::endl;
                database_->set_whisper_service_status("error");
                response.status_code = 500;
                response.status_text = "Internal Server Error";
                response.body = R"({"error": "Database error"})";
                return response;
            }

            if (model_path.empty()) {
                std::cout << "❌ No model configured - cannot start service" << std::endl;
                database_->set_whisper_service_status("error");
                response.status_code = 400;
                response.status_text = "Bad Request";
                response.body = R"({"error": "No model configured. Please select a model first."})";
                return response;
            }

            // Validate model file exists
            struct stat file_stat;
            if (stat(model_path.c_str(), &file_stat) != 0) {
                std::cout << "❌ Model file not found: " << model_path << std::endl;
                database_->set_whisper_service_status("error");
                response.status_code = 404;
                response.status_text = "Not Found";
                response.body = R"({"error": "Configured model file not found"})";
                return response;
            }

            // Kill existing whisper service processes
            std::cout << "🎤 Stopping existing whisper service..." << std::endl;
            system("pkill -TERM -f whisper-service");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            // Start new whisper service with configured model
            std::string start_command = "./whisper-service --model \"" + model_path + "\" &";
            std::cout << "🎤 Starting whisper service: " << start_command << std::endl;
            int start_result = system(start_command.c_str());

            if (start_result == 0) {
                std::cout << "✅ Whisper service started successfully" << std::endl;
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
            std::cout << "🎤 Whisper service disabled - stopping service..." << std::endl;

            // Kill existing whisper service processes
            system("pkill -TERM -f whisper-service");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            database_->set_whisper_service_status("stopped");
        }

    } else {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Failed to toggle whisper service"})";
    }

    return response;
}

// Legacy chunked upload function removed - replaced by streaming approach

HttpResponse SimpleHttpServer::api_upload_stream(const HttpRequest& request) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";
    response.headers["Access-Control-Allow-Origin"] = "*";

    // Get filename from header
    std::string filename;
    auto filename_it = request.headers.find("X-File-Name");
    if (filename_it != request.headers.end()) {
        filename = filename_it->second;
    } else {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Missing X-File-Name header"})";
        return response;
    }

    std::cout << "📤 STREAM: Receiving file: " << filename << " (" << request.body.length() << " bytes)" << std::endl;

    // Validate filename for security - Allow forward slashes for directory paths
    if (filename.find("..") != std::string::npos ||
        filename.find("\\") != std::string::npos) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Invalid filename"})";
        return response;
    }

    // Extract just the filename part for validation (handle directory paths)
    std::string basename = filename;
    size_t last_slash = filename.find_last_of('/');
    if (last_slash != std::string::npos) {
        basename = filename.substr(last_slash + 1);
    }

    // Validate file type using basename
    bool is_bin = basename.length() >= 4 && basename.substr(basename.length() - 4) == ".bin";
    bool is_mlmodelc = basename.length() >= 9 && basename.substr(basename.length() - 9) == ".mlmodelc";
    bool is_mlmodelc_zip = basename.length() >= 13 && basename.substr(basename.length() - 13) == ".mlmodelc.zip";
    bool is_json = basename.length() >= 5 && basename.substr(basename.length() - 5) == ".json";
    bool is_mil = basename.length() >= 4 && basename.substr(basename.length() - 4) == ".mil";

    if (!is_bin && !is_mlmodelc && !is_mlmodelc_zip && !is_json && !is_mil) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Invalid file type"})";
        return response;
    }

    // Create directory structure if needed
    std::string filepath = "models/" + filename;
    if (last_slash != std::string::npos) {
        std::string dir_path = "models/" + filename.substr(0, last_slash);
        std::string mkdir_cmd = "mkdir -p \"" + dir_path + "\"";
        system(mkdir_cmd.c_str());
    }

    // Write file directly to disk
    std::ofstream file(filepath, std::ios::binary);
    if (!file) {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Failed to create file"})";
        return response;
    }

    file.write(request.body.c_str(), request.body.length());
    file.close();

    std::cout << "✅ STREAM: File saved: " << filepath << std::endl;

    response.status_code = 200;
    response.status_text = "OK";
    response.body = R"({"status": "success", "message": "File uploaded successfully"})";
    return response;
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

    // FIXED: Safe JSON parsing to extract filenames - prevent infinite loop
    std::cout << "📋 Checking files request body: " << request.body << std::endl;

    // Check for empty request body
    if (request.body.empty()) {
        std::cout << "📋 Empty request body - returning empty file list" << std::endl;
        response.status_code = 200;
        response.status_text = "OK";
        response.body = "{\"existing_files\": []}";
        return response;
    }

    // Look for filenames array in JSON
    size_t filenames_pos = request.body.find("\"filenames\"");
    if (filenames_pos != std::string::npos) {
        size_t array_start = request.body.find("[", filenames_pos);
        size_t array_end = request.body.find("]", array_start);

        if (array_start != std::string::npos && array_end != std::string::npos) {
            std::string filenames_array = request.body.substr(array_start + 1, array_end - array_start - 1);
            std::cout << "📋 Extracted filenames array: " << filenames_array << std::endl;

            // Simple extraction of quoted strings
            size_t pos = 0;
            int safety_counter = 0; // Prevent infinite loops
            while (pos < filenames_array.length() && safety_counter < 100) {
                safety_counter++;

                size_t quote_start = filenames_array.find("\"", pos);
                if (quote_start == std::string::npos) break;

                size_t quote_end = filenames_array.find("\"", quote_start + 1);
                if (quote_end == std::string::npos) break;

                std::string filename = filenames_array.substr(quote_start + 1, quote_end - quote_start - 1);
                std::cout << "📋 Checking filename: " << filename << std::endl;

                // Check if file exists in models directory
                std::string filepath = "models/" + filename;
                struct stat file_stat;
                if (stat(filepath.c_str(), &file_stat) == 0) {
                    std::cout << "✅ File exists: " << filepath << std::endl;
                    if (!first) json_response += ", ";
                    json_response += "\"" + filename + "\"";
                    first = false;
                } else {
                    std::cout << "❌ File not found: " << filepath << std::endl;
                }

                pos = quote_end + 1;
            }
        }
    } else {
        // No filenames found in JSON - return empty list
        std::cout << "📋 No filenames found in request body" << std::endl;
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

    // Scan the models directory for model files (simple top-level scan)
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

        // Add to JSON response with safer string building
        for (const auto& model_path : model_files) {
            if (!first) {
                json_response += ",";
            }
            json_response += "{\"path\": \"";
            json_response += model_path;
            json_response += "\"}";
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

// LLaMA service management endpoints
HttpResponse SimpleHttpServer::api_llama_service_get(const HttpRequest& request) {
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

    bool enabled = false;
    std::string model_path = "models/llama-7b-q4_0.gguf";
    std::string status = "unknown";

    try {
        enabled = database_->get_llama_service_enabled();
        model_path = database_->get_llama_model_path();
        status = database_->get_llama_service_status();
    } catch (const std::exception& e) {
        std::cout << "❌ Database error in api_llama_service_get: " << e.what() << std::endl;
        // Use default values set above
    } catch (...) {
        std::cout << "❌ Unknown database error in api_llama_service_get" << std::endl;
        // Use default values set above
    }

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

HttpResponse SimpleHttpServer::api_llama_service_post(const HttpRequest& request) {
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

    bool success = database_->set_llama_model_path(model_path);

    if (success) {
        response.status_code = 200;
        response.status_text = "OK";
        response.body = "{\"success\": true, \"model_path\": \"" + model_path + "\"}";
    } else {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Failed to update model path"})";
    }

    return response;
}

HttpResponse SimpleHttpServer::api_llama_service_toggle(const HttpRequest& request) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";

    if (!database_) {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Database not available"})";
        return response;
    }

    // Check for specific enable/disable action
    bool current_enabled = database_->get_llama_service_enabled();
    bool new_enabled = !current_enabled;

    auto it = request.query_params.find("enable");
    if (it != request.query_params.end()) {
        new_enabled = (it->second == "true");
    }

    bool success = database_->set_llama_service_enabled(new_enabled);

    if (success) {
        // Update status based on enabled state
        std::string new_status = new_enabled ? "starting" : "stopped";
        database_->set_llama_service_status(new_status);

        response.status_code = 200;
        response.status_text = "OK";
        response.body = "{\"success\": true, \"enabled\": " +
                       std::string(new_enabled ? "true" : "false") +
                       ", \"status\": \"" + new_status + "\"}";

        if (new_enabled) {
            std::cout << "🦙 LLaMA service enabled - starting service..." << std::endl;

            // Get the configured model path from database
            std::string model_path;
            try {
                model_path = database_->get_llama_model_path();
                std::cout << "🦙 Got model path: " << model_path << std::endl;
            } catch (const std::exception& e) {
                std::cout << "❌ Database error getting model path: " << e.what() << std::endl;
                database_->set_llama_service_status("error");
                response.status_code = 500;
                response.status_text = "Internal Server Error";
                response.body = R"({"error": "Database error"})";
                return response;
            }

            if (model_path.empty()) {
                std::cout << "❌ No model configured - cannot start service" << std::endl;
                database_->set_llama_service_status("error");
                response.status_code = 400;
                response.status_text = "Bad Request";
                response.body = R"({"error": "No model configured. Please select a model first."})";
                return response;
            }

            // Validate model file exists
            struct stat file_stat;
            if (stat(model_path.c_str(), &file_stat) != 0) {
                std::cout << "❌ Model file not found: " << model_path << std::endl;
                database_->set_llama_service_status("error");
                response.status_code = 404;
                response.status_text = "Not Found";
                response.body = R"({"error": "Configured model file not found"})";
                return response;
            }

            // Kill existing llama service processes
            std::cout << "🦙 Stopping existing llama service..." << std::endl;
            system("pkill -TERM -f llama-service");
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));

            // Start new llama service with configured model
            std::string start_command = "./llama-service -m \"" + model_path + "\" -d whisper_talk.db -p 8083 --out-host 127.0.0.1 --out-port 8090 &";
            std::cout << "🦙 Starting llama service: " << start_command << std::endl;
            int start_result = system(start_command.c_str());

            if (start_result == 0) {
                std::cout << "✅ LLaMA service started successfully" << std::endl;
                database_->set_llama_service_status("running");
            } else {
                std::cout << "❌ Failed to start llama service" << std::endl;
                database_->set_llama_service_status("error");
                response.status_code = 500;
                response.status_text = "Internal Server Error";
                response.body = R"({"error": "Failed to start llama service"})";
                return response;
            }

        } else {
            std::cout << "🦙 LLaMA service disabled - stopping service..." << std::endl;

            // Kill existing llama service processes
            system("pkill -TERM -f llama-service");
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            database_->set_llama_service_status("stopped");
        }

    } else {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Failed to toggle llama service"})";
    }

    return response;
}

HttpResponse SimpleHttpServer::api_llama_models_get(const HttpRequest& request) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";

    // Check for custom models directory
    std::string models_dir = "models";
    auto dir_it = request.query_params.find("dir");
    if (dir_it != request.query_params.end()) {
        models_dir = dir_it->second;
    }

    // Check for file type filter (default: gguf)
    std::string file_type = "gguf";
    auto type_it = request.query_params.find("type");
    if (type_it != request.query_params.end()) {
        file_type = type_it->second;
    }

    // Scan for model files in directory
    std::vector<std::string> model_files;
    std::string extension = "." + file_type;

    DIR* dir = opendir(models_dir.c_str());
    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string filename = entry->d_name;
            if (filename.length() > extension.length() &&
                filename.substr(filename.length() - extension.length()) == extension) {
                model_files.push_back(models_dir + "/" + filename);
            }
        }
        closedir(dir);
    }

    // Sort model files
    std::sort(model_files.begin(), model_files.end());

    response.status_code = 200;
    response.status_text = "OK";

    std::string body = "{\"models\": [";
    for (size_t i = 0; i < model_files.size(); i++) {
        if (i > 0) body += ", ";

        // Get file size
        struct stat file_stat;
        std::string size_str = "unknown";
        if (stat(model_files[i].c_str(), &file_stat) == 0) {
            double size_mb = file_stat.st_size / (1024.0 * 1024.0);
            if (size_mb < 1024) {
                size_str = std::to_string((int)size_mb) + " MB";
            } else {
                size_str = std::to_string((int)(size_mb / 1024.0)) + " GB";
            }
        }

        body += "{\"path\": \"" + model_files[i] + "\", \"size\": \"" + size_str + "\"}";
    }
    body += "]}";

    response.body = body;
    return response;
}

HttpResponse SimpleHttpServer::api_llama_restart(const HttpRequest& request) {
    HttpResponse response;
    response.headers["Content-Type"] = "application/json";

    if (!database_) {
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Database not available"})";
        return response;
    }

    // Parse JSON body for restart options
    std::string custom_model_path;
    if (!request.body.empty()) {
        size_t model_pos = request.body.find("\"model_path\":");
        if (model_pos != std::string::npos) {
            size_t start = request.body.find("\"", model_pos + 13);
            if (start != std::string::npos) {
                start++; // Skip opening quote
                size_t end = request.body.find("\"", start);
                if (end != std::string::npos) {
                    custom_model_path = request.body.substr(start, end - start);
                }
            }
        }
    }

    // Check if service is enabled
    bool enabled = database_->get_llama_service_enabled();
    if (!enabled) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "LLaMA service is not enabled"})";
        return response;
    }

    std::cout << "🦙 Restarting LLaMA service..." << std::endl;
    database_->set_llama_service_status("restarting");

    // Kill existing processes
    system("pkill -TERM -f llama-service");
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Get model path (use custom if provided, otherwise database default)
    std::string model_path = custom_model_path.empty() ? database_->get_llama_model_path() : custom_model_path;
    if (model_path.empty()) {
        database_->set_llama_service_status("error");
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "No model configured"})";
        return response;
    }

    // Update database with new model path if custom one was provided
    if (!custom_model_path.empty()) {
        database_->set_llama_model_path(custom_model_path);
    }

    std::string start_command = "./llama-service -m \"" + model_path + "\" -d whisper_talk.db -p 8083 --out-host 127.0.0.1 --out-port 8090 &";
    int start_result = system(start_command.c_str());

    if (start_result == 0) {
        std::cout << "✅ LLaMA service restarted successfully" << std::endl;
        database_->set_llama_service_status("running");
        response.status_code = 200;
        response.status_text = "OK";
        response.body = R"({"success": true, "message": "LLaMA service restarted"})";
    } else {
        std::cout << "❌ Failed to restart llama service" << std::endl;
        database_->set_llama_service_status("error");
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Failed to restart llama service"})";
    }

    return response;
}

// Piper service API endpoints
HttpResponse SimpleHttpServer::api_piper_service_get(const HttpRequest& request) {
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

    // Check for detailed info request
    bool include_stats = false;
    auto it = request.query_params.find("stats");
    if (it != request.query_params.end() && it->second == "true") {
        include_stats = true;
    }

    bool enabled = database_->get_piper_service_enabled();
    std::string model_path = database_->get_piper_model_path();
    std::string espeak_data_path = database_->get_piper_espeak_data_path();
    std::string status = database_->get_piper_service_status();

    std::ostringstream json;
    json << "{"
         << "\"enabled\": " << (enabled ? "true" : "false") << ","
         << "\"model_path\": \"" << model_path << "\","
         << "\"espeak_data_path\": \"" << espeak_data_path << "\","
         << "\"status\": \"" << status << "\"";

    // Add stats if requested
    if (include_stats) {
        json << ","
             << "\"process_info\": {"
             << "\"pid\": \"" << (system("pgrep -f piper-service") == 0 ? "running" : "stopped") << "\","
             << "\"uptime\": \"unknown\","
             << "\"memory_usage\": \"unknown\""
             << "}";
    }

    json << "}";
    response.body = json.str();
    return response;
}

HttpResponse SimpleHttpServer::api_piper_service_post(const HttpRequest& request) {
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

    // Parse JSON body
    try {
        size_t enabled_pos = request.body.find("\"enabled\":");
        size_t model_pos = request.body.find("\"model_path\":");
        size_t espeak_pos = request.body.find("\"espeak_data_path\":");

        if (enabled_pos != std::string::npos) {
            size_t value_start = request.body.find(":", enabled_pos) + 1;
            size_t value_end = request.body.find_first_of(",}", value_start);
            std::string value = request.body.substr(value_start, value_end - value_start);

            // Remove whitespace and quotes
            value.erase(0, value.find_first_not_of(" \t\""));
            value.erase(value.find_last_not_of(" \t\"") + 1);

            bool enabled = (value == "true");
            database_->set_piper_service_enabled(enabled);
        }

        if (model_pos != std::string::npos) {
            size_t value_start = request.body.find("\"", model_pos + 13) + 1;
            size_t value_end = request.body.find("\"", value_start);
            std::string model_path = request.body.substr(value_start, value_end - value_start);
            database_->set_piper_model_path(model_path);
        }

        if (espeak_pos != std::string::npos) {
            size_t value_start = request.body.find("\"", espeak_pos + 19) + 1;
            size_t value_end = request.body.find("\"", value_start);
            std::string espeak_data_path = request.body.substr(value_start, value_end - value_start);
            database_->set_piper_espeak_data_path(espeak_data_path);
        }

        response.body = R"({"success": true, "message": "Piper service configuration updated"})";
    } catch (const std::exception& e) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "Invalid JSON format"})";
    }

    return response;
}

HttpResponse SimpleHttpServer::api_piper_service_toggle(const HttpRequest& request) {
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

    // Check for specific enable/disable action
    bool current_enabled = database_->get_piper_service_enabled();
    bool new_enabled = !current_enabled;

    auto it = request.query_params.find("enable");
    if (it != request.query_params.end()) {
        new_enabled = (it->second == "true");
    }

    database_->set_piper_service_enabled(new_enabled);

    std::string action = new_enabled ? "enabled" : "disabled";
    std::string status = new_enabled ? "running" : "stopped";

    database_->set_piper_service_status(status);

    if (new_enabled) {
        // Start Piper service
        std::string model_path = database_->get_piper_model_path();
        std::string espeak_data_path = database_->get_piper_espeak_data_path();

        // Kill any existing piper service
        system("pkill -f piper-service");
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));

        // Start new piper service with configured model
        std::string start_command = "./piper-service -m \"" + model_path + "\" -e \"" + espeak_data_path + "\" -d whisper_talk.db -p 8090 --out-host 127.0.0.1 --out-port 8091 &";
        std::cout << "🎤 Starting piper service: " << start_command << std::endl;
        int start_result = system(start_command.c_str());

        if (start_result == 0) {
            database_->set_piper_service_status("running");
            response.body = R"({"success": true, "message": "Piper service started", "enabled": true})";
        } else {
            database_->set_piper_service_enabled(false);
            database_->set_piper_service_status("error");
            response.status_code = 500;
            response.status_text = "Internal Server Error";
            response.body = R"({"error": "Failed to start Piper service"})";
        }
    } else {
        // Stop Piper service
        system("pkill -f piper-service");
        database_->set_piper_service_status("stopped");
        response.body = R"({"success": true, "message": "Piper service stopped", "enabled": false})";
    }

    return response;
}

HttpResponse SimpleHttpServer::api_piper_models_get(const HttpRequest& request) {
    HttpResponse response;
    response.status_code = 200;
    response.status_text = "OK";
    response.headers["Content-Type"] = "application/json";

    // Check for custom models directory
    std::string models_dir = "models";
    auto dir_it = request.query_params.find("dir");
    if (dir_it != request.query_params.end()) {
        models_dir = dir_it->second;
    }

    // Check for file type filter (default: onnx)
    std::string file_type = "onnx";
    auto type_it = request.query_params.find("type");
    if (type_it != request.query_params.end()) {
        file_type = type_it->second;
    }

    std::ostringstream json;
    json << "{\"models\": [";

    // Scan for model files in directory
    std::string extension = "." + file_type;
    DIR* dir = opendir(models_dir.c_str());
    bool first = true;

    if (dir) {
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string filename = entry->d_name;
            if (filename.length() > extension.length() &&
                filename.substr(filename.length() - extension.length()) == extension) {
                if (!first) json << ",";
                first = false;

                std::string full_path = models_dir + "/" + filename;
                struct stat file_stat;
                long file_size = 0;
                if (stat(full_path.c_str(), &file_stat) == 0) {
                    file_size = file_stat.st_size;
                }

                json << "{"
                     << "\"name\": \"" << filename << "\","
                     << "\"path\": \"" << full_path << "\","
                     << "\"size\": " << file_size
                     << "}";
            }
        }
        closedir(dir);
    }

    json << "]}";
    response.body = json.str();
    return response;
}

HttpResponse SimpleHttpServer::api_piper_restart(const HttpRequest& request) {
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

    // Parse JSON body for restart options
    std::string custom_model_path;
    std::string custom_espeak_path;
    if (!request.body.empty()) {
        size_t model_pos = request.body.find("\"model_path\":");
        if (model_pos != std::string::npos) {
            size_t start = request.body.find("\"", model_pos + 13);
            if (start != std::string::npos) {
                start++; // Skip opening quote
                size_t end = request.body.find("\"", start);
                if (end != std::string::npos) {
                    custom_model_path = request.body.substr(start, end - start);
                }
            }
        }

        size_t espeak_pos = request.body.find("\"espeak_data_path\":");
        if (espeak_pos != std::string::npos) {
            size_t start = request.body.find("\"", espeak_pos + 19);
            if (start != std::string::npos) {
                start++; // Skip opening quote
                size_t end = request.body.find("\"", start);
                if (end != std::string::npos) {
                    custom_espeak_path = request.body.substr(start, end - start);
                }
            }
        }
    }

    // Kill existing piper service
    system("pkill -f piper-service");
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    // Check if service should be running
    if (!database_->get_piper_service_enabled()) {
        database_->set_piper_service_status("stopped");
        response.body = R"({"success": true, "message": "Piper service is disabled"})";
        return response;
    }

    // Get paths (use custom if provided, otherwise database defaults)
    std::string model_path = custom_model_path.empty() ? database_->get_piper_model_path() : custom_model_path;
    std::string espeak_data_path = custom_espeak_path.empty() ? database_->get_piper_espeak_data_path() : custom_espeak_path;

    // Update database with new paths if custom ones were provided
    if (!custom_model_path.empty()) {
        database_->set_piper_model_path(custom_model_path);
    }
    if (!custom_espeak_path.empty()) {
        database_->set_piper_espeak_data_path(custom_espeak_path);
    }

    if (model_path.empty()) {
        response.status_code = 400;
        response.status_text = "Bad Request";
        response.body = R"({"error": "No model configured"})";
        return response;
    }

    std::string start_command = "./piper-service -m \"" + model_path + "\" -e \"" + espeak_data_path + "\" -d whisper_talk.db -p 8090 --out-host 127.0.0.1 --out-port 8091 &";
    int start_result = system(start_command.c_str());

    if (start_result == 0) {
        database_->set_piper_service_status("running");
        response.body = R"({"success": true, "message": "Piper service restarted"})";
    } else {
        database_->set_piper_service_status("error");
        response.status_code = 500;
        response.status_text = "Internal Server Error";
        response.body = R"({"error": "Failed to restart Piper service"})";
    }

    return response;
}

// Legacy chunked upload handler removed - replaced by streaming approach
