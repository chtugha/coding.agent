#!/bin/bash
# Start Kokoro TTS Service
# This script launches the Kokoro service with the correct Python environment

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$SCRIPT_DIR"

# Configuration
VENV_PATH="./venv-kokoro/bin/python3"
SERVICE_SCRIPT="./kokoro_service.py"
VOICE="${KOKORO_VOICE:-af_sky}"
TCP_PORT="${KOKORO_TCP_PORT:-8090}"
UDP_PORT="${KOKORO_UDP_PORT:-13001}"
DEVICE="${KOKORO_DEVICE:-mps}"

# Check if virtual environment exists
if [ ! -f "$VENV_PATH" ]; then
    echo "❌ Virtual environment not found at $VENV_PATH"
    echo "   Please run: python3.11 -m venv venv-kokoro && ./venv-kokoro/bin/pip install torch kokoro soundfile"
    exit 1
fi

# Check if service script exists
if [ ! -f "$SERVICE_SCRIPT" ]; then
    echo "❌ Service script not found at $SERVICE_SCRIPT"
    exit 1
fi

echo "🚀 Starting Kokoro TTS Service..."
echo "   Voice: $VOICE"
echo "   TCP Port: $TCP_PORT"
echo "   UDP Port: $UDP_PORT"
echo "   Device: $DEVICE"

# Launch the service (use exec to replace shell process)
# This ensures signals are properly forwarded to the Python process
exec "$VENV_PATH" -u "$SERVICE_SCRIPT" \
    --voice "$VOICE" \
    --tcp-port "$TCP_PORT" \
    --udp-port "$UDP_PORT" \
    --device "$DEVICE"

