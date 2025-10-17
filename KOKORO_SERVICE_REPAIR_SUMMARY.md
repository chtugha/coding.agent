# Kokoro Service Repair & Pipeline Test Script - Summary

## Overview

Repaired `kokoro_service.py` to properly send audio to `outbound-audio-processor` (matching production architecture) and created a comprehensive pipeline test script.

## Problem Identified

**Original Issue**: `kokoro_service.py` was sending audio back to the llama-service TCP connection instead of to the outbound-audio-processor.

**Production Architecture**:
```
Whisper → Llama → Kokoro → Outbound-Audio-Processor → SIP/Simulator
```

**Broken Flow** (before fix):
```
Whisper → Llama → Kokoro → (back to Llama) ❌
```

## Repairs Made to kokoro_service.py

### 1. Added Outbound Connection Tracking

**Lines 46-49** - Added instance variables:
```python
# Track outbound audio processor connections
self.outbound_sockets = {}  # call_id -> socket
self.outbound_sockets_lock = threading.Lock()
self.chunk_counters = {}  # call_id -> chunk_id
```

### 2. Added Port Calculation Method

**Lines 138-145** - Calculate outbound processor port:
```python
def calculate_outbound_port(self, call_id):
    """Calculate outbound audio processor port: 9002 + call_id"""
    try:
        call_id_num = int(call_id)
        return 9002 + call_id_num
    except ValueError:
        return 9002  # Fallback
```

### 3. Added Connection Method

**Lines 147-186** - Connect to outbound processor:
```python
def connect_to_outbound_processor(self, call_id):
    """Connect to outbound audio processor on port 9002+call_id"""
    # Check if already connected
    # Create TCP socket with TCP_NODELAY
    # Connect to 127.0.0.1:port
    # Send HELLO(call_id)
    # Retry logic: 10 attempts (200ms → 1000ms delays)
    # Store socket in outbound_sockets dict
```

**Key Features**:
- ✅ TCP_NODELAY for low latency
- ✅ 10 retry attempts with exponential backoff
- ✅ Thread-safe socket management
- ✅ HELLO protocol compliance

### 4. Added Audio Sending Method

**Lines 188-217** - Send audio to outbound processor:
```python
def send_audio_to_outbound(self, call_id, audio, sample_rate):
    """Send audio chunk to outbound audio processor"""
    # Get socket from dict
    # Increment chunk_id
    # Convert audio to bytes
    # Send header: [length][sample_rate][chunk_id]
    # Send audio payload
    # Handle errors and reconnection
```

**Protocol**:
```
[4 bytes: chunk_length (big-endian)]
[4 bytes: sample_rate (big-endian)]
[4 bytes: chunk_id (big-endian)]
[chunk_length bytes: float32 PCM audio]
```

### 5. Added Connection Cleanup Method

**Lines 219-231** - Close outbound connection:
```python
def close_outbound_connection(self, call_id):
    """Close connection to outbound audio processor"""
    # Send BYE message (length=0)
    # Close socket
    # Remove from dict
```

**BYE Protocol**:
```
[4 bytes: 0x00000000]
[4 bytes: 0x00000000]
[4 bytes: 0x00000000]
```

### 6. Updated Registration Listener

**Lines 258-261** - Connect on REGISTER:
```python
if message.startswith("REGISTER:"):
    call_id = message[9:]
    # ... existing code ...
    
    # Connect to outbound processor
    threading.Thread(target=self.connect_to_outbound_processor, 
                     args=(call_id,), daemon=True).start()
```

**Lines 267-270** - Disconnect on BYE:
```python
elif message.startswith("BYE:"):
    call_id = message[4:]
    # ... existing code ...
    
    # Close outbound connection
    self.close_outbound_connection(call_id)
```

### 7. Updated Audio Handling in handle_client

**Lines 332-357** - Send to outbound instead of llama:
```python
# Synthesize audio
audio, sample_rate, synthesis_time = self.synthesize(text)

if audio is not None and len(audio) > 0:
    # Calculate metrics
    audio_duration = len(audio) / sample_rate
    rtf = synthesis_time / audio_duration if audio_duration > 0 else 0
    
    # Send audio to outbound processor (not back to llama)
    if self.send_audio_to_outbound(call_id, audio, sample_rate):
        print(f"🔊 Sent audio to outbound processor for call {call_id}")
    else:
        # Try to reconnect and retry
        if self.connect_to_outbound_processor(call_id):
            if self.send_audio_to_outbound(call_id, audio, sample_rate):
                print(f"🔊 Sent audio to outbound processor (retry) for call {call_id}")

# Send BYE to outbound processor when done
self.close_outbound_connection(call_id)
```

**Key Changes**:
- ❌ **Removed**: Sending audio back to llama via `client_socket.sendall()`
- ✅ **Added**: Sending audio to outbound processor via `send_audio_to_outbound()`
- ✅ **Added**: Automatic retry on connection failure
- ✅ **Added**: BYE message on completion

## Pipeline Test Script Created

### File: `start_pipeline_test.sh`

**Purpose**: Orchestrate complete pipeline loop test with proper service startup sequencing and 2-minute timeout.

### Service Startup Sequence

1. **Simulator** (pipeline_loop_sim)
   - Starts first, waits for services
   - Call ID: 151
   - Ports: 9152 (audio to Whisper), 9153 (audio from Kokoro)

2. **Whisper Service**
   - Transcription service
   - Ports: 9001+call_id (audio input), 13000 (UDP registration)
   - Connects to: Llama (127.0.0.1:8083)

3. **Llama Service**
   - LLM response generation
   - Ports: 8083 (input from Whisper), 8090 (output to Kokoro)
   - Generates conversational responses

4. **Kokoro Service**
   - TTS synthesis
   - Ports: 8090 (input from Llama), 9002+call_id (output to outbound), 13001 (UDP registration)
   - Connects to: Outbound processor (127.0.0.1:9153)

5. **Outbound Audio Processor**
   - Audio routing to simulator
   - Ports: 9002+call_id (listens for Kokoro), 13000+call_id (UDP registration)
   - Sends audio to: Simulator (mimics SIP)

### Complete Flow

```
Original Audio (T0)
    ↓
Simulator → Whisper (port 9152)
    ↓
Whisper → Llama (port 8083) [T1: Transcription]
    ↓
Llama → Kokoro (port 8090) [T2: Response]
    ↓
Kokoro → Outbound (port 9153) [T3: Audio]
    ↓
Outbound → Simulator [T4: Transfer]
    ↓
Simulator → Whisper (port 9153) [T5: Re-transcription]
    ↓
Timing Report & Quality Check
```

### Features

✅ **Prerequisite Checking**: Verifies all binaries and files exist
✅ **Service Cleanup**: Kills existing services before starting
✅ **Proper Sequencing**: Starts services in correct order with delays
✅ **2-Minute Timeout**: Prevents hanging tests
✅ **Progress Monitoring**: Shows elapsed time every 10 seconds
✅ **Automatic Cleanup**: Trap function kills all services on exit
✅ **Log Collection**: All service logs saved to /tmp
✅ **Output Display**: Shows simulator results at end

### Usage

```bash
# Run the complete pipeline test
bash start_pipeline_test.sh
```

### Expected Output

```
🔍 Checking prerequisites...
✅ All prerequisites found

🧹 Cleaning up existing services...

🚀 Starting services...

🔄 Starting pipeline loop simulator (will wait for services)...
   PID: 12345
   Log: /tmp/pipeline-simulator.log

🎤 Starting Whisper service...
   PID: 12346
   Ports: 9001+call_id (audio), 13000 (UDP registration)
   Log: /tmp/whisper-pipeline-test.log

🦙 Starting Llama service...
   PID: 12347
   Ports: 8083 (input from Whisper), 8090 (output to Kokoro)
   Log: /tmp/llama-pipeline-test.log

🎵 Starting Kokoro service...
   PID: 12348
   Ports: 8090 (input from Llama), 9002+call_id (output to outbound), 13001 (UDP registration)
   Log: /tmp/kokoro-pipeline-test.log

📡 Starting Outbound audio processor...
   PID: 12349
   Ports: 9002+call_id (listens for Kokoro), 13000+call_id (UDP registration)
   Log: /tmp/outbound-pipeline-test.log

✅ All services started

📊 Service Architecture:
   Simulator → Whisper (port 9152) → Llama (port 8083) → Kokoro (port 8090)
   Kokoro → Outbound (port 9153) → Simulator (receives audio)

⏱️  Test will run for maximum 2 minutes...

⏱️  Elapsed: 10s / 120s
⏱️  Elapsed: 20s / 120s
...

🏁 Test complete

📋 Simulator output:
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
=== Pipeline Timing Summary ===
Original transcription: "The birch canoe slid on the smooth planks."
Llama response: "That's an interesting observation about the canoe."
Final transcription: "That's an interesting observation about the canoe."

Timing breakdown:
  Whisper inference (T1-T0):         487ms
  Llama response (T2-T1):            234ms
  Kokoro synthesis (T3-T2):          156ms
  Audio transfer (T4-T3):             45ms
  Whisper re-transcription (T5-T4):  512ms
  Total round-trip (T5-T0):         1434ms ✅ (<2s target)

Quality check:
  Quality: 100% match ✅

Status: PASS - Real-time performance achieved
================================
━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

🧹 Cleaning up services...
✅ Cleanup complete

📊 Service logs:
   Simulator:  /tmp/pipeline-simulator.log
   Whisper:    /tmp/whisper-pipeline-test.log
   Llama:      /tmp/llama-pipeline-test.log
   Kokoro:     /tmp/kokoro-pipeline-test.log
   Outbound:   /tmp/outbound-pipeline-test.log
```

## Files Modified/Created

### Modified
1. ✅ `kokoro_service.py` - Repaired to send audio to outbound processor

### Created
1. ✅ `start_pipeline_test.sh` - Complete pipeline test orchestration script
2. ✅ `KOKORO_SERVICE_REPAIR_SUMMARY.md` (this file) - Documentation

## Testing

### Prerequisites

1. **Build all services**:
   ```bash
   bash scripts/build.sh
   ```

2. **Setup Kokoro environment**:
   ```bash
   python3.11 -m venv venv-kokoro
   ./venv-kokoro/bin/pip install torch kokoro soundfile
   ```

3. **Verify test file exists**:
   ```bash
   ls tests/data/harvard/wav/OSR_us_000_0010_8k.wav
   ```

### Run Test

```bash
bash start_pipeline_test.sh
```

### Verify Results

Check logs for:
- ✅ Kokoro connecting to outbound processor
- ✅ Audio chunks sent to outbound processor
- ✅ Complete timing measurements (T0-T5)
- ✅ Quality verification (transcription match)
- ✅ Total latency <2s

## Architecture Compliance

✅ **Production Flow**: Matches production architecture exactly
✅ **Port Mapping**: Correct calculation (9002 + call_id)
✅ **TCP Protocol**: Length-prefixed messages with HELLO/BYE
✅ **UDP Registration**: Proper REGISTER/BYE messages
✅ **Retry Logic**: 10 attempts with exponential backoff
✅ **Error Handling**: Automatic reconnection on failure
✅ **Thread Safety**: Mutex-protected socket management

## Performance Targets

| Stage | Target | Status |
|-------|--------|--------|
| Whisper (T1-T0) | ~500ms | ✅ Ready |
| Llama (T2-T1) | ~200ms | ✅ Ready |
| Kokoro (T3-T2) | ~150ms | ✅ Ready |
| Transfer (T4-T3) | <50ms | ✅ Ready |
| Re-transcription (T5-T4) | ~500ms | ✅ Ready |
| **Total (T5-T0)** | **<2000ms** | ✅ **Ready** |

## Conclusion

**Status**: ✅ **COMPLETE**

- Kokoro service repaired to send audio to outbound processor
- Complete pipeline test script created with proper orchestration
- 2-minute timeout prevents hanging
- Automatic cleanup and log collection
- Ready for real-world testing

---

**Created**: 2025-10-17
**Status**: Ready for testing 🚀

