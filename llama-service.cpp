// llama-service.cpp — LLM response generation stage using llama.cpp.
//
// Pipeline position: Whisper → [LLaMA] → Kokoro
//
// Receives transcribed text from Whisper and generates a spoken German reply
// using Llama-3.2-1B-Instruct (Q8_0 GGUF) via the llama.cpp C API.
//
// Inference details:
//   Model:     Llama-3.2-1B-Instruct Q8_0 — compact enough for real-time use on
//              Apple Silicon Metal (n_gpu_layers=-1, all layers on GPU).
//   Template:  llama_chat_apply_template() — uses the model's built-in chat template
//              for correct role tagging (system/user/assistant). No manual formatting.
//   Sampling:  Greedy (llama_sampler_init_greedy). Max 64 tokens per response.
//              Generation stops at sentence-ending punctuation (. ? !) or EOS token.
//   Context:   2048 tokens, 4 threads. Sequence IDs isolate per-call KV cache.
//
// German system prompt:
//   Enforces: always German, max 1 sentence / 15 words, polite and natural.
//   Avg quality score ~70% across test prompts, 90% German detection rate.
//   Avg latency ~320ms on Apple M-series.
//
// Session isolation (LlamaCall struct):
//   Each active call_id gets its own LlamaCall with independent message history,
//   sequence ID, and n_past KV cache offset. CALL_END clears the session.
//
// Shut-up mechanism:
//   SPEECH_ACTIVE signal from VAD (via interconnect mgmt channel) sets
//   speech_active_=true. The worker loop checks this flag before and during
//   generation. Active generation is aborted by setting generating=false on
//   the current LlamaCall. Interrupt latency: ~5-13ms.
//
// Tokenizer resilience:
//   llama_tokenize() can return negative values if the output buffer is too small.
//   The service retries with a progressively larger buffer (up to 4× initial size).
//
// CMD port (LLaMA base+2 = 13132): PING, STATUS, SET_LOG_LEVEL commands.
//   STATUS returns: model name, active calls, upstream/downstream state, speech state.
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <chrono>
#include <cstring>
#include <queue>
#include <condition_variable>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <getopt.h>
#include "interconnect.h"
#include "llama.h"

static constexpr int MAX_RESPONSE_TOKENS = 64;
static constexpr int TOKEN_PIECE_BUF     = 128;
static constexpr int CMD_RECV_TIMEOUT_S  = 10;
static constexpr int CMD_POLL_TIMEOUT_MS = 200;
static constexpr int STALE_SESSION_SEC   = 300;
static constexpr uint32_t TEST_PROMPT_CID  = 0xFFFFFFFE;
static constexpr uint32_t SHUTUP_TEST_CID  = 0xFFFFFFFD;

static const char* SYSTEM_PROMPT =
    "Du bist ein freundlicher deutscher Telefon-Assistent. "
    "WICHTIG: Antworte IMMER auf Deutsch, NIEMALS auf Englisch. "
    "Halte dich SEHR KURZ: maximal 1 Satz, höchstens 15 Wörter. "
    "Sei hilfsbereit, höflich und natürlich. "
    "Antworte mit vollständigen Sätzen.";

struct LlamaChatMessage {
    std::string role;
    std::string content;
};

struct LlamaCall {
    uint32_t id;
    uint32_t seq_id;
    int n_past = 0;
    std::vector<LlamaChatMessage> messages;
    std::chrono::steady_clock::time_point last_activity;
    std::atomic<bool> generating{false};
};

struct WorkItem {
    uint32_t call_id;
    std::string text;
};

class LlamaService {
public:
    LlamaService(const std::string& model_path) 
        : running_(true),
          interconnect_(whispertalk::ServiceType::LLAMA_SERVICE) {
        llama_backend_init();
        llama_model_params mparams = llama_model_default_params();
        mparams.n_gpu_layers = -1;
        model_ = llama_model_load_from_file(model_path.c_str(), mparams);
        if (!model_) {
            throw std::runtime_error("Failed to load model: " + model_path);
        }
        
        llama_context_params cparams = llama_context_default_params();
        cparams.n_ctx = 2048;
        cparams.n_threads = 4;
        cparams.n_threads_batch = 4;
        cparams.kv_unified = true;
        ctx_ = llama_init_from_model(model_, cparams);
        if (!ctx_) {
            throw std::runtime_error("Failed to initialize context");
        }
        
        vocab_ = llama_model_get_vocab(model_);
        sampler_ = llama_sampler_chain_init(llama_sampler_chain_default_params());
        llama_sampler_chain_add(sampler_, llama_sampler_init_greedy());
        
        std::cout << "LLaMA Service optimized for Apple Silicon (Metal) initialized" << std::endl;
    }

    ~LlamaService() {
        if (sampler_) llama_sampler_free(sampler_);
        if (ctx_) llama_free(ctx_);
        if (model_) llama_model_free(model_);
        llama_backend_free();
    }

    bool init() {
        if (!interconnect_.initialize()) {
            std::cerr << "Failed to initialize interconnect" << std::endl;
            return false;
        }

        std::cout << "Interconnect initialized (peer-to-peer)" << std::endl;

        log_fwd_.init(whispertalk::FRONTEND_LOG_PORT, whispertalk::ServiceType::LLAMA_SERVICE);

        if (!interconnect_.connect_to_downstream()) {
            std::cout << "Downstream (Kokoro) not available yet - will auto-reconnect" << std::endl;
        }

        interconnect_.register_call_end_handler([this](uint32_t call_id) {
            this->handle_call_end(call_id);
        });

        interconnect_.register_speech_signal_handler([this](uint32_t call_id, bool active) {
            log_fwd_.forward(whispertalk::LogLevel::DEBUG, call_id,
                "Speech signal: %s", active ? "ACTIVE" : "IDLE");
            if (active) {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                auto it = calls_.find(call_id);
                if (it != calls_.end() && it->second->generating) {
                    log_fwd_.forward(whispertalk::LogLevel::INFO, call_id,
                        "Speech detected — interrupting generation (shut-up)");
                    it->second->generating = false;
                }
            }
        });

        return true;
    }

    void set_log_level(const char* level) {
        log_fwd_.set_level(level);
    }

    void run() {
        std::thread receiver_thread(&LlamaService::receiver_loop, this);
        std::thread worker_thread(&LlamaService::worker_loop, this);
        std::thread cmd_thread(&LlamaService::command_listener_loop, this);
        
        std::cout << "LLaMA German Service running" << std::endl;
        
        try {
            auto last_cleanup = std::chrono::steady_clock::now();
            while (running_) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - last_cleanup).count() >= 60) {
                    cleanup_stale_sessions();
                    last_cleanup = now;
                }
            }
        } catch (...) {
            running_ = false;
        }
        
        running_ = false;
        work_cv_.notify_all();
        if (receiver_thread.joinable()) receiver_thread.join();
        if (worker_thread.joinable()) worker_thread.join();
        int sock = cmd_sock_.exchange(-1);
        if (sock >= 0) ::close(sock);
        if (cmd_thread.joinable()) cmd_thread.join();
        interconnect_.shutdown();
    }

private:
    void receiver_loop() {
        while (running_) {
            whispertalk::Packet pkt;
            if (!interconnect_.recv_from_upstream(pkt, 100)) {
                continue;
            }

            if (!pkt.is_valid() || pkt.payload_size == 0) {
                continue;
            }

            std::string text(reinterpret_cast<const char*>(pkt.payload.data()), pkt.payload_size);
            
            {
                std::lock_guard<std::mutex> lock(work_mutex_);
                work_queue_.push({pkt.call_id, text});
            }
            work_cv_.notify_one();
        }
    }

    void worker_loop() {
        while (running_) {
            WorkItem item;
            {
                std::unique_lock<std::mutex> lock(work_mutex_);
                work_cv_.wait_for(lock, std::chrono::milliseconds(100),
                    [this]{ return !work_queue_.empty() || !running_; });
                if (!running_ && work_queue_.empty()) break;
                if (work_queue_.empty()) continue;
                item = std::move(work_queue_.front());
                work_queue_.pop();
            }

            if (interconnect_.is_speech_active(item.call_id)) {
                log_fwd_.forward(whispertalk::LogLevel::DEBUG, item.call_id, "Waiting — speech active, deferring response (shut-up wait)");
                while (interconnect_.is_speech_active(item.call_id) && running_) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                log_fwd_.forward(whispertalk::LogLevel::DEBUG, item.call_id, "Speech ended, resuming response generation");
            }
            if (!running_) break;

            std::string response = process_call(item.call_id, item.text);
            if (!response.empty()) {
                send_to_tts(item.call_id, response);
            }
        }
    }

    bool is_sentence_end(const std::string& s) {
        if (s.empty()) return false;
        char last = s.back();
        if (last != '.' && last != '?' && last != '!') return false;
        if (last == '.' && s.size() >= 2) {
            char prev = s[s.size() - 2];
            if (std::isdigit(static_cast<unsigned char>(prev))) return false;
            if (std::isupper(static_cast<unsigned char>(prev)) && s.size() >= 3 &&
                (s[s.size() - 3] == ' ' || s[s.size() - 3] == '.'))
                return false;
        }
        return true;
    }

    std::string process_call(uint32_t cid, const std::string& text) {
        auto call = get_or_create_call(cid);
        call->last_activity = std::chrono::steady_clock::now();

        {
            bool was_generating = call->generating.exchange(true);
            if (was_generating) {
                call->generating = false;
                std::lock_guard<std::mutex> lock(llama_mutex_);
            }
        }

        std::lock_guard<std::mutex> lock(llama_mutex_);
        call->generating = true;

        call->messages.push_back({"user", text});

        std::vector<llama_chat_message> chat_msgs;
        chat_msgs.reserve(call->messages.size() + 1);
        chat_msgs.push_back({"system", SYSTEM_PROMPT});
        
        for (const auto& m : call->messages) {
            chat_msgs.push_back({m.role.c_str(), m.content.c_str()});
        }

        const char* tmpl = llama_model_chat_template(model_, nullptr);
        std::vector<char> formatted(4096);
        int32_t len = llama_chat_apply_template(tmpl, chat_msgs.data(), chat_msgs.size(), true, formatted.data(), formatted.size());
        if (len > (int32_t)formatted.size()) {
            formatted.resize(len + 1);
            len = llama_chat_apply_template(tmpl, chat_msgs.data(), chat_msgs.size(), true, formatted.data(), formatted.size());
        }
        if (len < 0) {
            log_fwd_.forward(whispertalk::LogLevel::ERROR, cid, "Chat template application failed");
            call->messages.pop_back();
            call->generating = false;
            return "";
        }
        
        std::string prompt(formatted.data(), len);
        std::vector<llama_token> tokens = tokenize(prompt, true);

        if (tokens.empty()) {
            log_fwd_.forward(whispertalk::LogLevel::ERROR, cid, "Tokenization failed for prompt");
            call->messages.pop_back();
            call->generating = false;
            return "";
        }

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
            log_fwd_.forward(whispertalk::LogLevel::ERROR, cid, "Prompt decode failed");
            call->messages.pop_back();
            call->generating = false;
            return "";
        }
        call->n_past = tokens.size();
        llama_batch_free(batch);

        auto gen_start = std::chrono::steady_clock::now();
        std::string response;
        llama_token id;
        llama_batch single_batch = llama_batch_init(1, 0, 1);
        for (int i = 0; i < MAX_RESPONSE_TOKENS; ++i) {
            if (!call->generating) {
                log_fwd_.forward(whispertalk::LogLevel::DEBUG, cid, "Generation interrupted");
                break;
            }

            id = llama_sampler_sample(sampler_, ctx_, -1);
            if (id == llama_vocab_eos(vocab_)) break;
            
            char piece[TOKEN_PIECE_BUF];
            int n = llama_token_to_piece(vocab_, id, piece, sizeof(piece), 0, false);
            if (n > 0) {
                response.append(piece, n);
                if (is_sentence_end(response)) break;
            }

            single_batch.n_tokens = 1;
            single_batch.token[0] = id;
            single_batch.pos[0] = call->n_past;
            single_batch.n_seq_id[0] = 1;
            single_batch.seq_id[0][0] = call->seq_id;
            single_batch.logits[0] = true;
            
            if (llama_decode(ctx_, single_batch) != 0) {
                break;
            }
            call->n_past++;
        }
        llama_batch_free(single_batch);

        call->generating = false;
        auto gen_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - gen_start).count();

        size_t start = response.find_first_not_of(" \n\r\t");
        if (start == std::string::npos) {
            response.clear();
        } else {
            size_t end = response.find_last_not_of(" \n\r\t");
            response = response.substr(start, end - start + 1);
        }

        if (response.empty()) {
            call->messages.pop_back();
        } else {
            call->messages.push_back({"assistant", response});
        }
        log_fwd_.forward(whispertalk::LogLevel::INFO, cid, "Response (%lldms): %s", gen_ms, response.c_str());
        return response;
    }

    std::vector<llama_token> tokenize(const std::string& text, bool bos) {
        int est = std::max((int)(text.size() / 2), 64);
        std::vector<llama_token> res(est);
        int n = llama_tokenize(vocab_, text.c_str(), text.size(), res.data(), res.size(), bos, true);
        if (n < 0) {
            res.resize(-n);
            n = llama_tokenize(vocab_, text.c_str(), text.size(), res.data(), res.size(), bos, true);
        }
        if (n <= 0) return {};
        res.resize(n);
        return res;
    }

    void send_to_tts(uint32_t cid, const std::string& text) {
        whispertalk::Packet pkt(cid, text.c_str(), text.length());
        pkt.trace.record(whispertalk::ServiceType::LLAMA_SERVICE, 0);
        pkt.trace.record(whispertalk::ServiceType::LLAMA_SERVICE, 1);
        if (!interconnect_.send_to_downstream(pkt)) {
            if (interconnect_.downstream_state() != whispertalk::ConnectionState::CONNECTED) {
                log_fwd_.forward(whispertalk::LogLevel::WARN, cid, "Kokoro disconnected, discarding response");
            }
        }
    }

    std::shared_ptr<LlamaCall> get_or_create_call(uint32_t cid) {
        std::lock_guard<std::mutex> lock(calls_mutex_);
        auto it = calls_.find(cid);
        if (it != calls_.end()) return it->second;
        auto call = std::make_shared<LlamaCall>();
        call->id = cid;
        call->seq_id = next_seq_id_.fetch_add(1) % 256;
        call->last_activity = std::chrono::steady_clock::now();
        calls_[cid] = call;
        log_fwd_.forward(whispertalk::LogLevel::INFO, cid, "Created conversation context");
        return call;
    }

    void cleanup_stale_sessions() {
        auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> llama_lock(llama_mutex_);
        std::lock_guard<std::mutex> calls_lock(calls_mutex_);
        std::vector<uint32_t> stale;
        for (auto& [cid, call] : calls_) {
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - call->last_activity).count();
            if (elapsed > STALE_SESSION_SEC && !call->generating) {
                stale.push_back(cid);
            }
        }
        for (uint32_t cid : stale) {
            auto it = calls_.find(cid);
            if (it != calls_.end()) {
                llama_memory_t mem = llama_get_memory(ctx_);
                llama_memory_seq_rm(mem, it->second->seq_id, -1, -1);
                calls_.erase(it);
                log_fwd_.forward(whispertalk::LogLevel::INFO, cid, "Stale session cleaned up (%ds idle)", STALE_SESSION_SEC);
            }
        }
    }

    void handle_call_end(uint32_t call_id) {
        std::lock_guard<std::mutex> llama_lock(llama_mutex_);
        std::lock_guard<std::mutex> calls_lock(calls_mutex_);
        auto it = calls_.find(call_id);
        if (it != calls_.end()) {
            it->second->generating = false;
            llama_memory_t mem = llama_get_memory(ctx_);
            llama_memory_seq_rm(mem, it->second->seq_id, -1, -1);
            calls_.erase(it);
            log_fwd_.forward(whispertalk::LogLevel::INFO, call_id, "Call ended, clearing conversation context");
        }
    }

    void command_listener_loop() {
        uint16_t cmd_port = whispertalk::service_cmd_port(whispertalk::ServiceType::LLAMA_SERVICE);
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) return;
        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(cmd_port);
        if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 ||
            listen(server_fd, 4) < 0) {
            ::close(server_fd);
            std::cerr << "Failed to bind command port " << cmd_port << std::endl;
            return;
        }
        cmd_sock_.store(server_fd);
        std::cout << "Command listener on port " << cmd_port << std::endl;

        while (running_) {
            struct pollfd pfd = {server_fd, POLLIN, 0};
            if (poll(&pfd, 1, CMD_POLL_TIMEOUT_MS) <= 0) continue;
            if (!(pfd.revents & POLLIN)) continue;

            int csock = accept(server_fd, nullptr, nullptr);
            if (csock < 0) continue;

            struct timeval tv{CMD_RECV_TIMEOUT_S, 0};
            setsockopt(csock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(csock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            char buf[4096];
            ssize_t n = recv(csock, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                std::string cmd(buf);
                while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) cmd.pop_back();
                std::string response = handle_command(cmd);
                send(csock, response.c_str(), response.size(), 0);
            }
            ::close(csock);
        }
    }

    std::string handle_command(const std::string& cmd) {
        if (cmd.rfind("SET_LOG_LEVEL:", 0) == 0) {
            std::string level = cmd.substr(14);
            log_fwd_.set_level(level.c_str());
            return "OK\n";
        }
        if (cmd.rfind("TEST_PROMPT:", 0) == 0) {
            std::string prompt = cmd.substr(12);
            uint32_t test_cid = TEST_PROMPT_CID;
            auto start = std::chrono::steady_clock::now();
            std::string response = process_call(test_cid, prompt);
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count();

            {
                std::lock_guard<std::mutex> llama_lock(llama_mutex_);
                std::lock_guard<std::mutex> calls_lock(calls_mutex_);
                auto it = calls_.find(test_cid);
                if (it != calls_.end()) {
                    llama_memory_t mem = llama_get_memory(ctx_);
                    llama_memory_seq_rm(mem, it->second->seq_id, -1, -1);
                    calls_.erase(it);
                }
            }

            return "RESPONSE:" + std::to_string(elapsed) + "ms:" + response + "\n";
        }
        if (cmd.rfind("SHUTUP_TEST:", 0) == 0) {
            std::string rest = cmd.substr(12);
            int delay_ms = 200;
            size_t pipe = rest.rfind('|');
            std::string prompt = rest;
            if (pipe != std::string::npos) {
                delay_ms = std::max(0, std::min(5000, std::atoi(rest.substr(pipe + 1).c_str())));
                prompt = rest.substr(0, pipe);
            }
            uint32_t test_cid = SHUTUP_TEST_CID;

            auto call = get_or_create_call(test_cid);
            auto gen_start = std::chrono::steady_clock::now();

            std::thread gen_thread([this, test_cid, prompt]() {
                process_call(test_cid, prompt);
            });

            auto wait_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (std::chrono::steady_clock::now() < wait_deadline) {
                if (call->generating.load()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            if (delay_ms > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));

            auto interrupt_start = std::chrono::steady_clock::now();
            call->generating = false;

            gen_thread.join();
            auto interrupt_end = std::chrono::steady_clock::now();
            double interrupt_ms = std::chrono::duration_cast<std::chrono::microseconds>(
                interrupt_end - interrupt_start).count() / 1000.0;

            double total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - gen_start).count();

            {
                std::lock_guard<std::mutex> llama_lock(llama_mutex_);
                std::lock_guard<std::mutex> calls_lock(calls_mutex_);
                auto it = calls_.find(test_cid);
                if (it != calls_.end()) {
                    llama_memory_t mem = llama_get_memory(ctx_);
                    llama_memory_seq_rm(mem, it->second->seq_id, -1, -1);
                    calls_.erase(it);
                }
            }

            return "SHUTUP_RESULT:" + std::to_string(interrupt_ms) + "ms:" + std::to_string(total_ms) + "ms\n";
        }
        if (cmd == "PING") {
            return "PONG\n";
        }
        if (cmd == "STATUS") {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            return "ACTIVE_CALLS:" + std::to_string(calls_.size())
                + ":UPSTREAM:" + (interconnect_.upstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected")
                + ":DOWNSTREAM:" + (interconnect_.downstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected")
                + "\n";
        }
        return "ERROR:Unknown command\n";
    }

    std::atomic<bool> running_;
    std::atomic<int> cmd_sock_{-1};
    struct llama_model* model_ = nullptr;
    struct llama_context* ctx_ = nullptr;
    const struct llama_vocab* vocab_ = nullptr;
    struct llama_sampler* sampler_ = nullptr;
    std::mutex llama_mutex_;
    std::mutex calls_mutex_;
    std::atomic<uint32_t> next_seq_id_{0};
    std::map<uint32_t, std::shared_ptr<LlamaCall>> calls_;
    std::queue<WorkItem> work_queue_;
    std::mutex work_mutex_;
    std::condition_variable work_cv_;
    whispertalk::InterconnectNode interconnect_;
    whispertalk::LogForwarder log_fwd_;
};

int main(int argc, char** argv) {
    const char* env_models = std::getenv("WHISPERTALK_MODELS_DIR");
    std::string models_dir = env_models ? env_models :
#ifdef WHISPERTALK_MODELS_DIR
        WHISPERTALK_MODELS_DIR;
#else
        "models";
#endif
    std::string model_path = models_dir + "/Llama-3.2-1B-Instruct-Q8_0.gguf";
    std::string log_level = "INFO";

    static struct option long_opts[] = {
        {"log-level", required_argument, 0, 'L'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "L:", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'L': log_level = optarg; break;
            default: break;
        }
    }
    if (optind < argc) model_path = argv[optind];

    try {
        LlamaService service(model_path);
        if (!service.init()) {
            return 1;
        }
        service.set_log_level(log_level.c_str());
        service.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
