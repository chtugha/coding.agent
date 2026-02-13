# Phase 1.2 - Call Lifecycle Signaling Verification Guide

## Overview
Phase 1.2 implements CALL_START/CALL_END signal flow through the entire pipeline to enable proper call lifecycle management.

## Changes Summary

### 1. SIP Client ([./sip-client-main.cpp](../sip-client-main.cpp))
- **CALL_START**: Sends signal on INVITE accept (line 198)
- **CALL_END**: Sends signal on BYE received (line 213)
- Already implemented in Phase 1.1

### 2. Inbound Audio Processor ([./inbound-audio-processor.cpp](../inbound-audio-processor.cpp))
- **CALL_START** (lines 151-159):
  - Pre-allocates CallState for the call_id
  - Updates last_activity timestamp
  - Forwards signal to Whisper Service
- **CALL_END** (lines 160-182):
  - Forwards signal to Whisper Service immediately
  - Spawns cleanup thread with 200ms grace period
  - Closes TCP socket and removes CallState

### 3. Whisper Service ([./whisper-service.cpp](../whisper-service.cpp))
- **CALL_START** (lines 162-169):
  - Pre-allocates WhisperCall object
  - Prepares listener (no-op if already active)
  - Forwards signal to LLaMA Service
- **CALL_END** (lines 170-194):
  - Forwards signal to LLaMA Service
  - Stops transcription by setting connected=false
  - Closes TCP socket
  - Clears audio buffer and VAD state
  - Removes WhisperCall from map

### 4. LLaMA Service ([./llama-service.cpp](../llama-service.cpp))
- **CALL_START** (lines 218-225):
  - Pre-allocates LlamaCall object with unique sequence ID
  - Logs sequence ID allocation
  - Forwards signal to Kokoro Service
- **CALL_END** (lines 226-242):
  - Forwards signal to Kokoro Service
  - Clears KV cache for the sequence ID using `llama_memory_seq_rm()`
  - Removes LlamaCall from map

### 5. Kokoro Service ([./kokoro_service.py](../kokoro_service.py))
- **CALL_START** (lines 394-402):
  - Pre-allocates resources in registered_calls map
  - Records timestamp
  - Forwards signal to Outbound Audio Processor
- **CALL_END** (lines 403-415):
  - Forwards signal to Outbound Audio Processor
  - Closes outbound TCP connection
  - Removes call_id from registered_calls map

### 6. Outbound Audio Processor ([./outbound-audio-processor.cpp](../outbound-audio-processor.cpp))
- **CALL_START** (lines 138-144):
  - Pre-allocates CallState
  - Updates last_activity timestamp
- **CALL_END** (lines 145-148):
  - Calls end_call() to cleanup
- **end_call()** (lines 202-224):
  - Stops RTP scheduling by setting connected=false
  - Closes TCP and listen sockets
  - Clears audio buffer
  - Removes CallState from map

## Signal Flow

```
SIP Client (INVITE) 
  ↓ CALL_START:call_id
Inbound Audio Processor
  ↓ (forward + pre-allocate CallState)
Whisper Service
  ↓ (forward + pre-allocate WhisperCall)
LLaMA Service
  ↓ (forward + pre-allocate sequence ID)
Kokoro Service
  ↓ (forward + pre-allocate resources)
Outbound Audio Processor
  (pre-allocate CallState)
```

```
SIP Client (BYE)
  ↓ CALL_END:call_id
Inbound Audio Processor
  ↓ (forward + cleanup after 200ms)
Whisper Service
  ↓ (forward + stop transcription + close TCP)
LLaMA Service
  ↓ (forward + clear KV cache + cleanup)
Kokoro Service
  ↓ (forward + close connections + cleanup)
Outbound Audio Processor
  (stop RTP + close sockets + cleanup)
```

## Manual Verification Steps

### Prerequisites
1. Start all 6 services in separate terminals:
   ```bash
   # Terminal 1
   ./bin/inbound-audio-processor
   
   # Terminal 2
   ./bin/whisper-service models/whisper/ggml-base.en-q8_0.bin
   
   # Terminal 3
   ./bin/llama-service models/llama/Llama-3.2-1B-Instruct-Q8_0.gguf
   
   # Terminal 4
   python3 kokoro_service.py
   
   # Terminal 5
   ./bin/outbound-audio-processor
   
   # Terminal 6 (optional - SIP Client)
   ./bin/sip-client user server.example.com 5060
   ```

2. Verify all services are listening on their control sockets:
   ```bash
   ls -la /tmp/*.ctrl
   ```
   Expected output:
   ```
   /tmp/inbound-audio-processor.ctrl
   /tmp/whisper-service.ctrl
   /tmp/llama-service.ctrl
   /tmp/kokoro-service.ctrl
   /tmp/outbound-audio-processor.ctrl
   ```

### Test 1: Signal Propagation
1. Run the test script:
   ```bash
   python3 tests/test_call_lifecycle.py 1
   ```

2. Expected log sequence (check each service terminal):
   
   **Inbound Audio Processor**:
   ```
   🚦 CALL_START received for call_id 1
   📋 Pre-allocated CallState for call_id 1
   ```
   
   **Whisper Service**:
   ```
   🚦 CALL_START received for call_id 1
   📋 Prepared Whisper listener for call_id 1
   ```
   
   **LLaMA Service**:
   ```
   🚦 CALL_START received for call_id 1
   📋 Pre-allocated sequence ID 0 for call_id 1
   ```
   
   **Kokoro Service**:
   ```
   🚦 CALL_START received for call_id 1
   📋 Pre-allocated resources for call_id 1
   ```
   
   **Outbound Audio Processor**:
   ```
   🚦 CALL_START received for call_id 1
   📋 Pre-allocated resources for call_id 1
   ```

3. After 2 seconds, verify CALL_END propagation:
   
   **Inbound Audio Processor**:
   ```
   🚦 CALL_END received for call_id 1
   🧹 Cleaned up call_id 1 (200ms grace period)
   ```
   
   **Whisper Service**:
   ```
   🚦 CALL_END received for call_id 1
   🧹 Stopped transcription and cleaned up call_id 1
   ```
   
   **LLaMA Service**:
   ```
   🚦 CALL_END received for call_id 1
   🧹 Cleared conversation and stopped generation for call_id 1
   ```
   
   **Kokoro Service**:
   ```
   🚦 CALL_END received for call_id 1
   🧹 Stopped synthesis and closed connections for call_id 1
   ```
   
   **Outbound Audio Processor**:
   ```
   🚦 CALL_END received for call_id 1
   🧹 Stopped RTP scheduling and closed connections for call_id 1
   ```

### Test 2: Timing Verification
1. Record timestamps from logs
2. Verify CALL_END cleanup completes within 500ms total

### Test 3: Memory Leak Test
1. Run 10 consecutive calls:
   ```bash
   for i in {1..10}; do
     python3 tests/test_call_lifecycle.py $i
     sleep 1
   done
   ```

2. Monitor memory usage:
   ```bash
   # Before test
   ps aux | grep -E "inbound-audio-processor|whisper-service|llama-service|kokoro_service|outbound-audio-processor"
   
   # After test (should be similar)
   ps aux | grep -E "inbound-audio-processor|whisper-service|llama-service|kokoro_service|outbound-audio-processor"
   ```

3. Verify no resource accumulation (RSS should be stable)

## Success Criteria
- ✅ All services receive CALL_START signal
- ✅ All services log pre-allocation messages
- ✅ All services receive CALL_END signal
- ✅ All services log cleanup messages
- ✅ Signal propagation completes within 500ms
- ✅ No crashes or errors in any service
- ✅ Memory stable after 10 consecutive calls (no leaks)
- ✅ All services compile without errors or warnings

## Next Steps
After successful verification:
1. Proceed to Phase 1.3 - CoreML Warm-up
2. Consider adding automated integration tests
3. Test with real SIP calls using full pipeline

## Troubleshooting

### Signal not propagating
- Check that all services are running
- Verify Unix sockets exist in `/tmp/`
- Check for permission issues on Unix sockets
- Review service logs for connection errors

### Cleanup not happening
- Verify 200ms grace period is sufficient for your system
- Check for deadlocks or blocking operations
- Monitor CPU usage during cleanup

### Memory leaks
- Use valgrind or AddressSanitizer for detailed analysis
- Check for circular references or missing `close()` calls
- Verify all `std::shared_ptr` references are released
