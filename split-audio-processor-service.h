#pragma once

#include "base-audio-processor.h"
#include "inbound-audio-processor.h"
#include "outbound-audio-processor.h"
#include "audio-processor-interface.h"
#include "database.h"
#include <memory>
#include <functional>

// Combined Audio Processor Service using split-processor design
// Manages both inbound (Phone→Whisper) and outbound (Piper→Phone) audio processing
// with improved performance and separation of concerns
class SplitAudioProcessorService {
public:
    SplitAudioProcessorService();
    ~SplitAudioProcessorService();

    // Service lifecycle
    bool start(int base_port = 8083);
    void stop();
    bool is_running() const;

    // Call management
    void activate_for_call(const std::string& call_id);
    void deactivate_after_call();
    bool is_active() const;

    // Configuration
    void set_database(Database* database);

    // Audio processing interface (compatible with existing SIP client)
    void process_audio(const RTPAudioPacket& packet);
    void set_sip_client_callback(std::function<void(const std::string&, const std::vector<uint8_t>&)> callback);

    // Status
    struct ServiceStatus {
        bool is_running;
        bool is_active;
        std::string processor_type;
        size_t total_packets_processed;
        std::string current_call_id;
        struct {
            bool is_running;
            bool is_active;
            size_t packets_processed;
        } inbound;
        struct {
            bool is_running;
            bool is_active;
            size_t packets_processed;
        } outbound;
    };
    ServiceStatus get_status() const;

private:
    // Split processors
    std::unique_ptr<InboundAudioProcessor> inbound_processor_;
    std::unique_ptr<OutboundAudioProcessor> outbound_processor_;

    // Service state
    int base_port_;
    Database* database_;

    // Current call tracking
    std::string current_call_id_;
    std::mutex call_mutex_;
};

// Factory for creating split-processor services (compatible with existing code)
class SplitAudioProcessorServiceFactory {
public:
    static std::unique_ptr<SplitAudioProcessorService> create();
};

// Compatibility typedef for existing code
using AudioProcessorService = SplitAudioProcessorService;
using AudioProcessorServiceFactory = SplitAudioProcessorServiceFactory;
