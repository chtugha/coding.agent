#!/usr/bin/env bash
# test_pipeline_loop.sh
# Test the complete pipeline loop: Whisper → Llama → Kokoro → Whisper

set -e

ROOT_DIR="/Users/whisper/Documents/augment-projects/clean-repo"
cd "$ROOT_DIR"

# Configuration
WHISPER_BIN="$ROOT_DIR/bin/whisper-service"
LLAMA_BIN="$ROOT_DIR/bin/llama-service"
KOKORO_SCRIPT="$ROOT_DIR/kokoro_service.py"
KOKORO_VENV="$ROOT_DIR/venv-kokoro/bin/python3"
SIMULATOR_BIN="$ROOT_DIR/bin/pipeline_loop_sim"

WHISPER_MODEL="$ROOT_DIR/models/ggml-large-v3-turbo-q5_0.bin"
LLAMA_MODEL="$ROOT_DIR/models/llama-model.gguf"
DB_PATH="$ROOT_DIR/whisper_talk.db"

TEST_WAV="$ROOT_DIR/tests/data/harvard/wav/OSR_us_000_0010_8k.wav"

# Check if binaries exist
if [ ! -f "$WHISPER_BIN" ]; then
    echo "❌ Whisper binary not found: $WHISPER_BIN"
    echo "   Run: bash scripts/build.sh"
    exit 1
fi

if [ ! -f "$LLAMA_BIN" ]; then
    echo "❌ Llama binary not found: $LLAMA_BIN"
    echo "   Run: bash scripts/build.sh"
    exit 1
fi

if [ ! -f "$KOKORO_VENV" ]; then
    echo "❌ Kokoro virtual environment not found: $KOKORO_VENV"
    echo "   Run: python3.11 -m venv venv-kokoro && ./venv-kokoro/bin/pip install torch kokoro soundfile"
    exit 1
fi

if [ ! -f "$KOKORO_SCRIPT" ]; then
    echo "❌ Kokoro service script not found: $KOKORO_SCRIPT"
    exit 1
fi

if [ ! -f "$SIMULATOR_BIN" ]; then
    echo "❌ Pipeline simulator not found: $SIMULATOR_BIN"
    echo "   Run: bash scripts/build.sh"
    exit 1
fi

if [ ! -f "$TEST_WAV" ]; then
    echo "❌ Test WAV file not found: $TEST_WAV"
    exit 1
fi

# Kill any existing services
echo "🧹 Cleaning up existing services..."
pkill -9 whisper-service 2>/dev/null || true
pkill -9 llama-service 2>/dev/null || true
pkill -f kokoro_service.py 2>/dev/null || true
sleep 2

# Start services
echo ""
echo "🚀 Starting services..."
echo ""

# Start Whisper service
echo "🎤 Starting Whisper service..."
"$WHISPER_BIN" \
  --model "$WHISPER_MODEL" \
  --database "$DB_PATH" \
  --threads 8 \
  --llama-host 127.0.0.1 \
  --llama-port 8083 \
  > /tmp/whisper-pipeline-test.log 2>&1 &
WHISPER_PID=$!
echo "   PID: $WHISPER_PID"
sleep 3

# Start Llama service
echo "🦙 Starting Llama service..."
"$LLAMA_BIN" \
  --model "$LLAMA_MODEL" \
  --database "$DB_PATH" \
  --port 8083 \
  --threads 8 \
  --out-host 127.0.0.1 \
  --out-port 8090 \
  > /tmp/llama-pipeline-test.log 2>&1 &
LLAMA_PID=$!
echo "   PID: $LLAMA_PID"
sleep 3

# Start Kokoro service
echo "🎵 Starting Kokoro service..."
"$KOKORO_VENV" -u "$KOKORO_SCRIPT" \
  --voice af_sky \
  --tcp-port 8090 \
  --udp-port 13001 \
  --device mps \
  > /tmp/kokoro-pipeline-test.log 2>&1 &
KOKORO_PID=$!
echo "   PID: $KOKORO_PID"
sleep 5

echo ""
echo "✅ All services started"
echo ""

# Cleanup function
cleanup() {
    echo ""
    echo "🧹 Cleaning up services..."
    kill -TERM $WHISPER_PID 2>/dev/null || true
    kill -TERM $LLAMA_PID 2>/dev/null || true
    kill -TERM $KOKORO_PID 2>/dev/null || true
    sleep 2
    kill -9 $WHISPER_PID 2>/dev/null || true
    kill -9 $LLAMA_PID 2>/dev/null || true
    kill -9 $KOKORO_PID 2>/dev/null || true
    echo "✅ Cleanup complete"
}

# Set trap to cleanup on exit
trap cleanup EXIT INT TERM

# Run the pipeline loop simulator
echo "🔄 Running pipeline loop simulator..."
echo ""
"$SIMULATOR_BIN" "$TEST_WAV"

echo ""
echo "📊 Service logs:"
echo ""
echo "Whisper log: /tmp/whisper-pipeline-test.log"
echo "Llama log:   /tmp/llama-pipeline-test.log"
echo "Kokoro log:  /tmp/kokoro-pipeline-test.log"
echo ""

