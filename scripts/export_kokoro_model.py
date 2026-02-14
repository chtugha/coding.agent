#!/usr/bin/env python3
import sys
import os
import json
import importlib
import importlib.util
import types
import numpy as np
import torch

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
    sys.path.insert(0, os.path.dirname(__file__))
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

BUCKET_SIZES = [8, 16, 32, 64, 128, 256, 512]


class KModelTraceWrapper(torch.nn.Module):
    def __init__(self, kmodel):
        super().__init__()
        self.bert = kmodel.bert
        self.bert_encoder = kmodel.bert_encoder
        self.predictor = kmodel.predictor
        self.text_encoder = kmodel.text_encoder
        self.decoder = kmodel.decoder

    @torch.no_grad()
    def forward(self, input_ids: torch.LongTensor, ref_s: torch.FloatTensor, speed: torch.FloatTensor) -> torch.FloatTensor:
        input_lengths = torch.full(
            (input_ids.shape[0],),
            input_ids.shape[-1],
            device=input_ids.device,
            dtype=torch.long
        )
        text_mask = torch.arange(input_lengths.max()).unsqueeze(0).expand(input_lengths.shape[0], -1).type_as(input_lengths)
        text_mask = torch.gt(text_mask + 1, input_lengths.unsqueeze(1)).to(input_ids.device)
        bert_dur = self.bert(input_ids, attention_mask=(~text_mask).int())
        d_en = self.bert_encoder(bert_dur).transpose(-1, -2)
        s = ref_s[:, 128:]
        d = self.predictor.text_encoder(d_en, s, input_lengths, text_mask)
        x, _ = self.predictor.lstm(d)
        duration = self.predictor.duration_proj(x)
        duration = torch.sigmoid(duration).sum(axis=-1) / speed
        pred_dur = torch.round(duration).clamp(min=1).long().squeeze()
        indices = torch.repeat_interleave(torch.arange(input_ids.shape[1], device=input_ids.device), pred_dur)
        pred_aln_trg = torch.zeros((input_ids.shape[1], indices.shape[0]), device=input_ids.device)
        pred_aln_trg[indices, torch.arange(indices.shape[0], device=input_ids.device)] = 1
        pred_aln_trg = pred_aln_trg.unsqueeze(0).to(input_ids.device)
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
    print(f"Loading model from {MODEL_PATH}...")
    kmodel = KModel(config=CONFIG_PATH, model=MODEL_PATH)
    kmodel.eval()

    wrapper = KModelTraceWrapper(kmodel)
    wrapper.eval()

    voice_path = os.path.join(MODELS_DIR, 'voices', 'df_eva.pt')
    voice_pack = torch.load(voice_path, weights_only=True, map_location='cpu').float()
    print(f"Voice pack shape: {voice_pack.shape}")

    successful = []
    for L in BUCKET_SIZES:
        print(f"\n=== Bucket L={L} ===")
        example_ids = make_example_ids(L)
        ref_s = voice_pack[min(L - 2, voice_pack.shape[0] - 1)]
        example_speed = torch.tensor(1.0)

        print("  Testing inference...")
        try:
            with torch.no_grad():
                test_out = wrapper(example_ids, ref_s, example_speed)
            print(f"  Output: {test_out.shape}, range=[{test_out.min():.4f}, {test_out.max():.4f}]")
        except Exception as e:
            print(f"  Forward FAILED: {e}")
            continue

        print("  Tracing model...")
        with torch.no_grad():
            traced = torch.jit.trace(wrapper, (example_ids, ref_s, example_speed))

        output_path = os.path.join(OUTPUT_DIR, f'kokoro_german_L{L}.pt')
        traced.save(output_path)
        fsize = os.path.getsize(output_path) / 1024 / 1024
        print(f"  Saved: {output_path} ({fsize:.1f}MB)")
        successful.append(L)

    for voice_name in ['df_eva', 'dm_bernd']:
        vp = os.path.join(MODELS_DIR, 'voices', f'{voice_name}.pt')
        v = torch.load(vp, weights_only=True, map_location='cpu').float()
        out = os.path.join(OUTPUT_DIR, f'{voice_name}_embedding.pt')
        torch.save(v, out)
        print(f"Saved voice embedding {voice_name} shape={v.shape} to {out}")
        raw_out = os.path.join(OUTPUT_DIR, f'{voice_name}_voice.bin')
        v.squeeze(1).numpy().astype(np.float32).tofile(raw_out)
        print(f"Saved voice raw binary {voice_name} to {raw_out}")

    with open(os.path.join(OUTPUT_DIR, 'config.json'), 'r') as f:
        config = json.load(f)
    vocab_path = os.path.join(OUTPUT_DIR, 'vocab.json')
    with open(vocab_path, 'w', encoding='utf-8') as f:
        json.dump(config['vocab'], f, ensure_ascii=False, indent=2)
    print(f"Saved vocab to {vocab_path}")

    bucket_json = os.path.join(OUTPUT_DIR, 'buckets.json')
    with open(bucket_json, 'w') as f:
        json.dump({'bucket_sizes': successful, 'voice_dim': 256, 'voice_entries': int(voice_pack.shape[0])}, f)
    print(f"Saved bucket config to {bucket_json}")

    print(f"\nExport complete! {len(successful)}/{len(BUCKET_SIZES)} buckets: {successful}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
