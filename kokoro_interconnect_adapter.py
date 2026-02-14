#!/usr/bin/env python3
"""
Kokoro Interconnect Adapter
Temporary Python adapter that implements the interconnect protocol
for the existing kokoro_service.py until C++ port is complete.
"""

import socket
import struct
import threading
import time
import sys
from pathlib import Path

try:
    from kokoro import KPipeline
except ImportError:
    print("❌ Kokoro not installed. Install with: pip install torch kokoro")
    sys.exit(1)


class InterconnectAdapter:
    """Python implementation of interconnect protocol for Kokoro service"""
    
    SERVICE_TYPE_KOKORO = 5
    BASE_NEG_IN = 22222
    BASE_NEG_OUT = 33333
    
    def __init__(self, voice="df_eva", device="mps"):
        self.running = True
        self.is_master = False
        self.neg_in_port = 0
        self.neg_out_port = 0
        self.neg_in_sock = None
        self.neg_out_sock = None
        
        self.down_in_port = 0
        self.down_out_port = 0
        self.down_in_sock = None
        self.down_out_sock = None
        self.down_in_conn = None
        self.down_out_conn = None
        
        self.up_in_port = 0
        self.up_out_port = 0
        self.up_in_sock = None
        self.up_out_sock = None
        
        self.default_voice = voice
        self.pipeline = None
        self.device = device
        
        self.active_calls = {}
        
    def scan_and_bind_ports(self):
        """Scan for free ports and bind negotiation ports"""
        for i in range(0, 100):
            try:
                neg_in = self.BASE_NEG_IN + (i * 3)
                neg_out = self.BASE_NEG_OUT + (i * 3)
                
                sock_in = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock_in.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                sock_in.bind(('127.0.0.1', neg_in))
                
                sock_out = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                sock_out.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                sock_out.bind(('127.0.0.1', neg_out))
                
                self.neg_in_port = neg_in
                self.neg_out_port = neg_out
                self.neg_in_sock = sock_in
                self.neg_out_sock = sock_out
                
                if neg_in == self.BASE_NEG_IN:
                    self.is_master = True
                    print(f"🏆 Master mode: negotiation ports {neg_in}, {neg_out}")
                else:
                    self.is_master = False
                    print(f"👤 Slave mode: negotiation ports {neg_in}, {neg_out}")
                
                return True
                
            except OSError:
                if sock_in:
                    sock_in.close()
                if sock_out:
                    sock_out.close()
                continue
        
        return False
    
    def bind_traffic_ports(self):
        """Bind traffic ports based on negotiation ports"""
        self.down_in_port = self.neg_in_port + 1
        self.down_out_port = self.neg_in_port + 2
        self.up_in_port = self.neg_out_port + 1
        self.up_out_port = self.neg_out_port + 2
        
        self.down_in_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.down_in_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.down_in_sock.bind(('127.0.0.1', self.down_in_port))
        self.down_in_sock.listen(1)
        
        self.down_out_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.down_out_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.down_out_sock.bind(('127.0.0.1', self.down_out_port))
        self.down_out_sock.listen(1)
        
        print(f"📡 Traffic ports (down): {self.down_in_port}, {self.down_out_port}")
        print(f"📡 Traffic ports (up): {self.up_in_port}, {self.up_out_port}")
        
        return True
    
    def register_with_master(self):
        """Register as slave with master"""
        if self.is_master:
            return True
        
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect(('127.0.0.1', self.BASE_NEG_IN))
            msg = f"REGISTER {self.SERVICE_TYPE_KOKORO} {self.neg_in_port} {self.neg_out_port}\n"
            sock.sendall(msg.encode('utf-8'))
            response = sock.recv(1024).decode('utf-8')
            sock.close()
            
            if "REGISTER_ACK" in response:
                print(f"✅ Registered with master")
                return True
        except Exception as e:
            print(f"⚠️  Failed to register with master: {e}")
        
        return False
    
    def connect_to_downstream(self):
        """Connect to downstream (Outbound Audio Processor)"""
        try:
            sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            sock.connect(('127.0.0.1', self.BASE_NEG_IN))
            msg = f"GET_DOWNSTREAM {self.SERVICE_TYPE_KOKORO}\n"
            sock.sendall(msg.encode('utf-8'))
            response = sock.recv(1024).decode('utf-8')
            sock.close()
            
            if "DOWNSTREAM_PORTS" in response:
                parts = response.split()
                down_in = int(parts[1]) + 1
                down_out = int(parts[1]) + 2
                
                self.up_out_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.up_out_sock.connect(('127.0.0.1', down_in))
                
                self.up_in_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                self.up_in_sock.connect(('127.0.0.1', down_out))
                
                print(f"🔗 Connected to downstream (OAP) on ports {down_in}, {down_out}")
                return True
        except Exception as e:
            print(f"⚠️  Failed to connect to downstream: {e}")
        
        return False
    
    def accept_from_upstream(self):
        """Accept connections from upstream (LLaMA)"""
        threading.Thread(target=self._accept_down_in, daemon=True).start()
        threading.Thread(target=self._accept_down_out, daemon=True).start()
        return True
    
    def _accept_down_in(self):
        """Accept downstream in connection (LLaMA sends text here)"""
        print(f"👂 Waiting for upstream connection on port {self.down_in_port}...")
        self.down_in_conn, addr = self.down_in_sock.accept()
        print(f"✅ Upstream connected on down_in from {addr}")
    
    def _accept_down_out(self):
        """Accept downstream out connection (we send reverse data here if needed)"""
        print(f"👂 Waiting for upstream connection on port {self.down_out_port}...")
        self.down_out_conn, addr = self.down_out_sock.accept()
        print(f"✅ Upstream connected on down_out from {addr}")
    
    def recv_packet(self):
        """Receive a packet from upstream (LLaMA)"""
        if not self.down_in_conn:
            return None
        
        try:
            header = self.down_in_conn.recv(8)
            if len(header) != 8:
                return None
            
            call_id, payload_size = struct.unpack('!II', header)
            
            if payload_size == 0 or payload_size > 1024 * 1024:
                return None
            
            payload = b''
            while len(payload) < payload_size:
                chunk = self.down_in_conn.recv(payload_size - len(payload))
                if not chunk:
                    return None
                payload += chunk
            
            return {
                'call_id': call_id,
                'payload_size': payload_size,
                'payload': payload
            }
        except Exception as e:
            return None
    
    def send_packet(self, call_id, data):
        """Send a packet to downstream (OAP)"""
        if not self.up_out_sock:
            return False
        
        try:
            payload_size = len(data)
            header = struct.pack('!II', call_id, payload_size)
            self.up_out_sock.sendall(header + data)
            return True
        except Exception as e:
            print(f"❌ Failed to send packet: {e}")
            return False
    
    def initialize(self):
        """Initialize interconnect adapter"""
        if not self.scan_and_bind_ports():
            return False
        
        if not self.bind_traffic_ports():
            return False
        
        if not self.is_master:
            if not self.register_with_master():
                return False
        
        time.sleep(0.5)
        
        self.connect_to_downstream()
        
        self.accept_from_upstream()
        
        time.sleep(0.5)
        
        print(f"🔄 Loading Kokoro pipeline ({self.device})...")
        self.pipeline = KPipeline(lang_code='a', device=self.device)
        print(f"✅ Kokoro pipeline loaded")
        
        return True
    
    def synthesize(self, text, voice=None):
        """Synthesize text to audio"""
        v = voice or self.default_voice
        
        audio_chunks = []
        for result in self.pipeline(text, voice=v, speed=1.0):
            if result.audio is not None:
                audio_chunks.append(result.audio)
        
        if not audio_chunks:
            return None
        
        import numpy as np
        audio = np.concatenate(audio_chunks)
        
        if audio.dtype != np.float32:
            audio = audio.astype(np.float32)
        
        if len(audio.shape) > 1:
            audio = audio.flatten()
        
        return audio
    
    def run(self):
        """Main processing loop"""
        print(f"🎤 Kokoro Interconnect Adapter running...")
        
        while self.running:
            pkt = self.recv_packet()
            if not pkt:
                time.sleep(0.01)
                continue
            
            text = pkt['payload'].decode('utf-8', errors='ignore')
            call_id = pkt['call_id']
            
            print(f"📝 Synthesizing for call {call_id}: {text[:50]}...")
            
            audio = self.synthesize(text)
            if audio is not None:
                audio_bytes = audio.tobytes()
                self.send_packet(call_id, audio_bytes)
                print(f"🔊 Sent {len(audio_bytes)} bytes of audio for call {call_id}")


def main():
    adapter = InterconnectAdapter(voice="df_eva", device="mps")
    
    if not adapter.initialize():
        print("❌ Failed to initialize adapter")
        return 1
    
    try:
        adapter.run()
    except KeyboardInterrupt:
        print("\n🛑 Shutting down...")
    
    return 0


if __name__ == "__main__":
    sys.exit(main())
