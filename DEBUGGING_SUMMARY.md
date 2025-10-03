# Debugging Summary: Second Call Failure Issue

## Problem Statement
After the first call works perfectly, the second call consistently fails with Whisper not receiving REGISTER messages from the inbound audio processor.

**Symptoms:**
- Call 1 (e.g., 101): ✅ Works flawlessly - Whisper receives REGISTER, connects, full conversation works
- Call 2 (e.g., 102): ❌ Fails - Inbound processor sends REGISTER #1-12, but Whisper never responds
- No logs from Whisper showing "📥 Received REGISTER for call_id 102"
- Registration listener thread appears to still be running (no exit message)

## Root Cause Analysis

Through systematic code analysis, I identified several critical bugs that could contribute to this issue:

### Bug #1: Resource Leak in TCP Handler (CRITICAL)
**Location:** `whisper-service.cpp:694`

**Problem:**
```cpp
if (!read_tcp_hello(socket, received_call_id)) {
    std::cout << "❌ Failed to read TCP HELLO for call " << call_id << std::endl;
    return;  // <-- Early return without cleanup!
}
```

When `read_tcp_hello()` fails, the function returns WITHOUT:
1. Closing the socket
2. Removing the socket from `call_tcp_sockets_` map

This leaves a stale socket in the map, which could cause the registration listener to think a call is already connected when it's not.

**Fix Applied:**
```cpp
if (!read_tcp_hello(socket, received_call_id)) {
    std::cout << "❌ Failed to read TCP HELLO for call " << call_id << std::endl;
    // Cleanup on early exit
    close(socket);
    {
        std::lock_guard<std::mutex> lock(tcp_mutex_);
        call_tcp_sockets_.erase(call_id);
    }
    return;
}
```

### Bug #2: Mutex Lock Ordering Issue (CRITICAL)
**Location:** `whisper-service.cpp:616-647`

**Problem:**
`create_session()` was holding `sessions_mutex_` while calling `connect_llama_for_call()`, which:
1. Locks `tcp_mutex_` (line 838)
2. Performs network I/O (TCP connect with retries and sleeps)
3. Can take several seconds with up to 10 retry attempts

This creates a lock ordering issue:
- `create_session()`: locks `sessions_mutex_` → calls `connect_llama_for_call()` → locks `tcp_mutex_`
- Registration listener: locks `tcp_mutex_` to check/insert sockets

While this doesn't create a direct deadlock (detached threads), holding `sessions_mutex_` during network I/O is bad practice and could cause delays.

**Fix Applied:**
```cpp
bool StandaloneWhisperService::create_session(const std::string& call_id) {
    {
        std::lock_guard<std::mutex> lock(sessions_mutex_);
        // ... create session ...
    } // Release sessions_mutex_ before connecting to LLaMA

    // Connect to LLaMA OUTSIDE the sessions_mutex_ lock
    if (connect_llama_for_call(call_id)) {
        // ...
    }
    return true;
}
```

### Bug #3: Missing SO_REUSEADDR on UDP Socket
**Location:** `whisper-service.cpp:300-332`

**Problem:**
The UDP registration socket was not setting `SO_REUSEADDR`, which on some systems (especially macOS) can cause issues with socket reuse and packet reception after the socket has been used.

**Fix Applied:**
```cpp
// Set SO_REUSEADDR to allow quick restart
int reuse = 1;
if (setsockopt(registration_socket_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
    std::cerr << "⚠️ Failed to set SO_REUSEADDR on registration socket" << std::endl;
}
```

## Additional Improvements

### Removed Duplicate BYE Messages
**Location:** `sip-client-main.cpp:1458-1477`

The SIP client was sending BYE messages directly to Whisper (UDP 13000) and Kokoro (UDP 13001), but the audio processors already handle this. This caused duplicate BYE messages which could interfere with the UDP registration system.

**Fix:** Removed the duplicate BYE sending code from SIP client. Now only audio processors send BYE notifications.

## Testing Recommendations

1. **Restart all services** with the new binaries
2. **Make 3-4 consecutive calls** to verify the fix
3. **Monitor logs** for:
   - "📥 Received REGISTER for call_id X" - confirms Whisper receives registration
   - "✅ Successfully connected and created session for call X" - confirms connection succeeds
   - No resource leak warnings or errors

## Potential Remaining Issues

If the problem persists after these fixes, the issue may be:

1. **UDP packet loss at OS level** - Check with `netstat -s | grep -i udp` for dropped packets
2. **Port exhaustion** - Check with `lsof -nP | grep UDP` to see if ports are being leaked
3. **Firewall/routing issue** - Unlikely on localhost but worth checking
4. **Race condition in registration listener** - May need to add more synchronization

## Files Modified

1. `whisper-service.cpp` - Fixed resource leak, mutex ordering, added SO_REUSEADDR
2. `sip-client-main.cpp` - Removed duplicate BYE messages

## Commits

1. `b82e4ea` - Remove excessive logging, add exception handling to detached connection threads
2. `0da4480` - Remove duplicate BYE messages from SIP client
3. `a424467` - Fix critical bugs in Whisper service (resource leak, mutex ordering)
4. `f1fe92a` - Add SO_REUSEADDR to UDP registration socket

## Next Steps

If the issue persists, I recommend:

1. Add comprehensive logging to track UDP packet flow
2. Use `tcpdump` or `wireshark` to capture UDP traffic on port 13000
3. Check if there's a macOS-specific UDP socket behavior that needs special handling
4. Consider using a persistent UDP socket in the inbound processor instead of creating/destroying for each send

