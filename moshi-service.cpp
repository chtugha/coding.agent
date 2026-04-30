// moshi-service.cpp — Moshi full-duplex neural voice service.
//
// Pipeline position (moshi mode): IAP → [MOSHI_SERVICE] → OAP
//
// Replaces the entire VAD → Whisper → LLaMA → TTS chain with a single
// end-to-end neural voice model (Moshi). Receives 24kHz float32 PCM from
// IAP (in moshi mode), bridges audio to/from a moshi-backend subprocess
// via WebSocket (Opus-encoded), and forwards decoded audio to OAP.
//
// Per-call architecture:
//   Each call_id spawns a dedicated moshi-backend subprocess on an ephemeral
//   port. A per-call thread runs a Mongoose mg_mgr event loop to manage the
//   WebSocket connection to that subprocess. Audio flows:
//     IAP → (accumulate 80ms frames) → Opus encode → WS binary → moshi-backend
//     moshi-backend → WS binary → Opus decode → chunk 480 samples → OAP
//
// RAG integration:
//   On first audio packet for a call, fetches patient context from tomedo-crawl
//   (https://127.0.0.1:13181/caller/{call_id}) and injects it as a WebSocket
//   text frame before the first audio frame.
//
// CMD port (MOSHI base+2 = 13157): PING→PONG, STATUS, SET_LOG_LEVEL.
#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <map>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <deque>
#include <sstream>
#include <signal.h>
#include <getopt.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <fcntl.h>
#include <spawn.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <opus.h>

#include "interconnect.h"
#include "tls_cert.h"
#include "mongoose.h"

extern char **environ;

static std::atomic<bool> g_running{true};
static void sig_handler(int) { g_running = false; }

static constexpr int MOSHI_SAMPLE_RATE     = 24000;
static constexpr int MOSHI_FRAME_SAMPLES   = 1920;   // 80ms @ 24kHz
static constexpr int MOSHI_CHUNK_SAMPLES   = 480;    // 20ms @ 24kHz (IAP frame size)
static constexpr int OPUS_BITRATE          = 64000;
static constexpr int OPUS_MAX_PACKET       = 4000;
static constexpr int RAG_TIMEOUT_MS        = 500;
static constexpr size_t RAG_MAX_RESPONSE   = 256 * 1024;
static constexpr int WS_CONNECT_RETRY_MS   = 200;
static constexpr int WS_CONNECT_TIMEOUT_MS = 2000;
static constexpr int SUBPROCESS_KILL_WAIT  = 2;
static constexpr int CMD_POLL_TIMEOUT_MS   = 200;
static constexpr int CMD_RECV_TIMEOUT_S    = 10;
static constexpr int CMD_BUF_SIZE          = 4096;

struct MoshiCallState {
    ~MoshiCallState() {
        if (opus_enc) { opus_encoder_destroy(opus_enc); opus_enc = nullptr; }
        if (opus_dec) { opus_decoder_destroy(opus_dec); opus_dec = nullptr; }
    }

    uint32_t call_id{0};
    pid_t backend_pid{-1};
    int backend_port{0};

    struct mg_mgr mgr;
    struct mg_connection* ws_conn{nullptr};
    std::atomic<bool> ws_connected{false};
    std::thread ws_thread;
    std::atomic<bool> ws_running{false};

    bool rag_injected{false};

    std::vector<float> input_accumulator;
    OpusEncoder* opus_enc{nullptr};
    OpusDecoder* opus_dec{nullptr};

    std::mutex audio_mutex;
    std::mutex output_mutex;
    std::deque<std::vector<float>> output_chunks;

    struct WsFrame {
        std::vector<uint8_t> data;
        int op;
    };
    std::mutex ws_outbound_mutex;
    std::deque<WsFrame> ws_outbound_queue;

    std::chrono::steady_clock::time_point last_activity;
};

class MoshiService {
public:
    MoshiService(const std::string& backend_path, const std::string& backend_args)
        : running_(true),
          backend_path_(backend_path),
          backend_args_(backend_args),
          interconnect_(whispertalk::ServiceType::MOSHI_SERVICE) {
        prodigy_tls::ensure_certs();
        rag_ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (rag_ssl_ctx_) {
            SSL_CTX_set_verify(rag_ssl_ctx_, SSL_VERIFY_PEER, nullptr);
            std::string ca = prodigy_tls::cert_file_path();
            if (SSL_CTX_load_verify_locations(rag_ssl_ctx_, ca.c_str(), nullptr) != 1) {
                SSL_CTX_set_verify(rag_ssl_ctx_, SSL_VERIFY_NONE, nullptr);
            }
        }
    }

    ~MoshiService() {
        if (rag_ssl_ctx_) SSL_CTX_free(rag_ssl_ctx_);
    }

    bool init() {
        if (!interconnect_.initialize()) {
            std::cerr << "Failed to initialize interconnect" << std::endl;
            return false;
        }
        std::cout << "Interconnect initialized (peer-to-peer)" << std::endl;

        log_fwd_.init(whispertalk::FRONTEND_LOG_PORT, whispertalk::ServiceType::MOSHI_SERVICE);

        if (!interconnect_.connect_to_downstream()) {
            std::cout << "Downstream (OAP) not available yet - will auto-reconnect" << std::endl;
        }

        interconnect_.register_call_end_handler([this](uint32_t call_id) {
            this->handle_call_end(call_id);
        });

        resolve_rag_addr();

        return true;
    }

    void set_log_level(const char* level) {
        log_fwd_.set_level(level);
    }

    void run() {
        std::thread receiver_thread(&MoshiService::receiver_loop, this);
        std::thread sender_thread(&MoshiService::sender_loop, this);
        std::thread cmd_thread(&MoshiService::command_listener_loop, this);

        while (running_ && g_running) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        running_ = false;

        int sock = cmd_sock_.exchange(-1);
        if (sock >= 0) ::close(sock);

        std::map<uint32_t, std::shared_ptr<MoshiCallState>> to_cleanup;
        {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            to_cleanup.swap(calls_);
        }
        for (auto& [cid, state] : to_cleanup) {
            cleanup_call(state);
        }

        receiver_thread.join();
        sender_thread.join();
        cmd_thread.join();
        interconnect_.shutdown();
    }

private:
    void receiver_loop() {
        while (running_ && g_running) {
            whispertalk::Packet pkt;
            if (!interconnect_.recv_from_upstream(pkt, 100)) {
                continue;
            }

            if (!pkt.is_valid() || pkt.payload_size == 0 || (pkt.payload_size % sizeof(float)) != 0) {
                continue;
            }

            size_t sample_count = pkt.payload_size / sizeof(float);
            const float* samples = reinterpret_cast<const float*>(pkt.payload.data());

            std::shared_ptr<MoshiCallState> state;
            bool new_call = false;
            {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                auto it = calls_.find(pkt.call_id);
                if (it == calls_.end()) {
                    state = std::make_shared<MoshiCallState>();
                    state->call_id = pkt.call_id;
                    state->last_activity = std::chrono::steady_clock::now();
                    state->input_accumulator.reserve(MOSHI_FRAME_SAMPLES);

                    int err;
                    state->opus_enc = opus_encoder_create(MOSHI_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &err);
                    if (state->opus_enc) {
                        opus_encoder_ctl(state->opus_enc, OPUS_SET_BITRATE(OPUS_BITRATE));
                    }
                    state->opus_dec = opus_decoder_create(MOSHI_SAMPLE_RATE, 1, &err);

                    calls_[pkt.call_id] = state;
                    new_call = true;
                } else {
                    state = it->second;
                }
            }

            if (new_call) {
                if (!spawn_backend(state)) {
                    log_fwd_.forward(whispertalk::LogLevel::ERROR, pkt.call_id,
                        "Failed to spawn moshi-backend subprocess");
                    std::lock_guard<std::mutex> lock(calls_mutex_);
                    calls_.erase(pkt.call_id);
                    continue;
                }

                start_ws_client(state);
                inject_rag_context(state);

                log_fwd_.forward(whispertalk::LogLevel::INFO, pkt.call_id,
                    "New Moshi call: backend pid=%d port=%d", state->backend_pid, state->backend_port);
            }

            state->last_activity = std::chrono::steady_clock::now();
            process_audio_input(state, samples, sample_count);
        }
    }

    void sender_loop() {
        while (running_ && g_running) {
            bool sent_any = false;
            {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                for (auto& [cid, state] : calls_) {
                    std::vector<float> chunk;
                    {
                        std::lock_guard<std::mutex> olock(state->output_mutex);
                        if (!state->output_chunks.empty()) {
                            chunk = std::move(state->output_chunks.front());
                            state->output_chunks.pop_front();
                        }
                    }
                    if (!chunk.empty()) {
                        whispertalk::Packet out(cid, chunk.data(),
                            static_cast<uint32_t>(chunk.size() * sizeof(float)));
                        interconnect_.send_to_downstream(out);
                        sent_any = true;
                    }
                }
            }
            if (!sent_any) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
    }

    void enqueue_ws_frame(std::shared_ptr<MoshiCallState>& state,
                          const void* data, size_t len, int op) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        std::lock_guard<std::mutex> lock(state->ws_outbound_mutex);
        state->ws_outbound_queue.push_back({std::vector<uint8_t>(p, p + len), op});
    }

    void process_audio_input(std::shared_ptr<MoshiCallState>& state,
                             const float* samples, size_t count) {
        state->input_accumulator.insert(state->input_accumulator.end(),
                                        samples, samples + count);

        while (state->input_accumulator.size() >= (size_t)MOSHI_FRAME_SAMPLES) {
            if (state->ws_connected.load()) {
                std::lock_guard<std::mutex> alock(state->audio_mutex);
                if (state->opus_enc) {
                    unsigned char opus_buf[OPUS_MAX_PACKET];
                    int opus_len = opus_encode_float(state->opus_enc,
                        state->input_accumulator.data(), MOSHI_FRAME_SAMPLES,
                        opus_buf, OPUS_MAX_PACKET);

                    if (opus_len > 0) {
                        enqueue_ws_frame(state, opus_buf, opus_len, WEBSOCKET_OP_BINARY);
                    }
                }
            }

            state->input_accumulator.erase(state->input_accumulator.begin(),
                                           state->input_accumulator.begin() + MOSHI_FRAME_SAMPLES);
        }
    }

    static void ws_event_handler(struct mg_connection* c, int ev, void* ev_data) {
        auto* state = static_cast<MoshiCallState*>(c->fn_data);
        if (!state) return;

        if (ev == MG_EV_WS_OPEN) {
            state->ws_connected.store(true);
        } else if (ev == MG_EV_WS_MSG) {
            auto* msg = static_cast<struct mg_ws_message*>(ev_data);
            if ((msg->flags & WEBSOCKET_OP_BINARY) && state->opus_dec) {
                float pcm[MOSHI_FRAME_SAMPLES];
                int decoded = opus_decode_float(state->opus_dec,
                    reinterpret_cast<const unsigned char*>(msg->data.buf),
                    static_cast<int>(msg->data.len),
                    pcm, MOSHI_FRAME_SAMPLES, 0);

                if (decoded > 0) {
                    std::lock_guard<std::mutex> lock(state->output_mutex);
                    for (int i = 0; i < decoded; i += MOSHI_CHUNK_SAMPLES) {
                        int chunk_size = std::min(MOSHI_CHUNK_SAMPLES, decoded - i);
                        state->output_chunks.emplace_back(pcm + i, pcm + i + chunk_size);
                    }
                }
            }
        } else if (ev == MG_EV_CLOSE) {
            state->ws_connected.store(false);
            state->ws_conn = nullptr;
        } else if (ev == MG_EV_ERROR) {
            state->ws_connected.store(false);
        }
    }

    void start_ws_client(std::shared_ptr<MoshiCallState>& state) {
        state->ws_running.store(true);
        mg_mgr_init(&state->mgr);

        state->ws_thread = std::thread([this, state]() {
            std::string url = "ws://127.0.0.1:" + std::to_string(state->backend_port) + "/ws";
            auto start = std::chrono::steady_clock::now();

            while (state->ws_running.load() && !state->ws_connected.load()) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (elapsed > WS_CONNECT_TIMEOUT_MS) {
                    log_fwd_.forward(whispertalk::LogLevel::ERROR, state->call_id,
                        "WebSocket connect timeout after %dms", WS_CONNECT_TIMEOUT_MS);
                    break;
                }

                state->ws_conn = mg_ws_connect(&state->mgr, url.c_str(),
                    ws_event_handler, state.get(), NULL);
                if (state->ws_conn) {
                    for (int i = 0; i < WS_CONNECT_RETRY_MS / 10 && state->ws_running.load(); i++) {
                        mg_mgr_poll(&state->mgr, 10);
                        if (state->ws_connected.load()) break;
                    }
                }
                if (!state->ws_connected.load()) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(WS_CONNECT_RETRY_MS));
                }
            }

            while (state->ws_running.load()) {
                mg_mgr_poll(&state->mgr, 10);

                if (state->ws_connected.load() && state->ws_conn) {
                    std::deque<MoshiCallState::WsFrame> frames;
                    {
                        std::lock_guard<std::mutex> lock(state->ws_outbound_mutex);
                        frames.swap(state->ws_outbound_queue);
                    }
                    for (auto& f : frames) {
                        mg_ws_send(state->ws_conn, f.data.data(), f.data.size(), f.op);
                    }
                }
            }

            mg_mgr_free(&state->mgr);
        });
    }

    bool spawn_backend(std::shared_ptr<MoshiCallState>& state) {
        int port = allocate_ephemeral_port();
        if (port <= 0) return false;
        state->backend_port = port;

        std::string port_str = std::to_string(port);

        std::vector<std::string> arg_strings;
        arg_strings.push_back(backend_path_);
        arg_strings.push_back("--port");
        arg_strings.push_back(port_str);

        if (!backend_args_.empty()) {
            std::istringstream iss(backend_args_);
            std::string token;
            while (iss >> token) {
                arg_strings.push_back(token);
            }
        }

        std::vector<char*> argv;
        for (auto& s : arg_strings) argv.push_back(&s[0]);
        argv.push_back(nullptr);

        pid_t pid;
        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

        int ret = posix_spawn(&pid, backend_path_.c_str(), &actions, nullptr,
                              argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);

        if (ret != 0) {
            log_fwd_.forward(whispertalk::LogLevel::ERROR, state->call_id,
                "posix_spawn failed: %s", strerror(ret));
            return false;
        }

        state->backend_pid = pid;
        return true;
    }

    int allocate_ephemeral_port() {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return -1;

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;

        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            ::close(sock);
            return -1;
        }

        struct sockaddr_in bound{};
        socklen_t len = sizeof(bound);
        if (getsockname(sock, (struct sockaddr*)&bound, &len) < 0) {
            ::close(sock);
            return -1;
        }

        int port = ntohs(bound.sin_port);
        ::close(sock);
        return port;
    }

    void inject_rag_context(std::shared_ptr<MoshiCallState>& state) {
        state->rag_injected = true;
        if (!rag_addr_resolved_) return;

        std::string path = "/caller/" + std::to_string(state->call_id);
        std::string body = rag_http_get(path);
        if (body.empty()) {
            log_fwd_.forward(whispertalk::LogLevel::DEBUG, state->call_id,
                "No RAG context available for call");
            return;
        }

        enqueue_ws_frame(state, body.c_str(), body.size(), WEBSOCKET_OP_TEXT);
        log_fwd_.forward(whispertalk::LogLevel::INFO, state->call_id,
            "Enqueued RAG context (%zu bytes) as text frame", body.size());
    }

    void handle_call_end(uint32_t call_id) {
        std::shared_ptr<MoshiCallState> state;
        {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            auto it = calls_.find(call_id);
            if (it == calls_.end()) return;
            state = std::move(it->second);
            calls_.erase(it);
        }

        log_fwd_.forward(whispertalk::LogLevel::INFO, call_id, "Call ended, cleaning up");
        cleanup_call(state);
    }

    void cleanup_call(std::shared_ptr<MoshiCallState>& state) {
        if (state->ws_connected.load()) {
            uint8_t empty = 0;
            enqueue_ws_frame(state, &empty, 0, WEBSOCKET_OP_CLOSE);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        state->ws_running.store(false);

        if (state->ws_thread.joinable()) {
            state->ws_thread.join();
        }

        if (state->backend_pid > 0) {
            kill(state->backend_pid, SIGTERM);

            auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(SUBPROCESS_KILL_WAIT);
            int status;
            while (std::chrono::steady_clock::now() < deadline) {
                pid_t result = waitpid(state->backend_pid, &status, WNOHANG);
                if (result != 0) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            if (waitpid(state->backend_pid, &status, WNOHANG) == 0) {
                kill(state->backend_pid, SIGKILL);
                waitpid(state->backend_pid, &status, 0);
            }
            log_fwd_.forward(whispertalk::LogLevel::DEBUG, state->call_id,
                "Backend subprocess pid=%d terminated", state->backend_pid);
        }

        {
            std::lock_guard<std::mutex> alock(state->audio_mutex);
            if (state->opus_enc) { opus_encoder_destroy(state->opus_enc); state->opus_enc = nullptr; }
            if (state->opus_dec) { opus_decoder_destroy(state->opus_dec); state->opus_dec = nullptr; }
        }
    }

    bool resolve_rag_addr() {
        struct addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo* res = nullptr;
        if (getaddrinfo("127.0.0.1", "13181", &hints, &res) != 0 || !res)
            return false;
        memcpy(&rag_addr_, res->ai_addr, res->ai_addrlen);
        rag_addrlen_ = res->ai_addrlen;
        freeaddrinfo(res);
        rag_addr_resolved_ = true;
        return true;
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
            ::close(sock);
            return "";
        }
        int conn_err = 0;
        socklen_t conn_len = sizeof(conn_err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &conn_err, &conn_len);
        if (conn_err != 0) {
            ::close(sock);
            return "";
        }
        fcntl(sock, F_SETFL, flags);

        if (!rag_ssl_ctx_) { ::close(sock); return ""; }
        SSL* ssl = SSL_new(rag_ssl_ctx_);
        if (!ssl) { ::close(sock); return ""; }
        SSL_set_fd(ssl, sock);
        SSL_set_tlsext_host_name(ssl, "127.0.0.1");
        if (SSL_connect(ssl) != 1) {
            SSL_free(ssl); ::close(sock);
            return "";
        }

        std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:13181\r\nConnection: close\r\n\r\n";
        if (SSL_write(ssl, req.c_str(), static_cast<int>(req.size())) <= 0) {
            SSL_shutdown(ssl); SSL_free(ssl); ::close(sock);
            return "";
        }
        std::string response;
        char buf[4096];
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(RAG_TIMEOUT_MS);  // fresh deadline after connect+TLS
        while (response.size() < RAG_MAX_RESPONSE) {
            auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remaining <= 0) break;
            struct pollfd rpfd{sock, POLLIN, 0};
            if (poll(&rpfd, 1, (int)remaining) <= 0 || !(rpfd.revents & POLLIN)) break;
            int n = SSL_read(ssl, buf, sizeof(buf));
            if (n <= 0) break;
            response.append(buf, n);
        }
        SSL_shutdown(ssl); SSL_free(ssl); ::close(sock);

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

    void command_listener_loop() {
        uint16_t port = whispertalk::service_cmd_port(whispertalk::ServiceType::MOSHI_SERVICE);
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return;
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons(port);
        if (bind(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
            std::cerr << "Moshi cmd: bind port " << port << " failed" << std::endl;
            ::close(sock);
            return;
        }
        if (listen(sock, 4) < 0) {
            std::cerr << "Moshi cmd: listen failed" << std::endl;
            ::close(sock);
            return;
        }
        cmd_sock_.store(sock);
        std::cout << "Moshi command listener on port " << port << std::endl;
        while (running_ && g_running) {
            struct pollfd pfd{sock, POLLIN, 0};
            if (poll(&pfd, 1, CMD_POLL_TIMEOUT_MS) <= 0) continue;
            int csock = accept(sock, nullptr, nullptr);
            if (csock < 0) continue;
            struct timeval tv{CMD_RECV_TIMEOUT_S, 0};
            setsockopt(csock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            char buf[CMD_BUF_SIZE];
            int n = (int)recv(csock, buf, sizeof(buf) - 1, 0);
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
        if (cmd == "PING") return "PONG\n";
        if (cmd.rfind("SET_LOG_LEVEL:", 0) == 0) {
            std::string level = cmd.substr(14);
            log_fwd_.set_level(level.c_str());
            return "OK\n";
        }
        if (cmd == "STATUS") {
            size_t active;
            {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                active = calls_.size();
            }
            return "ACTIVE_CALLS:" + std::to_string(active)
                + ":UPSTREAM:" + (interconnect_.upstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected")
                + ":DOWNSTREAM:" + (interconnect_.downstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected")
                + "\n";
        }
        return "ERROR:Unknown command\n";
    }

    std::atomic<bool> running_;
    std::atomic<int> cmd_sock_{-1};
    std::string backend_path_;
    std::string backend_args_;
    whispertalk::InterconnectNode interconnect_;
    whispertalk::LogForwarder log_fwd_;
    std::mutex calls_mutex_;
    std::map<uint32_t, std::shared_ptr<MoshiCallState>> calls_;
    struct sockaddr_storage rag_addr_{};
    socklen_t rag_addrlen_{0};
    bool rag_addr_resolved_{false};
    SSL_CTX* rag_ssl_ctx_{nullptr};
};

int main(int argc, char** argv) {
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    std::string log_level = "INFO";
    std::string backend_path = "moshi-backend";
    std::string backend_args;

    static struct option long_opts[] = {
        {"log-level",          required_argument, 0, 'L'},
        {"moshi-backend-path", required_argument, 0, 'b'},
        {"moshi-backend-args", required_argument, 0, 'a'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "L:b:a:", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'L': log_level = optarg; break;
            case 'b': backend_path = optarg; break;
            case 'a': backend_args = optarg; break;
            default: break;
        }
    }

    std::cout << "Moshi backend path: " << backend_path << std::endl;

    try {
        MoshiService service(backend_path, backend_args);
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
