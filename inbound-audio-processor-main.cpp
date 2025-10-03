#include "inbound-audio-processor.h"
#include "shmem_audio_channel.h"
#include <iostream>
#include <signal.h>
#include <thread>
#include <chrono>
#include <memory>

// Global processor instance for signal handling
std::unique_ptr<InboundAudioProcessor> g_processor;

void signal_handler(int signal) {
    std::cout << "\n🛑 Received signal " << signal << ", shutting down inbound audio processor..." << std::endl;

    if (g_processor) {
        // Gracefully notify Whisper and close sockets before stopping
        g_processor->deactivate_after_call();
        g_processor->stop();
    }

    exit(0);
}

void setup_signal_handlers() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN); // Ignore broken pipe signals
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "Options:\n"
              << "  --port PORT        Base port for inbound processor (default: 8083)\n"
              << "  --call-id ID       Numeric call_id (required)\n"
              << "  --help            Show this help message\n"
              << "\nInbound Audio Processor - Handles Phone → Whisper audio processing\n"
              << "Consumes RTP payload frames from shared memory and forwards to Whisper service\n";
}

int main(int argc, char* argv[]) {
    std::cout << "🎤 Starting Inbound Audio Processor Service..." << std::endl;

    // Parse command line arguments
    int base_port = 8083;
    int call_id = -1;

    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--port" && i + 1 < argc) {
            base_port = std::stoi(argv[++i]);
        } else if (arg == "--call-id" && i + 1 < argc) {
            call_id = std::stoi(argv[++i]);
        } else {
            std::cerr << "❌ Unknown argument: " << arg << std::endl;
            print_usage(argv[0]);
            return 1;
        }
    }

    if (call_id < 0) {
        std::cerr << "❌ --call-id is required" << std::endl;
        print_usage(argv[0]);
        return 1;
    }

    std::cout << "📋 Configuration:" << std::endl;
    std::cout << "   Base port: " << base_port << std::endl;
    std::cout << "   Call-ID: " << call_id << std::endl;

    // Setup signal handlers
    setup_signal_handlers();

    // Create and start inbound processor
    try {
        g_processor = std::make_unique<InboundAudioProcessor>();

        if (!g_processor->start(base_port)) {
            std::cerr << "❌ Failed to start inbound audio processor" << std::endl;
            return 1;
        }

        // Open shared memory inbound channel (consumer role) - SIP client creates it
        auto channel = std::make_shared<ShmAudioChannel>();
        std::string name = "/ap_in_" + std::to_string(call_id);
        if (!channel->create_or_open(name, static_cast<uint32_t>(call_id), 2048, 512, /*create=*/false)) {
            std::cerr << "❌ Failed to open shared memory channel: " << name << std::endl;
            return 1;
        }
        channel->set_role_consumer(true);

        // Activate call context with numeric id (string form for compatibility)
        g_processor->activate_for_call(std::to_string(call_id));

        // Start reader thread to consume SHM frames and feed processor
        std::thread reader([channel]() {
            uint16_t seq = 0;
            uint32_t ts = 0;
            const uint32_t TIMESTAMP_INCREMENT = 160; // 20ms @8kHz
            std::vector<uint8_t> frame;
            while (g_processor && g_processor->is_running()) {
                if (channel->read_frame(frame)) {
                    // Wrap SHM bytes as RTPAudioPacket (assume PT=0 PCMU)
                    RTPAudioPacket pkt(0, frame, ts, seq);
                    g_processor->process_rtp_audio(pkt);
                    seq++;
                    ts += TIMESTAMP_INCREMENT;
                } else {
                    std::this_thread::sleep_for(std::chrono::milliseconds(2));
                }
            }
        });
        reader.detach();

        std::cout << "✅ Inbound Audio Processor started successfully" << std::endl;
        std::cout << "🎤 Ready to process Phone → Whisper audio streams" << std::endl;
        std::cout << "📡 Whisper TCP server: port " << (9001) << " + call_id" << std::endl;
        std::cout << "🔌 Inbound SHM channel: " << name << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "❌ Failed to create inbound processor: " << e.what() << std::endl;
        return 1;
    }

    // Main service loop
    std::cout << "🚀 Inbound Audio Processor running. Press Ctrl+C to stop." << std::endl;

    try {
        while (g_processor && g_processor->is_running()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ Runtime error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "🛑 Inbound Audio Processor stopped" << std::endl;
    return 0;
}
