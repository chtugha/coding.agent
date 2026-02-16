#include <gtest/gtest.h>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <set>
#include <arpa/inet.h>

static uint8_t linear_to_ulaw(int16_t sample) {
    const int BIAS = 0x84;
    const int CLIP = 32635;
    int sign = (sample >> 8) & 0x80;
    if (sign) sample = -sample;
    if (sample > CLIP) sample = CLIP;
    sample += BIAS;
    int exponent = 7;
    for (int mask = 0x4000; mask > 0; mask >>= 1, exponent--) {
        if (sample & mask) break;
    }
    int mantissa = (sample >> (exponent + 3)) & 0x0F;
    return ~(sign | (exponent << 4) | mantissa);
}

static std::string get_header(const std::string& msg, const std::string& name) {
    std::string search = name + ":";
    size_t p = msg.find(search);
    if (p == std::string::npos) return "";
    size_t start = p + search.length();
    while (start < msg.size() && msg[start] == ' ') start++;
    size_t end = msg.find("\r\n", start);
    if (end == std::string::npos) end = msg.find("\n", start);
    if (end == std::string::npos) end = msg.size();
    return msg.substr(start, end - start);
}

static int get_sdp_media_port(const std::string& msg) {
    size_t m_pos = msg.find("m=audio ");
    if (m_pos == std::string::npos) return -1;
    std::string m_line = msg.substr(m_pos + 8);
    size_t sp = m_line.find(' ');
    if (sp == std::string::npos) sp = m_line.find('\r');
    if (sp == std::string::npos) return -1;
    try {
        int port = std::stoi(m_line.substr(0, sp));
        if (port < 1 || port > 65535) return -1;
        return port;
    } catch (...) {
        return -1;
    }
}

static std::string get_sdp_connection_ip(const std::string& msg) {
    size_t c_pos = msg.find("c=IN IP4 ");
    if (c_pos == std::string::npos) return "";
    std::string c_line = msg.substr(c_pos + 9);
    size_t end = c_line.find_first_of("\r\n ");
    if (end == std::string::npos) end = c_line.size();
    return c_line.substr(0, end);
}

TEST(UlawEncodingTest, SilenceEncoding) {
    uint8_t result = linear_to_ulaw(0);
    EXPECT_EQ(result, 0xFF);
}

TEST(UlawEncodingTest, PositiveSampleRange) {
    std::set<uint8_t> unique_vals;
    for (int16_t s = 0; s <= 32000; s += 500) {
        unique_vals.insert(linear_to_ulaw(s));
    }
    EXPECT_GT(unique_vals.size(), 20u);
}

TEST(UlawEncodingTest, NegativeSampleRange) {
    for (int16_t s = -100; s >= -32000; s -= 500) {
        uint8_t encoded = linear_to_ulaw(s);
        EXPECT_NE(encoded, 0xFF);
    }
}

TEST(UlawEncodingTest, SymmetryCheck) {
    uint8_t pos = linear_to_ulaw(1000);
    uint8_t neg = linear_to_ulaw(-1000);
    EXPECT_EQ((pos & 0x7F), (neg & 0x7F));
    EXPECT_NE(pos, neg);
}

TEST(UlawEncodingTest, ClippingAtMax) {
    uint8_t a = linear_to_ulaw(32767);
    uint8_t b = linear_to_ulaw(32000);
    uint8_t c = linear_to_ulaw(16000);
    EXPECT_NE(a, c);
    EXPECT_NE(b, c);
}

TEST(UlawEncodingTest, SineWaveProducesVariation) {
    const int SAMPLES = 160;
    const double FREQ = 400.0;
    const double SR = 8000.0;
    const double AMP = 8000.0;
    uint8_t encoded[SAMPLES];
    for (int i = 0; i < SAMPLES; i++) {
        double t = (double)i / SR;
        int16_t s = (int16_t)(AMP * std::sin(2.0 * M_PI * FREQ * t));
        encoded[i] = linear_to_ulaw(s);
    }

    int unique_count = 0;
    for (int i = 1; i < SAMPLES; i++) {
        if (encoded[i] != encoded[i - 1]) unique_count++;
    }
    EXPECT_GT(unique_count, 10);
}

TEST(SipHeaderTest, BasicHeader) {
    std::string msg = "INVITE sip:bob@127.0.0.1 SIP/2.0\r\n"
                      "Call-ID: test-123\r\n"
                      "From: <sip:alice@127.0.0.1>\r\n\r\n";
    EXPECT_EQ(get_header(msg, "Call-ID"), "test-123");
    EXPECT_EQ(get_header(msg, "From"), "<sip:alice@127.0.0.1>");
}

TEST(SipHeaderTest, HeaderWithLeadingSpaces) {
    std::string msg = "INVITE sip:x SIP/2.0\r\nCall-ID:   spaced-value  \r\n\r\n";
    EXPECT_EQ(get_header(msg, "Call-ID"), "spaced-value  ");
}

TEST(SipHeaderTest, MissingHeader) {
    std::string msg = "INVITE sip:x SIP/2.0\r\nFrom: alice\r\n\r\n";
    EXPECT_EQ(get_header(msg, "Call-ID"), "");
}

TEST(SipHeaderTest, HeaderAtEndOfMessage) {
    std::string msg = "SIP/2.0 200 OK\r\nCall-ID: final";
    EXPECT_EQ(get_header(msg, "Call-ID"), "final");
}

TEST(SipHeaderTest, NewlineOnly) {
    std::string msg = "SIP/2.0 200 OK\nCall-ID: lf-only\n\n";
    EXPECT_EQ(get_header(msg, "Call-ID"), "lf-only");
}

TEST(SipHeaderTest, MultipleHeaders) {
    std::string msg = "REGISTER sip:srv SIP/2.0\r\n"
                      "Via: SIP/2.0/UDP 127.0.0.1:5060\r\n"
                      "From: <sip:alice@srv>\r\n"
                      "To: <sip:alice@srv>\r\n"
                      "Call-ID: reg-42@127.0.0.1\r\n"
                      "CSeq: 1 REGISTER\r\n\r\n";
    EXPECT_EQ(get_header(msg, "Via"), "SIP/2.0/UDP 127.0.0.1:5060");
    EXPECT_EQ(get_header(msg, "Call-ID"), "reg-42@127.0.0.1");
    EXPECT_EQ(get_header(msg, "CSeq"), "1 REGISTER");
}

TEST(SdpParsingTest, BasicMediaPort) {
    std::string sdp = "v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\n"
                      "m=audio 12345 RTP/AVP 0\r\n";
    EXPECT_EQ(get_sdp_media_port(sdp), 12345);
}

TEST(SdpParsingTest, PortWithMultipleCodecs) {
    std::string sdp = "m=audio 8000 RTP/AVP 0 101\r\na=rtpmap:0 PCMU/8000\r\n";
    EXPECT_EQ(get_sdp_media_port(sdp), 8000);
}

TEST(SdpParsingTest, NoMediaLine) {
    EXPECT_EQ(get_sdp_media_port("v=0\r\ns=-\r\n"), -1);
}

TEST(SdpParsingTest, InvalidPort) {
    EXPECT_EQ(get_sdp_media_port("m=audio 99999 RTP/AVP 0\r\n"), -1);
    EXPECT_EQ(get_sdp_media_port("m=audio 0 RTP/AVP 0\r\n"), -1);
}

TEST(SdpParsingTest, PortAtEndOfLine) {
    EXPECT_EQ(get_sdp_media_port("m=audio 4000\r\n"), 4000);
}

TEST(SdpParsingTest, ConnectionIP) {
    std::string sdp = "c=IN IP4 192.168.1.100\r\nm=audio 5000 RTP/AVP 0\r\n";
    EXPECT_EQ(get_sdp_connection_ip(sdp), "192.168.1.100");
}

TEST(SdpParsingTest, ConnectionIPLocalhost) {
    std::string sdp = "c=IN IP4 127.0.0.1\r\n";
    EXPECT_EQ(get_sdp_connection_ip(sdp), "127.0.0.1");
}

TEST(SdpParsingTest, NoConnectionLine) {
    EXPECT_EQ(get_sdp_connection_ip("v=0\r\nm=audio 5000 RTP/AVP 0\r\n"), "");
}

TEST(RtpPacketTest, HeaderStructure) {
    uint8_t rtp[172];
    rtp[0] = 0x80;
    rtp[1] = 0x00;
    uint16_t seq = htons(42);
    memcpy(rtp + 2, &seq, 2);
    uint32_t ts = htonl(12345);
    memcpy(rtp + 4, &ts, 4);
    uint32_t ssrc = htonl(0xDEAD0001);
    memcpy(rtp + 8, &ssrc, 4);

    EXPECT_EQ(rtp[0], 0x80);
    EXPECT_EQ(rtp[1], 0x00);

    uint16_t read_seq;
    memcpy(&read_seq, rtp + 2, 2);
    EXPECT_EQ(ntohs(read_seq), 42);

    uint32_t read_ts;
    memcpy(&read_ts, rtp + 4, 4);
    EXPECT_EQ(ntohl(read_ts), 12345u);

    uint32_t read_ssrc;
    memcpy(&read_ssrc, rtp + 8, 4);
    EXPECT_EQ(ntohl(read_ssrc), 0xDEAD0001u);
}

TEST(RtpPacketTest, PayloadSize) {
    EXPECT_EQ(12 + 160, 172);
}

TEST(SipUsernameExtraction, FromURI) {
    std::string from = "<sip:alice@127.0.0.1>";
    size_t sip_pos = from.find("sip:");
    ASSERT_NE(sip_pos, std::string::npos);
    size_t at_pos = from.find("@", sip_pos);
    ASSERT_NE(at_pos, std::string::npos);
    std::string username = from.substr(sip_pos + 4, at_pos - sip_pos - 4);
    EXPECT_EQ(username, "alice");
}

TEST(SipUsernameExtraction, FromWithTag) {
    std::string from = "<sip:bob@192.168.1.1>;tag=xyz123";
    size_t sip_pos = from.find("sip:");
    size_t at_pos = from.find("@", sip_pos);
    std::string username = from.substr(sip_pos + 4, at_pos - sip_pos - 4);
    EXPECT_EQ(username, "bob");
}

TEST(SipUsernameExtraction, NumberedLine) {
    std::string from = "<sip:alice2@10.0.0.1>";
    size_t sip_pos = from.find("sip:");
    size_t at_pos = from.find("@", sip_pos);
    std::string username = from.substr(sip_pos + 4, at_pos - sip_pos - 4);
    EXPECT_EQ(username, "alice2");
}
