import modal

app = modal.App("inspect-volume")

image = modal.Image.debian_slim().pip_install("soundfile")

@app.function(
    image=image,
    volumes={"/data": modal.Volume.from_name("moshi-german-data")},
)
def inspect_json():
    import os
    import soundfile as sf
    
    wav_path = "/data/stereo/med_0188.wav"
    if os.path.exists(wav_path):
        info = sf.info(wav_path)
        print(f"File: {wav_path}")
        print(f"  Sample Rate: {info.samplerate}")
        print(f"  Channels: {info.channels}")
        print(f"  Duration: {info.duration}")
    else:
        print(f"{wav_path} does not exist.")

@app.local_entrypoint()
def main():
    inspect_json.remote()
