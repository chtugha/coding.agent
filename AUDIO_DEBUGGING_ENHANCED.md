# Enhanced Audio Debugging - 2025-10-14

## Problem Analysis

You reported "no audio hearable" but the logs show:
- ✅ RTP packets ARE being sent (871 frames confirmed)
- ✅ Kokoro IS synthesizing audio
- ✅ Outbound processor IS receiving audio from Kokoro
- ✅ Outbound processor IS writing to shared memory (t3 logged)
- ✅ SIP client IS reading from shared memory
- ✅ SIP client IS sending RTP packets

**The question is**: Are the RTP packets containing AUDIO or SILENCE?

## The Issue

The previous logging showed:
```
🔊 Sent audio frame #1 (160 bytes) on port 10001
🔊 Sent audio frame #50 (160 bytes) on port 10001
```

But this was misleading! The code incremented `audio_frame_count` for EVERY successful SHM read, regardless of whether the frame contained actual audio or silence.

**Timeline Analysis**:
1. Outbound RTP thread starts immediately when call is established
2. Thread sends frames every 20ms (silence initially, since no audio yet)
3. ~4 seconds later, LLaMA generates first response
4. Kokoro synthesizes audio (669ms latency)
5. Outbound processor receives audio and writes to SHM
6. SIP client reads from SHM and sends as RTP

**The problem**: We couldn't tell if the SIP client was sending AUDIO or SILENCE after step 5!

## The Fix

Added intelligent frame detection to distinguish between audio and silence:

### Changes Made

**File**: `sip-client-main.cpp` (lines 2837-2903)

**1. Audio vs Silence Detection**:
```cpp
// Check if this looks like audio (not all 0xFF which is silence)
bool all_silence = true;
for (size_t i = 0; i < std::min(frame.size(), size_t(160)); ++i) {
    if (frame[i] != 0xFF) {
        all_silence = false;
        break;
    }
}
is_audio = !all_silence;
```

**2. Separate Counters**:
```cpp
int audio_frame_count = 0;      // Frames containing actual audio
int silence_frame_count = 0;    // Frames containing silence
```

**3. Transition Logging**:
```cpp
if (is_audio) {
    audio_frame_count++;
    if (!was_sending_audio) {
        std::cout << "🎤 Started sending AUDIO frames (was silence) on port " << local_rtp_port << std::endl;
        was_sending_audio = true;
    }
} else {
    silence_frame_count++;
    if (was_sending_audio) {
        std::cout << "🔇 Switched back to SILENCE frames (audio ended) on port " << local_rtp_port << std::endl;
        was_sending_audio = false;
    }
}
```

**4. Enhanced Exit Message**:
```cpp
std::cout << "Outbound stream thread exiting for call " << call_id 
          << " (sent " << audio_frame_count << " audio frames, " 
          << silence_frame_count << " silence frames, " 
          << frame_count << " total)" << std::endl;
```

## Expected New Logs

### Scenario 1: Audio IS Being Sent (Pipeline Working)
```
🎵 Outbound RTP thread started for call X on port 10001
📡 RTP send #1: 160 bytes to 192.168.10.5:XXXXX (port 10001)
📡 RTP send #2: 160 bytes to 192.168.10.5:XXXXX (port 10001)
...
[User speaks, LLaMA responds]
t1: LLaMA send-to-Kokoro [call X] ts=...
⏱️  Kokoro pipeline first audio: 669ms
t2: Kokoro first subchunk sent [call X] ts=...
⏱️  Outbound received first chunk from Kokoro ts=...
t3: First RTP frame sent [call X] ts=...
🎤 Started sending AUDIO frames (was silence) on port 10001  ← NEW!
🔊 Sent audio frame #1 (160 bytes) on port 10001
🔊 Sent audio frame #50 (160 bytes) on port 10001
...
🔇 Switched back to SILENCE frames (audio ended) on port 10001  ← NEW!
...
Outbound stream thread exiting (sent 250 audio frames, 621 silence frames, 871 total)  ← NEW!
```

**Interpretation**: Audio IS being sent! Problem is elsewhere (codec, network, phone).

### Scenario 2: Audio NOT Being Sent (Pipeline Broken)
```
🎵 Outbound RTP thread started for call X on port 10001
📡 RTP send #1: 160 bytes to 192.168.10.5:XXXXX (port 10001)
...
[User speaks, LLaMA responds]
t1: LLaMA send-to-Kokoro [call X] ts=...
⏱️  Kokoro pipeline first audio: 669ms
t2: Kokoro first subchunk sent [call X] ts=...
⏱️  Outbound received first chunk from Kokoro ts=...
t3: First RTP frame sent [call X] ts=...
[NO "🎤 Started sending AUDIO frames" message!]  ← PROBLEM!
...
Outbound stream thread exiting (sent 0 audio frames, 871 silence frames, 871 total)  ← PROBLEM!
```

**Interpretation**: Audio is NOT reaching the SIP client! Problem in SHM communication.

## Testing Instructions

### 1. Restart Services
```bash
cd /Users/whisper/Documents/augment-projects/clean-repo
pkill -f "sip-client|llama-service|whisper-service|kokoro|http-server|inbound-audio|outbound-audio"
./start-wildfire.sh
```

### 2. Make Test Call

Place a call and speak. Wait for LLaMA to respond.

### 3. Check Logs

**Look for these key messages**:

1. **Audio synthesis**:
   ```
   t1: LLaMA send-to-Kokoro [call X] ts=...
   ⏱️  Kokoro pipeline first audio: Xms
   t2: Kokoro first subchunk sent [call X] ts=...
   ```

2. **Audio reception by outbound processor**:
   ```
   ⏱️  Outbound received first chunk from Kokoro ts=...
   t3: First RTP frame sent [call X] ts=...
   ```

3. **Audio transmission by SIP client** (NEW):
   ```
   🎤 Started sending AUDIO frames (was silence) on port 10001
   ```

4. **Audio end** (NEW):
   ```
   🔇 Switched back to SILENCE frames (audio ended) on port 10001
   ```

5. **Final statistics** (NEW):
   ```
   Outbound stream thread exiting (sent X audio frames, Y silence frames, Z total)
   ```

## Diagnosis

### If You See "🎤 Started sending AUDIO frames"

**Audio IS being sent!** The problem is NOT in the pipeline. Check:

1. **Codec Compatibility**
   - Log shows: `🎯 Selected outbound RTP PT based on inbound: 8 (PCMA)`
   - PT=8 is G.711 A-law
   - Verify your phone supports G.711 A-law
   - If not, check if PT=0 (G.711 μ-law) is available

2. **Network/Firewall**
   - RTP is UDP, can be blocked by firewalls
   - Check if outbound UDP on port 10001 is allowed
   - Check if phone's firewall is blocking incoming RTP

3. **Phone Configuration**
   - Check phone's audio settings
   - Check speaker volume
   - Check if phone is in "mute" mode
   - Try a different phone/softphone

4. **Audio Level**
   - Kokoro audio might be too quiet
   - Check if audio conversion (24kHz → 8kHz) is reducing volume
   - Check if G.711 encoding is correct

### If You DON'T See "🎤 Started sending AUDIO frames"

**Audio is NOT being sent!** The problem IS in the pipeline. Possible causes:

1. **SHM Communication Broken**
   - Outbound processor writes to `/ap_out_140`
   - SIP client reads from `/ap_out_140`
   - Check if they're using the same SHM channel
   - Check SHM permissions

2. **Audio Data Corrupted**
   - Outbound processor receives audio from Kokoro
   - Converts 24kHz float32 → 8kHz G.711
   - Conversion might be producing all 0xFF (silence)
   - Check conversion logic

3. **Timing Issue**
   - SIP client might be reading from SHM before audio arrives
   - Check if `t3` timestamp matches `🎤 Started sending AUDIO` timestamp

## Next Steps

After you run the test call, share the logs and I'll analyze:

1. **Did you see "🎤 Started sending AUDIO frames"?**
   - YES → Problem is codec/network/phone (not pipeline)
   - NO → Problem is in pipeline (SHM or conversion)

2. **What were the final statistics?**
   - Example: `(sent 250 audio frames, 621 silence frames, 871 total)`
   - If audio frames = 0, pipeline is broken
   - If audio frames > 0, pipeline is working

3. **Did the timing make sense?**
   - `t3` should happen ~11ms after `t2`
   - `🎤 Started sending AUDIO` should happen ~20ms after `t3`
   - If timing is off, there might be a synchronization issue

## Technical Details

### G.711 Silence Detection

The code checks if a frame is silence by looking for all 0xFF bytes:
```cpp
bool all_silence = true;
for (size_t i = 0; i < 160; ++i) {
    if (frame[i] != 0xFF) {
        all_silence = false;
        break;
    }
}
```

**Why 0xFF?**
- G.711 μ-law: 0xFF = silence (-0 dBm0)
- G.711 A-law: 0xD5 = silence (but outbound processor uses 0xFF)

**Limitation**: If Kokoro generates very quiet audio that happens to be all 0xFF after G.711 encoding, it will be detected as silence. This is unlikely but possible.

### Audio Pipeline Flow

```
Kokoro (24kHz float32)
  ↓ TCP (40ms subchunks)
Outbound Processor
  ↓ Resample 24kHz → 8kHz
  ↓ Convert float32 → G.711 μ-law
  ↓ Write 160-byte frames to SHM (/ap_out_X)
SIP Client
  ↓ Read 160-byte frames from SHM
  ↓ Check if audio (not all 0xFF)
  ↓ Send as RTP packets (20ms intervals)
Phone
```

### Frame Timing

- **Kokoro**: Sends 40ms subchunks (960 samples @ 24kHz)
- **Outbound Processor**: Converts to 8kHz (320 samples) and splits into 2x 160-byte frames
- **SIP Client**: Sends 1 frame every 20ms (160 bytes = 20ms @ 8kHz)

So each Kokoro subchunk produces 2 RTP packets.

## Summary

**Changes**: Enhanced logging to distinguish audio from silence
**Goal**: Determine if audio is actually being sent or if only silence is being sent
**Next**: Run test call and check for "🎤 Started sending AUDIO frames" message

If you see that message, the pipeline is working and the problem is elsewhere.
If you don't see it, the pipeline is broken and we need to investigate SHM/conversion.

