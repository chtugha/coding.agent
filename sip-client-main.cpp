// SIP Client Module (Interconnect-based)
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
#include "interconnect.h"

struct CallSession {
    int id;
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

    CallSession(int i, std::string scid) : id(i), sip_call_id(scid) {
        ssrc = rand();
    }
};

class SipClient {
public:
    SipClient() : running_(true), next_id_(1), interconnect_(whispertalk::ServiceType::SIP_CLIENT) {
        srand(time(NULL));
    }

    bool init(const std::string& user, const std::string& server, int port) {
        user_ = user; server_ = server; server_port_ = port;
        
        sip_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = 0;
        bind(sip_sock_, (struct sockaddr*)&addr, sizeof(addr));
        
        socklen_t alen = sizeof(addr);
        getsockname(sip_sock_, (struct sockaddr*)&addr, &alen);
        local_port_ = ntohs(addr.sin_port);
        local_ip_ = "127.0.0.1";

        if (!interconnect_.initialize()) {
            std::cerr << "Failed to initialize interconnect" << std::endl;
            return false;
        }

        std::cout << "🔗 Interconnect initialized (master=" << interconnect_.is_master() << ")" << std::endl;

        if (!interconnect_.connect_to_downstream()) {
            std::cout << "⚠️  Downstream (IAP) not available yet - will auto-reconnect" << std::endl;
        }

        interconnect_.register_call_end_handler([this](uint32_t call_id) {
            this->handle_call_end(call_id);
        });

        return true;
    }

    void run() {
        std::thread sip_thread(&SipClient::sip_loop, this);
        std::thread out_thread(&SipClient::outbound_audio_loop, this);
        register_sip();
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            register_sip();
        }
        sip_thread.join();
        out_thread.join();
        interconnect_.shutdown();
    }

private:
    void sip_loop() {
        char buf[4096];
        while (running_) {
            struct sockaddr_in sender{};
            socklen_t slen = sizeof(sender);
            ssize_t n = recvfrom(sip_sock_, buf, sizeof(buf)-1, 0, (struct sockaddr*)&sender, &slen);
            if (n <= 0) continue;
            buf[n] = '\0';
            std::string msg(buf);
            if (msg.find("INVITE") == 0) handle_invite(msg, sender);
            else if (msg.find("BYE") == 0) handle_bye(msg, sender);
        }
    }

    void handle_invite(const std::string& msg, const struct sockaddr_in& sender) {
        auto get_header = [&](const std::string& h) {
            size_t p = msg.find(h + ":");
            if (p == std::string::npos) return std::string("");
            size_t e = msg.find("\r\n", p);
            return msg.substr(p + h.length() + 1, e - p - h.length() - 1);
        };

        std::string scid = get_header("Call-ID");
        std::string from = get_header("From");
        std::string to = get_header("To");
        std::string via = get_header("Via");
        std::string cseq_str = get_header("CSeq");
        int cseq = 0;
        if (!cseq_str.empty()) cseq = std::stoi(cseq_str.substr(0, cseq_str.find(' ')));

        uint32_t proposed_id = next_id_++;
        uint32_t cid = interconnect_.reserve_call_id(proposed_id);
        if (cid == 0) {
            std::cerr << "❌ Failed to reserve call_id, rejecting call" << std::endl;
            std::string resp = "SIP/2.0 503 Service Unavailable\r\nCall-ID: " + scid + "\r\n\r\n";
            sendto(sip_sock_, resp.c_str(), resp.length(), 0, (struct sockaddr*)&sender, sizeof(sender));
            return;
        }
        if (cid >= static_cast<uint32_t>(next_id_)) {
            next_id_ = cid + 1;
        }
        auto session = std::make_shared<CallSession>(cid, scid);
        
        size_t m_pos = msg.find("m=audio ");
        if (m_pos != std::string::npos) {
            std::string m_line = msg.substr(m_pos + 8);
            session->remote_port = std::stoi(m_line.substr(0, m_line.find(' ')));
            session->remote_ip = inet_ntoa(sender.sin_addr);
        }

        session->local_rtp_port = 10000 + cid;
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
        resp << "Call-ID: " << scid << "\r\nCSeq: " << cseq << " INVITE\r\nContact: <sip:" << user_ << "@" << local_ip_ << ":" << local_port_ << ">\r\n";
        resp << "Content-Type: application/sdp\r\n";
        std::ostringstream sdp;
        sdp << "v=0\r\no=whisper 123 456 IN IP4 " << local_ip_ << "\r\ns=-\r\nc=IN IP4 " << local_ip_ << "\r\nt=0 0\r\n";
        sdp << "m=audio " << session->local_rtp_port << " RTP/AVP 0 101\r\na=rtpmap:0 PCMU/8000\r\na=rtpmap:101 telephone-event/8000\r\n";
        resp << "Content-Length: " << sdp.str().length() << "\r\n\r\n" << sdp.str();
        std::string s = resp.str();
        sendto(sip_sock_, s.c_str(), s.length(), 0, (struct sockaddr*)&sender, sizeof(sender));
        std::cout << "📞 Accepted call " << cid << " on port " << session->local_rtp_port << std::endl;
    }

    void handle_bye(const std::string& msg, const struct sockaddr_in& sender) {
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
                sendto(sip_sock_, resp.c_str(), resp.length(), 0, (struct sockaddr*)&sender, sizeof(sender));
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
                std::cout << "🛑 Call " << call_id << " ended, cleaning up" << std::endl;
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

    void rtp_receiver_loop(std::shared_ptr<CallSession> session) {
        char buf[2048];
        struct sockaddr_in sender{};
        socklen_t slen = sizeof(sender);
        while (session->active && running_) {
            struct timeval tv{1, 0};
            setsockopt(session->rtp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            ssize_t n = recvfrom(session->rtp_sock, buf, sizeof(buf), 0, (struct sockaddr*)&sender, &slen);
            if (n < 12) continue;
            
            whispertalk::Packet pkt(session->id, buf, n);
            if (!interconnect_.send_to_downstream(pkt)) {
                if (interconnect_.downstream_state() != whispertalk::ConnectionState::CONNECTED) {
                    std::cout << "⚠️  [" << session->id << "] IAP disconnected, buffering/dropping RTP" << std::endl;
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
                sendto(session->rtp_sock, rtp, sizeof(rtp), 0, (struct sockaddr*)&dest, sizeof(dest));
            }
        }
    }

    void register_sip() {
        std::ostringstream req;
        req << "REGISTER sip:" << server_ << " SIP/2.0\r\nVia: SIP/2.0/UDP " << local_ip_ << ":" << local_port_ << "\r\n";
        req << "From: <sip:" << user_ << "@" << server_ << ">\r\nTo: <sip:" << user_ << "@" << server_ << ">\r\n";
        req << "Call-ID: reg-" << rand() << "@" << local_ip_ << "\r\nCSeq: 1 REGISTER\r\n";
        req << "Contact: <sip:" << user_ << "@" << local_ip_ << ":" << local_port_ << ">\r\nExpires: 3600\r\n\r\n";
        struct sockaddr_in srv{};
        srv.sin_family = AF_INET; srv.sin_port = htons(server_port_); srv.sin_addr.s_addr = inet_addr(server_.c_str());
        std::string s = req.str();
        sendto(sip_sock_, s.c_str(), s.length(), 0, (struct sockaddr*)&srv, sizeof(srv));
    }

    std::atomic<bool> running_;
    int sip_sock_, local_port_, server_port_, next_id_;
    std::string user_, server_, local_ip_;
    std::mutex calls_mutex_;
    std::map<std::string, std::shared_ptr<CallSession>> calls_;
    std::map<int, std::shared_ptr<CallSession>> id_to_call_;
    whispertalk::InterconnectNode interconnect_;
};

int main(int argc, char** argv) {
    if (argc < 3) { std::cout << "Usage: sip-client <user> <server> [port]" << std::endl; return 1; }
    SipClient client;
    if (client.init(argv[1], argv[2], argc > 3 ? atoi(argv[3]) : 5060)) client.run();
    return 0;
}
