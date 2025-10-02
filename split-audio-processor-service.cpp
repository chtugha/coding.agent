#include "split-audio-processor-service.h"
#include <iostream>

SplitAudioProcessorService::SplitAudioProcessorService()
    : base_port_(0)
    , database_(nullptr)
{
    // Create split processors
    inbound_processor_ = std::make_unique<InboundAudioProcessor>();
    outbound_processor_ = std::make_unique<OutboundAudioProcessor>();
}

SplitAudioProcessorService::~SplitAudioProcessorService() {
    stop();
}

bool SplitAudioProcessorService::start(int base_port) {
    if (is_running()) return true;
    
    base_port_ = base_port;
    
    // Start inbound processor (Phone → Whisper)
    if (!inbound_processor_->start(base_port)) {
        std::cout << "❌ Failed to start inbound audio processor" << std::endl;
        return false;
    }
    
    // Start outbound processor (Piper → Phone)
    if (!outbound_processor_->start(base_port + 100)) {
        std::cout << "❌ Failed to start outbound audio processor" << std::endl;
        inbound_processor_->stop();
        return false;
    }
    
    // Set database for both processors
    if (database_) {
        inbound_processor_->set_database(database_);
        outbound_processor_->set_database(database_);
    }
    
    std::cout << "✅ Split Audio Processor Service started successfully" << std::endl;
    std::cout << "📥 Inbound processor (Phone→Whisper) on base port " << base_port << std::endl;
    std::cout << "📤 Outbound processor (Piper→Phone) on base port " << (base_port + 100) << std::endl;
    
    return true;
}

void SplitAudioProcessorService::stop() {
    if (!is_running()) return;
    
    // Stop both processors
    if (inbound_processor_) {
        inbound_processor_->stop();
    }
    if (outbound_processor_) {
        outbound_processor_->stop();
    }
    
    std::cout << "🛑 Split Audio Processor Service stopped" << std::endl;
}

bool SplitAudioProcessorService::is_running() const {
    return (inbound_processor_ && inbound_processor_->is_running()) ||
           (outbound_processor_ && outbound_processor_->is_running());
}

void SplitAudioProcessorService::activate_for_call(const std::string& call_id) {
    {
        std::lock_guard<std::mutex> lock(call_mutex_);
        current_call_id_ = call_id;
    }
    
    // Activate both processors for the call
    if (inbound_processor_) {
        inbound_processor_->activate_for_call(call_id);
    }
    if (outbound_processor_) {
        outbound_processor_->activate_for_call(call_id);
    }
    
    std::cout << "🚀 Split Audio Processor Service activated for call: " << call_id << std::endl;
}

void SplitAudioProcessorService::deactivate_after_call() {
    // Deactivate both processors
    if (inbound_processor_) {
        inbound_processor_->deactivate_after_call();
    }
    if (outbound_processor_) {
        outbound_processor_->deactivate_after_call();
    }
    
    {
        std::lock_guard<std::mutex> lock(call_mutex_);
        current_call_id_.clear();
    }
    
    std::cout << "😴 Split Audio Processor Service deactivated" << std::endl;
}

bool SplitAudioProcessorService::is_active() const {
    return (inbound_processor_ && inbound_processor_->is_active()) ||
           (outbound_processor_ && outbound_processor_->is_active());
}

void SplitAudioProcessorService::set_database(Database* database) {
    database_ = database;
    
    // Set database for both processors
    if (inbound_processor_) {
        inbound_processor_->set_database(database);
    }
    if (outbound_processor_) {
        outbound_processor_->set_database(database);
    }
}

void SplitAudioProcessorService::process_audio(const RTPAudioPacket& packet) {
    // Route RTP audio to inbound processor (Phone → Whisper)
    if (inbound_processor_) {
        inbound_processor_->process_rtp_audio(packet);
    }
}

void SplitAudioProcessorService::set_sip_client_callback(std::function<void(const std::string&, const std::vector<uint8_t>&)> callback) {
    // Set callback for outbound processor (Piper → Phone)
    if (outbound_processor_) {
        outbound_processor_->set_sip_client_callback(callback);
    }
}

SplitAudioProcessorService::ServiceStatus SplitAudioProcessorService::get_status() const {
    ServiceStatus status;
    
    // Overall status
    status.is_running = is_running();
    status.is_active = is_active();
    status.processor_type = "Split";
    status.total_packets_processed = 0;
    
    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(call_mutex_));
        status.current_call_id = current_call_id_;
    }
    
    // Inbound processor status
    if (inbound_processor_) {
        auto inbound_status = inbound_processor_->get_status();
        status.inbound.is_running = inbound_status.is_running;
        status.inbound.is_active = inbound_status.is_active;
        status.inbound.packets_processed = inbound_status.total_packets_processed;
        status.total_packets_processed += inbound_status.total_packets_processed;
    }
    
    // Outbound processor status
    if (outbound_processor_) {
        auto outbound_status = outbound_processor_->get_status();
        status.outbound.is_running = outbound_status.is_running;
        status.outbound.is_active = outbound_status.is_active;
        status.outbound.packets_processed = outbound_status.total_packets_processed;
        status.total_packets_processed += outbound_status.total_packets_processed;
    }
    
    return status;
}

// Factory implementation
std::unique_ptr<SplitAudioProcessorService> SplitAudioProcessorServiceFactory::create() {
    return std::make_unique<SplitAudioProcessorService>();
}
