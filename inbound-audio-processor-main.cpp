#include "inbound-audio-processor.h"
#include "rtp-packet.h"
#include <iostream>
#include <signal.h>
#include <thread>
#include <chrono>
#include <memory>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <atomic>
#include <vector>

std::atomic<bool> g_running{true};
std::unique_ptr<InboundAudioProcessor> g_processor;

static void signal_handler(int sig) {
    std::cout << "\n🛑 Signal received (" << sig << "), shutting down..." << std::endl;
    g_running = false;
}

static void setup_signal_handlers() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
}

static void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "Options:\n"
              << "  --port PORT        UDP port to listen for RTP packets (default: 9001)\n"
              << "  --help            Show this help message\n";
}

int main(int argc, char* argv[]) {
    int listen_port = 9001;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--port" && i + 1 < argc) {
            listen_port = std::stoi(argv[++i]);
        } else {
            std::cerr << "❌ Unknown argument: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    setup_signal_handlers();

    std::cout << "🎤 Starting Standalone Inbound Audio Processor..." << std::endl;
    std::cout << "📡 Listening for RTP on UDP port: " << listen_port << std::endl;

    g_processor = std::make_unique<InboundAudioProcessor>();
    if (!g_processor->start(8083)) { // Base port for Whisper TCP is still needed
        std::cerr << "❌ Failed to start inbound audio processor" << std::endl;
        return 1;
    }

    // Create UDP socket
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(listen_port);

    if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sock);
        return 1;
    }

    // Set timeout for recvfrom to check g_running
    struct timeval tv;
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char buffer[2048];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    std::cout << "🚀 Ready to process audio packets" << std::endl;

    while (g_running) {
        ssize_t received = recvfrom(sock, buffer, sizeof(buffer), 0, (struct sockaddr*)&client_addr, &client_len);
        if (received > 0) {
            // Assume the first 4 bytes are the call_id as uint32_t, followed by RTP packet
            if (received < 4 + 12) continue; // Minimum size: call_id + RTP header

            uint32_t call_id = ntohl(*reinterpret_cast<uint32_t*>(buffer));
            std::string call_id_str = std::to_string(call_id);

            // Ensure call is active in processor
            if (!g_processor->is_call_active(call_id_str)) {
                g_processor->activate_for_call(call_id_str);
            }

            // Parse RTP packet
            RTPAudioPacket rtp_packet;
            // The existing RTPAudioPacket constructor or parse method might need to be used
            // Looking at the code, it seems RTPAudioPacket is a simple wrapper.
            // Let's use the raw data skipping the call_id.
            
            // Re-verify RTPAudioPacket structure
            // In sip-client-main.cpp:852: RTPAudioPacket packet(payload_type, audio_payload, timestamp, sequence);
            
            // Let's parse the RTP header properly
            uint8_t* rtp_data = (uint8_t*)buffer + 4;
            size_t rtp_len = received - 4;
            
            if (rtp_len >= 12) {
                uint8_t payload_type = rtp_data[1] & 0x7F;
                uint16_t sequence = (rtp_data[2] << 8) | rtp_data[3];
                uint32_t timestamp = (rtp_data[4] << 24) | (rtp_data[5] << 16) | (rtp_data[6] << 8) | rtp_data[7];
                std::vector<uint8_t> payload(rtp_data + 12, rtp_data + rtp_len);
                
                RTPAudioPacket pkt(payload_type, payload, timestamp, sequence);
                g_processor->process_rtp_audio(call_id_str, pkt);
            }
        }
    }

    g_processor->stop();
    close(sock);
    std::cout << "🛑 Inbound Audio Processor stopped cleanly" << std::endl;

    return 0;
}
