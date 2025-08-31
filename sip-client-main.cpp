// Standalone SIP Client Module - Optimized for Fast Audio Processing
// Handles incoming calls, creates database sessions, manages audio streams
// Manages SIP line connections and status updates

#include "database.h"
#include "audio-processor-interface.h"
#include "audio-processor-service.h"
#include <iostream>
#include <unordered_map>
#include <chrono>
#include <signal.h>
#include <thread>
#include <chrono>
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

// Forward declaration
class AudioProcessorService;

class SimpleSipClient {
public:
    SimpleSipClient();
    ~SimpleSipClient();

    bool init(Database* database, int specific_line_id = -1);
    bool init_audio_processors();

    // Simple audio routing
    // Removed: session registration functions - using sessionless routing
    void route_rtp_to_processor(const RTPAudioPacket& packet);


    // SIP networking
    bool setup_sip_listener();
    void sip_listener_loop();
    int allocate_dynamic_port(); // Legacy method
    int allocate_rtp_port_for_call(const std::string& call_id); // Dynamic line-based allocation
    void setup_rtp_listener(int rtp_port);
    bool start();
    void stop();

    // Call handling (sessionless)
    void handle_incoming_call(const std::string& caller_number, const std::string& call_id = "");
    void end_call(const std::string& call_id);

    // Audio streaming (sessionless)
    void process_rtp_audio(const std::string& call_id, const RTPAudioPacket& packet);
    void stream_audio_from_piper(const std::vector<uint8_t>& audio_data);
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
    void send_rtp_audio(const std::string& call_id, const std::vector<uint8_t>& audio_data,
                       int local_rtp_port, const std::string& dest_ip, int dest_port);

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

    // Per-line audio processors (simple)
    std::unordered_map<int, std::unique_ptr<AudioProcessorService>> line_audio_processors_;
    std::mutex processors_mutex_;

    // Sessionless audio routing
    std::unordered_map<std::string, int> call_id_to_rtp_port_; // call_id -> rtp_port (temporary)
    std::mutex audio_routing_mutex_;

    // RTP transmission state
    std::unordered_map<std::string, uint16_t> rtp_sequence_; // call_id -> sequence number
    std::unordered_map<std::string, uint32_t> rtp_timestamp_; // call_id -> timestamp
    std::unordered_map<std::string, std::pair<std::string, int>> rtp_destinations_; // call_id -> (ip, port)
    std::unordered_map<int, int> rtp_port_to_socket_; // rtp_port -> socket_fd (for symmetric RTP)
    std::mutex rtp_state_mutex_;

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

    // Main loops
    void sip_management_loop();
    void sip_message_listener();
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
    std::cout << "Usage: whisper-sip-client [options]\n"
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

    // Try to initialize audio processors (optional - SIP client works without them)
    init_audio_processors();

    return true;
}

bool SimpleSipClient::init_audio_processors() {
    // Optional: Try to create audio processors for enabled lines
    // If this fails, SIP client will still work but drop audio

    if (!database_) {
        std::cout << "⚠️ No database available, audio processors disabled" << std::endl;
        return true; // Still allow SIP client to run
    }

    auto sip_lines = database_->get_all_sip_lines();

    for (const auto& line : sip_lines) {
        if (line.enabled) {
            try {
                // Create audio processor for this line
                auto audio_processor = AudioProcessorServiceFactory::create();
                audio_processor->set_database(database_);

                // Start the audio processor (but it will sleep until activated)
                if (audio_processor->start(8083 + line.line_id)) {
                    // Set up connection callback for outgoing audio (sessionless)
                    audio_processor->set_sip_client_callback(
                        [this](const std::vector<uint8_t>& audio_data) {
                            this->stream_audio_from_piper(audio_data);
                        }
                    );

                    // Connect database for system speed configuration
                    audio_processor->set_database(database_);

                    line_audio_processors_[line.line_id] = std::move(audio_processor);
                    std::cout << "✅ Audio processor created for SIP line " << line.line_id << " (database connected)" << std::endl;
                } else {
                    std::cout << "⚠️ Failed to start audio processor for SIP line " << line.line_id
                              << " (audio will be dropped)" << std::endl;
                }
            } catch (const std::exception& e) {
                std::cout << "⚠️ Exception creating audio processor for line " << line.line_id
                          << ": " << e.what() << " (audio will be dropped)" << std::endl;
            }
        }
    }

    if (line_audio_processors_.empty()) {
        std::cout << "⚠️ No audio processors available - SIP client will run but drop all audio" << std::endl;
    } else {
        std::cout << "✅ Initialized " << line_audio_processors_.size() << " audio processors" << std::endl;
    }

    return true; // Always return true - SIP client can run without audio processors
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

void SimpleSipClient::setup_rtp_listener(int rtp_port) {
    // Create a basic UDP socket to listen on the RTP port
    // This makes the port "active" so the PBX can send audio to it
    int rtp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (rtp_sock < 0) {
        std::cout << "⚠️ Failed to create RTP socket for port " << rtp_port << std::endl;
        return;
    }

    struct sockaddr_in rtp_addr;
    memset(&rtp_addr, 0, sizeof(rtp_addr));
    rtp_addr.sin_family = AF_INET;
    rtp_addr.sin_addr.s_addr = INADDR_ANY;
    rtp_addr.sin_port = htons(rtp_port);

    if (bind(rtp_sock, (struct sockaddr*)&rtp_addr, sizeof(rtp_addr)) < 0) {
        std::cout << "⚠️ Failed to bind RTP socket to port " << rtp_port << std::endl;
        close(rtp_sock);
        return;
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
        rtp_threads_.emplace_back([this, rtp_sock, rtp_port]() {
        char buffer[2048];
        struct sockaddr_in sender_addr;
        socklen_t sender_len = sizeof(sender_addr);

        std::cout << "🎧 RTP receiver thread started for port " << rtp_port << std::endl;

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

                    // Store RTP destination for sending RTP back (sessionless)
                    {
                        std::lock_guard<std::mutex> lock(rtp_state_mutex_);
                        rtp_destinations_["default"] = {source_ip, source_port};
                        std::cout << "🎯 Stored RTP destination: " << source_ip << ":" << source_port << std::endl;
                    }
                } else if (packet_count % 100 == 0) {
                    std::cout << "🎵 RTP: " << packet_count << " packets received" << std::endl;
                }

                // Parse RTP packet and route to existing audio system
                if (received >= 12) { // Minimum RTP header size
                    uint8_t* rtp_data = (uint8_t*)buffer;

                    // Parse RTP header
                    uint8_t payload_type = rtp_data[1] & 0x7F;
                    uint16_t sequence = (rtp_data[2] << 8) | rtp_data[3];
                    uint32_t timestamp = (rtp_data[4] << 24) | (rtp_data[5] << 16) |
                                       (rtp_data[6] << 8) | rtp_data[7];

                    // Extract audio payload (skip 12-byte RTP header)
                    std::vector<uint8_t> audio_payload(rtp_data + 12, rtp_data + received);

                    // Create RTPAudioPacket for existing routing system
                    RTPAudioPacket packet(payload_type, audio_payload, timestamp, sequence);

                    // Direct sessionless routing to audio processor
                    route_rtp_to_processor(packet);
                }
            } else if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                // Real error (not timeout)
                if (running_) {
                    std::cout << "❌ RTP receiver error on port " << rtp_port << ": " << strerror(errno) << std::endl;
                }
                break;
            }
            // received == 0 or timeout - continue loop to check running_ flag
        }

        close(rtp_sock);
        std::cout << "🔌 RTP receiver thread ended for port " << rtp_port << std::endl;
        });
    }
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
            std::cout << "👂 SIP listener active (waiting for packets on port " << sip_listen_port_ << ")..." << std::endl;
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

    // Wait a moment to simulate ringing
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Send 200 OK response to accept the call
    send_sip_response(200, "OK", call_id, from, to, via, cseq, sender_addr, line_id);

    // Extract caller number from From header using RFC-compliant parsing
    std::string caller_number = extract_phone_number(from);
    std::cout << "📞 Extracted caller number: " << caller_number << " (from: " << from << ")" << std::endl;

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

    // Handle incoming call (sessionless)
    handle_incoming_call(caller_number, call_id);
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

        // Set up RTP listener on the allocated port (basic UDP socket)
        setup_rtp_listener(rtp_port);
        std::cout << "🎧 RTP listener set up on port " << rtp_port << " for call " << call_id << std::endl;

        // Store RTP port for this call_id (will be connected to session later)
        {
            std::lock_guard<std::mutex> lock(audio_routing_mutex_);
            call_id_to_rtp_port_[call_id] = rtp_port;
        }

        // Add SDP for audio with DTMF support (RFC 4733 compliant)
        std::string sdp =
            "v=0\r\n"
            "o=whisper 123456 654321 IN IP4 " + local_ip_ + "\r\n"
            "s=Whisper Talk Session\r\n"
            "c=IN IP4 " + local_ip_ + "\r\n"
            "t=0 0\r\n"
            "m=audio " + std::to_string(rtp_port) + " RTP/AVP 0 101\r\n"
            "a=rtpmap:0 PCMU/8000\r\n"
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

    // Deactivate audio processor (sessionless)
    int line_id = (specific_line_id_ != -1) ? specific_line_id_ : 1;
    {
        std::lock_guard<std::mutex> proc_lock(processors_mutex_);
        auto proc_it = line_audio_processors_.find(line_id);
        if (proc_it != line_audio_processors_.end() && proc_it->second) {
            proc_it->second->deactivate_after_call();
            std::cout << "😴 Audio processor deactivated for line " << line_id << std::endl;
        }
    }

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

    // Close SIP listener socket to unblock listener thread
    std::cout << "🛑 Closing SIP listener socket..." << std::endl;
    if (sip_listen_socket_ >= 0) {
        close(sip_listen_socket_);
        sip_listen_socket_ = -1;
        std::cout << "🛑 SIP listener socket closed" << std::endl;
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

        // Check timeout
        if (std::chrono::steady_clock::now() - cleanup_start > timeout) {
            std::cout << "⚠️ Thread cleanup timeout - forcing exit" << std::endl;
            return;
        }

        if (sip_listener_thread_.joinable()) {
            std::cout << "🛑 Joining SIP listener thread..." << std::endl;
            sip_listener_thread_.join();
            std::cout << "🛑 SIP listener thread joined" << std::endl;
        }

        // Check timeout
        if (std::chrono::steady_clock::now() - cleanup_start > timeout) {
            std::cout << "⚠️ Thread cleanup timeout - forcing exit" << std::endl;
            return;
        }

        if (connection_monitor_thread_.joinable()) {
            std::cout << "🛑 Joining connection monitor thread..." << std::endl;
            connection_monitor_thread_.join();
            std::cout << "🛑 Connection monitor thread joined" << std::endl;
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

    // Step 3: Activate audio processor for this line (no session needed)
    std::cout << "🎵 ACTIVATING audio processor for line " << line_id << " (sessionless)" << std::endl;

    // No session registration needed - direct audio processor activation

    // ACTIVATE audio processor for this call
    {
        std::lock_guard<std::mutex> lock(processors_mutex_);
        auto proc_it = line_audio_processors_.find(line_id);
        if (proc_it != line_audio_processors_.end()) {
            // Activate with call_id for TCP connections
            proc_it->second->activate_for_call(call_id);
            std::cout << "✅ Audio processor activated for line " << line_id << " (call: " << call_id << ")" << std::endl;
        } else {
            std::cout << "❌ No audio processor found for line " << line_id << std::endl;
        }
    }

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

        // Start RTP keepalive (sessionless)
        if (rtp_port > 0) {
            std::lock_guard<std::mutex> lock(rtp_threads_mutex_);
            rtp_threads_.emplace_back([call_id]() {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                std::cout << "🎵 Starting RTP keepalive for call " << call_id << std::endl;
                // TODO: implement sessionless RTP keepalive
            });
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

    // Simple cleanup - no session management needed
    int line_id = (specific_line_id_ != -1) ? specific_line_id_ : 1;

    {
        std::lock_guard<std::mutex> proc_lock(processors_mutex_);
        auto proc_it = line_audio_processors_.find(line_id);
        if (proc_it != line_audio_processors_.end()) {
            // Simple cleanup - no session management
            std::cout << "✅ Audio processor ready for next call on line " << line_id << std::endl;
        } else {
            std::cout << "⚠️ No audio processor found for line " << line_id << std::endl;
        }
    }

    // Simple cleanup - no session management needed
    std::cout << "🧹 Call cleanup complete (sessionless)" << std::endl;

    std::cout << "✅ Call ended successfully" << std::endl;
}

// Removed: process_rtp_audio - replaced with direct sessionless routing

void SimpleSipClient::stream_audio_from_piper(const std::vector<uint8_t>& audio_data) {
    std::cout << "🔊 Streaming " << audio_data.size() << " bytes of audio from Piper (sessionless)" << std::endl;

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

    // SIP client just handles RTP transmission - no audio conversion
    // The processor already converted Piper audio to G.711 format
    send_rtp_packets_to_pbx_sessionless(audio_data, rtp_port);
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

void SimpleSipClient::route_rtp_to_processor(const RTPAudioPacket& packet) {
    // Sessionless: Direct routing to audio processor
    std::lock_guard<std::mutex> lock(processors_mutex_);

    // For single-line mode, use the specific line's processor
    int line_id = (specific_line_id_ != -1) ? specific_line_id_ : 1;
    auto proc_it = line_audio_processors_.find(line_id);

    if (proc_it != line_audio_processors_.end() && proc_it->second->is_running()) {
        // Route directly to processor (no session needed)
        proc_it->second->process_audio(packet);
    }
    // else: Drop packet silently (processor not connected)
}

void SimpleSipClient::send_rtp_packets_to_pbx(const std::string& call_id, const std::vector<uint8_t>& g711_data, int local_rtp_port) {
    std::cout << "📤 Sending " << g711_data.size() << " bytes of G.711 audio to PBX for call: " << call_id << std::endl;

    // Get destination from captured RTP source address
    std::string dest_ip;
    int dest_port;

    {
        std::lock_guard<std::mutex> lock(rtp_state_mutex_);
        auto it = rtp_destinations_.find(call_id);
        if (it != rtp_destinations_.end()) {
            dest_ip = it->second.first;
            dest_port = it->second.second;
        } else {
            std::cout << "❌ No RTP destination found for call: " << call_id << std::endl;
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

    // Set up destination address
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(dest_port);
    inet_pton(AF_INET, dest_ip.c_str(), &dest_addr.sin_addr);

    // Send G.711 data as RTP packets (160 bytes per packet = 20ms)
    send_g711_as_rtp_packets(rtp_sock, g711_data, dest_addr, call_id);

    // Don't close socket - it's shared with RTP receiver for symmetric RTP
    std::cout << "🎵 Sent RTP packets from port " << local_rtp_port << " to " << dest_ip << ":" << dest_port << std::endl;
}

void SimpleSipClient::send_rtp_packets_to_pbx_sessionless(const std::vector<uint8_t>& g711_data, int local_rtp_port) {
    (void)local_rtp_port; // Unused parameter
    std::cout << "📤 Sending " << g711_data.size() << " bytes of G.711 audio to PBX (sessionless)" << std::endl;

    // Use first available destination (simplified for sessionless)
    // Get PBX IP from the first enabled SIP line in database
    std::string dest_ip = "127.0.0.1";  // Fallback
    {
        std::lock_guard<std::mutex> lock(sip_lines_mutex_);
        for (const auto& line : sip_lines_) {
            if (line.enabled) {
                dest_ip = line.server_ip;
                break;
            }
        }
    }
    int dest_port = 5004;  // Default RTP port

    // Create RTP socket
    int rtp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (rtp_sock < 0) {
        std::cout << "❌ Failed to create RTP socket" << std::endl;
        return;
    }

    // Set up destination address
    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(dest_port);
    inet_pton(AF_INET, dest_ip.c_str(), &dest_addr.sin_addr);

    // Send G.711 data as RTP packets (simplified - no session tracking)
    const size_t RTP_PAYLOAD_SIZE = 160; // 160 bytes = 20ms of G.711 audio
    static uint16_t sequence_num = rand() % 65536;
    static uint32_t timestamp = rand();

    // Send in chunks
    for (size_t offset = 0; offset < g711_data.size(); offset += RTP_PAYLOAD_SIZE) {
        size_t chunk_size = std::min(RTP_PAYLOAD_SIZE, g711_data.size() - offset);

        // Create RTP header (simplified)
        uint8_t rtp_packet[12 + 160];  // Fixed size: 12 byte header + max 160 byte payload
        rtp_packet[0] = 0x80; // Version 2, no padding, no extension, no CSRC
        rtp_packet[1] = 0x00; // PCMU payload type
        rtp_packet[2] = (sequence_num >> 8) & 0xFF;
        rtp_packet[3] = sequence_num & 0xFF;
        rtp_packet[4] = (timestamp >> 24) & 0xFF;
        rtp_packet[5] = (timestamp >> 16) & 0xFF;
        rtp_packet[6] = (timestamp >> 8) & 0xFF;
        rtp_packet[7] = timestamp & 0xFF;
        // SSRC (bytes 8-11) left as zero

        // Copy payload
        memcpy(rtp_packet + 12, g711_data.data() + offset, chunk_size);

        // Send packet
        sendto(rtp_sock, rtp_packet, 12 + chunk_size, 0,
               (struct sockaddr*)&dest_addr, sizeof(dest_addr));

        sequence_num++;
        timestamp += 160;
    }

    close(rtp_sock);
    std::cout << "🎵 Sent RTP packets (sessionless) to " << dest_ip << ":" << dest_port << std::endl;
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

    // Send G.711 data in 160-byte chunks (20ms packets)
    for (size_t offset = 0; offset < g711_data.size(); offset += RTP_PAYLOAD_SIZE) {
        size_t chunk_size = std::min(RTP_PAYLOAD_SIZE, g711_data.size() - offset);

        // Create RTP packet
        uint8_t rtp_packet[12 + RTP_PAYLOAD_SIZE]; // 12-byte header + payload

        // RTP Header (12 bytes)
        rtp_packet[0] = 0x80; // Version=2, Padding=0, Extension=0, CC=0
        rtp_packet[1] = 0x00; // Marker=0, Payload Type=0 (G.711 μ-law)
        rtp_packet[2] = (sequence_num >> 8) & 0xFF;
        rtp_packet[3] = sequence_num & 0xFF;
        rtp_packet[4] = (timestamp >> 24) & 0xFF;
        rtp_packet[5] = (timestamp >> 16) & 0xFF;
        rtp_packet[6] = (timestamp >> 8) & 0xFF;
        rtp_packet[7] = timestamp & 0xFF;
        rtp_packet[8] = 0x12; // SSRC (arbitrary)
        rtp_packet[9] = 0x34;
        rtp_packet[10] = 0x56;
        rtp_packet[11] = 0x78;

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

        // Small delay between packets (20ms)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
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
        // Check if processor is connected for current line
        int line_id = (specific_line_id_ != -1) ? specific_line_id_ : 1;
        bool processor_connected = false;

        {
            std::lock_guard<std::mutex> lock(processors_mutex_);
            auto proc_it = line_audio_processors_.find(line_id);
            processor_connected = (proc_it != line_audio_processors_.end() && proc_it->second->is_running());
        }

        // Send silence if no processor connected
        if (!processor_connected) {
            send_silence_rtp_packet_sessionless(rtp_port);
        }
        // If processor is connected, it should be generating its own RTP from Piper
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