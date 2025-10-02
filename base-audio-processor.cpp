#include "base-audio-processor.h"

#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cmath>

// Static member initialization
std::vector<int16_t> BaseAudioProcessor::ulaw_to_linear_table_;
std::vector<uint8_t> BaseAudioProcessor::linear_to_ulaw_table_;
bool BaseAudioProcessor::g711_tables_initialized_ = false;
std::mutex BaseAudioProcessor::g711_init_mutex_;

BaseAudioProcessor::BaseAudioProcessor()
    : running_(false)
    , active_(false)
    , base_port_(0)
    , database_(nullptr)
    , total_packets_processed_(0)
{
    // Initialize G.711 lookup tables if not already done
    std::lock_guard<std::mutex> lock(g711_init_mutex_);
    if (!g711_tables_initialized_) {
        init_g711_tables();
        g711_tables_initialized_ = true;
    }
}

BaseAudioProcessor::~BaseAudioProcessor() {
    stop();
}

void BaseAudioProcessor::stop() {
    if (!running_.load()) return;
    
    running_.store(false);
    active_.store(false);
    
    // Stop service advertiser
    if (service_advertiser_) {
        service_advertiser_->stop();
    }
    
    std::cout << "🛑 Base Audio Processor stopped" << std::endl;
}

void BaseAudioProcessor::activate_for_call(const std::string& call_id) {
    if (!running_.load()) return;
    
    {
        std::lock_guard<std::mutex> lock(call_mutex_);
        current_call_id_ = call_id;
    }
    
    active_.store(true);
    std::cout << "🚀 Audio Processor activated for call: " << call_id << std::endl;
}

void BaseAudioProcessor::deactivate_after_call() {
    if (!active_.load()) return;
    
    active_.store(false);
    
    {
        std::lock_guard<std::mutex> lock(call_mutex_);
        current_call_id_.clear();
    }
    
    std::cout << "😴 Audio Processor deactivated" << std::endl;
}

void BaseAudioProcessor::set_database(Database* database) {
    database_ = database;
}

BaseAudioProcessor::ProcessorStatus BaseAudioProcessor::get_status() const {
    ProcessorStatus status;
    status.is_running = running_.load();
    status.is_active = active_.load();
    status.processor_type = "Base";
    status.total_packets_processed = total_packets_processed_.load();
    
    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(call_mutex_));
        status.current_call_id = current_call_id_;
    }
    
    return status;
}

// TCP utility functions
bool BaseAudioProcessor::write_all_fd(int fd, const void* data, size_t size) {
    const uint8_t* ptr = static_cast<const uint8_t*>(data);
    size_t remaining = size;
    
    while (remaining > 0) {
        ssize_t written = write(fd, ptr, remaining);
        if (written <= 0) return false;
        ptr += written;
        remaining -= written;
    }
    return true;
}

// read_exact removed - only used by derived classes with socket-specific implementations

void BaseAudioProcessor::send_tcp_hello(int socket_fd, const std::string& call_id) {
    if (socket_fd < 0) return;
    
    uint32_t length = htonl(call_id.length());
    
    // Send length
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

void BaseAudioProcessor::send_tcp_bye(int socket_fd) {
    if (socket_fd < 0) return;
    
    uint32_t zero = 0;
    write_all_fd(socket_fd, &zero, 4);
    std::cout << "👋 TCP BYE sent" << std::endl;
}

int BaseAudioProcessor::calculate_port_offset(const std::string& call_id) {
    if (call_id.empty()) return 0;
    
    int call_id_num = 0;
    try {
        call_id_num = std::stoi(call_id);
    } catch (const std::exception&) {
        call_id_num = 0;
    }
    
    return call_id_num;
}

// G.711 lookup table initialization
void BaseAudioProcessor::init_g711_tables() {
    // Initialize μ-law to linear table
    ulaw_to_linear_table_.resize(256);
    for (int i = 0; i < 256; ++i) {
        uint8_t ulaw = i;
        ulaw = ~ulaw;
        int sign = (ulaw & 0x80) ? -1 : 1;
        int exponent = (ulaw >> 4) & 0x07;
        int mantissa = ulaw & 0x0F;
        int sample = (mantissa << 3) + 0x84;
        sample <<= exponent;
        sample = (sample - 0x84) * sign;
        ulaw_to_linear_table_[i] = static_cast<int16_t>(sample);
    }
    
    // Initialize linear to μ-law table
    linear_to_ulaw_table_.resize(65536);
    for (int i = 0; i < 65536; ++i) {
        int16_t linear = static_cast<int16_t>(i - 32768);
        int sign = (linear < 0) ? 0x80 : 0x00;
        if (linear < 0) linear = -linear;
        if (linear > 32635) linear = 32635;
        
        linear += 0x84;
        int exponent = 7;
        for (int exp_lut = 0x4000; linear < exp_lut && exponent > 0; exp_lut >>= 1, --exponent);
        int mantissa = (linear >> (exponent + 3)) & 0x0F;
        uint8_t ulaw = ~(sign | (exponent << 4) | mantissa);
        linear_to_ulaw_table_[i] = ulaw;
    }
}

// Audio conversion utilities
std::vector<uint8_t> BaseAudioProcessor::convert_float_to_g711_ulaw(const std::vector<float>& samples) {
    std::vector<uint8_t> result;
    result.reserve(samples.size());

    // Optimized conversion: Remove redundant bounds checking in inner loop
    for (float sample : samples) {
        // Fast clamp and convert to 16-bit
        int16_t linear = static_cast<int16_t>(std::max(-32767.0f, std::min(32767.0f, sample * 32767.0f)));
        uint16_t index = static_cast<uint16_t>(linear + 32768);
        result.push_back(linear_to_ulaw_table_[index]);
    }

    return result;
}

std::vector<float> BaseAudioProcessor::convert_g711_ulaw_to_float(const std::vector<uint8_t>& g711_data) {
    std::vector<float> result;
    result.reserve(g711_data.size());
    
    for (uint8_t ulaw : g711_data) {
        int16_t linear = ulaw_to_linear_table_[ulaw];
        float sample = static_cast<float>(linear) / 32767.0f;
        result.push_back(sample);
    }
    
    return result;
}

std::vector<float> BaseAudioProcessor::resample_linear(const std::vector<float>& in, int src_rate, int dst_rate) {
    if (src_rate == dst_rate) return in;
    if (in.empty()) return {};

    size_t out_n = (in.size() * dst_rate) / src_rate;
    std::vector<float> out(out_n);

    // Optimized resampling: Pre-calculate ratio and reduce bounds checking
    const double ratio = static_cast<double>(src_rate) / static_cast<double>(dst_rate);
    const size_t in_size_minus_1 = in.size() - 1;

    for (size_t i = 0; i < out_n; ++i) {
        const double src_pos = i * ratio;
        const size_t i0 = static_cast<size_t>(src_pos);
        if (i0 >= in_size_minus_1) {
            out[i] = in.back();
        } else {
            const double t = src_pos - static_cast<double>(i0);
            out[i] = static_cast<float>((1.0 - t) * in[i0] + t * in[i0 + 1]);
        }
    }

    return out;
}

std::vector<float> BaseAudioProcessor::lowpass_telephony(const std::vector<float>& in, int) {
    // Optimized anti-aliasing low-pass filter for telephony (4kHz cutoff for 8kHz Nyquist)
    // Optimized FIR filter coefficients (pre-calculated)
    static const float coeffs[] = {0.02f, 0.12f, 0.22f, 0.28f, 0.22f, 0.12f, 0.02f};
    static const int coeffs_len = 7;
    static const int half_len = 3;

    std::vector<float> out(in.size());
    const int in_size = static_cast<int>(in.size());

    // Optimized convolution with reduced bounds checking
    for (int i = 0; i < in_size; ++i) {
        float sum = 0.0f;
        const int start_j = std::max(0, half_len - i);
        const int end_j = std::min(coeffs_len, in_size + half_len - i);

        for (int j = start_j; j < end_j; ++j) {
            sum += coeffs[j] * in[i + j - half_len];
        }
        out[i] = sum;
    }
    
    return out;
}

