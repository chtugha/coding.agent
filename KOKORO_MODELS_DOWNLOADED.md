# Kokoro Models Downloaded

## ✅ Download Complete

All Kokoro TTS models and voices have been successfully downloaded to the `models/` directory.

## 📦 Downloaded Files

### Main Model (312 MB)
```
models/kokoro-model/
├── kokoro-v1_0.pth (312.05 MB) - Main Kokoro-82M model
└── config.json (2.3 KB) - Model configuration
```

### Voice Models (9 voices, ~511 KB each)
```
models/kokoro-voices/voices/
├── af_bella.pt (511 KB) - Bella (American Female)
├── af_sarah.pt (511 KB) - Sarah (American Female)
├── af_sky.pt (511 KB) - Sky (American Female)
├── am_adam.pt (511 KB) - Adam (American Male)
├── am_michael.pt (511 KB) - Michael (American Male)
├── bf_emma.pt (511 KB) - Emma (British Female)
├── bf_isabella.pt (511 KB) - Isabella (British Female)
├── bm_george.pt (511 KB) - George (British Male)
└── bm_lewis.pt (511 KB) - Lewis (British Male)
```

**Total Size:** ~316.6 MB (312 MB model + 4.6 MB voices)

## 🎤 Available Voices

| Voice File | Voice ID | Name | Language | Gender |
|------------|----------|------|----------|--------|
| af_sky.pt | af_sky | Sky | American English | Female |
| af_bella.pt | af_bella | Bella | American English | Female |
| af_sarah.pt | af_sarah | Sarah | American English | Female |
| am_adam.pt | am_adam | Adam | American English | Male |
| am_michael.pt | am_michael | Michael | American English | Male |
| bf_emma.pt | bf_emma | Emma | British English | Female |
| bf_isabella.pt | bf_isabella | Isabella | British English | Female |
| bm_george.pt | bm_george | George | British English | Male |
| bm_lewis.pt | bm_lewis | Lewis | British English | Male |

## 🔧 Model Cache Location

The Kokoro service automatically downloads models to the Hugging Face cache on first use:
```
~/.cache/huggingface/hub/models--hexgrad--Kokoro-82M/
```

However, having local copies in the `models/` directory provides:
- **Backup** - Models available even if HF is down
- **Version control** - Specific model versions preserved
- **Offline use** - No internet required after download
- **Faster loading** - Can configure service to use local models

## 📝 Download Scripts

Two Python scripts were created for downloading models:

### 1. download-kokoro-voices.py
Downloads all 9 voice models from Hugging Face.

**Usage:**
```bash
./venv-kokoro/bin/python3 download-kokoro-voices.py
```

**Features:**
- Downloads all voices to `models/kokoro-voices/voices/`
- Shows progress bars for each download
- Displays summary with file sizes
- Handles errors gracefully

### 2. download-kokoro-model.py
Downloads the main Kokoro-82M model and configuration.

**Usage:**
```bash
./venv-kokoro/bin/python3 download-kokoro-model.py
```

**Features:**
- Downloads main model (312 MB) to `models/kokoro-model/`
- Downloads config.json
- Shows file sizes and locations
- Progress tracking

## 🚀 Using Local Models

### Option 1: Use Default Cache (Current Setup)
The Kokoro service currently uses the Hugging Face cache automatically. No configuration needed.

### Option 2: Use Local Models (Future Enhancement)
To use the downloaded models from `models/` directory, you would need to:

1. **Modify kokoro_service.py** to specify local paths:
```python
# Instead of:
self.pipeline = KPipeline(lang_code='en-us', device=self.device_str)

# Use:
self.pipeline = KPipeline(
    lang_code='en-us', 
    device=self.device_str,
    repo_id='models/kokoro-model'  # Local path
)
```

2. **Set voice paths** to local directory:
```python
# Configure voice directory
os.environ['KOKORO_VOICE_DIR'] = 'models/kokoro-voices/voices'
```

**Note:** The current implementation works perfectly with the HF cache. Local models are available as a backup or for offline use.

## 📊 Storage Summary

```
models/
├── kokoro-model/           (312 MB)
│   ├── kokoro-v1_0.pth    (312 MB)
│   └── config.json        (2.3 KB)
└── kokoro-voices/          (4.6 MB)
    └── voices/
        ├── af_bella.pt    (511 KB)
        ├── af_sarah.pt    (511 KB)
        ├── af_sky.pt      (511 KB)
        ├── am_adam.pt     (511 KB)
        ├── am_michael.pt  (511 KB)
        ├── bf_emma.pt     (511 KB)
        ├── bf_isabella.pt (511 KB)
        ├── bm_george.pt   (511 KB)
        └── bm_lewis.pt    (511 KB)

Total: ~316.6 MB
```

## 🔄 Re-downloading Models

If you need to re-download models (e.g., after updates):

```bash
# Re-download all voices
./venv-kokoro/bin/python3 download-kokoro-voices.py

# Re-download main model
./venv-kokoro/bin/python3 download-kokoro-model.py
```

The scripts will overwrite existing files with the latest versions from Hugging Face.

## 🧹 Cleaning Up

To remove downloaded models and free up space:

```bash
# Remove voice models (4.6 MB)
rm -rf models/kokoro-voices/

# Remove main model (312 MB)
rm -rf models/kokoro-model/

# Remove HF cache (if needed)
rm -rf ~/.cache/huggingface/hub/models--hexgrad--Kokoro-82M/
```

**Warning:** Only remove the HF cache if you're sure you won't need the models, as they will need to be re-downloaded.

## ✅ Verification

To verify all models are downloaded correctly:

```bash
# Check voice models
ls -lh models/kokoro-voices/voices/

# Check main model
ls -lh models/kokoro-model/

# Count voice files (should be 9)
ls models/kokoro-voices/voices/*.pt | wc -l
```

Expected output:
- 9 voice files (~511 KB each)
- 1 main model file (312 MB)
- 1 config file (2.3 KB)

## 🎉 Status

✅ **All models downloaded successfully!**
- Main model: kokoro-v1_0.pth (312 MB)
- Configuration: config.json (2.3 KB)
- Voice models: 9 voices (4.6 MB total)
- Total size: ~316.6 MB

The Kokoro TTS service is ready to use with all available voices!

## 📚 Model Information

**Model:** Kokoro-82M
**Repository:** hexgrad/Kokoro-82M (Hugging Face)
**License:** Apache 2.0
**Parameters:** 82 million
**Architecture:** StyleTTS2-based
**Sample Rate:** 24 kHz
**Format:** PyTorch (.pth, .pt)

## 🔗 References

- **Kokoro GitHub:** https://github.com/hexgrad/kokoro
- **Hugging Face Model:** https://huggingface.co/hexgrad/Kokoro-82M
- **Documentation:** See KOKORO_INTEGRATION.md and KOKORO_COMPLETE.md

