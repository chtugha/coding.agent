#!/usr/bin/env python3
"""
Export Matcha-TTS models to CoreML for the Prodigy C++ matcha-service pipeline.

This script downloads (or uses a local checkpoint) a Matcha-TTS model and exports
all components to CoreML .mlmodelc bundles for static inference with no Python runtime.

Exported artifacts (in bin/models/matcha-german/coreml/ or --output-dir):
  - matcha_encoder.mlmodelc          CoreML text encoder (10s bucket, max size)
  - matcha_encoder_3s.mlmodelc       CoreML text encoder (3s bucket, 280 mel frames)
  - matcha_encoder_5s.mlmodelc       CoreML text encoder (5s bucket, 468 mel frames)
  - matcha_encoder_10s.mlmodelc      CoreML text encoder (10s bucket, 938 mel frames)
  - matcha_flow_3s.mlmodelc          CoreML ODE flow (10-step baked Euler, 3s bucket)
  - matcha_flow_5s.mlmodelc          CoreML ODE flow (10-step baked Euler, 5s bucket)
  - matcha_flow_10s.mlmodelc         CoreML ODE flow (10-step baked Euler, 10s bucket)
  - matcha_vocoder.mlmodelc          CoreML HiFi-GAN vocoder (mel -> waveform)
  vocab.json                         Phoneme-to-ID mapping (alongside coreml/)

German checkpoint status:
  No public pre-trained German Matcha-TTS checkpoint exists as of 2025.
  Options (in order of preference):
    1. Provide a fine-tuned German checkpoint via --checkpoint
    2. Use a community German fine-tune if available (check HuggingFace)
    3. Fall back to the English checkpoint (shivammehta25/Matcha-TTS) — output will
       be English-accented; usable for integration testing and pipeline validation

  If fine-tuning is desired: train on Thorsten-Voice (~23h single-speaker German,
  available at https://www.thorsten-voice.de/) using the Matcha-TTS training guide
  at https://github.com/shivammehta25/Matcha-TTS/wiki/Training
  A fine-tune from the English base typically requires ~10k steps (~2-4 GPU-hours on A100).

Architecture notes:
  Matcha-TTS = text encoder + OT-CFM flow (ODE matching) + HiFi-GAN vocoder
  The ODE flow is baked at export time by unrolling N Euler steps into a static graph.
  Each bucket (3s/5s/10s) gets separate encoder + flow models with fixed output shapes:
    3s  bucket: T_mel = 280 frames  (at hop=256, sr=24000; rounded to even for UNet skip-conn compat)
    5s  bucket: T_mel = 468 frames
    10s bucket: T_mel = 938 frames

  Encoder wrapper includes: text encoding + duration prediction + length regulation
  + padding to fixed T_mel. Input token_ids are padded to MAX_INPUT_TOKENS=512.

  matcha_encoder.mlmodelc is a copy of matcha_encoder_10s.mlmodelc (largest bucket)
  for backward compatibility with matcha-service.cpp which loads a single encoder.
  In that deployment, all synthesis uses T_mel=938 with the mask indicating valid frames.

Prerequisites:
  - macOS with Apple Silicon
  - conda (miniconda or miniforge)
  - Internet access (for model download if --checkpoint not provided)

Usage:
  python3 scripts/export_matcha_models.py
  python3 scripts/export_matcha_models.py --checkpoint /path/to/matcha_de.ckpt
  python3 scripts/export_matcha_models.py --output-dir /tmp/matcha_test
  python3 scripts/export_matcha_models.py --steps 10 --bucket-sizes 280,468,938
  python3 scripts/export_matcha_models.py --no-install

Required conda environment packages:
  torch>=2.0, coremltools>=8.0, numpy, huggingface_hub
  Matcha-TTS source: pip install git+https://github.com/shivammehta25/Matcha-TTS.git
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
REQUIRED_TORCH = '2.5.0'
REQUIRED_COREMLTOOLS = '8.1'

DEFAULT_OUTPUT_SUBDIR = 'matcha-german'
MAX_INPUT_TOKENS = 512
MEL_BINS = 80
DEFAULT_STEPS = 10

BUCKETS = [
    {'name': '3s',  'frames': 280,  'max_tokens': 50},
    {'name': '5s',  'frames': 468,  'max_tokens': 100},
    {'name': '10s', 'frames': 938,  'max_tokens': 999999},
]

HF_EN_REPO_ID = 'shivammehta25/Matcha-TTS'
HF_EN_FILENAME = 'matcha_ljspeech.ckpt'

GH_RELEASES_URL = 'https://github.com/shivammehta25/Matcha-TTS-checkpoints/releases/download/v1.0'
VOCODER_URLS = {
    'ljspeech': f'{GH_RELEASES_URL}/generator_v1',
    'universal': f'{GH_RELEASES_URL}/g_02500000',
}

KNOWN_DE_REPOS = [
    ('Flux9999/Matcha-TTS-German', 'matcha_german.ckpt'),
    ('freds0/Matcha-TTS-de', 'matcha_de.ckpt'),
]


def _patch_matcha_for_torchscript():
    """Patch matcha text_encoder classes to be TorchScript-compatible.

    LayerNorm.forward: builds a view shape with a dynamic list expression
        shape = [1, -1] + [1] * (n_dims - 2)
    which TorchScript cannot statically analyse.  For 3-D tensors [B, C, T] (the
    only shape used in the text encoder) the equivalent is unsqueeze(0).unsqueeze(-1).

    ConvReluNorm.forward: uses self.conv_layers[i] with a variable index, which
    TorchScript does not support.  Replaced with zip() enumeration.

    Must be called before the first torch.jit.script() on any Matcha encoder wrapper.
    """
    try:
        import math
        import torch
        import matcha.models.components.text_encoder as _te

        def forward(self, x: torch.Tensor) -> torch.Tensor:
            assert x.dim() == 3, f"LayerNorm patch expects 3D input [B,C,T], got {x.dim()}D"
            mean = torch.mean(x, 1, keepdim=True)
            variance = torch.mean((x - mean) ** 2, 1, keepdim=True)
            x = (x - mean) * torch.rsqrt(variance + self.eps)
            return x * self.gamma.unsqueeze(0).unsqueeze(-1) + self.beta.unsqueeze(0).unsqueeze(-1)

        _te.LayerNorm.forward = forward

        def forward(self, x: torch.Tensor, x_mask: torch.Tensor) -> torch.Tensor:
            x_org = x
            for conv, norm in zip(self.conv_layers, self.norm_layers):
                x = conv(x * x_mask)
                x = norm(x)
                x = self.relu_drop(x)
            x = x_org + self.proj(x)
            return x * x_mask

        _te.ConvReluNorm.forward = forward

        def forward(self, x: torch.Tensor, x_mask: torch.Tensor) -> torch.Tensor:
            attn_mask = x_mask.unsqueeze(2) * x_mask.unsqueeze(-1)
            for attn_layer, norm1, ffn, norm2 in zip(
                self.attn_layers, self.norm_layers_1,
                self.ffn_layers, self.norm_layers_2,
            ):
                x = x * x_mask
                y = attn_layer(x, x, attn_mask)
                y = self.drop(y)
                x = norm1(x + y)
                y = ffn(x, x_mask)
                y = self.drop(y)
                x = norm2(x + y)
            x = x * x_mask
            return x

        _te.Encoder.forward = forward

        def forward(self, x: torch.Tensor) -> torch.Tensor:
            b, h, t, d = x.shape
            x = x.permute(2, 0, 1, 3)
            self._build_cache(x)
            x_rope = x[..., : self.d]
            x_pass = x[..., self.d :]
            d_2 = self.d // 2
            neg_half_x = torch.cat([-x_rope[:, :, :, d_2:], x_rope[:, :, :, :d_2]], dim=-1)
            x_rope = (x_rope * self.cos_cached[: x.shape[0]]) + (neg_half_x * self.sin_cached[: x.shape[0]])
            out = torch.cat((x_rope, x_pass), dim=-1)
            return out.permute(1, 2, 0, 3)

        _te.RotaryPositionalEmbeddings.forward = forward

        def attention(self, query: torch.Tensor, key: torch.Tensor,
                      value: torch.Tensor,
                      mask: torch.Tensor = None):
            b, d, t_s = key.shape
            t_t = query.shape[2]
            c = d // self.n_heads
            query = query.view(b, self.n_heads, c, t_t).permute(0, 1, 3, 2)
            key = key.view(b, self.n_heads, c, t_s).permute(0, 1, 3, 2)
            value = value.view(b, self.n_heads, c, t_s).permute(0, 1, 3, 2)

            query = self.query_rotary_pe(query)
            key = self.key_rotary_pe(key)

            scores = torch.matmul(query, key.transpose(-2, -1)) / math.sqrt(self.k_channels)

            if mask is not None:
                scores = scores.masked_fill(mask == 0, -1e4)
            p_attn = torch.nn.functional.softmax(scores, dim=-1)
            p_attn = self.drop(p_attn)
            output = torch.matmul(p_attn, value)
            output = output.transpose(2, 3).contiguous().view(b, d, t_t)
            return output, p_attn

        _te.MultiHeadAttention.attention = attention

        import matcha.utils.model as _mu

        def sequence_mask(length: torch.Tensor, max_length: int = -1) -> torch.Tensor:
            if max_length < 0:
                max_length = int(length.max().item())
            x = torch.arange(max_length, dtype=length.dtype, device=length.device)
            return x.unsqueeze(0) < length.unsqueeze(1)

        _mu.sequence_mask = sequence_mask
        _te.sequence_mask = sequence_mask

        def forward(self, x: torch.Tensor, x_lengths: torch.Tensor,
                    spks: torch.Tensor = None) -> tuple:
            x = self.emb(x) * math.sqrt(self.n_channels)
            x = torch.transpose(x, 1, -1)
            x_mask = torch.unsqueeze(sequence_mask(x_lengths, x.size(2)), 1).to(x.dtype)
            x = self.prenet(x, x_mask)
            x = self.encoder(x, x_mask)
            mu = self.proj_m(x) * x_mask
            x_dp = torch.detach(x)
            logw = self.proj_w(x_dp, x_mask)
            return mu, logw, x_mask

        _te.TextEncoder.forward = forward
    except Exception as e:
        print(f'  [WARN] Could not patch matcha modules for TorchScript: {e}')


def run_cmd(cmd, check=True, capture=True):
    print(f'  $ {cmd}')
    result = subprocess.run(cmd, shell=True,
                            capture_output=capture, text=capture)
    if check and result.returncode != 0:
        print(f'  FAILED (exit {result.returncode})')
        if capture and result.stderr:
            print(result.stderr[-2000:])
        sys.exit(1)
    return result


def parse_args():
    parser = argparse.ArgumentParser(
        description='Export Matcha-TTS models to CoreML for matcha-service.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=__doc__,
    )
    parser.add_argument(
        '--checkpoint', default=None,
        help=(
            'Path to a local Matcha-TTS checkpoint (.ckpt or .pt). '
            'If omitted, the script searches HuggingFace for a German fine-tune, '
            'then falls back to the English checkpoint (shivammehta25/Matcha-TTS).'
        ),
    )
    parser.add_argument(
        '--output-dir', default=None,
        help=(
            f'Directory to save CoreML models and vocab.json. '
            f'Default: bin/models/{DEFAULT_OUTPUT_SUBDIR}/coreml/ relative to project root. '
            f'vocab.json is placed one level up (bin/models/{DEFAULT_OUTPUT_SUBDIR}/).'
        ),
    )
    parser.add_argument(
        '--steps', type=int, default=DEFAULT_STEPS,
        help=(
            f'Number of Euler ODE steps to bake into flow models (default: {DEFAULT_STEPS}). '
            f'Higher = better quality, slower export. Recommended range: 5-20.'
        ),
    )
    parser.add_argument(
        '--bucket-sizes', default=None,
        help=(
            'Comma-separated T_mel frame counts for 3 buckets (default: 280,468,938). '
            'Must be 3 values in ascending order, e.g. --bucket-sizes 256,512,1024.'
        ),
    )
    parser.add_argument(
        '--no-install', action='store_true',
        help='Skip conda environment dependency installation.',
    )
    parser.add_argument(
        '--encoder-only', action='store_true',
        help='Export only the text encoder components.',
    )
    parser.add_argument(
        '--flow-only', action='store_true',
        help='Export only the flow (ODE) models.',
    )
    parser.add_argument(
        '--vocoder-only', action='store_true',
        help='Export only the HiFi-GAN vocoder.',
    )
    return parser.parse_args()


def ensure_conda_env():
    print('\n=== Checking conda environment ===')
    result = run_cmd(f'conda env list | grep {CONDA_ENV_NAME}', check=False)
    if CONDA_ENV_NAME not in result.stdout:
        print(f'  Creating conda env {CONDA_ENV_NAME!r} with Python 3.11...')
        run_cmd(f'conda create -n {CONDA_ENV_NAME} python=3.11 -y')

    conda_prefix = run_cmd(
        f'conda run -n {CONDA_ENV_NAME} python -c "import sys; print(sys.prefix)"',
    ).stdout.strip()
    conda_python = os.path.join(conda_prefix, 'bin', 'python')

    print(f'  Installing dependencies in {CONDA_ENV_NAME}...')
    run_cmd(
        f'{conda_python} -m pip install -q '
        f'torch=={REQUIRED_TORCH} --index-url https://download.pytorch.org/whl/cpu',
        capture=False,
    )
    run_cmd(
        f'{conda_python} -m pip install -q '
        f'coremltools>={REQUIRED_COREMLTOOLS} numpy==1.26.4 huggingface_hub',
        capture=False,
    )
    run_cmd(
        f'{conda_python} -m pip install -q matcha-tts',
        capture=False,
    )

    version_check = run_cmd(
        f'{conda_python} -c "import torch, coremltools as ct, numpy as np; '
        f'print(f\'torch={{torch.__version__}} ct={{ct.__version__}} np={{np.__version__}}\')"'
    )
    print(f'  Versions: {version_check.stdout.strip()}')
    return conda_python


def _ensure_huggingface_hub():
    try:
        import huggingface_hub
        return huggingface_hub
    except ImportError:
        print('  Installing huggingface_hub...')
        subprocess.run(
            [sys.executable, '-m', 'pip', 'install', '-q', 'huggingface_hub'],
            check=True,
        )
        import huggingface_hub
        return huggingface_hub


def download_checkpoint(output_dir):
    """Download Matcha-TTS checkpoint. Tries German fine-tunes first, then English fallback."""
    print('\n=== Downloading Matcha-TTS checkpoint ===')
    os.makedirs(output_dir, exist_ok=True)
    hf = _ensure_huggingface_hub()

    hf_token = os.environ.get('HF_TOKEN', '')
    token = hf_token if hf_token else None

    for repo_id, filename in KNOWN_DE_REPOS:
        local_path = os.path.join(output_dir, filename)
        if os.path.isfile(local_path) and os.path.getsize(local_path) > 1_000_000:
            print(f'  Found cached German checkpoint: {local_path}')
            return local_path, False
        print(f'  Trying German fine-tune: {repo_id}/{filename}...')
        try:
            downloaded = hf.hf_hub_download(
                repo_id=repo_id, filename=filename, token=token,
            )
            shutil.copy2(downloaded, local_path)
            print(f'  German checkpoint downloaded: {local_path} '
                  f'({os.path.getsize(local_path) / 1e6:.1f} MB)')
            return local_path, False
        except Exception as e:
            print(f'  Not found: {e}')

    print()
    print('  No German Matcha-TTS checkpoint found on HuggingFace.')
    print('  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!')
    print('  WARNING: Falling back to English checkpoint (shivammehta25/Matcha-TTS).')
    print('  The exported models will produce ENGLISH speech, not German.')
    print('  German compound words and medical terms will be mispronounced.')
    print('  For German TTS:')
    print('    1. Fine-tune on Thorsten-Voice (~23h, https://www.thorsten-voice.de/)')
    print('    2. Save checkpoint and re-run: --checkpoint /path/to/matcha_de.ckpt')
    print('  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!')
    print()

    en_path = os.path.join(output_dir, HF_EN_FILENAME)
    if os.path.isfile(en_path) and os.path.getsize(en_path) > 1_000_000:
        print(f'  Using cached English checkpoint: {en_path}')
        return en_path, True

    print(f'  Downloading English checkpoint from {HF_EN_REPO_ID}...')
    try:
        downloaded = hf.hf_hub_download(
            repo_id=HF_EN_REPO_ID, filename=HF_EN_FILENAME, token=token,
        )
        shutil.copy2(downloaded, en_path)
        print(f'  OK ({os.path.getsize(en_path) / 1e6:.1f} MB) -> {en_path}')
        return en_path, True
    except Exception as e:
        print(f'  HuggingFace download failed: {e}')

    gh_url = f'{GH_RELEASES_URL}/{HF_EN_FILENAME}'
    print(f'  Trying GitHub Releases fallback: {gh_url}')
    try:
        import urllib.request
        import socket
        old_timeout = socket.getdefaulttimeout()
        socket.setdefaulttimeout(60)
        try:
            urllib.request.urlretrieve(gh_url, en_path)
        finally:
            socket.setdefaulttimeout(old_timeout)
        size_mb = os.path.getsize(en_path) / 1e6
        print(f'  OK ({size_mb:.1f} MB) -> {en_path}')
        return en_path, True
    except Exception as e2:
        print(f'  ERROR: GitHub Releases download also failed: {e2}')
        print(f'  Download manually from: {gh_url}')
        print(f'  Save to: {en_path}')
        print(f'  Then re-run with --checkpoint {en_path}')
        sys.exit(1)


def _load_matcha_tts_module():
    """Load the matcha_tts package, trying multiple install locations."""
    try:
        import matcha
        return matcha
    except ImportError:
        pass

    import site
    search_dirs = list(site.getsitepackages())

    conda_base = os.environ.get('CONDA_PREFIX', '')
    if conda_base:
        for pyver in ['python3.11', 'python3.10', 'python3.12']:
            d = os.path.join(conda_base, 'lib', pyver, 'site-packages')
            if os.path.isdir(d) and d not in search_dirs:
                search_dirs.append(d)

    for d in search_dirs:
        matcha_dir = os.path.join(d, 'matcha')
        if os.path.isdir(matcha_dir):
            if d not in sys.path:
                sys.path.insert(0, d)
            import matcha
            return matcha

    raise ImportError(
        'Could not import matcha package. Install with:\n'
        '  pip install matcha-tts'
    )


def _safe_torch_load(checkpoint_path):
    """Load a checkpoint, attempting weights_only=True first for security."""
    import torch
    import pickle
    try:
        ckpt = torch.load(checkpoint_path, map_location='cpu', weights_only=True)
        return ckpt
    except (pickle.UnpicklingError, RuntimeError, TypeError, AttributeError):
        pass

    print()
    print('  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!')
    print('  SECURITY WARNING: This checkpoint requires weights_only=False,')
    print('  which allows arbitrary Python code execution via pickle.')
    print('  Only load checkpoints from sources you trust.')
    print('  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!')
    print()
    return torch.load(checkpoint_path, map_location='cpu', weights_only=False)


def load_matcha_model(checkpoint_path):
    """Load a Matcha-TTS model from checkpoint. Returns (model, hparams)."""
    import torch
    print(f'\n=== Loading Matcha-TTS from {checkpoint_path} ===')

    ckpt = _safe_torch_load(checkpoint_path)
    print(f'  Checkpoint keys: {list(ckpt.keys()) if isinstance(ckpt, dict) else type(ckpt)}')

    if isinstance(ckpt, dict) and 'hyper_parameters' in ckpt:
        hparams = ckpt['hyper_parameters']
        print(f'  HParams keys: {list(hparams.keys())}')
    else:
        hparams = {}

    try:
        matcha_mod = _load_matcha_tts_module()
        from matcha.models.matcha_tts import MatchaTTS
        print('  Instantiating MatchaTTS from hyper_parameters...')

        if not hparams:
            raise RuntimeError('No hyper_parameters in checkpoint; cannot instantiate model')

        model = MatchaTTS(**hparams)

        state_dict = ckpt.get('state_dict', ckpt)
        if isinstance(state_dict, dict):
            missing, unexpected = model.load_state_dict(state_dict, strict=False)
            if missing:
                print(f'  Warning: missing keys ({len(missing)}): {missing[:5]}...')
            if unexpected:
                print(f'  Warning: unexpected keys ({len(unexpected)}): {unexpected[:5]}...')

        model.eval()
        print('  Model loaded successfully')
        return model, hparams

    except Exception as e:
        print(f'  MatchaTTS instantiation failed: {e}')
        print('  Attempting direct state_dict inspection...')

        if isinstance(ckpt, dict) and 'state_dict' in ckpt:
            sd_keys = list(ckpt['state_dict'].keys())[:10]
            print(f'  State dict sample keys: {sd_keys}')

        raise RuntimeError(
            f'Cannot load Matcha-TTS model from {checkpoint_path}.\n'
            f'Error: {e}\n'
            f'Ensure the matcha_tts package is installed and matches the checkpoint format.'
        )


def _download_vocoder(output_dir):
    """Download HiFi-GAN vocoder checkpoint from GitHub Releases."""
    vocoder_path = os.path.join(output_dir, 'generator_v1')
    if os.path.isfile(vocoder_path) and os.path.getsize(vocoder_path) > 1_000_000:
        print(f'  Using cached vocoder: {vocoder_path}')
        return vocoder_path

    url = VOCODER_URLS['ljspeech']
    print(f'  Downloading HiFi-GAN vocoder from {url}...')
    try:
        import urllib.request
        import socket
        old_timeout = socket.getdefaulttimeout()
        socket.setdefaulttimeout(120)
        try:
            urllib.request.urlretrieve(url, vocoder_path)
        finally:
            socket.setdefaulttimeout(old_timeout)
        size_mb = os.path.getsize(vocoder_path) / 1e6
        print(f'  OK ({size_mb:.1f} MB) -> {vocoder_path}')
        return vocoder_path
    except Exception as e:
        print(f'  Vocoder download failed: {e}')
        universal_url = VOCODER_URLS['universal']
        print(f'  Trying universal vocoder: {universal_url}...')
        try:
            vocoder_path_u = os.path.join(output_dir, 'g_02500000')
            old_timeout = socket.getdefaulttimeout()
            socket.setdefaulttimeout(120)
            try:
                urllib.request.urlretrieve(universal_url, vocoder_path_u)
            finally:
                socket.setdefaulttimeout(old_timeout)
            size_mb = os.path.getsize(vocoder_path_u) / 1e6
            print(f'  OK ({size_mb:.1f} MB) -> {vocoder_path_u}')
            return vocoder_path_u
        except Exception as e2:
            print(f'  Universal vocoder download also failed: {e2}')
            return None


def _load_and_attach_vocoder(model, checkpoint_dir):
    """Download, load, and attach HiFi-GAN vocoder to the model."""
    import torch

    print('\n=== Loading HiFi-GAN vocoder ===')
    os.makedirs(checkpoint_dir, exist_ok=True)

    vocoder_ckpt = _download_vocoder(checkpoint_dir)
    if vocoder_ckpt is None:
        print('  WARNING: Could not download vocoder checkpoint.')
        return False

    try:
        from matcha.hifigan.config import v1
        from matcha.hifigan.env import AttrDict
        from matcha.hifigan.models import Generator
    except ImportError:
        try:
            from matcha.utils.utils import get_hifigan as _
            import matcha.hifigan
            from matcha.hifigan.config import v1
            from matcha.hifigan.env import AttrDict
            from matcha.hifigan.models import Generator
        except ImportError as ie:
            print(f'  WARNING: Could not import HiFi-GAN modules: {ie}')
            print('  Ensure matcha-tts is installed with HiFi-GAN support.')
            return False

    try:
        h = AttrDict(v1)
        vocoder = Generator(h)
        ckpt_data = torch.load(vocoder_ckpt, map_location='cpu', weights_only=False)
        if 'generator' in ckpt_data:
            vocoder.load_state_dict(ckpt_data['generator'])
        else:
            vocoder.load_state_dict(ckpt_data)
        vocoder.eval()
        vocoder.remove_weight_norm()
        model.vocoder = vocoder
        print('  HiFi-GAN vocoder loaded and attached to model.')
        return True
    except Exception as e:
        print(f'  WARNING: Failed to load vocoder: {e}')
        return False


def _build_encoder_wrapper(model, T_mel, bucket_name):
    """
    Build a trace-compatible nn.Module wrapping the Matcha-TTS text encoder pipeline
    for a fixed T_mel bucket. Uses pure tensor operations (no .item() calls) so that
    torch.jit.trace() can record the computation graph safely.

    Input:
      input_ids [1, MAX_INPUT_TOKENS=512]  int64, padded with 0
      x_lengths [1]                         int64, actual token count
      speed     [1]                         float32

    Output:
      mu   [1, MEL_BINS, T_mel]  float32  (mean for flow matching)
      mask [1, 1, T_mel]         float32  (1.0 = valid frame, 0.0 = padding)
    """
    import torch
    import torch.nn as nn

    class MatchaEncoderWrapper(nn.Module):
        def __init__(self, inner_model, t_mel_fixed: int):
            super().__init__()
            self.encoder = inner_model.encoder
            self.t_mel = t_mel_fixed

        def forward(self, input_ids: torch.Tensor, x_lengths: torch.Tensor,
                    speed: torch.Tensor) -> tuple:
            mu_phone, logw, x_mask_enc = self.encoder(input_ids, x_lengths)

            spd = speed.clamp(min=0.1)
            w = torch.exp(logw) * x_mask_enc / spd
            w_ceil = torch.ceil(w).clamp(min=1.0)

            y_length = w_ceil.squeeze().sum().clamp(max=self.t_mel).long()
            y_length_t = y_length.unsqueeze(0)
            y_mask = (torch.arange(self.t_mel, device=input_ids.device).unsqueeze(0)
                      < y_length_t.unsqueeze(1)).unsqueeze(1).float()

            attn_mask = x_mask_enc.unsqueeze(-1) * y_mask.unsqueeze(2)
            w_int = w_ceil.squeeze(1).long()

            B = w_int.shape[0]
            T_y = self.t_mel
            cum_dur = torch.cumsum(w_int, dim=1).clamp(max=T_y)
            cum_dur_prev = torch.cat(
                [torch.zeros(B, 1, dtype=cum_dur.dtype, device=cum_dur.device),
                 cum_dur[:, :-1]], dim=1,
            )
            idx = torch.arange(T_y, device=w_int.device).unsqueeze(0).unsqueeze(0)
            path = ((idx >= cum_dur_prev.unsqueeze(-1)) &
                    (idx < cum_dur.unsqueeze(-1))).float()
            path = path * attn_mask.squeeze(1)
            attn = path

            mu_y = torch.matmul(
                attn.transpose(1, 2),
                mu_phone.transpose(1, 2),
            ).transpose(1, 2)

            pad_size = self.t_mel - mu_y.shape[-1]
            if pad_size > 0:
                mu_y = torch.nn.functional.pad(mu_y, (0, pad_size))
            elif pad_size < 0:
                mu_y = mu_y[:, :, :self.t_mel]

            return mu_y, y_mask

    return MatchaEncoderWrapper(model, T_mel)


def _build_flow_wrapper(model, T_mel, n_steps, bucket_name):
    """
    Build a nn.Module that wraps the Matcha-TTS flow/decoder with baked Euler steps.

    The ODE integration loop is unrolled to n_steps Euler steps as a static graph.

    Input:
      z    [1, MEL_BINS, T_mel]  float32  (noise)
      mu   [1, MEL_BINS, T_mel]  float32  (encoder mean)
      mask [1, 1, T_mel]         float32  (validity mask)

    Output:
      mel  [1, MEL_BINS, T_mel]  float32  (mel spectrogram)
    """
    import torch
    import torch.nn as nn

    decoder = model.decoder
    steps = n_steps

    class BakedEulerFlow(nn.Module):
        def __init__(self, flow_decoder, n_euler_steps):
            super().__init__()
            self.decoder = flow_decoder
            self.n_steps = n_euler_steps
            self.dt = 1.0 / n_euler_steps

        def forward(self, z: torch.Tensor, mu: torch.Tensor,
                    mask: torch.Tensor) -> torch.Tensor:
            t = 0.0
            x = z
            dt = self.dt
            for _ in range(self.n_steps):
                t_tensor = torch.tensor([t], dtype=torch.float32, device=z.device)
                vf = self.decoder.forward_flow(x, t_tensor, mu, mask)
                x = x + dt * vf
                t = t + dt
            return x * mask

    wrapper = BakedEulerFlow(decoder, steps)
    wrapper.eval()
    return wrapper


def _build_flow_wrapper_fallback(model, T_mel, n_steps, bucket_name):
    """
    Fallback flow wrapper for Matcha-TTS versions that expose the vector field
    via 'estimator' rather than 'forward_flow'. The method is detected once at
    construction time (__init__) and stored as a bool flag; the forward() method
    uses a plain if/else branch, which is compatible with both torch.jit.trace()
    and torch.jit.script() (unlike try/except AttributeError, which TorchScript
    cannot handle).
    """
    import torch
    import torch.nn as nn

    decoder = model.decoder
    steps = n_steps
    use_estimator = not hasattr(decoder, 'forward_flow')

    if use_estimator:
        class BakedEulerFlowFallback(nn.Module):
            def __init__(self, flow_decoder, n_euler_steps: int):
                super().__init__()
                self.decoder = flow_decoder
                self.n_steps = n_euler_steps
                self.dt = 1.0 / n_euler_steps

            def forward(self, z: torch.Tensor, mu: torch.Tensor,
                        mask: torch.Tensor) -> torch.Tensor:
                t = 0.0
                x = z
                dt = self.dt
                for _ in range(self.n_steps):
                    t_in = torch.full((z.shape[0],), t, dtype=torch.float32, device=z.device)
                    vf = self.decoder.estimator(x, mask, mu, t_in)
                    x = x + dt * vf
                    t = t + dt
                return x * mask
    else:
        class BakedEulerFlowFallback(nn.Module):
            def __init__(self, flow_decoder, n_euler_steps: int):
                super().__init__()
                self.decoder = flow_decoder
                self.n_steps = n_euler_steps
                self.dt = 1.0 / n_euler_steps

            def forward(self, z: torch.Tensor, mu: torch.Tensor,
                        mask: torch.Tensor) -> torch.Tensor:
                t = 0.0
                x = z
                dt = self.dt
                for _ in range(self.n_steps):
                    t_in = torch.full((z.shape[0],), t, dtype=torch.float32, device=z.device)
                    vf = self.decoder.forward_flow(x, t_in, mu, mask)
                    x = x + dt * vf
                    t = t + dt
                return x * mask

    wrapper = BakedEulerFlowFallback(decoder, steps)
    wrapper.eval()
    return wrapper


def _build_vocoder_wrapper(model):
    """
    Wrap the HiFi-GAN vocoder for CoreML export.

    Input:
      mel [1, MEL_BINS, T_mel]  float32

    Output:
      waveform [1, T_wav]  float32  (T_wav = T_mel * hop_size)
    """
    import torch
    import torch.nn as nn

    class VocoderWrapper(nn.Module):
        def __init__(self, vocoder):
            super().__init__()
            self.vocoder = vocoder

        def forward(self, mel: torch.Tensor) -> torch.Tensor:
            wav = self.vocoder(mel)
            if wav.dim() == 3:
                wav = wav.squeeze(1)
            return wav

    wrapper = VocoderWrapper(model.vocoder)
    wrapper.eval()
    return wrapper


def _compile_mlpackage_to_mlmodelc(mlpackage_path, output_dir):
    """Compile .mlpackage -> .mlmodelc using xcrun coremlcompiler."""
    name = os.path.splitext(os.path.basename(mlpackage_path))[0]
    mlmodelc_path = os.path.join(output_dir, name + '.mlmodelc')
    if os.path.exists(mlmodelc_path):
        shutil.rmtree(mlmodelc_path)
    result = run_cmd(
        f'xcrun coremlcompiler compile "{mlpackage_path}" "{output_dir}"',
        check=False,
    )
    if result.returncode != 0 or not os.path.exists(mlmodelc_path):
        print(f'  WARNING: coremlcompiler failed or produced no output.')
        print(f'  The .mlpackage is still usable but .mlmodelc compilation failed.')
        print(f'  Error: {result.stderr[:500] if result.stderr else "(none)"}')
        return None
    size_mb = sum(
        os.path.getsize(os.path.join(dp, f))
        for dp, dn, filenames in os.walk(mlmodelc_path)
        for f in filenames
    ) / 1e6
    print(f'  Compiled: {mlmodelc_path} ({size_mb:.1f} MB)')
    return mlmodelc_path


def export_encoder(model, coreml_dir, bucket, no_validate=False):
    """Export text encoder for a specific bucket. Returns path to .mlmodelc."""
    import torch
    import numpy as np
    import coremltools as ct

    bucket_name = bucket['name']
    T_mel = bucket['frames']
    print(f'\n=== Exporting encoder (bucket={bucket_name}, T_mel={T_mel}) ===')
    os.makedirs(coreml_dir, exist_ok=True)

    wrapper = _build_encoder_wrapper(model, T_mel, bucket_name)
    wrapper.eval()
    for m in wrapper.modules():
        m.eval()

    T_in = MAX_INPUT_TOKENS
    input_ids = torch.randint(1, 100, (1, T_in), dtype=torch.long)
    x_lengths = torch.tensor([min(20, T_in)], dtype=torch.long)
    speed = torch.tensor([1.0], dtype=torch.float32)

    print('  Testing forward pass...')
    try:
        with torch.no_grad():
            mu_ref, mask_ref = wrapper(input_ids, x_lengths, speed)
        print(f'  Forward OK: mu={mu_ref.shape}, mask={mask_ref.shape}')
    except Exception as e:
        print(f'  Forward pass failed: {e}')
        print('  Attempting to inspect decoder architecture...')
        print(f'  Encoder attrs: {[a for a in dir(model) if not a.startswith("_")]}')
        raise

    print('  Tracing encoder...')
    traced = None
    try:
        with torch.no_grad():
            traced = torch.jit.trace(wrapper, (input_ids, x_lengths, speed), strict=False)
    except Exception as e:
        print(f'  torch.jit.trace failed: {e}')
        print('  Falling back to torch.jit.script...')
        try:
            traced = torch.jit.script(wrapper)
        except Exception as e2:
            print(f'  torch.jit.script also failed: {e2}')
            print('  WARNING: Cannot trace or script encoder. Skipping CoreML export for this bucket.')
            return None

    print('  Validating traced model vs original...')
    with torch.no_grad():
        mu_traced, mask_traced = traced(input_ids, x_lengths, speed)
    mse_mu = float(((mu_ref - mu_traced) ** 2).mean())
    print(f'  MSE (mu): {mse_mu:.6f} {"OK" if mse_mu < 0.01 else "WARNING: HIGH"}')
    if mse_mu > 0.01:
        print(f'  ERROR: Encoder script MSE {mse_mu:.6f} exceeds threshold 0.01. Export aborted.')
        print('  Check encoder wrapper implementation for non-deterministic ops.')
        sys.exit(1)

    print('  Converting to CoreML...')
    try:
        enc_ml = ct.convert(
            traced,
            inputs=[
                ct.TensorType(name='input_ids',
                              shape=(1, MAX_INPUT_TOKENS), dtype=np.int64),
                ct.TensorType(name='x_lengths',
                              shape=(1,), dtype=np.int64),
                ct.TensorType(name='speed',
                              shape=(1,), dtype=np.float32),
            ],
            outputs=[
                ct.TensorType(name='mu'),
                ct.TensorType(name='mask'),
            ],
            convert_to='mlprogram',
            minimum_deployment_target=ct.target.macOS12,
            compute_precision=ct.precision.FLOAT16,
            compute_units=ct.ComputeUnit.ALL,
        )
    except Exception as e:
        print(f'  CoreML conversion failed: {e}')
        print('  Retrying with FLOAT32 precision...')
        enc_ml = ct.convert(
            traced,
            inputs=[
                ct.TensorType(name='input_ids',
                              shape=(1, MAX_INPUT_TOKENS), dtype=np.int64),
                ct.TensorType(name='x_lengths',
                              shape=(1,), dtype=np.int64),
                ct.TensorType(name='speed',
                              shape=(1,), dtype=np.float32),
            ],
            outputs=[
                ct.TensorType(name='mu'),
                ct.TensorType(name='mask'),
            ],
            convert_to='mlprogram',
            minimum_deployment_target=ct.target.macOS12,
            compute_precision=ct.precision.FLOAT32,
            compute_units=ct.ComputeUnit.ALL,
        )

    pkg_path = os.path.join(coreml_dir, f'matcha_encoder_{bucket_name}.mlpackage')
    enc_ml.save(pkg_path)
    print(f'  Saved: {pkg_path}')

    if not no_validate:
        print('  Validating CoreML encoder...')
        for n_tok in [10, 30, MAX_INPUT_TOKENS // 4]:
            n_tok = min(n_tok, MAX_INPUT_TOKENS)
            ids_np = np.zeros((1, MAX_INPUT_TOKENS), dtype=np.int32)
            ids_np[0, :n_tok] = np.random.randint(1, 80, n_tok)
            lens_np = np.array([n_tok], dtype=np.int32)
            spd_np = np.array([1.0], dtype=np.float32)
            t0 = time.time()
            out = enc_ml.predict({
                'input_ids': ids_np,
                'x_lengths': lens_np,
                'speed': spd_np,
            })
            elapsed = (time.time() - t0) * 1000
            mu_shape = out['mu'].shape
            mask_shape = out['mask'].shape
            mu_finite = np.isfinite(out['mu']).all()
            print(f'  n_tok={n_tok}: mu={mu_shape}, mask={mask_shape}, '
                  f'finite={mu_finite}, {elapsed:.0f}ms')
            if mu_shape != (1, MEL_BINS, T_mel):
                print(f'  ERROR: Expected mu shape (1, {MEL_BINS}, {T_mel}), got {mu_shape}')
            if not mu_finite:
                print('  ERROR: Non-finite values in encoder output!')

            ids_pt = torch.tensor(ids_np, dtype=torch.long)
            lens_pt = torch.tensor(lens_np, dtype=torch.long)
            spd_pt = torch.tensor(spd_np, dtype=torch.float32)
            with torch.no_grad():
                mu_pt, mask_pt = wrapper(ids_pt, lens_pt, spd_pt)
            mse = float(((mu_pt.numpy() - out['mu']) ** 2).mean())
            status = 'OK' if mse < 0.01 else ('ACCEPTABLE — FLOAT16 precision loss' if mse < 0.05 else 'CRITICAL')
            print(f'  MSE vs PyTorch: {mse:.6f} {status}')
            if mse > 0.05:
                print(f'  ERROR: CoreML encoder MSE {mse:.6f} exceeds 0.05 threshold.')
                print('  Try re-exporting with FLOAT32 precision or fewer architectural modifications.')
                sys.exit(1)

    mlmodelc = _compile_mlpackage_to_mlmodelc(pkg_path, coreml_dir)
    return mlmodelc or pkg_path


def export_flow(model, coreml_dir, bucket, n_steps, no_validate=False):
    """Export baked ODE flow model for a specific bucket. Returns path to .mlmodelc."""
    import torch
    import numpy as np
    import coremltools as ct

    bucket_name = bucket['name']
    T_mel = bucket['frames']
    print(f'\n=== Exporting flow (bucket={bucket_name}, T_mel={T_mel}, steps={n_steps}) ===')
    os.makedirs(coreml_dir, exist_ok=True)

    wrapper = _build_flow_wrapper(model, T_mel, n_steps, bucket_name)

    z = torch.randn(1, MEL_BINS, T_mel)
    mu = torch.randn(1, MEL_BINS, T_mel)
    mask = torch.ones(1, 1, T_mel)

    print('  Testing forward pass...')
    try:
        with torch.no_grad():
            mel_ref = wrapper(z, mu, mask)
        print(f'  Forward OK: mel={mel_ref.shape}')
    except Exception as e:
        print(f'  forward_flow attribute failed: {e}. Trying fallback wrapper...')
        wrapper = _build_flow_wrapper_fallback(model, T_mel, n_steps, bucket_name)
        try:
            with torch.no_grad():
                mel_ref = wrapper(z, mu, mask)
            print(f'  Fallback OK: mel={mel_ref.shape}')
        except Exception as e2:
            print(f'  Flow forward failed (both wrappers): {e2}')
            print('  Attempting decoder attribute scan...')
            print(f'  Decoder type: {type(model.decoder)}')
            print(f'  Decoder attrs: {[a for a in dir(model.decoder) if not a.startswith("_")][:20]}')
            raise

    print('  Tracing flow...')
    try:
        with torch.no_grad():
            traced = torch.jit.trace(wrapper, (z, mu, mask), strict=False)
        with torch.no_grad():
            mel_traced = traced(z, mu, mask)
        mse_trace = float(((mel_ref - mel_traced) ** 2).mean())
        print(f'  Trace MSE vs original: {mse_trace:.6f}')
        if mse_trace > 0.01:
            print('  WARNING: Trace MSE > 0.01. Trying torch.jit.script...')
            raise RuntimeError('MSE too high, try script')
    except Exception:
        print('  Using torch.jit.script for flow (dynamic branches detected)...')
        try:
            traced = torch.jit.script(wrapper)
        except Exception as e_script:
            print(f'  torch.jit.script failed: {e_script}')
            print('  ERROR: Trace MSE exceeds 0.01 and script() failed. Cannot produce valid flow model.')
            print('  Retry with --steps set to a smaller value, or debug the flow wrapper.')
            sys.exit(1)

    print('  Converting flow to CoreML...')
    try:
        flow_ml = ct.convert(
            traced,
            inputs=[
                ct.TensorType(name='z',    shape=(1, MEL_BINS, T_mel), dtype=np.float32),
                ct.TensorType(name='mu',   shape=(1, MEL_BINS, T_mel), dtype=np.float32),
                ct.TensorType(name='mask', shape=(1, 1, T_mel),        dtype=np.float32),
            ],
            outputs=[
                ct.TensorType(name='mel'),
            ],
            convert_to='mlprogram',
            minimum_deployment_target=ct.target.macOS12,
            compute_precision=ct.precision.FLOAT16,
            compute_units=ct.ComputeUnit.ALL,
        )
    except Exception as e:
        print(f'  CoreML conversion failed: {e}')
        print('  Retrying with CPU compute units...')
        flow_ml = ct.convert(
            traced,
            inputs=[
                ct.TensorType(name='z',    shape=(1, MEL_BINS, T_mel), dtype=np.float32),
                ct.TensorType(name='mu',   shape=(1, MEL_BINS, T_mel), dtype=np.float32),
                ct.TensorType(name='mask', shape=(1, 1, T_mel),        dtype=np.float32),
            ],
            outputs=[
                ct.TensorType(name='mel'),
            ],
            convert_to='mlprogram',
            minimum_deployment_target=ct.target.macOS12,
            compute_precision=ct.precision.FLOAT32,
            compute_units=ct.ComputeUnit.CPU_ONLY,
        )

    pkg_path = os.path.join(coreml_dir, f'matcha_flow_{bucket_name}.mlpackage')
    flow_ml.save(pkg_path)
    print(f'  Saved: {pkg_path}')

    if not no_validate:
        print('  Validating CoreML flow...')
        for desc, t_actual in [('short', T_mel // 4), ('medium', T_mel // 2), ('full', T_mel)]:
            z_np = np.random.randn(1, MEL_BINS, T_mel).astype(np.float32)
            mu_np = np.random.randn(1, MEL_BINS, T_mel).astype(np.float32)
            mask_np = np.zeros((1, 1, T_mel), dtype=np.float32)
            mask_np[0, 0, :t_actual] = 1.0

            t0 = time.time()
            out = flow_ml.predict({'z': z_np, 'mu': mu_np, 'mask': mask_np})
            elapsed = (time.time() - t0) * 1000
            mel_out = out['mel']
            mel_finite = np.isfinite(mel_out).all()
            print(f'  {desc} (t_actual={t_actual}): mel={mel_out.shape}, '
                  f'finite={mel_finite}, {elapsed:.0f}ms')

            z_pt = torch.from_numpy(z_np)
            mu_pt = torch.from_numpy(mu_np)
            mask_pt = torch.from_numpy(mask_np)
            with torch.no_grad():
                mel_pt = wrapper(z_pt, mu_pt, mask_pt).numpy()
            mse = float(((mel_pt - mel_out) ** 2).mean())
            status = 'OK' if mse < 0.01 else ('HIGH — FLOAT16 precision loss' if mse < 0.05 else 'CRITICAL')
            print(f'  MSE vs PyTorch: {mse:.6f} {status}')
            if mse > 0.01:
                print(f'  ERROR: CoreML flow MSE {mse:.6f} exceeds 0.01 threshold.')
                print('  The baked ODE flow is numerically inaccurate. Debugging steps:')
                print('    1. Re-export with FLOAT32 (edit compute_precision in export_flow)')
                print('    2. Try fewer ODE steps (--steps 5)')
                print('    3. Switch from trace() to script() for the flow wrapper')
                sys.exit(1)

    mlmodelc = _compile_mlpackage_to_mlmodelc(pkg_path, coreml_dir)
    return mlmodelc or pkg_path


def export_vocoder(model, coreml_dir, sample_T_mel=280, no_validate=False):
    """Export HiFi-GAN vocoder. Returns path to .mlmodelc."""
    import torch
    import numpy as np
    import coremltools as ct

    print(f'\n=== Exporting HiFi-GAN vocoder ===')
    os.makedirs(coreml_dir, exist_ok=True)

    if not hasattr(model, 'vocoder'):
        print('  WARNING: Model has no .vocoder attribute. Skipping vocoder export.')
        print('  The matcha-service will need a separately provided HiFi-GAN vocoder.')
        return None

    wrapper = _build_vocoder_wrapper(model)
    wrapper.eval()

    mel_in = torch.randn(1, MEL_BINS, sample_T_mel)
    print('  Testing vocoder forward pass...')
    try:
        with torch.no_grad():
            wav_ref = wrapper(mel_in)
        print(f'  Forward OK: wav={wav_ref.shape}')
    except Exception as e:
        print(f'  Vocoder forward failed: {e}')
        print(f'  Vocoder type: {type(model.vocoder)}')
        print(f'  Vocoder attrs: {[a for a in dir(model.vocoder) if not a.startswith("_")][:20]}')
        raise

    T_mel_fixed = sample_T_mel if sample_T_mel > 0 else BUCKETS[-1]['frames']
    mel_in_fixed = torch.randn(1, MEL_BINS, T_mel_fixed)

    print('  Tracing vocoder...')
    try:
        with torch.no_grad():
            traced = torch.jit.trace(wrapper, (mel_in_fixed,), strict=False)
        with torch.no_grad():
            wav_traced = traced(mel_in_fixed)
        with torch.no_grad():
            mse = float(((wrapper(mel_in_fixed) - wav_traced) ** 2).mean())
        print(f'  Trace MSE: {mse:.6f}')
        if mse > 0.01:
            print('  Trace MSE > 0.01. Trying torch.jit.script...')
            raise RuntimeError('MSE too high, try script')
    except Exception:
        try:
            traced = torch.jit.script(wrapper)
        except Exception as e_script:
            print(f'  torch.jit.script failed: {e_script}')
            print('  ERROR: Vocoder trace MSE exceeds 0.01 and script() failed. Cannot export vocoder.')
            sys.exit(1)

    print(f'  Converting vocoder to CoreML (fixed T_mel={T_mel_fixed})...')
    try:
        voc_ml = ct.convert(
            traced,
            inputs=[
                ct.TensorType(name='mel', shape=(1, MEL_BINS, T_mel_fixed),
                              dtype=np.float32),
            ],
            outputs=[
                ct.TensorType(name='waveform'),
            ],
            convert_to='mlprogram',
            minimum_deployment_target=ct.target.macOS12,
            compute_precision=ct.precision.FLOAT16,
            compute_units=ct.ComputeUnit.ALL,
        )
    except Exception as e:
        print(f'  CoreML conversion failed: {e}')
        print('  Retrying with FLOAT32...')
        voc_ml = ct.convert(
            traced,
            inputs=[
                ct.TensorType(name='mel', shape=(1, MEL_BINS, T_mel_fixed),
                              dtype=np.float32),
            ],
            outputs=[
                ct.TensorType(name='waveform'),
            ],
            convert_to='mlprogram',
            minimum_deployment_target=ct.target.macOS12,
            compute_precision=ct.precision.FLOAT32,
            compute_units=ct.ComputeUnit.CPU_ONLY,
        )

    pkg_path = os.path.join(coreml_dir, 'matcha_vocoder.mlpackage')
    voc_ml.save(pkg_path)
    print(f'  Saved: {pkg_path}')

    if not no_validate:
        print('  Validating CoreML vocoder (3 mel lengths: short, medium, full)...')
        validation_lengths = [
            ('short',  max(1, T_mel_fixed // 4)),
            ('medium', max(1, T_mel_fixed // 2)),
            ('full',   T_mel_fixed),
        ]
        for desc, t_active in validation_lengths:
            mel_np = np.random.randn(1, MEL_BINS, T_mel_fixed).astype(np.float32)
            mel_np[0, :, t_active:] = 0.0
            t0 = time.time()
            out = voc_ml.predict({'mel': mel_np})
            elapsed = (time.time() - t0) * 1000
            wav_out = out['waveform']
            mel_pt = torch.from_numpy(mel_np)
            with torch.no_grad():
                wav_pt = wrapper(mel_pt).numpy()
            mse = float(((wav_pt - wav_out) ** 2).mean())
            status = 'OK' if mse < 0.01 else ('HIGH — FLOAT16 precision loss' if mse < 0.05 else 'CRITICAL')
            print(f'  [{desc}] waveform={wav_out.shape}, finite={np.isfinite(wav_out).all()}, '
                  f'{elapsed:.0f}ms, MSE={mse:.6f} {status}')
            if mse > 0.01:
                print(f'  ERROR: CoreML vocoder MSE {mse:.6f} exceeds 0.01 threshold ({desc} input).')
                print('  Try re-exporting the vocoder with FLOAT32 compute precision.')
                sys.exit(1)

    mlmodelc = _compile_mlpackage_to_mlmodelc(pkg_path, coreml_dir)
    return mlmodelc or pkg_path


def save_vocab(model, vocab_path):
    """Save phoneme-to-ID vocabulary from the model checkpoint."""
    print(f'\n=== Saving vocab.json ===')

    vocab = None
    for attr in ['tokenizer', 'text_tokenizer', 'encoder', 'phone_encoder']:
        if hasattr(model, attr):
            obj = getattr(model, attr)
            if hasattr(obj, 'vocab') and isinstance(obj.vocab, dict):
                vocab = obj.vocab
                break
            if hasattr(obj, 'token2id') and isinstance(obj.token2id, dict):
                vocab = obj.token2id
                break
            if hasattr(obj, 'vocabulary') and isinstance(obj.vocabulary, dict):
                vocab = obj.vocabulary
                break

    if vocab is None and hasattr(model, 'encoder'):
        enc = model.encoder
        for sub_attr in ['text_embedding', 'embedding', 'emb']:
            if hasattr(enc, sub_attr):
                emb = getattr(enc, sub_attr)
                n = emb.weight.shape[0] if hasattr(emb, 'weight') else None
                if n:
                    print(f'  Inferred vocab size from {sub_attr}: {n}')
                    vocab = {str(i): i for i in range(n)}
                    break

    if vocab is None:
        print('  WARNING: Could not extract phoneme vocabulary from model.')
        print('  Creating a placeholder vocab.json with IPA symbols.')
        ipa_symbols = (
            ['_', '^', '$', ' '] +
            list('aeiouAEIOU') +
            ['b', 'd', 'f', 'g', 'h', 'j', 'k', 'l', 'm', 'n',
             'p', 'r', 's', 't', 'v', 'w', 'z', 'ŋ', 'ʃ', 'ʒ',
             'ʔ', 'ç', 'x', 'θ', 'ð', 'ʁ', 'œ', 'ø', 'ü', 'ä', 'ö',
             'ː', '̃', 'ˈ', 'ˌ', '|', '‖']
        )
        vocab = {sym: i for i, sym in enumerate(ipa_symbols)}
        print(f'  Placeholder vocab: {len(vocab)} symbols')

    with open(vocab_path, 'w', encoding='utf-8') as f:
        json.dump(vocab, f, ensure_ascii=False, indent=2)
    print(f'  Saved vocab.json: {vocab_path} ({len(vocab)} entries)')
    return vocab


def print_summary(results, coreml_dir, vocab_path, is_english_fallback):
    print('\n' + '=' * 60)
    print('=== Export Summary ===')
    print('=' * 60)

    if is_english_fallback:
        print()
        print('  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!')
        print('  LANGUAGE WARNING: Models are based on ENGLISH checkpoint.')
        print('  German speech quality will be degraded.')
        print('  Run fine-tuning on Thorsten-Voice for production German TTS.')
        print('  !!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!!')
        print()

    print(f'\n  Output directory: {coreml_dir}')
    for label, path in results.items():
        if path:
            exists = os.path.exists(path)
            size_mb = 0
            if exists:
                if os.path.isdir(path):
                    size_mb = sum(
                        os.path.getsize(os.path.join(dp, f))
                        for dp, dn, fns in os.walk(path) for f in fns
                    ) / 1e6
                else:
                    size_mb = os.path.getsize(path) / 1e6
            status = f'OK ({size_mb:.1f} MB)' if exists else 'MISSING'
            print(f'  {label:<45} {status}')
        else:
            print(f'  {label:<45} SKIPPED')

    if vocab_path and os.path.exists(vocab_path):
        size = os.path.getsize(vocab_path)
        print(f'  {"vocab.json":<45} OK ({size} bytes)')

    print()
    print('  Usage with matcha-service:')
    print(f'    WHISPERTALK_MODELS_DIR=bin/models bin/matcha-service')
    print()
    print('  matcha-service loads:')
    print(f'    {coreml_dir}/matcha_encoder.mlmodelc    (= 10s bucket, used for all lengths)')
    print(f'    {coreml_dir}/matcha_flow_{{3s,5s,10s}}.mlmodelc')
    print(f'    {coreml_dir}/matcha_vocoder.mlmodelc')
    print(f'    {os.path.dirname(coreml_dir)}/vocab.json')
    print()
    print('  NOTE: matcha_encoder.mlmodelc is the 10s model (938 mel frames).')
    print('  For per-bucket encoder selection, use matcha_encoder_{{3s,5s,10s}}.mlmodelc')
    print('  and update matcha-service.cpp to load bucket-specific encoders.')


def main():
    args = parse_args()

    buckets = [dict(b) for b in BUCKETS]

    if args.bucket_sizes:
        try:
            sizes = [int(s.strip()) for s in args.bucket_sizes.split(',')]
            if len(sizes) != 3:
                raise ValueError('Need exactly 3 bucket sizes')
            for i in range(len(buckets)):
                buckets[i]['frames'] = sizes[i]
        except Exception as e:
            print(f'ERROR: Invalid --bucket-sizes: {e}')
            sys.exit(1)

    if args.output_dir:
        coreml_dir = args.output_dir
        vocab_dir = coreml_dir
    else:
        base = os.path.join(ROOT_DIR, 'bin', 'models', DEFAULT_OUTPUT_SUBDIR)
        coreml_dir = os.path.join(base, 'coreml')
        vocab_dir = base

    os.makedirs(coreml_dir, exist_ok=True)
    os.makedirs(vocab_dir, exist_ok=True)

    checkpoint_dir = os.path.join(ROOT_DIR, 'bin', 'models', DEFAULT_OUTPUT_SUBDIR, 'checkpoint')

    if not args.no_install:
        conda_python = ensure_conda_env()
        if os.path.realpath(sys.executable) != os.path.realpath(conda_python):
            print(f'\n  Re-launching with conda python: {conda_python}')
            # NOTE: if new args are added to parse_args(), add them here too.
            relaunch_args = [conda_python, __file__, '--no-install']
            if args.checkpoint:
                relaunch_args += ['--checkpoint', args.checkpoint]
            relaunch_args += ['--output-dir', coreml_dir]
            if args.steps != DEFAULT_STEPS:
                relaunch_args += ['--steps', str(args.steps)]
            if args.encoder_only:
                relaunch_args.append('--encoder-only')
            if args.flow_only:
                relaunch_args.append('--flow-only')
            if args.vocoder_only:
                relaunch_args.append('--vocoder-only')
            try:
                os.execv(conda_python, relaunch_args)
            except OSError as e:
                print(f'[ERROR] Could not re-launch with {conda_python}: {e}')
                sys.exit(1)

    if args.checkpoint:
        checkpoint_path = args.checkpoint
        is_english_fallback = False
        print(f'\n=== Using provided checkpoint: {checkpoint_path} ===')
        if not os.path.isfile(checkpoint_path):
            print(f'ERROR: Checkpoint not found: {checkpoint_path}')
            sys.exit(1)
    else:
        os.makedirs(checkpoint_dir, exist_ok=True)
        checkpoint_path, is_english_fallback = download_checkpoint(checkpoint_dir)

    model, hparams = load_matcha_model(checkpoint_path)

    do_all = not (args.encoder_only or args.flow_only or args.vocoder_only)
    do_encoder = do_all or args.encoder_only
    do_flow = do_all or args.flow_only
    do_vocoder = do_all or args.vocoder_only

    results = {}

    if do_encoder:
        _patch_matcha_for_torchscript()
        for bucket in buckets:
            path = export_encoder(model, coreml_dir, bucket)
            results[f'matcha_encoder_{bucket["name"]}.mlmodelc'] = path

        enc_10s_src = os.path.join(coreml_dir, 'matcha_encoder_10s.mlmodelc')
        enc_default = os.path.join(coreml_dir, 'matcha_encoder.mlmodelc')
        if os.path.exists(enc_10s_src) and enc_10s_src != enc_default:
            if os.path.exists(enc_default):
                shutil.rmtree(enc_default) if os.path.isdir(enc_default) else os.remove(enc_default)
            if os.path.isdir(enc_10s_src):
                shutil.copytree(enc_10s_src, enc_default)
            else:
                shutil.copy2(enc_10s_src, enc_default)
            print(f'  Copied 10s encoder as default: {enc_default}')
            results['matcha_encoder.mlmodelc (default, =10s)'] = enc_default
        elif not os.path.exists(enc_10s_src):
            enc_pkg_10s = os.path.join(coreml_dir, 'matcha_encoder_10s.mlpackage')
            if os.path.exists(enc_pkg_10s):
                enc_pkg_default = os.path.join(coreml_dir, 'matcha_encoder.mlpackage')
                if not os.path.exists(enc_pkg_default):
                    shutil.copytree(enc_pkg_10s, enc_pkg_default)
                    print(f'  Copied 10s encoder package as default: {enc_pkg_default}')
                    results['matcha_encoder.mlpackage (default, =10s)'] = enc_pkg_default

    if do_flow:
        for bucket in buckets:
            path = export_flow(model, coreml_dir, bucket, args.steps)
            results[f'matcha_flow_{bucket["name"]}.mlmodelc'] = path

    if do_vocoder:
        if not hasattr(model, 'vocoder'):
            _load_and_attach_vocoder(model, checkpoint_dir)
        voc_path = export_vocoder(model, coreml_dir, sample_T_mel=buckets[-1]['frames'])
        results['matcha_vocoder.mlmodelc'] = voc_path

    vocab_path = os.path.join(vocab_dir, 'vocab.json')
    save_vocab(model, vocab_path)

    print_summary(results, coreml_dir, vocab_path, is_english_fallback)

    missing = [k for k, v in results.items() if v and not os.path.exists(v)]
    if missing:
        print(f'\n  WARNING: {len(missing)} artifact(s) missing: {missing}')
        sys.exit(1)

    print('\n=== Export complete ===')


if __name__ == '__main__':
    main()
