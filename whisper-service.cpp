#include "whisper-service.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cstring>
#include <sys/stat.h>
#include <algorithm>
#include <cmath>
#include <iomanip>

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

    // VAD-based processing for better sentence awareness
    std::lock_guard<std::recursive_mutex> lock(session_mutex_);
    audio_buffer_.insert(audio_buffer_.end(), audio_samples.begin(), audio_samples.end());

    // std::cout << "DEBUG [" << call_id_ << "] Buffer size: " << audio_buffer_.size() << " samples" << std::endl;
    
    if (audio_buffer_.size() > 1000000) {
        std::cout << "⚠️ [" << call_id_ << "] Buffer overflow, clearing" << std::endl;
        audio_buffer_.clear();
    }

    const size_t window_size = 320; // 20ms at 16kHz
    const float vad_threshold_sq = 0.00000225f; // 0.0015^2
    const float vad_stop_threshold_sq = vad_threshold_sq * 0.25f;
    
    size_t processed_until = 0;
    bool any_inference = false;

    while (processed_until + window_size <= audio_buffer_.size()) {
        std::vector<float> window(audio_buffer_.begin() + processed_until, 
                                  audio_buffer_.begin() + processed_until + window_size);
        float energy_sq = calculate_energy(window);
        
        bool speech_now = in_speech_ ? (energy_sq > vad_stop_threshold_sq) : (energy_sq > vad_threshold_sq);

        if (speech_now) {
            consec_speech_++;
            consec_silence_ = 0;
        } else {
            consec_silence_++;
            consec_speech_ = 0;
        }

        if (!in_speech_) {
            prebuffer_.insert(prebuffer_.end(), window.begin(), window.end());
            if (prebuffer_.size() > 8000) { // 500ms
                prebuffer_.erase(prebuffer_.begin(), prebuffer_.begin() + window_size);
            }

            if (consec_speech_ >= 2) { // 40ms of speech
                in_speech_ = true;
                std::cout << "🎙️ [" << call_id_ << "] VAD: Speech started" << std::endl;
                current_segment_.insert(current_segment_.end(), prebuffer_.begin(), prebuffer_.end());
                prebuffer_.clear();
            }
        }

        if (in_speech_) {
            current_segment_.insert(current_segment_.end(), window.begin(), window.end());
            
            // End of segment detected by silence or max length
            // Using 500ms-800ms silence hangover for better natural sentences
            if (consec_silence_ >= 30 || current_segment_.size() >= 320000) { // 600ms silence or 20s audio
                if (current_segment_.size() > 4800) { // Min 300ms
                    std::cout << "🎙️ [" << call_id_ << "] VAD: Segment complete (" << current_segment_.size() << " samples)" << std::endl;
                    if (process_window(current_segment_)) {
                        any_inference = true;
                    }
                }
                current_segment_.clear();
                in_speech_ = false;
                consec_silence_ = 0;
                consec_speech_ = 0;
            }
        }
        
        processed_until += window_size;
    }

    if (processed_until > 0) {
        audio_buffer_.erase(audio_buffer_.begin(), audio_buffer_.begin() + processed_until);
    }

    return any_inference;
}

float WhisperSession::calculate_energy(const std::vector<float>& samples) {
    if (samples.empty()) return 0.0f;
    float sum = 0.0f;
    for (float s : samples) sum += s * s;
    return sum / samples.size();
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
    // Optimized for real-time speed and sentence awareness
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.language = config_.language.c_str();
    wparams.n_threads = config_.n_threads;
    wparams.temperature = 0.0f;
    wparams.no_timestamps = true;
    wparams.translate = false;
    wparams.print_progress = false;
    wparams.print_realtime = false;
    
    // Performance optimizations
    wparams.single_segment = false;     // Allow multiple segments if Whisper finds them
    wparams.suppress_blank = true;      // Avoid [BLANK_AUDIO] hallucinations
    wparams.suppress_nst = true;        // Non-speech tokens suppression
    wparams.max_tokens = 0;             // No limit on tokens
    
    // Speedup inference - sentence awareness handled by internal VAD
    wparams.no_context = false;         // Enable context for better accuracy (VAD ensures clean segments)
    wparams.audio_ctx = 0;              // Default context size
    wparams.greedy.best_of = 1;         // Greedy only

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
            std::lock_guard<std::recursive_mutex> lock(session_mutex_);
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
    std::lock_guard<std::recursive_mutex> lock(session_mutex_);
    std::string result = latest_transcription_;
    latest_transcription_.clear(); // Clear after reading
    return result;
}

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
    registration_socket_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (registration_socket_ < 0) {
        std::cerr << "❌ Failed to create registration UDP socket" << std::endl;
        return;
    }

    int reuse = 1;
    setsockopt(registration_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
    setsockopt(registration_socket_, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

    int rcvbuf = 256 * 1024;
    setsockopt(registration_socket_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

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

    std::cout << "📡 Whisper registration listener thread started" << std::endl;

    while (registration_running_.load()) {
        client_len = sizeof(client_addr);
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(registration_socket_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        ssize_t n = recvfrom(registration_socket_, buffer, sizeof(buffer) - 1, 0,
                             (struct sockaddr*)&client_addr, &client_len);

        if (n > 0) {
            buffer[n] = '\0';
            std::string message(buffer);

            if (message.rfind("REGISTER:", 0) == 0) {
                std::string call_id = message.substr(9);
                // trim
                call_id.erase(0, call_id.find_first_not_of(" \t\r\n"));
                call_id.erase(call_id.find_last_not_of(" \t\r\n") + 1);

                bool already_connected = false;
                {
                    std::lock_guard<std::mutex> lock(tcp_mutex_);
                    if (call_tcp_sockets_.find(call_id) != call_tcp_sockets_.end()) {
                        already_connected = true;
                    }
                }

                if (!already_connected) {
                    AudioStreamInfo stream;
                    stream.call_id = call_id;
                    
                    // Consistent port calculation matching InboundAudioProcessor
                    unsigned int hash = 0;
                    for (char c : call_id) hash = hash * 31 + c;
                    stream.tcp_port = 13001 + (hash % 1000);
                    
                    stream.stream_type = "inbound";
                    
                    std::thread([this, stream]() {
                        if (connect_to_audio_stream(stream)) {
                            create_session(stream.call_id);
                        }
                    }).detach();
                }
            } else if (message.rfind("BYE:", 0) == 0) {
                std::string call_id = message.substr(4);
                call_id.erase(0, call_id.find_first_not_of(" \t\r\n"));
                call_id.erase(call_id.find_last_not_of(" \t\r\n") + 1);
                destroy_session(call_id);
            }
        }
    }
}

void StandaloneWhisperService::run_service_loop() {
    while (running_.load()) {
        discover_and_connect_streams();
        cleanup_inactive_sessions();
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

void StandaloneWhisperService::discover_and_connect_streams() {
    if (!database_) return;
    auto active_calls = database_->get_active_calls();
    for (const auto& call : active_calls) {
        std::string cid = std::to_string(call.id);
        {
            std::lock_guard<std::mutex> lock(tcp_mutex_);
            if (call_tcp_sockets_.find(cid) != call_tcp_sockets_.end()) continue;
        }
        AudioStreamInfo stream;
        stream.call_id = cid;
        
        // Consistent port calculation matching InboundAudioProcessor
        unsigned int hash = 0;
        for (char c : cid) hash = hash * 31 + c;
        stream.tcp_port = 13001 + (hash % 1000);
        
        if (connect_to_audio_stream(stream)) {
            create_session(cid);
        }
    }
}

bool StandaloneWhisperService::connect_to_audio_stream(const AudioStreamInfo& stream_info) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return false;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port = htons(stream_info.tcp_port);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(sock);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(tcp_mutex_);
        call_tcp_sockets_[stream_info.call_id] = sock;
        std::thread(&StandaloneWhisperService::handle_tcp_audio_stream, this, stream_info.call_id, sock).detach();
    }
    return true;
}

bool StandaloneWhisperService::create_session(const std::string& call_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    if (sessions_.find(call_id) != sessions_.end()) return false;

    WhisperSessionConfig cfg = config_;
    cfg.shared_ctx = warm_ctx_;
    cfg.shared_mutex = &warm_mutex_;

    auto session = std::make_unique<WhisperSession>(call_id, cfg);
    sessions_[call_id] = std::move(session);
    
    connect_llama_for_call(call_id);
    return true;
}

bool StandaloneWhisperService::destroy_session(const std::string& call_id) {
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.erase(call_id);
    }
    {
        std::lock_guard<std::mutex> lock(tcp_mutex_);
        auto it = call_tcp_sockets_.find(call_id);
        if (it != call_tcp_sockets_.end()) {
            close(it->second);
            call_tcp_sockets_.erase(it);
        }
    }
    return true;
}

void StandaloneWhisperService::cleanup_inactive_sessions() {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    auto now = std::chrono::steady_clock::now();
    for (auto it = sessions_.begin(); it != sessions_.end(); ) {
        if (std::chrono::duration_cast<std::chrono::seconds>(now - it->second->get_last_activity()).count() > 300) {
            it = sessions_.erase(it);
        } else {
            ++it;
        }
    }
}

void StandaloneWhisperService::handle_tcp_audio_stream(const std::string& call_id, int socket) {
    std::string received_call_id;
    if (!read_tcp_hello(socket, received_call_id)) {
        close(socket);
        return;
    }

    connect_llama_for_call(call_id);

    while (running_.load()) {
        std::vector<float> audio_samples;
        if (!read_tcp_audio_chunk(socket, audio_samples)) break;
        if (audio_samples.empty()) continue;

        WhisperSession* session = nullptr;
        {
            std::lock_guard<std::mutex> lock(sessions_mutex_);
            auto it = sessions_.find(call_id);
            if (it != sessions_.end()) session = it->second.get();
        }

        if (session) {
            if (session->process_audio_chunk(audio_samples)) {
                std::string raw_transcription = session->get_latest_transcription();
                if (!raw_transcription.empty()) {
                    std::string transcription = post_process_transcription(raw_transcription);
                    if (transcription.empty()) continue;

                    send_tcp_transcription(socket, transcription);
                    if (database_) database_->append_transcription(call_id, transcription);

                    // Forward to LLaMA logic
                    std::string trimmed = transcription;
                    trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
                    trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);

                    int word_count = 0;
                    bool in_word = false;
                    for (char c : trimmed) {
                        if (std::isspace(c)) in_word = false;
                        else if (!in_word) { word_count++; in_word = true; }
                    }

                    bool should_forward = (word_count >= 2);
                    if (!should_forward && word_count >= 1 && !trimmed.empty()) {
                        char last = trimmed.back();
                        if (last == '.' || last == '!' || last == '?') should_forward = true;
                    }

                    if (should_forward) {
                        send_llama_text(call_id, transcription);
                    }
                }
            }
        }
    }

    send_tcp_bye(socket);
    close(socket);
    {
        std::lock_guard<std::mutex> lock(tcp_mutex_);
        call_tcp_sockets_.erase(call_id);
        auto it = llama_sockets_.find(call_id);
        if (it != llama_sockets_.end()) {
            close(it->second);
            llama_sockets_.erase(it);
        }
    }
    destroy_session(call_id);
}

bool StandaloneWhisperService::read_tcp_hello(int socket, std::string& call_id) {
    uint32_t length;
    if (!read_exact_fd(socket, &length, 4)) return false;
    length = ntohl(length);
    if (length == 0 || length > 1000) return false;
    std::vector<char> buffer(length + 1);
    if (!read_exact_fd(socket, buffer.data(), length)) return false;
    buffer[length] = '\0';
    call_id = buffer.data();
    return true;
}

bool StandaloneWhisperService::read_tcp_audio_chunk(int socket, std::vector<float>& audio_samples) {
    uint32_t length;
    if (!read_exact_fd(socket, &length, 4)) return false;
    length = ntohl(length);
    if (length == 0xFFFFFFFF) return false;
    if (length == 0 || length > 2000000) return false;
    size_t count = length / sizeof(float);
    audio_samples.resize(count);
    return read_exact_fd(socket, audio_samples.data(), length);
}

bool StandaloneWhisperService::send_tcp_transcription(int socket, const std::string& transcription) {
    uint32_t length = htonl(transcription.length());
    if (!write_all_fd(socket, &length, 4)) return false;
    if (!transcription.empty()) return write_all_fd(socket, transcription.c_str(), transcription.length());
    return true;
}

bool StandaloneWhisperService::connect_llama_for_call(const std::string& call_id) {
    std::lock_guard<std::mutex> lock(tcp_mutex_);
    if (llama_sockets_.find(call_id) != llama_sockets_.end()) return true;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(llama_port_);
    addr.sin_addr.s_addr = inet_addr(llama_host_.c_str());

    if (connect(s, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        close(s);
        return false;
    }

    uint32_t n = htonl((uint32_t)call_id.size());
    send(s, &n, 4, 0);
    send(s, call_id.data(), call_id.size(), 0);
    llama_sockets_[call_id] = s;
    return true;
}

bool StandaloneWhisperService::send_llama_text(const std::string& call_id, const std::string& text) {
    int s = -1;
    {
        std::lock_guard<std::mutex> lock(tcp_mutex_);
        auto it = llama_sockets_.find(call_id);
        if (it != llama_sockets_.end()) s = it->second;
    }

    if (s < 0) {
        if (!connect_llama_for_call(call_id)) return false;
        std::lock_guard<std::mutex> lock(tcp_mutex_);
        s = llama_sockets_[call_id];
    }

    uint32_t l = htonl((uint32_t)text.size());
    if (!write_all_fd(s, &l, 4) || (!text.empty() && !write_all_fd(s, text.data(), text.size()))) {
        std::lock_guard<std::mutex> lock(tcp_mutex_);
        close(llama_sockets_[call_id]);
        llama_sockets_.erase(call_id);
        return false;
    }
    return true;
}

void StandaloneWhisperService::send_tcp_bye(int socket) {
    uint32_t bye = 0xFFFFFFFF;
    write_all_fd(socket, &bye, 4);
}

std::string StandaloneWhisperService::post_process_transcription(const std::string& text) {
    if (text.empty()) return "";
    std::string res = text;
    res.erase(0, res.find_first_not_of(" \t\r\n"));
    res.erase(res.find_last_not_of(" \t\r\n") + 1);
    
    // Hallucination filters (common in whisper silence)
    static const std::vector<std::string> filters = {
        "Thank you.", "Thanks for watching.", "Please subscribe.", 
        "Vielen Dank.", "Danke fürs Zuschauen.", "Abonnieren Sie."
    };
    
    for (const auto& f : filters) {
        if (res == f) return "";
    }

    if (res.empty()) return "";
    if (res[0] >= 'a' && res[0] <= 'z') res[0] -= 32;
    return res;
}
