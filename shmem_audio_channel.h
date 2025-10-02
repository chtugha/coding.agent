#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include <atomic>
#include <memory>

// POSIX shared memory
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

// Simple single-producer/single-consumer shared memory ring buffer for audio frames
// Frames are arbitrary byte payloads (e.g., RTP payloads or G.711 chunks)
// Channel name should be unique per call, e.g., "/ap_in_34" or "/ap_out_34"

struct ShmAudioHeader {
    uint32_t magic;            // 'APCH' = 0x41504348
    uint32_t version;          // 1
    uint32_t call_id;          // numeric call id
    std::atomic<uint32_t> write_index; // producer increments
    std::atomic<uint32_t> read_index;  // consumer increments
    std::atomic<uint32_t> connected_flags; // bit0=producer alive, bit1=consumer alive
    std::atomic<uint64_t> producer_heartbeat_ns;
    std::atomic<uint64_t> consumer_heartbeat_ns;
    uint32_t slot_size;        // bytes per slot
    uint32_t slot_count;       // number of slots
    uint8_t  reserved[64];
};

class ShmAudioChannel {
public:
    ~ShmAudioChannel() { close(); }

    bool create_or_open(const std::string& name, uint32_t call_id, size_t slot_size = 2048, size_t slot_count = 512, bool create = false) {
        name_ = name;
        size_t header_size = sizeof(ShmAudioHeader);
        size_t data_size = slot_size * slot_count;
        total_size_ = header_size + data_size;

        int flags = create ? (O_CREAT | O_RDWR) : O_RDWR;
        int fd = shm_open(name.c_str(), flags, 0666);
        if (fd < 0) {
            return false;
        }
        fd_ = fd;

        if (create) {
            if (ftruncate(fd_, total_size_) != 0) {
                ::close(fd_); fd_ = -1; return false;
            }
        }

        void* addr = mmap(nullptr, total_size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (addr == MAP_FAILED) {
            ::close(fd_); fd_ = -1; return false;
        }
        base_ = static_cast<uint8_t*>(addr);
        header_ = reinterpret_cast<ShmAudioHeader*>(base_);
        data_ = base_ + sizeof(ShmAudioHeader);

        if (create) {
            header_->magic = 0x41504348u; // 'APCH'
            header_->version = 1;
            header_->call_id = call_id;
            header_->write_index.store(0, std::memory_order_relaxed);
            header_->read_index.store(0, std::memory_order_relaxed);
            header_->connected_flags.store(0, std::memory_order_relaxed);
            header_->producer_heartbeat_ns.store(0, std::memory_order_relaxed);
            header_->consumer_heartbeat_ns.store(0, std::memory_order_relaxed);
            header_->slot_size = static_cast<uint32_t>(slot_size);
            header_->slot_count = static_cast<uint32_t>(slot_count);
        }

        // Sanity check if opening existing
        if (header_->magic != 0x41504348u || header_->slot_size != slot_size || header_->slot_count != slot_count) {
            // Allow differing sizes if opening; but we must adapt
            slot_size_ = header_->slot_size;
            slot_count_ = header_->slot_count;
        } else {
            slot_size_ = static_cast<uint32_t>(slot_size);
            slot_count_ = static_cast<uint32_t>(slot_count);
        }

        return true;
    }

    void set_role_producer(bool on) {
        role_producer_ = on;
        update_connected_flag();
    }
    void set_role_consumer(bool on) {
        role_consumer_ = on;
        update_connected_flag();
    }

    // Non-blocking write; returns false if ring is full
    bool write_frame(const uint8_t* data, size_t size) {
        if (!header_) return false;
        if (size > slot_size_) return false;
        uint32_t w = header_->write_index.load(std::memory_order_acquire);
        uint32_t r = header_->read_index.load(std::memory_order_acquire);
        if (((w + 1) % slot_count_) == r) {
            return false; // full
        }
        size_t offset = (static_cast<size_t>(w) * slot_size_);
        // Store frame length at start of slot (4 bytes), then data
        if (slot_size_ < size + 4) return false;
        uint32_t* len_ptr = reinterpret_cast<uint32_t*>(data_ + offset);
        *len_ptr = static_cast<uint32_t>(size);
        std::memcpy(data_ + offset + 4, data, size);
        header_->write_index.store((w + 1) % slot_count_, std::memory_order_release);
        heartbeat_producer();
        return true;
    }

    // Non-blocking read; returns false if empty
    bool read_frame(std::vector<uint8_t>& out) {
        if (!header_) return false;
        uint32_t w = header_->write_index.load(std::memory_order_acquire);
        uint32_t r = header_->read_index.load(std::memory_order_acquire);
        if (w == r) return false; // empty
        size_t offset = (static_cast<size_t>(r) * slot_size_);
        uint32_t* len_ptr = reinterpret_cast<uint32_t*>(data_ + offset);
        uint32_t len = *len_ptr;
        if (len > slot_size_ - 4) return false;
        out.resize(len);
        std::memcpy(out.data(), data_ + offset + 4, len);
        header_->read_index.store((r + 1) % slot_count_, std::memory_order_release);
        heartbeat_consumer();
        return true;
    }

    bool is_peer_alive(uint64_t now_ns, uint64_t timeout_ns = 5000000000ULL) const {
        if (!header_) return false;
        // If we are producer, check consumer heartbeat; if consumer, check producer
        if (role_producer_) {
            uint64_t hb = header_->consumer_heartbeat_ns.load(std::memory_order_relaxed);
            return (now_ns - hb) < timeout_ns;
        }
        if (role_consumer_) {
            uint64_t hb = header_->producer_heartbeat_ns.load(std::memory_order_relaxed);
            return (now_ns - hb) < timeout_ns;
        }
        // If neither, check both
        uint64_t hp = header_->producer_heartbeat_ns.load(std::memory_order_relaxed);
        uint64_t hc = header_->consumer_heartbeat_ns.load(std::memory_order_relaxed);
        return (now_ns - hp) < timeout_ns || (now_ns - hc) < timeout_ns;
    }

    uint32_t call_id() const { return header_ ? header_->call_id : 0; }
    uint32_t slot_size() const { return slot_size_; }

    void close() {
        if (base_) {
            munmap(base_, total_size_);
            base_ = nullptr;
            header_ = nullptr;
            data_ = nullptr;
        }
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    void update_connected_flag() {
        if (!header_) return;
        uint32_t flags = header_->connected_flags.load(std::memory_order_relaxed);
        if (role_producer_) flags |= 0x1; else flags &= ~0x1u;
        if (role_consumer_) flags |= 0x2; else flags &= ~0x2u;
        header_->connected_flags.store(flags, std::memory_order_relaxed);
    }
    void heartbeat_producer() {
        if (!header_) return;
        header_->producer_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);
    }
    void heartbeat_consumer() {
        if (!header_) return;
        header_->consumer_heartbeat_ns.store(now_ns(), std::memory_order_relaxed);
    }
    static uint64_t now_ns() {
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + static_cast<uint64_t>(ts.tv_nsec);
    }

    std::string name_;
    int fd_ = -1;
    size_t total_size_ = 0;
    uint8_t* base_ = nullptr;
    ShmAudioHeader* header_ = nullptr;
    uint8_t* data_ = nullptr;
    uint32_t slot_size_ = 0;
    uint32_t slot_count_ = 0;
    bool role_producer_ = false;
    bool role_consumer_ = false;
};

