#include "piper-service.h"
#include "database.h"

#include <iostream>
#include <string>
#include <csignal>
#include <atomic>
#include <memory>

static std::unique_ptr<StandalonePiperService> g_piper_service;
static std::atomic<bool> g_shutdown(false);

struct PiperArgs {
    std::string model_path = "models/voice.onnx";
    std::string config_path = "";
    std::string espeak_data_path = "espeak-ng-data";
    std::string db_path = "whisper_talk.db";
    int port = 8090;  // input from LLaMA
    int speaker_id = 0;
    float length_scale = 1.0f;
    float noise_scale = 0.667f;
    float noise_w_scale = 0.8f;
    std::string out_host = "127.0.0.1";  // output to audio processor
    int out_port = 8091;
    int max_concurrency = 4;
    bool verbose = false;
};

static void print_usage(const char* prog) {
    std::cout << "\n🎤 Standalone Piper TTS Service\n\n";
    std::cout << "Usage: " << prog << " [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -m, --model PATH           Piper model path [models/voice.onnx]\n";
    std::cout << "  -c, --config PATH          Piper config path [auto: model.onnx.json]\n";
    std::cout << "  -e, --espeak-data PATH     eSpeak-ng data path [espeak-ng-data]\n";
    std::cout << "  -d, --database PATH        Database path [whisper_talk.db]\n";
    std::cout << "  -p, --port N               TCP port to listen for LLaMA [8090]\n";
    std::cout << "  --speaker-id N             Speaker ID for multi-speaker models [0]\n";
    std::cout << "  --length-scale F           Speech speed (0.5=2x fast, 2.0=2x slow) [1.0]\n";
    std::cout << "  --noise-scale F            Synthesis noise level [0.667]\n";
    std::cout << "  --noise-w-scale F          Phoneme length variation [0.8]\n";
    std::cout << "  --out-host HOST            Audio processor host [127.0.0.1]\n";
    std::cout << "  --out-port PORT            Audio processor base port [8091]\n";
    std::cout << "  --max-concurrency N        Max concurrent syntheses [4, 1..hardware]\n";
    std::cout << "  -v, --verbose              Verbose output\n";
    std::cout << "  -h, --help                 Show this help\n\n";
    std::cout << "The service receives text from LLaMA service and sends audio to audio processors.\n";
    std::cout << "Each call gets its own Piper session and audio processor connection.\n\n";
}

static bool parse_args(int argc, char** argv, PiperArgs& a) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") { print_usage(argv[0]); return false; }
        else if (arg == "-m" || arg == "--model") {
            if (i + 1 < argc) a.model_path = argv[++i];
        }
        else if (arg == "-c" || arg == "--config") {
            if (i + 1 < argc) a.config_path = argv[++i];
        }
        else if (arg == "-e" || arg == "--espeak-data") {
            if (i + 1 < argc) a.espeak_data_path = argv[++i];
        }
        else if (arg == "-d" || arg == "--database") {
            if (i + 1 < argc) a.db_path = argv[++i];
        }
        else if (arg == "-p" || arg == "--port") {
            if (i + 1 < argc) a.port = std::stoi(argv[++i]);
        }
        else if (arg == "--speaker-id") {
            if (i + 1 < argc) a.speaker_id = std::stoi(argv[++i]);
        }
        else if (arg == "--length-scale") {
            if (i + 1 < argc) a.length_scale = std::stof(argv[++i]);
        }
        else if (arg == "--noise-scale") {
            if (i + 1 < argc) a.noise_scale = std::stof(argv[++i]);
        }
        else if (arg == "--noise-w-scale") {
            if (i + 1 < argc) a.noise_w_scale = std::stof(argv[++i]);
        }
        else if (arg == "--out-host") {
            if (i + 1 < argc) a.out_host = argv[++i];
        }
        else if (arg == "--out-port") {
            if (i + 1 < argc) a.out_port = std::stoi(argv[++i]);
        }
        else if (arg == "--max-concurrency") {
            if (i + 1 < argc) a.max_concurrency = std::stoi(argv[++i]);
        }
        else if (arg == "-v" || arg == "--verbose") {
            a.verbose = true;
        }
        else {
            std::cout << "❌ Unknown argument: " << arg << std::endl;
            print_usage(argv[0]);
            return false;
        }
    }
    return true;
}

static void on_signal(int sig) {
    std::cout << "\n🛑 Received signal " << sig << ", shutting down..." << std::endl;
    g_shutdown.store(true);
    if (g_piper_service) {
        g_piper_service->stop();
    }
}

int main(int argc, char** argv) {
    PiperArgs a;
    if (!parse_args(argc, argv, a)) return 1;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    // Create Piper service configuration
    PiperSessionConfig cfg;
    cfg.model_path = a.model_path;
    cfg.config_path = a.config_path;
    cfg.espeak_data_path = a.espeak_data_path;
    cfg.speaker_id = a.speaker_id;
    cfg.length_scale = a.length_scale;
    cfg.noise_scale = a.noise_scale;
    cfg.noise_w_scale = a.noise_w_scale;
    cfg.verbose = a.verbose;

    g_piper_service = std::make_unique<StandalonePiperService>(cfg);

    // Database is optional - service continues without it
    if (!g_piper_service->init_database(a.db_path)) {
        std::cout << "⚠️ Database unavailable - continuing without database support" << std::endl;
    }

    g_piper_service->set_output_endpoint(a.out_host, a.out_port);
    g_piper_service->set_max_concurrency(static_cast<size_t>(a.max_concurrency));

    if (!g_piper_service->start(a.port)) {
        std::cout << "❌ Failed to start Piper service" << std::endl;
        return 1;
    }

    std::cout << "🎤 Piper service running. Press Ctrl+C to stop." << std::endl;

    // Main service loop
    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        // Print stats periodically
        static auto last_stats = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last_stats).count() >= 30) {
            auto stats = g_piper_service->get_stats();
            std::cout << "📊 Piper Stats: " << stats.active_sessions << " active sessions, "
                      << stats.total_sessions_created << " total created, "
                      << stats.total_text_processed << " chars processed, "
                      << stats.total_audio_generated << " samples generated" << std::endl;
            last_stats = now;
        }
    }

    std::cout << "✅ Piper service shutdown complete" << std::endl;
    return 0;
}
