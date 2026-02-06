#include "outbound-audio-processor.h"
#include <filesystem>
#include <fstream>
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

std::atomic<bool> g_running{true};
std::unique_ptr<OutboundAudioProcessor> g_processor;

void signal_handler(int sig) {
    std::cout << "\n🛑 Signal received (" << sig << "), shutting down..." << std::endl;
    g_running = false;
}

void setup_signal_handlers() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
}

static std::string get_executable_dir(const char* argv0) {
    try {
        std::filesystem::path p = std::filesystem::canonical(argv0);
        return p.parent_path().string();
    } catch (...) {
        try { return std::filesystem::current_path().string(); } catch (...) { return std::string("."); }
    }
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "Options:\n"
              << "  --port PORT        Base port for Piper TCP (default: 8183)\n"
              << "  --dest-ip IP       UDP destination IP for RTP (default: 127.0.0.1)\n"
              << "  --dest-port PORT   UDP destination port for RTP (default: 10000)\n"
              << "  --call-id ID       Numeric call_id (default: 1)\n"
              << "  --help            Show this help message\n";
}

int main(int argc, char* argv[]) {
    int base_port = 8183;
    std::string dest_ip = "127.0.0.1";
    int dest_port = 10000;
    int call_id = 1;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--port" && i + 1 < argc) {
            base_port = std::stoi(argv[++i]);
        } else if (arg == "--dest-ip" && i + 1 < argc) {
            dest_ip = argv[++i];
        } else if (arg == "--dest-port" && i + 1 < argc) {
            dest_port = std::stoi(argv[++i]);
        } else if (arg == "--call-id" && i + 1 < argc) {
            call_id = std::stoi(argv[++i]);
        } else {
            std::cerr << "❌ Unknown argument: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    setup_signal_handlers();

    std::cout << "📤 Starting Standalone Outbound Audio Processor..." << std::endl;
    std::cout << "📡 Forwarding RTP to " << dest_ip << ":" << dest_port << " for call_id " << call_id << std::endl;

    try {
        g_processor = std::make_unique<OutboundAudioProcessor>();
        if (!g_processor->start(base_port)) {
            std::cerr << "❌ Failed to start outbound audio processor" << std::endl;
            return 1;
        }

        // Optional test silence source
        try {
            std::string dir = get_executable_dir(argv[0]);
            std::string wav2 = dir + "/SIP_SILENCE_WAV2.wav";
            if (std::filesystem::exists(wav2)) {
                if (g_processor->load_and_set_silence_wav2(wav2)) {
                    std::cout << "🎧 Loaded test WAV: " << wav2 << std::endl;
                }
            }
        } catch (...) {}

        g_processor->set_udp_output(dest_ip, dest_port, static_cast<uint32_t>(call_id));
        g_processor->activate_for_call(std::to_string(call_id));

        while (g_running && g_processor->is_running()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

    } catch (const std::exception& e) {
        std::cerr << "❌ Error: " << e.what() << std::endl;
        return 1;
    }

    if (g_processor) {
        g_processor->stop();
    }
    std::cout << "🛑 Outbound Audio Processor stopped cleanly" << std::endl;

    return 0;
}
