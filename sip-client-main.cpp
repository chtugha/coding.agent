// SIP Client Module (Interconnect-based, multi-line)
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sstream>
#include <iomanip>
#include <getopt.h>
#include "interconnect.h"

static constexpr int RTP_PORT_BASE = 10000;

struct CallSession {
    int id;
    int line_index;
    std::string sip_call_id;
    std::string remote_ip;
    int remote_port;
    int local_rtp_port;
    int rtp_sock;
    std::atomic<bool> active{true};
    std::thread rtp_thread;
    uint16_t seq = 0;
    uint32_t ts = 0;
    uint32_t ssrc = 0;
    std::atomic<uint64_t> rtp_rx_count{0};     // Total RTP packets received from network
    std::atomic<uint64_t> rtp_tx_count{0};     // Total RTP packets sent to network
    std::atomic<uint64_t> rtp_rx_bytes{0};     // Total bytes received
    std::atomic<uint64_t> rtp_tx_bytes{0};     // Total bytes sent
    std::atomic<uint64_t> rtp_fwd_count{0};    // Packets successfully forwarded to IAP
    std::atomic<uint64_t> rtp_discard_count{0}; // Packets discarded (IAP not connected)
    std::chrono::steady_clock::time_point start_time;

    CallSession(int i, int line, std::string scid) : id(i), line_index(line), sip_call_id(scid) {
        ssrc = rand();
        start_time = std::chrono::steady_clock::now();
    }
};

struct SipLine {
    int index;
    int sip_sock;
    int local_port;
    std::string user;
    std::string server_ip;
    std::string password;
    std::thread sip_thread;
    std::thread reg_thread;
    std::atomic<bool> registered{false};
    std::atomic<bool> line_running{true};
};

class SipClient {
public:
    SipClient() : running_(true), next_id_(1), interconnect_(whispertalk::ServiceType::SIP_CLIENT) {
        srand(time(NULL));
    }

    static constexpr const char* DEFAULT_LINE_NAMES[] = {
        "alice", "bob", "charlie", "david", "eve", "frank", "george", "helen", "ivan", "julia",
        "karl", "laura", "max", "nina", "oscar", "petra", "quinn", "rosa", "sam", "tina"
    };

    bool init(const std::string& user, const std::string& server, int port, int num_lines) {
        server_ = server;
        server_port_ = port;
        local_ip_ = "127.0.0.1";

        for (int i = 0; i < num_lines; ++i) {
            std::string line_user;
            if (num_lines == 1) {
                line_user = user;
            } else {
                line_user = (i < 20) ? DEFAULT_LINE_NAMES[i] : user + std::to_string(i + 1);
            }
            int idx = create_line(line_user, server, "");
            if (idx < 0) return false;
        }

        if (!interconnect_.initialize()) {
            std::cerr << "Failed to initialize interconnect" << std::endl;
            return false;
        }

        std::cout << "Interconnect initialized (peer-to-peer)" << std::endl;

        log_fwd_.init(whispertalk::FRONTEND_LOG_PORT, whispertalk::ServiceType::SIP_CLIENT);

        if (!interconnect_.connect_to_downstream()) {
            std::cout << "Downstream (IAP) not available yet - will auto-reconnect" << std::endl;
        }

        interconnect_.register_call_end_handler([this](uint32_t call_id) {
            this->handle_call_end(call_id);
        });

        cmd_port_ = whispertalk::service_cmd_port(whispertalk::ServiceType::SIP_CLIENT);
        int csock = socket(AF_INET, SOCK_STREAM, 0);
        if (csock >= 0) {
            int opt = 1;
            setsockopt(csock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
            struct sockaddr_in addr{};
            addr.sin_family = AF_INET;
            addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            addr.sin_port = htons(cmd_port_);
            if (bind(csock, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
                listen(csock, 4) < 0) {
                ::close(csock);
                std::cerr << "Failed to bind command port " << cmd_port_ << std::endl;
            } else {
                cmd_sock_.store(csock);
                std::cout << "Command port listening on " << cmd_port_ << std::endl;
            }
        }

        return true;
    }

    int add_line(const std::string& user, const std::string& server_ip, const std::string& password) {
        std::lock_guard<std::mutex> lock(lines_mutex_);
        log_fwd_.forward("INFO", 0, "Adding SIP line: user=%s server=%s", user.c_str(), server_ip.c_str());
        int idx = create_line(user, server_ip, password);
        if (idx < 0) {
            log_fwd_.forward("ERROR", 0, "Failed to create SIP line for user %s", user.c_str());
            return -1;
        }
        auto& line = lines_.back();
        line->sip_thread = std::thread(&SipClient::sip_loop, this, line);
        line->reg_thread = std::thread(&SipClient::registration_loop, this, line);
        std::cout << "Added line " << idx << " (" << user << "@" << server_ip << ")" << std::endl;
        log_fwd_.forward("INFO", 0, "SIP line %d added successfully: user=%s port=%d", idx, user.c_str(), line->local_port);
        return idx;
    }

    bool remove_line(int index) {
        std::lock_guard<std::mutex> lock(lines_mutex_);
        log_fwd_.forward("INFO", 0, "Removing SIP line %d", index);
        auto it = std::find_if(lines_.begin(), lines_.end(),
            [index](const std::shared_ptr<SipLine>& l) { return l->index == index; });
        if (it == lines_.end()) {
            log_fwd_.forward("WARN", 0, "SIP line %d not found", index);
            return false;
        }

        auto line = *it;
        std::string user = line->user;
        line->line_running = false;
        line->registered = false;

        log_fwd_.forward("INFO", 0, "Shutting down line %d (%s): stopping threads", index, user.c_str());
        if (line->sip_thread.joinable()) line->sip_thread.join();
        if (line->reg_thread.joinable()) line->reg_thread.join();

        if (line->sip_sock >= 0) close(line->sip_sock);

        int terminated_calls = 0;
        {
            std::lock_guard<std::mutex> clock(calls_mutex_);
            for (auto cit = calls_.begin(); cit != calls_.end(); ) {
                if (cit->second->line_index == index) {
                    log_fwd_.forward("INFO", cit->second->id, "Terminating call %d on line %d", cit->second->id, index);
                    cit->second->active = false;
                    cit = calls_.erase(cit);
                    terminated_calls++;
                } else {
                    ++cit;
                }
            }
        }

        lines_.erase(it);
        std::cout << "Removed line " << index << " (" << user << ")" << std::endl;
        log_fwd_.forward("INFO", 0, "SIP line %d removed: user=%s terminated_calls=%d", index, user.c_str(), terminated_calls);
        return true;
    }

    std::string list_lines() {
        std::lock_guard<std::mutex> lock(lines_mutex_);
        std::ostringstream out;
        out << "LINES";
        for (const auto& line : lines_) {
            out << " " << line->index << ":" << line->user
                << ":" << (line->registered ? "registered" : "unregistered");
        }
        return out.str();
    }

    // Returns stats wire format:
    //   "STATS <n_calls> DS:<0|1> <id>:<line>:<rx>:<tx>:<rx_bytes>:<tx_bytes>:<duration>:<fwd>:<discard> ..."
    // DS:1 = downstream (IAP) TCP connection active, DS:0 = disconnected.
    // Per-call fields: id=call_id, line=line_index, rx/tx=RTP packet counts,
    // rx_bytes/tx_bytes=total bytes, duration=seconds, fwd=forwarded to IAP, discard=discarded (IAP offline).
    std::string get_stats() {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto ds = interconnect_.downstream_state();
        bool ds_connected = (ds == whispertalk::ConnectionState::CONNECTED);
        std::ostringstream out;
        out << "STATS " << calls_.size() << " DS:" << (ds_connected ? "1" : "0");
        for (const auto& kv : id_to_call_) {
            auto session = kv.second;
            auto now = std::chrono::steady_clock::now();
            auto duration_sec = std::chrono::duration_cast<std::chrono::seconds>(now - session->start_time).count();
            out << " " << session->id 
                << ":" << session->line_index
                << ":" << session->rtp_rx_count.load()
                << ":" << session->rtp_tx_count.load()
                << ":" << session->rtp_rx_bytes.load()
                << ":" << session->rtp_tx_bytes.load()
                << ":" << duration_sec
                << ":" << session->rtp_fwd_count.load()
                << ":" << session->rtp_discard_count.load();
        }
        return out.str();
    }

    void run() {
        {
            std::lock_guard<std::mutex> lock(lines_mutex_);
            for (auto& line : lines_) {
                line->sip_thread = std::thread(&SipClient::sip_loop, this, line);
                line->reg_thread = std::thread(&SipClient::registration_loop, this, line);
            }
        }

        std::thread out_thread(&SipClient::outbound_audio_loop, this);
        std::thread cmd_thread(&SipClient::command_listener_loop, this);

        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        std::vector<std::shared_ptr<SipLine>> lines_copy;
        {
            std::lock_guard<std::mutex> lock(lines_mutex_);
            lines_copy = lines_;
            for (auto& line : lines_copy) line->line_running = false;
        }
        for (auto& line : lines_copy) {
            if (line->sip_thread.joinable()) line->sip_thread.join();
            if (line->reg_thread.joinable()) line->reg_thread.join();
        }
        out_thread.join();
        int sock = cmd_sock_.exchange(-1);
        if (sock >= 0) ::close(sock);
        cmd_thread.join();
        interconnect_.shutdown();
    }

private:
    std::atomic<int> cmd_sock_{-1};
    uint16_t cmd_port_ = 0;

    void command_listener_loop() {
        while (running_) {
            int lsock = cmd_sock_.load();
            if (lsock < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            struct pollfd pfd = {lsock, POLLIN, 0};
            int ret = poll(&pfd, 1, 200);
            if (ret <= 0) continue;
            if (!(pfd.revents & POLLIN)) continue;

            int csock = accept(lsock, nullptr, nullptr);
            if (csock < 0) continue;

            struct timeval tv{2, 0};
            setsockopt(csock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(csock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            char buf[4096];
            ssize_t n = recv(csock, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                std::string msg(buf, n);
                while (!msg.empty() && (msg.back() == '\n' || msg.back() == '\r' || msg.back() == '\0'))
                    msg.pop_back();
                std::string response = handle_line_command(msg);
                if (!response.empty()) {
                    ::send(csock, response.c_str(), response.size(), 0);
                }
            }
            close(csock);
        }
    }

    void sip_loop(std::shared_ptr<SipLine> line) {
        char buf[4096];
        while (running_ && line->line_running) {
            struct sockaddr_in sender{};
            socklen_t slen = sizeof(sender);
            struct timeval tv{1, 0};
            setsockopt(line->sip_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ssize_t n = recvfrom(line->sip_sock, buf, sizeof(buf)-1, 0, (struct sockaddr*)&sender, &slen);
            if (n <= 0) continue;
            buf[n] = '\0';
            std::string msg(buf);
            if (msg.find("INVITE") == 0) handle_invite(msg, sender, line);
            else if (msg.find("BYE") == 0) handle_bye(msg, sender, line);
            else if (msg.find("SIP/2.0 200") == 0 && msg.find("REGISTER") != std::string::npos) {
                bool was_registered = line->registered;
                line->registered = true;
                if (!was_registered) {
                    std::cout << "Line " << line->index << " (" << line->user << ") registered successfully" << std::endl;
                    log_fwd_.forward("INFO", 0, "Line %d (%s) registered successfully", line->index, line->user.c_str());
                }
            }
        }
    }

    void registration_loop(std::shared_ptr<SipLine> line) {
        while (running_ && line->line_running) {
            register_sip(line);
            for (int i = 0; i < 30 && running_ && line->line_running; ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    }

    void handle_invite(const std::string& msg, const struct sockaddr_in& sender, std::shared_ptr<SipLine> line) {
        auto get_header = [&](const std::string& h) {
            size_t p = msg.find(h + ":");
            if (p == std::string::npos) return std::string("");
            size_t start = p + h.length() + 1;
            while (start < msg.size() && msg[start] == ' ') start++;
            size_t e = msg.find("\r\n", start);
            if (e == std::string::npos) e = msg.size();
            return msg.substr(start, e - start);
        };

        std::string scid = get_header("Call-ID");
        std::string from = get_header("From");
        std::string to = get_header("To");
        std::string via = get_header("Via");
        std::string cseq_str = get_header("CSeq");
        int cseq = 0;
        if (!cseq_str.empty()) cseq = std::stoi(cseq_str.substr(0, cseq_str.find(' ')));

        uint32_t proposed_id;
        {
            std::lock_guard<std::mutex> lock(call_id_mutex_);
            proposed_id = next_id_++;
        }
        uint32_t cid = interconnect_.reserve_call_id(proposed_id);
        if (cid == 0) {
            std::cerr << "Failed to reserve call_id, rejecting call on line " << line->index << std::endl;
            log_fwd_.forward("ERROR", 0, "Failed to reserve call_id on line %d", line->index);
            std::string resp = "SIP/2.0 503 Service Unavailable\r\nCall-ID: " + scid + "\r\n\r\n";
            sendto(line->sip_sock, resp.c_str(), resp.length(), 0, (struct sockaddr*)&sender, sizeof(sender));
            return;
        }
        {
            std::lock_guard<std::mutex> lock(call_id_mutex_);
            if (cid >= next_id_) {
                next_id_ = cid + 1;
            }
        }

        auto session = std::make_shared<CallSession>(cid, line->index, scid);

        size_t m_pos = msg.find("m=audio ");
        if (m_pos != std::string::npos) {
            std::string m_line = msg.substr(m_pos + 8);
            session->remote_port = std::stoi(m_line.substr(0, m_line.find(' ')));
            session->remote_ip = inet_ntoa(sender.sin_addr);
        }

        session->local_rtp_port = RTP_PORT_BASE + cid;
        session->rtp_sock = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in raddr{};
        raddr.sin_family = AF_INET;
        raddr.sin_port = htons(session->local_rtp_port);
        raddr.sin_addr.s_addr = INADDR_ANY;
        while (bind(session->rtp_sock, (struct sockaddr*)&raddr, sizeof(raddr)) < 0) {
            session->local_rtp_port++;
            raddr.sin_port = htons(session->local_rtp_port);
        }

        {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            calls_[scid] = session;
            id_to_call_[cid] = session;
        }

        session->rtp_thread = std::thread(&SipClient::rtp_receiver_loop, this, session);

        std::ostringstream resp;
        resp << "SIP/2.0 200 OK\r\nVia: " << via << "\r\nFrom: " << from << "\r\nTo: " << to << ";tag=whisper" << cid << "\r\n";
        resp << "Call-ID: " << scid << "\r\nCSeq: " << cseq << " INVITE\r\nContact: <sip:" << line->user << "@" << local_ip_ << ":" << line->local_port << ">\r\n";
        resp << "Content-Type: application/sdp\r\n";
        std::ostringstream sdp;
        sdp << "v=0\r\no=whisper 123 456 IN IP4 " << local_ip_ << "\r\ns=-\r\nc=IN IP4 " << local_ip_ << "\r\nt=0 0\r\n";
        sdp << "m=audio " << session->local_rtp_port << " RTP/AVP 0 101\r\na=rtpmap:0 PCMU/8000\r\na=rtpmap:101 telephone-event/8000\r\n";
        resp << "Content-Length: " << sdp.str().length() << "\r\n\r\n" << sdp.str();
        std::string s = resp.str();
        sendto(line->sip_sock, s.c_str(), s.length(), 0, (struct sockaddr*)&sender, sizeof(sender));
        std::cout << "Accepted call " << cid << " on line " << line->index << " port " << session->local_rtp_port << std::endl;
        log_fwd_.forward("INFO", cid, "Accepted call on line %d port %d", line->index, session->local_rtp_port);
    }

    void handle_bye(const std::string& msg, const struct sockaddr_in& sender, std::shared_ptr<SipLine> line) {
        size_t p = msg.find("Call-ID:");
        if (p == std::string::npos) return;
        size_t e = msg.find("\r\n", p);
        std::string scid = msg.substr(p + 9, e - p - 9);
        int call_id = 0;
        {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            if (calls_.count(scid)) {
                call_id = calls_[scid]->id;
                calls_[scid]->active = false;
                std::string resp = "SIP/2.0 200 OK\r\nCall-ID: " + scid + "\r\n\r\n";
                sendto(line->sip_sock, resp.c_str(), resp.length(), 0, (struct sockaddr*)&sender, sizeof(sender));
            }
        }
        if (call_id > 0) {
            interconnect_.broadcast_call_end(call_id);
        }
    }

    void handle_call_end(uint32_t call_id) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        for (auto it = calls_.begin(); it != calls_.end(); ++it) {
            if (it->second->id == static_cast<int>(call_id)) {
                std::cout << "Call " << call_id << " ended, cleaning up" << std::endl;
                log_fwd_.forward("INFO", call_id, "Call ended, cleaning up");
                it->second->active = false;
                if (it->second->rtp_thread.joinable()) {
                    it->second->rtp_thread.detach();
                }
                id_to_call_.erase(call_id);
                calls_.erase(it);
                break;
            }
        }
    }

    // Receives RTP packets from the network and forwards them to IAP via interconnect.
    // Tracks packet counts: received, forwarded (sent to IAP), and discarded (IAP offline).
    void rtp_receiver_loop(std::shared_ptr<CallSession> session) {
        char buf[2048];
        struct sockaddr_in sender{};
        socklen_t slen = sizeof(sender);
        struct timeval tv{0, 100000};
        setsockopt(session->rtp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        while (session->active && running_) {
            ssize_t n = recvfrom(session->rtp_sock, buf, sizeof(buf), 0, (struct sockaddr*)&sender, &slen);
            if (n < 12) continue;

            session->rtp_rx_count++;
            session->rtp_rx_bytes += n;

            whispertalk::Packet pkt(session->id, buf, n);
            pkt.trace.record(whispertalk::ServiceType::SIP_CLIENT, 0);
            pkt.trace.record(whispertalk::ServiceType::SIP_CLIENT, 1);
            if (interconnect_.send_to_downstream(pkt)) {
                session->rtp_fwd_count++;
            } else {
                session->rtp_discard_count++;
                if (interconnect_.downstream_state() != whispertalk::ConnectionState::CONNECTED) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }
            }
        }
        close(session->rtp_sock);
    }

    void outbound_audio_loop() {
        while (running_) {
            whispertalk::Packet pkt;
            if (!interconnect_.recv_from_upstream(pkt, 100)) {
                continue;
            }

            if (!pkt.is_valid() || pkt.payload_size != 160) {
                continue;
            }

            std::shared_ptr<CallSession> session;
            {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                if (id_to_call_.count(pkt.call_id)) session = id_to_call_[pkt.call_id];
            }

            if (session && session->active) {
                uint8_t rtp[12 + 160];
                rtp[0] = 0x80; rtp[1] = 0x00;
                uint16_t seq = htons(session->seq++);
                memcpy(rtp + 2, &seq, 2);
                uint32_t ts = htonl(session->ts); session->ts += 160;
                memcpy(rtp + 4, &ts, 4);
                uint32_t ssrc = htonl(session->ssrc);
                memcpy(rtp + 8, &ssrc, 4);
                memcpy(rtp + 12, pkt.payload.data(), 160);
                struct sockaddr_in dest{};
                dest.sin_family = AF_INET;
                dest.sin_port = htons(session->remote_port);
                dest.sin_addr.s_addr = inet_addr(session->remote_ip.c_str());
                ssize_t sent = sendto(session->rtp_sock, rtp, sizeof(rtp), 0, (struct sockaddr*)&dest, sizeof(dest));
                if (sent > 0) {
                    session->rtp_tx_count++;
                    session->rtp_tx_bytes += sent;
                }
            }
        }
    }

    void register_sip(std::shared_ptr<SipLine> line) {
        std::string reg_server = line->server_ip.empty() ? server_ : line->server_ip;
        std::ostringstream req;
        req << "REGISTER sip:" << reg_server << " SIP/2.0\r\nVia: SIP/2.0/UDP " << local_ip_ << ":" << line->local_port << "\r\n";
        req << "From: <sip:" << line->user << "@" << reg_server << ">\r\nTo: <sip:" << line->user << "@" << reg_server << ">\r\n";
        req << "Call-ID: reg-" << rand() << "@" << local_ip_ << "\r\nCSeq: 1 REGISTER\r\n";
        req << "Contact: <sip:" << line->user << "@" << local_ip_ << ":" << line->local_port << ">\r\nExpires: 3600\r\n\r\n";
        struct sockaddr_in srv{};
        srv.sin_family = AF_INET; srv.sin_port = htons(server_port_); srv.sin_addr.s_addr = inet_addr(reg_server.c_str());
        std::string s = req.str();
        sendto(line->sip_sock, s.c_str(), s.length(), 0, (struct sockaddr*)&srv, sizeof(srv));
    }

    int create_line(const std::string& user, const std::string& server_ip, const std::string& password) {
        auto line = std::make_shared<SipLine>();
        line->index = next_line_index_++;
        line->user = user;
        line->server_ip = server_ip;
        line->password = password;

        line->sip_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (line->sip_sock < 0) {
            std::cerr << "Failed to create SIP socket for line " << line->index << std::endl;
            return -1;
        }
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = 0;
        bind(line->sip_sock, (struct sockaddr*)&addr, sizeof(addr));

        socklen_t alen = sizeof(addr);
        getsockname(line->sip_sock, (struct sockaddr*)&addr, &alen);
        line->local_port = ntohs(addr.sin_port);

        lines_.push_back(line);
        std::cout << "SIP line " << line->index << " (" << line->user << ") on port " << line->local_port << std::endl;
        return line->index;
    }

    std::string handle_line_command(const std::string& msg) {
        if (msg.substr(0, 9) == "ADD_LINE ") {
            std::istringstream iss(msg.substr(9));
            std::string user, server_ip, password;
            iss >> user >> server_ip >> password;
            if (user.empty() || server_ip.empty()) {
                return "ERROR Missing user or server_ip";
            }
            int idx = add_line(user, server_ip, password);
            if (idx < 0) return "ERROR Failed to create line";
            return "LINE_ADDED " + std::to_string(idx);
        }
        else if (msg.substr(0, 12) == "REMOVE_LINE ") {
            int index = -1;
            try { index = std::stoi(msg.substr(12)); } catch (...) { return "ERROR Invalid index"; }
            if (remove_line(index)) {
                return "LINE_REMOVED " + std::to_string(index);
            }
            return "ERROR Line " + std::to_string(index) + " not found";
        }
        else if (msg == "LIST_LINES") {
            return list_lines();
        }
        else if (msg == "GET_STATS") {
            return get_stats();
        }
        else if (msg == "PING") {
            return "PONG\n";
        }
        else if (msg == "STATUS") {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            std::lock_guard<std::mutex> llock(lines_mutex_);
            return "ACTIVE_CALLS:" + std::to_string(calls_.size())
                + ":LINES:" + std::to_string(lines_.size())
                + ":DOWNSTREAM:" + (interconnect_.downstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected")
                + "\n";
        }
        return "ERROR:Unknown command\n";
    }

    std::atomic<bool> running_;
    uint32_t next_id_;
    int next_line_index_ = 0;
    std::mutex call_id_mutex_;
    int server_port_;
    std::string server_, local_ip_;
    std::mutex calls_mutex_;
    std::mutex lines_mutex_;
    std::map<std::string, std::shared_ptr<CallSession>> calls_;
    std::map<uint32_t, std::shared_ptr<CallSession>> id_to_call_;
    std::vector<std::shared_ptr<SipLine>> lines_;
    whispertalk::InterconnectNode interconnect_;
    whispertalk::LogForwarder log_fwd_;
};

int main(int argc, char** argv) {
    std::string user, server;
    int port = 5060;
    int lines = 1;

    static struct option long_opts[] = {
        {"lines", required_argument, 0, 'l'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "l:", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'l': lines = std::max(1, atoi(optarg)); break;
            default: break;
        }
    }

    if (optind + 1 >= argc) {
        std::cout << "Usage: sip-client [--lines N] <user> <server> [port]" << std::endl;
        return 1;
    }

    user = argv[optind];
    server = argv[optind + 1];
    if (optind + 2 < argc) port = atoi(argv[optind + 2]);

    SipClient client;
    if (client.init(user, server, port, lines)) client.run();
    return 0;
}
