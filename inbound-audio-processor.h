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
    void process_rtp_audio(const std::string& call_id, uint8_t payload_type, const uint8_t* audio_data, size_t audio_len);
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
        std::mutex mutex; // Protects tcp_socket and buffers
        
        // Pre-allocated buffers for zero-allocation processing
        std::vector<float> pcm_buffer;
        
        // Initial buffering while waiting for TCP connection
        std::vector<float> initial_buffer;
        static constexpr size_t MAX_INITIAL_BUFFER_SAMPLES = 16000 * 10; // 10 seconds
        
        // Activity timing
        std::chrono::steady_clock::time_point last_activity;

        // Registration polling
        std::atomic<bool> registration_running{false};
        std::thread registration_thread;

        CallState(const std::string& id) : call_id(id) {
            last_activity = std::chrono::steady_clock::now();
            pcm_buffer.reserve(640); // 20ms at 16kHz is 320 samples, upsampling from 8kHz
        }
        
        ~CallState() {
            // Cleanup in InboundAudioProcessor::cleanup_call
        }
    };

    // Multi-call management
    std::unordered_map<std::string, std::shared_ptr<CallState>> active_calls_;
    mutable std::mutex calls_mutex_;

    // Connection management
    bool setup_whisper_tcp_socket(std::shared_ptr<CallState> state);
    void handle_whisper_tcp_connection(std::shared_ptr<CallState> state);
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

    // Control socket management
    void start_control_socket();
    void control_socket_loop();
    int ctrl_socket_ = -1;
    std::thread ctrl_thread_;

    // SIP UDP listener
    void sip_udp_loop();
    int sip_udp_sock_ = -1;
    std::thread sip_udp_thread_;

    // Maintenance thread for stale calls
    void maintenance_loop();
    std::thread maintenance_thread_;
};
