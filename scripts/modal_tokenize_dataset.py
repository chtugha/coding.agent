#!/usr/bin/env python3
"""
modal_tokenize_dataset.py — Tokenize audio + text for nu-dialogue fine-tuning on Modal
═══════════════════════════════════════════════════════════════════════════════════════

Runs on a Modal GPU (1× A10G is enough — Mimi encoding is fast).
Reads from the Modal volume "moshi-german-data" and writes back to the same volume.

WHAT THIS DOES
──────────────
Step A — Audio tokenization (GPU)
  Reads  /data/nu_dataset/audio/ep{N}.wav          (stereo, Ch0=A, Ch1=B)
  Writes /data/nu_dataset/tokenized_audio/ep{N}.npz
         → {"A": int32[8, T], "B": int32[8, T]}   (Mimi, 12.5 Hz, 8 RVQ levels)

Step B — Text tokenization (CPU)
  Reads  /data/nu_dataset/text/ep{N}.json          (nu-dialogue word transcript)
  Writes /data/nu_dataset/tokenized_text/ep{N}.npz
         → {"A": int32[T], "B": int32[T]}           (SPM 32k, frame-aligned)

Step C — Prepare parquet dataset
  Merges tokenized_audio + tokenized_text into
  /data/nu_dataset/parquet/train-001-of-NNN.parquet
  /data/nu_dataset/parquet/eval-001-of-NNN.parquet

VOLUME LAYOUT REQUIRED BEFORE RUNNING
──────────────────────────────────────
  /data/nu_dataset/audio/ep{N}.wav     ← uploaded from local machine
  /data/nu_dataset/text/ep{N}.json     ← uploaded from local machine
  /data/nu_dataset/train.jsonl         ← produced by convert_w2json_to_nu_format.py
  /data/nu_dataset/eval.jsonl          ← produced by convert_w2json_to_nu_format.py

Upload with:
  modal volume put moshi-german-data /Volumes/eHDD/moshi-rag-data/nu_dataset /

Run:
  modal run scripts/modal_tokenize_dataset.py
  modal run scripts/modal_tokenize_dataset.py --steps audio   # only step A
  modal run scripts/modal_tokenize_dataset.py --steps text    # only step B
  modal run scripts/modal_tokenize_dataset.py --steps parquet # only step C
"""

import modal

# ── Modal app & volume ─────────────────────────────────────────────────────────
app    = modal.App("moshi-tokenize-dataset")
volume = modal.Volume.from_name("moshi-german-data")

MOSHI_REPO     = "kyutai/moshiko-pytorch-bf16"
MIMI_FILE      = "tokenizer-e351c8d8-checkpoint125.safetensors"
TOKENIZER_REPO = "kyutai/moshiko-pytorch-bf16"
TOKENIZER_FILE = "tokenizer_spm_32k_3.model"
TEXT_PADDING_ID         = 3   # Kyutai SPM padding token id
END_OF_TEXT_PADDING_ID  = 0   # Token inserted just before next real text token
AUDIO_FRAME_RATE        = 12.5  # Mimi frames/sec

# ── Image ─────────────────────────────────────────────────────────────────────
image = (
    modal.Image.debian_slim(python_version="3.12")
    .apt_install("ffmpeg", "libsndfile1")
    .pip_install(
        "torch==2.4.1",
        "torchaudio==2.4.1",
        "moshi==0.1.0",
        "sentencepiece>=0.2.0",
        "huggingface_hub>=0.24.0",
        "numpy>=1.26",
        "pandas>=2.2",
        "pyarrow>=16.0",
        "tqdm",
        "soundfile",
    )
)


# ═══════════════════════════════════════════════════════════════════════════════
# Step A — Audio tokenization
# ═══════════════════════════════════════════════════════════════════════════════

@app.function(
    image=image,
    gpu="A10G",
    volumes={"/data": volume},
    timeout=3600,
)
def tokenize_audio(resume: bool = True):
    """Tokenize all stereo WAVs in /data/nu_dataset/audio/ using Mimi."""
    import os
    import numpy as np
    import torch
    import torchaudio
    from huggingface_hub import hf_hub_download
    from moshi.models import loaders
    from tqdm import tqdm

    audio_dir   = "/data/nu_dataset/audio"
    out_dir     = "/data/nu_dataset/tokenized_audio"
    os.makedirs(out_dir, exist_ok=True)

    wav_files = sorted(f for f in os.listdir(audio_dir) if f.endswith(".wav"))
    if not wav_files:
        print("No WAV files found in", audio_dir)
        return

    if resume:
        done = {os.path.splitext(f)[0] for f in os.listdir(out_dir) if f.endswith(".npz")}
        wav_files = [f for f in wav_files if os.path.splitext(f)[0] not in done]
        print(f"Resuming: {len(done)} already done, {len(wav_files)} remaining")

    device = torch.device("cuda")
    mimi_path = hf_hub_download(MOSHI_REPO, MIMI_FILE)
    mimi = loaders.get_mimi(filename=mimi_path, device=device)
    mimi.eval()
    print(f"Mimi loaded — frame_rate={mimi.frame_rate}, sample_rate={mimi.sample_rate}, "
          f"num_codebooks={mimi.num_codebooks}")

    resampler_cache: dict[int, torchaudio.transforms.Resample] = {}

    def _resample(wav: torch.Tensor, src_sr: int) -> torch.Tensor:
        if src_sr == mimi.sample_rate:
            return wav
        if src_sr not in resampler_cache:
            resampler_cache[src_sr] = torchaudio.transforms.Resample(
                src_sr, mimi.sample_rate).to(device)
        return resampler_cache[src_sr](wav)

    def _encode_channel(wav_1d: torch.Tensor) -> np.ndarray:
        """Encode a single mono channel. Returns int32 ndarray [K, T]."""
        wav_chunk_size = 60 * mimi.sample_rate  # 60 s chunks to avoid OOM
        n_chunks = max(1, -(-wav_1d.shape[0] // wav_chunk_size))
        parts = []
        for i in range(n_chunks):
            chunk = wav_1d[i * wav_chunk_size:(i + 1) * wav_chunk_size]
            with torch.no_grad():
                tokens = mimi.encode(chunk.reshape(1, 1, -1).to(device))  # [1, K, T]
            parts.append(tokens[0].cpu())
        return torch.cat(parts, dim=-1).numpy().astype(np.int32)

    errors = 0
    for fname in tqdm(wav_files, desc="Tokenizing audio"):
        name = os.path.splitext(fname)[0]
        wav_path = os.path.join(audio_dir, fname)
        out_path = os.path.join(out_dir, f"{name}.npz")
        try:
            wavs, sr = torchaudio.load(wav_path)
            assert wavs.shape[0] == 2, f"Expected stereo, got {wavs.shape[0]} channels"
            wavs = _resample(wavs.to(device), sr)
            a_tokens = _encode_channel(wavs[0])  # [K=8, T]
            b_tokens = _encode_channel(wavs[1])  # [K=8, T]
            np.savez_compressed(out_path, A=a_tokens, B=b_tokens)
        except Exception as e:
            print(f"  ERROR {fname}: {e}")
            errors += 1

    volume.commit()
    print(f"\nAudio tokenization done. errors={errors}")


# ═══════════════════════════════════════════════════════════════════════════════
# Step B — Text tokenization
# ═══════════════════════════════════════════════════════════════════════════════

@app.function(
    image=image,
    cpu=4,
    memory=4096,
    volumes={"/data": volume},
    timeout=1800,
)
def tokenize_text(resume: bool = True):
    """Tokenize all word-transcript JSONs in /data/nu_dataset/text/ using the Kyutai SPM."""
    import json
    import os
    import warnings

    import numpy as np
    from huggingface_hub import hf_hub_download
    from sentencepiece import SentencePieceProcessor
    from tqdm import tqdm

    text_dir = "/data/nu_dataset/text"
    out_dir  = "/data/nu_dataset/tokenized_text"
    os.makedirs(out_dir, exist_ok=True)

    json_files = sorted(f for f in os.listdir(text_dir) if f.endswith(".json"))
    if not json_files:
        print("No JSON files found in", text_dir)
        return

    if resume:
        done = {os.path.splitext(f)[0] for f in os.listdir(out_dir) if f.endswith(".npz")}
        json_files = [f for f in json_files if os.path.splitext(f)[0] not in done]
        print(f"Resuming: {len(done)} already done, {len(json_files)} remaining")

    spm_path = hf_hub_download(TOKENIZER_REPO, TOKENIZER_FILE)
    sp = SentencePieceProcessor(spm_path)
    print(f"SPM loaded — vocab_size={sp.vocab_size()}")

    def _tokenize_speaker(words: list[dict]) -> list[int]:
        """Convert a single-speaker word list to a frame-aligned token-id list."""
        if not words:
            return []
        words = sorted(words, key=lambda w: w["start"])

        # Build full text with spaces between words (German needs spaces)
        full_text = " ".join(w["word"].strip() for w in words)
        tokens = sp.encode_as_pieces(full_text)

        # Build character-level timeline
        char_timeline = []
        for seg in words:
            word_with_space = " " + seg["word"].strip()
            n_chars = len(word_with_space)
            dur_per_char = max((seg["end"] - seg["start"]) / n_chars, 1e-4)
            # First char is the leading space — share the word's start time
            for ci, ch in enumerate(word_with_space):
                char_timeline.append({
                    "char":  ch,
                    "start": seg["start"] + ci * dur_per_char,
                    "end":   seg["start"] + (ci + 1) * dur_per_char,
                })

        # Align tokens to the character timeline
        token_timeline = []
        remaining_chars = list(char_timeline)
        for token in tokens:
            # sentencepiece uses ▁ for word boundary (= leading space)
            clean = token.replace("▁", " ").lstrip(" ") if token.startswith("▁") else token
            n = len(token.replace("▁", " "))
            if not remaining_chars:
                break
            chunk = remaining_chars[:n]
            remaining_chars = remaining_chars[n:]
            if chunk:
                token_timeline.append({
                    "token": token,
                    "start": chunk[0]["start"],
                    "end":   chunk[-1]["end"],
                })

        # Produce frame-aligned token ID array
        if not token_timeline:
            return []
        num_frames = int((token_timeline[-1]["end"] + 1.0) * AUDIO_FRAME_RATE)
        token_ids = [TEXT_PADDING_ID] * num_frames
        sec_per_frame = 1.0 / AUDIO_FRAME_RATE

        for tok in token_timeline:
            frame_idx = int(tok["start"] / sec_per_frame)
            # Advance past already-filled frames
            while frame_idx < len(token_ids) and token_ids[frame_idx] != TEXT_PADDING_ID:
                frame_idx += 1
            if frame_idx >= len(token_ids):
                warnings.warn(f"Token overflow: dropped tail tokens")
                break
            token_ids[frame_idx] = sp.piece_to_id(tok["token"])
            # Mark preceding frame with end-of-text padding if empty
            if frame_idx > 0 and token_ids[frame_idx - 1] == TEXT_PADDING_ID:
                token_ids[frame_idx - 1] = END_OF_TEXT_PADDING_ID

        return token_ids

    errors = 0
    for fname in tqdm(json_files, desc="Tokenizing text"):
        name = os.path.splitext(fname)[0]
        json_path = os.path.join(text_dir, fname)
        out_path  = os.path.join(out_dir, f"{name}.npz")
        try:
            with open(json_path, encoding="utf-8") as fh:
                transcript = json.load(fh)
            words_a = [w for w in transcript if w["speaker"] == "A"]
            words_b = [w for w in transcript if w["speaker"] == "B"]
            ids_a = np.array(_tokenize_speaker(words_a), dtype=np.int32)
            ids_b = np.array(_tokenize_speaker(words_b), dtype=np.int32)
            np.savez_compressed(out_path, A=ids_a, B=ids_b)
        except Exception as e:
            print(f"  ERROR {fname}: {e}")
            errors += 1

    volume.commit()
    print(f"\nText tokenization done. errors={errors}")


# ═══════════════════════════════════════════════════════════════════════════════
# Step C — Assemble parquet dataset
# ═══════════════════════════════════════════════════════════════════════════════

@app.function(
    image=image,
    cpu=4,
    memory=8192,
    volumes={"/data": volume},
    timeout=1800,
)
def prepare_parquet():
    """Merge tokenized audio + text → parquet files for nu-dialogue finetune."""
    import json
    import os

    import numpy as np
    import pandas as pd
    from tqdm import tqdm

    audio_tok_dir = "/data/nu_dataset/tokenized_audio"
    text_tok_dir  = "/data/nu_dataset/tokenized_text"
    out_dir       = "/data/nu_dataset/parquet"
    os.makedirs(out_dir, exist_ok=True)

    def _merge(text_ids: np.ndarray, audio_ids: np.ndarray) -> list:
        """Merge text [T] + audio [K=8, T] into [K=9, T] list-of-lists."""
        T = audio_ids.shape[1]
        if text_ids.shape[0] > T:
            text_ids = text_ids[:T]
        elif text_ids.shape[0] < T:
            text_ids = np.concatenate(
                [text_ids, np.full(T - text_ids.shape[0], TEXT_PADDING_ID)], axis=0)
        merged = np.concatenate([text_ids[None], audio_ids], axis=0).astype(np.int32)
        return merged.tolist()

    def _build_manifest(jsonl_path: str) -> list[str]:
        """Return list of basenames from a .jsonl manifest."""
        names = []
        if not os.path.exists(jsonl_path):
            return names
        with open(jsonl_path) as fh:
            for line in fh:
                item = json.loads(line.strip())
                path = item.get("path", "")
                name = os.path.splitext(os.path.basename(path))[0]
                if name:
                    names.append(name)
        return names

    for split in ("train", "eval"):
        manifest = _build_manifest(f"/data/nu_dataset/{split}.jsonl")
        if not manifest:
            print(f"No {split}.jsonl found or empty — skipping {split} parquet")
            continue

        # Filter to names that have both audio and text tokens
        valid = []
        for name in manifest:
            a = os.path.join(audio_tok_dir, f"{name}.npz")
            t = os.path.join(text_tok_dir,  f"{name}.npz")
            if os.path.exists(a) and os.path.exists(t):
                valid.append(name)
            else:
                print(f"  {name}: missing tokenized files — skipped")

        print(f"{split}: {len(valid)}/{len(manifest)} episodes ready")

        rows = []
        for name in tqdm(valid, desc=f"Building {split} parquet"):
            audio_data = np.load(os.path.join(audio_tok_dir, f"{name}.npz"))
            text_data  = np.load(os.path.join(text_tok_dir,  f"{name}.npz"))
            rows.append({
                "dialogue_id": f"nu_dataset/{split}/{name}",
                "A": _merge(text_data["A"], audio_data["A"]),
                "B": _merge(text_data["B"], audio_data["B"]),
            })

        num_parts = max(1, -(-len(rows) // 100_000))
        for i in range(num_parts):
            chunk = rows[i * 100_000:(i + 1) * 100_000]
            out_path = os.path.join(out_dir, f"{split}-{i+1:03d}-of-{num_parts:03d}.parquet")
            pd.DataFrame(chunk).to_parquet(out_path, index=False)
            print(f"  Wrote {out_path}  ({len(chunk)} rows)")

    volume.commit()
    print("\nParquet preparation done.")


# ═══════════════════════════════════════════════════════════════════════════════
# Local entrypoint
# ═══════════════════════════════════════════════════════════════════════════════

@app.local_entrypoint()
def main(steps: str = "all"):
    """
    steps: comma-separated subset of: audio, text, parquet, all
    Example: modal run scripts/modal_tokenize_dataset.py --steps audio,text
    """
    run_all   = steps == "all"
    run_audio  = run_all or "audio"  in steps
    run_text   = run_all or "text"   in steps
    run_parquet = run_all or "parquet" in steps

    if run_audio:
        print("=== Step A: Tokenize Audio ===")
        tokenize_audio.remote(resume=True)

    if run_text:
        print("=== Step B: Tokenize Text ===")
        tokenize_text.remote(resume=True)

    if run_parquet:
        print("=== Step C: Prepare Parquet ===")
        prepare_parquet.remote()

    print("All requested steps complete.")
