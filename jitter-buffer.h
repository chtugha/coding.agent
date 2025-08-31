#pragma once

#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <chrono>
#include <atomic>

// Generic jitter buffer for audio data
template<typename T>
class JitterBuffer {
public:
    JitterBuffer(size_t max_buffer_size = 10, size_t min_buffer_size = 3)
        : max_buffer_size_(max_buffer_size), min_buffer_size_(min_buffer_size),
          running_(true), underrun_count_(0), overrun_count_(0) {}
    
    ~JitterBuffer() {
        stop();
    }
    
    // Add data to buffer (producer)
    bool push(const T& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (buffer_.size() >= max_buffer_size_) {
            // Buffer overrun - drop oldest data
            buffer_.pop();
            overrun_count_++;
        }
        
        buffer_.push(data);
        cv_.notify_one();
        return true;
    }
    
    // Get data from buffer (consumer) - blocks until data available
    bool pop(T& data, int timeout_ms = 1000) {
        std::unique_lock<std::mutex> lock(mutex_);
        
        // Wait for minimum buffer size to reduce jitter
        auto timeout = std::chrono::milliseconds(timeout_ms);
        if (!cv_.wait_for(lock, timeout, [this] { 
            return !running_ || buffer_.size() >= min_buffer_size_; 
        })) {
            return false; // Timeout
        }
        
        if (!running_ || buffer_.empty()) {
            return false;
        }
        
        data = buffer_.front();
        buffer_.pop();
        return true;
    }
    
    // Try to get data without blocking
    bool try_pop(T& data) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        if (buffer_.empty()) {
            underrun_count_++;
            return false;
        }
        
        data = buffer_.front();
        buffer_.pop();
        return true;
    }
    
    // Get current buffer status
    struct BufferStats {
        size_t current_size;
        size_t max_size;
        size_t min_size;
        size_t underrun_count;
        size_t overrun_count;
        bool is_healthy;
    };
    
    BufferStats get_stats() const {
        std::lock_guard<std::mutex> lock(mutex_);
        BufferStats stats;
        stats.current_size = buffer_.size();
        stats.max_size = max_buffer_size_;
        stats.min_size = min_buffer_size_;
        stats.underrun_count = underrun_count_;
        stats.overrun_count = overrun_count_;
        stats.is_healthy = (buffer_.size() >= min_buffer_size_ && buffer_.size() <= max_buffer_size_ * 0.8);
        return stats;
    }
    
    // Clear buffer
    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!buffer_.empty()) {
            buffer_.pop();
        }
    }
    
    // Stop buffer (for shutdown)
    void stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
        cv_.notify_all();
    }
    
    // Check if buffer is running
    bool is_running() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }
    
    // Get current size
    size_t size() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffer_.size();
    }
    
    // Check if buffer is empty
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffer_.empty();
    }

private:
    std::queue<T> buffer_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    
    size_t max_buffer_size_;
    size_t min_buffer_size_;
    std::atomic<bool> running_;
    
    // Statistics
    std::atomic<size_t> underrun_count_;
    std::atomic<size_t> overrun_count_;
};

// Specialized audio jitter buffers
using AudioSampleBuffer = JitterBuffer<std::vector<float>>;
using RTPPacketBuffer = JitterBuffer<std::vector<uint8_t>>;

// RTP packet with timing information
struct TimedRTPPacket {
    std::vector<uint8_t> data;
    uint32_t sequence_number;
    uint32_t timestamp;
    std::chrono::steady_clock::time_point arrival_time;
    
    TimedRTPPacket() : sequence_number(0), timestamp(0), 
                       arrival_time(std::chrono::steady_clock::now()) {}
    
    TimedRTPPacket(const std::vector<uint8_t>& packet_data, uint32_t seq, uint32_t ts)
        : data(packet_data), sequence_number(seq), timestamp(ts),
          arrival_time(std::chrono::steady_clock::now()) {}
};

using TimedRTPBuffer = JitterBuffer<TimedRTPPacket>;

// Audio chunk with session information
struct AudioChunkData {
    std::string session_id;
    std::vector<float> samples;
    std::chrono::steady_clock::time_point timestamp;
    
    AudioChunkData() : timestamp(std::chrono::steady_clock::now()) {}
    
    AudioChunkData(const std::string& sid, const std::vector<float>& audio_samples)
        : session_id(sid), samples(audio_samples), 
          timestamp(std::chrono::steady_clock::now()) {}
};

using AudioChunkBuffer = JitterBuffer<AudioChunkData>;
