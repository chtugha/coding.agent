# Performance Testing Checklist

## Quick Reference: What to Do and What to Look For

---

## ☑️ Pre-Test Setup

### 1. Restart Services
```bash
cd /Users/whisper/Documents/augment-projects/clean-repo
./start-wildfire.sh
```

### 2. Verify Services Running
```bash
ps aux | grep -E "(whisper-service|llama-service|kokoro|sip-client)" | grep -v grep
```
Expected: 4 processes

### 3. Check Kokoro Warmup
```bash
tail -100 /tmp/wildfire.log | grep "Warmup"
```
Expected: 3 warmup completions with times

---

## ☑️ Test Call

### Make Call
- From: Extension 16
- To: Llama1

### Test Script
1. **You**: "Hello, can you hear me?"
   - Wait for bot response
   - Note: First response may be slower (cold start)

2. **You**: "I need help with something."
   - Wait for bot response
   - Should be faster than first response

3. **You**: "Thank you, that's all."
   - Wait for bot response
   - Should be consistently fast

---

## ☑️ Performance Metrics to Collect

### For EACH Bot Response, Find These Lines:

#### Timing Logs (REQUIRED):
```
t1: LLaMA send-to-Kokoro [call X] ts=TIMESTAMP_1
⏱️  Kokoro pipeline first audio: XXXms
t2: Kokoro first subchunk sent [call X] ts=TIMESTAMP_2
t3: First RTP frame sent [call X] ts=TIMESTAMP_3
```

#### LLaMA Timing (REQUIRED):
```
⏱️  LLaMA timing [X]: tokenize=Xms, decode=Xms, generate=Xms (Y tokens), total=Xms
```

#### Half-Duplex Gate (REQUIRED):
```
🔒 [X] Half-duplex gate active for XXXms (text len=YY)
```

#### Transcription (REQUIRED):
```
📝 [X] Transcription: [your words]
🔴 VAD: speech end detected (hangover=400 ms) - sending immediately (overlap=200ms)
```

---

## ☑️ Calculate Latencies

### For Each Response:

#### t1→t2 Latency (Kokoro Synthesis)
```
TIMESTAMP_2 - TIMESTAMP_1 = ??? ms
```
**Target**: < 300ms (after first response)
**First response**: May be 500-700ms (acceptable)

#### t2→t3 Latency (Outbound Path)
```
TIMESTAMP_3 - TIMESTAMP_2 = ??? ms
```
**Target**: < 5ms
**Should be**: ~2-3ms

#### Total LLaMA Time
```
From "⏱️  LLaMA timing" log: total=??? ms
```
**Target**: < 600ms
**Should be**: 200-600ms depending on response length

---

## ☑️ Quality Checks

### Transcription Quality
- [ ] All your words were captured correctly
- [ ] No missing words at start or end of sentences
- [ ] No mid-word cuts
- [ ] Sentences make sense

**If issues**: Note which words were missing/clipped

---

### Conversational Behavior
- [ ] Bot never talked while it was still speaking
- [ ] Bot waited for you to finish before responding
- [ ] Responses were warm and empathetic (not robotic)
- [ ] Natural conversation flow

**If issues**: Note when bot interrupted itself

---

### Perceived Latency
Rate the perceived delay from when you stopped speaking to when you heard bot's voice:

- [ ] < 1 second (excellent, feels instant)
- [ ] 1-2 seconds (good, feels real-time)
- [ ] 2-3 seconds (acceptable, slight delay)
- [ ] > 3 seconds (too slow, needs work)

---

## ☑️ Results Summary Template

Copy this template and fill in your results:

```
=== PERFORMANCE TEST RESULTS ===

Date: [DATE]
Call ID: [X]

--- RESPONSE 1 (First response - may be slower) ---
Your utterance: "[what you said]"
Bot response: "[what bot said]"

Timing:
- t1→t2 latency: ??? ms
- t2→t3 latency: ??? ms
- LLaMA total: ??? ms
- Perceived delay: [< 1s / 1-2s / 2-3s / > 3s]

Quality:
- Transcription: [✓ perfect / ✗ issues: ...]
- Half-duplex: [✓ worked / ✗ bot talked over itself]
- Response tone: [✓ warm / ✗ robotic]

--- RESPONSE 2 (Should be faster) ---
Your utterance: "[what you said]"
Bot response: "[what bot said]"

Timing:
- t1→t2 latency: ??? ms
- t2→t3 latency: ??? ms
- LLaMA total: ??? ms
- Perceived delay: [< 1s / 1-2s / 2-3s / > 3s]

Quality:
- Transcription: [✓ perfect / ✗ issues: ...]
- Half-duplex: [✓ worked / ✗ bot talked over itself]
- Response tone: [✓ warm / ✗ robotic]

--- RESPONSE 3 (Should be consistently fast) ---
Your utterance: "[what you said]"
Bot response: "[what bot said]"

Timing:
- t1→t2 latency: ??? ms
- t2→t3 latency: ??? ms
- LLaMA total: ??? ms
- Perceived delay: [< 1s / 1-2s / 2-3s / > 3s]

Quality:
- Transcription: [✓ perfect / ✗ issues: ...]
- Half-duplex: [✓ worked / ✗ bot talked over itself]
- Response tone: [✓ warm / ✗ robotic]

--- OVERALL ASSESSMENT ---
Performance: [✓ Excellent / ✓ Good / ⚠️ Acceptable / ✗ Needs work]
Quality: [✓ Excellent / ✓ Good / ⚠️ Acceptable / ✗ Needs work]

Issues (if any):
- [List any problems]

Improvement from before: [Much better / Better / Same / Worse]
```

---

## ☑️ Quick Troubleshooting

### If Services Won't Start
```bash
# Check for port conflicts
lsof -i :8081 :8083 :8090 :5060

# Kill conflicting processes
kill -9 <PID>

# Try again
./start-wildfire.sh
```

---

### If No Audio on Phone
```bash
# Check SIP client
ps aux | grep sip-client

# Check RTP ports
lsof -i :10001 :10002

# Restart SIP client
pkill sip-client
cd bin && ./sip-client --database ../whisper_talk.db &
```

---

### If Transcriptions Are Garbled
- Check VAD logs for hangover/overlap values
- Should see: `hangover=400 ms` and `overlap=200ms`
- If still issues, report specific examples

---

### If Bot Talks Over Itself
- Check for half-duplex gate logs
- Should see: `🔒 [X] Half-duplex gate active for XXXms`
- If missing, report the issue

---

## ☑️ What to Share

### Minimum Required:
1. Filled-out results summary template (above)
2. Raw timing logs for 2-3 responses (t1/t2/t3 lines)
3. Overall impression: Better/Same/Worse than before?

### Helpful Additional Info:
1. Kokoro warmup times from service start
2. Any error messages or warnings
3. System load during test (`top -l 1 | grep CPU`)

---

## ☑️ Expected Results

### ✅ Success Indicators:
- t1→t2 latency < 300ms (after first response)
- t2→t3 latency < 5ms
- Total perceived delay < 2.5 seconds
- No word clipping
- Bot never talks over itself
- Warm, natural responses

### ⚠️ Acceptable (but could be better):
- t1→t2 latency 300-500ms
- Total perceived delay 2.5-3 seconds
- Occasional minor transcription issues
- Responses mostly natural

### ✗ Needs More Work:
- t1→t2 latency > 500ms consistently
- Total perceived delay > 3 seconds
- Frequent word clipping
- Bot talks over itself
- Robotic responses

---

## ☑️ Next Steps Based on Results

### If Results Are Good (✅):
- Document final metrics
- Mark optimization work as complete
- Enjoy real-time conversations!

### If Results Are Acceptable (⚠️):
- Identify specific pain points
- Implement targeted fixes
- Re-test

### If Results Need Work (✗):
- Share detailed logs
- Analyze bottlenecks
- Implement Priority 1-4 from PIPELINE_ANALYSIS.md

---

## Quick Commands Reference

### View Recent Logs
```bash
tail -200 /tmp/wildfire.log
```

### Search for Specific Call
```bash
grep "call 136" /tmp/wildfire.log
```

### Find All Timing Logs
```bash
grep -E "t1:|t2:|t3:|⏱️" /tmp/wildfire.log
```

### Check Service Status
```bash
ps aux | grep -E "(whisper|llama|kokoro|sip)" | grep -v grep
```

### Restart Everything
```bash
pkill -f "whisper-service|llama-service|kokoro|sip-client"
./start-wildfire.sh
```

---

## Remember

- **First response** will be slower (cold start) - this is expected
- **Subsequent responses** should be faster and consistent
- **Focus on perceived latency** - does it feel real-time?
- **Quality matters** - fast but garbled is not success

The goal is **real-time conversation** that feels natural, not just hitting arbitrary numbers.

---

## Questions to Ask Yourself

1. Could I have a natural conversation with this bot?
2. Does the delay feel acceptable for a phone call?
3. Are the transcriptions accurate enough?
4. Does the bot sound warm and helpful?
5. Is this better than before?

If you answer "yes" to most of these, the optimization was successful!

