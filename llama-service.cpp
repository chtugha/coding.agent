#include "llama-service.h"
#include "database.h"

// LLaMA includes
#include "llama.h"

#include <iostream>
#include <sstream>
#include <regex>
#include <cstring>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

// Helpers to match current llama.cpp API
static std::vector<llama_token> tokenize_text(struct llama_context * ctx, const std::string & text, bool add_bos) {
    const llama_model * model = llama_get_model(ctx);
    const llama_vocab * vocab = llama_model_get_vocab(model);
    int n_tokens = (int)text.size() + (add_bos ? 1 : 0);
    std::vector<llama_token> tokens(n_tokens);
    int r = llama_tokenize(vocab, text.c_str(), (int32_t)text.size(), tokens.data(), (int32_t)tokens.size(), add_bos, /*parse_special*/ false);
    if (r < 0) {
        tokens.resize(-r);
        int r2 = llama_tokenize(vocab, text.c_str(), (int32_t)text.size(), tokens.data(), (int32_t)tokens.size(), add_bos, false);
        (void)r2;
    } else {
        tokens.resize(r);
    }
    return tokens;
}

static std::string token_to_piece(struct llama_context * ctx, llama_token token) {
    const llama_model * model = llama_get_model(ctx);
    const llama_vocab * vocab = llama_model_get_vocab(model);
    std::vector<char> buf(8, 0);
    int r = llama_token_to_piece(vocab, token, buf.data(), (int32_t)buf.size(), /*lstrip*/ 0, /*special*/ false);
    if (r < 0) {
        buf.resize(-r);
        int r2 = llama_token_to_piece(vocab, token, buf.data(), (int32_t)buf.size(), 0, false);
        (void)r2;
    } else {
        buf.resize(r);
    }
    return std::string(buf.data(), buf.size());
}

// LLaMA Session Implementation
LlamaSession::LlamaSession(const std::string& call_id, const LlamaSessionConfig& config)
    : call_id_(call_id), config_(config), model_(nullptr), ctx_(nullptr),
      sampler_(nullptr), batch_(nullptr), is_active_(false) {
    last_activity_ = std::chrono::steady_clock::now();
}

LlamaSession::~LlamaSession() {
    cleanup_llama_context();
}

bool LlamaSession::initialize() {
    std::lock_guard<std::mutex> lock(session_mutex_);

    if (!initialize_llama_context()) {
        std::cout << "❌ Failed to initialize LLaMA context for call " << call_id_ << std::endl;
        return false;
    }

    // Initialize conversation with system prompt
    conversation_history_ = "Text transcript of a conversation where " + config_.person_name +
                           " talks with an AI assistant named " + config_.bot_name + ".\n" +
                           config_.bot_name + " is helpful, concise, and responds naturally.\n\n";

    is_active_.store(true);
    mark_activity();


    // Prime the system prompt into the KV cache for this session
    if (!prime_system_prompt()) {
        std::cout << "⚠️ Failed to prime system prompt for call " << call_id_ << std::endl;
    }

    std::cout << "✅ LLaMA session initialized for call " << call_id_ << std::endl;
    return true;
}

bool LlamaSession::initialize_llama_context() {
    // If service provided a shared warm context/model, reuse them
    if (config_.shared_ctx && config_.shared_model) {
        ctx_ = config_.shared_ctx;
        model_ = config_.shared_model;
        shared_mutex_ = config_.shared_mutex;
        ctx_shared_ = true;

        // Cache vocab pointer
        vocab_ = llama_model_get_vocab(model_);

        // Initialize sampler
        auto sampler_params = llama_sampler_chain_default_params();
        sampler_ = llama_sampler_chain_init(sampler_params);

        if (config_.temperature > 0.0f) {
            llama_sampler_chain_add(sampler_, llama_sampler_init_top_k(config_.top_k));
            llama_sampler_chain_add(sampler_, llama_sampler_init_top_p(config_.top_p, 1));
            llama_sampler_chain_add(sampler_, llama_sampler_init_temp(config_.temperature));
            llama_sampler_chain_add(sampler_, llama_sampler_init_dist(0));
        } else {
            llama_sampler_chain_add(sampler_, llama_sampler_init_greedy());
        }

        // Initialize batch
        batch_ = new llama_batch;
        *batch_ = llama_batch_init(config_.n_ctx, 0, 1);

        std::cout << "🔁 Reusing preloaded LLaMA model/context for call " << call_id_ << std::endl;
        return true;
    }

    // Otherwise, create a private context/model for this session (fallback)
    llama_backend_init();

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = config_.use_gpu ? config_.n_gpu_layers : 0;

    model_ = llama_model_load_from_file(config_.model_path.c_str(), model_params);
    if (!model_) {
        std::cout << "❌ Failed to load LLaMA model: " << config_.model_path << std::endl;
        return false;
    }

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = config_.n_ctx;
    ctx_params.n_threads = config_.n_threads;

    ctx_ = llama_init_from_model(model_, ctx_params);
    if (!ctx_) {
        std::cout << "❌ Failed to create LLaMA context" << std::endl;
        llama_model_free(model_);
        model_ = nullptr;
        return false;
    }

    vocab_ = llama_model_get_vocab(model_);

    auto sampler_params = llama_sampler_chain_default_params();
    sampler_ = llama_sampler_chain_init(sampler_params);

    if (config_.temperature > 0.0f) {
        llama_sampler_chain_add(sampler_, llama_sampler_init_top_k(config_.top_k));
        llama_sampler_chain_add(sampler_, llama_sampler_init_top_p(config_.top_p, 1));
        llama_sampler_chain_add(sampler_, llama_sampler_init_temp(config_.temperature));
        llama_sampler_chain_add(sampler_, llama_sampler_init_dist(0));
    } else {
        llama_sampler_chain_add(sampler_, llama_sampler_init_greedy());
    }

    batch_ = new llama_batch;
    *batch_ = llama_batch_init(config_.n_ctx, 0, 1);

    return true;
}

void LlamaSession::cleanup_llama_context() {
    if (batch_) {
        llama_batch_free(*batch_);
        delete batch_;
        batch_ = nullptr;
    }

    if (sampler_) {
        llama_sampler_free(sampler_);
        sampler_ = nullptr;
    }

    // Do not free shared context/model owned by the service
    if (!ctx_shared_) {
        if (ctx_) {
            llama_free(ctx_);
            ctx_ = nullptr;
        }
        if (model_) {
            llama_model_free(model_);
            model_ = nullptr;
        }
    } else {
        ctx_ = nullptr;
        model_ = nullptr;
    }
}

bool LlamaSession::prime_system_prompt() {
    if (!ctx_ || !batch_) return false;

    // Derive a stable sequence ID from call_id_
    int sid = 0;
    try {
        sid = std::stoi(call_id_);
    } catch (const std::exception&) {
        for (unsigned char c : call_id_) {
            sid = (int)((sid * 131u) + c) & 0x7fffffff; // simple hash
        }
    }
    if (sid < 0) sid = -sid;
    seq_id_ = sid % 256; // must be < n_seq_max configured in warm ctx

    // Clear any prior state for this sequence and prime with system prompt
    llama_memory_t mem = llama_get_memory(ctx_);
    llama_memory_seq_rm(mem, (llama_seq_id)seq_id_, 0, -1);

    // Tokenize the system prompt with BOS
    std::vector<llama_token> tokens = tokenize_text(ctx_, conversation_history_, true);
    n_past_ = 0;

    if (!tokens.empty()) {
        batch_->n_tokens = tokens.size();
        for (size_t i = 0; i < tokens.size(); ++i) {
            batch_->token[i] = tokens[i];
            batch_->pos[i] = n_past_ + (int)i;
            batch_->n_seq_id[i] = 1;
            batch_->seq_id[i][0] = seq_id_;
            batch_->logits[i] = i == tokens.size() - 1;
        }
        if (llama_decode(ctx_, *batch_) != 0) {
            return false;
        }
        n_past_ += (int)tokens.size();
    }

    primed_ = true;
    return true;
}


std::string LlamaSession::process_text(const std::string& input_text) {
    std::lock_guard<std::mutex> lock(session_mutex_);

    if (!is_active_.load() || !ctx_) {
        return "";
    }

    // When using shared warm context, serialize llama_decode across sessions
    std::unique_lock<std::mutex> shared_lock;
    if (shared_mutex_) {
        shared_lock = std::unique_lock<std::mutex>(*shared_mutex_);
    }

    mark_activity();

    // Format the conversation prompt
    std::string prompt = format_conversation_prompt(input_text);

    // Generate response
    std::string response = generate_response(prompt);

    if (!response.empty()) {
        latest_response_ = response;

        // Update conversation history
        conversation_history_ += config_.person_name + ": " + input_text + "\n";
        conversation_history_ += config_.bot_name + ": " + response + "\n";

        std::cout << "🦙 [" << call_id_ << "] Generated response: " << response << std::endl;
    }

    return response;
}

std::string LlamaSession::format_conversation_prompt(const std::string& user_input) {
    // For incremental decoding with KV reuse, only append the new turn
    std::string prompt;
    prompt += config_.person_name + ": " + user_input + "\n";
    prompt += config_.bot_name + ": ";
    return prompt;
}

std::string LlamaSession::generate_response(const std::string& prompt) {
    if (!ctx_ || !sampler_ || !batch_) {
        return "";
    }

    // Ensure the system prompt has been primed once
    if (!primed_) {
        if (!prime_system_prompt()) {
            std::cout << "❌ Failed to prime system prompt for call " << call_id_ << std::endl;
            return "";
        }
    }

    // Tokenize only the new turn (no BOS for incremental appends)
    std::vector<llama_token> tokens = tokenize_text(ctx_, prompt, false);
    if (tokens.empty()) {
        return "";
    }

    // Append the new prompt tokens at the current position for this sequence
    batch_->n_tokens = tokens.size();
    for (size_t i = 0; i < tokens.size(); i++) {
        batch_->token[i] = tokens[i];
        batch_->pos[i] = n_past_ + (int)i;
        batch_->n_seq_id[i] = 1;
        batch_->seq_id[i][0] = seq_id_;
        batch_->logits[i] = i == tokens.size() - 1;
    }

    if (llama_decode(ctx_, *batch_) != 0) {
        std::cout << "❌ Failed to decode prompt for call " << call_id_ << std::endl;
        return "";
    }
    n_past_ += (int)tokens.size();

    // Generate response tokens
    std::string response;
    for (int i = 0; i < config_.max_tokens; i++) {
        // Sample next token
        llama_token id = llama_sampler_sample(sampler_, ctx_, -1);

        // End of sequence
        if (id == llama_vocab_eos(vocab_)) {
            break;
        }

        // Convert token to text
        std::string token_text = token_to_piece(ctx_, id);
        response += token_text;

        // Stop when the model starts the next user turn
        if (response.find("\n" + config_.person_name + ":") != std::string::npos) {
            size_t pos = response.find("\n" + config_.person_name + ":");
            response = response.substr(0, pos);
            break;
        }

        // Feed the generated token back
        batch_->n_tokens = 1;
        batch_->token[0] = id;
        batch_->pos[0] = n_past_;
        batch_->n_seq_id[0] = 1;
        batch_->seq_id[0][0] = seq_id_;
        batch_->logits[0] = true;

        if (llama_decode(ctx_, *batch_) != 0) {
            break;
        }
        n_past_++;
    }

    // Cleanup response
    response = std::regex_replace(response, std::regex("^\\s+"), "");
    response = std::regex_replace(response, std::regex("\\s+$"), "");

    return response;
}

// Standalone LLaMA Service Implementation
StandaloneLlamaService::StandaloneLlamaService(const LlamaSessionConfig& default_config)
    : default_config_(default_config), server_socket_(-1), running_(false) {
}

StandaloneLlamaService::~StandaloneLlamaService() {
    stop();
}

bool StandaloneLlamaService::start(int tcp_port) {
    if (running_.load()) {
        std::cout << "⚠️ LLaMA service already running" << std::endl;
        return false;
    }

    std::cout << "🚀 Starting LLaMA service on TCP port " << tcp_port << std::endl;
    std::cout << "📁 Model: " << default_config_.model_path << std::endl;

    // Mark service as starting in DB if available
    if (database_) {
        database_->set_llama_service_status("starting");
    }

    // Eager warm load
    auto t0 = std::chrono::steady_clock::now();
    std::cout << "⏳ Preloading LLaMA model..." << std::endl;
    llama_backend_init();
    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = default_config_.use_gpu ? default_config_.n_gpu_layers : 0;
    warm_model_ = llama_model_load_from_file(default_config_.model_path.c_str(), model_params);
    if (!warm_model_) {
        std::cout << "❌ Failed to load LLaMA model: " << default_config_.model_path << std::endl;
        if (database_) database_->set_llama_service_status("error");
        return false;
    }

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = default_config_.n_ctx;
    ctx_params.n_threads = default_config_.n_threads;
    ctx_params.n_seq_max = 256; // allow many independent sequences in shared warm context
    warm_ctx_ = llama_init_from_model(warm_model_, ctx_params);
    if (!warm_ctx_) {
        std::cout << "❌ Failed to create LLaMA context" << std::endl;
        llama_model_free(warm_model_);
        warm_model_ = nullptr;
        if (database_) database_->set_llama_service_status("error");
        return false;
    }

    warm_loaded_ = true;
    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    std::cout << "✅ LLaMA model preloaded in " << ms << " ms" << std::endl;

    // Warm-up decode/generate to compile GPU kernels and allocate graphs
    try {
        const std::string warm_prompt = std::string("System: You are a helpful assistant.\n") +
                                        "User: hi\n" +
                                        "" + default_config_.bot_name + ": ";
        std::vector<llama_token> toks = tokenize_text(warm_ctx_, warm_prompt, /*add_bos*/ true);
        if (!toks.empty()) {
            llama_batch tmp = llama_batch_init(default_config_.n_ctx, 0, 1);
            // feed prompt tokens
            tmp.n_tokens = (int)toks.size();
            for (int i = 0; i < tmp.n_tokens; ++i) {
                tmp.token[i] = toks[i];
                tmp.pos[i] = i;
                tmp.n_seq_id[i] = 1;
                tmp.seq_id[i][0] = 0;
                tmp.logits[i] = (i == tmp.n_tokens - 1);
            }
            (void)llama_decode(warm_ctx_, tmp);

            // sample a couple of tokens and feed back
            auto sp = llama_sampler_chain_init(llama_sampler_chain_default_params());
            llama_sampler_chain_add(sp, llama_sampler_init_greedy());
            int n_past = (int)toks.size();
            for (int i = 0; i < 2; ++i) {
                llama_token id = llama_sampler_sample(sp, warm_ctx_, -1);
                tmp.n_tokens = 1;
                tmp.token[0] = id;
                tmp.pos[0] = n_past;
                tmp.n_seq_id[0] = 1;
                tmp.seq_id[0][0] = 0;
                tmp.logits[0] = true;
                (void)llama_decode(warm_ctx_, tmp);
                n_past++;
            }
            llama_sampler_free(sp);
            std::cout << "✅ LLaMA warm-up completed" << std::endl;
        }
    } catch (...) {
        std::cout << "⚠️ LLaMA warm-up threw exception (non-fatal)" << std::endl;
    }


    // Now mark running and start server
    running_.store(true);
    server_thread_ = std::thread(&StandaloneLlamaService::run_tcp_server, this, tcp_port);

    if (database_) {
        database_->set_llama_service_status("running");
    }

    return true;
}

void StandaloneLlamaService::stop() {
    if (!running_.load() && !warm_loaded_) {
        return;
    }

    std::cout << "🛑 Stopping LLaMA service..." << std::endl;

    running_.store(false);

    if (server_socket_ >= 0) {
        close(server_socket_);
        server_socket_ = -1;
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    cleanup_tcp_threads();

    // Cleanup all sessions
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        sessions_.clear();
    }

    // Free warm context/model
    if (warm_ctx_) { llama_free(warm_ctx_); warm_ctx_ = nullptr; }
    if (warm_model_) { llama_model_free(warm_model_); warm_model_ = nullptr; }
    warm_loaded_ = false;

    if (database_) {
        database_->set_llama_service_status("stopped");
    }

    std::cout << "✅ LLaMA service stopped" << std::endl;
}

bool StandaloneLlamaService::create_session(const std::string& call_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    if (sessions_.find(call_id) != sessions_.end()) {
        std::cout << "⚠️ LLaMA session already exists for call " << call_id << std::endl;
        return true;
    }

    // Use a copy of default config and inject shared warm context/model
    LlamaSessionConfig cfg = default_config_;
    if (warm_loaded_ && warm_model_ && warm_ctx_) {
        cfg.shared_model = warm_model_;
        cfg.shared_ctx = warm_ctx_;
        cfg.shared_mutex = &warm_mutex_;
    }

    auto session = std::make_unique<LlamaSession>(call_id, cfg);
    if (!session->initialize()) {
        return false;
    }

    sessions_[call_id] = std::move(session);
    std::cout << "✅ Created LLaMA session for call " << call_id << std::endl;
    return true;
}

bool StandaloneLlamaService::destroy_session(const std::string& call_id) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    auto it = sessions_.find(call_id);
    if (it == sessions_.end()) {
        return false;
    }

    sessions_.erase(it);
    std::cout << "🗑️ Destroyed LLaMA session for call " << call_id << std::endl;
    return true;
}

std::string StandaloneLlamaService::process_text_for_call(const std::string& call_id, const std::string& text) {
    std::lock_guard<std::mutex> lock(sessions_mutex_);

    auto it = sessions_.find(call_id);
    if (it == sessions_.end()) {
        std::cout << "❌ No LLaMA session found for call " << call_id << std::endl;
        return "";
    }

    return it->second->process_text(text);
}

bool StandaloneLlamaService::init_database(const std::string& db_path) {
    database_ = std::make_unique<Database>();
    if (!database_->init(db_path)) {
        std::cout << "❌ Failed to initialize database at " << db_path << std::endl;
        database_.reset();
        return false;
    }
    std::cout << "💾 LLaMA service connected to DB: " << db_path << std::endl;
    return true;
}

void StandaloneLlamaService::set_output_endpoint(const std::string& host, int port) {
    output_host_ = host;
    output_port_ = port;
    std::cout << "🔌 LLaMA output endpoint set to " << host << ":" << port << std::endl;
}

void StandaloneLlamaService::run_tcp_server(int port) {
    server_socket_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket_ < 0) {
        std::cout << "❌ Failed to create TCP server socket" << std::endl;
        return;
    }

    int opt = 1;
    setsockopt(server_socket_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    if (bind(server_socket_, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
        std::cout << "❌ Failed to bind TCP server socket to port " << port << std::endl;
        close(server_socket_);
        server_socket_ = -1;
        return;
    }

    if (listen(server_socket_, 16) < 0) {
        std::cout << "❌ Failed to listen on TCP server socket" << std::endl;
        close(server_socket_);
        server_socket_ = -1;
        return;
    }

    std::cout << "🦙 LLaMA service listening on TCP port " << port << std::endl;

    while (running_.load()) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);

        int client_socket = accept(server_socket_, (struct sockaddr*)&client_addr, &client_len);
        if (client_socket < 0) {
            if (running_.load()) {
                std::cout << "⚠️ Failed to accept TCP connection" << std::endl;
            }
            continue;
        }

        std::string call_id;
        if (!read_tcp_hello(client_socket, call_id)) {
            std::cout << "❌ Failed to read TCP HELLO" << std::endl;
            close(client_socket);
            continue;
        }

        create_session(call_id);

        call_tcp_threads_[call_id] = std::thread(&StandaloneLlamaService::handle_tcp_text_stream, this, call_id, client_socket);
    }
}

void StandaloneLlamaService::handle_tcp_text_stream(const std::string& call_id, int socket) {
    std::cout << "📥 Starting LLaMA text handler for call " << call_id << std::endl;

    while (running_.load()) {
        std::string text;
        if (!read_tcp_text_chunk(socket, text)) {
            break;
        }

        if (text == "BYE") {
            break;
        }

        if (text.empty()) {
            continue;
        }

        std::cout << "📝 Incoming text [" << call_id << "]: " << text << std::endl;
        std::string response = process_text_for_call(call_id, text);

        if (!response.empty()) {
            std::cout << "💬 Response [" << call_id << "]: " << response << std::endl;
            // 1) Write to DB (separate field)
            if (database_) {
                database_->append_llama_response(call_id, response);
            }
            // 2) Send to output endpoint if configured
            if (!output_host_.empty() && output_port_ > 0) {
                if (connect_output_for_call(call_id)) {
                    send_output_text(call_id, response);
                }
            }
            // 3) Also reply over inbound socket for optional consumers (non-whisper route)
            if (!send_tcp_response(socket, response)) {
                std::cout << "⚠️ Failed to send response back on inbound socket for call " << call_id << std::endl;
            }
        }
    }

    send_tcp_bye(socket);
    close(socket);
    destroy_session(call_id);
    close_output_for_call(call_id);
    std::cout << "📤 Ended LLaMA text handler for call " << call_id << std::endl;
}

bool StandaloneLlamaService::read_tcp_hello(int socket, std::string& call_id) {
    uint32_t length = 0;
    if (recv(socket, &length, 4, 0) != 4) return false;
    length = ntohl(length);
    if (length == 0 || length > 4096) return false;
    std::string buf(length, '\0');
    if (recv(socket, buf.data(), length, 0) != (ssize_t)length) return false;
    call_id = buf;
    std::cout << "👋 HELLO from whisper for call_id=" << call_id << std::endl;
    return true;
}

bool StandaloneLlamaService::read_tcp_text_chunk(int socket, std::string& text) {
    uint32_t length = 0;
    if (recv(socket, &length, 4, 0) != 4) return false;
    length = ntohl(length);
    if (length == 0xFFFFFFFF) { text = "BYE"; return true; }
    if (length == 0 || length > 10*1024*1024) return false;
    text.resize(length);
    if (recv(socket, text.data(), length, 0) != (ssize_t)length) return false;
    return true;
}

bool StandaloneLlamaService::send_tcp_response(int socket, const std::string& response) {
    uint32_t l = htonl((uint32_t)response.size());
    if (send(socket, &l, 4, 0) != 4) return false;
    if (!response.empty() && send(socket, response.data(), response.size(), 0) != (ssize_t)response.size()) return false;
    return true;
}

bool StandaloneLlamaService::send_tcp_bye(int socket) {
    uint32_t bye = 0xFFFFFFFF;
    return send(socket, &bye, 4, 0) == 4;
}

bool StandaloneLlamaService::connect_output_for_call(const std::string& call_id) {
    if (output_host_.empty() || output_port_ <= 0) return false;
    if (output_sockets_.count(call_id)) return true;

    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return false;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(output_port_);


    addr.sin_addr.s_addr = inet_addr(output_host_.c_str());
    if (connect(s, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(s);
        return false;
    }
    // Send HELLO(call_id)
    uint32_t n = htonl((uint32_t)call_id.size());
    if (send(s, &n, 4, 0) != 4) { close(s); return false; }
    if (send(s, call_id.data(), call_id.size(), 0) != (ssize_t)call_id.size()) { close(s); return false; }

    output_sockets_[call_id] = s;
    std::cout << "🔗 Connected output socket for call " << call_id << " to " << output_host_ << ":" << output_port_ << std::endl;
    return true;
}

bool StandaloneLlamaService::send_output_text(const std::string& call_id, const std::string& text) {
    auto it = output_sockets_.find(call_id);
    if (it == output_sockets_.end()) return false;
    int s = it->second;
    uint32_t l = htonl((uint32_t)text.size());
    if (send(s, &l, 4, 0) != 4) return false;
    if (!text.empty() && send(s, text.data(), text.size(), 0) != (ssize_t)text.size()) return false;
    return true;
}

void StandaloneLlamaService::close_output_for_call(const std::string& call_id) {
    auto it = output_sockets_.find(call_id);
    if (it != output_sockets_.end()) {
        send_tcp_bye(it->second);
        close(it->second);
        output_sockets_.erase(it);
    }
}


void StandaloneLlamaService::cleanup_tcp_threads() {
    for (auto & kv : call_tcp_threads_) {
        if (kv.second.joinable()) kv.second.join();
    }
    call_tcp_threads_.clear();
}
