#!/usr/bin/env python3
# train_modal.py — Step 3 of German Moshi fine-tune pipeline.
#
# Orchestrates three remote steps on Modal cloud infrastructure:
#   1. upload   — copies local stereo WAVs to a Modal persistent Volume
#   2. annotate — runs moshi-finetune's annotate.py (Whisper on GPU) to generate
#                 per-WAV JSON transcript files alongside each WAV in the Volume
#   3. train    — runs LoRA fine-tuning on an H100 and saves the checkpoint
#
# Usage:
#   modal run scripts/train_modal.py            # full pipeline (all three steps)
#   modal run scripts/train_modal.py --upload-only
#   modal run scripts/train_modal.py --annotate-only
#   modal run scripts/train_modal.py --train-only
#
# Requirements: pip install modal tqdm pyyaml
# Auth:         modal token set (done via "python3 -m modal setup")
# HF token:     modal secret create huggingface-token HF_TOKEN=hf_...
#               (needed to download kyutai/moshiko-pytorch-bf16)

import argparse
import json
from pathlib import Path

import modal

# ── paths ────────────────────────────────────────────────────────────────────

LOCAL_DATA_DIR  = Path(__file__).parent.parent / "data" / "moshi_german"
LOCAL_STEREO    = LOCAL_DATA_DIR / "stereo"
LOCAL_JSONL     = LOCAL_DATA_DIR / "train_local.jsonl"

# Modal-side paths (inside the Volume)
VOL_STEREO      = "/data/stereo"
VOL_JSONL       = "/data/train.jsonl"
VOL_CHECKPOINTS = "/checkpoints"

# ── Modal resources ───────────────────────────────────────────────────────────

app  = modal.App("moshi-german-finetune")
data_vol  = modal.Volume.from_name("moshi-german-data",        create_if_missing=True)
ckpt_vol  = modal.Volume.from_name("moshi-german-checkpoints", create_if_missing=True)

# Shared GPU image (annotate + train both need CUDA + moshi-finetune)
gpu_image = (
    modal.Image.debian_slim(python_version="3.12")
    .apt_install(["git", "ffmpeg", "wget"])
    .pip_install(
        "torch==2.5.1",
        "torchaudio==2.5.1",
        extra_index_url="https://download.pytorch.org/whl/cu124",
    )
    .pip_install(
        "sphn",
        "moshi",
        "whisper-timestamped",
        "auditok",
        "submitit",
        "huggingface_hub[cli]",
        "safetensors",
        "pyyaml",
        "tqdm",
    )
    .run_commands(
        "git clone --depth 1 https://github.com/kyutai-labs/moshi-finetune /moshi-finetune",
        "cd /moshi-finetune && pip install -e . --quiet",
    )
)

# ── LoRA training config ──────────────────────────────────────────────────────
# H100 80GB: batch=8, duration=80 fits comfortably.
# Reduce batch_size to 4 and duration_sec to 60 if you need to use an A100 40GB.

TRAIN_CONFIG = {
    "moshi_paths": {"hf_repo_id": "kyutai/moshiko-pytorch-bf16"},
    "full_finetuning": False,
    "lora": {
        "enable": True,
        "rank": 64,
        "scaling": 2.0,
        "ft_embed": False,
    },
    "first_codebook_weight_multiplier": 100.0,
    "text_padding_weight": 0.5,
    "duration_sec": 80,
    "batch_size": 8,
    "max_steps": 3000,
    "gradient_checkpointing": True,
    "optim": {
        "lr": 2e-6,
        "weight_decay": 0.1,
        "pct_start": 0.05,
    },
    "seed": 42,
    "log_freq": 10,
    "eval_freq": 300,
    "do_eval": False,
    "do_ckpt": True,
    "ckpt_freq": 500,
    "save_adapters": True,
    "data": {
        "train_data": VOL_JSONL,
        "shuffle": True,
        "eval_data": "",
    },
    "run_dir": f"{VOL_CHECKPOINTS}/run_001",
    "overwrite_run_dir": True,
}

# ── Step 1: upload ────────────────────────────────────────────────────────────

def do_upload():
    """Upload local stereo WAVs + manifest to Modal Volume (runs locally)."""
    try:
        from tqdm import tqdm
    except ImportError:
        raise SystemExit("Install tqdm first: pip install tqdm")

    if not LOCAL_STEREO.exists():
        raise SystemExit(f"Stereo directory not found: {LOCAL_STEREO}\n"
                         "Run synthesize_stereo.py first.")
    if not LOCAL_JSONL.exists():
        raise SystemExit(f"Manifest not found: {LOCAL_JSONL}\n"
                         "Run synthesize_stereo.py first.")

    wav_files = sorted(LOCAL_STEREO.glob("*.wav"))
    if not wav_files:
        raise SystemExit(f"No WAV files found in {LOCAL_STEREO}")

    print(f"Uploading {len(wav_files)} WAV files to Modal Volume 'moshi-german-data'...")

    # Rewrite local paths to Modal Volume paths in the manifest
    modal_entries = []
    with LOCAL_JSONL.open() as f:
        for line in f:
            entry = json.loads(line)
            wav_name = Path(entry["path"]).name
            modal_entries.append({
                "path":     f"{VOL_STEREO}/{wav_name}",
                "duration": entry["duration"],
            })

    with data_vol.batch_upload(force=True) as batch:
        for wav_path in tqdm(wav_files, unit="file", desc="WAVs"):
            batch.put_file(str(wav_path), f"stereo/{wav_path.name}")

        manifest_bytes = "\n".join(json.dumps(e) for e in modal_entries).encode()
        batch.put_bytes(manifest_bytes, "train.jsonl")

    total_h = sum(e["duration"] for e in modal_entries) / 3600
    print(f"✓ Uploaded {len(wav_files)} WAVs ({total_h:.2f} h audio) + manifest")

# ── Step 2: annotate ──────────────────────────────────────────────────────────

@app.function(
    gpu="A10G",
    volumes={"/data": data_vol},
    image=gpu_image,
    secrets=[modal.Secret.from_name("huggingface-token")],
    timeout=4 * 3600,
)
def do_annotate():
    """Run moshi-finetune annotate.py (Whisper) on the uploaded WAVs."""
    import subprocess

    cmd = [
        "python", "/moshi-finetune/annotate.py",
        "--local",
        "--lang", "de",
        "--whisper_model", "medium",
        VOL_JSONL,
    ]
    print("Running annotation:", " ".join(cmd))
    result = subprocess.run(cmd, capture_output=False)
    if result.returncode != 0:
        raise RuntimeError(f"annotate.py exited with code {result.returncode}")

    data_vol.commit()
    print("✓ Annotation complete — .json files written alongside each WAV")

# ── Step 3: train ─────────────────────────────────────────────────────────────

@app.function(
    gpu=["H100", "A100-80GB"],
    volumes={
        "/data":        data_vol,
        "/checkpoints": ckpt_vol,
    },
    image=gpu_image,
    secrets=[modal.Secret.from_name("huggingface-token")],
    timeout=5 * 3600,
)
def do_train():
    """Run moshi-finetune LoRA training on H100."""
    import subprocess
    import yaml

    config_path = "/tmp/german_moshi.yaml"
    with open(config_path, "w") as f:
        yaml.dump(TRAIN_CONFIG, f, default_flow_style=False)

    import os
    os.makedirs(f"{VOL_CHECKPOINTS}/run_001", exist_ok=True)

    cmd = [
        "torchrun", "--nproc-per-node", "1",
        "-m", "train",
        config_path,
    ]
    print("Starting training:", " ".join(cmd))
    result = subprocess.run(cmd, cwd="/moshi-finetune", capture_output=False)
    if result.returncode != 0:
        raise RuntimeError(f"Training exited with code {result.returncode}")

    ckpt_vol.commit()
    print("✓ Training complete — checkpoint saved to Modal Volume 'moshi-german-checkpoints'")
    print(f"\nDownload checkpoint with:")
    print(f"  modal volume get moshi-german-checkpoints /run_001 ./german_moshi_checkpoint")
    print(f"\nServe with moshi-backend:")
    print(f"  python -m moshi.server \\")
    print(f"    --lora-weight=./german_moshi_checkpoint/checkpoints/checkpoint_XXXXX/consolidated/lora.safetensors \\")
    print(f"    --config-path=./german_moshi_checkpoint/checkpoints/checkpoint_XXXXX/consolidated/config.json")

# ── local entrypoint ──────────────────────────────────────────────────────────

@app.local_entrypoint()
def main():
    parser = argparse.ArgumentParser(description="German Moshi fine-tune pipeline on Modal")
    parser.add_argument("--upload-only",   action="store_true")
    parser.add_argument("--annotate-only", action="store_true")
    parser.add_argument("--train-only",    action="store_true")
    args, _ = parser.parse_known_args()

    only = args.upload_only or args.annotate_only or args.train_only

    if not only or args.upload_only:
        print("\n── Step 1 / 3: Upload data ───────────────────────────────")
        do_upload()

    if not only or args.annotate_only:
        print("\n── Step 2 / 3: Annotate with Whisper (A10G) ─────────────")
        do_annotate.remote()

    if not only or args.train_only:
        print("\n── Step 3 / 3: LoRA training (H100) ─────────────────────")
        do_train.remote()

    print("\n✓ Pipeline complete.")
