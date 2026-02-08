#pragma once

#include "base-audio-processor.h"
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

    // UDP output to SIP client
    void set_udp_output(const std::string& ip, int port, uint32_t call_id);
    void clear_udp_output();

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
    int ctrl_socket_ = -1;
    std::thread ctrl_thread_;
    void control_socket_loop();

    struct CallState {
        std::string call_id;
        
        // UDP destination for outbound G.711 bytes
        std::string udp_dest_ip;
        int udp_dest_port = -1;
        uint32_t udp_call_id = 0;
        int udp_socket = -1;
        std::mutex udp_mutex;

        // TCP connection from Piper/Kokoro service
        int piper_tcp_socket = -1;
        int piper_tcp_port = -1;
        std::atomic<bool> piper_connected{false};
        std::thread piper_tcp_thread;
        std::mutex piper_mutex;

        // Output buffer and scheduler state
        std::vector<uint8_t> out_buffer;
        std::mutex out_buffer_mutex;
        bool pending_first_rtp = false;
        
        // Activity timing
        std::chrono::steady_clock::time_point last_activity;

        // Registration polling
        std::atomic<bool> registration_running{false};
        std::thread registration_thread;

        CallState(const std::string& id) : call_id(id) {
            last_activity = std::chrono::steady_clock::now();
        }
    };

    std::unordered_map<std::string, std::shared_ptr<CallState>> active_calls_;
    mutable std::mutex calls_mutex_;

    // Unified input type for extensible formats
    enum class AudioFileType { WAV, MP3, MP4, M4A, FLAC, OGG, UNKNOWN };

    // Common output scheduler for ALL calls
    std::thread output_thread_;
    std::atomic<bool> output_running_{false};

    // Test WAV2 (μ-law 8kHz) for silence source
    std::vector<uint8_t> silence_wav2_;
    size_t silence_wav2_pos_ = 0;

    // Internal methods
    std::shared_ptr<CallState> get_or_create_call_state(const std::string& call_id);
    void cleanup_call(std::shared_ptr<CallState> state);

    bool setup_piper_tcp_socket(std::shared_ptr<CallState> state);
    void handle_piper_tcp_connection(std::shared_ptr<CallState> state);
    void enqueue_g711_(std::shared_ptr<CallState> state, const std::vector<uint8_t>& g711);
    void start_output_scheduler_();
    void stop_output_scheduler_();
    
    // Maintenance loop for stale calls
    void maintenance_loop();
    std::thread maintenance_thread_;

    // Unified audio pipeline helpers
    std::vector<uint8_t> process_float_mono_to_ulaw(const std::vector<float>& mono, uint32_t sample_rate);
    static AudioFileType detect_audio_file_type(const std::vector<uint8_t>& bytes);
    static bool read_entire_file(const std::string& path, std::vector<uint8_t>& out);
    static bool parse_wav_header(const std::vector<uint8_t>& bytes,
                                 uint16_t& fmt, uint16_t& channels, uint32_t& rate,
                                 uint16_t& bits_per_sample, size_t& data_offset, size_t& data_size);
    std::vector<float> decode_bytes_to_float_mono(const std::vector<uint8_t>& bytes, AudioFileType type,
                                                  uint32_t& sample_rate);

    void make_silence_frame_(std::shared_ptr<CallState> state, std::vector<uint8_t>& frame);

    bool read_exact_from_socket(int socket_fd, void* data, size_t size);
    void process_piper_audio_chunk(std::shared_ptr<CallState> state, const std::vector<uint8_t>& payload, uint32_t sample_rate, uint32_t chunk_id);

    // Port calculation
    int calculate_piper_port(const std::string& call_id);

    // Registration polling
    void start_registration_polling(std::shared_ptr<CallState> state);
    void stop_registration_polling(std::shared_ptr<CallState> state);
    void registration_polling_thread(std::shared_ptr<CallState> state);
};
