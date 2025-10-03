#!/usr/bin/env python3
"""
Download Kokoro main model from Hugging Face
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

REPO_ID = "hexgrad/Kokoro-82M"
MODEL_DIR = "models/kokoro-model"

def download_file(filename, description):
    """Download a single file"""
    try:
        print(f"📥 Downloading {description}...")
        
        file_path = hf_hub_download(
            repo_id=REPO_ID,
            filename=filename,
            local_dir=MODEL_DIR,
            local_dir_use_symlinks=False
        )
        
        # Get file size
        size_mb = Path(file_path).stat().st_size / (1024 * 1024)
        print(f"✅ Downloaded {description} ({size_mb:.2f} MB)")
        print(f"   Location: {file_path}")
        return True
        
    except Exception as e:
        print(f"❌ Failed to download {description}: {e}")
        return False

def main():
    """Download Kokoro model files"""
    print("🎤 Kokoro Model Downloader")
    print(f"   Repository: {REPO_ID}")
    print(f"   Target directory: {MODEL_DIR}")
    print()
    
    # Create directory if it doesn't exist
    os.makedirs(MODEL_DIR, exist_ok=True)
    
    # Files to download
    files = [
        ("kokoro-v1_0.pth", "Main model (kokoro-v1_0.pth)"),
        ("config.json", "Model configuration"),
    ]
    
    success_count = 0
    
    for filename, description in files:
        if download_file(filename, description):
            success_count += 1
        print()
    
    # Summary
    print("=" * 60)
    print(f"✅ Successfully downloaded: {success_count}/{len(files)} files")
    
    if success_count == len(files):
        print("🎉 All model files downloaded successfully!")
    
    # List downloaded files
    model_path = Path(MODEL_DIR)
    if model_path.exists():
        print(f"\n📁 Downloaded files in {model_path}:")
        for file in sorted(model_path.glob("*")):
            if file.is_file():
                size_mb = file.stat().st_size / (1024 * 1024)
                print(f"   {file.name} ({size_mb:.2f} MB)")

if __name__ == "__main__":
    main()

