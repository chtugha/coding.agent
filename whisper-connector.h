#pragma once

#include <vector>
#include <string>
#include <functional>
#include <atomic>
#include <thread>
#include <queue>
#include <mutex>
#include <condition_variable>
#include "jitter-buffer.h"
#include "rtp-packet.h"

// Direct interface for routing chunks to Whisper service (no HTTP overhead)
class WhisperConnector {
public:
    WhisperConnector();
    ~WhisperConnector();

    // Connection management
    bool start();
    void stop();
    bool is_connected() const { return connected_.load(); }

    // Audio chunk routing - direct function calls
    void send_chunk(const std::string& session_id, const std::vector<float>& audio_chunk);
    std::string transcribe_chunk(const std::string& session_id, const std::vector<float>& audio_chunk);

    // Connection monitoring callback
    void set_connection_callback(std::function<void(bool)> callback) { connection_callback_ = callback; }
    
private:
    struct AudioChunk {
        std::string session_id;
        std::vector<float> audio_data;
    };

    std::atomic<bool> running_;
    std::atomic<bool> connected_;

    // Background processing
    std::thread worker_thread_;
    std::queue<AudioChunk> chunk_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;

    // Connection monitoring
    std::function<void(bool)> connection_callback_;
    
    // Background mechanisms
    void worker_loop();
    bool check_whisper_connection();
    void update_connection_status();
};

// Background mechanism for routing Piper audio to RTP
class PiperConnector {
public:
    PiperConnector();
    ~PiperConnector();
    
    // Connection management  
    bool start();
    void stop();
    bool is_sip_client_connected() const { return sip_connected_.load(); }
    
    // Audio routing
    void send_piper_audio(const std::string& session_id, const std::vector<uint8_t>& audio_data);
    void set_sip_client_callback(std::function<void(const std::string&, const std::vector<uint8_t>&)> callback);
    
private:
    struct PiperAudio {
        std::string session_id;
        std::vector<uint8_t> audio_data;
    };
    
    std::atomic<bool> running_;
    std::atomic<bool> sip_connected_;
    
    // Background processing
    std::thread worker_thread_;
    std::queue<PiperAudio> audio_queue_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    
    // SIP client connection
    std::function<void(const std::string&, const std::vector<uint8_t>&)> sip_callback_;

    // Jitter buffer for smooth outgoing audio
    std::unique_ptr<RTPPacketBuffer> outgoing_jitter_buffer_;

    // RFC 3550 compliant RTP session for TTS audio
    std::unique_ptr<RTPSession> rtp_session_;

    // Background mechanisms
    void worker_loop();
    void process_jitter_buffer();
    std::vector<uint8_t> convert_to_g711_rtp(const std::vector<uint8_t>& piper_audio);
    RTPPacket create_rtp_packet_from_audio(const std::vector<uint8_t>& audio_data);
};
