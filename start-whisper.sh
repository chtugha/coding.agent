#!/usr/bin/env bash
# start-whisper.sh
# Start the Whisper service

set -e

ROOT_DIR="/Users/whisper/Documents/augment-projects/clean-repo"
WHISPER_BIN="$ROOT_DIR/bin/whisper-service"
MODEL_PATH="$ROOT_DIR/models/ggml-base.en.bin"
DB_PATH="$ROOT_DIR/whisper_talk.db"

# Set library path for whisper.cpp
export DYLD_LIBRARY_PATH="$ROOT_DIR/bin:$ROOT_DIR/whisper-cpp/build/src:${DYLD_LIBRARY_PATH:-}"

echo "🎤 Starting Whisper Service..."
echo "   Binary: $WHISPER_BIN"
echo "   Model: $MODEL_PATH"
echo "   Database: $DB_PATH"
echo ""

exec "$WHISPER_BIN" \
  --model "$MODEL_PATH" \
  --database "$DB_PATH" \
  --threads 8 \
  --llama-host 127.0.0.1 \
  --llama-port 8083

