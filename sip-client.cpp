#include "sip-client.h"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <regex>

// AudioBuffer implementation
void AudioBuffer::add_samples(const std::vector<float>& new_samples) {
    std::lock_guard<std::mutex> lock(mutex);
    samples.insert(samples.end(), new_samples.begin(), new_samples.end());
    has_data = true;
    cv.notify_one();
}

bool AudioBuffer::get_samples(std::vector<float>& output, int timeout_ms) {
    std::unique_lock<std::mutex> lock(mutex);
    
    if (!cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] { return has_data; })) {
        return false; // Timeout
    }
    
    if (samples.empty()) {
        has_data = false;
        return false;
    }
    
    // Get available samples (or up to 1 second worth)
    const size_t max_samples = 16000; // 1 second at 16kHz
    size_t samples_to_take = std::min(samples.size(), max_samples);
    
    output.assign(samples.begin(), samples.begin() + samples_to_take);
    samples.erase(samples.begin(), samples.begin() + samples_to_take);
    
    if (samples.empty()) {
        has_data = false;
    }
    
    return true;
}

void AudioBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex);
    samples.clear();
    has_data = false;
}

// SipClient implementation
SipClient::SipClient(const SipClientConfig& config) : config_(config) {
    printf("Created SIP client: %s (%s@%s:%d)\n", 
           config_.client_id.c_str(), config_.username.c_str(), 
           config_.server_ip.c_str(), config_.server_port);
}

SipClient::~SipClient() {
    stop();
}

bool SipClient::start() {
    if (is_running_.load()) {
        return false;
    }
    
    printf("Starting SIP client: %s\n", config_.client_id.c_str());
    
    is_running_.store(true);
    
    // Start SIP worker thread
    sip_thread_ = std::thread(&SipClient::sip_worker, this);
    
    // Start audio processing thread
    audio_thread_ = std::thread(&SipClient::audio_worker, this);
    
    return true;
}

bool SipClient::stop() {
    if (!is_running_.load()) {
        return false;
    }
    
    printf("Stopping SIP client: %s\n", config_.client_id.c_str());
    
    is_running_.store(false);
    is_registered_.store(false);
    
    // Session management removed
    
    // Wait for threads to finish
    if (sip_thread_.joinable()) {
        sip_thread_.join();
    }
    if (audio_thread_.joinable()) {
        audio_thread_.join();
    }
    
    return true;
}

void SipClient::update_config(const SipClientConfig& config) {
    bool was_running = is_running_.load();
    
    if (was_running) {
        stop();
    }
    
    config_ = config;
    
    if (was_running) {
        start();
    }
}

// Call management methods removed

SipClient::Stats SipClient::get_stats() const {
    std::lock_guard<std::mutex> lock(stats_mutex_);
    
    Stats current_stats = stats_;
    
    // Active calls count removed
    current_stats.active_calls = 0;
    
    return current_stats;
}

void SipClient::sip_worker() {
    printf("SIP worker started for client: %s\n", config_.client_id.c_str());
    
    // Simulate SIP registration
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    is_registered_.store(true);
    printf("SIP client registered: %s@%s\n", config_.username.c_str(), config_.server_ip.c_str());
    
    // Main SIP processing loop
    while (is_running_.load()) {
        // TODO: Implement actual SIP protocol handling
        // For now, simulate incoming calls for testing
        
        if (is_registered_.load()) {
            // Simulate an incoming call every 30 seconds for testing
            static auto last_test_call = std::chrono::steady_clock::now();
            auto now = std::chrono::steady_clock::now();
            
            if (std::chrono::duration_cast<std::chrono::seconds>(now - last_test_call).count() > 30) {
                // Simulate incoming call
                std::string call_id = "test_call_" + std::to_string(std::time(nullptr));
                std::string caller = "+1234567890";
                
                // handle_incoming_call removed - no session management
                last_test_call = now;
            }
        }
        
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    printf("SIP worker stopped for client: %s\n", config_.client_id.c_str());
}

void SipClient::audio_worker() {
    printf("Audio worker started for client: %s\n", config_.client_id.c_str());

    while (is_running_.load()) {
        // Session management removed - simplified audio worker
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    printf("Audio worker stopped for client: %s\n", config_.client_id.c_str());
}

// handle_incoming_call method removed

// handle_call_ended method removed

// Audio processing methods removed - session management eliminated

// generate_tts_audio method removed - no TTS processing in SIP client

// SipClientManager implementation
SipClientManager::SipClientManager() {
}

SipClientManager::~SipClientManager() {
    stop_all_clients();
}

bool SipClientManager::init() {
    // Initialize database
    if (!database_.init()) {
        printf("Warning: Database initialization failed\n");
        return false;
    }

    is_initialized_ = true;
    printf("✅ SIP Client Manager initialized (pure SIP protocol handler)\n");
    return true;
}

bool SipClientManager::add_client(const SipClientConfig& config) {
    if (!is_initialized_) {
        fprintf(stderr, "SipClientManager not initialized\n");
        return false;
    }

    if (!is_valid_sip_config(config)) {
        fprintf(stderr, "Invalid SIP client configuration\n");
        return false;
    }

    std::lock_guard<std::mutex> lock(clients_mutex_);

    if (clients_.find(config.client_id) != clients_.end()) {
        fprintf(stderr, "SIP client already exists: %s\n", config.client_id.c_str());
        return false;
    }

    auto client = std::make_unique<SipClient>(config);
    clients_[config.client_id] = std::move(client);

    printf("Added SIP client: %s\n", config.client_id.c_str());
    return true;
}

bool SipClientManager::remove_client(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(clients_mutex_);

    auto it = clients_.find(client_id);
    if (it == clients_.end()) {
        return false;
    }

    it->second->stop();
    clients_.erase(it);

    printf("Removed SIP client: %s\n", client_id.c_str());
    return true;
}

bool SipClientManager::update_client(const std::string& client_id, const SipClientConfig& config) {
    if (!is_valid_sip_config(config)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(clients_mutex_);

    auto it = clients_.find(client_id);
    if (it == clients_.end()) {
        return false;
    }

    it->second->update_config(config);
    printf("Updated SIP client: %s\n", client_id.c_str());
    return true;
}

bool SipClientManager::start_all_clients() {
    std::lock_guard<std::mutex> lock(clients_mutex_);

    bool all_started = true;
    for (auto& [client_id, client] : clients_) {
        if (!client->start()) {
            all_started = false;
            fprintf(stderr, "Failed to start SIP client: %s\n", client_id.c_str());
        }
    }

    printf("Started %zu SIP clients\n", clients_.size());
    return all_started;
}

bool SipClientManager::stop_all_clients() {
    std::lock_guard<std::mutex> lock(clients_mutex_);

    for (auto& [client_id, client] : clients_) {
        client->stop();
    }

    printf("Stopped all SIP clients\n");
    return true;
}

bool SipClientManager::start_client(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(clients_mutex_);

    auto it = clients_.find(client_id);
    if (it == clients_.end()) {
        return false;
    }

    return it->second->start();
}

bool SipClientManager::stop_client(const std::string& client_id) {
    std::lock_guard<std::mutex> lock(clients_mutex_);

    auto it = clients_.find(client_id);
    if (it == clients_.end()) {
        return false;
    }

    return it->second->stop();
}

std::vector<SipClientConfig> SipClientManager::get_all_clients() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);

    std::vector<SipClientConfig> configs;
    for (const auto& [client_id, client] : clients_) {
        configs.push_back(client->get_config());
    }

    return configs;
}

std::vector<std::string> SipClientManager::get_active_clients() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);

    std::vector<std::string> active_clients;
    for (const auto& [client_id, client] : clients_) {
        if (client->is_running() && client->is_registered()) {
            active_clients.push_back(client_id);
        }
    }

    return active_clients;
}

SipClient::Stats SipClientManager::get_client_stats(const std::string& client_id) const {
    std::lock_guard<std::mutex> lock(clients_mutex_);

    auto it = clients_.find(client_id);
    if (it != clients_.end()) {
        return it->second->get_stats();
    }

    return SipClient::Stats{};
}

// Large AI processing method removed

// AI processing methods removed - SIP client manager is pure SIP protocol handler

void SipClient::handle_incoming_rtp(const std::vector<uint8_t>& rtp_data) {
    // Simple RTP handling - forward to audio processor
    std::cout << "📥 RTP packet received: size=" << rtp_data.size() << std::endl;

    // TODO: Forward to audio processor service
    // audio_processor_->process_rtp_packet(rtp_data);
}

void SipClient::handle_outgoing_audio(const std::vector<uint8_t>& audio_data) {
    // Simple audio handling - create RTP packet and send
    std::cout << "📤 Sending audio data: " << audio_data.size() << " bytes" << std::endl;

    // TODO: Create proper RTP packet and send
    send_rtp_packet_to_network(audio_data);
}

void SipClient::send_rtp_packet_to_network(const std::vector<uint8_t>& rtp_packet) {
    // Simple RTP packet transmission
    std::cout << "🌐 Sending RTP packet: " << rtp_packet.size() << " bytes" << std::endl;

    // TODO: Implement actual UDP socket transmission
    // Example UDP transmission (commented out):
    /*
    struct sockaddr_in dest_addr;
    // ... set up destination from SDP negotiation

    int rtp_socket = socket(AF_INET, SOCK_DGRAM, 0);
    if (rtp_socket < 0) return;

    ssize_t sent = sendto(rtp_socket, rtp_packet.data(), rtp_packet.size(), 0,
                         (struct sockaddr*)&dest_addr, sizeof(dest_addr));

    if (sent != (ssize_t)rtp_packet.size()) {
        std::cout << "❌ Failed to send complete RTP packet" << std::endl;
    }

    close(rtp_socket);
    */
}

// process_incoming_rtp_packet method removed - session management eliminated

// Utility functions
std::vector<float> convert_rtp_to_float(const uint8_t* rtp_data, size_t length) {
    std::vector<float> result;

    // Assume 16-bit PCM for now
    if (length % 2 != 0) {
        return result; // Invalid data
    }

    result.reserve(length / 2);
    for (size_t i = 0; i < length; i += 2) {
        int16_t sample = static_cast<int16_t>(rtp_data[i] | (rtp_data[i + 1] << 8));
        result.push_back(static_cast<float>(sample) / 32768.0f);
    }

    return result;
}

std::vector<uint8_t> convert_float_to_rtp(const std::vector<float>& audio_data) {
    std::vector<uint8_t> result;
    result.reserve(audio_data.size() * 2);

    for (float sample : audio_data) {
        // Clamp to [-1.0, 1.0] and convert to 16-bit
        sample = std::max(-1.0f, std::min(1.0f, sample));
        int16_t int_sample = static_cast<int16_t>(sample * 32767.0f);

        result.push_back(static_cast<uint8_t>(int_sample & 0xFF));
        result.push_back(static_cast<uint8_t>((int_sample >> 8) & 0xFF));
    }

    return result;
}

bool is_valid_sip_config(const SipClientConfig& config) {
    if (config.client_id.empty() || config.username.empty() ||
        config.server_ip.empty() || config.server_port <= 0) {
        return false;
    }

    // Basic IP address validation
    std::regex ip_regex(R"(^(?:[0-9]{1,3}\.){3}[0-9]{1,3}$)");
    if (!std::regex_match(config.server_ip, ip_regex)) {
        return false;
    }

    return true;
}
