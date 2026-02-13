// LLaMA Service (Optimized for Apple Silicon & German)
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
#include "llama.h"

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

struct LlamaChatMessage {
    std::string role;
    std::string content;
};

struct LlamaCall {
    int id;
    int seq_id;
    int n_past = 0;
    std::vector<LlamaChatMessage> messages;
};

class LlamaService {
public:
    LlamaService(const std::string& model_path) : running_(true),
        ctrl_listener_("/tmp/llama-service.ctrl",
                       [this](const std::string& msg) { handle_control_signal(msg); }) {
        llama_backend_init();
        llama_model_params mparams = llama_model_default_params();
        mparams.n_gpu_layers = -1; // Use all layers on GPU if possible
        model_ = llama_model_load_from_file(model_path.c_str(), mparams);
        if (!model_) {
            throw std::runtime_error("Failed to load model: " + model_path);
        }
        
        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx = 2048; // Sufficient for phone conversations
        cparams.n_threads = 8; // Parallel threads for prompt processing
        cparams.n_threads_batch = 8;
        ctx_ = llama_init_from_model(model_, cparams);
        if (!ctx_) {
            throw std::runtime_error("Failed to initialize context");
        }
        
        vocab_ = llama_model_get_vocab(model_);
        sampler_ = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(sampler_, llama_sampler_init_greedy()); // Greedy for speed and consistency
        
        std::cout << "🚀 LLaMA Service optimized for Apple Silicon (Metal) initialized" << std::endl;
    }

    ~LlamaService() {
        if (sampler_) llama_sampler_free(sampler_);
        if (ctx_) llama_free(ctx_);
        if (model_) llama_model_free(model_);
        llama_backend_free();
    }

    void run() {
        ctrl_listener_.start();
        
        int lsock = socket(AF_INET, SOCK_STREAM, 0);
        int opt = 1; setsockopt(lsock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(8083);
        addr.sin_addr.s_addr = INADDR_ANY;
        
        if (bind(lsock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Failed to bind to port 8083" << std::endl;
            ctrl_listener_.stop();
            return;
        }
        listen(lsock, 10);

        std::cout << "🇩🇪 LLaMA German Service listening on port 8083" << std::endl;

        while (running_) {
            int csock = accept(lsock, NULL, NULL);
            if (csock < 0) continue;
            
            std::thread([this, csock]() {
                char buf[8192];
                ssize_t n = recv(csock, buf, sizeof(buf)-1, 0);
                if (n > 0) {
                    buf[n] = '\0';
                    std::string msg(buf);
                    size_t sep = msg.find(':');
                    if (sep != std::string::npos) {
                        int cid = std::stoi(msg.substr(0, sep));
                        std::string payload = msg.substr(sep + 1);
                        if (payload == "CLEAR") {
                            clear_call(cid);
                        } else {
                            std::string response = process_call(cid, payload);
                            send_to_tts(cid, response);
                        }
                    }
                }
                close(csock);
            }).detach();
        }
    }

private:
    void handle_control_signal(const std::string& msg) {
        if (msg.find("CALL_START:") == 0) {
            int call_id = std::stoi(msg.substr(11));
            std::cout << "🚦 CALL_START received for call_id " << call_id << std::endl;
            
            auto call = get_or_create_call(call_id);
            std::cout << "📋 Pre-allocated sequence ID " << call->seq_id << " for call_id " << call_id << std::endl;
            
            ctrl_sender_.send_signal("/tmp/kokoro-service.ctrl", msg);
        } else if (msg.find("CALL_END:") == 0) {
            int call_id = std::stoi(msg.substr(9));
            std::cout << "🚦 CALL_END received for call_id " << call_id << std::endl;
            
            ctrl_sender_.send_signal("/tmp/kokoro-service.ctrl", msg);
            
            std::lock_guard<std::mutex> lock(calls_mutex_);
            if (calls_.count(call_id)) {
                std::lock_guard<std::mutex> llama_lock(llama_mutex_);
                
                llama_memory_t mem = llama_get_memory(ctx_);
                llama_memory_seq_rm(mem, calls_[call_id]->seq_id, -1, -1);
                
                calls_.erase(call_id);
                std::cout << "🧹 Cleared conversation and stopped generation for call_id " << call_id << std::endl;
            }
        }
    }

    void clear_call(int cid) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(cid)) {
            std::lock_guard<std::mutex> llama_lock(llama_mutex_);
            llama_memory_t mem = llama_get_memory(ctx_);
            llama_memory_seq_rm(mem, calls_[cid]->seq_id, -1, -1);
            calls_.erase(cid);
            std::cout << "🦙 [" << cid << "] Session cleared" << std::endl;
        }
    }

    std::string process_call(int cid, const std::string& text) {
        auto call = get_or_create_call(cid);
        std::lock_guard<std::mutex> lock(llama_mutex_);

        // Update history
        call->messages.push_back({"user", text});

        // Apply template
        std::vector<llama_chat_message> chat_msgs;
        // System prompt for lightning fast German responses
        chat_msgs.push_back({"system", "Du bist ein extrem effizienter Telefon-Assistent. Antworte IMMER auf DEUTSCH. Deine Antworten sind extrem kurz (max. 15 Wörter). Sei höflich aber komm sofort zum Punkt."});
        
        for (const auto& m : call->messages) {
            chat_msgs.push_back({m.role.c_str(), m.content.c_str()});
        }

        const char * tmpl = llama_model_chat_template(model_, nullptr);
        std::vector<char> formatted(4096);
        int32_t len = llama_chat_apply_template(tmpl, chat_msgs.data(), chat_msgs.size(), true, formatted.data(), formatted.size());
        if (len > (int32_t)formatted.size()) {
            formatted.resize(len);
            len = llama_chat_apply_template(tmpl, chat_msgs.data(), chat_msgs.size(), true, formatted.data(), formatted.size());
        }
        
        std::string prompt(formatted.data(), len);
        std::cout << "Debug: Formatted prompt length: " << len << " content: " << prompt.substr(0, 50) << "..." << std::endl;
        std::vector<llama_token> tokens = tokenize(prompt, true);
        std::cout << "Debug: Token count: " << tokens.size() << std::endl;

        if (tokens.empty()) {
            std::cerr << "Error: No tokens generated for prompt" << std::endl;
            return "Fehler.";
        }
        // or just use KV cache management if we wanted to be even more complex.
        // Here we just clear and re-process for absolute correctness with templates, 
        // 1B model is fast enough that this is sub-millisecond on M-series chips.
        llama_memory_t mem = llama_get_memory(ctx_);
        llama_memory_seq_rm(mem, call->seq_id, -1, -1);
        call->n_past = 0;

        llama_batch batch = llama_batch_init(tokens.size(), 0, 1);
        batch.n_tokens = tokens.size();
        for (size_t i = 0; i < tokens.size(); ++i) {
            batch.token[i] = tokens[i];
            batch.pos[i] = i;
            batch.n_seq_id[i] = 1;
            batch.seq_id[i][0] = call->seq_id;
            batch.logits[i] = (i == tokens.size() - 1);
        }

        if (llama_decode(ctx_, batch) != 0) {
            llama_batch_free(batch);
            return "Fehler.";
        }
        call->n_past = tokens.size();
        llama_batch_free(batch);

        std::string response;
        llama_token id;
        for (int i = 0; i < 64; ++i) { // Short responses limit
            id = llama_sampler_sample(sampler_, ctx_, -1);
            if (id == llama_vocab_eos(vocab_)) break;
            
            char piece[128];
            int n = llama_token_to_piece(vocab_, id, piece, sizeof(piece), 0, false);
            if (n > 0) response.append(piece, n);

            llama_batch b = llama_batch_init(1, 0, 1);
            b.n_tokens = 1;
            b.token[0] = id;
            b.pos[0] = call->n_past;
            b.n_seq_id[0] = 1;
            b.seq_id[0][0] = call->seq_id;
            b.logits[0] = true;
            
            if (llama_decode(ctx_, b) != 0) {
                llama_batch_free(b);
                break;
            }
            call->n_past++;
            llama_batch_free(b);
        }

        // Clean up response (remove potential artifacts)
        size_t start = response.find_first_not_of(" \n\r\t");
        if (start != std::string::npos) response = response.substr(start);

        call->messages.push_back({"assistant", response});
        std::cout << "🦙 [" << cid << "] DE: " << response << std::endl;
        return response;
    }

    std::vector<llama_token> tokenize(const std::string& text, bool bos) {
        std::vector<llama_token> res(text.size() + 2);
        int n = llama_tokenize(vocab_, text.c_str(), text.size(), res.data(), res.size(), bos, true);
        res.resize(n);
        return res;
    }

    void send_to_tts(int cid, const std::string& text) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(8090);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            std::string msg = std::to_string(cid) + ":" + text;
            send(sock, msg.c_str(), msg.length(), 0);
        }
        close(sock);
    }

    std::shared_ptr<LlamaCall> get_or_create_call(int cid) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        if (calls_.count(cid)) return calls_[cid];
        auto call = std::make_shared<LlamaCall>();
        call->id = cid;
        call->seq_id = next_seq_id_++;
        calls_[cid] = call;
        return call;
    }

    std::atomic<bool> running_;
    int next_seq_id_ = 0;
    struct llama_model* model_ = nullptr;
    struct llama_context* ctx_ = nullptr;
    const struct llama_vocab* vocab_ = nullptr;
    struct llama_sampler* sampler_ = nullptr;
    std::mutex llama_mutex_;
    std::mutex calls_mutex_;
    std::map<int, std::shared_ptr<LlamaCall>> calls_;
    ControlListener ctrl_listener_;
    ControlSignalSender ctrl_sender_;
};

int main(int argc, char** argv) {
    std::string model_path = "models/llama/Llama-3.2-1B-Instruct-Q8_0.gguf";
    if (argc >= 2) model_path = argv[1];
    
    try {
        LlamaService service(model_path);
        service.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
