# WER Optimization Guide - Harvard Sentence Test Files

## Executive Summary
This guide provides a step-by-step approach to optimize the whisper-service for zero WER (Word Error Rate) on all 25 Harvard sentence test files while maintaining real-time inference speed.

## Current Baseline
- **Model**: ggml-large-v3-turbo-q5_0.bin with CoreML acceleration
- **Current WER**: 0.05 (4 edits / 80 words) on OSR_us_000_0010_8k.wav
- **Inference Speed**: ~500ms for 2-3s audio (excellent)
- **Architecture**: ✅ Production-ready (dual TCP connections verified)

## Known Errors to Fix
1. "It is easy" vs "It's easy" (contraction expansion)
2. "rice" vs "Rice" (capitalization)
3. "study work" vs "steady work" (homophone confusion)

## Optimization Attempts (Priority Order)

### Attempt 1: BEAM_SEARCH with beam_size=5
**File**: `whisper-service.cpp` lines 142-154

**Change**:
```cpp
// FROM:
whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);

// TO:
whisper_full_params wparams = whisper_full_default_params(WHISPER_SAMPLING_BEAM_SEARCH);
wparams.beam_search.beam_size = 5;
wparams.suppress_blank = true;
```

**Rationale**: Beam search explores multiple hypotheses, better for homophones and contractions.

**Expected Result**: WER should decrease (better accuracy on homophones).

### Attempt 2: BEAM_SEARCH with beam_size=8
**Change**:
```cpp
wparams.beam_search.beam_size = 8;  // Maximum allowed
```

**Rationale**: More decoders = more thorough search.

**Expected Result**: Further WER reduction, but slower inference.

### Attempt 3: Temperature=0.1f with BEAM_SEARCH
**Change**:
```cpp
wparams.temperature = 0.1f;  // Slight randomness
wparams.beam_search.beam_size = 5;
```

**Rationale**: Slight randomness may help with contraction handling.

**Expected Result**: Possible improvement on contraction errors.

### Attempt 4: Language Hints
**Change**:
```cpp
wparams.language = "en";  // Explicit English
wparams.suppress_blank = true;
wparams.no_context = false;  // Use context
```

**Rationale**: Explicit language hints reduce ambiguity.

**Expected Result**: Better handling of capitalization and contractions.

### Attempt 5: Prompt Engineering
**Change**:
```cpp
wparams.initial_prompt = "The following is a Harvard sentence test. Transcribe exactly as spoken.";
```

**Rationale**: Prompt guides model behavior.

**Expected Result**: Better accuracy on standard test sentences.

## Testing Workflow

### Step 1: Modify Parameters
Edit `whisper-service.cpp` lines 142-154 with desired parameters.

### Step 2: Rebuild
```bash
cd /Users/whisper/Documents/augment-projects/clean-repo/build
make whisper-service whisper_inbound_sim -j4
```

### Step 3: Restart Service
```bash
pkill -9 whisper-service
sleep 2
/Users/whisper/Documents/augment-projects/clean-repo/bin/whisper-service \
  --model /Users/whisper/Documents/augment-projects/clean-repo/models/ggml-large-v3-turbo-q5_0.bin \
  --database /Users/whisper/Documents/augment-projects/clean-repo/whisper_talk.db \
  --threads 8 \
  --llama-host 127.0.0.1 \
  --llama-port 8083 &
sleep 8
```

### Step 4: Run Test
```bash
/Users/whisper/Documents/augment-projects/clean-repo/bin/whisper_inbound_sim \
  /Users/whisper/Documents/augment-projects/clean-repo/tests/data/harvard/wav/OSR_us_000_0010_8k.wav \
  /Users/whisper/Documents/augment-projects/clean-repo/tests/data/harvard/wav/OSR_us_000_0011_8k.wav \
  /Users/whisper/Documents/augment-projects/clean-repo/tests/data/harvard/wav/OSR_us_000_0012_8k.wav
```

### Step 5: Analyze Results
- Check WER for each file
- Document errors
- Compare with baseline

### Step 6: Document
Update `HARVARD_TEST_ANALYSIS.md` with results.

## Success Criteria
- ✅ WER = 0.0 on all 25 Harvard files
- ✅ Inference speed ≤ 500ms for 2-3s audio
- ✅ No session-specific state
- ✅ Changes reproducible

## Incremental Testing Strategy
1. **Batch 1** (3 files): OSR_us_000_0010, 0011, 0012
2. **Batch 2** (3 files): OSR_us_000_0013, 0014, 0015
3. **Batch 3** (5 files): OSR_us_000_0016-0020
4. **Batch 4** (5 files): OSR_us_000_0021-0025
5. **Batch 5** (5 files): OSR_us_000_0026-0030
6. **Batch 6** (4 files): OSR_us_000_0031-0034

Only proceed to next batch after achieving WER=0 on current batch.

## Production Implementation
Once WER=0 is achieved:
1. Apply final parameters to `simple-audio-processor.cpp`
2. Apply final parameters to `inbound-audio-processor.cpp`
3. Test with full SIP pipeline
4. Create final summary report

## Notes
- Keep changes minimal and targeted
- Stop and investigate if WER increases
- Maintain real-time inference speed
- Preserve sessionless design

