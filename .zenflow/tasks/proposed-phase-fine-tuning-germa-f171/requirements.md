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

### FR2. Dataset Alignment and Tokens Generation
- **What**: Training targets must be tokenized and aligned to Mimi audio codes using the extended 32k text tokenizer. Spontaneous datasets must be split dynamically based on speaker turn changes, converting raw audio into stereo audio chunks containing exactly one active speaker's turn. There will always be one channel muted. Specifically, if Speaker A (or any even index speaker) speaks, the left channel is muted (all zeros) and the right channel contains the unmuted speech. If Speaker B (or any odd index speaker) speaks, the right channel is muted (all zeros) and the left channel contains the unmuted speech. Sibling speakers continue this pattern (C: left muted, D: right muted, etc.).
- **No Fact Poisoning**: To prevent poisoning the alignment data and ensure correct validation against `whisper.cpp`, the matching alignment JSON files must contain ONLY the exact transcripts matching the words spoken in that turn. No facts or prefill reference tags are prepended.
- **Why**: Aligns text and audio tokens for full-duplex modeling, so the model learns the tight relationship between German speech phonetics, textual transcriptions, and conversational audio under proper stereo duplex configurations.

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
