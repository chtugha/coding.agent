"""
Export Kokoro decoder in 3 variants for performance comparison:
  1. ONNX (for ONNX Runtime + CoreML EP in C++)
  2. CoreML full decoder (disable_complex=True, HAR split approach from mattmireles/kokoro-coreml)
  3. CoreML decoder-only (no hn-nsf source; source computed on CPU in C++)

Prerequisites:
  - System Python: torch, onnx, onnxruntime (for Approach 1)
  - Conda kokoro_coreml: torch==2.5.0, coremltools==8.3.0 (for Approaches 2 & 3)
  - kokoro package, model files in bin/models/kokoro-german/

Usage:
  python3 scripts/export_decoder_variants.py --onnx          # Approach 1
  python3 scripts/export_decoder_variants.py --coreml-full   # Approach 2
  python3 scripts/export_decoder_variants.py --coreml-split  # Approach 3
  python3 scripts/export_decoder_variants.py --all           # All approaches
"""
import os
import sys
import json
import argparse
import time
import importlib
import importlib.util
import types
import numpy as np
import torch
import torch.nn as nn

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.join(SCRIPT_DIR, '..')
MODELS_DIR = os.path.join(ROOT_DIR, 'bin', 'models', 'kokoro-german')
CONFIG_PATH = os.path.join(MODELS_DIR, 'config.json')
MODEL_PATH = os.path.join(MODELS_DIR, 'kokoro-german-v1_1-de.pth')
OUTPUT_DIR = os.path.join(MODELS_DIR, 'decoder_variants')

BUCKETS = [
    {"name": "3s",  "asr_frames": 72,  "f0_frames": 144, "samples": 72000},
    {"name": "5s",  "asr_frames": 120, "f0_frames": 240, "samples": 120000},
    {"name": "10s", "asr_frames": 240, "f0_frames": 480, "samples": 240000},
]


def load_kokoro_model(disable_complex=True):
    search_dirs = [
        '/opt/homebrew/Caskroom/miniconda/base/envs/kokoro_coreml/lib/python3.11/site-packages/kokoro',
    ]
    for d in ['/opt/homebrew/lib', '/usr/local/lib', '/usr/lib']:
        if not os.path.isdir(d):
            continue
        for pyver in sorted(os.listdir(d), reverse=True):
            if not pyver.startswith('python3'):
                continue
            candidate = os.path.join(d, pyver, 'site-packages', 'kokoro')
            if os.path.isdir(candidate):
                search_dirs.append(candidate)

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
        raise RuntimeError("Cannot find kokoro package with required modules")

    print(f"Using kokoro from: {kokoro_pkg_dir} (disable_complex={disable_complex})")
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
    print(f"Loading model from {MODEL_PATH}...")
    kmodel = KModel(config=CONFIG_PATH, model=MODEL_PATH, disable_complex=disable_complex)
    kmodel.eval()
    return kmodel


def remove_training_ops(model):
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


class DecoderWrapper(nn.Module):
    def __init__(self, decoder, voice_dim=128):
        super().__init__()
        self.decoder = decoder
        self.voice_dim = voice_dim

    def forward(self, asr, F0_pred, N_pred, ref_s):
        s = ref_s[:, :self.voice_dim]
        waveform = self.decoder(asr, F0_pred, N_pred, s)
        return waveform.squeeze(0)


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


def export_onnx(kmodel):
    print("\n=== Approach 1: ONNX Export ===")
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    decoder = kmodel.decoder
    wrapper = DecoderWrapper(decoder)
    wrapper.eval()
    remove_training_ops(wrapper)
    patch_rsqrt(wrapper)

    for bucket in BUCKETS:
        name = bucket["name"]
        asr_f = bucket["asr_frames"]
        f0_f = bucket["f0_frames"]
        print(f"\n  Exporting ONNX bucket {name} (asr={asr_f}, f0={f0_f})...")

        asr = torch.randn(1, 512, asr_f)
        f0 = torch.randn(1, f0_f)
        n = torch.randn(1, f0_f)
        ref_s = torch.randn(1, 256)

        with torch.no_grad():
            out = wrapper(asr, f0, n, ref_s)
            print(f"    Torch output shape: {out.shape}")

        out_path = os.path.join(OUTPUT_DIR, f"kokoro_decoder_{name}.onnx")
        torch.onnx.export(
            wrapper,
            (asr, f0, n, ref_s),
            out_path,
            input_names=["asr", "F0_pred", "N_pred", "ref_s"],
            output_names=["waveform"],
            opset_version=17,
            do_constant_folding=True,
        )
        size_mb = os.path.getsize(out_path) / (1024 * 1024)
        print(f"    Saved: {out_path} ({size_mb:.1f} MB)")

        try:
            import onnxruntime as ort
            sess = ort.InferenceSession(out_path, providers=['CoreMLExecutionProvider', 'CPUExecutionProvider'])
            feed = {
                "asr": asr.numpy(),
                "F0_pred": f0.numpy(),
                "N_pred": n.numpy(),
                "ref_s": ref_s.numpy(),
            }
            t0 = time.time()
            result = sess.run(None, feed)
            elapsed = (time.time() - t0) * 1000
            print(f"    ONNX Runtime test: output shape {result[0].shape}, {elapsed:.0f}ms")
        except Exception as e:
            print(f"    ONNX Runtime test failed: {e}")

    print("\n  ONNX export complete!")


def export_coreml_full(kmodel):
    import coremltools as ct
    print("\n=== Approach 2: CoreML Full Decoder (disable_complex, HAR split) ===")
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    decoder = kmodel.decoder
    wrapper = DecoderWrapper(decoder)
    wrapper.eval()
    remove_training_ops(wrapper)
    patch_rsqrt(wrapper)

    for bucket in BUCKETS:
        name = bucket["name"]
        asr_f = bucket["asr_frames"]
        f0_f = bucket["f0_frames"]
        print(f"\n  Exporting CoreML full decoder bucket {name}...")

        asr = torch.zeros(1, 512, asr_f)
        f0 = torch.zeros(1, f0_f)
        n = torch.zeros(1, f0_f)
        ref_s = torch.zeros(1, 256)

        with torch.no_grad():
            out = wrapper(asr, f0, n, ref_s)
            print(f"    Torch output shape: {out.shape}")

        print("    Tracing...")
        with torch.no_grad():
            traced = torch.jit.trace(wrapper, (asr, f0, n, ref_s), strict=False)

        print("    Converting to CoreML...")
        try:
            mlmodel = ct.convert(
                traced,
                inputs=[
                    ct.TensorType(name="asr", shape=(1, 512, asr_f), dtype=np.float32),
                    ct.TensorType(name="F0_pred", shape=(1, f0_f), dtype=np.float32),
                    ct.TensorType(name="N_pred", shape=(1, f0_f), dtype=np.float32),
                    ct.TensorType(name="ref_s", shape=(1, 256), dtype=np.float32),
                ],
                outputs=[ct.TensorType(name="waveform")],
                convert_to="mlprogram",
                minimum_deployment_target=ct.target.macOS12,
                compute_precision=ct.precision.FLOAT16,
                compute_units=ct.ComputeUnit.ALL,
            )
            out_path = os.path.join(OUTPUT_DIR, f"kokoro_decoder_full_{name}.mlpackage")
            mlmodel.save(out_path)
            print(f"    Saved: {out_path}")

            test_in = {
                "asr": np.zeros((1, 512, asr_f), dtype=np.float32),
                "F0_pred": np.zeros((1, f0_f), dtype=np.float32),
                "N_pred": np.zeros((1, f0_f), dtype=np.float32),
                "ref_s": np.zeros((1, 256), dtype=np.float32),
            }
            t0 = time.time()
            test_out = mlmodel.predict(test_in)
            elapsed = (time.time() - t0) * 1000
            print(f"    CoreML test: outputs={list(test_out.keys())}, {elapsed:.0f}ms")
        except Exception as e:
            print(f"    CoreML conversion failed: {e}")
            import traceback
            traceback.print_exc()

    print("\n  CoreML full decoder export complete!")


def export_coreml_split(kmodel):
    import coremltools as ct
    print("\n=== Approach 3: CoreML Decoder-Only (no hn-nsf source) ===")
    os.makedirs(OUTPUT_DIR, exist_ok=True)

    decoder = kmodel.decoder

    for bucket in BUCKETS:
        name = bucket["name"]
        asr_f = bucket["asr_frames"]
        f0_f = bucket["f0_frames"]
        print(f"\n  Exporting CoreML split decoder bucket {name}...")

        har_c, har_t = compute_har_shapes(decoder, f0_f)
        print(f"    HAR shapes: channels={har_c}, time={har_t}")

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
            print(f"    Torch output shape: {out.shape}")

        print("    Tracing...")
        with torch.no_grad():
            traced = torch.jit.trace(wrapper, (asr, f0, n, ref_s, har), strict=False)

        print("    Converting to CoreML...")
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
            out_path = os.path.join(OUTPUT_DIR, f"kokoro_decoder_split_{name}.mlpackage")
            mlmodel.save(out_path)
            print(f"    Saved: {out_path}")

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
            print(f"    CoreML test: outputs={list(test_out.keys())}, {elapsed:.0f}ms")
        except Exception as e:
            print(f"    CoreML conversion failed: {e}")
            import traceback
            traceback.print_exc()

    meta = {"buckets": {}, "har_channels": har_c}
    for bucket in BUCKETS:
        har_c2, har_t2 = compute_har_shapes(decoder, bucket["f0_frames"])
        meta["buckets"][bucket["name"]] = {
            "asr_frames": bucket["asr_frames"],
            "f0_frames": bucket["f0_frames"],
            "har_channels": har_c2,
            "har_time": har_t2,
        }
    with open(os.path.join(OUTPUT_DIR, "split_config.json"), 'w') as f:
        json.dump(meta, f, indent=2)

    print("\n  CoreML split decoder export complete!")


def main():
    parser = argparse.ArgumentParser(description="Export Kokoro decoder variants")
    parser.add_argument("--onnx", action="store_true", help="Export ONNX (Approach 1)")
    parser.add_argument("--coreml-full", action="store_true", help="Export CoreML full decoder (Approach 2)")
    parser.add_argument("--coreml-split", action="store_true", help="Export CoreML split decoder (Approach 3)")
    parser.add_argument("--all", action="store_true", help="Export all variants")
    args = parser.parse_args()

    if not (args.onnx or args.coreml_full or args.coreml_split or args.all):
        parser.print_help()
        return 1

    if args.onnx or args.all:
        kmodel = load_kokoro_model(disable_complex=True)
        export_onnx(kmodel)

    if args.coreml_full or args.all:
        kmodel = load_kokoro_model(disable_complex=True)
        export_coreml_full(kmodel)

    if args.coreml_split or args.all:
        kmodel = load_kokoro_model(disable_complex=True)
        export_coreml_split(kmodel)

    print("\n=== All requested exports complete ===")
    return 0


if __name__ == '__main__':
    sys.exit(main())
