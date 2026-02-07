import socket
import struct
import time
import math

# Target: InboundAudioProcessor UDP port
UDP_IP = "127.0.0.1"
UDP_PORT = 9001

def create_rtp_packet(sequence, timestamp, payload_type, payload):
    # RTP Header (12 bytes)
    # V=2, P=0, X=0, CC=0 -> 0x80
    # M=0, PT -> payload_type
    header = struct.pack('!BBHII', 0x80, payload_type, sequence, timestamp, 0x12345678) # SSRC=0x12345678
    return header + payload

def encode_ulaw(sample):
    # Simple G.711 u-law encoder for sine wave
    # Just a very basic approximation for testing
    if sample > 0:
        return 0x80 | (0x7F - int(sample * 127))
    else:
        return (0x7F - int(abs(sample) * 127))

def send_audio_for_call(call_id, duration_sec=2.0):
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    print(f"📞 Starting simulated call {call_id}...")
    
    # 8kHz audio, 20ms packets (160 samples)
    samples_per_packet = 160
    packet_interval = 0.02
    total_packets = int(duration_sec / packet_interval)
    
    timestamp = 0
    sequence = 0
    
    for i in range(total_packets):
        # 1s signal, then 1s silence, then 1s signal...
        is_speech = (i // 50) % 2 == 0 # 50 * 20ms = 1s
        
        freq = 440 if call_id == 101 else 880
        audio_payload = bytearray()
        for j in range(samples_per_packet):
            t = (i * samples_per_packet + j) / 8000.0
            sample = math.sin(2 * math.pi * freq * t)
            if is_speech:
                # Use a value that decoded will be > vad_threshold
                # u-law 0x00 is max positive, 0x80 is max negative. 
                # Let's use 0x00 and 0x80 alternating
                audio_payload.append(0x00 if j % 2 == 0 else 0x80)
            else:
                audio_payload.append(0xFF) # Silence
        
        # InboundAudioProcessor expects: [4 bytes call_id] + [RTP packet]
        call_id_header = struct.pack('!I', call_id)
        rtp_pkt = create_rtp_packet(sequence, timestamp, 0, audio_payload) # PT=0 (u-law)
        
        sock.sendto(call_id_header + rtp_pkt, (UDP_IP, UDP_PORT))
        
        sequence = (sequence + 1) & 0xFFFF
        timestamp += samples_per_packet
        time.sleep(packet_interval)

    print(f"✅ Finished simulated call {call_id}")

if __name__ == "__main__":
    import threading
    
    # Run two calls concurrently
    t1 = threading.Thread(target=send_audio_for_call, args=(101, 5.0))
    t2 = threading.Thread(target=send_audio_for_call, args=(102, 5.0))
    
    t1.start()
    t2.start()
    
    t1.join()
    t2.join()
