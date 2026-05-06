#!/usr/bin/env python3
"""
Export DeepPhonemizer German G2P model to CoreML for the Prodigy C++ pipeline.

This script downloads the DeepPhonemizer German checkpoint from HuggingFace
and exports it to a CoreML .mlmodelc bundle for use by kokoro-service,
vits2-service, and matcha-service.

Exported artifacts (in bin/models/g2p/ or --output-dir):
  - de_g2p.mlmodelc       CoreML G2P model (character indices -> phoneme indices)
  - char_vocab.json       Character-to-integer input encoding
  - phoneme_vocab.json    Integer-to-IPA phoneme decoding

Prerequisites:
  - macOS with Apple Silicon
  - conda (miniconda or miniforge)
  - Internet access (for model download if --checkpoint not provided)

Usage:
  python3 scripts/export_g2p_model.py
  python3 scripts/export_g2p_model.py --output-dir /tmp/g2p_test
  python3 scripts/export_g2p_model.py --checkpoint /path/to/de_g2p.pt
  python3 scripts/export_g2p_model.py --no-install --output-dir /tmp/g2p_test

Required conda environment packages:
  torch, coremltools, deep-phonemizer
  (pip install torch coremltools deep-phonemizer)
"""
import os
import sys
import json
import time
import shutil
import subprocess
import argparse

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.realpath(os.path.join(SCRIPT_DIR, '..'))

CONDA_ENV_NAME = 'kokoro_coreml'
REQUIRED_TORCH = '2.5'
REQUIRED_COREMLTOOLS = '8.3'

HF_REPO_ID = 'as-ideas/DeepPhonemizer'
HF_REPO_ID_MIRROR = 'mrfakename/deep-phonemizer'
HF_MODEL_FILENAME = 'en_us_cmudict_ipa_forward.pt'
HF_MODEL_FILENAME_MIRROR = 'en_us_cmudict_ipa.pt'
HF_DE_MODEL_FILENAME = 'de_forward_en_ipa.pt'

S3_BASE_URL = 'https://public-asai-dl-models.s3.eu-central-1.amazonaws.com/DeepPhonemizer'
S3_LATIN_IPA = f'{S3_BASE_URL}/latin_ipa_forward.pt'
S3_EN_US_IPA = f'{S3_BASE_URL}/en_us_cmudict_ipa_forward.pt'

MAX_CHAR_LEN = 128

_FORWARD_STYLES = [
    ('positional',        lambda m, x, l: m(x, l)),
    ('x_only',            lambda m, x, l: m(x)),
    ('keyword_lengths',   lambda m, x, l: m(x, lengths=l)),
    ('attention_mask',    lambda m, x, l: m(input_ids=x, attention_mask=(x != 0).long())),
    ('dict_text',         lambda m, x, l: m({'text': x})),
]


def run_cmd(cmd, check=True):
    print(f"  $ {cmd}")
    result = subprocess.run(cmd, shell=True, capture_output=True, text=True)
    if check and result.returncode != 0:
        print(f"  FAILED: {result.stderr}")
        sys.exit(1)
    return result


def ensure_conda_env():
    print("\n=== Checking conda environment ===")
    result = run_cmd(f"conda env list | grep {CONDA_ENV_NAME}", check=False)
    if CONDA_ENV_NAME not in result.stdout:
        print(f"  Creating conda env '{CONDA_ENV_NAME}' with Python 3.11...")
        run_cmd(f"conda create -n {CONDA_ENV_NAME} python=3.11 -y")

    conda_prefix = run_cmd(
        f"conda run -n {CONDA_ENV_NAME} python -c \"import sys; print(sys.prefix)\"",
        check=True
    ).stdout.strip()
    conda_python = os.path.join(conda_prefix, 'bin', 'python')

    print(f"  Installing dependencies in {CONDA_ENV_NAME}...")
    run_cmd(f"{conda_python} -m pip install -q torch=={REQUIRED_TORCH}.0 --index-url https://download.pytorch.org/whl/cpu")
    run_cmd(f"{conda_python} -m pip install -q coremltools=={REQUIRED_COREMLTOOLS}.0 numpy==1.26.4")
    run_cmd(f"{conda_python} -m pip install -q deep-phonemizer huggingface_hub")

    version_check = run_cmd(
        f'{conda_python} -c "import torch, coremltools as ct, numpy as np; '
        f'print(f\'torch={{torch.__version__}} ct={{ct.__version__}} np={{np.__version__}}\')"'
    )
    print(f"  Versions: {version_check.stdout.strip()}")
    return conda_python


def _ensure_huggingface_hub():
    try:
        import huggingface_hub
        return huggingface_hub
    except ImportError:
        print("  Installing huggingface_hub...")
        subprocess.run([sys.executable, '-m', 'pip', 'install', '-q', 'huggingface_hub'],
                       check=True)
        import huggingface_hub
        return huggingface_hub


def _download_url(url, dest, timeout=60):
    import urllib.request
    import socket
    print(f"  Downloading {url} ...")
    old_timeout = socket.getdefaulttimeout()
    socket.setdefaulttimeout(timeout)
    try:
        urllib.request.urlretrieve(url, dest)
    finally:
        socket.setdefaulttimeout(old_timeout)
    size_mb = os.path.getsize(dest) / 1e6
    print(f"  OK ({size_mb:.1f} MB) -> {dest}")


def download_checkpoint(output_dir):
    print("\n=== Downloading DeepPhonemizer German checkpoint ===")
    os.makedirs(output_dir, exist_ok=True)

    local_path = os.path.join(output_dir, HF_DE_MODEL_FILENAME)
    if os.path.isfile(local_path) and os.path.getsize(local_path) > 100_000:
        print(f"  Already exists: {local_path}")
        return local_path, False

    print("  Trying latin_ipa_forward.pt from AWS S3 (supports de, en_us, en_uk, fr, es)...")
    try:
        _download_url(S3_LATIN_IPA, local_path)
        print("  Multilingual checkpoint (incl. German) downloaded successfully.")
        return local_path, False
    except Exception as e:
        print(f"  S3 download failed: {e}")

    hf = _ensure_huggingface_hub()
    hf_token = os.environ.get('HF_TOKEN', '')
    token = hf_token if hf_token else None

    hf_candidates = [
        (HF_REPO_ID, HF_DE_MODEL_FILENAME, False),
        (HF_REPO_ID_MIRROR, HF_MODEL_FILENAME_MIRROR, True),
    ]
    for repo_id, filename, is_english_fallback in hf_candidates:
        print(f"  Trying {filename} from {repo_id}...")
        try:
            downloaded = hf.hf_hub_download(
                repo_id=repo_id, filename=filename, token=token,
            )
            shutil.copy2(downloaded, local_path)
            if is_english_fallback:
                print()
                print("  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!")
                print("  WARNING: Falling back to ENGLISH-only model.")
                print("  The exported de_g2p.mlmodelc will phonemize text as ENGLISH,")
                print("  not German. Use --checkpoint with a German checkpoint for")
                print("  correct German G2P.")
                print("  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!")
                print()
            print(f"  OK ({os.path.getsize(local_path) / 1e6:.1f} MB) -> {local_path}")
            return local_path, is_english_fallback
        except Exception as e:
            print(f"  Failed to download {filename} from {repo_id}: {e}")

    print("  Trying en_us_cmudict_ipa_forward.pt from AWS S3 (English-only fallback)...")
    try:
        _download_url(S3_EN_US_IPA, local_path)
        print()
        print("  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!")
        print("  WARNING: Falling back to ENGLISH-only model from S3.")
        print("  The exported de_g2p.mlmodelc will phonemize text as ENGLISH,")
        print("  not German.")
        print("  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!")
        print()
        return local_path, True
    except Exception as e:
        print(f"  S3 English fallback failed: {e}")

    print("\n  ERROR: Could not download any DeepPhonemizer checkpoint.")
    print(f"  Kokoro and Matcha will fall back to espeak-ng for phonemization.")
    return None, False


def load_phonemizer(checkpoint_path):
    print(f"\n=== Loading DeepPhonemizer from {checkpoint_path} ===")
    try:
        from dp.phonemizer import Phonemizer
        phonemizer = Phonemizer.from_checkpoint(checkpoint_path)
        lang_keys = list(phonemizer.lang_phoneme_dict.keys()) if hasattr(phonemizer, 'lang_phoneme_dict') else 'N/A'
        print(f"  Loaded Phonemizer, lang codes: {lang_keys}")
        return phonemizer
    except Exception as e:
        print(f"  Failed to load via dp.phonemizer.Phonemizer: {e}")
        print("  Attempting direct checkpoint load...")

    import torch
    ckpt = torch.load(checkpoint_path, map_location='cpu', weights_only=False)
    print(f"  Checkpoint keys: {list(ckpt.keys()) if isinstance(ckpt, dict) else type(ckpt)}")

    if isinstance(ckpt, dict) and 'model' not in ckpt:
        print("  WARNING: Checkpoint dict has no 'model' key — model extraction may fail.")
        print(f"  Available keys: {list(ckpt.keys())}")
    return ckpt


def _build_vocabs_from_phonemizer(phonemizer):
    char_vocab = {}
    phoneme_vocab = []

    if hasattr(phonemizer, 'preprocessor'):
        prep = phonemizer.preprocessor
        if hasattr(prep, 'text_tokenizer'):
            tok = prep.text_tokenizer
            vocab_dict = None
            for attr in ('vocab', 'token_to_idx', 'char_to_idx', '_token_to_idx'):
                if hasattr(tok, attr):
                    vocab_dict = getattr(tok, attr)
                    if isinstance(vocab_dict, dict) and len(vocab_dict) > 0:
                        break
                    vocab_dict = None
            if vocab_dict:
                char_vocab = {str(k): int(v) for k, v in vocab_dict.items()}
        if hasattr(prep, 'phoneme_tokenizer'):
            ptok = prep.phoneme_tokenizer
            pvocab_dict = None
            for attr in ('vocab', 'token_to_idx', '_token_to_idx'):
                if hasattr(ptok, attr):
                    pvocab_dict = getattr(ptok, attr)
                    if isinstance(pvocab_dict, dict) and len(pvocab_dict) > 0:
                        break
                    pvocab_dict = None
            if pvocab_dict:
                inv = {int(v): str(k) for k, v in pvocab_dict.items()}
                phoneme_vocab = [inv.get(i, '') for i in range(max(inv.keys()) + 1)]

    if not char_vocab and hasattr(phonemizer, 'lang_phoneme_dict'):
        print("  Note: lang_phoneme_dict fallback found phoneme_vocab but not char_vocab.")
        lang_key = 'de' if 'de' in phonemizer.lang_phoneme_dict else list(phonemizer.lang_phoneme_dict.keys())[0]
        phoneme_set = phonemizer.lang_phoneme_dict[lang_key]
        phoneme_vocab = ['<pad>', '<sos>', '<eos>'] + sorted(phoneme_set)

    return char_vocab, phoneme_vocab


def _build_vocabs_from_checkpoint(ckpt):
    char_vocab = {}
    phoneme_vocab = []

    if not isinstance(ckpt, dict):
        return char_vocab, phoneme_vocab

    search_keys = ['config', 'text_tokenizer', 'phoneme_tokenizer', 'preprocessor']
    for key in search_keys:
        if key in ckpt:
            sub = ckpt[key]
            if isinstance(sub, dict):
                for vkey in ('vocab', 'token_to_idx', 'char_to_idx'):
                    if not char_vocab and vkey in sub:
                        char_vocab = {str(k): int(v) for k, v in sub[vkey].items()}
                for pkey in ('phonemes', 'phoneme_to_idx'):
                    if not phoneme_vocab and pkey in sub:
                        val = sub[pkey]
                        if isinstance(val, dict):
                            inv = {int(v): str(k) for k, v in val.items()}
                            phoneme_vocab = [inv.get(i, '') for i in range(max(inv.keys()) + 1)]
                        else:
                            phoneme_vocab = list(val)

    if not char_vocab and 'preprocessor' in ckpt:
        prep = ckpt['preprocessor']
        if isinstance(prep, dict):
            for sub_key in ('text_tokenizer', 'char_tokenizer'):
                if sub_key in prep and isinstance(prep[sub_key], dict):
                    for vkey in ('vocab', 'token_to_idx', 'char_to_idx'):
                        if vkey in prep[sub_key]:
                            char_vocab = {str(k): int(v) for k, v in prep[sub_key][vkey].items()}
                            break
                if char_vocab:
                    break
        elif hasattr(prep, 'text_tokenizer'):
            tok = prep.text_tokenizer
            for vkey in ('vocab', 'token_to_idx', 'char_to_idx', '_token_to_idx'):
                if hasattr(tok, vkey):
                    vdict = getattr(tok, vkey)
                    if isinstance(vdict, dict) and len(vdict) > 0:
                        char_vocab = {str(k): int(v) for k, v in vdict.items()}
                        break
            if not phoneme_vocab and hasattr(prep, 'phoneme_tokenizer'):
                ptok = prep.phoneme_tokenizer
                for pkey in ('vocab', 'token_to_idx', '_token_to_idx'):
                    if hasattr(ptok, pkey):
                        pdict = getattr(ptok, pkey)
                        if isinstance(pdict, dict) and len(pdict) > 0:
                            inv = {int(v): str(k) for k, v in pdict.items()}
                            phoneme_vocab = [inv.get(i, '') for i in range(max(inv.keys()) + 1)]
                            break

    return char_vocab, phoneme_vocab


def _extract_torch_model(phonemizer_or_ckpt):
    def _is_nn_module(obj):
        return hasattr(obj, 'parameters') and hasattr(obj, 'eval') and callable(getattr(obj, 'eval', None))

    if hasattr(phonemizer_or_ckpt, 'model'):
        candidate = phonemizer_or_ckpt.model
        if _is_nn_module(candidate):
            return candidate

    if hasattr(phonemizer_or_ckpt, 'predictor'):
        pred = phonemizer_or_ckpt.predictor
        if _is_nn_module(pred):
            return pred
        if hasattr(pred, 'model') and _is_nn_module(pred.model):
            return pred.model

    if isinstance(phonemizer_or_ckpt, dict):
        if 'model' in phonemizer_or_ckpt:
            return phonemizer_or_ckpt['model']

    raise RuntimeError(
        "Cannot extract PyTorch model from phonemizer object. "
        "Check DeepPhonemizer package version and checkpoint format."
    )


def _try_model_forward(model, x, lengths):
    """Probe the model's forward signature and return (output_tensor, style_string).

    style_string is one of: 'positional', 'x_only', 'keyword_lengths', 'attention_mask'.
    This style is used to build a TracableWrapper that matches the model's actual
    calling convention without try/except inside forward (which breaks tracing).
    """
    import torch

    for style, call in _FORWARD_STYLES:
        try:
            with torch.no_grad():
                out = call(model, x, lengths)
            if isinstance(out, tuple):
                out = out[0]
            if isinstance(out, torch.Tensor):
                print(f"    Forward pass succeeded (style={style}), output shape: {out.shape}")
                return out, style
        except Exception:
            continue

    raise RuntimeError(
        "Could not call model.forward() with any known signature. "
        "Check DeepPhonemizer model architecture."
    )


def _make_traceable_wrapper(torch_model, working_style):
    """Return an nn.Module whose forward delegates to torch_model using the
    discovered calling style. No try/except inside forward — tracing requires
    a single, deterministic execution path.
    """
    import torch

    if working_style == 'x_only':
        class TraceableWrapper(torch.nn.Module):
            def __init__(self, inner):
                super().__init__()
                self.inner = inner

            def forward(self, x, lengths):
                out = self.inner(x)
                if isinstance(out, tuple):
                    return out[0]
                return out

    elif working_style == 'keyword_lengths':
        class TraceableWrapper(torch.nn.Module):
            def __init__(self, inner):
                super().__init__()
                self.inner = inner

            def forward(self, x, lengths):
                out = self.inner(x, lengths=lengths)
                if isinstance(out, tuple):
                    return out[0]
                return out

    elif working_style == 'attention_mask':
        class TraceableWrapper(torch.nn.Module):
            def __init__(self, inner):
                super().__init__()
                self.inner = inner

            def forward(self, x, lengths):
                mask = (x != 0).long()
                out = self.inner(input_ids=x, attention_mask=mask)
                if isinstance(out, tuple):
                    return out[0]
                return out

    elif working_style == 'dict_text':
        class TraceableWrapper(torch.nn.Module):
            def __init__(self, inner):
                super().__init__()
                self.inner = inner

            def forward(self, x, lengths):
                out = self.inner({'text': x})
                if isinstance(out, tuple):
                    return out[0]
                return out

    else:
        class TraceableWrapper(torch.nn.Module):
            def __init__(self, inner):
                super().__init__()
                self.inner = inner

            def forward(self, x, lengths):
                out = self.inner(x, lengths)
                if isinstance(out, tuple):
                    return out[0]
                return out

    wrapper = TraceableWrapper(torch_model)
    wrapper.eval()
    return wrapper


def export_g2p_model(checkpoint_path, output_dir, use_script=False):
    import torch
    import numpy as np
    import coremltools as ct

    print(f"\n=== Exporting G2P Model to CoreML ===")
    print(f"  Checkpoint: {checkpoint_path}")
    print(f"  Output dir: {output_dir}")
    os.makedirs(output_dir, exist_ok=True)

    phonemizer_obj = load_phonemizer(checkpoint_path)

    char_vocab, phoneme_vocab = _build_vocabs_from_phonemizer(phonemizer_obj)
    if not char_vocab or not phoneme_vocab:
        print("  Warning: could not extract vocabs from phonemizer, trying raw checkpoint...")
        ckpt = torch.load(checkpoint_path, map_location='cpu', weights_only=False)
        char_vocab, phoneme_vocab = _build_vocabs_from_checkpoint(ckpt)

    if char_vocab:
        char_vocab_path = os.path.join(output_dir, 'char_vocab.json')
        with open(char_vocab_path, 'w', encoding='utf-8') as f:
            json.dump(char_vocab, f, ensure_ascii=False, indent=2)
        print(f"  Saved char_vocab.json ({len(char_vocab)} entries) -> {char_vocab_path}")
    else:
        print()
        print("  ERROR: Could not extract char_vocab from the checkpoint.")
        print("  The model's internal character indices are not accessible and cannot")
        print("  be reconstructed by this script. Without char_vocab.json the C++")
        print("  inference code cannot tokenize input text correctly.")
        print()
        print("  Possible causes:")
        print("    - Unsupported DeepPhonemizer version (try pip install deep-phonemizer==0.2.1)")
        print("    - Checkpoint format changed in the model repo")
        print()
        print("  Use --checkpoint with a checkpoint whose preprocessor exposes")
        print("  .text_tokenizer.vocab (standard DeepPhonemizer API).")
        sys.exit(1)

    if phoneme_vocab:
        phoneme_vocab_path = os.path.join(output_dir, 'phoneme_vocab.json')
        with open(phoneme_vocab_path, 'w', encoding='utf-8') as f:
            json.dump(phoneme_vocab, f, ensure_ascii=False, indent=2)
        print(f"  Saved phoneme_vocab.json ({len(phoneme_vocab)} entries) -> {phoneme_vocab_path}")
    else:
        print()
        print("  ERROR: Could not extract phoneme_vocab from the checkpoint.")
        print("  The model's internal phoneme index mapping is not accessible. Without")
        print("  phoneme_vocab.json the C++ inference code cannot decode model output")
        print("  indices to IPA strings, making the exported .mlmodelc unusable.")
        print()
        print("  Possible causes:")
        print("    - Unsupported DeepPhonemizer version (try pip install deep-phonemizer==0.2.1)")
        print("    - Checkpoint format changed in the model repo")
        print()
        print("  Use --checkpoint with a checkpoint whose preprocessor exposes")
        print("  .phoneme_tokenizer.vocab (standard DeepPhonemizer API).")
        sys.exit(1)

    try:
        torch_model = _extract_torch_model(phonemizer_obj)
    except RuntimeError as e:
        print(f"  Could not extract inner model: {e}")
        print("  Attempting attribute scan...")
        torch_model = None
        candidates = []
        for attr in ['model', 'net', 'encoder', 'transformer', 'predictor']:
            if hasattr(phonemizer_obj, attr):
                candidate = getattr(phonemizer_obj, attr)
                if hasattr(candidate, 'parameters'):
                    candidates.append((attr, candidate))
                elif attr == 'predictor' and hasattr(candidate, 'model') and hasattr(candidate.model, 'parameters'):
                    candidates.append((f'predictor.model', candidate.model))
        if candidates:
            path, torch_model = candidates[0]
            print(f"  Found model at .{path}")
        if torch_model is None:
            raise RuntimeError("Cannot locate inner PyTorch model for CoreML export.")

    torch_model.eval()

    pad_idx = char_vocab.get('<pad>', 0)
    unk_idx = char_vocab.get('<unk>', 1)

    def tokenize_word(word, vocab, max_len, pad_idx, unk_idx):
        ids = [vocab.get(c, unk_idx) for c in word]
        length = min(len(ids), max_len)
        ids = ids[:max_len]
        ids += [pad_idx] * (max_len - len(ids))
        return ids, length

    example_word = 'Bundesgesundheitsamt'
    example_ids, example_len = tokenize_word(example_word, char_vocab, MAX_CHAR_LEN, pad_idx, unk_idx)
    x_example = torch.tensor([example_ids], dtype=torch.int64)
    lengths_example = torch.tensor([example_len], dtype=torch.int64)

    print(f"  Testing forward pass with '{example_word}' (len={example_len})...")
    out_ref, working_style = _try_model_forward(torch_model, x_example, lengths_example)
    num_phonemes = out_ref.shape[-1] if out_ref.dim() >= 3 else None
    print(f"  Reference output shape: {out_ref.shape}")

    export_method = None
    traced_or_scripted = None

    if use_script:
        print("  Using torch.jit.script() for export (as requested)...")
        try:
            traced_or_scripted = torch.jit.script(torch_model)
            export_method = 'script'
        except Exception as e:
            print(f"  script() failed: {e}. Falling back to trace()...")

    if traced_or_scripted is None:
        print("  Using torch.jit.trace() for export...")
        traceable = _make_traceable_wrapper(torch_model, working_style)
        try:
            with torch.no_grad():
                traced_or_scripted = torch.jit.trace(
                    traceable,
                    (x_example, lengths_example),
                    strict=False,
                )
            export_method = 'trace'
        except Exception as e:
            print(f"  trace() failed: {e}")
            print("  Attempting script() as fallback...")
            traced_or_scripted = torch.jit.script(torch_model)
            export_method = 'script'

    print(f"  Export method: {export_method}")

    print("  Converting to CoreML...")
    coreml_inputs = [
        ct.TensorType(name="x", shape=(1, MAX_CHAR_LEN), dtype=np.int32),
        ct.TensorType(name="lengths", shape=(1,), dtype=np.int32),
    ]
    if num_phonemes is not None:
        coreml_outputs = [ct.TensorType(name="logits", shape=(1, MAX_CHAR_LEN, num_phonemes))]
    else:
        coreml_outputs = [ct.TensorType(name="logits")]

    mlmodel = None
    try:
        mlmodel = ct.convert(
            traced_or_scripted,
            inputs=coreml_inputs,
            outputs=coreml_outputs,
            convert_to="mlprogram",
            minimum_deployment_target=ct.target.macOS12,
            compute_precision=ct.precision.FLOAT16,
            compute_units=ct.ComputeUnit.ALL,
        )
    except Exception as e:
        print(f"  CoreML conversion with lengths input failed: {e}")
        if num_phonemes is not None:
            print("  Retrying without explicit output shape...")
            try:
                mlmodel = ct.convert(
                    traced_or_scripted,
                    inputs=coreml_inputs,
                    outputs=[ct.TensorType(name="logits")],
                    convert_to="mlprogram",
                    minimum_deployment_target=ct.target.macOS12,
                    compute_precision=ct.precision.FLOAT16,
                    compute_units=ct.ComputeUnit.ALL,
                )
            except Exception:
                pass

    if mlmodel is None:
        if working_style != 'x_only':
            print(f"  ERROR: x-only fallback is not applicable for working_style='{working_style}'.")
            print(f"  The model requires the '{working_style}' calling convention, which needs")
            print(f"  both inputs. CoreML conversion failed and there is no safe fallback.")
            print(f"  Try --use-script to attempt torch.jit.script() export instead.")
            sys.exit(1)

        print("  Retrying with x-only input (model does not use lengths)...")

        class XOnlyWrapper(torch.nn.Module):
            def __init__(self, inner):
                super().__init__()
                self.inner = inner

            def forward(self, x):
                out = self.inner(x)
                if isinstance(out, tuple):
                    return out[0]
                return out

        x_only = XOnlyWrapper(torch_model)
        x_only.eval()
        try:
            with torch.no_grad():
                traced_x_only = torch.jit.trace(x_only, (x_example,), strict=False)
        except Exception as trace_err:
            print(f"  ERROR: x-only trace also failed: {trace_err}")
            print(f"  Try --use-script to attempt torch.jit.script() export instead.")
            sys.exit(1)

        x_only_outputs = [ct.TensorType(name="logits")]
        try:
            mlmodel = ct.convert(
                traced_x_only,
                inputs=[ct.TensorType(name="x", shape=(1, MAX_CHAR_LEN), dtype=np.int32)],
                outputs=x_only_outputs,
                convert_to="mlprogram",
                minimum_deployment_target=ct.target.macOS12,
                compute_precision=ct.precision.FLOAT16,
                compute_units=ct.ComputeUnit.ALL,
            )
        except Exception as conv_err:
            print(f"  ERROR: x-only CoreML conversion also failed: {conv_err}")
            print(f"  Try --use-script to attempt torch.jit.script() export instead.")
            sys.exit(1)

    mlpackage_path = os.path.join(output_dir, 'de_g2p.mlpackage')
    mlmodel.save(mlpackage_path)
    print(f"  Saved: {mlpackage_path}")

    print("\n=== Validating CoreML export ===")
    validation_passed = _validate_coreml_export(
        mlmodel, torch_model, working_style, char_vocab, phoneme_vocab, pad_idx, unk_idx
    )

    if not validation_passed and export_method == 'trace':
        print("\n  Validation divergence detected — switching to torch.jit.script()...")
        try:
            scripted = torch.jit.script(torch_model)
            mlmodel = ct.convert(
                scripted,
                inputs=coreml_inputs,
                outputs=coreml_outputs,
                convert_to="mlprogram",
                minimum_deployment_target=ct.target.macOS12,
                compute_precision=ct.precision.FLOAT16,
                compute_units=ct.ComputeUnit.ALL,
            )
            mlmodel.save(mlpackage_path)
            print(f"  Re-saved with script(): {mlpackage_path}")
            _validate_coreml_export(
                mlmodel, torch_model, working_style, char_vocab, phoneme_vocab, pad_idx, unk_idx
            )
        except Exception as e:
            print(f"  script() fallback also failed: {e}")
            print("  Proceeding with trace()-based export (validation divergence noted).")

    print("\n  Compiling .mlpackage -> .mlmodelc ...")
    mlmodelc_path = os.path.join(output_dir, 'de_g2p.mlmodelc')
    if os.path.exists(mlmodelc_path):
        shutil.rmtree(mlmodelc_path)

    compile_result = run_cmd(
        f'xcrun coremlcompiler compile "{mlpackage_path}" "{output_dir}"',
        check=False
    )
    if compile_result.returncode != 0:
        print(f"  WARNING: xcrun coremlcompiler failed: {compile_result.stderr}")
        print("  The .mlpackage can still be used by CoreML at runtime (auto-compiled on first load).")
    elif os.path.exists(mlmodelc_path):
        print(f"  Compiled: {mlmodelc_path}")
    else:
        for entry in os.listdir(output_dir):
            if entry.endswith('.mlmodelc'):
                found = os.path.join(output_dir, entry)
                if found != mlmodelc_path:
                    shutil.move(found, mlmodelc_path)
                    print(f"  Compiled and renamed: {mlmodelc_path}")
                break

    return mlmodelc_path


def _validate_coreml_export(mlmodel, torch_model, working_style, char_vocab, phoneme_vocab, pad_idx, unk_idx):
    import torch
    import numpy as np

    test_cases = [
        ('Guten', 'short word'),
        ('Bundesgesundheitsamt', 'medium compound'),
        ('Sauerstoffsaettigungsmessung', 'long medical term'),
    ]

    print(f"  Running {len(test_cases)} validation test(s)...")
    all_passed = True

    for word, description in test_cases:
        ids = [char_vocab.get(c, unk_idx) for c in word]
        length = min(len(ids), MAX_CHAR_LEN)
        ids = ids[:MAX_CHAR_LEN]
        ids += [pad_idx] * (MAX_CHAR_LEN - len(ids))

        x_t = torch.tensor([ids], dtype=torch.int64)
        lengths_t = torch.tensor([length], dtype=torch.int64)

        call_fn = next((fn for style, fn in _FORWARD_STYLES if style == working_style), _FORWARD_STYLES[0][1])

        with torch.no_grad():
            try:
                pt_out = call_fn(torch_model, x_t, lengths_t)
                if isinstance(pt_out, tuple):
                    pt_out = pt_out[0]
            except Exception as e:
                print(f"    [{description}] PyTorch forward failed: {e}")
                all_passed = False
                continue

        x_np = np.array([ids], dtype=np.int32)
        lengths_np = np.array([length], dtype=np.int32)

        elapsed = 0.0
        try:
            t0 = time.time()
            cml_out = mlmodel.predict({"x": x_np, "lengths": lengths_np})
            elapsed = (time.time() - t0) * 1000
            cml_logits = list(cml_out.values())[0]
        except Exception:
            try:
                t0 = time.time()
                cml_out = mlmodel.predict({"x": x_np})
                elapsed = (time.time() - t0) * 1000
                cml_logits = list(cml_out.values())[0]
            except Exception as e2:
                print(f"    [{description}] CoreML predict failed: {e2}")
                all_passed = False
                continue

        pt_logits = pt_out.numpy()
        if pt_logits.shape != cml_logits.shape:
            print(f"    [{description}] Shape mismatch: PyTorch={pt_logits.shape} CoreML={cml_logits.shape}")
            all_passed = False
            continue

        pt_preds = np.argmax(pt_logits[0, :length], axis=-1)
        cml_preds = np.argmax(cml_logits[0, :length], axis=-1)
        match_rate = np.mean(pt_preds == cml_preds)

        if phoneme_vocab and len(phoneme_vocab) > int(max(pt_preds.max(), cml_preds.max())):
            pt_phones = ' '.join(phoneme_vocab[i] for i in pt_preds)
            cml_phones = ' '.join(phoneme_vocab[i] for i in cml_preds)
            print(f"    [{description}] '{word}' ({elapsed:.0f}ms)")
            print(f"      PyTorch:  {pt_phones}")
            print(f"      CoreML:   {cml_phones}")
        else:
            print(f"    [{description}] '{word}' match_rate={match_rate:.2%} ({elapsed:.0f}ms)")

        if match_rate < 0.9:
            print(f"    WARNING: Output divergence detected (match_rate={match_rate:.2%})")
            all_passed = False
        else:
            print(f"    OK (match_rate={match_rate:.2%})")

    return all_passed


def main():
    parser = argparse.ArgumentParser(
        description="Export DeepPhonemizer German G2P model to CoreML for Prodigy C++ pipeline"
    )
    parser.add_argument(
        "--checkpoint",
        default=None,
        help="Path to local DeepPhonemizer checkpoint (.pt file). "
             "If omitted, downloads from HuggingFace (as-ideas/DeepPhonemizer)."
    )
    parser.add_argument(
        "--output-dir",
        default=None,
        help="Output directory for exported artifacts. "
             "Defaults to bin/models/g2p/ relative to project root."
    )
    parser.add_argument(
        "--no-install",
        action="store_true",
        help="Skip conda env setup and dependency installation."
    )
    parser.add_argument(
        "--use-script",
        action="store_true",
        help="Force torch.jit.script() instead of trace(). "
             "Use if trace()-based export produces incorrect outputs."
    )
    args = parser.parse_args()

    output_dir = args.output_dir or os.path.join(ROOT_DIR, 'bin', 'models', 'g2p')

    if not args.no_install:
        conda_python = ensure_conda_env()
        if os.path.realpath(sys.executable) != os.path.realpath(conda_python):
            print(f"\n  Re-launching with conda python: {conda_python}")
            relaunch_args = [conda_python, __file__, '--no-install']
            if args.checkpoint:
                relaunch_args += ['--checkpoint', args.checkpoint]
            relaunch_args += ['--output-dir', output_dir]
            if args.use_script:
                relaunch_args.append('--use-script')
            os.execv(conda_python, relaunch_args)

    import torch
    print(f"\n  torch: {torch.__version__}")

    checkpoint_path = args.checkpoint
    is_english_fallback = False
    if checkpoint_path is None:
        dl_dir = os.path.join(ROOT_DIR, 'bin', 'models', 'g2p', 'checkpoint')
        checkpoint_path, is_english_fallback = download_checkpoint(dl_dir)
        if checkpoint_path is None:
            sys.exit(2)
    else:
        if not os.path.isfile(checkpoint_path):
            print(f"  ERROR: Checkpoint not found: {checkpoint_path}")
            sys.exit(1)

    export_g2p_model(checkpoint_path, output_dir, use_script=args.use_script)

    print("\n=== Export complete ===")
    if is_english_fallback:
        print()
        print("  WARNING: Exported model is based on the ENGLISH DeepPhonemizer checkpoint.")
        print("  German phonemization will be incorrect. Replace with a German checkpoint")
        print("  and re-run: python3 scripts/export_g2p_model.py --checkpoint de_g2p.pt")
        print()
    print(f"  CoreML model:     {output_dir}/de_g2p.mlmodelc")
    print(f"  Char vocab:       {output_dir}/char_vocab.json")
    print(f"  Phoneme vocab:    {output_dir}/phoneme_vocab.json")
    print()
    print("  Place these files in $WHISPERTALK_MODELS_DIR/g2p/ for kokoro-service,")
    print("  vits2-service, and matcha-service to use neural G2P.")
    return 0


if __name__ == '__main__':
    sys.exit(main())
