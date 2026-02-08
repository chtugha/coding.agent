// Standalone SIP Client Module - Optimized for Fast Audio Processing
// Handles incoming calls, creates database sessions, manages audio streams
// Manages SIP line connections and status updates

#include "database.h"
#include "audio-processor-interface.h"

#include <iostream>
#include <unordered_map>
#include <fstream>
#include <atomic>
#include <chrono>
#include <signal.h>
#include <thread>
#include <string>
#include <map>
#include <mutex>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <unistd.h>
#include <errno.h>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <ctime>
#include <openssl/evp.h>
#include <openssl/md5.h>

#include <spawn.h>
#include <sys/wait.h>
#include <mach-o/dyld.h>
#include <limits.h>
extern char **environ;
#include <sys/un.h>

static const char* kInboundCtrlSock = "/tmp/inbound-audio-processor.ctrl";
static const char* kOutboundCtrlSock = "/tmp/outbound-audio-processor.ctrl";

static bool send_control_command(const char* path, const std::string& cmd) {
    int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sfd < 0) { std::cout << "❌ ctrl socket() failed: " << strerror(errno) << std::endl; return false; }
    struct sockaddr_un addr{}; addr.sun_family = AF_UNIX; std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(sfd, (struct sockaddr*)&addr, sizeof(addr)) != 0) { std::cout << "❌ ctrl connect(" << path << ") failed: " << strerror(errno) << std::endl; close(sfd); return false; }
    ssize_t n = write(sfd, cmd.c_str(), cmd.size());
    if (n < 0) { std::cout << "❌ ctrl write failed: " << strerror(errno) << std::endl; close(sfd); return false; }
    close(sfd);
    return true;
}
static bool wait_for_control_socket_ready(const char* path, int timeout_ms = 2000) {
    const int step_ms = 50;
    int waited = 0;
    while (waited <= timeout_ms) {
        int sfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sfd >= 0) {
            struct sockaddr_un addr{}; addr.sun_family = AF_UNIX; std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
            if (connect(sfd, (struct sockaddr*)&addr, sizeof(addr)) == 0) { close(sfd); return true; }
            close(sfd);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(step_ms));
        waited += step_ms;
    }
    return false;
}

static bool send_control_command_retry(const char* path, const std::string& cmd, int retries = 5, int delay_ms = 100) {
    for (int i = 0; i < retries; ++i) {
        if (send_control_command(path, cmd)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
    return false;
}

static void whisper_register_notify(int call_num_id, int repeats = 5, int delay_ms = 100) {
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(13000);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 127.0.0.1
    std::string msg = std::string("REGISTER:") + std::to_string(call_num_id);
    for (int i = 1; i <= repeats; ++i) {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) {
            std::cout << "⚠️ [SIP->Whisper poke] Failed to create UDP socket: " << strerror(errno) << std::endl;
            break;
        }
        ssize_t sent = sendto(s, msg.data(), msg.size(), 0, (struct sockaddr*)&addr, sizeof(addr));
        if (sent < 0) {
            std::cout << "⚠️ [SIP->Whisper poke] REGISTER send failed for call_id " << call_num_id << ": " << strerror(errno) << std::endl;
        } else {
            if (i == 1 || i == repeats) {
                std::cout << "📤 [SIP->Whisper poke] Sent REGISTER (" << i << "/" << repeats << ") for call_id " << call_num_id << std::endl;
            }
        }
        close(s);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }
}





// Simple SIP client configuration
struct SipConfig {
    std::string username;
    std::string password;
    std::string server_ip;
    int server_port;
    bool auto_answer;
};

// Call state (local to SIP client) - sessionless
struct SipCallSession {
    std::string call_id;  // SIP Call-ID (not session_id)
    int caller_id;
    int call_num_id;      // Database primary key ID
    std::string phone_number;
    std::string status; // "ringing", "active", "ended"
    std::chrono::system_clock::time_point start_time;
    int internal_port; // Unique RTP port for this line (10000 + line_id)
};

// Helper function to calculate MD5 hash for SIP digest authentication
std::string calculate_md5(const std::string& input) {
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) return "";

    if (EVP_DigestInit_ex(ctx, EVP_md5(), nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    if (EVP_DigestUpdate(ctx, input.c_str(), input.length()) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len;
    if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1) {
        EVP_MD_CTX_free(ctx);
        return "";
    }

    EVP_MD_CTX_free(ctx);

    std::ostringstream ss;
    for (unsigned int i = 0; i < digest_len; i++) {
        ss << std::hex << std::setfill('0') << std::setw(2) << (int)digest[i];
    }
    return ss.str();
}


// G.711 codec conversion using direct lookup tables (ITU-T G.711)
// Direct table conversion avoids double quantization errors from μ-law→linear→A-law

// Direct A-law to μ-law conversion table (ITU-T G.711) - avoids double quantization
static const uint8_t alaw_to_ulaw_table[256] = {
    42, 43, 40, 41, 46, 47, 44, 45, 34, 35, 32, 33, 38, 39, 36, 37,
    58, 59, 56, 57, 62, 63, 60, 61, 50, 51, 48, 49, 54, 55, 52, 53,
    10, 11,  8,  9, 14, 15, 12, 13,  2,  3,  0,  1,  6,  7,  4,  5,
    26, 27, 24, 25, 30, 31, 28, 29, 18, 19, 16, 17, 22, 23, 20, 21,
   106,107,104,105,110,111,108,109, 98, 99, 96, 97,102,103,100,101,
   122,123,120,121,126,127,124,125,114,115,112,113,118,119,116,117,
    74, 75, 72, 73, 78, 79, 76, 77, 66, 67, 64, 65, 70, 71, 68, 69,
    90, 91, 88, 89, 94, 95, 92, 93, 82, 83, 80, 81, 86, 87, 84, 85,
   170,171,168,169,174,175,172,173,162,163,160,161,166,167,164,165,
   186,187,184,185,190,191,188,189,178,179,176,177,182,183,180,181,
   138,139,136,137,142,143,140,141,130,131,128,129,134,135,132,133,
   154,155,152,153,158,159,156,157,146,147,144,145,150,151,148,149,
   234,235,232,233,238,239,236,237,226,227,224,225,230,231,228,229,
   250,251,248,249,254,255,252,253,242,243,240,241,246,247,244,245,
   202,203,200,201,206,207,204,205,194,195,192,193,198,199,196,197,
   218,219,216,217,222,223,220,221,210,211,208,209,214,215,212,213
};

static std::vector<uint8_t> convert_alaw_to_ulaw(const std::vector<uint8_t>& in_alaw) {
    std::vector<uint8_t> out_ulaw;
    out_ulaw.reserve(in_alaw.size());
    for (uint8_t a : in_alaw) {
        out_ulaw.push_back(alaw_to_ulaw_table[a]);
    }
    return out_ulaw;
}


// Direct μ-law to A-law conversion table (ITU-T G.711) - avoids double quantization
static const uint8_t ulaw_to_alaw_table[256] = {
    42, 43, 40, 41, 46, 47, 44, 45, 34, 35, 32, 33, 38, 39, 36, 37,
    58, 59, 56, 57, 62, 63, 60, 61, 50, 51, 48, 49, 54, 55, 52, 53,
    10, 11,  8,  9, 14, 15, 12, 13,  2,  3,  0,  1,  6,  7,  4,  5,
    26, 27, 24, 25, 30, 31, 28, 29, 18, 19, 16, 17, 22, 23, 20, 21,
   106,107,104,105,110,111,108,109, 98, 99, 96, 97,102,103,100,101,
   122,123,120,121,126,127,124,125,114,115,112,113,118,119,116,117,
    74, 75, 72, 73, 78, 79, 76, 77, 66, 67, 64, 65, 70, 71, 68, 69,
    90, 91, 88, 89, 94, 95, 92, 93, 82, 83, 80, 81, 86, 87, 84, 85,
   170,171,168,169,174,175,172,173,162,163,160,161,166,167,164,165,
   186,187,184,185,190,191,188,189,178,179,176,177,182,183,180,181,
   138,139,136,137,142,143,140,141,130,131,128,129,134,135,132,133,
   154,155,152,153,158,159,156,157,146,147,144,145,150,151,148,149,
   234,235,232,233,238,239,236,237,226,227,224,225,230,231,228,229,
   250,251,248,249,254,255,252,253,242,243,240,241,246,247,244,245,
   202,203,200,201,206,207,204,205,194,195,192,193,198,199,196,197,
   218,219,216,217,222,223,220,221,210,211,208,209,214,215,212,213
};

static std::vector<uint8_t> convert_ulaw_to_alaw(const std::vector<uint8_t>& in_ulaw) {
    std::vector<uint8_t> out_alaw;
    out_alaw.reserve(in_ulaw.size());
    for (uint8_t u : in_ulaw) {
        out_alaw.push_back(ulaw_to_alaw_table[u]);
    }
    return out_alaw;
}

// Load μ-law mono 8kHz WAV data as raw bytes (for silence/testing)
static std::vector<uint8_t> load_ulaw_wav_bytes(const std::string& path) {
    std::vector<uint8_t> data;
    std::ifstream f(path, std::ios::binary);
    if (!f) return data;

    auto read_u32 = [&](uint32_t& v){ f.read(reinterpret_cast<char*>(&v), 4); return bool(f); };
    auto read_u16 = [&](uint16_t& v){ f.read(reinterpret_cast<char*>(&v), 2); return bool(f); };

    char riff[4]; f.read(riff, 4); if (strncmp(riff, "RIFF", 4) != 0) return {};
    uint32_t overall_size; if (!read_u32(overall_size)) return {};
    char wave[4]; f.read(wave, 4); if (strncmp(wave, "WAVE", 4) != 0) return {};

    uint16_t audio_format = 0, num_channels = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0;
    bool have_fmt = false;
    bool have_data = false;
    uint32_t data_size = 0;
    std::streampos data_pos = 0;

    while (f && (!have_fmt || !have_data)) {
        char chunk_id[4]; if (!f.read(chunk_id, 4)) break;
        uint32_t chunk_size; if (!read_u32(chunk_size)) break;
        if (strncmp(chunk_id, "fmt ", 4) == 0) {
            if (!read_u16(audio_format)) break; // 1=PCM, 6=A-law, 7=μ-law
            if (!read_u16(num_channels)) break;
            if (!read_u32(sample_rate)) break;
            uint32_t byte_rate; if (!read_u32(byte_rate)) break;
            uint16_t block_align; if (!read_u16(block_align)) break;
            if (!read_u16(bits_per_sample)) break;
            // skip any extra fmt bytes
            if (chunk_size > 16) f.seekg(chunk_size - 16, std::ios::cur);
            have_fmt = true;
        } else if (strncmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size;
            data_pos = f.tellg();
            f.seekg(chunk_size, std::ios::cur);
            have_data = true;
        } else {
            f.seekg(chunk_size, std::ios::cur);
        }
    }

    if (!have_fmt || !have_data) return {};
    if (!(audio_format == 7 && num_channels == 1 && sample_rate == 8000 && bits_per_sample == 8)) {
        // Only μ-law mono 8kHz 8-bit is supported for simplicity
        return {};
    }

    data.resize(data_size);
    f.clear();
    f.seekg(data_pos);
    f.read(reinterpret_cast<char*>(data.data()), data_size);
    if (!f) return {};
    return data;
}


// Forward declaration removed - using split-processor service

class SimpleSipClient {
public:
    SimpleSipClient();
    ~SimpleSipClient();

    bool init(Database* database, int specific_line_id = -1);

    // Simple audio routing
    // Removed: session registration functions - using sessionless routing
    void route_rtp_to_processor(int call_num_id, const uint8_t* rtp_data, size_t rtp_len);


    // SIP networking
    bool setup_sip_listener();
    void sip_listener_loop();
    int allocate_dynamic_port(); // Legacy method
    int allocate_rtp_port_for_call(const std::string& call_id); // Dynamic line-based allocation
    bool setup_rtp_listener(int rtp_port, int call_num_id);
    bool start();
    void stop();

    // Call handling (sessionless)
    void handle_incoming_call(const std::string& caller_number, const std::string& call_id = "");
    void end_call(const std::string& call_id);

    // Audio streaming (sessionless)
    void stream_audio_from_piper(const std::string& call_id, const std::vector<uint8_t>& audio_data);
    void send_rtp_packets_to_pbx(const std::string& call_id, const std::vector<uint8_t>& g711_data, int local_rtp_port);
    void send_rtp_packets_to_pbx_sessionless(const std::vector<uint8_t>& g711_data, int local_rtp_port);
    void send_g711_as_rtp_packets(int rtp_sock, const std::vector<uint8_t>& g711_data,
                                 const struct sockaddr_in& dest_addr, const std::string& call_id);
    void send_silence_rtp_packet(const std::string& call_id, int local_rtp_port);
    void send_silence_rtp_packet_sessionless(int rtp_port);
    void send_rtp_keepalive_packets();
    int get_actual_sip_port();
    void send_bye_response(const std::string& call_id, const std::string& from, const std::string& to,
                          const std::string& via, int cseq, const struct sockaddr_in& dest_addr);


    // Network configuration
    std::string detect_local_ip();
    void update_local_ip();
    void parse_and_display_user_info(const std::string& sip_response);
    void query_extension_user_info(const std::string& extension);
    void parse_options_response_for_user_info(const std::string& response);

private:
    Database* database_;
    bool running_;
    int specific_line_id_; // -1 means process all enabled lines
    std::thread sip_thread_;
    std::thread connection_monitor_thread_;

    // RTP thread management
    std::vector<std::thread> rtp_threads_;
    std::mutex rtp_threads_mutex_;

    // UDP Forwarding to/from Standalone Audio Processors
    int inbound_udp_sock_ = -1;
    struct sockaddr_in inbound_processor_addr_;
    int outbound_udp_sock_ = -1;
    std::thread outbound_audio_listener_thread_;
    void outbound_audio_listener_loop();

    // Process helpers
    std::string get_executable_dir();

    // Outbound sessionless audio management
    std::mutex audio_routing_mutex_;
    // RTP transmission state
    std::unordered_map<std::string, uint16_t> rtp_sequence_; // call_id -> sequence number
    std::unordered_map<std::string, uint32_t> rtp_timestamp_; // call_id -> timestamp
    std::unordered_map<std::string, uint32_t> rtp_ssrc_;      // call_id -> SSRC (randomized per call)
    std::unordered_map<std::string, std::pair<std::string, int>> rtp_destinations_; // call_id -> (ip, port)
    std::unordered_map<int, int> rtp_port_to_socket_; // rtp_port -> socket_fd (for symmetric RTP)
    std::unordered_map<std::string, int> rtp_selected_pt_; // call_id -> payload type (0=PCMU, 8=PCMA)
    std::mutex rtp_state_mutex_;

    // Outbound TTS de-dup (drop identical payloads within a short window)
    uint64_t last_tts_hash_ = 0;
    size_t last_tts_size_ = 0;
    std::chrono::steady_clock::time_point last_tts_time_;
    std::mutex tts_dedup_mutex_;




    // SIP networking
    int sip_listen_socket_;
    int sip_listen_port_;
    std::string local_ip_;
    std::thread sip_listener_thread_;

    // Registration state tracking
    std::map<int, bool> line_registered_; // line_id -> is_registered
    std::map<int, std::chrono::steady_clock::time_point> last_registration_; // line_id -> last_reg_time
    std::mutex registration_mutex_;

    // Status update tracking to avoid spam
    std::map<int, std::string> last_status_; // line_id -> last_status
    std::mutex status_mutex_;

    // Active calls
    std::map<std::string, SipCallSession> active_calls_;
    std::mutex calls_mutex_;

    // SIP line management
    std::vector<SipLineConfig> sip_lines_;
    std::mutex sip_lines_mutex_;

    // Number format handling (RFC 3966, E.164)
    std::string extract_phone_number(const std::string& sip_header);

    // REGISTER response forwarding
    std::mutex register_response_mutex_;
    std::condition_variable register_response_cv_;
    std::string pending_register_response_;
    bool register_response_ready_ = false;

    // Call ID mappings
    std::unordered_map<int, std::string> call_num_id_to_call_id_;
    std::unordered_map<std::string, int> call_id_to_rtp_port_;
    std::string current_call_id_;
    int current_call_num_id_ = 0;

    // Main loops
    void sip_management_loop();
    void connection_monitor_loop();

    // SIP line connection management
    void load_sip_lines_from_database(bool verbose = true);
    bool test_sip_connection(const SipLineConfig& line);
    void update_line_status(int line_id, const std::string& status);

    // SIP Digest Authentication
    std::string create_digest_response(const std::string& username, const std::string& password,
                                     const std::string& realm, const std::string& nonce,
                                     const std::string& method, const std::string& uri);
    std::string create_digest_response_with_qop(const std::string& username, const std::string& password,
                                              const std::string& realm, const std::string& nonce,
                                              const std::string& method, const std::string& uri,
                                              const std::string& qop, const std::string& nc, const std::string& cnonce);
    bool parse_www_authenticate(const std::string& auth_header, std::string& realm, std::string& nonce);
    bool send_authenticated_register(const SipLineConfig& line, const std::string& realm, const std::string& nonce, bool supports_qop = false, const std::string& call_id = "");

    // Incoming call handling
    void handle_sip_message(const std::string& message, const struct sockaddr_in& sender_addr);
    void handle_invite(const std::string& message, const struct sockaddr_in& sender_addr);
    void handle_ack(const std::string& message, const struct sockaddr_in& sender_addr);
    void handle_notify(const std::string& message, const struct sockaddr_in& sender_addr);
    void handle_bye(const std::string& message, const struct sockaddr_in& sender_addr);
    void send_sip_response(int code, const std::string& reason, const std::string& call_id, const std::string& from, const std::string& to, const std::string& via, int cseq, const struct sockaddr_in& dest_addr, int line_id = -1);


    // Port management - use call_id directly (dynamic allocation)
    int get_caller_port(int caller_id) const {
        // Legacy method - kept for compatibility
        return 10000 + caller_id;
    }

    // Dynamic RTP port calculation from line_id
    int calculate_rtp_port(int line_id) const {
        int port = 10000 + line_id;
        std::cout << "🔢 RTP port for line " << line_id << ": " << port << " (10000 + " << line_id << ")" << std::endl;
        return port;
    }
};

// Global variables for signal handling
static bool g_running = true;
static bool g_shutdown_in_progress = false;
static std::unique_ptr<SimpleSipClient> g_sip_client;

void signal_handler(int signal) {
    // Prevent double shutdown
    if (g_shutdown_in_progress) {
        std::cout << "\nShutdown already in progress, forcing exit..." << std::endl;
        std::cout << "🛑 Force terminating process..." << std::endl;
        _exit(1); // Force exit without cleanup if second signal
    }

    g_shutdown_in_progress = true;
    std::cout << "\n🛑 Received signal " << signal << ", shutting down gracefully..." << std::endl;

    // Immediately stop the SIP client to prevent hanging
    if (g_sip_client) {
        std::cout << "🛑 Stopping SIP client from signal handler..." << std::endl;
        g_sip_client->stop();
        std::cout << "🛑 SIP client stopped from signal handler" << std::endl;
    }

    g_running = false;
    std::cout << "🛑 Signal handler complete - exiting cleanly" << std::endl;

    // Exit immediately after cleanup
    std::exit(0);
}

void print_usage() {
    std::cout << "Usage: sip-client [options]\n"
              << "Options:\n"
              << "  --db PATH          Database file path (default: whisper_talk.db)\n"
              << "  --help             Show this help message\n"
              << "\nNote: SIP line configurations are read from the database.\n"
              << "      Use the web interface to configure SIP lines.\n"
              << "      RTP ports are auto-assigned as 10000 + line_id\n";
}

int main(int argc, char** argv) {
    std::string db_path = "whisper_talk.db";
    int specific_line_id = -1; // -1 means process all enabled lines

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--db" && i + 1 < argc) {
            db_path = argv[++i];

        } else if (arg == "--line-id" && i + 1 < argc) {
            specific_line_id = std::atoi(argv[++i]);
        } else if (arg == "--help") {
            print_usage();
            return 0;
        } else {
            std::cerr << "Unknown argument: " << arg << std::endl;
            print_usage();
            return 1;
        }
    }

    std::cout << "📞 Starting Whisper Talk LLaMA SIP Client..." << std::endl;
    std::cout << "   Database: " << db_path << std::endl;
    if (specific_line_id != -1) {
        std::cout << "   Target Line ID: " << specific_line_id << " (single line mode)" << std::endl;
    } else {
        std::cout << "   Target: All enabled lines" << std::endl;
    }

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

    // Create SIP client
    g_sip_client = std::make_unique<SimpleSipClient>();
    if (!g_sip_client->init(&database, specific_line_id)) {
        std::cerr << "❌ Failed to initialize SIP client!" << std::endl;
        return 1;
    }
    std::cout << "✅ SIP client initialized" << std::endl;

    // Start SIP client
    if (!g_sip_client->start()) {
        std::cerr << "❌ Failed to start SIP client!" << std::endl;
        return 1;
    }
    std::cout << "🚀 SIP client started and ready for calls" << std::endl;
    std::cout << "Press Ctrl+C to stop..." << std::endl;

    // Main loop
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10)); // Fast response
    }

    std::cout << "🛑 Shutting down SIP client..." << std::endl;
    if (g_sip_client) {
        g_sip_client->stop();
        g_sip_client.reset();
        std::cout << "🛑 SIP client stopped and reset" << std::endl;
    } else {
        std::cout << "🛑 SIP client was null (unexpected)" << std::endl;
    }
    database.close();
    std::cout << "✅ SIP client stopped cleanly" << std::endl;

    return 0;
}

// SimpleSipClient implementation
SimpleSipClient::SimpleSipClient() : database_(nullptr), running_(false),
                                   sip_listen_socket_(-1), sip_listen_port_(0), local_ip_("") {
    // Auto-detect local IP on startup
    update_local_ip();
}

SimpleSipClient::~SimpleSipClient() {
    std::cout << "🛑 SimpleSipClient destructor called" << std::endl;

    // Ensure cleanup happens even if stop() wasn't called
    if (running_) {
        std::cout << "🛑 SIP client still running in destructor - forcing stop" << std::endl;
        stop();
    }

    // Force close any remaining sockets
    if (sip_listen_socket_ >= 0) {
        std::cout << "🛑 Force closing SIP socket in destructor" << std::endl;
        close(sip_listen_socket_);
        sip_listen_socket_ = -1;
    }

    std::cout << "🛑 SimpleSipClient destructor complete" << std::endl;
}

bool SimpleSipClient::init(Database* database, int specific_line_id) {
    database_ = database;
    specific_line_id_ = specific_line_id;

    if (!database_) {
        std::cerr << "❌ Database is required for SIP client" << std::endl;
        return false;
    }

    // Load SIP lines from database
    load_sip_lines_from_database();

    return true;
}

bool SimpleSipClient::setup_sip_listener() {
    std::cout << "🔧 Setting up SIP listener..." << std::endl;

    // Create listening socket that will be used for both registration and listening
    sip_listen_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (sip_listen_socket_ < 0) {
        std::cout << "❌ Failed to create SIP listening socket: " << strerror(errno) << std::endl;
        return false;
    }

    // Enable SO_REUSEPORT to allow multiple sockets on same port (macOS/BSD)
    int reuse = 1;
    if (setsockopt(sip_listen_socket_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        std::cout << "❌ Failed to set SO_REUSEPORT on listener: " << strerror(errno) << std::endl;
        close(sip_listen_socket_);
        sip_listen_socket_ = -1;
        return false;
    }

    // Bind to any available port (let OS choose)
    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = 0; // Let OS choose port

    if (bind(sip_listen_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cout << "❌ Failed to bind SIP socket: " << strerror(errno) << std::endl;
        close(sip_listen_socket_);
        sip_listen_socket_ = -1;
        return false;
    }

    // Get the actual port assigned by OS
    socklen_t addr_len = sizeof(addr);
    if (getsockname(sip_listen_socket_, (struct sockaddr*)&addr, &addr_len) < 0) {
        std::cout << "❌ Failed to get socket name: " << strerror(errno) << std::endl;
        close(sip_listen_socket_);
        sip_listen_socket_ = -1;
        return false;
    }

    sip_listen_port_ = ntohs(addr.sin_port);
    std::cout << "🔌 OS allocated dynamic SIP port: " << sip_listen_port_ << std::endl;
    std::cout << "✅ SIP listener bound to port " << sip_listen_port_ << std::endl;

    // Initialize UDP socket for forwarding to Inbound Audio Processor
    inbound_udp_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (inbound_udp_sock_ < 0) {
        std::cout << "⚠️ Failed to create UDP socket for inbound audio forwarding: " << strerror(errno) << std::endl;
    } else {
        memset(&inbound_processor_addr_, 0, sizeof(inbound_processor_addr_));
        inbound_processor_addr_.sin_family = AF_INET;
        inbound_processor_addr_.sin_port = htons(9001); // Standard port for standalone Inbound Processor
        inbound_processor_addr_.sin_addr.s_addr = inet_addr("127.0.0.1");
        std::cout << "📡 Inbound audio forwarding initialized to 127.0.0.1:9001" << std::endl;
    }

    // Initialize UDP socket for receiving from Outbound Audio Processor
    outbound_udp_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (outbound_udp_sock_ < 0) {
        std::cout << "❌ Failed to create UDP socket for outbound audio reception: " << strerror(errno) << std::endl;
        return false;
    }

    struct sockaddr_in outbound_addr{};
    outbound_addr.sin_family = AF_INET;
    outbound_addr.sin_addr.s_addr = INADDR_ANY;
    outbound_addr.sin_port = htons(9002); // Standard port for SIP Client to receive G.711 packets

    int opt = 1;
    setsockopt(outbound_udp_sock_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(outbound_udp_sock_, (struct sockaddr*)&outbound_addr, sizeof(outbound_addr)) < 0) {
        std::cout << "❌ Failed to bind outbound audio reception socket to port 9002: " << strerror(errno) << std::endl;
        close(outbound_udp_sock_);
        outbound_udp_sock_ = -1;
        return false;
    }
    std::cout << "📡 Outbound audio reception initialized on port 9002" << std::endl;

    return true;
}

int SimpleSipClient::allocate_dynamic_port() {
    // Legacy method - kept for compatibility, but should use allocate_rtp_port_for_call() instead
    std::cout << "⚠️ Using legacy allocate_dynamic_port() - consider using call_id-based allocation" << std::endl;
    return 10000; // Fallback to standard RTP port
}

int SimpleSipClient::allocate_rtp_port_for_call(const std::string& call_id) {
    // Get line_id for this call (use specific_line_id_ or default to 1)
    int line_id = (specific_line_id_ != -1) ? specific_line_id_ : 1;

    int rtp_port = calculate_rtp_port(line_id);

    // Test if port is available
    int test_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (test_sock < 0) {
        std::cout << "⚠️ Failed to create test socket, using calculated port anyway: " << rtp_port << std::endl;
        return rtp_port;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(rtp_port);

    if (bind(test_sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
        close(test_sock);
        std::cout << "🎯 Allocated line-specific RTP port: " << rtp_port << " for line " << line_id << " (call " << call_id << ")" << std::endl;
        return rtp_port;
    } else {
        close(test_sock);
        std::cout << "⚠️ Port " << rtp_port << " not available, using anyway for line " << line_id << " (call " << call_id << ")" << std::endl;
        return rtp_port; // Use calculated port even if busy (will handle conflicts later)
    }
}

bool SimpleSipClient::setup_rtp_listener(int rtp_port, int call_num_id) {
    // If we already have a live socket/thread for this RTP port, reuse it across calls
    {
        std::lock_guard<std::mutex> lock(rtp_state_mutex_);
        auto it = rtp_port_to_socket_.find(rtp_port);
        if (it != rtp_port_to_socket_.end() && it->second >= 0) {
            std::cout << "✅ RTP port " << rtp_port << " already active — reusing existing socket/thread" << std::endl;
            return true;
        }
    }

    // Create a basic UDP socket to listen on the RTP port
    // This makes the port "active" so the PBX can send audio to it
    int rtp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (rtp_sock < 0) {
        std::cout << "⚠️ Failed to create RTP socket for port " << rtp_port << std::endl;
        return false;
    }

    struct sockaddr_in rtp_addr;
    memset(&rtp_addr, 0, sizeof(rtp_addr));
    rtp_addr.sin_family = AF_INET;
    rtp_addr.sin_addr.s_addr = INADDR_ANY;
    rtp_addr.sin_port = htons(rtp_port);

    // Enable address reuse for rapid rebinds across calls
    int opt = 1;
    setsockopt(rtp_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(rtp_sock, (struct sockaddr*)&rtp_addr, sizeof(rtp_addr)) < 0) {
        std::cout << "⚠️ Failed to bind RTP socket to port " << rtp_port << " (" << strerror(errno) << ")" << std::endl;
        close(rtp_sock);
        return false;
    }

    // Store socket for symmetric RTP (same socket for send/receive)
    {
        std::lock_guard<std::mutex> lock(rtp_state_mutex_);
        rtp_port_to_socket_[rtp_port] = rtp_sock;
    }

    // Keep the socket open and start RTP processing thread
    std::cout << "✅ RTP port " << rtp_port << " is ready for media (socket kept open)" << std::endl;

    // Start a simple RTP receiver thread for this port
    {
        std::lock_guard<std::mutex> lock(rtp_threads_mutex_);
        rtp_threads_.emplace_back([this, rtp_sock, rtp_port, call_num_id]() {
        char buffer[2048];
        struct sockaddr_in sender_addr;
        socklen_t sender_len = sizeof(sender_addr);

        std::cout << "🎧 RTP receiver thread started for port " << rtp_port << " (Call num_id: " << call_num_id << ")" << std::endl;

        // Set timeout on RTP socket so we can check running_ periodically
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        setsockopt(rtp_sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        int packet_count = 0;
        while (running_) {
            ssize_t received = recvfrom(rtp_sock, buffer, sizeof(buffer), 0,
                                      (struct sockaddr*)&sender_addr, &sender_len);
            if (received > 0) {
                packet_count++;

                // Log first packet and then every 100th packet to avoid spam
                if (packet_count == 1) {
                    std::string source_ip = inet_ntoa(sender_addr.sin_addr);
                    int source_port = ntohs(sender_addr.sin_port);
                    std::cout << "🎧 RTP audio stream started: " << received << " bytes from "
                             << source_ip << ":" << source_port << std::endl;

                    // Store RTP destination for sending RTP back (multi-call aware)
                    {
                        std::lock_guard<std::mutex> lock(rtp_state_mutex_);
                        std::string cid_key = std::to_string(call_num_id);
                        rtp_destinations_[cid_key] = {source_ip, source_port};
                        // Also keep "default" for legacy fallback
                        rtp_destinations_["default"] = {source_ip, source_port};
                        std::cout << "🎯 Stored RTP destination for call_num_id " << call_num_id << ": " << source_ip << ":" << source_port << std::endl;
                    }
                }

                // Forward raw RTP packet to standalone InboundAudioProcessor via UDP
                if (received >= 12) {
                    route_rtp_to_processor(call_num_id, (uint8_t*)buffer, received);
                }
            } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                // Transient UDP error (not timeout) — keep socket alive and continue
                if (running_) {
                    std::cout << "❌ RTP receiver error on port " << rtp_port << ": " << strerror(errno) << " — continuing" << std::endl;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            // received == 0 or timeout - continue loop to check running_ flag
        }

        {
            std::lock_guard<std::mutex> lock(rtp_state_mutex_);
            rtp_port_to_socket_.erase(rtp_port);
        }
        close(rtp_sock);
        std::cout << "🔌 RTP receiver thread ended for port " << rtp_port << std::endl;
        });
    }
    return true;
}

void SimpleSipClient::sip_listener_loop() {
    std::cout << "👂 Starting SIP listener on port " << sip_listen_port_ << std::endl;
    // std::cout << "🔍 SIP listener socket fd: " << sip_listen_socket_ << std::endl;

    // Basic socket validation
    if (sip_listen_socket_ < 0) {
        std::cout << "❌ INVALID SOCKET FD: " << sip_listen_socket_ << std::endl;
        return;
    }

    if (sip_listen_port_ <= 0) {
        std::cout << "❌ INVALID PORT: " << sip_listen_port_ << std::endl;
        return;
    }

    char buffer[4096];
    struct sockaddr_in sender_addr;
    socklen_t addr_len = sizeof(sender_addr);

    int loop_count = 0;
    while (running_) {
        loop_count++;

        // Debug: Show listener is active every 30 seconds
        if (loop_count % 30 == 0) {
            // std::cout << "👂 SIP listener active (waiting for packets on port " << sip_listen_port_ << ")..." << std::endl;  // suppressed
        }

        // Check for IP changes every 60 seconds
        if (loop_count % 60 == 0) {
            update_local_ip();
        }

        // Set a timeout so we can check running_ periodically
        struct timeval timeout;
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;
        setsockopt(sip_listen_socket_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        ssize_t received = recvfrom(sip_listen_socket_, buffer, sizeof(buffer) - 1, 0,
                                   (struct sockaddr*)&sender_addr, &addr_len);

        if (received > 0) {
            buffer[received] = '\0';
            std::string message(buffer);

            // std::cout << "📨 Received SIP message (" << received << " bytes)" << std::endl;
            // std::cout << "📋 From: " << inet_ntoa(sender_addr.sin_addr) << ":" << ntohs(sender_addr.sin_port) << std::endl;
            // std::cout << "📄 Message preview: " << message.substr(0, std::min(100, (int)message.length())) << "..." << std::endl;

            // Handle the SIP message
            handle_sip_message(message, sender_addr);
        } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            if (running_) {
                std::cout << "❌ SIP listener error: " << strerror(errno) << " (errno: " << errno << ")" << std::endl;
            }
            break;
        }
        // received == 0 or timeout - continue loop
    }

    std::cout << "👂 SIP listener stopped" << std::endl;
}

void SimpleSipClient::handle_sip_message(const std::string& message, const struct sockaddr_in& sender_addr) {
    // std::cout << "🔍 Parsing SIP message..." << std::endl;

    // Check if it's a SIP response (starts with "SIP/2.0")
    if (message.find("SIP/2.0") == 0) {
        // Check if it's a REGISTER response by looking for CSeq: REGISTER
        if (message.find("CSeq:") != std::string::npos &&
            (message.find("REGISTER") != std::string::npos || message.find("register") != std::string::npos)) {
            // std::cout << "📋 REGISTER response - forwarding to registration handler" << std::endl;
            // Forward this message to any waiting registration function
            // We'll use a simple approach: store the response for pickup
            {
                std::lock_guard<std::mutex> lock(register_response_mutex_);
                pending_register_response_ = message;
                register_response_ready_ = true;
            }
            register_response_cv_.notify_all();
            return;
        }
        // std::cout << "📋 SIP response: " << message.substr(0, message.find('\r')) << std::endl;
        return;
    }

    // Check if it's an INVITE (incoming call)
    if (message.find("INVITE ") == 0) {
        std::cout << "📞 Incoming INVITE detected!" << std::endl;
        handle_invite(message, sender_addr);
    }
    // Check if it's a BYE (call termination)
    else if (message.find("BYE ") == 0) {
        std::cout << "📞 Call termination (BYE) received" << std::endl;
        handle_bye(message, sender_addr);
    }
    // Check if it's an ACK
    else if (message.find("ACK ") == 0) {
        std::cout << "✅ ACK received - call established" << std::endl;
        handle_ack(message, sender_addr);
    }
    // Check if it's a NOTIFY (message waiting indication, etc.)
    else if (message.find("NOTIFY ") == 0) {
        std::cout << "📬 NOTIFY received - sending 200 OK" << std::endl;
        handle_notify(message, sender_addr);
    }
    // Other SIP messages
    else {
        // std::cout << "📋 Other SIP message: " << message.substr(0, message.find('\r')) << std::endl;
    }
}

void SimpleSipClient::handle_invite(const std::string& message, const struct sockaddr_in& sender_addr) {
    std::cout << "📞 Processing INVITE message..." << std::endl;

    // Parse key SIP headers
    std::string call_id, from, to, via, contact;
    int cseq = 0;

    std::istringstream iss(message);
    std::string line;

    while (std::getline(iss, line)) {
        if (line.find("Call-ID:") == 0) {
            call_id = line.substr(9);
            // Remove \r if present
            if (!call_id.empty() && call_id.back() == '\r') {
                call_id.pop_back();
            }
        } else if (line.find("From:") == 0) {
            from = line.substr(6);
            if (!from.empty() && from.back() == '\r') {
                from.pop_back();
            }
        } else if (line.find("To:") == 0) {
            to = line.substr(4);
            if (!to.empty() && to.back() == '\r') {
                to.pop_back();
            }
        } else if (line.find("Via:") == 0) {
            via = line.substr(5);
            if (!via.empty() && via.back() == '\r') {
                via.pop_back();
            }
        } else if (line.find("CSeq:") == 0) {
            std::string cseq_line = line.substr(6);
            if (!cseq_line.empty() && cseq_line.back() == '\r') {
                cseq_line.pop_back();
            }
            cseq = std::stoi(cseq_line.substr(0, cseq_line.find(' ')));
        }
    }

    std::cout << "📋 INVITE Details:" << std::endl;
    std::cout << "   Call-ID: " << call_id << std::endl;
    std::cout << "   From: " << from << std::endl;
    std::cout << "   To: " << to << std::endl;
    std::cout << "   CSeq: " << cseq << std::endl;

    // Get line_id for database-driven headers
    int line_id = (specific_line_id_ != -1) ? specific_line_id_ : 1;

    // Send 180 Ringing first (proper SIP call progression)
    std::cout << "📞 Sending 180 Ringing..." << std::endl;
    send_sip_response(180, "Ringing", call_id, from, to, via, cseq, sender_addr, line_id);

    // Extract caller number from From header using RFC-compliant parsing
    std::string caller_number = extract_phone_number(from);
    std::cout << "📞 Extracted caller number: " << caller_number << " (from: " << from << ")" << std::endl;

    // Launch audio processors BEFORE accepting call
    std::cout << "🚀 Pre-launching audio processors for call " << call_id << std::endl;
    handle_incoming_call(caller_number, call_id);

    // Wait for audio processors to initialize and connect to services
    std::cout << "⏳ Waiting for audio processors to initialize and connect to services..." << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(1500));

    // Now send 200 OK response to accept the call
    std::cout << "📞 Sending 200 OK (audio processors ready)..." << std::endl;
    send_sip_response(200, "OK", call_id, from, to, via, cseq, sender_addr, line_id);

    // Create and store call state for proper BYE handling (sessionless)
    {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        SipCallSession call_session;
        call_session.call_id = call_id;  // Store SIP Call-ID
        call_session.caller_id = -1; // Will be set in handle_incoming_call
        call_session.phone_number = caller_number;
        call_session.status = "active";
        call_session.start_time = std::chrono::system_clock::now();
        // Get line_id for this call
        int line_id = (specific_line_id_ != -1) ? specific_line_id_ : 1;
        call_session.internal_port = calculate_rtp_port(line_id); // Dynamic RTP port based on line_id

        active_calls_[call_id] = call_session;
        std::cout << "📞 Call state stored: " << call_id << " (status: active)" << std::endl;
    }
}

void SimpleSipClient::send_sip_response(int code, const std::string& reason, const std::string& call_id,
                                       const std::string& from, const std::string& to, const std::string& via,
                                       int cseq, const struct sockaddr_in& dest_addr, int line_id) {
    std::cout << "📤 Sending SIP " << code << " " << reason << " response..." << std::endl;

    // Create SIP response
    std::ostringstream response;
    response << "SIP/2.0 " << code << " " << reason << "\r\n";
    response << "Via: " << via << "\r\n";
    response << "From: " << from << "\r\n";
    response << "To: " << to << ";tag=tag-" << rand() % 10000 << "\r\n";
    response << "Call-ID: " << call_id << "\r\n";
    response << "CSeq: " << cseq << " INVITE\r\n";
    // Generate headers from database content if line_id provided
    std::string contact_user = "whisper"; // fallback
    std::string user_agent = "Whisper-Talk-LLaMA/1.0"; // fallback

    if (line_id > 0 && database_) {
        auto sip_lines = database_->get_all_sip_lines();
        for (const auto& line : sip_lines) {
            if (line.line_id == line_id) {
                contact_user = line.username;
                user_agent = "Whisper-Talk-LLaMA/1.0";
                break;
            }
        }
    }

    // Use the actual current SIP socket port, not the stored port
    int actual_sip_port = get_actual_sip_port();
    // std::cout << "🔍 Contact header using port: " << actual_sip_port << " (stored: " << sip_listen_port_ << ")" << std::endl;
    // Use local IP for Contact header (auto-detected)
    response << "Contact: <sip:" << contact_user << "@" << local_ip_ << ":" << actual_sip_port << ">\r\n";
    response << "Allow: INVITE, ACK, CANCEL, BYE, NOTIFY, REFER, MESSAGE, OPTIONS, INFO, SUBSCRIBE\r\n";
    response << "Supported: replaces, timer\r\n";
    response << "User-Agent: " << user_agent << "\r\n";

    // Only add SDP and allocate RTP port for 200 OK responses
    if (code == 200) {
        response << "Content-Type: application/sdp\r\n";

        // Allocate call-specific RTP port based on call_id
        int rtp_port = allocate_rtp_port_for_call(call_id);
        std::cout << "🎵 Allocated call-specific RTP port: " << rtp_port << " for call " << call_id << std::endl;

        // Resolve numeric call_id from active_calls_ for multi-call UDP routing
        int call_num_id = 0;
        {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            auto it = active_calls_.find(call_id);
            if (it != active_calls_.end()) {
                call_num_id = it->second.call_num_id;
            }
        }

        // Set up RTP listener on the allocated port (basic UDP socket)
        if (setup_rtp_listener(rtp_port, call_num_id)) {
            std::cout << "🎧 RTP listener set up on port " << rtp_port << " for call " << call_id << " (num_id: " << call_num_id << ")" << std::endl;
        } else {
            std::cout << "❌ Failed to set up RTP listener on port " << rtp_port << " for call " << call_id << std::endl;
        }

        // Store RTP port for this call_id (will be connected to session later)
        {
            std::lock_guard<std::mutex> lock(audio_routing_mutex_);
            call_id_to_rtp_port_[call_id] = rtp_port;
        }

        // Add SDP for audio with DTMF support (RFC 4733 compliant)
        // Offer both PCMU(0) and PCMA(8) to match PBX preference
        std::string sdp =
            "v=0\r\n"
            "o=whisper 123456 654321 IN IP4 " + local_ip_ + "\r\n"
            "s=Whisper Talk Session\r\n"
            "c=IN IP4 " + local_ip_ + "\r\n"
            "t=0 0\r\n"
            "m=audio " + std::to_string(rtp_port) + " RTP/AVP 0 8 101\r\n"
            "a=rtpmap:0 PCMU/8000\r\n"
            "a=rtpmap:8 PCMA/8000\r\n"
            "a=rtpmap:101 telephone-event/8000\r\n"
            "a=fmtp:101 0-15\r\n"
            "a=sendrecv\r\n";

        response << "Content-Length: " << sdp.length() << "\r\n";
        response << "\r\n";
        response << sdp;
    } else {
        // For non-200 responses (like 180 Ringing), no SDP
        response << "Content-Length: 0\r\n";
        response << "\r\n";
    }

    std::string response_str = response.str();

    // Debug: Show exactly what we're sending
    // std::cout << "🔍 SIP Response being sent:" << std::endl;
    // std::cout << "---BEGIN SIP RESPONSE---" << std::endl;
    // std::cout << response_str << std::endl;
    // std::cout << "---END SIP RESPONSE---" << std::endl;

    // Send response using the existing listening socket (for consistent source port)
    if (sip_listen_socket_ >= 0) {
        ssize_t sent = sendto(sip_listen_socket_, response_str.c_str(), response_str.length(), 0,
                             (struct sockaddr*)&dest_addr, sizeof(dest_addr));

        if (sent > 0) {
            std::cout << "✅ SIP response sent (" << sent << " bytes)" << std::endl;
        } else {
            std::cout << "❌ Failed to send SIP response: " << strerror(errno) << std::endl;
        }
        // Don't close the socket - it's the listening socket
    } else {
        std::cout << "❌ SIP listening socket not available for response" << std::endl;
    }
}

void SimpleSipClient::handle_bye(const std::string& message, const struct sockaddr_in& sender_addr) {
    std::cout << "📞 Processing BYE message..." << std::endl;

    // Parse key SIP headers from BYE message
    std::string call_id, from, to, via;
    int cseq = 0;

    std::istringstream iss(message);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.find("Call-ID:") == 0) {
            call_id = line.substr(9);
            call_id.erase(0, call_id.find_first_not_of(" \t"));
            call_id.erase(call_id.find_last_not_of(" \t\r\n") + 1);
        } else if (line.find("From:") == 0) {
            from = line.substr(5);
            from.erase(0, from.find_first_not_of(" \t"));
            from.erase(from.find_last_not_of(" \t\r\n") + 1);
        } else if (line.find("To:") == 0) {
            to = line.substr(3);
            to.erase(0, to.find_first_not_of(" \t"));
            to.erase(to.find_last_not_of(" \t\r\n") + 1);
        } else if (line.find("Via:") == 0) {
            via = line.substr(4);
            via.erase(0, via.find_first_not_of(" \t"));
            via.erase(via.find_last_not_of(" \t\r\n") + 1);
        } else if (line.find("CSeq:") == 0) {
            std::string cseq_line = line.substr(5);
            cseq_line.erase(0, cseq_line.find_first_not_of(" \t"));
            cseq = std::stoi(cseq_line);
        }
    }

    // Send simple 200 OK response to BYE (no SDP content)
    send_bye_response(call_id, from, to, via, cseq, sender_addr);

    // Sessionless BYE handling - just clean up RTP resources
    std::cout << "🔚 Processing BYE for call: " << call_id << " (sessionless)" << std::endl;

    // Clean up RTP port mapping for this call
    {
        std::lock_guard<std::mutex> lock(audio_routing_mutex_);
        auto rtp_it = call_id_to_rtp_port_.find(call_id);
        if (rtp_it != call_id_to_rtp_port_.end()) {
            int rtp_port = rtp_it->second;
            std::cout << "🎵 Cleaning up RTP port " << rtp_port << " for call " << call_id << std::endl;
            call_id_to_rtp_port_.erase(rtp_it);
        }
    }

    // End call in database
    if (database_) {
        database_->end_call(call_id);
    }

    // SHM-based cleanup for this call
    end_call(call_id);

    std::cout << "✅ BYE processed successfully (sessionless)" << std::endl;
}





bool SimpleSipClient::start() {
    std::cout << "🚀 SimpleSipClient::start() called" << std::endl;

    if (running_) {
        std::cout << "⚠️ SIP client already running" << std::endl;
        return false;
    }

    // Setup SIP listener first
    if (!setup_sip_listener()) {
        std::cout << "❌ Failed to setup SIP listener" << std::endl;
        return false;
    }
    std::cout << "✅ SIP listener setup complete" << std::endl;

    running_ = true;

    std::cout << "🚀 Starting SIP threads..." << std::endl;
    sip_thread_ = std::thread(&SimpleSipClient::sip_management_loop, this);
    std::cout << "✅ SIP management thread started" << std::endl;

    sip_listener_thread_ = std::thread(&SimpleSipClient::sip_listener_loop, this);
    std::cout << "✅ SIP listener thread started" << std::endl;

    connection_monitor_thread_ = std::thread(&SimpleSipClient::connection_monitor_loop, this);
    std::cout << "✅ Connection monitor thread started" << std::endl;

    outbound_audio_listener_thread_ = std::thread(&SimpleSipClient::outbound_audio_listener_loop, this);
    std::cout << "✅ Outbound audio listener thread started" << std::endl;

    return true;
}

void SimpleSipClient::stop() {
    if (!running_) {
        std::cout << "🛑 SIP client already stopped, returning" << std::endl;
        return; // Already stopped
    }

    std::cout << "🛑 Stopping SIP client..." << std::endl;
    std::cout << "🛑 Setting running_ = false" << std::endl;
    running_ = false;

    // Close sockets to unblock threads
    std::cout << "🛑 Closing network sockets..." << std::endl;
    if (sip_listen_socket_ >= 0) {
        close(sip_listen_socket_);
        sip_listen_socket_ = -1;
    }
    if (outbound_udp_sock_ >= 0) {
        close(outbound_udp_sock_);
        outbound_udp_sock_ = -1;
    }
    if (inbound_udp_sock_ >= 0) {
        close(inbound_udp_sock_);
        inbound_udp_sock_ = -1;
    }

    // Join threads safely with timeout
    std::cout << "🛑 Starting thread cleanup with 5 second timeout..." << std::endl;
    auto cleanup_start = std::chrono::steady_clock::now();
    const auto timeout = std::chrono::seconds(5);

    try {
        if (sip_thread_.joinable()) {
            std::cout << "🛑 Joining SIP management thread..." << std::endl;
            sip_thread_.join();
            std::cout << "🛑 SIP management thread joined" << std::endl;
        }

        if (sip_listener_thread_.joinable()) {
            std::cout << "🛑 Joining SIP listener thread..." << std::endl;
            sip_listener_thread_.join();
            std::cout << "🛑 SIP listener thread joined" << std::endl;
        }

        if (connection_monitor_thread_.joinable()) {
            std::cout << "🛑 Joining connection monitor thread..." << std::endl;
            connection_monitor_thread_.join();
            std::cout << "🛑 Connection monitor thread joined" << std::endl;
        }

        if (outbound_audio_listener_thread_.joinable()) {
            std::cout << "🛑 Joining outbound audio listener thread..." << std::endl;
            outbound_audio_listener_thread_.join();
            std::cout << "🛑 Outbound audio listener thread joined" << std::endl;
        }

        // Join all RTP threads with timeout check
        {
            std::lock_guard<std::mutex> lock(rtp_threads_mutex_);
            std::cout << "🛑 Joining " << rtp_threads_.size() << " RTP threads..." << std::endl;
            for (size_t i = 0; i < rtp_threads_.size(); ++i) {
                if (std::chrono::steady_clock::now() - cleanup_start > timeout) {
                    std::cout << "⚠️ RTP thread cleanup timeout - skipping remaining threads" << std::endl;
                    break;
                }

                if (rtp_threads_[i].joinable()) {
                    std::cout << "🛑 Joining RTP thread " << (i+1) << "..." << std::endl;
                    rtp_threads_[i].join();
                    std::cout << "🛑 RTP thread " << (i+1) << " joined" << std::endl;
                }
            }
            rtp_threads_.clear();
            std::cout << "🛑 All RTP threads cleaned up" << std::endl;
        }
    } catch (const std::exception& e) {
        std::cout << "⚠️ Thread join error: " << e.what() << std::endl;
    }

    std::cout << "✅ SIP client stopped" << std::endl;
}

std::string SimpleSipClient::extract_phone_number(const std::string& sip_header) {
    // Extract phone number from various SIP header formats:
    // - "Display Name" <sip:+15551234567@domain.com>
    // - <sip:1001@192.168.1.1>
    // - sip:+49301234567@starface.local
    // - tel:+1-555-123-4567

    std::string number;

    // Find the SIP URI or tel URI
    size_t sip_start = sip_header.find("sip:");
    size_t tel_start = sip_header.find("tel:");

    if (tel_start != std::string::npos) {
        // Handle tel: URI format (RFC 3966)
        size_t start = tel_start + 4;
        size_t end = sip_header.find_first_of(" \t\r\n>", start);
        if (end == std::string::npos) end = sip_header.length();

        number = sip_header.substr(start, end - start);

        // Remove tel: URI formatting (hyphens, dots, spaces)
        std::string clean_number;
        for (char c : number) {
            if (std::isdigit(c) || c == '+') {
                clean_number += c;
            }
        }
        number = clean_number;

    } else if (sip_start != std::string::npos) {
        // Handle sip: URI format
        size_t start = sip_start + 4;
        size_t at_pos = sip_header.find('@', start);

        if (at_pos != std::string::npos) {
            number = sip_header.substr(start, at_pos - start);
        }
    }

    // Clean up the number (remove non-digit characters except +)
    std::string clean_number;
    for (char c : number) {
        if (std::isdigit(c) || c == '+') {
            clean_number += c;
        }
    }

    // Handle various number formats
    if (clean_number.empty()) {
        return "unknown";
    }

    // If it starts with +, it's E.164 international format
    if (clean_number[0] == '+') {
        return clean_number;
    }

    // If it's a short extension (3-4 digits), keep as is
    if (clean_number.length() <= 4) {
        return clean_number;
    }

    // For longer numbers without +, assume it needs country code
    // This is configurable based on your PBX setup
    if (clean_number.length() >= 10) {
        // Assume US/Canada format if 10+ digits without country code
        return "+" + clean_number;
    }

    return clean_number;
}

void SimpleSipClient::handle_incoming_call(const std::string& caller_number, const std::string& call_id) {
    std::cout << "📞 Incoming call from: " << caller_number << std::endl;

    if (!database_) {
        std::cerr << "❌ No database connection available" << std::endl;
        return;
    }

    // Step 1: Get or create caller in database
    std::cout << "🔍 Looking up caller in database: " << caller_number << std::endl;
    int caller_id = database_->get_or_create_caller(caller_number);
    if (caller_id < 0) {
        std::cerr << "❌ Failed to create caller record for: " << caller_number << std::endl;
        return;
    }
    std::cout << "✅ Caller ID: " << caller_id << std::endl;

    // Step 2: Create call record in database
    int line_id = (specific_line_id_ != -1) ? specific_line_id_ : 1;
    if (!database_->create_call(call_id, caller_id, line_id, caller_number)) {
        std::cerr << "❌ Failed to create call record for: " << call_id << std::endl;
        return;
    }
    std::cout << "📞 Call record created in database: " << call_id << std::endl;
    // Resolve numeric call id from database row id for consistent port mapping
    Call db_call = database_->get_call(call_id);
    int call_num_id = db_call.id > 0 ? db_call.id : 0;
    std::string call_num_id_str = std::to_string(call_num_id);


    // Step 3: Register call locally (sessionless)
    {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        current_call_id_ = call_id;
        current_call_num_id_ = call_num_id;

        SipCallSession session;
        session.call_id = call_id;
        session.call_num_id = call_num_id;
        session.caller_id = caller_id;
        session.phone_number = caller_number;
        session.status = "active";
        session.start_time = std::chrono::system_clock::now();
        active_calls_[call_id] = session;
        call_num_id_to_call_id_[call_num_id] = call_id;
    }

    // Notify standalone services via control commands (if needed) or simple UDP poke
    // whisper_register_notify is still useful to pre-warm whisper
    whisper_register_notify(call_num_id);

    // Sessionless: Simple RTP port registration using call_id
    if (!call_id.empty()) {
        int rtp_port = -1;
        {
            std::lock_guard<std::mutex> lock(audio_routing_mutex_);
            auto rtp_it = call_id_to_rtp_port_.find(call_id);
            if (rtp_it != call_id_to_rtp_port_.end()) {
                rtp_port = rtp_it->second;
                // Sessionless: No need to map RTP port to session
                call_id_to_rtp_port_.erase(rtp_it);
                std::cout << "🎵 Registered RTP port " << rtp_port << " → call " << call_id << std::endl;
            }
        }


    }

    // Step 3: Assign unique port for this caller (sessionless)
    int caller_port = get_caller_port(caller_id);
    std::cout << "✅ Call setup complete for caller_id: " << caller_id << " (port: " << caller_port << ")" << std::endl;
    std::cout << "📱 Call answered automatically (sessionless). Active on port " << caller_port << std::endl;
    std::cout << "🎤 Ready to receive audio for call: " << call_id << " (port: " << caller_port << ")" << std::endl;

    // Real call is now active - audio will be processed when RTP packets arrive
    // Call will be ended when SIP BYE is received
}

void SimpleSipClient::end_call(const std::string& call_id) {
    std::cout << "📞 Ending call: " << call_id << " (sessionless)" << std::endl;

    {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto it = active_calls_.find(call_id);
        if (it != active_calls_.end()) {
            call_num_id_to_call_id_.erase(it->second.call_num_id);
            active_calls_.erase(it);
        }
    }

    // Reset current call context if matching
    {
        std::lock_guard<std::mutex> lock(audio_routing_mutex_);
        if (current_call_id_ == call_id) {
            current_call_id_.clear();
            current_call_num_id_ = 0;
        }
    }

    // RTP state cleanup
    {
        std::lock_guard<std::mutex> lock(rtp_state_mutex_);
        rtp_sequence_.erase(call_id);
        rtp_timestamp_.erase(call_id);
        rtp_ssrc_.erase(call_id);
        rtp_destinations_.erase(call_id);
        rtp_selected_pt_.erase(call_id);
    }

    std::cout << "🧹 Call cleanup complete (sessionless)" << std::endl;
    std::cout << "✅ Call ended successfully" << std::endl;
}

// Removed: process_rtp_audio - replaced with direct sessionless routing

void SimpleSipClient::stream_audio_from_piper(const std::string& call_id, const std::vector<uint8_t>& audio_data) {
    std::cout << "🔊 Streaming " << audio_data.size() << " bytes of audio from Piper for call " << call_id << std::endl;

    // Sessionless: Use current line's RTP port if available
    int rtp_port = -1;

    // Find active RTP port (sessionless)
    {
        std::lock_guard<std::mutex> lock(rtp_state_mutex_);
        if (!rtp_port_to_socket_.empty()) {
            // Use first available RTP port (sessionless approach)
            rtp_port = rtp_port_to_socket_.begin()->first;
        }
    }

    if (rtp_port == -1) {
        std::cout << "❌ No active RTP session available (sessionless)" << std::endl;
        return;
    }

    // Choose outbound codec/PT based on inbound RTP (mirror 0=PCMU or 8=PCMA)
    int selected_pt = 0; // default PCMU
    {
        std::lock_guard<std::mutex> lock(rtp_state_mutex_);
        auto it = rtp_selected_pt_.find("default");
        if (it != rtp_selected_pt_.end() && (it->second == 0 || it->second == 8)) {
            selected_pt = it->second;
        }
    }

    std::vector<uint8_t> out_data;
    if (selected_pt == 8) {
        // Convert incoming µ-law to A-law to match PBX expectation
        out_data = convert_ulaw_to_alaw(audio_data);
        std::cout << "🎚️ Converted µ-law->A-law for outbound RTP (PT=8)" << std::endl;
    } else {
        out_data = audio_data;
    }

    // De-duplicate: drop identical payloads within 10 seconds window
    auto fnv1a64 = [](const std::vector<uint8_t>& d) -> uint64_t {
        uint64_t h = 1469598103934665603ull; // FNV offset basis
        for (uint8_t b : d) {
            h ^= (uint64_t)b;
            h *= 1099511628211ull; // FNV prime
        }
        return h;
    };
    const uint64_t h = fnv1a64(out_data);
    {
        std::lock_guard<std::mutex> lk(tts_dedup_mutex_);
        auto now = std::chrono::steady_clock::now();
        if (h == last_tts_hash_ && out_data.size() == last_tts_size_ &&
            last_tts_time_.time_since_epoch().count() != 0 &&
            std::chrono::duration_cast<std::chrono::seconds>(now - last_tts_time_).count() < 10) {
            std::cout << "⚠️ Dropped duplicate TTS payload (same hash/size within 10s)" << std::endl;
            return;
        }
        last_tts_hash_ = h;
        last_tts_size_ = out_data.size();
        last_tts_time_ = now;
    }

    // Send using proper call_id for RTP state management
    send_rtp_packets_to_pbx(call_id, out_data, rtp_port);
}

// SIP Line Management Implementation
void SimpleSipClient::load_sip_lines_from_database(bool verbose) {
    std::lock_guard<std::mutex> lock(sip_lines_mutex_);
    sip_lines_ = database_->get_all_sip_lines();

    if (verbose) {
        std::cout << "📋 Loaded " << sip_lines_.size() << " SIP lines from database:" << std::endl;
        for (const auto& line : sip_lines_) {
            std::cout << "   Line " << line.line_id << ": " << line.username
                      << " @ " << line.server_ip << ":" << line.server_port
                      << " (status: " << line.status << ")" << std::endl;
        }
    }
}


// ==== Child process helpers: auto-launch/terminate audio processors per call ====
std::string SimpleSipClient::get_executable_dir() {
    char buf[PATH_MAX];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) != 0) {
        return ".";
    }
    char resolved[PATH_MAX];
    if (!realpath(buf, resolved)) {
        return ".";
    }
    std::string full(resolved);
    auto pos = full.find_last_of('/');
    if (pos == std::string::npos) return ".";
    return full.substr(0, pos);
}

bool SimpleSipClient::test_sip_connection(const SipLineConfig& line) {
    // Ensure SIP listener is ready before attempting registration
    if (sip_listen_port_ <= 0) {
        std::cout << "⚠️ SIP listener not ready yet, skipping registration for line " << line.line_id << std::endl;
        return false;
    }

    // std::cout << "\n🔍 ===== TESTING SIP REGISTRATION =====" << std::endl;
    // std::cout << "🔌 Using dynamic SIP port: " << sip_listen_port_ << std::endl;
    // std::cout << "📋 Line Details:" << std::endl;
    // std::cout << "   Line ID: " << line.line_id << std::endl;
    // std::cout << "   Extension: " << line.extension << std::endl;
    // std::cout << "   Username: " << line.username << std::endl;
    // std::cout << "   Password: " << line.password << std::endl;
    // std::cout << "   Server: " << line.server_ip << ":" << line.server_port << std::endl;
    // std::cout << "   Display Name: " << line.display_name << std::endl;
    // std::cout << "   Enabled: " << (line.enabled ? "YES" : "NO") << std::endl;
    // std::cout << "   Current Status: " << line.status << std::endl;

    if (!line.enabled) {
        std::cout << "⚠️  Line is disabled, skipping SIP registration" << std::endl;
        return false;
    }

    std::cout << "🔌 Creating UDP socket for SIP registration..." << std::endl;
    int sock = socket(AF_INET, SOCK_DGRAM, 0); // Create dedicated socket for registration
    if (sock < 0) {
        std::cout << "❌ Failed to create UDP socket: " << strerror(errno) << std::endl;
        return false;
    }

    // Enable SO_REUSEPORT to allow multiple sockets on same port (macOS/BSD)
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        std::cout << "❌ Failed to set SO_REUSEPORT: " << strerror(errno) << std::endl;
        close(sock);
        return false;
    }

    // Bind to the same port as our listener so PBX sees consistent source port
    struct sockaddr_in local_addr;
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(sip_listen_port_);

    if (bind(sock, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        std::cout << "❌ Failed to bind registration socket to port " << sip_listen_port_ << ": " << strerror(errno) << std::endl;
        close(sock);
        return false;
    }
    std::cout << "✅ Registration socket bound to port " << sip_listen_port_ << std::endl;
    std::cout << "✅ UDP socket created successfully (fd: " << sock << ")" << std::endl;

    std::cout << "🌐 Setting up server address..." << std::endl;
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(line.server_port);
    std::cout << "   Family: AF_INET" << std::endl;
    std::cout << "   Port: " << line.server_port << " (network order: " << ntohs(server_addr.sin_port) << ")" << std::endl;

    // Convert IP address or resolve hostname
    std::cout << "🔍 Resolving address: " << line.server_ip << std::endl;

    // First try direct IP address conversion
    int inet_result = inet_pton(AF_INET, line.server_ip.c_str(), &server_addr.sin_addr);
    if (inet_result == 1) {
        std::cout << "✅ Direct IP address conversion successful" << std::endl;
    } else {
        // Try hostname resolution
        std::cout << "🔍 Attempting hostname resolution for: " << line.server_ip << std::endl;

        struct addrinfo hints, *result;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;  // IPv4
        hints.ai_socktype = SOCK_DGRAM;

        int getaddr_result = getaddrinfo(line.server_ip.c_str(), nullptr, &hints, &result);
        if (getaddr_result != 0) {
            std::cout << "❌ Hostname resolution failed: " << gai_strerror(getaddr_result) << std::endl;
            close(sock);
            return false;
        }

        // Use the first result
        struct sockaddr_in* addr_in = (struct sockaddr_in*)result->ai_addr;
        server_addr.sin_addr = addr_in->sin_addr;

        char ip_str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &server_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
        std::cout << "✅ Hostname resolved to: " << ip_str << std::endl;

        freeaddrinfo(result);
    }

    // Set socket timeout
    std::cout << "⏱️  Setting socket timeouts (3 seconds)..." << std::endl;
    struct timeval timeout;
    timeout.tv_sec = 3;  // 3 second timeout
    timeout.tv_usec = 0;
    if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        std::cout << "⚠️  Failed to set receive timeout: " << strerror(errno) << std::endl;
    }
    if (setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0) {
        std::cout << "⚠️  Failed to set send timeout: " << strerror(errno) << std::endl;
    }

    // Create SIP REGISTER message
    std::cout << "📝 Creating SIP REGISTER message..." << std::endl;

    std::string call_id = "whisper-talk-" + std::to_string(time(nullptr));
    std::string from_tag = "tag-" + std::to_string((rand() % 9000) + 1000);

    std::ostringstream sip_register;
    sip_register << "REGISTER sip:" << line.server_ip << " SIP/2.0\r\n";
    sip_register << "Via: SIP/2.0/UDP " << local_ip_ << ":" << sip_listen_port_ << ";branch=z9hG4bK-" << ((rand() % 9000) + 1000) << "\r\n";
    sip_register << "From: <sip:" << line.username << "@" << line.server_ip << ">;tag=" << from_tag << "\r\n";
    sip_register << "To: <sip:" << line.username << "@" << line.server_ip << ">\r\n";
    sip_register << "Call-ID: " << call_id << "\r\n";
    sip_register << "CSeq: 1 REGISTER\r\n";
    sip_register << "Contact: <sip:" << line.username << "@" << local_ip_ << ":" << sip_listen_port_ << ">\r\n";
    sip_register << "Max-Forwards: 70\r\n";
    sip_register << "User-Agent: Whisper-Talk-LLaMA/2.0\r\n";
    sip_register << "Expires: 3600\r\n";
    sip_register << "Content-Length: 0\r\n";
    sip_register << "\r\n";

    std::string register_msg = sip_register.str();
    // std::cout << "📤 SIP REGISTER message created (" << register_msg.length() << " bytes)" << std::endl;
    // std::cout << "📋 Message preview:" << std::endl;
    // std::cout << "   REGISTER sip:" << line.server_ip << " SIP/2.0" << std::endl;
    // std::cout << "   From: " << line.username << "@" << line.server_ip << std::endl;
    // std::cout << "   Call-ID: " << call_id << std::endl;

    // Send SIP REGISTER message
    std::cout << "📡 Sending SIP REGISTER to " << line.server_ip << ":" << line.server_port << "..." << std::endl;
    auto start_time = std::chrono::steady_clock::now();

    ssize_t sent_bytes = sendto(sock, register_msg.c_str(), register_msg.length(), 0,
                               (struct sockaddr*)&server_addr, sizeof(server_addr));

    if (sent_bytes < 0) {
        std::cout << "❌ Failed to send SIP REGISTER: " << strerror(errno) << std::endl;
        close(sock);
        return false;
    }

    std::cout << "✅ SIP REGISTER sent successfully (" << sent_bytes << " bytes)" << std::endl;

    // Wait for response from the SIP listener thread
    std::cout << "⏳ Waiting for SIP response..." << std::endl;

    // Clear any previous response
    {
        std::lock_guard<std::mutex> lock(register_response_mutex_);
        register_response_ready_ = false;
        pending_register_response_.clear();
    }

    // Wait for the listener thread to forward the response
    std::unique_lock<std::mutex> lock(register_response_mutex_);
    bool response_received = register_response_cv_.wait_for(lock, std::chrono::seconds(5),
        [this] { return register_response_ready_; });

    if (!response_received) {
        std::cout << "❌ No SIP response received (timeout after 5000ms)" << std::endl;
        std::cout << "   Error: Timeout waiting for REGISTER response" << std::endl;
        std::cout << "===== SIP REGISTRATION TIMEOUT =====\n" << std::endl;
        close(sock);
        return false;
    }

    std::string response = pending_register_response_;
    register_response_ready_ = false;
    lock.unlock();

    ssize_t received_bytes = response.length();

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    close(sock);

    if (received_bytes > 0) {
        std::cout << "✅ SIP response received (" << received_bytes << " bytes, " << duration.count() << "ms)" << std::endl;

        // Response is already available as string from listener thread
        std::cout << "📥 SIP Response:" << std::endl;

        // Show first line of response
        size_t first_line_end = response.find("\r\n");
        if (first_line_end != std::string::npos) {
            std::string status_line = response.substr(0, first_line_end);
            std::cout << "   " << status_line << std::endl;

            // Check if it's a success response (2xx)
            if (status_line.find("SIP/2.0 2") != std::string::npos) {
                std::cout << "🎉 SIP REGISTRATION SUCCESSFUL!" << std::endl;
                std::cout << "===== SIP REGISTRATION COMPLETE =====\n" << std::endl;

                // Immediately update database status to "connected"
                update_line_status(line.line_id, "connected");

                return true;
            } else if (status_line.find("SIP/2.0 401") != std::string::npos ||
                      status_line.find("SIP/2.0 407") != std::string::npos) {
                std::cout << "🔐 Authentication challenge received - implementing digest auth" << std::endl;

                // Find WWW-Authenticate header
                std::string www_auth_line;
                size_t auth_pos = response.find("WWW-Authenticate:");
                if (auth_pos == std::string::npos) {
                    auth_pos = response.find("Proxy-Authenticate:");
                }

                if (auth_pos != std::string::npos) {
                    size_t line_end = response.find("\r\n", auth_pos);
                    if (line_end != std::string::npos) {
                        www_auth_line = response.substr(auth_pos, line_end - auth_pos);
                    }
                }

                if (www_auth_line.empty()) {
                    std::cout << "❌ No WWW-Authenticate header found" << std::endl;
                    std::cout << "===== SIP REGISTRATION FAILED =====\n" << std::endl;
                    return false;
                }

                // Parse authentication parameters
                std::string realm, nonce;
                if (!parse_www_authenticate(www_auth_line, realm, nonce)) {
                    std::cout << "❌ Failed to parse authentication parameters" << std::endl;
                    std::cout << "===== SIP REGISTRATION FAILED =====\n" << std::endl;
                    return false;
                }

                // Check if PBX supports qop
                bool supports_qop = (www_auth_line.find("qop=") != std::string::npos);
                std::cout << "🔍 PBX supports qop: " << (supports_qop ? "YES" : "NO") << std::endl;

                // Send authenticated REGISTER with same Call-ID
                std::cout << "🔐 Sending authenticated REGISTER..." << std::endl;

                // Reuse the same Call-ID from initial request (SIP RFC requirement)
                return send_authenticated_register(line, realm, nonce, supports_qop, call_id);
            } else {
                std::cout << "❌ SIP registration failed" << std::endl;
                std::cout << "===== SIP REGISTRATION FAILED =====\n" << std::endl;
                return false;
            }
        }
    } else {
        std::cout << "❌ No SIP response received (timeout after " << duration.count() << "ms)" << std::endl;
        std::cout << "   Error: " << strerror(errno) << std::endl;
        std::cout << "===== SIP REGISTRATION TIMEOUT =====\n" << std::endl;
        return false;
    }

    return false;
}

void SimpleSipClient::update_line_status(int line_id, const std::string& status) {
    // Check if status actually changed to avoid spam
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        if (last_status_[line_id] == status) {
            return; // Status hasn't changed, skip update
        }
        last_status_[line_id] = status;
    }

    if (database_->update_sip_line_status(line_id, status)) {
        std::cout << "📊 Line " << line_id << " status: " << status << std::endl;
    } else {
        std::cerr << "❌ Failed to update status for line " << line_id << std::endl;
    }
}

void SimpleSipClient::sip_management_loop() {
    std::cout << "📞 Starting SIP management loop (ready for real calls)..." << std::endl;

    while (running_) {
        // Fast loop for real-time SIP processing
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        if (!running_) break;
    }

    std::cout << "📞 SIP management loop stopped" << std::endl;
}

void SimpleSipClient::connection_monitor_loop() {
    // std::cout << "🔍 Starting SIP connection monitor..." << std::endl;

    // Wait a moment for SIP listener to be ready
    std::this_thread::sleep_for(std::chrono::seconds(2));

    // Initialize all line statuses on startup
    load_sip_lines_from_database(false);
    {
        std::lock_guard<std::mutex> lock(sip_lines_mutex_);
        for (const auto& line : sip_lines_) {
            if (!line.enabled) {
                update_line_status(line.line_id, "disabled");
            } else {
                update_line_status(line.line_id, "disconnected");
            }
        }
    }

    while (running_) {
        // Reload SIP lines from database (in case they were updated via web interface)
        load_sip_lines_from_database(false); // Silent reload

        // Test connections for enabled lines
        {
            std::lock_guard<std::mutex> lock(sip_lines_mutex_);
            for (const auto& line : sip_lines_) {
                // If specific line ID is set, only process that line
                if (specific_line_id_ != -1 && line.line_id != specific_line_id_) {
                    continue;
                }

                if (!line.enabled) {
                    // Always update disabled lines to disabled status
                    update_line_status(line.line_id, "disabled");
                    // Mark as not registered
                    std::lock_guard<std::mutex> reg_lock(registration_mutex_);
                    line_registered_[line.line_id] = false;
                    continue;
                }

                // Check if line is already registered
                bool is_registered = false;
                bool needs_refresh = false;
                {
                    std::lock_guard<std::mutex> reg_lock(registration_mutex_);
                    is_registered = line_registered_[line.line_id];

                    // Check if registration needs refresh (every 30 minutes)
                    auto now = std::chrono::steady_clock::now();
                    if (is_registered && last_registration_.count(line.line_id)) {
                        auto time_since_reg = std::chrono::duration_cast<std::chrono::minutes>(
                            now - last_registration_[line.line_id]);
                        needs_refresh = (time_since_reg.count() >= 30);
                    }
                }

                if (!is_registered || needs_refresh) {
                    // Only register if not registered or needs refresh
                    std::cout << "📞 " << (is_registered ? "Refreshing" : "Registering")
                              << " SIP line " << line.line_id << std::endl;

                    update_line_status(line.line_id, "connecting");
                    bool connected = test_sip_connection(line);

                    if (connected) {
                        std::lock_guard<std::mutex> reg_lock(registration_mutex_);
                        line_registered_[line.line_id] = true;
                        last_registration_[line.line_id] = std::chrono::steady_clock::now();
                        update_line_status(line.line_id, "connected");
                        std::cout << "✅ SIP line " << line.line_id << " registered successfully" << std::endl;
                    } else {
                        std::lock_guard<std::mutex> reg_lock(registration_mutex_);
                        line_registered_[line.line_id] = false;
                        update_line_status(line.line_id, "error");
                        std::cout << "❌ SIP line " << line.line_id << " registration failed" << std::endl;
                    }
                } else {
                    // Already registered and doesn't need refresh - ensure status is "connected"
                    update_line_status(line.line_id, "connected");
                    std::cout << "✅ SIP line " << line.line_id << " already registered (keeping alive)" << std::endl;
                }
            }
        }

        // Send RTP keepalive packets for active sessions
        send_rtp_keepalive_packets();

        // Wait 5 minutes before next connection check cycle (since we maintain registration)
        for (int i = 0; i < 300 && running_; ++i) {
            std::this_thread::sleep_for(std::chrono::seconds(1));

            // Send RTP keepalive every 20 seconds during wait
            if (i % 20 == 0) {
                send_rtp_keepalive_packets();
            }
        }
    }
}

std::string SimpleSipClient::create_digest_response(const std::string& username, const std::string& password,
                                                   const std::string& realm, const std::string& nonce,
                                                   const std::string& method, const std::string& uri) {
    // Calculate HA1 = MD5(username:realm:password)
    std::string ha1_input = username + ":" + realm + ":" + password;
    std::string ha1 = calculate_md5(ha1_input);

    // Calculate HA2 = MD5(method:uri)
    std::string ha2_input = method + ":" + uri;
    std::string ha2 = calculate_md5(ha2_input);

    // Calculate response = MD5(HA1:nonce:HA2)
    std::string response_input = ha1 + ":" + nonce + ":" + ha2;
    std::string response = calculate_md5(response_input);

    // std::cout << "🔐 Digest Authentication:" << std::endl;
    // std::cout << "   Username: " << username << std::endl;
    // std::cout << "   Password: " << password << std::endl;
    // std::cout << "   Realm: " << realm << std::endl;
    // std::cout << "   Nonce: " << nonce << std::endl;
    // std::cout << "   HA1 Input: " << ha1_input << std::endl;
    // std::cout << "   HA1: " << ha1 << std::endl;
    // std::cout << "   HA2: " << ha2 << std::endl;
    // std::cout << "   Response: " << response << std::endl;

    return response;
}

bool SimpleSipClient::parse_www_authenticate(const std::string& auth_header, std::string& realm, std::string& nonce) {
    // std::cout << "🔍 Parsing WWW-Authenticate header:" << std::endl;
    // std::cout << "   " << auth_header << std::endl;

    // Extract realm
    size_t realm_pos = auth_header.find("realm=\"");
    if (realm_pos != std::string::npos) {
        realm_pos += 7; // Skip 'realm="'
        size_t realm_end = auth_header.find("\"", realm_pos);
        if (realm_end != std::string::npos) {
            realm = auth_header.substr(realm_pos, realm_end - realm_pos);
        }
    }

    // Extract nonce
    size_t nonce_pos = auth_header.find("nonce=\"");
    if (nonce_pos != std::string::npos) {
        nonce_pos += 7; // Skip 'nonce="'
        size_t nonce_end = auth_header.find("\"", nonce_pos);
        if (nonce_end != std::string::npos) {
            nonce = auth_header.substr(nonce_pos, nonce_end - nonce_pos);
        }
    }

    std::cout << "   Extracted realm: '" << realm << "'" << std::endl;
    std::cout << "   Extracted nonce: '" << nonce << "'" << std::endl;

    return !realm.empty() && !nonce.empty();
}

bool SimpleSipClient::send_authenticated_register(const SipLineConfig& line, const std::string& realm, const std::string& nonce, bool supports_qop, const std::string& call_id) {
    // std::cout << "\n🔐 ===== SENDING AUTHENTICATED REGISTER =====" << std::endl;

    // Create socket for authenticated request, bound to same port as listener
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cout << "❌ Failed to create UDP socket: " << strerror(errno) << std::endl;
        return false;
    }

    // Enable SO_REUSEPORT to allow multiple sockets on same port (macOS/BSD)
    int reuse = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        std::cout << "❌ Failed to set SO_REUSEPORT: " << strerror(errno) << std::endl;
        close(sock);
        return false;
    }

    // Bind to the same port as our listener so PBX sees consistent source port
    struct sockaddr_in local_addr;
    local_addr.sin_family = AF_INET;
    local_addr.sin_addr.s_addr = INADDR_ANY;
    local_addr.sin_port = htons(sip_listen_port_);

    if (bind(sock, (struct sockaddr*)&local_addr, sizeof(local_addr)) < 0) {
        std::cout << "❌ Failed to bind auth socket to port " << sip_listen_port_ << ": " << strerror(errno) << std::endl;
        close(sock);
        return false;
    }

    // Set up server address
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(line.server_port);

    // Resolve IP address or hostname
    int inet_result = inet_pton(AF_INET, line.server_ip.c_str(), &server_addr.sin_addr);
    if (inet_result != 1) {
        // Try hostname resolution
        struct addrinfo hints, *result;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;

        int getaddr_result = getaddrinfo(line.server_ip.c_str(), nullptr, &hints, &result);
        if (getaddr_result != 0) {
            std::cout << "❌ Failed to resolve address: " << line.server_ip << std::endl;
            return false;
        }

        struct sockaddr_in* addr_in = (struct sockaddr_in*)result->ai_addr;
        server_addr.sin_addr = addr_in->sin_addr;
        freeaddrinfo(result);
    }

    // Calculate digest response (with or without qop based on PBX support)
    std::string uri = "sip:" + line.server_ip;
    std::string digest_response;
    std::string cnonce, nc;

    if (supports_qop) {
        cnonce = std::to_string(rand() % 100000);
        nc = "00000001";
        digest_response = create_digest_response_with_qop(line.username, line.password, realm, nonce, "REGISTER", uri, "auth", nc, cnonce);
    } else {
        // std::cout << "🔐 Using simple digest (no qop) for STARFACE PBX" << std::endl;
        digest_response = create_digest_response(line.username, line.password, realm, nonce, "REGISTER", uri);
    }

    // Create authenticated SIP REGISTER message
    std::string actual_call_id = call_id.empty() ? ("whisper-talk-auth-" + std::to_string(time(nullptr))) : call_id;
    std::string from_tag = "tag-auth-" + std::to_string((rand() % 9000) + 1000);

    std::ostringstream sip_register;
    sip_register << "REGISTER sip:" << line.server_ip << " SIP/2.0\r\n";
    sip_register << "Via: SIP/2.0/UDP " << local_ip_ << ":" << sip_listen_port_ << ";branch=z9hG4bK-auth-" << ((rand() % 9000) + 1000) << "\r\n";
    sip_register << "From: <sip:" << line.username << "@" << line.server_ip << ">;tag=" << from_tag << "\r\n";
    sip_register << "To: <sip:" << line.username << "@" << line.server_ip << ">\r\n";
    sip_register << "Call-ID: " << actual_call_id << "\r\n";
    sip_register << "CSeq: 2 REGISTER\r\n";
    sip_register << "Contact: <sip:" << line.username << "@" << local_ip_ << ":" << sip_listen_port_ << ">\r\n";
    sip_register << "Authorization: Digest username=\"" << line.username << "\", realm=\"" << realm
                 << "\", nonce=\"" << nonce << "\", uri=\"" << uri << "\", response=\"" << digest_response
                 << "\", algorithm=MD5";
    if (supports_qop) {
        sip_register << ", qop=auth, nc=" << nc << ", cnonce=\"" << cnonce << "\"";
    }
    sip_register << "\r\n";
    sip_register << "Max-Forwards: 70\r\n";
    sip_register << "User-Agent: Whisper-Talk-LLaMA/2.0\r\n";
    sip_register << "Expires: 3600\r\n";
    sip_register << "Content-Length: 0\r\n";
    sip_register << "\r\n";

    std::string register_msg = sip_register.str();
    // std::cout << "📤 Authenticated REGISTER created (" << register_msg.length() << " bytes)" << std::endl;
    // std::cout << "📋 Authorization header included with digest response" << std::endl;
    // std::cout << "🔍 Raw SIP message being sent:" << std::endl;
    // std::cout << "---BEGIN SIP MESSAGE---" << std::endl;
    // std::cout << register_msg << std::endl;
    // std::cout << "---END SIP MESSAGE---" << std::endl;

    // Send authenticated REGISTER
    std::cout << "📡 Sending authenticated REGISTER..." << std::endl;
    auto start_time = std::chrono::steady_clock::now();

    ssize_t sent_bytes = sendto(sock, register_msg.c_str(), register_msg.length(), 0,
                               (struct sockaddr*)&server_addr, sizeof(server_addr));

    if (sent_bytes < 0) {
        std::cout << "❌ Failed to send authenticated REGISTER: " << strerror(errno) << std::endl;
        close(sock);
        return false;
    }

    std::cout << "✅ Authenticated REGISTER sent (" << sent_bytes << " bytes)" << std::endl;

    // Wait for final response from the SIP listener thread
    std::cout << "⏳ Waiting for authentication response..." << std::endl;

    // Clear any previous response
    {
        std::lock_guard<std::mutex> lock(register_response_mutex_);
        register_response_ready_ = false;
        pending_register_response_.clear();
    }

    // Wait for the listener thread to forward the response
    std::unique_lock<std::mutex> lock(register_response_mutex_);
    bool response_received = register_response_cv_.wait_for(lock, std::chrono::seconds(5),
        [this] { return register_response_ready_; });

    if (!response_received) {
        std::cout << "❌ No authentication response received (timeout after 5000ms)" << std::endl;
        std::cout << "   Error: Timeout waiting for authenticated REGISTER response" << std::endl;
        close(sock);
        return false;
    }

    std::string response = pending_register_response_;
    register_response_ready_ = false;
    lock.unlock();

    ssize_t received_bytes = response.length();

    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);

    close(sock);

    if (received_bytes > 0) {
        // Response is already available as string from listener thread

        std::cout << "✅ Authentication response received (" << received_bytes << " bytes, " << duration.count() << "ms)" << std::endl;

        // Parse final response
        size_t first_line_end = response.find("\r\n");
        if (first_line_end != std::string::npos) {
            std::string status_line = response.substr(0, first_line_end);
            std::cout << "📥 Final Response: " << status_line << std::endl;

            if (status_line.find("SIP/2.0 2") != std::string::npos) {
                std::cout << "🎉 SIP REGISTRATION SUCCESSFUL!" << std::endl;
                std::cout << "✅ Successfully authenticated with PBX using digest authentication" << std::endl;

                // Parse response headers to extract user information
                parse_and_display_user_info(response);

                // Query PBX for detailed user information about this extension
                query_extension_user_info(line.username);

                std::cout << "===== SIP REGISTRATION COMPLETE =====\n" << std::endl;

                // Immediately update database status to "connected"
                update_line_status(line.line_id, "connected");

                return true;
            } else {
                std::cout << "❌ Authentication failed: " << status_line << std::endl;
                // std::cout << "📋 Full response for debugging:" << std::endl;
                // std::cout << response << std::endl;
                // std::cout << "===== SIP REGISTRATION FAILED =====\n" << std::endl;
                return false;
            }
        }
    } else {
        std::cout << "❌ No authentication response received (timeout)" << std::endl;
        std::cout << "===== SIP REGISTRATION TIMEOUT =====\n" << std::endl;
        return false;
    }

    return false;
}

std::string SimpleSipClient::create_digest_response_with_qop(const std::string& username, const std::string& password,
                                                           const std::string& realm, const std::string& nonce,
                                                           const std::string& method, const std::string& uri,
                                                           const std::string& qop, const std::string& nc, const std::string& cnonce) {
    // Calculate HA1 = MD5(username:realm:password)
    std::string ha1_input = username + ":" + realm + ":" + password;
    std::string ha1 = calculate_md5(ha1_input);

    // Calculate HA2 = MD5(method:uri)
    std::string ha2_input = method + ":" + uri;
    std::string ha2 = calculate_md5(ha2_input);

    // With qop=auth: response = MD5(HA1:nonce:nc:cnonce:qop:HA2)
    std::string response_input = ha1 + ":" + nonce + ":" + nc + ":" + cnonce + ":" + qop + ":" + ha2;
    std::string response = calculate_md5(response_input);

    // std::cout << "🔐 Digest Authentication with QOP:" << std::endl;
    // std::cout << "   Username: " << username << std::endl;
    // std::cout << "   Password: " << password << std::endl;
    // std::cout << "   Realm: " << realm << std::endl;
    // std::cout << "   Nonce: " << nonce << std::endl;
    // std::cout << "   QOP: " << qop << std::endl;
    // std::cout << "   NC: " << nc << std::endl;
    // std::cout << "   CNonce: " << cnonce << std::endl;
    // std::cout << "   HA1: " << ha1 << std::endl;
    // std::cout << "   HA2: " << ha2 << std::endl;
    // std::cout << "   Response: " << response << std::endl;

    return response;
}

// Simple audio routing methods
// Removed: session registration functions - using sessionless routing

void SimpleSipClient::route_rtp_to_processor(int call_num_id, const uint8_t* rtp_data, size_t rtp_len) {
    if (inbound_udp_sock_ < 0) return;

    // Fast-path: Prepend 4-byte call_num_id to RTP packet and send via UDP
    uint8_t stack_buf[2048];
    if (rtp_len + 4 > sizeof(stack_buf)) return;

    uint32_t cid_net = htonl(static_cast<uint32_t>(call_num_id));
    memcpy(stack_buf, &cid_net, 4);
    memcpy(stack_buf + 4, rtp_data, rtp_len);

    sendto(inbound_udp_sock_, stack_buf, rtp_len + 4, 0,
           (struct sockaddr*)&inbound_processor_addr_, sizeof(inbound_processor_addr_));
}

void SimpleSipClient::send_rtp_packets_to_pbx(const std::string& call_id, const std::vector<uint8_t>& g711_data, int local_rtp_port) {
    // Get destination from captured RTP source address
    std::string dest_ip;
    int dest_port;

    static int debug_counter = 0;
    bool should_log = (++debug_counter <= 3 || debug_counter % 100 == 0);

    {
        std::lock_guard<std::mutex> lock(rtp_state_mutex_);
        // Try call_id first, then fall back to "default" for sessionless operation
        auto it = rtp_destinations_.find(call_id);
        if (it == rtp_destinations_.end()) {
            it = rtp_destinations_.find("default");
        }
        if (it != rtp_destinations_.end()) {
            dest_ip = it->second.first;
            dest_port = it->second.second;
            if (should_log) {
                std::cout << "📡 RTP send #" << debug_counter << ": " << g711_data.size() << " bytes to " << dest_ip << ":" << dest_port << " (port " << local_rtp_port << ")" << std::endl;
            }
        } else {
            if (should_log) {
                std::cout << "⚠️  RTP send #" << debug_counter << ": No destination found for call " << call_id << " (skipping)" << std::endl;
            }
            return;
        }
    }

    // Get existing RTP socket for symmetric RTP (same port for send/receive)
    int rtp_sock = -1;
    {
        std::lock_guard<std::mutex> lock(rtp_state_mutex_);
        auto it = rtp_port_to_socket_.find(local_rtp_port);
        if (it != rtp_port_to_socket_.end()) {
            rtp_sock = it->second;
        }
    }

    if (rtp_sock < 0) {
        std::cout << "❌ No RTP socket found for port " << local_rtp_port << std::endl;
        return;
    }

    // Decide outbound payload type (mirror inbound if known)
    int selected_pt = 0; // 0=PCMU default
    {
        std::lock_guard<std::mutex> lock(rtp_state_mutex_);
        auto it = rtp_selected_pt_.find(call_id);
        if (it != rtp_selected_pt_.end() && (it->second == 0 || it->second == 8)) {
            selected_pt = it->second;
        } else {
            auto it2 = rtp_selected_pt_.find("default");
            if (it2 != rtp_selected_pt_.end() && (it2->second == 0 || it2->second == 8)) {
                selected_pt = it2->second;
            }
        }
    }

    // Convert to match PT if needed: our SHM bytes are μ-law by convention
    std::vector<uint8_t> out_bytes;
    if (selected_pt == 8) {
        out_bytes = convert_ulaw_to_alaw(g711_data);
    } else {
        out_bytes = g711_data;
    }

    // Set up destination address
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(dest_port);
    inet_pton(AF_INET, dest_ip.c_str(), &dest_addr.sin_addr);

    // Send G.711 data as RTP packets (160 bytes per packet = 20ms)
    send_g711_as_rtp_packets(rtp_sock, out_bytes, dest_addr, call_id);

    // Don't close socket - it's shared with RTP receiver for symmetric RTP
}

void SimpleSipClient::send_rtp_packets_to_pbx_sessionless(const std::vector<uint8_t>& g711_data, int local_rtp_port) {
    // Use symmetric RTP destination captured from inbound RTP
    std::string dest_ip;
    int dest_port = -1;
    {
        std::lock_guard<std::mutex> lock(rtp_state_mutex_);
        auto it = rtp_destinations_.find("default");
        if (it != rtp_destinations_.end()) {
            dest_ip = it->second.first;
            dest_port = it->second.second;
        }
    }
    if (dest_ip.empty() || dest_port <= 0) {
        return;
    }

    // Reuse the existing RTP socket bound for receive (symmetric RTP)
    int rtp_sock = -1;
    {
        std::lock_guard<std::mutex> lock(rtp_state_mutex_);
        auto it = rtp_port_to_socket_.find(local_rtp_port);
        if (it != rtp_port_to_socket_.end()) {
            rtp_sock = it->second;
        }
    }
    if (rtp_sock < 0) {
        return;
    }

    // Destination address
    struct sockaddr_in dest_addr{};
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(dest_port);
    inet_pton(AF_INET, dest_ip.c_str(), &dest_addr.sin_addr);

    // Send using the same packetization/timing as call-bound path, keyed by "default"
    send_g711_as_rtp_packets(rtp_sock, g711_data, dest_addr, "default");
}

void SimpleSipClient::send_g711_as_rtp_packets(int rtp_sock, const std::vector<uint8_t>& g711_data,
                                              const struct sockaddr_in& dest_addr, const std::string& call_id) {
    const size_t RTP_PAYLOAD_SIZE = 160; // 160 bytes = 20ms of G.711 audio
    const uint32_t TIMESTAMP_INCREMENT = 160; // 160 samples per packet at 8kHz

    // Get/initialize RTP state for this call
    uint16_t sequence_num;
    uint32_t timestamp;
    {
        std::lock_guard<std::mutex> lock(rtp_state_mutex_);
        if (rtp_sequence_.find(call_id) == rtp_sequence_.end()) {
            rtp_sequence_[call_id] = rand() % 65536; // Random initial sequence
            rtp_timestamp_[call_id] = rand(); // Random initial timestamp
        }
        sequence_num = rtp_sequence_[call_id];
        timestamp = rtp_timestamp_[call_id];
    }

    // Decide outbound payload type (mirror inbound if known)
    int payload_type = 0; // 0=PCMU default
    {
        std::lock_guard<std::mutex> lock(rtp_state_mutex_);
        auto it = rtp_selected_pt_.find(call_id);
        if (it != rtp_selected_pt_.end() && (it->second == 0 || it->second == 8)) {
            payload_type = it->second;
        } else {
            // Fallback to sessionless default if using "default" key
            auto it2 = rtp_selected_pt_.find("default");
            if (it2 != rtp_selected_pt_.end() && (it2->second == 0 || it2->second == 8)) {
                payload_type = it2->second;
            }
        }
    }

    // Send G.711 data in 160-byte chunks (20ms packets)
    for (size_t offset = 0; offset < g711_data.size(); offset += RTP_PAYLOAD_SIZE) {
        size_t chunk_size = std::min(RTP_PAYLOAD_SIZE, g711_data.size() - offset);

        // Create RTP packet
        uint8_t rtp_packet[12 + RTP_PAYLOAD_SIZE]; // 12-byte header + payload

        // RTP Header (12 bytes)
        rtp_packet[0] = 0x80; // Version=2, Padding=0, Extension=0, CC=0
        rtp_packet[1] = (uint8_t)(payload_type & 0x7F); // Marker=0, Payload Type mirrored (0=PCMU, 8=PCMA)
        rtp_packet[2] = (sequence_num >> 8) & 0xFF;
        rtp_packet[3] = sequence_num & 0xFF;
        rtp_packet[4] = (timestamp >> 24) & 0xFF;
        rtp_packet[5] = (timestamp >> 16) & 0xFF;
        rtp_packet[6] = (timestamp >> 8) & 0xFF;
        rtp_packet[7] = timestamp & 0xFF;
        // SSRC: randomize per call-id the first time
        uint32_t ssrc;
        {
            std::lock_guard<std::mutex> lock(rtp_state_mutex_);
            auto itss = rtp_ssrc_.find(call_id);
            if (itss == rtp_ssrc_.end()) {
                uint32_t rnd = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
                if (rnd == 0) rnd = 0x12345678; // avoid zero SSRC
                rtp_ssrc_[call_id] = rnd;
                ssrc = rnd;
                std::cout << " SSRC initialized for call " << call_id << ": 0x" << std::hex << ssrc << std::dec << std::endl;
            } else {
                ssrc = itss->second;
            }
        }
        rtp_packet[8]  = (uint8_t)((ssrc >> 24) & 0xFF);
        rtp_packet[9]  = (uint8_t)((ssrc >> 16) & 0xFF);
        rtp_packet[10] = (uint8_t)((ssrc >> 8) & 0xFF);
        rtp_packet[11] = (uint8_t)(ssrc & 0xFF);

        // Copy G.711 payload
        memcpy(rtp_packet + 12, g711_data.data() + offset, chunk_size);

        // Send RTP packet
        ssize_t sent = sendto(rtp_sock, rtp_packet, 12 + chunk_size, 0,
                             (struct sockaddr*)&dest_addr, sizeof(dest_addr));

        if (sent < 0) {
            std::cout << "❌ Failed to send RTP packet" << std::endl;
            break;
        }

        // Update RTP state
        sequence_num++;
        timestamp += TIMESTAMP_INCREMENT;

        // Minimal pacing to prevent overwhelming the PBX (1ms instead of 20ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    // Update call RTP state
    {
        std::lock_guard<std::mutex> lock(rtp_state_mutex_);
        rtp_sequence_[call_id] = sequence_num;
        rtp_timestamp_[call_id] = timestamp;
    }
}

void SimpleSipClient::send_silence_rtp_packet(const std::string& call_id, int local_rtp_port) {
    // Create 160 bytes of G.711 μ-law silence (0xFF is silence in μ-law)
    std::vector<uint8_t> silence_data(160, 0xFF);

    // Send silence as regular RTP packet
    send_rtp_packets_to_pbx(call_id, silence_data, local_rtp_port);
}

void SimpleSipClient::send_silence_rtp_packet_sessionless(int rtp_port) {
    // Create 160 bytes of G.711 μ-law silence (0xFF is silence in μ-law)
    std::vector<uint8_t> silence_data(160, 0xFF);

    // Send silence as sessionless RTP packet
    send_rtp_packets_to_pbx_sessionless(silence_data, rtp_port);
}

void SimpleSipClient::send_rtp_keepalive_packets() {
    // Sessionless: Send silence packets for all active RTP ports
    std::vector<int> active_rtp_ports;

    {
        std::lock_guard<std::mutex> lock(rtp_state_mutex_);
        for (const auto& [rtp_port, socket_fd] : rtp_port_to_socket_) {
            active_rtp_ports.push_back(rtp_port);
        }
    }

    for (int rtp_port : active_rtp_ports) {
        // Always send a keepalive silence packet; outbound thread will send real audio when available
        send_silence_rtp_packet_sessionless(rtp_port);
    }
}

int SimpleSipClient::get_actual_sip_port() {
    // Get the actual port from the SIP socket
    struct sockaddr_in addr;
    socklen_t addr_len = sizeof(addr);

    if (getsockname(sip_listen_socket_, (struct sockaddr*)&addr, &addr_len) == 0) {
        return ntohs(addr.sin_port);
    }

    // Fallback to stored port if getsockname fails
    return sip_listen_port_;
}

void SimpleSipClient::send_bye_response(const std::string& call_id, const std::string& from, const std::string& to,
                                       const std::string& via, int cseq, const struct sockaddr_in& dest_addr) {
    // Create simple 200 OK response for BYE (no SDP content)
    std::ostringstream response;

    response << "SIP/2.0 200 OK\r\n";
    response << "Via: " << via << ";rport\r\n";
    response << "From: " << from << "\r\n";
    response << "To: " << to << "\r\n";
    response << "Call-ID: " << call_id << "\r\n";
    response << "CSeq: " << cseq << " BYE\r\n";  // Correct method for BYE response
    response << "User-Agent: Whisper-Talk-LLaMA/2.0\r\n";
    response << "Content-Length: 0\r\n";
    response << "\r\n";

    std::string response_str = response.str();

    std::cout << "🔍 BYE Response being sent:\n---BEGIN BYE RESPONSE---\n" << response_str << "---END BYE RESPONSE---" << std::endl;

    // Send response using the existing listening socket
    if (sip_listen_socket_ >= 0) {
        ssize_t sent = sendto(sip_listen_socket_, response_str.c_str(), response_str.length(), 0,
                             (struct sockaddr*)&dest_addr, sizeof(dest_addr));

        if (sent > 0) {
            std::cout << "✅ BYE response sent (" << sent << " bytes)" << std::endl;
        } else {
            std::cout << "❌ Failed to send BYE response: " << strerror(errno) << std::endl;
        }
    } else {
        std::cout << "❌ SIP listening socket not available for BYE response" << std::endl;
    }
}

void SimpleSipClient::handle_ack(const std::string& message, const struct sockaddr_in& sender_addr) {
    (void)message; (void)sender_addr; // Unused parameters
    std::cout << "📞 Processing ACK message - call fully established" << std::endl;

    // Sessionless ACK handling - call is now fully established
    std::cout << "🎉 Call fully established (sessionless)!" << std::endl;

    // No session management needed - audio processor already activated
    // RTP stream is already active and working
}

void SimpleSipClient::handle_notify(const std::string& message, const struct sockaddr_in& sender_addr) {
    std::cout << "📬 Processing NOTIFY message..." << std::endl;

    // Parse key SIP headers from NOTIFY
    std::string call_id, from, to, via, cseq_line;

    std::istringstream iss(message);
    std::string line;

    while (std::getline(iss, line)) {
        if (line.find("Call-ID:") == 0) {
            call_id = line;  // Keep the full "Call-ID: value" line
            if (!call_id.empty() && call_id.back() == '\r') {
                call_id.pop_back();
            }
        } else if (line.find("From:") == 0) {
            from = line;
            if (!from.empty() && from.back() == '\r') {
                from.pop_back();
            }
        } else if (line.find("To:") == 0) {
            to = line;
            if (!to.empty() && to.back() == '\r') {
                to.pop_back();
            }
        } else if (line.find("Via:") == 0) {
            via = line;
            if (!via.empty() && via.back() == '\r') {
                via.pop_back();
            }
        } else if (line.find("CSeq:") == 0) {
            cseq_line = line;
            if (!cseq_line.empty() && cseq_line.back() == '\r') {
                cseq_line.pop_back();
            }
        }
    }

    // Send 200 OK response to NOTIFY
    std::ostringstream response;
    response << "SIP/2.0 200 OK\r\n";
    response << via << ";rport\r\n";
    response << from << "\r\n";
    response << to << "\r\n";
    response << call_id << "\r\n";
    response << cseq_line << "\r\n";
    response << "User-Agent: Whisper-Talk-LLaMA/2.0\r\n";
    response << "Content-Length: 0\r\n";
    response << "\r\n";

    std::string response_str = response.str();

    // Send response using the SIP listening socket
    if (sip_listen_socket_ >= 0) {
        ssize_t sent = sendto(sip_listen_socket_, response_str.c_str(), response_str.length(), 0,
                             (struct sockaddr*)&sender_addr, sizeof(sender_addr));
        if (sent > 0) {
            std::cout << "✅ NOTIFY 200 OK sent (" << sent << " bytes)" << std::endl;
        } else {
            std::cout << "❌ Failed to send NOTIFY response: " << strerror(errno) << std::endl;
        }
    } else {
        std::cout << "❌ SIP listening socket not available for NOTIFY response" << std::endl;
    }
}

// Network configuration methods
std::string SimpleSipClient::detect_local_ip() {
    // Auto-detect local IP by connecting to a remote address (doesn't actually send data)
    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cout << "⚠️ Failed to create socket for IP detection, using localhost" << std::endl;
        return "127.0.0.1";
    }

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(53); // DNS port
    inet_pton(AF_INET, "8.8.8.8", &dest_addr.sin_addr); // Google DNS

    if (connect(sock, (struct sockaddr*)&dest_addr, sizeof(dest_addr)) < 0) {
        close(sock);
        std::cout << "⚠️ Failed to connect for IP detection, using localhost" << std::endl;
        return "127.0.0.1";
    }

    struct sockaddr_in local_addr;
    socklen_t addr_len = sizeof(local_addr);
    if (getsockname(sock, (struct sockaddr*)&local_addr, &addr_len) < 0) {
        close(sock);
        std::cout << "⚠️ Failed to get socket name for IP detection, using localhost" << std::endl;
        return "127.0.0.1";
    }

    close(sock);

    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &local_addr.sin_addr, ip_str, INET_ADDRSTRLEN);

    return std::string(ip_str);
}

void SimpleSipClient::parse_and_display_user_info(const std::string& sip_response) {
    std::cout << "👤 Parsing user registration information..." << std::endl;

    // Parse headers line by line
    std::istringstream response_stream(sip_response);
    std::string line;

    std::string contact_header;
    std::string to_header;
    std::string server_header;
    std::string expires_header;
    std::string user_agent_header;
    std::string display_name;
    std::string phone_number;

    while (std::getline(response_stream, line)) {
        // Remove \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.find("Contact:") == 0) {
            contact_header = line;
        } else if (line.find("To:") == 0) {
            to_header = line;
        } else if (line.find("Server:") == 0) {
            server_header = line;
        } else if (line.find("Expires:") == 0) {
            expires_header = line;
        } else if (line.find("User-Agent:") == 0) {
            user_agent_header = line;
        } else if (line.find("P-Asserted-Identity:") == 0) {
            // STARFACE might use this for user identity
            std::cout << "👤 User Identity: " << line << std::endl;
        } else if (line.find("Remote-Party-ID:") == 0) {
            // Alternative user identity header
            std::cout << "👤 Remote Party: " << line << std::endl;
        } else if (line.find("X-") == 0) {
            // Custom headers that might contain user info
            std::cout << "🔧 Custom Header: " << line << std::endl;
        }
    }

    // Display parsed information
    std::cout << "📋 Registration Details:" << std::endl;
    if (!to_header.empty()) {
        std::cout << "📞 Extension: " << to_header << std::endl;
    }
    if (!contact_header.empty()) {
        std::cout << "🔗 Contact: " << contact_header << std::endl;
    }
    if (!server_header.empty()) {
        std::cout << "🖥️  Server: " << server_header << std::endl;
    }
    if (!expires_header.empty()) {
        std::cout << "⏰ Expires: " << expires_header << std::endl;
    }
    if (!user_agent_header.empty()) {
        std::cout << "📱 User Agent: " << user_agent_header << std::endl;
    }

    // Show complete response for debugging
    std::cout << "\n📋 Complete SIP Response (for user info analysis):" << std::endl;
    std::cout << "----------------------------------------" << std::endl;
    std::cout << sip_response << std::endl;
    std::cout << "----------------------------------------" << std::endl;
}

void SimpleSipClient::query_extension_user_info(const std::string& extension) {
    std::cout << "\n🔍 Querying PBX for user information about extension: " << extension << std::endl;

    // Get server info from current line configuration
    std::string server_ip;
    int server_port;
    {
        std::lock_guard<std::mutex> lock(sip_lines_mutex_);
        if (sip_lines_.empty()) {
            std::cout << "❌ No SIP lines configured" << std::endl;
            return;
        }
        server_ip = sip_lines_[0].server_ip;
        server_port = sip_lines_[0].server_port;
    }

    // Create SIP OPTIONS request to query extension details
    std::string call_id = "whisper-options-" + std::to_string(time(nullptr));
    std::string branch = "z9hG4bK-options-" + std::to_string(rand() % 10000);

    std::ostringstream options_msg;
    options_msg << "OPTIONS sip:" << extension << "@" << server_ip << " SIP/2.0\r\n";
    options_msg << "Via: SIP/2.0/UDP " << local_ip_ << ":" << sip_listen_port_ << ";branch=" << branch << "\r\n";
    options_msg << "From: <sip:" << extension << "@" << server_ip << ">;tag=options-" << rand() % 10000 << "\r\n";
    options_msg << "To: <sip:" << extension << "@" << server_ip << ">\r\n";
    options_msg << "Call-ID: " << call_id << "\r\n";
    options_msg << "CSeq: 1 OPTIONS\r\n";
    options_msg << "Contact: <sip:" << extension << "@" << local_ip_ << ":" << sip_listen_port_ << ">\r\n";
    options_msg << "User-Agent: Whisper-Talk-LLaMA/2.0\r\n";
    options_msg << "Accept: application/sdp\r\n";
    options_msg << "Content-Length: 0\r\n";
    options_msg << "\r\n";

    std::string options_request = options_msg.str();

    // Send OPTIONS request
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip.c_str(), &server_addr.sin_addr);

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        std::cout << "❌ Failed to create OPTIONS socket" << std::endl;
        return;
    }

    // Set timeout
    struct timeval timeout;
    timeout.tv_sec = 3;
    timeout.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    std::cout << "📡 Sending SIP OPTIONS to query extension details..." << std::endl;
    ssize_t sent = sendto(sock, options_request.c_str(), options_request.length(), 0,
                         (struct sockaddr*)&server_addr, sizeof(server_addr));

    if (sent < 0) {
        std::cout << "❌ Failed to send OPTIONS request" << std::endl;
        close(sock);
        return;
    }

    // Wait for response
    char buffer[4096];
    ssize_t received = recvfrom(sock, buffer, sizeof(buffer) - 1, 0, nullptr, nullptr);
    close(sock);

    if (received > 0) {
        buffer[received] = '\0';
        std::string response(buffer);
        std::cout << "✅ OPTIONS response received (" << received << " bytes)" << std::endl;

        // Parse OPTIONS response for user information
        parse_options_response_for_user_info(response);
    } else {
        std::cout << "❌ No OPTIONS response received (timeout or error)" << std::endl;
    }
}

void SimpleSipClient::parse_options_response_for_user_info(const std::string& response) {
    std::cout << "👤 Analyzing OPTIONS response for user details..." << std::endl;

    // Parse headers line by line
    std::istringstream response_stream(response);
    std::string line;

    bool found_user_info = false;

    while (std::getline(response_stream, line)) {
        // Remove \r if present
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        // Look for user-related headers
        if (line.find("P-Asserted-Identity:") == 0) {
            std::cout << "👤 User Identity: " << line << std::endl;
            found_user_info = true;
        } else if (line.find("Remote-Party-ID:") == 0) {
            std::cout << "👤 Remote Party: " << line << std::endl;
            found_user_info = true;
        } else if (line.find("P-Preferred-Identity:") == 0) {
            std::cout << "👤 Preferred Identity: " << line << std::endl;
            found_user_info = true;
        } else if (line.find("Display-Name:") == 0) {
            std::cout << "👤 Display Name: " << line << std::endl;
            found_user_info = true;
        } else if (line.find("X-User:") == 0 || line.find("X-Name:") == 0 || line.find("X-Extension:") == 0) {
            std::cout << "👤 User Info: " << line << std::endl;
            found_user_info = true;
        }
    }

    if (!found_user_info) {
        std::cout << "ℹ️  No user information found in OPTIONS response" << std::endl;
        std::cout << "📋 Complete OPTIONS Response:" << std::endl;
        std::cout << "----------------------------------------" << std::endl;
        std::cout << response << std::endl;
        std::cout << "----------------------------------------" << std::endl;
    }
}

void SimpleSipClient::update_local_ip() {
    std::string new_ip = detect_local_ip();
    if (new_ip != local_ip_) {
        std::string old_ip = local_ip_;
        local_ip_ = new_ip;
        std::cout << "🌐 Local IP updated: " << old_ip << " -> " << local_ip_ << std::endl;
    } else if (local_ip_.empty()) {
        local_ip_ = new_ip;
        std::cout << "🌐 Local IP detected: " << local_ip_ << std::endl;
    }
}

void SimpleSipClient::outbound_audio_listener_loop() {
    uint8_t buffer[2048];
    struct sockaddr_in from_addr;
    socklen_t from_len = sizeof(from_addr);

    std::cout << "👂 Outbound audio listener loop started on port 9002" << std::endl;

    while (running_) {
        ssize_t n = recvfrom(outbound_udp_sock_, buffer, sizeof(buffer), 0, (struct sockaddr*)&from_addr, &from_len);
        if (n < 4) {
            if (n < 0 && running_) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    std::cerr << "⚠️ outbound recvfrom failed: " << strerror(errno) << std::endl;
                }
            }
            continue;
        }

        // Extract call_num_id (first 4 bytes, network order)
        uint32_t call_num_id = ntohl(*reinterpret_cast<uint32_t*>(buffer));
        
        std::string call_id;
        int local_rtp_port = -1;
        std::pair<std::string, int> dest;

        // Find call info
        {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            auto it = call_num_id_to_call_id_.find((int)call_num_id);
            if (it != call_num_id_to_call_id_.end()) {
                call_id = it->second;
                auto session_it = active_calls_.find(call_id);
                if (session_it != active_calls_.end()) {
                    local_rtp_port = session_it->second.internal_port;
                }
            }
        }

        if (call_id.empty() || local_rtp_port == -1) continue;

        // Get destination
        {
            std::lock_guard<std::mutex> lock(rtp_state_mutex_);
            auto it = rtp_destinations_.find(call_id);
            if (it != rtp_destinations_.end()) {
                dest = it->second;
            }
        }

        if (dest.first.empty()) continue;

        struct sockaddr_in dest_addr{};
        dest_addr.sin_family = AF_INET;
        dest_addr.sin_port = htons(dest.second);
        inet_pton(AF_INET, dest.first.c_str(), &dest_addr.sin_addr);

        int rtp_sock = -1;
        {
            std::lock_guard<std::mutex> lock(rtp_state_mutex_);
            auto it = rtp_port_to_socket_.find(local_rtp_port);
            if (it != rtp_port_to_socket_.end()) {
                rtp_sock = it->second;
            }
        }

        if (rtp_sock < 0) continue;

        // Forward the G.711 data (after the 4-byte header) as RTP
        std::vector<uint8_t> g711_data(buffer + 4, buffer + n);
        send_g711_as_rtp_packets(rtp_sock, g711_data, dest_addr, call_id);
    }
    
    std::cout << "👂 Outbound audio listener loop exiting" << std::endl;
}
