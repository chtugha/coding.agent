#!/usr/bin/env python3
import socket
import struct
import threading
import time
import statistics
import uuid

HOST = '127.0.0.1'
PORT = 8090
TEXT = "Hello from concurrent client. This is a load test for the Piper TTS service."


def send_message(sock, message: str):
    data = message.encode('utf-8')
    sock.sendall(struct.pack('!I', len(data)))
    sock.sendall(data)


def client_request(call_id: str, results: dict):
    start = time.perf_counter()
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((HOST, PORT))
        send_message(s, call_id)
        send_message(s, TEXT)
        send_message(s, 'BYE')
        # Read response (optional)
        try:
            hdr = s.recv(4)
            if len(hdr) == 4:
                l = struct.unpack('!I', hdr)[0]
                if l > 0:
                    _ = s.recv(l)
        except Exception:
            pass
        s.close()
        ok = True
    except Exception:
        ok = False
    end = time.perf_counter()
    results[call_id] = (ok, end - start)


def run_load(concurrency: int = 4):
    threads = []
    results = {}
    for _ in range(concurrency):
        cid = f"load_{uuid.uuid4().hex[:8]}"
        t = threading.Thread(target=client_request, args=(cid, results))
        threads.append(t)
        t.start()
    for t in threads:
        t.join()
    durations = [d for ok, d in results.values() if ok]
    successes = sum(1 for ok, _ in results.values() if ok)
    return successes, durations


def p95(durs):
    if not durs:
        return 0.0
    s = sorted(durs)
    idx = max(0, int(0.95 * (len(s) - 1)))
    return s[idx]


if __name__ == '__main__':
    for conc in [1, 2, 4, 6]:
        t0 = time.perf_counter()
        succ, durs = run_load(conc)
        t1 = time.perf_counter()
        if durs:
            print(f"concurrency={conc} successes={succ}/{conc} total_time={t1-t0:.2f}s avg={statistics.mean(durs):.2f}s p95={p95(durs):.2f}s")
        else:
            print(f"concurrency={conc} successes={succ}/{conc} total_time={t1-t0:.2f}s")

