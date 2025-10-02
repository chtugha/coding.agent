#pragma once

#include "database.h"
#include "service-advertisement.h"
#include <memory>
#include <string>
#include <atomic>
#include <thread>
#include <mutex>
#include <functional>
#include <unordered_map>

// Base class for audio processors with shared functionality
class BaseAudioProcessor {
public:
    BaseAudioProcessor();
    virtual ~BaseAudioProcessor();

    // Service lifecycle
    virtual bool start(int base_port) = 0;
    virtual void stop();
    bool is_running() const { return running_.load(); }

    // Call management
    virtual void activate_for_call(const std::string& call_id);
    virtual void deactivate_after_call();
    bool is_active() const { return active_.load(); }

    // Configuration
    void set_database(Database* database);

    // Status
    struct ProcessorStatus {
        bool is_running;
        bool is_active;
        std::string processor_type;
        size_t total_packets_processed;
        std::string current_call_id;
    };
    virtual ProcessorStatus get_status() const;

protected:
    // Shared state
    std::atomic<bool> running_;
    std::atomic<bool> active_;
    int base_port_;
    Database* database_;
    std::atomic<size_t> total_packets_processed_;

    // Call management
    std::string current_call_id_;
    std::mutex call_mutex_;

    // Service advertisement
    std::unique_ptr<ServiceAdvertiser> service_advertiser_;

    // TCP utility functions
    bool write_all_fd(int fd, const void* data, size_t size);
    void send_tcp_hello(int socket_fd, const std::string& call_id);
    void send_tcp_bye(int socket_fd);

    // Port calculation utilities
    int calculate_port_offset(const std::string& call_id);

    // Audio conversion utilities
    std::vector<uint8_t> convert_float_to_g711_ulaw(const std::vector<float>& samples);
    std::vector<float> convert_g711_ulaw_to_float(const std::vector<uint8_t>& g711_data);
    std::vector<float> resample_linear(const std::vector<float>& in, int src_rate, int dst_rate);
    std::vector<float> lowpass_telephony(const std::vector<float>& in, int = 0);

private:
    // G.711 lookup tables (shared across all processors)
    static std::vector<int16_t> ulaw_to_linear_table_;
    static std::vector<uint8_t> linear_to_ulaw_table_;
    static void init_g711_tables();
    static bool g711_tables_initialized_;
    static std::mutex g711_init_mutex_;
};

// Factory for creating specialized processors
class BaseAudioProcessorFactory {
public:
    enum class ProcessorType {
        INBOUND,   // Handles Phone → Whisper
        OUTBOUND   // Handles Piper → Phone
    };

    static std::unique_ptr<BaseAudioProcessor> create(ProcessorType type);
};
