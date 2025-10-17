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

        # Track outbound audio processor connections
        self.outbound_sockets = {}  # call_id -> socket
        self.outbound_sockets_lock = threading.Lock()
        self.chunk_counters = {}  # call_id -> chunk_id

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

    def calculate_outbound_port(self, call_id):
        """Calculate outbound audio processor port: 9002 + call_id"""
        try:
            call_id_num = int(call_id)
            return 9002 + call_id_num
        except ValueError:
            return 9002  # Fallback

    def connect_to_outbound_processor(self, call_id):
        """Connect to outbound audio processor on port 9002+call_id"""
        # Check if already connected
        with self.outbound_sockets_lock:
            if call_id in self.outbound_sockets:
                return True

        port = self.calculate_outbound_port(call_id)
        max_attempts = 10

        for attempt in range(1, max_attempts + 1):
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)  # Disable Nagle
                s.connect(('127.0.0.1', port))

                # Send HELLO(call_id)
                call_id_bytes = call_id.encode('utf-8')
                length = struct.pack('!I', len(call_id_bytes))
                s.sendall(length)
                s.sendall(call_id_bytes)

                with self.outbound_sockets_lock:
                    self.outbound_sockets[call_id] = s
                    self.chunk_counters[call_id] = 0

                print(f"🔗 Connected to outbound processor on port {port} for call_id {call_id}")
                return True

            except Exception as e:
                if s:
                    try:
                        s.close()
                    except:
                        pass

                sleep_ms = 200 if attempt <= 5 else 1000
                if attempt == 1 or attempt == 5 or attempt == 9:
                    print(f"⚠️  Outbound connect attempt {attempt}/{max_attempts} failed — retrying in {sleep_ms}ms")
                time.sleep(sleep_ms / 1000.0)

        print(f"❌ Failed to connect to outbound processor on port {port} after {max_attempts} attempts")
        return False

    def send_audio_to_outbound(self, call_id, audio, sample_rate):
        """Send audio chunk to outbound audio processor"""
        with self.outbound_sockets_lock:
            if call_id not in self.outbound_sockets:
                return False

            s = self.outbound_sockets[call_id]
            self.chunk_counters[call_id] += 1
            chunk_id = self.chunk_counters[call_id]

        try:
            # Convert to bytes
            audio_bytes = audio.tobytes()

            # Send audio chunk: [length][sample_rate][chunk_id][audio_data]
            header = struct.pack('!III', len(audio_bytes), sample_rate, chunk_id)
            s.sendall(header)
            s.sendall(audio_bytes)

            return True

        except Exception as e:
            print(f"❌ Failed to send audio to outbound processor: {e}")
            # Close and remove socket
            with self.outbound_sockets_lock:
                if call_id in self.outbound_sockets:
                    try:
                        self.outbound_sockets[call_id].close()
                    except:
                        pass
                    del self.outbound_sockets[call_id]
            return False

    def close_outbound_connection(self, call_id):
        """Close connection to outbound audio processor"""
        with self.outbound_sockets_lock:
            if call_id in self.outbound_sockets:
                try:
                    # Send BYE (length=0)
                    bye = struct.pack('!III', 0, 0, 0)
                    self.outbound_sockets[call_id].sendall(bye)
                    self.outbound_sockets[call_id].close()
                except:
                    pass
                del self.outbound_sockets[call_id]
                print(f"🔌 Closed outbound connection for call_id {call_id}")

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

                        # Connect to outbound processor
                        threading.Thread(target=self.connect_to_outbound_processor, args=(call_id,), daemon=True).start()

                    elif message.startswith("BYE:"):
                        call_id = message[4:]
                        with self.registered_calls_lock:
                            if call_id in self.registered_calls:
                                del self.registered_calls[call_id]
                        print(f"📤 Received BYE for call_id {call_id}")

                        # Close outbound connection
                        self.close_outbound_connection(call_id)

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
                    # Calculate audio duration and real-time factor
                    audio_duration = len(audio) / sample_rate
                    rtf = synthesis_time / audio_duration if audio_duration > 0 else 0

                    print(f"🎵 Synthesized {len(audio)} samples @{sample_rate}Hz for call {call_id}")
                    print(f"   ⚡ Synthesis: {synthesis_time:.3f}s | Audio: {audio_duration:.3f}s | RTF: {rtf:.3f}x")

                    # Send audio to outbound processor (not back to llama)
                    if self.send_audio_to_outbound(call_id, audio, sample_rate):
                        print(f"🔊 Sent audio to outbound processor for call {call_id}")
                    else:
                        print(f"⚠️  Failed to send audio to outbound processor for call {call_id}")
                        # Try to reconnect
                        if self.connect_to_outbound_processor(call_id):
                            # Retry sending
                            if self.send_audio_to_outbound(call_id, audio, sample_rate):
                                print(f"🔊 Sent audio to outbound processor (retry) for call {call_id}")
                else:
                    print(f"⚠️  Synthesis failed for call {call_id}")

            # Send BYE to outbound processor when done
            self.close_outbound_connection(call_id)
            
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

