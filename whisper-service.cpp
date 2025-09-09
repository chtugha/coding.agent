#include "whisper-service.h"
#include <iostream>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <cstring>
#include <sys/stat.h>

// Real whisper.cpp integration
#include "whisper-cpp/include/whisper.h"

// WhisperSession Implementation
WhisperSession::WhisperSession(const std::string& call_id, const WhisperSessionConfig& config)
    : call_id_(call_id), ctx_(nullptr), is_active_(true), config_(config) {
    
    last_activity_ = std::chrono::steady_clock::now();
    
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
    if (ctx_) {
        whisper_free(ctx_);
        ctx_ = nullptr;
    }
}

bool WhisperSession::process_audio_chunk(const std::vector<float>& audio_samples) {
    if (!is_active_.load() || !ctx_) {
        return false;
    }
    
    std::lock_guard<std::mutex> lock(session_mutex_);
    
    // Add audio to buffer (no VAD - use chunks as provided)
    audio_buffer_.insert(audio_buffer_.end(), audio_samples.begin(), audio_samples.end());
    
    mark_activity();
    
    // Process if we have enough audio (e.g., 1 second worth)
    const size_t min_samples = config_.language == "en" ? 8000 : 16000; // 1 second at 8kHz/16kHz
    
    if (audio_buffer_.size() >= min_samples) {
        return process_buffered_audio();
    }
    
    return true;
}

bool WhisperSession::process_buffered_audio() {
    if (!ctx_ || audio_buffer_.empty()) {
        return false;
    }
    
    // Create real whisper parameters
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.language = config_.language.c_str();
    wparams.n_threads = config_.n_threads;
    wparams.temperature = config_.temperature;
    wparams.no_timestamps = config_.no_timestamps;
    wparams.translate = config_.translate;
    wparams.print_progress = false;
    wparams.print_realtime = false;
    
    // Process audio with whisper
    int result = whisper_full(ctx_, wparams, audio_buffer_.data(), audio_buffer_.size());
    
    if (result == 0) {
        // Extract transcription
        const int n_segments = whisper_full_n_segments(ctx_);
        std::string transcription;
        
        for (int i = 0; i < n_segments; ++i) {
            const char* text = whisper_full_get_segment_text(ctx_, i);
            transcription += text;
        }
        
        if (!transcription.empty()) {
            latest_transcription_ = transcription;
            std::cout << "📝 [" << call_id_ << "] Transcription: " << transcription << std::endl;
        }
        
        // Clear processed audio buffer
        audio_buffer_.clear();
        
        return true;
    }
    
    std::cout << "❌ Whisper processing failed for call " << call_id_ << std::endl;
    return false;
}

std::string WhisperSession::get_latest_transcription() {
    std::lock_guard<std::mutex> lock(session_mutex_);
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

    running_.store(true);

    // Start service discovery loop
    discovery_thread_ = std::thread(&StandaloneWhisperService::run_service_loop, this);

    std::cout << "🎤 Standalone Whisper Service started" << std::endl;
    std::cout << "📡 Model: " << config.model_path << std::endl;
    std::cout << "💾 Database: " << db_path << std::endl;
    std::cout << "🔍 Discovering audio streams on port 13000..." << std::endl;

    return true;
}

void StandaloneWhisperService::stop() {
    if (!running_.load()) return;
    
    running_.store(false);
    
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
        
        // Join all TCP threads
        for (auto& pair : call_tcp_threads_) {
            if (pair.second.joinable()) {
                pair.second.join();
            }
        }
        call_tcp_threads_.clear();
    }
    
    // Destroy all sessions
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.clear();
    }
    
    if (discovery_thread_.joinable()) {
        discovery_thread_.join();
    }
    
    std::cout << "🛑 Standalone Whisper Service stopped" << std::endl;
}

void StandaloneWhisperService::run_service_loop() {
    last_discovery_ = std::chrono::steady_clock::now();
    
    while (running_.load()) {
        auto now = std::chrono::steady_clock::now();
        
        // Discover new streams every 5 seconds
        if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_discovery_).count() > 5000) {
            discover_and_connect_streams();
            last_discovery_ = now;
        }
        
        // Cleanup inactive sessions
        cleanup_inactive_sessions();
        
        // Log session statistics
        log_session_stats();
        
        std::this_thread::sleep_for(std::chrono::milliseconds(1000));
    }
}

void StandaloneWhisperService::discover_and_connect_streams() {
    auto streams = service_discovery_->discover_streams("127.0.0.1", 13000);
    
    std::cout << "🔍 Discovered " << streams.size() << " audio streams" << std::endl;
    
    for (const auto& stream : streams) {
        // Check if we're already connected to this stream
        {
            std::lock_guard<std::mutex> lock(tcp_mutex_);
            if (call_tcp_sockets_.find(stream.call_id) != call_tcp_sockets_.end()) {
                continue; // Already connected
            }
        }
        
        std::cout << "🔗 Connecting to new audio stream: " << stream.call_id 
                  << " on port " << stream.tcp_port << std::endl;
        
        if (connect_to_audio_stream(stream)) {
            create_session(stream.call_id);
        }
    }
}

bool StandaloneWhisperService::connect_to_audio_stream(const AudioStreamInfo& stream_info) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        std::cout << "❌ Failed to create socket for call " << stream_info.call_id << std::endl;
        return false;
    }
    
    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_port = htons(stream_info.tcp_port);
    
    if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cout << "❌ Failed to connect to audio stream " << stream_info.call_id 
                  << " on port " << stream_info.tcp_port << std::endl;
        close(sock);
        return false;
    }
    
    std::cout << "✅ Connected to audio stream " << stream_info.call_id 
              << " on port " << stream_info.tcp_port << std::endl;
    
    // Store connection
    {
        std::lock_guard<std::mutex> lock(tcp_mutex_);
        call_tcp_sockets_[stream_info.call_id] = sock;
        
        // Start TCP handler thread
        call_tcp_threads_[stream_info.call_id] = std::thread(
            &StandaloneWhisperService::handle_tcp_audio_stream, this, stream_info.call_id, sock);
    }
    
    return true;
}

bool StandaloneWhisperService::create_session(const std::string& call_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    if (sessions_.find(call_id) != sessions_.end()) {
        return false; // Session already exists
    }
    
    auto session = std::make_unique<WhisperSession>(call_id, config_);
    if (!session->is_active()) {
        return false;
    }
    
    sessions_[call_id] = std::move(session);
    std::cout << "🎤 Created whisper session for call " << call_id << std::endl;
    
    return true;
}

bool StandaloneWhisperService::destroy_session(const std::string& call_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);
    
    auto it = sessions_.find(call_id);
    if (it != sessions_.end()) {
        sessions_.erase(it);
        std::cout << "🗑️ Destroyed whisper session for call " << call_id << std::endl;
        return true;
    }
    
    return false;
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
    if (!sessions_.empty()) {
        std::cout << "📊 Active whisper sessions: " << sessions_.size() << std::endl;
    }
}

void StandaloneWhisperService::handle_tcp_audio_stream(const std::string& call_id, int socket) {
    std::cout << "🎧 Starting TCP audio handler for call " << call_id << std::endl;

    // Read HELLO message
    std::string received_call_id;
    if (!read_tcp_hello(socket, received_call_id)) {
        std::cout << "❌ Failed to read TCP HELLO for call " << call_id << std::endl;
        return;
    }

    if (received_call_id != call_id) {
        std::cout << "⚠️ Call ID mismatch: expected " << call_id << ", got " << received_call_id << std::endl;
    }

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
                        // Send transcription via TCP
                        send_tcp_transcription(socket, transcription);

                        // Append transcription to database
                        if (database_) {
                            database_->append_transcription(call_id, transcription);
                        }
                    }
                }
            }
        }
    }

    send_tcp_bye(socket);
    std::cout << "🎧 TCP audio handler ended for call " << call_id << std::endl;
}

bool StandaloneWhisperService::read_tcp_hello(int socket, std::string& call_id) {
    uint32_t length;
    if (recv(socket, &length, 4, 0) != 4) {
        return false;
    }

    length = ntohl(length);
    if (length == 0 || length > 1000) {
        return false;
    }

    std::vector<char> buffer(length + 1);
    if (recv(socket, buffer.data(), length, 0) != (ssize_t)length) {
        return false;
    }

    buffer[length] = '\0';
    call_id = std::string(buffer.data());

    std::cout << "📡 TCP HELLO received: " << call_id << std::endl;
    return true;
}

bool StandaloneWhisperService::read_tcp_audio_chunk(int socket, std::vector<float>& audio_samples) {
    uint32_t length;
    ssize_t received = recv(socket, &length, 4, 0);

    if (received != 4) {
        return false;
    }

    length = ntohl(length);

    // Check for BYE message
    if (length == 0xFFFFFFFF) {
        std::cout << "📡 TCP BYE received" << std::endl;
        return false;
    }

    if (length == 0 || length > 1000000) {
        return false;
    }

    // Read audio data
    size_t float_count = length / sizeof(float);
    audio_samples.resize(float_count);

    if (recv(socket, audio_samples.data(), length, 0) != (ssize_t)length) {
        return false;
    }

    std::cout << "📤 TCP audio chunk received: " << float_count << " samples" << std::endl;
    return true;
}

bool StandaloneWhisperService::send_tcp_transcription(int socket, const std::string& transcription) {
    uint32_t length = htonl(transcription.length());

    // Send length prefix
    if (send(socket, &length, 4, 0) != 4) {
        return false;
    }

    // Send transcription text
    if (send(socket, transcription.c_str(), transcription.length(), 0) != (ssize_t)transcription.length()) {
        return false;
    }

    std::cout << "📝 TCP transcription sent: " << transcription << std::endl;
    return true;
}

void StandaloneWhisperService::send_tcp_bye(int socket) {
    uint32_t bye_marker = 0xFFFFFFFF;
    send(socket, &bye_marker, 4, 0);
    std::cout << "📡 TCP BYE sent" << std::endl;
}
