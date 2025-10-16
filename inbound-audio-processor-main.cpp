#include "inbound-audio-processor.h"
#include "shmem_audio_channel.h"
#include <iostream>
#include <signal.h>
#include <thread>
#include <chrono>
#include <memory>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

// Control socket path
static const char* kInboundCtrlSock = "/tmp/inbound-audio-processor.ctrl";

// Global processor instance and SHM I/O
std::unique_ptr<InboundAudioProcessor> g_processor;
std::shared_ptr<ShmAudioChannel> g_in_channel;
std::thread g_reader_thread;
std::atomic<bool> g_reader_running{false};
std::mutex g_reader_mutex;

static void stop_reader_locked_() {
    if (g_reader_running.exchange(false)) {
        if (g_reader_thread.joinable()) g_reader_thread.join();
    }
}

static void start_reader_locked_() {
    if (g_reader_running.exchange(true)) return;
    g_reader_thread = std::thread([]() {
        uint16_t seq = 0; uint32_t ts = 0; const uint32_t kInc = 160; // 20ms @8kHz
        std::vector<uint8_t> frame;
        while (g_reader_running.load()) {
            if (g_in_channel && g_in_channel->read_frame(frame)) {
                RTPAudioPacket pkt(0, frame, ts, seq);
                if (g_processor) g_processor->process_rtp_audio(pkt);
                seq++; ts += kInc;
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
    });
}

static bool open_inbound_channel_(int call_id) {
    auto ch = std::make_shared<ShmAudioChannel>();
    std::string name = std::string("/ap_in_") + std::to_string(call_id);
    if (!ch->create_or_open(name, static_cast<uint32_t>(call_id), 2048, 512, /*create=*/false)) {
        std::cerr << "❌ Failed to open shared memory channel: " << name << std::endl;
        return false;
    }
    ch->set_role_consumer(true);
    g_in_channel = ch;
    std::cout << "🔌 Inbound SHM channel bound: " << name << std::endl;
    return true;
}

static void handle_control_connection_(int cfd, int base_port) {
    char buf[256]; ssize_t n = read(cfd, buf, sizeof(buf)-1); if (n <= 0) return; buf[n] = '\0';
    std::string cmd(buf);
    if (cmd.rfind("ACTIVATE", 0) == 0) {
        // Format: ACTIVATE <call_id>\n
        int call_id = -1;
        try { call_id = std::stoi(cmd.substr(9)); } catch (...) { call_id = -1; }
        if (call_id >= 0) {
            std::lock_guard<std::mutex> lk(g_reader_mutex);
            stop_reader_locked_();
            if (!open_inbound_channel_(call_id)) return;
            if (g_processor && !g_processor->is_running()) g_processor->start(base_port);
            if (g_processor) g_processor->activate_for_call(std::to_string(call_id));
            start_reader_locked_();
            std::cout << "✅ Activated for call " << call_id << std::endl;
        }
    } else if (cmd.rfind("DEACTIVATE", 0) == 0) {
        std::lock_guard<std::mutex> lk(g_reader_mutex);
        stop_reader_locked_();
        if (g_processor) g_processor->deactivate_after_call();
        g_in_channel.reset();
        std::cout << "😴 Deactivated (SLEEPING)" << std::endl;
    } else if (cmd.rfind("SHUTDOWN", 0) == 0) {
        std::lock_guard<std::mutex> lk(g_reader_mutex);
        stop_reader_locked_();
        if (g_processor) { g_processor->deactivate_after_call(); g_processor->stop(); }
        std::cout << "🛑 Shutdown requested" << std::endl;
        exit(0);
    }
}

static void control_server_thread_(int base_port) {
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) return;
    ::unlink(kInboundCtrlSock);
    struct sockaddr_un addr{}; addr.sun_family = AF_UNIX; std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", kInboundCtrlSock);
    if (bind(sfd, (struct sockaddr*)&addr, sizeof(addr)) != 0) { close(sfd); return; }
    listen(sfd, 4);
    std::cout << "📮 Control socket listening at " << kInboundCtrlSock << std::endl;
    while (true) {
        int cfd = accept(sfd, nullptr, nullptr);
        if (cfd < 0) continue;
        handle_control_connection_(cfd, base_port);
        close(cfd);
    }
}

static void signal_handler(int sig) {
    if (sig == SIGINT) {
        std::cout << "\n🛑 SIGINT - exiting inbound processor" << std::endl;
        if (g_processor) { g_processor->deactivate_after_call(); g_processor->stop(); }
        exit(0);
    } else if (sig == SIGTERM) {
        std::cout << "\n😴 SIGTERM - deactivating (sleep)" << std::endl;
        std::lock_guard<std::mutex> lk(g_reader_mutex);
        stop_reader_locked_();
        if (g_processor) g_processor->deactivate_after_call();
    }
}

static void setup_signal_handlers() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);
}

static void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [options]\n"
              << "Options:\n"
              << "  --port PORT        Base port for inbound processor (default: 8083)\n"
              << "  --call-id ID       Numeric call_id (optional)\n"
              << "  --help            Show this help message\n";
}

int main(int argc, char* argv[]) {
    std::cout << "🎤 Starting Inbound Audio Processor Service..." << std::endl;

    int base_port = 8083; int call_id = -1;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--help") { print_usage(argv[0]); return 0; }
        else if (arg == "--port" && i + 1 < argc) { base_port = std::stoi(argv[++i]); }
        else if (arg == "--call-id" && i + 1 < argc) { call_id = std::stoi(argv[++i]); }
        else { std::cerr << "❌ Unknown argument: " << arg << std::endl; return 1; }
    }

    setup_signal_handlers();

    try {
        g_processor = std::make_unique<InboundAudioProcessor>();
        if (!g_processor->start(base_port)) { std::cerr << "❌ Failed to start inbound audio processor" << std::endl; return 1; }

        // Start control socket server
        std::thread([base_port]() { control_server_thread_(base_port); }).detach();
    } catch (const std::exception& e) {
        std::cerr << "❌ Failed to create inbound processor: " << e.what() << std::endl; return 1; }

    // If call_id provided, activate immediately
    if (call_id >= 0) {
        std::lock_guard<std::mutex> lk(g_reader_mutex);
        if (open_inbound_channel_(call_id)) {
            g_processor->activate_for_call(std::to_string(call_id));
            start_reader_locked_();
        }
    } else {
        std::cout << "😴 Waiting for ACTIVATE via control socket " << kInboundCtrlSock << std::endl;
    }

    try {
        while (g_processor && g_processor->is_running()) std::this_thread::sleep_for(std::chrono::seconds(1));
    } catch (const std::exception& e) { std::cerr << "❌ Runtime error: " << e.what() << std::endl; return 1; }

    std::cout << "🛑 Inbound Audio Processor stopped" << std::endl;
    return 0;
}
