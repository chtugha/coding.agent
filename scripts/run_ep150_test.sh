#!/bin/bash
cd /Users/whisper/zenflow_projects/coding.agent
echo "Starting preparation of episode 150..."
echo "Press Ctrl+C to stop"
echo ""
python3 scripts/prepare_german_dataset.py --dataset podcast --episodes 150-150 --output /Volumes/eHDD/moshi-rag-data/processed/podcast_test_fixed
echo ""
echo "Script finished. Press any key to close..."
read -n 1

# Made with Bob
