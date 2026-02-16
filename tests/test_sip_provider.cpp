#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <sstream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>

static std::atomic<bool> g_running{true};

void sig_handler(int) { g_running = false; }

struct RegisteredUser {
    std::string username;
    std::string ip;
    int sip_port;
    std::chrono::steady_clock::time_point registered_at;
};

struct CallLeg {
    std::string user;
    std::string sip_call_id;
    std::string ip;
    int sip_port;
    int client_rtp_port;
    int relay_port;
    int relay_sock;
    bool answered;
};

struct ActiveCall {
    int id;
    CallLeg leg_a;
    CallLeg leg_b;
    std::thread relay_a_to_b;
    std::thread relay_b_to_a;
    std::thread inject_thread;
    std::atomic<bool> active{true};
    std::atomic<uint64_t> pkts_a_to_b{0};
    std::atomic<uint64_t> pkts_b_to_a{0};
    std::atomic<uint64_t> bytes_a_to_b{0};
    std::atomic<uint64_t> bytes_b_to_a{0};
    std::chrono::steady_clock::time_point started_at;
};

static uint8_t linear_to_ulaw(int16_t sample) {
    const int BIAS = 0x84;
    const int CLIP = 32635;
    int sign = (sample >> 8) & 0x80;
    if (sign) sample = -sample;
    if (sample > CLIP) sample = CLIP;
    sample += BIAS;
    int exponent = 7;
    for (int mask = 0x4000; mask > 0; mask >>= 1, exponent--) {
        if (sample & mask) break;
    }
    int mantissa = (sample >> (exponent + 3)) & 0x0F;
    return ~(sign | (exponent << 4) | mantissa);
}

class TestSipProvider {
public:
    bool init(int sip_port, const std::string& local_ip) {
        local_ip_ = local_ip;
        sip_port_ = sip_port;

        sip_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sip_sock_ < 0) {
            std::cerr << "Failed to create SIP socket\n";
            return false;
        }

        int reuse = 1;
        setsockopt(sip_sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(sip_port);

        if (bind(sip_sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Failed to bind SIP port " << sip_port << "\n";
            close(sip_sock_);
            return false;
        }

        std::printf("Test SIP Provider listening on %s:%d\n", local_ip.c_str(), sip_port);
        return true;
    }

    void run(int duration_sec, bool inject_tone) {
        duration_ = duration_sec;
        inject_tone_ = inject_tone;

        std::printf("Waiting for 2 SIP clients to register...\n");

        while (g_running) {
            char buf[4096];
            struct sockaddr_in sender{};
            socklen_t slen = sizeof(sender);
            struct timeval tv{1, 0};
            setsockopt(sip_sock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            ssize_t n = recvfrom(sip_sock_, buf, sizeof(buf) - 1, 0,
                                 (struct sockaddr*)&sender, &slen);
            if (n > 0) {
                buf[n] = '\0';
                std::string msg(buf, n);
                handle_sip_message(msg, sender);
            }

            {
                std::lock_guard<std::mutex> lock(users_mutex_);
                if (users_.size() >= 2 && !call_initiated_) {
                    initiate_call();
                }
            }

            if (call_ && !call_->active) break;
        }

        print_results();
        shutdown_call();
        close(sip_sock_);
    }

private:
    std::string get_header(const std::string& msg, const std::string& name) {
        std::string search = name + ":";
        size_t p = msg.find(search);
        if (p == std::string::npos) return "";
        size_t start = p + search.length();
        while (start < msg.size() && msg[start] == ' ') start++;
        size_t end = msg.find("\r\n", start);
        if (end == std::string::npos) end = msg.find("\n", start);
        if (end == std::string::npos) end = msg.size();
        return msg.substr(start, end - start);
    }

    void handle_sip_message(const std::string& msg, const struct sockaddr_in& sender) {
        if (msg.find("REGISTER") == 0) {
            handle_register(msg, sender);
        } else if (msg.find("SIP/2.0 200") == 0) {
            handle_200_ok(msg, sender);
        } else if (msg.find("BYE") == 0) {
            handle_bye(msg, sender);
        }
    }

    void handle_register(const std::string& msg, const struct sockaddr_in& sender) {
        std::string from = get_header(msg, "From");
        std::string contact = get_header(msg, "Contact");
        std::string call_id = get_header(msg, "Call-ID");

        std::string username;
        size_t sip_pos = from.find("sip:");
        if (sip_pos != std::string::npos) {
            size_t at_pos = from.find("@", sip_pos);
            if (at_pos != std::string::npos) {
                username = from.substr(sip_pos + 4, at_pos - sip_pos - 4);
            }
        }

        std::string sender_ip = inet_ntoa(sender.sin_addr);
        int sender_port = ntohs(sender.sin_port);

        {
            std::lock_guard<std::mutex> lock(users_mutex_);
            users_[username] = {username, sender_ip, sender_port,
                                std::chrono::steady_clock::now()};
        }

        std::ostringstream resp;
        resp << "SIP/2.0 200 OK\r\n";
        resp << "From: " << from << "\r\n";
        resp << "Call-ID: " << call_id << "\r\n";
        resp << "CSeq: 1 REGISTER\r\n";
        resp << "Contact: " << contact << "\r\n";
        resp << "Expires: 3600\r\n\r\n";

        std::string s = resp.str();
        sendto(sip_sock_, s.c_str(), s.length(), 0,
               (struct sockaddr*)&sender, sizeof(sender));

        std::printf("REGISTER: %s (%s:%d)\n", username.c_str(),
                    sender_ip.c_str(), sender_port);
    }

    int create_relay_socket(int& bound_port) {
        int sock = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = 0;
        bind(sock, (struct sockaddr*)&addr, sizeof(addr));
        socklen_t alen = sizeof(addr);
        getsockname(sock, (struct sockaddr*)&addr, &alen);
        bound_port = ntohs(addr.sin_port);
        return sock;
    }

    void initiate_call() {
        call_initiated_ = true;

        auto it = users_.begin();
        auto& ua = it->second; ++it;
        auto& ub = it->second;

        call_ = std::make_shared<ActiveCall>();
        call_->id = 1;
        call_->started_at = std::chrono::steady_clock::now();

        call_->leg_a.user = ua.username;
        call_->leg_a.sip_call_id = "call-" + std::to_string(call_->id) + "-a";
        call_->leg_a.ip = ua.ip;
        call_->leg_a.sip_port = ua.sip_port;
        call_->leg_a.answered = false;
        call_->leg_a.client_rtp_port = 0;
        call_->leg_a.relay_sock = create_relay_socket(call_->leg_a.relay_port);

        call_->leg_b.user = ub.username;
        call_->leg_b.sip_call_id = "call-" + std::to_string(call_->id) + "-b";
        call_->leg_b.ip = ub.ip;
        call_->leg_b.sip_port = ub.sip_port;
        call_->leg_b.answered = false;
        call_->leg_b.client_rtp_port = 0;
        call_->leg_b.relay_sock = create_relay_socket(call_->leg_b.relay_port);

        send_invite(call_->leg_a, ub.username);
        send_invite(call_->leg_b, ua.username);

        std::printf("INVITE sent to %s (relay %d) and %s (relay %d)\n",
                    ua.username.c_str(), call_->leg_a.relay_port,
                    ub.username.c_str(), call_->leg_b.relay_port);
    }

    void send_invite(CallLeg& leg, const std::string& caller) {
        std::ostringstream sdp;
        sdp << "v=0\r\no=provider 1 1 IN IP4 " << local_ip_ << "\r\n";
        sdp << "s=WhisperTalk Test\r\nc=IN IP4 " << local_ip_ << "\r\nt=0 0\r\n";
        sdp << "m=audio " << leg.relay_port << " RTP/AVP 0 101\r\n";
        sdp << "a=rtpmap:0 PCMU/8000\r\na=rtpmap:101 telephone-event/8000\r\n";
        std::string sdp_str = sdp.str();

        std::ostringstream inv;
        inv << "INVITE sip:" << leg.user << "@" << leg.ip << " SIP/2.0\r\n";
        inv << "Via: SIP/2.0/UDP " << local_ip_ << ":" << sip_port_ << "\r\n";
        inv << "From: <sip:" << caller << "@" << local_ip_ << ">;tag=prov" << leg.sip_call_id << "\r\n";
        inv << "To: <sip:" << leg.user << "@" << leg.ip << ">\r\n";
        inv << "Call-ID: " << leg.sip_call_id << "\r\n";
        inv << "CSeq: 1 INVITE\r\n";
        inv << "Contact: <sip:provider@" << local_ip_ << ":" << sip_port_ << ">\r\n";
        inv << "Content-Type: application/sdp\r\n";
        inv << "Content-Length: " << sdp_str.length() << "\r\n\r\n";
        inv << sdp_str;

        struct sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(leg.sip_port);
        dest.sin_addr.s_addr = inet_addr(leg.ip.c_str());

        std::string s = inv.str();
        sendto(sip_sock_, s.c_str(), s.length(), 0,
               (struct sockaddr*)&dest, sizeof(dest));
    }

    void handle_200_ok(const std::string& msg, const struct sockaddr_in&) {
        std::string call_id = get_header(msg, "Call-ID");
        if (!call_) return;

        std::lock_guard<std::mutex> lock(call_mutex_);
        CallLeg* leg = nullptr;
        if (call_->leg_a.sip_call_id == call_id) leg = &call_->leg_a;
        else if (call_->leg_b.sip_call_id == call_id) leg = &call_->leg_b;
        if (!leg || leg->answered) return;

        size_t m_pos = msg.find("m=audio ");
        if (m_pos != std::string::npos) {
            std::string m_line = msg.substr(m_pos + 8);
            leg->client_rtp_port = std::stoi(m_line.substr(0, m_line.find(' ')));
        }
        leg->answered = true;

        std::printf("200 OK from %s (RTP port %d)\n",
                    leg->user.c_str(), leg->client_rtp_port);

        if (call_->leg_a.answered && call_->leg_b.answered) {
            start_relay();
        }
    }

    void start_relay() {
        std::printf("\n=== Both legs answered — starting RTP relay ===\n");
        std::printf("  Leg A: %s RTP %d <-> relay %d\n",
                    call_->leg_a.user.c_str(), call_->leg_a.client_rtp_port,
                    call_->leg_a.relay_port);
        std::printf("  Leg B: %s RTP %d <-> relay %d\n",
                    call_->leg_b.user.c_str(), call_->leg_b.client_rtp_port,
                    call_->leg_b.relay_port);

        call_->relay_a_to_b = std::thread(&TestSipProvider::relay_thread, this,
            call_, &call_->leg_a, &call_->leg_b, &call_->pkts_a_to_b, &call_->bytes_a_to_b);
        call_->relay_b_to_a = std::thread(&TestSipProvider::relay_thread, this,
            call_, &call_->leg_b, &call_->leg_a, &call_->pkts_b_to_a, &call_->bytes_b_to_a);

        if (inject_tone_) {
            call_->inject_thread = std::thread(&TestSipProvider::inject_audio, this, call_);
        }

        stats_thread_ = std::thread(&TestSipProvider::stats_loop, this, call_);

        end_thread_ = std::thread([this]() {
            std::this_thread::sleep_for(std::chrono::seconds(duration_));
            if (g_running && call_ && call_->active) {
                std::printf("\nCall duration (%ds) expired — ending call\n", duration_);
                call_->active = false;
            }
        });
    }

    void relay_thread(std::shared_ptr<ActiveCall> call,
                      CallLeg* from_leg, CallLeg* to_leg,
                      std::atomic<uint64_t>* pkt_count,
                      std::atomic<uint64_t>* byte_count) {
        char buf[2048];
        struct sockaddr_in sender{};
        socklen_t slen;

        struct sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(to_leg->client_rtp_port);
        dest.sin_addr.s_addr = inet_addr(to_leg->ip.c_str());

        while (call->active && g_running) {
            slen = sizeof(sender);
            struct timeval tv{0, 100000};
            setsockopt(from_leg->relay_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            ssize_t n = recvfrom(from_leg->relay_sock, buf, sizeof(buf), 0,
                                 (struct sockaddr*)&sender, &slen);
            if (n > 0) {
                sendto(to_leg->relay_sock, buf, n, 0,
                       (struct sockaddr*)&dest, sizeof(dest));
                (*pkt_count)++;
                (*byte_count) += n;
            }
        }
    }

    void inject_audio(std::shared_ptr<ActiveCall> call) {
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

        const int SAMPLE_RATE = 8000;
        const int PKT_SAMPLES = 160;
        const double FREQ = 400.0;
        const double AMP = 8000.0;
        const int INJECT_SECONDS = 3;

        uint16_t seq = 0;
        uint32_t ts = 0;
        uint32_t ssrc = 0xDEAD0001;
        int sample_idx = 0;

        int total_pkts = (INJECT_SECONDS * SAMPLE_RATE) / PKT_SAMPLES;

        struct sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(call->leg_a.client_rtp_port);
        dest.sin_addr.s_addr = inet_addr(call->leg_a.ip.c_str());

        std::printf("Injecting %ds test tone (%.0fHz) into leg A (%s)\n",
                    INJECT_SECONDS, FREQ, call->leg_a.user.c_str());

        for (int p = 0; p < total_pkts && call->active && g_running; p++) {
            uint8_t rtp[12 + PKT_SAMPLES];
            rtp[0] = 0x80;
            rtp[1] = 0x00;
            uint16_t seq_n = htons(seq++);
            memcpy(rtp + 2, &seq_n, 2);
            uint32_t ts_n = htonl(ts); ts += PKT_SAMPLES;
            memcpy(rtp + 4, &ts_n, 4);
            uint32_t ssrc_n = htonl(ssrc);
            memcpy(rtp + 8, &ssrc_n, 4);

            for (int i = 0; i < PKT_SAMPLES; i++) {
                double t = (double)(sample_idx++) / SAMPLE_RATE;
                int16_t s = (int16_t)(AMP * std::sin(2.0 * M_PI * FREQ * t));
                rtp[12 + i] = linear_to_ulaw(s);
            }

            sendto(call->leg_a.relay_sock, rtp, sizeof(rtp), 0,
                   (struct sockaddr*)&dest, sizeof(dest));

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        std::printf("Audio injection complete\n");
    }

    void stats_loop(std::shared_ptr<ActiveCall> call) {
        auto start = std::chrono::steady_clock::now();
        while (call->active && g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!call->active || !g_running) break;
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
            std::printf("[%llds] Relay: A->B %llu pkts (%llu KB) | B->A %llu pkts (%llu KB)\n",
                        elapsed,
                        call->pkts_a_to_b.load(), call->bytes_a_to_b.load() / 1024,
                        call->pkts_b_to_a.load(), call->bytes_b_to_a.load() / 1024);
        }
    }

    void handle_bye(const std::string& msg, const struct sockaddr_in& sender) {
        std::string call_id = get_header(msg, "Call-ID");
        std::string resp = "SIP/2.0 200 OK\r\nCall-ID: " + call_id + "\r\n\r\n";
        sendto(sip_sock_, resp.c_str(), resp.length(), 0,
               (struct sockaddr*)&sender, sizeof(sender));

        if (call_) {
            std::printf("BYE received for %s — ending call\n", call_id.c_str());
            call_->active = false;
        }
    }

    void send_bye_to_leg(const CallLeg& leg) {
        std::ostringstream bye;
        bye << "BYE sip:" << leg.user << "@" << leg.ip << " SIP/2.0\r\n";
        bye << "Via: SIP/2.0/UDP " << local_ip_ << ":" << sip_port_ << "\r\n";
        bye << "Call-ID: " << leg.sip_call_id << "\r\n";
        bye << "CSeq: 2 BYE\r\n\r\n";

        struct sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(leg.sip_port);
        dest.sin_addr.s_addr = inet_addr(leg.ip.c_str());

        std::string s = bye.str();
        sendto(sip_sock_, s.c_str(), s.length(), 0,
               (struct sockaddr*)&dest, sizeof(dest));
    }

    void print_results() {
        if (!call_) {
            std::printf("\nNo call was established.\n");
            return;
        }

        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - call_->started_at).count();

        std::printf("\n");
        std::printf("=========================================\n");
        std::printf("   Test SIP Provider — Call Results\n");
        std::printf("=========================================\n");
        std::printf("  Duration:     %llds\n", elapsed);
        std::printf("  Leg A:        %s (RTP %d)\n",
                    call_->leg_a.user.c_str(), call_->leg_a.client_rtp_port);
        std::printf("  Leg B:        %s (RTP %d)\n",
                    call_->leg_b.user.c_str(), call_->leg_b.client_rtp_port);
        std::printf("-----------------------------------------\n");
        std::printf("  A -> B:       %llu packets  (%llu KB)\n",
                    call_->pkts_a_to_b.load(), call_->bytes_a_to_b.load() / 1024);
        std::printf("  B -> A:       %llu packets  (%llu KB)\n",
                    call_->pkts_b_to_a.load(), call_->bytes_b_to_a.load() / 1024);
        std::printf("-----------------------------------------\n");

        bool a_to_b_ok = call_->pkts_a_to_b > 0;
        bool b_to_a_ok = call_->pkts_b_to_a > 0;

        if (a_to_b_ok && b_to_a_ok) {
            std::printf("  Result:       PASS (bidirectional audio)\n");
        } else if (a_to_b_ok || b_to_a_ok) {
            std::printf("  Result:       PARTIAL (unidirectional only)\n");
        } else {
            std::printf("  Result:       FAIL (no audio flow)\n");
        }
        std::printf("=========================================\n\n");
    }

    void shutdown_call() {
        if (!call_) return;

        if (call_->active) {
            send_bye_to_leg(call_->leg_a);
            send_bye_to_leg(call_->leg_b);
            call_->active = false;
        }

        if (call_->relay_a_to_b.joinable()) call_->relay_a_to_b.join();
        if (call_->relay_b_to_a.joinable()) call_->relay_b_to_a.join();
        if (call_->inject_thread.joinable()) call_->inject_thread.join();
        if (stats_thread_.joinable()) stats_thread_.join();
        if (end_thread_.joinable()) end_thread_.join();

        close(call_->leg_a.relay_sock);
        close(call_->leg_b.relay_sock);
    }

    int sip_sock_ = -1;
    int sip_port_ = 5060;
    std::string local_ip_;
    int duration_ = 30;
    bool inject_tone_ = false;
    bool call_initiated_ = false;

    std::mutex users_mutex_;
    std::map<std::string, RegisteredUser> users_;

    std::mutex call_mutex_;
    std::shared_ptr<ActiveCall> call_;
    std::thread stats_thread_;
    std::thread end_thread_;
};

int main(int argc, char* argv[]) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int port = 5060;
    int duration = 30;
    bool inject = false;
    std::string ip = "127.0.0.1";

    static struct option long_opts[] = {
        {"port",     required_argument, 0, 'p'},
        {"duration", required_argument, 0, 'd'},
        {"inject",   no_argument,       0, 'i'},
        {"ip",       required_argument, 0, 'b'},
        {"help",     no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:d:ib:h", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'p': port = atoi(optarg); break;
            case 'd': duration = atoi(optarg); break;
            case 'i': inject = true; break;
            case 'b': ip = optarg; break;
            case 'h':
                std::printf("Usage: test_sip_provider [OPTIONS]\n\n");
                std::printf("  -p, --port PORT       SIP listen port (default: 5060)\n");
                std::printf("  -d, --duration SECS   Call duration in seconds (default: 30)\n");
                std::printf("  -i, --inject          Inject 3s test tone into leg A\n");
                std::printf("  -b, --ip ADDR         Local IP address (default: 127.0.0.1)\n");
                std::printf("  -h, --help            Show this help\n\n");
                std::printf("Example:\n");
                std::printf("  # Terminal 1: Start provider\n");
                std::printf("  test_sip_provider --port 5060 --duration 60 --inject\n\n");
                std::printf("  # Terminal 2: Start pipeline A\n");
                std::printf("  sip-client alice 127.0.0.1 5060\n\n");
                std::printf("  # Terminal 3: Start pipeline B\n");
                std::printf("  sip-client bob 127.0.0.1 5060\n\n");
                return 0;
            default: break;
        }
    }

    std::printf("Test SIP Provider (B2BUA)\n");
    std::printf("  Port:     %d\n", port);
    std::printf("  Duration: %ds\n", duration);
    std::printf("  Inject:   %s\n", inject ? "yes (3s 400Hz tone)" : "no");
    std::printf("\n");

    TestSipProvider provider;
    if (!provider.init(port, ip)) return 1;
    provider.run(duration, inject);

    return 0;
}
