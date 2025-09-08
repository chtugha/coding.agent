#pragma once

#include "audio-processor-interface.h"
#include "simple-audio-processor.h"
#include "database.h"
#include "jitter-buffer.h"
#include "service-advertisement.h"
#include <memory>
#include <string>
#include <atomic>
#include <thread>
#include <functional>
#include <unordered_map>

// Forward declarations removed - no WhisperService dependency

// Standalone Audio Processor Service
// Runs independently, SIP client connects to it via interface
class AudioProcessorService {
public:
    AudioProcessorService();
    ~AudioProcessorService();
    
    // Service lifecycle
    bool start(int port = 8083);
    void stop();
    bool is_running() const { return running_.load(); }

    // Sleep/Wake mechanism
    void activate_for_call(const std::string& call_id = "");   // Wake up processor for incoming call
    void deactivate_after_call(); // Put processor to sleep after call ends
    bool is_active() const { return active_.load(); }
    
    // Configuration
    void set_database(Database* database);
    // Whisper service methods removed - clean output connector only
    
    // Audio processing interface for SIP clients (session management removed)
    void process_audio(const RTPAudioPacket& packet);

    // Connection management (session management removed)
    void set_sip_client_callback(std::function<void(const std::vector<uint8_t>&)> callback);
    void handle_outgoing_audio(const std::vector<uint8_t>& audio_data);
    
    // Status
    struct ServiceStatus {
        bool is_running;
        std::string processor_type;
        size_t total_packets_processed;
        std::string whisper_endpoint;
    };
    ServiceStatus get_status() const;
    
private:
    // Audio processor implementation
    class ServiceAudioInterface : public SipAudioInterface {
    public:
        ServiceAudioInterface(AudioProcessorService* service);
        
        void send_to_whisper(const std::string& call_id, const std::vector<float>& audio_samples) override;
        void on_audio_processing_error(const std::string& call_id, const std::string& error) override;
        void on_audio_chunk_ready(const std::string& call_id, size_t chunk_size_samples) override;
        
    private:
        AudioProcessorService* service_;
    };
    
    std::unique_ptr<AudioProcessor> audio_processor_;
    std::unique_ptr<ServiceAudioInterface> audio_interface_;

    // Background connectors removed - clean output connector only

    // Service state
    std::atomic<bool> running_;
    std::atomic<bool> active_;  // true = processing calls, false = sleeping
    int service_port_;
    
    // Configuration
    Database* database_;

    // Statistics
    std::atomic<size_t> total_packets_processed_;

    // Connection to SIP client (session management removed)
    std::function<void(const std::vector<uint8_t>&)> sip_client_callback_;

    // TCP socket management
    int outgoing_tcp_socket_;  // For sending audio to external services
    int incoming_tcp_socket_;  // For receiving audio from external services
    int outgoing_tcp_port_;    // Port for outgoing connections (calculated from call_id)
    int incoming_tcp_port_;    // Port for incoming connections (calculated from call_id)
    std::atomic<bool> outgoing_connected_;
    std::atomic<bool> incoming_connected_;
    std::string current_call_id_;
    std::mutex tcp_mutex_;

    // TCP connection threads
    std::thread outgoing_tcp_thread_;
    std::thread incoming_tcp_thread_;

    // Service advertisement for external services
    std::unique_ptr<ServiceAdvertiser> service_advertiser_;

    // Audio processing buffers (session management removed)
    std::unique_ptr<AudioChunkBuffer> incoming_audio_buffer_;
    std::unique_ptr<RTPPacketBuffer> outgoing_audio_buffer_;
    std::mutex buffers_mutex_;
    
    // Internal methods (session management removed)
    void handle_whisper_transcription(const std::vector<float>& audio_samples);
    bool check_sip_client_connection();
    std::string simulate_whisper_transcription(const std::vector<float>& audio_samples);

    // TCP connection methods
    bool has_external_peer_connected() const;
    void forward_to_external_service(const std::vector<float>& audio_samples);

    // TCP socket management
    bool setup_outgoing_tcp_socket(const std::string& call_id);
    bool setup_incoming_tcp_socket(const std::string& call_id);
    void handle_outgoing_tcp_connection();
    void handle_incoming_tcp_connection();
    void send_tcp_hello(int socket_fd, const std::string& call_id);
    void send_tcp_audio_chunk(int socket_fd, const std::vector<float>& audio_samples);
    void send_tcp_bye(int socket_fd);

    // Port calculation from call_id (direct numeric conversion)
    int calculate_outgoing_port(const std::string& call_id);
    int calculate_incoming_port(const std::string& call_id);

    // Audio buffer processing (session management removed)
    void process_buffered_audio();
    void process_outgoing_buffer();
    // Removed: G.711 conversion functions - using shared functions from SimpleAudioProcessor
    std::vector<uint8_t> convert_float_to_g711_ulaw(const std::vector<float>& samples);
};

// Audio Processor Service Factory
class AudioProcessorServiceFactory {
public:
    enum class ProcessorType {
        SIMPLE,
        FAST,
        DEBUG
    };
    
    static std::unique_ptr<AudioProcessorService> create();
};
