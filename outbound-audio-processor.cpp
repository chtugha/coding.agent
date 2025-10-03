#include "outbound-audio-processor.h"
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fstream>
#include <sys/time.h>


OutboundAudioProcessor::OutboundAudioProcessor()
    : piper_tcp_socket_(-1)
    , piper_tcp_port_(-1)
    , piper_connected_(false)
    , registration_running_(false)
{
}

OutboundAudioProcessor::~OutboundAudioProcessor() {
    stop();
}

bool OutboundAudioProcessor::start(int base_port) {
    if (running_.load()) return true;

    base_port_ = base_port;

    running_.store(true);
    active_.store(false); // Start in sleeping state

    std::cout << "😴 Outbound Audio Processor started (SLEEPING) on base port " << base_port << std::endl;
    std::cout << "📡 TCP sockets will be created dynamically based on call_id" << std::endl;

    return true;
}

void OutboundAudioProcessor::stop() {
    BaseAudioProcessor::stop();

    // Stop registration polling
    stop_registration_polling();

    // Stop output scheduler first
    stop_output_scheduler_();

    // Close TCP socket
    {
        std::lock_guard<std::mutex> lock(piper_mutex_);
        if (piper_tcp_socket_ >= 0) {
            close(piper_tcp_socket_);
            piper_tcp_socket_ = -1;
        }
        piper_connected_.store(false);
    }

    // Join TCP thread
    if (piper_tcp_thread_.joinable()) {
        piper_tcp_thread_.join();
    }

    std::cout << "🛑 Outbound Audio Processor stopped" << std::endl;
}

void OutboundAudioProcessor::set_sip_client_callback(std::function<void(const std::string&, const std::vector<uint8_t>&)> callback) {
    sip_client_callback_ = callback;
}

void OutboundAudioProcessor::activate_for_call(const std::string& call_id) {
    BaseAudioProcessor::activate_for_call(call_id);

    if (!active_.load()) return;

    // Determine Kokoro (Piper) TCP port (server lives in Kokoro now)
    piper_tcp_port_ = calculate_piper_port(call_id);

    // Start UDP registration listener and, upon REGISTER, connect to Kokoro
    start_registration_polling(call_id);

    std::cout << "✅ Outbound Audio Processor ACTIVE - will connect to Kokoro on port " << piper_tcp_port_ << " for call " << call_id << std::endl;
}

void OutboundAudioProcessor::deactivate_after_call() {
    // Stop registration polling
    stop_registration_polling();


    BaseAudioProcessor::deactivate_after_call();

    // Stop output scheduler
    stop_output_scheduler_();

    // Close Piper TCP connection
    {
        std::lock_guard<std::mutex> lock(piper_mutex_);
        if (piper_tcp_socket_ >= 0) {
            close(piper_tcp_socket_);
            piper_tcp_socket_ = -1;
        }
        piper_connected_.store(false);
    }

    // Join TCP thread
    if (piper_tcp_thread_.joinable()) {
        piper_tcp_thread_.join();
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
std::vector<uint8_t> OutboundAudioProcessor::process_float_mono_to_ulaw(const std::vector<float>& mono, uint32_t sample_rate) {
    if (mono.empty()) return {};
    std::vector<float> work = mono;
    if (sample_rate > 8000) work = lowpass_telephony(work, sample_rate);
    if (sample_rate != 8000) work = resample_linear(work, sample_rate, 8000);
    return convert_float_to_g711_ulaw(work);
}

OutboundAudioProcessor::AudioFileType OutboundAudioProcessor::detect_audio_file_type(const std::vector<uint8_t>& bytes) {
    if (bytes.size() >= 12 && bytes[0]=='R' && bytes[1]=='I' && bytes[2]=='F' && bytes[3]=='F' && bytes[8]=='W' && bytes[9]=='A' && bytes[10]=='V' && bytes[11]=='E') return AudioFileType::WAV;
    if (bytes.size() >= 3 && bytes[0]==0xFF && (bytes[1]&0xE0)==0xE0) return AudioFileType::MP3;
    if (bytes.size() >= 12 && bytes[4]=='f' && bytes[5]=='t' && bytes[6]=='y' && bytes[7]=='p') return AudioFileType::MP4; // MP4/M4A
    if (bytes.size() >= 4 && bytes[0]=='f' && bytes[1]=='L' && bytes[2]=='a' && bytes[3]=='C') return AudioFileType::FLAC;
    if (bytes.size() >= 4 && bytes[0]=='O' && bytes[1]=='g' && bytes[2]=='g' && bytes[3]=='S') return AudioFileType::OGG;
    return AudioFileType::UNKNOWN;
}

bool OutboundAudioProcessor::read_entire_file(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    std::streampos len = f.tellg();
    if (len <= 0) return false;
    out.resize(static_cast<size_t>(len));
    f.seekg(0, std::ios::beg);
    f.read(reinterpret_cast<char*>(out.data()), len);
    return bool(f);
}

bool OutboundAudioProcessor::parse_wav_header(const std::vector<uint8_t>& b,
                                 uint16_t& fmt, uint16_t& channels, uint32_t& rate,
                                 uint16_t& bits_per_sample, size_t& data_offset, size_t& data_size) {
    if (b.size() < 44) return false;
    if (!(b[0]=='R'&&b[1]=='I'&&b[2]=='F'&&b[3]=='F'&&b[8]=='W'&&b[9]=='A'&&b[10]=='V'&&b[11]=='E')) return false;
    size_t pos = 12;
    bool have_fmt=false, have_data=false;
    while (pos + 8 <= b.size()) {
        uint32_t id = *reinterpret_cast<const uint32_t*>(&b[pos]); pos += 4;
        uint32_t sz = *reinterpret_cast<const uint32_t*>(&b[pos]); pos += 4;
        if (pos + sz > b.size()) break;
        if (id == 0x20746d66u && sz >= 16) { // 'fmt '
            fmt = *reinterpret_cast<const uint16_t*>(&b[pos+0]);
            channels = *reinterpret_cast<const uint16_t*>(&b[pos+2]);
            rate = *reinterpret_cast<const uint32_t*>(&b[pos+4]);
            bits_per_sample = *reinterpret_cast<const uint16_t*>(&b[pos+14]);
            have_fmt = true;
        } else if (id == 0x61746164u) { // 'data'
            data_offset = pos;
            data_size = sz;
            have_data = true;
        }
        pos += sz;
    }
    return have_fmt && have_data;
}

std::vector<float> OutboundAudioProcessor::decode_bytes_to_float_mono(const std::vector<uint8_t>& bytes, AudioFileType type, uint32_t& sample_rate) {
    sample_rate = 0;
    std::vector<float> mono;
    if (type == AudioFileType::WAV) {
        uint16_t fmt=0, ch=0, bps=0; uint32_t rate=0; size_t off=0, sz=0;
        if (!parse_wav_header(bytes, fmt, ch, rate, bps, off, sz) || ch==0) return {};
        sample_rate = rate;
        const uint8_t* p = bytes.data() + off; const uint8_t* end = p + sz;
        auto push_frame = [&](double acc){ mono.push_back(static_cast<float>(acc / ch)); };
        if (fmt == 1) { // PCM
            if (bps == 8) {
                while (p < end) { double acc=0.0; for (int c=0;c<ch && p<end;++c) acc += double(int(int8_t(*p++ - 128))<<8) / 32768.0; push_frame(acc);} }
            else if (bps == 16) { while (p + 2*ch <= end) { double acc=0.0; for (int c=0;c<ch;++c){ int16_t v = int16_t(p[0] | (p[1]<<8)); p+=2; acc += double(v)/32768.0; } push_frame(acc);} }
            else if (bps == 24) { while (p + 3*ch <= end) { double acc=0.0; for (int c=0;c<ch;++c){ int32_t v = (p[0]|(p[1]<<8)|(p[2]<<16)); if (v & 0x00800000) v |= 0xFF000000; p+=3; acc += double(v)/32768.0; } push_frame(acc);} }
            else if (bps == 32) { while (p + 4*ch <= end) { double acc=0.0; for (int c=0;c<ch;++c){ int32_t v = (p[0]|(p[1]<<8)|(p[2]<<16)|(p[3]<<24)); p+=4; acc += double(v)/32768.0; } push_frame(acc);} }
            else return {};
        } else if (fmt == 3) { // IEEE float32
            while (p + 4*ch <= end) { double acc=0.0; const float* pf = reinterpret_cast<const float*>(p); for (int c=0;c<ch;++c) acc += pf[c]; p += 4*ch; push_frame(acc);}
        } else if (fmt == 6 || fmt == 7) { // A-law or μ-law
            auto alaw_to_linear = [](uint8_t a)->int16_t{ a^=0x55; int sign=a&0x80; int exp=(a&0x70)>>4; int mant=a&0x0F; int sample=(mant<<4)+8; if(exp) sample=(sample+0x100)<<(exp-1); if(sign) sample=-sample; return (int16_t)sample; };
            auto mulaw_to_linear = [](uint8_t u)->int16_t{ u=~u; int t=((u&0x0F)<<3)+0x84; t <<= ((unsigned)u&0x70)>>4; return (u&0x80)?(0x84-t):(t-0x84); };
            while (p + ch <= end) { double acc=0.0; for (int c=0;c<ch;++c){ uint8_t b=*p++; int16_t lin = (fmt==6)? alaw_to_linear(b): mulaw_to_linear(b); acc += double(lin)/32768.0; } push_frame(acc);}
        } else {
            return {};
        }
    } else {
        // Unsupported types today (MP3/MP4/M4A/FLAC/OGG) — placeholder for future decoders
        return {};
    }
    return mono;
}


void OutboundAudioProcessor::set_shared_memory_out(std::shared_ptr<ShmAudioChannel> channel) {
    out_channel_ = std::move(channel);
    ensure_output_running();
}

void OutboundAudioProcessor::set_silence_wav2_bytes(const std::vector<uint8_t>& bytes) {
    std::lock_guard<std::mutex> lk(out_buffer_mutex_);
    silence_wav2_ = bytes;
    silence_wav2_pos_ = 0;
}

void OutboundAudioProcessor::ensure_output_running() {
    if (running_.load() && !output_running_.load()) {
        start_output_scheduler_();
    }
}

BaseAudioProcessor::ProcessorStatus OutboundAudioProcessor::get_status() const {
    auto status = BaseAudioProcessor::get_status();
    status.processor_type = "Outbound";
    return status;
}

bool OutboundAudioProcessor::load_and_set_silence_wav2(const std::string& wav_path) {
    std::vector<uint8_t> file_bytes;
    if (!read_entire_file(wav_path, file_bytes)) return false;
    auto type = detect_audio_file_type(file_bytes);
    uint32_t src_rate = 0;
    auto mono = decode_bytes_to_float_mono(file_bytes, type, src_rate);
    if (mono.empty() || src_rate == 0) return false;
    auto g711 = process_float_mono_to_ulaw(mono, src_rate);
    if (g711.empty()) return false;
    set_silence_wav2_bytes(g711);
    return true;
}




void OutboundAudioProcessor::enqueue_g711_(const std::vector<uint8_t>& g711) {
    std::lock_guard<std::mutex> lk(out_buffer_mutex_);
    // Cap buffer to avoid unbounded latency (keep ~12s @20ms frames); drop oldest to preserve continuity
    constexpr size_t kMaxBytes = 160 * 600; // 600 frames * 160 bytes/frame ≈ 12s
    if (!g711.empty()) {
        // Trim oldest if this insert would exceed cap
        if (out_buffer_.size() + g711.size() > kMaxBytes) {
            size_t overflow = (out_buffer_.size() + g711.size()) - kMaxBytes;
            overflow = std::min(overflow, out_buffer_.size());
            if (overflow > 0) {
                out_buffer_.erase(out_buffer_.begin(), out_buffer_.begin() + overflow);
                std::cout << "⚠️ Outbound buffer trimmed " << overflow << " bytes to keep up" << std::endl;
            }
        }
        out_buffer_.insert(out_buffer_.end(), g711.begin(), g711.end());
    }
}

void OutboundAudioProcessor::make_silence_frame_(std::vector<uint8_t>& frame) {
    frame.resize(160, 0xFF); // μ-law silence

    // Only play test WAV if Piper is NOT connected
    // Once Piper connects, we play actual silence until Piper audio arrives
    if (!piper_connected_.load() && !silence_wav2_.empty()) {
        for (size_t i = 0; i < 160; ++i) {
            frame[i] = silence_wav2_[silence_wav2_pos_];
            silence_wav2_pos_ = (silence_wav2_pos_ + 1) % silence_wav2_.size();
        }
    }
}

void OutboundAudioProcessor::start_output_scheduler_() {
    if (output_running_.exchange(true)) return;
    output_thread_ = std::thread([this]() {
        auto next = std::chrono::steady_clock::now();
        const auto interval = std::chrono::milliseconds(20);
        while (running_.load() && output_running_.load()) {
            std::vector<uint8_t> frame;
            bool have_audio = false;
            if (out_channel_) {
                std::lock_guard<std::mutex> lk(out_buffer_mutex_);
                if (out_buffer_.size() >= 160) {
                    frame.assign(out_buffer_.begin(), out_buffer_.begin() + 160);
                    out_buffer_.erase(out_buffer_.begin(), out_buffer_.begin() + 160);
                    have_audio = true;
                }
            }
            if (!have_audio) {
                make_silence_frame_(frame);
            }
            if (out_channel_) {
                (void)out_channel_->write_frame(frame.data(), frame.size());
            }
            next += interval;
            std::this_thread::sleep_until(next);
        }
    });
}

void OutboundAudioProcessor::stop_output_scheduler_() {
    if (!output_running_.exchange(false)) return;
    if (output_thread_.joinable()) output_thread_.join();
}

// Private methods
bool OutboundAudioProcessor::setup_piper_tcp_socket(const std::string& call_id) {
    piper_tcp_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (piper_tcp_socket_ < 0) {
        std::cout << "❌ Failed to create Piper TCP socket" << std::endl;
        return false;
    }

    int opt = 1;
    setsockopt(piper_tcp_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    piper_tcp_port_ = calculate_piper_port(call_id);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(piper_tcp_port_);

    if (bind(piper_tcp_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cout << "❌ Failed to bind Piper TCP socket to port " << piper_tcp_port_ << std::endl;
        close(piper_tcp_socket_);
        piper_tcp_socket_ = -1;
        return false;
    }

    if (listen(piper_tcp_socket_, 1) < 0) {
        std::cout << "❌ Failed to listen on Piper TCP socket" << std::endl;
        close(piper_tcp_socket_);
        piper_tcp_socket_ = -1;
        return false;
    }

    // Start connection handler thread
    piper_tcp_thread_ = std::thread(&OutboundAudioProcessor::handle_piper_tcp_connection, this);

    std::cout << "✅ Piper TCP socket listening on port " << piper_tcp_port_ << " for call " << call_id << std::endl;
    return true;
}

int OutboundAudioProcessor::calculate_piper_port(const std::string& call_id) {
    int port = 9002 + calculate_port_offset(call_id);
    std::cout << "🔢 Piper port for call " << call_id << ": " << port << " (9002 + " << calculate_port_offset(call_id) << ")" << std::endl;
    return port;
}

void OutboundAudioProcessor::handle_piper_tcp_connection() {
    std::cout << "👂 Piper TCP connection handler started" << std::endl;

    while (running_.load() && piper_tcp_socket_ >= 0) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_socket = accept(piper_tcp_socket_, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (!running_.load()) break;
            std::cout << "❌ Failed to accept Piper client connection" << std::endl;
            continue; // keep accepting
        }

        std::cout << "🔗 Piper client connected" << std::endl;
        piper_connected_.store(true);

        // Read HELLO message and verify call_id
        std::string expected_call_id;
        {
            std::lock_guard<std::mutex> lock(call_mutex_);
            expected_call_id = current_call_id_;
        }
        bool hello_ok = false;
        uint32_t length = 0;
        if (read_exact_from_socket(client_socket, &length, 4)) {
            length = ntohl(length);
            if (length > 0 && length < 1024) {
                std::vector<char> call_id_buffer(length + 1);
                if (read_exact_from_socket(client_socket, call_id_buffer.data(), length)) {
                    call_id_buffer[length] = '\0';
                    std::string received_call_id(call_id_buffer.data());
                    std::cout << "📡 TCP HELLO received from Piper for call: " << received_call_id << std::endl;
                    if (received_call_id == expected_call_id) hello_ok = true;
                }
            }
        }
        if (!hello_ok) {
            std::cout << "⚠️ Piper HELLO missing/mismatch; closing connection" << std::endl;
            close(client_socket);
            piper_connected_.store(false);
            continue;
        }

        // Process incoming audio data for this connection
        while (running_.load() && piper_connected_.load()) {
            uint32_t chunk_length = 0, sample_rate = 0, chunk_id = 0;

            // Read chunk header: [length][sample_rate][chunk_id]
            if (!read_exact_from_socket(client_socket, &chunk_length, 4) ||
                !read_exact_from_socket(client_socket, &sample_rate, 4) ||
                !read_exact_from_socket(client_socket, &chunk_id, 4)) {
                break;
            }

            chunk_length = ntohl(chunk_length);
            sample_rate = ntohl(sample_rate);
            chunk_id = ntohl(chunk_id);

            if (chunk_length == 0) {
                std::cout << "📡 TCP BYE received from Piper" << std::endl;
                break;
            }

            // Allow up to 10MB chunks (long sentences can generate large audio)
            if (chunk_length > 10 * 1024 * 1024) {
                std::cout << "⚠️ Piper chunk too large (" << chunk_length << " bytes) — dropping" << std::endl;
                break;
            }

            // Duplicate check per call
            std::string call_id;
            {
                std::lock_guard<std::mutex> lock(call_mutex_);
                call_id = current_call_id_;
            }
            {
                std::lock_guard<std::mutex> lk(chunk_dedup_mutex_);
                uint32_t &last = last_chunk_id_[call_id];
                if (chunk_id <= last) {
                    std::vector<uint8_t> tmp(std::min(chunk_length, 4096u));
                    uint32_t remaining = chunk_length;
                    while (remaining > 0 && running_.load()) {
                        uint32_t to_read = std::min(remaining, static_cast<uint32_t>(tmp.size()));
                        if (!read_exact_from_socket(client_socket, tmp.data(), to_read)) break;
                        remaining -= to_read;
                    }
                    std::cout << "⚠️ Dropped duplicate Piper TTS chunk id " << chunk_id << " (last=" << last << ") for call " << call_id << std::endl;
                    continue;
                }
            }

            // Read payload
            std::vector<uint8_t> payload(chunk_length);
            if (!read_exact_from_socket(client_socket, payload.data(), chunk_length)) break;

            // Process audio chunk (enqueue into scheduler buffer)
            process_piper_audio_chunk(payload, sample_rate, chunk_id);

            // Update last chunk ID
            {
                std::lock_guard<std::mutex> lk(chunk_dedup_mutex_);
                uint32_t &last = last_chunk_id_[call_id];
                last = std::max(last, chunk_id);
            }
        }

        close(client_socket);
        piper_connected_.store(false);
        std::cout << "🔌 Piper client disconnected" << std::endl;
        // Continue accepting new connections
    }

    std::cout << "👂 Piper TCP connection handler stopped" << std::endl;
}

bool OutboundAudioProcessor::read_exact_from_socket(int socket_fd, void* data, size_t size) {
    uint8_t* ptr = static_cast<uint8_t*>(data);
    size_t remaining = size;

    while (remaining > 0) {
        ssize_t received = read(socket_fd, ptr, remaining);
        if (received <= 0) return false;
        ptr += received;
        remaining -= received;
    }
    return true;
}

void OutboundAudioProcessor::process_piper_audio_chunk(const std::vector<uint8_t>& payload, uint32_t sample_rate, uint32_t chunk_id) {
    // Unified pipeline: Piper provides float32 PCM
    if ((payload.size() % 4) == 0) {
        const float* f = reinterpret_cast<const float*>(payload.data());
        std::vector<float> mono(f, f + (payload.size() / 4));
        auto g711 = process_float_mono_to_ulaw(mono, sample_rate);
        enqueue_g711_(g711);
        std::cout << "📤 Piper TTS enqueued (float->G711): " << g711.size() << " bytes @8kHz, src_rate=" << sample_rate << ", id=" << chunk_id << std::endl;
    } else {
        // If Piper ever sends already-encoded bytes, accept as-is
        enqueue_g711_(payload);
        std::cout << "📤 Piper TTS enqueued (bytes passthrough): " << payload.size() << " bytes, id=" << chunk_id << std::endl;
    }
}

// Registration polling implementation
void OutboundAudioProcessor::start_registration_polling(const std::string& call_id) {
    stop_registration_polling(); // Stop any existing polling

    registration_running_.store(true);
    registration_thread_ = std::thread(&OutboundAudioProcessor::registration_polling_thread, this, call_id);

    std::cout << "🔄 Started registration polling for call " << call_id << std::endl;
}

void OutboundAudioProcessor::stop_registration_polling() {
    registration_running_.store(false);

    if (registration_thread_.joinable()) {
        registration_thread_.join();
    }
}

void OutboundAudioProcessor::registration_polling_thread(const std::string& call_id) {
    // Listen for UDP REGISTER from Kokoro on 13000 + call
    int offset = calculate_port_offset(call_id);
    int udp_port = 13000 + offset;

    int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        std::cout << "❌ Failed to create UDP socket for REGISTER listener" << std::endl;
        return;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(udp_port);

    if (bind(udp_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cout << "❌ Failed to bind UDP REGISTER listener to port " << udp_port << std::endl;
        close(udp_sock);
        return;
    }

    std::cout << "📡 Outbound waiting for REGISTER on UDP port " << udp_port << " for call " << call_id << std::endl;

    // Wait for REGISTER then connect to Kokoro (server on 9002+call)
    char buf[256];
    while (registration_running_.load() && running_.load() && active_.load() && !piper_connected_.load()) {
        struct timeval tv{1,0};
        setsockopt(udp_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        struct sockaddr_in src{}; socklen_t slen = sizeof(src);
        ssize_t n = recvfrom(udp_sock, buf, sizeof(buf)-1, 0, (struct sockaddr*)&src, &slen);
        if (n <= 0) continue;
        buf[n] = '\0';
        std::string msg(buf);
        if (msg.rfind("REGISTER:", 0) != 0) continue;
        std::string received = msg.substr(9);
        if (received != call_id) continue;

        // Connect to Kokoro server at 127.0.0.1:piper_tcp_port_
        int s = -1;
        for (int attempt = 1; attempt <= 10 && registration_running_.load() && running_.load() && active_.load(); ++attempt) {
            s = socket(AF_INET, SOCK_STREAM, 0);
            if (s < 0) break;
            struct sockaddr_in dst{};
            dst.sin_family = AF_INET;
            dst.sin_addr.s_addr = inet_addr("127.0.0.1");
            dst.sin_port = htons(piper_tcp_port_);
            if (connect(s, (struct sockaddr*)&dst, sizeof(dst)) == 0) break;
            close(s); s = -1;
            int sleep_ms = (attempt <= 5) ? 200 : 1000;
            if (attempt == 1 || attempt == 5 || attempt == 9) {
                std::cout << "⚠️ Kokoro connect attempt " << attempt << "/10 failed — retrying in " << sleep_ms << "ms" << std::endl;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
        }
        if (s < 0) {
            std::cout << "❌ Failed to connect to Kokoro server on port " << piper_tcp_port_ << std::endl;
            continue;
        }

        // Send HELLO(call_id)
        uint32_t nlen = htonl(static_cast<uint32_t>(call_id.size()));
        if (write(s, &nlen, 4) != 4 || write(s, call_id.data(), call_id.size()) != (ssize_t)call_id.size()) {
            std::cout << "❌ Failed to send HELLO to Kokoro" << std::endl;
            close(s);
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(piper_mutex_);
            piper_tcp_socket_ = s;
            piper_connected_.store(true);
        }
        std::cout << "🔗 Connected to Kokoro on port " << piper_tcp_port_ << " for call " << call_id << std::endl;

        // Read incoming audio stream until disconnect
        while (running_.load() && piper_connected_.load()) {
            uint32_t chunk_length = 0, sample_rate = 0, chunk_id = 0;
            if (!read_exact_from_socket(s, &chunk_length, 4) ||
                !read_exact_from_socket(s, &sample_rate, 4) ||
                !read_exact_from_socket(s, &chunk_id, 4)) {
                break;
            }
            chunk_length = ntohl(chunk_length);
            sample_rate = ntohl(sample_rate);
            chunk_id = ntohl(chunk_id);
            if (chunk_length == 0) {
                std::cout << "📡 TCP BYE received from Kokoro" << std::endl;
                break;
            }
            if (chunk_length > 10 * 1024 * 1024) {
                std::cout << "⚠️ Kokoro chunk too large (" << chunk_length << ") — dropping" << std::endl;
                break;
            }
            std::vector<uint8_t> payload(chunk_length);
            if (!read_exact_from_socket(s, payload.data(), chunk_length)) break;
            process_piper_audio_chunk(payload, sample_rate, chunk_id);
        }

        close(s);
        {
            std::lock_guard<std::mutex> lock(piper_mutex_);
            piper_tcp_socket_ = -1;
        }
        piper_connected_.store(false);
        std::cout << "🔌 Disconnected from Kokoro" << std::endl;
    }

    close(udp_sock);
    std::cout << "🛑 Registration listener stopped for call " << call_id << std::endl;
}

// Unused functions removed for performance optimization
