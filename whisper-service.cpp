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
#include "whisper-cpp/include/whisper.h"

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
    WhisperService(const std::string& model_path) : running_(true), model_path_(model_path) {
        whisper_context_params cparams = whisper_context_default_params();
        cparams.use_gpu = true;
        ctx_ = whisper_init_from_file_with_params(model_path.c_str(), cparams);
    }

    ~WhisperService() {
        if (ctx_) whisper_free(ctx_);
    }

    void run() {
        std::thread listen_thread(&WhisperService::listen_loop, this);
        std::thread llama_thread(&WhisperService::llama_connector_loop, this);
        while (running_) {
            process_calls();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        listen_thread.join();
        llama_thread.join();
    }

private:
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
                if (call->audio_buffer.size() > 16000) { // 1 second
                    // Very basic VAD based on energy
                    float energy = 0;
                    for (float s : call->audio_buffer) energy += s * s;
                    energy /= call->audio_buffer.size();

                    if (energy > 0.0001f) {
                        call->in_speech = true;
                        call->silence_count = 0;
                    } else {
                        call->silence_count++;
                    }

                    if (call->in_speech && call->silence_count > 30) { // ~0.5s silence after speech
                        to_process = std::move(call->audio_buffer);
                        call->in_speech = false;
                        call->silence_count = 0;
                    } else if (call->audio_buffer.size() > 16000 * 10) { // Max 10s
                        to_process = std::move(call->audio_buffer);
                    }
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
};

int main(int argc, char** argv) {
    if (argc < 2) { std::cout << "Usage: whisper-service <model_path>" << std::endl; return 1; }
    WhisperService service(argv[1]);
    service.run();
    return 0;
}
