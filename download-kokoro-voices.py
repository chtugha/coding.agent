#!/usr/bin/env python3
"""
Download Kokoro voice models from Hugging Face
"""

import os
import sys
from pathlib import Path

# Check if huggingface_hub is installed
try:
    from huggingface_hub import hf_hub_download
except ImportError:
    print("❌ huggingface_hub not installed")
    print("   Installing huggingface_hub...")
    import subprocess
    subprocess.check_call([sys.executable, "-m", "pip", "install", "huggingface_hub"])
    from huggingface_hub import hf_hub_download

# Kokoro voice models
VOICES = [
    "af_sky",
    "af_bella", 
    "af_sarah",
    "am_adam",
    "am_michael",
    "bf_emma",
    "bf_isabella",
    "bm_george",
    "bm_lewis"
]

REPO_ID = "hexgrad/Kokoro-82M"
VOICES_DIR = "models/kokoro-voices"

def download_voice(voice_id):
    """Download a single voice model"""
    try:
        print(f"📥 Downloading {voice_id}...")
        
        # Download the voice file
        file_path = hf_hub_download(
            repo_id=REPO_ID,
            filename=f"voices/{voice_id}.pt",
            local_dir=VOICES_DIR,
            local_dir_use_symlinks=False
        )
        
        print(f"✅ Downloaded {voice_id} to {file_path}")
        return True
        
    except Exception as e:
        print(f"❌ Failed to download {voice_id}: {e}")
        return False

def main():
    """Download all Kokoro voices"""
    print("🎤 Kokoro Voice Downloader")
    print(f"   Repository: {REPO_ID}")
    print(f"   Target directory: {VOICES_DIR}")
    print(f"   Voices to download: {len(VOICES)}")
    print()
    
    # Create directory if it doesn't exist
    os.makedirs(VOICES_DIR, exist_ok=True)
    
    # Download each voice
    success_count = 0
    failed_voices = []
    
    for voice_id in VOICES:
        if download_voice(voice_id):
            success_count += 1
        else:
            failed_voices.append(voice_id)
        print()
    
    # Summary
    print("=" * 60)
    print(f"✅ Successfully downloaded: {success_count}/{len(VOICES)} voices")
    
    if failed_voices:
        print(f"❌ Failed to download: {', '.join(failed_voices)}")
    else:
        print("🎉 All voices downloaded successfully!")
    
    # List downloaded files
    voices_path = Path(VOICES_DIR) / "voices"
    if voices_path.exists():
        print(f"\n📁 Downloaded files in {voices_path}:")
        for file in sorted(voices_path.glob("*.pt")):
            size_mb = file.stat().st_size / (1024 * 1024)
            print(f"   {file.name} ({size_mb:.2f} MB)")

if __name__ == "__main__":
    main()

