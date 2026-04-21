// tts-engine-client.h — TTS engine side of the generic TTS dock protocol.
//
// A TTS engine process (kokoro, neutts, future ones) owns one
// `EngineClient` instance instead of an `InterconnectNode`. The client
// opens a local TCP connection to the TTS stage's engine-dock port
// (`service_engine_port(TTS_SERVICE)` == 13143 on 127.0.0.1), performs a
// one-line JSON HELLO handshake, and then exchanges tag-prefixed frames
// with the dock:
//
//     0x01  = serialized `Packet` (audio payload engine→dock, or
//             text payload dock→engine; the `Packet` structure itself is
//             identical in both directions).
//     0x02  = management frame: 1 byte `MgmtMsgType` followed by the
//             same per-type payload used by `InterconnectNode`
//             (CALL_END/SPEECH_ACTIVE/SPEECH_IDLE carry a 4-byte call_id;
//             CUSTOM carries 2-byte length + UTF-8 payload).
//
// Concurrency:
//   - One background thread owns the socket, runs the connect/HELLO
//     state machine, and reads frames.
//   - Text `Packet`s are pushed onto a bounded SPSC queue. `recv_text`
//     is a blocking pop with timeout called from the engine's main loop.
//   - Management frames fire user-registered handlers inline on the
//     receive thread (same convention as `InterconnectNode`).
//   - `send_audio` is safe to call from any thread; sends are serialised
//     behind a mutex.
//
// Latency notes:
//   - Socket uses TCP_NODELAY and SO_NOSIGPIPE (where available).
//   - No TLS on this hop — the engine-dock channel is loopback-only and
//     the dock binds to 127.0.0.1.
//   - Audio `Packet`s flow as one tag byte + 8-byte header + payload,
//     written with two `send` calls gathered via `iovec` where possible.

#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "interconnect.h"

namespace whispertalk {

// Framing tags on the engine-dock channel. Must match tts-service.cpp.
enum class EngineFrameTag : uint8_t {
    PACKET = 0x01,
    MGMT   = 0x02,
};

// Negotiated audio format sent in the HELLO line. Kept as a POD so both
// sides of the link can validate identically. Values are hard contracts
// with the OAP stage, not magic numbers.
struct EngineAudioFormat {
    uint32_t sample_rate = 24000;      // Hz
    uint16_t channels    = 1;
    std::string format   = "f32le";    // little-endian float32 (owned)
};

class EngineClient {
public:
    static constexpr int RECONNECT_INTERVAL_MS = 200;     // matches InterconnectNode::downstream_connect_loop
    static constexpr int CONNECT_TIMEOUT_MS    = 2000;
    static constexpr int HELLO_TIMEOUT_MS      = 2000;
    static constexpr int RECV_POLL_TIMEOUT_MS  = 200;
    static constexpr int SEND_TIMEOUT_MS       = 200;
    static constexpr size_t MAX_HELLO_LINE     = 1024;
    static constexpr size_t MAX_TEXT_QUEUE     = 32;      // bounded backpressure
    static constexpr size_t MAX_PACKET_HEADER  = 8;       // Packet::serialized prefix

    EngineClient() = default;
    ~EngineClient() { shutdown(); }

    EngineClient(const EngineClient&) = delete;
    EngineClient& operator=(const EngineClient&) = delete;

    // Engine identity sent in HELLO. Accepted characters: [A-Za-z0-9_-],
    // up to 32 bytes. Must be set before start().
    void set_name(const std::string& name) { name_ = name; }
    const std::string& name() const { return name_; }

    // Optional override of the dock endpoint. Defaults to 127.0.0.1:13143.
    void set_endpoint(const std::string& host, uint16_t port) {
        host_ = host;
        port_ = port;
    }

    void set_audio_format(const EngineAudioFormat& fmt) { fmt_ = fmt; }

    // Handlers are invoked on the receive thread. Register before start().
    void register_call_end_handler(std::function<void(uint32_t)> h) {
        call_end_handler_ = std::move(h);
    }
    void register_speech_signal_handler(std::function<void(uint32_t, bool)> h) {
        speech_signal_handler_ = std::move(h);
    }
    // CUSTOM-mgmt handlers, keyed by exact payload (e.g. "SHUTDOWN",
    // "FLUSH_TTS"). The dock uses CUSTOM to carry engine-specific
    // commands that must bypass the sidetone guard.
    void register_custom_handler(const std::string& key,
                                 std::function<void()> h) {
        std::lock_guard<std::mutex> lock(custom_mutex_);
        custom_handlers_[key] = std::move(h);
    }

    // Start the background connect/recv thread. Returns false if the
    // engine has no name set.
    bool start() {
        if (name_.empty()) {
            std::fprintf(stderr, "[EngineClient] start() without name\n");
            return false;
        }
        if (host_.empty()) host_ = "127.0.0.1";
        if (port_ == 0) port_ = service_engine_port(ServiceType::TTS_SERVICE);
        if (port_ == 0) {
            std::fprintf(stderr, "[EngineClient] no engine-dock port configured\n");
            return false;
        }
        running_.store(true);
        worker_ = std::thread(&EngineClient::run_loop, this);
        return true;
    }

    void shutdown() {
        if (!running_.exchange(false)) return;
        close_socket();
        {
            std::lock_guard<std::mutex> lock(queue_mutex_);
            text_queue_.clear();
            queue_cv_.notify_all();
        }
        if (worker_.joinable()) worker_.join();
    }

    bool is_connected() const { return connected_.load(std::memory_order_relaxed); }

    // Blocking pop of the next text `Packet` received from the dock.
    // Returns false on timeout or shutdown.
    bool recv_text(Packet& out, int timeout_ms = 100) {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        if (!queue_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms),
                                [this] { return !text_queue_.empty() || !running_.load(); })) {
            return false;
        }
        if (!running_.load() && text_queue_.empty()) return false;
        if (text_queue_.empty()) return false;
        out = std::move(text_queue_.front());
        text_queue_.pop_front();
        return true;
    }

    // Send an audio `Packet` to the dock (tag 0x01).
    bool send_audio(const Packet& pkt) {
        int sock = socket_.load(std::memory_order_acquire);
        if (sock < 0 || !connected_.load(std::memory_order_acquire)) return false;
        std::lock_guard<std::mutex> lock(send_mutex_);
        // Re-check after grabbing the lock; socket might have been reset.
        sock = socket_.load(std::memory_order_acquire);
        if (sock < 0) return false;
        return send_packet_frame(sock, pkt);
    }

    // Send a CALL_END mgmt frame to the dock. Rarely used by engines
    // (the dock typically originates signals). Provided for symmetry.
    bool send_call_end(uint32_t call_id) {
        return send_mgmt_call_id(MgmtMsgType::CALL_END, call_id);
    }

private:
    // --- configuration ---
    std::string host_;
    uint16_t port_ = 0;
    std::string name_;
    EngineAudioFormat fmt_{};

    // --- runtime state ---
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<int> socket_{-1};
    std::thread worker_;

    mutable std::mutex send_mutex_;

    mutable std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<Packet> text_queue_;

    std::function<void(uint32_t)> call_end_handler_;
    std::function<void(uint32_t, bool)> speech_signal_handler_;

    mutable std::mutex custom_mutex_;
    std::map<std::string, std::function<void()>> custom_handlers_;

    // --- main loop ---
    void run_loop() {
        while (running_.load()) {
            int sock = connect_and_hello();
            if (sock < 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_INTERVAL_MS));
                continue;
            }
            socket_.store(sock, std::memory_order_release);
            connected_.store(true, std::memory_order_release);
            std::fprintf(stderr, "[EngineClient:%s] docked at %s:%u\n",
                         name_.c_str(), host_.c_str(), (unsigned)port_);

            recv_loop(sock);

            connected_.store(false, std::memory_order_release);
            // Exclusive close: shutdown() may have already exchanged the fd
            // out and closed it to unblock recv_loop. Only close if we still
            // own the descriptor, otherwise the OS could have recycled the
            // fd number for another open resource.
            int owned = socket_.exchange(-1, std::memory_order_acq_rel);
            if (owned >= 0) {
                ::shutdown(owned, SHUT_RDWR);
                ::close(owned);
            }

            if (running_.load()) {
                std::fprintf(stderr, "[EngineClient:%s] lost connection, reconnecting in %d ms\n",
                             name_.c_str(), RECONNECT_INTERVAL_MS);
                std::this_thread::sleep_for(std::chrono::milliseconds(RECONNECT_INTERVAL_MS));
            }
        }
    }

    int connect_and_hello() {
        int sock = tcp_connect(host_.c_str(), port_, CONNECT_TIMEOUT_MS);
        if (sock < 0) return -1;

        configure_socket(sock);

        // Build HELLO JSON (one line, trailing \n).
        char hello[256];
        int n = std::snprintf(hello, sizeof(hello),
                              "{\"name\":\"%s\",\"sample_rate\":%u,\"channels\":%u,\"format\":\"%s\"}\n",
                              name_.c_str(),
                              (unsigned)fmt_.sample_rate,
                              (unsigned)fmt_.channels,
                              fmt_.format.empty() ? "f32le" : fmt_.format.c_str());
        if (n <= 0 || (size_t)n >= sizeof(hello)) {
            ::close(sock);
            return -1;
        }
        if (!send_all(sock, hello, (size_t)n, HELLO_TIMEOUT_MS)) {
            ::close(sock);
            return -1;
        }

        // Read reply line (OK\n or ERR <reason>\n). Bounded read.
        std::string line;
        if (!recv_line(sock, line, MAX_HELLO_LINE, HELLO_TIMEOUT_MS)) {
            ::close(sock);
            return -1;
        }
        if (line == "OK") return sock;

        std::fprintf(stderr, "[EngineClient:%s] HELLO rejected: %s\n",
                     name_.c_str(), line.c_str());
        ::close(sock);
        return -1;
    }

    void recv_loop(int sock) {
        while (running_.load()) {
            uint8_t tag;
            if (!recv_exact(sock, &tag, 1, RECV_POLL_TIMEOUT_MS)) {
                if (!running_.load()) return;
                if (errno == 0) continue;  // idle timeout: keep waiting
                return;
            }
            switch (static_cast<EngineFrameTag>(tag)) {
                case EngineFrameTag::PACKET: {
                    Packet pkt;
                    if (!recv_packet_body(sock, pkt)) {
                        return;
                    }
                    enqueue_text(std::move(pkt));
                    break;
                }
                case EngineFrameTag::MGMT: {
                    if (!handle_mgmt_frame(sock)) return;
                    break;
                }
                default:
                    std::fprintf(stderr, "[EngineClient:%s] unknown frame tag 0x%02x\n",
                                 name_.c_str(), (unsigned)tag);
                    return;
            }
        }
    }

    void enqueue_text(Packet pkt) {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (text_queue_.size() >= MAX_TEXT_QUEUE) {
            text_queue_.pop_front();  // drop oldest; bounded backpressure
            std::fprintf(stderr, "[EngineClient:%s] text queue full, dropping oldest\n",
                         name_.c_str());
        }
        text_queue_.push_back(std::move(pkt));
        queue_cv_.notify_one();
    }

    bool recv_packet_body(int sock, Packet& pkt) {
        uint8_t hdr[8];
        if (!recv_exact(sock, hdr, sizeof(hdr), RECV_POLL_TIMEOUT_MS * 5)) return false;
        uint32_t net_cid, net_size;
        std::memcpy(&net_cid, hdr, 4);
        std::memcpy(&net_size, hdr + 4, 4);
        uint32_t call_id = ntohl(net_cid);
        uint32_t size    = ntohl(net_size);
        if (call_id == 0) return false;
        if (size > Packet::MAX_PAYLOAD_SIZE) return false;
        pkt.call_id = call_id;
        pkt.payload_size = size;
        pkt.payload.resize(size);
        if (size > 0) {
            if (!recv_exact(sock, pkt.payload.data(), size, RECV_POLL_TIMEOUT_MS * 10)) return false;
        }
        return pkt.is_valid();
    }

    bool handle_mgmt_frame(int sock) {
        uint8_t type;
        if (!recv_exact(sock, &type, 1, RECV_POLL_TIMEOUT_MS * 5)) return false;
        MgmtMsgType mt = static_cast<MgmtMsgType>(type);
        switch (mt) {
            case MgmtMsgType::CALL_END:
            case MgmtMsgType::SPEECH_ACTIVE:
            case MgmtMsgType::SPEECH_IDLE: {
                uint8_t cid_buf[4];
                if (!recv_exact(sock, cid_buf, 4, RECV_POLL_TIMEOUT_MS * 5)) return false;
                uint32_t net_cid;
                std::memcpy(&net_cid, cid_buf, 4);
                uint32_t call_id = ntohl(net_cid);
                if (mt == MgmtMsgType::CALL_END) {
                    if (call_end_handler_) call_end_handler_(call_id);
                } else if (speech_signal_handler_) {
                    speech_signal_handler_(call_id, mt == MgmtMsgType::SPEECH_ACTIVE);
                }
                return true;
            }
            case MgmtMsgType::PING: {
                // Respond immediately with PONG. Dock's keepalive logic.
                uint8_t frame[2] = {
                    static_cast<uint8_t>(EngineFrameTag::MGMT),
                    static_cast<uint8_t>(MgmtMsgType::PONG)
                };
                std::lock_guard<std::mutex> lock(send_mutex_);
                return send_all(sock, frame, sizeof(frame), SEND_TIMEOUT_MS);
            }
            case MgmtMsgType::PONG:
                return true;
            case MgmtMsgType::CUSTOM: {
                uint8_t len_buf[2];
                if (!recv_exact(sock, len_buf, 2, RECV_POLL_TIMEOUT_MS * 5)) return false;
                uint16_t net_len;
                std::memcpy(&net_len, len_buf, 2);
                uint16_t len = ntohs(net_len);
                if (len == 0) return true;  // empty payload: ignore
                std::vector<uint8_t> payload(len);
                if (!recv_exact(sock, payload.data(), len, RECV_POLL_TIMEOUT_MS * 5)) return false;
                std::string key(reinterpret_cast<char*>(payload.data()), len);
                std::function<void()> fn;
                {
                    std::lock_guard<std::mutex> lock(custom_mutex_);
                    auto it = custom_handlers_.find(key);
                    if (it != custom_handlers_.end()) fn = it->second;
                }
                if (fn) fn();
                return true;
            }
            default:
                std::fprintf(stderr, "[EngineClient:%s] unknown mgmt type %u\n",
                             name_.c_str(), (unsigned)type);
                return false;
        }
    }

    bool send_packet_frame(int sock, const Packet& pkt) {
        const size_t body_size = pkt.serialized_size();
        std::vector<uint8_t> body(body_size);
        pkt.serialize_into(body.data());

        uint8_t tag = static_cast<uint8_t>(EngineFrameTag::PACKET);
        iovec iov[2];
        iov[0].iov_base = &tag;
        iov[0].iov_len  = 1;
        iov[1].iov_base = body.data();
        iov[1].iov_len  = body.size();
        return send_iov(sock, iov, 2, SEND_TIMEOUT_MS);
    }

    bool send_mgmt_call_id(MgmtMsgType type, uint32_t call_id) {
        int sock = socket_.load(std::memory_order_acquire);
        if (sock < 0) return false;
        uint8_t buf[6];
        buf[0] = static_cast<uint8_t>(EngineFrameTag::MGMT);
        buf[1] = static_cast<uint8_t>(type);
        uint32_t net_cid = htonl(call_id);
        std::memcpy(buf + 2, &net_cid, 4);
        std::lock_guard<std::mutex> lock(send_mutex_);
        sock = socket_.load(std::memory_order_acquire);
        if (sock < 0) return false;
        return send_all(sock, buf, sizeof(buf), SEND_TIMEOUT_MS);
    }

    void close_socket() {
        int sock = socket_.exchange(-1, std::memory_order_acq_rel);
        if (sock >= 0) {
            ::shutdown(sock, SHUT_RDWR);
            ::close(sock);
        }
        connected_.store(false, std::memory_order_release);
    }

    // --- low-level socket helpers ---
    static int tcp_connect(const char* host, uint16_t port, int timeout_ms) {
        int sock = ::socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return -1;
        int flags = fcntl(sock, F_GETFL, 0);
        fcntl(sock, F_SETFL, flags | O_NONBLOCK);

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = inet_addr(host);
        addr.sin_port = htons(port);

        int ret = ::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        if (ret < 0 && errno != EINPROGRESS) {
            ::close(sock);
            return -1;
        }
        if (ret != 0) {
            pollfd pfd = {sock, POLLOUT, 0};
            int pr = ::poll(&pfd, 1, timeout_ms);
            if (pr <= 0) { ::close(sock); return -1; }
            int err = 0; socklen_t elen = sizeof(err);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &err, &elen);
            if (err != 0) { ::close(sock); return -1; }
        }
        fcntl(sock, F_SETFL, flags);
        return sock;
    }

    static void configure_socket(int sock) {
#ifdef SO_NOSIGPIPE
        int nosig = 1;
        setsockopt(sock, SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
#endif
        int nodelay = 1;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
        int keepalive = 1;
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));
    }

    static bool send_all(int sock, const void* data, size_t len, int timeout_ms) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        size_t sent = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (sent < len) {
            auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remain <= 0) return false;
            pollfd pfd = {sock, POLLOUT, 0};
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

    static bool send_iov(int sock, iovec* iov, int iovcnt, int timeout_ms) {
        // Compute total length; fall through to sendall if writev partial.
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
            pollfd pfd = {sock, POLLOUT, 0};
            int pr = ::poll(&pfd, 1, static_cast<int>(remain));
            if (pr <= 0) return false;
            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) return false;

            // Build adjusted iovec accounting for partial sends.
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

    static bool recv_exact(int sock, void* buf, size_t len, int timeout_ms) {
        uint8_t* p = static_cast<uint8_t*>(buf);
        size_t got = 0;
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (got < len) {
            auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remain <= 0) { errno = 0; return false; }
            pollfd pfd = {sock, POLLIN, 0};
            int pr = ::poll(&pfd, 1, static_cast<int>(remain));
            if (pr == 0) { errno = 0; return false; }
            if (pr < 0) return false;
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

    static bool recv_line(int sock, std::string& out, size_t max_len, int timeout_ms) {
        out.clear();
        auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (out.size() < max_len) {
            auto remain = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now()).count();
            if (remain <= 0) return false;
            pollfd pfd = {sock, POLLIN, 0};
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
            // Security: dock replies are strictly printable ASCII
            // (`OK\n` or `ERR <reason>\n`). Reject any other control
            // byte so we don't propagate garbage to callers.
            if (static_cast<unsigned char>(ch) < 0x20 ||
                static_cast<unsigned char>(ch) == 0x7f) return false;
            out.push_back(ch);
        }
        return false;
    }
};

}  // namespace whispertalk
