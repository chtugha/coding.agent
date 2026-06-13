import os
import wave
import json
import modal

app = modal.App("prepare-dataset-step1")

# Configure a Modal image with datasets, torchaudio, etc.
image = (
    modal.Image.debian_slim()
    .apt_install("ffmpeg")
    .pip_install("datasets", "torch", "torchaudio", "soundfile", "numpy", "torchcodec")
)

@app.function(
    image=image,
    volumes={"/data": modal.Volume.from_name("moshi-german-data")},
    timeout=600,
)
def process_step1():
    from datasets import load_dataset
    import torchaudio
    import torch

    print("Step 1: Downloading nyrahealth/disfluency_speech_german...")
    try:
        ds = load_dataset("nyrahealth/disfluency_speech_german")
        test_split = ds["test"]
        print(f"Downloaded disfluency dataset. Found {len(test_split)} samples.")
    except Exception as e:
        print("Failed to download nyrahealth/disfluency_speech_german:", e)
        return

    disfluency_dir = "/data/disfluency"
    os.makedirs(disfluency_dir, exist_ok=True)

    disfluency_samples = []

    print("Saving disfluency samples to /data/disfluency...")
    for idx, sample in enumerate(test_split):
        audio_data = sample["audio"]
        array = audio_data["array"]
        orig_sr = audio_data["sampling_rate"]
        
        # Resample to 24000Hz (Mimi standard)
        tensor = torch.tensor(array, dtype=torch.float32).unsqueeze(0) # [1, T]
        if orig_sr != 24000:
            resampler = torchaudio.transforms.Resample(orig_sr, 24000)
            tensor = resampler(tensor)
        
        wav_filename = f"nyra_{idx:04d}.wav"
        wav_path = os.path.join(disfluency_dir, wav_filename)
        
        # Save as 24kHz WAV
        torchaudio.save(wav_path, tensor, 24000)
        
        # Calculate duration
        duration = tensor.shape[1] / 24000.0
        
        # Parse transcript & create alignments
        transcript = sample["verbatim_transcript"] or sample["intended_transcript"] or ""
        words = transcript.strip().split()
        alignments = []
        if words:
            word_dur = duration / len(words)
            for w_idx, word in enumerate(words):
                alignments.append([
                    word,
                    [w_idx * word_dur, (w_idx + 1) * word_dur],
                    "SPEAKER_MAIN"
                ])
        
        json_filename = f"nyra_{idx:04d}.json"
        json_path = os.path.join(disfluency_dir, json_filename)
        with open(json_path, "w", encoding="utf-8") as f:
            json.dump({"alignments": alignments}, f, indent=2, ensure_ascii=False)
            
        disfluency_samples.append({
            "path": f"/data/disfluency/{wav_filename}",
            "duration": duration
        })

    print(f"Successfully processed {len(disfluency_samples)} disfluency samples.")

    # 4. Split 500 medical files in /data/stereo
    print("Splitting medical conversations in /data/stereo...")
    stereo_dir = "/data/stereo"
    if not os.path.exists(stereo_dir):
        print(f"Error: {stereo_dir} does not exist.")
        return

    all_files = os.listdir(stereo_dir)
    wav_files = sorted([f for f in all_files if f.endswith(".wav")])
    print(f"Found {len(wav_files)} medical wav files in /data/stereo.")

    med_train = []
    med_valid = []

    # Let's split 450 for train, 50 for validation
    for idx, f in enumerate(wav_files):
        wav_path = os.path.join(stereo_dir, f)
        # get duration using wave
        try:
            with wave.open(wav_path, "rb") as w:
                frames = w.getnframes()
                rate = w.getframerate()
                duration = frames / float(rate)
        except Exception as e:
            print(f"Error reading {f} duration:", e)
            duration = 0.0

        item = {
            "path": f"/data/stereo/{f}",
            "duration": duration
        }
        if idx < 450:
            med_train.append(item)
        else:
            med_valid.append(item)

    print(f"Split medical files: {len(med_train)} train, {len(med_valid)} validation.")

    # Write initial train.jsonl and valid.jsonl files under /data
    train_path = "/data/train.jsonl"
    valid_path = "/data/valid.jsonl"

    print("Writing initial train.jsonl and valid.jsonl...")
    with open(train_path, "w", encoding="utf-8") as f:
        for item in med_train:
            f.write(json.dumps(item) + "\n")

    with open(valid_path, "w", encoding="utf-8") as f:
        for item in med_valid:
            f.write(json.dumps(item) + "\n")
        for item in disfluency_samples:
            f.write(json.dumps(item) + "\n")

    print(f"Initial train.jsonl written with {len(med_train)} entries.")
    print(f"Initial valid.jsonl written with {len(med_valid) + len(disfluency_samples)} entries (50 medical + {len(disfluency_samples)} disfluency).")

@app.local_entrypoint()
def main():
    process_step1.remote()
