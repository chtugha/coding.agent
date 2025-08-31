#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <atomic>
#include <chrono>

// Service advertisement for external audio processing services
// Audio processor advertises available streams, external services connect

struct AudioStreamInfo {
    std::string call_id;
    int tcp_port;                    // Port where audio processor is listening
    std::string stream_type;         // "pcm_float" for outgoing audio
    int sample_rate;                 // 8000 for G.711
    int channels;                    // 1 for mono
    std::chrono::steady_clock::time_point created_time;
    std::chrono::steady_clock::time_point last_activity;
    bool is_active;
};

class ServiceAdvertiser {
public:
    ServiceAdvertiser();
    ~ServiceAdvertiser();
    
    // Start/stop advertisement server
    bool start(int advertisement_port = 13000);
    void stop();
    
    // Advertise audio stream availability
    bool advertise_stream(const std::string& call_id, int tcp_port, 
                         const std::string& stream_type = "pcm_float");
    bool remove_stream_advertisement(const std::string& call_id);
    
    // Update stream activity
    void update_stream_activity(const std::string& call_id);
    
    // Get all active streams
    std::vector<AudioStreamInfo> get_active_streams() const;
    
private:
    std::atomic<bool> running_;
    int advertisement_port_;
    int server_socket_;
    std::thread server_thread_;
    
    // Active stream advertisements
    std::unordered_map<std::string, AudioStreamInfo> active_streams_;
    mutable std::mutex streams_mutex_;
    
    // Advertisement server methods
    void run_advertisement_server();
    void handle_discovery_request(int client_socket);
    std::string create_advertisement_response() const;
    void cleanup_inactive_streams();
};

// Service discovery for external services to find available streams
class ServiceDiscovery {
public:
    ServiceDiscovery();
    ~ServiceDiscovery();
    
    // Discover available audio streams
    std::vector<AudioStreamInfo> discover_streams(const std::string& server_host = "127.0.0.1", 
                                                 int advertisement_port = 13000);
    
    // Find specific stream by call_id
    bool find_stream(const std::string& call_id, AudioStreamInfo& stream_info,
                    const std::string& server_host = "127.0.0.1", 
                    int advertisement_port = 13000);
    
private:
    std::string query_advertisement_server(const std::string& host, int port);
    std::vector<AudioStreamInfo> parse_advertisement_response(const std::string& response);
};
