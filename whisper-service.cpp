#include "whisper-service.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <sys/stat.h>

// Real whisper.cpp integration
#include "whisper-cpp/include/whisper.h"

// Robust I/O helpers to avoid partial reads/writes on TCP
static bool read_exact_fd(int fd, void* buf, size_t nbytes) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t off = 0;
    while (off < nbytes) {
        ssize_t m = recv(fd, p + off, nbytes - off, 0);
        if (m <= 0) return false;
        off += static_cast<size_t>(m);
    }
    return true;
}

static bool write_all_fd(int fd, const void* buf, size_t nbytes) {
    const uint8_t* p = static_cast<const uint8_t*>(buf);
    size_t off = 0;
    while (off < nbytes) {
        ssize_t m = send(fd, p + off, nbytes - off, 0);
        if (m <= 0) return false;
        off += static_cast<size_t>(m);
    }
    return true;
}

// WhisperSession Implementation
WhisperSession::WhisperSession(const std::string& call_id, const WhisperSessionConfig& config)
    : call_id_(call_id), ctx_(nullptr), is_active_(true), config_(config) {

    last_activity_ = std::chrono::steady_clock::now();

    // Reuse shared preloaded context if provided
    if (config_.shared_ctx) {
        ctx_ = config_.shared_ctx;
        shared_mutex_ = config_.shared_mutex;
        ctx_shared_ = true;
        if (!ctx_) {
            std::cout << "❌ Shared whisper context is null" << std::endl;
            is_active_.store(false);
            return;
        }
        std::cout << "🔁 Reusing preloaded Whisper model for call " << call_id << std::endl;
        std::cout << "✅ Whisper session created for call " << call_id << std::endl;
        return;
    }

    // Fallback: load per-session (should not happen in normal operation)
    if (!initialize_whisper_context()) {
        std::cout << "❌ Failed to initialize whisper context for call " << call_id << std::endl;
        is_active_.store(false);
    } else {
        std::cout << "✅ Whisper session created for call " << call_id << std::endl;
    }
}

WhisperSession::~WhisperSession() {
    cleanup_whisper_context();
    std::cout << "🗑️ Whisper session destroyed for call " << call_id_ << std::endl;
}

bool WhisperSession::initialize_whisper_context() {
    // Validate model file exists
    struct stat file_stat;
    if (stat(config_.model_path.c_str(), &file_stat) != 0) {
        std::cout << "❌ Model file not found: " << config_.model_path << std::endl;
        return false;
    }

    std::cout << "📂 Loading Whisper model: " << config_.model_path << std::endl;

    // Initialize real whisper context with modern API
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = config_.use_gpu;
    ctx_ = whisper_init_from_file_with_params(config_.model_path.c_str(), cparams);

    if (!ctx_) {
        std::cout << "❌ Failed to load Whisper model: " << config_.model_path << std::endl;
        return false;
    }

    std::cout << "✅ Whisper model loaded successfully for call " << call_id_ << std::endl;
    return true;
}

void WhisperSession::cleanup_whisper_context() {
    if (ctx_ && !ctx_shared_) {
        whisper_free(ctx_);
        ctx_ = nullptr;
    }
}

bool WhisperSession::process_audio_chunk(const std::vector<float>& audio_samples) {
    if (!is_active_.load() || !ctx_) {
        return false;
    }

    if (audio_samples.empty()) {
        return true; // Nothing to process
    }

    // Update activity timestamp
    mark_activity();

    // IMMEDIATE PROCESSING - NO BUFFERING, NO DELAYS
    // Process the audio chunk as soon as it arrives for real-time speed
    return process_window(audio_samples);
}

// Process a single window with whisper inference
bool WhisperSession::process_window(const std::vector<float>& window) {
    if (!is_active_.load() || !ctx_) {
        return false;
    }

    // Serialize access to shared whisper context if needed
    std::unique_lock<std::mutex> shared_lock;
    auto t_mutex_start = std::chrono::high_resolution_clock::now();

    if (shared_mutex_) {
        shared_lock = std::unique_lock<std::mutex>(*shared_mutex_);
    }

    auto t_mutex_acquired = std::chrono::high_resolution_clock::now();
    auto mutex_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t_mutex_acquired - t_mutex_start).count();

    if (mutex_wait_ms > 10) {
        std::cout << "⏳ [" << call_id_ << "] Mutex wait: " << mutex_wait_ms << "ms" << std::endl;
    }

    // Create real whisper parameters
    // Using GREEDY sampling (baseline) - BEAM_SEARCH testing showed no improvement
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.language = config_.language.c_str();
    wparams.n_threads = config_.n_threads;
    wparams.temperature = 0.0f;
    wparams.no_timestamps = true;
    wparams.translate = false;
    wparams.print_progress = false;
    wparams.print_realtime = false;

    // Diagnostics before inference
    double secs_in = static_cast<double>(window.size()) / 16000.0;

    auto t_inference_start = std::chrono::high_resolution_clock::now();
    int result = whisper_full(ctx_, wparams, window.data(), window.size());
    auto t_inference_end = std::chrono::high_resolution_clock::now();

    auto inference_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t_inference_end - t_inference_start).count();

    std::cout << "⚡ [" << call_id_ << "] Inference: " << inference_ms << "ms ("
              << secs_in << "s audio)" << std::endl;

    if (result == 0) {
        // Extract transcription
        const int n_segments = whisper_full_n_segments(ctx_);
        std::string transcription;

        for (int i = 0; i < n_segments; ++i) {
            const char* text = whisper_full_get_segment_text(ctx_, i);
            if (text) {
                transcription += text;
            }
        }

        if (!transcription.empty()) {
            // Store transcription immediately - no deduplication delays
            std::lock_guard<std::mutex> lock(session_mutex_);
            latest_transcription_ = transcription;

            std::cout << "📝 [" << call_id_ << "] Transcription: " << transcription << std::endl;
            return true;
        }
    } else {
        std::cout << "❌ Whisper processing failed for call " << call_id_ << std::endl;
    }

    return false;
}



std::string WhisperSession::get_latest_transcription() {
    std::lock_guard<std::mutex> lock(session_mutex_);
    std::string result = latest_transcription_;
    latest_transcription_.clear(); // Clear after reading
    return result;
}

// Removed deduplication and buffer size functions - not needed for real-time speed

// StandaloneWhisperService Implementation
StandaloneWhisperService::StandaloneWhisperService()
    : running_(false) {

    service_discovery_ = std::make_unique<ServiceDiscovery>();
}

StandaloneWhisperService::~StandaloneWhisperService() {
    stop();
}

bool StandaloneWhisperService::start(const WhisperSessionConfig& config, const std::string& db_path) {
    if (running_.load()) return true;

    config_ = config;

    // Initialize database connection
    database_ = std::make_unique<Database>();
    if (!database_->init(db_path)) {
        std::cout << "❌ Failed to initialize database: " << db_path << std::endl;
        return false;
    }

    // Mark service as starting
    database_->set_whisper_service_status("starting");

    // Eagerly load Whisper model to avoid lazy load on first TCP connection
    std::cout << "⏳ Preloading Whisper model: " << config_.model_path << std::endl;
    auto t0 = std::chrono::steady_clock::now();
    whisper_context_params cparams = whisper_context_default_params();
    cparams.use_gpu = config_.use_gpu;
    cparams.flash_attn = true;           // Enable flash attention for Metal (20-30% speedup)
    cparams.gpu_device = 0;              // Use primary GPU
    cparams.dtw_token_timestamps = false; // Disable timestamps for speed (5-10% speedup)
    warm_ctx_ = whisper_init_from_file_with_params(config_.model_path.c_str(), cparams);
    if (!warm_ctx_) {
        std::cout << "❌ Whisper preload failed for model: " << config_.model_path << std::endl;
        database_->set_whisper_service_status("error");
        return false;
    }
    warm_loaded_ = true;
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "✅ Whisper model preloaded in " << ms << " ms" << std::endl;

    // Warm-up inference to compile GPU kernels and allocate graphs
    try {
        std::vector<float> silence(16000, 0.0f); // ~1s @16kHz
        whisper_full_params wp = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
        wp.no_timestamps = true;
        wp.print_progress = false;
        wp.print_realtime = false;
        int wres = whisper_full(warm_ctx_, wp, silence.data(), silence.size());
        if (wres == 0) {
            std::cout << "✅ Whisper warm-up inference completed" << std::endl;
        } else {
            std::cout << "⚠️ Whisper warm-up failed (non-fatal)" << std::endl;
        }
    } catch (...) {
        std::cout << "⚠️ Whisper warm-up threw exception (non-fatal)" << std::endl;
    }


    // Only now mark running and launch discovery
    running_.store(true);
    discovery_thread_ = std::thread(&StandaloneWhisperService::run_service_loop, this);

    // Start registration listener
    start_registration_listener();

    // Update DB and log
    database_->set_whisper_service_status("running");
    std::cout << "🎤 Standalone Whisper Service started" << std::endl;
    std::cout << "📡 Model: " << config.model_path << std::endl;
    std::cout << "💾 Database: " << db_path << std::endl;
    std::cout << "🔍 Listening for audio processor registrations on UDP port 13000..." << std::endl;

    return true;
}

void StandaloneWhisperService::stop() {
    if (!running_.load() && !warm_loaded_) return;

    running_.store(false);

    // Stop registration listener
    stop_registration_listener();

    // Close all TCP connections
    {
        std::lock_guard<std::mutex> lock(tcp_mutex_);
        for (auto& pair : call_tcp_sockets_) {
            if (pair.second >= 0) {
                send_tcp_bye(pair.second);
                close(pair.second);
            }
        }
        call_tcp_sockets_.clear();
        // Note: TCP handler threads are detached and will exit when sockets close
    }

    // Destroy all sessions
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.clear();
    }

    if (discovery_thread_.joinable()) {
        discovery_thread_.join();
    }

    // Free preloaded model context
    if (warm_ctx_) {
        whisper_free(warm_ctx_);
        warm_ctx_ = nullptr;
        warm_loaded_ = false;
    }

    if (database_) {
        database_->set_whisper_service_status("stopped");
    }

    std::cout << "🛑 Standalone Whisper Service stopped" << std::endl;
}

void StandaloneWhisperService::start_registration_listener() {
    // Create UDP socket for registration messages
    registration_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (registration_socket_ < 0) {
        std::cerr << "❌ Failed to create registration UDP socket" << std::endl;
        return;
    }

    // Set SO_REUSEADDR/SO_REUSEPORT to allow quick restart and avoid stray binds
    int reuse = 1;
    if (setsockopt(registration_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        std::cerr << "⚠️ Failed to set SO_REUSEADDR on registration socket" << std::endl;
    }
#ifdef SO_REUSEPORT
    if (setsockopt(registration_socket_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse)) < 0) {
        std::cerr << "⚠️ Failed to set SO_REUSEPORT on registration socket" << std::endl;
    }
#endif

    // Increase receive buffer size to prevent packet drops
    int rcvbuf = 256 * 1024; // 256KB
    if (setsockopt(registration_socket_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
        std::cerr << "⚠️ Failed to set SO_RCVBUF on registration socket" << std::endl;
    } else {
        // Verify the actual buffer size set
        int actual_rcvbuf = 0;
        socklen_t optlen = sizeof(actual_rcvbuf);
        if (getsockopt(registration_socket_, SOL_SOCKET, SO_RCVBUF, &actual_rcvbuf, &optlen) == 0) {
            std::cout << "📊 UDP receive buffer size: " << actual_rcvbuf << " bytes" << std::endl;
        }
    }

    // Bind to loopback on port 13000 (explicitly) to avoid interface ambiguity
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(13000);

    if (bind(registration_socket_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "❌ Failed to bind registration UDP socket to 127.0.0.1:13000" << std::endl;
        close(registration_socket_);
        registration_socket_ = -1;
        return;
    }

    registration_running_.store(true);
    registration_thread_ = std::thread(&StandaloneWhisperService::registration_listener_thread, this);

    std::cout << "📡 Whisper registration listener started on UDP port 13000" << std::endl;
}

void StandaloneWhisperService::stop_registration_listener() {
    registration_running_.store(false);

    if (registration_socket_ >= 0) {
        close(registration_socket_);
        registration_socket_ = -1;
    }

    if (registration_thread_.joinable()) {
        registration_thread_.join();
    }
}

void StandaloneWhisperService::registration_listener_thread() {
    char buffer[256];
    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    std::cout << "📡 Whisper registration listener thread started (FD: " << registration_socket_ << ")" << std::endl;

    int message_count = 0;
    int loop_count = 0;
    while (registration_running_.load()) {
        loop_count++;

        // Verify socket is still valid
        if (registration_socket_ < 0) {
            std::cout << "❌ Registration socket became invalid (FD: " << registration_socket_ << "), exiting listener" << std::endl;
            break;
        }

        // Periodic socket health check every 100 iterations
        if (loop_count % 100 == 0) {
            int error = 0;
            socklen_t errlen = sizeof(error);
            if (getsockopt(registration_socket_, SOL_SOCKET, SO_ERROR, &error, &errlen) == 0) {
                if (error != 0) {
                    std::cout << "⚠️ Socket error detected (FD=" << registration_socket_ << "): " << strerror(error) << std::endl;
                }
            }

            // Check for bytes available in buffer
            #ifdef SO_NREAD
            int bytes_available = 0;
            socklen_t bytes_len = sizeof(bytes_available);
            if (getsockopt(registration_socket_, SOL_SOCKET, SO_NREAD, &bytes_available, &bytes_len) == 0) {
                if (bytes_available > 0) {
                    std::cout << "📊 Bytes available in UDP buffer: " << bytes_available << std::endl;
                }
            }
            #endif
        }

        // Ensure client_len is reset before each recvfrom (per POSIX semantics)
        client_len = sizeof(client_addr);

        // Set timeout on socket (1s)
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(registration_socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        ssize_t n = recvfrom(registration_socket_, buffer, sizeof(buffer) - 1, 0,
                             (struct sockaddr*)&client_addr, &client_len);

        if (n > 0) {
            message_count++;
            std::cout << "📨 UDP message #" << message_count << " received (" << n << " bytes) from "
                      << inet_ntoa(client_addr.sin_addr) << ":" << ntohs(client_addr.sin_port) << std::endl;
            buffer[n] = '\0';
            std::string message(buffer);

            try {
                // Parse message: "REGISTER:<call_id>" or "BYE:<call_id>"
                if (message.rfind("REGISTER:", 0) == 0) {
                    std::string call_id = message.substr(9);
                    // Trim any stray whitespace
                    while (!call_id.empty() && isspace(static_cast<unsigned char>(call_id.back()))) call_id.pop_back();
                    while (!call_id.empty() && isspace(static_cast<unsigned char>(call_id.front()))) call_id.erase(call_id.begin());
                    int call_id_num = std::stoi(call_id);

                    std::cout << "📥 Received REGISTER for call_id " << call_id << std::endl;

                    // Check if already connected or connecting (idempotent registration)
                    bool should_connect = false;
                    {
                        std::lock_guard<std::mutex> lock(tcp_mutex_);
                        if (call_tcp_sockets_.find(call_id) != call_tcp_sockets_.end()) {
                            std::cout << "✅ Already connected to call " << call_id << " - ignoring duplicate REGISTER" << std::endl;
                            continue;
                        }
                        // Mark as connecting by inserting placeholder (-1) to prevent duplicate connection attempts
                        call_tcp_sockets_[call_id] = -1;
                        should_connect = true;
                    }

                    if (should_connect) {
                        // Calculate port: 9001 + call_id
                        int inbound_port = 9001 + call_id_num;

                        // Create stream info and connect
                        AudioStreamInfo stream;
                        stream.call_id = call_id;
                        stream.tcp_port = inbound_port;
                        stream.stream_type = "inbound";
                        stream.sample_rate = 8000;
                        stream.channels = 1;

                        std::cout << "🔗 Whisper connecting to inbound audio stream: " << call_id
                                  << " on port " << inbound_port << std::endl;

                        // Spawn connection attempt in separate thread to avoid blocking registration listener
                        std::thread([this, stream, call_id]() {
                            try {
                                if (connect_to_audio_stream(stream)) {
                                    create_session(call_id);
                                    std::cout << "✅ Successfully connected and created session for call " << call_id << std::endl;
                                } else {
                                    std::cout << "⚠️ Failed to connect to inbound processor for call " << call_id << std::endl;
                                    // Remove placeholder on failure so it can retry
                                    std::lock_guard<std::mutex> lock(tcp_mutex_);
                                    auto it = call_tcp_sockets_.find(call_id);
                                    if (it != call_tcp_sockets_.end() && it->second == -1) {
                                        call_tcp_sockets_.erase(it);
                                    }
                                }
                            } catch (const std::exception& e) {
                                std::cout << "❌ Connection thread exception for call " << call_id << ": " << e.what() << std::endl;
                                // Remove placeholder on exception
                                std::lock_guard<std::mutex> lock(tcp_mutex_);
                                auto it = call_tcp_sockets_.find(call_id);
                                if (it != call_tcp_sockets_.end() && it->second == -1) {
                                    call_tcp_sockets_.erase(it);
                                }
                            } catch (...) {
                                std::cout << "❌ Unknown exception in connection thread for call " << call_id << std::endl;
                                // Remove placeholder on exception
                                std::lock_guard<std::mutex> lock(tcp_mutex_);
                                auto it = call_tcp_sockets_.find(call_id);
                                if (it != call_tcp_sockets_.end() && it->second == -1) {
                                    call_tcp_sockets_.erase(it);
                                }
                            }
                        }).detach();
                    }
                } else if (message.rfind("BYE:", 0) == 0) {
                    std::string call_id = message.substr(4);
                    while (!call_id.empty() && isspace(static_cast<unsigned char>(call_id.back()))) call_id.pop_back();
                    while (!call_id.empty() && isspace(static_cast<unsigned char>(call_id.front()))) call_id.erase(call_id.begin());
                    std::cout << "📤 Received BYE for call_id " << call_id << std::endl;
                    destroy_session(call_id);
                }
            } catch (const std::exception& e) {
                std::cout << "⚠️ Registration parse/handle error: '" << message << "' : " << e.what() << std::endl;
                // continue loop
            }
        } else if (n == 0 || (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
        } else if (n < 0) {
            std::cout << "⚠️ recvfrom error (FD=" << registration_socket_ << "): " << strerror(errno)
                      << " (errno=" << errno << ")" << std::endl;
        }
    }

    std::cout << "📡 Whisper registration listener thread exiting (FD was: " << registration_socket_ << ")" << std::endl;
}

void StandaloneWhisperService::run_service_loop() {
    last_discovery_ = std::chrono::steady_clock::now();

    while (running_.load()) {
        auto now = std::chrono::steady_clock::now();

        // Discover new streams every 1 second (faster to recover between calls)
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_discovery_).count() > 1000) {
            discover_and_connect_streams();
            last_discovery_ = now;
        }

        // Cleanup inactive sessions
        cleanup_inactive_sessions();

        // Log session statistics
        // log_session_stats();  // suppressed to reduce console spam

        // Fast polling for real-time responsiveness
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void StandaloneWhisperService::discover_and_connect_streams() {
    // DIRECT CONNECTION MODE: Check database for active calls and connect directly
    // Port calculation: 9001 + call_num_id (from database row ID)

    if (!database_) return;

    // Get all active calls from database
    auto active_calls = database_->get_active_calls();

    for (const auto& call : active_calls) {
        std::string call_id_str = std::to_string(call.id);

        // Check if we're already connected to this stream
        {
            std::lock_guard<std::mutex> lock(tcp_mutex_);
            if (call_tcp_sockets_.find(call_id_str) != call_tcp_sockets_.end()) {
                continue; // Already connected
            }
        }

        // Calculate inbound processor port: 9001 + call_num_id
        int inbound_port = 9001 + call.id;

        // Create stream info for direct connection
        AudioStreamInfo stream;
        stream.call_id = call_id_str;
        stream.tcp_port = inbound_port;
        stream.stream_type = "inbound";
        stream.sample_rate = 8000;
        stream.channels = 1;

        std::cout << "🔗 Whisper connecting to inbound audio stream: " << call_id_str
                  << " on port " << inbound_port << std::endl;

        if (connect_to_audio_stream(stream)) {
            create_session(call_id_str);
        }
    }
}

bool StandaloneWhisperService::connect_to_audio_stream(const AudioStreamInfo& stream_info) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cout << "❌ Failed to create socket for call " << stream_info.call_id << std::endl;
        return false;
    }

    // Set socket to non-blocking mode for connection with timeout
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_port = htons(stream_info.tcp_port);

    int connect_result = connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr));

    if (connect_result < 0) {
        if (errno == EINPROGRESS) {
            // Connection in progress - wait with timeout (2 seconds)
            fd_set write_fds;
            FD_ZERO(&write_fds);
            FD_SET(sock, &write_fds);

            struct timeval timeout;
            timeout.tv_sec = 2;
            timeout.tv_usec = 0;

            int select_result = select(sock + 1, NULL, &write_fds, NULL, &timeout);

            if (select_result > 0) {
                // Check if connection succeeded
                int error = 0;
                socklen_t len = sizeof(error);
                getsockopt(sock, SOL_SOCKET, SO_ERROR, &error, &len);

                if (error != 0) {
                    std::cout << "❌ Failed to connect to audio stream " << stream_info.call_id
                              << " on port " << stream_info.tcp_port << ": " << strerror(error) << std::endl;
                    close(sock);
                    return false;
                }
            } else if (select_result == 0) {
                std::cout << "❌ Connection timeout for audio stream " << stream_info.call_id
                          << " on port " << stream_info.tcp_port << std::endl;
                close(sock);
                return false;
            } else {
                std::cout << "❌ Select error for audio stream " << stream_info.call_id << std::endl;
                close(sock);
                return false;
            }
        } else {
            std::cout << "❌ Failed to connect to audio stream " << stream_info.call_id
                      << " on port " << stream_info.tcp_port << ": " << strerror(errno) << std::endl;
            close(sock);
            return false;
        }
    }

    // Set socket back to blocking mode
    fcntl(sock, F_SETFL, flags);

    std::cout << "✅ Connected to audio stream " << stream_info.call_id
              << " on port " << stream_info.tcp_port << std::endl;

    // Store connection
    {
        std::lock_guard<std::mutex> lock(tcp_mutex_);
        call_tcp_sockets_[stream_info.call_id] = sock;

        // Start TCP handler thread and detach it (thread will clean itself up)
        std::thread handler_thread(&StandaloneWhisperService::handle_tcp_audio_stream, this, stream_info.call_id, sock);
        handler_thread.detach();
    }

    return true;
}

bool StandaloneWhisperService::create_session(const std::string& call_id) {
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);

        if (sessions_.find(call_id) != sessions_.end()) {
            return false; // Session already exists
        }

        // Provide shared preloaded context to the session
        WhisperSessionConfig cfg = config_;
        cfg.shared_ctx = warm_ctx_;
        cfg.shared_mutex = &warm_mutex_;

        auto session = std::make_unique<WhisperSession>(call_id, cfg);
        if (!session->is_active()) {
            return false;
        }

        sessions_[call_id] = std::move(session);
        std::cout << "🎤 Created whisper session for call " << call_id << std::endl;
    } // Release sessions_mutex_ before connecting to LLaMA

    // Immediately connect to LLaMA service to eliminate first-transcription delay
    // This is done OUTSIDE the sessions_mutex_ lock to avoid holding it during network I/O
    if (connect_llama_for_call(call_id)) {
        std::cout << "🔗 Pre-connected to LLaMA service for call " << call_id << std::endl;
    } else {
        std::cout << "⚠️ Failed to pre-connect to LLaMA service for call " << call_id << " (will retry on first transcription)" << std::endl;
    }

    return true;
}

bool StandaloneWhisperService::destroy_session(const std::string& call_id) {
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);

        auto it = sessions_.find(call_id);
        if (it != sessions_.end()) {
            sessions_.erase(it);
        } else {
            // Session not found, but still clean up TCP socket
            std::cout << "⚠️ Session not found for call " << call_id << " during destroy" << std::endl;
        }
    }

    // Clean up TCP socket for this call (outside sessions_mutex_ to avoid lock ordering issues)
    {
        std::lock_guard<std::mutex> lock(tcp_mutex_);
        auto tcp_it = call_tcp_sockets_.find(call_id);
        if (tcp_it != call_tcp_sockets_.end()) {
            int sock = tcp_it->second;
            if (sock >= 0) {
                close(sock);
                std::cout << "🔌 Closed TCP socket (FD " << sock << ") for call " << call_id << std::endl;
            }
            call_tcp_sockets_.erase(tcp_it);
            std::cout << "🧹 Removed call " << call_id << " from TCP sockets map" << std::endl;
        }
    }

    // DO NOT close LLaMA socket - keep it open for next call
    // The socket is per-call_id and will be reused or cleaned up on service stop
    // This enables sessionless architecture where TCP connections persist across calls

    // Force immediate discovery on next loop to pick up any new active calls
    last_discovery_ = std::chrono::steady_clock::time_point{};

    std::cout << "🗑️ Destroyed whisper session for call " << call_id << " (keeping LLaMA connection open)" << std::endl;
    return true;
}

void StandaloneWhisperService::cleanup_inactive_sessions() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    auto now = std::chrono::steady_clock::now();
    auto it = sessions_.begin();

    while (it != sessions_.end()) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second->get_last_activity());

        if (age.count() > 300) { // 5 minutes timeout
            std::cout << "🗑️ Removing inactive session: " << it->first << std::endl;
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

void StandaloneWhisperService::log_session_stats() const {
    // Note: Can't lock const mutex, so just read size without lock for stats
    // std::cout << "📊 Active whisper sessions: " << sessions_.size() << std::endl;  // suppressed
}

void StandaloneWhisperService::handle_tcp_audio_stream(const std::string& call_id, int socket) {
    std::cout << "🎧 Starting TCP audio handler for call " << call_id << std::endl;

    // Read HELLO message
    std::string received_call_id;
    if (!read_tcp_hello(socket, received_call_id)) {
        std::cout << "❌ Failed to read TCP HELLO for call " << call_id << std::endl;
        // Cleanup on early exit
        close(socket);
        {
            std::lock_guard<std::mutex> lock(tcp_mutex_);
            call_tcp_sockets_.erase(call_id);
        }
        return;
    }

    if (received_call_id != call_id) {
        std::cout << "⚠️ Call ID mismatch: expected " << call_id << ", got " << received_call_id << std::endl;
    }

    // Ensure LLaMA connection for this call
    connect_llama_for_call(call_id);

    // Process audio chunks
    while (running_.load()) {
        std::vector<float> audio_samples;

        if (!read_tcp_audio_chunk(socket, audio_samples)) {
            break; // Connection closed or error
        }

        if (audio_samples.empty()) {
            continue; // No audio data
        }

        // Process audio with whisper session
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = sessions_.find(call_id);
            if (it != sessions_.end()) {
                if (it->second->process_audio_chunk(audio_samples)) {
                    // Check for new transcription
                    std::string transcription = it->second->get_latest_transcription();
                    if (!transcription.empty()) {
                        // Send transcription via TCP back to audio source (optional)
                        send_tcp_transcription(socket, transcription);

                        // Append transcription to database
                        if (database_) {
                            database_->append_transcription(call_id, transcription);
                        }

                        // Filter out incomplete/short transcriptions before forwarding to LLaMA
                        // Only forward if it's a complete sentence or meaningful phrase
                        std::string trimmed = transcription;
                        // Trim whitespace
                        size_t start = trimmed.find_first_not_of(" \t\n\r");
                        size_t end = trimmed.find_last_not_of(" \t\n\r");
                        if (start != std::string::npos && end != std::string::npos) {
                            trimmed = trimmed.substr(start, end - start + 1);
                        }

                        // Count words (simple space-based count)
                        int word_count = 0;
                        bool in_word = false;
                        for (char c : trimmed) {
                            if (std::isspace(c)) {
                                in_word = false;
                            } else if (!in_word) {
                                word_count++;
                                in_word = true;
                            }
                        }

                        // Only forward if:
                        // 1. At least 2 words (filters out "Hello?", "But they...")
                        // 2. OR ends with punctuation (complete sentence)
                        bool should_forward = false;
                        if (word_count >= 2) {
                            should_forward = true;
                        } else if (word_count >= 1 && !trimmed.empty()) {
                            char last_char = trimmed.back();
                            if (last_char == '.' || last_char == '!' || last_char == '?') {
                                should_forward = true;
                            }
                        }

                        if (should_forward) {
                            // Forward to LLaMA service via dedicated TCP connection
                            std::cout << "➡️ Forwarding to LLaMA [" << call_id << "]: " << transcription << std::endl;
                            send_llama_text(call_id, transcription);
                        } else {
                            std::cout << "⏭️ Skipping incomplete transcription [" << call_id << "]: " << transcription
                                      << " (words=" << word_count << ")" << std::endl;
                        }
                    }
                }
            }
        }
    }

    send_tcp_bye(socket);
    std::cout << "📡 TCP BYE sent to audio input socket for call " << call_id << std::endl;

    // Send BYE to llama-service socket
    {
        std::lock_guard<std::mutex> tlock(tcp_mutex_);
        auto llama_it = llama_sockets_.find(call_id);
        if (llama_it != llama_sockets_.end() && llama_it->second >= 0) {
            std::cout << "📡 Sending BYE to llama socket for call " << call_id << std::endl;
            send_tcp_bye(llama_it->second);
            close(llama_it->second);
            llama_sockets_.erase(llama_it);
            std::cout << "📡 TCP BYE sent to llama socket for call " << call_id << std::endl;
        } else {
            std::cout << "⚠️  No llama socket found for call " << call_id << std::endl;
        }
    }

    // Close socket and remove from map
    close(socket);
    {
        std::lock_guard<std::mutex> tlock(tcp_mutex_);
        auto it = call_tcp_sockets_.find(call_id);
        if (it != call_tcp_sockets_.end()) {
            call_tcp_sockets_.erase(it);
        }
    }
    // Destroy whisper session
    destroy_session(call_id);
    std::cout << "🎧 TCP audio handler ended for call " << call_id << std::endl;
    // log_session_stats();  // suppressed to reduce console spam
}

bool StandaloneWhisperService::read_tcp_hello(int socket, std::string& call_id) {
    uint32_t length;
    if (!read_exact_fd(socket, &length, 4)) {
        return false;
    }

    length = ntohl(length);
    if (length == 0 || length > 1000) {
        return false;
    }

    std::vector<char> buffer(length + 1);
    if (!read_exact_fd(socket, buffer.data(), length)) {
        return false;
    }

    buffer[length] = '\0';
    call_id = std::string(buffer.data());

    std::cout << "📡 TCP HELLO received: " << call_id << std::endl;
    return true;
}

bool StandaloneWhisperService::read_tcp_audio_chunk(int socket, std::vector<float>& audio_samples) {
    uint32_t length;
    if (!read_exact_fd(socket, &length, 4)) {
        return false;
    }

    length = ntohl(length);

    // Check for BYE message
    if (length == 0xFFFFFFFF) {
        std::cout << "📡 TCP BYE received" << std::endl;
        return false;
    }

    if (length == 0 || length > 2000000) { // up to ~30s of 16kHz float32
        return false;
    }

    // Read audio data
    size_t float_count = length / sizeof(float);
    audio_samples.resize(float_count);

    if (!read_exact_fd(socket, audio_samples.data(), length)) {
        return false;
    }

    // Simple stats for visibility
    double sumsq = 0.0;
    for (size_t i = 0; i < float_count; ++i) { double v = audio_samples[i]; sumsq += (double)v * (double)v; }
    double rms = float_count ? std::sqrt(sumsq / (double)float_count) : 0.0;
    double approx_sr = 16000.0; // pipeline default
    double secs = float_count / approx_sr;
    std::cout << "📤 TCP audio chunk received: " << float_count << " samples (~" << secs << " s), RMS=" << rms << std::endl;
    return true;
}

bool StandaloneWhisperService::send_tcp_transcription(int socket, const std::string& transcription) {
    uint32_t length = htonl(transcription.length());

    // Send length prefix
    if (!write_all_fd(socket, &length, 4)) {
        return false;
    }

    // Send transcription text
    if (!transcription.empty() && !write_all_fd(socket, transcription.c_str(), transcription.length())) {
        return false;
    }

    std::cout << "📝 TCP transcription sent: " << transcription << std::endl;
    return true;
}


// LLaMA client helpers
bool StandaloneWhisperService::connect_llama_for_call(const std::string& call_id) {
    // Hold tcp_mutex_ for the entire connect + publish to prevent duplicate races
    std::unique_lock<std::mutex> lock(tcp_mutex_);
    if (llama_sockets_.find(call_id) != llama_sockets_.end()) return true;

    const int max_attempts = 10;

    for (int attempt = 1; attempt <= max_attempts; ++attempt) {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) {
            if (attempt < max_attempts) {
                // Fast retry - 50ms for real-time speed
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(llama_port_);
        addr.sin_addr.s_addr = inet_addr(llama_host_.c_str());

        if (connect(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
            close(s);
            if (attempt < max_attempts) {
                // Fast retry - 50ms for real-time speed
                if (attempt == 1 || attempt == 5 || attempt == max_attempts - 1) {
                    std::cout << "⚠️ LLaMA connection attempt " << attempt << "/" << max_attempts
                              << " failed for call " << call_id << " - retrying in 50ms" << std::endl;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            std::cout << "❌ Failed to connect to LLaMA for call " << call_id
                      << " after " << max_attempts << " attempts" << std::endl;
            return false;
        }

        // Send HELLO(call_id)
        uint32_t n = htonl((uint32_t)call_id.size());
        if (send(s, &n, 4, 0) != 4) {
            close(s);
            if (attempt < max_attempts) {
                // Fast retry - 50ms for real-time speed
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            return false;
        }
        if (send(s, call_id.data(), call_id.size(), 0) != (ssize_t)call_id.size()) {
            close(s);
            if (attempt < max_attempts) {
                // Fast retry - 50ms for real-time speed
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }
            return false;
        }

        // Publish the connected socket while still under the mutex
        llama_sockets_[call_id] = s;
        std::cout << "🦙 Connected to LLaMA for call " << call_id << " at " << llama_host_
                  << ":" << llama_port_ << " (attempt " << attempt << ")" << std::endl;
        return true;
    }

    return false;
}

bool StandaloneWhisperService::send_llama_text(const std::string& call_id, const std::string& text) {
    // First, try to get a socket without holding it across a potential connect
    int s = -1;
    {
        std::lock_guard<std::mutex> lock(tcp_mutex_);
        auto it = llama_sockets_.find(call_id);
        if (it != llama_sockets_.end()) s = it->second;
    }

    if (s < 0) {
        if (!connect_llama_for_call(call_id)) return false;
        std::lock_guard<std::mutex> lock(tcp_mutex_);
        auto it2 = llama_sockets_.find(call_id);
        if (it2 == llama_sockets_.end()) return false;
        s = it2->second;
    }

    uint32_t l = htonl((uint32_t)text.size());
    if (!write_all_fd(s, &l, 4) || (!text.empty() && !write_all_fd(s, text.data(), text.size()))) {
        // On failure, drop the socket so future sends can reconnect cleanly
        std::lock_guard<std::mutex> lock(tcp_mutex_);
        auto it3 = llama_sockets_.find(call_id);
        if (it3 != llama_sockets_.end()) {
            close(it3->second);
            llama_sockets_.erase(it3);
        }
        return false;
    }
    return true;
}

void StandaloneWhisperService::send_tcp_bye(int socket) {
    uint32_t bye_marker = 0xFFFFFFFF;
    write_all_fd(socket, &bye_marker, 4);
    std::cout << "📡 TCP BYE sent" << std::endl;
}

// Post-processing to improve transcription accuracy
std::string StandaloneWhisperService::post_process_transcription(const std::string& text) {
    if (text.empty()) return text;

    std::string result = text;

    // 1. Remove duplicate words at boundaries (e.g., "smooth smooth" → "smooth")
    // Pattern: word followed by space and same word
    size_t pos = 0;
    while ((pos = result.find(' ', pos)) != std::string::npos) {
        if (pos + 1 < result.size()) {
            // Find the word before the space
            size_t word_start = result.rfind(' ', pos - 1);
            if (word_start == std::string::npos) word_start = 0;
            else word_start++;

            std::string word1 = result.substr(word_start, pos - word_start);

            // Find the word after the space
            size_t word_end = result.find(' ', pos + 1);
            if (word_end == std::string::npos) word_end = result.size();

            std::string word2 = result.substr(pos + 1, word_end - pos - 1);

            // If words match (case-insensitive), remove the duplicate
            if (!word1.empty() && !word2.empty()) {
                std::string w1_lower = word1, w2_lower = word2;
                std::transform(w1_lower.begin(), w1_lower.end(), w1_lower.begin(), ::tolower);
                std::transform(w2_lower.begin(), w2_lower.end(), w2_lower.begin(), ::tolower);

                if (w1_lower == w2_lower) {
                    result.erase(pos, word_end - pos);
                    continue;
                }
            }
        }
        pos++;
    }

    // 2. Normalize common contractions
    // "It is" → "It's"
    pos = 0;
    while ((pos = result.find("It is", pos)) != std::string::npos) {
        // Check if it's at word boundary
        bool at_start = (pos == 0 || result[pos-1] == ' ' || result[pos-1] == '\n');
        bool at_end = (pos + 5 >= result.size() || result[pos+5] == ' ' || result[pos+5] == '\n');
        if (at_start && at_end) {
            result.replace(pos, 5, "It's");
        }
        pos += 4;
    }

    // 3. Capitalize first letter if it's lowercase
    if (!result.empty() && result[0] >= 'a' && result[0] <= 'z') {
        result[0] = result[0] - 'a' + 'A';
    }

    // 4. Capitalize after sentence endings (. ! ?)
    for (size_t i = 0; i + 2 < result.size(); i++) {
        if ((result[i] == '.' || result[i] == '!' || result[i] == '?') && result[i+1] == ' ') {
            if (result[i+2] >= 'a' && result[i+2] <= 'z') {
                result[i+2] = result[i+2] - 'a' + 'A';
            }
        }
    }

    return result;
}
