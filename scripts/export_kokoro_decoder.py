import os
import sys
import json
import numpy as np
import torch
import torch.nn as nn

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.join(SCRIPT_DIR, '..')
MODELS_DIR = os.path.join(ROOT_DIR, 'bin', 'models', 'kokoro-german')
CONFIG_PATH = os.path.join(MODELS_DIR, 'config.json')
MODEL_PATH = os.path.join(MODELS_DIR, 'kokoro-german-v1_1-de.pth')
OUTPUT_DIR = os.path.join(MODELS_DIR, 'coreml')

import types
import importlib
import importlib.util

kokoro_pkg_dir = None
for d in ['/opt/homebrew/lib', '/usr/local/lib', '/usr/lib']:
    if not os.path.isdir(d):
        continue
    for pyver in sorted(os.listdir(d), reverse=True):
        if not pyver.startswith('python3'):
            continue
        candidate = os.path.join(d, pyver, 'site-packages', 'kokoro')
        if os.path.isdir(candidate):
            kokoro_pkg_dir = candidate
            break
    if kokoro_pkg_dir:
        break

if not kokoro_pkg_dir:
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


def remove_weight_norms_and_freeze(model):
    for name, module in model.named_modules():
        try:
            torch.nn.utils.remove_weight_norm(module)
        except ValueError:
            pass
    for p in model.parameters():
        p.requires_grad_(False)
    for b in model.buffers():
        b.requires_grad_(False)


class PostAlignmentModel(nn.Module):
    def __init__(self, kmodel):
        super().__init__()
        self.F0Ntrain = kmodel.predictor.F0Ntrain
        self.decoder = kmodel.decoder

    @torch.no_grad()
    def forward(self, en: torch.FloatTensor, asr: torch.FloatTensor,
                s: torch.FloatTensor, ref_s_content: torch.FloatTensor) -> torch.FloatTensor:
        F0_pred, N_pred = self.F0Ntrain(en, s)
        audio = self.decoder(asr, F0_pred, N_pred, ref_s_content).squeeze()
        return audio


def main():
    print(f"Loading model from {MODEL_PATH}...")
    kmodel = KModel(config=CONFIG_PATH, model=MODEL_PATH)
    kmodel.eval()

    post_model = PostAlignmentModel(kmodel)
    post_model.eval()
    remove_weight_norms_and_freeze(post_model)

    remaining = sum(1 for p in post_model.parameters() if p.requires_grad)
    print(f"Remaining requires_grad params: {remaining}")

    voice_path = os.path.join(MODELS_DIR, 'voices', 'df_eva.pt')
    voice_pack = torch.load(voice_path, weights_only=True, map_location='cpu').float().squeeze(1)
    ref_s = voice_pack[50].unsqueeze(0)
    s = ref_s[:, 128:]
    ref_s_content = ref_s[:, :128]

    DECODER_LENGTHS = [64, 128, 256, 512, 1024]

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    successful = []

    for L in DECODER_LENGTHS:
        print(f"\n=== Decoder L={L} (aligned sequence length) ===")
        en = torch.randn(1, 640, L)
        asr = torch.randn(1, 512, L)

        print("  Testing forward pass...")
        try:
            with torch.no_grad():
                test_out = post_model(en, asr, s, ref_s_content)
            print(f"  Output: {test_out.shape}, range=[{test_out.min():.4f}, {test_out.max():.4f}]")
        except Exception as e:
            print(f"  Forward FAILED: {e}")
            continue

        print("  Tracing model...")
        try:
            with torch.no_grad():
                traced = torch.jit.trace(post_model, (en, asr, s, ref_s_content))
            out_path = os.path.join(OUTPUT_DIR, f'kokoro_decoder_L{L}.pt')
            traced.save(out_path)
            fsize = os.path.getsize(out_path) / 1024 / 1024
            print(f"  Saved: {out_path} ({fsize:.1f}MB)")
            successful.append(L)
        except Exception as e:
            print(f"  Trace FAILED: {e}")
            continue

    config_path = os.path.join(OUTPUT_DIR, 'coreml_config.json')
    with open(config_path, 'r') as f:
        config = json.load(f)
    config['decoder_lengths'] = successful
    config['d_dim'] = 640
    config['t_en_dim'] = 512
    with open(config_path, 'w') as f:
        json.dump(config, f, indent=2)
    print(f"\nUpdated: {config_path}")

    print(f"\nDecoder export complete! {len(successful)}/{len(DECODER_LENGTHS)} buckets: {successful}")
    return 0 if successful else 1


if __name__ == '__main__':
    sys.exit(main())
