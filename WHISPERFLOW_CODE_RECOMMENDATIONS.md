# WhisperFlow - Specific Code Recommendations

**Date:** 2025-10-13  
**Status:** Analysis Only - No Changes Made  
**Target Files:** `whisper-service.cpp`, `whisper-service.h`

---

## Phase 1: Performance Instrumentation (Week 1-2)

### 1.1 Add Timing Measurements

**Location:** `whisper-service.cpp`, line 103 (in `WhisperSession::process_audio_chunk`)

**Current Code:**
```cpp
bool WhisperSession::process_audio_chunk(const std::vector<float>& audio_samples) {
    // ... existing code ...
    
    // Process audio chunk directly with whisper
    int result = whisper_full(ctx_, wparams, audio_samples.data(), audio_samples.size());
    
    // ... existing code ...
}
```

**Recommended Addition:**
```cpp
bool WhisperSession::process_audio_chunk(const std::vector<float>& audio_samples) {
    // ... existing code ...
    
    // START TIMING
    auto t_start = std::chrono::high_resolution_clock::now();
    
    // Process audio chunk directly with whisper
    int result = whisper_full(ctx_, wparams, audio_samples.data(), audio_samples.size());
    
    // END TIMING
    auto t_end = std::chrono::high_resolution_clock::now();
    auto inference_ms = std::chrono::duration_cast<std::chrono::milliseconds>(t_end - t_start).count();
    
    // Calculate per-word latency (approximate)
    const int n_segments = whisper_full_n_segments(ctx_);
    int word_count = 0;
    for (int i = 0; i < n_segments; ++i) {
        const char* text = whisper_full_get_segment_text(ctx_, i);
        // Simple word count (split by spaces)
        std::string segment(text);
        word_count += std::count(segment.begin(), segment.end(), ' ') + 1;
    }
    
    double per_word_latency_ms = word_count > 0 ? (double)inference_ms / word_count : 0.0;
    
    std::cout << "⏱️ [" << call_id_ << "] Inference: " << inference_ms << "ms, "
              << "Words: " << word_count << ", "
              << "Per-word: " << per_word_latency_ms << "ms" << std::endl;
    
    // Log to database for analysis
    if (database_) {
        database_->log_performance_metric(call_id_, "inference_ms", inference_ms);
        database_->log_performance_metric(call_id_, "per_word_latency_ms", per_word_latency_ms);
    }
    
    // ... existing code ...
}
```

**Benefits:**
- Establishes baseline latency metrics
- Enables data-driven optimization
- Tracks per-word latency (key metric from paper)

**Effort:** 1-2 hours  
**Risk:** Minimal (logging only)

---

### 1.2 Add Mutex Contention Tracking

**Location:** `whisper-service.cpp`, line 116 (in `WhisperSession::process_audio_chunk`)

**Current Code:**
```cpp
// Serialize access to shared whisper context if needed
std::unique_lock<std::mutex> shared_lock;
if (shared_mutex_) {
    shared_lock = std::unique_lock<std::mutex>(*shared_mutex_);
}
```

**Recommended Addition:**
```cpp
// Serialize access to shared whisper context if needed
std::unique_lock<std::mutex> shared_lock;
auto t_mutex_start = std::chrono::high_resolution_clock::now();
if (shared_mutex_) {
    shared_lock = std::unique_lock<std::mutex>(*shared_mutex_);
}
auto t_mutex_acquired = std::chrono::high_resolution_clock::now();
auto mutex_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
    t_mutex_acquired - t_mutex_start).count();

if (mutex_wait_ms > 10) {  // Log if wait > 10ms
    std::cout << "⏳ [" << call_id_ << "] Mutex wait: " << mutex_wait_ms << "ms" << std::endl;
}

// Log to database for analysis
if (database_ && mutex_wait_ms > 0) {
    database_->log_performance_metric(call_id_, "mutex_wait_ms", mutex_wait_ms);
}
```

**Benefits:**
- Identifies mutex contention bottlenecks
- Quantifies multi-call performance impact
- Guides optimization priorities

**Effort:** 1 hour  
**Risk:** Minimal

---

### 1.3 Add CPU/GPU Utilization Tracking

**Location:** `whisper-service.h`, add new member variables

**Recommended Addition:**
```cpp
class StandaloneWhisperService {
private:
    // ... existing members ...
    
    // Performance tracking
    struct PerformanceMetrics {
        std::atomic<uint64_t> total_inference_ms{0};
        std::atomic<uint64_t> total_mutex_wait_ms{0};
        std::atomic<uint64_t> total_chunks_processed{0};
        std::atomic<uint64_t> total_words_transcribed{0};
        std::chrono::steady_clock::time_point start_time;
    };
    PerformanceMetrics metrics_;
    
    // Add method to print stats
    void print_performance_stats() const;
};
```

**Location:** `whisper-service.cpp`, add implementation

**Recommended Addition:**
```cpp
void StandaloneWhisperService::print_performance_stats() const {
    auto now = std::chrono::steady_clock::now();
    auto uptime_s = std::chrono::duration_cast<std::chrono::seconds>(
        now - metrics_.start_time).count();
    
    if (uptime_s == 0) return;
    
    uint64_t total_inference = metrics_.total_inference_ms.load();
    uint64_t total_mutex_wait = metrics_.total_mutex_wait_ms.load();
    uint64_t total_chunks = metrics_.total_chunks_processed.load();
    uint64_t total_words = metrics_.total_words_transcribed.load();
    
    double avg_inference_ms = total_chunks > 0 ? (double)total_inference / total_chunks : 0.0;
    double avg_mutex_wait_ms = total_chunks > 0 ? (double)total_mutex_wait / total_chunks : 0.0;
    double avg_per_word_ms = total_words > 0 ? (double)total_inference / total_words : 0.0;
    double throughput_words_per_sec = (double)total_words / uptime_s;
    
    std::cout << "\n📊 Performance Statistics:" << std::endl;
    std::cout << "   Uptime: " << uptime_s << "s" << std::endl;
    std::cout << "   Chunks processed: " << total_chunks << std::endl;
    std::cout << "   Words transcribed: " << total_words << std::endl;
    std::cout << "   Avg inference: " << avg_inference_ms << "ms/chunk" << std::endl;
    std::cout << "   Avg mutex wait: " << avg_mutex_wait_ms << "ms/chunk" << std::endl;
    std::cout << "   Avg per-word latency: " << avg_per_word_ms << "ms/word" << std::endl;
    std::cout << "   Throughput: " << throughput_words_per_sec << " words/s\n" << std::endl;
}
```

**Benefits:**
- Comprehensive performance overview
- Identifies system-wide bottlenecks
- Tracks throughput over time

**Effort:** 2-3 hours  
**Risk:** Minimal

---

## Phase 2: Streaming Inference (Week 3-4)

### 2.1 Implement Overlapping Windows

**Location:** `whisper-service.cpp`, modify `handle_tcp_audio_stream`

**Current Approach:**
- Processes complete chunks independently
- No overlap or result reuse

**Recommended Approach:**
```cpp
void StandaloneWhisperService::handle_tcp_audio_stream(const std::string& call_id, int socket) {
    // ... existing HELLO handling ...
    
    // Streaming buffer for overlapping windows
    std::vector<float> streaming_buffer;
    const size_t window_size = 5 * 16000;  // 5 seconds at 16kHz
    const size_t overlap_size = 1 * 16000;  // 1 second overlap
    const size_t stride = window_size - overlap_size;  // 4 seconds stride
    
    while (running_.load()) {
        std::vector<float> audio_samples;
        
        if (!read_tcp_audio_chunk(socket, audio_samples)) {
            break;
        }
        
        if (audio_samples.empty()) {
            continue;
        }
        
        // Append to streaming buffer
        streaming_buffer.insert(streaming_buffer.end(), 
                               audio_samples.begin(), 
                               audio_samples.end());
        
        // Process overlapping windows
        while (streaming_buffer.size() >= window_size) {
            // Extract window
            std::vector<float> window(streaming_buffer.begin(), 
                                     streaming_buffer.begin() + window_size);
            
            // Process window with whisper session
            {
                std::lock_guard<std::mutex> lock(sessions_mutex_);
                auto it = sessions_.find(call_id);
                if (it != sessions_.end()) {
                    if (it->second->process_audio_chunk(window)) {
                        // Check for new transcription
                        std::string transcription = it->second->get_latest_transcription();
                        if (!transcription.empty()) {
                            // ... existing transcription handling ...
                        }
                    }
                }
            }
            
            // Slide window by stride (keep overlap)
            streaming_buffer.erase(streaming_buffer.begin(), 
                                  streaming_buffer.begin() + stride);
        }
    }
    
    // Process remaining buffer
    if (!streaming_buffer.empty()) {
        // ... process final chunk ...
    }
    
    // ... existing cleanup ...
}
```

**Benefits:**
- 30-50% latency reduction for first transcription
- Smoother streaming experience
- Better handling of continuous speech

**Effort:** 1-2 days  
**Risk:** Low (can A/B test)

**Trade-offs:**
- Increased memory usage (buffer overhead)
- Slightly more CPU for buffer management
- Need to handle duplicate transcriptions at boundaries

---

### 2.2 Optimize Mutex Usage

**Location:** `whisper-service.cpp`, line 116

**Current Approach:**
- Single mutex protects entire inference
- Held for entire `whisper_full()` call

**Recommended Approach:**

**Option A: Reduce Critical Section (Quick Win)**
```cpp
bool WhisperSession::process_audio_chunk(const std::vector<float>& audio_samples) {
    // ... existing validation ...
    
    // Copy context pointer under lock, then release
    whisper_context* ctx_copy = nullptr;
    {
        std::unique_lock<std::mutex> shared_lock;
        if (shared_mutex_) {
            shared_lock = std::unique_lock<std::mutex>(*shared_mutex_);
        }
        ctx_copy = ctx_;
    }  // Release lock here
    
    // Process audio WITHOUT holding lock
    // NOTE: This assumes whisper_full() is thread-safe for read-only context
    // VERIFY THIS ASSUMPTION before implementing!
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    // ... set params ...
    
    int result = whisper_full(ctx_copy, wparams, audio_samples.data(), audio_samples.size());
    
    // ... rest of processing ...
}
```

**Option B: Per-Call Context (Better, but more memory)**
```cpp
// In StandaloneWhisperService::create_session
// Instead of sharing warm_ctx_, create per-call contexts
// This eliminates mutex contention entirely but uses more memory

WhisperSessionConfig cfg = config_;
cfg.shared_ctx = nullptr;  // Don't share context
cfg.shared_mutex = nullptr;

// Each session loads its own context (uses more memory)
auto session = std::make_unique<WhisperSession>(call_id, cfg);
```

**Benefits:**
- 20-40% improvement in multi-call scenarios
- Reduced latency spikes
- Better scalability

**Effort:** 1 week (including testing)  
**Risk:** Medium (requires careful verification of thread-safety)

**Trade-offs:**
- Option A: Assumes whisper_full() is thread-safe (needs verification)
- Option B: Increased memory usage (one context per call)

---

## Phase 3: Advanced Optimizations (Month 2-3)

### 3.1 Investigate Beam Search Support

**Location:** Research whisper.cpp capabilities

**Current Code:**
```cpp
whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
```

**Investigation Steps:**

1. **Check whisper.cpp API for beam search:**
```cpp
// Look for these in whisper.h:
// - WHISPER_SAMPLING_BEAM_SEARCH
// - whisper_full_params.beam_search
// - whisper_full_params.beam_size
```

2. **If beam search is available, prototype:**
```cpp
whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
wparams.beam_search.beam_size = 5;  // Typical beam size
wparams.beam_search.patience = 1.0f;
```

3. **Implement beam pruning (if API supports it):**
```cpp
// Pseudocode - actual implementation depends on whisper.cpp API
struct BeamState {
    std::vector<int> token_ids;
    float score;
    // ... other beam state ...
};

// Reuse beam state from previous window
std::unordered_map<std::string, BeamState> beam_cache_;

// In process_audio_chunk:
auto it = beam_cache_.find(call_id_);
if (it != beam_cache_.end()) {
    // Initialize beam search with previous state
    wparams.beam_search.initial_state = &it->second;
}

// After inference, save beam state
beam_cache_[call_id_] = extract_beam_state(ctx_);
```

**Benefits:**
- 1.5x-3x latency reduction (per paper)
- Better accuracy
- Reduced redundant computation

**Effort:** 2-3 weeks (depends on whisper.cpp API)  
**Risk:** High (may not be supported, may require whisper.cpp modifications)

**Next Steps:**
1. Review whisper.cpp documentation
2. Check GitHub issues for beam search support
3. Prototype with simple beam search (no pruning)
4. Measure latency vs accuracy trade-offs
5. Implement beam pruning if feasible

---

### 3.2 CPU/GPU Pipelining

**Location:** `whisper-service.cpp`, modify inference strategy

**Current Approach:**
- GPU used for all processing
- Sequential execution

**Recommended Approach:**

**Step 1: Profile encoding vs decoding costs**
```cpp
// Add timing around whisper_full() internals
// This may require whisper.cpp modifications or profiling tools

// Pseudocode:
auto t_encode_start = now();
// ... encoding phase ...
auto t_encode_end = now();
auto encoding_ms = duration(t_encode_start, t_encode_end);

auto t_decode_start = now();
// ... decoding phase ...
auto t_decode_end = now();
auto decoding_ms = duration(t_decode_start, t_decode_end);

std::cout << "Encoding: " << encoding_ms << "ms, Decoding: " << decoding_ms << "ms" << std::endl;
```

**Step 2: Implement CPU/GPU split (if whisper.cpp supports it)**
```cpp
// Check if whisper.cpp allows separate encoding/decoding
whisper_context_params cparams = whisper_context_default_params();
cparams.use_gpu = false;  // CPU for encoding
whisper_context* cpu_ctx = whisper_init_from_file_with_params(model_path, cparams);

cparams.use_gpu = true;  // GPU for decoding
whisper_context* gpu_ctx = whisper_init_from_file_with_params(model_path, cparams);

// Use CPU context for encoding, GPU context for decoding
// This may require whisper.cpp API changes
```

**Step 3: Dynamic tuning**
```cpp
// Adjust CPU/GPU split based on runtime metrics
struct ResourceProfile {
    double encoding_cpu_ms;
    double encoding_gpu_ms;
    double decoding_cpu_ms;
    double decoding_gpu_ms;
};

// Periodically profile and adjust
if (encoding_cpu_ms < encoding_gpu_ms * 0.8) {
    // CPU encoding is faster, use it
    use_cpu_for_encoding = true;
}
```

**Benefits:**
- 20-30% throughput improvement
- 15-25% power reduction
- Better hardware utilization

**Effort:** 2-4 weeks  
**Risk:** High (may require whisper.cpp modifications)

**Next Steps:**
1. Profile encoding vs decoding costs
2. Check if whisper.cpp supports separate contexts
3. Prototype CPU encoding + GPU decoding
4. Measure performance improvement
5. Implement dynamic tuning

---

## Database Schema for Metrics

**Recommended Addition:**

```sql
CREATE TABLE IF NOT EXISTS performance_metrics (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    call_id TEXT NOT NULL,
    metric_name TEXT NOT NULL,
    metric_value REAL NOT NULL,
    timestamp DATETIME DEFAULT CURRENT_TIMESTAMP,
    FOREIGN KEY (call_id) REFERENCES calls(id)
);

CREATE INDEX idx_perf_call_id ON performance_metrics(call_id);
CREATE INDEX idx_perf_metric_name ON performance_metrics(metric_name);
CREATE INDEX idx_perf_timestamp ON performance_metrics(timestamp);
```

**Usage:**
```cpp
void Database::log_performance_metric(const std::string& call_id, 
                                     const std::string& metric_name, 
                                     double metric_value) {
    std::string sql = "INSERT INTO performance_metrics (call_id, metric_name, metric_value) "
                     "VALUES (?, ?, ?)";
    // ... execute SQL ...
}
```

---

## Testing Strategy

### Unit Tests
```cpp
// Test streaming buffer management
TEST(StreamingInference, OverlappingWindows) {
    std::vector<float> buffer;
    // ... test window extraction and overlap ...
}

// Test mutex contention tracking
TEST(PerformanceMetrics, MutexWaitTracking) {
    // ... test mutex wait time measurement ...
}
```

### Integration Tests
```cpp
// Test multi-call performance
TEST(MultiCall, ThroughputDegradation) {
    // Start 2, 4, 8 concurrent calls
    // Measure throughput degradation
    // Assert < 50% degradation at 4 calls
}

// Test streaming latency
TEST(StreamingInference, FirstTokenLatency) {
    // Measure time to first transcription
    // Assert < 1 second for 5s audio
}
```

### Accuracy Tests
```cpp
// Test accuracy with streaming
TEST(StreamingInference, AccuracyRegression) {
    // Compare transcriptions: streaming vs batch
    // Assert WER (Word Error Rate) < 5% difference
}
```

---

## Summary

**Phase 1 (Week 1-2): Instrumentation**
- Add timing measurements (1-2 hours)
- Add mutex contention tracking (1 hour)
- Add performance stats (2-3 hours)
- **Total effort:** 1-2 days

**Phase 2 (Week 3-4): Quick Wins**
- Implement streaming inference (1-2 days)
- Optimize mutex usage (1 week)
- **Total effort:** 1.5-2 weeks

**Phase 3 (Month 2-3): Advanced**
- Investigate beam search (2-3 weeks)
- Implement CPU/GPU pipelining (2-4 weeks)
- **Total effort:** 1-2 months

**Expected Results:**
- Phase 1: Baseline metrics established
- Phase 2: 30-50% latency reduction
- Phase 3: 1.5x-3x latency reduction (if successful)

---

**Next Action:** Review recommendations with team and prioritize Phase 1 implementation.

