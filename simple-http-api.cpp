#include "simple-http-api.h"
#include "database.h"
#include <iostream>
#include <sstream>
#include <fstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>

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
    char buffer[4096];
    ssize_t bytes_read = recv(client_socket, buffer, sizeof(buffer) - 1, 0);
    
    if (bytes_read <= 0) {
        close(client_socket);
        return;
    }
    
    buffer[bytes_read] = '\0';
    std::string raw_request(buffer);
    
    HttpRequest request = parse_request(raw_request);
    HttpResponse response = handle_request(request);
    std::string response_str = create_response(response);
    
    send(client_socket, response_str.c_str(), response_str.length(), 0);
    close(client_socket);
}

HttpRequest SimpleHttpServer::parse_request(const std::string& raw_request) {
    HttpRequest request;
    std::istringstream stream(raw_request);
    std::string line;
    
    // Parse request line
    if (std::getline(stream, line)) {
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
        size_t colon_pos = line.find(':');
        if (colon_pos != std::string::npos) {
            std::string key = line.substr(0, colon_pos);
            std::string value = line.substr(colon_pos + 2);
            if (!value.empty() && value.back() == '\r') {
                value.pop_back();
            }
            request.headers[key] = value;
        }
    }
    
    // Parse body (if any)
    std::string body_line;
    while (std::getline(stream, body_line)) {
        request.body += body_line + "\n";
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
    // Only log non-polling API requests to reduce spam
    if (request.path != "/api/status" &&
        !(request.method == "GET" && request.path == "/api/sip-lines")) {
        std::cout << request.method << " " << request.path << std::endl;
    }

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
                    <div class="form-row">
                        <div class="form-group">
                            <label for="extension">Extension:</label>
                            <input type="text" id="extension" name="extension" placeholder="e.g. 1002">
                        </div>
                        <div class="form-group">
                            <label for="displayName">Display Name:</label>
                            <input type="text" id="displayName" name="displayName" placeholder="AI Assistant Line">
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
            <h2>API Endpoints</h2>
            <ul>
                <li><a href="/api/status">/api/status</a> - System status</li>
                <li><a href="/api/callers">/api/callers</a> - Caller list</li>
                <li><a href="/api/sip-lines">/api/sip-lines</a> - SIP lines</li>
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
            const extension = document.getElementById('extension').value;
            const displayName = document.getElementById('displayName').value;

            console.log('Form values:', {
                serverIp, serverPort, username, password, extension, displayName
            });

            if (!username) {
                alert('Username is required!');
                return;
            }

            const sipLineData = {
                server_ip: serverIp || '192.168.1.100',
                server_port: parseInt(serverPort) || 5060,
                username: username,
                password: password,
                extension: extension || username,
                display_name: displayName || `AI Assistant ${username}`
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
                document.getElementById('extension').value = '';
                document.getElementById('displayName').value = '';
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
        setInterval(loadSipLines, 10000); // Refresh SIP lines every 10 seconds

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
                if (line.status === 'connected') {
                    statusClass = 'status-online';
                } else if (line.status === 'connecting') {
                    statusClass = 'status-warning';
                } else if (line.status === 'error') {
                    statusClass = 'status-error';
                } else if (line.status === 'disabled') {
                    statusClass = 'status-disabled';
                } else {
                    statusClass = 'status-offline'; // disconnected, unknown, etc.
                }

                const hasPassword = line.password && line.password.length > 0;
                html += `
                    <div class="status-item">
                        <h4>Line ${line.line_id}: ${line.extension}</h4>
                        <p><strong>Server:</strong> ${line.server_ip}:${line.server_port}</p>
                        <p><strong>Username:</strong> ${line.username}</p>
                        <p><strong>Password:</strong> ${hasPassword ? '●●●●●●' : 'Not set'}</p>
                        <p><strong>Display:</strong> ${line.display_name}</p>
                        <div class="${statusClass}">● ${line.status}</div>
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
    // Only log non-polling API requests to reduce spam
    if (request.path != "/api/status" &&
        !(request.method == "GET" && request.path == "/api/sip-lines")) {
        std::cout << "API Request: " << request.method << " " << request.path << std::endl;
    }

    if (request.path == "/api/status") {
        return api_status(request);
    } else if (request.path == "/api/callers") {
        return api_callers(request);
    } else if (request.path.length() > 15 && request.path.substr(0, 15) == "/api/sip-lines/" && request.method == "DELETE") {
        // Extract line_id from path like /api/sip-lines/1
        std::cout << "Matched DELETE sip-lines endpoint: " << request.path << std::endl;
        std::string path_suffix = request.path.substr(15); // Remove "/api/sip-lines/"
        int line_id = std::atoi(path_suffix.c_str());
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
            std::cout << "Extracted line_id for toggle: " << line_id << std::endl;
            return api_sip_lines_toggle(request, line_id);
        }
    } else if (request.path == "/api/sip-lines") {
        if (request.method == "GET") {
            return api_sip_lines(request);
        } else if (request.method == "POST") {
            return api_sip_lines_post(request);
        }
    } else if (request.path == "/api/system/speed") {
        std::cout << "Matched system speed endpoint" << std::endl;
        if (request.method == "GET") {
            return api_system_speed_get(request);
        } else if (request.method == "POST") {
            return api_system_speed_post(request);
        }
    // Session and Whisper endpoints removed
    }
    // Note: /api/whisper/transcribe removed - using direct interface now

    // Debug: log unmatched requests
    std::cout << "UNMATCHED API request: " << request.method << " " << request.path << std::endl;
    std::cout << "Path length: " << request.path.length() << std::endl;
    if (request.path.length() >= 15) {
        std::cout << "First 15 chars: '" << request.path.substr(0, 15) << "'" << std::endl;
    }

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
    response.body = R"({
        "status": "online",
        "modules": {
            "http_server": "online",
            "database": "online",
            "sip_client": "offline",
            "whisper": "offline",
            "llama": "offline",
            "piper": "offline"
        }
    })";
    response.headers["Content-Type"] = "application/json";
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
             << "\"extension\":\"" << line.extension << "\","
             << "\"username\":\"" << line.username << "\","
             << "\"password\":\"" << line.password << "\","
             << "\"server_ip\":\"" << line.server_ip << "\","
             << "\"server_port\":" << line.server_port << ","
             << "\"display_name\":\"" << line.display_name << "\","
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
    std::string extension = "";
    std::string display_name = "";

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
        extension = extract_json_string("extension");
        display_name = extract_json_string("display_name");

        int port = extract_json_number("server_port");
        if (port > 0) server_port = port;

        // Debug output
        std::cout << "Extracted values:" << std::endl;
        std::cout << "  server_ip: '" << server_ip << "'" << std::endl;
        std::cout << "  server_port: " << server_port << std::endl;
        std::cout << "  username: '" << username << "'" << std::endl;
        std::cout << "  password: '" << password << "'" << std::endl;
        std::cout << "  extension: '" << extension << "'" << std::endl;
        std::cout << "  display_name: '" << display_name << "'" << std::endl;
    }

    // Create new SIP line in database
    std::string final_extension = extension.empty() ? username : extension;
    std::string final_display_name = display_name.empty() ? ("AI Assistant " + username) : display_name;

    int line_id = database_->create_sip_line(final_extension, username, password,
                                           server_ip, server_port, final_display_name);

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
                line_info = "Line " + std::to_string(line.line_id) + " (" + line.extension + " @ " + line.server_ip + ":" + std::to_string(line.server_port) + ")";
                break;
            }
        }

        if (line_enabled) {
            // Line was enabled - start SIP client for this line
            std::cout << "🚀 Starting SIP client for enabled " << line_info << std::endl;

            // Start SIP client in background with specific line ID
            std::string command = "/Users/whisper/Documents/augment-projects/clean-repo/whisper-sip-client --line-id " + std::to_string(line_id) + " &";
            int result = system(command.c_str());

            if (result == 0) {
                std::cout << "✅ SIP client started successfully for " << line_info << std::endl;
                response.body = R"({"success": true, "message": "SIP line enabled and client started"})";
            } else {
                std::cout << "⚠️ SIP client start failed for " << line_info << std::endl;
                response.body = R"({"success": true, "message": "SIP line enabled but client start failed"})";
            }
        } else {
            // Line was disabled - stop SIP client for this line
            std::cout << "🛑 SIP line disabled: " << line_info << std::endl;

            // Kill any existing SIP client processes for this line
            std::string kill_command = "pkill -f 'whisper-sip-client.*--line-id " + std::to_string(line_id) + "'";
            system(kill_command.c_str());

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
