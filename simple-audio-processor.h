#pragma once

#include "audio-processor-interface.h"
#include "database.h"
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <chrono>

// Fast, simple audio processor - minimal overhead
class SimpleAudioProcessor : public AudioProcessor {
public:
    SimpleAudioProcessor(SipAudioInterface* sip_interface);
    ~SimpleAudioProcessor();
    
    // AudioProcessor interface
    bool start() override;
    void stop() override;
    bool is_running() const override { return running_.load(); }
    
    void start_session(const AudioSessionParams& params) override;
    void end_session(const std::string& call_id) override;
    void process_audio(const std::string& call_id, const RTPAudioPacket& packet) override;
    
    void set_whisper_endpoint(const std::string& endpoint) override { whisper_endpoint_ = endpoint; }
    std::string get_processor_name() const override { return "SimpleAudioProcessor"; }
    
    // Configuration
    void set_chunk_duration_ms(int ms) { chunk_duration_ms_ = ms; }
    void set_vad_threshold(float threshold) { vad_threshold_ = threshold; }
    void set_silence_timeout_ms(int ms) { silence_timeout_ms_ = ms; }
    void set_database(Database* database) { database_ = database; }

    // Static versions for shared use
    static std::vector<float> convert_g711_ulaw_static(const std::vector<uint8_t>& data);
    static std::vector<float> convert_g711_alaw_static(const std::vector<uint8_t>& data);

private:
    SipAudioInterface* sip_interface_;
    std::atomic<bool> running_;

    // Sessionless audio processing
    std::vector<float> global_audio_buffer_;
    std::mutex audio_buffer_mutex_;
    std::chrono::steady_clock::time_point last_speech_time_;
    std::chrono::steady_clock::time_point chunk_start_time_;
    bool has_speech_;
    int sample_rate_;
    
    // Configuration
    std::string whisper_endpoint_;
    int chunk_duration_ms_;
    float vad_threshold_;
    int silence_timeout_ms_;
    Database* database_;

    // Chunking configuration
    static constexpr size_t TARGET_CHUNK_SIZE = 16000 * 2; // 2 seconds at 16kHz
    static constexpr float SILENCE_THRESHOLD = 0.01f;
    static constexpr int DEFAULT_SYSTEM_SPEED = 3; // Medium speed
    
    // Fast audio processing methods
    std::vector<float> decode_rtp_audio(const RTPAudioPacket& packet);
    std::vector<float> convert_g711_ulaw(const std::vector<uint8_t>& data);
    std::vector<float> convert_g711_alaw(const std::vector<uint8_t>& data);
    std::vector<float> convert_pcm16(const std::vector<uint8_t>& data);

    // Audio chunk processing
    void send_audio_chunk_sessionless();

    // DTMF handling (RFC 4733)
    void handle_dtmf_event(const RTPAudioPacket& packet);
    
    // Fast VAD
    bool has_speech(const std::vector<float>& samples);
    float calculate_energy(const std::vector<float>& samples);
    
    // Chunk management (sessionless)
    bool should_send_chunk_sessionless();
    std::vector<float> prepare_whisper_chunk(const std::vector<float>& audio);

    // Advanced chunking with system speed (consumes from pcm_data as chunks are created)
    std::vector<std::vector<float>> create_chunks_from_pcm(std::vector<float>& pcm_data, int system_speed);
    bool detect_silence_gap(const std::vector<float>& audio_segment, float threshold = 0.01f);
    std::vector<float> pad_chunk_to_target_size(const std::vector<float>& chunk, size_t target_size);
    int get_system_speed_from_database();
    
    // Utility
    std::vector<float> resample_8k_to_16k(const std::vector<float>& input);
};

// Debug audio processor - with logging
class DebugAudioProcessor : public AudioProcessor {
public:
    DebugAudioProcessor(SipAudioInterface* sip_interface);
    
    bool start() override;
    void stop() override;
    bool is_running() const override { return running_.load(); }
    
    void start_session(const AudioSessionParams& params) override;
    void end_session(const std::string& call_id) override;
    void process_audio(const std::string& call_id, const RTPAudioPacket& packet) override;
    
    void set_whisper_endpoint(const std::string& endpoint) override { whisper_endpoint_ = endpoint; }
    std::string get_processor_name() const override { return "DebugAudioProcessor"; }
    
    // Debug configuration
    void set_log_audio_stats(bool enable) { log_audio_stats_ = enable; }
    void set_save_audio_files(bool enable) { save_audio_files_ = enable; }
    
private:
    SipAudioInterface* sip_interface_;
    std::atomic<bool> running_;
    std::string whisper_endpoint_;
    
    // Debug options
    bool log_audio_stats_;
    bool save_audio_files_;
    
    // Statistics (sessionless)
    std::unordered_map<std::string, size_t> call_packet_counts_;
    std::unordered_map<std::string, size_t> call_audio_bytes_;
    std::mutex stats_mutex_;

    void log_packet_info(const std::string& call_id, const RTPAudioPacket& packet);
    void log_call_stats(const std::string& call_id);
    void save_audio_chunk(const std::string& call_id, const std::vector<float>& audio);
};

// Fast lookup tables for G.711 decoding
class G711Tables {
public:
    static const std::vector<float>& get_ulaw_table();
    static const std::vector<float>& get_alaw_table();
    
public:
    static void initialize_tables();

private:
    static std::vector<float> ulaw_table_;
    static std::vector<float> alaw_table_;
    static bool tables_initialized_;
};
