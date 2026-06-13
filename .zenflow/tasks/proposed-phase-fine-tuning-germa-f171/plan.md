# Full SDD workflow

## Configuration
- **Artifacts Path**: {@artifacts_path} → `.zenflow/tasks/{task_id}`

---

## Agent Instructions

---

## Workflow Steps

### [x] Step: Requirements
<!-- chat-id: 7e031612-5f3d-4d0c-a9b8-39e496de4fc1 -->

Create a Product Requirements Document (PRD) based on the feature description.

1. Review existing codebase to understand current architecture and patterns
2. Analyze the feature definition and identify unclear aspects
3. Ask the user for clarifications on aspects that significantly impact scope or user experience
4. Make reasonable decisions for minor details based on context and conventions
5. If user can't clarify, make a decision, state the assumption, and continue

Focus on **what** the feature should do and **why**, not **how** it should be built. Do not include technical implementation details, technology choices, or code-level decisions — those belong in the Technical Specification.

Save the PRD to `{@artifacts_path}/requirements.md`.

### [x] Step: Technical Specification
<!-- chat-id: 1308f022-33bf-485f-a562-d6932b9a8d74 -->

Create a technical specification based on the PRD in `{@artifacts_path}/requirements.md`.

1. Review existing codebase architecture and identify reusable components
2. Define the implementation approach

Do not include implementation steps, phases, or task breakdowns — those belong in the Planning step.

Save to `{@artifacts_path}/spec.md` with:
- Technical context (language, dependencies)
- Implementation approach referencing existing code patterns
- Source code structure changes
- Data model / API / interface changes
- Verification approach using project lint/test commands

### [x] Step: Planning
<!-- chat-id: 2a508ff4-2817-4278-83bd-eecca936554f -->

Create a detailed implementation plan based on `./.zenflow/tasks/proposed-phase-fine-tuning-germa-f171/spec.md`.

1. Break down the work into concrete tasks
2. Each task should reference relevant contracts and include verification steps
3. Replace the Implementation step below with the planned tasks

Rule of thumb for step size: each step should represent a coherent unit of work (e.g., implement a component, add an API endpoint). Avoid steps that are too granular (single function) or too broad (entire feature).

Important: unit tests must be part of each implementation task, not separate tasks. Each task should implement the code and its tests together, if relevant.

If the feature is trivial and doesn't warrant full specification, update this workflow to remove unnecessary steps and explain the reasoning to the user.

Save to `./.zenflow/tasks/proposed-phase-fine-tuning-germa-f171/plan.md`.

### [x] Step: Standalone Tokenizer Extension
<!-- chat-id: 0906e901-656a-4127-90b3-7f864487e2d0 -->
Create the script `./scripts/extend_tokenizer.py` that loads the base SentencePiece model `tokenizer_spm_32k_3.model`, appends the seven German characters (`ä`, `ö`, `ü`, `ß`, `Ä`, `Ö`, `Ü`), saves the extended model as `/data/tokenizer_spm_32k_de.model` on the Modal volume `/data`.
- **Verification**: Run a validation Python script inside `/Users/whisper/tokenizer/.venv` that checks that the piece size is exactly 32,007 and encodes a test German string without producing unknown bytes or multi-byte tokens.

### [x] Step: Dataset Preparation and Mimi Alignment
<!-- chat-id: a36b901d-28f1-4b04-b384-230b5ec9f9c6 -->
Process all 8 datasets (BeMaTac, GCSC, CallFriend, CallHome, Gemischtes Hack Podcast, Medical, Nyrahealth Disfluency, Mozilla German Spontaneous) from `/Volumes/eHDD/moshi-rag-data/datasets/` into a single processed folder at `/Volumes/eHDD/moshi-rag-data/processed/`.

**CRITICAL REQUIREMENTS — DATA QUALITY AND COMPLETENESS:**
- **ALL files from every dataset MUST be processed.** No file count limits, no sample limits, no duration caps.
- **Data quality and accuracy is the absolute priority.** No shortcuts, no skipped files, no silent error swallowing.
- **Every error must be logged** with dataset name, file ID, and error message for debugging.
- **No padding, no fading** added during channel muting.
- **FACTS** (astronomical/quantum sentences) are only added to SPEAKER_MAIN chunks with > 1 minute of speech.

**Processing pipeline:**
1. All stereo input → mono (channel average) → resample to 48kHz → double-mono (same audio on both channels).
2. Split at speaker turn boundaries: midpoint between end of one speaker's last word and start of next speaker's first word.
3. `_main` files: Left channel = audio, right channel = zeros, JSON = SPEAKER_MAIN words only.
4. `_other` files: Left channel = zeros, right channel = audio, JSON = SPEAKER_OTHER words only.
5. Single-speaker datasets (Nyrahealth, Mozilla) → only `_main` files, no splitting.
6. FACTS injection: ~1% probability on SPEAKER_MAIN chunks > 60s.
7. No manifest `.jsonl` generation — delegated to subsequent step.

**Verification**: Run `scripts/verify_all_files.py` to verify:
- Correct stereo format (48kHz, 16-bit, proper channel zeroing)
- JSON alignment integrity (correct speaker labels, valid timestamps)
- WER checks via whisper-cli on sampled files from each dataset
- FACTS distribution summary

### [ ] Step: Configure and Setup Modal LoRA Training
Configure and save the LoRA training config file `/Users/whisper/zenflow_projects/moshi-finetune/configs/moshi_7b_de.yaml` and the training script `/Users/whisper/zenflow_projects/moshi-finetune/modal_train_rag.py`.
- Configure Rank 64, Scaling 2.0, unfreeze text embedding and linear output projection layers (`ft_embed: true`), setting `lr: 2e-6`, and `max_steps: 3000`.
- **Verification**: Run a local/Modal check or a dry-run to ensure the configurations and script load without syntax errors or parameter mismatches.

### [ ] Step: Execute Two-Stage LoRA Fine-Tuning on Modal
Run the distributed training orchestrator `/Users/whisper/zenflow_projects/moshi-finetune/modal_train_rag.py` in two distinct stages on high-end GPU instances on Modal:
1. **Stage 1 (Conversational German Alignment)**: Fine-tune on the spontaneous, natural, true stereo-duplex German conversations (BeMaTac, German_Conversational_Speech_Corpus, German.CallFriend.Corpus, German.CallHome.Corpus, and Gemischtes.Hack.Podcast) stored on the Modal persistent volume. Evaluate performance against the `nyrahealth/disfluency_speech_german` validation split to ensure precise conversational and phonetic alignment.
2. **Stage 2 (Medical Domain Specialization)**: Continually fine-tune the resulting Stage 1 checkpoint on the 15 hours of custom medical duplex conversations mixed with validation splits and RAG injection targets.
- Monitor convergence metrics (training loss, evaluation loss, learning rate decay) on Weights & Biases, using the Nyra disfluency dataset as our primary validation split.
- Confirm checkpoints are saved in `/checkpoints/moshi_7b_de` on the Modal checkpoints volume.
- **Verification**: Check that train/val loss curves consistently decrease for both stages and verify the existence of the final merged checkpoint directories.

### [ ] Step: Execute Two-Stage 1B STT Model Retraining on Modal
Retrain the German 1B STT model on Modal to reuse the same conversational and medical datasets.
1. **Stage 1 (Conversational German Alignment)**: Train/fine-tune the 1B STT model on the spontaneous German conversational datasets (BeMaTac, German_Conversational_Speech_Corpus, German.CallFriend.Corpus, German.CallHome.Corpus, and Gemischtes.Hack.Podcast).
2. **Stage 2 (Medical Specialization)**: Fine-tune the checkpoint from Stage 1 on the 15 hours of custom medical duplex conversations to focus on clinical terms while retaining conversational speech robustness.
- Monitor convergence metrics on Weights & Biases.
- Save final checkpoints to `/checkpoints/stt_1b_de` on the Modal checkpoints volume.
- **Verification**: Check training loss and run STT validation tests to ensure lower word error rates (WER) across both conversational and clinical speech.

### [ ] Step: Merge LoRA Adapters with Base Model
Use the renamed `./scripts/merge_moshi-rag_lora.py` script to merge the trained Rank 64 LoRA adapter weights back into the base `kyutai/moshika-rag-candle-bf16` weights, passing the `--text-tokenizer-file /data/tokenizer_spm_32k_de.model` parameter explicitly to handle the extended vocabulary size.
- The output merged file must be saved in `bfloat16` directly to `/Volumes/eHDD/moshi-rag-data/models/moshiko-de-merged.safetensors` (or symlinked back to `./bin/models/moshiko-de-merged.safetensors`) to preserve space on the main SSD.
- **Verification**: Verify that the file exists at `/Volumes/eHDD/moshi-rag-data/models/moshiko-de-merged.safetensors` (or symlinked path) and check its headers/tensors metadata to ensure text layers have been correctly resized to size 32,007.

### [ ] Step: Quantize Merged Model to GGUF format
Convert the merged `bfloat16` safetensors model into a `Q8_0` quantized GGUF format compatible with Candle using the `./moshi-rag/scripts/safetensors_to_gguf_q8.py` utility.
- Save the quantized model output to `/Volumes/eHDD/moshi-rag-data/models/moshiko-de-merged-q8.gguf` (and optionally symlink to `./bin/models/moshiko-de-merged-q8.gguf`).
- **Verification**: Check that the output GGUF file exists, is approximately ~7-8GB, and validates successfully using GGUF reader checks.

### [ ] Step: Configure Local Rust Service and Verify Performance
Integrate and run the merged 7B LM and the retrained 1B STT model locally on the Apple Silicon device (M4 Mac Mini) using Metal hardware acceleration.
- Update configuration file `./moshi-rag/rust/moshi-backend/config-rag.json` and its variants (e.g. `./moshi-rag/rust/moshi-backend/config-q8.json`) with the paths pointing to `/Volumes/eHDD/moshi-rag-data/models/moshiko-de-merged-q8.gguf`, `./bin/models/tokenizer_spm_32k_de.model`, and the retrained `/Volumes/eHDD/moshi-rag-data/models/stt-1b-de.safetensors` (or symlinks under `./bin/models/`).
- Run the local backend `bin/moshi-rag-service`.
- Run the pipeline integration test to verify the complete dialogue loop:
  `python3 ./tests/run_pipeline_test.py --engine moshi-rag --model moshiko-de-merged-q8`
- **Verification**: Ensure the standalone rust backend starts successfully with Metal support, maintains step latency <110ms, and the pipeline test passes with character-level accuracy meeting target thresholds.

### [ ] Step: Deploy to Hugging Face
Upload the merged weights, quantized GGUF files, config.json, the retrained 1B STT model, and the LoRA adapter checkpoints to Hugging Face repository endpoints. As the training dataset cotntains real persons speech it needs to be put into a private hf endpoint.
- **Verification**: Confirm that files are successfully uploaded and can be resolved/pulled from Hugging Face.
