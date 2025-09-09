#include "llama-service.h"
#include "database.h"

#include <iostream>
#include <string>
#include <csignal>
#include <atomic>
#include <memory>

static std::unique_ptr<StandaloneLlamaService> g_llama_service;
static std::atomic<bool> g_shutdown(false);

struct LlamaArgs {
    std::string model_path = "models/llama-7b-q4_0.gguf";
    std::string db_path    = "whisper_talk.db";
    int port               = 8083; // input from whisper
    int n_threads          = 4;
    int n_ctx              = 2048;
    int n_gpu_layers       = 999;
    float temperature      = 0.3f;
    bool use_gpu           = true;
    bool flash_attn        = false;
    std::string person     = "User";
    std::string bot        = "Assistant";
    std::string out_host   = "";  // empty disables output
    int out_port           = 0;
};

static void print_usage(const char* prog) {
    std::cout << "\n🦙 Standalone LLaMA Service\n\n";
    std::cout << "Usage: " << prog << " [options]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -m, --model PATH           LLaMA model path [models/llama-7b-q4_0.gguf]\n";
    std::cout << "  -d, --database PATH        Database path [whisper_talk.db]\n";
    std::cout << "  -p, --port N               TCP port to listen for Whisper [8083]\n";
    std::cout << "  --threads N                Threads for LLaMA [4]\n";
    std::cout << "  --ctx N                    Context length [2048]\n";
    std::cout << "  --ngl N                    GPU layers [999]\n";
    std::cout << "  --temp F                   Temperature [0.3]\n";
    std::cout << "  --no-gpu                   Disable GPU\n";
    std::cout << "  --flash-attn               Enable flash attention\n";
    std::cout << "  --person NAME              User name in prompt [User]\n";
    std::cout << "  --bot NAME                 Bot name in prompt [Assistant]\n";
    std::cout << "  --out-host HOST            Output endpoint host (optional)\n";
    std::cout << "  --out-port PORT            Output endpoint port (optional)\n";
}

static bool parse_args(int argc, char** argv, LlamaArgs& a) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") { print_usage(argv[0]); return false; }
        else if (arg == "-m" || arg == "--model") { a.model_path = argv[++i]; }
        else if (arg == "-d" || arg == "--database") { a.db_path = argv[++i]; }
        else if (arg == "-p" || arg == "--port") { a.port = std::stoi(argv[++i]); }
        else if (arg == "--threads") { a.n_threads = std::stoi(argv[++i]); }
        else if (arg == "--ctx") { a.n_ctx = std::stoi(argv[++i]); }
        else if (arg == "--ngl") { a.n_gpu_layers = std::stoi(argv[++i]); }
        else if (arg == "--temp") { a.temperature = std::stof(argv[++i]); }
        else if (arg == "--no-gpu") { a.use_gpu = false; }
        else if (arg == "--flash-attn") { a.flash_attn = true; }
        else if (arg == "--person") { a.person = argv[++i]; }
        else if (arg == "--bot") { a.bot = argv[++i]; }
        else if (arg == "--out-host") { a.out_host = argv[++i]; }
        else if (arg == "--out-port") { a.out_port = std::stoi(argv[++i]); }
        else { std::cout << "Unknown arg: " << arg << "\n"; print_usage(argv[0]); return false; }
    }
    return true;
}

static void on_signal(int sig) {
    std::cout << "\n🛑 Signal " << sig << " received" << std::endl;
    g_shutdown.store(true);
    if (g_llama_service) g_llama_service->stop();
}

int main(int argc, char** argv) {
    LlamaArgs a;
    if (!parse_args(argc, argv, a)) return 1;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    LlamaSessionConfig cfg;
    cfg.model_path = a.model_path;
    cfg.n_threads = a.n_threads;
    cfg.n_ctx = a.n_ctx;
    cfg.n_gpu_layers = a.n_gpu_layers;
    cfg.temperature = a.temperature;
    cfg.use_gpu = a.use_gpu;
    cfg.flash_attn = a.flash_attn;
    cfg.person_name = a.person;
    cfg.bot_name = a.bot;

    g_llama_service = std::make_unique<StandaloneLlamaService>(cfg);
    if (!g_llama_service->init_database(a.db_path)) {
        return 1;
    }
    if (!a.out_host.empty() && a.out_port > 0) {
        g_llama_service->set_output_endpoint(a.out_host, a.out_port);
    }

    if (!g_llama_service->start(a.port)) {
        return 1;
    }

    std::cout << "\n🦙 LLaMA service started on port " << a.port << ", model: " << a.model_path << std::endl;
    std::cout << "DB: " << a.db_path << std::endl;
    if (!a.out_host.empty()) {
        std::cout << "Output endpoint: " << a.out_host << ":" << a.out_port << std::endl;
    }
    std::cout << "Press Ctrl+C to stop." << std::endl;

    while (!g_shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    g_llama_service.reset();
    std::cout << "✅ LLaMA service stopped" << std::endl;
    return 0;
}

