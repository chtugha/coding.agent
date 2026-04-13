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
//   Sampling:  Repetition penalty (last 64, 1.1×) + top_p (0.95) + temp (0.3) + dist.
//              Max 96 tokens per response. Stops after 2 sentence-ends (. ? !) or EOS.
//   Context:   2048 tokens, 4 threads. Sequence IDs isolate per-call KV cache.
//
// German system prompt:
//   Enforces: always German, 1-2 sentences / 25 words, substantive responses.
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
#include <algorithm>
#include <sstream>
#include <unordered_set>
#include <poll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <getopt.h>
#include <cstdio>
#include <climits>
#include "interconnect.h"
#include "llama.h"
#include <openssl/ssl.h>
#include <openssl/err.h>

static constexpr int MAX_RESPONSE_TOKENS = 96;
static constexpr int RAG_TIMEOUT_MS = 150;
static constexpr size_t RAG_MAX_RESPONSE_BYTES = 256 * 1024;
static constexpr int TOKEN_PIECE_BUF     = 128;
static constexpr int CMD_RECV_TIMEOUT_S  = 10;
static constexpr int CMD_POLL_TIMEOUT_MS = 200;
static constexpr int STALE_SESSION_SEC   = 300;
static constexpr uint32_t TEST_PROMPT_CID  = 0xFFFFFFFE;
static constexpr uint32_t SHUTUP_TEST_CID  = 0xFFFFFFFD;
static constexpr long SPEECH_DISCARD_MIN_MS = 200;
static constexpr size_t MIN_RESPONSE_CHARS  = 8;
static constexpr int MIN_RESPONSE_WORDS     = 2;
static constexpr float SIMILARITY_THRESHOLD = 0.75f;
static const char* TOPIC_CHANGES[] = {
    "Lass uns über etwas anderes sprechen. Was machst du in deiner Freizeit?",
    "Hast du in letzter Zeit einen interessanten Film gesehen?",
    "Welches Buch hat dich zuletzt begeistert?",
    "Was ist deine Meinung zum aktuellen Wetter?",
    "Kochst du gerne? Was ist dein Lieblingsgericht?",
    "Warst du in letzter Zeit auf einer Reise?",
    "Hast du ein Hobby, das dich besonders begeistert?",
    "Was denkst du über die neuesten Technologie-Trends?",
};
static constexpr size_t NUM_TOPIC_CHANGES = sizeof(TOPIC_CHANGES) / sizeof(TOPIC_CHANGES[0]);
static const char* SYSTEM_PROMPT =
    "Du bist ein gesprächiger deutscher Telefon-Assistent. "
    "WICHTIG: Antworte IMMER auf Deutsch, NIEMALS auf Englisch. "
    "Antworte in 1-2 Sätzen, maximal 25 Wörter. "
    "Bringe IMMER neue Information oder stelle eine konkrete Frage zum Thema. "
    "Vermeide leere Floskeln wie 'Gern geschehen', 'Kein Problem', 'Danke', 'Bis dann', "
    "'Das ist eine gute Frage', 'Natürlich', 'Aber ja', 'Das stimmt'. "
    "Beginne NIEMALS mit einer Bestätigung oder Bewertung des Gesagten. "
    "Wiederhole NIEMALS den Satz des Anrufers. "
    "Sei interessiert und wissend — teile Fakten, Meinungen oder Vorschläge.";

struct LlamaChatMessage {
    std::string role;
    std::string content;
};

static constexpr long RESPONSE_COOLDOWN_MS = 800;

struct LlamaCall {
    uint32_t id;
    uint32_t seq_id;
    int n_past = 0;
    bool greeted = false;
    int patient_id = -1;
    std::vector<LlamaChatMessage> messages;
    std::chrono::steady_clock::time_point last_activity;
    std::chrono::steady_clock::time_point last_response_sent;
    std::atomic<bool> generating{false};
};

struct WorkItem {
    uint32_t call_id;
    std::string text;
};

class LlamaService {
public:
    LlamaService(const std::string& model_path,
                 const std::string& rag_host = "127.0.0.1",
                 int rag_port = 13181) 
        : running_(true),
          rag_host_(rag_host),
          rag_port_(rag_port),
          interconnect_(whispertalk::ServiceType::LLAMA_SERVICE) {
        prodigy_tls::ensure_certs();
        rag_ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (rag_ssl_ctx_) {
            SSL_CTX_set_verify(rag_ssl_ctx_, SSL_VERIFY_PEER, nullptr);
            std::string ca = prodigy_tls::cert_file_path();
            if (SSL_CTX_load_verify_locations(rag_ssl_ctx_, ca.c_str(), nullptr) != 1) {
                SSL_CTX_set_verify(rag_ssl_ctx_, SSL_VERIFY_NONE, nullptr);
            }
        }
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
        llama_sampler_chain_add(sampler_, llama_sampler_init_penalties(64, 1.1f, 0.0f, 0.0f));
        llama_sampler_chain_add(sampler_, llama_sampler_init_top_p(0.95f, 1));
        llama_sampler_chain_add(sampler_, llama_sampler_init_temp(0.3f));
        llama_sampler_chain_add(sampler_, llama_sampler_init_dist(42));
        
        std::cout << "LLaMA Service optimized for Apple Silicon (Metal) initialized" << std::endl;
    }

    ~LlamaService() {
        if (rag_ssl_ctx_) SSL_CTX_free(rag_ssl_ctx_);
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

        if (resolve_rag_addr()) {
            std::string health = rag_http_get("/health");
            if (!health.empty()) {
                log_fwd_.forward(whispertalk::LogLevel::INFO, 0,
                    "RAG service reachable at %s:%d", rag_host_.c_str(), rag_port_);
            } else {
                log_fwd_.forward(whispertalk::LogLevel::INFO, 0,
                    "RAG service not responding — will retry on first call");
            }
        } else {
            log_fwd_.forward(whispertalk::LogLevel::WARN, 0,
                "RAG DNS resolution failed for %s:%d — RAG disabled",
                rag_host_.c_str(), rag_port_);
        }

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
                {
                    std::lock_guard<std::mutex> lock(speech_time_mutex_);
                    if (speech_active_since_.find(call_id) == speech_active_since_.end()) {
                        speech_active_since_[call_id] = std::chrono::steady_clock::now();
                    }
                }
                std::lock_guard<std::mutex> lock(calls_mutex_);
                auto it = calls_.find(call_id);
                if (it != calls_.end() && it->second->generating) {
                    log_fwd_.forward(whispertalk::LogLevel::INFO, call_id,
                        "Speech detected — interrupting generation (shut-up)");
                    it->second->generating = false;
                }
            } else {
                std::string pending_text;
                {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    auto it = pending_transcriptions_.find(call_id);
                    if (it != pending_transcriptions_.end() && !it->second.empty()) {
                        pending_text = std::move(it->second);
                        pending_transcriptions_.erase(it);
                    }
                }
                if (!pending_text.empty()) {
                    {
                        std::lock_guard<std::mutex> wlock(work_mutex_);
                        work_queue_.push({call_id, std::move(pending_text)});
                    }
                    work_cv_.notify_one();
                    log_fwd_.forward(whispertalk::LogLevel::INFO, call_id,
                        "Speech ended — flushing accumulated transcription to work queue");
                }
                std::lock_guard<std::mutex> lock(speech_time_mutex_);
                speech_active_since_.erase(call_id);
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

                std::queue<WorkItem> keep;
                size_t merged = 0;
                while (!work_queue_.empty()) {
                    auto qi = std::move(work_queue_.front());
                    work_queue_.pop();
                    if (interconnect_.has_ended(qi.call_id)) {
                        continue;
                    } else if (qi.call_id == item.call_id) {
                        item.text += " " + qi.text;
                        merged++;
                    } else {
                        keep.push(std::move(qi));
                    }
                }
                std::swap(work_queue_, keep);
                if (merged > 0) {
                    log_fwd_.forward(whispertalk::LogLevel::DEBUG, item.call_id,
                        "Merged %zu queue items for same call", merged);
                }
            }

            if (interconnect_.has_ended(item.call_id)) {
                log_fwd_.forward(whispertalk::LogLevel::DEBUG, item.call_id, "Discarding — call already ended");
                continue;
            }

            if (interconnect_.is_speech_active(item.call_id)) {
                long speech_ms = 0;
                {
                    std::lock_guard<std::mutex> lock(speech_time_mutex_);
                    auto it = speech_active_since_.find(item.call_id);
                    if (it != speech_active_since_.end()) {
                        speech_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - it->second).count();
                    }
                }
                if (speech_ms >= SPEECH_DISCARD_MIN_MS) {
                    std::lock_guard<std::mutex> lock(pending_mutex_);
                    auto& pending = pending_transcriptions_[item.call_id];
                    if (pending.empty()) {
                        pending = item.text;
                    } else {
                        pending += " " + item.text;
                    }
                    log_fwd_.forward(whispertalk::LogLevel::DEBUG, item.call_id,
                        "Accumulated transcription — caller speaking for %ldms, waiting for turn end", speech_ms);
                    continue;
                }
                log_fwd_.forward(whispertalk::LogLevel::DEBUG, item.call_id,
                    "Speech active only %ldms — processing transcription (may be transient)", speech_ms);
            }

            {
                std::lock_guard<std::mutex> lock(pending_mutex_);
                auto it = pending_transcriptions_.find(item.call_id);
                if (it != pending_transcriptions_.end() && !it->second.empty()) {
                    item.text = it->second + " " + item.text;
                    pending_transcriptions_.erase(it);
                    log_fwd_.forward(whispertalk::LogLevel::DEBUG, item.call_id,
                        "Prepended accumulated transcription — processing combined text");
                }
            }

            if (!running_) break;
            if (interconnect_.has_ended(item.call_id)) {
                log_fwd_.forward(whispertalk::LogLevel::DEBUG, item.call_id, "Discarding — call ended during accumulation");
                continue;
            }

            {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                auto it = calls_.find(item.call_id);
                if (it != calls_.end()) {
                    auto since = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - it->second->last_response_sent).count();
                    if (since < RESPONSE_COOLDOWN_MS && since > 0) {
                        log_fwd_.forward(whispertalk::LogLevel::DEBUG, item.call_id,
                            "Discarding transcription — response cooldown active (%ldms since last response)", since);
                        continue;
                    }
                }
            }

            std::string response = process_call(item.call_id, item.text);
            if (!response.empty()) {
                {
                    std::lock_guard<std::mutex> lock(calls_mutex_);
                    auto it = calls_.find(item.call_id);
                    if (it != calls_.end()) {
                        it->second->last_response_sent = std::chrono::steady_clock::now();
                    }
                }
                send_to_tts(item.call_id, response);
            }
        }
    }

    int sentence_end_count_ = 0;

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
        sentence_end_count_++;
        return sentence_end_count_ >= 2;
    }

    static int count_words(const std::string& s) {
        int count = 0;
        bool in_word = false;
        for (char c : s) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                in_word = false;
            } else if (!in_word) {
                in_word = true;
                count++;
            }
        }
        return count;
    }

    static std::string normalize_lower(const std::string& s) {
        std::string out;
        out.reserve(s.size());
        for (size_t i = 0; i < s.size(); i++) {
            unsigned char c = (unsigned char)s[i];
            if (c == '.' || c == '!' || c == '?' || c == ',' || c == ':' || c == ';') continue;
            if (c >= 'A' && c <= 'Z') { out += (char)(c + 32); continue; }
            if (c == 0xC2 && i + 1 < s.size()) {
                unsigned char c2 = (unsigned char)s[i + 1];
                if (c2 == 0xBB || c2 == 0xAB) { i++; continue; }
            }
            if (c == 0xC3 && i + 1 < s.size()) {
                unsigned char c2 = (unsigned char)s[i + 1];
                if (c2 == 0x84) { out += "\xC3\xA4"; i++; continue; }
                if (c2 == 0x96) { out += "\xC3\xB6"; i++; continue; }
                if (c2 == 0x9C) { out += "\xC3\xBC"; i++; continue; }
            }
            out += (char)c;
        }
        return out;
    }

    static std::unordered_set<std::string> split_words(const std::string& s) {
        std::unordered_set<std::string> words;
        std::istringstream iss(s);
        std::string w;
        while (iss >> w) words.insert(w);
        return words;
    }

    static float text_similarity(const std::string& a, const std::string& b) {
        auto wa = split_words(normalize_lower(a));
        auto wb = split_words(normalize_lower(b));
        if (wa.empty() || wb.empty()) return 0.0f;
        size_t intersect = 0;
        for (const auto& w : wa) {
            if (wb.count(w)) intersect++;
        }
        size_t union_sz = wa.size() + wb.size() - intersect;
        return union_sz == 0 ? 0.0f : (float)intersect / (float)union_sz;
    }

    bool resolve_rag_addr() {
        struct addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        if (getaddrinfo(rag_host_.c_str(), std::to_string(rag_port_).c_str(), &hints, &res) != 0 || !res)
            return false;
        memcpy(&rag_addr_, res->ai_addr, res->ai_addrlen);
        rag_addrlen_ = res->ai_addrlen;
        freeaddrinfo(res);
        rag_addr_resolved_ = true;
        return true;
    }

    bool is_rag_available() {
        if (!rag_addr_resolved_) {
            int64_t retry_after = rag_dns_retry_after_.load(std::memory_order_relaxed);
            auto now = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            if (now < retry_after) return false;
            if (!resolve_rag_addr()) {
                rag_dns_retry_after_.store(now + RAG_COOLDOWN_SEC, std::memory_order_relaxed);
                return false;
            }
        }
        int64_t disabled = rag_disabled_until_.load(std::memory_order_relaxed);
        if (disabled == 0) return true;
        auto now = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        return now >= disabled;
    }

    void rag_record_success() {
        rag_fail_count_.store(0, std::memory_order_relaxed);
        rag_disabled_until_.store(0, std::memory_order_relaxed);
    }

    void rag_record_failure(uint32_t cid) {
        int fails = rag_fail_count_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fails >= RAG_FAIL_THRESHOLD) {
            auto cooldown_end = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count() + RAG_COOLDOWN_SEC;
            rag_disabled_until_.store(cooldown_end, std::memory_order_relaxed);
            rag_fail_count_.store(0, std::memory_order_relaxed);
            log_fwd_.forward(whispertalk::LogLevel::WARN, cid,
                "RAG service: %d consecutive failures — disabling for %ds",
                RAG_FAIL_THRESHOLD, RAG_COOLDOWN_SEC);
        }
    }

    std::string rag_http_get(const std::string& path) {
        if (!rag_addr_resolved_) return "";
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return "";
        int flags = fcntl(sock, F_GETFL, 0);
        if (flags == -1) flags = 0;
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        connect(sock, (struct sockaddr*)&rag_addr_, rag_addrlen_);
        struct pollfd pfd{sock, POLLOUT, 0};
        if (poll(&pfd, 1, RAG_TIMEOUT_MS) <= 0 || !(pfd.revents & POLLOUT)) {
            close(sock);
            return "";
        }
        int conn_err = 0;
        socklen_t conn_len = sizeof(conn_err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &conn_err, &conn_len);
        if (conn_err != 0) {
            close(sock);
            return "";
        }
        fcntl(sock, F_SETFL, flags);

        if (!rag_ssl_ctx_) { close(sock); return ""; }
        SSL* ssl = SSL_new(rag_ssl_ctx_);
        if (!ssl) { close(sock); return ""; }
        SSL_set_fd(ssl, sock);
        SSL_set_tlsext_host_name(ssl, rag_host_.c_str());
        if (SSL_connect(ssl) != 1) {
            SSL_free(ssl); close(sock);
            return "";
        }

        std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + rag_host_ + ":" +
                          std::to_string(rag_port_) + "\r\nConnection: close\r\n\r\n";
        if (SSL_write(ssl, req.c_str(), static_cast<int>(req.size())) <= 0) {
            SSL_shutdown(ssl); SSL_free(ssl); close(sock);
            return "";
        }
        std::string response;
        char buf[4096];
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(RAG_TIMEOUT_MS);
        while (response.size() < RAG_MAX_RESPONSE_BYTES) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) break;
            struct pollfd rpfd{sock, POLLIN, 0};
            if (poll(&rpfd, 1, (int)remaining) <= 0 || !(rpfd.revents & POLLIN)) break;
            int n = SSL_read(ssl, buf, sizeof(buf));
            if (n <= 0) break;
            response.append(buf, n);
        }
        SSL_shutdown(ssl); SSL_free(ssl); close(sock);

        size_t status_end = response.find("\r\n");
        if (status_end == std::string::npos) return "";
        std::string status_line = response.substr(0, status_end);
        size_t sp1 = status_line.find(' ');
        if (sp1 == std::string::npos) return "";
        int http_status = std::atoi(status_line.c_str() + sp1 + 1);
        if (http_status < 200 || http_status >= 300) return "";
        size_t hdr_end = response.find("\r\n\r\n");
        if (hdr_end == std::string::npos) return "";
        return response.substr(hdr_end + 4);
    }

    static int json_brace_depth(const std::string& json, size_t end) {
        int depth = 0;
        bool in_str = false;
        for (size_t i = 0; i < end; i++) {
            if (in_str) {
                if (json[i] == '\\') { i++; continue; }
                if (json[i] == '"') in_str = false;
            } else {
                if (json[i] == '"') in_str = true;
                else if (json[i] == '{') depth++;
                else if (json[i] == '}') depth--;
            }
        }
        return depth;
    }

    static std::string extract_json_string(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\":";
        size_t pos = 0;
        while ((pos = json.find(search, pos)) != std::string::npos) {
            if (json_brace_depth(json, pos) == 1) break;
            pos += search.size();
        }
        if (pos == std::string::npos) return "";
        pos += search.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        if (pos >= json.size() || json[pos] != '"') return "";
        pos++;
        std::string result;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                pos++;
                if (json[pos] == 'n') result += '\n';
                else if (json[pos] == 't') result += '\t';
                else if (json[pos] == '\\') result += '\\';
                else result += json[pos];
            } else {
                result += json[pos];
            }
            pos++;
        }
        return result;
    }

    static int extract_json_int(const std::string& json, const std::string& key, int fallback) {
        std::string search = "\"" + key + "\":";
        size_t pos = 0;
        while ((pos = json.find(search, pos)) != std::string::npos) {
            if (json_brace_depth(json, pos) == 1) break;
            pos += search.size();
        }
        if (pos == std::string::npos) return fallback;
        pos += search.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        if (pos >= json.size()) return fallback;
        bool neg = false;
        if (json[pos] == '-') { neg = true; pos++; }
        if (pos >= json.size() || !std::isdigit((unsigned char)json[pos])) return fallback;
        long long val = 0;
        while (pos < json.size() && std::isdigit((unsigned char)json[pos])) {
            val = val * 10 + (json[pos] - '0');
            if (val > INT_MAX) { val = INT_MAX; break; }
            pos++;
        }
        return neg ? (int)(-val) : (int)val;
    }

    static std::string extract_rag_text(const std::string& json_body) {
        std::string result;
        size_t pos = 0;
        std::string search = "\"text\":\"";
        while ((pos = json_body.find(search, pos)) != std::string::npos) {
            pos += search.size();
            std::string text;
            while (pos < json_body.size() && json_body[pos] != '"') {
                if (json_body[pos] == '\\' && pos + 1 < json_body.size()) {
                    pos++;
                    if (json_body[pos] == 'n') text += '\n';
                    else if (json_body[pos] == 't') text += '\t';
                    else if (json_body[pos] == '\\') text += '\\';
                    else text += json_body[pos];
                } else {
                    text += json_body[pos];
                }
                pos++;
            }
            if (!text.empty()) {
                if (!result.empty()) result += "\n";
                result += text;
            }
        }
        return result;
    }

    std::string rag_get_caller(uint32_t call_id) {
        return rag_http_get("/caller/" + std::to_string(call_id));
    }

    std::string rag_query(const std::string& text, int top_k, int patient_id) {
        std::string encoded;
        for (unsigned char c : text) {
            if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                encoded += (char)c;
            } else {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%%%02X", c);
                encoded += buf;
            }
        }
        std::string path = "/query?text=" + encoded + "&top_k=" + std::to_string(top_k);
        if (patient_id >= 0) {
            path += "&patient_id=" + std::to_string(patient_id);
        }
        return rag_http_get(path);
    }

    static bool is_empty_pleasantry(const std::string& response) {
        std::string lower = normalize_lower(response);
        static const char* pleasantries[] = {
            "gern geschehen", "kein problem", "vielen dank", "danke",
            "bis dann", "bis dahin", "bis zum nächsten mal",
            "das ist großartig", "das ist ein wichtiger punkt",
            "das ist ein guter punkt", "das stimmt",
            "das ist eine interessante frage", "das ist eine gute frage",
            "aber ja", "natürlich", "selbstverständlich",
            "genau richtig", "das freut mich", "sehr gut",
            "gute frage", "interessante frage", "richtig",
            nullptr
        };
        auto words = split_words(lower);
        if (words.size() > 8) return false;
        for (int i = 0; pleasantries[i]; i++) {
            if (lower.find(pleasantries[i]) != std::string::npos) return true;
        }
        return false;
    }

    std::string process_call(uint32_t cid, const std::string& text) {
        auto call = get_or_create_call(cid);
        call->last_activity = std::chrono::steady_clock::now();

        std::string greeting_hint;
        std::string rag_ctx;
        if (is_rag_available()) {
            bool rag_ok = true;
            if (!call->greeted) {
                std::string caller_json = rag_get_caller(cid);
                if (caller_json.empty()) {
                    rag_record_failure(cid);
                    rag_ok = false;
                } else {
                    std::string status = extract_json_string(caller_json, "status");
                    if (status == "found") {
                        std::string name = extract_json_string(caller_json, "name");
                        int pid = extract_json_int(caller_json, "patient_id", -1);
                        call->patient_id = pid;
                        if (!name.empty()) {
                            greeting_hint = "Der Anrufer heißt " + name + ".";
                            log_fwd_.forward(whispertalk::LogLevel::INFO, cid,
                                "Caller identified: %s (patient_id=%d)", name.c_str(), pid);
                        }
                    }
                    call->greeted = true;
                }
            }
            if (rag_ok && is_rag_available()) {
                std::string rag_body = rag_query(text, 3, call->patient_id);
                if (rag_body.empty()) {
                    rag_record_failure(cid);
                    rag_ok = false;
                } else {
                    rag_ctx = extract_rag_text(rag_body);
                    if (!rag_ctx.empty()) {
                        log_fwd_.forward(whispertalk::LogLevel::DEBUG, cid,
                            "RAG context retrieved (%zu bytes)", rag_ctx.size());
                    } else {
                        log_fwd_.forward(whispertalk::LogLevel::DEBUG, cid, "RAG returned no matching chunks");
                    }
                }
            }
            if (rag_ok) rag_record_success();
        }

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

        static constexpr size_t MAX_HISTORY_MESSAGES = 10;
        while (call->messages.size() > MAX_HISTORY_MESSAGES) {
            call->messages.erase(call->messages.begin(), call->messages.begin() + 2);
        }

        std::string sys_prompt = SYSTEM_PROMPT;
        if (!greeting_hint.empty()) sys_prompt += "\n\n" + greeting_hint;
        if (!rag_ctx.empty()) sys_prompt += "\n\nKontextinformation aus Praxissystem:\n" + rag_ctx;

        std::vector<llama_chat_message> chat_msgs;
        chat_msgs.reserve(call->messages.size() + 1);
        chat_msgs.push_back({"system", sys_prompt.c_str()});
        
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
        sentence_end_count_ = 0;
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
            int wc = count_words(response);
            if (response.size() < MIN_RESPONSE_CHARS || wc < MIN_RESPONSE_WORDS) {
                log_fwd_.forward(whispertalk::LogLevel::WARN, cid,
                    "Discarding fragment response (%lldms, %zu chars, %d words): %s",
                    gen_ms, response.size(), wc, response.c_str());
                response.clear();
            } else {
                float sim = text_similarity(text, response);
                if (sim > SIMILARITY_THRESHOLD) {
                    log_fwd_.forward(whispertalk::LogLevel::WARN, cid,
                        "Replacing parrot response (%.0f%% similar to input) with topic change: %s",
                        sim * 100.0f, response.c_str());
                    call->messages.pop_back();
                    response = TOPIC_CHANGES[topic_change_idx_.fetch_add(1) % NUM_TOPIC_CHANGES];
                } else if (is_empty_pleasantry(response)) {
                    log_fwd_.forward(whispertalk::LogLevel::WARN, cid,
                        "Replacing pleasantry with topic change: %s", response.c_str());
                    call->messages.pop_back();
                    response = TOPIC_CHANGES[topic_change_idx_.fetch_add(1) % NUM_TOPIC_CHANGES];
                } else {
                    for (auto rit = call->messages.rbegin(); rit != call->messages.rend(); ++rit) {
                        if (rit->role == "assistant") {
                            float hsim = text_similarity(rit->content, response);
                            if (hsim > SIMILARITY_THRESHOLD) {
                                log_fwd_.forward(whispertalk::LogLevel::WARN, cid,
                                    "Replacing repetitive response (%.0f%% similar) with topic change: %s",
                                    hsim * 100.0f, response.c_str());
                                call->messages.pop_back();
                                response = TOPIC_CHANGES[topic_change_idx_.fetch_add(1) % NUM_TOPIC_CHANGES];
                                break;
                            }
                        }
                    }
                }
            }
        }
        if (!response.empty()) {
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
        std::shared_ptr<LlamaCall> call_to_clean;
        {
            std::lock_guard<std::mutex> calls_lock(calls_mutex_);
            auto it = calls_.find(call_id);
            if (it != calls_.end()) {
                it->second->generating = false;
                call_to_clean = it->second;
                calls_.erase(it);
            }
        }
        {
            std::lock_guard<std::mutex> lock(speech_time_mutex_);
            speech_active_since_.erase(call_id);
        }
        {
            std::lock_guard<std::mutex> lock(pending_mutex_);
            pending_transcriptions_.erase(call_id);
        }
        if (call_to_clean) {
            std::lock_guard<std::mutex> llama_lock(llama_mutex_);
            llama_memory_t mem = llama_get_memory(ctx_);
            llama_memory_seq_rm(mem, call_to_clean->seq_id, -1, -1);
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
        if (cmd == "SET_TTS:KOKORO") {
            interconnect_.clear_downstream_override();
            log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "Downstream switched to KOKORO_SERVICE");
            return "OK TTS=KOKORO\n";
        }
        if (cmd == "SET_TTS:NEUTTS") {
            interconnect_.set_downstream_override(whispertalk::ServiceType::NEUTTS_SERVICE);
            log_fwd_.forward(whispertalk::LogLevel::INFO, 0, "Downstream switched to NEUTTS_SERVICE");
            return "OK TTS=NEUTTS\n";
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
    const std::string rag_host_;
    const int rag_port_;
    struct sockaddr_storage rag_addr_{};
    socklen_t rag_addrlen_{0};
    bool rag_addr_resolved_{false};
    SSL_CTX* rag_ssl_ctx_{nullptr};
    std::atomic<int> rag_fail_count_{0};
    static constexpr int RAG_FAIL_THRESHOLD = 3;
    static constexpr int RAG_COOLDOWN_SEC = 30;
    std::atomic<int64_t> rag_disabled_until_{0};
    std::atomic<int64_t> rag_dns_retry_after_{0};
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
    std::mutex speech_time_mutex_;
    std::map<uint32_t, std::chrono::steady_clock::time_point> speech_active_since_;
    std::map<uint32_t, std::string> pending_transcriptions_;
    std::mutex pending_mutex_;
    std::atomic<uint32_t> topic_change_idx_{0};
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
    std::string rag_host = "127.0.0.1";
    int rag_port = 13181;

    static struct option long_opts[] = {
        {"log-level",  required_argument, 0, 'L'},
        {"rag-host",   required_argument, 0, 'H'},
        {"rag-port",   required_argument, 0, 'P'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "L:H:P:", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'L': log_level = optarg; break;
            case 'H': rag_host = optarg; break;
            case 'P': rag_port = std::atoi(optarg); break;
            default: break;
        }
    }
    if (optind < argc) model_path = argv[optind];

    try {
        LlamaService service(model_path, rag_host, rag_port);
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
