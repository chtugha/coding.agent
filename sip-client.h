#pragma once

#include "database.h"
#include "jitter-buffer.h"
#include "rtp-packet.h"
#include <string>
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <thread>
#include <queue>
#include <chrono>
#include <condition_variable>
#include <unordered_map>

// AI processing parameters removed - SIP client is now pure SIP protocol handler

// LLaMA helper functions removed - SIP client is pure SIP protocol handler

// SIP client configuration
struct SipClientConfig {
    std::string client_id;
    std::string username;
    std::string password;
    std::string server_ip;
    int server_port = 5060;
    std::string display_name;
    bool auto_answer = true;
    int expires = 3600; // Registration expiry in seconds
    
    // AI configuration
    std::string ai_persona = "helpful assistant";
    std::string greeting = "Hello! How can I help you today?";
    bool use_tts = true;
    std::string tts_voice = "default";
};

// Audio buffer for RTP processing
struct AudioBuffer {
    std::vector<float> samples;
    std::mutex mutex;
    std::condition_variable cv;
    bool has_data = false;
    
    void add_samples(const std::vector<float>& new_samples);
    bool get_samples(std::vector<float>& output, int timeout_ms = 1000);
    void clear();
};

// SipCallSession struct removed - no session management

// Individual SIP client (represents one phone number/extension)
class SipClient {
public:
    SipClient(const SipClientConfig& config);
    ~SipClient();
    
    // SIP operations
    bool start();
    bool stop();
    bool is_registered() const { return is_registered_.load(); }
    bool is_running() const { return is_running_.load(); }
    
    // Configuration
    const SipClientConfig& get_config() const { return config_; }
    void update_config(const SipClientConfig& config);
    
    // Call management removed
    
    // Statistics
    struct Stats {
        int total_calls = 0;
        int active_calls = 0;
        std::chrono::steady_clock::time_point last_call_time;
        std::chrono::seconds total_call_duration{0};
    };
    Stats get_stats() const;

private:
    SipClientConfig config_;
    
    // SIP state
    std::atomic<bool> is_running_{false};
    std::atomic<bool> is_registered_{false};
    
    // Call management removed
    
    // Threading
    std::thread sip_thread_;
    std::thread audio_thread_;
    
    // Statistics
    mutable std::mutex stats_mutex_;
    Stats stats_;
    
    // Internal methods
    void sip_worker();
    void audio_worker();
    // Call handling methods removed
    
    // Simple audio routing (no session management)
    void handle_incoming_rtp(const std::vector<uint8_t>& rtp_data);
    void handle_outgoing_audio(const std::vector<uint8_t>& audio_data);
    void send_rtp_packet_to_network(const std::vector<uint8_t>& rtp_packet);
};

// SIP client manager - manages multiple SIP clients
class SipClientManager {
public:
    SipClientManager();
    ~SipClientManager();
    
    // Initialize with shared AI resources
    bool init();
    
    // Client management
    bool add_client(const SipClientConfig& config);
    bool remove_client(const std::string& client_id);
    bool update_client(const std::string& client_id, const SipClientConfig& config);
    
    // Operations
    bool start_all_clients();
    bool stop_all_clients();
    bool start_client(const std::string& client_id);
    bool stop_client(const std::string& client_id);
    
    // Information
    std::vector<SipClientConfig> get_all_clients() const;
    std::vector<std::string> get_active_clients() const;
    SipClient::Stats get_client_stats(const std::string& client_id) const;
    
    // AI processing removed - SIP client manager is pure SIP protocol handler

private:
    // Database for SIP line configuration
    Database database_;

    // Client management
    std::unordered_map<std::string, std::unique_ptr<SipClient>> clients_;
    mutable std::mutex clients_mutex_;

    // Initialization state
    bool is_initialized_ = false;
};

// Utility functions
std::vector<float> convert_rtp_to_float(const uint8_t* rtp_data, size_t length);
std::vector<uint8_t> convert_float_to_rtp(const std::vector<float>& audio_data);
bool is_valid_sip_config(const SipClientConfig& config);
