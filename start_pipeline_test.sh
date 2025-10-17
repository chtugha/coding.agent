#!/usr/bin/env bash
# start_pipeline_test.sh
# Complete pipeline loop test: Whisper → Llama → Kokoro → Whisper
# Implements proper service startup sequencing with 2-minute timeout

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
LLAMA_MODEL="$ROOT_DIR/models/Llama-3.2-3B-Instruct-Phishing-v1.Q5_K_M.gguf"
DB_PATH="$ROOT_DIR/whisper_talk.db"

TEST_WAV="$ROOT_DIR/tests/data/harvard/wav/OSR_us_000_0010_8k.wav"
CALL_ID="151"

# Check if binaries exist
echo "🔍 Checking prerequisites..."

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

echo "✅ All prerequisites found"
echo ""

# Kill any existing services
echo "🧹 Cleaning up existing services..."
pkill -9 whisper-service 2>/dev/null || true
pkill -9 llama-service 2>/dev/null || true
pkill -f kokoro_service.py 2>/dev/null || true
pkill -9 outbound-audio-processor 2>/dev/null || true
pkill -9 pipeline_loop_sim 2>/dev/null || true
sleep 2

# Cleanup function
cleanup() {
    echo ""
    echo "🧹 Cleaning up services..."
    
    # Kill simulator first
    if [ ! -z "$SIMULATOR_PID" ]; then
        kill -TERM $SIMULATOR_PID 2>/dev/null || true
        sleep 1
        kill -9 $SIMULATOR_PID 2>/dev/null || true
    fi
    
    # Kill services
    if [ ! -z "$WHISPER_PID" ]; then
        kill -TERM $WHISPER_PID 2>/dev/null || true
        sleep 1
        kill -9 $WHISPER_PID 2>/dev/null || true
    fi
    
    if [ ! -z "$LLAMA_PID" ]; then
        kill -TERM $LLAMA_PID 2>/dev/null || true
        sleep 1
        kill -9 $LLAMA_PID 2>/dev/null || true
    fi
    
    if [ ! -z "$KOKORO_PID" ]; then
        kill -TERM $KOKORO_PID 2>/dev/null || true
        sleep 1
        kill -9 $KOKORO_PID 2>/dev/null || true
    fi

    echo "✅ Cleanup complete"
    echo ""
    echo "📊 Service logs:"
    echo "   Simulator:  /tmp/pipeline-simulator.log"
    echo "   Whisper:    /tmp/whisper-pipeline-test.log"
    echo "   Llama:      /tmp/llama-pipeline-test.log"
    echo "   Kokoro:     /tmp/kokoro-pipeline-test.log"
    echo ""
}

# Set trap to cleanup on exit
trap cleanup EXIT INT TERM

# Start services in proper order
echo "🚀 Starting services..."
echo ""

# Step 1: Start Llama service FIRST (must be on port 8083)
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
echo "   Ports: 8083 (input from Whisper), 8090 (output to Kokoro)"
echo "   Log: /tmp/llama-pipeline-test.log"
sleep 3

# Step 2: Start Kokoro service
echo "🎵 Starting Kokoro service..."
"$KOKORO_VENV" -u "$KOKORO_SCRIPT" \
  --voice af_sky \
  --tcp-port 8090 \
  --udp-port 13001 \
  --device mps \
  > /tmp/kokoro-pipeline-test.log 2>&1 &
KOKORO_PID=$!
echo "   PID: $KOKORO_PID"
echo "   Ports: 8090 (input from Llama), 9002+call_id (output to simulator), 13001 (UDP registration)"
echo "   Log: /tmp/kokoro-pipeline-test.log"
sleep 5

# Step 3: Start Whisper service
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
echo "   Ports: 9001+call_id (audio), 13000 (UDP registration)"
echo "   Log: /tmp/whisper-pipeline-test.log"
sleep 3

# Step 4: Start simulator (mimics outbound-audio-processor)
echo "🔄 Starting pipeline loop simulator..."
"$SIMULATOR_BIN" "$TEST_WAV" > /tmp/pipeline-simulator.log 2>&1 &
SIMULATOR_PID=$!
echo "   PID: $SIMULATOR_PID"
echo "   Ports: 9152 (audio to Whisper), 9153 (audio from Kokoro)"
echo "   Log: /tmp/pipeline-simulator.log"
sleep 2

echo ""
echo "✅ All services started"
echo ""
echo "📊 Service Architecture:"
echo "   Simulator → Whisper (port 9152) → Llama (port 8083) → Kokoro (port 8090) → Simulator (port 9153)"
echo "   Complete loop: Original audio → Whisper → Llama → Kokoro → Simulator → Whisper"
echo ""
echo "⏱️  Test will run for maximum 2 minutes..."
echo ""

# Wait for simulator to complete (with 2-minute timeout)
TIMEOUT=120
ELAPSED=0
while kill -0 $SIMULATOR_PID 2>/dev/null; do
    sleep 1
    ELAPSED=$((ELAPSED + 1))
    
    if [ $ELAPSED -ge $TIMEOUT ]; then
        echo ""
        echo "⏰ Timeout reached (2 minutes) - stopping test"
        break
    fi
    
    # Show progress every 10 seconds
    if [ $((ELAPSED % 10)) -eq 0 ]; then
        echo "⏱️  Elapsed: ${ELAPSED}s / ${TIMEOUT}s"
    fi
done

echo ""
echo "🏁 Test complete"
echo ""

# Show simulator output
echo "📋 Simulator output:"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
tail -100 /tmp/pipeline-simulator.log
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo ""

# Cleanup will be called by trap

