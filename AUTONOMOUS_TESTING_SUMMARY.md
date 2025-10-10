# Autonomous Testing Summary - Round 1

## Problem Statement
Second call consistently fails with Whisper service not connecting to the inbound audio processor. First call works perfectly.

## Root Cause Analysis

### Evidence from User Logs
- **Call 1 (ID: 111)**: ✅ Works perfectly
  - Inbound processor sends REGISTER messages
  - Whisper receives them: "📥 Received REGISTER for call_id 111"
  - Connection established successfully
  
- **Call 2 (ID: 112)**: ❌ Fails
  - Inbound processor sends 14 REGISTER messages to UDP 13000
  - SIP client sends 5 additional REGISTER "poke" messages
  - **Whisper NEVER logs receiving any REGISTER messages**
  - No connection established
  - Audio chunks dropped: "⚠️ No Whisper client connected, dropping chunk"

### Key Observation
The failure is NOT in sending (inbound processor logs successful sends), but in **receiving** (Whisper's `recvfrom()` never returns data for call 2).

## Fixes Applied

### Fix #1: TCP Socket Map Cleanup (CRITICAL)
**File**: `whisper-service.cpp` - `destroy_session()` (lines 689-702)

**Problem**: 
- When a call ended, `destroy_session()` closed the TCP socket FD
- BUT it never removed the entry from `call_tcp_sockets_` map
- This caused socket FD leaks and stale map entries

**Fix**:
```cpp
{
    std::lock_guard<std::mutex> lock(tcp_mutex_);
    auto tcp_it = call_tcp_sockets_.find(call_id);
    if (tcp_it != call_tcp_sockets_.end()) {
        int sock = tcp_it->second;
        if (sock >= 0) {
            close(sock);
            std::cout << "🔌 Closed TCP socket (FD " << sock << ") for call " << call_id << std::endl;
        }
        call_tcp_sockets_.erase(tcp_it);  // <-- THIS WAS MISSING!
        std::cout << "🧹 Removed call " << call_id << " from TCP sockets map" << std::endl;
    }
}
```

**Impact**: Prevents resource exhaustion and potential blocking of new connections

### Fix #2: Increased UDP Receive Buffer
**File**: `whisper-service.cpp` - `start_registration_listener()` (lines 320-330)

**Problem**: Default UDP receive buffer might be too small, causing packet drops

**Fix**:
```cpp
int rcvbuf = 256 * 1024; // 256KB
if (setsockopt(registration_socket_, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf)) < 0) {
    std::cerr << "⚠️ Failed to set SO_RCVBUF on registration socket" << std::endl;
} else {
    int actual_rcvbuf = 0;
    socklen_t optlen = sizeof(actual_rcvbuf);
    if (getsockopt(registration_socket_, SOL_SOCKET, SO_RCVBUF, &actual_rcvbuf, &optlen) == 0) {
        std::cout << "📊 UDP receive buffer size: " << actual_rcvbuf << " bytes" << std::endl;
    }
}
```

**Impact**: Prevents packet drops if REGISTER messages arrive faster than they're processed

### Fix #3: Comprehensive Diagnostic Logging
**Files**: `whisper-service.cpp`, `inbound-audio-processor.cpp`

**Added to Whisper service**:
- Message counter to track all received UDP packets
- Loop counter for periodic health checks
- Socket FD tracking in all log messages
- Socket health checks every 100 iterations
- Check for bytes available in UDP buffer (macOS SO_NREAD)
- Enhanced idle heartbeat with FD and running state
- Enhanced error logging with errno values

**Added to Inbound processor**:
- Bytes sent confirmation for each REGISTER message
- Detailed error reporting with errno for send failures

**Impact**: Will immediately reveal where the failure occurs in the next test

## Testing Performed

### Round 1: System Startup and Verification
1. ✅ Cleaned up all existing processes
2. ✅ Started wildfire HTTP server (port 8081)
3. ✅ Started SIP client successfully (PID 66407, port 53110)
4. ✅ Started Whisper service via API (PID 68511)
5. ✅ Started LLaMA service via API (PID 66908)
6. ✅ Started Kokoro service via API (PID 66945)
7. ✅ All services running and healthy

### Limitation
Cannot make actual SIP calls from PBX to test the two-call scenario. The system is ready and waiting for real calls.

## Expected Diagnostic Output on Next Test

### Whisper Startup
```
📊 UDP receive buffer size: 524288 bytes
📡 Whisper registration listener started on UDP port 13000
📡 Whisper registration listener thread started (FD: 6)
```

### First Call (should work as before)
```
📨 UDP message #1 received (12 bytes) from 127.0.0.1:XXXXX
📥 Received REGISTER for call_id 111
🔗 Whisper connecting to inbound audio stream: 111 on port 9112
✅ Successfully connected and created session for call 111
```

### First Call End
```
📤 Received BYE for call_id 111
🔌 Closed TCP socket (FD 8) for call 111
🧹 Removed call 111 from TCP sockets map  <-- NEW: Confirms cleanup
🗑️ Destroyed whisper session for call 111
```

### Second Call - Success Case
```
📨 UDP message #2 received (12 bytes) from 127.0.0.1:XXXXX  <-- Should see this
📥 Received REGISTER for call_id 112
🔗 Whisper connecting to inbound audio stream: 112 on port 9113
✅ Successfully connected and created session for call 112
```

### Second Call - Failure Patterns

**Pattern A: UDP packets not received (most likely)**
```
⏳ Whisper registration listener idle (5s, FD=6, running=1, waiting on UDP 127.0.0.1:13000)
⏳ Whisper registration listener idle (10s, FD=6, running=1, waiting on UDP 127.0.0.1:13000)
```
No "📨 UDP message #2 received" means `recvfrom()` is not returning data

**Pattern B: Socket became invalid**
```
❌ Registration socket became invalid (FD: -1), exiting listener
```

**Pattern C: Socket has errors**
```
⚠️ Socket error detected (FD=6): <error message>
```

**Pattern D: Buffer overflow**
```
📊 Bytes available in UDP buffer: 262144
```

## Next Steps

### If Second Call Now Works
✅ **Problem solved!** The TCP socket leak was the root cause.

### If Pattern A (packets not received)
Possible causes:
1. OS-level packet filtering or firewall on localhost
2. UDP socket entered a bad state between calls
3. Kernel-level packet drops (check with `netstat -s | grep -i udp`)
4. Race condition in socket operations

**Action**: 
- Check kernel UDP statistics for drops
- Add `strace` to Whisper service to see system calls
- Consider recreating UDP socket between calls

### If Pattern B (socket invalid)
Something is closing the UDP socket between calls.

**Action**:
- Audit all `close()` calls in codebase
- Check for file descriptor reuse bugs
- Add assertion to verify socket FD doesn't change

### If Pattern C (socket errors)
Socket entered error state due to some operation.

**Action**:
- Investigate what operation caused the error
- Check if any code is writing to the UDP socket (should be read-only)

### If Pattern D (buffer overflow)
Listener thread is blocked or not processing fast enough.

**Action**:
- Investigate what's blocking the thread
- Check if any mutex is held too long
- Consider making UDP processing fully non-blocking

## Files Modified

### whisper-service.cpp
- Lines 308-330: Increased UDP receive buffer, added diagnostics
- Lines 359-408: Enhanced registration listener with health checks
- Lines 472-485: Enhanced error logging and idle heartbeat
- Lines 689-702: Fixed TCP socket cleanup in `destroy_session()`

### inbound-audio-processor.cpp
- Lines 389-406: Enhanced REGISTER sending diagnostics

## Build Status
✅ Successfully compiled with no errors or warnings
✅ New binary deployed: `/Users/whisper/Documents/augment-projects/clean-repo/bin/whisper-service`
✅ Service restarted with new binary (PID 68511)

## Confidence Level
**Medium-High** that Fix #1 (TCP socket cleanup) will resolve the issue.

The TCP socket leak could cause:
- File descriptor exhaustion
- Stale entries blocking new connections
- Map lookup failures

If this doesn't fix it, the comprehensive diagnostics will immediately reveal the exact failure point.

## Testing Instructions for User

1. Make first call from PBX → should work ✅
2. Hang up first call
3. Make second call from PBX → check if it works
4. Capture logs from:
   - Whisper service output
   - SIP client output (`/tmp/sip-client.log`)
   - Inbound processor output (if separate)
5. Look for the diagnostic patterns described above
6. Report findings

## Autonomous Testing Limitation

Cannot proceed with further rounds without ability to make actual SIP calls from PBX. The system is ready and instrumented for testing, but requires real telephony hardware/software to generate calls.

**Recommendation**: User should test with the current fixes and report results. The enhanced diagnostics will provide clear direction for next steps if the issue persists.

## UDP Socket Verification Tests

Performed manual UDP tests to verify the receiving mechanism:

### Test 1: Single REGISTER
```bash
echo -n "REGISTER:999" | nc -u -w1 127.0.0.1 13000
```
**Result**: ✅ SUCCESS
```
📨 UDP message #1 received (12 bytes) from 127.0.0.1:53244
📥 Received REGISTER for call_id 999
```

### Test 2: Second REGISTER
```bash
echo -n "REGISTER:888" | nc -u -w1 127.0.0.1 13000
```
**Result**: ✅ SUCCESS
```
📨 UDP message #2 received (12 bytes) from 127.0.0.1:65092
📥 Received REGISTER for call_id 888
```

### Test 3: BYE Message
```bash
echo -n "BYE:999" | nc -u -w1 127.0.0.1 13000
```
**Result**: ✅ SUCCESS
```
📨 UDP message #3 received (7 bytes) from 127.0.0.1:61486
📤 Received BYE for call_id 999
🗑️ Destroyed whisper session for call 999
```

### Test 4: Full Call Sequence (REGISTER → BYE → REGISTER)
```bash
echo -n "REGISTER:777" | nc -u -w1 127.0.0.1 13000
echo -n "BYE:777" | nc -u -w1 127.0.0.1 13000
echo -n "REGISTER:666" | nc -u -w1 127.0.0.1 13000
```
**Result**: ✅ SUCCESS - All three messages received in order
```
📨 UDP message #4 received (12 bytes) from 127.0.0.1:62179
📥 Received REGISTER for call_id 777
📨 UDP message #5 received (7 bytes) from 127.0.0.1:53464
📤 Received BYE for call_id 777
📨 UDP message #6 received (12 bytes) from 127.0.0.1:57406
📥 Received REGISTER for call_id 666
```

### Conclusion from UDP Tests

**The UDP receiving mechanism is working perfectly.** Whisper successfully received and processed:
- 6 consecutive UDP messages
- Multiple REGISTER messages
- BYE messages
- Full call sequences (REGISTER → BYE → REGISTER)

This proves that:
1. ✅ The UDP socket is correctly bound to 127.0.0.1:13000
2. ✅ The `recvfrom()` loop is working correctly
3. ✅ Message parsing is working correctly
4. ✅ The socket remains valid across multiple messages
5. ✅ The BYE → REGISTER sequence does not break UDP reception

**Implication**: The issue in the user's scenario must be specific to the real call handling flow, not the UDP mechanism itself. Possible causes:
- Inbound processor not actually sending packets during second call (despite logging)
- Timing issue where Whisper is busy during packet arrival
- Race condition that only occurs with real SIP calls
- Network routing issue specific to the inbound processor's sending

The TCP socket cleanup fix is still critical and may resolve the issue by preventing resource exhaustion.

