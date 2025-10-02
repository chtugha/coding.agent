#include "inbound-audio-processor.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// InboundAudioInterface Implementation
InboundAudioProcessor::InboundAudioInterface::InboundAudioInterface(InboundAudioProcessor* processor)
    : processor_(processor) {}

void InboundAudioProcessor::InboundAudioInterface::send_to_whisper(const std::string& call_id, const std::vector<float>& audio_samples) {
    if (processor_) {
        std::cout << "🎤 Sending " << audio_samples.size() << " audio samples to Whisper for call: " << call_id << std::endl;
        processor_->forward_to_whisper(audio_samples);
    }
}

void InboundAudioProcessor::InboundAudioInterface::on_audio_processing_error(const std::string& call_id, const std::string& error) {
    std::cout << "❌ Inbound audio processing error for call " << call_id << ": " << error << std::endl;
}

void InboundAudioProcessor::InboundAudioInterface::on_audio_chunk_ready(const std::string& call_id, size_t chunk_size_samples) {
    std::cout << "✅ Inbound audio chunk ready for call " << call_id << ": " << chunk_size_samples << " samples" << std::endl;
}

// InboundAudioProcessor Implementation
InboundAudioProcessor::InboundAudioProcessor()
    : whisper_listen_socket_(-1)
    , whisper_tcp_socket_(-1)
    , whisper_tcp_port_(-1)
    , whisper_connected_(false)
    , sip_client_listen_socket_(-1)
    , sip_server_running_(false)
{
    // Create audio interface
    audio_interface_ = std::make_unique<InboundAudioInterface>(this);
    
    // Create simple audio processor
    audio_processor_ = std::make_unique<SimpleAudioProcessor>(audio_interface_.get());
}

InboundAudioProcessor::~InboundAudioProcessor() {
    stop();
}

bool InboundAudioProcessor::start(int base_port) {
    if (running_.load()) return true;
    
    base_port_ = base_port;
    
    // Start audio processor
    if (!audio_processor_->start()) {
        std::cout << "❌ Failed to start inbound audio processor" << std::endl;
        return false;
    }
    
    // Start service advertiser
    service_advertiser_ = std::make_unique<ServiceAdvertiser>();
    if (!service_advertiser_->start(13000 + base_port)) {
        std::cout << "❌ Failed to start inbound service advertiser" << std::endl;
        return false;
    }
    
    // Connect database to processor for system speed configuration
    if (database_) {
        auto simple_processor = dynamic_cast<SimpleAudioProcessor*>(audio_processor_.get());
        if (simple_processor) {
            simple_processor->set_database(database_);
        }
    }
    
    running_.store(true);
    active_.store(false); // Start in sleeping state

    // Start SIP client server
    // start_sip_client_server(base_port);

    std::cout << "😴 Inbound Audio Processor started (SLEEPING) on base port " << base_port << std::endl;
    std::cout << "📡 TCP sockets will be created dynamically based on call_id" << std::endl;
    std::cout << "📢 Service advertiser running on port " << (13000 + base_port) << std::endl;
    std::cout << "🔌 SIP client server running on port " << base_port << std::endl;
    
    return true;
}

void InboundAudioProcessor::stop() {
    BaseAudioProcessor::stop();

    // Stop SIP client server
    sip_server_running_.store(false);
    if (sip_client_listen_socket_ >= 0) {
        close(sip_client_listen_socket_);
        sip_client_listen_socket_ = -1;
    }
    if (sip_server_thread_.joinable()) {
        sip_server_thread_.join();
    }

    // Close TCP sockets
    {
        std::lock_guard<std::mutex> lock(whisper_mutex_);
        if (whisper_tcp_socket_ >= 0) {
            send_tcp_bye(whisper_tcp_socket_);
            close(whisper_tcp_socket_);
            whisper_tcp_socket_ = -1;
        }
        if (whisper_listen_socket_ >= 0) {
            close(whisper_listen_socket_);
            whisper_listen_socket_ = -1;
        }
        whisper_connected_.store(false);
    }
    
    // Join TCP thread
    if (whisper_tcp_thread_.joinable()) {
        whisper_tcp_thread_.join();
    }
    
    if (audio_processor_) {
        audio_processor_->stop();
    }
    
    std::cout << "🛑 Inbound Audio Processor stopped" << std::endl;
}

void InboundAudioProcessor::process_rtp_audio(const RTPAudioPacket& packet) {
    if (!running_.load() || !active_.load() || !audio_processor_) {
        return;
    }
    
    // Optimize: Avoid mutex in audio-critical path
    std::string cid = current_call_id_; // Atomic read
    if (cid.empty()) cid = "global";
    
    audio_processor_->process_audio(cid, packet);
    total_packets_processed_.fetch_add(1);
}

void InboundAudioProcessor::activate_for_call(const std::string& call_id) {
    BaseAudioProcessor::activate_for_call(call_id);
    
    if (!active_.load()) return;
    
    // Setup TCP socket for Whisper connection
    bool ok = setup_whisper_tcp_socket(call_id);
    if (!ok) {
        std::cout << "❌ Failed to set up Whisper TCP server for call " << call_id << std::endl;
        return;
    }
    
    // Advertise audio stream for Whisper service
    if (service_advertiser_) {
        service_advertiser_->advertise_stream(call_id, whisper_tcp_port_, "pcm_float");
    }
    
    std::cout << "✅ Inbound Audio Processor ACTIVE - Whisper stream ready for call " << call_id << std::endl;
}

void InboundAudioProcessor::deactivate_after_call() {
    BaseAudioProcessor::deactivate_after_call();
    
    // Close Whisper TCP connection
    {
        std::lock_guard<std::mutex> lock(whisper_mutex_);
        if (whisper_tcp_socket_ >= 0) {
            send_tcp_bye(whisper_tcp_socket_);
            close(whisper_tcp_socket_);
            whisper_tcp_socket_ = -1;
        }
        if (whisper_listen_socket_ >= 0) {
            close(whisper_listen_socket_);
            whisper_listen_socket_ = -1;
        }
        whisper_connected_.store(false);
    }
    
    // Join TCP thread
    if (whisper_tcp_thread_.joinable()) {
        whisper_tcp_thread_.join();
    }
    
    // Remove service advertisement
    if (service_advertiser_) {
        std::string call_id;
        {
            std::lock_guard<std::mutex> lock(call_mutex_);
            call_id = current_call_id_;
        }
        if (!call_id.empty()) {
            service_advertiser_->remove_stream_advertisement(call_id);
        }
    }
}

BaseAudioProcessor::ProcessorStatus InboundAudioProcessor::get_status() const {
    auto status = BaseAudioProcessor::get_status();
    status.processor_type = "Inbound";
    return status;
}

// Private methods
bool InboundAudioProcessor::setup_whisper_tcp_socket(const std::string& call_id) {
    whisper_listen_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (whisper_listen_socket_ < 0) {
        std::cout << "❌ Failed to create Whisper TCP listen socket" << std::endl;
        return false;
    }
    
    int opt = 1;
    setsockopt(whisper_listen_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    
    whisper_tcp_port_ = calculate_whisper_port(call_id);
    
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(whisper_tcp_port_);
    
    if (bind(whisper_listen_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cout << "❌ Failed to bind Whisper TCP server to port " << whisper_tcp_port_ << std::endl;
        close(whisper_listen_socket_);
        whisper_listen_socket_ = -1;
        return false;
    }
    
    if (listen(whisper_listen_socket_, 1) < 0) {
        std::cout << "❌ Failed to listen on Whisper TCP socket" << std::endl;
        close(whisper_listen_socket_);
        whisper_listen_socket_ = -1;
        return false;
    }
    
    // Start connection handler
    whisper_tcp_thread_ = std::thread(&InboundAudioProcessor::handle_whisper_tcp_connection, this);
    
    std::cout << "✅ Whisper TCP server listening on port " << whisper_tcp_port_ << " for call " << call_id << std::endl;
    return true;
}

int InboundAudioProcessor::calculate_whisper_port(const std::string& call_id) {
    int port = 9001 + calculate_port_offset(call_id);
    std::cout << "🔢 Whisper port for call " << call_id << ": " << port << " (9001 + " << calculate_port_offset(call_id) << ")" << std::endl;
    return port;
}

void InboundAudioProcessor::handle_whisper_tcp_connection() {
    std::cout << "👂 Whisper TCP connection handler started" << std::endl;
    
    while (running_.load() && whisper_listen_socket_ >= 0) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        
        int client_socket = accept(whisper_listen_socket_, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (running_.load()) {
                std::cout << "❌ Failed to accept Whisper client connection" << std::endl;
            }
            break;
        }
        
        {
            std::lock_guard<std::mutex> lock(whisper_mutex_);
            if (whisper_tcp_socket_ >= 0) close(whisper_tcp_socket_);
            whisper_tcp_socket_ = client_socket;
            whisper_connected_.store(true);
        }
        
        std::string call_id;
        {
            std::lock_guard<std::mutex> lock(call_mutex_);
            call_id = current_call_id_;
        }
        
        std::cout << "🔗 Whisper client connected for call " << call_id << std::endl;
        
        // Send HELLO immediately
        if (!call_id.empty()) {
            send_tcp_hello(whisper_tcp_socket_, call_id);
        }
        
        // Keep connection alive - no read loop needed for outgoing-only connection
        while (running_.load() && whisper_connected_.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    
    std::cout << "👂 Whisper TCP connection handler stopped" << std::endl;
}

void InboundAudioProcessor::forward_to_whisper(const std::vector<float>& audio_samples) {
    if (!has_whisper_connected()) {
        std::cout << "⚠️ No Whisper client connected, dropping chunk of " << audio_samples.size() << " samples" << std::endl;
        return;
    }
    
    std::lock_guard<std::mutex> lock(whisper_mutex_);
    if (whisper_tcp_socket_ >= 0) {
        send_tcp_audio_chunk(whisper_tcp_socket_, audio_samples);
    }
}

void InboundAudioProcessor::send_tcp_audio_chunk(int socket_fd, const std::vector<float>& audio_samples) {
    if (socket_fd < 0 || audio_samples.empty()) return;
    
    size_t data_size = audio_samples.size() * sizeof(float);
    uint32_t length = htonl(data_size);
    
    // Send length prefix
    if (!write_all_fd(socket_fd, &length, 4)) {
        std::cout << "❌ Failed to send TCP audio chunk length to Whisper" << std::endl;
        whisper_connected_.store(false);
        close(socket_fd);
        return;
    }
    
    // Send audio data
    if (!write_all_fd(socket_fd, audio_samples.data(), data_size)) {
        std::cout << "❌ Failed to send TCP audio chunk data to Whisper" << std::endl;
        whisper_connected_.store(false);
        close(socket_fd);
        return;
    }
    
    std::cout << "📤 Sent " << audio_samples.size() << " samples to Whisper" << std::endl;
}

bool InboundAudioProcessor::has_whisper_connected() const {
    return whisper_connected_.load();
}
