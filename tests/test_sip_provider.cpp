
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

static constexpr int MAX_LINES = 20;

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
    int inject_sock;
    bool answered;
};

struct ActiveCall {
    int id;
    std::vector<CallLeg> legs;
    std::vector<std::thread> relay_threads;
    std::thread inject_thread;
    std::atomic<bool> active{true};
    std::atomic<bool> relay_started{false};
    std::atomic<bool> injecting{false};
    std::mutex inject_meta_mutex;
    std::string injecting_file;
    std::atomic<int> injecting_leg_idx{-1};
    std::atomic<uint64_t> total_pkts{0};
    std::atomic<uint64_t> total_bytes{0};
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

// TestSipProvider: a SIP B2BUA test provider supporting up to 20 lines.
//
// Lines register via SIP REGISTER. Calls are NOT auto-initiated.
// The frontend controls which lines participate in a call via:
//   - POST /conference {"users":["alice1","bob2",...]} — start conference between selected users
//   - POST /hangup — end current call
//   - GET  /users — list registered users
//   - POST /inject {"file":"sample.wav","leg":"alice1"} — inject audio into a specific leg
//   - GET  /status — call status and relay stats
//   - GET  /files — list WAV files in Testfiles directory
//   - GET  /calls — list active calls with per-leg detail
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

        std::printf("Test SIP Provider listening on %s:%d (HTTP: %d, Testfiles: %s, Max lines: %d)\n",
                    local_ip.c_str(), sip_port, http_port, testfiles_dir.c_str(), MAX_LINES);
        return true;
    }

    void run() {
        std::printf("Waiting for SIP clients to register (no auto-call — use /conference to start)...\n");

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
        } else if (mg_strcmp(hm->uri, mg_str("/stop_inject")) == 0) {
            handle_stop_inject(c);
        } else if (mg_strcmp(hm->uri, mg_str("/status")) == 0) {
            handle_status(c);
        } else if (mg_strcmp(hm->uri, mg_str("/calls")) == 0) {
            handle_calls(c);
        } else if (mg_strcmp(hm->uri, mg_str("/users")) == 0) {
            handle_users(c);
        } else if (mg_strcmp(hm->uri, mg_str("/conference")) == 0) {
            handle_conference(c, hm);
        } else if (mg_strcmp(hm->uri, mg_str("/hangup")) == 0) {
            handle_hangup(c);
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

    // GET /users — list all registered SIP users
    void handle_users(struct mg_connection* c) {
        std::lock_guard<std::mutex> lock(users_mutex_);
        std::ostringstream json;
        json << "{\"users\":[";
        bool first = true;
        for (const auto& kv : users_) {
            if (!first) json << ",";
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - kv.second.registered_at).count();
            json << "{\"username\":\"" << kv.second.username
                 << "\",\"ip\":\"" << kv.second.ip
                 << "\",\"port\":" << kv.second.sip_port
                 << ",\"registered_sec\":" << elapsed << "}";
            first = false;
        }
        json << "],\"max_lines\":" << MAX_LINES << "}";
        mg_http_reply(c, 200, CORS_HEADERS, "%s", json.str().c_str());
    }

    // POST /conference {"users":["alice1","bob2",...]}
    // Starts a conference call between the specified registered users (2-20).
    void handle_conference(struct mg_connection* c, struct mg_http_message* hm) {
        if (mg_strcmp(hm->method, mg_str("POST")) != 0) {
            mg_http_reply(c, 400, CORS_HEADERS, "{\"error\":\"POST required\"}");
            return;
        }

        if (call_ && call_->active) {
            mg_http_reply(c, 409, CORS_HEADERS, "{\"error\":\"A call is already active. Use /hangup first.\"}");
            return;
        }

        std::string body(hm->body.buf, hm->body.len);
        std::vector<std::string> requested_users;

        int idx = 0;
        while (true) {
            std::string path = "$[" + std::to_string(idx) + "]";
            char* val = mg_json_get_str(hm->body, path.c_str());
            if (!val) break;
            requested_users.push_back(val);
            free(val);
            idx++;
        }

        if (requested_users.empty()) {
            int toklen = 0;
            int arr_offset = mg_json_get(hm->body, "$.users", &toklen);
            if (arr_offset >= 0 && toklen > 0) {
                struct mg_str arr_str = mg_str_n(hm->body.buf + arr_offset, toklen);
                int i = 0;
                while (true) {
                    std::string p = "$[" + std::to_string(i) + "]";
                    char* v = mg_json_get_str(arr_str, p.c_str());
                    if (!v) break;
                    requested_users.push_back(v);
                    free(v);
                    i++;
                }
            }
        }

        if (requested_users.size() < 2) {
            mg_http_reply(c, 400, CORS_HEADERS, "{\"error\":\"Need at least 2 users for a conference\"}");
            return;
        }
        if (requested_users.size() > MAX_LINES) {
            mg_http_reply(c, 400, CORS_HEADERS, "{\"error\":\"Maximum %d users in a conference\"}", MAX_LINES);
            return;
        }

        std::lock_guard<std::mutex> lock(users_mutex_);
        std::vector<RegisteredUser> participants;
        for (const auto& uname : requested_users) {
            auto it = users_.find(uname);
            if (it == users_.end()) {
                mg_http_reply(c, 404, CORS_HEADERS, "{\"error\":\"User '%s' not registered\"}", uname.c_str());
                return;
            }
            participants.push_back(it->second);
        }

        initiate_conference(participants);
        mg_http_reply(c, 200, CORS_HEADERS, "{\"success\":true,\"legs\":%d}", (int)participants.size());
    }

    // POST /hangup — end the current call
    void handle_hangup(struct mg_connection* c) {
        if (!call_ || !call_->active) {
            mg_http_reply(c, 200, CORS_HEADERS, "{\"success\":true,\"message\":\"No active call\"}");
            return;
        }
        shutdown_call();
        call_.reset();
        mg_http_reply(c, 200, CORS_HEADERS, "{\"success\":true,\"message\":\"Call ended\"}");
    }

    // POST /inject {"file":"sample.wav","leg":"alice1"}
    // The "leg" field is the username of the leg to inject into.
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

        if (!call_ || !call_->active || !call_->relay_started) {
            mg_http_reply(c, 409, CORS_HEADERS, "{\"error\":\"No active call to inject into\"}");
            return;
        }

        int leg_idx = -1;
        for (size_t i = 0; i < call_->legs.size(); i++) {
            if (call_->legs[i].user == leg) {
                leg_idx = static_cast<int>(i);
                break;
            }
        }

        if (leg == "a" && call_->legs.size() >= 1) leg_idx = 0;
        else if (leg == "b" && call_->legs.size() >= 2) leg_idx = 1;

        if (leg_idx < 0) {
            mg_http_reply(c, 404, CORS_HEADERS, "{\"error\":\"Leg '%s' not found in active call\"}", leg.c_str());
            return;
        }

        std::string full_path = testfiles_dir_ + "/" + file;
        struct stat st;
        if (stat(full_path.c_str(), &st) != 0) {
            mg_http_reply(c, 404, CORS_HEADERS, "{\"error\":\"File not found: %s\"}", file.c_str());
            return;
        }

        bool no_silence = false;
        mg_json_get_bool(hm->body, "$.no_silence", &no_silence);

        std::string err = inject_file(full_path, leg_idx, no_silence);
        if (!err.empty()) {
            mg_http_reply(c, 500, CORS_HEADERS, "{\"error\":\"%s\"}", err.c_str());
            return;
        }

        {
            std::lock_guard<std::mutex> lk(call_->inject_meta_mutex);
            call_->injecting_file = file;
        }
        call_->injecting_leg_idx.store(leg_idx);
        mg_http_reply(c, 200, CORS_HEADERS, "{\"success\":true,\"injecting\":\"%s\",\"leg\":\"%s\"}",
                      file.c_str(), call_->legs[leg_idx].user.c_str());
    }

    void handle_stop_inject(struct mg_connection* c) {
        if (!call_ || !call_->injecting) {
            mg_http_reply(c, 200, CORS_HEADERS, "{\"success\":true,\"was_injecting\":false}");
            return;
        }
        cancel_injection();
        mg_http_reply(c, 200, CORS_HEADERS, "{\"success\":true,\"was_injecting\":true}");
    }

    void handle_status(struct mg_connection* c) {
        std::ostringstream json;
        json << "{\"call_active\":" << (call_ && call_->active ? "true" : "false");
        if (call_) {
            json << ",\"legs\":" << call_->legs.size();
            json << ",\"relay_stats\":{";
            json << "\"total_pkts\":" << call_->total_pkts.load();
            json << ",\"total_bytes\":" << call_->total_bytes.load();
            json << "}";
            if (call_->injecting) {
                std::lock_guard<std::mutex> lk(call_->inject_meta_mutex);
                int idx = call_->injecting_leg_idx.load();
                json << ",\"injecting\":\"" << call_->injecting_file << "\"";
                json << ",\"injecting_leg\":\"" << (idx >= 0 && idx < (int)call_->legs.size()
                    ? call_->legs[idx].user : "unknown") << "\"";
            } else {
                json << ",\"injecting\":null";
            }
        }
        json << "}";
        mg_http_reply(c, 200, CORS_HEADERS, "%s", json.str().c_str());
    }

    void handle_calls(struct mg_connection* c) {
        std::ostringstream json;
        json << "{\"calls\":[";
        if (call_ && call_->active) {
            json << "{\"id\":" << call_->id;
            json << ",\"legs\":[";
            for (size_t i = 0; i < call_->legs.size(); i++) {
                if (i > 0) json << ",";
                json << "{\"user\":\"" << call_->legs[i].user
                     << "\",\"answered\":" << (call_->legs[i].answered ? "true" : "false")
                     << ",\"rtp_port\":" << call_->legs[i].client_rtp_port
                     << ",\"relay_port\":" << call_->legs[i].relay_port << "}";
            }
            json << "]";
            json << ",\"relay_started\":" << (call_->relay_started ? "true" : "false");
            json << ",\"injecting\":" << (call_->injecting ? "true" : "false");
            auto now = std::chrono::steady_clock::now();
            auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - call_->started_at).count();
            json << ",\"duration\":" << duration;
            json << "}";
        }
        json << "]";
        {
            std::lock_guard<std::mutex> lock(users_mutex_);
            json << ",\"users\":" << users_.size();
        }
        json << "}";
        mg_http_reply(c, 200, CORS_HEADERS, "%s", json.str().c_str());
    }

    std::string inject_file(const std::string& path, int leg_idx, bool no_silence = false) {
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
        call_->inject_thread = std::thread(&TestSipProvider::inject_rtp_stream, this, call_, leg_idx, std::move(ulaw), no_silence);
        return "";
    }

    void cancel_injection() {
        if (!call_) return;
        call_->injecting = false;
        if (call_->inject_thread.joinable()) {
            call_->inject_thread.join();
        }
    }

    // Injects RTP audio into the specified leg of the active call.
    // Uses the dedicated inject_sock (separate from relay_sock) to avoid data races.
    // If no_silence is true, sends a 1s silence tail then stops; otherwise sends silence indefinitely.
    void inject_rtp_stream(std::shared_ptr<ActiveCall> call, int leg_idx, std::vector<uint8_t> ulaw_samples, bool no_silence) {
        static constexpr int PKT_SAMPLES = 160;

        if (leg_idx < 0 || leg_idx >= (int)call->legs.size()) {
            std::fprintf(stderr, "Invalid leg index %d for injection\n", leg_idx);
            call->injecting = false;
            return;
        }

        CallLeg* target = &call->legs[leg_idx];

        struct sockaddr_in dest{};
        dest.sin_family = AF_INET;
        dest.sin_port = htons(target->client_rtp_port);
        dest.sin_addr.s_addr = inet_addr(target->ip.c_str());

        std::random_device rd;
        uint32_t ssrc = rd();
        uint16_t seq = 0;
        uint32_t ts = 0;

        size_t total_pkts = ulaw_samples.size() / PKT_SAMPLES;
        std::printf("Injecting %zu RTP packets (%.1fs) into leg %s%s\n",
                    total_pkts, ulaw_samples.size() / 8000.0, target->user.c_str(),
                    no_silence ? " [no-silence]" : "");

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

            sendto(target->inject_sock, rtp, sizeof(rtp), 0,
                   (struct sockaddr*)&dest, sizeof(dest));

            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }

        if (no_silence) {
            static constexpr int SILENCE_TAIL_MS = 1000;
            static constexpr int SILENCE_TAIL_PKTS = SILENCE_TAIL_MS / 20;
            std::printf("Audio done, sending %dms silence tail then stopping\n", SILENCE_TAIL_MS);
            for (int i = 0; i < SILENCE_TAIL_PKTS && call->active && call->injecting && g_running; i++) {
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
                std::memset(rtp + 12, 0x7F, PKT_SAMPLES);
                sendto(target->inject_sock, rtp, sizeof(rtp), 0,
                       (struct sockaddr*)&dest, sizeof(dest));
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
            call->injecting = false;
        } else {
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
                std::memset(rtp + 12, 0x7F, PKT_SAMPLES);
                sendto(target->inject_sock, rtp, sizeof(rtp), 0,
                       (struct sockaddr*)&dest, sizeof(dest));
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }

        {
            std::lock_guard<std::mutex> lk(call->inject_meta_mutex);
            call->injecting_file.clear();
        }
        call->injecting_leg_idx.store(-1);
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

        {
            std::lock_guard<std::mutex> lock(users_mutex_);
            if (users_.size() >= MAX_LINES && users_.find(username) == users_.end()) {
                std::fprintf(stderr, "REGISTER: max %d users reached, rejecting %s\n", MAX_LINES, username.c_str());
                std::string resp = "SIP/2.0 503 Service Unavailable\r\nFrom: " + from +
                                   "\r\nCall-ID: " + call_id + "\r\nCSeq: 1 REGISTER\r\n\r\n";
                sendto(sip_sock_, resp.c_str(), resp.length(), 0, (struct sockaddr*)&sender, sizeof(sender));
                return;
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

    // Starts a conference call between the given participants.
    // For each participant, creates a leg with a relay socket and sends SIP INVITE.
    // When all legs answer, starts relay threads that forward RTP between all legs (conference bridge).
    void initiate_conference(const std::vector<RegisteredUser>& participants) {
        call_ = std::make_shared<ActiveCall>();
        call_->id = ++next_call_id_;
        call_->started_at = std::chrono::steady_clock::now();

        for (size_t i = 0; i < participants.size(); i++) {
            CallLeg leg;
            leg.user = participants[i].username;
            leg.sip_call_id = "conf-" + std::to_string(call_->id) + "-" + std::to_string(i);
            leg.ip = participants[i].ip;
            leg.sip_port = participants[i].sip_port;
            leg.answered = false;
            leg.client_rtp_port = 0;
            leg.relay_sock = create_relay_socket(leg.relay_port);

            if (leg.relay_sock < 0) {
                std::fprintf(stderr, "Failed to create relay socket for %s — aborting conference\n", leg.user.c_str());
                call_->active = false;
                return;
            }

            leg.inject_sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (leg.inject_sock < 0) {
                std::fprintf(stderr, "Failed to create inject socket for %s\n", leg.user.c_str());
                close(leg.relay_sock);
                call_->active = false;
                return;
            }

            call_->legs.push_back(std::move(leg));
        }

        for (auto& leg : call_->legs) {
            send_invite(leg);
        }

        std::printf("INVITE sent to %zu participants for conference %d\n", participants.size(), call_->id);
    }

    void send_invite(CallLeg& leg) {
        std::ostringstream sdp;
        sdp << "v=0\r\no=provider 1 1 IN IP4 " << local_ip_ << "\r\n";
        sdp << "s=WhisperTalk Test\r\nc=IN IP4 " << local_ip_ << "\r\nt=0 0\r\n";
        sdp << "m=audio " << leg.relay_port << " RTP/AVP 0 101\r\n";
        sdp << "a=rtpmap:0 PCMU/8000\r\na=rtpmap:101 telephone-event/8000\r\n";
        std::string sdp_str = sdp.str();

        std::ostringstream inv;
        inv << "INVITE sip:" << leg.user << "@" << leg.ip << " SIP/2.0\r\n";
        inv << "Via: SIP/2.0/UDP " << local_ip_ << ":" << sip_port_ << "\r\n";
        inv << "From: <sip:provider@" << local_ip_ << ">;tag=prov" << leg.sip_call_id << "\r\n";
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
        for (auto& l : call_->legs) {
            if (l.sip_call_id == call_id) { leg = &l; break; }
        }
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

        bool all_answered = true;
        for (const auto& l : call_->legs) {
            if (!l.answered) { all_answered = false; break; }
        }

        if (all_answered && !call_->relay_started) {
            call_->relay_started = true;
            start_relay();
        }
    }

    // Conference bridge relay: for each leg, receives RTP and forwards to all other legs.
    void start_relay() {
        std::printf("\n=== All %zu legs answered — starting conference relay ===\n", call_->legs.size());
        for (size_t i = 0; i < call_->legs.size(); i++) {
            std::printf("  Leg %zu: %s RTP %s:%d <-> relay %d\n", i,
                        call_->legs[i].user.c_str(), call_->legs[i].ip.c_str(),
                        call_->legs[i].client_rtp_port, call_->legs[i].relay_port);
        }

        for (size_t i = 0; i < call_->legs.size(); i++) {
            call_->relay_threads.emplace_back(&TestSipProvider::conference_relay_thread, this, call_, i);
        }

        stats_thread_ = std::thread(&TestSipProvider::stats_loop, this, call_);
    }

    // Each relay thread: receive from one leg's relay socket, forward to all other legs' client RTP ports.
    void conference_relay_thread(std::shared_ptr<ActiveCall> call, size_t from_idx) {
        char buf[2048];
        struct sockaddr_in sender{};
        socklen_t slen;

        int from_sock = call->legs[from_idx].relay_sock;

        while (call->active && g_running) {
            slen = sizeof(sender);
            struct timeval tv{0, 100000};
            setsockopt(from_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

            ssize_t n = recvfrom(from_sock, buf, sizeof(buf), 0,
                                 (struct sockaddr*)&sender, &slen);
            if (n <= 0) continue;

            for (size_t i = 0; i < call->legs.size(); i++) {
                if (i == from_idx) continue;
                struct sockaddr_in dest{};
                dest.sin_family = AF_INET;
                dest.sin_port = htons(call->legs[i].client_rtp_port);
                dest.sin_addr.s_addr = inet_addr(call->legs[i].ip.c_str());
                sendto(call->legs[i].relay_sock, buf, n, 0,
                       (struct sockaddr*)&dest, sizeof(dest));
            }
            call->total_pkts++;
            call->total_bytes += n;
        }
    }

    void stats_loop(std::shared_ptr<ActiveCall> call) {
        auto start = std::chrono::steady_clock::now();
        while (call->active && g_running) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            if (!call->active || !g_running) break;
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - start).count();
            std::printf("[%llds] Conference relay: %llu pkts (%llu KB)\n",
                        elapsed,
                        call->total_pkts.load(), call->total_bytes.load() / 1024);
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
        std::printf("   Test SIP Provider — Conference Results\n");
        std::printf("=========================================\n");
        std::printf("  Duration:     %llds\n", elapsed);
        std::printf("  Legs:         %zu\n", call_->legs.size());
        for (size_t i = 0; i < call_->legs.size(); i++) {
            std::printf("  Leg %zu:       %s (RTP %s:%d, answered=%s)\n", i,
                        call_->legs[i].user.c_str(), call_->legs[i].ip.c_str(),
                        call_->legs[i].client_rtp_port,
                        call_->legs[i].answered ? "yes" : "no");
        }
        std::printf("-----------------------------------------\n");
        std::printf("  Total relay:  %llu packets  (%llu KB)\n",
                    call_->total_pkts.load(), call_->total_bytes.load() / 1024);
        std::printf("-----------------------------------------\n");

        bool all_answered = true;
        for (const auto& l : call_->legs) {
            if (!l.answered) { all_answered = false; break; }
        }
        std::printf("  Result:       %s\n", all_answered ? "PASS" : "INCOMPLETE");
        std::printf("=========================================\n\n");
    }

    void shutdown_call() {
        if (!call_) return;

        cancel_injection();

        if (call_->active) {
            for (auto& leg : call_->legs) {
                send_bye_to_leg(leg);
            }
            call_->active = false;
        }

        for (auto& t : call_->relay_threads) {
            if (t.joinable()) t.join();
        }
        if (stats_thread_.joinable()) stats_thread_.join();

        for (auto& leg : call_->legs) {
            if (leg.relay_sock >= 0) close(leg.relay_sock);
            if (leg.inject_sock >= 0) close(leg.inject_sock);
        }
    }

    int sip_sock_ = -1;
    int sip_port_ = 5060;
    int http_port_ = 22011;
    std::string local_ip_;
    std::string testfiles_dir_;
    int next_call_id_ = 0;

    struct mg_mgr mgr_;

    std::mutex users_mutex_;
    std::map<std::string, RegisteredUser> users_;

    std::mutex call_mutex_;
    std::shared_ptr<ActiveCall> call_;
    std::thread stats_thread_;
};

int main(int argc, char* argv[]) {
    setlinebuf(stdout);
    setlinebuf(stderr);
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
                std::printf("  GET  /files       List WAV files in testfiles directory\n");
                std::printf("  GET  /users       List registered SIP users\n");
                std::printf("  POST /conference  Start conference: {\"users\":[\"alice1\",\"bob2\"]}\n");
                std::printf("  POST /hangup      End current call\n");
                std::printf("  POST /inject      Inject audio: {\"file\":\"sample.wav\",\"leg\":\"alice1\"}\n");
                std::printf("  GET  /status      Call status and relay stats\n");
                std::printf("  GET  /calls       List active calls with per-leg detail\n\n");
                std::printf("Max lines: %d\n\n", MAX_LINES);
                return 0;
            default: break;
        }
    }

    std::printf("Test SIP Provider (B2BUA / Conference Bridge)\n");
    std::printf("  SIP Port:     %d\n", port);
    std::printf("  HTTP Port:    %d\n", http_port);
    std::printf("  Testfiles:    %s\n", testfiles_dir.c_str());
    std::printf("  Max Lines:    %d\n", MAX_LINES);
    std::printf("\n");

    TestSipProvider provider;
    if (!provider.init(port, ip, testfiles_dir, http_port)) return 1;
    provider.run();

    return 0;
}
