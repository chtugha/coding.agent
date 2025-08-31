// Standalone HTTP Server Module
// Serves web interface and manages database via REST API

#include "simple-http-api.h"
#include "database.h"
#include <iostream>
#include <signal.h>
#include <thread>
#include <chrono>
#include <unistd.h>
#include <vector>

// Global variables for signal handling
static bool g_running = true;
static std::unique_ptr<SimpleHttpServer> g_server;

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_running = false;
    if (g_server) {
        g_server->stop();
    }
}

void print_usage() {
    std::cout << "Usage: whisper-http-server [options]\n"
              << "Options:\n"
              << "  --port PORT     HTTP server port (default: 8081)\n"
              << "  --db PATH       Database file path (default: whisper_talk.db)\n"
              << "  --help          Show this help message\n";
}

int main(int argc, char** argv) {
    int port = 8081;
    std::string db_path = "whisper_talk.db";
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (arg == "--db" && i + 1 < argc) {
            db_path = argv[++i];
        } else if (arg == "--help") {
            print_usage();
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            print_usage();
            return 1;
        }
    }
    
    std::cout << "🌐 Starting Whisper Talk LLaMA HTTP Server..." << std::endl;
    std::cout << "   Port: " << port << std::endl;
    std::cout << "   Database: " << db_path << std::endl;
    
    // Setup signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    
    // Initialize database
    Database database;
    if (!database.init(db_path)) {
        std::cerr << "❌ Failed to initialize database!" << std::endl;
        return 1;
    }
    std::cout << "✅ Database initialized" << std::endl;

    // HTTP server no longer hosts Whisper service - it runs independently

    // Whisper service removed - runs as independent service

    // Create HTTP server for web interface only (no Whisper service)
    g_server = std::make_unique<SimpleHttpServer>(port, &database);
    std::cout << "✅ HTTP server initialized (web interface only)" << std::endl;
    
    // Start server
    if (!g_server->start()) {
        std::cerr << "❌ Failed to start HTTP server!" << std::endl;
        return 1;
    }
    std::cout << "🚀 HTTP server listening on port " << port << std::endl;
    std::cout << "📱 Open http://localhost:" << port << " in your browser" << std::endl;
    std::cout << "Press Ctrl+C to stop..." << std::endl;
    
    // Main loop
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    std::cout << "🛑 Shutting down HTTP server..." << std::endl;
    g_server->stop();

    database.close();
    std::cout << "✅ HTTP server stopped cleanly" << std::endl;
    
    return 0;
}
