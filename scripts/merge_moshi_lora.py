#!/usr/bin/env python3
"""
merge_moshi_lora.py — Merge a Moshi LoRA adapter into its base model weights.

Usage:
    python3 scripts/merge_moshi_lora.py \\
        --language de \\
        --lora-dir bin/models/german_moshi_lora/run_001/checkpoints/checkpoint_003000/consolidated \\
        --output bin/models/moshiko-de-merged.safetensors

    # Auto-select latest checkpoint in a run directory:
    python3 scripts/merge_moshi_lora.py \\
        --language de \\
        --lora-run-dir bin/models/german_moshi_lora/run_001 \\
        --output bin/models/moshiko-de-merged.safetensors

    # Override base model HuggingFace repo (default: kyutai/moshiko-pytorch-bf16):
    python3 scripts/merge_moshi_lora.py \\
        --language fr \\
        --lora-dir bin/models/french_moshi_lora/run_001/checkpoints/checkpoint_005000/consolidated \\
        --base-repo kyutai/moshika-pytorch-bf16 \\
        --output bin/models/moshiko-fr-merged.safetensors

Merge formula (from moshi/modules/lora.py LoRALinear.merge_weight):
    W_merged = W_base + scaling * (lora_B @ lora_A)

The merged safetensors file is saved with the same key names as the base model,
so the Rust moshi-backend can load it directly via lm_model_file in config.json.
"""

import argparse
import json
import os
import sys
from pathlib import Path


def find_latest_checkpoint(run_dir: Path) -> Path:
    """Return the consolidated dir of the numerically highest checkpoint in run_dir."""
    ckpt_root = run_dir / "checkpoints"
    if not ckpt_root.is_dir():
        print(f"[ERROR] No 'checkpoints' directory found under {run_dir}", file=sys.stderr)
        sys.exit(1)

    candidates = []
    for d in ckpt_root.iterdir():
        if d.is_dir() and d.name.startswith("checkpoint_"):
            try:
                step = int(d.name.split("_", 1)[1])
                consolidated = d / "consolidated"
                lora_file = consolidated / "lora.safetensors"
                if lora_file.is_file():
                    candidates.append((step, consolidated))
            except (ValueError, IndexError):
                continue

    if not candidates:
        print(f"[ERROR] No valid checkpoint directories found under {ckpt_root}", file=sys.stderr)
        sys.exit(1)

    candidates.sort(key=lambda x: x[0], reverse=True)
    step, path = candidates[0]
    print(f"[INFO] Auto-selected checkpoint step={step}: {path}")
    return path


def load_lora_config(lora_dir: Path) -> dict:
    cfg_path = lora_dir / "config.json"
    if not cfg_path.is_file():
        print(f"[WARN] No config.json in {lora_dir} — using defaults (rank=64, scaling=2.0)")
        return {"lora_rank": 64, "lora_scaling": 2.0}
    with open(cfg_path) as f:
        cfg = json.load(f)
    return cfg


def download_base_model(hf_repo: str, cache_dir: Path) -> Path:
    """Download or locate the base model safetensors from HuggingFace."""
    try:
        from huggingface_hub import hf_hub_download, list_repo_files
    except ImportError:
        print("[ERROR] huggingface_hub not installed. Run: pip install huggingface-hub", file=sys.stderr)
        sys.exit(1)

    print(f"[INFO] Resolving base model from HuggingFace repo: {hf_repo}")
    cache_dir.mkdir(parents=True, exist_ok=True)

    try:
        repo_files = list(list_repo_files(hf_repo))
    except Exception as e:
        print(f"[ERROR] Could not list files in {hf_repo}: {e}", file=sys.stderr)
        sys.exit(1)

    safetensor_files = sorted([f for f in repo_files if f.endswith(".safetensors") and "lora" not in f.lower()])
    if not safetensor_files:
        print(f"[ERROR] No safetensors files found in {hf_repo}", file=sys.stderr)
        sys.exit(1)

    if len(safetensor_files) == 1:
        filename = safetensor_files[0]
        print(f"[INFO] Downloading {filename} from {hf_repo}")
        local_path = hf_hub_download(repo_id=hf_repo, filename=filename, cache_dir=str(cache_dir))
        return Path(local_path)

    index_files = [f for f in safetensor_files if "index" in f]
    if index_files:
        shard_files = [f for f in safetensor_files if "index" not in f]
        print(f"[INFO] Sharded model ({len(shard_files)} shards) — downloading all")
        paths = []
        for fn in shard_files:
            print(f"  Downloading shard: {fn}")
            p = hf_hub_download(repo_id=hf_repo, filename=fn, cache_dir=str(cache_dir))
            paths.append(Path(p))
        return paths

    print(f"[INFO] Downloading {safetensor_files[0]} from {hf_repo}")
    local_path = hf_hub_download(repo_id=hf_repo, filename=safetensor_files[0], cache_dir=str(cache_dir))
    return Path(local_path)


def load_base_tensors(base_path) -> dict:
    """Load base model tensors. Accepts a single Path or list of shard Paths."""
    try:
        from safetensors import safe_open
    except ImportError:
        print("[ERROR] safetensors not installed. Run: pip install safetensors", file=sys.stderr)
        sys.exit(1)

    import torch
    tensors = {}

    paths = base_path if isinstance(base_path, list) else [base_path]
    for p in paths:
        print(f"[INFO] Loading base model shard: {p}")
        with safe_open(str(p), framework="pt", device="cpu") as f:
            for key in f.keys():
                tensors[key] = f.get_tensor(key)

    print(f"[INFO] Base model loaded — {len(tensors)} tensors")
    return tensors


def load_lora_tensors(lora_dir: Path) -> dict:
    """Load LoRA adapter tensors from lora.safetensors."""
    try:
        from safetensors import safe_open
    except ImportError:
        print("[ERROR] safetensors not installed. Run: pip install safetensors", file=sys.stderr)
        sys.exit(1)

    lora_path = lora_dir / "lora.safetensors"
    if not lora_path.is_file():
        print(f"[ERROR] lora.safetensors not found at {lora_path}", file=sys.stderr)
        sys.exit(1)

    import torch
    tensors = {}
    with safe_open(str(lora_path), framework="pt", device="cpu") as f:
        for key in f.keys():
            tensors[key] = f.get_tensor(key)

    print(f"[INFO] LoRA adapter loaded — {len(tensors)} tensors ({len(tensors)//2} LoRA pairs)")
    return tensors


def merge_weights(base: dict, lora: dict, rank: int, scaling: float) -> dict:
    """
    Merge LoRA adapters into base weights.

    For each LoRA pair (<prefix>.lora_A.weight, <prefix>.lora_B.weight):
        merged = base[<prefix>.weight] + scaling * (lora_B @ lora_A)

    Keys that have no corresponding LoRA pair are copied unchanged.
    """
    import torch

    lora_a_keys = {k for k in lora if k.endswith(".lora_A.weight")}
    lora_b_keys = {k for k in lora if k.endswith(".lora_B.weight")}
    prefixes = {k[: -len(".lora_A.weight")] for k in lora_a_keys}

    n_merged = 0
    n_skipped = 0
    merged = dict(base)

    for prefix in sorted(prefixes):
        a_key = prefix + ".lora_A.weight"
        b_key = prefix + ".lora_B.weight"

        if a_key not in lora or b_key not in lora:
            print(f"[WARN] Incomplete LoRA pair for prefix {prefix} — skipping", file=sys.stderr)
            n_skipped += 1
            continue

        base_key = prefix + ".weight"
        if base_key not in base:
            print(f"[WARN] Base key not found: {base_key} — skipping LoRA for {prefix}", file=sys.stderr)
            n_skipped += 1
            continue

        lora_A = lora[a_key]
        lora_B = lora[b_key]
        W = base[base_key]

        dtype = W.dtype
        delta = scaling * (lora_B.to(torch.float32) @ lora_A.to(torch.float32))
        merged[base_key] = (W.to(torch.float32) + delta).to(dtype)
        n_merged += 1

    print(f"[INFO] Merged {n_merged} LoRA pairs ({n_skipped} skipped)")
    return merged


def save_merged(tensors: dict, output_path: Path):
    """Save merged tensors to a safetensors file."""
    try:
        from safetensors.torch import save_file
    except ImportError:
        print("[ERROR] safetensors not installed. Run: pip install safetensors", file=sys.stderr)
        sys.exit(1)

    output_path.parent.mkdir(parents=True, exist_ok=True)
    print(f"[INFO] Saving merged model to {output_path} ...")
    save_file(tensors, str(output_path))
    size_mb = output_path.stat().st_size / 1024 / 1024
    print(f"[INFO] Saved {len(tensors)} tensors ({size_mb:.1f} MB)")


def generate_backend_config(output_path: Path, language: str, hf_repo: str, rank: int, scaling: float,
                             mimi_model_file: str, text_tokenizer_file: str):
    """Generate a ready-to-use moshi-backend config.json next to the merged model."""
    config_path = output_path.parent / f"moshi-{language}-backend-config.json"
    config = {
        "instance_name": f"moshi-{language}-backend",
        "hf_repo": hf_repo,
        "lm_model_file": str(output_path.resolve()),
        "text_tokenizer_file": text_tokenizer_file,
        "log_dir": str(output_path.parent.resolve()),
        "mimi_model_file": mimi_model_file,
        "mimi_num_codebooks": 8,
        "addr": "127.0.0.1",
        "port": 8998,
        "cert_dir": "."
    }
    with open(config_path, "w") as f:
        json.dump(config, f, indent=2)
    print(f"[INFO] Backend config written to {config_path}")
    print(f"[INFO]   Edit 'text_tokenizer_file' and 'mimi_model_file' to point to")
    print(f"[INFO]   the downloaded tokenizer and Mimi checkpoint from {hf_repo}")


def main():
    parser = argparse.ArgumentParser(
        description="Merge a Moshi LoRA adapter into base model weights for Rust backend use",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument(
        "--lora-dir",
        type=Path,
        help="Path to the consolidated checkpoint directory containing lora.safetensors and config.json",
    )
    source.add_argument(
        "--lora-run-dir",
        type=Path,
        help="Path to a training run directory — the highest-numbered checkpoint is auto-selected",
    )

    parser.add_argument(
        "--language",
        required=True,
        help="Language code for this LoRA (e.g. 'de', 'en', 'fr'). Used in output filenames.",
    )
    parser.add_argument(
        "--base-repo",
        default="kyutai/moshiko-pytorch-bf16",
        help="HuggingFace repo ID for the base Moshi model (default: kyutai/moshiko-pytorch-bf16)",
    )
    parser.add_argument(
        "--output",
        type=Path,
        help="Output path for the merged safetensors file (default: bin/models/moshiko-<lang>-merged.safetensors)",
    )
    parser.add_argument(
        "--cache-dir",
        type=Path,
        default=Path("bin/models/.hf_cache"),
        help="Local cache directory for HuggingFace downloads (default: bin/models/.hf_cache)",
    )
    parser.add_argument(
        "--scaling",
        type=float,
        default=None,
        help="LoRA scaling factor — overrides value from config.json (default: read from config.json)",
    )
    parser.add_argument(
        "--rank",
        type=int,
        default=None,
        help="LoRA rank — overrides value from config.json (default: read from config.json)",
    )
    parser.add_argument(
        "--mimi-model-file",
        default="hf://kyutai/moshiko-pytorch-bf16/tokenizer-e351c8d8-checkpoint125.safetensors",
        help="Mimi model file path for the generated backend config.json",
    )
    parser.add_argument(
        "--text-tokenizer-file",
        default="hf://kyutai/moshiko-pytorch-bf16/tokenizer_spm_32k_3.model",
        help="Text tokenizer file path for the generated backend config.json",
    )
    parser.add_argument(
        "--no-backend-config",
        action="store_true",
        help="Skip generating moshi-<lang>-backend-config.json next to the merged model",
    )

    args = parser.parse_args()

    try:
        import torch
    except ImportError:
        print("[ERROR] PyTorch not installed. Activate the whispertalk conda env:", file=sys.stderr)
        print("        conda activate whispertalk && pip install torch safetensors huggingface_hub", file=sys.stderr)
        sys.exit(1)

    lora_dir = args.lora_dir if args.lora_dir else find_latest_checkpoint(args.lora_run_dir)

    lora_cfg = load_lora_config(lora_dir)
    rank = args.rank if args.rank is not None else lora_cfg.get("lora_rank", 64)
    scaling = args.scaling if args.scaling is not None else lora_cfg.get("lora_scaling", 2.0)

    print(f"[INFO] LoRA config: rank={rank}, scaling={scaling}")

    output_path = args.output if args.output else Path(f"bin/models/moshiko-{args.language}-merged.safetensors")
    if output_path.is_file():
        print(f"[WARN] Output already exists: {output_path}")
        answer = input("  Overwrite? [y/N] ").strip().lower()
        if answer != "y":
            print("[INFO] Aborted.")
            sys.exit(0)

    base_path = download_base_model(args.base_repo, args.cache_dir)
    base_tensors = load_base_tensors(base_path)
    lora_tensors = load_lora_tensors(lora_dir)

    merged_tensors = merge_weights(base_tensors, lora_tensors, rank=rank, scaling=scaling)

    save_merged(merged_tensors, output_path)

    if not args.no_backend_config:
        generate_backend_config(
            output_path=output_path,
            language=args.language,
            hf_repo=args.base_repo,
            rank=rank,
            scaling=scaling,
            mimi_model_file=args.mimi_model_file,
            text_tokenizer_file=args.text_tokenizer_file,
        )

    print(f"\n[DONE] Merged model ready: {output_path}")
    print(f"       Start the Rust backend with:")
    print(f"         cargo run --features metal --bin moshi-backend -r \\")
    print(f"           -- --config bin/models/moshi-{args.language}-backend-config.json standalone")


if __name__ == "__main__":
    main()
