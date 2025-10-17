#!/bin/bash
set -e

cd /Users/whisper/Documents/augment-projects/clean-repo

echo "=== Building whisper-service and whisper_inbound_sim ==="
cd build
make whisper-service whisper_inbound_sim -j4
cd ..

echo "=== Killing any existing whisper-service ==="
pkill -9 whisper-service || true
sleep 2

echo "=== Starting whisper-service ==="
./bin/whisper-service \
  --model ./models/ggml-large-v3-turbo-q5_0.bin \
  --database ./whisper_talk.db \
  --threads 8 \
  --llama-host 127.0.0.1 \
  --llama-port 8083 > /tmp/whisper-service.log 2>&1 &

WHISPER_PID=$!
sleep 8

echo "=== Running WER test on first 3 Harvard files ==="
./bin/whisper_inbound_sim \
  ./tests/data/harvard/wav/OSR_us_000_0010_8k.wav \
  ./tests/data/harvard/wav/OSR_us_000_0011_8k.wav \
  ./tests/data/harvard/wav/OSR_us_000_0012_8k.wav

echo "=== Test completed ==="
kill $WHISPER_PID || true

