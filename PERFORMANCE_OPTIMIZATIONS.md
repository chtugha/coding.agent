# Real-Time Voice Conversation Performance Optimizations

## Summary
This document details all optimizations made to achieve real-time voice conversation performance with target latency < 2 seconds from user speech end to bot audio start.

## Target Performance Metrics
- **User speech → Whisper transcription**: < 600ms
- **Whisper → LLaMA response generation**: < 800ms  
- **LLaMA text ready (t1) → First audible audio (t3)**: < 300ms
- **Total user speech end → bot audio start**: < 2000ms

## Bottlenecks Identified from Log Analysis

### Critical Bottleneck: Kokoro TTS Synthesis Startup (300-700ms)
**Problem**: The t1→t2 latency (LLaMA sends text → Kokoro first subchunk) was 300-700ms
- First response: 685ms
- Second response: 344ms  
- Third response: 600ms

**Root Cause**: PyTorch model inference startup cost in the Kokoro pipeline generator

### Secondary Issues:
1. **Outbound conversion worker**: Processing only 1 job per 20ms tick added latency
2. **VAD cutting**: 120ms overlap insufficient, causing word loss
3. **LLaMA generation**: 80 token cap could be reduced further
4. **Whisper chunking**: 1.2s max chunk size delays transcription availability

---

## Optimizations Implemented

### 1. Kokoro TTS Optimizations (bin/kokoro_service.py)

#### A. Enhanced Warmup (Lines 65-83)
**Before**: Single warmup phrase "Hi."
**After**: Multiple warmup phrases to fully compile GPU kernels
```python
warmup_phrases = ["Hi.", "Hello there.", "Yes, I can help you."]
for phrase in warmup_phrases:
    # Synthesize and discard to warm up model
```
**Expected Impact**: Reduces cold-start penalty, subsequent syntheses should be 100-200ms faster

#### B. Reduced Subchunk Size (Line 338)
**Before**: 60ms subchunks (1440 samples @ 24kHz)
**After**: 40ms subchunks (960 samples @ 24kHz)
**Impact**: Audio starts flowing 20ms sooner per chunk

#### C. Added Detailed Timing Instrumentation (Lines 334-346)
```python
pipeline_start = time.time()
for result in self.pipeline(text, voice=self.voice, speed=1.0):
    if first_audio_time is None and result.audio is not None:
        first_audio_time = time.time()
        print(f"⏱️  Kokoro pipeline first audio: {(first_audio_time - pipeline_start)*1000:.1f}ms")
```
**Purpose**: Measure internal pipeline latency to identify further optimization opportunities

---

### 2. Outbound Audio Processor Optimizations (outbound-audio-processor.cpp)

#### A. Removed Conversion Rate Limiting (Lines 337-357)
**Before**: Processed at most 1 conversion job per 20ms tick
```cpp
while (buffered < 160 && jobs_processed < 1) {
    // Process only 1 job per tick
    jobs_processed++;
}
```

**After**: Process ALL available jobs immediately
```cpp
while (true) {
    ConversionJob job;
    // Get job from queue
    if (!has_job) break;
    // Process immediately
}
```
**Impact**: Eliminates artificial 20ms delays in conversion pipeline

#### B. Added Timing Instrumentation (Lines 598-605)
```cpp
if (chunk_id == 1) {
    auto now = std::chrono::system_clock::now();
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    std::cout << "⏱️  Outbound received first chunk from Kokoro ts=" << ms << std::endl;
}
```
**Purpose**: Track when outbound processor receives first chunk from Kokoro

---

### 3. VAD and Audio Chunking Optimizations (simple-audio-processor.cpp)

#### A. Increased Hangover Duration (Line 344)
**Before**: 300ms hangover
**After**: 400ms hangover
**Impact**: Prevents premature speech-end detection, reduces word clipping

#### B. Increased Overlap Between Chunks (Lines 402, 428)
**Before**: 120ms overlap (~1920 samples @ 16kHz)
**After**: 200ms overlap (~3200 samples @ 16kHz)
**Impact**: Ensures Whisper receives continuous audio context at chunk boundaries, prevents missing words

#### C. Reduced Max Chunk Size (Line 86)
**Before**: 1200ms (1.2 seconds)
**After**: 1000ms (1.0 seconds)
**Impact**: Reduces worst-case delay before transcription by 200ms

---

### 4. LLaMA Generation Optimizations (llama-service.cpp)

#### A. Reduced Max Token Generation (Line 386)
**Before**: 80 tokens max
**After**: 50 tokens max
**Impact**: Faster response generation (estimated 200-300ms reduction for typical responses)

#### B. Improved System Prompt (Lines 73-84)
**Before**: Bureaucratic "SHORT, DIRECT answers"
**After**: Warm, empathetic conversational style
```cpp
"You are a calm and patient phone assistant...
- Speak naturally and conversationally
- Be warm, understanding, and empathetic
- Keep responses brief (1-2 sentences) but kind"
```
**Impact**: Better conversational quality, naturally shorter responses

#### C. Strengthened Half-Duplex Gate (Lines 1141-1147)
**Before**: 14 chars/sec + 300ms margin
**After**: 12 chars/sec + 500ms margin + debug logging
```cpp
double secs = std::max(0.8, (double)text.size() / 12.0) + 0.5;
std::cout << "🔒 Half-duplex gate active for " << (int)(secs*1000) << "ms" << std::endl;
```
**Impact**: Prevents bot from talking over itself, more natural conversation flow

---

## Expected Performance Improvements

### Before Optimizations (from user logs):
- **Whisper inference**: ~500ms for 1.2s audio ✓ (acceptable)
- **LLaMA generation**: 300-1000ms (varies by response length)
- **t1→t2 (LLaMA→Kokoro)**: 300-700ms ❌ (too slow)
- **t2→t3 (Kokoro→RTP)**: ~2ms ✓ (excellent)
- **Total latency**: ~2-3 seconds ❌

### After Optimizations (estimated):
- **Whisper inference**: ~450ms for 1.0s audio (50ms faster)
- **LLaMA generation**: 200-600ms (200-400ms faster due to 50 token cap)
- **t1→t2 (LLaMA→Kokoro)**: 100-300ms (300-400ms faster after warmup)
- **t2→t3 (Kokoro→RTP)**: ~2ms (unchanged, already optimal)
- **Total latency**: ~1.0-1.5 seconds ✓ (target achieved)

---

## Validation Instructions

### Key Metrics to Monitor in Logs:

1. **Kokoro Warmup** (on service start):
```
✅ Warmup 'Hi.': XXXms
✅ Warmup 'Hello there.': XXXms
✅ Warmup 'Yes, I can help you.': XXXms
```
First warmup will be slow (~500-800ms), subsequent ones should be faster (~100-300ms)

2. **Per-Response Timing** (during call):
```
t1: LLaMA send-to-Kokoro [call X] ts=TIMESTAMP
⏱️  Kokoro pipeline first audio: XXXms
t2: Kokoro first subchunk sent [call X] ts=TIMESTAMP
⏱️  Outbound received first chunk from Kokoro ts=TIMESTAMP
t3: First RTP frame sent [call X] ts=TIMESTAMP
```

3. **Half-Duplex Gate** (should appear after each bot response):
```
🔒 [X] Half-duplex gate active for XXXms (text len=YY)
```

4. **VAD Quality** (check transcriptions are complete):
```
📝 [X] Transcription: [complete sentence without missing words]
🔴 VAD: speech end detected (hangover=400 ms) - sending immediately (overlap=200ms)
```

### Success Criteria:
- ✅ t1→t2 latency < 300ms (after first synthesis)
- ✅ No word clipping in transcriptions
- ✅ Bot waits until it finishes speaking before responding
- ✅ Total latency < 2 seconds for typical exchanges

---

## Additional Optimizations Considered But Not Implemented

### 1. Torch.compile() for Kokoro
**Reason**: Requires PyTorch 2.0+ and may not work with MPS backend
**Potential gain**: 20-30% faster inference
**Risk**: May break existing setup

### 2. Smaller Whisper Model
**Reason**: User specifically chose large-v3-turbo for quality
**Potential gain**: 200-300ms faster inference
**Trade-off**: Lower transcription accuracy

### 3. Streaming LLaMA Generation
**Reason**: Would require significant architecture changes
**Potential gain**: Could start TTS before full response is generated
**Complexity**: High

### 4. Pre-generate Common Responses
**Reason**: Limited applicability for dynamic conversations
**Potential gain**: Near-zero latency for cached responses
**Trade-off**: Less natural, limited coverage

---

## Files Modified

1. **bin/kokoro_service.py**
   - Enhanced warmup (lines 65-83)
   - Reduced subchunk size (line 338)
   - Added timing instrumentation (lines 334-346)

2. **outbound-audio-processor.cpp**
   - Removed conversion rate limiting (lines 337-357)
   - Added timing instrumentation (lines 598-605)

3. **simple-audio-processor.cpp**
   - Increased hangover (line 344)
   - Increased overlap (lines 402, 428)
   - Reduced max chunk size (line 86)

4. **llama-service.cpp**
   - Reduced max tokens (line 386)
   - Improved system prompt (lines 73-84)
   - Strengthened half-duplex gate (lines 1141-1147)

---

## Next Steps for User

1. **Restart all services** via start-wildfire.sh or web interface
2. **Place a test call** with 3-4 conversational turns
3. **Share logs** focusing on:
   - Kokoro warmup times
   - t1→t2→t3 timestamps for each response
   - Half-duplex gate activation logs
   - Transcription quality (any missing words?)
4. **Report perceived latency** - does it feel real-time now?

If latency is still too high after these optimizations, the next step would be to profile the Kokoro pipeline internals or consider alternative TTS engines with lower startup latency.

