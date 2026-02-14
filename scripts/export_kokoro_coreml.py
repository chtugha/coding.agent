#!/usr/bin/env python3
"""
Export Kokoro TTS model to CoreML format.
Strategy: Export multiple fixed-size CoreML models (bucketed by input length)
to avoid dynamic shape issues with pack_padded_sequence and expand ops.
At runtime, C++ code pads input to the nearest bucket size.
"""
import sys
import os
import json
import importlib
import importlib.util
import types
import numpy as np
import torch
import torch.nn as nn

kokoro_pkg_dir = None
for d in ['/opt/homebrew/lib', '/usr/local/lib', '/usr/lib']:
    for pyver in sorted(os.listdir(d) if os.path.isdir(d) else [], reverse=True):
        if not pyver.startswith('python3'): continue
        candidate = os.path.join(d, pyver, 'site-packages', 'kokoro')
        if os.path.isdir(candidate):
            kokoro_pkg_dir = candidate
            break
    if kokoro_pkg_dir: break

if not kokoro_pkg_dir:
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
    from kokoro.model import KModel
else:
    kokoro = types.ModuleType('kokoro')
    kokoro.__path__ = [kokoro_pkg_dir]
    kokoro.__package__ = 'kokoro'
    sys.modules['kokoro'] = kokoro
    for submod in ['istftnet', 'modules', 'model']:
        spec = importlib.util.spec_from_file_location(
            f'kokoro.{submod}', f'{kokoro_pkg_dir}/{submod}.py', submodule_search_locations=[])
        mod = importlib.util.module_from_spec(spec)
        sys.modules[f'kokoro.{submod}'] = mod
        spec.loader.exec_module(mod)
    from kokoro.model import KModel

MODELS_DIR = os.path.join(os.path.dirname(__file__), '..', 'bin', 'models', 'kokoro-german')
CONFIG_PATH = os.path.join(MODELS_DIR, 'config.json')
MODEL_PATH = os.path.join(MODELS_DIR, 'kokoro-german-v1_1-de.pth')
OUTPUT_DIR = MODELS_DIR

BUCKET_SIZES = [16, 32, 64, 128, 256, 512]


class KokoroCoreMLWrapper(nn.Module):
    def __init__(self, kmodel):
        super().__init__()
        self.bert = kmodel.bert
        self.bert_encoder = kmodel.bert_encoder
        self.predictor = kmodel.predictor
        self.text_encoder = kmodel.text_encoder
        self.decoder = kmodel.decoder

    @torch.no_grad()
    def forward(self, input_ids, ref_s, speed):
        input_lengths = torch.tensor([input_ids.shape[-1]], dtype=torch.long)
        text_mask = torch.arange(input_ids.shape[-1]).unsqueeze(0).type_as(input_lengths)
        text_mask = torch.gt(text_mask + 1, input_lengths.unsqueeze(1))
        bert_dur = self.bert(input_ids, attention_mask=(~text_mask).int())
        d_en = self.bert_encoder(bert_dur).transpose(-1, -2)
        s = ref_s[:, 128:]
        d = self.predictor.text_encoder(d_en, s, input_lengths, text_mask)
        x, _ = self.predictor.lstm(d)
        duration = self.predictor.duration_proj(x)
        duration = torch.sigmoid(duration).sum(axis=-1) / speed
        pred_dur = torch.round(duration).clamp(min=1).long().squeeze()
        indices = torch.repeat_interleave(
            torch.arange(input_ids.shape[1]), pred_dur)
        pred_aln_trg = torch.zeros(
            (input_ids.shape[1], indices.shape[0]))
        pred_aln_trg[indices, torch.arange(indices.shape[0])] = 1
        pred_aln_trg = pred_aln_trg.unsqueeze(0)
        en = d.transpose(-1, -2) @ pred_aln_trg
        F0_pred, N_pred = self.predictor.F0Ntrain(en, s)
        t_en = self.text_encoder(input_ids, input_lengths, text_mask)
        asr = t_en @ pred_aln_trg
        audio = self.decoder(asr, F0_pred, N_pred, ref_s[:, :128]).squeeze()
        return audio


def make_example_ids(L):
    ids = torch.zeros(1, L, dtype=torch.long)
    ids[0, 0] = 0
    ids[0, -1] = 0
    for i in range(1, L - 1):
        ids[0, i] = 43 + (i % 25)
    return ids


def main():
    import coremltools as ct

    print(f"Loading Kokoro model from {MODEL_PATH}...")
    kmodel = KModel(config=CONFIG_PATH, model=MODEL_PATH)
    kmodel.eval()

    wrapper = KokoroCoreMLWrapper(kmodel)
    wrapper.eval()

    voice_path = os.path.join(MODELS_DIR, 'voices', 'df_eva.pt')
    voice_pack = torch.load(voice_path, weights_only=True, map_location='cpu').float()
    print(f"Voice pack shape: {voice_pack.shape}")

    print(f"\nExporting {len(BUCKET_SIZES)} bucket models: {BUCKET_SIZES}")
    successful = []

    for L in BUCKET_SIZES:
        print(f"\n=== Bucket L={L} ===")
        example_ids = make_example_ids(L)
        ref_s = voice_pack[min(L - 2, voice_pack.shape[0] - 1)]
        speed = torch.tensor([1.0])

        print(f"  Testing forward pass...")
        try:
            with torch.no_grad():
                out = wrapper(example_ids, ref_s, speed)
            print(f"  PyTorch output: {out.shape}, range=[{out.min():.4f}, {out.max():.4f}]")
        except Exception as e:
            print(f"  Forward pass failed: {e}")
            continue

        print(f"  Tracing...")
        try:
            with torch.no_grad():
                traced = torch.jit.trace(wrapper, (example_ids, ref_s, speed))
        except Exception as e:
            print(f"  Tracing failed: {e}")
            continue

        print(f"  Converting to CoreML...")
        try:
            mlmodel = ct.convert(
                traced,
                inputs=[
                    ct.TensorType(name="input_ids", shape=(1, L), dtype=np.int32),
                    ct.TensorType(name="ref_s", shape=(1, 256), dtype=np.float32),
                    ct.TensorType(name="speed", shape=(1,), dtype=np.float32),
                ],
                outputs=[
                    ct.TensorType(name="audio"),
                ],
                convert_to="mlprogram",
                minimum_deployment_target=ct.target.macOS15,
            )
        except Exception as e:
            print(f"  CoreML conversion failed: {e}")
            continue

        out_name = f'kokoro_german_L{L}.mlpackage'
        out_path = os.path.join(OUTPUT_DIR, out_name)
        mlmodel.save(out_path)
        print(f"  Saved: {out_path}")

        print(f"  Verifying CoreML prediction...")
        try:
            predictions = mlmodel.predict({
                "input_ids": example_ids.numpy().astype(np.int32),
                "ref_s": ref_s.numpy().astype(np.float32),
                "speed": speed.numpy().astype(np.float32),
            })
            audio_out = predictions["audio"]
            print(f"  CoreML output: shape={audio_out.shape}, range=[{np.min(audio_out):.4f}, {np.max(audio_out):.4f}]")

            ref_np = out.numpy()
            coreml_flat = audio_out.flatten()[:ref_np.shape[0]]
            min_len = min(ref_np.shape[0], coreml_flat.shape[0])
            diff = np.abs(ref_np[:min_len] - coreml_flat[:min_len])
            print(f"  Max diff vs PyTorch: {diff.max():.6f}, Mean diff: {diff.mean():.6f}")
            successful.append(L)
        except Exception as e:
            print(f"  Prediction failed: {e}")
            os.system(f"rm -rf '{out_path}'")
            continue

    if not successful:
        print("\nFATAL: No models converted successfully!")
        return 1

    print(f"\n=== Successfully converted {len(successful)} models: {successful} ===")

    for voice_name in ['df_eva', 'dm_bernd']:
        vp = os.path.join(MODELS_DIR, 'voices', f'{voice_name}.pt')
        v = torch.load(vp, weights_only=True, map_location='cpu').float()
        out_path = os.path.join(OUTPUT_DIR, f'{voice_name}_voice.bin')
        v.numpy().tofile(out_path)
        print(f"Saved voice '{voice_name}' raw float32: shape={v.shape} -> {out_path}")

    bucket_json = os.path.join(OUTPUT_DIR, 'buckets.json')
    with open(bucket_json, 'w') as f:
        json.dump({"bucket_sizes": successful}, f)
    print(f"Saved bucket config to {bucket_json}")

    print(f"\nCompile all models with:")
    for L in successful:
        pkg = os.path.join(OUTPUT_DIR, f'kokoro_german_L{L}.mlpackage')
        print(f"  xcrun coremlcompiler compile '{pkg}' '{OUTPUT_DIR}'")

    return 0


if __name__ == '__main__':
    sys.exit(main())
