#!/usr/bin/env python3
import os
import sys
import struct

def main():
    import torch

    pt_path = sys.argv[1] if len(sys.argv) > 1 else "bin/models/kokoro-german/decoder_variants/kokoro_har_3s.pt"
    out_path = sys.argv[2] if len(sys.argv) > 2 else "bin/models/kokoro-german/decoder_variants/har_weights.bin"

    model = torch.jit.load(pt_path, map_location='cpu')
    model.eval()

    sd = dict(model.named_parameters())
    sd.update(dict(model.named_buffers()))

    w = sd['m_source.l_linear.weight'].detach().cpu().numpy().flatten()
    b = sd['m_source.l_linear.bias'].detach().cpu().numpy().flatten()
    real = sd['stft.weight_forward_real'].detach().cpu().numpy().squeeze(1)
    imag = sd['stft.weight_forward_imag'].detach().cpu().numpy().squeeze(1)

    assert w.shape == (9,), f"Expected weight shape (9,), got {w.shape}"
    assert b.shape == (1,), f"Expected bias shape (1,), got {b.shape}"
    assert real.shape == (11, 20), f"Expected real shape (11,20), got {real.shape}"
    assert imag.shape == (11, 20), f"Expected imag shape (11,20), got {imag.shape}"

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, 'wb') as f:
        f.write(b'HAR1')
        f.write(struct.pack('9f', *w))
        f.write(struct.pack('f', b[0]))
        f.write(struct.pack('220f', *real.flatten()))
        f.write(struct.pack('220f', *imag.flatten()))

    print(f"Saved HAR weights: {out_path} ({os.path.getsize(out_path)} bytes)")
    print(f"  l_linear weight: {w}")
    print(f"  l_linear bias: {b[0]}")

if __name__ == '__main__':
    main()
