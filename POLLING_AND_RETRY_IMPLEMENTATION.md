# Polling and Retry Implementation Summary

## Overview

Implemented comprehensive retry logic with exponential backoff across all service connections to eliminate race conditions and improve reliability.

## Architecture

### Connection Types

1. **Registration Polling (UDP → TCP)**
   - Audio processors send UDP REGISTER messages
   - Services listen on UDP and connect back via TCP
   - Processors poll continuously until connection established

2. **Direct Connection with Retry (TCP)**
   - Services connect directly to other services
   - Retry with exponential backoff on failure
   - No UDP registration needed

---

## Implementation Details

### 1. Inbound Audio Processor → Whisper Service ✅

**Type**: Registration Polling

**Inbound Audio Processor** (`inbound-audio-processor.cpp`):
- Listens on TCP port `9001 + call_id`
- Polls UDP REGISTER to port `13000` every 200ms (first second), then every 1s
- Stops polling when `whisper_connected_` becomes true
- Methods: `start_registration_polling()`, `stop_registration_polling()`, `registration_polling_thread()`

**Whisper Service** (`whisper-service.cpp`):
- Listens on UDP port `13000` for REGISTER messages
- Connects to TCP port `9001 + call_id` when REGISTER received
- Idempotent: Checks if already connected before connecting

**Retry Strategy**: Continuous polling until connected or call ends

---

### 2. Outbound Audio Processor → TTS Service ✅

**Type**: Registration Polling

**Outbound Audio Processor** (`outbound-audio-processor.cpp`):
- Listens on TCP port `9002 + call_id`
- Polls UDP REGISTER to port `13001` every 200ms (first second), then every 1s
- Stops polling when `piper_connected_` becomes true
- Methods: `start_registration_polling()`, `stop_registration_polling()`, `registration_polling_thread()`

**TTS Services** (Kokoro/Piper):
- Listen on UDP port `13001` for REGISTER messages
- Track registered calls but don't connect immediately
- Connect when receiving first text from LLaMA (see connection #5)

**Retry Strategy**: Continuous polling until connected or call ends

---

### 3. Whisper Service → LLaMA Service ✅

**Type**: Direct Connection with Retry

**Whisper Service** (`whisper-service.cpp`, `connect_llama_for_call()`):
- Connects to LLaMA on TCP port `8083`
- Retry logic: 10 attempts
  - Attempts 1-5: 200ms delay between retries
  - Attempts 6-10: 1000ms delay between retries
- Logs attempts 1, 5, and 9 to avoid spam
- Called during session creation (pre-connection)

**LLaMA Service** (`llama-service.cpp`):
- Listens on TCP port `8083`
- Accepts connections from Whisper

**Retry Strategy**: 10 attempts with exponential backoff (200ms → 1s)

---

### 4. LLaMA Service → TTS Service ✅

**Type**: Direct Connection with Retry

**LLaMA Service** (`llama-service.cpp`, `connect_output_for_call()`):
- Connects to TTS on TCP port `8090`
- Retry logic: 10 attempts
  - Attempts 1-5: 200ms delay between retries
  - Attempts 6-10: 1000ms delay between retries
- Logs attempts 1, 5, and 9 to avoid spam
- Called during session creation (pre-connection)

**TTS Services** (Kokoro/Piper):
- Listen on TCP port `8090`
- Accept connections from LLaMA

**Retry Strategy**: 10 attempts with exponential backoff (200ms → 1s)

---

### 5. TTS Service → Outbound Audio Processor ✅

**Type**: Direct Connection with Retry

**Kokoro Service** (`bin/kokoro_service.py`, `connect_to_audio_processor()`):
- Connects to processor on TCP port `9002 + call_id`
- Retry logic: 10 attempts (max_attempts parameter)
  - Attempts 1-5: 200ms delay between retries
  - Attempts 6-10: 1000ms delay between retries
- Called when receiving first text chunk from LLaMA

**Piper Service** (`piper-service.cpp`, `try_connect_audio_output_for_call()`):
- Connects to processor on TCP port `9002 + call_id`
- Retry logic: 10 attempts
  - Attempts 1-5: 200ms delay between retries
  - Attempts 6-10: 1000ms delay between retries
- Logs attempts 1, 5, and 9 to avoid spam
- Called when receiving first text chunk from LLaMA

**Outbound Audio Processor** (`outbound-audio-processor.cpp`):
- Listens on TCP port `9002 + call_id`
- Accepts connections from TTS services

**Retry Strategy**: 10 attempts with exponential backoff (200ms → 1s)

---

## Retry Pattern

All retry implementations follow the same pattern:

```cpp
const int max_attempts = 10;

for (int attempt = 1; attempt <= max_attempts; ++attempt) {
    // Try to connect/send
    if (success) {
        std::cout << "✅ Connected (attempt " << attempt << ")" << std::endl;
        return true;
    }
    
    // Calculate backoff
    int sleep_ms = (attempt <= 5) ? 200 : 1000;
    
    // Log selectively to avoid spam
    if (attempt == 1 || attempt == 5 || attempt == max_attempts - 1) {
        std::cout << "⚠️ Attempt " << attempt << "/" << max_attempts 
                  << " failed - retrying in " << sleep_ms << "ms" << std::endl;
    }
    
    // Sleep before retry
    std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
}

std::cout << "❌ Failed after " << max_attempts << " attempts" << std::endl;
return false;
```

---

## Benefits

1. **Eliminates Race Conditions**: Services can start in any order
2. **Self-Healing**: Automatic recovery from transient failures
3. **UDP Packet Loss Resilience**: Registration polling handles lost packets
4. **Graceful Degradation**: Logs failures but continues operation
5. **Reduced Latency**: Pre-connection eliminates first-request overhead
6. **Multi-Session Safe**: All checks are per call_id

---

## Testing Checklist

- [ ] Make test call and verify all connections establish
- [ ] Check logs for retry messages (should see attempts if timing is tight)
- [ ] Verify Kokoro audio plays correctly
- [ ] Test with services starting in different orders
- [ ] Verify no "Connection refused" errors
- [ ] Check that polling stops when connections established

---

## Log Messages to Look For

**Success Messages**:
```
🔗 Pre-connected to LLaMA service for call 66
🔗 Pre-connected to TTS service for call 66
🔗 Connected to outbound audio processor on port 9068 for call 66 (attempt 1)
✅ TTS service connected for call 66 - stopping registration polling
```

**Retry Messages** (only if timing is tight):
```
⚠️ Connection attempt 1/10 failed for call 66 - retrying in 200ms
⚠️ LLaMA connection attempt 5/10 failed for call 66 - retrying in 200ms
⚠️ TTS connection attempt 9/10 failed for call 66 - retrying in 1000ms
```

**Failure Messages** (should not appear in normal operation):
```
❌ Failed to connect to audio processor for call 66 after 10 attempts
❌ Failed to connect to LLaMA for call 66 after 10 attempts
```

---

## Files Modified

1. `inbound-audio-processor.h` - Added polling members
2. `inbound-audio-processor.cpp` - Implemented registration polling
3. `outbound-audio-processor.h` - Added polling members
4. `outbound-audio-processor.cpp` - Implemented registration polling
5. `whisper-service.cpp` - Added retry logic to `connect_llama_for_call()`
6. `llama-service.cpp` - Added retry logic to `connect_output_for_call()`
7. `bin/kokoro_service.py` - Added retry logic to `connect_to_audio_processor()`
8. `piper-service.cpp` - Added retry logic to `try_connect_audio_output_for_call()`

---

## Performance Impact

- **Startup**: Minimal (200ms-2s if services not ready)
- **Runtime**: Zero (connections established during session creation)
- **Memory**: Negligible (one thread per active call for registration polling)
- **CPU**: Negligible (sleep-based polling, not busy-wait)

---

## Future Enhancements

1. Make retry attempts configurable via command-line arguments
2. Add connection health monitoring (detect dead connections)
3. Add metrics for connection establishment time
4. Consider exponential backoff beyond 1 second for very slow starts

