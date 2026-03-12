#!/usr/bin/env python3
"""
Convert neucodec_decoder.onnx → CoreML .mlpackage via graph surgery.

Graph surgery steps:
  1. Replace SplitToSequence (axis=0,keepdims=0) + SequenceAt (constant index)
     with Gather (axis=0, scalar index) — equivalent to tensor indexing.
  2. Fix ReduceMean opset-18 style (axes as second input) →
     opset-13 style (axes as attribute), since the axes are all constants.
  3. Downgrade model opset_import to 13.
  4. Convert fixed ONNX → PyTorch via onnx2torch → trace → coremltools.

Self-managing: re-executes itself inside a pinned conda env (neucodec_coreml)
that has torch==2.5.0, coremltools==8.3.0, onnx, onnx2torch.
"""

import os
import sys
import subprocess

REQUIRED_ENV = "neucodec_coreml"
REQUIRED_PACKAGES = {
    "torch": "2.5.0",
    "coremltools": "8.3.0",
    "numpy": "1.26.4",
    "onnx": "1.16.1",
    "onnx2torch": None,
}

def _inside_required_env():
    conda_prefix = os.environ.get("CONDA_DEFAULT_ENV", "")
    return conda_prefix == REQUIRED_ENV


def _reexec_in_env():
    """Create the neucodec_coreml conda env if needed, then re-exec this script in it."""
    conda_base = subprocess.check_output(
        ["conda", "info", "--base"], text=True
    ).strip()
    python = os.path.join(conda_base, "envs", REQUIRED_ENV, "bin", "python3")

    if not os.path.exists(python):
        print(f"[neucodec-coreml] Creating conda env '{REQUIRED_ENV}'...", flush=True)
        subprocess.run(
            ["conda", "create", "-n", REQUIRED_ENV, "python=3.11", "-y"],
            check=True,
        )
        pip = os.path.join(conda_base, "envs", REQUIRED_ENV, "bin", "pip")
        pkgs = []
        for pkg, ver in REQUIRED_PACKAGES.items():
            pkgs.append(f"{pkg}=={ver}" if ver else pkg)
        subprocess.run([pip, "install", "--quiet"] + pkgs, check=True)
        print("[neucodec-coreml] Environment ready.", flush=True)

    os.execv(python, [python] + sys.argv)


def fix_onnx_graph(model):
    """
    Apply graph surgery to make the model onnx2torch-compatible:
      1. SplitToSequence + SequenceAt → Gather (with scalar index)
      2. ReduceMean opset-18 → opset-13 (axes from input → attribute)
      3. Set opset version to 13
    """
    import numpy as np
    from onnx import helper, numpy_helper, TensorProto

    graph = model.graph

    inits = {i.name: numpy_helper.to_array(i) for i in graph.initializer}

    # --- Step 1: SplitToSequence + SequenceAt → Gather ---
    seq_sources = {}
    sts_ids = set()

    for node in graph.node:
        if node.op_type != "SplitToSequence":
            continue
        axis = 0
        for attr in node.attribute:
            if attr.name == "axis":
                axis = attr.i
        seq_sources[node.output[0]] = (node.input[0], axis)
        sts_ids.add(id(node))

    seqat_ids = set()
    gather_nodes = []
    scalar_added = {}

    def get_scalar_init(val):
        key = int(val)
        if key not in scalar_added:
            name = f"__scalar_gather_idx_{key}__"
            scalar_added[key] = name
            arr = np.array(key, dtype=np.int64)
            graph.initializer.append(numpy_helper.from_array(arr, name=name))
        return scalar_added[key]

    for node in graph.node:
        if node.op_type != "SequenceAt":
            continue
        seq_name = node.input[0]
        idx_name = node.input[1]
        if seq_name not in seq_sources:
            print(f"  WARNING: SequenceAt input '{seq_name}' has no matching SplitToSequence — skipping", file=sys.stderr)
            continue
        if idx_name not in inits:
            print(f"  WARNING: SequenceAt index '{idx_name}' is not a constant initializer — skipping", file=sys.stderr)
            continue

        idx_val = int(inits[idx_name].flat[0])
        scalar_name = get_scalar_init(idx_val)
        data_name, axis = seq_sources[seq_name]

        gather = helper.make_node(
            "Gather",
            inputs=[data_name, scalar_name],
            outputs=[node.output[0]],
            axis=axis,
        )
        gather_nodes.append(gather)
        seqat_ids.add(id(node))

    print(f"  Replaced {len(sts_ids)} SplitToSequence + {len(seqat_ids)} SequenceAt → Gather", flush=True)

    # --- Step 2: ReduceMean opset-18 → opset-13 (axes from input to attribute) ---
    rm_fixed = 0
    for node in graph.node:
        if node.op_type != "ReduceMean":
            continue
        if len(node.input) < 2 or not node.input[1]:
            continue
        axes_name = node.input[1]
        if axes_name not in inits:
            print(f"  WARNING: ReduceMean axes '{axes_name}' is not constant — cannot convert", file=sys.stderr)
            continue

        axes = [int(a) for a in inits[axes_name].flat]
        keepdims = 1
        for attr in node.attribute:
            if attr.name == "keepdims":
                keepdims = attr.i

        data_input = node.input[0]
        node.ClearField("input")
        node.input.append(data_input)
        node.ClearField("attribute")
        node.attribute.append(helper.make_attribute("axes", axes))
        node.attribute.append(helper.make_attribute("keepdims", keepdims))
        rm_fixed += 1

    print(f"  Fixed {rm_fixed} ReduceMean nodes (axes input → attribute)", flush=True)

    # --- Rebuild node list (remove SplitToSequence and SequenceAt) ---
    surviving = [n for n in graph.node if id(n) not in sts_ids and id(n) not in seqat_ids]
    surviving.extend(gather_nodes)
    graph.ClearField("node")
    graph.node.extend(surviving)

    # --- Set opset to 13 ---
    for op in model.opset_import:
        if op.domain == "":
            op.version = 13

    return model


def main():
    import argparse
    parser = argparse.ArgumentParser(description="Convert NeuCodec ONNX → CoreML mlpackage")
    parser.add_argument("onnx_path", help="Path to neucodec_decoder.onnx")
    parser.add_argument("output_dir", help="Directory to write neucodec_decoder.mlpackage into")
    args = parser.parse_args()

    onnx_path = args.onnx_path
    output_dir = args.output_dir
    os.makedirs(output_dir, exist_ok=True)
    mlpackage_path = os.path.join(output_dir, "neucodec_decoder.mlpackage")

    if os.path.exists(mlpackage_path):
        print(f"[neucodec-coreml] mlpackage already exists at {mlpackage_path} — skipping conversion")
        return 0

    import onnx
    import onnx2torch
    import coremltools as ct
    import numpy as np
    import torch

    print(f"[neucodec-coreml] Loading ONNX: {onnx_path}", flush=True)
    model = onnx.load(onnx_path)
    opset = next((op.version for op in model.opset_import if op.domain == ""), "?")
    print(f"  opset={opset}, nodes={len(model.graph.node)}", flush=True)

    print("[neucodec-coreml] Applying graph surgery...", flush=True)
    model = fix_onnx_graph(model)

    print("[neucodec-coreml] Validating surgered ONNX...", flush=True)
    try:
        onnx.checker.check_model(model)
        print("  Validation OK", flush=True)
    except Exception as e:
        print(f"  Validation WARNING (non-fatal): {e}", file=sys.stderr)

    print("[neucodec-coreml] Converting ONNX → PyTorch via onnx2torch...", flush=True)
    torch_model = onnx2torch.convert(model)
    torch_model.eval()

    print("[neucodec-coreml] Tracing PyTorch model (T=256)...", flush=True)
    example = torch.zeros(1, 1, 256, dtype=torch.int64)
    with torch.no_grad():
        traced = torch.jit.trace(torch_model, example, check_trace=False)

    print("[neucodec-coreml] Converting traced model → CoreML mlpackage...", flush=True)
    mlmodel = ct.convert(
        traced,
        inputs=[
            ct.TensorType(
                name="codes",
                shape=(1, 1, ct.RangeDim(min_size=1, max_size=2048)),
                dtype=np.int64,
            )
        ],
        outputs=[ct.TensorType(name="audio", dtype=np.float32)],
        compute_units=ct.ComputeUnit.ALL,
        minimum_deployment_target=ct.target.macOS13,
    )

    mlmodel.save(mlpackage_path)
    print(f"[neucodec-coreml] Saved: {mlpackage_path}", flush=True)
    return 0


if __name__ == "__main__":
    if not _inside_required_env():
        _reexec_in_env()
    sys.exit(main())
