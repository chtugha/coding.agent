# Testing Round 1: Second Call Connection Failure

## Changes Applied

### Critical Fix #1: TCP Socket Cleanup
**File**: `whisper-service.cpp` - `destroy_session()`
- **Problem**: When a call ended, the TCP socket FD was closed but the entry was never removed from `call_tcp_sockets_` map
- **Impact**: Socket FD leaks, potential blocking of new connections
- **Fix**: Added proper cleanup to erase the map entry after closing the socket

### Enhancement #2: Comprehensive UDP Diagnostics
**File**: `whisper-service.cpp` - `registration_listener_thread()`
- Added message counter to track all received UDP packets
- Added loop counter for health checks
- Added socket FD tracking in all log messages
- Added periodic socket health checks (every 100 iterations)
- Added check for bytes available in UDP receive buffer
- Enhanced idle heartbeat with FD and running state
- Enhanced error logging with errno values

### Enhancement #3: Increased UDP Receive Buffer
**File**: `whisper-service.cpp` - `start_registration_listener()`
- Increased UDP receive buffer from default to 256KB
- Added logging to show actual buffer size allocated by OS
- **Rationale**: Prevents packet drops if REGISTER messages arrive faster than they're processed

### Enhancement #4: Inbound Processor Diagnostics
**File**: `inbound-audio-processor.cpp` - `registration_polling_thread()`
- Added bytes sent confirmation for each REGISTER message
- Added detailed error reporting with errno for send failures

## Testing Procedure

1. **Start the system**:
   ```bash
   ./start-wildfire.sh
   ```

2. **Open web interface**: http://localhost:8081

3. **Start services** from web interface:
   - Whisper service
   - LLaMA service
   - Kokoro service

4. **Make first call** from PBX to the SIP client

5. **Verify first call works** (should work as before)

6. **Hang up first call**

7. **Make second call** from PBX

8. **Capture logs** from:
   - Whisper service output
   - SIP client output (`/tmp/sip-client.log`)
   - Inbound audio processor output

## Expected Diagnostic Output

### On Whisper Startup
```
📊 UDP receive buffer size: 524288 bytes
📡 Whisper registration listener started on UDP port 13000
📡 Whisper registration listener thread started (FD: 6)
```

### During First Call (call_id 111)
```
📨 UDP message #1 received (12 bytes) from 127.0.0.1:54321
📥 Received REGISTER for call_id 111
🔗 Whisper connecting to inbound audio stream: 111 on port 9112
✅ Successfully connected and created session for call 111
🎤 Created whisper session for call 111
```

### On First Call End
```
📤 Received BYE for call_id 111
🔌 Closed TCP socket (FD 8) for call 111
🧹 Removed call 111 from TCP sockets map
🗑️ Destroyed whisper session for call 111 (keeping LLaMA connection open)
```

### During Second Call (call_id 112) - SUCCESS CASE
```
📨 UDP message #2 received (12 bytes) from 127.0.0.1:54322
📥 Received REGISTER for call_id 112
🔗 Whisper connecting to inbound audio stream: 112 on port 9113
✅ Successfully connected and created session for call 112
```

### During Second Call (call_id 112) - FAILURE CASE
If the second call still fails, the logs will show one of these patterns:

**Pattern A: UDP packets not received**
```
⏳ Whisper registration listener idle (5s, FD=6, running=1, waiting on UDP 127.0.0.1:13000)
⏳ Whisper registration listener idle (10s, FD=6, running=1, waiting on UDP 127.0.0.1:13000)
```
(No "📨 UDP message #2 received" - packets are being sent but not received)

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
(Large number indicates packets are queued but not being processed)

### Inbound Processor Output
```
📤 Sent REGISTER #1 for call_id 112 (12 bytes to 127.0.0.1:13000)
📤 Sent REGISTER #2 for call_id 112 (12 bytes to 127.0.0.1:13000)
...
```

## What to Look For

1. **Message counter**: Does it increment for the second call?
   - If YES: Whisper IS receiving packets, problem is elsewhere
   - If NO: Packets are being sent but not received by Whisper

2. **Socket FD**: Does it remain consistent?
   - Should stay the same value (e.g., FD=6) throughout

3. **Socket health**: Any error messages?
   - Look for "Socket error detected" messages

4. **Cleanup**: Are TCP sockets being cleaned up?
   - Look for "🧹 Removed call X from TCP sockets map" after each call

5. **Bytes sent**: Are REGISTER messages being sent successfully?
   - Look for "📤 Sent REGISTER #N" with positive byte count

## Next Steps Based on Results

### If second call now works:
✅ Problem solved! The TCP socket leak was the root cause.

### If Pattern A (packets not received):
- Possible OS-level packet filtering or firewall
- Possible UDP socket state issue
- Need to investigate kernel-level packet drops

### If Pattern B (socket invalid):
- Something is closing the UDP socket between calls
- Need to audit all `close()` calls in the codebase

### If Pattern C (socket errors):
- Socket entered error state
- Need to investigate what operation caused the error

### If Pattern D (buffer overflow):
- Listener thread is blocked or not processing fast enough
- Need to investigate what's blocking the thread

## Files Modified

- `whisper-service.cpp`: Lines 308-330, 359-408, 472-485, 676-712
- `inbound-audio-processor.cpp`: Lines 389-406

## Build Status

✅ Successfully compiled with no errors
✅ New binary deployed: `/Users/whisper/Documents/augment-projects/clean-repo/bin/whisper-service`

