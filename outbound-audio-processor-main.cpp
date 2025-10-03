#include "outbound-audio-processor.h"
#include "shmem_audio_channel.h"
#include <filesystem>
#include <fstream>

#include <iostream>
#include <signal.h>
#include <thread>
#include <chrono>
#include <memory>

// Global processor instance for signal handling
std::unique_ptr<OutboundAudioProcessor> g_processor;

void signal_handler(int signal) {
    std::cout << "\n🛑 Received signal " << signal << ", shutting down outbound audio processor..." << std::endl;

    if (g_processor) {
        // Gracefully notify Piper and close sockets before stopping
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
              << "  --port PORT        Base port for outbound processor (default: 8183)\n"
              << "  --call-id ID       Numeric call_id (required)\n"
              << "  --help            Show this help message\n"
              << "\nOutbound Audio Processor - Handles Piper → Phone audio processing\n"
              << "Receives TTS audio from Piper service and writes G.711 to shared memory for SIP client\n";
}

int main(int argc, char* argv[]) {
    std::cout << "📤 Starting Outbound Audio Processor Service..." << std::endl;

    // Parse command line arguments
    int base_port = 8183;
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

    // Create and start outbound processor
    try {
        g_processor = std::make_unique<OutboundAudioProcessor>();

        if (!g_processor->start(base_port)) {
            std::cerr << "❌ Failed to start outbound audio processor" << std::endl;
            return 1;
        }

        // Optional test silence source: SIP_SILENCE_WAV2.wav next to binary
        try {
            std::string dir = get_executable_dir(argv[0]);
            std::string wav2 = dir + "/SIP_SILENCE_WAV2.wav";
            std::ifstream tf(wav2, std::ios::binary);
            if (tf.good()) {
                bool ok = g_processor->load_and_set_silence_wav2(wav2);
                if (ok) {
                    std::cout << " Loaded test WAV (converted to \u03bc-law mono 8kHz) from " << wav2 << std::endl;
                } else {
                    std::cout << "\u26a0\ufe0f Found SIP_SILENCE_WAV2.wav but could not parse/convert; ignoring" << std::endl;
                }
            }
        } catch (...) { /* ignore */ }

        // Open shared memory out channel (producer role) - SIP client creates it
        auto channel = std::make_shared<ShmAudioChannel>();
        std::string name = "/ap_out_" + std::to_string(call_id);
        if (!channel->create_or_open(name, static_cast<uint32_t>(call_id), 2048, 512, /*create=*/false)) {
            std::cerr << "❌ Failed to open shared memory channel: " << name << std::endl;
            return 1;
        }
        channel->set_role_producer(true);
        g_processor->set_shared_memory_out(channel);

        // Activate call context with numeric id
        g_processor->activate_for_call(std::to_string(call_id));

        std::cout << "✅ Outbound Audio Processor started successfully" << std::endl;
        std::cout << "📤 Ready to process Piper → Phone audio streams" << std::endl;
        std::cout << "📡 Outbound waiting for REGISTER on UDP port " << (13000 + call_id)
                  << "; will connect to Kokoro TCP " << (9002 + call_id) << std::endl;
        std::cout << "🔌 Outbound SHM channel: " << name << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "❌ Failed to create outbound processor: " << e.what() << std::endl;
        return 1;
    }

    // Main service loop
    std::cout << "🚀 Outbound Audio Processor running. Press Ctrl+C to stop." << std::endl;

    try {
        while (g_processor && g_processor->is_running()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    } catch (const std::exception& e) {
        std::cerr << "❌ Runtime error: " << e.what() << std::endl;
        return 1;
    }

    std::cout << "🛑 Outbound Audio Processor stopped" << std::endl;
    return 0;
}
