#!/usr/bin/env python3
import os
import socket
import struct
import subprocess
import threading
import time
import uuid
import statistics

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BIN = os.path.join(ROOT, 'bin', 'piper-service')
MODEL = os.path.join(ROOT, 'models', 'en_US-ryan-high.onnx')
ESPEAK = os.path.join(ROOT, 'libpiper', 'build', 'espeak_ng-install', 'share', 'espeak-ng-data')
DB = os.path.join(ROOT, 'whisper_talk.db')
HOST = '127.0.0.1'
PORT = 8090
OUT_HOST = '127.0.0.1'
OUT_PORT = 8091
DYLD = os.path.join(ROOT, 'libpiper', 'build') + ':' + os.path.join(ROOT, 'whisper-cpp', 'build', 'src') + ':' + os.path.join(ROOT, 'llama-cpp', 'build', 'bin')

CALLS = [
    ("load_test_001", "Hello one. The quick brown fox jumps over the lazy dog."),
    ("load_test_002", "Hello two. Concurrent synthesis should be safe and fast."),
    ("load_test_003", "Hello three. Each session must be isolated and resilient."),
    ("load_test_004", "Hello four. Audio output may be unavailable and must not block."),
]


def send_message(sock, message: str):
    data = message.encode('utf-8')
    sock.sendall(struct.pack('!I', len(data)))
    sock.sendall(data)


def client_request(call_id: str, text: str, results: dict):
    t0 = time.perf_counter()
    ok = False
    try:
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        s.connect((HOST, PORT))
        send_message(s, call_id)
        send_message(s, text)
        send_message(s, 'BYE')
        # Read optional response
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
    t1 = time.perf_counter()
    results[call_id] = (ok, t1 - t0)


def ps_rss_kb(pid: int) -> int:
    try:
        out = subprocess.check_output(["ps", "-o", "rss=", "-p", str(pid)], text=True)
        return int(out.strip())
    except Exception:
        return 0


def run_test():
    env = os.environ.copy()
    env['DYLD_LIBRARY_PATH'] = DYLD
    args = [
        BIN,
        '-m', MODEL,
        '-e', ESPEAK,
        '-d', DB,
        '-p', str(PORT),
        '--out-host', OUT_HOST,
        '--out-port', str(OUT_PORT),
        '--max-concurrency', '2',  # force queuing with 4 concurrent sessions
        '-v'
    ]

    print("Starting piper-service with --max-concurrency=2 for queue validation...")
    proc = subprocess.Popen(args, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)

    # Wait for listening line
    ready = False
    t_start = time.time()
    while time.time() - t_start < 15:
        line = proc.stdout.readline()
        if not line:
            time.sleep(0.1)
            continue
        # print(line.rstrip())  # optional debug
        if 'listening on TCP port' in line:
            ready = True
            break
    if not ready:
        proc.terminate()
        print("Service failed to start")
        return False

    # Run clients
    results = {}
    threads = []
    peak_rss = 0

    def sample_mem():
        nonlocal peak_rss
        while any(t.is_alive() for t in threads):
            rss = ps_rss_kb(proc.pid)
            peak_rss = max(peak_rss, rss)
            time.sleep(0.2)

    for call_id, text in CALLS:
        t = threading.Thread(target=client_request, args=(call_id, text, results))
        threads.append(t)

    sampler = threading.Thread(target=sample_mem)
    t0 = time.perf_counter()
    for t in threads: t.start()
    sampler.start()
    for t in threads: t.join()
    t1 = time.perf_counter()

    # Stop sampler
    sampler.join(timeout=0.1)

    # Stop service
    proc.terminate()
    try:
        proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()

    # Report
    successes = sum(1 for ok, _ in results.values() if ok)
    durs = {cid: dur for cid, (ok, dur) in results.items() if ok}
    avg = statistics.mean(durs.values()) if durs else 0.0

    print("Results:")
    for cid in sorted(durs.keys()):
        print(f"  {cid}: {durs[cid]:.2f}s")
    print(f"  total: {(t1 - t0):.2f}s  successes: {successes}/{len(CALLS)}  avg: {avg:.2f}s  peak_rss_kb: {peak_rss}")

    # Basic assertions for success criteria
    if successes != len(CALLS):
        print("FAIL: Not all sessions succeeded")
        return False
    if avg > 10.0:
        print("FAIL: Average latency too high")
        return False
    return True


if __name__ == '__main__':
    ok = run_test()
    print("OK" if ok else "NOT OK")

