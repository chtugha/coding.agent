#pragma once
#include <string>

inline void FrontendServer::log_receiver_loop() {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cerr << "Failed to create log socket\n";
        return;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(log_port_);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind log socket to port " << log_port_ << "\n";
        close(sock);
        return;
    }

    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buffer[UDP_BUFFER_SIZE];
    while (!s_sigint_received) {
        ssize_t n = recv(sock, buffer, sizeof(buffer) - 1, 0);
        if (n > 0) {
            buffer[n] = '\0';
            process_log_message(std::string(buffer, n));
        }
    }

    close(sock);
}

inline void FrontendServer::process_log_message(const std::string& msg) {
    if (msg.empty()) return;

    size_t p1 = msg.find(' ');
    size_t p2 = (p1 != std::string::npos) ? msg.find(' ', p1 + 1) : std::string::npos;
    size_t p3 = (p2 != std::string::npos) ? msg.find(' ', p2 + 1) : std::string::npos;

    if (p1 == std::string::npos || p2 == std::string::npos || p3 == std::string::npos) {
        return;
    }

    LogEntry entry;

    time_t now = time(nullptr);
    char timebuf[64];
    struct tm tm_buf;
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", localtime_r(&now, &tm_buf));
    entry.timestamp = timebuf;

    entry.service = msg.substr(0, p1);
    entry.level = msg.substr(p1 + 1, p2 - p1 - 1);
    try {
        entry.call_id = static_cast<uint32_t>(std::stoul(msg.substr(p2 + 1, p3 - p2 - 1)));
    } catch (const std::exception&) {
        entry.call_id = 0;
    }
    entry.message = msg.substr(p3 + 1);

    {
        std::lock_guard<std::mutex> lock(logs_mutex_);
        entry.seq = ++log_seq_;
        if (recent_logs_.size() >= MAX_RECENT_LOGS) {
            recent_logs_.pop_front();
        }
        recent_logs_.push_back(entry);
    }

    enqueue_log(entry);

    {
        std::lock_guard<std::mutex> lock(sse_queue_mutex_);
        sse_queue_.push_back(entry);
    }
}

inline void FrontendServer::enqueue_log(const LogEntry& entry) {
    std::lock_guard<std::mutex> lock(log_queue_mutex_);
    log_queue_.push_back(entry);
}

inline void FrontendServer::flush_log_queue() {
    std::vector<LogEntry> batch;
    {
        std::lock_guard<std::mutex> lock(log_queue_mutex_);
        if (log_queue_.empty()) return;
        batch.swap(log_queue_);
    }

    if (!db_) return;

    if (sqlite3_exec(db_, "BEGIN", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "flush_log_queue: BEGIN failed: " << sqlite3_errmsg(db_) << "\n";
        return;
    }
    const char* sql = "INSERT INTO logs (timestamp, service, call_id, level, message) VALUES (?, ?, ?, ?, ?)";
    sqlite3_stmt* stmt;
    if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
        std::cerr << "flush_log_queue: prepare failed: " << sqlite3_errmsg(db_) << "\n";
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        return;
    }
    for (const auto& entry : batch) {
        sqlite3_bind_text(stmt, 1, entry.timestamp.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 2, entry.service.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(stmt, 3, static_cast<int>(entry.call_id));
        sqlite3_bind_text(stmt, 4, entry.level.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 5, entry.message.c_str(), -1, SQLITE_TRANSIENT);
        if (sqlite3_step(stmt) != SQLITE_DONE) {
            std::cerr << "flush_log_queue: insert failed: " << sqlite3_errmsg(db_) << "\n";
        }
        sqlite3_reset(stmt);
    }
    sqlite3_finalize(stmt);
    if (sqlite3_exec(db_, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) {
        std::cerr << "flush_log_queue: COMMIT failed: " << sqlite3_errmsg(db_) << "\n";
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
    }
}

inline void FrontendServer::rotate_logs() {
    if (!db_) return;
    static const std::string sql =
        "DELETE FROM logs WHERE timestamp < datetime('now', '-"
        + std::to_string(LOG_RETENTION_DAYS) + " days')";
    sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, nullptr);
}

inline void FrontendServer::handle_sse_stream(struct mg_connection *c, struct mg_http_message *hm) {
    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        if (sse_connections_.size() >= MAX_SSE_CONNECTIONS) {
            mg_http_reply(c, 503, "", "Too many SSE connections\n");
            return;
        }
    }

    char service_filter[30] = {0};
    mg_http_get_var(&hm->query, "service", service_filter, sizeof(service_filter));

    c->data[0] = 'S';
    memset(c->data + 1, 0, MG_DATA_SIZE - 1);
    if (service_filter[0]) {
        strncpy(c->data + 1, service_filter, MG_DATA_SIZE - 2);
    }

    mg_printf(c,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/event-stream; charset=utf-8\r\n"
        "Cache-Control: no-cache\r\n"
        "Connection: keep-alive\r\n"
        "Transfer-Encoding: chunked\r\n\r\n");

    {
        std::lock_guard<std::mutex> lock(sse_mutex_);
        sse_connections_.push_back(c);
    }
}

inline void FrontendServer::remove_sse_connection(struct mg_connection *c) {
    std::lock_guard<std::mutex> lock(sse_mutex_);
    sse_connections_.erase(
        std::remove(sse_connections_.begin(), sse_connections_.end(), c),
        sse_connections_.end());
}

inline void FrontendServer::flush_sse_queue() {
    std::vector<LogEntry> batch;
    {
        std::lock_guard<std::mutex> lock(sse_queue_mutex_);
        if (sse_queue_.empty()) return;
        batch.swap(sse_queue_);
    }

    std::lock_guard<std::mutex> lock(sse_mutex_);
    if (sse_connections_.empty()) return;

    for (const auto& entry : batch) {
        const std::string& svc = entry.service;
        std::string json = "{\"timestamp\":\"" + escape_json(entry.timestamp) +
            "\",\"service\":\"" + escape_json(svc) +
            "\",\"level\":\"" + escape_json(entry.level) +
            "\",\"call_id\":" + std::to_string(entry.call_id) +
            ",\"message\":\"" + escape_json(entry.message) + "\"}";
        std::string sse_msg = "data: " + json + "\n\n";

        for (auto* c : sse_connections_) {
            std::string filter(c->data + 1, strnlen(c->data + 1, MG_DATA_SIZE - 1));
            if (!filter.empty() && svc != filter) continue;
            mg_http_printf_chunk(c, "%s", sse_msg.c_str());
        }
    }
}
