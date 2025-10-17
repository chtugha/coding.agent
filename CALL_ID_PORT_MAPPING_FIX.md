# Call ID Port Mapping Architecture Fix

## Problem Summary

The whisper_inbound_sim test simulator has a critical architecture bug that prevents testing multiple Harvard sentence files sequentially. The issue is related to how the simulator manages the transcription receiver (llama-service mock) across multiple tests.

## Root Cause

### Issue 1: TranscriptionReceiver Created Inside Loop (FIXED)
**Original Code** (tests/whisper_inbound_sim.cpp lines 427-446):
```cpp
for (int idx = 0; idx < std::min(argc - 1, 3); ++idx) {
    // ...
    TranscriptionReceiver rx_server;  // ❌ Created inside loop
    if (!rx_server.start_listening()) {  // ❌ Tries to bind port 8083 each time
        std::cerr << "failed to start transcription receiver\n";
        return 1;
    }
    // ...
}
```

**Problem**: The `TranscriptionReceiver` binds to port 8083. Creating it inside the loop means:
- Test 1: Binds to port 8083 ✅
- Test 2: Tries to bind to port 8083 again ❌ (port already in use)
- Test 3: Tries to bind to port 8083 again ❌ (port already in use)

**Solution**: Create `TranscriptionReceiver` ONCE outside the loop and reuse it for all tests.

**Fixed Code**:
```cpp
// Setup transcription receiver ONCE (mimics llama-service on port 8083)
TranscriptionReceiver rx_server;
if (!rx_server.start_listening()) {
    std::cerr << "failed to start transcription receiver\n";
    return 1;
}

for (int idx = 0; idx < std::min(argc - 1, 3); ++idx) {
    // ... test code ...
    
    // Reset for next test
    rx_server.reset_for_next_test();
}

// Cleanup at end
rx_server.cleanup();
```

### Issue 2: Whisper-Service Not Sending BYE to LLaMA Socket (IN PROGRESS)
**Problem**: When whisper-service finishes processing a call, it sends BYE to the audio input socket but NOT to the llama-service socket. This causes the simulator's `receive_loop()` to block forever waiting for more data.

**Current Code** (whisper-service.cpp line 886):
```cpp
send_tcp_bye(socket);  // ✅ Sends BYE to audio input socket
// ❌ Missing: send BYE to llama socket
```

**Solution**: Send BYE to both sockets before closing them.

**Fixed Code** (whisper-service.cpp lines 886-902):
```cpp
send_tcp_bye(socket);
std::cout << "📡 TCP BYE sent to audio input socket for call " << call_id << std::endl;

// Send BYE to llama-service socket
{
    std::lock_guard<std::mutex> tlock(tcp_mutex_);
    auto llama_it = llama_sockets_.find(call_id);
    if (llama_it != llama_sockets_.end() && llama_it->second >= 0) {
        std::cout << "📡 Sending BYE to llama socket for call " << call_id << std::endl;
        send_tcp_bye(llama_it->second);
        close(llama_it->second);
        llama_sockets_.erase(llama_it);
        std::cout << "📡 TCP BYE sent to llama socket for call " << call_id << std::endl;
    } else {
        std::cout << "⚠️  No llama socket found for call " << call_id << std::endl;
    }
}
```

## Architecture Verification

### Correct Call ID Port Mapping

**Production Behavior**:
1. **UDP Registration**: inbound-audio-processor sends `REGISTER:<call_id>` to whisper-service on UDP port 13000
2. **Inbound Audio Connection**: whisper-service connects to `9001 + call_id` for audio input
   - Example: call_id=151 → port 9152
   - Example: call_id=152 → port 9153
3. **Outbound Transcription Connection**: whisper-service connects to llama-service on port 8083
   - Sends `HELLO:<call_id>` to identify the conversation
   - Sends transcription chunks with length-prefixed protocol
   - Sends `BYE` (0xFFFFFFFF) when call ends

**Simulator Behavior** (must match production):
1. **UDP Registration**: Simulator sends `REGISTER:<call_id>` to whisper-service on UDP port 13000
2. **Inbound Audio Server**: Simulator listens on `9001 + call_id` and accepts connection from whisper-service
3. **Outbound Transcription Server**: Simulator listens on port 8083 (mimics llama-service)
   - Accepts connection from whisper-service
   - Reads `HELLO:<call_id>` to identify the conversation
   - Reads transcription chunks until `BYE` (0xFFFFFFFF)

### Test Sequence for Multiple Files

**Test 1** (OSR_us_000_0010_8k.wav):
- call_id = 151
- Audio port = 9152 (9001 + 151)
- Transcription port = 8083 (shared)
- Expected: WER calculation for file 1

**Test 2** (OSR_us_000_0011_8k.wav):
- call_id = 152
- Audio port = 9153 (9001 + 152)
- Transcription port = 8083 (shared, reused)
- Expected: WER calculation for file 2

**Test 3** (OSR_us_000_0012_8k.wav):
- call_id = 153
- Audio port = 9154 (9001 + 153)
- Transcription port = 8083 (shared, reused)
- Expected: WER calculation for file 3

## Implementation Status

### ✅ Completed
1. Fixed TranscriptionReceiver to be created once outside loop
2. Added `reset_for_next_test()` method to clear transcriptions between tests
3. Added `cleanup()` method to close server socket at end
4. Modified test loop to call `reset_for_next_test()` after each test
5. Added logging to whisper-service to debug BYE message sending

### 🔄 In Progress
1. Rebuild whisper-service with BYE fix
2. Test with all 3 Harvard files to verify call_id isolation
3. Verify WER is calculated independently for each file

### ⏭️ Pending
1. Run full test suite on all 25 Harvard files
2. Document final WER results
3. Apply optimizations to inbound-audio-processor.cpp

## Testing Commands

### Rebuild
```bash
cd /Users/whisper/Documents/augment-projects/clean-repo
bash scripts/build.sh --no-piper
```

### Run Test
```bash
killall -9 whisper-service whisper_inbound_sim
bash /Users/whisper/Documents/augment-projects/clean-repo/quick_test.sh
```

### Check Logs
```bash
# Whisper-service log
tail -100 /tmp/whisper-service.log

# Test output
tail -100 /tmp/wer_test.log
```

## Expected Output

### Successful Test Run
```
🦙 Simulator listening for Whisper transcriptions on TCP port 8083
=== Test 1: OSR_us_000_0010_8k.wav (call_id=151, port=9152) ===
📤 REGISTER sent for call_id 151
🔗 whisper-service connected from 127.0.0.1:xxxxx
📡 HELLO sent: 151
🔗 Whisper connected to simulator on port 8083
👋 HELLO from Whisper: call_id=151
[... transcriptions ...]
📡 BYE received from Whisper
✅ WER: 0.05 (edits=4/80)
⚠️  non-zero WER detected (continuing with remaining tests)
=== OK: OSR_us_000_0010_8k.wav ===

=== Test 2: OSR_us_000_0011_8k.wav (call_id=152, port=9153) ===
📤 REGISTER sent for call_id 152
🔗 whisper-service connected from 127.0.0.1:xxxxx
📡 HELLO sent: 152
🔗 Whisper connected to simulator on port 8083
👋 HELLO from Whisper: call_id=152
[... transcriptions ...]
📡 BYE received from Whisper
✅ WER: 0.XX (edits=X/YY)
=== OK: OSR_us_000_0011_8k.wav ===

=== Test 3: OSR_us_000_0012_8k.wav (call_id=153, port=9154) ===
[... similar output ...]

All tests completed.
```

## Files Modified

1. **tests/whisper_inbound_sim.cpp**
   - Moved `TranscriptionReceiver` creation outside loop
   - Added `reset_for_next_test()` and `cleanup()` methods
   - Modified test loop to reset between tests

2. **whisper-service.cpp**
   - Added BYE sending to llama socket
   - Added debug logging for BYE messages

## Next Steps

1. Verify whisper-service rebuild completed successfully
2. Run test and check for BYE messages in log
3. If BYE is sent correctly, test should proceed to file 2 and 3
4. Document WER results for all 3 files
5. Expand to all 25 Harvard files

