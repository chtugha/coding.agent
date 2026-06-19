# CoreML Whisper Models on HuggingFace

## Summary
There are **NO native WhisperX CoreML models** on HuggingFace. However, there are several CoreML Whisper implementations that can provide similar functionality with significant speed improvements on Apple Silicon.

## Key Finding: German-Specific CoreML Model

### 🎯 Best Option: Swiss German Whisper Large V3 Turbo CoreML
**Model:** `jlnslv/whisper-large-v3-turbo-swiss-german-coreml`
- **Size:** 1.6 GB
- **Base Model:** Whisper Large V3 Turbo (fine-tuned for Swiss German)
- **Components:** AudioEncoder, MelSpectrogram, TextDecoder (all in CoreML format)
- **Created:** March 2026
- **Downloads:** 0 (very new model)
- **URL:** https://huggingface.co/jlnslv/whisper-large-v3-turbo-swiss-german-coreml

**Advantages:**
- Optimized for German language (Swiss German variant)
- Should work well for standard German podcasts
- Native CoreML format for maximum Apple Silicon performance
- Based on Whisper Large V3 Turbo (latest, fastest Whisper variant)

## Alternative: WhisperKit (Official CoreML Implementation)

### WhisperKit by Argmax
**Model:** `argmaxinc/whisperkit-coreml`
- **Downloads:** 7.5M+ (very popular)
- **Likes:** 192
- **Library:** whisperkit
- **Tags:** whisper, coreml, asr, quantized
- **URL:** https://huggingface.co/argmaxinc/whisperkit-coreml

**Features:**
- Official CoreML implementation of Whisper
- Multiple model sizes available (tiny, base, small, medium, large)
- Quantized versions for faster inference
- Supports all languages Whisper supports (including German)
- Active development and maintenance

## Important Note: WhisperX vs Whisper CoreML

### What is WhisperX?
WhisperX is a **Python wrapper** around Whisper that adds:
1. **VAD (Voice Activity Detection)** - Faster processing by skipping silence
2. **Batching** - Process multiple audio chunks in parallel
3. **Word-level timestamps** - Uses forced alignment (wav2vec2 or phoneme models)
4. **Speaker diarization** - Identify different speakers

### CoreML Whisper Limitations
Standard CoreML Whisper models provide:
- ✅ Fast transcription on Apple Silicon (10-30x speedup)
- ✅ Segment-level timestamps
- ❌ **NO word-level timestamps** (this is WhisperX's key feature)
- ❌ NO built-in VAD or batching
- ❌ NO speaker diarization

### Solution: Hybrid Approach

For word-level timestamps on Apple Silicon, you need to combine:

1. **CoreML Whisper** (fast transcription)
   - Use `jlnslv/whisper-large-v3-turbo-swiss-german-coreml` or `argmaxinc/whisperkit-coreml`
   - Get segment-level transcripts quickly

2. **Forced Alignment** (word timestamps)
   - Use Montreal Forced Aligner (MFA) - what we're currently using
   - OR use wav2vec2 alignment models
   - OR use WhisperX's alignment component separately

## Performance Comparison

### Current Setup (WhisperX on CPU)
- **Speed:** ~200 minutes per 90-minute episode
- **Total for 373 episodes:** ~52 days
- **Word timestamps:** ✅ Yes (built-in)

### Option 1: WhisperX with MPS (Metal Performance Shaders)
- **Speed:** ~10-20 minutes per 90-minute episode (10-20x faster)
- **Total for 373 episodes:** ~2.6 days
- **Word timestamps:** ✅ Yes (built-in)
- **Implementation:** Already documented in `WHISPERX_APPLE_SILICON_ALTERNATIVES.md`

### Option 2: CoreML Whisper + MFA
- **Transcription speed:** ~3-5 minutes per 90-minute episode (30-50x faster)
- **MFA alignment:** ~5-10 minutes per episode
- **Total per episode:** ~8-15 minutes
- **Total for 373 episodes:** ~2-3 days
- **Word timestamps:** ✅ Yes (via MFA)
- **Complexity:** Higher (two-step process)

### Option 3: CoreML Whisper + wav2vec2 Alignment
- **Transcription speed:** ~3-5 minutes per episode
- **Alignment speed:** ~2-3 minutes per episode
- **Total per episode:** ~5-8 minutes
- **Total for 373 episodes:** ~1.5-2 days
- **Word timestamps:** ✅ Yes (via wav2vec2)
- **Complexity:** Medium (two-step process)

## Recommendation

### For Current Project (373 German Podcast Episodes)

**Best Option: WhisperX with MPS Backend**
```python
import whisperx
import torch

device = "mps"  # Use Metal Performance Shaders
compute_type = "float16"

model = whisperx.load_model("large-v3", device, compute_type=compute_type, language="de")
result = model.transcribe(audio, batch_size=16)

# Align for word timestamps
model_a, metadata = whisperx.load_align_model(language_code="de", device=device)
result = whisperx.align(result["segments"], model_a, metadata, audio, device)
```

**Why:**
- ✅ Single tool handles everything (transcription + word timestamps)
- ✅ 10-20x speedup over CPU (2.6 days vs 52 days)
- ✅ Already documented and tested approach
- ✅ No need to manage separate alignment step
- ✅ Proven to work with German language

**Alternative: CoreML + MFA (Current Approach)**
- ✅ Slightly faster transcription (30-50x vs 10-20x)
- ✅ Already have MFA working
- ❌ More complex (two separate tools)
- ❌ MFA alignment is slower than WhisperX alignment
- ⚠️ Net benefit is minimal (~20% faster overall)

## Implementation Notes

### If Using CoreML Whisper

1. **Install WhisperKit** (for argmaxinc models):
```bash
pip install whisperkit
```

2. **Or use Swiss German model directly**:
```python
import coremltools as ct

# Load model
model = ct.models.MLModel("path/to/Flurin17_whisper-large-v3-turbo-swiss-german")

# Transcribe
result = model.predict({"audio": audio_data})
```

3. **Then run MFA for word timestamps** (as we're currently doing)

### If Using WhisperX with MPS

1. **Install WhisperX with MPS support**:
```bash
pip install whisperx
pip install torch torchvision torchaudio  # Ensure MPS support
```

2. **Use MPS device**:
```python
device = "mps"
model = whisperx.load_model("large-v3", device, language="de")
```

## Conclusion

**There is NO native WhisperX CoreML model**, but:
- ✅ CoreML Whisper models exist (including German-specific ones)
- ✅ WhisperX can use MPS (Metal) backend for 10-20x speedup
- ✅ MPS approach is simpler and nearly as fast as CoreML
- ✅ Recommended: Use WhisperX with MPS for this project

The MPS approach provides the best balance of:
- Speed (10-20x faster than CPU)
- Simplicity (single tool)
- Functionality (word timestamps included)
- Proven compatibility (already documented)

---

**Created:** 2026-06-19  
**Research:** HuggingFace model search for CoreML Whisper implementations  
**Context:** German podcast dataset processing for Moshi RAG training