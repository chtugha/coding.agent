#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <cerrno>
#include <sstream>
#include <fstream>
#include <random>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <getopt.h>
#include <signal.h>
#include <dirent.h>
#include "mongoose.h"

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
    std::atomic<bool> relay_started{false};
    std::atomic<bool> injecting{false};
    std::string injecting_file;
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

struct WavData {
    std::vector<int16_t> samples;
    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    std::string error;
};

static WavData load_wav_file(const std::string& path) {
    WavData result;

    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        result.error = "File not found: " + path;
        return result;
    }
    if (st.st_size > 50 * 1024 * 1024) {
        result.error = "File too large (>50MB): " + path;
        return result;
    }

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        result.error = "Cannot open file: " + path;
        return result;
    }

    char riff_header[12];
    f.read(riff_header, 12);
    if (!f || std::memcmp(riff_header, "RIFF", 4) != 0 || std::memcmp(riff_header + 8, "WAVE", 4) != 0) {
        result.error = "Invalid RIFF/WAVE header";
        return result;
    }

    uint16_t audio_format = 0, num_channels = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0;
    std::vector<uint8_t> raw_data;
    bool got_fmt = false, got_data = false;

    while (f && !got_data) {
        char chunk_id[4];
        uint32_t chunk_size;
        f.read(chunk_id, 4);
        f.read(reinterpret_cast<char*>(&chunk_size), 4);
        if (!f) break;

        if (std::memcmp(chunk_id, "fmt ", 4) == 0) {
            if (chunk_size < 16) { result.error = "fmt chunk too small"; return result; }
            f.read(reinterpret_cast<char*>(&audio_format), 2);
            f.read(reinterpret_cast<char*>(&num_channels), 2);
            f.read(reinterpret_cast<char*>(&sample_rate), 4);
            f.seekg(6, std::ios::cur);
            f.read(reinterpret_cast<char*>(&bits_per_sample), 2);
            if (chunk_size > 16) f.seekg(chunk_size - 16, std::ios::cur);
            got_fmt = true;
        } else if (std::memcmp(chunk_id, "data", 4) == 0) {
            if (!got_fmt) { result.error = "data chunk before fmt chunk"; return result; }
            raw_data.resize(chunk_size);
            f.read(reinterpret_cast<char*>(raw_data.data()), chunk_size);
            got_data = true;
        } else {
            f.seekg(chunk_size, std::ios::cur);
        }
    }

    if (!got_fmt || !got_data) {
        result.error = "Missing fmt or data chunk";
        return result;
    }

    if (audio_format != 1 && audio_format != 3) {
        result.error = "Unsupported audio format: " + std::to_string(audio_format) + " (only PCM=1 and IEEE float=3 supported)";
        return result;
    }

    result.sample_rate = sample_rate;
    result.channels = num_channels;

    size_t frame_size = num_channels * (bits_per_sample / 8);
    if (frame_size == 0) { result.error = "Invalid bits_per_sample"; return result; }
    size_t num_frames = raw_data.size() / frame_size;
    result.samples.reserve(num_frames);

    for (size_t i = 0; i < num_frames; i++) {
        const uint8_t* frame = raw_data.data() + i * frame_size;
        int32_t mono_sum = 0;

        for (uint16_t ch = 0; ch < num_channels; ch++) {
            const uint8_t* s = frame + ch * (bits_per_sample / 8);
            int32_t val = 0;

            if (audio_format == 1 && bits_per_sample == 16) {
                val = static_cast<int16_t>(s[0] | (s[1] << 8));
            } else if (audio_format == 1 && bits_per_sample == 24) {
                val = static_cast<int32_t>((s[0] << 8) | (s[1] << 16) | (s[2] << 24)) >> 16;
            } else if (audio_format == 3 && bits_per_sample == 32) {
                float fval;
                std::memcpy(&fval, s, 4);
                val = static_cast<int32_t>(fval * 32767.0f);
                if (val > 32767) val = 32767;
                if (val < -32768) val = -32768;
            } else {
                result.error = "Unsupported format/bits: " + std::to_string(audio_format) + "/" + std::to_string(bits_per_sample);
                result.samples.clear();
                return result;
            }
            mono_sum += val;
        }

        result.samples.push_back(static_cast<int16_t>(mono_sum / num_channels));
    }

    return result;
}

static std::vector<int16_t> resample_to_8khz(const std::vector<int16_t>& samples, uint32_t source_rate) {
    if (source_rate == 8000) return samples;

    if (source_rate == 16000) {
        std::vector<int16_t> out;
        out.reserve(samples.size() / 2);
        for (size_t i = 0; i + 1 < samples.size(); i += 2) {
            out.push_back(static_cast<int16_t>((static_cast<int32_t>(samples[i]) + samples[i + 1]) / 2));
        }
        return out;
    }

    static constexpr int FILTER_TAPS = 15;
    static constexpr int HALF_TAPS = FILTER_TAPS / 2;
    double cutoff = 3400.0 / (source_rate / 2.0);

    double coeffs[FILTER_TAPS];
    double sum = 0;
    for (int n = 0; n < FILTER_TAPS; n++) {
        int k = n - HALF_TAPS;
        double hamming = 0.54 - 0.46 * std::cos(2.0 * M_PI * n / (FILTER_TAPS - 1));
        double sinc_val = (k == 0) ? 1.0 : std::sin(M_PI * cutoff * k) / (M_PI * k);
        coeffs[n] = sinc_val * hamming * cutoff;
        sum += coeffs[n];
    }
    for (int n = 0; n < FILTER_TAPS; n++) coeffs[n] /= sum;

    double ratio = static_cast<double>(source_rate) / 8000.0;
    size_t out_len = static_cast<size_t>(samples.size() / ratio);
    std::vector<int16_t> out(out_len);

    for (size_t i = 0; i < out_len; i++) {
        double src_pos = i * ratio;
        double filtered = 0;

        for (int t = 0; t < FILTER_TAPS; t++) {
            int idx = static_cast<int>(src_pos) - HALF_TAPS + t;
            if (idx >= 0 && idx < static_cast<int>(samples.size())) {
                filtered += samples[idx] * coeffs[t];
            }
        }

        int32_t clamped = static_cast<int32_t>(filtered);
        if (clamped > 32767) clamped = 32767;
        if (clamped < -32768) clamped = -32768;
        out[i] = static_cast<int16_t>(clamped);
    }

    return out;
}

static const char* CORS_HEADERS = "Content-Type: application/json\r\nAccess-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET, POST, OPTIONS\r\nAccess-Control-Allow-Headers: Content-Type\r\n";

class TestSipProvider {
public:
    bool init(int sip_port, const std::string& local_ip, const std::string& testfiles_dir, int http_port) {
        local_ip_ = local_ip;
        sip_port_ = sip_port;
        testfiles_dir_ = testfiles_dir;
        http_port_ = http_port;

        sip_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
        if (sip_sock_ < 0) {
            std::fprintf(stderr, "Failed to create SIP socket: %s\n", strerror(errno));
            return false;
        }

        int reuse = 1;
        setsockopt(sip_sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(sip_port);

        if (bind(sip_sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::fprintf(stderr, "Failed to bind SIP port %d: %s\n", sip_port, strerror(errno));
            close(sip_sock_);
            sip_sock_ = -1;
            return false;
        }

        mg_mgr_init(&mgr_);
        std::string listen_addr = "http://0.0.0.0:" + std::to_string(http_port);
        struct mg_connection* c = mg_http_listen(&mgr_, listen_addr.c_str(), http_handler_static, this);
        if (!c) {
            std::fprintf(stderr, "Failed to start HTTP server on port %d\n", http_port);
            close(sip_sock_);
            sip_sock_ = -1;
            return false;
        }

        std::printf("Test SIP Provider listening on %s:%d (HTTP: %d, Testfiles: %s)\n",
                    local_ip.c_str(), sip_port, http_port, testfiles_dir.c_str());
        return true;
    }

    void run() {
        std::printf("Waiting for SIP clients to register...\n");

        while (g_running) {
            mg_mgr_poll(&mgr_, 1);

            char buf[4096];
            struct sockaddr_in sender{};
            socklen_t slen = sizeof(sender);
            struct timeval tv{0, 1000};
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
        }

        print_results();
        shutdown_call();
        mg_mgr_free(&mgr_);
        if (sip_sock_ >= 0) close(sip_sock_);
    }

private:
    static void http_handler_static(struct mg_connection* c, int ev, void* ev_data) {
        if (ev == MG_EV_HTTP_MSG) {
            TestSipProvider* self = static_cast<TestSipProvider*>(c->fn_data);
            self->http_handler(c, static_cast<struct mg_http_message*>(ev_data));
        }
    }

    void http_handler(struct mg_connection* c, struct mg_http_message* hm) {
        if (mg_strcmp(hm->method, mg_str("OPTIONS")) == 0) {
            mg_http_reply(c, 204, CORS_HEADERS, "");
            return;
        }

        if (mg_strcmp(hm->uri, mg_str("/files")) == 0) {
            handle_files(c);
        } else if (mg_strcmp(hm->uri, mg_str("/inject")) == 0) {
            handle_inject(c, hm);
        } else if (mg_strcmp(hm->uri, mg_str("/status")) == 0) {
            handle_status(c);
        } else {
            mg_http_reply(c, 404, CORS_HEADERS, "{\"error\":\"Not found\"}");
        }
    }

    void handle_files(struct mg_connection* c) {
        DIR* dir = opendir(testfiles_dir_.c_str());
        if (!dir) {
            mg_http_reply(c, 500, CORS_HEADERS, "{\"error\":\"Cannot open Testfiles directory\"}");
            return;
        }

        std::ostringstream json;
        json << "{\"files\":[";
        bool first = true;
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            std::string name = entry->d_name;
            if (name.size() > 4 && name.substr(name.size() - 4) == ".wav") {
                struct stat st;
                std::string full = testfiles_dir_ + "/" + name;
                if (stat(full.c_str(), &st) == 0) {
                    if (!first) json << ",";
                    json << "{\"name\":\"" << name << "\",\"size_bytes\":" << st.st_size << "}";
                    first = false;
                }
            }
        }
        closedir(dir);
        json << "]}";
        mg_http_reply(c, 200, CORS_HEADERS, "%s", json.str().c_str());
    }

    void handle_inject(struct mg_connection* c, struct mg_http_message* hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 400, CORS_HEADERS, "{\"error\":\"POST required\"}");
            return;
        }

        char* file_str = mg_json_get_str(hm->body, "$.file");
        char* leg_str = mg_json_get_str(hm->body, "$.leg");

        if (!file_str || !leg_str) {
            free(file_str);
            free(leg_str);
            mg_http_reply(c, 400, CORS_HEADERS, "{\"error\":\"Missing 'file' or 'leg' in JSON body\"}");
            return;
        }

        std::string file(file_str);
        std::string leg(leg_str);
        free(file_str);
        free(leg_str);

        if (leg != "a" && leg != "b") {
            mg_http_reply(c, 400, CORS_HEADERS, "{\"error\":\"Invalid leg '%s', must be 'a' or 'b'\"}", leg.c_str());
            return;
        }

        if (!call_ || !call_->active || !call_->relay_started) {
            mg_http_reply(c, 409, CORS_HEADERS, "{\"error\":\"No active call to inject into\"}");
            return;
        }

        std::string full_path = testfiles_dir_ + "/" + file;
        struct stat st;
        if (stat(full_path.c_str(), &st) != 0) {
            mg_http_reply(c, 404, CORS_HEADERS, "{\"error\":\"File not found: %s\"}", file.c_str());
            return;
        }

        std::string err = inject_file(full_path, leg);
        if (!err.empty()) {
            mg_http_reply(c, 500, CORS_HEADERS, "{\"error\":\"%s\"}", err.c_str());
            return;
        }

        call_->injecting_file = file;
        mg_http_reply(c, 200, CORS_HEADERS, "{\"success\":true,\"injecting\":\"%s\",\"leg\":\"%s\"}", file.c_str(), leg.c_str());
    }

    void handle_status(struct mg_connection* c) {
        std::ostringstream json;
        json << "{\"call_active\":" << (call_ && call_->active ? "true" : "false");
        if (call_) {
            json << ",\"relay_stats\":{";
            json << "\"pkts_a_to_b\":" << call_->pkts_a_to_b.load();
            json << ",\"pkts_b_to_a\":" << call_->pkts_b_to_a.load();
            json << ",\"bytes_a_to_b\":" << call_->bytes_a_to_b.load();
            json << ",\"bytes_b_to_a\":" << call_->bytes_b_to_a.load();
            json << "}";
            if (call_->injecting) {
                json << ",\"injecting\":\"" << call_->injecting_file << "\"";
            } else {
                json << ",\"injecting\":null";
            }
        }
        json << "}";
        mg_http_reply(c, 200, CORS_HEADERS, "%s", json.str().c_str());
    }

    std::string inject_file(const std::string& path, const std::string& leg) {
        WavData wav = load_wav_file(path);
        if (!wav.error.empty()) return wav.error;
        if (wav.samples.empty()) return "WAV file contains no samples";

        std::vector<int16_t> resampled = resample_to_8khz(wav.samples, wav.sample_rate);
        std::vector<uint8_t> ulaw(resampled.size());
        for (size_t i = 0; i < resampled.size(); i++) {
            ulaw[i] = linear_to_ulaw(resampled[i]);
        }

        cancel_injection();

        call_->injecting = true;
        call_->inject_thread = std::thread(&TestSipProvider::inject_rtp_stream, this, call_, leg, std::move(ulaw));
        return "";
    }

    void cancel_injection() {
        if (!call_) return;
        call_->injecting = false;
        if (call_->inject_thread.joinable()) {
            call_->inject_thread.join();
        }
    }

    void inject_rtp_stream(std::shared_ptr<ActiveCall> call, std::string leg, std::vector<uint8_t> ulaw_samples) {
        static constexpr int PKT_SAMPLES = 160;

        CallLeg* target = (leg == "a") ? &call->leg_a : &call->leg_b;

        struct sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(target->client_rtp_port);
        dest.sin_addr.s_addr = inet_addr(target->ip.c_str());

        std::random_device rd;
        uint32_t ssrc = rd();
        uint16_t seq = 0;
        uint32_t ts = 0;

        size_t total_pkts = ulaw_samples.size() / PKT_SAMPLES;
        std::printf("Injecting %zu RTP packets (%.1fs) into leg %s (%s)\n",
                    total_pkts, ulaw_samples.size() / 8000.0, leg.c_str(), target->user.c_str());

        for (size_t offset = 0; offset + PKT_SAMPLES <= ulaw_samples.size() && call->active && call->injecting && g_running; offset += PKT_SAMPLES) {
            uint8_t rtp[12 + PKT_SAMPLES];
            rtp[0] = 0x80;
            rtp[1] = 0x00;
            uint16_t seq_n = htons(seq++);
            std::memcpy(rtp + 2, &seq_n, 2);
            uint32_t ts_n = htonl(ts);
            ts += PKT_SAMPLES;
            std::memcpy(rtp + 4, &ts_n, 4);
            uint32_t ssrc_n = htonl(ssrc);
            std::memcpy(rtp + 8, &ssrc_n, 4);
            std::memcpy(rtp + 12, ulaw_samples.data() + offset, PKT_SAMPLES);

            sendto(target->relay_sock, rtp, sizeof(rtp), 0,
                   (struct sockaddr*)&dest, sizeof(dest));

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        std::printf("Audio done, sending silence until next injection or call end\n");
        while (call->active && call->injecting && g_running) {
            uint8_t rtp[12 + PKT_SAMPLES];
            rtp[0] = 0x80;
            rtp[1] = 0x00;
            uint16_t seq_n = htons(seq++);
            std::memcpy(rtp + 2, &seq_n, 2);
            uint32_t ts_n = htonl(ts);
            ts += PKT_SAMPLES;
            std::memcpy(rtp + 4, &ts_n, 4);
            uint32_t ssrc_n = htonl(ssrc);
            std::memcpy(rtp + 8, &ssrc_n, 4);
            std::memset(rtp + 12, 0xFF, PKT_SAMPLES);
            sendto(target->relay_sock, rtp, sizeof(rtp), 0,
                   (struct sockaddr*)&dest, sizeof(dest));
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        call->injecting_file.clear();
        std::printf("RTP stream stopped\n");
    }

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

    int get_sdp_media_port(const std::string& msg) {
        size_t m_pos = msg.find("m=audio ");
        if (m_pos == std::string::npos) return -1;
        std::string m_line = msg.substr(m_pos + 8);
        size_t sp = m_line.find(' ');
        if (sp == std::string::npos) sp = m_line.find('\r');
        if (sp == std::string::npos) return -1;
        try {
            int port = std::stoi(m_line.substr(0, sp));
            if (port < 1 || port > 65535) return -1;
            return port;
        } catch (...) {
            return -1;
        }
    }

    std::string get_sdp_connection_ip(const std::string& msg) {
        size_t c_pos = msg.find("c=IN IP4 ");
        if (c_pos == std::string::npos) return "";
        std::string c_line = msg.substr(c_pos + 9);
        size_t end = c_line.find_first_of("\r\n ");
        if (end == std::string::npos) end = c_line.size();
        return c_line.substr(0, end);
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

        if (username.empty()) {
            std::fprintf(stderr, "REGISTER: could not extract username from From header\n");
            return;
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
        if (sock < 0) {
            std::fprintf(stderr, "Failed to create relay socket: %s\n", strerror(errno));
            bound_port = -1;
            return -1;
        }
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = 0;
        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::fprintf(stderr, "Failed to bind relay socket: %s\n", strerror(errno));
            close(sock);
            bound_port = -1;
            return -1;
        }
        socklen_t alen = sizeof(addr);
        if (getsockname(sock, (struct sockaddr*)&addr, &alen) < 0) {
            std::fprintf(stderr, "getsockname failed: %s\n", strerror(errno));
            close(sock);
            bound_port = -1;
            return -1;
        }
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

        if (call_->leg_a.relay_sock < 0 || call_->leg_b.relay_sock < 0) {
            std::fprintf(stderr, "Failed to create relay sockets — aborting call\n");
            call_->active = false;
            return;
        }

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
        if (sendto(sip_sock_, s.c_str(), s.length(), 0,
                   (struct sockaddr*)&dest, sizeof(dest)) < 0) {
            std::fprintf(stderr, "Failed to send INVITE to %s: %s\n",
                        leg.user.c_str(), strerror(errno));
        }
    }

    void send_ack(const CallLeg& leg, const struct sockaddr_in& dest) {
        std::ostringstream ack;
        ack << "ACK sip:" << leg.user << "@" << leg.ip << " SIP/2.0\r\n";
        ack << "Via: SIP/2.0/UDP " << local_ip_ << ":" << sip_port_ << "\r\n";
        ack << "From: <sip:provider@" << local_ip_ << ">;tag=prov" << leg.sip_call_id << "\r\n";
        ack << "To: <sip:" << leg.user << "@" << leg.ip << ">\r\n";
        ack << "Call-ID: " << leg.sip_call_id << "\r\n";
        ack << "CSeq: 1 ACK\r\n\r\n";

        std::string s = ack.str();
        if (sendto(sip_sock_, s.c_str(), s.length(), 0,
                   (struct sockaddr*)&dest, sizeof(dest)) < 0) {
            std::fprintf(stderr, "Failed to send ACK to %s: %s\n",
                        leg.user.c_str(), strerror(errno));
        }
    }

    void handle_200_ok(const std::string& msg, const struct sockaddr_in& sender) {
        std::string call_id = get_header(msg, "Call-ID");
        if (!call_) return;

        std::lock_guard<std::mutex> lock(call_mutex_);
        CallLeg* leg = nullptr;
        if (call_->leg_a.sip_call_id == call_id) leg = &call_->leg_a;
        else if (call_->leg_b.sip_call_id == call_id) leg = &call_->leg_b;
        if (!leg || leg->answered) return;

        int rtp_port = get_sdp_media_port(msg);
        if (rtp_port > 0) {
            leg->client_rtp_port = rtp_port;
        } else {
            std::fprintf(stderr, "200 OK from %s: could not parse RTP port from SDP\n",
                        leg->user.c_str());
            return;
        }

        std::string sdp_ip = get_sdp_connection_ip(msg);
        if (!sdp_ip.empty()) {
            leg->ip = sdp_ip;
        }

        leg->answered = true;

        send_ack(*leg, sender);

        std::printf("200 OK from %s (RTP %s:%d) — ACK sent\n",
                    leg->user.c_str(), leg->ip.c_str(), leg->client_rtp_port);

        if (call_->leg_a.answered && call_->leg_b.answered && !call_->relay_started) {
            call_->relay_started = true;
            start_relay();
        }
    }

    void start_relay() {
        std::printf("\n=== Both legs answered — starting RTP relay ===\n");
        std::printf("  Leg A: %s RTP %s:%d <-> relay %d\n",
                    call_->leg_a.user.c_str(), call_->leg_a.ip.c_str(),
                    call_->leg_a.client_rtp_port, call_->leg_a.relay_port);
        std::printf("  Leg B: %s RTP %s:%d <-> relay %d\n",
                    call_->leg_b.user.c_str(), call_->leg_b.ip.c_str(),
                    call_->leg_b.client_rtp_port, call_->leg_b.relay_port);

        call_->relay_a_to_b = std::thread(&TestSipProvider::relay_thread, this,
            call_, &call_->leg_a, &call_->leg_b, &call_->pkts_a_to_b, &call_->bytes_a_to_b);
        call_->relay_b_to_a = std::thread(&TestSipProvider::relay_thread, this,
            call_, &call_->leg_b, &call_->leg_a, &call_->pkts_b_to_a, &call_->bytes_b_to_a);

        stats_thread_ = std::thread(&TestSipProvider::stats_loop, this, call_);
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

        int from_sock = from_leg->relay_sock;
        int to_sock = to_leg->relay_sock;

        while (call->active && g_running) {
            slen = sizeof(sender);
            struct timeval tv{0, 100000};
            setsockopt(from_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            ssize_t n = recvfrom(from_sock, buf, sizeof(buf), 0,
                                 (struct sockaddr*)&sender, &slen);
            if (n > 0) {
                sendto(to_sock, buf, n, 0,
                       (struct sockaddr*)&dest, sizeof(dest));
                (*pkt_count)++;
                (*byte_count) += n;
            }
        }
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
        std::printf("  Leg A:        %s (RTP %s:%d)\n",
                    call_->leg_a.user.c_str(), call_->leg_a.ip.c_str(),
                    call_->leg_a.client_rtp_port);
        std::printf("  Leg B:        %s (RTP %s:%d)\n",
                    call_->leg_b.user.c_str(), call_->leg_b.ip.c_str(),
                    call_->leg_b.client_rtp_port);
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

        cancel_injection();

        if (call_->active) {
            send_bye_to_leg(call_->leg_a);
            send_bye_to_leg(call_->leg_b);
            call_->active = false;
        }

        if (call_->relay_a_to_b.joinable()) call_->relay_a_to_b.join();
        if (call_->relay_b_to_a.joinable()) call_->relay_b_to_a.join();
        if (stats_thread_.joinable()) stats_thread_.join();

        if (call_->leg_a.relay_sock >= 0) close(call_->leg_a.relay_sock);
        if (call_->leg_b.relay_sock >= 0) close(call_->leg_b.relay_sock);
    }

    int sip_sock_ = -1;
    int sip_port_ = 5060;
    int http_port_ = 22011;
    std::string local_ip_;
    std::string testfiles_dir_;
    bool call_initiated_ = false;

    struct mg_mgr mgr_;

    std::mutex users_mutex_;
    std::map<std::string, RegisteredUser> users_;

    std::mutex call_mutex_;
    std::shared_ptr<ActiveCall> call_;
    std::thread stats_thread_;
};

int main(int argc, char* argv[]) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    int port = 5060;
    int http_port = 22011;
    std::string ip = "127.0.0.1";
    std::string testfiles_dir = "Testfiles";

    static struct option long_opts[] = {
        {"port",          required_argument, 0, 'p'},
        {"ip",            required_argument, 0, 'b'},
        {"http-port",     required_argument, 0, 'H'},
        {"testfiles-dir", required_argument, 0, 't'},
        {"help",          no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "p:b:H:t:h", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'p': port = atoi(optarg); break;
            case 'b': ip = optarg; break;
            case 'H': http_port = atoi(optarg); break;
            case 't': testfiles_dir = optarg; break;
            case 'h':
                std::printf("Usage: test_sip_provider [OPTIONS]\n\n");
                std::printf("  -p, --port PORT           SIP listen port (default: 5060)\n");
                std::printf("  -b, --ip ADDR             Local IP address (default: 127.0.0.1)\n");
                std::printf("  -H, --http-port PORT      HTTP control port (default: 22011)\n");
                std::printf("  -t, --testfiles-dir DIR   Testfiles directory (default: Testfiles)\n");
                std::printf("  -h, --help                Show this help\n\n");
                std::printf("HTTP Endpoints:\n");
                std::printf("  GET  /files    List WAV files in testfiles directory\n");
                std::printf("  POST /inject   Inject audio: {\"file\":\"sample_01.wav\",\"leg\":\"a\"}\n");
                std::printf("  GET  /status   Call status and relay stats\n\n");
                return 0;
            default: break;
        }
    }

    std::printf("Test SIP Provider (B2BUA)\n");
    std::printf("  SIP Port:     %d\n", port);
    std::printf("  HTTP Port:    %d\n", http_port);
    std::printf("  Testfiles:    %s\n", testfiles_dir.c_str());
    std::printf("\n");

    TestSipProvider provider;
    if (!provider.init(port, ip, testfiles_dir, http_port)) return 1;
    provider.run();

    return 0;
}
