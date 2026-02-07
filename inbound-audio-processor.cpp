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

    std::cout << "🚀 Inbound Audio Processor started (MULTI-CALL) on base port " << base_port << std::endl;
    return true;
}

void InboundAudioProcessor::stop() {
    if (!running_.load()) return;
    
    running_.store(false);
    
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
    if (!running_.load()) return;

    auto state = get_or_create_call_state(call_id);
    if (!state) return;

    state->last_activity = std::chrono::steady_clock::now();

    // Fast audio decoding
    std::vector<float> audio_samples = decode_rtp_audio(packet, call_id);
    if (audio_samples.empty()) return;

    process_call_audio(state, audio_samples);
    total_packets_processed_.fetch_add(1);
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

// Internal processing methods
std::vector<float> InboundAudioProcessor::decode_rtp_audio(const RTPAudioPacket& packet, const std::string& /*call_id*/) {
    if (packet.audio_data.empty()) return {};

    // Handle DTMF events (RFC 4733) - payload type 101
    if (packet.payload_type == 101) {
        // DTMF handling could be added here if needed
        return {};
    }

    // Fast codec detection and decoding
    switch (packet.payload_type) {
        case 0:  // G.711 μ-law
            return convert_g711_ulaw(packet.audio_data);
        case 8:  // G.711 A-law
            return convert_g711_alaw(packet.audio_data);
        case 10: // PCM 16-bit
        case 11:
            return convert_pcm16(packet.audio_data);
        default:
            return {}; // Unsupported codec
    }
}

std::vector<float> InboundAudioProcessor::convert_g711_ulaw(const std::vector<uint8_t>& data) {
    std::vector<float> samples;
    samples.reserve(data.size() * 2); // Upsample 8kHz -> 16kHz

    for (size_t i = 0; i < data.size(); ++i) {
        float s = ulaw_table[data[i]];
        float next = (i + 1 < data.size()) ? ulaw_table[data[i + 1]] : s;
        samples.push_back(s);
        samples.push_back(0.5f * (s + next)); // Linear interpolation upsampling
    }
    return samples;
}

std::vector<float> InboundAudioProcessor::convert_g711_alaw(const std::vector<uint8_t>& data) {
    std::vector<float> samples;
    samples.reserve(data.size() * 2);

    for (size_t i = 0; i < data.size(); ++i) {
        float s = alaw_table[data[i]];
        float next = (i + 1 < data.size()) ? alaw_table[data[i + 1]] : s;
        samples.push_back(s);
        samples.push_back(0.5f * (s + next));
    }
    return samples;
}

std::vector<float> InboundAudioProcessor::convert_pcm16(const std::vector<uint8_t>& data) {
    std::vector<float> samples;
    samples.reserve(data.size() / 2);
    for (size_t i = 0; i < data.size(); i += 2) {
        if (i + 1 < data.size()) {
            int16_t sample = (data[i + 1] << 8) | data[i];
            samples.push_back(sample / 32768.0f);
        }
    }
    return samples;
}

void InboundAudioProcessor::process_call_audio(std::shared_ptr<CallState> state, const std::vector<float>& samples) {
    if (samples.empty()) return;
    forward_to_whisper(state, samples);
}

int InboundAudioProcessor::get_system_speed_from_database() {
    if (database_) {
        return database_->get_system_speed();
    }
    return 3;
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
        if (client_socket < 0) break;
        
        state->tcp_socket = client_socket;
        state->connected.store(true);
        
        std::cout << "🔗 Whisper client connected for call " << state->call_id << std::endl;
        send_tcp_hello(state->tcp_socket, state->call_id);
        
        while (running_.load() && state->connected.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        
        close(state->tcp_socket);
        state->tcp_socket = -1;
        state->connected.store(false);
    }
}

void InboundAudioProcessor::forward_to_whisper(std::shared_ptr<CallState> state, const std::vector<float>& audio_samples) {
    if (!state->connected.load() || state->tcp_socket < 0) return;
    send_tcp_audio_chunk(state, audio_samples);
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
        if (state->connected.load()) break;

        int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
        if (udp_sock >= 0) {
            sendto(udp_sock, reg_msg.c_str(), reg_msg.length(), 0, (struct sockaddr*)&whisper_addr, sizeof(whisper_addr));
            close(udp_sock);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

void InboundAudioProcessor::cleanup_call(std::shared_ptr<CallState> state) {
    stop_registration_polling(state);
    
    state->connected.store(false);
    if (state->tcp_socket >= 0) {
        send_tcp_bye(state->tcp_socket);
        close(state->tcp_socket);
        state->tcp_socket = -1;
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
    return state;
}

int InboundAudioProcessor::calculate_whisper_port(const std::string& call_id) {
    return 9001 + calculate_port_offset(call_id);
}

int InboundAudioProcessor::calculate_port_offset(const std::string& call_id) {
    unsigned int hash = 0;
    for (char c : call_id) hash = hash * 31 + c;
    return hash % 1000;
}
