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

Key mapping: The Moshi training code decomposes attention projections
(in_projs.0..N, out_projs.0..N) but the base HF checkpoint stores them
concatenated (in_proj_weight, out_proj.weight). This script handles the
decomposition automatically during merge.

Memory-efficient: Uses numpy + safetensors mmap to merge on machines with
limited RAM (tested on 16GB with a 14.6GB bf16 base model). Processes one
tensor at a time and writes the output incrementally.

The merged safetensors file is saved with the same key names as the base model,
so the Rust moshi-backend can load it directly via lm_model_file in config.json.
"""

import argparse
import gc
import json
import os
import re
import struct
import sys
from pathlib import Path

import numpy as np


DTYPE_MAP = {
    "F16": ("float16", 2),
    "BF16": ("bfloat16", 2),
    "F32": ("float32", 4),
    "F64": ("float64", 8),
    "I8": ("int8", 1),
    "I16": ("int16", 2),
    "I32": ("int32", 4),
    "I64": ("int64", 8),
    "U8": ("uint8", 1),
    "U16": ("uint16", 2),
    "U32": ("uint32", 4),
    "U64": ("uint64", 8),
    "BOOL": ("bool", 1),
}

NP_TO_ST_DTYPE = {
    "float16": "F16",
    "bfloat16": "BF16",
    "float32": "F32",
    "float64": "F64",
    "int8": "I8",
    "int16": "I16",
    "int32": "I32",
    "int64": "I64",
    "uint8": "U8",
    "uint16": "U16",
    "uint32": "U32",
    "uint64": "U64",
    "bool": "BOOL",
}


def parse_safetensors_header(path: Path) -> tuple:
    with open(path, "rb") as f:
        header_len = struct.unpack("<Q", f.read(8))[0]
        header_json = f.read(header_len)
    header = json.loads(header_json)
    data_offset = 8 + header_len
    return header, data_offset


def load_tensor_numpy(path: Path, data_offset: int, meta: dict) -> np.ndarray:
    dtype_str, elem_size = DTYPE_MAP[meta["dtype"]]
    shape = meta["shape"]
    begin, end = meta["data_offsets"]
    n_bytes = end - begin

    if dtype_str == "bfloat16":
        raw = np.memmap(path, dtype=np.uint16, mode="r",
                        offset=data_offset + begin, shape=(n_bytes // 2,))
        return np.array(raw)

    np_dtype = np.dtype(dtype_str)
    n_elems = 1
    for s in shape:
        n_elems *= s
    raw = np.memmap(path, dtype=np_dtype, mode="r",
                    offset=data_offset + begin, shape=tuple(shape))
    return np.array(raw)


def bf16_to_f32(arr: np.ndarray) -> np.ndarray:
    return np.frombuffer(
        np.stack([np.zeros_like(arr), arr], axis=-1).tobytes(),
        dtype=np.float32
    ).reshape(arr.shape if len(arr.shape) > 0 else (arr.size,))


def f32_to_bf16(arr: np.ndarray) -> np.ndarray:
    return np.frombuffer(arr.tobytes(), dtype=np.uint16).reshape(
        list(arr.shape) + [2]
    )[..., 1].copy()


def find_latest_checkpoint(run_dir: Path) -> Path:
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


def get_base_paths(base_path) -> list:
    return base_path if isinstance(base_path, list) else [base_path]


def load_lora_numpy(lora_dir: Path) -> dict:
    lora_path = lora_dir / "lora.safetensors"
    if not lora_path.is_file():
        print(f"[ERROR] lora.safetensors not found at {lora_path}", file=sys.stderr)
        sys.exit(1)

    header, data_offset = parse_safetensors_header(lora_path)
    tensors = {}
    for key, meta in header.items():
        if key == "__metadata__":
            continue
        arr = load_tensor_numpy(lora_path, data_offset, meta)
        is_bf16 = meta["dtype"] == "BF16"
        if is_bf16:
            tensors[key] = bf16_to_f32(arr)
        else:
            dtype_str = DTYPE_MAP[meta["dtype"]][0]
            if dtype_str == "float16":
                tensors[key] = arr.astype(np.float32)
            else:
                tensors[key] = arr if dtype_str == "float32" else arr.astype(np.float32)
        tensors[key] = tensors[key].reshape(meta["shape"])

    print(f"[INFO] LoRA adapter loaded — {len(tensors)} tensors ({len(tensors)//2} LoRA pairs)")
    return tensors


def build_lora_mapping(lora: dict, base_keys: set) -> dict:
    prefixes = {k[: -len(".lora_A.weight")] for k in lora if k.endswith(".lora_A.weight")}

    mapping = {}
    n_direct = 0
    n_decomposed = 0
    n_skipped = 0

    for prefix in sorted(prefixes):
        a_key = prefix + ".lora_A.weight"
        b_key = prefix + ".lora_B.weight"
        if a_key not in lora or b_key not in lora:
            print(f"[WARN] Incomplete LoRA pair for prefix {prefix} — skipping", file=sys.stderr)
            n_skipped += 1
            continue

        base_key = prefix + ".weight"
        if base_key in base_keys:
            mapping.setdefault(base_key, []).append((0, a_key, b_key))
            n_direct += 1
            continue

        m = re.match(r'^(.*\.self_attn)\.(in_projs|out_projs)\.(\d+)$', prefix)
        if m:
            attn_prefix, proj_type, idx_str = m.group(1), m.group(2), int(m.group(3))
            if proj_type == "in_projs":
                concat_key = attn_prefix + ".in_proj_weight"
            else:
                concat_key = attn_prefix + ".out_proj.weight"

            if concat_key in base_keys:
                mapping.setdefault(concat_key, []).append((idx_str, a_key, b_key))
                n_decomposed += 1
                continue

        print(f"[WARN] No base key for LoRA prefix {prefix} — skipping", file=sys.stderr)
        n_skipped += 1

    for k in mapping:
        mapping[k].sort(key=lambda x: x[0])

    print(f"[INFO] LoRA mapping: {n_direct} direct, {n_decomposed} decomposed (concat), {n_skipped} skipped")
    return mapping


def merge_one_tensor(W_raw: np.ndarray, shape: list, dtype_name: str,
                     entries: list, lora: dict, scaling: float) -> bytes:
    is_bf16 = dtype_name == "BF16"
    n_chunks = len(entries)

    if is_bf16:
        W_f32 = bf16_to_f32(W_raw).reshape(shape).copy()
    elif DTYPE_MAP[dtype_name][0] == "float16":
        W_f32 = W_raw.astype(np.float32).reshape(shape)
    else:
        W_f32 = W_raw.reshape(shape).astype(np.float32, copy=True)
    del W_raw

    if n_chunks == 1 and entries[0][0] == 0:
        _, a_key, b_key = entries[0]
        delta = scaling * (lora[b_key] @ lora[a_key])
        W_f32 += delta
        del delta
    else:
        chunk_size = W_f32.shape[0] // n_chunks
        for idx, a_key, b_key in entries:
            delta = scaling * (lora[b_key] @ lora[a_key])
            start = idx * chunk_size
            W_f32[start:start + chunk_size] += delta
            del delta

    if is_bf16:
        result = f32_to_bf16(W_f32)
    elif DTYPE_MAP[dtype_name][0] == "float16":
        result = W_f32.astype(np.float16)
    else:
        result = W_f32
    del W_f32

    out = result.tobytes()
    del result
    return out


def write_safetensors_streaming(output_path: Path, base_paths: list, lora: dict,
                                 lora_mapping: dict, scaling: float):
    base_headers = []
    for p in base_paths:
        hdr, off = parse_safetensors_header(p)
        base_headers.append((p, hdr, off))

    all_keys = []
    key_source = {}
    for p, hdr, off in base_headers:
        for key, meta in hdr.items():
            if key == "__metadata__":
                continue
            all_keys.append(key)
            key_source[key] = (p, meta, off)

    out_header = {}
    current_offset = 0
    n_merged = 0
    n_copied = 0

    for key in all_keys:
        _, meta, _ = key_source[key]
        begin, end = meta["data_offsets"]
        n_bytes = end - begin
        out_header[key] = {
            "dtype": meta["dtype"],
            "shape": meta["shape"],
            "data_offsets": [current_offset, current_offset + n_bytes],
        }
        current_offset += n_bytes

    header_json = json.dumps(out_header, separators=(",", ":")).encode("utf-8")
    pad = (8 - len(header_json) % 8) % 8
    header_json += b" " * pad

    total_data = current_offset
    print(f"[INFO] Writing {output_path} ({total_data / 1024 / 1024:.1f} MB data, {len(all_keys)} tensors)...")

    temp_path = output_path.with_suffix(".tmp")
    output_path.parent.mkdir(parents=True, exist_ok=True)

    with open(temp_path, "wb") as f:
        f.write(struct.pack("<Q", len(header_json)))
        f.write(header_json)

        for ki, key in enumerate(all_keys):
            p, meta, data_offset = key_source[key]
            begin, end = meta["data_offsets"]
            n_bytes = end - begin

            if key not in lora_mapping:
                with open(p, "rb") as src:
                    src.seek(data_offset + begin)
                    remaining = n_bytes
                    while remaining > 0:
                        chunk = src.read(min(remaining, 8 * 1024 * 1024))
                        f.write(chunk)
                        remaining -= len(chunk)
                n_copied += 1
            else:
                W_raw = load_tensor_numpy(p, data_offset, meta)
                merged_bytes = merge_one_tensor(
                    W_raw, meta["shape"], meta["dtype"],
                    lora_mapping[key], lora, scaling)
                f.write(merged_bytes)
                del W_raw, merged_bytes
                n_merged += 1
                gc.collect()

            if (ki + 1) % 50 == 0 or ki == len(all_keys) - 1:
                print(f"[INFO]   {ki+1}/{len(all_keys)} tensors written ({n_merged} merged, {n_copied} copied)")

    temp_path.rename(output_path)
    size_mb = output_path.stat().st_size / 1024 / 1024
    print(f"[INFO] Saved {len(all_keys)} tensors ({size_mb:.1f} MB)")


def generate_backend_config(output_path: Path, language: str, hf_repo: str, rank: int, scaling: float,
                             mimi_model_file: str, text_tokenizer_file: str):
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
    base_paths = get_base_paths(base_path)

    lora_tensors = load_lora_numpy(lora_dir)

    base_keys = set()
    for p in base_paths:
        hdr, _ = parse_safetensors_header(p)
        base_keys.update(k for k in hdr if k != "__metadata__")
    print(f"[INFO] Base model has {len(base_keys)} tensors")

    lora_mapping = build_lora_mapping(lora_tensors, base_keys)

    write_safetensors_streaming(output_path, base_paths, lora_tensors, lora_mapping, scaling=scaling)

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
