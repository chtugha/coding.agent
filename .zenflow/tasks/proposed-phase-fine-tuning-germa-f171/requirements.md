# Product Requirements Document: Fine-Tuning German Moshi-RAG LoRA (7B)

## 1. Overview and Business Context
Moshi-RAG is a duplex conversational language model trained to support natural, real-time, low-latency dialog. It uses teacher-forcing context prefilling to inject text-based references (such as search results, medical guidelines, or context snippets) during live conversation.

To support high-quality medical and conversational voice assistant capabilities in the German market, the 7B parameters duplex LM must be fine-tuned to understand and generate German speech, handle German phonetics/characters natively, and retain its native RAG teacher-forcing context prefilling mechanism.

---

## 2. Scope & Pre-existing Work Analysis
We analyzed the local files, Modal volumes (`moshi-german-data` and `moshi-german-checkpoints`), and previous phase plans. Below is the alignment mapping:

- **Completed Work (Do Not Duplicate)**:
  - **STT Fine-Tuning**: The 1B parameter Speech-to-Text model (`kyutai/stt-1b-en_fr`) has already been successfully fine-tuned on ~600 hours of German speech data over 3000 steps on Modal. The merged and quantized models are exported and available for integration.
  - **Dataset Preparation**: The target training datasets are fully formatted and stored on the Modal volume `/data`:
    - **General Conversational Data**: ~600 hours of general German conversational speech used in the STT phase are available on the volume (`/data/train.jsonl` and `/data/valid.jsonl`).
    - **Medical Data**: The **500 custom medical conversations** (~15-50 hours) are already formatted into duplex stereo files with main and other speaker audio tracks under `/data/stereo` (files `med_0000` to `med_0299` and `mec_0000` to `mec_0057`). No further manual re-formatting of these files is necessary.
  - **Finetuning Scripts & Vocab Resizing**: The python codebase (`/Users/whisper/zenflow_projects/moshi-finetune/finetune/wrapped_model.py`) already has built-in, generic support for dynamically resizing the model vocabulary (resizing text embedding and output linear head layers, copying base weights, and initializing new token weights) when a custom tokenizer is loaded.

- **Pending Work (Scope of Current/Next Steps)**:
  - **32k Tokenizer Extension**: The base 32k SentencePiece tokenizer of the 7B Moshi model (`tokenizer_spm_32k_3.model`) has **not** been extended. The existing German tokenizer (`/Users/whisper/tokenizer/tokenizer_model/tokenizer_de.model`) is the 8k variant for STT-1B.
  - **Moshi-RAG 7B LoRA Training**: The 7B duplex LM has not yet been fine-tuned on German speech or medical dialog.

---

## 3. Product Objectives
- **German Language Comprehension and Generation**: Enable the 7B duplex model to naturally understand, interact, and generate speech in German.
- **Vocabulary Support**: Correctly handle German special characters (`ä`, `ö`, `ü`, `ß`) without falling back to out-of-vocabulary representation.
- **RAG Capability Preservation**: Retain native support for the teacher-forcing context prefill mechanism and its prefill tokens.
- **Hardware Compatibility**: Produce high-performance, compact variants of the merged model suitable for low-latency local deployment (e.g., M4 Mac Mini).

---

## 4. Functional Requirements

### FR1. Extended Tokenizer Alignment
- **What**: The 32k text SentencePiece tokenizer from the base model `kyutai/moshika-rag-candle-bf16` must be extended with German characters (`ä`, `ö`, `ü`, `ß`).
- **Why**: Ensures proper orthographic representation and parsing of German words without segmenting them into unknown tokens or single bytes.

### FR2. Dataset Alignment and Redesign
- **What**: Training targets must be prepared in a single processed folder. All **8 datasets** (BeMaTac, GCSC, CallFriend, CallHome, Gemischtes Hack Podcast, Medical, Nyrahealth Disfluency, Mozilla German Spontaneous) will be processed as follows:
  1. Any stereo dialog file is first converted to mono (by averaging channels) to merge both speakers into a single mono track.
  2. The mono audio is resampled/converted to **48 kHz / 16-Bit / Stereo PCM** containing the same audio information on both channels (**double-mono**).
  3. The double-mono audio is split into chunks at speaker turn boundaries. The splitting points are calculated to occur exactly at the **midpoint** between the end of one speaker's last word and the start of the next speaker's first word, ensuring no mid-word/sentence cuts.
  4. **No padding** and **no fading in or out** is added.
  5. **`_main` files** (right channel muted): Left channel (Channel 0) contains the active double-mono audio, right channel is completely zeroed. The corresponding JSON transcript contains ONLY the timestamped text of `SPEAKER_MAIN`.
  6. **`_other` files** (left channel muted): Right channel (Channel 1) contains the active double-mono audio, left channel is completely zeroed. The corresponding JSON transcript contains ONLY the timestamped text of `SPEAKER_OTHER`.
  7. **Single-speaker datasets** (Nyrahealth, Mozilla) produce only `_main` files with no splitting.
  8. **RAG Prefill (FACTS) Injection**: ~1% of `SPEAKER_MAIN` chunks get astronomical/quantum fact prefills prepended, but **only** for chunks containing more than one minute of speech.
  9. Output is stored in a single processed folder under dataset-specific subfolders. No training/validation `.jsonl` manifest files are generated in this step.
  10. **ALL files from every dataset MUST be processed.** No file count limits, no sample limits, no duration caps. Data quality and accuracy is the absolute priority.
- **Why**: Aligns text and audio tokens for full-duplex modeling under a single processed directory schema, ensuring absolute timestamp accuracy and precise gating without artificial padding/fading artifacts, which enables robust training.

### FR3. Model Resizing and Weight Initialization
- **What**: The model's text embedding and linear projection head layers must be resized to match the new piece size of the extended 32k tokenizer. New weights for the German tokens must be initialized appropriately while copying all base vocabulary weights.
- **Why**: Prepares the 7B architecture to process the newly added German tokens during training.

### FR4. LoRA Fine-Tuning Setup
- **What**: Configures the 7B Moshi-RAG model for training using a Rank 64 LoRA adapter. The training configuration must unfreeze the resized text embeddings during adapter training to allow learning of German text semantics, while the scaling factor must be set to 2.0.
- **Why**: Restricting trainable weights to the LoRA parameters and embeddings ensures resource-efficient fine-tuning on Modal GPUs while preventing catastrophic forgetting.

### FR5. Model Merging and Formats Export
- **What**: The trained LoRA adapter weights must be merged into the base `kyutai/moshika-rag-candle-bf16` weights. The resulting merged model must be exported in:
  - High-fidelity `bfloat16` weights.
  - Quantized `int8` (GGUF format compatible with Candle/Rust) to run efficiently with Metal hardware acceleration on local target devices (such as an M4 Mac Mini).
- **Why**: Ensures portability of the model and real-time execution capabilities in deployment environments where resource and latency bounds are strict.

### FR6. Hugging Face Hosting
- **What**: Exported model weights, quantized weights, configuration, and LoRA adapters must be uploaded to Hugging Face repositories for distribution and integration.
- **Why**: Simplifies deployment pulls and maintains version control of assets.

---

## 5. Non-Functional Requirements & Constraints
- **Latency**: The fine-tuned model's GGUF variant must sustain real-time duplex dialogue steps on an M4 Mac Mini under Metal acceleration (target steady-state step latency <110ms).
- **RAG Token Handling**: The fine-tuning process must preserve the exact functionality of the RAG context prefill tokens and context injection logic defined in the model configuration.
- **Training Resources**: Training must run efficiently on Modal using high-performance GPU instances (e.g., A100 or H200) with a maximum timeout and checkpoints volume storage configuration.
