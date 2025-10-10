# Outbound Audio Failure Analysis - Call 116

## Problem Statement
Second call (call_id 116) has no audio output. User cannot hear anything.
- ✅ First call (115): Complete audio output works
- ✅ Second call (116): Inbound path works (Whisper → LLaMA)
- ❌ Second call (116): Outbound path fails (no audio output)

## Log Analysis

### Call 115 (First Call) - ✅ WORKING
```
📝 Synthesizing for call 115: Yes, I can hear you. Your volume is good.
🔊 Sent chunk#2 (100800 samples @24000Hz) for call 115
   ⚡ Synthesis: 0.548s | Audio: 4.200s | RTF: 0.131x
📤 Piper TTS enqueued (float->G711): 33600 bytes @8kHz
```
**Audio synthesis and output working perfectly.**

### Call 116 (Second Call) - ❌ NO AUDIO
```
🦙 [116] Generated response: Hi, how can I help you today?
💬 Response [116]: Hi, how can I help you today?
🦙 LLaMA response appended to call 116: Hi, how can I help you today?
```
**NO synthesis logs, NO audio chunks, NO TTS output!**

LLaMA generates responses but they never reach the TTS service.

## Missing Logs for Call 116

The following logs that appeared for call 115 are **completely missing** for call 116:

1. ❌ No "📝 Synthesizing for call 116"
2. ❌ No "🔊 Sent chunk#N"
3. ❌ No "📤 Piper TTS enqueued"
4. ❌ No "✅ Piper TCP socket listening on port 9118" (Kokoro side)
5. ❌ No "🔄 Started registration listener for call 116" (outbound processor)
6. ❌ No "📡 Outbound waiting for REGISTER on UDP port 13116"
7. ❌ No "✅ Outbound Audio Processor ACTIVE"
8. ❌ No "🔌 Outbound SHM channel bound: /ap_out_116"

## Root Cause Identified

### Issue #1: Outbound Processor Not Activating Properly

The outbound processor receives the ACTIVATE command but fails to complete activation:

**Evidence:**
- ✅ SIP client creates SHM: "✅ Outbound SHM ready: /ap_out_116"
- ✅ Control command sent successfully (no error logged)
- ✅ Generic log appears: "✅ Activated for call 116"
- ❌ But NO outbound-specific logs appear
- ❌ No "🔌 Outbound SHM channel bound"
- ❌ No "✅ Outbound Audio Processor ACTIVE"
- ❌ No registration polling started

**Analysis:**
The control handler in `outbound-audio-processor-main.cpp` logs "✅ Activated for call 116" but the actual `activate_for_call()` function in `OutboundAudioProcessor` never completes or logs its success message.

This suggests one of:
1. `activate_for_call()` returns early due to `running_` being false
2. `open_outbound_channel_()` fails silently
3. The outbound processor crashed/exited after call 115

### Issue #2: Kokoro Not Receiving Connection

Kokoro logs show it's listening and sending REGISTER:
```
✅ Piper TCP socket listening on port 9118 for call 116
📤 Sent REGISTER #1 for call_id 116 to UDP 13116
```

But there's no corresponding log from the outbound processor:
- ❌ No "📡 Outbound waiting for REGISTER on UDP port 13116"
- ❌ No "Received REGISTER from Kokoro"
- ❌ No "Connected to Kokoro"

This confirms the outbound processor's registration polling thread never started.

### Issue #3: Binary Out of Date

Checking binary timestamps:
```
-rwxr-xr-x@ 1 whisper  staff  158056 Oct  9 22:05 bin/inbound-audio-processor
-rwxr-xr-x@ 1 whisper  staff  178888 Oct  7 09:45 bin/outbound-audio-processor  <-- OLD!
```

The outbound processor binary is from Oct 7, while the inbound processor was rebuilt on Oct 9.

**This is likely the root cause.** The outbound processor is running old code that doesn't have the persistent processor architecture fixes.

## Architecture Flow (Expected)

### Correct Outbound Audio Flow:
1. LLaMA generates text response
2. LLaMA sends text to Kokoro on TCP 8090
3. Kokoro synthesizes audio
4. Kokoro creates TCP server on port 9002 + call_id (9118 for call 116)
5. Kokoro sends REGISTER to UDP 13000 + call_id (13116 for call 116)
6. Outbound processor listens on UDP 13116
7. Outbound processor receives REGISTER
8. Outbound processor connects to Kokoro on TCP 9118
9. Kokoro sends audio chunks to outbound processor
10. Outbound processor writes G.711 audio to SHM `/ap_out_116`
11. SIP client reads from SHM and sends RTP to caller

### Where It Breaks for Call 116:
- Steps 1-4: ✅ Working (Kokoro logs show this)
- Step 5: ✅ Working (Kokoro sends REGISTER)
- Step 6: ❌ **FAILS** - Outbound processor not listening
- Steps 7-11: ❌ Never reached

## Fix Applied

### Rebuild Outbound Processor
```bash
cd build
rm -f CMakeFiles/outbound-audio-processor.dir/*.o
make outbound-audio-processor -j8
```

**Result:**
```
-rwxr-xr-x@ 1 whisper  staff  178888 Oct 10 10:30 bin/outbound-audio-processor
```

Binary now updated to Oct 10 10:30.

## Expected Behavior After Fix

With the rebuilt binary, the outbound processor should:

1. ✅ Start in SLEEPING mode when first spawned
2. ✅ Receive ACTIVATE command for call 116
3. ✅ Open SHM channel `/ap_out_116`
4. ✅ Start registration polling on UDP 13116
5. ✅ Receive REGISTER from Kokoro
6. ✅ Connect to Kokoro on TCP 9118
7. ✅ Receive audio chunks from Kokoro
8. ✅ Write G.711 audio to SHM
9. ✅ User hears audio output

## Testing Required

1. Restart the system (kill all processes)
2. Start services via web interface
3. Make first call → should work as before
4. Make second call → **should now have audio output**

Look for these diagnostic logs for call 116:
```
🔌 Outbound SHM channel bound: /ap_out_116
✅ Outbound Audio Processor ACTIVE - will connect to Kokoro on port 9118 for call 116
🔄 Started registration listener for call 116
📡 Outbound waiting for REGISTER on UDP port 13116 for call 116
✅ Kokoro connected for call 116
📤 Piper TTS enqueued (float->G711): XXXXX bytes @8kHz
```

## Additional Investigation Needed

If the rebuild doesn't fix the issue, investigate:

1. **Check if outbound processor is actually running:**
   ```bash
   ps aux | grep outbound-audio-processor
   ```

2. **Check if it's receiving ACTIVATE commands:**
   - Look for "✅ Activated for call 116" in outbound processor output
   - Check control socket: `ls -la /tmp/outbound-audio-processor.ctrl`

3. **Check if SHM channel exists:**
   ```bash
   ls -la /dev/shm/ap_out_116  # Linux
   # or check macOS shared memory
   ```

4. **Check for crashes:**
   - Look for "Outbound Audio Processor stopped" or segfault messages
   - Check if PID changes between calls

5. **Add more diagnostic logging:**
   - Log when `activate_for_call()` is entered
   - Log when `open_outbound_channel_()` is called
   - Log the return value of each step
   - Log `running_` and `active_` flags

## Files Involved

- `outbound-audio-processor.cpp` - Main processor logic
- `outbound-audio-processor-main.cpp` - Control socket handler
- `bin/kokoro_service.py` - Kokoro TTS service
- `sip-client-main.cpp` - Spawns and activates processors

## Similar to Whisper Issue

This issue is similar to the Whisper UDP registration problem that was fixed:
- First call works
- Second call fails
- UDP REGISTER messages not being received
- Processor not connecting to service

The fix for Whisper was:
1. TCP socket cleanup in `destroy_session()`
2. Enhanced diagnostic logging
3. Increased UDP buffer size

The outbound processor may need similar fixes, but first we need to verify it's running the latest code.

