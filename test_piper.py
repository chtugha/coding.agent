#!/usr/bin/env python3
"""
Test script for Piper TTS service
"""
import socket
import struct
import sys

def send_message(sock, message):
    """Send a length-prefixed message"""
    length = len(message)
    sock.send(struct.pack('!I', length))  # Network byte order (big-endian)
    sock.send(message.encode('utf-8'))

def test_piper_service(host='localhost', port=8090):
    """Test the Piper TTS service"""
    try:
        # Connect to piper service
        sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        sock.connect((host, port))
        print(f"Connected to Piper service at {host}:{port}")
        
        # Send HELLO with call_id
        call_id = "test_call_123"
        print(f"Sending HELLO with call_id: {call_id}")
        send_message(sock, call_id)
        
        # Send text to synthesize
        text = "Hello, this is a test of the Piper text-to-speech service. How are you today?"
        print(f"Sending text: {text}")
        send_message(sock, text)
        
        # Send BYE to end the session
        print("Sending BYE")
        send_message(sock, "BYE")
        
        # Try to read response
        try:
            response_len = struct.unpack('!I', sock.recv(4))[0]
            if response_len > 0:
                response = sock.recv(response_len).decode('utf-8')
                print(f"Response: {response}")
        except:
            print("No response received (this is normal)")
        
        sock.close()
        print("Test completed successfully")
        
    except Exception as e:
        print(f"Error: {e}")
        return False
    
    return True

if __name__ == "__main__":
    # Test multiple requests to verify continuous operation
    for i in range(3):
        print(f"\n=== Test {i+1} ===")
        test_piper_service()
        import time
        time.sleep(1)
