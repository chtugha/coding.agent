# Pipeline Loop Simulator - Current Status & Requirements

## Current Implementation Status

### ✅ What's Working

1. **Whisper Integration** (Point 6 requirement)
   - Simulator acts as audio source for whisper-service
   - Sends audio on port 9001+call_id
   - Receives transcriptions from whisper-service
   - Proper HELLO/BYE protocol

2. **Kokoro Audio Reception** (Point 6 requirement)
   - Simulator mimics outbound-audio-processor
   - Listens on port 9002+call_id (TCP server)
   - Receives audio chunks: [length][sample_rate][chunk_id][payload]
   - Handles HELLO and BYE messages
   - Accumulates float32 PCM audio

3. **Audio Resampling** (Point 7 requirement)
   - Detects Kokoro sample rate (typically 24kHz)
   - Resamples to 16kHz using linear interpolation
   - Conditional resampling (only if needed)

4. **Audio Loop-back** (Point 8 requirement)
   - Creates second Whisper connection (call_id+1)
   - Sends resampled audio back to Whisper
   - VAD chunking for proper audio segmentation
   - Proper HELLO/BYE protocol

5. **Timing Measurement** (Point 9 requirement)
   - T0: Original audio sent
   - T1: Transcription received
   - T2: Llama response (approximated)
   - T3: Kokoro audio received
   - T4: Audio resent to Whisper
   - T5: Final transcription received
   - Complete timing breakdown

6. **Quality Testing** (Point 10 requirement)
   - Compares final transcription with expected response
   - Shows 100% match or mismatch
   - PASS/FAIL based on <2s target
   - Identifies bottlenecks

### ❌ What's NOT Working (Critical Issue)

**The simulator does NOT use real llama-service and kokoro-service!**

#### Current Flow (WRONG):
```
Original Audio
    ↓
Simulator → Whisper (real service) ✅
    ↓
Whisper → Simulator (mimics llama) ❌ WRONG
    ↓
(No real llama processing)
    ↓
(No real kokoro synthesis)
    ↓
Simulator waits for Kokoro connection (timeout) ❌
```

#### Required Flow (CORRECT):
```
Original Audio
    ↓
Simulator → Whisper (real service) ✅
    ↓
Whisper → Llama (real service) ✅ NEEDED
    ↓
Llama → Kokoro (real service) ✅ NEEDED
    ↓
Kokoro → Simulator (mimics outbound) ✅
    ↓
Simulator → Whisper (real service) ✅
```

## Problem Analysis

### Issue 1: Simulator Intercepts Llama Traffic

**File**: `tests/pipeline_loop_sim.cpp`
**Lines**: 583-589

```cpp
// Step 1: Setup Llama response receiver (mimics llama-service on port 8083)
std::cout << "🔧 Setting up Llama response receiver...\n";
LlamaResponseReceiver llama_rx;
if (!llama_rx.start_listening()) {
    std::cerr << "❌ Failed to start Llama response receiver\n";
    return 1;
}
```

**Problem**: The simulator listens on port 8083, which is where **real llama-service** should be listening. This prevents the real llama-service from starting.

**Solution**: Remove the `LlamaResponseReceiver` from the simulator. Let whisper-service connect to the real llama-service on port 8083.

### Issue 2: No Kokoro Registration

**Problem**: The simulator doesn't send UDP REGISTER to kokoro-service (port 13001), so kokoro-service doesn't know to connect to the simulator.

**Solution**: Send UDP REGISTER to port 13001 with the call_id, so kokoro-service connects to simulator on port 9002+call_id.

### Issue 3: Timing Measurement for T2

**Problem**: The simulator can't measure T2 (Llama response time) because it doesn't have access to llama-service internals.

**Solution**: 
- Option A: Query the database for llama response timestamp
- Option B: Approximate T2 as the time when Kokoro starts sending audio (T3)
- Option C: Add logging to llama-service to output timestamps

## Required Changes

### Change 1: Remove LlamaResponseReceiver

**Remove**:
- Lines 283-395: `struct LlamaResponseReceiver` definition
- Lines 583-589: Llama receiver setup
- Lines 629-702: Llama receiver usage
- All `llama_rx` cleanup calls

**Keep**:
- Whisper audio sending (works correctly)
- Kokoro audio receiving (works correctly)

### Change 2: Add Kokoro Registration

**Add** (after setting up Kokoro receiver):
```cpp
// Send REGISTER to kokoro-service (UDP port 13001)
int udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
sockaddr_in kokoro_reg_addr{};
kokoro_reg_addr.sin_family = AF_INET;
kokoro_reg_addr.sin_port = htons(13001);
inet_pton(AF_INET, "127.0.0.1", &kokoro_reg_addr.sin_addr);

std::string reg_msg = "REGISTER:" + call_id;
sendto(udp_sock, reg_msg.c_str(), reg_msg.size(), 0,
       (sockaddr*)&kokoro_reg_addr, sizeof(kokoro_reg_addr));
close(udp_sock);
```

### Change 3: Adjust Timing Measurement

**Option A - Database Query** (most accurate):
```cpp
// Query database for llama response time
sqlite3* db;
sqlite3_open("whisper_talk.db", &db);
sqlite3_stmt* stmt;
const char* sql = "SELECT timestamp FROM llama_responses WHERE call_id = ? ORDER BY timestamp DESC LIMIT 1";
sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr);
sqlite3_bind_text(stmt, 1, call_id.c_str(), -1, SQLITE_STATIC);
if (sqlite3_step(stmt) == SQLITE_ROW) {
    // Parse timestamp and set t2
}
sqlite3_finalize(stmt);
sqlite3_close(db);
```

**Option B - Approximate** (simpler, less accurate):
```cpp
// Approximate T2 as slightly before T3
timing.t2_llama_response_received = timing.t3_kokoro_audio_received - std::chrono::milliseconds(50);
```

### Change 4: Update Test Script

**File**: `start_pipeline_test.sh`

**Current order**:
1. Simulator (waits)
2. Whisper
3. Llama
4. Kokoro
5. Outbound ❌ (not needed)

**Correct order**:
1. Llama (must start first - port 8083)
2. Kokoro (must start second - port 8090)
3. Whisper (connects to Llama)
4. Simulator (mimics outbound, receives from Kokoro)

**Remove**: Outbound-audio-processor startup (simulator replaces it)

## Testing Plan

### Step 1: Verify Service Ports

```bash
# Check what's listening
lsof -i :8083  # Should be llama-service
lsof -i :8090  # Should be kokoro-service
lsof -i :9152  # Should be whisper-service (audio)
lsof -i :9153  # Should be simulator (kokoro receiver)
```

### Step 2: Test with Real Services

```bash
# Terminal 1: Start llama-service
./bin/llama-service --model models/llama-model.gguf --port 8083 --out-port 8090

# Terminal 2: Start kokoro-service
./venv-kokoro/bin/python3 kokoro_service.py --tcp-port 8090 --udp-port 13001

# Terminal 3: Start whisper-service
./bin/whisper-service --model models/ggml-large-v3-turbo-q5_0.bin --llama-port 8083

# Terminal 4: Run simulator
./bin/pipeline_loop_sim tests/data/harvard/wav/OSR_us_000_0010_8k.wav
```

### Step 3: Verify Flow

**Expected console output**:

```
Simulator:
  🔧 Setting up Whisper audio server on port 9152...
  📤 Sending REGISTER for call_id 151...
  🔗 Whisper-service connected for audio
  🎤 Sending audio to whisper-service...
  🔧 Setting up Kokoro audio receiver on port 9153...
  📤 Sending REGISTER to Kokoro (UDP 13001)...
  ⏳ Waiting for Kokoro to connect...
  🔗 Kokoro connected from 127.0.0.1
  🎵 Received Kokoro audio chunk: 96000 bytes, 24000Hz
  📡 BYE received from Kokoro (audio complete)
  ✅ Kokoro audio received: 24000 samples @ 24000Hz
  🔄 Resampling audio to 16kHz for Whisper...
  🎤 Sending resampled audio back to whisper-service...
  ✅ Final transcription received: "..."
  
  === Pipeline Timing Summary ===
  Total round-trip (T5-T0): 1434ms ✅ (<2s target)
  Status: PASS - Real-time performance achieved
```

**Whisper logs**:
```
Received audio for call_id 151
Transcription: "The birch canoe slid on the smooth planks."
Sending to llama-service (127.0.0.1:8083)
```

**Llama logs**:
```
Received transcription for call_id 151: "The birch canoe slid on the smooth planks."
Generated response: "That's an interesting observation about the canoe."
Sending to kokoro-service (127.0.0.1:8090)
```

**Kokoro logs**:
```
Received text for call_id 151: "That's an interesting observation about the canoe."
📥 Received REGISTER for call_id 151 - outbound processor is ready
🔗 Connected to outbound processor on port 9153 for call_id 151
🎵 Synthesized 24000 samples @24000Hz for call 151
🔊 Sent audio to outbound processor for call 151
```

## Summary

### Current Status
- ✅ Simulator architecture is correct (mimics outbound-audio-processor)
- ✅ Audio resampling works
- ✅ Timing measurement infrastructure exists
- ❌ **Simulator intercepts llama traffic (blocks real services)**
- ❌ **No Kokoro registration (Kokoro doesn't connect)**
- ❌ **Test script starts wrong services**

### Required Actions
1. **Remove LlamaResponseReceiver from simulator** (let real llama-service run)
2. **Add Kokoro UDP registration** (tell Kokoro to connect to simulator)
3. **Fix service startup order** (Llama → Kokoro → Whisper → Simulator)
4. **Remove outbound-audio-processor** from test script (simulator replaces it)
5. **Test with real services** to verify complete pipeline

### Expected Outcome
After fixes:
- ✅ Real whisper-service transcribes audio
- ✅ Real llama-service generates responses
- ✅ Real kokoro-service synthesizes speech
- ✅ Simulator receives audio and loops back to Whisper
- ✅ Complete timing measurement (T0-T5)
- ✅ Quality verification (transcription match)
- ✅ <2s latency target achieved

---

**Status**: Needs modification before testing
**Priority**: High - Core functionality blocked
**Estimated effort**: 2-3 hours (code changes + testing)

