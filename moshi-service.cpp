// moshi-service.cpp — Moshi full-duplex neural voice service.
//
// Pipeline position (moshi mode): IAP → [MOSHI_SERVICE] → OAP
//
// Replaces the entire VAD → Whisper → LLaMA → TTS chain with a single
// end-to-end neural voice model (Moshi). Receives 24kHz float32 PCM from
// IAP (in moshi mode) and forwards decoded audio to OAP.
//
// Backend architecture:
//   moshi-service maintains a registry of persistent moshi-backend processes
//   (one per configured language), started at service init time. Each backend
//   is a compiled Rust moshi-backend binary running with batch_size ≥ 1.
//   On a new call, the service picks the backend matching the current pipeline
//   language and claims a WebSocket slot. If no language-specific backend is
//   available the default (English) backend is used. When the batch is full
//   the call is rejected gracefully.
//
// Moshi WebSocket protocol (binary-framed):
//   Every WS binary message starts with 1 byte message type (MT):
//     MT=0  Handshake  — 8 bytes payload: u32 proto-version + u32 model-version
//     MT=1  Audio      — OGG/Opus container (ogg pages wrapping Opus @ 24kHz mono)
//     MT=2  Text       — UTF-8 string
//     MT=3  Control    — 1-byte sub-command
//     MT=4  Metadata   — JSON UTF-8
//     MT=5  Error      — UTF-8 error string
//     MT=6  Ping
//     MT=7  ColoredText
//     MT=8  ReferenceText
//     MT=9  ColoredReferenceText
//   Audio from us→backend: MT=1 + OGG-wrapped Opus (24kHz mono, 960-sample frames)
//   Audio from backend→us: MT=1 + OGG-wrapped Opus → decode → float32 PCM → OAP
//
// Language routing:
//   --backend-config <lang>:<path/to/config.json>  (repeatable)
//   --default-language <lang>                       (default: "en")
//   --backend-batch-size <N>                        (default: 4)
//   On call start the service reads the current pipeline language from the
//   interconnect metadata (if present) or uses the default.
//
// RAG / tomedo context injection:
//   On first audio packet, fetches patient context from tomedo-crawl
//   (https://127.0.0.1:13181/caller/{call_id}) and injects it as a
//   Metadata frame (MT=4, JSON) before the first audio frame. This gives
//   Moshi the patient name and any other context fields available at call start.
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
#include <fstream>
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

static constexpr int MOSHI_SAMPLE_RATE      = 24000;
static constexpr int MOSHI_OPUS_FRAME       = 960;    // 40ms @ 24kHz — valid Opus frame size
static constexpr int MOSHI_CHUNK_SAMPLES    = 480;    // 20ms @ 24kHz (IAP frame size)
static constexpr int OPUS_BITRATE           = 64000;
static constexpr int OPUS_MAX_PACKET        = 4000;
static constexpr int RAG_TIMEOUT_MS         = 500;
static constexpr size_t RAG_MAX_RESPONSE    = 256 * 1024;
static constexpr int WS_CONNECT_RETRY_MS    = 200;
static constexpr int WS_CONNECT_TIMEOUT_MS  = 4000;
static constexpr int SUBPROCESS_KILL_WAIT   = 3;
static constexpr int CMD_POLL_TIMEOUT_MS    = 200;
static constexpr int CMD_RECV_TIMEOUT_S     = 10;
static constexpr int CMD_BUF_SIZE           = 4096;
static constexpr int BACKEND_STARTUP_WAIT_MS= 3000;  // grace after spawning backend

// ─── Moshi wire protocol constants ──────────────────────────────────────────
static constexpr uint8_t MT_HANDSHAKE   = 0;
static constexpr uint8_t MT_AUDIO       = 1;
static constexpr uint8_t MT_TEXT        = 2;
static constexpr uint8_t MT_CONTROL     = 3;
static constexpr uint8_t MT_METADATA    = 4;
static constexpr uint8_t MT_ERROR       = 5;
static constexpr uint8_t MT_PING        = 6;
static constexpr uint8_t MT_COLORED_TEXT= 7;
static constexpr uint8_t MT_REFERENCE   = 8;
static constexpr uint8_t MT_COLORED_REF = 9;

// ─── OGG page builder (minimal, enough to wrap one Opus packet per page) ───
// OGG capture pattern + header fields as per RFC 3533.
// We produce one OGG page per Opus frame, which is what the moshi-backend does.
static std::vector<uint8_t> ogg_opus_head() {
    // OpusHead identification header (19 bytes)
    std::vector<uint8_t> h = {
        'O','p','u','s','H','e','a','d',
        1,                                      // version
        1,                                      // channel count
        0x00, 0x0B,                             // pre-skip (LE) = 2816
        0xC0, 0x5D, 0x00, 0x00,                 // input sample rate = 24000 LE
        0x00, 0x00,                             // output gain
        0                                       // channel mapping family
    };
    return h;
}

static std::vector<uint8_t> ogg_opus_tags() {
    // OpusTags comment header (minimal)
    std::string vendor = "moshi-service";
    std::vector<uint8_t> t;
    t.insert(t.end(), {'O','p','u','s','T','a','g','s'});
    uint32_t vlen = static_cast<uint32_t>(vendor.size());
    t.push_back(vlen & 0xff); t.push_back((vlen>>8)&0xff);
    t.push_back((vlen>>16)&0xff); t.push_back((vlen>>24)&0xff);
    t.insert(t.end(), vendor.begin(), vendor.end());
    t.push_back(0); t.push_back(0); t.push_back(0); t.push_back(0);
    return t;
}

// Compute CRC-32 for OGG (poly 0x04C11DB7, byte-by-byte)
static uint32_t ogg_crc32(const uint8_t* data, size_t len) {
    static const auto table = [](){
        std::array<uint32_t,256> t{};
        for (int i = 0; i < 256; i++) {
            uint32_t crc = static_cast<uint32_t>(i) << 24;
            for (int j = 0; j < 8; j++)
                crc = (crc & 0x80000000u) ? ((crc << 1) ^ 0x04C11DB7u) : (crc << 1);
            t[static_cast<size_t>(i)] = crc;
        }
        return t;
    }();
    uint32_t crc = 0;
    for (size_t i = 0; i < len; i++)
        crc = (crc << 8) ^ table[((crc >> 24) ^ data[i]) & 0xff];
    return crc;
}

struct OggPage {
    uint32_t serial{42};
    uint32_t sequence{0};
    uint64_t granule{0};
    bool bos{false};
    bool eos{false};
};

static std::vector<uint8_t> build_ogg_page(OggPage& pg,
                                             const uint8_t* packet, size_t pkt_len) {
    // OGG page structure (RFC 3533)
    std::vector<uint8_t> buf;
    buf.reserve(27 + 1 + pkt_len);

    // capture pattern
    buf.insert(buf.end(), {'O','g','g','S'});
    buf.push_back(0);  // version

    uint8_t flags = 0;
    if (pg.bos) flags |= 0x02;
    if (pg.eos) flags |= 0x04;
    buf.push_back(flags);

    // granule position (8 bytes LE)
    uint64_t gp = pg.granule;
    for (int i = 0; i < 8; i++) { buf.push_back(gp & 0xff); gp >>= 8; }

    // stream serial (4 bytes LE)
    uint32_t ser = pg.serial;
    for (int i = 0; i < 4; i++) { buf.push_back(ser & 0xff); ser >>= 8; }

    // sequence number (4 bytes LE)
    uint32_t seq = pg.sequence++;
    for (int i = 0; i < 4; i++) { buf.push_back(seq & 0xff); seq >>= 8; }

    // CRC placeholder (4 bytes)
    size_t crc_offset = buf.size();
    buf.push_back(0); buf.push_back(0); buf.push_back(0); buf.push_back(0);

    // page_segments (1 byte) + segment table
    // For simplicity (packet ≤ 255 bytes per segment, max 255 segments = 65025 bytes)
    // We build the lacing values for the full packet then the lacing terminator.
    std::vector<uint8_t> lacing;
    size_t remaining = pkt_len;
    while (remaining >= 255) { lacing.push_back(255); remaining -= 255; }
    lacing.push_back(static_cast<uint8_t>(remaining));

    buf.push_back(static_cast<uint8_t>(lacing.size()));
    buf.insert(buf.end(), lacing.begin(), lacing.end());
    buf.insert(buf.end(), packet, packet + pkt_len);

    // Fill CRC
    uint32_t crc = ogg_crc32(buf.data(), buf.size());
    buf[crc_offset+0] = crc & 0xff;
    buf[crc_offset+1] = (crc >> 8) & 0xff;
    buf[crc_offset+2] = (crc >> 16) & 0xff;
    buf[crc_offset+3] = (crc >> 24) & 0xff;

    pg.bos = false;
    return buf;
}

// Build a complete OGG/Opus stream prefix (identification + comment headers in BOS pages).
static std::vector<uint8_t> build_ogg_opus_preamble(uint32_t serial) {
    OggPage pg;
    pg.serial = serial;
    pg.bos = true;
    pg.granule = static_cast<uint64_t>(-1);

    auto head = ogg_opus_head();
    auto page0 = build_ogg_page(pg, head.data(), head.size());

    pg.granule = static_cast<uint64_t>(-1);
    auto tags = ogg_opus_tags();
    auto page1 = build_ogg_page(pg, tags.data(), tags.size());

    std::vector<uint8_t> out;
    out.insert(out.end(), page0.begin(), page0.end());
    out.insert(out.end(), page1.begin(), page1.end());
    return out;
}

// ─── Backend descriptor ─────────────────────────────────────────────────────
struct BackendConfig {
    std::string language;
    std::string config_path;
    std::string binary_path;   // path to moshi-backend binary
    int         port{0};       // assigned at spawn time
    int         batch_size{4};
};

// ─── Per-call state ─────────────────────────────────────────────────────────
struct MoshiCallState {
    ~MoshiCallState() {
        if (opus_enc) { opus_encoder_destroy(opus_enc); opus_enc = nullptr; }
        if (opus_dec) { opus_decoder_destroy(opus_dec); opus_dec = nullptr; }
    }

    uint32_t call_id{0};
    std::string language;

    // WebSocket connection to the backend slot
    struct mg_mgr mgr{};
    struct mg_connection* ws_conn{nullptr};
    std::atomic<bool> ws_connected{false};
    std::thread ws_thread;
    std::atomic<bool> ws_running{false};

    // Audio codec
    std::vector<float> input_accumulator;
    OpusEncoder* opus_enc{nullptr};
    OpusDecoder* opus_dec{nullptr};

    // OGG state (per-call stream)
    OggPage ogg_out{};
    bool ogg_preamble_sent{false};
    uint64_t ogg_granule{0};

    // Output audio queue
    std::mutex output_mutex;
    std::deque<std::vector<float>> output_chunks;

    // Outbound WS frame queue (opaque: MT byte already prepended)
    struct WsFrame {
        std::vector<uint8_t> data;
    };
    std::mutex ws_outbound_mutex;
    std::deque<WsFrame> ws_outbound_queue;

    bool rag_injected{false};

    std::mutex text_mutex;
    std::deque<std::string> pending_text;

    std::chrono::steady_clock::time_point last_activity;
};

// ─── Per-backend process state ───────────────────────────────────────────────
struct BackendProcess {
    BackendConfig config;
    pid_t pid{-1};
    int port{0};
    std::atomic<bool> running{false};
};

// ─── MoshiService ────────────────────────────────────────────────────────────
class MoshiService {
public:
    MoshiService(std::vector<BackendConfig> backends,
                 std::string default_language)
        : running_(true)
        , default_language_(std::move(default_language))
        , interconnect_(whispertalk::ServiceType::MOSHI_SERVICE)
    {
        prodigy_tls::ensure_certs();
        rag_ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (rag_ssl_ctx_) {
            SSL_CTX_set_verify(rag_ssl_ctx_, SSL_VERIFY_PEER, nullptr);
            std::string ca = prodigy_tls::cert_file_path();
            if (SSL_CTX_load_verify_locations(rag_ssl_ctx_, ca.c_str(), nullptr) != 1)
                SSL_CTX_set_verify(rag_ssl_ctx_, SSL_VERIFY_NONE, nullptr);
        }

        for (auto& bc : backends) {
            auto bp = std::make_shared<BackendProcess>();
            bp->config = bc;
            backend_processes_[bc.language] = bp;
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
        std::cout << "Interconnect initialized" << std::endl;

        log_fwd_.init(whispertalk::FRONTEND_LOG_PORT, whispertalk::ServiceType::MOSHI_SERVICE);

        if (!interconnect_.connect_to_downstream())
            std::cout << "Downstream (OAP) not available yet — will auto-reconnect" << std::endl;

        interconnect_.register_call_end_handler([this](uint32_t call_id) {
            handle_call_end(call_id);
        });

        resolve_rag_addr();

        for (auto& [lang, bp] : backend_processes_) {
            if (!spawn_backend_process(bp))
                std::cerr << "WARNING: Failed to start moshi backend for language '" << lang << "'" << std::endl;
        }

        return true;
    }

    void set_log_level(const char* level) { log_fwd_.set_level(level); }

    void run() {
        std::thread receiver_thread(&MoshiService::receiver_loop, this);
        std::thread sender_thread(&MoshiService::sender_loop, this);
        std::thread cmd_thread(&MoshiService::command_listener_loop, this);

        while (running_ && g_running)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        running_ = false;

        {
            int sock = cmd_sock_.exchange(-1);
            if (sock >= 0) ::close(sock);
        }

        std::map<uint32_t, std::shared_ptr<MoshiCallState>> to_cleanup;
        {
            std::lock_guard<std::mutex> lock(calls_mutex_);
            to_cleanup.swap(calls_);
        }
        for (auto& [cid, state] : to_cleanup)
            cleanup_call(state);

        for (auto& [lang, bp] : backend_processes_)
            stop_backend_process(bp);

        receiver_thread.join();
        sender_thread.join();
        cmd_thread.join();
        interconnect_.shutdown();
    }

private:
    // ── Audio receiver loop ─────────────────────────────────────────────────
    void receiver_loop() {
        while (running_ && g_running) {
            whispertalk::Packet pkt;
            if (!interconnect_.recv_from_upstream(pkt, 100))
                continue;

            if (!pkt.is_valid() || pkt.payload_size == 0
                || (pkt.payload_size % sizeof(float)) != 0)
                continue;

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
                    state->language = default_language_;
                    state->last_activity = std::chrono::steady_clock::now();
                    state->input_accumulator.reserve(MOSHI_OPUS_FRAME);

                    // OGG stream serial is call_id (unique per call)
                    state->ogg_out.serial = pkt.call_id;
                    state->ogg_out.bos = true;

                    int err;
                    state->opus_enc = opus_encoder_create(MOSHI_SAMPLE_RATE, 1,
                                                          OPUS_APPLICATION_VOIP, &err);
                    if (state->opus_enc)
                        opus_encoder_ctl(state->opus_enc, OPUS_SET_BITRATE(OPUS_BITRATE));
                    state->opus_dec = opus_decoder_create(MOSHI_SAMPLE_RATE, 1, &err);

                    calls_[pkt.call_id] = state;
                    new_call = true;
                } else {
                    state = it->second;
                }
            }

            if (new_call) {
                auto* bp = pick_backend(state->language);
                if (!bp) {
                    log_fwd_.forward(whispertalk::LogLevel::ERROR, pkt.call_id,
                        "No backend available for language '%s'", state->language.c_str());
                    std::lock_guard<std::mutex> lock(calls_mutex_);
                    calls_.erase(pkt.call_id);
                    continue;
                }

                start_ws_client(state, bp->port);

                // RAG: async fetch from tomedo-crawl
                if (rag_addr_resolved_ && rag_ssl_ctx_) {
                    SSL_CTX_up_ref(rag_ssl_ctx_);
                    auto ssl_copy = rag_ssl_ctx_;
                    auto addr_copy = rag_addr_;
                    auto addrlen_copy = rag_addrlen_;
                    auto rag_state = state;
                    std::thread([rag_state, ssl_copy, addr_copy, addrlen_copy]() {
                        std::string path = "/caller/" + std::to_string(rag_state->call_id);
                        std::string body = rag_http_get_static(path, addr_copy, addrlen_copy, ssl_copy);
                        if (!body.empty()) {
                            // Inject as MT=4 Metadata frame
                            std::vector<uint8_t> frame;
                            frame.push_back(MT_METADATA);
                            frame.insert(frame.end(), body.begin(), body.end());
                            std::lock_guard<std::mutex> lock(rag_state->ws_outbound_mutex);
                            rag_state->ws_outbound_queue.push_back({std::move(frame)});
                        }
                        SSL_CTX_free(ssl_copy);
                    }).detach();
                }

                log_fwd_.forward(whispertalk::LogLevel::INFO, pkt.call_id,
                    "New Moshi call: language=%s backend_port=%d",
                    state->language.c_str(), bp->port);
            }

            state->last_activity = std::chrono::steady_clock::now();
            process_audio_input(state, samples, sample_count);
        }
    }

    // ── Audio sender loop ───────────────────────────────────────────────────
    void sender_loop() {
        struct PendingChunk { uint32_t call_id; std::vector<float> data; };
        struct PendingText { uint32_t call_id; std::string text; };
        while (running_ && g_running) {
            std::vector<PendingChunk> pending;
            std::vector<PendingText> pending_texts;
            {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                for (auto& [cid, state] : calls_) {
                    {
                        std::lock_guard<std::mutex> olock(state->output_mutex);
                        while (!state->output_chunks.empty()) {
                            pending.push_back({cid, std::move(state->output_chunks.front())});
                            state->output_chunks.pop_front();
                        }
                    }
                    {
                        std::lock_guard<std::mutex> tlock(state->text_mutex);
                        while (!state->pending_text.empty()) {
                            pending_texts.push_back({cid, std::move(state->pending_text.front())});
                            state->pending_text.pop_front();
                        }
                    }
                }
            }
            for (auto& p : pending) {
                whispertalk::Packet out(p.call_id, p.data.data(),
                    static_cast<uint32_t>(p.data.size() * sizeof(float)));
                interconnect_.send_to_downstream(out);
            }
            for (auto& t : pending_texts) {
                log_fwd_.forward(whispertalk::LogLevel::INFO, t.call_id,
                    "Moshi transcription: %s", t.text.c_str());
            }
            if (pending.empty() && pending_texts.empty())
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    // ── Audio processing — accumulate to MOSHI_OPUS_FRAME, encode, OGG-wrap ─
    void process_audio_input(std::shared_ptr<MoshiCallState>& state,
                              const float* samples, size_t count) {
        state->input_accumulator.insert(state->input_accumulator.end(),
                                        samples, samples + count);

        while (state->input_accumulator.size() >= static_cast<size_t>(MOSHI_OPUS_FRAME)) {
            if (!state->ws_connected.load()) {
                // Drop the frame while not yet connected
                state->input_accumulator.erase(state->input_accumulator.begin(),
                    state->input_accumulator.begin() + MOSHI_OPUS_FRAME);
                continue;
            }

            // Send OGG preamble (OpusHead + OpusTags) once per call
            if (!state->ogg_preamble_sent) {
                auto preamble = build_ogg_opus_preamble(state->ogg_out.serial);
                std::vector<uint8_t> frame;
                frame.push_back(MT_AUDIO);
                frame.insert(frame.end(), preamble.begin(), preamble.end());
                enqueue_ws_frame(state, std::move(frame));
                state->ogg_preamble_sent = true;
            }

            if (state->opus_enc) {
                unsigned char opus_buf[OPUS_MAX_PACKET];
                int opus_len = opus_encode_float(state->opus_enc,
                    state->input_accumulator.data(), MOSHI_OPUS_FRAME,
                    opus_buf, OPUS_MAX_PACKET);

                if (opus_len > 0) {
                    state->ogg_granule += MOSHI_OPUS_FRAME;
                    state->ogg_out.granule = state->ogg_granule;
                    auto page = build_ogg_page(state->ogg_out, opus_buf,
                                               static_cast<size_t>(opus_len));
                    std::vector<uint8_t> frame;
                    frame.push_back(MT_AUDIO);
                    frame.insert(frame.end(), page.begin(), page.end());
                    enqueue_ws_frame(state, std::move(frame));
                }
            }

            state->input_accumulator.erase(state->input_accumulator.begin(),
                state->input_accumulator.begin() + MOSHI_OPUS_FRAME);
        }
    }

    void enqueue_ws_frame(std::shared_ptr<MoshiCallState>& state,
                           std::vector<uint8_t> frame) {
        std::lock_guard<std::mutex> lock(state->ws_outbound_mutex);
        state->ws_outbound_queue.push_back({std::move(frame)});
    }

    // ── WebSocket event handler ─────────────────────────────────────────────
    static void ws_event_handler(struct mg_connection* c, int ev, void* ev_data) {
        auto* state = static_cast<MoshiCallState*>(c->fn_data);
        if (!state) return;

        if (ev == MG_EV_WS_OPEN) {
            state->ws_connected.store(true);

            // Send handshake to the backend: MT=0 + 8 bytes (proto=0, model=0)
            uint8_t hs[9] = {MT_HANDSHAKE, 0,0,0,0, 0,0,0,0};
            mg_ws_send(c, hs, sizeof(hs), WEBSOCKET_OP_BINARY);

        } else if (ev == MG_EV_WS_MSG) {
            auto* msg = static_cast<struct mg_ws_message*>(ev_data);
            if (!msg || msg->data.len < 1) return;

            uint8_t mt = static_cast<uint8_t>(msg->data.buf[0]);
            const uint8_t* payload = reinterpret_cast<const uint8_t*>(msg->data.buf + 1);
            size_t plen = msg->data.len - 1;

            if (mt == MT_AUDIO && plen > 0 && state->opus_dec) {
                // Backend sends OGG/Opus → we need to extract Opus packets from OGG pages
                // and decode them. The moshi Rust server produces exactly one Opus packet
                // per OGG page (960 samples). We parse the OGG page to extract the packet.
                decode_ogg_audio(state, payload, plen);
            } else if (mt == MT_ERROR && plen > 0) {
                std::string err(reinterpret_cast<const char*>(payload), plen);
                (void)err;
            } else if ((mt == MT_REFERENCE || mt == MT_COLORED_REF) && plen > 0) {
                std::string txt(reinterpret_cast<const char*>(payload), plen);
                std::lock_guard<std::mutex> lock(state->text_mutex);
                state->pending_text.push_back(std::move(txt));
            }

        } else if (ev == MG_EV_CLOSE) {
            state->ws_connected.store(false);
            state->ws_conn = nullptr;
        } else if (ev == MG_EV_ERROR) {
            state->ws_connected.store(false);
        }
    }

    // Parse OGG pages from raw bytes and Opus-decode any audio packets found.
    // This minimal parser handles the simple case moshi-backend produces:
    // one OGG page per Opus packet, no packet continuation across pages.
    static void decode_ogg_audio(MoshiCallState* state,
                                  const uint8_t* data, size_t len) {
        size_t pos = 0;
        while (pos + 27 <= len) {
            // Validate capture pattern
            if (data[pos] != 'O' || data[pos+1] != 'g' ||
                data[pos+2] != 'g' || data[pos+3] != 'S') {
                break;
            }

            if (pos + 27 > len) break;

            uint8_t nseg = data[pos + 26];
            if (pos + 27 + nseg > len) break;

            // Build lacing segment table to compute total body size
            size_t body_size = 0;
            for (int i = 0; i < nseg; i++)
                body_size += data[pos + 27 + i];

            size_t header_size = 27 + nseg;
            if (pos + header_size + body_size > len) break;

            const uint8_t* opus_packet = data + pos + header_size;

            // Skip OGG header pages (OpusHead / OpusTags)
            bool is_header = (body_size >= 8 &&
                (memcmp(opus_packet, "OpusHead", 8) == 0 ||
                 memcmp(opus_packet, "OpusTags", 8) == 0));

            if (!is_header && body_size > 0 && state->opus_dec) {
                float pcm[MOSHI_OPUS_FRAME * 2];
                int decoded = opus_decode_float(state->opus_dec, opus_packet,
                    static_cast<int>(body_size), pcm, MOSHI_OPUS_FRAME, 0);
                if (decoded > 0) {
                    std::lock_guard<std::mutex> lock(state->output_mutex);
                    for (int i = 0; i < decoded; i += MOSHI_CHUNK_SAMPLES) {
                        int chunk = std::min(MOSHI_CHUNK_SAMPLES, decoded - i);
                        state->output_chunks.emplace_back(pcm + i, pcm + i + chunk);
                    }
                }
            }

            pos += header_size + body_size;
        }
    }

    // ── WebSocket client thread ─────────────────────────────────────────────
    void start_ws_client(std::shared_ptr<MoshiCallState>& state, int port) {
        state->ws_running.store(true);
        mg_mgr_init(&state->mgr);

        state->ws_thread = std::thread([this, state, port]() {
            std::string url = "ws://127.0.0.1:" + std::to_string(port) + "/api/chat";
            auto start = std::chrono::steady_clock::now();

            while (state->ws_running.load() && !state->ws_connected.load()) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start).count();
                if (elapsed > WS_CONNECT_TIMEOUT_MS) {
                    log_fwd_.forward(whispertalk::LogLevel::ERROR, state->call_id,
                        "WebSocket connect timeout after %dms to port %d",
                        WS_CONNECT_TIMEOUT_MS, port);
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
                if (!state->ws_connected.load())
                    std::this_thread::sleep_for(std::chrono::milliseconds(WS_CONNECT_RETRY_MS));
            }

            while (state->ws_running.load()) {
                mg_mgr_poll(&state->mgr, 10);

                if (state->ws_connected.load() && state->ws_conn) {
                    std::deque<MoshiCallState::WsFrame> frames;
                    {
                        std::lock_guard<std::mutex> lock(state->ws_outbound_mutex);
                        frames.swap(state->ws_outbound_queue);
                    }
                    for (auto& f : frames)
                        mg_ws_send(state->ws_conn, f.data.data(), f.data.size(),
                                   WEBSOCKET_OP_BINARY);
                }
            }

            mg_mgr_free(&state->mgr);
        });
    }

    // ── Backend process management ──────────────────────────────────────────
    BackendProcess* pick_backend(const std::string& language) {
        {
            auto it = backend_processes_.find(language);
            if (it != backend_processes_.end() && it->second->running.load())
                return it->second.get();
        }
        // Fall back to default language
        auto it = backend_processes_.find(default_language_);
        if (it != backend_processes_.end() && it->second->running.load())
            return it->second.get();
        return nullptr;
    }

    static int read_port_from_config(const std::string& config_path) {
        std::ifstream ifs(config_path);
        if (!ifs.is_open()) return -1;
        std::string content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
        static const std::string needle = "\"port\"";
        bool in_string = false;
        bool escaped = false;
        for (size_t i = 0; i + needle.size() <= content.size(); i++) {
            char ch = content[i];
            if (escaped) { escaped = false; continue; }
            if (ch == '\\' && in_string) { escaped = true; continue; }
            if (ch == '"' && !in_string) {
                if (content.compare(i, needle.size(), needle) == 0) {
                    size_t j = i + needle.size();
                    while (j < content.size() && (content[j] == ' ' || content[j] == '\t' || content[j] == '\n' || content[j] == '\r'))
                        j++;
                    if (j >= content.size() || content[j] != ':') continue;
                    j++;
                    while (j < content.size() && (content[j] == ' ' || content[j] == '\t'))
                        j++;
                    int port = 0;
                    while (j < content.size() && content[j] >= '0' && content[j] <= '9') {
                        port = port * 10 + (content[j] - '0');
                        j++;
                    }
                    if (port > 0) return port;
                }
                in_string = true;
                continue;
            }
            if (ch == '"' && in_string) { in_string = false; continue; }
        }
        return -1;
    }

    bool spawn_backend_process(std::shared_ptr<BackendProcess>& bp) {
        int port = read_port_from_config(bp->config.config_path);
        if (port <= 0) {
            std::cerr << "Failed to read port from config '" << bp->config.config_path
                      << "' for language '" << bp->config.language << "'" << std::endl;
            return false;
        }
        bp->port = port;

        const std::string& binary = bp->config.binary_path.empty()
            ? "moshi-backend" : bp->config.binary_path;

        std::vector<std::string> arg_strings;
        arg_strings.push_back(binary);
        arg_strings.push_back("--config");
        arg_strings.push_back(bp->config.config_path);
        arg_strings.push_back("standalone");

        std::vector<char*> argv;
        for (auto& s : arg_strings) argv.push_back(&s[0]);
        argv.push_back(nullptr);

        posix_spawn_file_actions_t actions;
        posix_spawn_file_actions_init(&actions);
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);

        pid_t pid;
        int ret = posix_spawn(&pid, binary.c_str(), &actions, nullptr,
                              argv.data(), environ);
        posix_spawn_file_actions_destroy(&actions);

        if (ret != 0) {
            std::cerr << "Failed to spawn backend for language '" << bp->config.language
                      << "': " << strerror(ret) << std::endl;
            return false;
        }

        bp->pid = pid;
        bp->running.store(true);

        std::cout << "Spawned moshi backend: lang=" << bp->config.language
                  << " pid=" << pid << " port=" << port << std::endl;

        std::this_thread::sleep_for(std::chrono::milliseconds(BACKEND_STARTUP_WAIT_MS));
        return true;
    }

    void stop_backend_process(std::shared_ptr<BackendProcess>& bp) {
        if (!bp->running.load() || bp->pid <= 0) return;
        bp->running.store(false);
        kill(bp->pid, SIGTERM);
        auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(SUBPROCESS_KILL_WAIT);
        int status;
        while (std::chrono::steady_clock::now() < deadline) {
            if (waitpid(bp->pid, &status, WNOHANG) != 0) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        kill(bp->pid, SIGKILL);
        waitpid(bp->pid, &status, 0);
    }

    // ── Call lifecycle ──────────────────────────────────────────────────────
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
        // Send WS close frame
        if (state->ws_connected.load()) {
            // MT=3 (Control) + EndTurn (B=1)
            uint8_t ctrl[2] = {MT_CONTROL, 1};
            {
                std::vector<uint8_t> f(ctrl, ctrl + 2);
                std::lock_guard<std::mutex> lock(state->ws_outbound_mutex);
                state->ws_outbound_queue.push_back({std::move(f)});
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }

        state->ws_running.store(false);
        if (state->ws_thread.joinable())
            state->ws_thread.join();

        // Opus codecs cleaned up in destructor
    }

    // ── RAG HTTP fetch (tomedo-crawl) ───────────────────────────────────────
    static std::string rag_http_get_static(const std::string& path,
                                            const struct sockaddr_storage& addr,
                                            socklen_t addrlen,
                                            SSL_CTX* ssl_ctx) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return "";
        int flags = fcntl(sock, F_GETFL, 0);
        if (flags == -1) flags = 0;
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);
        connect(sock, (const struct sockaddr*)&addr, addrlen);
        struct pollfd pfd{sock, POLLOUT, 0};
        if (poll(&pfd, 1, RAG_TIMEOUT_MS) <= 0 || !(pfd.revents & POLLOUT)) {
            ::close(sock); return "";
        }
        int conn_err = 0; socklen_t clen = sizeof(conn_err);
        getsockopt(sock, SOL_SOCKET, SO_ERROR, &conn_err, &clen);
        if (conn_err != 0) { ::close(sock); return ""; }
        fcntl(sock, F_SETFL, flags);

        if (!ssl_ctx) { ::close(sock); return ""; }
        SSL* ssl = SSL_new(ssl_ctx);
        if (!ssl) { ::close(sock); return ""; }
        SSL_set_fd(ssl, sock);
        SSL_set_tlsext_host_name(ssl, "127.0.0.1");
        if (SSL_connect(ssl) != 1) { SSL_free(ssl); ::close(sock); return ""; }

        std::string req = "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:13181\r\nConnection: close\r\n\r\n";
        if (SSL_write(ssl, req.c_str(), static_cast<int>(req.size())) <= 0) {
            SSL_shutdown(ssl); SSL_free(ssl); ::close(sock); return "";
        }
        std::string response;
        char buf[4096];
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(RAG_TIMEOUT_MS);
        while (response.size() < RAG_MAX_RESPONSE) {
            auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (rem <= 0) break;
            struct pollfd rpfd{sock, POLLIN, 0};
            if (poll(&rpfd, 1, (int)rem) <= 0 || !(rpfd.revents & POLLIN)) break;
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

    // ── CMD listener ────────────────────────────────────────────────────────
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
            ::close(sock); return;
        }
        if (listen(sock, 4) < 0) {
            ::close(sock); return;
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
                while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r'))
                    cmd.pop_back();
                std::string response = handle_command(cmd);
                send(csock, response.c_str(), response.size(), 0);
            }
            ::close(csock);
        }
        ::close(sock);
    }

    std::string handle_command(const std::string& cmd) {
        if (cmd == "PING") return "PONG\n";
        if (cmd.rfind("SET_LOG_LEVEL:", 0) == 0) {
            log_fwd_.set_level(cmd.substr(14).c_str());
            return "OK\n";
        }
        if (cmd == "STATUS") {
            size_t active;
            {
                std::lock_guard<std::mutex> lock(calls_mutex_);
                active = calls_.size();
            }
            std::string backends_info;
            for (auto& [lang, bp] : backend_processes_) {
                backends_info += lang + ":" + (bp->running.load() ? "up" : "down")
                    + "@" + std::to_string(bp->port) + ";";
            }
            return "ACTIVE_CALLS:" + std::to_string(active)
                + ":UPSTREAM:" + (interconnect_.upstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected")
                + ":DOWNSTREAM:" + (interconnect_.downstream_state() == whispertalk::ConnectionState::CONNECTED ? "connected" : "disconnected")
                + ":BACKENDS:" + backends_info
                + "\n";
        }
        return "ERROR:Unknown command\n";
    }

    // ── Members ─────────────────────────────────────────────────────────────
    std::atomic<bool> running_;
    std::atomic<int> cmd_sock_{-1};
    std::string default_language_;

    whispertalk::InterconnectNode interconnect_;
    whispertalk::LogForwarder log_fwd_;

    std::mutex calls_mutex_;
    std::map<uint32_t, std::shared_ptr<MoshiCallState>> calls_;

    // Language → backend process
    std::map<std::string, std::shared_ptr<BackendProcess>> backend_processes_;

    struct sockaddr_storage rag_addr_{};
    socklen_t rag_addrlen_{0};
    bool rag_addr_resolved_{false};
    SSL_CTX* rag_ssl_ctx_{nullptr};
};

// ─── main ────────────────────────────────────────────────────────────────────
int main(int argc, char** argv) {
    signal(SIGINT,  sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    std::string log_level       = "INFO";
    std::string default_language= "en";
    std::string default_binary  = "moshi-backend";

    // Backend configs: --backend-config <lang>:<config_path>[:<binary>]
    std::vector<BackendConfig> backends;

    static struct option long_opts[] = {
        {"log-level",        required_argument, 0, 'L'},
        {"backend-config",   required_argument, 0, 'B'},
        {"default-language", required_argument, 0, 'd'},
        {"backend-binary",   required_argument, 0, 'b'},
        {0, 0, 0, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "L:B:d:b:", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'L': log_level = optarg; break;
            case 'd': default_language = optarg; break;
            case 'b': default_binary = optarg; break;
            case 'B': {
                // Format: <lang>:<config_path>  or  <lang>:<config_path>:<binary>
                std::string spec(optarg);
                auto p1 = spec.find(':');
                if (p1 == std::string::npos) {
                    std::cerr << "Invalid --backend-config: expected lang:path" << std::endl;
                    return 1;
                }
                BackendConfig bc;
                bc.language    = spec.substr(0, p1);
                auto rest      = spec.substr(p1 + 1);
                auto p2        = rest.find(':');
                if (p2 != std::string::npos) {
                    bc.config_path = rest.substr(0, p2);
                    bc.binary_path = rest.substr(p2 + 1);
                } else {
                    bc.config_path = rest;
                }
                backends.push_back(bc);
                break;
            }
            default: break;
        }
    }

    // If no backends configured, show usage hint
    if (backends.empty()) {
        std::cerr << "No backends configured. Use --backend-config <lang>:<config.json>" << std::endl;
        std::cerr << "Example:" << std::endl;
        std::cerr << "  moshi-service \\" << std::endl;
        std::cerr << "    --backend-config en:bin/models/moshi-en-backend-config.json \\" << std::endl;
        std::cerr << "    --backend-config de:bin/models/moshi-de-backend-config.json \\" << std::endl;
        std::cerr << "    --default-language en" << std::endl;
        return 1;
    }

    // Fill in default binary for any backend that has none specified
    for (auto& bc : backends) {
        if (bc.binary_path.empty())
            bc.binary_path = default_binary;
    }

    std::cout << "Moshi service starting:" << std::endl;
    std::cout << "  Default language: " << default_language << std::endl;
    for (auto& bc : backends)
        std::cout << "  Backend [" << bc.language << "]: " << bc.config_path
                  << " (" << bc.binary_path << ")" << std::endl;

    try {
        MoshiService service(backends, default_language);
        if (!service.init()) return 1;
        service.set_log_level(log_level.c_str());
        service.run();
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
