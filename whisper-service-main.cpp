#include "whisper-service.h"
#include <iostream>
#include <csignal>
#include <atomic>

// Global service instance for signal handling
std::unique_ptr<StandaloneWhisperService> g_whisper_service;
std::atomic<bool> g_shutdown_requested(false);

void signal_handler(int signal) {
    std::cout << "\n🛑 Shutdown signal received (" << signal << ")" << std::endl;
    g_shutdown_requested.store(true);
    
    if (g_whisper_service) {
        g_whisper_service->stop();
    }
}

bool parse_whisper_service_args(int argc, char** argv, WhisperServiceArgs& args) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "-h" || arg == "--help") {
            print_whisper_service_usage(argv[0]);
            return false;
        }
        else if (arg == "-m" || arg == "--model") {
            if (i + 1 < argc) {
                args.model_path = argv[++i];
            }
        }
        else if (arg == "-d" || arg == "--database") {
            if (i + 1 < argc) {
                args.database_path = argv[++i];
            }
        }
        else if (arg == "-t" || arg == "--threads") {
            if (i + 1 < argc) {
                args.n_threads = std::stoi(argv[++i]);
            }
        }
        else if (arg == "-l" || arg == "--language") {
            if (i + 1 < argc) {
                args.language = argv[++i];
            }
        }
        else if (arg == "--host") {
            if (i + 1 < argc) {
                args.discovery_host = argv[++i];
            }
        }
        else if (arg == "--port") {
            if (i + 1 < argc) {
                args.discovery_port = std::stoi(argv[++i]);
            }
        }
        else if (arg == "--no-gpu") {
            args.use_gpu = false;
        }
        else if (arg == "--translate") {
            args.translate = true;
        }
        else if (arg == "--no-timestamps") {
            args.no_timestamps = true;
        }
        else if (arg == "--llama-host") {
            if (i + 1 < argc) {
                args.llama_host = argv[++i];
            }
        }
        else if (arg == "--llama-port") {
            if (i + 1 < argc) {
                args.llama_port = std::stoi(argv[++i]);
            }
        }
        else if (arg == "-v" || arg == "--verbose") {
            args.verbose = true;
        }
        else if (arg == "--discovery-interval") {
            if (i + 1 < argc) {
                args.discovery_interval_ms = std::stoi(argv[++i]);
            }
        }
        else {
            std::cout << "❌ Unknown argument: " << arg << std::endl;
            print_whisper_service_usage(argv[0]);
            return false;
        }
    }
    
    return true;
}

void print_whisper_service_usage(const char* program_name) {
    std::cout << "\n🎤 Standalone Whisper Service\n" << std::endl;
    std::cout << "Usage: " << program_name << " [options]\n" << std::endl;
    std::cout << "Options:" << std::endl;
    std::cout << "  -h, --help                 Show this help message" << std::endl;
    std::cout << "  -m, --model PATH           Whisper model path [models/ggml-base.en.bin]" << std::endl;
    std::cout << "  -d, --database PATH        Database path [whisper_talk.db]" << std::endl;
    std::cout << "  -t, --threads N            Number of threads [4]" << std::endl;
    std::cout << "  -l, --language LANG        Language code [en]" << std::endl;
    std::cout << "  --host HOST                Discovery server host [127.0.0.1]" << std::endl;
    std::cout << "  --port PORT                Discovery server port [13000]" << std::endl;
    std::cout << "  --llama-host HOST          LLaMA service host [127.0.0.1]" << std::endl;
    std::cout << "  --llama-port PORT          LLaMA service port [8083]" << std::endl;
    std::cout << "  --no-gpu                   Disable GPU acceleration" << std::endl;
    std::cout << "  --translate                Translate to English" << std::endl;
    std::cout << "  --no-timestamps            Disable timestamps" << std::endl;
    std::cout << "  -v, --verbose              Verbose output" << std::endl;
    std::cout << "  --discovery-interval MS    Discovery interval [5000]" << std::endl;
    std::cout << "\nThe service automatically discovers and connects to audio streams" << std::endl;
    std::cout << "advertised by SIP clients on the discovery port." << std::endl;
    std::cout << "\nExample:" << std::endl;
    std::cout << "  " << program_name << " -m models/ggml-base.en.bin -t 8 --verbose" << std::endl;
    std::cout << std::endl;
}

int main(int argc, char** argv) {
    std::cout << "🎤 Standalone Whisper Service v1.0" << std::endl;
    std::cout << "🔗 Connects to SIP audio streams via TCP sockets" << std::endl;
    std::cout << "📡 Completely independent and replaceable service" << std::endl;
    std::cout << std::endl;
    
    // Parse command line arguments
    WhisperServiceArgs args;
    if (!parse_whisper_service_args(argc, argv, args)) {
        return 1;
    }
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Create whisper configuration
    WhisperSessionConfig config;
    config.model_path = args.model_path;
    config.n_threads = args.n_threads;
    config.use_gpu = args.use_gpu;
    config.language = args.language;
    config.temperature = args.temperature;
    config.no_timestamps = args.no_timestamps;
    config.translate = args.translate;
    
    // Create and start whisper service
    g_whisper_service = std::make_unique<StandaloneWhisperService>();

    // Configure LLaMA endpoint before starting
    g_whisper_service->set_llama_endpoint(args.llama_host, args.llama_port);

    if (!g_whisper_service->start(config, args.database_path)) {
        std::cout << "❌ Failed to start whisper service" << std::endl;
        return 1;
    }
    
    std::cout << "✅ Whisper service started successfully" << std::endl;
    std::cout << "🔍 Discovering audio streams on " << args.discovery_host << ":" << args.discovery_port << std::endl;
    std::cout << "💡 Press Ctrl+C to shutdown gracefully" << std::endl;
    std::cout << std::endl;
    
    // Main service loop
    while (!g_shutdown_requested.load() && g_whisper_service->is_running()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
    
    std::cout << "🛑 Shutting down whisper service..." << std::endl;
    g_whisper_service->stop();
    g_whisper_service.reset();
    
    std::cout << "✅ Whisper service shutdown complete" << std::endl;
    return 0;
}
