# Technical Specification: Fine-Tuning German Moshi-RAG LoRA (7B)

## 1. Technical Context

This technical specification details the design and adaptation of the 7B duplex conversational RAG model (`kyutai/moshika-rag-candle-bf16`) to German speech while retaining its teacher-forcing text prefill capabilities.

### Target Environment & Platform
- **Base Model**: `kyutai/moshika-rag-candle-bf16` (7B parameters, duplex LM)
- **Local Runtime**: macOS 15+ (Metal acceleration on Apple Silicon / M4 Mac Mini) via `bin/moshi-rag-service`
- **Training Host**: Modal cloud runtime using high-end NVIDIA GPU instances (H200 or A100 80GB)
- **Modal Volume Storage**:
  - `moshi-german-data` (mounted as `/data` in training containers)
  - `moshi-german-checkpoints` (mounted as `/checkpoints` in training containers)

### Key Dependencies
- `moshi>=0.2.11` — Core duplex language modeling and serving
- `torch>=2.3` and `torchaudio` — Deep learning operations
- `sentencepiece` — Subword tokenization and protobuf model manipulation
- `safetensors` — Efficient, memory-mapped tensor serialization
- `numpy` — Array manipulation for custom merging and quantization pipelines
- `gguf` — Q8 model quantization and GGUF metadata serialization
- `huggingface-hub` — Model distribution and repository management

---

## 2. Implementation Approach

The overall architecture involves five primary phases: vocabulary extension, dataset configuration, LoRA training on Modal, weight merging/conversion, and local execution verification.

### 2.1 Extended Tokenizer Alignment
To prevent orthographic fragmentation of German text (such as treating lowercase and uppercase umlauts `ä`, `ö`, `ü`, `ß`, `Ä`, `Ö`, `Ü` as multi-byte unknown tokens), the base 32,000-piece SentencePiece tokenizer (`tokenizer_spm_32k_3.model`) must be extended to 32,007 pieces.

- **Process**:
  - A standalone script will load `tokenizer_spm_32k_3.model` using `sentencepiece.sentencepiece_model_pb2.ModelProto`.
  - It will check for the existence of lowercase `ä`, `ö`, `ü`, `ß`, and uppercase counterparts `Ä`, `Ö`, `Ü`.
  - If missing, it will append them as `NORMAL` pieces with default score representations.
  - The extended tokenizer will be saved on the Modal data volume as `/data/tokenizer_spm_32k_de.model`.

### 2.2 Model Resizing & Weight Initialization
The 7B duplex model contains:
- `text_emb`: Input embedding matrix of shape `[vocab_size + 1, embedding_dim]`
- `text_linear`: Output logits projection linear head of shape `[vocab_size, hidden_dim]`

Since the vocabulary size is expanded from 32,000 to 32,007:
- The input embedding and projection layers must be resized dynamically during initialization on the training rank.
- Sibling weights for the base vocabulary (indexes `0` to `31,999`) are copied exactly.
- Newly added German tokens are initialized using a normal distribution (`mean=0, std=0.02`).
- The special padding/initial token weight (normally positioned at the very end of the old embedding dimension) is shifted to the new end boundary.
- During LoRA training, both the resized `text_emb` and `text_linear` layers are unfrozen (`ft_embed: true`) to learn embedding space alignments, while the rest of the base model remains frozen.

### 2.3 Dataset Alignment and Redesigned Format Mapping
The model trains on double-mono stereo-duplex speech. The processing script `scripts/prepare_german_dataset.py` processes all **8 datasets** (BeMaTac, GCSC, CallFriend, CallHome, Gemischtes Hack Podcast, Medical, Nyrahealth Disfluency, Mozilla German Spontaneous) from `/Volumes/eHDD/moshi-rag-data/datasets/` into a single processed folder at `/Volumes/eHDD/moshi-rag-data/processed/`.

**Processing pipeline per dialogue:**
1. All stereo input audio is first converted to **mono** (channel average).
2. Mono audio is resampled to **48 kHz** and duplicated across both channels to produce **48 kHz / 16-Bit / Stereo PCM double-mono**.
3. The double-mono audio is split into chunks at **speaker turn boundaries**. Split points are calculated at the **midpoint** between the end of one speaker's last word and the start of the next speaker's first word, ensuring no mid-word or mid-sentence cuts.
4. **No padding** and **no fading** in or out is added.

For each chunk, one channel is muted completely (zeroed) while the other remains active:
- **`_main` files** (Right channel muted): Left channel (Channel 0) contains the active double-mono audio, Right channel (Channel 1) is completely zeroed. The matching `.json` transcript contains ONLY the timestamped words of `SPEAKER_MAIN`.
- **`_other` files** (Left channel muted): Left channel (Channel 0) is completely zeroed, Right channel (Channel 1) contains the active double-mono audio. The matching `.json` transcript contains ONLY the timestamped words of `SPEAKER_OTHER`.

**Single-speaker datasets** (Nyrahealth, Mozilla) produce only `_main` files with no splitting.

**RAG Prefill Injections (FACTS)**: Astronomical and quantum prefill facts are injected probabilistically (~1%) but **only** on `SPEAKER_MAIN` chunks containing more than one minute of speech.

**CRITICAL: ALL files from every dataset MUST be processed.** No file count limits, no sample limits, no duration caps. Data quality and accuracy is the absolute priority.

No manifest `.jsonl` files are produced in this preparation step — manifest generation is delegated to a subsequent script.

### 2.4 LoRA Training Setup (Modal)
We will define a custom training configuration file `/Users/whisper/zenflow_projects/moshi-finetune/configs/moshi_7b_de.yaml`:
```yaml
moshi_paths:
  hf_repo_id: "kyutai/moshika-rag-candle-bf16"
  tokenizer_path: "/data/tokenizer_spm_32k_de.model"

lora:
  enable: true
  rank: 64
  scaling: 2.0
  ft_embed: true

duration_sec: 100
batch_size: 16
max_steps: 3000

first_codebook_weight_multiplier: 100.0
text_padding_weight: 0.1

optim:
  lr: 2e-6
  weight_decay: 0.1
  pct_start: 0.05

data:
  train_data: "/data/train.jsonl"
  eval_data: "/data/valid.jsonl"
  shuffle: true

run_dir: "/checkpoints/moshi_7b_de"
overwrite_run_dir: true

seed: 42
log_freq: 10
eval_freq: 250
ckpt_freq: 500
save_adapters: true
```

The orchestration script `/Users/whisper/zenflow_projects/moshi-finetune/modal_train_rag.py` executes `torchrun` distributed training across multiple A100 or H200 GPUs.

### 2.5 Merging, Quantization, and Export
- **Merged weights**: Slower local deployment on a 16GB M4 Mac Mini prohibits using raw bfloat16 models (~15GB). The LoRA adapter will be merged with the base `kyutai/moshika-rag-candle-bf16` weights using `./scripts/merge_moshi-rag_lora.py` with the explicit `--text-tokenizer-file /data/tokenizer_spm_32k_de.model` parameter to resize base weights before merging.
- **Int8 GGUF Quantization**: The merged bfloat16 safetensors will be quantized to `Q8_0` format using `./moshi-rag/scripts/safetensors_to_gguf_q8.py` to ensure efficient Metal hardware accelerated inference and low latency step targets (<110ms).

---

## 3. Source Code Changes

This fine-tuning phase utilizes the existing codebase architecture and introduces targeted scripts and config files.

```
/Users/whisper/zenflow_projects/
├── coding.agent/                       # Main repository (workspace)
│   ├── scripts/
│   │   ├── merge_moshi-rag_lora.py    # [Renamed] LoRA merge tool for RAG models
│   │   └── extend_tokenizer.py        # [New] Standalone SPM extension script
│   └── moshi-rag/
│       └── scripts/
│           └── safetensors_to_gguf_q8.py # [Existing] GGUF Q8 exporter
└── moshi-finetune/                     # Sibling fine-tuning repository
    ├── configs/
    │   └── moshi_7b_de.yaml           # [New] Training parameters config
    └── modal_train_rag.py             # [New] Modal execution runner
```

### 3.1 New Components
- **`extend_tokenizer.py`**:
  - Located in `./scripts/extend_tokenizer.py`
  - Leverages `/Users/whisper/tokenizer/.venv/bin/python` environment
  - Modifies the SPM protobuf structure to append `ä`, `ö`, `ü`, `ß` and uppercase `Ä`, `Ö`, `Ü`
- **`moshi_7b_de.yaml`**:
  - Located in `/Users/whisper/zenflow_projects/moshi-finetune/configs/moshi_7b_de.yaml`
  - Configures LoRA Rank 64, Scaling 2.0, and unfreezes the text head/embedding
- **`modal_train_rag.py`**:
  - Located in `/Users/whisper/zenflow_projects/moshi-finetune/modal_train_rag.py`
  - Schedules training runs on Modal using H200 GPUs and mounts the `moshi-german-data` volumes

---

## 4. Interfaces & Data Models

### 4.1 Tokenizer Interface
The SentencePiece extension adds seven tokens:
- Token 32,000: `ä`
- Token 32,001: `ö`
- Token 32,002: `ü`
- Token 32,003: `ß`
- Token 32,004: `Ä`
- Token 32,005: `Ö`
- Token 32,006: `Ü`

These map directly to the newly allocated index positions in `text_emb.weight` and `text_linear.weight`.

### 4.2 Configuration Parameters
Moshi-RAG's context prefilling uses a special `rag_token_id`. This ID must remain unchanged and unmasked in the configuration file `config.json` to ensure teacher-forcing text prefill inputs are processed natively.

### 4.3 Standalone Rust Service Configuration
To verify and run the merged German Moshi-RAG model local deployment under Apple Silicon/Metal acceleration, the standalone Rust service must be configured.
Update `./moshi-rag/rust/moshi-backend/config-rag.json` and its variants (e.g. `./moshi-rag/rust/moshi-backend/config-q8.json`):
- **`lm_model_file`**: Set to the local path of the merged GGUF model: `./bin/models/moshiko-de-merged-q8.gguf`
- **`text_tokenizer_file`**: Set to the local path of the extended tokenizer model: `./bin/models/tokenizer_spm_32k_de.model`

---

## 5. Verification Approach

### 5.1 Training Convergence
- Monitor train loss, evaluation loss, and learning rate curves on Weights & Biases (W&B).
- Validate that the training evaluation loss drops consistently during the 3000 steps.

### 5.2 Tokenizer Extensions Verification
Run a verification script inside `/Users/whisper/tokenizer/.venv`:
```python
import sentencepiece as spm
sp = spm.SentencePieceProcessor(model_file="/data/tokenizer_spm_32k_de.model")
assert sp.get_piece_size() == 32007
assert sp.encode("Änderung über Große Straße", out_type=str) == [" Ä", "n", "d", "e", "r", "u", "n", "g", " ", "ü", "b", "e", "r", " G", "r", "o", "ß", "e", " S", "t", "r", "a", "ß", "e"]
```

### 5.3 Execution on M4 Mac Mini (Metal)
1. Launch the standalone Rust `moshi-rag-service` using the merged GGUF Q8_0 weights.
2. Measure step execution latencies on Metal to ensure step turnaround is <110ms.
3. Verify conversational comprehension and generation in German by speaking into the duplex channel.

### 5.4 End-to-End Pipeline Integration Test
Verify the complete dialogue pipeline end-to-end (WER, similarity metrics, latency) using the automated test suite.
Execute the following command:
```bash
python3 ./tests/run_pipeline_test.py --engine moshi-rag --model moshiko-de-merged-q8
```
This script will:
- Inject transcription WAV audio files from the `./Testfiles` directory via the local `SipProvider`.
- Verify transcription correctness against ground truth `.txt` files.
- Ensure character-level Levenshtein similarity meets the test suite thresholds (>= 90% WARN, >= 99.5% PASS).

---

## 6. Codebase Findings & Data Structure Reference

### 6.1 Audio Channel Requirements
- **Moshi Fine-Tuning**: 
  - Expects stereo audio.
  - The left channel (Channel 0) is processed for the main speaker (`SPEAKER_MAIN`), while the second channel (Channel 1) is the other speaker.
  - Under our updated double-mono design:
    - **Right channel muted chunks** (`_main`) contain `SPEAKER_MAIN` audio on Channel 0, and Channel 1 is muted. The transcription represents only what Channel 0 is saying, perfectly matching expectations.
    - **Left channel muted chunks** (`_other`) contain `SPEAKER_OTHER` audio on Channel 1, and Channel 0 is muted. The transcription represents only what Channel 1 is saying, aligning with the target formats.
- **STT-1B Model Training**:
  - Requires mono audio.
  - Since both channels of our Double-Mono are identical, extraction of either channel yields a perfect, pristine mono file for training.

### 6.2 Raw Dataset Folder Structure on `/Volumes/eHDD/moshi-rag-data/datasets/`
- **BeMaTac** (stereo 44.1kHz, EXB transcripts, ~10 files):
  - Transcripts: `BeMaTac/l1_exmaralda_2/*.exb` & `l2_exmaralda_2/*.exb` (EXMARaLDA XML, SPK0/SPK1 norm tiers)
  - Audio: `BeMaTac/l1_wav_2/*.wav` & `l2_wav_2/*.wav`
- **German Conversational Speech Corpus (GCSC)** (mono 16kHz, TXT transcripts, 20 WAV / 10 pairs):
  - Transcripts: `German_Conversational_Speech_Corpus/TXT/*.txt` (tab-separated with `[start, end]` timestamps)
  - Audio: `German_Conversational_Speech_Corpus/WAV/*.wav` (separate mono file per speaker)
- **German CallFriend Corpus** (stereo 8kHz, CHA transcripts, 60 files):
  - Transcripts: `German.CallFriend.Corpus/CallFriendTranscript/*.cha` (CHAT format with bullet timestamps)
  - Audio: `German.CallFriend.Corpus/CallFriendWav/*.wav`
- **German CallHome Corpus** (stereo 8kHz, CHA transcripts, 120 files):
  - Transcripts: `German.CallHome.Corpus/CallHomeTranscript/*.cha` (CHAT format)
  - Audio: `German.CallHome.Corpus/CallHomeWav/*.wav`
- **Gemischtes Hack Podcast** (MP3, Whisper JSON transcripts, episodes 150-300):
  - Transcripts: `Gemischtes.Hack.Podcast/transcripts/episode_*.json` (Speaker A/B segments)
  - Audio: `Gemischtes.Hack.Podcast/*.mp3`
- **Medical Conversations** (stereo WAV, SPEAKER_MAIN + SPEAKER_OTHER alignments, 500 files):
  - Audio & Alignments: `medical/stereo/*.wav` and `medical/stereo/*.json`
- **Nyrahealth Disfluency** (mono 16kHz, HF format, 202 files):
  - Audio: `nyrahealth/*.wav`
  - Metadata: `nyrahealth/*.json` (intended_transcript / verbatim_transcript fields)
- **Mozilla German Spontaneous Speech** (MP3, TSV metadata, 232 files):
  - Audio: `Moziila.German.Spontaneous/sps-corpus-3.0-2026-03-09-de/audios/*.mp3`
  - Metadata: `Moziila.German.Spontaneous/sps-corpus-3.0-2026-03-09-de/ss-corpus-de.tsv` (transcription column)

