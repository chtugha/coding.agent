#pragma once

#include "base-audio-processor.h"
#include "shmem_audio_channel.h"
#include <thread>
#include <functional>
#include <unordered_map>
#include <memory>

// Specialized processor for outbound audio: Piper → Phone
// Receives float32 PCM audio from Piper service via TCP,
// applies anti-aliasing and downsamples from 22050Hz to 8kHz,
// converts to G.711, and forwards to SIP client via shared memory for RTP transmission
class OutboundAudioProcessor : public BaseAudioProcessor {
public:
    OutboundAudioProcessor();
    ~OutboundAudioProcessor() override;

    // Service lifecycle
    bool start(int base_port) override;
    void stop() override;

    // SIP client callback interface (deprecated, kept for compatibility)
    void set_sip_client_callback(std::function<void(const std::string&, const std::vector<uint8_t>&)> callback);

    // Shared memory output to SIP client
    void set_shared_memory_out(std::shared_ptr<ShmAudioChannel> channel);

    // Optional μ-law WAV2 silence data for testing (auto-cycled by scheduler)
    void set_silence_wav2_bytes(const std::vector<uint8_t>& bytes);


    // Load arbitrary WAV file and convert to μ-law mono 8kHz for test silence
    bool load_and_set_silence_wav2(const std::string& wav_path);

    // Ensure output scheduler is running (continuous stream)
    void ensure_output_running();

    // Call management
    void activate_for_call(const std::string& call_id) override;
    void deactivate_after_call() override;

    // Status
    ProcessorStatus get_status() const override;

private:
    // Unified input type for extensible formats
    enum class AudioFileType { WAV, MP3, MP4, M4A, FLAC, OGG, UNKNOWN };


    // Output scheduler for continuous 20ms frames
    std::thread output_thread_;
    std::atomic<bool> output_running_{false};
    std::vector<uint8_t> out_buffer_; // queued G.711 bytes from Piper
    std::mutex out_buffer_mutex_;

    // Test WAV2 (μ-law 8kHz) for silence source
    std::vector<uint8_t> silence_wav2_;
    size_t silence_wav2_pos_ = 0;


private:
    // SIP client callback (deprecated)
    std::function<void(const std::string&, const std::vector<uint8_t>&)> sip_client_callback_;

    // Shared memory channel for outbound G.711 bytes
    std::shared_ptr<ShmAudioChannel> out_channel_;

    // TCP connection from Piper service
    int piper_tcp_socket_;
    int piper_tcp_port_;
    std::atomic<bool> piper_connected_;
    std::thread piper_tcp_thread_;
    std::mutex piper_mutex_;

    // Registration polling
    std::atomic<bool> registration_running_;
    std::thread registration_thread_;

    // Deduplication state for incoming TTS chunks (per call)
    std::unordered_map<std::string, uint32_t> last_chunk_id_;
    std::mutex chunk_dedup_mutex_;

    // Internal methods
    bool setup_piper_tcp_socket(const std::string& call_id);
    void handle_piper_tcp_connection();
    void enqueue_g711_(const std::vector<uint8_t>& g711);
    void start_output_scheduler_();
    void stop_output_scheduler_();
    // Unified audio pipeline helpers
    std::vector<uint8_t> process_float_mono_to_ulaw(const std::vector<float>& mono, uint32_t sample_rate);
    static AudioFileType detect_audio_file_type(const std::vector<uint8_t>& bytes);
    static bool read_entire_file(const std::string& path, std::vector<uint8_t>& out);
    static bool parse_wav_header(const std::vector<uint8_t>& bytes,
                                 uint16_t& fmt, uint16_t& channels, uint32_t& rate,
                                 uint16_t& bits_per_sample, size_t& data_offset, size_t& data_size);
    std::vector<float> decode_bytes_to_float_mono(const std::vector<uint8_t>& bytes, AudioFileType type,
                                                  uint32_t& sample_rate);

    void make_silence_frame_(std::vector<uint8_t>& frame);

    bool read_exact_from_socket(int socket_fd, void* data, size_t size);
    void process_piper_audio_chunk(const std::vector<uint8_t>& payload, uint32_t sample_rate, uint32_t chunk_id);

    // Port calculation
    int calculate_piper_port(const std::string& call_id);

    // Registration polling
    void start_registration_polling(const std::string& call_id);
    void stop_registration_polling();
    void registration_polling_thread(const std::string& call_id);
};
