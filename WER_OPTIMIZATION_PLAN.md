# WER Optimization Plan - Harvard Sentence Test Files

## Current Status
- **Architecture**: ✅ Fixed - Simulator now uses production TCP connections
- **Current WER**: 0.05 (4 edits / 80 words) on OSR_us_000_0010_8k.wav
- **Inference Speed**: ~500ms for 2-3s audio (excellent, real-time)
- **Model**: ggml-large-v3-turbo-q5_0.bin with CoreML acceleration

## Known Errors (OSR_us_000_0010_8k.wav)
1. "It is easy" vs "It's easy" (contraction expansion)
2. "rice" vs "Rice" (capitalization)
3. "study work" vs "steady work" (homophone confusion)

## Optimization Strategy

### Phase 1: Incremental Testing (3 files at a time)
**Batch 1**: OSR_us_000_0010, 0011, 0012
**Batch 2**: OSR_us_000_0013, 0014, 0015
**Batch 3**: OSR_us_000_0016, 0017, 0018
**Batch 4**: OSR_us_000_0019, 0020, 0021
**Batch 5**: OSR_us_000_0022, 0023, 0024
**Batch 6**: OSR_us_000_0025, 0026, 0027
**Batch 7**: OSR_us_000_0028, 0029, 0030
**Batch 8**: OSR_us_000_0031, 0032, 0033
**Batch 9**: OSR_us_000_0034 (final file)

### Phase 2: Parameter Optimization (Priority Order)

#### 2.1 Whisper-Service Inference Parameters
**File**: `whisper-service.cpp` lines 142-154

**Current Settings**:
```cpp
whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
wparams.temperature = 0.0f;
wparams.no_timestamps = true;
wparams.translate = false;
wparams.print_progress = false;
wparams.print_realtime = false;
```

**Optimization Attempts** (in order):
1. **Try BEAM_SEARCH with beam_size=5**:
   - Change: `WHISPER_SAMPLING_GREEDY` → `WHISPER_SAMPLING_BEAM_SEARCH`
   - Add: `wparams.beam_search.beam_size = 5;`
   - Rationale: Beam search explores multiple hypotheses, better for homophones

2. **Try BEAM_SEARCH with beam_size=8**:
   - Change: `beam_size = 8` (max allowed)
   - Rationale: More decoders = better accuracy

3. **Try temperature=0.1f with BEAM_SEARCH**:
   - Change: `temperature = 0.1f`
   - Rationale: Slight randomness may help with contractions

4. **Try language hints**:
   - Add: `wparams.language = "en";` (explicit English)
   - Rationale: Reduce ambiguity

5. **Try suppress_blank=true**:
   - Add: `wparams.suppress_blank = true;`
   - Rationale: Prevent blank tokens

#### 2.2 VAD Parameters (if needed)
**File**: `tests/whisper_inbound_sim.cpp` VadConfig struct

**Current Settings**:
```cpp
struct VadConfig {
    float threshold = 0.02f;
    int hangover_ms = 900;
    int pre_roll_ms = 350;
    int overlap_ms = 250;
};
```

**Optimization Attempts** (only if inference params don't work):
1. Reduce threshold to 0.01f (more sensitive)
2. Increase hangover_ms to 1200 (longer silence tolerance)
3. Adjust pre_roll_ms to 500 (more context)

### Phase 3: Testing Workflow

**For Each Optimization Attempt**:
```bash
# 1. Modify parameters in whisper-service.cpp or whisper_inbound_sim.cpp
# 2. Rebuild
cd /Users/whisper/Documents/augment-projects/clean-repo/build
make whisper-service whisper_inbound_sim -j4

# 3. Restart whisper-service
pkill -9 whisper-service
sleep 2
/Users/whisper/Documents/augment-projects/clean-repo/bin/whisper-service \
  --model /Users/whisper/Documents/augment-projects/clean-repo/models/ggml-large-v3-turbo-q5_0.bin \
  --database /Users/whisper/Documents/augment-projects/clean-repo/whisper_talk.db \
  --threads 8 \
  --llama-host 127.0.0.1 \
  --llama-port 8083 &
sleep 5

# 4. Run test on first 3 files
/Users/whisper/Documents/augment-projects/clean-repo/bin/whisper_inbound_sim \
  /Users/whisper/Documents/augment-projects/clean-repo/tests/data/harvard/wav/OSR_us_000_0010_8k.wav \
  /Users/whisper/Documents/augment-projects/clean-repo/tests/data/harvard/wav/OSR_us_000_0011_8k.wav \
  /Users/whisper/Documents/augment-projects/clean-repo/tests/data/harvard/wav/OSR_us_000_0012_8k.wav

# 5. Analyze results and document in HARVARD_TEST_ANALYSIS.md
```

### Phase 4: Success Criteria
- ✅ WER = 0.0 on all 25 Harvard files
- ✅ Inference speed ≤ 500ms for 2-3s audio
- ✅ No session-specific state (sessionless design)
- ✅ Changes documented and reproducible

### Phase 5: Production Implementation
- Apply final VAD parameters to `simple-audio-processor.cpp`
- Apply final inference parameters to `whisper-service.cpp`
- Test with full SIP pipeline
- Create final summary report

## Expected Outcomes
- **Best Case**: BEAM_SEARCH with beam_size=5 achieves WER=0 on all files
- **Likely Case**: Need to try 2-3 parameter combinations
- **Worst Case**: May need VAD adjustments or prompt engineering

## Documentation
- Update `HARVARD_TEST_ANALYSIS.md` with each test result
- Track WER, inference time, and errors for each file
- Document final parameters and rationale

