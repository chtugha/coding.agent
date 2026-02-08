#!/usr/bin/env bash
# Test script to start all services with proper library paths

set -e

ROOT_DIR="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT_DIR"

# Setup library paths
export DYLD_LIBRARY_PATH="$ROOT_DIR/whisper-cpp/build/src:$ROOT_DIR/llama-cpp/build/bin:$DYLD_LIBRARY_PATH"

# Find ONNX Runtime library (if exists, though we might use Kokoro now)
ORT_LIB_DIR="$(ls -d "$ROOT_DIR/libpiper/lib"/onnxruntime-*/lib 2>/dev/null | head -n1 || true)"
if [ -n "$ORT_LIB_DIR" ]; then
    export DYLD_LIBRARY_PATH="$ORT_LIB_DIR:$DYLD_LIBRARY_PATH"
fi

echo "🔧 Library path: $DYLD_LIBRARY_PATH"
echo ""

# Kill any existing processes
echo "🛑 Killing existing processes..."
pkill -9 -f "sip-client|whisper-service|llama-service|piper-service|kokoro_service|inbound-audio-processor|outbound-audio-processor" 2>/dev/null || true
sleep 1

# Start services in correct order: HTTP → SIP → Whisper → LLaMA → Kokoro
echo "🚀 Starting HTTP server..."
./bin/http-server > /tmp/http-server.log 2>&1 &
HTTP_PID=$!
echo "   PID: $HTTP_PID"
sleep 2

echo "🚀 Starting Inbound Audio Processor..."
./bin/inbound-audio-processor > /tmp/inbound-audio.log 2>&1 &
INBOUND_PID=$!
echo "   PID: $INBOUND_PID"
sleep 1

echo "🚀 Starting Outbound Audio Processor..."
./bin/outbound-audio-processor > /tmp/outbound-audio.log 2>&1 &
OUTBOUND_PID=$!
echo "   PID: $OUTBOUND_PID"
sleep 1

echo "🚀 Starting SIP client..."
./bin/sip-client --user 1000 --pass password --server 127.0.0.1 > /tmp/sip-client.log 2>&1 &
SIP_PID=$!
echo "   PID: $SIP_PID"
sleep 2

echo "🚀 Starting Whisper service..."
# Using the model found in whisper-cpp/models
./bin/whisper-service -m whisper-cpp/models/ggml-large-v3.bin --llama-host 127.0.0.1 --llama-port 8083 > /tmp/whisper.log 2>&1 &
WHISPER_PID=$!
echo "   PID: $WHISPER_PID"
sleep 3

echo "🚀 Starting LLaMA service..."
./bin/llama-service -m models/llama-model.gguf --out-host 127.0.0.1 --out-port 8090 > /tmp/llama.log 2>&1 &
LLAMA_PID=$!
echo "   PID: $LLAMA_PID"
sleep 3

echo "🚀 Starting Kokoro TTS service..."
./venv-kokoro/bin/python3 kokoro_service.py > /tmp/kokoro.log 2>&1 &
KOKORO_PID=$!
echo "   PID: $KOKORO_PID"
sleep 3

echo ""
echo "✅ All services started!"
echo ""
echo "📊 Service PIDs:"
echo "   HTTP:    $HTTP_PID"
echo "   INBOUND: $INBOUND_PID"
echo "   OUTBOUND:$OUTBOUND_PID"
echo "   SIP:     $SIP_PID"
echo "   Whisper: $WHISPER_PID"
echo "   LLaMA:   $LLAMA_PID"
echo "   Kokoro:  $KOKORO_PID"
echo ""

echo "🔍 Checking service status..."
sleep 2

ps -p $HTTP_PID > /dev/null && echo "✅ HTTP running" || echo "❌ HTTP died"
ps -p $INBOUND_PID > /dev/null && echo "✅ INBOUND running" || echo "❌ INBOUND died"
ps -p $OUTBOUND_PID > /dev/null && echo "✅ OUTBOUND running" || echo "❌ OUTBOUND died"
ps -p $SIP_PID > /dev/null && echo "✅ SIP running" || echo "❌ SIP died"
ps -p $WHISPER_PID > /dev/null && echo "✅ Whisper running" || echo "❌ Whisper died"
ps -p $LLAMA_PID > /dev/null && echo "✅ LLaMA running" || echo "❌ LLaMA died"
ps -p $KOKORO_PID > /dev/null && echo "✅ Kokoro running" || echo "❌ Kokoro died"

echo ""
echo "Logs are available in /tmp/*.log"
echo "Press Ctrl+C to stop all services..."

# Wait for user interrupt
trap "echo ''; echo '🛑 Stopping services...'; kill $HTTP_PID $INBOUND_PID $OUTBOUND_PID $SIP_PID $WHISPER_PID $LLAMA_PID $KOKORO_PID 2>/dev/null; exit 0" INT TERM

wait

