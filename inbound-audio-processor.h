#pragma once

#include "base-audio-processor.h"
#include "audio-processor-interface.h"
#include "simple-audio-processor.h"
#include <thread>
#include <functional>

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

    // Audio processing interface
    void process_rtp_audio(const RTPAudioPacket& packet);

    // Network interface for SIP client communication
    void start_sip_client_server(int port);

    // Call management
    void activate_for_call(const std::string& call_id) override;
    void deactivate_after_call() override;
    std::string get_current_call_id() const { return current_call_id_; }

    // Status
    ProcessorStatus get_status() const override;

private:
    // Audio processor implementation
    class InboundAudioInterface : public SipAudioInterface {
    public:
        InboundAudioInterface(InboundAudioProcessor* processor);
        void send_to_whisper(const std::string& call_id, const std::vector<float>& audio_samples) override;
        void on_audio_processing_error(const std::string& call_id, const std::string& error) override;
        void on_audio_chunk_ready(const std::string& call_id, size_t chunk_size_samples) override;

    private:
        InboundAudioProcessor* processor_;
    };

    // Core audio processor
    std::unique_ptr<AudioProcessor> audio_processor_;
    std::unique_ptr<InboundAudioInterface> audio_interface_;

    // TCP connection to Whisper service
    int whisper_listen_socket_;
    int whisper_tcp_socket_;
    int whisper_tcp_port_;
    std::atomic<bool> whisper_connected_;
    std::thread whisper_tcp_thread_;

    // Registration polling
    std::atomic<bool> registration_running_;
    std::thread registration_thread_;

    std::mutex whisper_mutex_;

    // Internal methods
    bool setup_whisper_tcp_socket(const std::string& call_id);
    void handle_whisper_tcp_connection();
    void forward_to_whisper(const std::vector<float>& audio_samples);
    void send_tcp_audio_chunk(int socket_fd, const std::vector<float>& audio_samples);
    bool has_whisper_connected() const;

    // Registration polling
    void start_registration_polling(const std::string& call_id);
    void stop_registration_polling();
    void registration_polling_thread(const std::string& call_id);

    // Port calculation
    int calculate_whisper_port(const std::string& call_id);
};
