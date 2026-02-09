# Technical Specification: WhisperTalk Real-Time Optimization

**Document Version**: 1.2  
**Last Updated**: 2026-02-09  
**Status**: Final - Ready for Planning Phase

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-02-09 | Zencoder | Initial specification |
| 1.1 | 2026-02-09 | Zencoder | Critical fixes: KV cache rollback, VAD buffer, signal ordering, error handling |
| 1.2 | 2026-02-09 | Zencoder | Test corpus generation fix, optional improvements: German compound words, port collision, metrics disk management, CoreML warm-up |

---

## 1. Technical Context

### 1.1 Language & Build System
- **Language**: C++ (C++17), Python 3.9+
- **Build System**: CMake 3.22+
- **Compiler**: Clang with C++17 support
- **Platform**: macOS (Apple Silicon M1/M2/M3)

### 1.2 Core Dependencies
- **whisper.cpp**: CoreML-optimized ASR engine
- **llama.cpp**: Metal-accelerated LLM inference
- **Kokoro TTS**: PyTorch with MPS (Metal Performance Shaders)
- **Standard Libraries**: POSIX sockets, pthreads, std::thread

### 1.3 Current Architecture
Six standalone services in a linear pipeline:
```
SIP Client(s) → Inbound Audio Processor → Whisper Service → 
LLaMA Service → Kokoro Service → Outbound Audio Processor → SIP Client(s)
```

**Communication Protocols**:
- UDP: SIP Client ↔ Audio Processors (RTP with 4-byte call_id prefix)
- TCP: All intermediate services (streaming connections)
- Unix Sockets: Control plane (CALL_START/CALL_END signals)

---

## 2. Implementation Approach

### 2.1 Protocol Enhancements

#### 2.1.1 Call Lifecycle Signaling Protocol
All services will implement a unified control protocol for call lifecycle events:

**Signal Format**:
```
CALL_START:<call_id>
CALL_END:<call_id>
DATA:<call_id>:<payload>
```

**Implementation Strategy**:
- **SIP Client**: Send CALL_START via Unix socket `/tmp/inbound-audio-processor.ctrl` on INVITE accept
- **Each Service**: 
  - Listen on Unix socket `/tmp/<service-name>.ctrl` for control signals
  - Forward signals to downstream service via Unix socket
  - On CALL_END, clean up call-specific resources (threads, buffers, sockets)

**Unix Socket Communication Pattern with Error Handling**:
```cpp
// Sender side (example from SIP Client)
int send_control_signal(const std::string& target_path, const std::string& message) {
    int ctrl_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (ctrl_sock < 0) {
        std::cerr << "Failed to create control socket" << std::endl;
        return -1;
    }
    
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, target_path.c_str(), sizeof(addr.sun_path) - 1);
    
    // Set timeout to avoid blocking forever
    struct timeval tv{.tv_sec = 0, .tv_usec = 100000};  // 100ms
    setsockopt(ctrl_sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    
    ssize_t sent = sendto(ctrl_sock, message.c_str(), message.length(), 0, 
                          (struct sockaddr*)&addr, sizeof(addr));
    close(ctrl_sock);
    
    if (sent < 0) {
        // Non-critical: Log but continue (service may not be running yet)
        std::cerr << "Warning: Failed to send control signal to " << target_path 
                  << " (service may not be ready)" << std::endl;
        return -1;
    }
    return 0;
}

// Receiver side (example from Inbound Audio Processor)
int setup_control_listener(const std::string& socket_path) {
    int ctrl_sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (ctrl_sock < 0) {
        std::cerr << "Failed to create control socket" << std::endl;
        return -1;
    }
    
    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path.c_str(), sizeof(addr.sun_path) - 1);
    
    // Clean up old socket if exists (prevent bind failure)
    unlink(addr.sun_path);
    
    if (bind(ctrl_sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "Failed to bind control socket: " << strerror(errno) << std::endl;
        close(ctrl_sock);
        return -1;
    }
    
    // Set non-blocking for graceful shutdown
    int flags = fcntl(ctrl_sock, F_GETFL, 0);
    fcntl(ctrl_sock, F_SETFL, flags | O_NONBLOCK);
    
    return ctrl_sock;
}

// Listener thread receives signals
void control_listener_thread(int ctrl_sock) {
    char buf[256];
    while (running_) {
        ssize_t n = recv(ctrl_sock, buf, sizeof(buf) - 1, 0);
        if (n > 0) {
            buf[n] = '\0';
            handle_control_signal(std::string(buf));
        } else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            std::cerr << "Control socket error: " << strerror(errno) << std::endl;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}
```

**Key Design Points**:
- Signals are **advisory, not critical**: If a service isn't ready, the sender logs a warning but continues
- Receiver uses `unlink()` before bind to handle restarts cleanly
- Timeouts prevent indefinite blocking
- Non-blocking receive allows graceful shutdown
- Signal ordering (CALL_START before audio) is best-effort, not guaranteed

#### 2.1.2 Call ID Management
**Current State**: Sequential ID assignment starting from 1 in each SIP client instance.

**Problem**: Multiple SIP client instances will generate conflicting IDs.

**Solution**: Instance-based ID allocation using environment variable or command-line argument:
```cpp
// SIP Client enhancement
int instance_id = 0;  // 0-9 from environment or CLI
int next_id_offset = instance_id * 1000;  // Each instance gets 1000 IDs
int call_id = next_id_offset + (sequential_counter++ % 1000);
```

**Port Mapping** (Server Listeners): 
- **Whisper Service**: Listens on ports `13000-13099` (100 ports mapped via `13000 + (call_id % 100)`)
  - Inbound Processor connects TO Whisper as TCP client
- **LLaMA Service**: Listens on port `8083` (single port, multiplexed by call_id in message)
  - Whisper connects TO LLaMA as TCP client
- **Kokoro Service**: Listens on port `8090` (single port, spawns thread per call)
  - LLaMA connects TO Kokoro as TCP client
  - Kokoro then connects TO Outbound Processor as client
- **Outbound Processor**: Listens on ports `8090-8189` (100 ports mapped via `8090 + (call_id % 100)`)
  - Kokoro connects TO Outbound as TCP client

**Range Support**: 0-9999 call IDs (10 instances × 1000 calls each)

**Port Collision Behavior**: With >100 concurrent calls, multiple calls share the same listening port via modulo mapping. Services differentiate calls using the `call_id` prefix in TCP streams. Maximum concurrent calls: 9999 (limited by call_id space, not ports).

**Connection Initiation Summary**:
```
Inbound (client) → Whisper (server on 13000+call_id%100)
Whisper (client) → LLaMA (server on 8083)
LLaMA (client)   → Kokoro (server on 8090)
Kokoro (client)  → Outbound (server on 8090+call_id%100)
```

---

### 2.2 Service-Specific Implementations

#### 2.2.1 SIP Client (`sip-client-main.cpp`)

**Changes Required**:
1. **Instance ID Support**:
   - Add CLI argument: `--instance-id <0-9>`
   - Validate instance ID and compute ID offset
   
2. **Call Lifecycle Signaling**:
   - On INVITE accept: Send `CALL_START:<call_id>` to `/tmp/inbound-audio-processor.ctrl`
   - On BYE received: Send `CALL_END:<call_id>` to `/tmp/inbound-audio-processor.ctrl`
   
3. **Multi-Instance Port Management**:
   - RTP receive ports: `10000 + call_id` (already dynamic)
   - SIP signaling: Each instance binds to random port (already works)
   - No conflicts expected as instances use separate processes

**New Components**:
```cpp
class ControlSignalSender {
public:
    void send_to_inbound(const std::string& signal);
private:
    int ctrl_sock_;
    struct sockaddr_un inbound_addr_;
};
```

**Integration Point**: 
- In `handle_invite()`: After creating CallSession, send CALL_START
- In `handle_bye()`: Before closing RTP socket, send CALL_END

#### 2.2.2 Inbound Audio Processor (`inbound-audio-processor.cpp`)

**Changes Required**:
1. **Control Signal Reception**:
   - New thread listening on Unix socket `/tmp/inbound-audio-processor.ctrl`
   - On `CALL_START`: Pre-allocate CallState object
   - On `CALL_END`: Close TCP connection to Whisper, forward to Whisper's control socket, cleanup CallState
   
2. **Crash Resilience** (already mostly implemented):
   - Current behavior: Dumps to /dev/null when Whisper disconnected ✓
   - Enhancement needed: Explicit reconnection logging with call_id

3. **Graceful Shutdown**:
   - On CALL_END signal: Stop audio conversion thread for that call_id
   - Forward CALL_END to `/tmp/whisper-service.ctrl`

**New Components**:
```cpp
class ControlListener {
public:
    void listen_thread();
    void handle_signal(const std::string& msg);
private:
    int ctrl_sock_;
    std::function<void(std::string)> callback_;
};

class ControlForwarder {
public:
    void forward_to_whisper(const std::string& signal);
private:
    int ctrl_sock_;
    struct sockaddr_un whisper_addr_;
};
```

**Integration**:
- Main thread spawns `ControlListener` thread
- On CALL_START: Call `get_or_create_call()` to pre-allocate
- On CALL_END: Lookup CallState, close sockets, forward signal, erase from map

#### 2.2.3 Whisper Service (`whisper-service.cpp`)

**Changes Required**:
1. **Dynamic Port Listening**:
   - Current: Listens on 10 fixed ports (13001-13010)
   - New: Listen on ports `13000 + (call_id % 100)` → 100 ports (13000-13099)
   - Implementation: Spawn 100 listener threads (lightweight, mostly blocked in accept())
   
2. **Control Signal Handling**:
   - Listen on `/tmp/whisper-service.ctrl`
   - On `CALL_START`: Log and prepare (listener already active)
   - On `CALL_END`: Stop transcription for call_id, close TCP to inbound, forward to LLaMA
   
3. **Crash Resilience**:
   - **Inbound disconnection**: Already handled (recv returns 0, closes socket)
   - **LLaMA disconnection**: Buffer up to 10 transcriptions, discard oldest if LLaMA unreachable
   
4. **VAD Optimization**:
   - Current: 100ms windows, 800ms silence, 0.00005 energy threshold
   - Enhancement: Add prosody detection (avoid mid-word cuts)
     - Check for sustained energy drop (not just single window below threshold)
     - Use minimum 3 consecutive low-energy windows before declaring silence
   - Add VAD metrics logging: segment count, average length, false positives

**New Components**:
```cpp
struct VADMetrics {
    int segments_detected = 0;
    float avg_segment_length = 0.0f;
    int false_positives = 0;
    int false_negatives = 0;
};

class TranscriptionBuffer {
public:
    void push(int call_id, const std::string& text);
    std::string pop_oldest(int call_id);
private:
    std::map<int, std::deque<std::string>> buffers_;
    const size_t MAX_BUFFER_SIZE = 10;
};
```

**Integration**:
- Replace fixed loop (1-10) with loop (0-99) for listener threads
- Add `ControlListener` for Unix socket
- On CALL_END: Lock call mutex, clear audio buffer, close sockets, forward signal

#### 2.2.4 LLaMA Service (`llama-service.cpp`)

**Changes Required**:
1. **Sentence Completion Detection** (Implemented in LLaMA service):
   - **Location**: LLaMA TCP receiver thread buffers text from Whisper
   - **Logic**: Accumulate text chunks until sentence-ending punctuation detected (`.`, `?`, `!`)
   - **Timeout**: If no punctuation within 2 seconds of last chunk, treat as complete
   - **Max Buffer**: 5 seconds total wait time (safety limit to avoid indefinite delay)
   - **Implementation**:
     ```cpp
     struct SentenceBuffer {
         std::string accumulated_text;
         std::chrono::steady_clock::time_point last_chunk_time;
         std::chrono::steady_clock::time_point buffer_start_time;
         
         bool is_complete() {
             auto now = std::chrono::steady_clock::now();
             
             // Check for sentence-ending punctuation
             if (accumulated_text.find_last_of(".?!") != std::string::npos) {
                 return true;
             }
             
             // 2-second timeout since last chunk
             auto since_last = std::chrono::duration_cast<std::chrono::seconds>(
                 now - last_chunk_time).count();
             if (since_last >= 2) return true;
             
             // 5-second absolute timeout (safety)
             auto total_time = std::chrono::duration_cast<std::chrono::seconds>(
                 now - buffer_start_time).count();
             if (total_time >= 5) return true;
             
             return false;
         }
         
         void add_chunk(const std::string& text) {
             if (accumulated_text.empty()) {
                 buffer_start_time = std::chrono::steady_clock::now();
             }
             accumulated_text += text + " ";  // Space between chunks
             last_chunk_time = std::chrono::steady_clock::now();
         }
     };
     ```
   - **Why LLaMA, not Whisper**: Whisper sends transcriptions immediately after VAD trigger; buffering in LLaMA allows Whisper to remain stateless and focused on ASR
   
   - **Known Limitation - German Abbreviations**: Current implementation may incorrectly trigger on abbreviations like "Dr.", "z.B.", "usw.". For production deployment, consider enhanced logic:
     ```cpp
     bool is_sentence_ending(const std::string& text) {
         std::string trimmed = trim(text);
         if (trimmed.empty()) return false;
         
         char last = trimmed.back();
         if (last != '.' && last != '?' && last != '!') return false;
         
         // Check for common German abbreviations (simple heuristic)
         static const std::vector<std::string> abbrevs = 
             {"Dr.", "z.B.", "usw.", "bzw.", "etc.", "ca."};
         for (const auto& abbrev : abbrevs) {
             if (trimmed.length() >= abbrev.length() &&
                 trimmed.substr(trimmed.length() - abbrev.length()) == abbrev) {
                 return false;  // Don't end on abbreviation
             }
         }
         
         return true;  // Valid sentence ending
     }
     ```
   - **Alternative**: Use 2-second timeout as primary trigger (more robust than punctuation detection)
   
   - **German Compound Word Edge Case**: German compound words spoken with pauses (e.g., "Donaudampfschifffahrtsgesellschaft" with pauses between components) may trigger the 2-second timeout mid-word, causing sentence fragmentation. This is acceptable as LLaMA can handle fragmented input contextually. If needed for heavily technical German conversations, increase timeout to 3 seconds, but this trades responsiveness for completeness.
   
2. **Response Interruption**:
   - Currently generates responses in blocking call to llama.cpp
   - Solution: Use `llama_decode` in streaming mode with cancellation check
   - Implementation:
     ```cpp
     std::atomic<bool> should_stop{false};
     while (!should_stop && !is_eog) {
         llama_decode(...);
         llama_sampling_sample(...);
         // Check for new input every 5 tokens
         if (token_count % 5 == 0 && has_new_input(call_id)) {
             should_stop = true;
             clear_partial_response(call_id);
         }
     }
     ```
   
3. **Crash Resilience**:
   - **Whisper disconnection**: Keep conversation context (KV cache) alive, accept reconnection
   - **Kokoro disconnection**: Discard generated responses (no buffering)
   - TCP connection handling: Non-blocking sends with error checking
   
4. **Control Signal Handling**:
   - Listen on `/tmp/llama-service.ctrl`
   - On `CALL_START`: Pre-allocate conversation context (sequence ID in KV cache)
   - On `CALL_END`: Clear conversation history, forward to Kokoro

**New Components**:
```cpp
class ConversationManager {
public:
    void start_conversation(int call_id);
    void add_user_input(int call_id, const std::string& text);
    std::string generate_response(int call_id, std::atomic<bool>& stop_flag);
    void end_conversation(int call_id);
private:
    std::map<int, std::vector<std::string>> histories_;
    std::map<int, int> seq_ids_;  // KV cache sequence IDs
};

class InputMonitor {
public:
    bool has_new_input(int call_id);
    void notify_new_input(int call_id);
private:
    std::map<int, std::atomic<bool>> flags_;
};
```

**Integration**:
- Wrap llama_decode loop with interruption checks every 5 tokens
- Add sentence completion buffer in TCP receiver thread
- Forward CALL_END to `/tmp/kokoro-service.ctrl`

#### 2.2.5 Kokoro Service (`kokoro_service.py`)

**Changes Required**:
1. **Control Signal Handling**:
   - Python Unix socket listener for `/tmp/kokoro-service.ctrl`
   - On `CALL_START`: Pre-allocate synthesis resources for call_id
   - On `CALL_END`: Stop synthesis, close TCP to outbound processor, cleanup buffers
   
2. **Crash Resilience**:
   - **LLaMA disconnection**: Current TCP accept loop handles this (recv returns 0)
   - **Outbound disconnection**: Dump audio to /dev/null (already implemented)
   - Enhancement: Retry connection to outbound processor every 2 seconds
   
3. **Multi-Call Management**:
   - Currently handles multiple calls via threading
   - Ensure thread-safe access to PyTorch model (use locks or per-thread model instances)

**New Components**:
```python
class ControlListener(threading.Thread):
    def run(self):
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
        sock.bind('/tmp/kokoro-service.ctrl')
        while running:
            data, addr = sock.recvfrom(1024)
            self.handle_signal(data.decode())
    
    def handle_signal(self, msg):
        if msg.startswith("CALL_START:"):
            call_id = int(msg.split(":")[1])
            # Pre-allocate resources
        elif msg.startswith("CALL_END:"):
            call_id = int(msg.split(":")[1])
            # Cleanup and forward to outbound
```

**Integration**:
- Start `ControlListener` thread in main()
- On CALL_END: Close TCP connection, send signal to `/tmp/outbound-audio-processor.ctrl`

#### 2.2.6 Outbound Audio Processor (`outbound-audio-processor.cpp`)

**Changes Required**:
1. **Control Signal Handling**:
   - Already listens on `/tmp/outbound-audio-processor.ctrl` for ACTIVATE signal
   - Extend to handle `CALL_START` (alias for ACTIVATE) and `CALL_END`
   - On `CALL_END`: Stop 20ms scheduling thread, close TCP from Kokoro, cleanup buffers
   
2. **Crash Resilience**:
   - **Kokoro disconnection**: Already handled (TCP recv returns 0)
   - **SIP Client UDP failure**: UDP is fire-and-forget, no change needed
   
3. **Graceful Shutdown**:
   - On CALL_END: Stop RTP scheduling thread cleanly
   - No forwarding needed (end of pipeline)

**Integration**:
- Extend existing control socket handler to parse CALL_END
- Add flag to stop scheduling thread: `std::atomic<bool> should_stop_per_call[call_id]`

---

### 2.3 Data Flow Enhancements

#### 2.3.1 Call Start Sequence
```
1. SIP Client receives INVITE
2. SIP Client allocates call_id (instance_id * 1000 + counter)
3. SIP Client sends CALL_START to Inbound Processor [Unix socket]
4. SIP Client sends 200 OK to SIP network
5. Inbound Processor forwards CALL_START to Whisper [Unix socket]
6. Whisper forwards CALL_START to LLaMA [Unix socket]
7. LLaMA forwards CALL_START to Kokoro [Unix socket]
8. Kokoro forwards CALL_START to Outbound Processor [Unix socket]
9. Audio flow begins: SIP → Inbound → Whisper → LLaMA → Kokoro → Outbound → SIP
```

#### 2.3.2 Call End Sequence (Signal-First Ordering)
```
1. SIP Client receives BYE
2. SIP Client sends CALL_END to Inbound Processor [Unix socket]

3. Inbound Processor:
   - Forwards CALL_END to Whisper [Unix socket] FIRST
   - Stops audio conversion thread for call_id
   - Waits 200ms grace period (allow downstream to prepare)
   - Closes TCP to Whisper

4. Whisper:
   - Forwards CALL_END to LLaMA [Unix socket] FIRST
   - Stops transcription for call_id
   - Waits 200ms grace period
   - Closes TCP from Inbound

5. LLaMA:
   - Forwards CALL_END to Kokoro [Unix socket] FIRST
   - Stops any active generation (sets stop_flag)
   - Clears conversation history for call_id
   - Waits 200ms grace period
   - Closes TCP connections

6. Kokoro:
   - Forwards CALL_END to Outbound [Unix socket] FIRST
   - Stops synthesis for call_id
   - Waits 200ms grace period
   - Closes TCP to Outbound Processor

7. Outbound Processor:
   - Stops RTP scheduling for call_id
   - Closes TCP from Kokoro
   - No further forwarding (end of pipeline)
```

**Key Design Principle: Signal-First, Close-Later**
- **Rationale**: Forward CALL_END before closing connections to avoid race conditions
- **Grace Period**: 200ms delay allows downstream services to receive signal and prepare for shutdown
- **Idempotency**: CALL_END signals are idempotent (safe to receive multiple times)
- **Timeout**: If downstream doesn't respond, proceed with cleanup anyway (non-blocking)

**Grace Period Implementation Example**:
```cpp
// Example from Inbound Audio Processor on CALL_END
void handle_call_end(int call_id) {
    // 1. Forward signal FIRST
    send_control_signal("/tmp/whisper-service.ctrl", 
                       "CALL_END:" + std::to_string(call_id));
    
    // 2. Stop local processing
    auto call_state = calls_[call_id];
    call_state->active = false;  // Signal processing thread to stop
    
    // 3. Grace period (allow downstream to receive and process signal)
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // 4. Now safe to close connections
    {
        std::lock_guard<std::mutex> lock(call_state->mutex);
        if (call_state->tcp_socket != -1) {
            close(call_state->tcp_socket);
            call_state->tcp_socket = -1;
        }
    }
    
    // 5. Cleanup resources
    calls_.erase(call_id);
}
```

#### 2.3.3 Crash Recovery Flows

**Scenario: Whisper Service Crashes**
```
1. Inbound Processor detects send() failure on TCP
2. Inbound closes socket, marks call as disconnected
3. Audio continues decoding, output dumped
4. Every 2s: Attempt reconnect to port 13000+(call_id%100)
5. On Whisper restart: Listeners accept connections
6. Inbound reconnects, resumes streaming (no backlog)
7. Whisper resumes transcription (context lost, acceptable)
```

**Scenario: LLaMA Service Crashes**
```
1. Whisper detects send() failure on TCP port 8083
2. Whisper buffers up to 10 transcriptions
3. On buffer full: Discard oldest transcription
4. Every 2s: Attempt reconnect to port 8083
5. On LLaMA restart: Accept connection
6. Whisper reconnects, sends buffered transcriptions
7. LLaMA processes (conversation context preserved in KV cache file if persisted)
```

---

### 2.4 VAD Algorithm Enhancement

**Current Algorithm**:
```cpp
// 100ms windows (1600 samples at 16kHz)
float energy = sum(samples^2) / count;
if (energy > 0.00005f) in_speech = true;
if (in_speech && energy < threshold) silence_count++;
if (silence_count > 8) segment_complete = true;  // 800ms silence
```

**Enhanced Algorithm** (German-optimized with proper buffer management):
```cpp
// Process audio buffer in 100ms windows (1600 samples at 16kHz)
void process_audio_buffer(WhisperCall* call) {
    std::vector<float> to_transcribe;
    
    std::lock_guard<std::mutex> lock(call->mutex);
    
    // Process all complete windows in buffer
    while (call->audio_buffer.size() >= 1600) {
        // Calculate energy for current window
        float energy = 0.0f;
        for (int i = 0; i < 1600; ++i) {
            energy += call->audio_buffer[i] * call->audio_buffer[i];
        }
        energy /= 1600.0f;
        
        // Speech start: Single high-energy window
        if (energy > 0.00005f && !call->in_speech) {
            call->in_speech = true;
            call->silence_count = 0;
        }
        
        // Speech end: 8 consecutive low-energy windows (800ms)
        // Note: Keeping 800ms (not 300ms) to avoid cutting German compound words
        if (call->in_speech && energy < 0.00005f) {
            call->silence_count++;
        } else if (call->in_speech) {
            call->silence_count = 0;  // Reset on any speech
        }
        
        // Trigger transcription on 800ms silence after speech OR 8s max
        bool should_transcribe = false;
        if (call->in_speech && call->silence_count >= 8) {  // 800ms silence
            should_transcribe = true;
            call->in_speech = false;
            call->silence_count = 0;
        } else if (call->audio_buffer.size() >= 16000 * 8) {  // 8 seconds max
            should_transcribe = true;
            call->in_speech = false;
            call->silence_count = 0;
        }
        
        if (should_transcribe && call->audio_buffer.size() > 0) {
            // Move entire buffer to transcription queue
            to_transcribe = std::move(call->audio_buffer);
            call->audio_buffer.clear();
            break;  // Stop processing, transcribe accumulated audio
        } else {
            // Remove processed window and continue (FIX: was missing in current code)
            // Note: Loop continues to next while iteration, processing next 1600 samples
            // If buffer has < 1600 samples remaining, loop exits naturally
            call->audio_buffer.erase(call->audio_buffer.begin(), 
                                    call->audio_buffer.begin() + 1600);
        }
    }
    
    // Transcribe outside the lock (avoid blocking audio receiver)
    if (!to_transcribe.empty()) {
        transcribe_and_send(call->id, to_transcribe);
    }
}
```

**Key Fixes from Current Code**:
1. **Window Removal**: `erase()` call after processing each window (was missing, caused infinite buffer growth)
2. **Complete Loop**: Process ALL windows in buffer, not just first one
3. **Proper Locking**: Acquire lock once, process all windows, release before transcription
4. **Conservative Timing**: Keep 800ms silence threshold (not 300ms) based on German speech patterns

**Rationale**:
- 800ms silence is appropriate for German (compound words, thinking pauses: "äh", "also")
- Window removal prevents memory growth bug in current implementation
- Processing all buffered windows reduces latency (don't wait for next iteration)

**Metrics Collection**:
```cpp
struct VADMetrics {
    int call_id;
    int segments_detected;
    std::vector<float> segment_lengths;  // in seconds
    int low_energy_frames;  // for false positive analysis
    std::chrono::steady_clock::time_point start_time;
    
    void log_to_file(const std::string& path);
};
```

---

### 2.5 LLaMA Response Interruption

**Current Issue**: llama.cpp's `llama_decode` is blocking for entire response generation.

**Solution**: Token-by-token generation with periodic cancellation checks.

**Implementation with KV Cache Rollback**:
```cpp
std::string generate_with_interruption(int call_id, llama_context* ctx, 
                                       const std::string& prompt, 
                                       std::atomic<bool>& stop_flag,
                                       int seq_id) {
    // Apply chat template and encode prompt
    std::vector<llama_token> tokens = tokenize(prompt);
    
    // Save KV cache position BEFORE generation (for rollback on interrupt)
    int n_past_checkpoint = llama_get_kv_cache_used_cells(ctx);
    
    // Decode prompt into KV cache
    llama_decode(ctx, llama_batch_get_one(tokens.data(), tokens.size(), 
                                          n_past_checkpoint, seq_id));
    
    std::string response;
    int n_tokens_generated = 0;
    int n_past = n_past_checkpoint + tokens.size();
    
    while (true) {
        // Check for interruption BEFORE sampling (more responsive)
        if (stop_flag.load()) {
            std::cout << "⏸️  Response interrupted for call " << call_id 
                      << " after " << n_tokens_generated << " tokens" << std::endl;
            
            // CRITICAL: Rollback KV cache to checkpoint (remove partial response)
            llama_kv_cache_seq_rm(ctx, seq_id, n_past_checkpoint, -1);
            
            return "";  // Discard partial response, cache is clean
        }
        
        // Sample next token
        llama_token id = llama_sampling_sample(ctx, NULL, seq_id);
        if (llama_token_is_eog(llama_get_model(ctx), id)) break;
        
        // Decode token (adds to KV cache)
        response += llama_token_to_piece(ctx, id);
        llama_decode(ctx, llama_batch_get_one(&id, 1, n_past, seq_id));
        n_past++;
        n_tokens_generated++;
        
        // Check for interruption every 5 tokens (~every 50ms at 100 tok/s)
        if (n_tokens_generated % 5 == 0 && stop_flag.load()) {
            // Rollback KV cache (remove partial response tokens)
            llama_kv_cache_seq_rm(ctx, seq_id, n_past_checkpoint, -1);
            
            std::cout << "⏸️  Response interrupted for call " << call_id 
                      << " (discarded " << n_tokens_generated << " tokens)" << std::endl;
            return "";
        }
        
        // Safety limit: 50 tokens max (for German "short and sharp" responses)
        if (n_tokens_generated >= 50) break;
    }
    
    return response;
}
```

**Key Design Points**:
1. **Checkpoint Before Generation**: Save `n_past` before generating response tokens
2. **KV Cache Rollback**: Use `llama_kv_cache_seq_rm(ctx, seq_id, checkpoint, -1)` to remove partial response
3. **Sequence ID**: Use per-call `seq_id` to isolate conversations in shared context
4. **Early Check**: Check `stop_flag` before sampling (more responsive than checking after decode)

**Triggering Interruption**:
- When new transcription arrives from Whisper while generating
- Set per-call `stop_flag` atomic bool
- Generation loop checks flag every 5 tokens
- On interrupt: Rollback KV cache, discard partial text, ready for new input

**Testing KV Cache Correctness**:
- Verify conversation context preserved after interruption
- Test: Interrupt mid-response, send new input, verify response uses full history
- Monitor `llama_get_kv_cache_used_cells()` to detect leaks

---

### 2.6 Performance Monitoring

**Metrics to Collect** (optional, low overhead):
```cpp
struct CallMetrics {
    int call_id;
    std::chrono::steady_clock::time_point call_start;
    std::chrono::steady_clock::time_point call_end;
    
    // Per-stage latencies (in milliseconds)
    std::vector<int> vad_latencies;
    std::vector<int> whisper_latencies;
    std::vector<int> llama_latencies;
    std::vector<int> kokoro_latencies;
    
    // Throughput
    int total_segments;
    int total_tokens_generated;
    
    void export_to_csv(const std::string& path);
};
```

**Collection Points**:
- Whisper: Start timer on VAD trigger, stop on transcription complete
- LLaMA: Start on text receive, stop on response complete
- Kokoro: Start on text receive, stop on audio synthesis complete

**Export Format and Aggregation**:
- **File Location**: Each service writes to `/tmp/metrics/<service_name>_<call_id>.csv`
  - Example: `/tmp/metrics/whisper_123.csv`, `/tmp/metrics/llama_123.csv`
- **Timestamp Format**: ISO 8601 with milliseconds (e.g., `2026-02-09T12:00:01.234+01:00`)
- **Real-time vs. Post-processing**: Files written during call, aggregated after call ends
- **Aggregation**: Integration test harness merges all service CSVs for call_id into single timeline

**CSV Schema**:
```csv
call_id,stage,event,timestamp,latency_ms
123,vad,start,2026-02-09T12:00:01.149+01:00,
123,vad,end,2026-02-09T12:00:01.234+01:00,85
123,whisper,start,2026-02-09T12:00:01.234+01:00,
123,whisper,end,2026-02-09T12:00:01.654+01:00,420
123,llama,start,2026-02-09T12:00:01.654+01:00,
123,llama,end,2026-02-09T12:00:01.934+01:00,280
123,kokoro,start,2026-02-09T12:00:01.934+01:00,
123,kokoro,end,2026-02-09T12:00:02.129+01:00,195
```

**Aggregated Metrics** (computed by test harness):
- End-to-end latency: `kokoro_end - vad_start = 980ms`
- Per-stage breakdown for bottleneck identification

**Disk Management**: Each call writes approximately 1KB of CSV data. For 10,000 calls, this totals ~10MB. For production deployments or long-running stress tests, implement log rotation (delete files older than 7 days) or use in-memory aggregation to prevent disk exhaustion.

---

## 3. Source Code Structure Changes

### 3.1 New Files
None. All changes within existing 6 service files.

### 3.2 Modified Files

#### 3.2.1 `sip-client-main.cpp`
**Changes**:
- Add `--instance-id` CLI argument parsing
- Add `ControlSignalSender` class
- Modify `handle_invite()` to send CALL_START
- Modify `handle_bye()` to send CALL_END

**Lines of Code Added**: ~80 LOC

#### 3.2.2 `inbound-audio-processor.cpp`
**Changes**:
- Add `ControlListener` class (Unix socket receiver)
- Add `ControlForwarder` class (Unix socket sender)
- Modify `cleanup_inactive_calls()` to forward CALL_END
- Add explicit call_id logging on reconnection

**Lines of Code Added**: ~120 LOC

#### 3.2.3 `whisper-service.cpp`
**Changes**:
- Expand listener loop from 10 ports to 100 ports
- Add `ControlListener` for Unix socket
- Add `TranscriptionBuffer` class (LLaMA disconnect buffering)
- Enhance VAD algorithm with 3-window hysteresis
- Add `VADMetrics` struct and logging

**Lines of Code Added**: ~200 LOC

#### 3.2.4 `llama-service.cpp`
**Changes**:
- Add `ConversationManager` class
- Add `InputMonitor` class (interruption detection)
- Refactor generation to token-by-token with cancellation checks
- Add sentence completion buffer (wait for `.?!` or timeout)
- Add `ControlListener` for Unix socket

**Lines of Code Added**: ~250 LOC

#### 3.2.5 `kokoro_service.py`
**Changes**:
- Add `ControlListener` class (Python threading)
- Add Unix socket sender for forwarding CALL_END
- Add explicit cleanup on CALL_END signal

**Lines of Code Added**: ~60 LOC

#### 3.2.6 `outbound-audio-processor.cpp`
**Changes**:
- Extend control socket handler to parse CALL_END
- Add per-call stop flags for RTP scheduling threads

**Lines of Code Added**: ~40 LOC

**Total New Code**: ~750 LOC across 6 files

### 3.3 Build System Changes

**CMakeLists.txt** (root):
- No changes required for control sockets (standard POSIX)
- Existing build commands remain unchanged:
  ```bash
  mkdir -p build && cd build
  cmake ..
  make
  ```

**Test Executables**:
- Add test targets in tests/ subdirectory
- New CMake targets:
  - `test_inbound_processor` 
  - `test_whisper_vad`
  - `test_outbound_timing`
- Python tests don't require CMake changes (standalone scripts)

**Dependencies**:
- No new external dependencies
- All Unix socket code uses existing POSIX headers (`<sys/un.h>`, `<sys/socket.h>`)
- llama.cpp KV cache functions already available in current version

**System Requirements**:
- Increase file descriptor limit for 100 Whisper listeners:
  ```bash
  ulimit -n 4096  # Required before starting Whisper service
  ```
- Add to service startup script or document in README

---

## 4. Backward Compatibility

### 4.1 Breaking Changes
This is a **breaking release** that requires coordinated upgrade of all services:

**Not Compatible**:
- Old SIP client with new Inbound Processor (expects CALL_START signal)
- Old services without control socket listeners (will miss CALL_END signals)
- Whisper service with only 10 ports vs. new 100-port requirement

**Migration Path**:
1. Stop all running services
2. Build and deploy all 6 updated services simultaneously
3. Restart in order: Outbound → Kokoro → LLaMA → Whisper → Inbound → SIP Client
4. No data migration needed (services are stateless)

### 4.2 Optional Features
**Control Signals**: While signals improve cleanup, services continue operating without them (graceful degradation)
- Missing CALL_START: Services auto-activate on first data packet (current behavior)
- Missing CALL_END: Services rely on TCP connection close and 60s timeout

**Metrics Collection**: Optional, disabled by default
- Enable via CLI flag: `--enable-metrics`
- No performance impact when disabled

### 4.3 Configuration Compatibility
**Unchanged**:
- Port numbers (except Whisper expansion from 10 to 100)
- RTP packet format (4-byte call_id prefix)
- Audio formats (G.711, PCM float32)
- SIP protocol compatibility

**New**:
- SIP Client `--instance-id` argument (default: 0 for single instance)
- Control socket paths in `/tmp/` (auto-created, no config needed)

---

## 5. Data Model / API / Interface Changes

### 5.1 Control Protocol (New)
**Transport**: Unix datagram sockets  
**Endpoints**:
- `/tmp/inbound-audio-processor.ctrl`
- `/tmp/whisper-service.ctrl`
- `/tmp/llama-service.ctrl`
- `/tmp/kokoro-service.ctrl`
- `/tmp/outbound-audio-processor.ctrl`

**Message Format**:
```
CALL_START:<call_id>
CALL_END:<call_id>
```

**No changes to existing data protocols** (UDP RTP, TCP streaming remain unchanged).

### 5.2 CLI Interface Changes

**SIP Client**:
```bash
# Before
./bin/sip-client <user> <server> <port>

# After
./bin/sip-client <user> <server> <port> [--instance-id <0-9>]
```

**All other services**: No CLI changes.

### 5.3 Port Allocation

**Before**:
- Whisper: 13001-13010 (10 ports)
- Kokoro: 8090-8099 (10 ports)

**After**:
- Whisper: 13000-13099 (100 ports, modulo mapped)
- Kokoro: 8090-8189 (100 ports, modulo mapped)

**RTP Ports** (unchanged):
- SIP Client local RTP: Dynamic 10000+call_id
- SIP signaling: Dynamic per instance

---

## 5. Delivery Phases

### Phase 1: Call Lifecycle Signaling (REQ-2.x)
**Goal**: Implement CALL_START/CALL_END propagation through entire pipeline.

**Tasks**:
1. Implement Unix socket infrastructure in all 6 services
2. SIP Client: Send CALL_START on INVITE, CALL_END on BYE
3. Each service: Forward signals downstream
4. Each service: Clean up resources on CALL_END
5. Test: Single call with manual BYE, verify cleanup in all services

**Verification**:
- Log messages at each stage confirm signal propagation
- Memory inspection shows resource cleanup (no leaks)
- Test script: Start call, send BYE, check all service logs
- CoreML warm-up: Run dummy transcription on Whisper startup to warm up model (avoids 200-500ms first-call latency spike)

**Estimated Effort**: 2-3 days

---

### Phase 2: Multi-Instance SIP Client (REQ-1.x)
**Goal**: Support multiple SIP client instances with collision-free call IDs.

**Tasks**:
1. Add `--instance-id` CLI argument to SIP Client
2. Implement ID offset calculation (instance_id * 1000)
3. Test: Launch 3 instances with IDs 0, 1, 2
4. Verify: No call ID collisions, no port conflicts
5. Stress test: 10 concurrent calls across 3 instances

**Verification**:
- Run 3 instances simultaneously, accept calls on each
- Confirm call IDs are unique (inspect logs)
- Performance: All 10 calls complete successfully

**Estimated Effort**: 1 day

---

### Phase 3: Crash Resilience (REQ-3.x)
**Goal**: All services survive partner crashes and auto-reconnect.

**Tasks**:
1. Inbound Processor: Already implements dump-to-null, verify logging
2. Whisper Service: Add transcription buffering (max 10) for LLaMA disconnect
3. LLaMA Service: Discard responses if Kokoro unavailable
4. Kokoro Service: Verify dump-to-null, add reconnection logging
5. Test: Kill each service during active call, verify auto-recovery

**Verification**:
- Test matrix: Kill each of 5 services (Inbound, Whisper, LLaMA, Kokoro, Outbound)
- Verify: No crashes in partner services
- Verify: Reconnection within 3 seconds
- Memory: No accumulation (RSS stable after restart)

**Estimated Effort**: 2 days

---

### Phase 4: VAD Optimization (REQ-4.x)
**Goal**: Improve segmentation accuracy for German speech, add metrics.

**Tasks**:
1. Implement 3-window hysteresis in Whisper VAD
2. Add sustained silence verification (500ms)
3. Implement VADMetrics struct and logging
4. Collect test corpus: 50 German sentences
5. Benchmark: Measure segmentation accuracy (>95% target)

**Verification**:
- Test corpus: 50 German sentences (5-30 words each)
- Measure: Correct segmentations, false cuts, latency
- Target: >95% accuracy, <1s latency start-of-speech to processing
- Export metrics to CSV for analysis

**Estimated Effort**: 2 days

---

### Phase 5: LLaMA Enhancements (REQ-5.x)
**Goal**: Sentence completion detection and response interruption.

**Tasks**:
1. Implement sentence buffer (wait for `.?!` or 2s timeout)
2. Refactor generation to token-by-token with stop_flag checks
3. Add InputMonitor class for interruption signaling
4. Test: Send incomplete sentence, verify no premature response
5. Test: Interrupt long response with new input, verify <200ms switch

**Verification**:
- Test: Send "Wie ist" + 1s pause + "das Wetter?" → Single response
- Test: Start 50-token response, interrupt at token 10, verify stop <200ms
- Measure: Interruption latency (target <200ms)

**Estimated Effort**: 2-3 days

---

### Phase 6: Integration Testing (REQ-7.x)
**Goal**: Comprehensive end-to-end testing of complete pipeline.

**Tasks**:
1. Develop test harness for automated pipeline testing
2. Test scenario 1: Single call end-to-end (INVITE → conversation → BYE)
3. Test scenario 2: 10 concurrent calls
4. Test scenario 3: Service crash recovery (kill/restart each service individually)
5. Test scenario 4: Long conversation (20+ turns, memory stability)
6. Test scenario 5: Rapid churn (100 calls in 5 minutes)
7. Test scenario 6: Chaos testing (kill random service every 5s during 20-turn conversation)
8. Test scenario 7: Cascade failure (kill Whisper+LLaMA simultaneously, verify recovery)
9. Test scenario 8: Service restart during reconnection (kill service while partner is attempting reconnect)
10. Collect metrics: Latency, memory, CPU, errors

**Verification**:
- All scenarios pass (automated pass/fail)
- Latency: 90th percentile <1.5s end-to-end
- Memory: Stable RSS over 1-hour test
- No crashes or hangs

**Test Infrastructure**:
- Python test harness with SIP message injection
- Mock RTP audio generator (clean German speech samples)
- Metrics collector and aggregator

**Estimated Effort**: 3-4 days

---

### Phase 7: Performance Monitoring (REQ-6.x, NFR-4)
**Goal**: Add optional metrics collection with minimal overhead.

**Tasks**:
1. Implement CallMetrics struct in each service
2. Add timestamp logging at stage boundaries
3. Export metrics to CSV (optional, disabled by default)
4. Validate <1% CPU overhead for metrics collection
5. Benchmark: Compare with/without metrics enabled

**Verification**:
- Run 50-call test with metrics enabled
- Export CSV, analyze latencies (confirm 90th percentile targets)
- CPU usage: <1% difference vs. no metrics

**Estimated Effort**: 1 day

---

## 6. Verification Approach

### 6.1 Unit Testing (Per Service)
Each service will have standalone tests using mock inputs:

**SIP Client Test**:
- Script sends mock INVITE/BYE messages
- Verify: CALL_START/CALL_END signals sent to Unix socket
- Verify: RTP packets forwarded with correct call_id prefix

**Inbound Processor Test**:
- Send mock RTP packets on UDP 9001
- Verify: PCM output on TCP (connect to port 13000+call_id)
- Verify: CALL_END signal forwarded on Unix socket

**Whisper Service Test**:
- Feed PCM audio file (known German sentence)
- Verify: Transcription accuracy (WER <5%)
- Verify: VAD correctly segments speech/silence

**LLaMA Service Test**:
- Send text input on TCP 8083
- Verify: Response quality (manual review)
- Verify: Latency <300ms for 15-word response
- Verify: Interruption works (<200ms)

**Kokoro Service Test**:
- Send German text to TCP 8090
- Verify: Audio output quality (SNR >30dB)
- Verify: Latency <200ms for 15 words

**Outbound Processor Test**:
- Send mock PCM on TCP
- Verify: G.711 RTP output on UDP 9002
- Verify: 20ms timing accuracy (jitter <5ms)

**Test Location**: `tests/` directory, one script per service.

### 6.2 Integration Testing
**Harness**: Python script that orchestrates full pipeline:
1. Start all 6 services
2. Send mock SIP INVITE
3. Stream RTP audio (pre-recorded German speech)
4. Capture audio output from Outbound Processor
5. Send BYE, verify cleanup

**Scenarios**:
- Single call (baseline)
- 10 concurrent calls (scalability)
- Service restarts during call (resilience)
- 20-turn conversation (memory stability)
- 100 rapid calls (churn)

**Metrics Collection**:
- Parse service logs for latency timestamps
- Monitor RSS memory every 10s
- Aggregate results to CSV

**Test Location**: `tests/pipeline_integration_test.py`

### 6.3 Lint and Type Checking
**C++ Services**:
```bash
# Build with all warnings
cmake -DCMAKE_CXX_FLAGS="-Wall -Wextra -Werror" ..
make
```

**Python Services**:
```bash
# Type checking
mypy kokoro_service.py --strict

# Linting
ruff check kokoro_service.py
```

### 6.4 Acceptance Criteria
- All unit tests pass (6/6 services)
- All integration scenarios pass (5/5)
- No memory leaks (valgrind or stable RSS over 1 hour)
- Latency targets met (90th percentile <1.5s)
- VAD accuracy >95% on test corpus
- Interruption latency <200ms
- 100 concurrent calls supported (stress test)

---

## 7. Risk Assessment

### 7.1 Technical Risks

**Risk**: llama.cpp interruption may not work cleanly (KV cache corruption)  
**Mitigation**: Test thoroughly with sequence IDs; fall back to discarding entire generation if needed  
**Impact**: Medium (affects REQ-5.2)

**Risk**: 100 TCP listener threads in Whisper may exhaust file descriptors  
**Mitigation**: Increase `ulimit -n` to 4096; listeners are lightweight (blocked in accept)  
**Impact**: Low

**Risk**: VAD optimization may introduce latency (sustained silence check)  
**Mitigation**: Keep verification window short (500ms); benchmark before/after  
**Impact**: Low

**Risk**: Unix socket buffering may cause signal loss under high load  
**Mitigation**: Use SOCK_DGRAM (datagram) mode; signals are idempotent (OK to miss occasional CALL_START)  
**Impact**: Low

### 7.2 Performance Risks

**Risk**: 100 concurrent calls may exceed M1 GPU capacity  
**Mitigation**: Profile with Activity Monitor; reduce max calls if needed (throttle at SIP client)  
**Impact**: Medium (affects NFR-2)

**Risk**: Metrics collection overhead >1%  
**Mitigation**: Make metrics optional (disabled by default); use lock-free atomics for counters  
**Impact**: Low

### 7.3 Integration Risks

**Risk**: Multi-instance SIP clients may conflict on RTP ports  
**Mitigation**: Use instance_id * 1000 offset + dynamic binding with retry  
**Impact**: Low

**Risk**: Crash recovery may cause audio glitches (user-facing)  
**Mitigation**: Acceptable trade-off for resilience; document behavior  
**Impact**: Low (expected in telephony)

---

## 8. Assumptions and Dependencies

### 8.1 Assumptions
- Services run on localhost (127.0.0.1) with <1ms network latency
- SIP server is external and reliable (no fallback needed)
- German is primary language (VAD and LLaMA tuned for German)
- Apple Silicon hardware available (M1/M2/M3)
- Operating system: macOS 12+ (Unix socket support)

### 8.2 Dependencies
- **whisper.cpp**: Must support CoreML backend (already confirmed)
- **llama.cpp**: Must support Metal backend and token-by-token generation (already confirmed)
- **Kokoro**: PyTorch with MPS support (already confirmed)
- **Build Tools**: CMake 3.22+, Clang with C++17
- **Runtime**: Python 3.9+ with torch, numpy

### 8.3 External Constraints
- No changes to SIP protocol (must remain compatible with external SIP server)
- No changes to G.711/RTP standards (telephony interoperability)
- Services must remain standalone executables (no consolidation into monolith)

---

## 9. Success Metrics

### 9.1 Functional Success
- ✅ All REQ-1.x through REQ-7.x acceptance criteria met
- ✅ All unit tests pass (6/6 services)
- ✅ All integration tests pass (5/5 scenarios)

### 9.2 Performance Success
- ✅ End-to-end latency: 90th percentile <1.5s
- ✅ VAD accuracy: >95% on German test corpus
- ✅ Interruption latency: <200ms
- ✅ Crash recovery: <3s reconnection time
- ✅ Concurrent calls: 100 calls supported with stable performance

### 9.3 Non-Functional Success
- ✅ No memory leaks (stable RSS over 1-hour test)
- ✅ No crashes under load (100 call stress test)
- ✅ Code maintainability: Single file per service (no change)
- ✅ Metrics overhead: <1% CPU (optional collection)

---

## 10. Implementation Timeline

| Phase | Effort | Duration | Dependencies |
|-------|--------|----------|--------------|
| Phase 0: Baseline Measurement | 1 day | Pre-Week 1 | None |
| Phase 1: Call Lifecycle | 2-3 days | Week 1 | Phase 0 |
| Phase 2: Multi-Instance | 1 day | Week 1 | Phase 1 |
| Phase 3: Crash Resilience | 2 days | Week 1-2 | Phase 1 |
| Phase 4: VAD Optimization | 2 days | Week 2 | Phase 1 |
| Phase 5: LLaMA Enhancements | 2-3 days | Week 2 | Phase 1, 3 |
| Phase 6: Integration Testing | 3-4 days | Week 3 | All phases |
| Phase 7: Performance Monitoring | 1 day | Week 3 | Phase 6 |

**Total Estimated Effort**: 14-18 days  
**Calendar Duration**: ~3 weeks (with some parallelization)

### Phase 0: Baseline Measurement (Pre-Implementation)
**Goal**: Establish current system performance metrics for comparison.

**Tasks**:
1. Run current system with 10 concurrent calls (no code changes)
2. Measure: end-to-end latency, per-stage latency, memory usage, CPU utilization
3. Benchmark VAD accuracy on test corpus (using current algorithm)
4. Document results in `baseline_metrics.md`
5. Identify current bottlenecks

**Verification**:
- Baseline metrics documented and reviewed
- Clear understanding of current performance envelope
- Regression detection: Phase 6 results compared against baseline

**Estimated Effort**: 1 day

---

## 11. Testing Infrastructure

### 11.1 Test Files

**Per-Service Tests** (location: `tests/`):
- `test_sip_client.py` - Mock INVITE/BYE injection (Python for TCP socket scripting)
- `test_inbound_processor.cpp` - Mock RTP sender (C++ for performance)
- `test_whisper_vad.cpp` - VAD accuracy benchmark (C++ for direct buffer access)
- `test_llama_interruption.py` - Response cancellation (Python for timing control)
- `test_kokoro_latency.py` - TTS performance (Python for Kokoro integration)
- `test_outbound_timing.cpp` - RTP schedule verification (C++ for timing precision)

**Integration Tests**:
- `tests/pipeline_integration_test.py` - Main test harness
- `tests/test_corpus/` - German audio samples (50 sentences)

**Language Choice**: Python tests used for services requiring complex timing, TCP connection management, and scripting flexibility. C++ tests used for performance-critical and buffer-level validation.

### 11.2 Test Data Generation

**Audio Corpus Creation**:
- **Source**: 50 German sentences from Harvard Open Speech Corpus or custom set
- **Synthesis Method**: 
  1. Use Kokoro TTS service (already in pipeline) to generate clean audio
  2. Alternative: Record native German speakers for production testing
- **Format**: 
  - Input format: WAV 24kHz float32 (Kokoro output)
  - Converted to: G.711 μ-law 8kHz (telephony input simulation)
- **Duration**: 5-30 words per sentence (3-15 seconds)
- **Generation Script**: `scripts/generate_test_corpus.py`

**Script Implementation** (Direct Kokoro Import):
```python
#!/usr/bin/env python3
"""
Test corpus generator for WhisperTalk integration testing.
Uses Kokoro library directly (not the TCP service).
"""
import sys
import numpy as np
from pathlib import Path

try:
    from kokoro import KPipeline
    import soundfile as sf  # For writing WAV files
except ImportError:
    print("Error: Required packages not installed")
    print("Install with: pip install kokoro soundfile")
    sys.exit(1)

# German test sentences (50 total)
sentences = [
    "Wie ist das Wetter heute?",
    "Ich möchte einen Termin vereinbaren.",
    "Können Sie mir helfen?",
    "Guten Tag, wie geht es Ihnen?",
    "Ich habe eine Frage zu meiner Bestellung.",
    "Können Sie das bitte wiederholen?",
    "Vielen Dank für Ihre Hilfe.",
    "Auf Wiedersehen und einen schönen Tag.",
    "Ich verstehe das nicht ganz.",
    "Könnten Sie bitte langsamer sprechen?",
    # ... 40 more sentences (expand to 50 for full corpus)
]

def generate_corpus():
    # Initialize Kokoro with German model
    print("Loading Kokoro pipeline (German)...")
    pipeline = KPipeline(lang_code='a', device='mps')  # Use 'a' for multi-lang
    
    # Create output directory
    output_dir = Path('tests/test_corpus')
    output_dir.mkdir(parents=True, exist_ok=True)
    
    # Generate ground truth JSON
    transcripts = {}
    
    for i, text in enumerate(sentences):
        print(f"Synthesizing {i+1}/{len(sentences)}: {text[:50]}...")
        
        # Generate audio using Kokoro (returns numpy array at 24kHz)
        audio = pipeline(text, voice='af_bella', speed=1.0, lang='de')
        
        # Save 24kHz version
        wav_24k = output_dir / f'sent_{i:02d}_24k.wav'
        sf.write(wav_24k, audio, 24000)
        
        # Downsample to 8kHz for telephony simulation
        # Simple decimation (every 3rd sample: 24000/3 = 8000)
        audio_8k = audio[::3]
        wav_8k = output_dir / f'sent_{i:02d}_8k.wav'
        sf.write(wav_8k, audio_8k, 8000, subtype='PCM_16')
        
        # Add to ground truth
        transcripts[f'sent_{i:02d}_8k.wav'] = text
    
    # Write ground truth JSON
    import json
    with open(output_dir / 'transcripts.json', 'w', encoding='utf-8') as f:
        json.dump(transcripts, f, ensure_ascii=False, indent=2)
    
    print(f"\n✅ Generated {len(sentences)} audio files")
    print(f"📁 Output: {output_dir}")
    print(f"📋 Ground truth: {output_dir / 'transcripts.json'}")

if __name__ == '__main__':
    generate_corpus()
```

**Alternative: Using ffmpeg for resampling** (if higher quality needed):
```bash
# After generating 24kHz files with Kokoro
for f in tests/test_corpus/sent_*_24k.wav; do
    ffmpeg -i "$f" -ar 8000 -ac 1 -acodec pcm_mulaw \
           "${f/_24k/_8k_mulaw}.wav"
done
```

**Note on Kokoro Service**: The `kokoro_service.py` is a TCP service for runtime synthesis, not a CLI tool. For test corpus generation, we use the Kokoro library directly via Python import. This avoids starting/stopping the service and simplifies the generation script.

**Ground Truth**: `tests/test_corpus/transcripts.json`
```json
{
  "sent_00.wav": "Wie ist das Wetter heute?",
  "sent_01.wav": "Ich möchte einen Termin vereinbaren.",
  ...
}
```

**Validation**: Run Whisper on corpus, measure Word Error Rate (WER) <5%

### 11.3 Test Execution Commands

```bash
# Build all services
cd build && cmake .. && make

# Run per-service tests
./tests/test_inbound_processor
./tests/test_whisper_vad
./tests/test_outbound_timing
python3 tests/test_sip_client.py
python3 tests/test_llama_interruption.py
python3 tests/test_kokoro_latency.py

# Run integration tests
python3 tests/pipeline_integration_test.py --scenario all

# Stress test (100 calls)
python3 tests/pipeline_integration_test.py --scenario stress --calls 100

# Memory leak check (requires valgrind)
valgrind --leak-check=full ./bin/whisper-service <model>
```

### 11.4 CI/CD Integration (Optional)
If CI/CD is available, add:
```yaml
test:
  script:
    - cmake build && make
    - make test  # Runs all C++ tests
    - python3 -m pytest tests/  # Runs all Python tests
  artifacts:
    paths:
      - metrics/*.csv
```

---

## 12. Documentation Updates

**Files to Update**:
1. [.zencoder/rules/sip-client.md](./sip-client.md) - Add multi-instance section, CALL_START/END signaling
2. [.zencoder/rules/inbound-audio-processor.md](./inbound-audio-processor.md) - Add control socket details
3. [.zencoder/rules/whisper-service.md](./whisper-service.md) - Update VAD algorithm, add 100-port range
4. [.zencoder/rules/llama-service.md](./llama-service.md) - Add interruption mechanism, sentence completion
5. [.zencoder/rules/kokoro-service.md](./kokoro-service.md) - Add control socket details
6. [.zencoder/rules/outbound-audio-processor.md](./outbound-audio-processor.md) - Add CALL_END handling

**Content**: Update "Internal Function" and "Inbound/Outbound Connections" sections to reflect new control plane.

---

## 13. Glossary

- **VAD**: Voice Activity Detection - algorithm to distinguish speech from silence
- **CoreML**: Apple's ML framework with hardware acceleration (Neural Engine + GPU)
- **Metal**: Apple's GPU programming framework (compute shaders)
- **MPS**: Metal Performance Shaders - optimized ML ops for PyTorch
- **KV Cache**: Key-Value cache in transformers (stores attention states for conversation context)
- **Unix Socket**: Inter-process communication using filesystem paths (lower latency than TCP localhost)
- **RTP**: Real-time Transport Protocol (audio streaming over UDP)
- **G.711**: ITU-T telephony codec (μ-law variant, 64 kbps)
- **SIP**: Session Initiation Protocol (call signaling)

---

## Document History

| Version | Date | Author | Changes |
|---------|------|--------|---------|
| 1.0 | 2026-02-09 | Zencoder | Initial technical specification based on requirements.md |
| 1.1 | 2026-02-09 | Zencoder | Critical fixes: Unix socket error handling, VAD buffer management, KV cache rollback, signal-first ordering, sentence buffer edge cases, metrics standardization, test data generation |
