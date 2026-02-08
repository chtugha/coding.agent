#include "outbound-audio-processor.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fstream>
#include <sys/time.h>
#include <sys/un.h>
#include <netinet/tcp.h>
#include <deque>
#include <condition_variable>
#include <atomic>

// Lightweight async conversion worker (file-local)
namespace {
struct ConversionJob {
    std::string call_id;
    std::vector<uint8_t> payload; // raw PCM float32 bytes or already-encoded G711
    uint32_t sample_rate{0};
    uint32_t chunk_id{0};
    bool is_float{true};
};

std::mutex g_conv_mtx;
std::condition_variable g_conv_cv;
std::deque<ConversionJob> g_conv_q;
std::thread g_conv_thread;
std::atomic<bool> g_conv_running{false};

void push_conversion_job(const std::string& call_id, const std::vector<uint8_t>& payload, uint32_t sample_rate, uint32_t chunk_id, bool is_float) {
    ConversionJob job;
    job.call_id = call_id;
    job.payload = payload;
    job.sample_rate = sample_rate;
    job.chunk_id = chunk_id;
    job.is_float = is_float;
    {
        std::lock_guard<std::mutex> lk(g_conv_mtx);
        g_conv_q.emplace_back(std::move(job));
    }
    g_conv_cv.notify_one();
}
} // namespace


OutboundAudioProcessor::OutboundAudioProcessor() {
}

OutboundAudioProcessor::~OutboundAudioProcessor() {
    stop();
}

bool OutboundAudioProcessor::start(int base_port) {
    if (running_.load()) return true;

    base_port_ = base_port;
    running_.store(true);
    active_.store(true);

    if (!g_conv_running.exchange(true)) {
        g_conv_thread = std::thread([this]() {
            while (g_conv_running.load()) {
                ConversionJob job;
                {
                    std::unique_lock<std::mutex> lk(g_conv_mtx);
                    g_conv_cv.wait(lk, []{ return !g_conv_q.empty() || !g_conv_running.load(); });
                    if (!g_conv_running.load() && g_conv_q.empty()) break;
                    job = std::move(g_conv_q.front());
                    g_conv_q.pop_front();
                }

                std::shared_ptr<CallState> state;
                {
                    std::lock_guard<std::mutex> lock(calls_mutex_);
                    auto it = active_calls_.find(job.call_id);
                    if (it != active_calls_.end()) state = it->second;
                }

                if (state) {
                    std::vector<uint8_t> g711;
                    if (job.is_float) {
                        std::vector<float> mono(job.payload.size() / 4);
                        memcpy(mono.data(), job.payload.data(), job.payload.size());
                        g711 = process_float_mono_to_ulaw(mono, job.sample_rate);
                    } else {
                        g711 = job.payload; // Already G711
                    }
                    enqueue_g711_(state, g711);
                    state->last_activity = std::chrono::steady_clock::now();
                }
            }
        });
    }

    ctrl_thread_ = std::thread(&OutboundAudioProcessor::control_socket_loop, this);
    maintenance_thread_ = std::thread(&OutboundAudioProcessor::maintenance_loop, this);
    start_output_scheduler_();

    std::cout << "🚀 Outbound Audio Processor started on base port " << base_port << std::endl;
    return true;
}

void OutboundAudioProcessor::stop() {
    if (!running_.load()) return;
    
    running_.store(false);
    
    if (ctrl_socket_ >= 0) {
        close(ctrl_socket_);
        ctrl_socket_ = -1;
    }
    unlink("/tmp/outbound-audio-processor.ctrl");

    if (ctrl_thread_.joinable()) ctrl_thread_.join();
    if (maintenance_thread_.joinable()) maintenance_thread_.join();

    g_conv_running.store(false);
    g_conv_cv.notify_all();
    if (g_conv_thread.joinable()) g_conv_thread.join();

    stop_output_scheduler_();

    std::lock_guard<std::mutex> lock(calls_mutex_);
    for (auto& pair : active_calls_) {
        cleanup_call(pair.second);
    }
    active_calls_.clear();

    std::cout << "🛑 Outbound Audio Processor stopped" << std::endl;
}

void OutboundAudioProcessor::control_socket_loop() {
    const char* socket_path = "/tmp/outbound-audio-processor.ctrl";
    unlink(socket_path);

    ctrl_socket_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctrl_socket_ < 0) return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(ctrl_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "❌ Failed to bind outbound control socket" << std::endl;
        return;
    }

    if (listen(ctrl_socket_, 5) < 0) return;

    std::cout << "🎮 Outbound Control Socket listening on " << socket_path << std::endl;

    while (running_.load()) {
        struct sockaddr_un remote;
        socklen_t len = sizeof(remote);
        int client_fd = accept(ctrl_socket_, (struct sockaddr*)&remote, &len);
        if (client_fd < 0) {
            if (running_.load()) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        char buf[256];
        ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            std::string cmd(buf);
            
            if (cmd.find("ACTIVATE:") == 0) {
                std::string call_id = cmd.substr(9);
                auto state = get_or_create_call_state(call_id);
                std::string resp = "OK:" + std::to_string(state->piper_tcp_port);
                send(client_fd, resp.c_str(), resp.length(), 0);
            } else if (cmd.find("DEACTIVATE:") == 0) {
                std::string call_id = cmd.substr(11);
                std::lock_guard<std::mutex> lock(calls_mutex_);
                auto it = active_calls_.find(call_id);
                if (it != active_calls_.end()) {
                    cleanup_call(it->second);
                    active_calls_.erase(it);
                }
                send(client_fd, "OK", 2, 0);
            }
        }
        close(client_fd);
    }
}

void OutboundAudioProcessor::set_sip_client_callback(std::function<void(const std::string&, const std::vector<uint8_t>&)> callback) {
}

void OutboundAudioProcessor::activate_for_call(const std::string& call_id) {
    get_or_create_call_state(call_id);
}

void OutboundAudioProcessor::deactivate_after_call() {
}

BaseAudioProcessor::ProcessorStatus OutboundAudioProcessor::get_status() const {
    auto status = BaseAudioProcessor::get_status();
    status.processor_type = "Outbound (Multi-Call)";
    std::lock_guard<std::mutex> lock(calls_mutex_);
    status.active_calls = active_calls_.size();
    return status;
}

std::vector<uint8_t> OutboundAudioProcessor::process_float_mono_to_ulaw(const std::vector<float>& mono, uint32_t sample_rate) {
    if (mono.empty()) return {};
    std::vector<float> work = mono;
    if (sample_rate > 8000) work = lowpass_telephony(work, sample_rate);
    if (sample_rate != 8000) work = resample_linear(work, sample_rate, 8000);
    return convert_float_to_g711_ulaw(work);
}

std::shared_ptr<OutboundAudioProcessor::CallState> OutboundAudioProcessor::get_or_create_call_state(const std::string& call_id) {
    std::lock_guard<std::mutex> lock(calls_mutex_);
    auto it = active_calls_.find(call_id);
    if (it != active_calls_.end()) return it->second;

    auto state = std::make_shared<CallState>(call_id);
    state->piper_tcp_port = calculate_piper_port(call_id);
    state->udp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    state->udp_dest_ip = "127.0.0.1";
    state->udp_dest_port = 9002;
    state->udp_call_id = static_cast<uint32_t>(std::stoul(call_id));

    active_calls_[call_id] = state;
    
    start_registration_polling(state);
    
    std::cout << "📞 Created CallState for outbound call " << call_id << " on port " << state->piper_tcp_port << std::endl;
    return state;
}

void OutboundAudioProcessor::cleanup_call(std::shared_ptr<CallState> state) {
    stop_registration_polling(state);
    
    state->piper_connected.store(false);
    {
        std::lock_guard<std::mutex> lock(state->piper_mutex);
        if (state->piper_tcp_socket >= 0) {
            close(state->piper_tcp_socket);
            state->piper_tcp_socket = -1;
        }
    }
    
    if (state->udp_socket >= 0) {
        close(state->udp_socket);
        state->udp_socket = -1;
    }
    
    if (state->piper_tcp_thread.joinable()) {
        state->piper_tcp_thread.join();
    }
}

void OutboundAudioProcessor::maintenance_loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        std::vector<std::shared_ptr<CallState>> stale_calls;
        auto now = std::chrono::steady_clock::now();
        {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            for (auto it = active_calls_.begin(); it != active_calls_.end(); ) {
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - it->second->last_activity).count();
                if (duration > 30) { // 30 seconds of inactivity for outbound
                    stale_calls.push_back(it->second);
                    it = active_calls_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& s : stale_calls) cleanup_call(s);
    }
}

void OutboundAudioProcessor::enqueue_g711_(std::shared_ptr<CallState> state, const std::vector<uint8_t>& g711) {
    if (g711.empty()) return;
    std::lock_guard<std::mutex> lk(state->out_buffer_mutex);
    // Max 10s buffer
    if (state->out_buffer.size() + g711.size() > 160 * 500) {
        state->out_buffer.erase(state->out_buffer.begin(), state->out_buffer.begin() + g711.size());
    }
    state->out_buffer.insert(state->out_buffer.end(), g711.begin(), g711.end());
}

void OutboundAudioProcessor::make_silence_frame_(std::shared_ptr<CallState> state, std::vector<uint8_t>& frame) {
    frame.resize(160, 0xFF);
    if (!state->piper_connected.load() && !silence_wav2_.empty()) {
        std::lock_guard<std::mutex> lock(calls_mutex_); // for silence_wav2_pos_ safety if shared, but let's just use 0
        for (size_t i = 0; i < 160; ++i) {
            frame[i] = silence_wav2_[silence_wav2_pos_ % silence_wav2_.size()];
            silence_wav2_pos_ = (silence_wav2_pos_ + 1) % silence_wav2_.size();
        }
    }
}

void OutboundAudioProcessor::start_output_scheduler_() {
    if (output_running_.exchange(true)) return;
    output_thread_ = std::thread([this]() {
        auto next = std::chrono::steady_clock::now();
        while (output_running_.load()) {
            std::vector<std::shared_ptr<CallState>> current_calls;
            {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                for (auto& p : active_calls_) current_calls.push_back(p.second);
            }

            for (auto& state : current_calls) {
                std::vector<uint8_t> frame(160);
                bool has_audio = false;
                {
                    std::lock_guard<std::mutex> lk(state->out_buffer_mutex);
                    if (state->out_buffer.size() >= 160) {
                        memcpy(frame.data(), state->out_buffer.data(), 160);
                        state->out_buffer.erase(state->out_buffer.begin(), state->out_buffer.begin() + 160);
                        has_audio = true;
                        state->pending_first_rtp = false;
                    }
                }

                if (!has_audio) {
                    make_silence_frame_(state, frame);
                }

                if (state->udp_socket >= 0 && state->udp_dest_port > 0) {
                    struct sockaddr_in addr{};
                    addr.sin_family = AF_INET;
                    addr.sin_port = htons(state->udp_dest_port);
                    inet_pton(AF_INET, state->udp_dest_ip.c_str(), &addr.sin_addr);

                    std::vector<uint8_t> pkt(4 + 160);
                    uint32_t cid_net = htonl(state->udp_call_id);
                    memcpy(pkt.data(), &cid_net, 4);
                    memcpy(pkt.data() + 4, frame.data(), 160);

                    sendto(state->udp_socket, pkt.data(), pkt.size(), 0, (struct sockaddr*)&addr, sizeof(addr));
                }
            }

            next += std::chrono::milliseconds(20);
            std::this_thread::sleep_until(next);
        }
    });
}

void OutboundAudioProcessor::stop_output_scheduler_() {
    output_running_.store(false);
    if (output_thread_.joinable()) output_thread_.join();
}

int OutboundAudioProcessor::calculate_piper_port(const std::string& call_id) {
    unsigned int hash = 0;
    for (char c : call_id) hash = hash * 31 + c;
    return 8091 + (hash % 1000);
}

void OutboundAudioProcessor::start_registration_polling(std::shared_ptr<CallState> state) {
    state->registration_running.store(true);
    state->registration_thread = std::thread(&OutboundAudioProcessor::registration_polling_thread, this, state);
}

void OutboundAudioProcessor::stop_registration_polling(std::shared_ptr<CallState> state) {
    state->registration_running.store(false);
    if (state->registration_thread.joinable()) state->registration_thread.join();
}

void OutboundAudioProcessor::registration_polling_thread(std::shared_ptr<CallState> state) {
    int udp_port = 14000 + (calculate_piper_port(state->call_id) % 1000);
    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) return;
    int opt = 1;
    setsockopt(udp_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(udp_port);

    if (bind(udp_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(udp_sock);
        return;
    }

    std::cout << "📡 Outbound REGISTER listener on port " << udp_port << " for call " << state->call_id << std::endl;

    char buf[256];
    while (state->registration_running.load() && running_.load()) {
        struct timeval tv{1, 0};
        setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        struct sockaddr_in src{}; socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(udp_sock, buf, sizeof(buf) - 1, 0, (struct sockaddr*)&src, &slen);
        if (n <= 0) continue;
        buf[n] = '\0';
        std::string msg(buf);
        if (msg.find("REGISTER:") != 0) continue;

        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) continue;
        struct sockaddr_in dst{};
        dst.sin_family = AF_INET;
        dst.sin_addr.s_addr = inet_addr("127.0.0.1");
        dst.sin_port = htons(state->piper_tcp_port);
        
        if (connect(s, (struct sockaddr*)&dst, sizeof(dst)) == 0) {
            int flag = 1;
            setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));
            uint32_t nlen = htonl(static_cast<uint32_t>(state->call_id.size()));
            write(s, &nlen, 4);
            write(s, state->call_id.data(), state->call_id.size());
            
            {
                std::lock_guard<std::mutex> lock(state->piper_mutex);
                state->piper_tcp_socket = s;
                state->piper_connected.store(true);
            }
            
            while (state->registration_running.load() && running_.load() && state->piper_connected.load()) {
                uint32_t len=0, rate=0, id=0;
                if (!read_exact_from_socket(s, &len, 4) || !read_exact_from_socket(s, &rate, 4) || !read_exact_from_socket(s, &id, 4)) break;
                len = ntohl(len); rate = ntohl(rate); id = ntohl(id);
                if (len == 0 || len > 10*1024*1024) break;
                std::vector<uint8_t> payload(len);
                if (!read_exact_from_socket(s, payload.data(), len)) break;
                push_conversion_job(state->call_id, payload, rate, id, (len % 4 == 0));
                state->last_activity = std::chrono::steady_clock::now();
            }
            close(s);
            {
                std::lock_guard<std::mutex> lock(state->piper_mutex);
                state->piper_tcp_socket = -1;
                state->piper_connected.store(false);
            }
        } else {
            close(s);
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    close(udp_sock);
}

bool OutboundAudioProcessor::read_exact_from_socket(int socket_fd, void* data, size_t size) {
    uint8_t* ptr = static_cast<uint8_t*>(data);
    size_t remaining = size;
    while (remaining > 0) {
        ssize_t received = read(socket_fd, ptr, remaining);
        if (received <= 0) return false;
        ptr += received;
        remaining -= received;
    }
    return true;
}

void OutboundAudioProcessor::set_udp_output(const std::string& ip, int port, uint32_t call_id) {}
void OutboundAudioProcessor::clear_udp_output() {}
void OutboundAudioProcessor::set_silence_wav2_bytes(const std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> lk(calls_mutex_);
    silence_wav2_ = bytes;
    silence_wav2_pos_ = 0;
}
bool OutboundAudioProcessor::load_and_set_silence_wav2(const std::string& path) {
    std::vector<uint8_t> bytes;
    if (!read_entire_file(path, bytes)) return false;
    uint32_t rate = 0;
    auto mono = decode_bytes_to_float_mono(bytes, detect_audio_file_type(bytes), rate);
    if (mono.empty()) return false;
    set_silence_wav2_bytes(process_float_mono_to_ulaw(mono, rate));
    return true;
}

// Utility methods from original file (kept but simplified)
OutboundAudioProcessor::AudioFileType OutboundAudioProcessor::detect_audio_file_type(const std::vector<uint8_t>& b) {
    if (b.size() >= 12 && b[0]=='R' && b[1]=='I' && b[2]=='F' && b[3]=='F') return AudioFileType::WAV;
    return AudioFileType::UNKNOWN;
}
bool OutboundAudioProcessor::read_entire_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary); if (!f) return false;
    f.seekg(0, std::ios::end); out.resize(f.tellg()); f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()), out.size()); return true;
}
bool OutboundAudioProcessor::parse_wav_header(const std::vector<uint8_t>& b, uint16_t& fmt, uint16_t& ch, uint32_t& rate, uint16_t& bps, size_t& off, size_t& sz) {
    if (b.size() < 44) return false; fmt = *reinterpret_cast<const uint16_t*>(&b[20]); ch = *reinterpret_cast<const uint16_t*>(&b[22]);
    rate = *reinterpret_cast<const uint32_t*>(&b[24]); bps = *reinterpret_cast<const uint16_t*>(&b[34]); off = 44; sz = b.size() - 44; return true;
}
std::vector<float> OutboundAudioProcessor::decode_bytes_to_float_mono(const std::vector<uint8_t>& b, AudioFileType t, uint32_t& r) {
    uint16_t fmt, ch, bps; size_t off, sz; if (!parse_wav_header(b, fmt, ch, r, bps, off, sz)) return {};
    std::vector<float> mono; const int16_t* p = reinterpret_cast<const int16_t*>(b.data() + off);
    for (size_t i = 0; i < sz / 2; i += ch) mono.push_back(p[i] / 32768.0f); return mono;
}
void OutboundAudioProcessor::ensure_output_running() { start_output_scheduler_(); }
void OutboundAudioProcessor::process_piper_audio_chunk(std::shared_ptr<CallState> s, const std::vector<uint8_t>& p, uint32_t r, uint32_t i) {}
