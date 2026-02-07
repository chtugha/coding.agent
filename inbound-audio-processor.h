#pragma once

#include "base-audio-processor.h"
#include "audio-processor-interface.h"
#include <thread>
#include <functional>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <vector>
#include <chrono>

// Specialized processor for inbound audio: Phone → Whisper
// Handles RTP packets from SIP client, converts G.711 to float32 PCM,
// upsamples from 8kHz to 16kHz, and forwards to Whisper service via TCP
class InboundAudioProcessor : public BaseAudioProcessor {
public:
    InboundAudioProcessor();
    ~InboundAudioProcessor() override;

    // Service lifecycle
    bool start(int base_port) override;
    void stop() override;

    // Audio processing interface (extended for multiple calls)
    void process_rtp_audio(const std::string& call_id, const RTPAudioPacket& packet);
    
    // Legacy support for single-call interface
    void process_rtp_audio(const RTPAudioPacket& packet);

    // Network interface for SIP client communication
    void start_sip_client_server(int port);

    // Call management
    void activate_for_call(const std::string& call_id) override;
    void deactivate_after_call() override;
    void deactivate_call(const std::string& call_id);
    
    bool is_call_active(const std::string& call_id) const;
    std::string get_current_call_id() const { return current_call_id_; }

    // Status
    ProcessorStatus get_status() const override;

private:
    // Internal state for each call
    struct CallState {
        std::string call_id;
        
        // Whisper TCP connection
        int listen_socket = -1;
        int tcp_socket = -1;
        int tcp_port = -1;
        std::atomic<bool> connected{false};
        std::thread tcp_thread;
        
        // Audio processing state (merged from SimpleAudioProcessor)
        std::vector<float> audio_buffer;
        std::mutex buffer_mutex;
        
        // Registration polling
        std::atomic<bool> registration_running{false};
        std::thread registration_thread;
        
        // Activity timing
        std::chrono::steady_clock::time_point last_activity;

        CallState(const std::string& id) : call_id(id) {
            last_activity = std::chrono::steady_clock::now();
        }
        
        ~CallState() {
            // Cleanup in InboundAudioProcessor::cleanup_call
        }
    };

    // Multi-call management
    std::unordered_map<std::string, std::shared_ptr<CallState>> active_calls_;
    mutable std::mutex calls_mutex_;

    // Internal processing methods (moved from SimpleAudioProcessor)
    std::vector<float> decode_rtp_audio(const RTPAudioPacket& packet, const std::string& call_id);
    std::vector<float> convert_g711_ulaw(const std::vector<uint8_t>& data);
    std::vector<float> convert_g711_alaw(const std::vector<uint8_t>& data);
    std::vector<float> convert_pcm16(const std::vector<uint8_t>& data);
    
    void process_call_audio(std::shared_ptr<CallState> state, const std::vector<float>& samples);
    int get_system_speed_from_database();

    // Connection management
    bool setup_whisper_tcp_socket(std::shared_ptr<CallState> state);
    void handle_whisper_tcp_connection(std::shared_ptr<CallState> state);
    void forward_to_whisper(std::shared_ptr<CallState> state, const std::vector<float>& audio_samples);
    void send_tcp_audio_chunk(std::shared_ptr<CallState> state, const std::vector<float>& audio_samples);
    void send_tcp_hello(int socket_fd, const std::string& call_id);
    void send_tcp_bye(int socket_fd);
    bool write_all_fd(int fd, const void* data, size_t len);

    // Registration polling
    void start_registration_polling(std::shared_ptr<CallState> state);
    void stop_registration_polling(std::shared_ptr<CallState> state);
    void registration_polling_thread(std::shared_ptr<CallState> state);

    // Cleanup
    void cleanup_call(std::shared_ptr<CallState> state);
    std::shared_ptr<CallState> get_or_create_call_state(const std::string& call_id);

    // Port calculation
    int calculate_whisper_port(const std::string& call_id);
    int calculate_port_offset(const std::string& call_id);
};
