#!/usr/bin/env python3
"""
Test script for Kokoro TTS service
"""

import socket
import struct
import sys

def test_kokoro_service():
    """Test the Kokoro service by sending a text message and receiving audio"""
    
    # Connect to Kokoro service
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    
    try:
        print("🔗 Connecting to Kokoro service on port 8090...")
        sock.connect(('127.0.0.1', 8090))
        print("✅ Connected!")
        
        # Send HELLO message with call_id
        call_id = "999"
        hello_msg = call_id.encode('utf-8')
        sock.send(struct.pack('!I', len(hello_msg)))
        sock.send(hello_msg)
        print(f"👋 Sent HELLO with call_id={call_id}")
        
        # Send text to synthesize
        text = "Hello, this is a test of the Kokoro text to speech service."
        text_bytes = text.encode('utf-8')
        sock.send(struct.pack('!I', len(text_bytes)))
        sock.send(text_bytes)
        print(f"📝 Sent text: {text}")
        
        # Receive audio chunk
        print("🎧 Waiting for audio...")
        
        # Read header: [length][sample_rate][chunk_id]
        header = sock.recv(12)
        if len(header) != 12:
            print(f"❌ Failed to receive header (got {len(header)} bytes)")
            return False
        
        length, sample_rate, chunk_id = struct.unpack('!III', header)
        print(f"📦 Received header: length={length}, sample_rate={sample_rate}, chunk_id={chunk_id}")
        
        # Read audio data
        audio_data = b''
        while len(audio_data) < length:
            chunk = sock.recv(min(4096, length - len(audio_data)))
            if not chunk:
                break
            audio_data += chunk
        
        print(f"🔊 Received {len(audio_data)} bytes of audio data")
        print(f"   Expected {length / 4} float32 samples")
        print(f"   Duration: {(length / 4) / sample_rate:.2f} seconds")
        
        # Send BYE message
        sock.send(struct.pack('!I', 0xFFFFFFFF))
        print(f"👋 Sent BYE")
        
        print("✅ Test completed successfully!")
        return True
        
    except Exception as e:
        print(f"❌ Test failed: {e}")
        import traceback
        traceback.print_exc()
        return False
    finally:
        sock.close()


if __name__ == "__main__":
    success = test_kokoro_service()
    sys.exit(0 if success else 1)

