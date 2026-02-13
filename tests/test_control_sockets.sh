#!/bin/bash
set -e

echo "🧪 Testing Unix Socket Control Infrastructure"
echo "=============================================="

# Clean up any existing sockets
rm -f /tmp/inbound-audio-processor.ctrl
rm -f /tmp/whisper-service.ctrl
rm -f /tmp/llama-service.ctrl
rm -f /tmp/kokoro-service.ctrl
rm -f /tmp/outbound-audio-processor.ctrl

cd "$(dirname "$0")/.."

# Start services in background
echo "📍 Starting Inbound Audio Processor..."
./bin/inbound-audio-processor > /tmp/inbound-test.log 2>&1 &
INBOUND_PID=$!
sleep 1

echo "📍 Starting Whisper Service (without model for socket test)..."
# We'll skip whisper since it needs a model
# ./bin/whisper-service dummy > /tmp/whisper-test.log 2>&1 &
# WHISPER_PID=$!
# sleep 1

echo "📍 Starting LLaMA Service (without model for socket test)..."
# We'll skip llama since it needs a model
# ./bin/llama-service dummy > /tmp/llama-test.log 2>&1 &
# LLAMA_PID=$!
# sleep 1

echo "📍 Starting Outbound Audio Processor..."
./bin/outbound-audio-processor > /tmp/outbound-test.log 2>&1 &
OUTBOUND_PID=$!
sleep 1

# Check if sockets were created
echo ""
echo "🔍 Checking for Unix sockets..."
SOCKETS_OK=true

if [ -S /tmp/inbound-audio-processor.ctrl ]; then
    echo "✅ /tmp/inbound-audio-processor.ctrl exists"
else
    echo "❌ /tmp/inbound-audio-processor.ctrl NOT found"
    SOCKETS_OK=false
fi

if [ -S /tmp/outbound-audio-processor.ctrl ]; then
    echo "✅ /tmp/outbound-audio-processor.ctrl exists"
else
    echo "❌ /tmp/outbound-audio-processor.ctrl NOT found"
    SOCKETS_OK=false
fi

# Test sending a signal
echo ""
echo "🧪 Testing signal propagation..."
echo "Sending CALL_START:1 to Inbound Audio Processor..."
echo "CALL_START:1" | nc -U /tmp/inbound-audio-processor.ctrl || echo "⚠️  Signal send failed (expected if Whisper not running)"

sleep 1

# Check logs
echo ""
echo "📋 Inbound Processor Log:"
tail -5 /tmp/inbound-test.log || true

echo ""
echo "📋 Outbound Processor Log:"
tail -5 /tmp/outbound-test.log || true

# Clean up
echo ""
echo "🧹 Cleaning up..."
kill $INBOUND_PID 2>/dev/null || true
kill $OUTBOUND_PID 2>/dev/null || true
sleep 1

rm -f /tmp/inbound-test.log /tmp/outbound-test.log

if [ "$SOCKETS_OK" = true ]; then
    echo "✅ Socket infrastructure test PASSED"
    exit 0
else
    echo "❌ Socket infrastructure test FAILED"
    exit 1
fi
