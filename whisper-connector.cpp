#include "whisper-connector.h"
#include <iostream>
#include <chrono>

// HTTP helper functions removed - using direct interface now

// WhisperConnector Implementation - Direct Interface
WhisperConnector::WhisperConnector() : running_(false), connected_(false) {}

WhisperConnector::~WhisperConnector() {
    stop();
}

bool WhisperConnector::start() {
    if (running_.load()) return true;

    running_.store(true);

    // Set connected to true (no external service dependency)
    connected_.store(true);

    // Start background worker
    worker_thread_ = std::thread(&WhisperConnector::worker_loop, this);

    std::cout << "🔗 WhisperConnector started (stub mode)" << std::endl;
    std::cout << "📊 Initial connection status: " << (connected_.load() ? "connected" : "disconnected") << std::endl;
    return true;
}

void WhisperConnector::stop() {
    if (!running_.load()) return;

    running_.store(false);
    queue_cv_.notify_all();

    if (worker_thread_.joinable()) worker_thread_.join();

    std::cout << "🔗 WhisperConnector stopped" << std::endl;
}

void WhisperConnector::send_chunk(const std::string& session_id, const std::vector<float>& audio_chunk) {
    if (!running_.load()) return;
    
    if (connected_.load()) {
        // Route to Whisper service
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            chunk_queue_.push({session_id, audio_chunk});
        }
        queue_cv_.notify_one();
    }
    // else: Route to null (drop chunk silently)
}

std::string WhisperConnector::transcribe_chunk(const std::string& session_id, const std::vector<float>& audio_chunk) {
    if (!connected_.load()) {
        std::cout << "⚠️  Whisper service not connected" << std::endl;
        return "";
    }

    std::cout << "📤 Transcribing " << audio_chunk.size() << " samples for session: " << session_id << std::endl;

    // Stub implementation - return empty transcription
    std::cout << "🔇 Stub mode: returning empty transcription" << std::endl;
    return "";
}

void WhisperConnector::worker_loop() {
    while (running_.load()) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return !chunk_queue_.empty() || !running_.load(); });
        
        if (!running_.load()) break;
        
        if (!chunk_queue_.empty()) {
            AudioChunk chunk = chunk_queue_.front();
            chunk_queue_.pop();
            lock.unlock();
            
            // Send to Whisper service via direct call
            transcribe_chunk(chunk.session_id, chunk.audio_data);
        }
    }
}

// Connection monitoring removed - using direct interface with immediate status checking

// Direct interface methods - no HTTP overhead

bool WhisperConnector::check_whisper_connection() {
    // Stub implementation - always return true
    return true;
}

void WhisperConnector::update_connection_status() {
    bool new_status = check_whisper_connection();
    bool old_status = connected_.load();

    if (new_status != old_status) {
        connected_.store(new_status);
        if (connection_callback_) {
            connection_callback_(new_status);
        }
        std::cout << "🔗 Whisper connection: " << (new_status ? "CONNECTED" : "DISCONNECTED") << std::endl;
    }
}

// PiperConnector Implementation
PiperConnector::PiperConnector() : running_(false), sip_connected_(false),
    outgoing_jitter_buffer_(std::make_unique<RTPPacketBuffer>(8, 2)), // 8 max, 2 min
    rtp_session_(std::make_unique<RTPSession>()) {} // RFC 3550 compliant RTP session

PiperConnector::~PiperConnector() {
    stop();
    if (outgoing_jitter_buffer_) {
        outgoing_jitter_buffer_->stop();
    }
}

bool PiperConnector::start() {
    if (running_.load()) return true;
    
    running_.store(true);
    
    // Start background worker
    worker_thread_ = std::thread(&PiperConnector::worker_loop, this);
    
    std::cout << "🔊 PiperConnector started" << std::endl;
    return true;
}

void PiperConnector::stop() {
    if (!running_.load()) return;
    
    running_.store(false);
    queue_cv_.notify_all();
    
    if (worker_thread_.joinable()) worker_thread_.join();
    
    std::cout << "🔊 PiperConnector stopped" << std::endl;
}

void PiperConnector::send_piper_audio(const std::string& session_id, const std::vector<uint8_t>& audio_data) {
    if (!running_.load() || !rtp_session_) return;

    if (sip_connected_.load()) {
        // Create RFC 3550 compliant RTP packet from Piper audio
        RTPPacket rtp_packet = create_rtp_packet_from_audio(audio_data);

        // Serialize to wire format (NO session_id or internal data)
        std::vector<uint8_t> wire_packet = rtp_packet.serialize();

        // Add to jitter buffer for smooth transmission
        if (outgoing_jitter_buffer_) {
            outgoing_jitter_buffer_->push(wire_packet);
        }

        // Process jitter buffer
        process_jitter_buffer();

        std::cout << "🎵 Created RFC 3550 RTP packet from Piper audio: seq="
                  << rtp_packet.get_sequence_number() << ", ts=" << rtp_packet.get_timestamp()
                  << ", payload=" << rtp_packet.get_payload().size() << " bytes" << std::endl;
    }
    // else: Route to null (drop Piper stream silently)
}

void PiperConnector::set_sip_client_callback(std::function<void(const std::string&, const std::vector<uint8_t>&)> callback) {
    sip_callback_ = callback;
    sip_connected_.store(callback != nullptr);
}

void PiperConnector::worker_loop() {
    while (running_.load()) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return !audio_queue_.empty() || !running_.load(); });
        
        if (!running_.load()) break;
        
        if (!audio_queue_.empty()) {
            PiperAudio audio = audio_queue_.front();
            audio_queue_.pop();
            lock.unlock();
            
            // Convert to G.711 RTP and send to SIP client
            std::vector<uint8_t> g711_rtp = convert_to_g711_rtp(audio.audio_data);
            if (sip_callback_) {
                sip_callback_(audio.session_id, g711_rtp);
            }
        }
    }
}

void PiperConnector::process_jitter_buffer() {
    if (!outgoing_jitter_buffer_) return;

    // Try to get buffered audio for smooth transmission
    std::vector<uint8_t> buffered_audio;
    if (outgoing_jitter_buffer_->try_pop(buffered_audio)) {
        // Send buffered audio to SIP client
        if (sip_callback_) {
            sip_callback_("", buffered_audio); // Empty session_id for now
            std::cout << "📤 Sent jitter-buffered audio: " << buffered_audio.size() << " bytes" << std::endl;
        }
    }
}

RTPPacket PiperConnector::create_rtp_packet_from_audio(const std::vector<uint8_t>& audio_data) {
    // Convert Piper audio to G.711 μ-law payload
    // Assuming Piper outputs 16-bit PCM, convert to float first
    std::vector<float> float_samples;
    float_samples.reserve(audio_data.size() / 2);

    for (size_t i = 0; i < audio_data.size(); i += 2) {
        if (i + 1 < audio_data.size()) {
            int16_t sample = static_cast<int16_t>(audio_data[i] | (audio_data[i + 1] << 8));
            float_samples.push_back(static_cast<float>(sample) / 32768.0f);
        }
    }

    // Convert to G.711 μ-law
    std::vector<uint8_t> g711_payload = RTPCodec::float_to_g711_ulaw(float_samples);

    // Create RFC 3550 compliant RTP packet
    // NO session_id or internal data is included
    return rtp_session_->create_packet(
        RTPPayloadType::PCMU,           // G.711 μ-law
        g711_payload,                   // Audio payload only
        RTPTiming::G711_TIMESTAMP_INCREMENT, // Standard 160 samples increment
        false                           // No marker bit
    );
}

std::vector<uint8_t> PiperConnector::convert_to_g711_rtp(const std::vector<uint8_t>& piper_audio) {
    // Legacy method - now creates full RTP packet
    RTPPacket packet = create_rtp_packet_from_audio(piper_audio);
    return packet.serialize();
}
