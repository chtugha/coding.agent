import os
import sys
import re
import json
import glob
import subprocess
import tempfile
import numpy as np
import soundfile as sf

WHISPER_CLI = os.path.join(os.path.dirname(os.path.dirname(__file__)), "whisper-cpp", "build", "bin", "whisper-cli")
WHISPER_MODEL = os.path.join(os.path.dirname(os.path.dirname(__file__)), "bin", "models", "ggml-large-v3-turbo-q5_0.bin")
MED_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/medical/stereo"


def whisper_transcribe(audio_mono, sr, timeout=120):
    if sr != 16000:
        from scipy.signal import resample as scipy_resample
        num_samples = int(len(audio_mono) * 16000 / sr)
        audio_mono = scipy_resample(audio_mono, num_samples).astype(np.float32)

    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        tmp_path = tmp.name
        sf.write(tmp_path, audio_mono, 16000)

    try:
        cmd = [WHISPER_CLI, "-m", WHISPER_MODEL, "-l", "de", "-ml", "1", "-f", tmp_path]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout)
        lines = res.stdout.strip().split("\n")
        words = []
        for line in lines:
            m = re.match(r"\[(\d{2}):(\d{2}):(\d{2})\.(\d{3})\s*-->\s*(\d{2}):(\d{2}):(\d{2})\.(\d{3})\]\s*(.*)", line)
            if m:
                start_t = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000.0
                end_t = int(m.group(5)) * 3600 + int(m.group(6)) * 60 + int(m.group(7)) + int(m.group(8)) / 1000.0
                text = m.group(9).strip()
                if not text:
                    continue
                seg_words = text.split()
                if seg_words:
                    seg_dur = max((end_t - start_t) / len(seg_words), 0.01)
                    for wi, w in enumerate(seg_words):
                        ws = start_t + wi * seg_dur
                        we = ws + seg_dur
                        words.append((w, ws, we))
        return words
    except Exception as e:
        print(f"  Whisper error: {e}", file=sys.stderr)
        return []
    finally:
        os.unlink(tmp_path)


def main():
    wav_files = sorted(glob.glob(os.path.join(MED_DIR, "*.wav")))
    print(f"Found {len(wav_files)} medical WAV files in {MED_DIR}")

    updated = 0
    skipped = 0
    errors = 0

    for i, wav_path in enumerate(wav_files):
        fid = os.path.splitext(os.path.basename(wav_path))[0]
        json_path = wav_path.replace(".wav", ".json")

        if not os.path.exists(json_path):
            print(f"  {fid}: Missing JSON, skipping")
            errors += 1
            continue

        with open(json_path, "r", encoding="utf-8") as f:
            data = json.load(f)

        aligns = data.get("alignments", [])
        has_other = any(a[2] == "SPEAKER_OTHER" for a in aligns)
        if has_other:
            skipped += 1
            if (i + 1) % 50 == 0:
                print(f"  Progress: {i+1}/{len(wav_files)} (updated={updated}, skipped={skipped}, errors={errors})")
            continue

        try:
            audio, sr = sf.read(wav_path)
            if audio.ndim != 2 or audio.shape[1] < 2:
                print(f"  {fid}: Not stereo, skipping")
                errors += 1
                continue

            right_ch = audio[:, 1].astype(np.float32)
            rms = np.sqrt(np.mean(right_ch ** 2))
            if rms < 0.001:
                skipped += 1
                continue

            other_words = whisper_transcribe(right_ch, sr)

            if other_words:
                for w, ws, we in other_words:
                    aligns.append([w, [round(ws, 3), round(we, 3)], "SPEAKER_OTHER"])

                aligns.sort(key=lambda a: a[1][0])
                data["alignments"] = aligns

                with open(json_path, "w", encoding="utf-8") as f:
                    json.dump(data, f, ensure_ascii=False)

                updated += 1
            else:
                skipped += 1

        except Exception as e:
            print(f"  {fid}: Error - {e}", file=sys.stderr)
            errors += 1

        if (i + 1) % 10 == 0 or i == len(wav_files) - 1:
            print(f"  Progress: {i+1}/{len(wav_files)} (updated={updated}, skipped={skipped}, errors={errors})")

    print(f"\nDone: updated={updated}, skipped={skipped}, errors={errors}")


if __name__ == "__main__":
    main()
