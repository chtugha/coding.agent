# Performance Optimization Plan

## Current Performance Analysis

### From Recent Call Log (Call ID: 61)

**Whisper Performance:**
- Chunk 1: 14720 samples (~0.92s) - Processing time: ~100-200ms (estimated)
- Chunk 2: 14080 samples (~0.88s) - Processing time: ~100-200ms
- Chunk 3: 11520 samples (~0.72s) - Processing time: ~80-150ms
- Chunk 4: 19840 samples (~1.24s) - Processing time: ~150-250ms
- Chunk 5: 11520 samples (~0.72s) - Processing time: ~80-150ms
- Chunk 6: 11520 samples (~0.72s) - Processing time: ~80-150ms
- Chunk 7: 8000 samples (~0.50s) - Processing time: ~60-120ms

**Kokoro TTS Performance:**
- Response 1: RTF 0.594x (1.53s synthesis for 2.58s audio) ✅
- Response 2: RTF 0.210x (1.50s synthesis for 7.15s audio) ✅✅
- Response 3: RTF 0.170x (0.77s synthesis for 4.50s audio) ✅✅
- Response 4: RTF 0.142x (1.03s synthesis for 7.23s audio) ✅✅✅
- Response 5: RTF 0.182x (0.69s synthesis for 3.78s audio) ✅✅
- Response 6: RTF 0.268x (0.63s synthesis for 2.35s audio) ✅✅
- Response 7: RTF 0.953x (1.31s synthesis for 1.38s audio) ⚠️

**LLaMA Performance:**
- Response generation: ~500-1500ms per response (estimated from logs)
- Token generation speed: ~20-40 tokens/second (estimated)

## Optimization Priorities

### 1. Whisper Optimization (HIGH PRIORITY)
**Current:** Using 4 threads, Metal acceleration
**Target:** Reduce inference time by 30-50%

#### A. Optimize Thread Count
```cpp
// Current: threads=4
// Optimal for M4: 8-10 threads (M4 has 10 cores: 4 performance + 6 efficiency)
```

**Changes needed in `whisper-service.cpp`:**
- Increase thread count from 4 to 8-10
- Test with different thread counts to find optimal
- Consider using performance cores only

#### B. Enable Flash Attention
```cpp
// Current: flash_attn = 0
// Enable: flash_attn = 1 (can reduce memory and improve speed)
```

#### C. Optimize Batch Processing
- Current: Processing one chunk at a time
- Potential: Batch multiple small chunks if they arrive quickly

### 2. LLaMA Optimization (MEDIUM PRIORITY)
**Current:** Using shared warm context, Metal acceleration
**Target:** Reduce response generation time by 20-30%

#### A. Increase Batch Size
```cpp
// Current: Default batch size
// Optimal: Increase batch size for faster token generation
```

#### B. Optimize Sampling Parameters
```cpp
// Current: top_k, top_p, temperature sampling
// Consider: Greedy sampling for faster generation (if quality is acceptable)
```

#### C. Enable Flash Attention
```cpp
// Current: Using standard attention
// Enable: Flash attention for faster inference
```

### 3. Kokoro Optimization (LOW PRIORITY)
**Current:** Already excellent performance (0.14-0.59x RTF)
**Target:** Maintain current performance, optimize edge cases

#### A. Optimize Short Text Synthesis
- Response 7 had RTF 0.953x (slower for very short text)
- Consider caching common phrases
- Optimize phonemization for short texts

#### B. Batch Text Processing
- If multiple responses are queued, process in batch
- Reduces overhead from model initialization

### 4. Network Optimization (LOW PRIORITY)
**Current:** TCP connections between services
**Target:** Reduce latency by 10-20ms

#### A. Enable TCP_NODELAY
- Already enabled in some places
- Ensure all TCP connections use TCP_NODELAY

#### B. Increase Socket Buffer Sizes
- Reduce blocking on large audio chunks
- Optimize for 24kHz audio streaming

## Implementation Plan

### Phase 1: Whisper Optimization (Immediate)

**File:** `whisper-service.cpp`

**Changes:**
1. Increase thread count to 8-10
2. Enable flash attention
3. Add performance monitoring

**Expected Impact:** 30-50% faster Whisper inference

### Phase 2: LLaMA Optimization (Next)

**File:** `llama-service.cpp`

**Changes:**
1. Optimize batch size
2. Test greedy sampling vs temperature sampling
3. Enable flash attention if available

**Expected Impact:** 20-30% faster LLaMA response generation

### Phase 3: System-Wide Tuning (Final)

**Files:** Multiple

**Changes:**
1. Profile end-to-end latency
2. Optimize TCP buffer sizes
3. Add performance metrics logging

**Expected Impact:** 10-20% overall latency reduction

## Performance Metrics to Track

### Before Optimization
- **Whisper:** ~100-200ms per chunk (0.5-1.5s audio)
- **LLaMA:** ~500-1500ms per response
- **Kokoro:** 0.14-0.59x RTF (already excellent)
- **Total Latency:** ~1.5-3.0s from speech end to audio start

### After Optimization (Target)
- **Whisper:** ~50-100ms per chunk (50% faster)
- **LLaMA:** ~350-1000ms per response (30% faster)
- **Kokoro:** 0.10-0.50x RTF (maintain/improve)
- **Total Latency:** ~1.0-2.0s from speech end to audio start

## Detailed Optimization Steps

### Step 1: Optimize Whisper Thread Count

**Current Code (whisper-service.cpp):**
```cpp
wparams.n_threads = 4;
```

**Optimized Code:**
```cpp
// M4 has 10 cores (4 performance + 6 efficiency)
// Use 8 threads for optimal performance
wparams.n_threads = 8;
```

**Testing:**
- Test with 4, 6, 8, 10 threads
- Measure inference time for each
- Choose optimal based on results

### Step 2: Enable Flash Attention in Whisper

**Current Code:**
```cpp
whisper_init_with_params_no_state: flash attn = 0
```

**Optimized Code:**
```cpp
// Enable flash attention for faster inference
wparams.flash_attn = true;
```

**Note:** Check if whisper.cpp version supports flash attention

### Step 3: Optimize LLaMA Batch Size

**Current Code (llama-service.cpp):**
```cpp
*batch_ = llama_batch_init(config_.n_ctx, 0, 1);
```

**Optimized Code:**
```cpp
// Increase batch size for faster token generation
*batch_ = llama_batch_init(config_.n_ctx, 0, 512);
```

### Step 4: Add Performance Monitoring

**Add to all services:**
```cpp
auto start = std::chrono::high_resolution_clock::now();
// ... processing ...
auto end = std::chrono::high_resolution_clock::now();
auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
std::cout << "⏱️ Processing time: " << duration.count() << "ms" << std::endl;
```

## Testing Procedure

### 1. Baseline Measurement
- Make 10 test calls
- Record Whisper, LLaMA, Kokoro times
- Calculate average latency

### 2. Apply Optimizations
- Implement changes one at a time
- Recompile and test after each change
- Compare performance

### 3. Validate Results
- Ensure quality is maintained
- Check for any regressions
- Verify stability

## Expected Results

### Conservative Estimate
- **Whisper:** 30% faster → ~70-140ms per chunk
- **LLaMA:** 20% faster → ~400-1200ms per response
- **Total:** 25% faster overall → ~1.1-2.3s latency

### Optimistic Estimate
- **Whisper:** 50% faster → ~50-100ms per chunk
- **LLaMA:** 30% faster → ~350-1000ms per response
- **Total:** 40% faster overall → ~0.9-1.8s latency

## Risks and Mitigation

### Risk 1: Quality Degradation
- **Mitigation:** Test thoroughly, compare transcription/generation quality
- **Fallback:** Revert to previous settings if quality drops

### Risk 2: Increased CPU/GPU Usage
- **Mitigation:** Monitor system resources during testing
- **Fallback:** Reduce thread count if system becomes unstable

### Risk 3: Compatibility Issues
- **Mitigation:** Check library versions support new features
- **Fallback:** Use alternative optimizations if features unavailable

## Next Steps

1. **Implement Whisper thread optimization** (5 minutes)
2. **Test and measure results** (10 minutes)
3. **Implement LLaMA optimizations** (10 minutes)
4. **Test and measure results** (10 minutes)
5. **Fine-tune based on results** (15 minutes)

**Total Time:** ~50 minutes for complete optimization

## Success Criteria

✅ Whisper inference time reduced by at least 30%
✅ LLaMA response time reduced by at least 20%
✅ No degradation in transcription/generation quality
✅ System remains stable under load
✅ Overall latency reduced by at least 25%

