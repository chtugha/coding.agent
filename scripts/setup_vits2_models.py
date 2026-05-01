#!/usr/bin/env python3
"""
Download and set up Piper TTS VITS2 German voice models for vits2-service.

Piper TTS models are distributed as .onnx files with .onnx.json config files.
They are consumed directly by piper_create() via libpiper — no ONNX→CoreML
conversion is required.

Downloaded artifacts (in bin/models/vits2-german/ or --output-dir):
  - <voice>.onnx        ONNX voice model (consumed directly by ONNX Runtime)
  - <voice>.onnx.json   Voice config with phoneme set, sample rate, speaker info

Prerequisites:
  - Internet access (for model download if --checkpoint not provided)
  - Python 3.8+ with standard library only (no extra packages required)

Usage:
  python3 scripts/setup_vits2_models.py
  python3 scripts/setup_vits2_models.py --output-dir /tmp/vits2_test
  python3 scripts/setup_vits2_models.py --voice de_DE-thorsten_emotional-medium
  python3 scripts/setup_vits2_models.py --checkpoint /path/to/de_DE-thorsten-high.onnx
  python3 scripts/setup_vits2_models.py --model-url https://example.com/voice.onnx

Piper model format:
  The .onnx model and .onnx.json config are passed directly to piper_create():
    piper_create(model_path, config_path, espeak_data_path)
  The config_path can be NULL if it equals model_path + ".json" — this script
  ensures both files are saved with matching names in the output directory.

Available German voices (from rhasspy/piper-voices on HuggingFace):
  de_DE-thorsten-high         (default, single speaker, high quality)
  de_DE-thorsten-medium       (single speaker, medium quality, faster)
  de_DE-thorsten_emotional-medium  (single speaker, emotional styles)
  de_DE-kerstin-low           (single speaker, low quality, fast)
  de_DE-eva_k-x_low           (single speaker, x-low quality, very fast)
  de_DE-mls-medium            (multi-speaker, 237 speakers)
"""
import os
import sys
import json
import shutil
import hashlib
import argparse
import urllib.request
import urllib.error

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT_DIR = os.path.realpath(os.path.join(SCRIPT_DIR, '..'))

DEFAULT_OUTPUT_SUBDIR = 'vits2-german'
DEFAULT_VOICE = 'de_DE-thorsten-high'

PIPER_HF_BASE = 'https://huggingface.co/rhasspy/piper-voices/resolve/main'

VOICE_CATALOG = {
    'de_DE-thorsten-high': {
        'lang': 'de',
        'quality': 'high',
        'speakers': 1,
        'hf_path': 'de/de_DE/thorsten/high',
    },
    'de_DE-thorsten-medium': {
        'lang': 'de',
        'quality': 'medium',
        'speakers': 1,
        'hf_path': 'de/de_DE/thorsten/medium',
    },
    'de_DE-thorsten_emotional-medium': {
        'lang': 'de',
        'quality': 'medium',
        'speakers': 1,
        'hf_path': 'de/de_DE/thorsten_emotional/medium',
    },
    'de_DE-kerstin-low': {
        'lang': 'de',
        'quality': 'low',
        'speakers': 1,
        'hf_path': 'de/de_DE/kerstin/low',
    },
    'de_DE-eva_k-x_low': {
        'lang': 'de',
        'quality': 'x_low',
        'speakers': 1,
        'hf_path': 'de/de_DE/eva_k/x_low',
    },
    'de_DE-mls-medium': {
        'lang': 'de',
        'quality': 'medium',
        'speakers': 237,
        'hf_path': 'de/de_DE/mls/medium',
    },
}

MIN_ONNX_SIZE = 1_000_000
MIN_JSON_SIZE = 100


def parse_args():
    parser = argparse.ArgumentParser(
        description='Download Piper TTS VITS2 German models for vits2-service.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        '--output-dir',
        default=None,
        help=(
            f'Directory to save model files. '
            f'Default: bin/models/{DEFAULT_OUTPUT_SUBDIR}/ relative to project root.'
        ),
    )
    parser.add_argument(
        '--voice',
        default=DEFAULT_VOICE,
        help=(
            f'Piper voice name to download. Default: {DEFAULT_VOICE}. '
            f'Known voices: {", ".join(sorted(VOICE_CATALOG.keys()))}'
        ),
    )
    parser.add_argument(
        '--model-url',
        default=None,
        help=(
            'Direct URL to the .onnx model file. If provided, --voice is used '
            'only to derive the output filename. The .json config URL is inferred '
            'by appending .json to the model URL.'
        ),
    )
    parser.add_argument(
        '--checkpoint',
        default=None,
        help=(
            'Path to a local .onnx model file. The matching .onnx.json config '
            'must be in the same directory or provided via --config-path.'
        ),
    )
    parser.add_argument(
        '--config-path',
        default=None,
        help=(
            'Path to the .onnx.json config file when using --checkpoint. '
            'Defaults to <checkpoint>.json if not provided.'
        ),
    )
    parser.add_argument(
        '--sha256',
        default=None,
        help='Expected SHA-256 hex digest of the .onnx file for integrity verification.',
    )
    parser.add_argument(
        '--list-voices',
        action='store_true',
        help='List known German voice names and exit.',
    )
    return parser.parse_args()


def list_voices():
    print('Known German Piper voices:')
    for name, info in sorted(VOICE_CATALOG.items()):
        speakers = f'{info["speakers"]} speaker' + ('s' if info['speakers'] > 1 else '')
        print(f'  {name:<40}  quality={info["quality"]:<8}  {speakers}')
    print()
    print('Pass --voice <name> to download a specific voice.')
    print('Pass --model-url <url> to download from a custom URL.')


_DOWNLOAD_TIMEOUT = 30
_CHUNK_SIZE = 65536


def download_file(url, dest_path, description=''):
    os.makedirs(os.path.dirname(dest_path), exist_ok=True)
    label = description or os.path.basename(dest_path)
    print(f'  Fetching {label}')
    print(f'  URL: {url}')
    tmp_path = dest_path + '.tmp'
    try:
        response = urllib.request.urlopen(url, timeout=_DOWNLOAD_TIMEOUT)
        total_size = int(response.headers.get('Content-Length', 0))
        downloaded = 0
        with open(tmp_path, 'wb') as f:
            while True:
                chunk = response.read(_CHUNK_SIZE)
                if not chunk:
                    break
                f.write(chunk)
                downloaded += len(chunk)
                if total_size > 0:
                    pct = min(100, downloaded * 100 // total_size)
                    mb_done = downloaded / 1e6
                    mb_total = total_size / 1e6
                    print(f'\r  Downloading... {pct:3d}%  {mb_done:.1f}/{mb_total:.1f} MB', end='', flush=True)
                else:
                    print(f'\r  Downloading... {downloaded / 1e6:.1f} MB', end='', flush=True)
        print()
        shutil.move(tmp_path, dest_path)
        size = os.path.getsize(dest_path)
        print(f'  Saved: {dest_path} ({size / 1e6:.2f} MB)')
        return True
    except KeyboardInterrupt:
        print()
        print('  Interrupted.')
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
        raise
    except urllib.error.HTTPError as e:
        print()
        print(f'  ERROR: HTTP {e.code} — {e.reason} for {url}')
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
        return False
    except urllib.error.URLError as e:
        print()
        print(f'  ERROR: {e.reason}')
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
        return False
    except Exception as e:
        print()
        print(f'  ERROR: {e}')
        if os.path.exists(tmp_path):
            os.remove(tmp_path)
        return False


def verify_sha256(path, expected_hex):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(65536), b''):
            h.update(chunk)
    actual = h.hexdigest()
    if actual.lower() != expected_hex.lower():
        print(f'  ERROR: SHA-256 mismatch for {os.path.basename(path)}')
        print(f'    expected: {expected_hex}')
        print(f'    actual:   {actual}')
        return False
    print(f'  SHA-256 OK: {actual[:16]}...')
    return True


def build_hf_urls(voice_name):
    info = VOICE_CATALOG.get(voice_name)
    if info:
        hf_path = info['hf_path']
        model_url = f'{PIPER_HF_BASE}/{hf_path}/{voice_name}.onnx'
        config_url = f'{PIPER_HF_BASE}/{hf_path}/{voice_name}.onnx.json'
    else:
        model_url = f'{PIPER_HF_BASE}/{voice_name}.onnx'
        config_url = f'{PIPER_HF_BASE}/{voice_name}.onnx.json'
    return model_url, config_url


def copy_local_file(src, dest, label):
    if not os.path.isfile(src):
        print(f'  ERROR: {label} not found at: {src}')
        return False
    if os.path.realpath(src) == os.path.realpath(dest):
        print(f'  Already in place: {dest}')
        return True
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    shutil.copy2(src, dest)
    size = os.path.getsize(dest)
    print(f'  Copied {label}: {dest} ({size / 1e6:.2f} MB)')
    return True


def main():
    args = parse_args()

    if args.list_voices:
        list_voices()
        sys.exit(0)

    output_dir = args.output_dir or os.path.join(ROOT_DIR, 'bin', 'models', DEFAULT_OUTPUT_SUBDIR)
    os.makedirs(output_dir, exist_ok=True)

    voice_name = args.voice
    onnx_filename = f'{voice_name}.onnx'
    json_filename = f'{voice_name}.onnx.json'
    onnx_dest = os.path.join(output_dir, onnx_filename)
    json_dest = os.path.join(output_dir, json_filename)

    print(f'=== Piper VITS2 model setup ===')
    print(f'  Voice:      {voice_name}')
    print(f'  Output dir: {output_dir}')

    if voice_name in VOICE_CATALOG:
        info = VOICE_CATALOG[voice_name]
        print(f'  Quality:    {info["quality"]}')
        print(f'  Speakers:   {info["speakers"]}')
    print()

    if args.checkpoint:
        print('=== Using local checkpoint ===')
        onnx_src = args.checkpoint
        if args.config_path:
            json_src = args.config_path
        else:
            json_src = args.checkpoint + '.json'
            if not os.path.isfile(json_src):
                base = os.path.splitext(args.checkpoint)[0]
                json_src = base + '.json'
        ok_onnx = copy_local_file(onnx_src, onnx_dest, 'ONNX model')
        ok_json = copy_local_file(json_src, json_dest, 'JSON config')
        if not ok_onnx or not ok_json:
            sys.exit(1)
    else:
        if args.model_url:
            model_url = args.model_url
            config_url = args.model_url + '.json'
            print(f'=== Downloading from custom URL ===')
        else:
            model_url, config_url = build_hf_urls(voice_name)
            print(f'=== Downloading from HuggingFace (rhasspy/piper-voices) ===')

        onnx_exists = os.path.isfile(onnx_dest) and os.path.getsize(onnx_dest) >= MIN_ONNX_SIZE
        json_exists = os.path.isfile(json_dest) and os.path.getsize(json_dest) >= MIN_JSON_SIZE

        if onnx_exists:
            print(f'  ONNX model already present ({os.path.getsize(onnx_dest) / 1e6:.2f} MB), skipping download.')
        else:
            ok = download_file(model_url, onnx_dest, f'{onnx_filename}')
            if not ok:
                print()
                print('  ERROR: Could not download ONNX model.')
                print(f'  Download manually from:')
                print(f'    {model_url}')
                print(f'  Save to: {onnx_dest}')
                sys.exit(1)

        if json_exists:
            print(f'  JSON config already present, skipping download.')
        else:
            ok = download_file(config_url, json_dest, f'{json_filename}')
            if not ok:
                print()
                print('  ERROR: Could not download JSON config.')
                print(f'  Download manually from:')
                print(f'    {config_url}')
                print(f'  Save to: {json_dest}')
                sys.exit(1)

    print()
    print('=== Verification ===')

    onnx_size = os.path.getsize(onnx_dest)
    json_size = os.path.getsize(json_dest)
    ok = True

    if onnx_size < MIN_ONNX_SIZE:
        print(f'  ERROR: ONNX model too small ({onnx_size} bytes < {MIN_ONNX_SIZE}). File may be corrupt.')
        ok = False
    else:
        print(f'  ONNX model: {onnx_dest}  ({onnx_size / 1e6:.2f} MB) OK')

    if json_size < MIN_JSON_SIZE:
        print(f'  ERROR: JSON config too small ({json_size} bytes < {MIN_JSON_SIZE}). File may be corrupt.')
        ok = False
    else:
        print(f'  JSON config: {json_dest}  ({json_size} bytes) OK')

    if args.sha256:
        if not verify_sha256(onnx_dest, args.sha256):
            ok = False

    if not ok:
        sys.exit(1)

    try:
        with open(json_dest, 'r', encoding='utf-8') as f:
            cfg = json.load(f)
        audio = cfg.get('audio', {})
        sr = audio.get('sample_rate', 'unknown')
        phoneme_type = cfg.get('phoneme_type', 'unknown')
        espeak_voice = cfg.get('espeak', {}).get('voice', 'unknown')
        speaker_count = len(cfg.get('speaker_id_map', {})) or 1
        print()
        print('=== Config summary ===')
        print(f'  Sample rate:   {sr} Hz')
        print(f'  Phoneme type:  {phoneme_type}')
        print(f'  espeak voice:  {espeak_voice}')
        print(f'  Speakers:      {speaker_count}')
    except Exception as e:
        print(f'  WARNING: Could not parse JSON config: {e}')

    print()
    print('=== Setup complete ===')
    print(f'  Model files are ready for vits2-service in: {output_dir}')
    print()
    print('  Usage with vits2-service:')
    print(f'    WHISPERTALK_MODELS_DIR=bin/models bin/vits2-service --model-dir {output_dir}')
    print()
    print('  piper_create() expects:')
    print(f'    model_path:  {onnx_dest}')
    print(f'    config_path: {json_dest}  (or NULL — libpiper appends .json automatically)')


if __name__ == '__main__':
    main()
