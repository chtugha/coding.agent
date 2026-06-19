# Waveform Alignment Algorithm Analysis

## Test Results Summary

### V3 Algorithm (Simple Offset)
- **Approach**: Find initial offset, keep entire audio from there
- **Episode 150 Result**: Perfect! 0.0s difference
- **Pros**: Simple, works for episodes without ads
- **Cons**: Cannot handle mid-episode ads

### V4 Algorithm (Continuous Tracking)
- **Approach**: Track correlation continuously, detect ads when it drops
- **Episode 150 Result**: FAILED - kept only 30s out of 5367s
- **Problem**: Word-level pattern (evenly distributed) doesn't match actual speech rhythm
- **Correlation**: Dropped from 0.364 to -0.050 after 30s

## Root Cause Analysis

The fundamental issue with V4:
1. We distribute words evenly within segments (constant speed assumption)
2. Real speech has variable rhythm - pauses, emphasis, speed changes
3. This causes pattern drift even without ads
4. Correlation drops naturally over time, triggering false ad detection

## Solution: Hybrid Approach (V5)

### Strategy
1. **Primary**: Use V3's simple offset approach (works for 90% of episodes)
2. **Ad Detection**: Only when there's a significant gap in transcript
3. **Validation**: Check if cleaned duration matches transcript ±30s

### Algorithm
```
1. Find initial offset using cross-correlation
2. Check transcript for large gaps (>60s between segments)
3. If no large gaps:
   - Use simple approach: keep audio from offset to (offset + duration)
4. If large gaps exist:
   - Track through each gap
   - Verify correlation before/after gap
   - Cut gap if correlation drops significantly
5. Validate final duration matches transcript
```

### Key Insight
**Most podcast episodes don't have ads in the middle!**
- Ads are usually at start/end (handled by initial offset)
- Mid-episode ads are rare
- When they exist, they create large gaps in transcript timestamps
- We can detect these gaps and handle them specifically

## Next Steps
1. Implement V5 hybrid algorithm
2. Test on episode 150 (no ads) - expect perfect match like V3
3. Test on episode with mid-ads - expect ad removal
4. Validate with 10 episodes