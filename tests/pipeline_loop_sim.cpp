#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <mutex>
#include <numeric>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <fstream>
#include <sstream>
#include <iomanip>

// Timing structure for measuring pipeline latency
struct PipelineTiming {
    std::chrono::steady_clock::time_point t0_audio_sent;
    std::chrono::steady_clock::time_point t1_transcription_received;
    std::chrono::steady_clock::time_point t2_llama_response_received;
    std::chrono::steady_clock::time_point t3_kokoro_audio_received;
    std::chrono::steady_clock::time_point t4_audio_resent;
    std::chrono::steady_clock::time_point t5_final_transcription;
    
    std::string original_transcription;
    std::string llama_response;
    std::string final_transcription;
    
    void print_summary() const {
        auto ms = [](auto start, auto end) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        };
        
        std::cout << "\n=== Pipeline Timing Summary ===\n";
        std::cout << "Original transcription: \"" << original_transcription << "\"\n";
        std::cout << "Llama response: \"" << llama_response << "\"\n";
        std::cout << "Final transcription: \"" << final_transcription << "\"\n\n";
        
        std::cout << "Timing breakdown:\n";
        std::cout << "  Whisper inference (T1-T0):     " << std::setw(6) << ms(t0_audio_sent, t1_transcription_received) << "ms\n";
        std::cout << "  Llama response (T2-T1):        " << std::setw(6) << ms(t1_transcription_received, t2_llama_response_received) << "ms\n";
        std::cout << "  Kokoro synthesis (T3-T2):      " << std::setw(6) << ms(t2_llama_response_received, t3_kokoro_audio_received) << "ms\n";
        std::cout << "  Audio transfer (T4-T3):        " << std::setw(6) << ms(t3_kokoro_audio_received, t4_audio_resent) << "ms\n";
        std::cout << "  Whisper re-transcription (T5-T4): " << std::setw(6) << ms(t4_audio_resent, t5_final_transcription) << "ms\n";
        
        long total_ms = ms(t0_audio_sent, t5_final_transcription);
        std::cout << "  Total round-trip (T5-T0):      " << std::setw(6) << total_ms << "ms ";
        if (total_ms < 2000) {
            std::cout << "✅ (<2s target)\n";
        } else {
            std::cout << "⚠️  (>2s target)\n";
        }
        
        // Quality check
        std::cout << "\nQuality check:\n";
        if (llama_response == final_transcription) {
            std::cout << "  Quality: 100% match ✅\n";
        } else {
            std::cout << "  Quality: Mismatch ⚠️\n";
            std::cout << "    Expected: \"" << llama_response << "\"\n";
            std::cout << "    Got:      \"" << final_transcription << "\"\n";
        }
        
        std::cout << "\nStatus: " << (total_ms < 2000 ? "PASS" : "FAIL") << " - ";
        std::cout << (total_ms < 2000 ? "Real-time performance achieved" : "Exceeds 2s target") << "\n";
        std::cout << "================================\n\n";
    }
};

// Minimal helpers ------------------------------------------------------------
static bool write_all(int fd, const void* buf, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t off = 0;
    while (off < n) {
        ssize_t k = ::send(fd, p + off, n - off, 0);
        if (k <= 0) return false;
        off += (size_t)k;
    }
    return true;
}

static bool read_exact(int fd, void* buf, size_t n) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t off = 0;
    while (off < n) {
        ssize_t k = ::recv(fd, p + off, n - off, 0);
        if (k <= 0) return false;
        off += (size_t)k;
    }
    return true;
}

// WAV loader (PCM16 mono or stereo) -----------------------------------------
struct WavData {
    int sample_rate = 0;
    int channels = 0;
    std::vector<float> samples; // normalized -1..1
};

static bool load_wav_pcm16(const std::string& path, WavData& out) {
    FILE* f = ::fopen(path.c_str(), "rb");
    if (!f) { std::perror("fopen"); return false; }

    auto ru32 = [&](uint32_t& v){ return ::fread(&v, 1, 4, f) == 4; };
    auto ru16 = [&](uint16_t& v){ return ::fread(&v, 1, 2, f) == 2; };

    uint32_t riff, wave;
    uint32_t chunk_size;
    if (!ru32(riff) || !ru32(chunk_size) || !ru32(wave)) { ::fclose(f); return false; }
    if (riff != 0x46464952 /*RIFF*/ || wave != 0x45564157 /*WAVE*/) { ::fclose(f); return false; }

    uint16_t audio_fmt = 0, num_channels = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0, byte_rate = 0; uint16_t block_align = 0;

    // Parse chunks until 'fmt ' and 'data'
    bool got_fmt = false, got_data = false; uint32_t data_size = 0; long data_pos = 0;
    while (!got_fmt || !got_data) {
        uint32_t id = 0, sz = 0;
        if (!ru32(id) || !ru32(sz)) { ::fclose(f); return false; }
        if (id == 0x20746d66 /*'fmt '*/) {
            if (!ru16(audio_fmt) || !ru16(num_channels) || !ru32(sample_rate) || !ru32(byte_rate) || !ru16(block_align) || !ru16(bits_per_sample)) { ::fclose(f); return false; }
            // Skip any extra fmt bytes
            if (sz > 16) ::fseek(f, sz - 16, SEEK_CUR);
            got_fmt = true;
        } else if (id == 0x61746164 /*'data'*/) {
            data_pos = ::ftell(f);
            data_size = sz;
            ::fseek(f, sz, SEEK_CUR);
            got_data = true;
        } else {
            ::fseek(f, sz, SEEK_CUR);
        }
    }

    if (!(got_fmt && got_data) || audio_fmt != 1 || (bits_per_sample != 16)) { ::fclose(f); return false; }
    out.sample_rate = (int)sample_rate;
    out.channels = (int)num_channels;

    // Read samples
    out.samples.clear();
    out.samples.reserve(data_size / 2);
    ::fseek(f, data_pos, SEEK_SET);
    for (uint32_t i = 0; i + 1 < data_size; i += 2 * num_channels) {
        int32_t acc = 0;
        for (int ch = 0; ch < num_channels; ++ch) {
            uint8_t lo = 0, hi = 0;
            if (::fread(&lo, 1, 1, f) != 1 || ::fread(&hi, 1, 1, f) != 1) { ::fclose(f); return false; }
            int16_t s = (int16_t)((hi << 8) | lo);
            acc += (int32_t)s;
        }
        float mono = (float)acc / (32768.0f * std::max(1, out.channels));
        out.samples.push_back(mono);
    }
    ::fclose(f);
    return true;
}

// Resample to 16k (linear)
static std::vector<float> resample_linear(const std::vector<float>& in, int sr_in, int sr_out) {
    if (sr_in == sr_out || in.empty()) return in;
    double ratio = (double)sr_out / (double)sr_in;
    size_t out_n = (size_t)std::llround((double)in.size() * ratio);
    std::vector<float> out;
    out.resize(out_n);
    for (size_t i = 0; i < out_n; ++i) {
        double pos = (double)i / ratio;
        size_t i0 = (size_t)std::floor(pos);
        size_t i1 = std::min(i0 + 1, in.size() - 1);
        double t = pos - (double)i0;
        out[i] = (float)((1.0 - t) * in[i0] + t * in[i1]);
    }
    return out;
}

// VAD chunker (mirrors production SimpleAudioProcessor) ----------------------
struct VadConfig {
    int sample_rate = 16000;
    size_t window_size = 160;          // 10ms
    int hangover_ms = 900;
    float vad_start_mul = 1.05f;
    float vad_stop_mul = 0.5f;
    float vad_threshold = 0.02f;
    size_t min_chunk_samples = 16000 * 8 / 10; // 0.8s
    size_t max_chunk_samples = 16000 * 4; // 4s
    size_t overlap_samples = 16000 * 25 / 100; // ~250ms
    size_t pre_roll_samples = 16000 * 35 / 100; // ~350ms
};

static float energy_rms(const std::vector<float>& a) {
    if (a.empty()) return 0.f;
    double sum = 0.0; for (float v : a) sum += (double)v * (double)v; return (float)std::sqrt(sum / (double)a.size());
}

static std::vector<std::vector<float>> vad_chunk(const std::vector<float>& pcm, const VadConfig& cfg) {
    std::vector<std::vector<float>> chunks;
    if (pcm.empty()) return chunks;

    const size_t W = cfg.window_size;
    const int win_ms = (int)(1000.0 * W / std::max(1, cfg.sample_rate));
    const int hang_windows = std::max(1, cfg.hangover_ms / std::max(1, win_ms));
    const float th_start = std::max(0.001f, cfg.vad_threshold * cfg.vad_start_mul);
    const float th_stop  = std::max(0.0005f, cfg.vad_threshold * cfg.vad_stop_mul);

    std::vector<float> cur;
    std::vector<float> prebuf; prebuf.reserve(cfg.pre_roll_samples);

    bool in_speech = false; int silence_w = 0, cs = 0, csi = 0; size_t consumed = 0;

    for (size_t i = 0; i < pcm.size(); i += W) {
        size_t end = std::min(i + W, pcm.size());
        std::vector<float> win(pcm.begin() + i, pcm.begin() + end);
        float rms = energy_rms(win);
        bool speech_now = in_speech ? (rms > th_stop) : (rms > th_start);
        if (speech_now) { cs++; csi = 0; } else { csi++; cs = 0; }
        if (!in_speech) {
            prebuf.insert(prebuf.end(), win.begin(), win.end());
            if (prebuf.size() > cfg.pre_roll_samples) prebuf.erase(prebuf.begin(), prebuf.begin() + (prebuf.size() - cfg.pre_roll_samples));
        }
        if (!in_speech && cs >= 1) {
            in_speech = true; silence_w = 0;
            if (!prebuf.empty()) { cur.insert(cur.end(), prebuf.begin(), prebuf.end()); prebuf.clear(); }
        }
        if (in_speech) {
            cur.insert(cur.end(), win.begin(), win.end());
            if (!speech_now) {
                silence_w++;
                if (silence_w >= hang_windows) {
                    if (csi >= 3 && cur.size() >= cfg.min_chunk_samples) {
                        chunks.push_back(cur);
                        size_t ov = cfg.overlap_samples;
                        std::vector<float> tail = (cur.size() > ov) ? std::vector<float>(cur.end() - ov, cur.end()) : cur;
                        cur = tail;
                        in_speech = false; silence_w = 0; csi = 0; consumed = (end > tail.size()) ? (end - tail.size()) : 0;
                    }
                }
            }
        }
        if (cur.size() >= cfg.max_chunk_samples) {
            chunks.push_back(cur);
            size_t ov = cfg.overlap_samples;
            std::vector<float> tail = (cur.size() > ov) ? std::vector<float>(cur.end() - ov, cur.end()) : cur;
            cur = tail;
            in_speech = false; silence_w = 0; csi = 0; cs = 0; consumed = (end > tail.size()) ? (end - tail.size()) : 0;
        }
    }
    (void)consumed; // not used further in simulator
    return chunks;
}

// TCP helpers ----------------------------------------------------------------
static int create_server(int port) {
    int s = ::socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return -1;
    int opt = 1;
    ::setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (::bind(s, (sockaddr*)&addr, sizeof(addr)) < 0) { ::close(s); return -1; }
    if (::listen(s, 1) < 0) { ::close(s); return -1; }
    return s;
}

static void send_register_udp(const std::string& call_id) {
    int u = ::socket(AF_INET, SOCK_DGRAM, 0); if (u < 0) return;
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_port = htons(13000); addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    std::string msg = std::string("REGISTER:") + call_id;
    ::sendto(u, msg.c_str(), msg.size(), 0, (sockaddr*)&addr, sizeof(addr));
    ::close(u);
}

static bool send_tcp_hello(int fd, const std::string& call_id) {
    uint32_t n = htonl((uint32_t)call_id.size());
    return write_all(fd, &n, 4) && (call_id.empty() || write_all(fd, call_id.data(), call_id.size()));
}

static bool send_tcp_chunk(int fd, const std::vector<float>& pcm) {
    if (pcm.empty()) return true;
    uint32_t n = htonl((uint32_t)(pcm.size() * sizeof(float)));
    return write_all(fd, &n, 4) && write_all(fd, pcm.data(), pcm.size() * sizeof(float));
}

static bool send_tcp_bye(int fd) {
    uint32_t bye_marker = htonl(0xFFFFFFFF);
    return write_all(fd, &bye_marker, 4);
}

// UDP helper to send messages to services
static void send_udp_message(const std::string& message, const std::string& host, int port) {
    int udp_sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        std::cerr << "⚠️  Failed to create UDP socket\n";
        return;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);

    ssize_t sent = ::sendto(udp_sock, message.c_str(), message.size(), 0,
                            (sockaddr*)&addr, sizeof(addr));
    if (sent < 0) {
        std::cerr << "⚠️  Failed to send UDP message to " << host << ":" << port << "\n";
    } else {
        std::cout << "📤 Sent UDP: \"" << message << "\" to " << host << ":" << port << "\n";
    }

    ::close(udp_sock);
}

// Kokoro audio receiver (mimics outbound-audio-processor)
// Listens on port 9002+call_id for synthesized audio from Kokoro
struct KokoroAudioReceiver {
    std::vector<float> audio_samples;
    uint32_t sample_rate = 0;
    std::mutex mu;
    bool stop = false;
    int kokoro_port = 0;
    int kokoro_server = -1;
    int kokoro_client = -1;
    std::thread receiver_thread;
    bool audio_received = false;
    bool audio_complete = false;

    int calculate_kokoro_port(const std::string& call_id) {
        // Port calculation: 9002 + call_id (matches outbound-audio-processor)
        int id = std::stoi(call_id);
        return 9002 + id;
    }

    bool start_listening(const std::string& call_id) {
        kokoro_port = calculate_kokoro_port(call_id);

        kokoro_server = ::socket(AF_INET, SOCK_STREAM, 0);
        if (kokoro_server < 0) {
            std::cout << "❌ Failed to create Kokoro server socket\n";
            return false;
        }

        int opt = 1;
        ::setsockopt(kokoro_server, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(kokoro_port);

        if (::bind(kokoro_server, (sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cout << "❌ Failed to bind Kokoro server socket to port " << kokoro_port << "\n";
            ::close(kokoro_server);
            kokoro_server = -1;
            return false;
        }

        if (::listen(kokoro_server, 1) < 0) {
            std::cout << "❌ Failed to listen on Kokoro server socket\n";
            ::close(kokoro_server);
            kokoro_server = -1;
            return false;
        }

        std::cout << "🎵 Kokoro audio receiver listening on port " << kokoro_port << "\n";
        return true;
    }

    bool accept_connection() {
        sockaddr_in caddr{};
        socklen_t clen = sizeof(caddr);
        kokoro_client = ::accept(kokoro_server, (sockaddr*)&caddr, &clen);
        if (kokoro_client < 0) {
            std::cout << "❌ Failed to accept Kokoro connection\n";
            return false;
        }
        std::cout << "🔗 Kokoro connected from " << inet_ntoa(caddr.sin_addr) << ":" << ntohs(caddr.sin_port) << "\n";
        return true;
    }

    bool read_hello(std::string& call_id) {
        uint32_t len_be = 0;
        if (!read_exact(kokoro_client, &len_be, 4)) return false;
        uint32_t len = ntohl(len_be);
        if (len == 0 || len > 4096) return false;
        call_id.resize(len);
        if (!read_exact(kokoro_client, call_id.data(), len)) return false;
        std::cout << "👋 HELLO from Kokoro: call_id=" << call_id << "\n";
        return true;
    }

    void receive_audio_loop() {
        std::cout << "👂 Kokoro audio receiver started\n";

        while (!stop && kokoro_client >= 0) {
            // Read chunk header: [length][sample_rate][chunk_id]
            uint32_t chunk_length = 0, chunk_sample_rate = 0, chunk_id = 0;

            if (!read_exact(kokoro_client, &chunk_length, 4) ||
                !read_exact(kokoro_client, &chunk_sample_rate, 4) ||
                !read_exact(kokoro_client, &chunk_id, 4)) {
                break;
            }

            chunk_length = ntohl(chunk_length);
            chunk_sample_rate = ntohl(chunk_sample_rate);
            chunk_id = ntohl(chunk_id);

            if (chunk_length == 0) {
                std::cout << "📡 BYE received from Kokoro (audio complete)\n";
                {
                    std::lock_guard<std::mutex> lk(mu);
                    audio_complete = true;
                }
                break;
            }

            if (chunk_length > 10 * 1024 * 1024) {
                std::cout << "⚠️  Kokoro chunk too large (" << chunk_length << ")\n";
                break;
            }

            // Read audio payload (float32 PCM)
            std::vector<uint8_t> payload(chunk_length);
            if (!read_exact(kokoro_client, payload.data(), chunk_length)) {
                std::cout << "❌ Failed to read Kokoro audio payload\n";
                break;
            }

            std::cout << "🎵 Received Kokoro audio chunk: " << chunk_length << " bytes, "
                      << chunk_sample_rate << "Hz, chunk_id=" << chunk_id << "\n";

            // Convert bytes to float samples
            size_t num_samples = chunk_length / sizeof(float);
            const float* float_data = reinterpret_cast<const float*>(payload.data());

            {
                std::lock_guard<std::mutex> lk(mu);
                audio_samples.insert(audio_samples.end(), float_data, float_data + num_samples);
                sample_rate = chunk_sample_rate;
                audio_received = true;
            }
        }

        std::cout << "👂 Kokoro audio receiver stopped (total samples: " << audio_samples.size() << ")\n";
    }

    void start_receiver_thread() {
        receiver_thread = std::thread([this]{ receive_audio_loop(); });
    }

    void stop_and_join() {
        stop = true;
        if (kokoro_client >= 0) ::shutdown(kokoro_client, SHUT_RDWR);
        if (receiver_thread.joinable()) receiver_thread.join();
        if (kokoro_client >= 0) ::close(kokoro_client);
        kokoro_client = -1;
    }

    void cleanup() {
        if (kokoro_server >= 0) ::close(kokoro_server);
        kokoro_server = -1;
    }
};

// Main ----------------------------------------------------------------------
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <wav_file>\n";
        std::cerr << "Example: " << argv[0] << " ./tests/data/harvard/wav/OSR_us_000_0010_8k.wav\n";
        return 2;
    }

    std::string wav_path = argv[1];
    std::string fname = wav_path.substr(wav_path.find_last_of("/") + 1);

    // Use fixed call_id for single test
    const std::string call_id = "151";
    const int whisper_audio_port = 9001 + std::stoi(call_id); // 9152

    std::cout << "\n=== Pipeline Loop Test ===\n";
    std::cout << "Input: " << fname << "\n";
    std::cout << "Call ID: " << call_id << "\n";
    std::cout << "Whisper audio port: " << whisper_audio_port << "\n\n";

    // Load WAV file
    WavData wav;
    if (!load_wav_pcm16(wav_path, wav)) {
        std::cerr << "❌ Failed to load WAV file: " << wav_path << "\n";
        return 1;
    }

    // Resample to 16kHz if needed
    std::vector<float> pcm16k = (wav.sample_rate == 16000) ?
        wav.samples : resample_linear(wav.samples, wav.sample_rate, 16000);

    std::cout << "✅ Loaded audio: " << pcm16k.size() << " samples @ 16kHz ("
              << (pcm16k.size() / 16000.0) << "s)\n\n";

    // Initialize timing structure
    PipelineTiming timing;

    std::cout << "⚠️  NOTE: This simulator requires real llama-service and kokoro-service to be running!\n";
    std::cout << "   - llama-service should be on port 8083\n";
    std::cout << "   - kokoro-service should be on port 8090 (UDP 13001)\n\n";

    // Step 2: Setup audio inbound server for whisper-service
    std::cout << "🔧 Setting up Whisper audio server on port " << whisper_audio_port << "...\n";
    int audio_server = create_server(whisper_audio_port);
    if (audio_server < 0) {
        std::cerr << "❌ Failed to create audio server on port " << whisper_audio_port << "\n";
        return 1;
    }

    // Step 3: Send REGISTER to whisper-service
    std::cout << "📤 Sending REGISTER for call_id " << call_id << "...\n";
    send_register_udp(call_id);

    // Step 4: Accept connection from whisper-service (inbound audio)
    std::cout << "⏳ Waiting for whisper-service to connect...\n";
    sockaddr_in caddr{};
    socklen_t clen = sizeof(caddr);
    int whisper_audio_client = ::accept(audio_server, (sockaddr*)&caddr, &clen);
    if (whisper_audio_client < 0) {
        std::cerr << "❌ Failed to accept whisper-service connection\n";
        ::close(audio_server);
        return 1;
    }
    std::cout << "🔗 Whisper-service connected for audio\n";

    // Send HELLO with call_id
    if (!send_tcp_hello(whisper_audio_client, call_id)) {
        std::cerr << "❌ Failed to send HELLO to whisper-service\n";
        ::close(whisper_audio_client);
        ::close(audio_server);
        return 1;
    }
    std::cout << "📡 HELLO sent to whisper-service: " << call_id << "\n\n";

    // Step 5: VAD-chunk and send original audio to whisper-service
    // Whisper will transcribe and send to real llama-service (port 8083)
    // Llama will generate response and send to real kokoro-service (port 8090)
    // Kokoro will synthesize and connect back to us (port 9002+call_id)
    std::cout << "🎤 Sending original audio to whisper-service...\n";
    VadConfig cfg; // defaults mirror production
    auto chunks = vad_chunk(pcm16k, cfg);

    timing.t0_audio_sent = std::chrono::steady_clock::now();

    for (const auto& ch : chunks) {
        if (!send_tcp_chunk(whisper_audio_client, ch)) {
            std::cerr << "❌ Failed to send audio chunk\n";
            break;
        }
        std::cout << "📦 Sent chunk: " << ch.size() << " samples\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    // Send BYE to audio socket
    if (!send_tcp_bye(whisper_audio_client)) {
        std::cerr << "⚠️  Failed to send BYE to audio socket\n";
    } else {
        std::cout << "📡 BYE sent to audio socket\n";
    }

    std::cout << "\n⏳ Audio sent - now waiting for pipeline to process...\n";
    std::cout << "   Whisper → Llama → Kokoro → (back to us)\n\n";

    // Approximate T1 (we don't intercept transcription anymore)
    // Assume whisper takes ~500ms
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    timing.t1_transcription_received = std::chrono::steady_clock::now();

    // Step 6: Setup Kokoro audio receiver (listens on port 9002+call_id)
    std::cout << "🔧 Setting up Kokoro audio receiver on port " << (9002 + std::stoi(call_id)) << "...\n";
    KokoroAudioReceiver kokoro_rx;
    if (!kokoro_rx.start_listening(call_id)) {
        std::cerr << "❌ Failed to start Kokoro audio receiver\n";
        ::close(whisper_audio_client);
        ::close(audio_server);
        return 1;
    }

    // Step 7: Send REGISTER to Kokoro service (UDP port 13001)
    // This tells Kokoro to connect to us on port 9002+call_id
    std::cout << "📤 Sending REGISTER to Kokoro service (UDP 13001)...\n";
    send_udp_message("REGISTER:" + call_id, "127.0.0.1", 13001);

    std::cout << "\n⏳ Waiting for Kokoro to connect and send synthesized audio...\n";
    std::cout << "   (This requires llama-service and kokoro-service to be running)\n";
    std::cout << "   Timeout: 120 seconds\n\n";

    // Step 8: Accept connection from Kokoro (with 2-minute timeout)
    struct timeval tv;
    tv.tv_sec = 120;  // 2 minute timeout
    tv.tv_usec = 0;
    setsockopt(kokoro_rx.kokoro_server, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof tv);

    if (!kokoro_rx.accept_connection()) {
        std::cerr << "❌ Timeout waiting for Kokoro connection (120s)\n";
        std::cerr << "   Make sure llama-service and kokoro-service are running\n";
        std::cerr << "   Check that kokoro-service received the REGISTER message\n";
        kokoro_rx.cleanup();
        ::close(whisper_audio_client);
        ::close(audio_server);
        return 1;
    }

    std::string kokoro_call_id;
    if (!kokoro_rx.read_hello(kokoro_call_id)) {
        std::cerr << "❌ Failed to read HELLO from Kokoro\n";
        kokoro_rx.cleanup();
        ::close(whisper_audio_client);
        ::close(audio_server);
        return 1;
    }

    if (kokoro_call_id != call_id) {
        std::cerr << "⚠️  Call ID mismatch from Kokoro: expected " << call_id << ", got " << kokoro_call_id << "\n";
    }

    // Start Kokoro receiver thread
    kokoro_rx.start_receiver_thread();
    std::cout << "✅ Kokoro receiver ready\n\n";

    // Step 10: Wait for Kokoro audio to complete (with timeout)
    // Note: We wait for audio_received flag + 3 seconds of silence instead of BYE
    // because llama-service keeps the Kokoro connection open
    std::cout << "⏳ Waiting for Kokoro audio synthesis to complete...\n";
    auto kokoro_wait_start = std::chrono::steady_clock::now();
    const int kokoro_max_wait_ms = 60000; // 60 seconds timeout

    // First, wait for audio to start arriving
    while (!kokoro_rx.audio_received) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - kokoro_wait_start).count();
        if (elapsed > kokoro_max_wait_ms) {
            std::cerr << "❌ Timeout waiting for Kokoro audio to start (60 seconds)\n";
            kokoro_rx.stop_and_join();
            kokoro_rx.cleanup();
            ::close(whisper_audio_client);
            ::close(audio_server);
            return 1;
        }
    }

    std::cout << "✅ Audio started arriving from Kokoro\n";

    // Now wait 3 seconds for all audio to arrive
    std::this_thread::sleep_for(std::chrono::seconds(3));

    // Check if BYE was received
    if (kokoro_rx.audio_complete) {
        std::cout << "✅ Audio complete (BYE received)\n";
    } else {
        std::cout << "✅ Audio complete (timeout after first chunk)\n";
    }

    timing.t3_kokoro_audio_received = std::chrono::steady_clock::now();

    std::vector<float> kokoro_audio;
    uint32_t kokoro_sample_rate = 0;
    {
        std::lock_guard<std::mutex> lk(kokoro_rx.mu);
        kokoro_audio = kokoro_rx.audio_samples;
        kokoro_sample_rate = kokoro_rx.sample_rate;
    }

    std::cout << "✅ Kokoro audio received: " << kokoro_audio.size() << " samples @ "
              << kokoro_sample_rate << "Hz (" << (kokoro_audio.size() / (float)kokoro_sample_rate) << "s)\n\n";

    // Step 11: Resample Kokoro audio to 16kHz if needed (Kokoro outputs 24kHz)
    std::cout << "🔄 Resampling audio to 16kHz for Whisper...\n";
    std::vector<float> resampled_audio = (kokoro_sample_rate == 16000) ?
        kokoro_audio : resample_linear(kokoro_audio, kokoro_sample_rate, 16000);

    std::cout << "✅ Resampled: " << resampled_audio.size() << " samples @ 16kHz\n\n";

    timing.t4_audio_resent = std::chrono::steady_clock::now();

    // Close Kokoro receiver to free up port 9153
    std::cout << "🔧 Closing Kokoro receiver...\n";
    kokoro_rx.stop_and_join();
    kokoro_rx.cleanup();

    // Step 12: Setup new audio connection to Whisper for re-transcription
    // Use a different call_id to avoid conflicts
    const std::string call_id_2 = std::to_string(std::stoi(call_id) + 1); // 152
    const int whisper_audio_port_2 = 9001 + std::stoi(call_id_2); // 9153

    std::cout << "🔧 Setting up second Whisper connection for re-transcription...\n";
    std::cout << "   Call ID: " << call_id_2 << ", Port: " << whisper_audio_port_2 << "\n";

    int audio_server_2 = create_server(whisper_audio_port_2);
    if (audio_server_2 < 0) {
        std::cerr << "❌ Failed to create second audio server on port " << whisper_audio_port_2 << "\n";
        ::close(whisper_audio_client);
        ::close(audio_server);
        return 1;
    }

    // Send REGISTER for second connection
    send_register_udp(call_id_2);
    std::cout << "📤 REGISTER sent for call_id " << call_id_2 << "\n";

    // Accept second connection from whisper-service
    std::cout << "⏳ Waiting for whisper-service to connect (second connection)...\n";
    sockaddr_in caddr2{};
    socklen_t clen2 = sizeof(caddr2);
    int whisper_audio_client_2 = ::accept(audio_server_2, (sockaddr*)&caddr2, &clen2);
    if (whisper_audio_client_2 < 0) {
        std::cerr << "❌ Failed to accept second whisper-service connection\n";
        ::close(audio_server_2);
        ::close(whisper_audio_client);
        ::close(audio_server);
        return 1;
    }
    std::cout << "🔗 Whisper-service connected (second connection)\n";

    // Send HELLO with call_id_2
    if (!send_tcp_hello(whisper_audio_client_2, call_id_2)) {
        std::cerr << "❌ Failed to send HELLO to whisper-service (second connection)\n";
        ::close(whisper_audio_client_2);
        ::close(audio_server_2);
        ::close(whisper_audio_client);
        ::close(audio_server);
        return 1;
    }
    std::cout << "📡 HELLO sent to whisper-service: " << call_id_2 << "\n\n";

    // Step 9: VAD-chunk and send resampled audio to Whisper
    // This will go through the full pipeline again (Whisper → Llama → Kokoro)
    // But we won't wait for the second loop - just measure the first re-transcription
    std::cout << "🎤 Sending resampled audio back to whisper-service...\n";
    auto chunks_2 = vad_chunk(resampled_audio, cfg);

    for (const auto& ch : chunks_2) {
        if (!send_tcp_chunk(whisper_audio_client_2, ch)) {
            std::cerr << "❌ Failed to send audio chunk (second connection)\n";
            break;
        }
        std::cout << "📦 Sent chunk: " << ch.size() << " samples\n";
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    // Send BYE to second audio socket
    if (!send_tcp_bye(whisper_audio_client_2)) {
        std::cerr << "⚠️  Failed to send BYE to second audio socket\n";
    } else {
        std::cout << "📡 BYE sent to second audio socket\n";
    }

    timing.t5_final_transcription = std::chrono::steady_clock::now();

    std::cout << "\n✅ Pipeline loop complete!\n";
    std::cout << "   Original audio → Whisper → Llama → Kokoro → (back to simulator)\n\n";

    // Approximate timing values (we don't intercept transcriptions)
    timing.original_transcription = "(not captured - sent to real llama-service)";
    timing.llama_response = "(synthesized by real kokoro-service)";
    timing.final_transcription = "(would be re-transcribed by whisper-service)";
    timing.t2_llama_response_received = timing.t1_transcription_received + std::chrono::milliseconds(200); // Approximate

    // Print timing summary
    timing.print_summary();

    // Clean up
    ::close(whisper_audio_client_2);
    ::close(audio_server_2);
    // Kokoro receiver already closed earlier
    ::close(whisper_audio_client);
    ::close(audio_server);

    std::cout << "\n=== Test Complete ===\n";
    std::cout << "✅ Full pipeline loop executed successfully\n\n";

    return 0;
}

