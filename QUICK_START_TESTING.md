# Quick Start: Testing Real-Time Performance

## Services Have Been Rebuilt
All optimizations have been compiled. Services need to be restarted.

## Step 1: Restart Services

### Option A: Via start-wildfire.sh (Recommended)
```bash
cd /Users/whisper/Documents/augment-projects/clean-repo
./start-wildfire.sh
```
Then open http://localhost:8081 and start all services via the web interface.

### Option B: Manual Start (if start-wildfire.sh doesn't work)
```bash
cd /Users/whisper/Documents/augment-projects/clean-repo/bin

# Start Whisper
./whisper-service --database ../whisper_talk.db \
  --model ../models/ggml-large-v3-turbo-q5_0.bin \
  --llama-host 127.0.0.1 --llama-port 8083 &

# Start LLaMA
./llama-service --database ../whisper_talk.db \
  --model ../models/Llama-3.2-3B-Instruct-Phishing-v1.Q5_K_M.gguf \
  --output-host 127.0.0.1 --output-port 8090 &

# Start Kokoro
python3 kokoro_service.py &

# Start SIP Client
./sip-client --database ../whisper_talk.db &
```

## Step 2: Verify Services Are Running

```bash
ps aux | grep -E "(whisper-service|llama-service|kokoro|sip-client)" | grep -v grep
```

You should see 4 processes running.

## Step 3: Check Kokoro Warmup

```bash
# Look for warmup completion in logs
tail -100 /tmp/wildfire.log | grep -A 5 "Warming up Kokoro"
```

Expected output:
```
🔥 Warming up Kokoro pipeline...
✅ Warmup 'Hi.': XXXms
✅ Warmup 'Hello there.': XXXms
✅ Warmup 'Yes, I can help you.': XXXms
```

First warmup will be slow (500-800ms), subsequent ones should be faster (100-300ms).

## Step 4: Place Test Call

Call from extension 16 to Llama1.

### Test Conversation Script:
1. **You**: "Hello, can you hear me?"
2. **Bot**: [Should respond within 1-2 seconds]
3. **You**: "I need help with something."
4. **Bot**: [Should respond without talking over itself]
5. **You**: "Thank you, that's all."
6. **Bot**: [Should respond politely]

## Step 5: Analyze Performance

### A. Check Overall Latency
Look for these log patterns for EACH bot response:

```
t1: LLaMA send-to-Kokoro [call X] ts=TIMESTAMP_1
⏱️  Kokoro pipeline first audio: XXXms
t2: Kokoro first subchunk sent [call X] ts=TIMESTAMP_2
⏱️  Outbound received first chunk from Kokoro ts=TIMESTAMP_3
t3: First RTP frame sent [call X] ts=TIMESTAMP_4
```

**Calculate**:
- t1→t2 latency: TIMESTAMP_2 - TIMESTAMP_1 (target: < 300ms)
- t2→t3 latency: TIMESTAMP_4 - TIMESTAMP_2 (should be ~2-5ms)

### B. Check Half-Duplex Gate
After each bot response, you should see:
```
🔒 [X] Half-duplex gate active for XXXms (text len=YY)
```

If you DON'T see this, the bot may talk over itself.

### C. Check VAD Quality
Look for transcriptions:
```
📝 [X] Transcription: [your complete sentence]
🔴 VAD: speech end detected (hangover=400 ms) - sending immediately (overlap=200ms)
```

**Check**: Are your sentences complete? Any missing words?

### D. Check LLaMA Generation Speed
```
⏱️  LLaMA timing [X]: tokenize=Xms, decode=Xms, generate=Xms (Y tokens), total=Xms
```

With 50 token cap, generation should be 200-600ms for typical responses.

## Step 6: Share Results

Please share:

1. **Perceived latency**: Does it feel real-time? (< 2 seconds from your speech end to hearing bot voice)

2. **Key timing logs** for 2-3 bot responses:
```
t1: LLaMA send-to-Kokoro [call X] ts=...
⏱️  Kokoro pipeline first audio: ...ms
t2: Kokoro first subchunk sent [call X] ts=...
t3: First RTP frame sent [call X] ts=...
⏱️  LLaMA timing [X]: ... total=...ms
```

3. **Transcription quality**: Any missing or clipped words?

4. **Conversational behavior**: Did the bot ever talk over itself?

## Expected Performance

### Target Metrics (should be achieved):
- ✅ User speech → Whisper transcription: < 600ms
- ✅ LLaMA generation: < 600ms (with 50 token cap)
- ✅ t1→t2 (LLaMA→Kokoro): < 300ms (after warmup)
- ✅ t2→t3 (Kokoro→RTP): < 5ms
- ✅ **Total latency: < 1.5 seconds**

### If Performance Is Still Slow

If t1→t2 is still > 300ms after the first response:

1. **Check Kokoro warmup completed successfully**
   ```bash
   grep "Warmup" /tmp/wildfire.log
   ```

2. **Check system load**
   ```bash
   top -l 1 | grep "CPU usage"
   ```

3. **Check if MPS (Metal) is being used**
   ```bash
   grep "Using Metal" /tmp/wildfire.log
   ```

4. **Try restarting Kokoro service alone**
   ```bash
   pkill -f kokoro_service.py
   cd /Users/whisper/Documents/augment-projects/clean-repo/bin
   python3 kokoro_service.py &
   ```

## Troubleshooting

### Services Won't Start
```bash
# Check for port conflicts
lsof -i :8081  # HTTP server
lsof -i :8083  # LLaMA
lsof -i :8090  # Kokoro
lsof -i :5060  # SIP

# Kill conflicting processes
kill -9 <PID>
```

### No Audio on Phone
```bash
# Check SIP client is running
ps aux | grep sip-client

# Check RTP ports
lsof -i :10001
lsof -i :10002
```

### Transcriptions Are Garbled
- VAD may be cutting too aggressively
- Check hangover and overlap settings in logs:
  ```
  🔴 VAD: speech end detected (hangover=400 ms) - sending immediately (overlap=200ms)
  ```
- If still issues, we can increase hangover to 500ms

### Bot Talks Over Itself
- Check for half-duplex gate logs:
  ```
  🔒 [X] Half-duplex gate active for XXXms
  ```
- If missing, the gate isn't working
- Share logs for debugging

## Summary of Changes

### What Was Optimized:
1. **Kokoro TTS**: Enhanced warmup, smaller subchunks (40ms), better instrumentation
2. **Outbound Processor**: Removed conversion rate limiting, process all jobs immediately
3. **VAD**: Increased hangover (400ms), increased overlap (200ms), reduced max chunk (1.0s)
4. **LLaMA**: Reduced max tokens (50), improved prompt, strengthened half-duplex gate

### Expected Improvement:
- **Before**: 2-3 seconds total latency
- **After**: 1.0-1.5 seconds total latency
- **Improvement**: ~50% faster, should feel real-time

## Next Steps After Testing

Based on your test results, we can:

1. **If still too slow**: Profile Kokoro internals, consider alternative TTS
2. **If transcriptions poor**: Fine-tune VAD parameters
3. **If bot interrupts**: Adjust half-duplex gate timing
4. **If quality issues**: Revert token cap or adjust prompt

All optimizations are documented in `PERFORMANCE_OPTIMIZATIONS.md`.

