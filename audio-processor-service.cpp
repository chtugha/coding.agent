#include "audio-processor-service.h"
#include "simple-audio-processor.h"
#include "service-advertisement.h"
#include <iostream>
#include <sstream>
#include <iomanip>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <thread>
#include <string>
#include <stdexcept>

// ServiceAudioInterface Implementation
AudioProcessorService::ServiceAudioInterface::ServiceAudioInterface(AudioProcessorService* service)
    : service_(service) {}

void AudioProcessorService::ServiceAudioInterface::send_to_whisper(const std::string& call_id, const std::vector<float>& audio_samples) {
    if (service_) {
        std::cout << "🎤 Sending " << audio_samples.size() << " audio samples to Whisper for call: " << call_id << std::endl;
        service_->handle_whisper_transcription(audio_samples);
    }
}

void AudioProcessorService::ServiceAudioInterface::on_audio_processing_error(const std::string& call_id, const std::string& error) {
    std::cout << "❌ Audio processing error for call " << call_id << ": " << error << std::endl;
}

void AudioProcessorService::ServiceAudioInterface::on_audio_chunk_ready(const std::string& call_id, size_t chunk_size_samples) {
    std::cout << "✅ Audio chunk ready for call " << call_id << ": " << chunk_size_samples << " samples" << std::endl;
}

// AudioProcessorService Implementation
AudioProcessorService::AudioProcessorService()
    : running_(false), active_(false), service_port_(8083), database_(nullptr),
      total_packets_processed_(0), outgoing_tcp_socket_(-1), incoming_tcp_socket_(-1),
      outgoing_tcp_port_(-1), incoming_tcp_port_(-1),
      outgoing_connected_(false), incoming_connected_(false) {

    // Create audio interface
    audio_interface_ = std::make_unique<ServiceAudioInterface>(this);

    // Create simple audio processor
    audio_processor_ = std::make_unique<SimpleAudioProcessor>(audio_interface_.get());

    // Create service advertiser
    service_advertiser_ = std::make_unique<ServiceAdvertiser>();

    // Connect database to processor for system speed configuration
    if (database_) {
        auto simple_processor = dynamic_cast<SimpleAudioProcessor*>(audio_processor_.get());
        if (simple_processor) {
            simple_processor->set_database(database_);
        }
    }
}

AudioProcessorService::~AudioProcessorService() {
    stop();
}

bool AudioProcessorService::start(int port) {
    if (running_.load()) return true;

    service_port_ = port;

    // Start audio processor (but keep it sleeping)
    if (!audio_processor_->start()) {
        std::cout << "❌ Failed to start audio processor" << std::endl;
        return false;
    }

    // Start service advertiser
    if (!service_advertiser_->start(13000)) {
        std::cout << "❌ Failed to start service advertiser" << std::endl;
        return false;
    }

    running_.store(true);
    active_.store(false); // Start in sleeping state

    std::cout << "😴 Audio Processor Service started (SLEEPING) on port " << port << std::endl;
    std::cout << "📡 TCP sockets will be created dynamically based on call_id" << std::endl;
    std::cout << "📢 Service advertiser running on port 13000" << std::endl;

    return true;
}

void AudioProcessorService::stop() {
    if (!running_.load()) return;

    running_.store(false);

    // Stop service advertiser
    if (service_advertiser_) {
        service_advertiser_->stop();
    }

    // Close TCP sockets
    {
        std::lock_guard<std::mutex> lock(tcp_mutex_);
        if (outgoing_tcp_socket_ >= 0) {
            send_tcp_bye(outgoing_tcp_socket_);
            close(outgoing_tcp_socket_);
            outgoing_tcp_socket_ = -1;
        }
        if (incoming_tcp_socket_ >= 0) {
            close(incoming_tcp_socket_);
            incoming_tcp_socket_ = -1;
        }
        outgoing_connected_.store(false);
        incoming_connected_.store(false);
    }

    // Join TCP threads
    if (outgoing_tcp_thread_.joinable()) {
        outgoing_tcp_thread_.join();
    }
    if (incoming_tcp_thread_.joinable()) {
        incoming_tcp_thread_.join();
    }

    if (audio_processor_) {
        audio_processor_->stop();
    }

    std::cout << "🛑 Audio Processor Service stopped" << std::endl;
}

// Session management methods removed

void AudioProcessorService::process_audio(const RTPAudioPacket& packet) {
    if (!running_.load() || !active_.load() || !audio_processor_) {
        // Processor sleeping - drop audio packets silently
        return;
    }

    // Process audio directly without session management
    std::thread([this, packet]() {
        // Decode RTP packet to audio samples
        std::vector<float> audio_samples;

        // Use shared G.711 conversion from SimpleAudioProcessor
        if (packet.payload_type == 0) { // G.711 μ-law
            audio_samples = SimpleAudioProcessor::convert_g711_ulaw_static(packet.audio_data);
        } else if (packet.payload_type == 8) { // G.711 A-law
            audio_samples = SimpleAudioProcessor::convert_g711_alaw_static(packet.audio_data);
        } else {
            return; // Unsupported codec
        }

        if (!audio_samples.empty()) {
            // Add to single jitter buffer
            AudioChunkData chunk_data("", audio_samples); // No session ID needed

            std::lock_guard<std::mutex> lock(buffers_mutex_);
            if (incoming_audio_buffer_) {
                incoming_audio_buffer_->push(chunk_data);
            }
        }
    }).detach();

    // Process buffered audio
    process_buffered_audio();
    total_packets_processed_.fetch_add(1);
}

void AudioProcessorService::handle_whisper_transcription(const std::vector<float>& audio_samples) {
    std::cout << "📤 Clean output connector: " << audio_samples.size() << " samples ready" << std::endl;

    // Clean output connector - drops packets until a peer connects
    if (has_external_peer_connected()) {
        // Forward to external AI service when peer is connected
        forward_to_external_service(audio_samples);
        std::cout << "✅ Audio forwarded to external AI service" << std::endl;
    } else {
        std::cout << "⚠️  No external peer connected, dropping audio chunk" << std::endl;
    }
}

std::string AudioProcessorService::simulate_whisper_transcription(const std::vector<float>& audio_samples) {
    // Simple simulation based on audio characteristics
    float energy = 0.0f;
    for (float sample : audio_samples) {
        energy += sample * sample;
    }
    energy = std::sqrt(energy / audio_samples.size());
    
    std::ostringstream oss;
    oss << "Audio chunk processed (";
    oss << std::fixed << std::setprecision(1) << (audio_samples.size() / 16000.0f) << "s, ";
    oss << std::fixed << std::setprecision(3) << energy << " energy)";
    
    return oss.str();
}

// update_database_transcription method removed - no session management

AudioProcessorService::ServiceStatus AudioProcessorService::get_status() const {
    ServiceStatus status;
    status.is_running = running_.load();
    // No session management - active_sessions field removed
    status.total_packets_processed = total_packets_processed_.load();
    status.whisper_endpoint = "clean-output-connector";
    
    if (audio_processor_) {
        status.processor_type = audio_processor_->get_processor_name();

        // Add sleep/active state info
        if (running_.load()) {
            if (active_.load()) {
                status.processor_type += " (ACTIVE)";
            } else {
                status.processor_type += " (SLEEPING)";
            }
        }
    } else {
        status.processor_type = "None";
    }

    return status;
}

void AudioProcessorService::handle_outgoing_audio(const std::vector<uint8_t>& audio_data) {
    // Use single jitter buffer for smooth outgoing audio
    {
        std::lock_guard<std::mutex> lock(buffers_mutex_);
        if (!outgoing_audio_buffer_) {
            // Create outgoing jitter buffer
            outgoing_audio_buffer_ = std::make_unique<RTPPacketBuffer>(6, 2); // 6 max, 2 min
        }

        // Add to outgoing buffer
        outgoing_audio_buffer_->push(audio_data);
    }

    // Process buffered outgoing audio
    process_outgoing_buffer();
}

bool AudioProcessorService::check_sip_client_connection() {
    // Simple connection check - if callback is set, assume connected
    return sip_client_callback_ != nullptr;
}

void AudioProcessorService::set_sip_client_callback(std::function<void(const std::vector<uint8_t>&)> callback) {
    sip_client_callback_ = callback;
}

void AudioProcessorService::set_database(Database* database) {
    database_ = database;

    // Also set database for the audio processor
    if (audio_processor_) {
        auto simple_processor = dynamic_cast<SimpleAudioProcessor*>(audio_processor_.get());
        if (simple_processor) {
            simple_processor->set_database(database);
        }
    }
}

void AudioProcessorService::activate_for_call(const std::string& call_id) {
    if (!running_.load()) return;

    if (!active_.load()) {
        std::cout << "🚀 ACTIVATING Audio Processor - Call incoming!" << std::endl;

        // Store call_id for TCP connections
        {
            std::lock_guard<std::mutex> lock(tcp_mutex_);
            current_call_id_ = call_id;
        }

        // Setup TCP sockets with call_id-based ports
        if (!setup_outgoing_tcp_socket(call_id)) {
            std::cout << "❌ Failed to setup outgoing TCP socket for call " << call_id << std::endl;
            return;
        }

        if (!setup_incoming_tcp_socket(call_id)) {
            std::cout << "❌ Failed to setup incoming TCP socket for call " << call_id << std::endl;
            return;
        }

        // Advertise outgoing audio stream for external services
        if (service_advertiser_) {
            service_advertiser_->advertise_stream(call_id, outgoing_tcp_port_, "pcm_float");
        }

        active_.store(true);
        std::cout << "✅ Audio Processor ACTIVE - TCP sockets ready and advertised for call " << call_id << std::endl;
    }
}

void AudioProcessorService::deactivate_after_call() {
    if (active_.load()) {
        std::cout << "😴 DEACTIVATING Audio Processor - Call ended" << std::endl;

        // Send BYE and close TCP connections
        {
            std::lock_guard<std::mutex> lock(tcp_mutex_);

            // Close outgoing TCP connection
            if (outgoing_connected_.load() && outgoing_tcp_socket_ >= 0) {
                send_tcp_bye(outgoing_tcp_socket_);
                close(outgoing_tcp_socket_);
                outgoing_tcp_socket_ = -1;
                outgoing_connected_.store(false);
                std::cout << "🔌 Outgoing TCP connection closed (port " << outgoing_tcp_port_ << ")" << std::endl;
            }

            // Close incoming TCP connection and stop thread
            if (incoming_tcp_socket_ >= 0) {
                close(incoming_tcp_socket_);
                incoming_tcp_socket_ = -1;
                incoming_connected_.store(false);
                std::cout << "🔌 Incoming TCP connection closed (port " << incoming_tcp_port_ << ")" << std::endl;
            }

            // Join incoming thread if it exists
            if (incoming_tcp_thread_.joinable()) {
                incoming_tcp_thread_.join();
            }

            // Remove stream advertisement
            if (service_advertiser_ && !current_call_id_.empty()) {
                service_advertiser_->remove_stream_advertisement(current_call_id_);
            }

            // Reset ports
            outgoing_tcp_port_ = -1;
            incoming_tcp_port_ = -1;
            current_call_id_.clear();
        }

        active_.store(false);
        std::cout << "💤 Audio Processor SLEEPING - TCP sockets closed, advertisement removed" << std::endl;
    }
}

void AudioProcessorService::process_buffered_audio() {
    std::lock_guard<std::mutex> lock(buffers_mutex_);
    if (!incoming_audio_buffer_) return;

    // Try to get buffered audio chunk
    AudioChunkData chunk_data;
    if (incoming_audio_buffer_->try_pop(chunk_data)) {
        // Convert to RTPAudioPacket format for existing processor
        RTPAudioPacket packet;
        packet.payload_type = 0; // G.711 μ-law
        packet.sequence_number = 0;
        packet.timestamp = 0;

        // Convert float samples back to G.711 for compatibility
        packet.audio_data = convert_float_to_g711_ulaw(chunk_data.samples);

        // Process through existing audio processor (no session management)
        if (audio_processor_) {
            audio_processor_->process_audio("default", packet);  // Use default session ID
        }
    }
}

void AudioProcessorService::process_outgoing_buffer() {
    std::lock_guard<std::mutex> lock(buffers_mutex_);
    if (!outgoing_audio_buffer_) return;

    // Try to get buffered outgoing audio
    std::vector<uint8_t> audio_data;
    if (outgoing_audio_buffer_->try_pop(audio_data)) {
        // Simple background routing: SIP client vs null
        bool sip_client_connected = check_sip_client_connection();

        if (sip_client_connected) {
            // Route to SIP client for RTP transmission
            if (sip_client_callback_) {
                sip_client_callback_(audio_data);
            }
        }
        // else: Route to null (drop Piper stream silently)
    }
}

// cleanup_session_buffers method removed - no per-session buffers

// Removed: convert_g711_ulaw_to_float - using shared function from SimpleAudioProcessor

// Removed: convert_g711_alaw_to_float - using shared function from SimpleAudioProcessor

std::vector<uint8_t> AudioProcessorService::convert_float_to_g711_ulaw(const std::vector<float>& samples) {
    std::vector<uint8_t> result;
    result.reserve(samples.size());

    // Linear PCM to G.711 μ-law conversion (simplified)
    for (float sample : samples) {
        // Clamp to [-1.0, 1.0]
        sample = std::max(-1.0f, std::min(1.0f, sample));
        int16_t pcm_sample = static_cast<int16_t>(sample * 32767.0f);

        // Simple μ-law encoding (this is a simplified version)
        uint8_t ulaw_byte = 0;
        if (pcm_sample < 0) {
            ulaw_byte = 0x7F;
            pcm_sample = -pcm_sample;
        }

        // Find the appropriate μ-law value (simplified)
        if (pcm_sample >= 8159) ulaw_byte |= 0x70;
        else if (pcm_sample >= 4063) ulaw_byte |= 0x60;
        else if (pcm_sample >= 2015) ulaw_byte |= 0x50;
        else if (pcm_sample >= 991) ulaw_byte |= 0x40;
        else if (pcm_sample >= 479) ulaw_byte |= 0x30;
        else if (pcm_sample >= 223) ulaw_byte |= 0x20;
        else if (pcm_sample >= 95) ulaw_byte |= 0x10;

        result.push_back(ulaw_byte);
    }

    return result;
}

// TCP Socket Implementation
bool AudioProcessorService::setup_outgoing_tcp_socket(const std::string& call_id) {
    outgoing_tcp_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (outgoing_tcp_socket_ < 0) {
        std::cout << "❌ Failed to create outgoing TCP socket" << std::endl;
        return false;
    }

    // Set socket options
    int opt = 1;
    setsockopt(outgoing_tcp_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    outgoing_tcp_port_ = calculate_outgoing_port(call_id);
    std::cout << "✅ Outgoing TCP socket created for call " << call_id << " (will connect on demand to port " << outgoing_tcp_port_ << ")" << std::endl;
    return true;
}

bool AudioProcessorService::setup_incoming_tcp_socket(const std::string& call_id) {
    incoming_tcp_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (incoming_tcp_socket_ < 0) {
        std::cout << "❌ Failed to create incoming TCP socket" << std::endl;
        return false;
    }

    // Set socket options
    int opt = 1;
    setsockopt(incoming_tcp_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    incoming_tcp_port_ = calculate_incoming_port(call_id);

    // Bind to listen for incoming connections
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(incoming_tcp_port_);

    if (bind(incoming_tcp_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cout << "❌ Failed to bind incoming TCP socket to port " << incoming_tcp_port_ << std::endl;
        close(incoming_tcp_socket_);
        incoming_tcp_socket_ = -1;
        return false;
    }

    if (listen(incoming_tcp_socket_, 1) < 0) {
        std::cout << "❌ Failed to listen on incoming TCP socket" << std::endl;
        close(incoming_tcp_socket_);
        incoming_tcp_socket_ = -1;
        return false;
    }

    // Start incoming connection handler thread
    incoming_tcp_thread_ = std::thread(&AudioProcessorService::handle_incoming_tcp_connection, this);

    std::cout << "✅ Incoming TCP socket listening on port " << incoming_tcp_port_ << " for call " << call_id << std::endl;
    return true;
}

void AudioProcessorService::send_tcp_hello(int socket_fd, const std::string& call_id) {
    if (socket_fd < 0) return;

    uint32_t length = htonl(call_id.length());

    // Send length prefix
    if (send(socket_fd, &length, 4, 0) != 4) {
        std::cout << "❌ Failed to send TCP HELLO length" << std::endl;
        return;
    }

    // Send call_id
    if (send(socket_fd, call_id.c_str(), call_id.length(), 0) != (ssize_t)call_id.length()) {
        std::cout << "❌ Failed to send TCP HELLO call_id" << std::endl;
        return;
    }

    std::cout << "📡 TCP HELLO sent: " << call_id << std::endl;
}

void AudioProcessorService::send_tcp_audio_chunk(int socket_fd, const std::vector<float>& audio_samples) {
    if (socket_fd < 0 || audio_samples.empty()) return;

    // Convert to bytes for transmission
    size_t data_size = audio_samples.size() * sizeof(float);
    uint32_t length = htonl(data_size);

    // Send length prefix
    if (send(socket_fd, &length, 4, 0) != 4) {
        std::cout << "❌ Failed to send TCP audio chunk length" << std::endl;
        return;
    }

    // Send audio data
    if (send(socket_fd, audio_samples.data(), data_size, 0) != (ssize_t)data_size) {
        std::cout << "❌ Failed to send TCP audio chunk data" << std::endl;
        return;
    }

    std::cout << "📤 TCP audio chunk sent: " << audio_samples.size() << " samples" << std::endl;
}

void AudioProcessorService::send_tcp_bye(int socket_fd) {
    if (socket_fd < 0) return;

    uint32_t bye_marker = 0xFFFFFFFF;

    if (send(socket_fd, &bye_marker, 4, 0) != 4) {
        std::cout << "❌ Failed to send TCP BYE" << std::endl;
        return;
    }

    std::cout << "📡 TCP BYE sent" << std::endl;
}

void AudioProcessorService::handle_outgoing_tcp_connection() {
    // This method handles connecting to external services when needed
    // Called when we have audio to send and no connection exists
    std::cout << "🔌 Outgoing TCP connection handler ready" << std::endl;
}

void AudioProcessorService::handle_incoming_tcp_connection() {
    std::cout << "👂 Incoming TCP connection handler started on port " << incoming_tcp_port_ << std::endl;

    while (running_.load()) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // Accept incoming connection (blocking)
        int client_socket = accept(incoming_tcp_socket_, (struct sockaddr*)&client_addr, &client_len);

        if (client_socket < 0) {
            if (running_.load()) {
                std::cout << "❌ Failed to accept incoming TCP connection" << std::endl;
            }
            continue;
        }

        std::cout << "🔗 Incoming TCP connection accepted" << std::endl;
        incoming_connected_.store(true);

        // Handle this connection in a separate thread
        std::thread([this, client_socket]() {
            // Read HELLO message first
            uint32_t length;
            if (recv(client_socket, &length, 4, 0) == 4) {
                length = ntohl(length);
                if (length > 0 && length < 1000) {
                    std::vector<char> call_id_buffer(length + 1);
                    if (recv(client_socket, call_id_buffer.data(), length, 0) == (ssize_t)length) {
                        call_id_buffer[length] = '\0';
                        current_call_id_ = std::string(call_id_buffer.data());
                        std::cout << "📡 TCP HELLO received for call: " << current_call_id_ << std::endl;
                    }
                }
            }

            // Process incoming audio data
            while (running_.load() && incoming_connected_.load()) {
                uint32_t chunk_length;
                ssize_t received = recv(client_socket, &chunk_length, 4, 0);

                if (received != 4) break;

                chunk_length = ntohl(chunk_length);

                // Check for BYE message
                if (chunk_length == 0xFFFFFFFF) {
                    std::cout << "📡 TCP BYE received" << std::endl;
                    break;
                }

                if (chunk_length > 0 && chunk_length < 1000000) { // Reasonable limit
                    std::vector<uint8_t> audio_data(chunk_length);
                    if (recv(client_socket, audio_data.data(), chunk_length, 0) == (ssize_t)chunk_length) {
                        // Convert to G.711 and send to SIP client
                        if (sip_client_callback_) {
                            sip_client_callback_(audio_data);
                            std::cout << "📤 TCP audio forwarded to SIP client: " << chunk_length << " bytes" << std::endl;
                        }
                    }
                }
            }

            close(client_socket);
            incoming_connected_.store(false);
            std::cout << "🔌 Incoming TCP connection closed" << std::endl;
        }).detach();
    }

    std::cout << "👂 Incoming TCP connection handler stopped" << std::endl;
}

// Port calculation from call_id (direct numeric conversion)
int AudioProcessorService::calculate_outgoing_port(const std::string& call_id) {
    if (call_id.empty()) return 9001; // Fallback

    // Convert call_id to integer and add to base port
    int call_id_num = 0;
    try {
        call_id_num = std::stoi(call_id);
    } catch (const std::exception&) {
        // If call_id is not numeric, use fallback
        call_id_num = 0;
    }

    int port = 9001 + call_id_num;
    std::cout << "🔢 Outgoing port for call " << call_id << ": " << port << " (9001 + " << call_id_num << ")" << std::endl;
    return port;
}

int AudioProcessorService::calculate_incoming_port(const std::string& call_id) {
    if (call_id.empty()) return 9002; // Fallback

    // Convert call_id to integer and add to base port
    int call_id_num = 0;
    try {
        call_id_num = std::stoi(call_id);
    } catch (const std::exception&) {
        // If call_id is not numeric, use fallback
        call_id_num = 0;
    }

    int port = 9002 + call_id_num;
    std::cout << "🔢 Incoming port for call " << call_id << ": " << port << " (9002 + " << call_id_num << ")" << std::endl;
    return port;
}

// TCP Output Connector Implementation
bool AudioProcessorService::has_external_peer_connected() const {
    return outgoing_connected_.load();
}

void AudioProcessorService::forward_to_external_service(const std::vector<float>& audio_samples) {
    std::lock_guard<std::mutex> lock(tcp_mutex_);

    // Try to connect if not connected
    if (!outgoing_connected_.load() && outgoing_tcp_socket_ >= 0) {
        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_addr.s_addr = inet_addr("127.0.0.1"); // localhost for now
        server_addr.sin_port = htons(outgoing_tcp_port_);

        if (connect(outgoing_tcp_socket_, (struct sockaddr*)&server_addr, sizeof(server_addr)) == 0) {
            outgoing_connected_.store(true);
            std::cout << "🔗 Connected to external service on port " << outgoing_tcp_port_ << std::endl;

            // Send HELLO with current call_id
            if (!current_call_id_.empty()) {
                send_tcp_hello(outgoing_tcp_socket_, current_call_id_);
            }
        } else {
            std::cout << "⚠️ Failed to connect to external service on port " << outgoing_tcp_port_ << std::endl;
            return;
        }
    }

    // Send audio chunk if connected
    if (outgoing_connected_.load()) {
        send_tcp_audio_chunk(outgoing_tcp_socket_, audio_samples);
    }
}

// Factory Implementation
std::unique_ptr<AudioProcessorService> AudioProcessorServiceFactory::create() {
    return std::make_unique<AudioProcessorService>();
}
