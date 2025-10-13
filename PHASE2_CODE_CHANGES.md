# Phase 2: Specific Code Changes

**Status:** PLANNING ONLY - NO CODE CHANGES YET  
**Reference:** See `PHASE2_IMPLEMENTATION_PLAN.md` for full context

---

## File 1: whisper-service.h

### Change 1: Add Streaming State to WhisperSession

**Location:** After line 57 (inside WhisperSession class, private section)

**Current Code:**
```cpp
private:
    whisper_context* ctx_;
    whisper_full_params wparams_;
    std::string call_id_;
    std::string latest_transcription_;
    std::mutex session_mutex_;
    
    // Shared context (optional)
    whisper_context* shared_ctx_;
    std::mutex* shared_mutex_;
```

**Add After Line 57:**
```cpp
    // Streaming inference state
    std::vector<float> streaming_buffer_;
    std::string previous_transcription_;  // For deduplication
    size_t window_size_ = 5 * 16000;      // 5 seconds at 16kHz
    size_t overlap_size_ = 1 * 16000;     // 1 second overlap
    size_t stride_ = 4 * 16000;           // 4 seconds stride
```

**Rationale:** Per-call streaming state, no global state

---

### Change 2: Add Helper Methods to WhisperSession

**Location:** After line 41 (inside WhisperSession class, public section)

**Current Code:**
```cpp
public:
    WhisperSession(const std::string& call_id, const WhisperSessionConfig& config);
    ~WhisperSession();
    
    bool process_audio_chunk(const std::vector<float>& audio_samples);
    std::string get_latest_transcription();
```

**Add After Line 41:**
```cpp
    // Streaming inference helpers
    bool process_window(const std::vector<float>& window);
    std::string deduplicate_transcription(const std::string& current);
    size_t get_buffer_size() const;  // For testing
```

**Rationale:** Encapsulate streaming logic in helper methods

---

### Change 3: Add Performance Metrics to StandaloneWhisperService

**Location:** After line 107 (inside StandaloneWhisperService class, private section)

**Current Code:**
```cpp
private:
    // Shared warm context
    whisper_context* warm_ctx_;
    std::mutex warm_mutex_;
```

**Add After Line 107:**
```cpp
    // Performance metrics
    struct PerformanceMetrics {
        std::atomic<uint64_t> total_mutex_wait_ms{0};
        std::atomic<uint64_t> total_inference_ms{0};
        std::atomic<uint64_t> total_chunks_processed{0};
        std::atomic<uint64_t> max_mutex_wait_ms{0};
    };
    PerformanceMetrics metrics_;
```

**Rationale:** Track performance for optimization decisions

---

## File 2: whisper-service.cpp

### Change 4: Enable CoreML/Metal Optimizations

**Location:** Lines 207-209 (context initialization)

**Current Code:**
```cpp
whisper_context_params cparams = whisper_context_default_params();
cparams.use_gpu = config_.use_gpu;
warm_ctx_ = whisper_init_from_file_with_params(config_.model_path.c_str(), cparams);
```

**Replace With:**
```cpp
whisper_context_params cparams = whisper_context_default_params();
cparams.use_gpu = config_.use_gpu;
cparams.flash_attn = true;           // Enable flash attention for Metal (20-30% speedup)
cparams.gpu_device = 0;              // Use primary GPU
cparams.dtw_token_timestamps = false; // Disable timestamps for speed (5-10% speedup)
warm_ctx_ = whisper_init_from_file_with_params(config_.model_path.c_str(), cparams);
```

**Rationale:** Enable Metal-specific optimizations for faster inference

**Risk:** MINIMAL (graceful fallback if not supported)

---

### Change 5: Add Mutex Wait Instrumentation

**Location:** Lines 116-119 (mutex acquisition in process_audio_chunk)

**Current Code:**
```cpp
// Serialize access to shared whisper context if needed
std::unique_lock<std::mutex> shared_lock;
if (shared_mutex_) {
    shared_lock = std::unique_lock<std::mutex>(*shared_mutex_);
}
```

**Replace With:**
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

// Update metrics (if service pointer available)
// metrics_.total_mutex_wait_ms += mutex_wait_ms;
// metrics_.max_mutex_wait_ms = std::max(metrics_.max_mutex_wait_ms.load(), (uint64_t)mutex_wait_ms);
```

**Rationale:** Measure contention to guide optimization decisions

**Risk:** MINIMAL (logging only)

---

### Change 6: Rewrite process_audio_chunk for Streaming

**Location:** Lines 103-167 (entire process_audio_chunk method)

**Current Code:**
```cpp
bool WhisperSession::process_audio_chunk(const std::vector<float>& audio_samples) {
    if (audio_samples.empty()) {
        return false;
    }
    
    // Serialize access to shared whisper context if needed
    std::unique_lock<std::mutex> shared_lock;
    if (shared_mutex_) {
        shared_lock = std::unique_lock<std::mutex>(*shared_mutex_);
    }
    
    // Session-local lock for updating session state
    std::lock_guard<std::mutex> session_lock(session_mutex_);
    
    // Process audio chunk directly with whisper
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.language = "en";
    wparams.n_threads = 4;
    wparams.no_timestamps = true;
    wparams.single_segment = false;
    
    int result = whisper_full(ctx_, wparams, audio_samples.data(), audio_samples.size());
    
    if (result == 0) {
        // Extract transcription from segments
        const int n_segments = whisper_full_n_segments(ctx_);
        std::string transcription;
        
        for (int i = 0; i < n_segments; ++i) {
            const char* text = whisper_full_get_segment_text(ctx_, i);
            if (text) {
                transcription += text;
            }
        }
        
        if (!transcription.empty()) {
            latest_transcription_ = transcription;
            std::cout << "📝 [" << call_id_ << "] Transcription: " << transcription << std::endl;
            return true;
        }
    }
    
    return false;
}
```

**Replace With:**
```cpp
bool WhisperSession::process_audio_chunk(const std::vector<float>& audio_samples) {
    if (audio_samples.empty()) {
        return false;
    }
    
    // Append to streaming buffer
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        streaming_buffer_.insert(streaming_buffer_.end(), 
                                 audio_samples.begin(), 
                                 audio_samples.end());
        
        // Cap buffer at 10 seconds to prevent overflow
        const size_t max_buffer_size = 10 * 16000;
        if (streaming_buffer_.size() > max_buffer_size) {
            std::cout << "⚠️ [" << call_id_ << "] Buffer overflow, flushing old data" << std::endl;
            streaming_buffer_.erase(streaming_buffer_.begin(), 
                                   streaming_buffer_.begin() + (streaming_buffer_.size() - max_buffer_size));
        }
    }
    
    // Process overlapping windows
    bool processed_any = false;
    while (true) {
        std::vector<float> window;
        
        // Extract window under session lock (fast operation)
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (streaming_buffer_.size() < window_size_) {
                break; // Not enough data for full window
            }
            
            // Copy window
            window.assign(streaming_buffer_.begin(), 
                         streaming_buffer_.begin() + window_size_);
        }
        
        // Process window (holds warm_mutex_)
        if (process_window(window)) {
            processed_any = true;
        }
        
        // Slide window by stride
        {
            std::lock_guard<std::mutex> lock(session_mutex_);
            if (streaming_buffer_.size() >= stride_) {
                streaming_buffer_.erase(streaming_buffer_.begin(), 
                                       streaming_buffer_.begin() + stride_);
            } else {
                break;
            }
        }
    }
    
    return processed_any;
}
```

**Rationale:** Implement streaming inference with overlapping windows

**Risk:** MEDIUM (new logic, needs thorough testing)

---

### Change 7: Add process_window Method

**Location:** After process_audio_chunk method (new method)

**Add New Method:**
```cpp
bool WhisperSession::process_window(const std::vector<float>& window) {
    // Serialize access to shared whisper context if needed
    std::unique_lock<std::mutex> shared_lock;
    auto t_mutex_start = std::chrono::high_resolution_clock::now();
    
    if (shared_mutex_) {
        shared_lock = std::unique_lock<std::mutex>(*shared_mutex_);
    }
    
    auto t_mutex_acquired = std::chrono::high_resolution_clock::now();
    auto mutex_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t_mutex_acquired - t_mutex_start).count();
    
    if (mutex_wait_ms > 10) {
        std::cout << "⏳ [" << call_id_ << "] Mutex wait: " << mutex_wait_ms << "ms" << std::endl;
    }
    
    // Process window with whisper
    whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
    wparams.language = "en";
    wparams.n_threads = 4;
    wparams.no_timestamps = true;
    wparams.single_segment = false;
    
    auto t_inference_start = std::chrono::high_resolution_clock::now();
    int result = whisper_full(ctx_, wparams, window.data(), window.size());
    auto t_inference_end = std::chrono::high_resolution_clock::now();
    
    auto inference_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        t_inference_end - t_inference_start).count();
    
    std::cout << "⚡ [" << call_id_ << "] Inference: " << inference_ms << "ms" << std::endl;
    
    if (result == 0) {
        // Extract transcription from segments
        const int n_segments = whisper_full_n_segments(ctx_);
        std::string transcription;
        
        for (int i = 0; i < n_segments; ++i) {
            const char* text = whisper_full_get_segment_text(ctx_, i);
            if (text) {
                transcription += text;
            }
        }
        
        if (!transcription.empty()) {
            // Deduplicate with previous transcription
            std::string new_text = deduplicate_transcription(transcription);
            
            if (!new_text.empty()) {
                std::lock_guard<std::mutex> lock(session_mutex_);
                latest_transcription_ = new_text;
                previous_transcription_ = transcription;
                
                std::cout << "📝 [" << call_id_ << "] New text: " << new_text << std::endl;
                return true;
            }
        }
    }
    
    return false;
}
```

**Rationale:** Separate window processing logic for clarity

**Risk:** MEDIUM (new method, needs testing)

---

### Change 8: Add deduplicate_transcription Method

**Location:** After process_window method (new method)

**Add New Method:**
```cpp
std::string WhisperSession::deduplicate_transcription(const std::string& current) {
    if (previous_transcription_.empty()) {
        return current; // First transcription
    }
    
    // Find longest common prefix
    size_t common_len = 0;
    size_t max_len = std::min(previous_transcription_.size(), current.size());
    
    for (size_t i = 0; i < max_len; ++i) {
        if (previous_transcription_[i] == current[i]) {
            common_len++;
        } else {
            break;
        }
    }
    
    // Extract new text (everything after common prefix)
    if (common_len < current.size()) {
        std::string new_text = current.substr(common_len);
        
        // Trim leading whitespace
        size_t start = new_text.find_first_not_of(" \t\n\r");
        if (start != std::string::npos) {
            new_text = new_text.substr(start);
        }
        
        return new_text;
    }
    
    return ""; // No new text
}
```

**Rationale:** Remove repeated text from overlapping windows

**Risk:** LOW (simple string processing)

---

### Change 9: Add get_buffer_size Method (for testing)

**Location:** After deduplicate_transcription method (new method)

**Add New Method:**
```cpp
size_t WhisperSession::get_buffer_size() const {
    std::lock_guard<std::mutex> lock(session_mutex_);
    return streaming_buffer_.size();
}
```

**Rationale:** Allow unit tests to verify buffer management

**Risk:** MINIMAL (testing only)

---

## Summary of Changes

### whisper-service.h
- ✅ Add streaming state to WhisperSession (3 new members)
- ✅ Add helper methods to WhisperSession (3 new methods)
- ✅ Add performance metrics to StandaloneWhisperService (1 new struct)

### whisper-service.cpp
- ✅ Enable CoreML/Metal optimizations (3 lines)
- ✅ Add mutex wait instrumentation (10 lines)
- ✅ Rewrite process_audio_chunk for streaming (~50 lines)
- ✅ Add process_window method (~60 lines)
- ✅ Add deduplicate_transcription method (~30 lines)
- ✅ Add get_buffer_size method (~5 lines)

**Total Lines Changed:** ~160 lines  
**Total New Code:** ~145 lines  
**Total Modified Code:** ~15 lines

---

## Compilation

**No changes to build system needed!**

```bash
cd build
make whisper-service -j8
```

**Expected:** Clean compilation with no warnings

---

## Testing Commands

### Unit Tests (if implemented)
```bash
cd build
make whisper-service-tests -j8
./whisper-service-tests
```

### Integration Tests
```bash
# Start services
./start-wildfire.sh

# Make test call via web interface
# Monitor logs for:
# - "📝 [call_id] New text: ..." (streaming working)
# - "⏳ [call_id] Mutex wait: Xms" (contention tracking)
# - "⚡ [call_id] Inference: Xms" (performance tracking)
```

---

## Rollback

If issues arise, revert with:

```bash
git checkout main -- whisper-service.h whisper-service.cpp
cd build
make whisper-service -j8
```

---

**Status:** Ready for implementation after plan approval  
**Next:** Review plan, approve, create feature branch, begin Week 1

