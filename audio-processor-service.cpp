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
#include <cerrno>

// Robust I/O helpers for TCP (avoid partial send/recv issues)
static bool write_all_fd(int fd, const void* buf, size_t nbytes) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t off = 0;
    while (off < nbytes) {
        ssize_t m = send(fd, p + off, nbytes - off, 0);
        if (m <= 0) {
            return false;
        }
        off += static_cast<size_t>(m);
    }
    return true;
}

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
      total_packets_processed_(0), outgoing_listen_socket_(-1), outgoing_tcp_socket_(-1), incoming_tcp_socket_(-1),
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
        // Close active outgoing client socket
        if (outgoing_tcp_socket_ >= 0) {
            send_tcp_bye(outgoing_tcp_socket_);
            close(outgoing_tcp_socket_);
            outgoing_tcp_socket_ = -1;
        }
        // Close outgoing listen socket
        if (outgoing_listen_socket_ >= 0) {
            close(outgoing_listen_socket_);
            outgoing_listen_socket_ = -1;
        }
        // Close incoming listen socket
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
        return;
    }

    // Directly forward the RTP packet to the internal processor (avoid double conversion)

    {
        std::string cid;
        {
            std::lock_guard<std::mutex> lock(tcp_mutex_);
            cid = current_call_id_;
        }
        if (cid.empty()) cid = "global";
        audio_processor_->process_audio(cid, packet);
    }
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
        bool ok_out = setup_outgoing_tcp_socket(call_id);
        if (!ok_out) {
            std::cout << "❌ Failed to set up OUTGOING (Whisper) TCP server for call " << call_id << std::endl;
        }

        bool ok_in = setup_incoming_tcp_socket(call_id);
        if (!ok_in) {
            std::cout << "⚠️ Failed to set up INCOMING (Piper) TCP listener for call " << call_id << " — continuing without TTS return path" << std::endl;
        }

        // Advertise outgoing audio stream for external services even if incoming failed
        if (ok_out && service_advertiser_) {
            service_advertiser_->advertise_stream(call_id, outgoing_tcp_port_, "pcm_float");
        }

        if (ok_out) {
            active_.store(true);
            std::cout << "✅ Audio Processor ACTIVE - Outgoing stream ready and advertised for call " << call_id << std::endl;
        } else {
            std::cout << "❌ Audio Processor activation failed: no outgoing stream" << std::endl;
        }
    }
}

void AudioProcessorService::deactivate_after_call() {
    if (active_.load()) {
        std::cout << "😴 DEACTIVATING Audio Processor - Call ended" << std::endl;

        // Send BYE and close TCP connections
        // Prepare to remove advertisement after releasing tcp_mutex_
        std::string call_id_to_remove;

        {
            std::lock_guard<std::mutex> lock(tcp_mutex_);

            // Close outgoing TCP client connection
            if (outgoing_connected_.load() && outgoing_tcp_socket_ >= 0) {
                send_tcp_bye(outgoing_tcp_socket_);
                close(outgoing_tcp_socket_);
                outgoing_tcp_socket_ = -1;
                outgoing_connected_.store(false);
                std::cout << "🔌 Outgoing TCP connection closed (port " << outgoing_tcp_port_ << ")" << std::endl;
            }
            // Close outgoing listen socket
            if (outgoing_listen_socket_ >= 0) {
                close(outgoing_listen_socket_);
                outgoing_listen_socket_ = -1;
            }

            // Close incoming TCP listen socket
            if (incoming_tcp_socket_ >= 0) {
                close(incoming_tcp_socket_);
                incoming_tcp_socket_ = -1;
                incoming_connected_.store(false);
                std::cout << "🔌 Incoming TCP connection closed (port " << incoming_tcp_port_ << ")" << std::endl;
            }

            // Save ID for post-unlock removal
            call_id_to_remove = current_call_id_;

            // Reset ports
            outgoing_tcp_port_ = -1;
            incoming_tcp_port_ = -1;
            current_call_id_.clear();
        }

        // Remove stream advertisement (try both raw and sanitized ids) after sockets are down
        if (service_advertiser_ && !call_id_to_remove.empty()) {
            bool removed = service_advertiser_->remove_stream_advertisement(call_id_to_remove);
            if (!removed) {
                std::string sanitized = call_id_to_remove;
                for (char &ch : sanitized) if (ch == ':') ch = '_';
                removed = service_advertiser_->remove_stream_advertisement(sanitized);
            }
            if (removed) {
                std::cout << "📢 Stream advertisement removed for call " << call_id_to_remove << std::endl;
            } else {
                std::cout << "⚠️ No stream advertisement entry found to remove for call " << call_id_to_remove << std::endl;
            }
        }

        // Join TCP threads outside of tcp_mutex_
        if (outgoing_tcp_thread_.joinable()) {
            outgoing_tcp_thread_.join();
        }
        if (incoming_tcp_thread_.joinable()) {
            incoming_tcp_thread_.join();
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

        packet.audio_data = convert_float_to_g711_ulaw(chunk_data.samples);

        // Process through existing audio processor (no session management)
        if (audio_processor_) {
            {
                std::string cid;
                {
                    std::lock_guard<std::mutex> lock(tcp_mutex_);
                    cid = current_call_id_;
                }
                if (cid.empty()) cid = "global";
                audio_processor_->process_audio(cid, packet);
            }
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
    // Create a listening server socket for Whisper to connect to
    outgoing_listen_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (outgoing_listen_socket_ < 0) {
        std::cout << "❌ Failed to create outgoing TCP listen socket" << std::endl;
        return false;
    }

    // Set socket options
    int opt = 1;
    setsockopt(outgoing_listen_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    outgoing_tcp_port_ = calculate_outgoing_port(call_id);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(outgoing_tcp_port_);

    if (bind(outgoing_listen_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cout << "❌ Failed to bind outgoing TCP server to port " << outgoing_tcp_port_ << std::endl;
        close(outgoing_listen_socket_);
        outgoing_listen_socket_ = -1;
        return false;
    }

    if (listen(outgoing_listen_socket_, 5) < 0) {
        std::cout << "❌ Failed to listen on outgoing TCP server" << std::endl;
        close(outgoing_listen_socket_);
        outgoing_listen_socket_ = -1;
        return false;
    }

    // Start outgoing connection handler (server accept loop)
    outgoing_tcp_thread_ = std::thread(&AudioProcessorService::handle_outgoing_tcp_connection, this);

    std::cout << "✅ Outgoing TCP server listening on port " << outgoing_tcp_port_ << " for call " << call_id << std::endl;
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
    if (!write_all_fd(socket_fd, &length, 4)) {
        std::cout << "❌ Failed to send TCP HELLO length" << std::endl;
        return;
    }

    // Send call_id
    if (!write_all_fd(socket_fd, call_id.c_str(), call_id.length())) {
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
    if (!write_all_fd(socket_fd, &length, 4)) {
        std::cout << "❌ Failed to send TCP audio chunk length" << std::endl;
        // Mark disconnected and close
        outgoing_connected_.store(false);
        close(socket_fd);
        return;
    }

    // Send audio data
    if (!write_all_fd(socket_fd, audio_samples.data(), data_size)) {
        std::cout << "❌ Failed to send TCP audio chunk data" << std::endl;
        // Mark disconnected and close
        outgoing_connected_.store(false);
        close(socket_fd);
        return;
    }

    std::cout << "📤 TCP audio chunk sent: " << audio_samples.size() << " samples" << std::endl;
}

void AudioProcessorService::send_tcp_bye(int socket_fd) {
    if (socket_fd < 0) return;

    uint32_t bye_marker = 0xFFFFFFFF;

    if (!write_all_fd(socket_fd, &bye_marker, 4)) {
        std::cout << "❌ Failed to send TCP BYE" << std::endl;
        return;
    }

    std::cout << "📡 TCP BYE sent" << std::endl;
}

void AudioProcessorService::handle_outgoing_tcp_connection() {
    std::cout << "👂 Outgoing TCP server started on port " << outgoing_tcp_port_ << std::endl;

    while (running_.load()) {
        // Check if listen socket is still valid
        {
            std::lock_guard<std::mutex> lock(tcp_mutex_);
            if (outgoing_listen_socket_ < 0) {
                std::cout << "🔌 Outgoing TCP listen socket closed, exiting handler" << std::endl;
                break;
            }
        }

        struct sockaddr_in client_addr{};
        socklen_t client_len = sizeof(client_addr);
        int client_socket = accept(outgoing_listen_socket_, (struct sockaddr*)&client_addr, &client_len);

        if (client_socket < 0) {
            if (errno == EBADF || errno == EINVAL) {
                std::cout << "🔌 Outgoing TCP listen socket closed during accept, exiting handler" << std::endl;
                break;
            }
            if (errno == 53) { // macOS EPROTO during shutdown
                // benign during teardown; skip noisy error
                continue;
            }
            if (running_.load()) {
                std::cout << "❌ Failed to accept outgoing TCP client (errno: " << errno << ")" << std::endl;
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(tcp_mutex_);
            // Close any previous client
            if (outgoing_tcp_socket_ >= 0) close(outgoing_tcp_socket_);
            outgoing_tcp_socket_ = client_socket;
            outgoing_connected_.store(true);
        }

        std::cout << "🔗 Whisper client connected for call " << current_call_id_ << std::endl;

        // Send HELLO immediately with current call_id
        if (!current_call_id_.empty()) {
            send_tcp_hello(outgoing_tcp_socket_, current_call_id_);
        }

        // No read loop needed; we only transmit. Monitor socket liveness with a tiny watcher.
        std::thread([this]() {
            // Periodically test socket by sending zero-length (not protocol) or use recv peek
            // Here we just wait for running_ to go false; actual send failures are handled in send path.
            while (running_.load() && outgoing_connected_.load()) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            std::cout << "🔌 Whisper client disconnected" << std::endl;
        }).detach();
    }

    std::cout << "👂 Outgoing TCP server stopped" << std::endl;
}

void AudioProcessorService::handle_incoming_tcp_connection() {
    std::cout << "👂 Incoming TCP connection handler started on port " << incoming_tcp_port_ << std::endl;

    while (running_.load()) {
        // Check if socket is still valid before attempting accept
        {
            std::lock_guard<std::mutex> lock(tcp_mutex_);
            if (incoming_tcp_socket_ < 0) {
                std::cout << "🔌 Incoming TCP socket closed, exiting handler" << std::endl;
                break;
            }
        }

        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        // Accept incoming connection (blocking)
        int client_socket = accept(incoming_tcp_socket_, (struct sockaddr*)&client_addr, &client_len);

        if (client_socket < 0) {
            // Check if socket was closed (errno EBADF = bad file descriptor)
            if (errno == EBADF || errno == EINVAL) {
                std::cout << "🔌 Incoming TCP socket closed during accept, exiting handler" << std::endl;
                break;
            }
            if (errno == 53) { // macOS EPROTO during shutdown
                // benign during teardown
                continue;
            }

            if (running_.load()) {
                std::cout << "❌ Failed to accept incoming TCP connection (errno: " << errno << ")" << std::endl;
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

    if (!outgoing_connected_.load() || outgoing_tcp_socket_ < 0) {
        // Whisper has not connected yet; drop or buffer if needed
        std::cout << "⚠️ No Whisper client connected on port " << outgoing_tcp_port_ << ", dropping chunk of " << audio_samples.size() << " samples" << std::endl;
        return;
    }

    // Send audio chunk to connected Whisper client
    send_tcp_audio_chunk(outgoing_tcp_socket_, audio_samples);
}

// Factory Implementation
std::unique_ptr<AudioProcessorService> AudioProcessorServiceFactory::create() {
    return std::make_unique<AudioProcessorService>();
}
