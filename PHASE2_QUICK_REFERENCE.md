# Phase 2 Implementation: Quick Reference

**Status:** PLANNING ONLY - NO CODE CHANGES YET  
**Full Plan:** See `PHASE2_IMPLEMENTATION_PLAN.md`

---

## 🎯 Goals

1. **Streaming Inference**: 30-50% latency reduction via overlapping windows
2. **Mutex Optimization**: 20-40% throughput improvement via reduced contention

---

## 📋 Key Changes Summary

### Streaming Inference

**What:** Process audio in 5-second overlapping windows (1-second overlap, 4-second stride)

**Where:** `whisper-service.cpp` lines 103-167 (process_audio_chunk)

**How:**
- Add `streaming_buffer_` to WhisperSession
- Extract 5s windows from buffer
- Process each window with whisper_full()
- Deduplicate overlapping transcriptions
- Slide window by 4 seconds

**Memory:** ~80KB per call

### Mutex Optimization

**What:** Reduce mutex contention during multi-call scenarios

**Where:** `whisper-service.cpp` lines 116-119 (mutex acquisition)

**How:**
- **Phase 2A (Week 1):** Add instrumentation to measure contention
- **Phase 2B (Week 2):** Implement optimization based on data
  - Option 1: Read-write lock (if whisper_full is read-only)
  - Option 2: Context pool (if high contention)

**Risk:** MEDIUM (requires careful verification)

### CoreML/Metal Optimizations

**What:** Enable Metal-specific optimizations for faster inference

**Where:** `whisper-service.cpp` line 208 (context initialization)

**How:**
```cpp
cparams.flash_attn = true;           // 20-30% speedup
cparams.gpu_device = 0;              // Explicit GPU selection
cparams.dtw_token_timestamps = false; // 5-10% speedup
```

**Risk:** MINIMAL (graceful fallback)

---

## 🔧 Files to Modify

### whisper-service.h

**Add to WhisperSession (after line 57):**
```cpp
// Streaming inference state
std::vector<float> streaming_buffer_;
std::string previous_transcription_;
size_t window_size_ = 5 * 16000;   // 5 seconds
size_t overlap_size_ = 1 * 16000;  // 1 second
size_t stride_ = 4 * 16000;        // 4 seconds
```

**Add to StandaloneWhisperService (after line 107):**
```cpp
// Performance metrics
struct PerformanceMetrics {
    std::atomic<uint64_t> total_mutex_wait_ms{0};
    std::atomic<uint64_t> total_inference_ms{0};
    std::atomic<uint64_t> total_chunks_processed{0};
};
PerformanceMetrics metrics_;
```

### whisper-service.cpp

**Modify process_audio_chunk() (lines 103-167):**
- Add buffer management logic
- Extract windows from buffer
- Process each window
- Deduplicate transcriptions

**Add new methods:**
- `process_window()` - Process single 5s window
- `deduplicate_transcription()` - Remove repeated text from overlap

**Add instrumentation (line 116):**
- Track mutex wait time
- Log if wait > 10ms
- Update performance metrics

**Enable CoreML optimizations (line 208):**
- Set flash_attn = true
- Set gpu_device = 0
- Set dtw_token_timestamps = false

---

## ✅ Verification Checklist

### Functional Requirements
- [ ] All existing functionality preserved
- [ ] Multi-call capability maintained
- [ ] Sessionless architecture intact
- [ ] No crashes or data races
- [ ] Transcription accuracy maintained (WER < 5% difference)

### Performance Requirements
- [ ] First-transcription latency reduced by 30-50%
- [ ] Multi-call throughput improved by 20-40%
- [ ] Memory overhead < 100KB per call
- [ ] No performance regression in single-call scenarios

### Interface Compatibility
- [ ] UDP registration unchanged (port 13000)
- [ ] TCP audio streaming unchanged (port 9001+call_id)
- [ ] TCP LLaMA forwarding unchanged (port 8083)
- [ ] Database schema unchanged

---

## 🧪 Testing Strategy

### Unit Tests
1. **Buffer Management:** Verify buffer append, window extraction, stride
2. **Deduplication:** Test prefix matching with various inputs
3. **Mutex Instrumentation:** Verify metrics are tracked correctly

### Integration Tests
1. **Single Call:** Verify streaming works end-to-end
2. **Multi-Call:** Test 2, 4, 8 concurrent calls
3. **Quality:** Compare transcriptions before/after

### Performance Tests
1. **Latency:** Measure time to first transcription
2. **Throughput:** Measure transcriptions/sec with concurrent calls
3. **Accuracy:** Calculate Word Error Rate (WER)

---

## ⚠️ Critical Risks

| Risk | Mitigation |
|------|------------|
| whisper_full() not thread-safe | Verify before implementing read-write lock |
| Streaming buffer overflow | Cap at 10 seconds, flush on overflow |
| Deduplication edge cases | Log failures, fall back to full transcription |
| Performance regression | Benchmark before/after, rollback plan |

---

## 📅 Timeline

### Week 1: Streaming Inference
- **Day 1-2:** Buffer infrastructure
- **Day 3-4:** Window processing & deduplication
- **Day 5:** Testing & refinement

### Week 2: Mutex Optimization
- **Day 1-2:** Instrumentation & profiling
- **Day 3-4:** Implement optimization
- **Day 5:** CoreML optimizations & final tests

**Total:** 1.5-2 weeks

---

## 🔄 Rollback Plan

If critical issues arise:

1. **Identify issue** (logs, metrics, accuracy)
2. **Disable optimizations** (compile-time flags)
3. **Rebuild & redeploy**
4. **Analyze & fix**

```cpp
// Add to whisper-service.cpp:
#define ENABLE_STREAMING_INFERENCE 0
#define ENABLE_MUTEX_OPTIMIZATION 0
```

---

## 📊 Expected Results

### Conservative Estimates
- **Latency:** 30-40% reduction
- **Throughput:** 20-30% improvement
- **Memory:** +80KB per call
- **Accuracy:** No degradation

### Optimistic Estimates
- **Latency:** 40-50% reduction
- **Throughput:** 30-40% improvement
- **Memory:** +80KB per call
- **Accuracy:** Potential improvement (better cross-boundary handling)

---

## 🚀 Next Steps

1. **Review** this plan and full implementation plan
2. **Approve** or request changes
3. **Create feature branch:** `feature/phase2-streaming-mutex-opt`
4. **Begin Week 1 implementation**
5. **Test and validate** after each week
6. **Merge to main** after final approval

---

## 📚 Related Documents

- **Full Implementation Plan:** `PHASE2_IMPLEMENTATION_PLAN.md` (detailed 13-section plan)
- **WhisperFlow Analysis:** `WHISPERFLOW_ANALYSIS.md` (research paper comparison)
- **Code Recommendations:** `WHISPERFLOW_CODE_RECOMMENDATIONS.md` (all phases)
- **Executive Summary:** `WHISPERFLOW_EXECUTIVE_SUMMARY.md` (high-level overview)

---

## ❓ Open Questions

1. **whisper_full() Thread-Safety:**
   - Is it safe to call concurrently on same context?
   - Need to verify before implementing read-write lock

2. **Optimal Window Size:**
   - Is 5 seconds optimal?
   - Should it be configurable?

3. **Context Pool Size:**
   - If needed, how many contexts? (2-3 = 2-6GB memory)

4. **Deduplication Algorithm:**
   - Is simple prefix matching sufficient?
   - Or use more sophisticated diff?

---

**Status:** Ready for review and approval  
**Contact:** Review with team before proceeding to implementation

