#!/usr/bin/env python3
# synthesize_stereo.py — Step 2 of German Moshi fine-tune pipeline.
#
# Reads scripts/dialogues.json, synthesizes each turn with TTS,
# and assembles two-channel stereo WAVs:
#   Ch0 (left)  = MOSHI voice (receptionist)
#   Ch1 (right) = USER  voice (caller)
#
# TTS provider (env TTS_PROVIDER, default "openai"):
#   openai — tts-1-hd with nova / onyx  (preferred, natural German prosody)
#   google — de-DE-Neural2-F / de-DE-Neural2-D  (alternative native German)
#
# Output:
#   data/moshi_german/stereo/*.wav      — 24kHz 16-bit stereo WAVs
#   data/moshi_german/train_local.jsonl — local-path manifest for Modal upload
#
# Features:
#   • Per-turn TTS cache — safe to interrupt and resume
#   • Exponential-backoff retry on API errors
#   • Rate-limit throttle (1 req/s sustained)
#   • Progress bar
#
# Requirements: pip install numpy scipy tqdm
#               pip install google-cloud-texttospeech  (for google provider)
#               pip install openai                     (for openai provider)
# Env:          TTS_PROVIDER=openai|google  (default: openai)
#               OPENAI_API_KEY                  (for openai)
#               GOOGLE_APPLICATION_CREDENTIALS  (for google)

import io
import json
import os
import time
import wave
from math import gcd
from pathlib import Path

import numpy as np
from dotenv import load_dotenv
from tqdm import tqdm

load_dotenv(Path(__file__).parent.parent / ".env")

DIALOGUES_PATH = Path(__file__).parent / "dialogues.json"
DATA_DIR       = Path(__file__).parent.parent / "data" / "moshi_german"
CACHE_DIR      = DATA_DIR / "tts_cache"
STEREO_DIR     = DATA_DIR / "stereo"
MANIFEST_PATH  = DATA_DIR / "train_local.jsonl"

SAMPLE_RATE   = 24000          # Moshi's native rate
GAP_SECONDS   = 0.12           # silence between turns (120 ms)
MIN_REQ_GAP   = 1.05           # seconds between TTS API calls (rate-limit safety)

TTS_PROVIDER  = os.environ.get("TTS_PROVIDER", "openai").lower()

if TTS_PROVIDER == "openai":
    from openai import OpenAI
    _openai_client = OpenAI()
    MOSHI_VOICE = "nova"
    USER_VOICE  = "onyx"
elif TTS_PROVIDER == "google":
    try:
        from google.cloud import texttospeech as gtts
        _google_client = gtts.TextToSpeechClient()
    except ImportError:
        raise SystemExit(
            "Google Cloud TTS selected but google-cloud-texttospeech not installed.\n"
            "  pip install google-cloud-texttospeech\n"
            "Or set TTS_PROVIDER=openai to use OpenAI TTS instead."
        )
    MOSHI_VOICE = "de-DE-Neural2-F"
    USER_VOICE  = "de-DE-Neural2-D"
else:
    raise SystemExit(f"Unknown TTS_PROVIDER={TTS_PROVIDER!r}. Use 'google' or 'openai'.")

_last_request_at: float = 0.0


def _throttle():
    global _last_request_at
    elapsed = time.monotonic() - _last_request_at
    if elapsed < MIN_REQ_GAP:
        time.sleep(MIN_REQ_GAP - elapsed)
    _last_request_at = time.monotonic()


def _is_valid_wav(path: Path) -> bool:
    try:
        with wave.open(str(path), "rb") as wf:
            return wf.getnframes() > 0
    except Exception:
        return False


def _synthesize_openai(text: str, voice: str, cache_path: Path) -> bytes:
    response = _openai_client.audio.speech.create(
        model="tts-1-hd",
        voice=voice,
        input=text,
        response_format="wav",
        speed=0.95,
    )
    return response.content


def _synthesize_google(text: str, voice: str, cache_path: Path) -> bytes:
    synthesis_input = gtts.SynthesisInput(text=text)
    voice_params = gtts.VoiceSelectionParams(
        language_code="de-DE",
        name=voice,
    )
    audio_config = gtts.AudioConfig(
        audio_encoding=gtts.AudioEncoding.LINEAR16,
        sample_rate_hertz=SAMPLE_RATE,
        speaking_rate=0.95,
    )
    response = _google_client.synthesize_speech(
        input=synthesis_input, voice=voice_params, audio_config=audio_config,
    )
    pcm_data = response.audio_content
    if len(pcm_data) == 0:
        raise ValueError("Google TTS returned empty audio")
    buf = io.BytesIO()
    with wave.open(buf, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(pcm_data)
    return buf.getvalue()


def synthesize(text: str, voice: str, cache_path: Path) -> np.ndarray:
    """Return float32 mono audio at SAMPLE_RATE. Uses cache if available."""
    if cache_path.exists() and _is_valid_wav(cache_path):
        return _read_wav_float(cache_path)

    synth_fn = _synthesize_openai if TTS_PROVIDER == "openai" else _synthesize_google

    for attempt in range(5):
        try:
            _throttle()
            raw = synth_fn(text, voice, cache_path)
            cache_path.write_bytes(raw)
            return _read_wav_float_bytes(raw)
        except Exception as exc:
            wait = 2 ** attempt
            tqdm.write(f"  ⚠ TTS error (attempt {attempt + 1}): {exc} — retry in {wait}s")
            time.sleep(wait)

    raise RuntimeError(f"TTS failed after 5 attempts: {text[:60]!r}")


def _read_wav_float(path: Path) -> np.ndarray:
    with wave.open(str(path), "rb") as wf:
        raw = wf.readframes(wf.getnframes())
        sr  = wf.getframerate()
        sw  = wf.getsampwidth()
    return _raw_to_float(raw, sr, sw)


def _read_wav_float_bytes(data: bytes) -> np.ndarray:
    with wave.open(io.BytesIO(data), "rb") as wf:
        raw = wf.readframes(wf.getnframes())
        sr  = wf.getframerate()
        sw  = wf.getsampwidth()
    return _raw_to_float(raw, sr, sw)


def _raw_to_float(raw: bytes, sr: int, sw: int) -> np.ndarray:
    if sw == 2:
        audio = np.frombuffer(raw, dtype=np.int16).astype(np.float32) / 32768.0
    elif sw == 4:
        audio = np.frombuffer(raw, dtype=np.int32).astype(np.float32) / 2_147_483_648.0
    else:
        raise ValueError(f"Unsupported sample width {sw}")
    if sr != SAMPLE_RATE:
        audio = _resample(audio, sr, SAMPLE_RATE)
    return audio


def _resample(audio: np.ndarray, from_sr: int, to_sr: int) -> np.ndarray:
    if from_sr == to_sr:
        return audio
    from scipy.signal import resample_poly
    g = gcd(to_sr, from_sr)
    return resample_poly(audio, to_sr // g, from_sr // g).astype(np.float32)


def assemble_stereo(dialogue: dict) -> tuple[np.ndarray, float]:
    """
    Returns (stereo_array, duration_seconds).
    stereo_array shape: (N, 2) — column 0 = Moshi, column 1 = User.
    """
    gap = np.zeros(int(GAP_SECONDS * SAMPLE_RATE), dtype=np.float32)
    moshi_ch: list[np.ndarray] = []
    user_ch:  list[np.ndarray] = []

    turns = dialogue["turns"]
    for turn_idx, turn in enumerate(turns):
        speaker = turn["speaker"]
        voice   = MOSHI_VOICE if speaker == "moshi" else USER_VOICE
        cache_name = f"{dialogue['id']}_t{turn_idx:03d}_{speaker}.wav"
        audio = synthesize(turn["text"], voice, CACHE_DIR / cache_name)
        n     = len(audio)
        is_last = turn_idx == len(turns) - 1

        if speaker == "moshi":
            moshi_ch.append(audio)
            user_ch.append(np.zeros(n, dtype=np.float32))
        else:
            user_ch.append(audio)
            moshi_ch.append(np.zeros(n, dtype=np.float32))

        if not is_last:
            moshi_ch.append(gap)
            user_ch.append(gap)

    left  = np.concatenate(moshi_ch) if moshi_ch else np.zeros(SAMPLE_RATE, dtype=np.float32)
    right = np.concatenate(user_ch)  if user_ch  else np.zeros(SAMPLE_RATE, dtype=np.float32)

    max_len = max(len(left), len(right))
    left  = np.pad(left,  (0, max_len - len(left)))
    right = np.pad(right, (0, max_len - len(right)))

    stereo   = np.stack([left, right], axis=1)
    duration = max_len / SAMPLE_RATE
    return stereo, duration


def write_stereo_wav(path: Path, audio: np.ndarray):
    """Write (N, 2) float32 array as 16-bit stereo WAV at SAMPLE_RATE."""
    pcm = (audio * 32767.0).clip(-32768, 32767).astype(np.int16)
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(2)
        wf.setsampwidth(2)
        wf.setframerate(SAMPLE_RATE)
        wf.writeframes(pcm.tobytes())


def get_wav_duration(path: Path) -> float:
    with wave.open(str(path), "rb") as wf:
        return wf.getnframes() / wf.getframerate()


def main():
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    CACHE_DIR.mkdir(parents=True, exist_ok=True)
    STEREO_DIR.mkdir(parents=True, exist_ok=True)

    dialogues = json.loads(DIALOGUES_PATH.read_text(encoding="utf-8"))
    print(f"Found {len(dialogues)} dialogues in {DIALOGUES_PATH.name}")

    manifest: list[dict] = []
    failed:   list[str]  = []
    total_turns = sum(len(d.get("turns", [])) for d in dialogues)
    print(f"Total turns to synthesize: {total_turns}  (cached turns are skipped)\n")

    with tqdm(dialogues, unit="dialogue") as pbar:
        for dialogue in pbar:
            did      = dialogue["id"]
            wav_path = STEREO_DIR / f"{did}.wav"
            pbar.set_description(f"{did} ({dialogue.get('scenario','?')[:20]})")

            if wav_path.exists() and _is_valid_wav(wav_path):
                duration = get_wav_duration(wav_path)
                manifest.append({"path": str(wav_path.resolve()), "duration": duration})
                continue

            try:
                stereo, duration = assemble_stereo(dialogue)
                write_stereo_wav(wav_path, stereo)
                manifest.append({"path": str(wav_path.resolve()), "duration": duration})
            except Exception as exc:
                tqdm.write(f"  ✗ {did} FAILED: {exc}")
                failed.append(did)

    with MANIFEST_PATH.open("w", encoding="utf-8") as f:
        for entry in manifest:
            f.write(json.dumps(entry) + "\n")

    total_hours   = sum(e["duration"] for e in manifest) / 3600
    total_minutes = total_hours * 60

    print(f"\n{'═' * 56}")
    print(f"  ✓ {len(manifest)} stereo WAVs  → {STEREO_DIR}")
    print(f"  ✓ Manifest          → {MANIFEST_PATH}")
    print(f"  ✓ Total audio:        {total_hours:.2f} h  ({total_minutes:.0f} min)")
    if failed:
        print(f"  ✗ Failed ({len(failed)}): {failed}")
    print(f"{'═' * 56}")
    print("\nNext step: modal run scripts/train_modal.py")


if __name__ == "__main__":
    main()
