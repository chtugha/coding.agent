#!/usr/bin/env bash
# Test script to start all services with proper library paths

set -e

ROOT_DIR="/Users/whisper/Documents/augment-projects/clean-repo"
cd "$ROOT_DIR"

# Setup library paths
export DYLD_LIBRARY_PATH="$ROOT_DIR/whisper-cpp/build/src:$ROOT_DIR/libpiper/build:$DYLD_LIBRARY_PATH"

# Find ONNX Runtime library
ORT_LIB_DIR="$(ls -d "$ROOT_DIR/libpiper/lib"/onnxruntime-*/lib 2>/dev/null | head -n1 || true)"
if [ -n "$ORT_LIB_DIR" ]; then
    export DYLD_LIBRARY_PATH="$ORT_LIB_DIR:$DYLD_LIBRARY_PATH"
fi

echo "🔧 Library path: $DYLD_LIBRARY_PATH"
echo ""

# Kill any existing processes
echo "🛑 Killing existing processes..."
pkill -9 -f "sip-client|whisper-service|llama-service|piper-service|inbound-audio-processor|outbound-audio-processor" 2>/dev/null || true
sleep 1

# Start services in correct order: HTTP → SIP → Whisper → LLaMA → Piper
echo "🚀 Starting HTTP server..."
./bin/http-server > /tmp/http-server.log 2>&1 &
HTTP_PID=$!
echo "   PID: $HTTP_PID"
sleep 2

echo "🚀 Starting SIP client..."
./bin/sip-client --db whisper_talk.db > /tmp/sip-client.log 2>&1 &
SIP_PID=$!
echo "   PID: $SIP_PID"
sleep 3

echo "🚀 Starting Whisper service..."
./bin/whisper-service -m models/ggml-small.en.bin -d whisper_talk.db --llama-host 127.0.0.1 --llama-port 8083 > /tmp/whisper.log 2>&1 &
WHISPER_PID=$!
echo "   PID: $WHISPER_PID"
sleep 3

echo "🚀 Starting LLaMA service..."
./bin/llama-service -m models/Llama-3.2-3B-Instruct-Phishing-v1.Q5_K_M.gguf -d whisper_talk.db --piper-host 127.0.0.1 --piper-port 8084 > /tmp/llama.log 2>&1 &
LLAMA_PID=$!
echo "   PID: $LLAMA_PID"
sleep 3

echo "🚀 Starting Piper service..."
./bin/piper-service -m models/en_US-ryan-high.onnx -d whisper_talk.db > /tmp/piper.log 2>&1 &
PIPER_PID=$!
echo "   PID: $PIPER_PID"
sleep 3

echo ""
echo "✅ All services started!"
echo ""
echo "📊 Service PIDs:"
echo "   HTTP:    $HTTP_PID"
echo "   SIP:     $SIP_PID"
echo "   Whisper: $WHISPER_PID"
echo "   LLaMA:   $LLAMA_PID"
echo "   Piper:   $PIPER_PID"
echo ""
echo "📝 Logs:"
echo "   HTTP:    tail -f /tmp/http-server.log"
echo "   SIP:     tail -f /tmp/sip-client.log"
echo "   Whisper: tail -f /tmp/whisper.log"
echo "   LLaMA:   tail -f /tmp/llama.log"
echo "   Piper:   tail -f /tmp/piper.log"
echo ""
echo "🔍 Checking service status..."
sleep 2

ps -p $HTTP_PID > /dev/null && echo "✅ HTTP running" || echo "❌ HTTP died"
ps -p $SIP_PID > /dev/null && echo "✅ SIP running" || echo "❌ SIP died"
ps -p $WHISPER_PID > /dev/null && echo "✅ Whisper running" || echo "❌ Whisper died"
ps -p $LLAMA_PID > /dev/null && echo "✅ LLaMA running" || echo "❌ LLaMA died"
ps -p $PIPER_PID > /dev/null && echo "✅ Piper running" || echo "❌ Piper died"

echo ""
echo "Press Ctrl+C to stop all services..."

# Wait for user interrupt
trap "echo ''; echo '🛑 Stopping services...'; kill $HTTP_PID $SIP_PID $WHISPER_PID $LLAMA_PID $PIPER_PID 2>/dev/null; exit 0" INT TERM

wait

