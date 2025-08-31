#pragma once

#include <vector>
#include <cstdint>
#include <cstring>
#include <chrono>

// RFC 3550 compliant RTP packet structure
class RTPPacket {
public:
    // RTP Header fields (RFC 3550)
    struct Header {
        uint8_t version : 2;        // Version (always 2)
        uint8_t padding : 1;        // Padding flag
        uint8_t extension : 1;      // Extension flag
        uint8_t cc : 4;             // CSRC count
        uint8_t marker : 1;         // Marker bit
        uint8_t payload_type : 7;   // Payload type
        uint16_t sequence_number;   // Sequence number
        uint32_t timestamp;         // RTP timestamp
        uint32_t ssrc;              // Synchronization source identifier
        // CSRC list follows if cc > 0
        // Extension header follows if extension = 1
    } __attribute__((packed));

    RTPPacket() {
        header_.version = 2;
        header_.padding = 0;
        header_.extension = 0;
        header_.cc = 0;
        header_.marker = 0;
        header_.payload_type = 0;
        header_.sequence_number = 0;
        header_.timestamp = 0;
        header_.ssrc = 0;
    }

    // Create RTP packet with payload
    RTPPacket(uint8_t payload_type, uint16_t seq_num, uint32_t timestamp, 
              uint32_t ssrc, const std::vector<uint8_t>& payload, bool marker = false) {
        header_.version = 2;
        header_.padding = 0;
        header_.extension = 0;
        header_.cc = 0;
        header_.marker = marker ? 1 : 0;
        header_.payload_type = payload_type;
        header_.sequence_number = htons(seq_num);
        header_.timestamp = htonl(timestamp);
        header_.ssrc = htonl(ssrc);
        payload_ = payload;
    }

    // Parse RTP packet from raw data
    static RTPPacket parse(const uint8_t* data, size_t length) {
        RTPPacket packet;
        
        if (length < sizeof(Header)) {
            return packet; // Invalid packet
        }

        // Copy header (network byte order)
        memcpy(&packet.header_, data, sizeof(Header));
        
        // Convert from network byte order
        packet.header_.sequence_number = ntohs(packet.header_.sequence_number);
        packet.header_.timestamp = ntohl(packet.header_.timestamp);
        packet.header_.ssrc = ntohl(packet.header_.ssrc);

        // Extract payload (skip CSRC list if present)
        size_t header_size = sizeof(Header) + (packet.header_.cc * 4);
        if (length > header_size) {
            size_t payload_size = length - header_size;
            packet.payload_.resize(payload_size);
            memcpy(packet.payload_.data(), data + header_size, payload_size);
        }

        return packet;
    }

    // Serialize to wire format (RFC 3550 compliant)
    std::vector<uint8_t> serialize() const {
        std::vector<uint8_t> packet;
        packet.resize(sizeof(Header) + payload_.size());

        // Copy header in network byte order
        Header net_header = header_;
        net_header.sequence_number = htons(header_.sequence_number);
        net_header.timestamp = htonl(header_.timestamp);
        net_header.ssrc = htonl(header_.ssrc);
        
        memcpy(packet.data(), &net_header, sizeof(Header));
        
        // Copy payload
        if (!payload_.empty()) {
            memcpy(packet.data() + sizeof(Header), payload_.data(), payload_.size());
        }

        return packet;
    }

    // Accessors
    uint8_t get_payload_type() const { return header_.payload_type; }
    uint16_t get_sequence_number() const { return header_.sequence_number; }
    uint32_t get_timestamp() const { return header_.timestamp; }
    uint32_t get_ssrc() const { return header_.ssrc; }
    bool get_marker() const { return header_.marker == 1; }
    const std::vector<uint8_t>& get_payload() const { return payload_; }

    // Setters
    void set_payload_type(uint8_t pt) { header_.payload_type = pt; }
    void set_sequence_number(uint16_t seq) { header_.sequence_number = seq; }
    void set_timestamp(uint32_t ts) { header_.timestamp = ts; }
    void set_ssrc(uint32_t ssrc) { header_.ssrc = ssrc; }
    void set_marker(bool marker) { header_.marker = marker ? 1 : 0; }
    void set_payload(const std::vector<uint8_t>& payload) { payload_ = payload; }

    // Validation
    bool is_valid() const {
        return header_.version == 2 && 
               header_.payload_type < 128 && // Dynamic payload types are 96-127
               !payload_.empty();
    }

    // Size information
    size_t header_size() const { return sizeof(Header) + (header_.cc * 4); }
    size_t payload_size() const { return payload_.size(); }
    size_t total_size() const { return header_size() + payload_size(); }

private:
    Header header_;
    std::vector<uint8_t> payload_;
};

// RTP session state for proper sequence/timestamp management
class RTPSession {
public:
    RTPSession(uint32_t ssrc = 0) : ssrc_(ssrc), sequence_number_(0), timestamp_(0) {
        if (ssrc_ == 0) {
            // Generate random SSRC (RFC 3550 recommendation)
            ssrc_ = static_cast<uint32_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        }
        
        // Initialize with random values (RFC 3550 recommendation)
        sequence_number_ = static_cast<uint16_t>(ssrc_ & 0xFFFF);
        timestamp_ = static_cast<uint32_t>(ssrc_);
    }

    // Create next RTP packet for this session
    RTPPacket create_packet(uint8_t payload_type, const std::vector<uint8_t>& payload, 
                           uint32_t timestamp_increment = 160, bool marker = false) {
        RTPPacket packet(payload_type, sequence_number_, timestamp_, ssrc_, payload, marker);
        
        // Update session state
        sequence_number_++;
        timestamp_ += timestamp_increment;
        
        return packet;
    }

    // Getters
    uint32_t get_ssrc() const { return ssrc_; }
    uint16_t get_current_sequence() const { return sequence_number_; }
    uint32_t get_current_timestamp() const { return timestamp_; }

    // Manual control (for specific use cases)
    void set_sequence_number(uint16_t seq) { sequence_number_ = seq; }
    void set_timestamp(uint32_t ts) { timestamp_ = ts; }

private:
    uint32_t ssrc_;
    uint16_t sequence_number_;
    uint32_t timestamp_;
};

// Common RTP payload types (RFC 3551)
namespace RTPPayloadType {
    constexpr uint8_t PCMU = 0;         // G.711 μ-law
    constexpr uint8_t PCMA = 8;         // G.711 A-law
    constexpr uint8_t G722 = 9;         // G.722
    constexpr uint8_t TELEPHONE_EVENT = 101; // RFC 4733 DTMF
}

// RTP timing constants for common codecs
namespace RTPTiming {
    constexpr uint32_t G711_SAMPLES_PER_PACKET = 160;  // 20ms at 8kHz
    constexpr uint32_t G711_TIMESTAMP_INCREMENT = 160; // 160 samples per packet
    constexpr uint32_t G722_SAMPLES_PER_PACKET = 320;  // 20ms at 16kHz
    constexpr uint32_t G722_TIMESTAMP_INCREMENT = 320; // 320 samples per packet
}

// Utility functions for codec conversion
namespace RTPCodec {
    // Convert float samples to G.711 μ-law
    inline std::vector<uint8_t> float_to_g711_ulaw(const std::vector<float>& samples) {
        std::vector<uint8_t> result;
        result.reserve(samples.size());

        for (float sample : samples) {
            // Clamp to [-1.0, 1.0]
            sample = std::max(-1.0f, std::min(1.0f, sample));
            int16_t pcm_sample = static_cast<int16_t>(sample * 32767.0f);

            // Simple μ-law encoding (ITU-T G.711)
            uint8_t ulaw_byte = 0xFF; // Default silence
            int16_t abs_sample = abs(pcm_sample);

            if (abs_sample >= 8159) ulaw_byte = 0x70;
            else if (abs_sample >= 4063) ulaw_byte = 0x60;
            else if (abs_sample >= 2015) ulaw_byte = 0x50;
            else if (abs_sample >= 991) ulaw_byte = 0x40;
            else if (abs_sample >= 479) ulaw_byte = 0x30;
            else if (abs_sample >= 223) ulaw_byte = 0x20;
            else if (abs_sample >= 95) ulaw_byte = 0x10;
            else ulaw_byte = 0x00;

            if (pcm_sample < 0) ulaw_byte |= 0x80;
            result.push_back(ulaw_byte ^ 0xFF); // Complement for μ-law
        }

        return result;
    }

    // Convert float samples to G.711 A-law
    inline std::vector<uint8_t> float_to_g711_alaw(const std::vector<float>& samples) {
        std::vector<uint8_t> result;
        result.reserve(samples.size());

        for (float sample : samples) {
            // Clamp to [-1.0, 1.0]
            sample = std::max(-1.0f, std::min(1.0f, sample));
            int16_t pcm_sample = static_cast<int16_t>(sample * 32767.0f);

            // Simple A-law encoding (ITU-T G.711)
            uint8_t alaw_byte = 0x55; // Default silence
            int16_t abs_sample = abs(pcm_sample);

            if (abs_sample >= 4096) alaw_byte = 0x70;
            else if (abs_sample >= 2048) alaw_byte = 0x60;
            else if (abs_sample >= 1024) alaw_byte = 0x50;
            else if (abs_sample >= 512) alaw_byte = 0x40;
            else if (abs_sample >= 256) alaw_byte = 0x30;
            else if (abs_sample >= 128) alaw_byte = 0x20;
            else if (abs_sample >= 64) alaw_byte = 0x10;
            else alaw_byte = 0x00;

            if (pcm_sample < 0) alaw_byte |= 0x80;
            result.push_back(alaw_byte ^ 0x55); // Complement for A-law
        }

        return result;
    }

    // Convert G.711 μ-law to float samples
    inline std::vector<float> g711_ulaw_to_float(const std::vector<uint8_t>& data) {
        // μ-law decode table (simplified)
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

        std::vector<float> result;
        result.reserve(data.size());

        for (uint8_t byte : data) {
            int16_t sample = ulaw_table[byte];
            result.push_back(static_cast<float>(sample) / 32768.0f);
        }

        return result;
    }

    // Convert G.711 A-law to float samples
    inline std::vector<float> g711_alaw_to_float(const std::vector<uint8_t>& data) {
        std::vector<float> result;
        result.reserve(data.size());

        for (uint8_t byte : data) {
            // A-law decoding (simplified implementation)
            uint8_t sign = byte & 0x80;
            uint8_t exponent = (byte & 0x70) >> 4;
            uint8_t mantissa = byte & 0x0F;

            int16_t sample = 0;
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
}

// Internal session tracking (NEVER transmitted over network)
struct InternalSessionData {
    std::string session_id;                    // Internal tracking only
    std::string caller_phone;                  // Internal tracking only
    int line_id;                              // Internal tracking only
    std::chrono::steady_clock::time_point created_time;
    std::chrono::steady_clock::time_point last_activity;
    
    // This data is NEVER included in RTP packets
    // It's only used for internal session management
};
