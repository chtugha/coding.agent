#include "inbound-audio-processor.h"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/un.h>

// G.711 μ-law decode table (fast lookup)
static float ulaw_table[256];
static float alaw_table[256];
static bool tables_initialized = false;

static void initialize_g711_tables() {
    if (tables_initialized) return;
    
    // μ-law table
    const int16_t ulaw_decode[256] = {
        -32124,-31100,-30076,-29052,-28028,-27004,-25980,-24956,
        -23932,-22908,-21884,-20860,-19836,-18812,-17788,-16764,
        -15996,-15484,-14972,-14460,-13948,-13436,-12924,-12412,
        -11900,-11388,-10876,-10364, -9852, -9340, -8828, -8316,
        -7932, -7676, -7420, -7164, -6908, -6652, -6396, -6140,
        -5884, -5628, -5372, -5116, -4860, -4604, -4348, -4092,
        -3900, -3772, -3644, -3516, -3388, -3260, -3132, -3004,
        -2876, -2748, -2620, -2492, -2364, -2236, -2108, -1980,
        -1884, -1820, -1756, -1692, -1628, -1564, -1500, -1436,
        -1372, -1308, -1244, -1180, -1116, -1052,  -988,  -924,
        -876,  -844,  -812,  -780,  -748,  -716,  -684,  -652,
        -620,  -588,  -556,  -524,  -492,  -460,  -428,  -396,
        -372,  -356,  -340,  -324,  -308,  -292,  -276,  -260,
        -244,  -228,  -212,  -196,  -180,  -164,  -148,  -132,
        -120,  -112,  -104,   -96,   -88,   -80,   -72,   -64,
        -56,   -48,   -40,   -32,   -24,   -16,    -8,     0,
        32124, 31100, 30076, 29052, 28028, 27004, 25980, 24956,
        23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764,
        15996, 15484, 14972, 14460, 13948, 13436, 12924, 12412,
        11900, 11388, 10876, 10364,  9852,  9340,  8828,  8316,
        7932,  7676,  7420,  7164,  6908,  6652,  6396,  6140,
        5884,  5628,  5372,  5116,  4860,  4604,  4348,  4092,
        3900,  3772,  3644,  3516,  3388,  3260,  3132,  3004,
        2876,  2748,  2620,  2492,  2364,  2236,  2108,  1980,
        1884,  1820,  1756,  1692,  1628,  1564,  1500,  1436,
        1372,  1308,  1244,  1180,  1116,  1052,   988,   924,
        876,   844,   812,   780,   748,   716,   684,   652,
        620,   588,   556,   524,   492,   460,   428,   396,
        372,   356,   340,   324,   308,   292,   276,   260,
        244,   228,   212,   196,   180,   164,   148,   132,
        120,   112,   104,    96,    88,    80,    72,    64,
        56,    48,    40,    32,    24,    16,     8,     0
    };
    
    for (int i = 0; i < 256; ++i) {
        ulaw_table[i] = ulaw_decode[i] / 32768.0f;
        // Proper ITU-T G.711 A-law decode
        uint8_t a = static_cast<uint8_t>(i) ^ 0x55; // complement per spec
        int sign = a & 0x80;
        int exp  = (a & 0x70) >> 4;
        int mant = a & 0x0F;
        int sample = (mant << 4) + 8; // add rounding bias
        if (exp != 0) {
            sample = (sample + 0x100) << (exp - 1);
        }
        if (sign) sample = -sample;
        alaw_table[i] = static_cast<float>(sample) / 32768.0f;
    }
    
    tables_initialized = true;
}

InboundAudioProcessor::InboundAudioProcessor() {
    initialize_g711_tables();
}

InboundAudioProcessor::~InboundAudioProcessor() {
    stop();
}

bool InboundAudioProcessor::start(int base_port) {
    if (running_.load()) return true;
    
    base_port_ = base_port;
    running_.store(true);
    active_.store(true); // Always active in multi-call mode

    start_control_socket();
    
    maintenance_thread_ = std::thread(&InboundAudioProcessor::maintenance_loop, this);

    std::cout << "🚀 Inbound Audio Processor started (MULTI-CALL) on base port " << base_port << std::endl;
    return true;
}

void InboundAudioProcessor::start_control_socket() {
    ctrl_thread_ = std::thread(&InboundAudioProcessor::control_socket_loop, this);
}

void InboundAudioProcessor::control_socket_loop() {
    const char* socket_path = "/tmp/inbound-audio-processor.ctrl";
    unlink(socket_path);

    ctrl_socket_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctrl_socket_ < 0) return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    if (bind(ctrl_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(ctrl_socket_);
        return;
    }

    listen(ctrl_socket_, 5);
    std::cout << "🎮 Control socket listening on " << socket_path << std::endl;

    while (running_.load()) {
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(ctrl_socket_, &fds);

        int ret = select(ctrl_socket_ + 1, &fds, nullptr, nullptr, &tv);
        if (ret <= 0) continue;

        int client_fd = accept(ctrl_socket_, nullptr, nullptr);
        if (client_fd < 0) continue;

        char buf[128];
        memset(buf, 0, sizeof(buf));
        ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            std::string cmd(buf);
            if (cmd.find("REQUEST_ID:") == 0) {
                int requested_id = std::stoi(cmd.substr(11));
                int granted_id = requested_id;

                // Check if ID is already in use
                std::lock_guard<std::mutex> lock(calls_mutex_);
                bool in_use = true;
                while (in_use) {
                    in_use = false;
                    for (const auto& pair : active_calls_) {
                        if (pair.first == std::to_string(granted_id)) {
                            in_use = true;
                            granted_id++;
                            break;
                        }
                    }
                }

                char resp[64];
                snprintf(resp, sizeof(resp), "GRANTED_ID:%d", granted_id);
                send(client_fd, resp, strlen(resp), 0);
                std::cout << "🆔 Granted call_num_id: " << granted_id << " (requested: " << requested_id << ")" << std::endl;
            } else if (cmd.find("DEACTIVATE:") == 0) {
                std::string call_id = cmd.substr(11);
                deactivate_call(call_id);
            } else if (cmd.find("ACTIVATE:") == 0) {
                std::string call_id = cmd.substr(9);
                activate_for_call(call_id);
            }
        }
        close(client_fd);
    }
    close(ctrl_socket_);
    unlink(socket_path);
}

void InboundAudioProcessor::stop() {
    if (!running_.load()) return;
    
    running_.store(false);

    if (ctrl_thread_.joinable()) {
        ctrl_thread_.join();
    }

    if (sip_udp_thread_.joinable()) {
        sip_udp_thread_.join();
    }

    if (maintenance_thread_.joinable()) {
        maintenance_thread_.join();
    }
    
    std::lock_guard<std::mutex> lock(calls_mutex_);
    for (auto& pair : active_calls_) {
        cleanup_call(pair.second);
    }
    active_calls_.clear();

    std::cout << "🛑 Inbound Audio Processor stopped" << std::endl;
}

void InboundAudioProcessor::process_rtp_audio(const RTPAudioPacket& packet) {
    // Legacy support: uses current_call_id_ or "global"
    std::string cid = current_call_id_;
    if (cid.empty()) cid = "global";
    process_rtp_audio(cid, packet);
}

void InboundAudioProcessor::process_rtp_audio(const std::string& call_id, const RTPAudioPacket& packet) {
    process_rtp_audio(call_id, packet.payload_type, packet.audio_data.data(), packet.audio_data.size());
}

void InboundAudioProcessor::process_rtp_audio(const std::string& call_id, uint8_t payload_type, const uint8_t* audio_data, size_t audio_len) {
    if (!running_.load()) return;

    auto state = get_or_create_call_state(call_id);
    if (!state) return;

    state->last_activity = std::chrono::steady_clock::now();

    if (payload_type == 101) return; // Ignore DTMF

    // Zero-allocation decoding and upsampling
    if (payload_type == 0 || payload_type == 8) { // G.711
        if (state->pcm_buffer.size() != audio_len * 2) {
            state->pcm_buffer.resize(audio_len * 2);
        }
        float* dst = state->pcm_buffer.data();
        const float* table = (payload_type == 0) ? ulaw_table : alaw_table;
        
        for (size_t i = 0; i < audio_len; ++i) {
            float s = table[audio_data[i]];
            dst[i*2] = s;
            float next = (i + 1 < audio_len) ? table[audio_data[i+1]] : s;
            dst[i*2 + 1] = 0.5f * (s + next);
        }
    } else if (payload_type == 10 || payload_type == 11) { // PCM16
        size_t n = audio_len / 2;
        if (state->pcm_buffer.size() != n) {
            state->pcm_buffer.resize(n);
        }
        float* dst = state->pcm_buffer.data();
        for (size_t i = 0; i < n; ++i) {
            int16_t sample = (audio_data[i*2 + 1] << 8) | audio_data[i*2];
            dst[i] = sample / 32768.0f;
        }
    } else {
        return; // Unsupported payload
    }

    if (!state->pcm_buffer.empty()) {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->connected.load() && state->tcp_socket >= 0) {
            // Flush initial buffer if any
            if (!state->initial_buffer.empty()) {
                send_tcp_audio_chunk(state, state->initial_buffer);
                state->initial_buffer.clear();
            }
            send_tcp_audio_chunk(state, state->pcm_buffer);
        } else {
            // Buffer temporarily if not connected yet (sliding window)
            if (state->initial_buffer.size() + state->pcm_buffer.size() > CallState::MAX_INITIAL_BUFFER_SAMPLES) {
                size_t excess = (state->initial_buffer.size() + state->pcm_buffer.size()) - CallState::MAX_INITIAL_BUFFER_SAMPLES;
                if (excess < state->initial_buffer.size()) {
                    state->initial_buffer.erase(state->initial_buffer.begin(), state->initial_buffer.begin() + excess);
                } else {
                    state->initial_buffer.clear();
                }
            }
            state->initial_buffer.insert(state->initial_buffer.end(), state->pcm_buffer.begin(), state->pcm_buffer.end());
        }
        total_packets_processed_.fetch_add(1);
    }
}

void InboundAudioProcessor::activate_for_call(const std::string& call_id) {
    auto state = get_or_create_call_state(call_id);
    
    if (state && !state->connected.load() && state->listen_socket < 0) {
        std::cout << "📞 Activating for call: " << call_id << std::endl;
        setup_whisper_tcp_socket(state);
        start_registration_polling(state);
    }
    
    // Set as current call for legacy single-call components
    {
        std::lock_guard<std::mutex> lock(call_mutex_);
        current_call_id_ = call_id;
    }
    active_.store(true);
}

bool InboundAudioProcessor::is_call_active(const std::string& call_id) const {
    std::lock_guard<std::mutex> lock(calls_mutex_);
    return active_calls_.find(call_id) != active_calls_.end();
}

void InboundAudioProcessor::deactivate_after_call() {
    std::string cid;
    {
        std::lock_guard<std::mutex> lock(call_mutex_);
        cid = current_call_id_;
    }
    if (!cid.empty()) {
        deactivate_call(cid);
    }
}

void InboundAudioProcessor::deactivate_call(const std::string& call_id) {
    std::cout << "📞 Deactivating call: " << call_id << std::endl;
    
    std::shared_ptr<CallState> state;
    {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto it = active_calls_.find(call_id);
        if (it != active_calls_.end()) {
            state = it->second;
            active_calls_.erase(it);
        }
    }
    
    if (state) {
        cleanup_call(state);
    }
}

InboundAudioProcessor::ProcessorStatus InboundAudioProcessor::get_status() const {
    auto status = BaseAudioProcessor::get_status();
    status.processor_type = "Inbound (Multi-Call)";
    
    std::lock_guard<std::mutex> lock(calls_mutex_);
    status.active_calls = active_calls_.size();
    return status;
}

// Connection management
bool InboundAudioProcessor::setup_whisper_tcp_socket(std::shared_ptr<CallState> state) {
    state->listen_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (state->listen_socket < 0) return false;
    
    int opt = 1;
    setsockopt(state->listen_socket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    state->tcp_port = calculate_whisper_port(state->call_id);
    
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(state->tcp_port);
    
    if (bind(state->listen_socket, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(state->listen_socket);
        state->listen_socket = -1;
        return false;
    }
    
    if (listen(state->listen_socket, 1) < 0) {
        close(state->listen_socket);
        state->listen_socket = -1;
        return false;
    }
    
    state->tcp_thread = std::thread(&InboundAudioProcessor::handle_whisper_tcp_connection, this, state);
    std::cout << "✅ Whisper TCP server listening on port " << state->tcp_port << " for call " << state->call_id << std::endl;
    return true;
}

void InboundAudioProcessor::handle_whisper_tcp_connection(std::shared_ptr<CallState> state) {
    while (running_.load() && state->listen_socket >= 0) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_socket = accept(state->listen_socket, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->tcp_socket = client_socket;
            state->connected.store(true);
            std::cout << "🔗 Whisper client connected for call " << state->call_id << " on port " << state->tcp_port << std::endl;
            send_tcp_hello(state->tcp_socket, state->call_id);
            
            // Flush initial buffer if any (limit to 5s)
            if (!state->initial_buffer.empty()) {
                send_tcp_audio_chunk(state, state->initial_buffer);
                state->initial_buffer.clear();
            }
        }
        
        // Wait for connection to close by blocking on a read
        char dummy;
        while (running_.load() && state->connected.load()) {
            ssize_t n = recv(client_socket, &dummy, 1, MSG_PEEK);
            if (n <= 0) {
                if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                    continue;
                }
                break; 
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (state->tcp_socket >= 0) {
                close(state->tcp_socket);
                state->tcp_socket = -1;
            }
            state->connected.store(false);
            std::cout << "🔌 Whisper client disconnected for call " << state->call_id << std::endl;
        }
    }
}

void InboundAudioProcessor::send_tcp_audio_chunk(std::shared_ptr<CallState> state, const std::vector<float>& audio_samples) {
    size_t data_size = audio_samples.size() * sizeof(float);
    uint32_t length = htonl(data_size);
    
    if (!write_all_fd(state->tcp_socket, &length, 4) || 
        !write_all_fd(state->tcp_socket, audio_samples.data(), data_size)) {
        state->connected.store(false);
    }
}

void InboundAudioProcessor::send_tcp_hello(int socket_fd, const std::string& call_id) {
    uint32_t length = htonl(call_id.length());
    if (!write_all_fd(socket_fd, &length, 4)) return;
    write_all_fd(socket_fd, call_id.c_str(), call_id.length());
}

void InboundAudioProcessor::send_tcp_bye(int socket_fd) {
    uint32_t zero = 0xFFFFFFFF; // Special value for BYE
    write_all_fd(socket_fd, &zero, 4);
}

bool InboundAudioProcessor::write_all_fd(int fd, const void* data, size_t len) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    while (len > 0) {
        ssize_t written = write(fd, ptr, len);
        if (written <= 0) return false;
        ptr += written;
        len -= written;
    }
    return true;
}

void InboundAudioProcessor::start_registration_polling(std::shared_ptr<CallState> state) {
    state->registration_running.store(true);
    state->registration_thread = std::thread(&InboundAudioProcessor::registration_polling_thread, this, state);
}

void InboundAudioProcessor::stop_registration_polling(std::shared_ptr<CallState> state) {
    state->registration_running.store(false);
    if (state->registration_thread.joinable()) {
        state->registration_thread.join();
    }
}

void InboundAudioProcessor::registration_polling_thread(std::shared_ptr<CallState> state) {
    struct sockaddr_in whisper_addr;
    memset(&whisper_addr, 0, sizeof(whisper_addr));
    whisper_addr.sin_family = AF_INET;
    whisper_addr.sin_port = htons(13000);
    whisper_addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    std::string reg_msg = "REGISTER:" + state->call_id;

    while (state->registration_running.load() && running_.load()) {
        if (!state->connected.load()) {
            int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
            if (udp_sock >= 0) {
                sendto(udp_sock, reg_msg.c_str(), reg_msg.length(), 0, (struct sockaddr*)&whisper_addr, sizeof(whisper_addr));
                close(udp_sock);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

void InboundAudioProcessor::start_sip_client_server(int port) {
    sip_udp_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sip_udp_sock_ < 0) return;

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sip_udp_sock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "❌ Failed to bind SIP UDP listener on port " << port << std::endl;
        close(sip_udp_sock_);
        sip_udp_sock_ = -1;
        return;
    }

    std::cout << "👂 SIP UDP listener started on port " << port << std::endl;
    sip_udp_thread_ = std::thread(&InboundAudioProcessor::sip_udp_loop, this);
}

void InboundAudioProcessor::sip_udp_loop() {
    uint8_t buf[2048];
    while (running_.load()) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        ssize_t n = recvfrom(sip_udp_sock_, buf, sizeof(buf), 0, (struct sockaddr*)&client_addr, &client_len);
        if (n < 4) continue;

        uint32_t cid_net;
        memcpy(&cid_net, buf, 4);
        int call_num_id = ntohl(cid_net);
        std::string call_id = std::to_string(call_num_id);

        if (n < 16) continue;

        uint8_t payload_type = buf[4 + 1] & 0x7F;
        const uint8_t* rtp_payload = buf + 4 + 12;
        size_t payload_len = n - 4 - 12;

        process_rtp_audio(call_id, payload_type, rtp_payload, payload_len);
    }
}

void InboundAudioProcessor::cleanup_call(std::shared_ptr<CallState> state) {
    stop_registration_polling(state);
    
    state->connected.store(false);
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->tcp_socket >= 0) {
            send_tcp_bye(state->tcp_socket);
            close(state->tcp_socket);
            state->tcp_socket = -1;
        }
    }
    
    if (state->listen_socket >= 0) {
        close(state->listen_socket);
        state->listen_socket = -1;
    }
    
    if (state->tcp_thread.joinable()) {
        state->tcp_thread.join();
    }
}

std::shared_ptr<InboundAudioProcessor::CallState> InboundAudioProcessor::get_or_create_call_state(const std::string& call_id) {
    std::lock_guard<std::mutex> lock(calls_mutex_);
    auto it = active_calls_.find(call_id);
    if (it != active_calls_.end()) {
        return it->second;
    }
    
    auto state = std::make_shared<CallState>(call_id);
    active_calls_[call_id] = state;

    // Auto-activate for new calls
    std::cout << "📞 Auto-activating for new call: " << call_id << std::endl;
    setup_whisper_tcp_socket(state);
    start_registration_polling(state);

    return state;
}

int InboundAudioProcessor::calculate_whisper_port(const std::string& call_id) {
    return 13001 + calculate_port_offset(call_id);
}

int InboundAudioProcessor::calculate_port_offset(const std::string& call_id) {
    unsigned int hash = 0;
    for (char c : call_id) hash = hash * 31 + c;
    return hash % 1000;
}

void InboundAudioProcessor::maintenance_loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        
        std::vector<std::shared_ptr<CallState>> stale_calls;
        auto now = std::chrono::steady_clock::now();
        
        {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            for (auto it = active_calls_.begin(); it != active_calls_.end(); ) {
                auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - it->second->last_activity).count();
                if (duration > 15) { // 15 seconds of inactivity
                    std::cout << "⏳ Call " << it->first << " timed out (stale for " << duration << "s)" << std::endl;
                    stale_calls.push_back(it->second);
                    it = active_calls_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        
        for (auto& state : stale_calls) {
            cleanup_call(state);
        }
    }
}
