#!/usr/bin/env python3
import sys
import os
import json
import torch

sys.path.insert(0, os.path.dirname(__file__))

from kokoro.model import KModel

MODELS_DIR = os.path.join(os.path.dirname(__file__), 'bin', 'models', 'kokoro-german')
CONFIG_PATH = os.path.join(MODELS_DIR, 'config.json')
MODEL_PATH = os.path.join(MODELS_DIR, 'kokoro-german-v1_1-de.pth')
OUTPUT_DIR = MODELS_DIR


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


def main():
    print(f"Loading model from {MODEL_PATH}...")
    kmodel = KModel(config=CONFIG_PATH, model=MODEL_PATH)
    kmodel.eval()

    wrapper = KModelTraceWrapper(kmodel)
    wrapper.eval()

    voice_path = os.path.join(MODELS_DIR, 'voices', 'df_eva.pt')
    voice_pack = torch.load(voice_path, weights_only=True, map_location='cpu').float()
    print(f"Voice pack shape: {voice_pack.shape}")

    phoneme_len = 3
    ref_s = voice_pack[phoneme_len - 1]
    print(f"ref_s shape (for {phoneme_len} phonemes): {ref_s.shape}")

    example_ids = torch.LongTensor([[0, 43, 54, 57, 0]])
    example_speed = torch.tensor(1.0)

    print("Testing inference first...")
    with torch.no_grad():
        test_out = wrapper(example_ids, ref_s, example_speed)
    print(f"Test audio shape: {test_out.shape}")

    print("Tracing model...")
    with torch.no_grad():
        traced = torch.jit.trace(wrapper, (example_ids, ref_s, example_speed))

    output_path = os.path.join(OUTPUT_DIR, 'kokoro_german.pt')
    traced.save(output_path)
    print(f"Saved TorchScript model to {output_path}")

    for voice_name in ['df_eva', 'dm_bernd']:
        vp = os.path.join(MODELS_DIR, 'voices', f'{voice_name}.pt')
        v = torch.load(vp, weights_only=True, map_location='cpu').float()
        out = os.path.join(OUTPUT_DIR, f'{voice_name}_embedding.pt')
        torch.save(v, out)
        print(f"Saved voice embedding {voice_name} shape={v.shape} to {out}")

    with open(os.path.join(OUTPUT_DIR, 'config.json'), 'r') as f:
        config = json.load(f)
    vocab_path = os.path.join(OUTPUT_DIR, 'vocab.json')
    with open(vocab_path, 'w', encoding='utf-8') as f:
        json.dump(config['vocab'], f, ensure_ascii=False, indent=2)
    print(f"Saved vocab to {vocab_path}")

    print("\nVerifying exported model...")
    loaded = torch.jit.load(output_path)
    with torch.no_grad():
        test_audio = loaded(example_ids, ref_s, example_speed)
    print(f"Verification output shape: {test_audio.shape}")
    print(f"Output range: [{test_audio.min():.4f}, {test_audio.max():.4f}]")
    print(f"Non-zero samples: {(test_audio.abs() > 1e-6).sum().item()}/{test_audio.numel()}")

    print("\nExport complete!")
    return 0


if __name__ == '__main__':
    sys.exit(main())
