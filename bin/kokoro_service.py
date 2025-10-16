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

        # Warmup: synthesize multiple short phrases to fully compile GPU kernels
        print(f"🔥 Warming up Kokoro pipeline...")
        warmup_phrases = ["Hi.", "Hello there.", "Yes, I can help you."]
        for phrase in warmup_phrases:
            warmup_start = time.time()
            try:
                for result in self.pipeline(phrase, voice=self.voice, speed=1.0):
                    if result.audio is not None:
                        pass  # discard warmup audio
                warmup_time = (time.time() - warmup_start) * 1000
                print(f"✅ Warmup '{phrase}': {warmup_time:.0f}ms")
            except Exception as e:
                print(f"⚠️  Warmup failed (non-fatal): {e}")
                break

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
    
    # Per-call: Listen on TCP 9002+call for outbound processor and actively send UDP REGISTER to 13000+call
    def _accept_outbound_after_register(self, call_id, max_first_sec_attempts=5):
        call_id_num = int(call_id)
        listen_port = 9002 + call_id_num
        udp_target_port = 13000 + call_id_num

        # Create TCP server for outbound to connect
        server_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        server_socket.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server_socket.bind(('127.0.0.1', listen_port))
        server_socket.listen(1)
        server_socket.settimeout(0.2)
        print(f"✅ Piper TCP socket listening on port {listen_port} for call {call_id}")

        # UDP socket for REGISTER sends
        udp_sock = socket.socket(socket.AF_INET, SOCK_DGRAM)
        target = ('127.0.0.1', udp_target_port)

        attempt = 0
        start_time = time.time()
        client_socket = None
        try:
            while self.running and client_socket is None:
                # Send REGISTER periodically (200ms in first second, then 1s)
                now = time.time()
                elapsed = now - start_time
                sleep_time = 0.2 if elapsed < 1.0 else 1.0
                attempt += 1
                msg = f"REGISTER:{call_id}".encode('utf-8')
                udp_sock.sendto(msg, target)
                if attempt in (1, 5):
                    print(f"📤 Sent REGISTER #{attempt} for call_id {call_id} to UDP {udp_target_port}")

                # Try to accept inbound connection from outbound processor
                try:
                    client_socket, addr = server_socket.accept()
                except socket.timeout:
                    time.sleep(sleep_time)
                    continue

            if client_socket is None:
                return None

            # Disable Nagle to reduce latency
            try:
                client_socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except Exception:
                pass

            # Expect HELLO(call_id) from outbound
            length_bytes = self._recv_exact(client_socket, 4)
            if not length_bytes:
                client_socket.close()
                return None
            length = struct.unpack('!I', length_bytes)[0]
            if length == 0 or length > 1024:
                client_socket.close()
                return None
            cid = self._recv_exact(client_socket, length)
            if not cid or cid.decode('utf-8') != call_id:
                client_socket.close()
                return None

            print(f"📡 TCP HELLO received from Piper for call: {call_id}")
            return client_socket
        finally:
            try:
                server_socket.close()
            except:
                pass
            try:
                udp_sock.close()
            except:
                pass

    def connect_to_audio_processor(self, call_id, max_attempts=10):
        """Connect to the outbound audio processor for this call with retry logic"""
        call_id_num = int(call_id)
        port = 9002 + call_id_num

        for attempt in range(1, max_attempts + 1):
            # Check if the outbound processor has registered
            with self.registered_calls_lock:
                is_registered = call_id in self.registered_calls

            if not is_registered:
                if attempt < max_attempts:
                    sleep_time = 0.2 if attempt <= 5 else 1.0
                    if attempt == 1 or attempt == 5 or attempt == max_attempts - 1:
                        print(f"⚠️ Connection attempt {attempt}/{max_attempts} failed for call {call_id}: outbound processor not registered - retrying in {sleep_time}s")
                    time.sleep(sleep_time)
                    continue
                else:
                    print(f"❌ Failed to connect to audio processor for call {call_id} after {max_attempts} attempts: not registered")
                    return None

            try:
                # Create socket
                audio_socket = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
                audio_socket.connect(('127.0.0.1', port))
                # Disable Nagle for low-latency streaming
                try:
                    audio_socket.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                except Exception:
                    pass

                # Send HELLO message
                hello_data = call_id.encode('utf-8')
                header = struct.pack('!I', len(hello_data))
                audio_socket.sendall(header)
                audio_socket.sendall(hello_data)

                print(f"🔗 Connected to outbound audio processor on port {port} for call {call_id} (attempt {attempt})")
                return audio_socket
            except Exception as e:
                if attempt < max_attempts:
                    # 200ms for first 5 attempts (first second), then 1s
                    sleep_time = 0.2 if attempt <= 5 else 1.0
                    if attempt == 1 or attempt == 5 or attempt == max_attempts - 1:
                        print(f"⚠️ Connection attempt {attempt}/{max_attempts} failed for call {call_id}: {e} - retrying in {sleep_time}s")
                    time.sleep(sleep_time)
                else:
                    print(f"❌ Failed to connect to audio processor for call {call_id} after {max_attempts} attempts: {e}")
                    return None

    def _recv_exact(self, sock, n):
        data = b''
        while len(data) < n:
            chunk = sock.recv(n - len(data))
            if not chunk:
                return None
            data += chunk
        return data

    def handle_client(self, client_socket, addr):
        """Handle a single client connection from LLaMA"""
        audio_socket = None
        call_id = None
        try:
            # Read HELLO message (call_id)
            length_bytes = self._recv_exact(client_socket, 4)
            if not length_bytes:
                return

            length = struct.unpack('!I', length_bytes)[0]
            if length == 0 or length > 1024:
                return

            call_id_bytes = self._recv_exact(client_socket, length)
            if not call_id_bytes:
                return

            call_id = call_id_bytes.decode('utf-8')
            print(f"👋 HELLO from LLaMA for call_id={call_id}")

            # Listen for outbound audio processor and announce via UDP REGISTER
            audio_socket = self._accept_outbound_after_register(call_id)
            if not audio_socket:
                print(f"❌ Cannot send audio for call {call_id} - outbound processor did not connect")
                return

            # Process text chunks
            chunk_id = 0
            while self.running:
                # Read text chunk length
                length_bytes = self._recv_exact(client_socket, 4)
                if not length_bytes:
                    break

                length = struct.unpack('!I', length_bytes)[0]

                # Check for BYE message
                if length == 0xFFFFFFFF:
                    print(f"📤 BYE received for call_id={call_id}")
                    break

                if length == 0 or length > 10 * 1024 * 1024:
                    break

                # Read text
                text_bytes = self._recv_exact(client_socket, length)
                if not text_bytes:
                    break

                text = text_bytes.decode('utf-8')
                print(f"📝 Synthesizing for call {call_id}: {text[:50]}...")

                # Stream audio as it is generated to minimize latency
                start_time = time.time()
                pipeline_start = None
                first_audio_time = None
                sample_rate = 24000
                sent_samples_total = 0
                subchunk_ms = 40  # reduced to 40ms for ultra-low latency
                subchunk_size = int(sample_rate * (subchunk_ms / 1000.0))

                try:
                    pipeline_start = time.time()
                    for result in self.pipeline(text, voice=self.voice, speed=1.0):
                        if first_audio_time is None and result.audio is not None:
                            first_audio_time = time.time()
                            print(f"⏱️  Kokoro pipeline first audio: {(first_audio_time - pipeline_start)*1000:.1f}ms")
                        if result.audio is None:
                            continue
                        # Ensure float32 1D
                        audio = result.audio
                        # Convert torch.Tensor -> numpy
                        if isinstance(audio, torch.Tensor):
                            audio = audio.detach().cpu().numpy()
                        if audio.dtype != np.float32:
                            audio = audio.astype(np.float32)
                        if len(audio.shape) > 1:
                            audio = audio.reshape(-1)
                        # Slice into smaller frames for faster playout
                        first_sent = False
                        for i in range(0, len(audio), subchunk_size):
                            frame = audio[i:i+subchunk_size]
                            if frame.size == 0:
                                continue
                            chunk_id += 1
                            audio_bytes = frame.tobytes()
                            header = struct.pack('!III', len(audio_bytes), sample_rate, chunk_id)
                            audio_socket.sendall(header)
                            audio_socket.sendall(audio_bytes)

                            if not first_sent:
                                # t2: Kokoro first subchunk sent (timestamp in seconds)
                                print(f"t2: Kokoro first subchunk sent [call {call_id}] ts={time.time():.6f}")
                                first_sent = True

                            sent_samples_total += frame.size
                            synth_elapsed = time.time() - start_time
                            audio_duration = sent_samples_total / sample_rate
                            rtf = synth_elapsed / audio_duration if audio_duration > 0 else 0.0
                except Exception as e:
                    print(f"❌ Streaming synthesis failed: {e}")
                    break


        except Exception as e:
            print(f"❌ Client handler error: {e}")
        finally:
            # Close audio socket
            if audio_socket:
                try:
                    # Send BYE to audio processor
                    bye_marker = struct.pack('!I', 0)
                    audio_socket.sendall(bye_marker)
                    audio_socket.close()
                except:
                    pass
            # Close LLaMA socket
            try:
                client_socket.close()
            except:
                pass
            print(f"🔌 Client disconnected for call {call_id if call_id else 'unknown'}")
    
    def run(self):
        """Run the Kokoro TCP server (LLaMA->Kokoro). Outbound will connect per-call to 9002+call."""

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

