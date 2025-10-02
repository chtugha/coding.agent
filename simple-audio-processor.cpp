#include "simple-audio-processor.h"
#include <iostream>
#include <algorithm>
#include <cmath>

// G.711 μ-law decode table (fast lookup)
std::vector<float> G711Tables::ulaw_table_;
std::vector<float> G711Tables::alaw_table_;
bool G711Tables::tables_initialized_ = false;

void G711Tables::initialize_tables() {
    if (tables_initialized_) return;
    
    ulaw_table_.resize(256);
    alaw_table_.resize(256);
    
    // μ-law table
    const int16_t ulaw_decode[256] = {
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
    
    for (int i = 0; i < 256; ++i) {
        ulaw_table_[i] = ulaw_decode[i] / 32768.0f;
        // Proper ITU-T G.711 A-law decode
        uint8_t a = static_cast<uint8_t>(i) ^ 0x55; // complement per spec
        int sign = a & 0x80;
        int exp  = (a & 0x70) >> 4;
        int mant = a & 0x0F;
        int sample = (mant << 4) + 8; // add rounding bias
        if (exp != 0) {
            sample = (sample + 0x100) << (exp - 1);
        }
        if (sign) sample = -sample;
        alaw_table_[i] = static_cast<float>(sample) / 32768.0f;
    }
    
    tables_initialized_ = true;
}

const std::vector<float>& G711Tables::get_ulaw_table() {
    initialize_tables();
    return ulaw_table_;
}

const std::vector<float>& G711Tables::get_alaw_table() {
    initialize_tables();
    return alaw_table_;
}

// SimpleAudioProcessor Implementation
SimpleAudioProcessor::SimpleAudioProcessor(SipAudioInterface* sip_interface)
    : sip_interface_(sip_interface), running_(false),
      has_speech_(false), sample_rate_(16000),
      chunk_duration_ms_(3000), vad_threshold_(0.01f), silence_timeout_ms_(500), database_(nullptr) {

    G711Tables::initialize_tables(); // Initialize lookup tables
    last_speech_time_ = std::chrono::steady_clock::now();
    chunk_start_time_ = std::chrono::steady_clock::now();
}

SimpleAudioProcessor::~SimpleAudioProcessor() {
    stop();
}

bool SimpleAudioProcessor::start() {
    if (running_.load()) return true;
    
    running_.store(true);
    std::cout << "🎵 SimpleAudioProcessor started" << std::endl;
    return true;
}

void SimpleAudioProcessor::stop() {
    if (!running_.load()) return;

    running_.store(false);

    // Clear global audio buffer (sessionless)
    std::lock_guard<std::mutex> lock(audio_buffer_mutex_);
    global_audio_buffer_.clear();

    std::cout << "🛑 SimpleAudioProcessor stopped" << std::endl;
}

void SimpleAudioProcessor::start_session(const AudioSessionParams& params) {
    // Sessionless: reset buffers/state at call start to avoid cross-call residue
    {
        std::lock_guard<std::mutex> lock(audio_buffer_mutex_);
        global_audio_buffer_.clear();
        has_speech_ = false;
    }
    chunk_start_time_ = std::chrono::steady_clock::now();

    std::cout << "🎵 Audio processor ready (sessionless mode)"
              << " (line " << params.line_id << ", caller: " << params.caller_phone << ", call: " << params.call_id << ")" << std::endl;
}

void SimpleAudioProcessor::end_session(const std::string& call_id) {
    // Sessionless: Send any remaining audio before ending
    std::lock_guard<std::mutex> lock(audio_buffer_mutex_);
    if (!global_audio_buffer_.empty()) {
        send_audio_chunk_sessionless();
    }

    std::cout << "🔚 Audio processor cleanup (sessionless mode) for call: " << call_id << std::endl;
}

void SimpleAudioProcessor::process_audio(const std::string& call_id, const RTPAudioPacket& packet) {
    if (!running_.load()) return;

    // Fast audio decoding
    std::vector<float> audio_samples = decode_rtp_audio(packet);
    if (audio_samples.empty()) return;


    std::lock_guard<std::mutex> lock(audio_buffer_mutex_);

    // Append audio to global buffer (sessionless)
    global_audio_buffer_.insert(global_audio_buffer_.end(), audio_samples.begin(), audio_samples.end());

    // Fast VAD check
    if (has_speech(audio_samples)) {
        has_speech_ = true;
        last_speech_time_ = std::chrono::steady_clock::now();
    }

    // Use dynamic chunking based on system speed
    int system_speed = get_system_speed_from_database();

    // Create chunks using system speed
    auto chunks = create_chunks_from_pcm(global_audio_buffer_, system_speed);

    // Send each chunk
    for (const auto& chunk : chunks) {
        if (sip_interface_) {
            sip_interface_->send_to_whisper(call_id, chunk);
            sip_interface_->on_audio_chunk_ready(call_id, chunk.size());
        }
        std::cout << "📤 Sent dynamic chunk: " << chunk.size() << " samples (speed=" << system_speed << ") for call: " << call_id << std::endl;
    }

    // Processed samples are removed inside create_chunks_from_pcm; keep remainder in buffer
}

std::vector<float> SimpleAudioProcessor::decode_rtp_audio(const RTPAudioPacket& packet) {
    if (packet.audio_data.empty()) return {};

    // Handle DTMF events (RFC 4733) - payload type 101
    if (packet.payload_type == 101) {
        handle_dtmf_event(packet);
        return {}; // DTMF events don't contain audio samples
    }

    // Fast codec detection and decoding
    switch (packet.payload_type) {
        case 0:  // G.711 μ-law
            return convert_g711_ulaw(packet.audio_data);
        case 8:  // G.711 A-law
            return convert_g711_alaw(packet.audio_data);
        case 10: // PCM 16-bit
        case 11:
            return convert_pcm16(packet.audio_data);
        default:
            return {}; // Unsupported codec
    }
}

std::vector<float> SimpleAudioProcessor::convert_g711_ulaw(const std::vector<uint8_t>& data) {
    const auto& table = G711Tables::get_ulaw_table();
    std::vector<float> samples;
    samples.reserve(data.size() * 2); // Upsample 8kHz -> 16kHz

    if (data.empty()) return samples;

    for (size_t i = 0; i < data.size(); ++i) {
        float s = table[data[i]];
        float next = (i + 1 < data.size()) ? table[data[i + 1]] : s;
        samples.push_back(s);
        samples.push_back(0.5f * (s + next)); // Linear interpolation sample
    }

    return samples;
}

std::vector<float> SimpleAudioProcessor::convert_g711_alaw(const std::vector<uint8_t>& data) {
    const auto& table = G711Tables::get_alaw_table();
    std::vector<float> samples;
    samples.reserve(data.size() * 2); // Upsample 8kHz -> 16kHz

    if (data.empty()) return samples;

    for (size_t i = 0; i < data.size(); ++i) {
        float s = table[data[i]];
        float next = (i + 1 < data.size()) ? table[data[i + 1]] : s;
        samples.push_back(s);
        samples.push_back(0.5f * (s + next));
    }

    return samples;
}

std::vector<float> SimpleAudioProcessor::convert_pcm16(const std::vector<uint8_t>& data) {
    std::vector<float> samples;
    samples.reserve(data.size() / 2);
    
    for (size_t i = 0; i < data.size(); i += 2) {
        if (i + 1 < data.size()) {
            int16_t sample = (data[i + 1] << 8) | data[i];
            samples.push_back(sample / 32768.0f);
        }
    }
    
    return samples;
}

// Static versions for shared use
std::vector<float> SimpleAudioProcessor::convert_g711_ulaw_static(const std::vector<uint8_t>& data) {
    const auto& table = G711Tables::get_ulaw_table();
    std::vector<float> samples;
    samples.reserve(data.size() * 2);

    if (data.empty()) return samples;

    for (size_t i = 0; i < data.size(); ++i) {
        float s = table[data[i]];
        float next = (i + 1 < data.size()) ? table[data[i + 1]] : s;
        samples.push_back(s);
        samples.push_back(0.5f * (s + next));
    }

    return samples;
}

std::vector<float> SimpleAudioProcessor::convert_g711_alaw_static(const std::vector<uint8_t>& data) {
    const auto& table = G711Tables::get_alaw_table();
    std::vector<float> samples;
    samples.reserve(data.size() * 2);

    if (data.empty()) return samples;

    for (size_t i = 0; i < data.size(); ++i) {
        float s = table[data[i]];
        float next = (i + 1 < data.size()) ? table[data[i + 1]] : s;
        samples.push_back(s);
        samples.push_back(0.5f * (s + next));
    }

    return samples;
}

bool SimpleAudioProcessor::has_speech(const std::vector<float>& samples) {
    return calculate_energy(samples) > vad_threshold_;
}

float SimpleAudioProcessor::calculate_energy(const std::vector<float>& samples) {
    if (samples.empty()) return 0.0f;
    
    float sum = 0.0f;
    for (float sample : samples) {
        sum += sample * sample;
    }
    
    return std::sqrt(sum / samples.size());
}

bool SimpleAudioProcessor::should_send_chunk_sessionless() {
    if (!has_speech_ || global_audio_buffer_.empty()) return false;

    auto now = std::chrono::steady_clock::now();
    auto chunk_duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - chunk_start_time_).count();
    auto silence_duration = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_speech_time_).count();

    return (chunk_duration >= chunk_duration_ms_) || (silence_duration >= silence_timeout_ms_);
}

// Legacy method removed - using sessionless version only

void SimpleAudioProcessor::send_audio_chunk_sessionless() {
    if (global_audio_buffer_.empty()) return;

    // Prepare chunk for Whisper (16kHz, proper size)
    std::vector<float> whisper_chunk = prepare_whisper_chunk(global_audio_buffer_);

    // Send to SIP interface
    if (sip_interface_) {
        sip_interface_->send_to_whisper("global", whisper_chunk);
        sip_interface_->on_audio_chunk_ready("global", whisper_chunk.size());
    }

    // Reset global buffer
    global_audio_buffer_.clear();
    has_speech_ = false;
    chunk_start_time_ = std::chrono::steady_clock::now();

    std::cout << "📤 Sent audio chunk: " << whisper_chunk.size() << " samples (sessionless)" << std::endl;
}

std::vector<std::vector<float>> SimpleAudioProcessor::create_chunks_from_pcm(std::vector<float>& pcm_data, int system_speed) {
    std::vector<std::vector<float>> chunks;
    if (pcm_data.empty()) return chunks;

    // System speed determines chunking strategy:
    // 1 = slow (max audio per chunk)
    // 5 = fast (word-level-ish chunks)

    size_t window_size = 320; // 20ms at 16 kHz
    size_t min_chunk_size = window_size * (6 - system_speed); // Inverse relationship
    int window_ms = static_cast<int>(1000.0 * window_size / std::max(1, sample_rate_));
    int hangover_ms = 200; // keep ~200ms after last speech to avoid cutting words
    int hangover_windows = std::max(1, hangover_ms / std::max(1, window_ms));

    // Hysteresis thresholds to avoid rapid toggling
    float vad_start_threshold = std::max(0.001f, vad_threshold_ * 1.5f);
    float vad_stop_threshold  = std::max(0.0005f, vad_threshold_ * 0.5f);
    int speech_required = 2;   // require N consecutive speech windows to start
    int silence_required = 3;  // require N consecutive silence windows (post-hangover) to end

    // Target chunk size derived from configuration (defaults to ~3s)
    size_t target_size = static_cast<size_t>(std::max(1, sample_rate_) * (chunk_duration_ms_ / 1000.0f));
    if (target_size == 0) target_size = 16000 * 3;

    std::vector<float> current_chunk;
    bool in_speech = false;
    int silence_windows = 0;
    int consec_speech = 0;
    int consec_silence = 0;
    size_t consumed_until = 0; // index in pcm_data consumed by emitted chunks

    for (size_t i = 0; i < pcm_data.size(); i += window_size) {
        size_t end = std::min(i + window_size, pcm_data.size());
        std::vector<float> window(pcm_data.begin() + i, pcm_data.begin() + end);

        float win_rms = calculate_energy(window);
        bool speech_now = in_speech ? (win_rms > vad_stop_threshold) : (win_rms > vad_start_threshold);

        if (speech_now) {
            consec_speech++;
            consec_silence = 0;
        } else {
            consec_silence++;
            consec_speech = 0;
        }

        if (!in_speech && consec_speech >= speech_required) {
            in_speech = true;
            silence_windows = 0;
            // VAD: speech start (suppressed)
        }

        if (in_speech) {
            // Always accumulate while in speech (and during hangover)
            current_chunk.insert(current_chunk.end(), window.begin(), window.end());

            if (!speech_now) {
                // During hangover, keep a few windows to avoid cutting words
                silence_windows++;
                if (silence_windows >= hangover_windows) {
                    // After hangover, require additional consecutive silence windows
                    if (consec_silence >= silence_required && current_chunk.size() >= min_chunk_size) {
                        float chunk_rms = calculate_energy(current_chunk);
                        double secs = static_cast<double>(current_chunk.size()) / std::max(1, sample_rate_);
                        std::cout << "📦 Chunk created (end_of_speech): " << current_chunk.size()
                                  << " samples (~" << secs << " s), meanRMS=" << chunk_rms << std::endl;
                        chunks.push_back(pad_chunk_to_target_size(current_chunk, target_size));
                        current_chunk.clear();
                        in_speech = false;
                        silence_windows = 0;
                        consec_silence = 0;
                        consumed_until = end;
                        std::cout << "🔴 VAD: speech end (hangover=" << hangover_ms << " ms)" << std::endl;
                    }
                }
            }
        }

        // Force chunk creation if too large
        if (current_chunk.size() >= target_size) {
            float chunk_rms = calculate_energy(current_chunk);
            double secs = static_cast<double>(current_chunk.size()) / std::max(1, sample_rate_);
            std::cout << "📦 Chunk created (max_size): " << current_chunk.size()
                      << " samples (~" << secs << " s), meanRMS=" << chunk_rms << std::endl;
            chunks.push_back(pad_chunk_to_target_size(current_chunk, target_size));
            current_chunk.clear();
            in_speech = false;
            silence_windows = 0;
            consec_silence = 0;
            consec_speech = 0;
            consumed_until = end;
        }
    }

    // Do not force end_of_input emission here; keep remainder in pcm_data for next call
    if (consumed_until > 0 && consumed_until <= pcm_data.size()) {
        pcm_data.erase(pcm_data.begin(), pcm_data.begin() + consumed_until);
    }

    return chunks;
}

bool SimpleAudioProcessor::detect_silence_gap(const std::vector<float>& audio_segment, float threshold) {
    if (audio_segment.empty()) return true;

    float energy = 0.0f;
    for (float sample : audio_segment) {
        energy += sample * sample;
    }
    energy /= audio_segment.size();

    return energy < threshold;
}

std::vector<float> SimpleAudioProcessor::pad_chunk_to_target_size(const std::vector<float>& chunk, size_t target_size) {
    std::vector<float> padded_chunk = chunk;

    if (padded_chunk.size() < target_size) {
        // Pad with silence
        padded_chunk.resize(target_size, 0.0f);
    } else if (padded_chunk.size() > target_size) {
        // Truncate to target size
        padded_chunk.resize(target_size);
    }

    return padded_chunk;
}

int SimpleAudioProcessor::get_system_speed_from_database() {
    // Processors are standalone and DB-free; always use default system speed
    return DEFAULT_SYSTEM_SPEED;
}

std::vector<float> SimpleAudioProcessor::prepare_whisper_chunk(const std::vector<float>& audio) {
    std::vector<float> chunk = audio;
    
    // Ensure minimum size (1 second at 16kHz)
    size_t min_samples = 16000;
    if (chunk.size() < min_samples) {
        chunk.resize(min_samples, 0.0f); // Pad with silence
    }
    
    // Ensure maximum size (30 seconds at 16kHz)
    size_t max_samples = 16000 * 30;
    if (chunk.size() > max_samples) {
        chunk.resize(max_samples);
    }
    
    return chunk;
}

// Factory Implementation
std::unique_ptr<AudioProcessor> AudioProcessorFactory::create(ProcessorType type) {
    switch (type) {
        case ProcessorType::SIMPLE_PIPELINE:
        case ProcessorType::FAST_PIPELINE:
            return nullptr; // Need SipAudioInterface parameter
        case ProcessorType::DEBUG_PIPELINE:
            return nullptr; // Need SipAudioInterface parameter
        default:
            return nullptr;
    }
}

std::vector<std::string> AudioProcessorFactory::get_available_types() {
    return {"SimpleAudioProcessor", "DebugAudioProcessor"};
}

void SimpleAudioProcessor::handle_dtmf_event(const RTPAudioPacket& packet) {
    if (packet.audio_data.size() < 4) {
        std::cout << "⚠️ Invalid DTMF event packet (too small)" << std::endl;
        return;
    }

    // RFC 4733 DTMF Event format:
    // 0                   1                   2                   3
    // 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
    // |     event     |E|R| volume    |          duration             |
    // +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+

    uint8_t event = packet.audio_data[0];
    uint8_t flags_volume = packet.audio_data[1];
    uint16_t duration = (packet.audio_data[2] << 8) | packet.audio_data[3];

    bool end_of_event = (flags_volume & 0x80) != 0;
    // bool reserved = (flags_volume & 0x40) != 0;  // Reserved bit, not used
    uint8_t volume = flags_volume & 0x3F;

    // Convert event number to DTMF character
    char dtmf_char = '?';
    if (event <= 9) {
        dtmf_char = '0' + event;
    } else if (event == 10) {
        dtmf_char = '*';
    } else if (event == 11) {
        dtmf_char = '#';
    } else if (event == 12) {
        dtmf_char = 'A';
    } else if (event == 13) {
        dtmf_char = 'B';
    } else if (event == 14) {
        dtmf_char = 'C';
    } else if (event == 15) {
        dtmf_char = 'D';
    }

    std::cout << "📞 DTMF Event: '" << dtmf_char << "' (event=" << (int)event
              << ", volume=" << (int)volume << ", duration=" << duration
              << ", end=" << (end_of_event ? "yes" : "no") << ")" << std::endl;

    // TODO: Process DTMF digit for call control or menu navigation
    // This could be used for:
    // - Interactive voice response (IVR) menus
    // - Call transfer requests
    // - Feature activation (e.g., recording, mute)
}
