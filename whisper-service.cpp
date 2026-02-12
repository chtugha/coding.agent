// Whisper Service (Consolidated)
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <chrono>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/un.h>
#include "whisper-cpp/include/whisper.h"

class ControlSignalSender {
public:
    bool send_signal(const std::string& socket_path, const std::string& message) {
        int sock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (sock < 0) return false;

        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 100000;
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);

        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            close(sock);
            return false;
        }

        ssize_t sent = send(sock, message.c_str(), message.length(), MSG_NOSIGNAL);
        close(sock);
        return sent > 0;
    }
};

class ControlListener {
public:
    ControlListener(const std::string& socket_path, std::function<void(const std::string&)> callback)
        : socket_path_(socket_path), callback_(callback), running_(false) {}

    ~ControlListener() {
        stop();
    }

    void start() {
        running_ = true;
        listen_thread_ = std::thread(&ControlListener::listen_loop, this);
    }

    void stop() {
        running_ = false;
        if (listen_thread_.joinable()) {
            listen_thread_.join();
        }
        unlink(socket_path_.c_str());
    }

private:
    void listen_loop() {
        unlink(socket_path_.c_str());
        int lsock = socket(AF_UNIX, SOCK_STREAM, 0);
        if (lsock < 0) {
            std::cerr << "❌ Failed to create control socket: " << socket_path_ << std::endl;
            return;
        }

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

        if (bind(lsock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "❌ Failed to bind control socket: " << socket_path_ << std::endl;
            close(lsock);
            return;
        }

        listen(lsock, 10);
        std::cout << "🎧 Control listener started: " << socket_path_ << std::endl;

        while (running_) {
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(lsock, &readfds);
            
            struct timeval tv;
            tv.tv_sec = 1;
            tv.tv_usec = 0;

            int ret = select(lsock + 1, &readfds, NULL, NULL, &tv);
            if (ret <= 0) continue;

            int csock = accept(lsock, NULL, NULL);
            if (csock < 0) continue;

            char buf[256];
            ssize_t n = recv(csock, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                std::string msg(buf);
                callback_(msg);
            }
            close(csock);
        }
        close(lsock);
    }

    std::string socket_path_;
    std::function<void(const std::string&)> callback_;
    std::atomic<bool> running_;
    std::thread listen_thread_;
};

struct WhisperCall {
    int id;
    int tcp_socket = -1;
    std::atomic<bool> connected{false};
    std::vector<float> audio_buffer;
    std::mutex mutex;
    std::string latest_text;
    bool in_speech = false;
    int silence_count = 0;
};

class WhisperService {
public:
    WhisperService(const std::string& model_path) : running_(true), model_path_(model_path),
        ctrl_listener_("/tmp/whisper-service.ctrl",
                       [this](const std::string& msg) { handle_control_signal(msg); }) {
        whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu = true;
        ctx_ = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    }

    ~WhisperService() {
        if (ctx_) whisper_free(ctx_);
    }

    void run() {
        ctrl_listener_.start();
        std::thread listen_thread(&WhisperService::listen_loop, this);
        std::thread llama_thread(&WhisperService::llama_connector_loop, this);
        while (running_) {
            process_calls();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        listen_thread.join();
        llama_thread.join();
        ctrl_listener_.stop();
    }

private:
    void handle_control_signal(const std::string& msg) {
        if (msg.find("CALL_START:") == 0) {
            int call_id = std::stoi(msg.substr(11));
            std::cout << "🚦 CALL_START received for call_id " << call_id << std::endl;
            
            auto call = get_or_create_call(call_id);
            std::cout << "📋 Prepared Whisper listener for call_id " << call_id << std::endl;
            
            ctrl_sender_.send_signal("/tmp/llama-service.ctrl", msg);
        } else if (msg.find("CALL_END:") == 0) {
            int call_id = std::stoi(msg.substr(9));
            std::cout << "🚦 CALL_END received for call_id " << call_id << std::endl;
            
            ctrl_sender_.send_signal("/tmp/llama-service.ctrl", msg);
            
            std::lock_guard<std::mutex> lock(calls_mutex_);
            if (calls_.count(call_id)) {
                auto call = calls_[call_id];
                std::lock_guard<std::mutex> clock(call->mutex);
                
                call->connected = false;
                if (call->tcp_socket != -1) {
                    close(call->tcp_socket);
                    call->tcp_socket = -1;
                }
                
                call->audio_buffer.clear();
                call->in_speech = false;
                call->silence_count = 0;
                
                calls_.erase(call_id);
                std::cout << "🧹 Stopped transcription and cleaned up call_id " << call_id << std::endl;
            }
        }
    }

    void listen_loop() {
        // We'll listen on multiple ports for multiple calls?
        // Or one port that receives all?
        // The Inbound Processor sends to 13000 + CID.
        // So we need to probe/listen on many ports or have a fixed pool.
        // For consolidation, let's just listen on a range.
        for (int i = 1; i <= 10; ++i) {
            std::thread([this, i]() {
                int lsock = socket(AF_INET, SOCK_STREAM, 0);
                int opt = 1; setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
                struct sockaddr_in addr{};
                addr.sin_family = AF_INET;
                addr.sin_port = htons(13000 + i);
                addr.sin_addr.s_addr = INADDR_ANY;
                if (bind(lsock, (struct sockaddr*)&addr, sizeof(addr)) < 0) return;
                listen(lsock, 1);
                while (running_) {
                    struct sockaddr_in client_addr{};
                    socklen_t slen = sizeof(client_addr);
                    int csock = accept(lsock, (struct sockaddr*)&client_addr, &slen);
                    if (csock < 0) continue;
                    handle_inbound_connection(i, csock);
                }
            }).detach();
        }
    }

    void handle_inbound_connection(int cid, int sock) {
        auto call = get_or_create_call(cid);
        call->tcp_socket = sock;
        call->connected = true;
        std::cout << "📥 Inbound audio connected for call " << cid << std::endl;
        
        float buf[1024];
        while (running_ && call->connected) {
            ssize_t n = recv(sock, buf, sizeof(buf), 0);
            if (n <= 0) break;
            std::lock_guard<std::mutex> lock(call->mutex);
            call->audio_buffer.insert(call->audio_buffer.end(), buf, buf + (n / 4));
        }
        call->connected = false;
        close(sock);
        send_to_llama(cid, "CLEAR");
        std::cout << "📥 Inbound audio disconnected for call " << cid << std::endl;
    }

    void process_calls() {
        std::vector<std::shared_ptr<WhisperCall>> active;
        {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            for (auto& p : calls_) active.push_back(p.second);
        }

        for (auto& call : active) {
            std::vector<float> to_process;
            {
                std::lock_guard<std::mutex> lock(call->mutex);
                
                // Process in 100ms chunks (1600 samples)
                while (call->audio_buffer.size() >= 1600) {
                    float energy = 0;
                    for (int i = 0; i < 1600; ++i) energy += call->audio_buffer[i] * call->audio_buffer[i];
                    energy /= 1600.0f;

                    if (energy > 0.00005f) { // Slightly more sensitive for telephony
                        call->in_speech = true;
                        call->silence_count = 0;
                    } else {
                        if (call->in_speech) call->silence_count++;
                    }

                    // If we found speech and then silence, or buffer too long
                    if (call->in_speech && call->silence_count > 8) { // ~800ms silence after speech
                        to_process = std::move(call->audio_buffer);
                        call->in_speech = false;
                        call->silence_count = 0;
                        break;
                    } else if (call->audio_buffer.size() > 16000 * 8) { // Max 8s safety limit
                        to_process = std::move(call->audio_buffer);
                        call->in_speech = false;
                        call->silence_count = 0;
                        break;
                    }
                    
                    // If not triggering yet, we just keep accumulating. 
                    // To avoid infinite accumulation if no silence is found, we have the 8s limit.
                    // We only "break" the while loop if we decided to process.
                    break; 
                }
            }

            if (!to_process.empty()) {
                whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
                wparams.language = "de"; // German as requested
                wparams.n_threads = 4;
                wparams.no_timestamps = true;

                std::lock_guard<std::mutex> lock(whisper_mutex_);
                if (whisper_full(ctx_, wparams, to_process.data(), to_process.size()) == 0) {
                    int n_segments = whisper_full_n_segments(ctx_);
                    std::string text;
                    for (int i = 0; i < n_segments; ++i) {
                        text += whisper_full_get_segment_text(ctx_, i);
                    }
                    if (!text.empty()) {
                        std::cout << "📝 [" << call->id << "] Transcription: " << text << std::endl;
                        send_to_llama(call->id, text);
                    }
                }
            }
        }
    }

    void llama_connector_loop() {
        // Just keep a persistent connection or open/close?
        // Let's try to connect to port 8083.
    }

    void send_to_llama(int cid, const std::string& text) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(8083);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            std::string msg = std::to_string(cid) + ":" + text;
            send(sock, msg.c_str(), msg.length(), 0);
        }
        close(sock);
    }

    std::shared_ptr<WhisperCall> get_or_create_call(int cid) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(cid)) return calls_[cid];
        auto call = std::make_shared<WhisperCall>();
        call->id = cid;
        calls_[cid] = call;
        return call;
    }

    std::atomic<bool> running_;
    std::string model_path_;
    struct whisper_context* ctx_ = nullptr;
    std::mutex whisper_mutex_;
    std::mutex calls_mutex_;
    std::map<int, std::shared_ptr<WhisperCall>> calls_;
    ControlListener ctrl_listener_;
    ControlSignalSender ctrl_sender_;
};

int main(int argc, char** argv) {
    if (argc < 2) { std::cout << "Usage: whisper-service <model_path>" << std::endl; return 1; }
    WhisperService service(argv[1]);
    service.run();
    return 0;
}
