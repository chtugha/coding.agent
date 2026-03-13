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
    "onnx2torch": "1.5.15",
}

def _inside_required_env():
    marker = os.sep + os.path.join("envs", REQUIRED_ENV) + os.sep
    return marker in sys.executable


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
      2. ReduceMean opset-18 → opset-17 (axes from input → attribute)
      3. Shape start/end → Shape + Slice (onnx2torch compat)
      4. Strip opset-18 attrs (Reshape allowzero, Split num_outputs)
      5. Fix Clip empty min/max → explicit constants
      6. Set opset version to 17
    """
    import numpy as np
    from onnx import helper, numpy_helper, TensorProto

    graph = model.graph

    seen_names = set()
    for idx, node in enumerate(graph.node):
        if not node.name or node.name in seen_names:
            node.name = f"__auto_{idx}__"
        seen_names.add(node.name)

    inits = {i.name: numpy_helper.to_array(i) for i in graph.initializer}

    # --- Step 1: SplitToSequence + SequenceAt → Gather ---
    seq_sources = {}
    remove_names = set()

    for node in graph.node:
        if node.op_type != "SplitToSequence":
            continue
        axis = 0
        for attr in node.attribute:
            if attr.name == "axis":
                axis = attr.i
        seq_sources[node.output[0]] = (node.input[0], axis)
        remove_names.add(node.name)

    replacements = {}
    scalar_added = {}

    def get_scalar_init(val):
        key = int(val)
        if key not in scalar_added:
            name = f"__scalar_gather_idx_{key}__"
            scalar_added[key] = name
            arr = np.array(key, dtype=np.int64)
            graph.initializer.append(numpy_helper.from_array(arr, name=name))
        return scalar_added[key]

    seqat_count = 0
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
        replacements[node.name] = [gather]
        remove_names.add(node.name)
        seqat_count += 1

    print(f"  Replaced SplitToSequence + {seqat_count} SequenceAt → Gather", flush=True)

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

    # --- Step 3: Shape with start/end attrs (opset 15+) → Shape + Slice (opset 13) ---
    shape_counter = 0
    for node in graph.node:
        if node.op_type != "Shape":
            continue
        if not any(attr.name in ("start", "end") for attr in node.attribute):
            continue

        start_val = 0
        end_val = None
        for attr in node.attribute:
            if attr.name == "start":
                start_val = attr.i
            elif attr.name == "end":
                end_val = attr.i

        if start_val == 0 and end_val is None:
            node.ClearField("attribute")
            shape_counter += 1
            continue

        shape_counter += 1
        full_shape_name = f"__shape_full_{shape_counter}__"
        plain_shape = helper.make_node(
            "Shape", inputs=[node.input[0]], outputs=[full_shape_name],
        )

        starts_name = f"__shape_starts_{shape_counter}__"
        ends_name = f"__shape_ends_{shape_counter}__"
        axes_name_s = f"__shape_axes_{shape_counter}__"
        graph.initializer.append(numpy_helper.from_array(np.array([start_val], dtype=np.int64), starts_name))
        end_arr = np.array([end_val if end_val is not None else 2**63 - 1], dtype=np.int64)
        graph.initializer.append(numpy_helper.from_array(end_arr, ends_name))
        graph.initializer.append(numpy_helper.from_array(np.array([0], dtype=np.int64), axes_name_s))

        slice_node = helper.make_node(
            "Slice",
            inputs=[full_shape_name, starts_name, ends_name, axes_name_s],
            outputs=[node.output[0]],
        )
        replacements[node.name] = [plain_shape, slice_node]
        remove_names.add(node.name)

    print(f"  Fixed {shape_counter} Shape nodes", flush=True)

    # --- Step 4: Strip opset-18+ attributes not valid at opset 17 ---
    STRIP_ATTRS = {
        "Reshape": {"allowzero"},
        "Split": {"num_outputs"},
    }
    strip_counts = {}
    for node in graph.node:
        bad_attrs = STRIP_ATTRS.get(node.op_type)
        if not bad_attrs:
            continue
        attrs_to_keep = [a for a in node.attribute if a.name not in bad_attrs]
        if len(attrs_to_keep) < len(node.attribute):
            node.ClearField("attribute")
            node.attribute.extend(attrs_to_keep)
            strip_counts[node.op_type] = strip_counts.get(node.op_type, 0) + 1
    for op, cnt in strip_counts.items():
        print(f"  Stripped opset-18 attrs from {cnt} {op} nodes", flush=True)

    # --- Step 5: Fix Clip nodes with empty-string min/max inputs ---
    clip_fixed = 0
    clip_inits_added = {}
    def get_clip_const(val, suffix):
        key = (val, suffix)
        if key not in clip_inits_added:
            name = f"__clip_{suffix}__"
            clip_inits_added[key] = name
            graph.initializer.append(numpy_helper.from_array(np.array(val, dtype=np.float32), name))
        return clip_inits_added[key]

    for node in graph.node:
        if node.op_type != "Clip":
            continue
        inputs = list(node.input)
        changed = False
        while len(inputs) < 3:
            inputs.append("")
        if len(inputs) >= 2 and inputs[1] == "":
            inputs[1] = get_clip_const(float("-inf"), "neg_inf")
            changed = True
        if len(inputs) >= 3 and inputs[2] == "":
            inputs[2] = get_clip_const(float("inf"), "pos_inf")
            changed = True
        if changed:
            node.ClearField("input")
            node.input.extend(inputs)
            clip_fixed += 1
    if clip_fixed:
        print(f"  Fixed {clip_fixed} Clip nodes (empty min/max → explicit constants)", flush=True)

    # --- Rebuild node list (in-place replacement to preserve topological order) ---
    new_nodes = []
    for node in graph.node:
        if node.name in remove_names:
            if node.name in replacements:
                new_nodes.extend(replacements[node.name])
        else:
            new_nodes.append(node)
    graph.ClearField("node")
    graph.node.extend(new_nodes)

    # --- Set opset to 17 (minimum for LayerNormalization; ReduceMean axes-as-attribute is still valid) ---
    for op in model.opset_import:
        if op.domain == "":
            op.version = 17

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
        print("  Proceeding — onnx2torch will be the real validation.", flush=True)

    print("[neucodec-coreml] Converting ONNX → PyTorch via onnx2torch...", flush=True)
    torch_model = onnx2torch.convert(model)
    torch_model.eval()

    ENUM_SIZES = [32, 64, 128, 192, 256, 384, 512, 768, 1024, 1500]
    TRACE_SIZE = 256

    print(f"[neucodec-coreml] Tracing PyTorch model (T={TRACE_SIZE})...", flush=True)
    example = torch.zeros(1, 1, TRACE_SIZE, dtype=torch.int32)
    with torch.no_grad():
        traced = torch.jit.trace(torch_model, example, check_trace=False)

    print(f"[neucodec-coreml] Converting traced model → CoreML mlpackage (EnumeratedShapes: {ENUM_SIZES})...", flush=True)
    enumerated = ct.EnumeratedShapes(
        shapes=[(1, 1, s) for s in ENUM_SIZES],
        default=(1, 1, TRACE_SIZE),
    )
    mlmodel = ct.convert(
        traced,
        inputs=[
            ct.TensorType(
                name="codes",
                shape=enumerated,
                dtype=np.int32,
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
