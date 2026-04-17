// tts-service.cpp — generic TTS stage/dock.
//
// Sits between LLAMA_SERVICE (text upstream) and OUTBOUND_AUDIO_PROCESSOR
// (audio downstream) in the Prodigy pipeline. The dock exposes two
// roles:
//
//   1. A normal `InterconnectNode` peer on the pipeline (mgmt/data
//      sockets on 13140/13141) so upstream LLaMA and downstream OAP
//      see the TTS stage like any other service.
//
//   2. An engine-dock listen socket on 127.0.0.1:13143
//      (`service_engine_port(TTS_SERVICE)`). TTS engines (kokoro,
//      neutts, future ones) open a local TCP connection, send a
//      one-line JSON HELLO, and then exchange tag-prefixed frames:
//        0x01 = serialized `Packet` (dock→engine text; engine→dock
//               audio).
//        0x02 = management frame (1-byte `MgmtMsgType` + payload).
//
// Engine-slot model ("last connect wins"):
//   - At most one engine may be active. A new engine that completes
//     HELLO successfully swaps out the previous one: the new slot is
//     installed atomically, CUSTOM SHUTDOWN goes to the outgoing
//     engine, CUSTOM FLUSH_TTS goes downstream to OAP, and the old
//     TCP socket is force-closed after a 2-second grace window.
//   - A malformed HELLO is rejected with `ERR <reason>\n` and never
//     disturbs the currently active engine.
//
// All connections are loopback-only. The engine-dock channel is
// plain TCP (no TLS) because it never leaves 127.0.0.1 and the
// dock binds to `INADDR_LOOPBACK` only. TCP_NODELAY and a 128 KiB
// socket buffer are set on every engine socket.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <getopt.h>

#include "interconnect.h"
#include "tts-common.h"          // shared TTS audio constants
#include "tts-engine-client.h"   // EngineFrameTag, shared protocol constants

using namespace whispertalk;

namespace {

// Protocol constants. No magic numbers on the hot path.
// Audio format: single source of truth is whispertalk::tts::k* in tts-common.h.
using whispertalk::tts::kTTSSampleRate;
using whispertalk::tts::kTTSChannels;
constexpr const char* kTTSFormat        = "f32le";

constexpr int kEngineAcceptPollMs       = 100;
constexpr int kEngineHelloTimeoutMs     = 2000;
constexpr size_t kEngineHelloMaxLine    = 1024;
constexpr size_t kEngineNameMaxLen      = 32;

constexpr int kEngineRecvPollMs         = 100;
constexpr int kEngineHeaderTimeoutMs    = 500;
constexpr int kEnginePayloadTimeoutMs   = 5000;
constexpr int kEngineSendTimeoutMs      = 500;

constexpr int kEnginePingIntervalMs     = 200;
constexpr int kEnginePingMaxMisses      = 3;

constexpr int kEngineSwapGraceMs        = 2000;

constexpr size_t kEngineSocketBufBytes  = 128 * 1024;  // ≥ 2 audio frames
// Application-level cap on CUSTOM mgmt payload (the wire field is 16-bit
// unsigned; this is a tighter semantic limit so the runtime check is
// meaningful and doesn't blow memory on a bogus frame).
constexpr uint16_t kCustomMgmtMaxLen    = 4096;
static_assert(kCustomMgmtMaxLen <= 0xFFFF, "CUSTOM length field is uint16_t");

constexpr int kCmdAcceptPollMs          = 200;
constexpr int kCmdRecvTimeoutMs         = 10 * 1000;
constexpr int kCmdListenBacklog         = 4;
constexpr int kEngineListenBacklog      = 4;

constexpr int kDropLogRateLimitMs       = 1000;

// Upstream recv poll (main loop) / backoff when upstream is FAILED.
constexpr int kUpstreamRecvPollMs       = 100;
constexpr int kUpstreamFailedBackoffMs  = 200;
// Timeout for the FLUSH_TTS CUSTOM mgmt request/response to OAP.
constexpr int kFlushTtsReplyTimeoutMs   = 200;

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool set_socket_options(int sock) {
    int nodelay = 1;
    if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay)) < 0) return false;
    int keepalive = 1;
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
#ifdef SO_NOSIGPIPE
    int nosig = 1;
    setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif
    int bufsz = static_cast<int>(kEngineSocketBufBytes);
    setsockopt(sock, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
    return true;
}

bool recv_exact(int sock, void* buf, size_t len, int timeout_ms) {
    uint8_t* p = static_cast<uint8_t*>(buf);
    size_t got = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (got < len) {
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remain <= 0) return false;
        pollfd pfd{sock, POLLIN, 0};
        int pr = ::poll(&pfd, 1, static_cast<int>(remain));
        if (pr <= 0) return false;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
        ssize_t n = ::recv(sock, p + got, len - got, 0);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return false;
        }
        got += static_cast<size_t>(n);
    }
    return true;
}

bool recv_line(int sock, std::string& out, size_t max_len, int timeout_ms) {
    out.clear();
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (out.size() < max_len) {
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remain <= 0) return false;
        pollfd pfd{sock, POLLIN, 0};
        int pr = ::poll(&pfd, 1, static_cast<int>(remain));
        if (pr <= 0) return false;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
        char ch;
        ssize_t n = ::recv(sock, &ch, 1, 0);
        if (n == 0) return false;
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return false;
        }
        if (ch == '\n') return true;
        if (ch == '\r') continue;
        // Reject control bytes in HELLO line (security: bounded alphabet).
        if (static_cast<unsigned char>(ch) < 0x20 || static_cast<unsigned char>(ch) == 0x7f) return false;
        out.push_back(ch);
    }
    return false;  // line exceeded max_len without newline
}

bool send_all(int sock, const void* data, size_t len, int timeout_ms) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t sent = 0;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (sent < len) {
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remain <= 0) return false;
        pollfd pfd{sock, POLLOUT, 0};
        int pr = ::poll(&pfd, 1, static_cast<int>(remain));
        if (pr <= 0) return false;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
        int flags = 0;
#ifdef MSG_NOSIGNAL
        flags |= MSG_NOSIGNAL;
#endif
        ssize_t n = ::send(sock, p + sent, len - sent, flags);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

bool send_iov(int sock, iovec* iov, int iovcnt, int timeout_ms) {
    size_t total = 0;
    for (int i = 0; i < iovcnt; i++) total += iov[i].iov_len;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    size_t sent = 0;
    int idx = 0;
    size_t off = 0;
    while (sent < total) {
        auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remain <= 0) return false;
        pollfd pfd{sock, POLLOUT, 0};
        int pr = ::poll(&pfd, 1, static_cast<int>(remain));
        if (pr <= 0) return false;
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;
        iovec local[8];
        int lcount = 0;
        if (iovcnt - idx > 8) return false;
        for (int i = idx; i < iovcnt; i++) {
            iovec& dst = local[lcount++];
            if (i == idx) {
                dst.iov_base = static_cast<uint8_t*>(iov[i].iov_base) + off;
                dst.iov_len  = iov[i].iov_len - off;
            } else {
                dst = iov[i];
            }
        }
        ssize_t n = ::writev(sock, local, lcount);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            return false;
        }
        sent += static_cast<size_t>(n);
        size_t adv = static_cast<size_t>(n);
        while (adv > 0 && idx < iovcnt) {
            size_t avail = iov[idx].iov_len - off;
            if (adv >= avail) { adv -= avail; idx++; off = 0; }
            else { off += adv; adv = 0; }
        }
    }
    return true;
}

// Minimal JSON extraction for the HELLO line. The HELLO line is
// machine-generated by trusted local engines, but we still validate
// every field and reject anything outside the strict schema.
bool extract_json_string(const std::string& line, const std::string& key, std::string& out) {
    std::string pattern = "\"" + key + "\"";
    auto k = line.find(pattern);
    if (k == std::string::npos) return false;
    auto colon = line.find(':', k + pattern.size());
    if (colon == std::string::npos) return false;
    auto q1 = line.find('"', colon);
    if (q1 == std::string::npos) return false;
    auto q2 = line.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;
    out = line.substr(q1 + 1, q2 - q1 - 1);
    return true;
}

bool extract_json_uint(const std::string& line, const std::string& key, uint64_t& out) {
    std::string pattern = "\"" + key + "\"";
    auto k = line.find(pattern);
    if (k == std::string::npos) return false;
    auto colon = line.find(':', k + pattern.size());
    if (colon == std::string::npos) return false;
    size_t i = colon + 1;
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i >= line.size() || !std::isdigit(static_cast<unsigned char>(line[i]))) return false;
    uint64_t v = 0;
    while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
        v = v * 10 + static_cast<uint64_t>(line[i] - '0');
        if (v > 0xffffffffULL) return false;  // overflow
        i++;
    }
    out = v;
    return true;
}

bool valid_engine_name(const std::string& name) {
    if (name.empty() || name.size() > kEngineNameMaxLen) return false;
    for (char c : name) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-')) return false;
    }
    return true;
}

}  // namespace

// Per-slot state. Shared between the accept thread (producer) and the
// recv/ping threads (consumers). Held via shared_ptr so the swap
// watcher can safely outlive the recv/ping threads.
struct EngineSlot {
    int fd = -1;
    std::string name;
    uint64_t generation = 0;
    std::atomic<bool> alive{true};             // recv loop still running
    std::atomic<int64_t> last_pong_ms{0};
    std::atomic<int> consecutive_missed_pings{0};
    std::thread recv_thread;
    std::thread ping_thread;
    std::mutex send_mutex;                     // serialize outbound writes on fd
};

class TTSDock {
public:
    TTSDock() : node_(ServiceType::TTS_SERVICE) {}

    bool initialize() {
        if (!node_.initialize()) {
            std::fprintf(stderr, "[TTS] Failed to initialize interconnect node\n");
            return false;
        }

        // Tee handlers: when the inherited mgmt_recv_loop receives
        // CALL_END/SPEECH_ACTIVE/SPEECH_IDLE from LLaMA, it auto-
        // forwards to OAP. We additionally tee to the active engine.
        node_.register_call_end_handler([this](uint32_t cid) {
            tee_call_end_to_engine(cid);
        });
        node_.register_speech_signal_handler([this](uint32_t cid, bool active) {
            tee_speech_to_engine(cid, active);
        });

        // Engine-dock listen socket on 127.0.0.1 only.
        uint16_t engine_port = service_engine_port(ServiceType::TTS_SERVICE);
        if (engine_port == 0) {
            std::fprintf(stderr, "[TTS] engine-dock port not configured\n");
            return false;
        }
        engine_listen_sock_ = create_loopback_listen_socket(engine_port, kEngineListenBacklog);
        if (engine_listen_sock_ < 0) {
            std::fprintf(stderr, "[TTS] Failed to bind engine-dock port %u\n",
                         static_cast<unsigned>(engine_port));
            return false;
        }

        log_fwd_.init(FRONTEND_LOG_PORT, ServiceType::TTS_SERVICE);
        std::fprintf(stderr, "[TTS] engine-dock listening on 127.0.0.1:%u\n",
                     static_cast<unsigned>(engine_port));
        std::fprintf(stderr, "[TTS] no engine connected\n");

        return true;
    }

    void run() {
        running_.store(true);

        if (!node_.connect_to_downstream()) {
            std::fprintf(stderr, "[TTS] OAP downstream not yet available, auto-reconnecting\n");
        }

        engine_accept_thread_ = std::thread(&TTSDock::engine_accept_loop, this);
        cmd_thread_ = std::thread(&TTSDock::command_listener_loop, this);

        // Main loop: forward upstream (LLaMA) text Packets to the
        // active engine. This thread is also the single producer on
        // the dock→engine text path.
        while (running_.load()) {
            Packet pkt;
            if (!node_.recv_from_upstream(pkt, kUpstreamRecvPollMs)) {
                if (node_.upstream_state() == ConnectionState::FAILED) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(kUpstreamFailedBackoffMs));
                }
                continue;
            }
            pkt.trace.record(ServiceType::TTS_SERVICE, 1);  // 1 = inbound
            forward_text_to_engine(pkt);
        }

        shutdown();
    }

    void shutdown() {
        if (!running_.exchange(false)) return;

        // Close engine listen socket so accept loop exits.
        int el = engine_listen_sock_.exchange(-1);
        if (el >= 0) {
            ::shutdown(el, SHUT_RDWR);
            ::close(el);
        }

        // Close cmd listen socket so cmd loop exits.
        int cs = cmd_listen_sock_.exchange(-1);
        if (cs >= 0) {
            ::shutdown(cs, SHUT_RDWR);
            ::close(cs);
        }

        if (engine_accept_thread_.joinable()) engine_accept_thread_.join();
        if (cmd_thread_.joinable()) cmd_thread_.join();

        // Tear down any active slot.
        std::shared_ptr<EngineSlot> slot;
        {
            std::lock_guard<std::mutex> lock(slot_mutex_);
            slot = active_slot_;
            active_slot_.reset();
        }
        if (slot) retire_slot(slot, /*send_flush=*/false);

        // Join any swap-watcher threads spawned by install_new_slot so
        // they cannot outlive TTSDock and reference freed members.
        std::vector<std::thread> watchers;
        {
            std::lock_guard<std::mutex> wlock(watchers_mutex_);
            watchers = std::move(swap_watchers_);
            swap_watchers_.clear();
        }
        for (auto& t : watchers) {
            if (t.joinable()) t.join();
        }

        node_.shutdown();
    }

    void set_log_level(const char* level) { log_fwd_.set_level(level); }

private:
    // ---------- engine accept / HELLO validation ----------

    int create_loopback_listen_socket(uint16_t port, int backlog) {
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return -1;
        int opt = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // security: loopback only
        addr.sin_port = htons(port);
        if (::bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(sock);
            return -1;
        }
        if (::listen(sock, backlog) < 0) {
            ::close(sock);
            return -1;
        }
        return sock;
    }

    void engine_accept_loop() {
        while (running_.load()) {
            int listen_sock = engine_listen_sock_.load();
            if (listen_sock < 0) return;

            pollfd pfd{listen_sock, POLLIN, 0};
            int pr = ::poll(&pfd, 1, kEngineAcceptPollMs);
            if (pr <= 0) continue;
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return;
            if (!(pfd.revents & POLLIN)) continue;

            sockaddr_in addr{};
            socklen_t alen = sizeof(addr);
            int fd = ::accept(listen_sock, reinterpret_cast<sockaddr*>(&addr), &alen);
            if (fd < 0) continue;

            if (!set_socket_options(fd)) {
                ::close(fd);
                continue;
            }

            // Reject anything not originating from loopback. The socket
            // already binds to INADDR_LOOPBACK so this is belt-and-
            // braces, but we re-check to keep the contract explicit.
            if (addr.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) {
                std::fprintf(stderr, "[TTS] Rejecting non-loopback engine connection\n");
                ::shutdown(fd, SHUT_RDWR);
                ::close(fd);
                continue;
            }

            handle_new_engine(fd);
        }
    }

    void handle_new_engine(int fd) {
        std::string line;
        if (!recv_line(fd, line, kEngineHelloMaxLine, kEngineHelloTimeoutMs)) {
            reply_err(fd, "hello_timeout_or_too_long");
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            return;
        }

        std::string name;
        std::string format;
        uint64_t sr = 0, ch = 0;
        if (!extract_json_string(line, "name", name) ||
            !extract_json_uint(line, "sample_rate", sr) ||
            !extract_json_uint(line, "channels", ch) ||
            !extract_json_string(line, "format", format)) {
            reply_err(fd, "hello_missing_field");
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            return;
        }

        if (!valid_engine_name(name)) {
            reply_err(fd, "hello_bad_name");
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            return;
        }
        if (sr != kTTSSampleRate) {
            reply_err(fd, "hello_bad_sample_rate");
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            return;
        }
        if (ch != kTTSChannels) {
            reply_err(fd, "hello_bad_channels");
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            return;
        }
        if (format != kTTSFormat) {
            reply_err(fd, "hello_bad_format");
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            return;
        }

        const char ok_line[] = "OK\n";
        if (!send_all(fd, ok_line, sizeof(ok_line) - 1, kEngineHelloTimeoutMs)) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
            return;
        }

        install_new_slot(fd, name);
    }

    void reply_err(int fd, const char* reason) {
        char buf[128];
        int n = std::snprintf(buf, sizeof(buf), "ERR %s\n", reason);
        if (n > 0) send_all(fd, buf, static_cast<size_t>(n), kEngineHelloTimeoutMs);
    }

    // ---------- slot swap / retire ----------

    void install_new_slot(int fd, const std::string& name) {
        auto new_slot = std::make_shared<EngineSlot>();
        new_slot->fd = fd;
        new_slot->name = name;
        new_slot->generation = next_generation_.fetch_add(1) + 1;
        new_slot->last_pong_ms.store(now_ms());
        new_slot->consecutive_missed_pings.store(0);

        std::shared_ptr<EngineSlot> old_slot;
        {
            std::lock_guard<std::mutex> lock(slot_mutex_);
            old_slot = active_slot_;
            active_slot_ = new_slot;
        }

        // Spawn recv + ping threads for the new slot. They run as long
        // as the slot remains active (generation-matches).
        new_slot->recv_thread = std::thread(&TTSDock::engine_recv_loop, this, new_slot);
        new_slot->ping_thread = std::thread(&TTSDock::engine_ping_loop, this, new_slot);

        if (old_slot) {
            std::fprintf(stderr, "[TTS] engine swapped (%s -> %s)\n",
                         old_slot->name.c_str(), new_slot->name.c_str());
            log_fwd_.forward(LogLevel::INFO, 0, "engine swapped (%s -> %s)",
                             old_slot->name.c_str(), new_slot->name.c_str());

            // Tell the outgoing engine to shut down; give OAP a chance
            // to discard buffered PCM from the old engine.
            send_mgmt_custom(old_slot, "SHUTDOWN");
            send_flush_tts_to_oap();

            // Off-hot-path watcher: wait up to kEngineSwapGraceMs for
            // the old recv thread to exit on its own (TCP close from
            // engine). After the grace window, force the socket down
            // so threads unblock. Close fd only after both threads
            // are joined.
            //
            // Tracked in `swap_watchers_` (not detached) so `shutdown()`
            // joins it before TTSDock is destroyed — otherwise the
            // lambda could outlive `this` and reference dead members.
            std::thread watcher([old_slot]() {
                auto start = std::chrono::steady_clock::now();
                while (old_slot->alive.load() &&
                       std::chrono::steady_clock::now() - start <
                           std::chrono::milliseconds(kEngineSwapGraceMs)) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(25));
                }
                if (old_slot->alive.load()) {
                    ::shutdown(old_slot->fd, SHUT_RDWR);
                }
                if (old_slot->recv_thread.joinable()) old_slot->recv_thread.join();
                if (old_slot->ping_thread.joinable()) old_slot->ping_thread.join();
                ::close(old_slot->fd);
            });
            {
                std::lock_guard<std::mutex> wlock(watchers_mutex_);
                swap_watchers_.push_back(std::move(watcher));
            }
        } else {
            std::fprintf(stderr, "[TTS] engine connected (%s)\n", new_slot->name.c_str());
            log_fwd_.forward(LogLevel::INFO, 0, "engine connected (%s)",
                             new_slot->name.c_str());
        }
    }

    // Called when an active slot's TCP socket closes unexpectedly or
    // when the dock itself is shutting down. Joins the slot's threads
    // and closes the fd.
    void retire_slot(std::shared_ptr<EngineSlot> slot, bool send_flush) {
        // Ensure threads see generation change and exit.
        if (slot->alive.exchange(false)) {
            ::shutdown(slot->fd, SHUT_RDWR);
        }
        if (slot->recv_thread.joinable() && std::this_thread::get_id() != slot->recv_thread.get_id()) {
            slot->recv_thread.join();
        }
        if (slot->ping_thread.joinable() && std::this_thread::get_id() != slot->ping_thread.get_id()) {
            slot->ping_thread.join();
        }
        ::close(slot->fd);

        if (send_flush) send_flush_tts_to_oap();
    }

    void handle_active_disconnect(const std::shared_ptr<EngineSlot>& slot) {
        bool was_active = false;
        {
            std::lock_guard<std::mutex> lock(slot_mutex_);
            if (active_slot_ && active_slot_->generation == slot->generation) {
                active_slot_.reset();
                was_active = true;
            }
        }
        if (was_active) {
            std::fprintf(stderr, "[TTS] engine disconnected (%s)\n", slot->name.c_str());
            std::fprintf(stderr, "[TTS] no engine connected\n");
            log_fwd_.forward(LogLevel::INFO, 0, "engine disconnected (%s)",
                             slot->name.c_str());
            send_flush_tts_to_oap();
        }
    }

    // ---------- engine recv / ping loops ----------

    void engine_recv_loop(std::shared_ptr<EngineSlot> slot) {
        const int fd = slot->fd;
        const uint64_t my_gen = slot->generation;
        while (running_.load() && slot_is_current(my_gen)) {
            pollfd pfd{fd, POLLIN, 0};
            int pr = ::poll(&pfd, 1, kEngineRecvPollMs);
            if (pr <= 0) continue;
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) break;
            if (!(pfd.revents & POLLIN)) continue;

            uint8_t tag = 0;
            ssize_t n = ::recv(fd, &tag, 1, 0);
            if (n == 0) break;  // EOF
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
                break;
            }
            // Drop stale frames the instant we notice we're no longer
            // the active slot. This check is also the fast-path latency
            // guard (spec: drop non-active frames early).
            if (!slot_is_current(my_gen)) break;

            if (tag == static_cast<uint8_t>(EngineFrameTag::PACKET)) {
                if (!recv_and_forward_audio_packet(fd)) break;
            } else if (tag == static_cast<uint8_t>(EngineFrameTag::MGMT)) {
                if (!handle_engine_mgmt(slot)) break;
            } else {
                std::fprintf(stderr, "[TTS] unknown frame tag 0x%02x from engine %s\n",
                             static_cast<unsigned>(tag), slot->name.c_str());
                break;
            }
        }

        slot->alive.store(false);
        handle_active_disconnect(slot);
    }

    bool recv_and_forward_audio_packet(int fd) {
        uint8_t hdr[8];
        if (!recv_exact(fd, hdr, sizeof(hdr), kEngineHeaderTimeoutMs)) return false;
        uint32_t net_cid, net_size;
        std::memcpy(&net_cid, hdr, 4);
        std::memcpy(&net_size, hdr + 4, 4);
        uint32_t size = ntohl(net_size);
        if (size > Packet::MAX_PAYLOAD_SIZE) return false;

        std::vector<uint8_t> full(8 + size);
        std::memcpy(full.data(), hdr, 8);
        if (size > 0) {
            if (!recv_exact(fd, full.data() + 8, size, kEnginePayloadTimeoutMs)) return false;
        }
        Packet pkt;
        if (!Packet::deserialize(full.data(), full.size(), pkt)) return false;
        (void)net_cid;

        pkt.trace.record(ServiceType::TTS_SERVICE, 0);  // 0 = outbound
        node_.send_to_downstream(pkt);
        return true;
    }

    bool handle_engine_mgmt(const std::shared_ptr<EngineSlot>& slot) {
        const int fd = slot->fd;
        uint8_t type = 0;
        if (!recv_exact(fd, &type, 1, kEngineHeaderTimeoutMs)) return false;
        MgmtMsgType mt = static_cast<MgmtMsgType>(type);
        switch (mt) {
            case MgmtMsgType::CALL_END:
            case MgmtMsgType::SPEECH_ACTIVE:
            case MgmtMsgType::SPEECH_IDLE: {
                uint8_t cid_buf[4];
                if (!recv_exact(fd, cid_buf, 4, kEngineHeaderTimeoutMs)) return false;
                // Engines are consumers of these signals, not producers.
                // Silently ignore, but the frame must still be fully
                // read to stay synchronized with the stream.
                return true;
            }
            case MgmtMsgType::PING: {
                uint8_t frame[2] = {
                    static_cast<uint8_t>(EngineFrameTag::MGMT),
                    static_cast<uint8_t>(MgmtMsgType::PONG),
                };
                std::lock_guard<std::mutex> lock(slot->send_mutex);
                return send_all(fd, frame, sizeof(frame), kEngineSendTimeoutMs);
            }
            case MgmtMsgType::PONG: {
                slot->last_pong_ms.store(now_ms());
                slot->consecutive_missed_pings.store(0);
                return true;
            }
            case MgmtMsgType::CUSTOM: {
                uint8_t len_buf[2];
                if (!recv_exact(fd, len_buf, 2, kEngineHeaderTimeoutMs)) return false;
                uint16_t net_len;
                std::memcpy(&net_len, len_buf, 2);
                uint16_t len = ntohs(net_len);
                if (len > kCustomMgmtMaxLen) return false;
                if (len > 0) {
                    std::vector<uint8_t> payload(len);
                    if (!recv_exact(fd, payload.data(), len, kEngineHeaderTimeoutMs)) return false;
                    // Engine-originated CUSTOM is accepted for protocol
                    // completeness but not acted on by the dock today.
                }
                return true;
            }
            default:
                std::fprintf(stderr, "[TTS] unknown mgmt type %u from engine %s\n",
                             static_cast<unsigned>(type), slot->name.c_str());
                return false;
        }
    }

    void engine_ping_loop(std::shared_ptr<EngineSlot> slot) {
        const int fd = slot->fd;
        const uint64_t my_gen = slot->generation;
        while (running_.load() && slot_is_current(my_gen) && slot->alive.load()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(kEnginePingIntervalMs));
            if (!slot_is_current(my_gen) || !slot->alive.load()) break;

            uint8_t frame[2] = {
                static_cast<uint8_t>(EngineFrameTag::MGMT),
                static_cast<uint8_t>(MgmtMsgType::PING),
            };
            {
                std::lock_guard<std::mutex> lock(slot->send_mutex);
                if (!send_all(fd, frame, sizeof(frame), kEngineSendTimeoutMs)) break;
            }

            int64_t since = now_ms() - slot->last_pong_ms.load();
            if (since > kEnginePingIntervalMs * kEnginePingMaxMisses) {
                std::fprintf(stderr, "[TTS] keepalive timeout for engine %s (%lld ms since pong)\n",
                             slot->name.c_str(), static_cast<long long>(since));
                ::shutdown(fd, SHUT_RDWR);
                break;
            }
        }
    }

    // ---------- forwarding (hot path) ----------

    bool slot_is_current(uint64_t gen) const {
        std::shared_ptr<EngineSlot> s;
        {
            std::lock_guard<std::mutex> lock(slot_mutex_);
            s = active_slot_;
        }
        return s && s->generation == gen;
    }

    std::shared_ptr<EngineSlot> current_slot() const {
        std::lock_guard<std::mutex> lock(slot_mutex_);
        return active_slot_;
    }

    void forward_text_to_engine(const Packet& pkt) {
        auto slot = current_slot();
        if (!slot) {
            log_dropped_text(pkt.call_id);
            return;
        }
        uint8_t tag = static_cast<uint8_t>(EngineFrameTag::PACKET);
        auto body = pkt.serialize();
        iovec iov[2];
        iov[0].iov_base = &tag;
        iov[0].iov_len  = 1;
        iov[1].iov_base = body.data();
        iov[1].iov_len  = body.size();
        std::lock_guard<std::mutex> lock(slot->send_mutex);
        if (!send_iov(slot->fd, iov, 2, kEngineSendTimeoutMs)) {
            std::fprintf(stderr, "[TTS] failed to forward text to engine %s\n",
                         slot->name.c_str());
        }
    }

    void tee_call_end_to_engine(uint32_t call_id) {
        auto slot = current_slot();
        if (!slot) return;
        send_mgmt_call_id(slot, MgmtMsgType::CALL_END, call_id);
    }

    void tee_speech_to_engine(uint32_t call_id, bool active) {
        auto slot = current_slot();
        if (!slot) return;
        send_mgmt_call_id(slot,
                          active ? MgmtMsgType::SPEECH_ACTIVE : MgmtMsgType::SPEECH_IDLE,
                          call_id);
    }

    bool send_mgmt_call_id(const std::shared_ptr<EngineSlot>& slot,
                           MgmtMsgType type, uint32_t call_id) {
        uint8_t buf[6];
        buf[0] = static_cast<uint8_t>(EngineFrameTag::MGMT);
        buf[1] = static_cast<uint8_t>(type);
        uint32_t net_cid = htonl(call_id);
        std::memcpy(buf + 2, &net_cid, 4);
        std::lock_guard<std::mutex> lock(slot->send_mutex);
        return send_all(slot->fd, buf, sizeof(buf), kEngineSendTimeoutMs);
    }

    bool send_mgmt_custom(const std::shared_ptr<EngineSlot>& slot,
                          const std::string& payload) {
        if (payload.size() > kCustomMgmtMaxLen) return false;
        std::vector<uint8_t> buf(4 + payload.size());
        buf[0] = static_cast<uint8_t>(EngineFrameTag::MGMT);
        buf[1] = static_cast<uint8_t>(MgmtMsgType::CUSTOM);
        uint16_t net_len = htons(static_cast<uint16_t>(payload.size()));
        std::memcpy(buf.data() + 2, &net_len, 2);
        if (!payload.empty()) {
            std::memcpy(buf.data() + 4, payload.data(), payload.size());
        }
        std::lock_guard<std::mutex> lock(slot->send_mutex);
        return send_all(slot->fd, buf.data(), buf.size(), kEngineSendTimeoutMs);
    }

    void send_flush_tts_to_oap() {
        // InterconnectNode::send_custom_to_downstream is a
        // request/response call. We don't care about the response for
        // a flush; empty string on timeout is fine.
        node_.send_custom_to_downstream("FLUSH_TTS", kFlushTtsReplyTimeoutMs);
    }

    void log_dropped_text(uint32_t call_id) {
        int64_t now = now_ms();
        std::lock_guard<std::mutex> lock(drop_log_mutex_);
        auto it = last_drop_log_ms_.find(call_id);
        if (it != last_drop_log_ms_.end() && (now - it->second) < kDropLogRateLimitMs) return;
        last_drop_log_ms_[call_id] = now;
        std::fprintf(stderr, "[TTS] WARN: dropping text for call %u, no engine docked\n",
                     call_id);
        log_fwd_.forward(LogLevel::WARN, call_id,
                         "dropping text, no engine docked");
    }

    // ---------- command port ----------

    void command_listener_loop() {
        uint16_t port = service_cmd_port(ServiceType::TTS_SERVICE);
        int sock = create_loopback_listen_socket(port, kCmdListenBacklog);
        if (sock < 0) {
            std::fprintf(stderr, "[TTS] cmd: bind port %u failed\n", static_cast<unsigned>(port));
            return;
        }
        cmd_listen_sock_.store(sock);
        std::fprintf(stderr, "[TTS] command listener on port %u\n", static_cast<unsigned>(port));

        while (running_.load()) {
            int listen_sock = cmd_listen_sock_.load();
            if (listen_sock < 0) return;

            pollfd pfd{listen_sock, POLLIN, 0};
            int pr = ::poll(&pfd, 1, kCmdAcceptPollMs);
            if (pr <= 0) continue;
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return;
            if (!(pfd.revents & POLLIN)) continue;

            int csock = ::accept(listen_sock, nullptr, nullptr);
            if (csock < 0) continue;

            struct timeval tv{kCmdRecvTimeoutMs / 1000, (kCmdRecvTimeoutMs % 1000) * 1000};
            setsockopt(csock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            setsockopt(csock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

            char buf[512];
            int n = static_cast<int>(::recv(csock, buf, sizeof(buf) - 1, 0));
            if (n > 0) {
                buf[n] = '\0';
                std::string cmd(buf);
                while (!cmd.empty() && (cmd.back() == '\n' || cmd.back() == '\r')) cmd.pop_back();
                std::string resp = handle_command(cmd);
                ::send(csock, resp.data(), resp.size(), 0);
            }
            ::close(csock);
        }
    }

    std::string handle_command(const std::string& cmd) {
        if (cmd == "PING") return "PONG\n";
        if (cmd == "STATUS") {
            auto slot = current_slot();
            if (!slot) return "NONE\n";
            return std::string("ACTIVE ") + slot->name + "\n";
        }
        if (cmd.rfind("SET_LOG_LEVEL:", 0) == 0) {
            std::string level = cmd.substr(std::strlen("SET_LOG_LEVEL:"));
            log_fwd_.set_level(level.c_str());
            return "OK\n";
        }
        return "ERR unknown_command\n";
    }

    // ---------- members ----------

    InterconnectNode node_;
    LogForwarder log_fwd_;

    std::atomic<bool> running_{false};
    std::atomic<int> engine_listen_sock_{-1};
    std::atomic<int> cmd_listen_sock_{-1};

    std::thread engine_accept_thread_;
    std::thread cmd_thread_;

    mutable std::mutex slot_mutex_;
    std::shared_ptr<EngineSlot> active_slot_;
    std::atomic<uint64_t> next_generation_{0};

    // Background watchers that tear down retired engine slots after a
    // swap. Tracked here (not detached) so `shutdown()` can join them
    // before TTSDock goes out of scope.
    std::mutex watchers_mutex_;
    std::vector<std::thread> swap_watchers_;

    mutable std::mutex drop_log_mutex_;
    std::map<uint32_t, int64_t> last_drop_log_ms_;
};

static TTSDock* g_dock = nullptr;

static void signal_handler(int) {
    if (g_dock) {
        std::fprintf(stderr, "\n[TTS] shutting down\n");
        g_dock->shutdown();
    }
}

int main(int argc, char* argv[]) {
    setlinebuf(stdout);
    setlinebuf(stderr);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGPIPE, SIG_IGN);

    std::string log_level = "INFO";
    static struct option long_opts[] = {
        {"log-level", required_argument, 0, 'L'},
        {"help",      no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };
    int opt;
    while ((opt = getopt_long(argc, argv, "L:h", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'L': log_level = optarg; break;
            case 'h':
                std::printf("Usage: tts-service [--log-level LEVEL]\n");
                return 0;
            default: break;
        }
    }

    std::fprintf(stderr, "[TTS] starting generic TTS stage/dock\n");

    TTSDock dock;
    g_dock = &dock;
    if (!dock.initialize()) return 1;
    dock.set_log_level(log_level.c_str());
    dock.run();
    return 0;
}
