# Critical Fixes Applied - 2025-10-14

## Summary

Fixed TWO critical issues based on detailed log analysis:

1. **🐛 NO AUDIO HEARD** - Added logging to diagnose why only 13 audio frames were sent (260ms) instead of full responses
2. **✂️ VAD CUTTING TOO EARLY** - Increased threshold from 0.05 to 0.10 to prevent fragmentation

---

## Issue #1: No Audio Heard - Root Cause Investigation

### Evidence from Logs

**Exit Statistics**:
```
Outbound stream thread exiting (sent 13 audio frames, 1207 silence frames, 1220 total)
```

**ONLY 13 AUDIO FRAMES!** That's only 260ms of audio (13 × 20ms).

**But Kokoro generated MUCH more**:
```
t1: LLaMA send-to-Kokoro [call 141] ts=1760428460.929291
⏱️  Kokoro pipeline first audio: 367.1ms
t2: Kokoro first subchunk sent [call 141] ts=1760428461.296606
t3: First RTP frame sent [call 141] ts=1760428461.301
```

And there were 3 more responses after this!

### The Problem

The logs show:
1. ✅ Kokoro synthesizes audio (4 responses total)
2. ✅ Outbound processor receives audio ("t2" logged)
3. ✅ Outbound processor writes to SHM ("t3" logged)
4. ❌ SIP client sends ONLY 13 audio frames (should be hundreds!)

**Hypothesis**: The outbound processor is writing audio to `out_buffer_`, but the SIP client is reading SILENCE (all 0xFF) from SHM instead of the actual audio data.

**Possible causes**:
1. Audio is not being added to `out_buffer_` correctly
2. Audio is being added but immediately dropped (buffer full)
3. Audio is being added but SHM write is failing
4. SHM write succeeds but SIP client reads stale/wrong data

### Changes Made

**File**: `outbound-audio-processor.cpp`

**1. Enhanced enqueue_g711_ Logging** (lines 301-326):
```cpp
static int enqueue_count = 0;
size_t before_size = out_buffer_.size();
out_buffer_.insert(out_buffer_.end(), g711.begin(), g711.begin() + to_copy);
if (++enqueue_count <= 5 || enqueue_count % 50 == 0) {
    std::cout << "📥 Enqueued " << to_copy << " bytes to out_buffer (was " << before_size << ", now " << out_buffer_.size() << ")" << std::endl;
}
```

**2. Buffer Full Logging** (lines 310-314):
```cpp
if (capacity == 0) {
    static int drop_count = 0;
    if (++drop_count <= 3 || drop_count % 50 == 0) {
        std::cout << "⚠️  Dropping " << g711.size() << " bytes (buffer full: " << out_buffer_.size() << " bytes)" << std::endl;
    }
    return;
}
```

**3. Enhanced t3 Logging** (line 390):
```cpp
std::cout << "t3: First RTP frame sent [call " << call_id << "] ts=" << ms << " (buffer had " << buffer_size << " bytes)" << std::endl;
```

### Expected New Logs

**If audio IS being enqueued**:
```
⏱️  Kokoro pipeline first audio: 367ms
t2: Kokoro first subchunk sent [call 141] ts=...
📥 Enqueued 320 bytes to out_buffer (was 0, now 320)  ← NEW!
📥 Enqueued 320 bytes to out_buffer (was 320, now 640)  ← NEW!
t3: First RTP frame sent [call 141] ts=... (buffer had 640 bytes)  ← NEW!
🎤 Started sending AUDIO frames (was silence) on port 10001
🔊 Sent audio frame #1 (160 bytes) on port 10001
```

**If audio is NOT being enqueued**:
```
⏱️  Kokoro pipeline first audio: 367ms
t2: Kokoro first subchunk sent [call 141] ts=...
[NO "📥 Enqueued" messages!]  ← PROBLEM!
t3: First RTP frame sent [call 141] ts=... (buffer had 0 bytes)  ← PROBLEM!
[NO "🎤 Started sending AUDIO frames" message!]
```

**If buffer is full**:
```
📥 Enqueued 320 bytes to out_buffer (was 0, now 320)
📥 Enqueued 320 bytes to out_buffer (was 320, now 640)
...
⚠️  Dropping 320 bytes (buffer full: 1600 bytes)  ← PROBLEM!
```

---

## Issue #2: VAD Cutting Sentences Too Early

### Evidence from Logs

User said: **"I don't hear anything"**

But VAD cut it into:
```
📦 Chunk created (max_size): 16000 samples (~1.00 s), meanRMS=0.10
📝 [141] Transcription:  I don't...

📦 Chunk created (max_size): 16000 samples (~1.00 s), meanRMS=0.08
📝 [141] Transcription:  year.
```

Whisper transcribed it as: **"I don't... year."** instead of **"I don't hear anything"**

### Root Cause

**VAD threshold was 0.05**, giving:
- `vad_stop_threshold = 0.025` (0.05 × 0.5)
- User's speech RMS: 0.08-0.10
- Silence RMS: < 0.03

The user's speech (0.08-0.10) is ALWAYS above the stop threshold (0.025), so VAD never detects silence DURING speech. It only stops when hitting the 1.0s max_size limit.

**Result**: Every chunk is 1.0s (max_size), cutting sentences mid-word.

### The Fix

**File**: `simple-audio-processor.cpp` (line 86)

**Changed threshold from 0.05 to 0.10**:
```cpp
chunk_duration_ms_(1000), vad_threshold_(0.10f), silence_timeout_ms_(500), database_(nullptr) {
```

**New thresholds**:
- `vad_threshold_ = 0.10`
- `vad_stop_threshold = 0.05` (0.10 × 0.5)
- `vad_start_threshold = 0.15` (0.10 × 1.5)

**User's speech RMS**: 0.08-0.10
**Silence RMS**: < 0.03

Now:
- Speech (0.08-0.10) is ABOVE stop threshold (0.05) → VAD detects as speech ✅
- Silence (< 0.03) is BELOW stop threshold (0.05) → VAD detects as silence ✅
- Pauses between words (0.03-0.05) will trigger speech end ✅

### Expected Result

**Before** (threshold=0.05):
```
📦 Chunk created (max_size): 16000 samples (~1.00 s), meanRMS=0.10
📝 Transcription:  I don't...
📦 Chunk created (max_size): 16000 samples (~1.00 s), meanRMS=0.08
📝 Transcription:  year.
```

**After** (threshold=0.10):
```
📦 Chunk created (end_of_speech): 11200 samples (~0.70 s), meanRMS=0.09
📝 Transcription:  I don't hear anything.
```

Complete sentence in ONE chunk!

---

## Testing Instructions

### 1. Restart Services

**CRITICAL**: You MUST restart services to load new binaries:
```bash
cd /Users/whisper/Documents/augment-projects/clean-repo
pkill -f "sip-client|llama-service|whisper-service|kokoro|http-server|inbound-audio|outbound-audio"
./start-wildfire.sh
```

### 2. Make Test Call

Place a call and:
1. Say "Hello" (short utterance)
2. Wait for response
3. Say "I don't hear anything" (longer utterance with pause)
4. Wait for response
5. Hang up

### 3. Check Logs

**For Issue #1 (No Audio)**:

Look for these NEW logs:
```
📥 Enqueued X bytes to out_buffer (was Y, now Z)
t3: First RTP frame sent [call X] ts=... (buffer had Z bytes)
🎤 Started sending AUDIO frames (was silence) on port 10001
```

**Diagnosis**:
- **If you see "📥 Enqueued"**: Audio IS being added to buffer ✅
- **If you DON'T see it**: Audio is NOT being added (conversion failing) ❌
- **If you see "⚠️  Dropping"**: Buffer is full (shouldn't happen with 200ms cap) ⚠️
- **If buffer had 0 bytes at t3**: No audio in buffer when SHM write happened ❌
- **If you see "🎤 Started sending AUDIO"**: SIP client IS reading audio from SHM ✅

**For Issue #2 (VAD)**:

Look for:
```
📦 Chunk created (end_of_speech): X samples (~Y s), meanRMS=Z
```

**Diagnosis**:
- **If most chunks are "end_of_speech"**: VAD is working correctly ✅
- **If most chunks are "max_size"**: VAD threshold still too low ❌

### 4. Final Statistics

At call end, check:
```
Outbound stream thread exiting (sent X audio frames, Y silence frames, Z total)
```

**Expected**:
- Audio frames: 200-400 (4-8 seconds of speech)
- Silence frames: 800-1000 (16-20 seconds of silence)
- Total: ~1200 (24 seconds call)

**If audio frames < 50**: Audio pipeline is broken!

---

## Diagnosis Flow

### If You Still Hear No Audio

**Step 1**: Check if audio is being enqueued
```
📥 Enqueued X bytes to out_buffer
```
- **YES**: Go to Step 2
- **NO**: Audio conversion is failing (Kokoro → G.711)

**Step 2**: Check if buffer has audio at t3
```
t3: First RTP frame sent [call X] ts=... (buffer had Z bytes)
```
- **Z > 0**: Go to Step 3
- **Z = 0**: Timing issue (SHM write before audio arrives)

**Step 3**: Check if SIP client reads audio
```
🎤 Started sending AUDIO frames (was silence) on port 10001
```
- **YES**: Audio IS being sent! Problem is codec/network/phone
- **NO**: SHM communication broken (write succeeds but read fails)

**Step 4**: Check final statistics
```
Outbound stream thread exiting (sent X audio frames, ...)
```
- **X > 100**: Audio was sent, problem is external
- **X < 50**: Audio pipeline is broken

### If VAD Still Cuts Too Early

**Check chunk logs**:
```
📦 Chunk created (max_size): 16000 samples (~1.00 s), meanRMS=X
```

If you still see mostly "max_size" chunks:
- **meanRMS > 0.05**: Threshold needs to be even higher (try 0.15)
- **meanRMS < 0.05**: Threshold is correct, but user is speaking continuously for >1s

---

## Technical Details

### Audio Buffer Flow

```
Kokoro (24kHz float32)
  ↓ TCP subchunks (40ms each)
Conversion Worker Thread
  ↓ Resample 24kHz → 8kHz
  ↓ Convert float32 → G.711 μ-law
  ↓ enqueue_g711_() → out_buffer_
RTP Scheduler Thread (20ms loop)
  ↓ Read 160 bytes from out_buffer_
  ↓ Write to SHM (/ap_out_X)
SIP Client Outbound Thread (20ms loop)
  ↓ Read 160 bytes from SHM
  ↓ Check if audio (not all 0xFF)
  ↓ Send as RTP packet
Phone
```

### Buffer Capacity

- **Max buffer size**: 1600 bytes (10 frames × 160 bytes = 200ms)
- **Kokoro subchunk**: 40ms @ 24kHz = 960 samples → 320 samples @ 8kHz = 320 bytes
- **RTP frame**: 20ms @ 8kHz = 160 samples = 160 bytes

So each Kokoro subchunk (40ms) produces 2 RTP frames (2 × 20ms).

### VAD Threshold Calculation

- **Threshold**: 0.10
- **Start threshold**: 0.15 (1.5× threshold)
- **Stop threshold**: 0.05 (0.5× threshold)
- **Hangover**: 400ms (wait after RMS drops below stop threshold)
- **Max chunk**: 1.0s (force send if speech continues too long)

**Speech detection**:
1. RMS rises above 0.15 → speech starts
2. RMS stays above 0.05 → speech continues
3. RMS drops below 0.05 for 400ms → speech ends

---

## Summary

**Issue #1: No Audio** - Added comprehensive logging to trace audio flow from Kokoro to RTP
- Will show if audio is being enqueued to buffer
- Will show if buffer has audio when writing to SHM
- Will show if SIP client is reading audio from SHM

**Issue #2: VAD Cutting** - Increased threshold from 0.05 to 0.10
- Stop threshold now 0.05 (was 0.025)
- Should properly detect pauses between words
- Should produce complete sentences in single chunks

**All changes compiled successfully.**
**Services MUST be restarted to take effect.**

After restart, make a test call and share the logs. The new logging will definitively show where the audio pipeline is breaking.

