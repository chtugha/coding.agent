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
    if (!g_processor->start(8083)) { // Base port for Whisper TCP
        std::cerr << "❌ Failed to start inbound audio processor" << std::endl;
        return 1;
    }

    // Start UDP listener within the processor
    g_processor->start_sip_client_server(listen_port);

    std::cout << "🚀 Ready to process audio packets" << std::endl;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    g_processor->stop();
    std::cout << "🛑 Inbound Audio Processor stopped cleanly" << std::endl;

    return 0;
}
