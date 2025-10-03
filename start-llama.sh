#!/usr/bin/env bash
# start-llama.sh
# Start the LLaMA service

set -e

ROOT_DIR="/Users/whisper/Documents/augment-projects/clean-repo"
LLAMA_BIN="$ROOT_DIR/bin/llama-service"
MODEL_PATH="$ROOT_DIR/models/llama-model.gguf"
DB_PATH="$ROOT_DIR/whisper_talk.db"

echo "🦙 Starting LLaMA Service..."
echo "   Binary: $LLAMA_BIN"
echo "   Model: $MODEL_PATH"
echo "   Database: $DB_PATH"
echo ""

exec "$LLAMA_BIN" \
  --model "$MODEL_PATH" \
  --database "$DB_PATH" \
  --threads 8 \
  --ctx-size 2048 \
  --whisper-port 8083 \
  --kokoro-port 8090

