#!/usr/bin/env python3
"""
Export all Kokoro TTS German models for the WhisperTalk C++ CoreML pipeline.

This script is self-contained: it downloads the Kokoro model, installs all
dependencies in a conda environment, and exports everything needed to run
kokoro-service in C++.

Exported artifacts (in bin/models/kokoro-german/):
  - coreml/kokoro_duration.mlmodelc    CoreML duration model (ANE)
  - decoder_variants/kokoro_decoder_split_{3s,5s,10s}.mlmodelc  CoreML decoder (ANE)
  - decoder_variants/kokoro_har_{3s,5s,10s}.pt                  HAR TorchScript (CPU)
  - decoder_variants/split_config.json  Bucket dimensions
  - {df_eva,dm_bernd}_voice.bin         Raw float32 voice embeddings
  - vocab.json                          Phoneme-to-ID mapping

Prerequisites:
  - macOS with Apple Silicon
  - conda (miniconda or miniforge)
  - Internet access (for model download)

Usage:
  python3 scripts/export_kokoro_models.py           # Full export (installs deps)
  python3 scripts/export_kokoro_models.py --no-install  # Skip dependency install
  python3 scripts/export_kokoro_models.py --duration-only
  python3 scripts/export_kokoro_models.py --decoder-only
  python3 scripts/export_kokoro_models.py --voices-only

Architecture:
  Duration model: BERT + prosody prediction -> fixed 512-token input, runs on ANE
  Decoder: Vocoder with HAR source split, 3 bucketed sizes (3s/5s/10s), runs on ANE
  HAR models: SineGen+STFT source (uses complex ops), TorchScript on CPU (~20KB each)
  Alignment: repeat_interleave done on CPU in C++ (data-dependent, incompatible with CoreML)

Tested with:
  - conda env: torch==2.5.0, coremltools==8.3.0, numpy==1.26.4
  - kokoro package from https://github.com/Thomcle/kokoro_german or pip install kokoro
"""
import os
import sys
import json
import time
import shutil
import subprocess
import argparse
import importlib
import importlib.util
import types

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.join(SCRIPT_DIR, '..')
MODELS_DIR = os.path.join(ROOT_DIR, 'bin', 'models', 'kokoro-german')
CONFIG_PATH = os.path.join(MODELS_DIR, 'config.json')
MODEL_PATH = os.path.join(MODELS_DIR, 'kokoro-german-v1_1-de.pth')
COREML_DIR = os.path.join(MODELS_DIR, 'coreml')
DECODER_DIR = os.path.join(MODELS_DIR, 'decoder_variants')

CONDA_ENV_NAME = 'kokoro_coreml'
REQUIRED_TORCH = '2.5'
REQUIRED_COREMLTOOLS = '8.3'

MODEL_URL = 'https://huggingface.co/Tundragoon/Kokoro-German/resolve/main/kokoro-german-v1_1-de.pth'
CONFIG_URL = 'https://huggingface.co/Tundragoon/Kokoro-German/resolve/main/config.json'
VOICE_URLS = {
    'df_eva': 'https://huggingface.co/Tundragoon/Kokoro-German/resolve/main/voices/df_eva.pt',
    'dm_bernd': 'https://huggingface.co/Tundragoon/Kokoro-German/resolve/main/voices/dm_bernd.pt',
}

BUCKETS = [
    {"name": "3s",  "asr_frames": 72,  "f0_frames": 144},
    {"name": "5s",  "asr_frames": 120, "f0_frames": 240},
    {"name": "10s", "asr_frames": 240, "f0_frames": 480},
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
    run_cmd(f"{conda_python} -m pip install -q kokoro")

    version_check = run_cmd(
        f'{conda_python} -c "import torch, coremltools as ct, numpy as np; '
        f'print(f\'torch={{torch.__version__}} ct={{ct.__version__}} np={{np.__version__}}\')"'
    )
    print(f"  Versions: {version_check.stdout.strip()}")
    return conda_python


def download_models():
    print("\n=== Downloading models ===")
    os.makedirs(MODELS_DIR, exist_ok=True)
    os.makedirs(os.path.join(MODELS_DIR, 'voices'), exist_ok=True)

    hf_token = os.environ.get('HF_TOKEN', '')
    if not hf_token:
        print("  HF_TOKEN not set. The Kokoro model repo may require authentication.")
        print("  Get a token at: https://huggingface.co/settings/tokens")
        hf_token = input("  Paste your HuggingFace token (or press Enter to try without): ").strip()
    auth_header = f'-H "Authorization: Bearer {hf_token}"' if hf_token else ''

    downloads = [
        (MODEL_PATH, MODEL_URL),
        (CONFIG_PATH, CONFIG_URL),
    ]
    for voice_name, url in VOICE_URLS.items():
        downloads.append((os.path.join(MODELS_DIR, 'voices', f'{voice_name}.pt'), url))

    for path, url in downloads:
        if os.path.exists(path) and os.path.getsize(path) > 1000:
            print(f"  Already exists: {os.path.basename(path)}")
            continue
        if os.path.exists(path) and os.path.getsize(path) <= 1000:
            os.remove(path)
        print(f"  Downloading {os.path.basename(path)}...")
        run_cmd(f'curl -L {auth_header} -o "{path}" "{url}"')
        if not os.path.exists(path) or os.path.getsize(path) < 1000:
            print(f"  FAILED: {path} is missing or too small")
            if not hf_token:
                print(f"  The HuggingFace repo may require authentication.")
                print(f"  Set HF_TOKEN and retry: export HF_TOKEN=hf_...")
            sys.exit(1)
        print(f"  OK ({os.path.getsize(path) / 1e6:.1f} MB)")


def load_kokoro_model(disable_complex=True):
    search_dirs = []
    conda_base = os.environ.get('CONDA_PREFIX', '')
    if conda_base:
        for pyver in ['python3.11', 'python3.10', 'python3.12']:
            d = os.path.join(conda_base, 'lib', pyver, 'site-packages', 'kokoro')
            if os.path.isdir(d):
                search_dirs.append(d)

    for d in ['/opt/homebrew/lib', '/usr/local/lib', '/usr/lib']:
        if not os.path.isdir(d):
            continue
        for pyver in sorted(os.listdir(d), reverse=True):
            if not pyver.startswith('python3'):
                continue
            candidate = os.path.join(d, pyver, 'site-packages', 'kokoro')
            if os.path.isdir(candidate):
                search_dirs.append(candidate)

    for base in [
        '/opt/homebrew/Caskroom/miniconda/base/envs/kokoro_coreml/lib',
        os.path.expanduser('~/miniconda3/envs/kokoro_coreml/lib'),
        os.path.expanduser('~/miniforge3/envs/kokoro_coreml/lib'),
    ]:
        if not os.path.isdir(base):
            continue
        for pyver in sorted(os.listdir(base), reverse=True):
            if not pyver.startswith('python3'):
                continue
            d = os.path.join(base, pyver, 'site-packages', 'kokoro')
            if os.path.isdir(d):
                search_dirs.append(d)

    kokoro_pkg_dir = None
    for d in search_dirs:
        if os.path.isdir(d):
            has_custom_stft = os.path.isfile(os.path.join(d, 'custom_stft.py'))
            if disable_complex and not has_custom_stft:
                continue
            kokoro_pkg_dir = d
            break

    if not kokoro_pkg_dir:
        for d in search_dirs:
            if os.path.isdir(d):
                kokoro_pkg_dir = d
                if not os.path.isfile(os.path.join(d, 'custom_stft.py')):
                    disable_complex = False
                    print(f"  WARNING: custom_stft.py not found, using disable_complex=False")
                break

    if not kokoro_pkg_dir:
        try:
            from kokoro.model import KModel as _
            import kokoro
            kokoro_pkg_dir = os.path.dirname(kokoro.__file__)
        except ImportError:
            raise RuntimeError("Cannot find kokoro package. Install via: pip install kokoro")

    print(f"  Using kokoro from: {kokoro_pkg_dir} (disable_complex={disable_complex})")
    kokoro = types.ModuleType('kokoro')
    kokoro.__path__ = [kokoro_pkg_dir]
    kokoro.__package__ = 'kokoro'
    sys.modules['kokoro'] = kokoro

    submods = ['istftnet', 'modules', 'model']
    if disable_complex:
        submods = ['custom_stft'] + submods
    for submod in submods:
        fpath = os.path.join(kokoro_pkg_dir, f'{submod}.py')
        if not os.path.isfile(fpath):
            continue
        spec = importlib.util.spec_from_file_location(f'kokoro.{submod}', fpath)
        if spec and spec.loader:
            mod = importlib.util.module_from_spec(spec)
            sys.modules[f'kokoro.{submod}'] = mod
            spec.loader.exec_module(mod)

    from kokoro.model import KModel
    print(f"  Loading model from {MODEL_PATH}...")
    kmodel = KModel(config=CONFIG_PATH, model=MODEL_PATH, disable_complex=disable_complex)
    kmodel.eval()
    return kmodel


def remove_training_ops(model):
    import torch.nn as nn
    for name, module in model.named_modules():
        if isinstance(module, nn.Dropout):
            parent_name = '.'.join(name.split('.')[:-1])
            child_name = name.split('.')[-1]
            parent = model.get_submodule(parent_name) if parent_name else model
            setattr(parent, child_name, nn.Identity())
        elif isinstance(module, nn.BatchNorm1d):
            module.eval()
            module.track_running_stats = False
        elif isinstance(module, nn.LSTM):
            module.eval()


_RSQRT_2 = 1.0 / (2.0 ** 0.5)

def patch_rsqrt(model):
    from kokoro.istftnet import AdainResBlk1d
    def _patched_forward(self, x, s):
        out = self._residual(x, s)
        out = (out + self._shortcut(x)) * _RSQRT_2
        return out
    for module in model.modules():
        if isinstance(module, AdainResBlk1d):
            module.forward = types.MethodType(_patched_forward, module)


def export_duration_model(kmodel):
    import torch
    import torch.nn as nn
    import numpy as np
    import coremltools as ct
    from kokoro.modules import AdaLayerNorm

    print("\n=== Exporting Duration Model (CoreML) ===")

    class CoreMLFriendlyTextEncoder(nn.Module):
        def __init__(self, original_encoder):
            super().__init__()
            self.embedding = original_encoder.embedding
            self.cnn = original_encoder.cnn
            self.lstm = original_encoder.lstm

        def forward(self, x, input_lengths, m):
            x = self.embedding(x)
            x = x.transpose(1, 2)
            m = m.unsqueeze(1)
            x.masked_fill_(m, 0.0)
            for c in self.cnn:
                x = c(x)
                x.masked_fill_(m, 0.0)
            x = x.transpose(1, 2)
            self.lstm.flatten_parameters()
            x, _ = self.lstm(x)
            x = x.transpose(-1, -2)
            x.masked_fill_(m, 0.0)
            return x

    class CoreMLFriendlyDurationEncoder(nn.Module):
        def __init__(self, original_encoder):
            super().__init__()
            self.lstms = original_encoder.lstms
            self.dropout = original_encoder.dropout

        def forward(self, x, style, text_lengths, m):
            masks = m
            x = x.permute(2, 0, 1)
            seq_len = x.shape[0]
            s = style.unsqueeze(0).repeat(seq_len, 1, 1)
            x = torch.cat([x, s], axis=-1)
            x.masked_fill_(masks.unsqueeze(-1).transpose(0, 1), 0.0)
            x = x.transpose(0, 1)
            x = x.transpose(-1, -2)
            for block in self.lstms:
                if isinstance(block, AdaLayerNorm):
                    x = block(x.transpose(-1, -2), style).transpose(-1, -2)
                    x = torch.cat([x, s.permute(1, 2, 0)], axis=1)
                    x.masked_fill_(masks.unsqueeze(-1).transpose(-1, -2), 0.0)
                else:
                    x = x.transpose(-1, -2)
                    block.flatten_parameters()
                    x, _ = block(x)
                    x = nn.functional.dropout(x, p=self.dropout, training=False)
                    x = x.transpose(-1, -2)
            return x.transpose(-1, -2)

    class DurationModel(nn.Module):
        def __init__(self, kmodel):
            super().__init__()
            self.kmodel = kmodel
            self.kmodel.text_encoder = CoreMLFriendlyTextEncoder(kmodel.text_encoder)
            self.kmodel.predictor.text_encoder = CoreMLFriendlyDurationEncoder(kmodel.predictor.text_encoder)
            if hasattr(self.kmodel.bert.embeddings, 'token_type_ids'):
                delattr(self.kmodel.bert.embeddings, 'token_type_ids')

        def forward(self, input_ids, ref_s, speed, attention_mask):
            k = self.kmodel
            input_lengths = attention_mask.sum(dim=-1).to(torch.long)
            text_mask = attention_mask == 0
            token_type_ids = torch.zeros_like(input_ids)
            bert_dur = k.bert(input_ids, attention_mask=attention_mask, token_type_ids=token_type_ids)
            d_en = k.bert_encoder(bert_dur).transpose(-1, -2)
            s = ref_s[:, 128:]
            d = k.predictor.text_encoder(d_en, s, input_lengths, text_mask)
            x, _ = k.predictor.lstm(d)
            duration = k.predictor.duration_proj(x)
            duration = torch.sigmoid(duration).sum(axis=-1) / speed
            pred_dur = torch.round(duration).clamp(min=1).long()
            t_en = k.text_encoder(input_ids, input_lengths, text_mask)
            ref_s_out = ref_s + torch.zeros_like(ref_s)
            return pred_dur, d, t_en, s, ref_s_out

    duration_model = DurationModel(kmodel)
    duration_model.eval()
    remove_training_ops(duration_model)
    for module in duration_model.modules():
        module.eval()

    T = 512
    input_ids = torch.randint(0, 100, (1, T), dtype=torch.int32)
    ref_s = torch.zeros(1, 256, dtype=torch.float32)
    speed = torch.tensor([1.0], dtype=torch.float32)
    attention_mask = torch.ones(1, T, dtype=torch.int32)

    print("  Testing forward pass...")
    with torch.no_grad():
        outputs = duration_model(input_ids, ref_s, speed, attention_mask)
        print(f"  Output shapes: {[o.shape for o in outputs]}")

    print("  Tracing model...")
    with torch.no_grad():
        traced = torch.jit.trace(duration_model, (input_ids, ref_s, speed, attention_mask), strict=False)

    print("  Converting to CoreML...")
    duration_ml = ct.convert(
        traced,
        inputs=[
            ct.TensorType(name="input_ids", shape=(1, 512), dtype=np.int32),
            ct.TensorType(name="ref_s", shape=(1, 256), dtype=np.float32),
            ct.TensorType(name="speed", shape=(1,), dtype=np.float32),
            ct.TensorType(name="attention_mask", shape=(1, 512), dtype=np.int32),
        ],
        outputs=[
            ct.TensorType(name="pred_dur"),
            ct.TensorType(name="d"),
            ct.TensorType(name="t_en"),
            ct.TensorType(name="s"),
            ct.TensorType(name="ref_s_out"),
        ],
        convert_to="mlprogram",
        minimum_deployment_target=ct.target.macOS12,
        compute_precision=ct.precision.FLOAT16,
        compute_units=ct.ComputeUnit.ALL,
    )

    os.makedirs(COREML_DIR, exist_ok=True)
    out_path = os.path.join(COREML_DIR, "kokoro_duration.mlpackage")
    duration_ml.save(out_path)
    print(f"  Saved: {out_path}")

    print("  Validating...")
    for test_tokens in [16, 64, 128]:
        ids = np.zeros((1, 512), dtype=np.int32)
        ids[0, :test_tokens] = np.random.randint(1, 100, test_tokens)
        mask = np.zeros((1, 512), dtype=np.int32)
        mask[0, :test_tokens] = 1
        test_input = {
            "input_ids": ids,
            "ref_s": np.zeros((1, 256), dtype=np.float32),
            "speed": np.array([1.0], dtype=np.float32),
            "attention_mask": mask,
        }
        t0 = time.time()
        test_output = duration_ml.predict(test_input)
        elapsed = (time.time() - t0) * 1000
        print(f"  tokens={test_tokens}: OK ({elapsed:.0f}ms), outputs={list(test_output.keys())}")

    print("  Compiling .mlpackage -> .mlmodelc ...")
    mlmodelc_path = os.path.join(COREML_DIR, "kokoro_duration.mlmodelc")
    if os.path.exists(mlmodelc_path):
        shutil.rmtree(mlmodelc_path)
    compile_result = run_cmd(
        f'xcrun coremlcompiler compile "{out_path}" "{COREML_DIR}"',
        check=False
    )
    if os.path.exists(mlmodelc_path):
        print(f"  Compiled: {mlmodelc_path}")
    else:
        print(f"  WARNING: Compilation did not produce .mlmodelc, using .mlpackage at runtime")

    meta = {
        "duration_model": "kokoro_duration.mlpackage",
        "duration_input_length": 512,
        "sample_rate": 24000,
        "voice_dim": 256,
        "style_dim": 128,
    }
    meta_path = os.path.join(COREML_DIR, "coreml_config.json")
    with open(meta_path, 'w') as f:
        json.dump(meta, f, indent=2)

    print("  Duration model export complete!")


def export_split_decoder(kmodel):
    import torch
    import torch.nn as nn
    import numpy as np
    import coremltools as ct

    print("\n=== Exporting Split Decoder (CoreML + HAR TorchScript) ===")
    os.makedirs(DECODER_DIR, exist_ok=True)

    decoder = kmodel.decoder

    class DecoderNoSourceWrapper(nn.Module):
        def __init__(self, decoder):
            super().__init__()
            self.decoder = decoder
            self.gen = decoder.generator
            self.num_kernels = self.gen.num_kernels
            self.num_upsamples = self.gen.num_upsamples

        def forward(self, asr, F0_pred, N_pred, ref_s, har):
            s = ref_s[:, :128]
            F0 = self.decoder.F0_conv(F0_pred.unsqueeze(1))
            N = self.decoder.N_conv(N_pred.unsqueeze(1))
            x = torch.cat([asr, F0, N], dim=1)
            x = self.decoder.encode(x, s)
            asr_res = self.decoder.asr_res(asr)
            res = True
            for block in self.decoder.decode:
                if res:
                    x = torch.cat([x, asr_res, F0, N], dim=1)
                x = block(x, s)
                if getattr(block, 'upsample_type', 'none') != 'none':
                    res = False
            for i in range(self.num_upsamples):
                x = torch.nn.functional.leaky_relu(x, negative_slope=0.1)
                x_source = self.gen.noise_convs[i](har)
                x_source = self.gen.noise_res[i](x_source, s)
                x = self.gen.ups[i](x)
                if i == self.num_upsamples - 1:
                    x = self.gen.reflection_pad(x)
                x = x + x_source
                xs = None
                for j in range(self.num_kernels):
                    if xs is None:
                        xs = self.gen.resblocks[i * self.num_kernels + j](x, s)
                    else:
                        xs += self.gen.resblocks[i * self.num_kernels + j](x, s)
                x = xs / self.num_kernels
            x = torch.nn.functional.leaky_relu(x)
            x = self.gen.conv_post(x)
            spec = torch.exp(x[:, :self.gen.post_n_fft // 2 + 1, :])
            phase = torch.sin(x[:, self.gen.post_n_fft // 2 + 1:, :])
            return self.gen.stft.inverse(spec, phase).squeeze(0)

    def compute_har_shapes(decoder, f0_len):
        with torch.no_grad():
            gen = decoder.generator
            f0 = torch.zeros((1, f0_len), dtype=torch.float32)
            f0_up = gen.f0_upsamp(f0[:, None]).transpose(1, 2)
            har_source, _, _ = gen.m_source(f0_up)
            har_source = har_source.transpose(1, 2).squeeze(1)
            har_spec, har_phase = gen.stft.transform(har_source)
        return har_spec.shape[1], har_spec.shape[2]

    class HarSourceModel(nn.Module):
        def __init__(self, generator):
            super().__init__()
            self.f0_upsamp = generator.f0_upsamp
            self.m_source = generator.m_source
            self.stft = generator.stft

        def forward(self, f0):
            f0_up = self.f0_upsamp(f0[:, None]).transpose(1, 2)
            har_source, _, _ = self.m_source(f0_up)
            har_source = har_source.transpose(1, 2).squeeze(1)
            har_spec, har_phase = self.stft.transform(har_source)
            har = torch.cat([har_spec, har_phase], dim=1)
            return har

    meta = {"buckets": {}}

    for bucket in BUCKETS:
        name = bucket["name"]
        asr_f = bucket["asr_frames"]
        f0_f = bucket["f0_frames"]
        print(f"\n  --- Bucket {name} ---")

        har_c, har_t = compute_har_shapes(decoder, f0_f)
        print(f"  HAR shapes: channels={har_c}, time={har_t}")

        print("  Exporting HAR TorchScript model...")
        har_model = HarSourceModel(decoder.generator)
        har_model.eval()
        with torch.no_grad():
            f0_example = torch.zeros(1, f0_f)
            har_traced = torch.jit.trace(har_model, (f0_example,))
            har_path = os.path.join(DECODER_DIR, f"kokoro_har_{name}.pt")
            har_traced.save(har_path)
            print(f"  Saved HAR: {har_path} ({os.path.getsize(har_path) / 1024:.0f} KB)")

        print("  Exporting CoreML split decoder...")
        wrapper = DecoderNoSourceWrapper(decoder)
        wrapper.eval()
        remove_training_ops(wrapper)
        patch_rsqrt(wrapper)

        asr = torch.zeros(1, 512, asr_f)
        f0 = torch.zeros(1, f0_f)
        n = torch.zeros(1, f0_f)
        ref_s = torch.zeros(1, 256)
        har = torch.zeros(1, har_c * 2, har_t)

        with torch.no_grad():
            out = wrapper(asr, f0, n, ref_s, har)
            print(f"  Torch output shape: {out.shape}")

        with torch.no_grad():
            traced = torch.jit.trace(wrapper, (asr, f0, n, ref_s, har), strict=False)

        try:
            mlmodel = ct.convert(
                traced,
                inputs=[
                    ct.TensorType(name="asr", shape=(1, 512, asr_f), dtype=np.float32),
                    ct.TensorType(name="F0_pred", shape=(1, f0_f), dtype=np.float32),
                    ct.TensorType(name="N_pred", shape=(1, f0_f), dtype=np.float32),
                    ct.TensorType(name="ref_s", shape=(1, 256), dtype=np.float32),
                    ct.TensorType(name="har", shape=(1, har_c * 2, har_t), dtype=np.float32),
                ],
                outputs=[ct.TensorType(name="waveform")],
                convert_to="mlprogram",
                minimum_deployment_target=ct.target.macOS12,
                compute_precision=ct.precision.FLOAT16,
                compute_units=ct.ComputeUnit.ALL,
            )
            mlpackage_path = os.path.join(DECODER_DIR, f"kokoro_decoder_split_{name}.mlpackage")
            mlmodel.save(mlpackage_path)
            print(f"  Saved: {mlpackage_path}")

            test_in = {
                "asr": np.zeros((1, 512, asr_f), dtype=np.float32),
                "F0_pred": np.zeros((1, f0_f), dtype=np.float32),
                "N_pred": np.zeros((1, f0_f), dtype=np.float32),
                "ref_s": np.zeros((1, 256), dtype=np.float32),
                "har": np.zeros((1, har_c * 2, har_t), dtype=np.float32),
            }
            t0 = time.time()
            test_out = mlmodel.predict(test_in)
            elapsed = (time.time() - t0) * 1000
            print(f"  CoreML test: {elapsed:.0f}ms, outputs={list(test_out.keys())}")

            print(f"  Compiling .mlpackage -> .mlmodelc ...")
            mlmodelc_path = os.path.join(DECODER_DIR, f"kokoro_decoder_split_{name}.mlmodelc")
            if os.path.exists(mlmodelc_path):
                shutil.rmtree(mlmodelc_path)
            run_cmd(f'xcrun coremlcompiler compile "{mlpackage_path}" "{DECODER_DIR}"', check=False)
            if os.path.exists(mlmodelc_path):
                print(f"  Compiled: {mlmodelc_path}")
        except Exception as e:
            print(f"  CoreML conversion failed: {e}")
            import traceback
            traceback.print_exc()

        meta["buckets"][name] = {
            "asr_frames": asr_f,
            "f0_frames": f0_f,
            "har_channels": har_c,
            "har_time": har_t,
        }

    with open(os.path.join(DECODER_DIR, "split_config.json"), 'w') as f:
        json.dump(meta, f, indent=2)

    print("\n  Split decoder export complete!")


def export_voices_and_vocab(kmodel):
    import torch
    import numpy as np

    print("\n=== Exporting Voices and Vocab ===")

    for voice_name in ['df_eva', 'dm_bernd']:
        vp = os.path.join(MODELS_DIR, 'voices', f'{voice_name}.pt')
        if not os.path.exists(vp):
            print(f"  SKIP: {vp} not found")
            continue
        v = torch.load(vp, weights_only=True, map_location='cpu').float()
        raw_out = os.path.join(MODELS_DIR, f'{voice_name}_voice.bin')
        v.squeeze(1).numpy().astype(np.float32).tofile(raw_out)
        print(f"  Saved {voice_name}: {raw_out} ({os.path.getsize(raw_out) / 1e6:.1f} MB)")

    with open(CONFIG_PATH, 'r') as f:
        config = json.load(f)
    vocab_path = os.path.join(MODELS_DIR, 'vocab.json')
    with open(vocab_path, 'w', encoding='utf-8') as f:
        json.dump(config['vocab'], f, ensure_ascii=False, indent=2)
    print(f"  Saved vocab: {vocab_path} ({len(config['vocab'])} entries)")


def main():
    parser = argparse.ArgumentParser(description="Export Kokoro TTS models for WhisperTalk C++ CoreML pipeline")
    parser.add_argument("--no-install", action="store_true", help="Skip conda env setup")
    parser.add_argument("--no-download", action="store_true", help="Skip model download")
    parser.add_argument("--duration-only", action="store_true", help="Export duration model only")
    parser.add_argument("--decoder-only", action="store_true", help="Export decoder only")
    parser.add_argument("--voices-only", action="store_true", help="Export voices/vocab only")
    args = parser.parse_args()

    export_all = not (args.duration_only or args.decoder_only or args.voices_only)

    if not args.no_install:
        conda_python = ensure_conda_env()
        print(f"\n  NOTE: If this script fails, re-run with the conda python directly:")
        print(f"  {conda_python} {__file__} --no-install")

    if not args.no_download:
        download_models()

    import torch
    print(f"\n  torch: {torch.__version__}")

    kmodel = load_kokoro_model(disable_complex=True)

    if export_all or args.duration_only:
        export_duration_model(kmodel)

    if export_all or args.decoder_only:
        export_split_decoder(kmodel)

    if export_all or args.voices_only:
        export_voices_and_vocab(kmodel)

    print("\n=== All exports complete ===")
    print(f"  Duration model: {COREML_DIR}/")
    print(f"  Split decoder:  {DECODER_DIR}/")
    print(f"  Voices:         {MODELS_DIR}/*_voice.bin")
    print(f"  Vocab:          {MODELS_DIR}/vocab.json")
    return 0


if __name__ == '__main__':
    sys.exit(main())
