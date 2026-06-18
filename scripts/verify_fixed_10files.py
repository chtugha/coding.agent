#!/usr/bin/env python3
"""
Verify the first 10 files from the fixed preparation output.
"""
import os
import sys

# Add parent directory to path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Import and run the main verification function
from verify_podcasts_enhanced import verify_podcasts

FIXED_DIR = "/Volumes/eHDD/moshi-rag-data/processed/podcast_test_fixed"

def main():
    print("=" * 60)
    print("Verifying First 10 Files from Fixed Preparation")
    print("=" * 60)
    print(f"Directory: {FIXED_DIR}")
    print()
    
    # Run verification on first 10 files
    verify_podcasts(FIXED_DIR, force=True, max_files=10)

if __name__ == "__main__":
    main()

# Made with Bob
