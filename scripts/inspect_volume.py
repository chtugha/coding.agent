import modal

app = modal.App("inspect-volume")

@app.function(
    volumes={"/data": modal.Volume.from_name("moshi-german-data")},
)
def inspect_json():
    import os
    import json
    
    json_path = "/data/stereo/mec_0000.json"
    if os.path.exists(json_path):
        with open(json_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            print(json.dumps(data, indent=2)[:1000])
    else:
        print(f"{json_path} does not exist.")

@app.local_entrypoint()
def main():
    inspect_json.remote()
