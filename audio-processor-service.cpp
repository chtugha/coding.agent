#include "audio-processor-service.h"
#include "simple-audio-processor.h"
#include <iostream>
#include <sstream>
#include <iomanip>

// ServiceAudioInterface Implementation
AudioProcessorService::ServiceAudioInterface::ServiceAudioInterface(AudioProcessorService* service)
    : service_(service) {}

void AudioProcessorService::ServiceAudioInterface::send_to_whisper(const std::string& session_id, const std::vector<float>& audio_samples) {
    if (service_) {
        service_->handle_whisper_transcription(audio_samples);
    }
}

void AudioProcessorService::ServiceAudioInterface::on_audio_processing_error(const std::string& session_id, const std::string& error) {
    std::cout << "❌ Audio processing error: " << error << std::endl;
}

void AudioProcessorService::ServiceAudioInterface::on_audio_chunk_ready(const std::string& session_id, size_t chunk_size_samples) {
    std::cout << "✅ Audio chunk ready: " << chunk_size_samples << " samples" << std::endl;
}

// AudioProcessorService Implementation
AudioProcessorService::AudioProcessorService()
    : running_(false), active_(false), service_port_(8083), database_(nullptr),
      total_packets_processed_(0) {
    
    // Create audio interface
    audio_interface_ = std::make_unique<ServiceAudioInterface>(this);
    
    // Create simple audio processor
    audio_processor_ = std::make_unique<SimpleAudioProcessor>(audio_interface_.get());

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

    running_.store(true);
    active_.store(false); // Start in sleeping state

    std::cout << "😴 Audio Processor Service started (SLEEPING) on port " << port << std::endl;
    std::cout << "📡 Clean output connector ready" << std::endl;

    return true;
}

void AudioProcessorService::stop() {
    if (!running_.load()) return;
    
    running_.store(false);
    
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

        // Simple RTP decoding
        if (packet.payload_type == 0) { // G.711 μ-law
            audio_samples = convert_g711_ulaw_to_float(packet.audio_data);
        } else if (packet.payload_type == 8) { // G.711 A-law
            audio_samples = convert_g711_alaw_to_float(packet.audio_data);
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

void AudioProcessorService::activate_for_call() {
    if (!running_.load()) return;

    if (!active_.load()) {
        std::cout << "🚀 ACTIVATING Audio Processor - Call incoming!" << std::endl;

        // Clean output connector ready - no background services needed
        std::cout << "🔗 Clean output connector activated - ready for external peer" << std::endl;

        // No background connectors needed - clean output connector only

        active_.store(true);
        std::cout << "✅ Audio Processor ACTIVE - Processing threads started" << std::endl;
    }
}

void AudioProcessorService::deactivate_after_call() {
    if (active_.load()) {
        std::cout << "😴 DEACTIVATING Audio Processor - Call ended" << std::endl;

        // Clean output connector deactivated
        std::cout << "🔗 Clean output connector deactivated" << std::endl;

        // No background connectors to stop

        active_.store(false);
        std::cout << "💤 Audio Processor SLEEPING - Processing threads stopped" << std::endl;
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

std::vector<float> AudioProcessorService::convert_g711_ulaw_to_float(const std::vector<uint8_t>& data) {
    std::vector<float> result;
    result.reserve(data.size());

    // G.711 μ-law to linear PCM conversion
    static const int16_t ulaw_table[256] = {
        -32124,-31100,-30076,-29052,-28028,-27004,-25980,-24956,
        -23932,-22908,-21884,-20860,-19836,-18812,-17788,-16764,
        -15996,-15484,-14972,-14460,-13948,-13436,-12924,-12412,
        -11900,-11388,-10876,-10364, -9852, -9340, -8828, -8316,
         -7932, -7676, -7420, -7164, -6908, -6652, -6396, -6140,
         -5884, -5628, -5372, -5116, -4860, -4604, -4348, -4092,
         -3900, -3772, -3644, -3516, -3388, -3260, -3132, -3004,
         -2876, -2748, -2620, -2492, -2364, -2236, -2108, -1980,
         -1884, -1820, -1756, -1692, -1628, -1564, -1500, -1436,
         -1372, -1308, -1244, -1180, -1116, -1052,  -988,  -924,
          -876,  -844,  -812,  -780,  -748,  -716,  -684,  -652,
          -620,  -588,  -556,  -524,  -492,  -460,  -428,  -396,
          -372,  -356,  -340,  -324,  -308,  -292,  -276,  -260,
          -244,  -228,  -212,  -196,  -180,  -164,  -148,  -132,
          -120,  -112,  -104,   -96,   -88,   -80,   -72,   -64,
           -56,   -48,   -40,   -32,   -24,   -16,    -8,     0,
         32124, 31100, 30076, 29052, 28028, 27004, 25980, 24956,
         23932, 22908, 21884, 20860, 19836, 18812, 17788, 16764,
         15996, 15484, 14972, 14460, 13948, 13436, 12924, 12412,
         11900, 11388, 10876, 10364,  9852,  9340,  8828,  8316,
          7932,  7676,  7420,  7164,  6908,  6652,  6396,  6140,
          5884,  5628,  5372,  5116,  4860,  4604,  4348,  4092,
          3900,  3772,  3644,  3516,  3388,  3260,  3132,  3004,
          2876,  2748,  2620,  2492,  2364,  2236,  2108,  1980,
          1884,  1820,  1756,  1692,  1628,  1564,  1500,  1436,
          1372,  1308,  1244,  1180,  1116,  1052,   988,   924,
           876,   844,   812,   780,   748,   716,   684,   652,
           620,   588,   556,   524,   492,   460,   428,   396,
           372,   356,   340,   324,   308,   292,   276,   260,
           244,   228,   212,   196,   180,   164,   148,   132,
           120,   112,   104,    96,    88,    80,    72,    64,
            56,    48,    40,    32,    24,    16,     8,     0
    };

    for (uint8_t byte : data) {
        int16_t sample = ulaw_table[byte];
        result.push_back(static_cast<float>(sample) / 32768.0f);
    }

    return result;
}

std::vector<float> AudioProcessorService::convert_g711_alaw_to_float(const std::vector<uint8_t>& data) {
    std::vector<float> result;
    result.reserve(data.size());

    // G.711 A-law to linear PCM conversion (simplified)
    for (uint8_t byte : data) {
        // A-law decoding (simplified implementation)
        int16_t sample = 0;
        uint8_t sign = byte & 0x80;
        uint8_t exponent = (byte & 0x70) >> 4;
        uint8_t mantissa = byte & 0x0F;

        if (exponent == 0) {
            sample = mantissa << 4;
        } else {
            sample = ((mantissa | 0x10) << (exponent + 3));
        }

        if (sign) sample = -sample;

        result.push_back(static_cast<float>(sample) / 32768.0f);
    }

    return result;
}

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

// Clean Output Connector Implementation
bool AudioProcessorService::has_external_peer_connected() const {
    // Check if external AI service peer is connected
    // For now, always return false - no peer connected
    return false;
}

void AudioProcessorService::forward_to_external_service(const std::vector<float>& audio_samples) {
    // Forward audio to external AI service when peer is connected
    // This is where external AI service integration would happen
    std::cout << "🔗 Forwarding " << audio_samples.size() << " samples to external AI service" << std::endl;

    // TODO: Implement actual forwarding to external AI service
    // Example: HTTP POST to external service, WebSocket, TCP, etc.
}

// Factory Implementation
std::unique_ptr<AudioProcessorService> AudioProcessorServiceFactory::create(ProcessorType type) {
    // For now, always create simple processor
    // In future, could create different types based on parameter
    return std::make_unique<AudioProcessorService>();
}
