#!/bin/bash
# Quick WER test script

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

# Run test
echo "Running WER test on first 3 Harvard files..."
./bin/whisper_inbound_sim \
  ./tests/data/harvard/wav/OSR_us_000_0010_8k.wav \
  ./tests/data/harvard/wav/OSR_us_000_0011_8k.wav \
  ./tests/data/harvard/wav/OSR_us_000_0012_8k.wav 2>&1 | tee /tmp/wer_test.log

# Kill service
kill $WHISPER_PID 2>/dev/null || true
pkill -9 whisper-service 2>/dev/null || true

echo "Test completed. Results in /tmp/wer_test.log"

