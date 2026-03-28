// sip-client-main.cpp — SIP/RTP gateway bridging the telephone network to the pipeline.
//
// Pipeline position: [SIP_CLIENT] ↔ IAP (outbound RTP) / OAP (inbound TTS)
//
// This is the entry and exit point of the pipeline for real telephone calls.
// It implements a minimal SIP stack sufficient for registration + call handling,
// and raw RTP I/O over UDP sockets.
//
// SIP signaling (raw UDP, port 5060 or configured):
//   Registration: Sends REGISTER with Digest authentication. Re-registers every
//     60s. Parses WWW-Authenticate challenge for MD5-hash Digest credentials.
//   INVITE handling: Parses SDP to extract remote IP/port, allocates a local RTP
//     port, responds with 200 OK + SDP, starts RTP threads.
//   BYE: Terminates the matching call session; sends CALL_END downstream.
//   Multi-line: Supports up to N SIP registrations simultaneously (ADD_LINE command),
//     each with its own SIP UDP socket, registration thread, and RTP thread.
//
// RTP routing:
//   Inbound (network → pipeline): Each active call has an rtp_thread that recvfrom()s
//     RTP packets from the network and forwards them as Packet frames to the IAP
//     via interconnect send_to_downstream(). Packet includes the full RTP header
//     (12 bytes) — IAP strips it.
//   Outbound (pipeline → network): OAP connects to SIP_CLIENT's listen port (13100/13101)
//     and pushes 160-byte G.711 frames. SIP_CLIENT wraps them in RTP headers
//     (seq, ts, ssrc) and sendto() to the remote caller.
//
// Session management:
//   CallSession: tracks call_id, SIP Call-ID, remote IP/port, local RTP socket,
//     RTP counters (rx/tx packets, bytes, forwarded, discarded), and start time.
//   Numeric call_id is assigned from a monotonic counter (next_id_++) and passed
//     through the entire pipeline for session isolation.
//   Stale calls: hang up automatically if no RTP received for 60s (configurable).
//
// CMD port (SIP base+2 = 13102):
//   ADD_LINE:<user>:<server>:<port>:<password>  — register a new SIP account.
//   GET_STATS                                   — JSON stats for all active calls.
//   PING / STATUS                               — health check / status summary.
//   SET_LOG_LEVEL:<LEVEL>                       — change log verbosity at runtime.
//
// RTP port allocation: starts at RTP_PORT_BASE (10000), increments by 2 per call.
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
#include <ifaddrs.h>
#include <net/if.h>
#include <CommonCrypto/CommonDigest.h>
#include "interconnect.h"

static std::string detect_local_ip(const std::string& target_ip) {
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) return "127.0.0.1";
    struct sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(9);
    dest.sin_addr.s_addr = inet_addr(target_ip.c_str());
    if (connect(sock, (struct sockaddr*)&dest, sizeof(dest)) < 0) {
        close(sock);
        return "127.0.0.1";
    }
    struct sockaddr_in local{};
    socklen_t len = sizeof(local);
    getsockname(sock, (struct sockaddr*)&local, &len);
    close(sock);
    char addr_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local.sin_addr, addr_str, sizeof(addr_str));
    if (strcmp(addr_str, "0.0.0.0") == 0) return "127.0.0.1";
    return std::string(addr_str);
}

static std::string md5_hex(const std::string& input) {
    unsigned char digest[CC_MD5_DIGEST_LENGTH];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CC_MD5(input.c_str(), (CC_LONG)input.length(), digest);
#pragma clang diagnostic pop
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", digest[i]);
    hex[32] = '\0';
    return std::string(hex);
}

static std::string extract_sip_field(const std::string& header, const std::string& field) {
    std::string search = field + "=";
    size_t pos = header.find(search);
    if (pos == std::string::npos) return "";
    pos += search.length();
    if (pos < header.length() && header[pos] == '"') {
        pos++;
        size_t end = header.find('"', pos);
        if (end == std::string::npos) return "";
        return header.substr(pos, end - pos);
    }
    size_t end = header.find(',', pos);
    if (end == std::string::npos) end = header.length();
    while (end > pos && (header[end - 1] == ' ' || header[end - 1] == '\r' || header[end - 1] == '\n')) end--;
    return header.substr(pos, end - pos);
}

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
        ssrc = arc4random();
        start_time = std::chrono::steady_clock::now();
    }
};

struct SipLine {
    int index;
    int sip_sock;
    int local_port;
    int server_port = 5060;
    std::string user;
    std::string server_ip;
    std::string local_ip;
    std::string password;
    std::thread sip_thread;
    std::thread reg_thread;
    std::atomic<bool> registered{false};
    std::atomic<bool> line_running{true};
    std::string auth_realm;
    std::string auth_nonce;
    std::atomic<int> reg_cseq{1};
    std::string from_tag = std::to_string(arc4random());
    std::string reg_call_id;
    // Expires interval granted by the PBX in the REGISTER 200 OK.
    // Default 3600s; updated from the server response. Re-registration fires
    // at 2/3 of this value (~2400s by default) to keep the registration alive.
    std::atomic<int> granted_expires{3600};
};

class SipClient {
public:
    SipClient() : running_(true), next_id_(1), interconnect_(whispertalk::ServiceType::SIP_CLIENT) {
    }

    static constexpr const char* DEFAULT_LINE_NAMES[] = {
        "alice", "bob", "charlie", "david", "eve", "frank", "george", "helen", "ivan", "julia",
        "karl", "laura", "max", "nina", "oscar", "petra", "quinn", "rosa", "sam", "tina"
    };

    bool init(const std::string& user, const std::string& server, int port, int num_lines) {
        server_ = server;
        server_port_ = port;
        if (!server.empty()) local_ip_ = detect_local_ip(server);

        for (int i = 0; i < num_lines; ++i) {
            std::string line_user;
            if (num_lines == 1) {
                line_user = user;
            } else {
                line_user = (i < 20) ? DEFAULT_LINE_NAMES[i] : user + std::to_string(i + 1);
            }
            int idx = create_line(line_user, server, "", port);
            if (idx < 0) return false;
        }

        if (!interconnect_.initialize()) {
            std::cerr << "SIP_CLIENT: Failed to initialize interconnect" << std::endl;
            return false;
        }

        log_fwd_.init(whispertalk::FRONTEND_LOG_PORT, whispertalk::ServiceType::SIP_CLIENT);
        log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "Interconnect initialized");

        if (!interconnect_.connect_to_downstream()) {
            log_fwd_.forward(whispertalk::LogLevel::WARN, 0, "Downstream (IAP) not available yet - will auto-reconnect");
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
                log_fwd_.forward(whispertalk::LogLevel::ERROR, 0, "Failed to bind command port %d", cmd_port_);
            } else {
                cmd_sock_.store(csock);
                log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "Command port listening on %d", cmd_port_);
            }
        }

        return true;
    }

    void set_log_level(const char* level) {
        log_fwd_.set_level(level);
    }

    int add_line(const std::string& user, const std::string& server_ip, const std::string& password, int port = 5060) {
        std::lock_guard<std::mutex> lock(lines_mutex_);
        log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "Adding SIP line: user=%s server=%s port=%d", user.c_str(), server_ip.c_str(), port);
        int idx = create_line(user, server_ip, password, port);
        if (idx < 0) {
            log_fwd_.forward(whispertalk::LogLevel::ERROR, 0, "Failed to create SIP line for user %s", user.c_str());
            return -1;
        }
        auto& line = lines_.back();
        line->sip_thread = std::thread(&SipClient::sip_loop, this, line);
        line->reg_thread = std::thread(&SipClient::registration_loop, this, line);
        log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "SIP line %d added successfully: user=%s server=%s port=%d", idx, user.c_str(), server_ip.c_str(), line->local_port);
        return idx;
    }

    bool remove_line(int index) {
        std::lock_guard<std::mutex> lock(lines_mutex_);
        log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "Removing SIP line %d", index);
        auto it = std::find_if(lines_.begin(), lines_.end(),
            [index](const std::shared_ptr<SipLine>& l) { return l->index == index; });
        if (it == lines_.end()) {
            log_fwd_.forward(whispertalk::LogLevel::WARN, 0, "SIP line %d not found", index);
            return false;
        }

        auto line = *it;
        std::string user = line->user;
        line->line_running = false;
        line->registered = false;

        log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "Shutting down line %d (%s): stopping threads", index, user.c_str());
        if (line->sip_thread.joinable()) line->sip_thread.join();
        if (line->reg_thread.joinable()) line->reg_thread.join();

        if (line->sip_sock >= 0) close(line->sip_sock);

        int terminated_calls = 0;
        {
            std::lock_guard<std::mutex> clock(calls_mutex_);
            for (auto cit = calls_.begin(); cit != calls_.end(); ) {
                if (cit->second->line_index == index) {
                    log_fwd_.forward(whispertalk::LogLevel::INFO, cit->second->id, "Terminating call %d on line %d", cit->second->id, index);
                    cit->second->active = false;
                    cit = calls_.erase(cit);
                    terminated_calls++;
                } else {
                    ++cit;
                }
            }
        }

        lines_.erase(it);
        log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "SIP line %d removed: user=%s terminated_calls=%d", index, user.c_str(), terminated_calls);
        return true;
    }

    std::string list_lines() {
        std::lock_guard<std::mutex> lock(lines_mutex_);
        std::ostringstream out;
        out << "LINES";
        for (const auto& line : lines_) {
            out << " " << line->index << ":" << line->user
                << ":" << (line->registered ? "registered" : "unregistered")
                << ":" << line->server_ip << ":" << line->server_port
                << ":" << line->local_ip;
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

    static std::string cseq_method(const std::string& msg) {
        size_t p = msg.find("CSeq:");
        if (p == std::string::npos) return "";
        size_t eol = msg.find_first_of("\r\n", p);
        if (eol == std::string::npos) eol = msg.size();
        size_t sp = msg.rfind(' ', eol);
        if (sp == std::string::npos || sp <= p) return "";
        return msg.substr(sp + 1, eol - sp - 1);
    }

    void send_sip_response(const std::string& request, const char* status, const struct sockaddr_in& sender, std::shared_ptr<SipLine> line) {
        auto get_hdr = [&](const std::string& h) -> std::string {
            size_t p = request.find(h + ":");
            if (p == std::string::npos) return "";
            size_t s = p + h.length() + 1;
            while (s < request.size() && request[s] == ' ') s++;
            size_t e = request.find("\r\n", s);
            if (e == std::string::npos) e = request.size();
            return request.substr(s, e - s);
        };
        std::string via = get_hdr("Via");
        std::string from = get_hdr("From");
        std::string to = get_hdr("To");
        std::string callid = get_hdr("Call-ID");
        std::string cseq = get_hdr("CSeq");
        std::ostringstream resp;
        resp << "SIP/2.0 " << status << "\r\n";
        if (!via.empty()) resp << "Via: " << via << "\r\n";
        if (!from.empty()) resp << "From: " << from << "\r\n";
        if (!to.empty()) resp << "To: " << to << "\r\n";
        if (!callid.empty()) resp << "Call-ID: " << callid << "\r\n";
        if (!cseq.empty()) resp << "CSeq: " << cseq << "\r\n";
        resp << "Content-Length: 0\r\n\r\n";
        std::string s = resp.str();
        sendto(line->sip_sock, s.c_str(), s.length(), 0, (struct sockaddr*)&sender, sizeof(sender));
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
            log_fwd_.forward(whispertalk::LogLevel::TRACE, 0, "SIP RX line %d: %.120s", line->index, buf);
            if (msg.find("INVITE") == 0) {
                char sender_ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &sender.sin_addr, sender_ip, sizeof(sender_ip));
                log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "INVITE received on line %d from %s:%d",
                    line->index, sender_ip, ntohs(sender.sin_port));
                handle_invite(msg, sender, line);
            }
            else if (msg.find("BYE") == 0) handle_bye(msg, sender, line);
            else if (msg.find("ACK") == 0) {
                log_fwd_.forward(whispertalk::LogLevel::DEBUG, 0, "ACK received on line %d", line->index);
            }
            else if (msg.find("NOTIFY") == 0 || msg.find("OPTIONS") == 0 || msg.find("CANCEL") == 0) {
                send_sip_response(msg, "200 OK", sender, line);
            }
            else if (msg.find("SIP/2.0 200") == 0 && cseq_method(msg) == "REGISTER") {
                // Parse Expires granted by the server; store so registration_loop can
                // reschedule at 2/3 of the correct interval instead of a fixed 30s.
                size_t ep = msg.find("\r\nExpires:");
                if (ep == std::string::npos) ep = msg.find("\r\nexpires:");
                if (ep != std::string::npos) {
                    size_t vs = ep + 10; // "\r\nExpires:" length
                    while (vs < msg.size() && msg[vs] == ' ') vs++;
                    size_t ve = msg.find_first_of("\r\n", vs);
                    if (ve != std::string::npos) {
                        try {
                            int exp = std::stoi(msg.substr(vs, ve - vs));
                            if (exp > 0) line->granted_expires = exp;
                        } catch (...) {}
                    }
                }
                bool was_registered = line->registered;
                line->registered = true;
                if (!was_registered) {
                    std::string lip = line->local_ip.empty() ? local_ip_ : line->local_ip;
                    log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "Line %d (%s) registered successfully (contact=%s:%d expires=%ds)",
                        line->index, line->user.c_str(), lip.c_str(), line->local_port, line->granted_expires.load());
                }
            }
            else if ((msg.find("SIP/2.0 401") == 0 || msg.find("SIP/2.0 407") == 0) && cseq_method(msg) == "REGISTER") {
                std::string www_auth;
                size_t ap = msg.find("WWW-Authenticate:");
                if (ap == std::string::npos) ap = msg.find("Proxy-Authenticate:");
                if (ap != std::string::npos) {
                    size_t ae = msg.find("\r\n", ap);
                    if (ae != std::string::npos) www_auth = msg.substr(ap, ae - ap);
                }
                if (!www_auth.empty()) {
                    line->auth_realm = extract_sip_field(www_auth, "realm");
                    line->auth_nonce = extract_sip_field(www_auth, "nonce");
                    log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "Auth challenge on line %d: realm=%s, sending credentials",
                        line->index, line->auth_realm.c_str());
                    register_sip(line, true);
                } else {
                    log_fwd_.forward(whispertalk::LogLevel::WARN, 0, "401/407 without WWW-Authenticate on line %d", line->index);
                }
            }
        }
    }

    void registration_loop(std::shared_ptr<SipLine> line) {
        while (running_ && line->line_running) {
            // Preemptive auth: if we already have credentials from a previous 401
            // challenge, send them directly to avoid a round-trip. If the cached
            // nonce has since expired, the PBX will respond with 401 stale=true and
            // the sip_loop 401 handler will transparently retry with the fresh nonce
            // — so this always degrades gracefully to a normal challenge cycle.
            bool preemptive = !line->auth_realm.empty() && !line->auth_nonce.empty()
                              && !line->password.empty();
            register_sip(line, preemptive);

            // Re-register at 2/3 of the server-granted Expires interval (RFC 3261).
            // Minimum 60s to handle very short-lived registrations; checked in 1s
            // increments so the loop exits quickly when the service shuts down.
            int sleep_secs = std::max(60, line->granted_expires.load() * 2 / 3);
            for (int i = 0; i < sleep_secs && running_ && line->line_running; ++i) {
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
        if (!cseq_str.empty()) {
            try { cseq = std::stoi(cseq_str.substr(0, cseq_str.find(' '))); } catch (...) {}
        }

        {
            std::string trying = "SIP/2.0 100 Trying\r\nVia: " + via + "\r\nFrom: " + from + "\r\nTo: " + to + "\r\nCall-ID: " + scid + "\r\nCSeq: " + std::to_string(cseq) + " INVITE\r\nContent-Length: 0\r\n\r\n";
            sendto(line->sip_sock, trying.c_str(), trying.length(), 0, (struct sockaddr*)&sender, sizeof(sender));
        }

        uint32_t proposed_id;
        {
            std::lock_guard<std::mutex> lock(call_id_mutex_);
            proposed_id = next_id_++;
        }
        uint32_t cid = interconnect_.reserve_call_id(proposed_id);
        if (cid == 0) {
            log_fwd_.forward(whispertalk::LogLevel::ERROR, 0, "Failed to reserve call_id on line %d", line->index);
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

        // Parse SDP c= line for the media IP (may differ from the SIP signaling
        // address when the PBX uses a dedicated media proxy or RTP relay).
        size_t c_pos = msg.find("\r\nc=IN IP4 ");
        if (c_pos != std::string::npos) {
            size_t vs = c_pos + 11; // "\r\nc=IN IP4 " is 11 chars
            size_t ve = msg.find_first_of("\r\n", vs);
            if (ve != std::string::npos) session->remote_ip = msg.substr(vs, ve - vs);
        }
        // Fall back to the UDP packet source if no c= line is present.
        if (session->remote_ip.empty()) {
            char rip[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sender.sin_addr, rip, sizeof(rip));
            session->remote_ip = rip;
        }

        size_t m_pos = msg.find("m=audio ");
        if (m_pos != std::string::npos) {
            std::string m_line = msg.substr(m_pos + 8);
            try { session->remote_port = std::stoi(m_line.substr(0, m_line.find(' '))); } catch (...) {}
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

        std::string lip = line->local_ip.empty() ? local_ip_ : line->local_ip;
        std::ostringstream resp;
        resp << "SIP/2.0 200 OK\r\nVia: " << via << "\r\nFrom: " << from << "\r\nTo: " << to << ";tag=whisper" << cid << "\r\n";
        resp << "Call-ID: " << scid << "\r\nCSeq: " << cseq << " INVITE\r\nContact: <sip:" << line->user << "@" << lip << ":" << line->local_port << ">\r\n";
        resp << "Content-Type: application/sdp\r\n";
        std::ostringstream sdp;
        auto now_ts = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        sdp << "v=0\r\no=whisper " << cid << " " << now_ts << " IN IP4 " << lip << "\r\ns=-\r\nc=IN IP4 " << lip << "\r\nt=0 0\r\n";
        sdp << "m=audio " << session->local_rtp_port << " RTP/AVP 0 101\r\na=rtpmap:0 PCMU/8000\r\na=rtpmap:101 telephone-event/8000\r\n";
        resp << "Content-Length: " << sdp.str().length() << "\r\n\r\n" << sdp.str();
        std::string s = resp.str();
        sendto(line->sip_sock, s.c_str(), s.length(), 0, (struct sockaddr*)&sender, sizeof(sender));
        log_fwd_.forward(whispertalk::LogLevel::INFO, cid, "Accepted call on line %d port %d", line->index, session->local_rtp_port);
    }

    void handle_bye(const std::string& msg, const struct sockaddr_in& sender, std::shared_ptr<SipLine> line) {
        auto get_header = [&](const std::string& h) -> std::string {
            size_t p = msg.find(h + ":");
            if (p == std::string::npos) return "";
            size_t s = p + h.length() + 1;
            while (s < msg.size() && msg[s] == ' ') s++;
            size_t e = msg.find("\r\n", s);
            if (e == std::string::npos) e = msg.size();
            return msg.substr(s, e - s);
        };

        std::string scid = get_header("Call-ID");
        if (scid.empty()) return;
        std::string via = get_header("Via");
        std::string from = get_header("From");
        std::string to = get_header("To");
        std::string cseq = get_header("CSeq");

        int call_id = 0;
        {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            if (calls_.count(scid)) {
                call_id = calls_[scid]->id;
                calls_[scid]->active = false;
                std::ostringstream resp;
                resp << "SIP/2.0 200 OK\r\n";
                if (!via.empty()) resp << "Via: " << via << "\r\n";
                if (!from.empty()) resp << "From: " << from << "\r\n";
                if (!to.empty()) resp << "To: " << to << "\r\n";
                resp << "Call-ID: " << scid << "\r\n";
                if (!cseq.empty()) resp << "CSeq: " << cseq << "\r\n";
                resp << "Content-Length: 0\r\n\r\n";
                std::string s = resp.str();
                sendto(line->sip_sock, s.c_str(), s.length(), 0, (struct sockaddr*)&sender, sizeof(sender));
            }
        }
        if (call_id > 0) {
            interconnect_.broadcast_call_end(call_id);
        }
    }

    void handle_call_end(uint32_t call_id) {
        std::thread rtp_thread_to_join;
        {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            for (auto it = calls_.begin(); it != calls_.end(); ++it) {
                if (it->second->id == static_cast<int>(call_id)) {
                    log_fwd_.forward(whispertalk::LogLevel::INFO, call_id, "Call ended, cleaning up");
                    it->second->active = false;
                    if (it->second->rtp_thread.joinable()) {
                        rtp_thread_to_join = std::move(it->second->rtp_thread);
                    }
                    id_to_call_.erase(call_id);
                    calls_.erase(it);
                    break;
                }
            }
        }
        if (rtp_thread_to_join.joinable()) {
            rtp_thread_to_join.join();
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

    void register_sip(std::shared_ptr<SipLine> line, bool with_auth = false) {
        std::string reg_server = line->server_ip.empty() ? server_ : line->server_ip;
        std::string lip = line->local_ip.empty() ? local_ip_ : line->local_ip;
        if (line->reg_call_id.empty()) {
            line->reg_call_id = "reg-" + std::to_string(line->index) + "-" + std::to_string(arc4random()) + "@" + lip;
        }
        int cseq = line->reg_cseq++;
        std::ostringstream req;
        req << "REGISTER sip:" << reg_server << " SIP/2.0\r\n";
        req << "Via: SIP/2.0/UDP " << lip << ":" << line->local_port << ";rport;branch=z9hG4bK" << arc4random() << "\r\n";
        req << "Max-Forwards: 70\r\n";
        req << "From: <sip:" << line->user << "@" << reg_server << ">;tag=" << line->from_tag << "\r\n";
        req << "To: <sip:" << line->user << "@" << reg_server << ">\r\n";
        req << "Call-ID: " << line->reg_call_id << "\r\n";
        req << "CSeq: " << cseq << " REGISTER\r\n";
        req << "Contact: <sip:" << line->user << "@" << lip << ":" << line->local_port << ">\r\n";
        req << "Expires: 3600\r\n";
        req << "User-Agent: Prodigy/1.0\r\n";
        if (with_auth && !line->auth_nonce.empty() && !line->password.empty()) {
            std::string uri = "sip:" + reg_server;
            std::string ha1 = md5_hex(line->user + ":" + line->auth_realm + ":" + line->password);
            std::string ha2 = md5_hex("REGISTER:" + uri);
            std::string response = md5_hex(ha1 + ":" + line->auth_nonce + ":" + ha2);
            req << "Authorization: Digest username=\"" << line->user << "\","
                << "realm=\"" << line->auth_realm << "\","
                << "nonce=\"" << line->auth_nonce << "\","
                << "uri=\"" << uri << "\","
                << "response=\"" << response << "\","
                << "algorithm=MD5\r\n";
        }
        req << "Content-Length: 0\r\n\r\n";
        struct sockaddr_in srv{};
        srv.sin_family = AF_INET; srv.sin_port = htons(line->server_port); srv.sin_addr.s_addr = inet_addr(reg_server.c_str());
        std::string s = req.str();
        sendto(line->sip_sock, s.c_str(), s.length(), 0, (struct sockaddr*)&srv, sizeof(srv));
    }

    int create_line(const std::string& user, const std::string& server_ip, const std::string& password, int port = 5060) {
        auto line = std::make_shared<SipLine>();
        line->index = next_line_index_++;
        line->user = user;
        line->server_ip = server_ip;
        line->password = password;
        line->server_port = port;
        line->local_ip = server_ip.empty() ? local_ip_ : detect_local_ip(server_ip);

        line->sip_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (line->sip_sock < 0) {
            log_fwd_.forward(whispertalk::LogLevel::ERROR, 0, "Failed to create SIP socket for line %d", line->index);
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
        log_fwd_.forward(whispertalk::LogLevel::DEBUG, 0, "SIP line %d created: user=%s port=%d local_ip=%s",
            line->index, line->user.c_str(), line->local_port, line->local_ip.c_str());
        return line->index;
    }

    std::string handle_line_command(const std::string& msg) {
        if (msg.substr(0, 9) == "ADD_LINE ") {
            std::istringstream iss(msg.substr(9));
            std::string user, server_ip, port_str;
            iss >> user >> server_ip >> port_str;
            if (user.empty() || server_ip.empty()) {
                return "ERROR Missing user or server_ip";
            }
            int port = 5060;
            if (!port_str.empty()) {
                try { port = std::stoi(port_str); } catch (...) { port = 5060; }
                if (port < 1 || port > 65535) port = 5060;
            }
            std::string password;
            // Skip exactly one delimiter space between port and password, then
            // read the rest of the line verbatim (passwords may contain spaces).
            if (!iss.eof()) iss.ignore(1);
            std::getline(iss, password);
            if (password == "-") password.clear();
            int idx = add_line(user, server_ip, password, port);
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
        else if (msg.rfind("SET_LOG_LEVEL:", 0) == 0) {
            std::string level = msg.substr(14);
            log_fwd_.set_level(level.c_str());
            return "OK\n";
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
    int lines = 0;
    std::string log_level = "INFO";

    static struct option long_opts[] = {
        {"lines",     required_argument, 0, 'l'},
        {"log-level", required_argument, 0, 'L'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "l:L:", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'l': lines = atoi(optarg); if (lines < 0) lines = 0; break;
            case 'L': log_level = optarg; break;
            default: break;
        }
    }

    if (lines > 0 && optind + 1 >= argc) {
        std::cout << "Usage: sip-client [--lines N] [--log-level LEVEL] [<user> <server> [port]]" << std::endl;
        std::cout << "       When --lines 0 (default), starts with no registered lines." << std::endl;
        return 1;
    }

    if (lines > 0) {
        user = argv[optind];
        server = argv[optind + 1];
        if (optind + 2 < argc) port = atoi(argv[optind + 2]);
    }

    SipClient client;
    if (client.init(user, server, port, lines)) {
        client.set_log_level(log_level.c_str());
        client.run();
    }
    return 0;
}
