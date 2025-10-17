#!/bin/bash
# Test script for next 3 Harvard files (0013, 0014, 0015)

cd /Users/whisper/Documents/augment-projects/clean-repo

# Kill any existing service
pkill -9 whisper-service 2>/dev/null || true
sleep 2

# Start service in background
echo "Starting whisper-service..."
./bin/whisper-service \
  --model ./models/ggml-large-v3-turbo-q5_0.bin \
  --database ./whisper_talk.db \
  --threads 8 \
  --llama-host 127.0.0.1 \
  --llama-port 8083 > /tmp/whisper-service.log 2>&1 &

WHISPER_PID=$!
sleep 10

# Run test on next 3 files
echo "Running WER test on next 3 Harvard files (0013, 0014, 0015)..."
./bin/whisper_inbound_sim \
  ./tests/data/harvard/wav/OSR_us_000_0013_8k.wav \
  ./tests/data/harvard/wav/OSR_us_000_0014_8k.wav \
  ./tests/data/harvard/wav/OSR_us_000_0015_8k.wav 2>&1 | tee /tmp/wer_test_next3.log

# Kill service
kill $WHISPER_PID 2>/dev/null || true
pkill -9 whisper-service 2>/dev/null || true

echo "Test completed. Results in /tmp/wer_test_next3.log"

