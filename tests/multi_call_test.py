import socket
import struct
import time
import random

def create_rtp_packet(payload_type, sequence, timestamp, ssrc, payload):
    # RTP Header (12 bytes)
    # V=2, P=0, X=0, CC=0 (0x80)
    # M=0, PT=payload_type
    header = struct.pack('!BBHII', 
                         0x80, 
                         payload_type & 0x7F, 
                         sequence, 
                         timestamp, 
                         ssrc)
    return header + payload

def send_call_audio(sock, addr, call_id, duration_sec=5):
    print(f"📞 Starting call simulation for ID: {call_id}")
    ssrc = random.randint(0, 0xFFFFFFFF)
    sequence = random.randint(0, 0xFFFF)
    timestamp = random.randint(0, 0xFFFFFFFF)
    
    # G.711 PCMU payload (all zeros for silence, though PCMU silence isn't 0, but it works for testing)
    payload = b'\xff' * 160 # 20ms of "silence" in PCMU
    
    start_time = time.time()
    while time.time() - start_time < duration_sec:
        # Prefix with 4-byte call_id
        prefix = struct.pack('!I', int(call_id))
        rtp = create_rtp_packet(0, sequence, timestamp, ssrc, payload)
        
        sock.sendto(prefix + rtp, addr)
        
        sequence = (sequence + 1) & 0xFFFF
        timestamp = (timestamp + 160) & 0xFFFFFFFF
        
        time.sleep(0.02) # 20ms intervals

def main():
    target_addr = ('127.0.0.1', 9001)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    
    print("🚀 Starting multi-call simulation...")
    
    # We'll simulate 3 concurrent calls
    # In a real script we would use threads, but let's just send packets in a loop for multiple IDs
    
    calls = [1001, 1002, 1003]
    ssrcs = {cid: random.randint(0, 0xFFFFFFFF) for cid in calls}
    sequences = {cid: random.randint(0, 0xFFFF) for cid in calls}
    timestamps = {cid: random.randint(0, 0xFFFFFFFF) for cid in calls}
    
    payload = b'\xff' * 160
    
    start_time = time.time()
    duration = 10
    
    print(f"📡 Sending interleaved audio for calls {calls} to {target_addr}")
    
    while time.time() - start_time < duration:
        for cid in calls:
            prefix = struct.pack('!I', cid)
            rtp = create_rtp_packet(0, sequences[cid], timestamps[cid], ssrcs[cid], payload)
            sock.sendto(prefix + rtp, target_addr)
            
            sequences[cid] = (sequences[cid] + 1) & 0xFFFF
            timestamps[cid] = (timestamps[cid] + 160) & 0xFFFFFFFF
            
        time.sleep(0.02)
        
    print("✅ Multi-call simulation finished.")

if __name__ == "__main__":
    main()
