#!/usr/bin/env python3
"""
Export Kokoro TTS German model to CoreML two-stage pipeline.

Based on mattmireles/kokoro-coreml approach:
  Stage 1: Duration model (BERT + prosody prediction) -> fixed 512-token input
  Stage 2: Decoder-only model (vocoder) -> fixed-shape bucketed inputs

The dynamic repeat_interleave alignment is done on the C++ host side,
avoiding CoreML's inability to handle data-dependent output shapes.

Prerequisites:
  - torch==2.5.0, coremltools==8.3.0, numpy==1.26.4
  - kokoro package (pip install kokoro)
  - Model files in bin/models/kokoro-german/

Usage:
  /opt/homebrew/Caskroom/miniconda/base/envs/kokoro_coreml/bin/python3 scripts/export_kokoro_coreml.py

Output (in bin/models/kokoro-german/coreml/):
  - kokoro_duration.mlpackage
  - kokoro_decoder_3s.mlpackage
  - kokoro_decoder_5s.mlpackage
  - kokoro_decoder_10s.mlpackage
"""
import os
import sys
import json
import numpy as np
import torch
import torch.nn as nn
import coremltools as ct

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.join(SCRIPT_DIR, '..')
MODELS_DIR = os.path.join(ROOT_DIR, 'bin', 'models', 'kokoro-german')
CONFIG_PATH = os.path.join(MODELS_DIR, 'config.json')
MODEL_PATH = os.path.join(MODELS_DIR, 'kokoro-german-v1_1-de.pth')
OUTPUT_DIR = os.path.join(MODELS_DIR, 'coreml')

from kokoro.model import KModel
from kokoro.modules import AdaLayerNorm


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

def patch_rsqrt_int_issue(model):
    from kokoro.istftnet import AdainResBlk1d
    def _patched_forward(self, x, s):
        out = self._residual(x, s)
        out = (out + self._shortcut(x)) * _RSQRT_2
        return out
    for module in model.modules():
        if isinstance(module, AdainResBlk1d):
            import types
            module.forward = types.MethodType(_patched_forward, module)


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


def export_duration_model(kmodel):
    print("\n=== Exporting Duration Model ===")
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

    print("Testing forward pass...")
    with torch.no_grad():
        outputs = duration_model(input_ids, ref_s, speed, attention_mask)
        print(f"Output shapes: {[o.shape for o in outputs]}")

    print("Tracing model...")
    with torch.no_grad():
        traced = torch.jit.trace(duration_model, (input_ids, ref_s, speed, attention_mask), strict=False)

    print("Converting to CoreML...")
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

    os.makedirs(OUTPUT_DIR, exist_ok=True)
    out_path = os.path.join(OUTPUT_DIR, "kokoro_duration.mlpackage")
    duration_ml.save(out_path)
    print(f"Saved: {out_path}")

    print("Validating...")
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
        test_output = duration_ml.predict(test_input)
        print(f"  tokens={test_tokens}: OK, outputs={list(test_output.keys())}")

    print("Duration model export complete!")
    return out_path


def main():
    print(f"torch: {torch.__version__}, coremltools: {ct.__version__}, numpy: {np.__version__}")
    print(f"Loading model from {MODEL_PATH}...")

    kmodel = KModel(config=CONFIG_PATH, model=MODEL_PATH, disable_complex=True)
    kmodel.eval()

    out_path = export_duration_model(kmodel)

    meta_path = os.path.join(OUTPUT_DIR, "coreml_config.json")
    meta = {
        "duration_model": "kokoro_duration.mlpackage",
        "duration_input_length": 512,
        "sample_rate": 24000,
        "voice_dim": 256,
        "style_dim": 128,
    }
    with open(meta_path, 'w') as f:
        json.dump(meta, f, indent=2)
    print(f"\nSaved CoreML config: {meta_path}")

    print("\nDuration model CoreML export complete!")
    print("NOTE: Decoder stays on TorchScript (hn-nsf source uses torch.multiply which is")
    print("      incompatible with coremltools). The hybrid approach uses CoreML for the")
    print("      duration/prosody model (BERT) and TorchScript for the decoder (vocoder).")
    return 0


if __name__ == '__main__':
    sys.exit(main())
