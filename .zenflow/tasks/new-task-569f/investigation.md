# Investigation: `ane_transformers` missing during CoreML conversion

## Bug Summary

The `runmetoinstalleverythingfirst` script fails at the "Generating Whisper CoreML encoders" step with:

```
ModuleNotFoundError: No module named 'ane_transformers'
```

The error occurs when `whisper-cpp/models/convert-whisper-to-coreml.py` is invoked (line 273 of the install script). That Python script imports `ane_transformers.reference.layer_norm.LayerNormANE` (line 10 of the conversion script).

## Root Cause Analysis

The `setup_python_env()` function (lines 149-177) installs these pip packages into the `whispertalk` conda environment:

- protobuf
- coremltools
- openai-whisper
- huggingface-hub
- onnx

However, `whisper-cpp/models/requirements-coreml.txt` lists **four** required packages:

- torch
- coremltools
- openai-whisper
- **ane_transformers**

The `ane-transformers` package (Apple's Neural Engine Transformers, pip name `ane-transformers`, import name `ane_transformers`) is **not installed** by the script.

## Affected Components

- **File**: `runmetoinstalleverythingfirst`
- **Function**: `setup_python_env()` (lines 149-177) — missing `ane-transformers` from pip installs
- **Downstream**: `generate_whisper_coreml_encoder()` (line 273) — calls `convert-whisper-to-coreml.py` which requires `ane_transformers`

## Proposed Solution

Add `ane-transformers` to the pip install commands in `setup_python_env()`, after the existing installs (e.g., after line 175):

```bash
"$WHISPERTALK_PYTHON" -m pip install --quiet ane-transformers
```

This is the only change needed. The package is available on PyPI (`ane-transformers==0.1.3`, published by Apple Inc.) and is the standard dependency listed in whisper.cpp's own `requirements-coreml.txt`.

## Implementation Notes

### Changes Made

1. **Added `ane-transformers` install** (line 176 of `runmetoinstalleverythingfirst`):
   Added `"$WHISPERTALK_PYTHON" -m pip install --quiet ane-transformers` to `setup_python_env()` after the `onnx` install.

2. **Fixed `python3` → `$WHISPERTALK_PYTHON`** in `generate_whisper_coreml_encoder()` (line 274):
   The CoreML conversion was invoked via bare `python3` inside a subshell. While `activate_conda_env` sets PATH, if it falls back to the manual PATH setup, the subshell may not inherit it. Using `$WHISPERTALK_PYTHON` (the absolute path) is consistent with how every other Python invocation in the script works and avoids this edge case.

### Dependency Verification

- **`torch`**: Not explicitly installed, but is a transitive dependency of `openai-whisper` (confirmed via `pip show openai-whisper` — Requires: `torch`, `numpy`, `tqdm`, etc.). No action needed.

### Known Risk

- The error output includes a warning: `Torch version 2.10.0 has not been tested with coremltools`. This is a compatibility warning from coremltools, not a blocking error. If CoreML conversion fails after this fix, pinning torch to `<=2.7.0` (the latest tested version per the warning) in the pip installs may be required.
