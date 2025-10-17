# Pipeline Loop Simulator - Complete Voice Processing Loop Test

## Overview

The Pipeline Loop Simulator (`pipeline_loop_sim`) is an end-to-end test tool that measures and optimizes the complete voice processing pipeline:

```
Original Audio → Whisper → Llama → Kokoro → Whisper (re-transcription)
```

This creates a complete conversational AI loop where:
1. Original audio is transcribed by Whisper
2. Transcription is sent to Llama for response generation
3. Llama response is synthesized by Kokoro TTS
4. Synthesized audio is sent back to Whisper for verification
5. Final transcription is compared with Llama response for quality check

## Objective

Measure and optimize the transfer time from llama-service output through kokoro-service speech synthesis back to whisper-service input, ensuring **real-time performance (<2s total latency)**.

## Architecture

### Service Flow

```
┌─────────────────────────────────────────────────────────────────┐
│                     Pipeline Loop Simulator                      │
│                                                                   │
│  1. Sends original audio to Whisper (port 9001+call_id)         │
│  2. Receives transcription from Whisper (mimics Llama on 8083)  │
│  3. Waits for Llama to generate response                         │
│  4. Waits for Kokoro to synthesize audio                         │
│  5. Receives synthesized audio from Kokoro (port 9002+call_id)  │
│  6. Sends synthesized audio back to Whisper                      │
│  7. Receives final transcription                                 │
│  8. Measures timing and quality                                  │
└─────────────────────────────────────────────────────────────────┘
         │                    │                    │
         ▼                    ▼                    ▼
   ┌──────────┐         ┌──────────┐        ┌──────────┐
   │ Whisper  │────────▶│  Llama   │───────▶│ Kokoro   │
   │ Service  │         │ Service  │        │ Service  │
   │ (port    │         │ (port    │        │ (port    │
   │ 9001+id) │         │ 8083)    │        │ 8090)    │
   └──────────┘         └──────────┘        └──────────┘
         ▲                                        │
         │                                        │
         └────────────────────────────────────────┘
                  Audio feedback loop
```

### Port Assignments

| Service | Port | Purpose |
|---------|------|---------|
| Whisper (audio in) | 9001 + call_id | Receives audio for transcription |
| Whisper → Llama | 8083 | Sends transcriptions to Llama |
| Llama → Kokoro | 8090 | Sends text to Kokoro for synthesis |
| Kokoro → Outbound | 9002 + call_id | Sends synthesized audio |
| Whisper (registration) | 13000 | UDP registration messages |
| Kokoro (registration) | 13001 | UDP registration messages |

### Call ID

- Fixed call_id: `151`
- Whisper audio port: `9152` (9001 + 151)
- Kokoro audio port: `9153` (9002 + 151)

## Implementation Details

### Key Features

1. **Single WAV File Test**: Simplified to test one file at a time for focused latency measurement
2. **Production Architecture**: Uses same TCP connections and protocols as production services
3. **Timing Measurement**: Captures 6 timing points (T0-T5) for detailed latency breakdown
4. **Quality Verification**: Compares final transcription with Llama response
5. **2-Minute Timeout**: Prevents hanging on service failures

### Timing Points

| Point | Description | Measurement |
|-------|-------------|-------------|
| **T0** | Original audio sent to Whisper | Start time |
| **T1** | Transcription received from Whisper | Whisper latency (T1-T0) |
| **T2** | Llama response generated | Llama latency (T2-T1) |
| **T3** | Kokoro audio received | Kokoro synthesis (T3-T2) |
| **T4** | Audio sent back to Whisper | Transfer time (T4-T3) |
| **T5** | Final transcription received | Re-transcription (T5-T4) |

**Total Round-Trip**: T5 - T0 (target: <2000ms)

### Protocol Details

#### TCP Length-Prefixed Protocol

All TCP messages use 4-byte big-endian length prefix:

```
[4 bytes: length] [payload]
```

#### HELLO Message

```
[4 bytes: call_id length] [call_id string]
```

#### Audio Chunk (Whisper)

```
[4 bytes: byte length] [float32 PCM samples]
```

#### Audio Chunk (Kokoro)

```
[4 bytes: chunk length] [4 bytes: sample rate] [4 bytes: chunk_id] [float32 PCM samples]
```

#### BYE Message

```
[4 bytes: 0xFFFFFFFF]
```

## Usage

### Prerequisites

1. **Build the simulator**:
   ```bash
   bash scripts/build.sh
   ```

2. **Ensure services are available**:
   - `bin/whisper-service` (built)
   - `bin/llama-service` (built)
   - `kokoro_service.py` (Python script)
   - `venv-kokoro/bin/python3` (virtual environment with Kokoro)

3. **Test WAV file**:
   - Default: `tests/data/harvard/wav/OSR_us_000_0010_8k.wav`
   - Format: 8kHz or 16kHz, 16-bit PCM, mono or stereo

### Running the Test

#### Option 1: Automated Test Script (Recommended)

```bash
bash test_pipeline_loop.sh
```

This script:
- Starts all required services (Whisper, Llama, Kokoro)
- Runs the pipeline loop simulator
- Cleans up services on exit
- Shows service logs

#### Option 2: Manual Service Management

1. **Start Whisper service**:
   ```bash
   ./bin/whisper-service \
     --model ./models/ggml-large-v3-turbo-q5_0.bin \
     --database ./whisper_talk.db \
     --threads 8 \
     --llama-host 127.0.0.1 \
     --llama-port 8083 &
   ```

2. **Start Llama service**:
   ```bash
   ./bin/llama-service \
     --model ./models/llama-model.gguf \
     --database ./whisper_talk.db \
     --port 8083 \
     --threads 8 \
     --out-host 127.0.0.1 \
     --out-port 8090 &
   ```

3. **Start Kokoro service**:
   ```bash
   ./venv-kokoro/bin/python3 -u ./kokoro_service.py \
     --voice af_sky \
     --tcp-port 8090 \
     --udp-port 13001 \
     --device mps &
   ```

4. **Run simulator**:
   ```bash
   ./bin/pipeline_loop_sim ./tests/data/harvard/wav/OSR_us_000_0010_8k.wav
   ```

### Expected Output

```
=== Pipeline Loop Test ===
Input: OSR_us_000_0010_8k.wav
Call ID: 151
Whisper audio port: 9152

✅ Loaded audio: 80000 samples @ 16kHz (5.0s)

🔧 Setting up Llama response receiver...
🦙 Simulator listening for Whisper transcriptions on TCP port 8083
🔧 Setting up Whisper audio server on port 9152...
📤 Sending REGISTER for call_id 151...
⏳ Waiting for whisper-service to connect...
🔗 Whisper-service connected for audio
📡 HELLO sent to whisper-service: 151

⏳ Waiting for whisper-service to connect to Llama receiver...
🔗 Whisper connected to simulator on port 8083
👋 HELLO from Whisper: call_id=151
✅ Llama receiver ready

🎤 Sending original audio to whisper-service...
📦 Sent chunk: 12800 samples
📦 Sent chunk: 16000 samples
📡 BYE sent to audio socket

⏳ Waiting for transcription from whisper-service...
📝 Transcription RX: The birch canoe slid on the smooth planks.
✅ Transcription received: "The birch canoe slid on the smooth planks."

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
```

## Current Status

### ✅ Phase 2 Complete - Full Loop Implementation

1. **Basic simulator structure** ✅ - Single WAV file test
2. **Whisper audio sending** ✅ - VAD chunking and TCP protocol
3. **Transcription receiving** ✅ - Mimics llama-service on port 8083
4. **Timing infrastructure** ✅ - PipelineTiming structure with T0-T5
5. **Build integration** ✅ - CMakeLists.txt updated
6. **Test script** ✅ - Automated service management
7. **Kokoro audio receiving** ✅ - Listens on port 9002+call_id
8. **Audio resampling** ✅ - 24kHz → 16kHz conversion
9. **Audio re-sending** ✅ - VAD-chunked back to Whisper
10. **Final transcription** ✅ - Receives and verifies quality
11. **Complete timing** ✅ - All T0-T5 measurements
12. **Quality verification** ✅ - Compares transcriptions

### Complete Loop Flow

```
✅ T0: Original audio → Whisper
✅ T1: Transcription received
✅ T2: Llama response (via real llama-service)
✅ T3: Kokoro audio received (via real kokoro-service)
✅ T4: Audio resampled and re-sent to Whisper
✅ T5: Final transcription received
✅ Timing summary printed
✅ Quality check performed
```

### Implementation Details

1. **Kokoro Audio Reception** (T3)
   - Listens on port 9002+call_id (matches outbound-audio-processor)
   - Accepts connection from Kokoro service
   - Receives audio chunks with protocol: [length][sample_rate][chunk_id][payload]
   - Accumulates all float32 PCM samples
   - Detects BYE message (length=0) for completion

2. **Audio Resampling** (T3→T4)
   - Detects Kokoro sample rate (typically 24kHz)
   - Resamples to 16kHz using linear interpolation
   - Maintains audio quality for Whisper

3. **Audio Re-sending** (T4)
   - Creates new call_id (call_id+1) to avoid conflicts
   - Sets up new audio server on port 9001+(call_id+1)
   - Sends REGISTER via UDP
   - VAD-chunks resampled audio
   - Sends to Whisper with proper protocol

4. **Final Transcription** (T5)
   - Receives transcription on original llama receiver (port 8083)
   - Captures T5 timestamp
   - Stores in timing structure

5. **Complete Timing Report**
   - Calculates all latencies (T1-T0, T2-T1, T3-T2, T4-T3, T5-T4)
   - Prints total round-trip (T5-T0)
   - Verifies <2s target
   - Shows quality match percentage

## Performance Targets

| Metric | Target | Acceptable | Notes |
|--------|--------|------------|-------|
| **Total Latency** | <1500ms | <2000ms | T5-T0 |
| **Whisper Inference** | ~500ms | <1000ms | T1-T0 |
| **Llama Response** | ~200ms | <500ms | T2-T1 |
| **Kokoro Synthesis** | ~150ms | <300ms | T3-T2 |
| **Audio Transfer** | <50ms | <100ms | T4-T3 |
| **Re-transcription** | ~500ms | <1000ms | T5-T4 |
| **Quality Match** | 100% | >95% | Transcription accuracy |

## Troubleshooting

### Services Not Starting

```bash
# Check if ports are in use
lsof -i :8083  # Llama port
lsof -i :8090  # Kokoro port
lsof -i :9152  # Whisper audio port

# Kill existing services
pkill -9 whisper-service
pkill -9 llama-service
pkill -f kokoro_service.py
```

### Timeout Errors

- **Whisper not connecting**: Check if whisper-service is running and listening on port 13000 for registrations
- **Transcription timeout**: Check whisper-service logs (`/tmp/whisper-pipeline-test.log`)
- **Llama timeout**: Check llama-service logs (`/tmp/llama-pipeline-test.log`)
- **Kokoro timeout**: Check kokoro-service logs (`/tmp/kokoro-pipeline-test.log`)

### Audio Format Issues

- Ensure WAV file is 16-bit PCM (8kHz or 16kHz)
- Simulator automatically resamples to 16kHz
- Kokoro outputs 24kHz float32 - needs resampling for Whisper

## Next Steps

1. **Complete Kokoro audio receiving** - Implement KokoroAudioReceiver connection and audio reception
2. **Implement audio resampling** - 24kHz → 16kHz for Whisper re-transcription
3. **Complete timing measurements** - Capture all T0-T5 timing points
4. **Add quality metrics** - WER calculation for final transcription
5. **Test with multiple files** - Verify consistency across different audio samples
6. **Optimize bottlenecks** - Identify and fix slow components
7. **Add stress testing** - Test with concurrent calls

## Files

- **Simulator**: `tests/pipeline_loop_sim.cpp`
- **Test Script**: `test_pipeline_loop.sh`
- **Documentation**: `PIPELINE_LOOP_SIMULATOR.md` (this file)
- **Build**: `CMakeLists.txt` (updated)

## References

- **Whisper Service**: `whisper-service.cpp`
- **Llama Service**: `llama-service.cpp`
- **Kokoro Service**: `kokoro_service.py`
- **Outbound Audio Processor**: `outbound-audio-processor.cpp` (reference for Kokoro audio format)
- **Inbound Simulator**: `tests/whisper_inbound_sim.cpp` (reference implementation)

