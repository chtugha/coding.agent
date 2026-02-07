#!/usr/bin/env python3
import sys
import os
import torch
import numpy as np
from pathlib import Path

# Add the current directory to sys.path to import kokoro_service
sys.path.append(os.getcwd())

try:
    from kokoro_service import KokoroTCPService
except ImportError as e:
    print(f"❌ Failed to import KokoroTCPService: {e}")
    sys.exit(1)

def verify_german():
    print("🧪 Verifying German TTS implementation...")
    
    # Initialize service (device=cpu for verification if mps is not available in this environment, 
    # but the service handles fallback anyway)
    service = KokoroTCPService(device="cpu") 
    
    # Check if German pipeline is loaded
    if 'de' not in service.pipelines:
        print("❌ German pipeline not loaded!")
        return False
    
    print("✅ German pipeline loaded.")
    
    # Check available voices
    print(f"🗣️ Cached voices: {list(service.cached_voices.keys())}")
    
    test_text = "Guten Tag, wie geht es Ihnen heute?"
    test_voice = "df_eva" # One of the German voices
    
    if test_voice not in service.cached_voices:
        print(f"⚠️ Voice {test_voice} not in cached_voices, checking if it exists in directories...")
    
    print(f"📝 Synthesizing German text: '{test_text}' with voice '{test_voice}'...")
    
    audio, sample_rate, synthesis_time = service.synthesize(test_text, voice_name=test_voice)
    
    if audio is not None and len(audio) > 0:
        print(f"✅ Success! Generated {len(audio)} samples at {sample_rate}Hz.")
        print(f"⚡ Synthesis time: {synthesis_time:.3f}s")
        return True
    else:
        print("❌ Failed to generate audio.")
        return False

if __name__ == "__main__":
    if verify_german():
        print("🎉 German TTS verification PASSED")
        sys.exit(0)
    else:
        print("💀 German TTS verification FAILED")
        sys.exit(1)
