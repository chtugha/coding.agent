#pragma once

#include <vector>
#include <string>
#include <cstdint>

// Simple RTP packet structure
struct RTPAudioPacket {
    uint8_t payload_type;                 // Codec identifier
    std::vector<uint8_t> audio_data;      // Raw audio payload
    uint32_t timestamp;                   // RTP timestamp
    uint16_t sequence_number;             // RTP sequence
    
    RTPAudioPacket() : payload_type(0), timestamp(0), sequence_number(0) {}
    
    RTPAudioPacket(uint8_t pt, const std::vector<uint8_t>& data, uint32_t ts = 0, uint16_t seq = 0)
        : payload_type(pt), audio_data(data), timestamp(ts), sequence_number(seq) {}
};

// Session parameters for audio processing
struct AudioSessionParams {
    std::string session_id;               // Database session ID
    std::string caller_phone;             // Caller phone number
    int line_id;                          // SIP line ID
    
    AudioSessionParams() : line_id(-1) {}
    
    AudioSessionParams(const std::string& sid, const std::string& phone, int lid)
        : session_id(sid), caller_phone(phone), line_id(lid) {}
};

// Simple audio processor interface - can be swapped out easily
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;
    
    // Lifecycle
    virtual bool start() = 0;
    virtual void stop() = 0;
    virtual bool is_running() const = 0;
    
    // Session management
    virtual void start_session(const AudioSessionParams& params) = 0;
    virtual void end_session(const std::string& session_id) = 0;
    
    // Audio processing - main method
    virtual void process_audio(const std::string& session_id, const RTPAudioPacket& packet) = 0;
    
    // Configuration
    virtual void set_whisper_endpoint(const std::string& endpoint) = 0;
    virtual std::string get_processor_name() const = 0;
};

// Factory for creating audio processors
class AudioProcessorFactory {
public:
    enum class ProcessorType {
        SIMPLE_PIPELINE,    // Basic pipeline with VAD
        FAST_PIPELINE,      // Optimized for speed
        DEBUG_PIPELINE      // With logging/debugging
    };
    
    static std::unique_ptr<AudioProcessor> create(ProcessorType type);
    static std::vector<std::string> get_available_types();
};

// SIP Client Audio Interface - what the SIP client implements
class SipAudioInterface {
public:
    virtual ~SipAudioInterface() = default;
    
    // Called by audio processor when it needs to send processed audio
    virtual void send_to_whisper(const std::string& session_id, const std::vector<float>& audio_samples) = 0;
    
    // Called by audio processor for status updates
    virtual void on_audio_processing_error(const std::string& session_id, const std::string& error) = 0;
    virtual void on_audio_chunk_ready(const std::string& session_id, size_t chunk_size_samples) = 0;
};
