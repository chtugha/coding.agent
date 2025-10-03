#!/usr/bin/env python3
"""
Kokoro TTS Service - Drop-in replacement for Piper
Uses PyTorch with Metal Performance Shaders for Apple Silicon optimization
Compatible with existing C++ TCP client code
"""

import torch
import socket
from socket import SOCK_DGRAM
import struct
import sys
import signal
import threading
import numpy as np
import time
from pathlib import Path

# Check if kokoro is installed
try:
    from kokoro import KPipeline
except ImportError:
    print("❌ Kokoro not installed. Install with: pip install torch kokoro")
    sys.exit(1)


class KokoroTCPService:
    def __init__(self, voice="af_sky", tcp_port=8090, udp_port=13001, device="mps"):
        """
        Initialize Kokoro TTS service

        Args:
            voice: Voice to use (af_sky, af_bella, af_sarah, am_adam, am_michael, bf_emma, bf_isabella, bm_george, bm_lewis)
            tcp_port: TCP port for LLaMA connections
            udp_port: UDP port for audio processor registrations
            device: PyTorch device (mps for Apple Silicon, cuda for NVIDIA, cpu for fallback)
        """
        self.voice = voice
        self.tcp_port = tcp_port
        self.udp_port = udp_port
        self.running = True
        self.registration_running = True

        # Track registered audio processors
        self.registered_calls = {}  # call_id -> timestamp
        self.registered_calls_lock = threading.Lock()

        # Performance tracking
        self.synthesis_times = []  # List of synthesis times for monitoring
        self.synthesis_lock = threading.Lock()
        self.total_syntheses = 0
        self.total_audio_samples = 0

        # Setup device
        if device == "mps" and torch.backends.mps.is_available():
            self.device_str = "mps"
            print(f"✅ Using Metal Performance Shaders (Apple Silicon)")
        elif device == "cuda" and torch.cuda.is_available():
            self.device_str = "cuda"
            print(f"✅ Using CUDA (NVIDIA GPU)")
        else:
            self.device_str = "cpu"
            print(f"⚠️  Using CPU (slower)")

        # Initialize Kokoro pipeline
        print(f"🔄 Loading Kokoro model...")
        self.pipeline = KPipeline(lang_code='en-us', device=self.device_str)
        print(f"✅ Kokoro model loaded")

        print(f"🎤 Kokoro TTS Service initialized")
        print(f"   Voice: {voice}")
        print(f"   Device: {self.device_str}")
        print(f"   TCP Port: {tcp_port}")
        print(f"   UDP Port: {udp_port}")
    
    def synthesize(self, text, speed=1.0):
        """
        Synthesize text to audio

        Args:
            text: Text to synthesize
            speed: Speech speed (1.0 = normal)

        Returns:
            tuple: (numpy array of float32 audio samples, sample_rate, synthesis_time)
        """
        start_time = time.time()

        try:
            # Collect all audio chunks from the pipeline
            audio_chunks = []
            for result in self.pipeline(text, voice=self.voice, speed=speed):
                if result.audio is not None:
                    audio_chunks.append(result.audio)

            if not audio_chunks:
                print(f"⚠️  No audio generated for text: {text[:50]}...")
                return None, 0, 0

            # Concatenate all chunks
            audio = np.concatenate(audio_chunks)

            # Ensure float32
            if audio.dtype != np.float32:
                audio = audio.astype(np.float32)

            # Ensure 1D array
            if len(audio.shape) > 1:
                audio = audio.flatten()

            # Kokoro generates at 24kHz
            sample_rate = 24000

            # Calculate synthesis time
            synthesis_time = time.time() - start_time

            # Track performance metrics
            with self.synthesis_lock:
                self.synthesis_times.append(synthesis_time)
                # Keep only last 100 synthesis times
                if len(self.synthesis_times) > 100:
                    self.synthesis_times.pop(0)
                self.total_syntheses += 1
                self.total_audio_samples += len(audio)

            return audio, sample_rate, synthesis_time
        except Exception as e:
            print(f"❌ Synthesis error: {e}")
            import traceback
            traceback.print_exc()
            return None, 0, 0
    
    def registration_listener_thread(self):
        """Listen for UDP registration messages from outbound audio processors"""
        try:
            # Create UDP socket
            udp_socket = socket.socket(socket.AF_INET, SOCK_DGRAM)
            udp_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            udp_socket.bind(('127.0.0.1', self.udp_port))
            udp_socket.settimeout(1.0)

            print(f"📡 UDP registration listener started on port {self.udp_port}")

            while self.registration_running:
                try:
                    data, addr = udp_socket.recvfrom(256)
                    message = data.decode('utf-8').strip()

                    if message.startswith("REGISTER:"):
                        call_id = message[9:]
                        with self.registered_calls_lock:
                            self.registered_calls[call_id] = threading.current_thread().ident
                        print(f"📥 Received REGISTER for call_id {call_id} - outbound processor is ready")

                    elif message.startswith("BYE:"):
                        call_id = message[4:]
                        with self.registered_calls_lock:
                            if call_id in self.registered_calls:
                                del self.registered_calls[call_id]
                        print(f"📤 Received BYE for call_id {call_id}")

                except socket.timeout:
                    continue
                except Exception as e:
                    if self.registration_running:
                        print(f"❌ Registration listener error: {e}")

        except Exception as e:
            print(f"❌ Failed to start UDP registration listener: {e}")
        finally:
            try:
                udp_socket.close()
            except:
                pass
            print(f"🛑 UDP registration listener stopped")

    def handle_client(self, client_socket, addr):
        """Handle a single client connection"""
        try:
            # Read HELLO message (call_id)
            length_bytes = client_socket.recv(4)
            if len(length_bytes) != 4:
                return
            
            length = struct.unpack('!I', length_bytes)[0]
            if length == 0 or length > 1024:
                return
            
            call_id_bytes = client_socket.recv(length)
            if len(call_id_bytes) != length:
                return
            
            call_id = call_id_bytes.decode('utf-8')
            print(f"👋 HELLO from LLaMA for call_id={call_id}")
            
            # Process text chunks
            chunk_id = 0
            while self.running:
                # Read text chunk length
                length_bytes = client_socket.recv(4)
                if len(length_bytes) != 4:
                    break
                
                length = struct.unpack('!I', length_bytes)[0]
                
                # Check for BYE message
                if length == 0xFFFFFFFF:
                    print(f"📤 BYE received for call_id={call_id}")
                    break
                
                if length == 0 or length > 10 * 1024 * 1024:
                    break
                
                # Read text
                text_bytes = client_socket.recv(length)
                if len(text_bytes) != length:
                    break
                
                text = text_bytes.decode('utf-8')
                print(f"📝 Synthesizing for call {call_id}: {text[:50]}...")

                # Synthesize audio
                audio, sample_rate, synthesis_time = self.synthesize(text)

                if audio is not None and len(audio) > 0:
                    chunk_id += 1

                    # Convert to bytes
                    audio_bytes = audio.tobytes()

                    # Send audio chunk: [length][sample_rate][chunk_id][audio_data]
                    header = struct.pack('!III', len(audio_bytes), sample_rate, chunk_id)

                    try:
                        client_socket.sendall(header)
                        client_socket.sendall(audio_bytes)

                        # Calculate audio duration and real-time factor
                        audio_duration = len(audio) / sample_rate
                        rtf = synthesis_time / audio_duration if audio_duration > 0 else 0

                        print(f"🔊 Sent chunk#{chunk_id} ({len(audio)} samples @{sample_rate}Hz) for call {call_id}")
                        print(f"   ⚡ Synthesis: {synthesis_time:.3f}s | Audio: {audio_duration:.3f}s | RTF: {rtf:.3f}x")
                    except Exception as e:
                        print(f"❌ Failed to send audio: {e}")
                        break
                else:
                    print(f"⚠️  Synthesis failed for call {call_id}")
            
        except Exception as e:
            print(f"❌ Client handler error: {e}")
        finally:
            try:
                client_socket.close()
            except:
                pass
            print(f"🔌 Client disconnected for call {call_id if 'call_id' in locals() else 'unknown'}")
    
    def run(self):
        """Run the TCP server and UDP registration listener"""
        # Start UDP registration listener in a separate thread
        registration_thread = threading.Thread(
            target=self.registration_listener_thread,
            daemon=True
        )
        registration_thread.start()

        # Create TCP socket
        server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)

        try:
            server_socket.bind(('127.0.0.1', self.tcp_port))
            server_socket.listen(16)
            server_socket.settimeout(1.0)  # Allow checking self.running periodically

            print(f"🚀 Kokoro service listening on TCP port {self.tcp_port}")
            print(f"💡 Press Ctrl+C to shutdown gracefully")

            while self.running:
                try:
                    client_socket, addr = server_socket.accept()

                    # Handle client in a separate thread
                    client_thread = threading.Thread(
                        target=self.handle_client,
                        args=(client_socket, addr),
                        daemon=True
                    )
                    client_thread.start()

                except socket.timeout:
                    continue
                except Exception as e:
                    if self.running:
                        print(f"❌ Accept error: {e}")

        except Exception as e:
            print(f"❌ Server error: {e}")
        finally:
            server_socket.close()
            self.registration_running = False
            print(f"🛑 Kokoro service stopped")
    
    def stop(self):
        """Stop the service"""
        self.running = False
        self.registration_running = False

    def get_performance_stats(self):
        """Get performance statistics"""
        with self.synthesis_lock:
            if not self.synthesis_times:
                return {
                    'total_syntheses': 0,
                    'avg_synthesis_time': 0,
                    'min_synthesis_time': 0,
                    'max_synthesis_time': 0,
                    'total_audio_samples': 0
                }

            return {
                'total_syntheses': self.total_syntheses,
                'avg_synthesis_time': sum(self.synthesis_times) / len(self.synthesis_times),
                'min_synthesis_time': min(self.synthesis_times),
                'max_synthesis_time': max(self.synthesis_times),
                'total_audio_samples': self.total_audio_samples
            }


def signal_handler(signum, frame):
    """Handle shutdown signals"""
    print(f"\n🛑 Received signal {signum}, shutting down...")
    if 'service' in globals():
        service.stop()
    sys.exit(0)


def main():
    import argparse

    parser = argparse.ArgumentParser(description='Kokoro TTS Service')
    parser.add_argument('-v', '--voice', default='af_sky',
                       help='Voice to use (default: af_sky)')
    parser.add_argument('-t', '--tcp-port', type=int, default=8090,
                       help='TCP port for LLaMA connections (default: 8090)')
    parser.add_argument('-u', '--udp-port', type=int, default=13001,
                       help='UDP port for audio processor registrations (default: 13001)')
    parser.add_argument('-d', '--device', default='mps',
                       choices=['mps', 'cuda', 'cpu'],
                       help='PyTorch device (default: mps for Apple Silicon)')

    args = parser.parse_args()

    # Setup signal handlers
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    # Create and run service
    global service
    service = KokoroTCPService(
        voice=args.voice,
        tcp_port=args.tcp_port,
        udp_port=args.udp_port,
        device=args.device
    )
    service.run()


if __name__ == "__main__":
    main()

