#!/usr/bin/env python3
"""Convert a safetensors model to GGUF Q8_0 format for candle.

Processes tensors one at a time via mmap to avoid loading the full model.
Weight matrices (2D+) are quantized to Q8_0; normalization/bias tensors (1D)
and small tensors are kept as F32.

Handles BF16 safetensors by converting to F32 via uint16 bit-shift (no PyTorch needed).
"""

import argparse
import json
import mmap
import struct
import numpy as np
from pathlib import Path
from gguf import GGUFWriter, GGMLQuantizationType


Q8_BLOCK_SIZE = 32
Q8_BYTES_PER_BLOCK = 34


def bf16_to_f32(raw_bytes: bytes, numel: int) -> np.ndarray:
    u16 = np.frombuffer(raw_bytes, dtype=np.uint16)[:numel]
    u32 = u16.astype(np.uint32) << 16
    return u32.view(np.float32)


def quantize_q8_0_rows(data_f32: np.ndarray) -> tuple:
    shape = list(data_f32.shape)
    last_dim = shape[-1]
    pad = (Q8_BLOCK_SIZE - last_dim % Q8_BLOCK_SIZE) % Q8_BLOCK_SIZE
    padded_last = last_dim + pad
    blocks_per_row = padded_last // Q8_BLOCK_SIZE
    bytes_per_row = blocks_per_row * Q8_BYTES_PER_BLOCK

    rows = data_f32.reshape(-1, last_dim).astype(np.float32)
    n_rows = rows.shape[0]

    if pad:
        rows = np.concatenate([rows, np.zeros((n_rows, pad), dtype=np.float32)], axis=1)

    rows = rows.reshape(n_rows * blocks_per_row, Q8_BLOCK_SIZE)
    abs_max = np.max(np.abs(rows), axis=1)
    scales = np.where(abs_max == 0, 1.0, abs_max / 127.0).astype(np.float32)
    inv_scales = np.where(abs_max == 0, 0.0, 127.0 / abs_max)
    quants = np.clip(np.round(rows * inv_scales[:, None]), -128, 127).astype(np.int8)
    scales_f16 = scales.astype(np.float16)

    n_blocks = n_rows * blocks_per_row
    result = np.empty(n_blocks * Q8_BYTES_PER_BLOCK, dtype=np.uint8)
    for i in range(n_blocks):
        offset = i * Q8_BYTES_PER_BLOCK
        result[offset:offset+2] = np.frombuffer(scales_f16[i:i+1].tobytes(), dtype=np.uint8)
        result[offset+2:offset+Q8_BYTES_PER_BLOCK] = quants[i].view(np.uint8)

    byte_shape = shape[:-1] + [bytes_per_row]
    result = result.reshape(byte_shape)
    return result, byte_shape


def read_safetensors_header(path: str):
    with open(path, "rb") as f:
        header_size = struct.unpack("<Q", f.read(8))[0]
        header_json = f.read(header_size).decode("utf-8")
    header = json.loads(header_json)
    data_offset = 8 + header_size
    header.pop("__metadata__", None)
    return header, data_offset


def load_tensor_f32(mm, data_offset: int, info: dict) -> tuple:
    dtype_str = info["dtype"]
    shape = info["shape"]
    offsets = info["data_offsets"]
    start = data_offset + offsets[0]
    end = data_offset + offsets[1]
    raw = mm[start:end]
    numel = 1
    for s in shape:
        numel *= s

    if dtype_str == "BF16":
        arr = bf16_to_f32(raw, numel)
    elif dtype_str == "F16":
        arr = np.frombuffer(raw, dtype=np.float16)[:numel].astype(np.float32)
    elif dtype_str == "F32":
        arr = np.frombuffer(raw, dtype=np.float32)[:numel].copy()
    else:
        arr = np.frombuffer(raw, dtype=np.float32)[:numel].copy()

    return arr.reshape(shape), shape


def should_quantize(name: str, shape: list) -> bool:
    if len(shape) < 2:
        return False
    numel = 1
    for s in shape:
        numel *= s
    if numel < Q8_BLOCK_SIZE:
        return False
    skip_patterns = ['alpha', 'bias', 'norm', 'embed']
    name_lower = name.lower()
    for pat in skip_patterns:
        if pat in name_lower:
            return False
    return True


def convert(input_path: str, output_path: str):
    print(f"Opening safetensors: {input_path}")
    header, data_offset = read_safetensors_header(input_path)
    tensor_names = sorted(header.keys())
    n_tensors = len(tensor_names)
    print(f"Found {n_tensors} tensors, data_offset={data_offset}")

    f = open(input_path, "rb")
    mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)

    writer = GGUFWriter(output_path, arch="")

    quantized_count = 0
    kept_f32_count = 0

    for i, name in enumerate(tensor_names):
        info = header[name]
        arr, shape = load_tensor_f32(mm, data_offset, info)

        if should_quantize(name, shape):
            q_data, byte_shape = quantize_q8_0_rows(arr)
            writer.add_tensor(name, q_data, raw_shape=byte_shape, raw_dtype=GGMLQuantizationType.Q8_0)
            quantized_count += 1
            dtype_str = "Q8_0"
        else:
            writer.add_tensor(name, arr, raw_dtype=GGMLQuantizationType.F32)
            kept_f32_count += 1
            dtype_str = "F32"

        if (i + 1) % 50 == 0 or i == n_tensors - 1:
            print(f"  [{i+1}/{n_tensors}] {name}: {shape} -> {dtype_str}")

    mm.close()
    f.close()

    print(f"\nWriting GGUF file...")
    print(f"  Quantized: {quantized_count} tensors to Q8_0")
    print(f"  Kept F32:  {kept_f32_count} tensors")
    writer.write_header_to_file()
    writer.write_kv_data_to_file()
    writer.write_tensors_to_file()
    writer.close()

    out_size = Path(output_path).stat().st_size / (1024**3)
    print(f"Output: {output_path} ({out_size:.2f} GB)")


def main():
    parser = argparse.ArgumentParser(description="Convert safetensors to GGUF Q8_0")
    parser.add_argument("--input", required=True, help="Path to input safetensors file")
    parser.add_argument("--output", required=True, help="Path to output GGUF file")
    args = parser.parse_args()

    convert(args.input, args.output)


if __name__ == "__main__":
    main()
