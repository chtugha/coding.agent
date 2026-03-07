#!/usr/bin/env python3
"""Stage 7 Diagnostic Collection Script.

Automates N test runs that each:
  1. Start all services (if not already running)
  2. Wait for warmup
  3. Connect 1 testline to test_sip_provider
  4. Enable WAV recording in test_sip_provider and OAP
  5. Inject a sample file into the testline
  6. Wait for pipeline completion
  7. Hang up (triggers WAV file write)
  8. Collect logs (after hangup so CALL_END events are captured)
  9. Wait for OAP WAV write, then inventory WAV files in run directory

Usage:
    python3 tests/run_stage7.py [--output-dir stage7_output] [--iterations 10]
                                [--testfiles-dir Testfiles] [--no-start-services]
                                [--warmup 10]
"""
import argparse
import glob
import json
import os
import signal
import sys
import time
import urllib.request
import urllib.error

FRONTEND = "http://127.0.0.1:8080"
SIP_PROVIDER = "http://127.0.0.1:22011"

PIPELINE_SERVICES = [
    "SIP_CLIENT",
    "INBOUND_AUDIO_PROCESSOR",
    "VAD_SERVICE",
    "WHISPER_SERVICE",
    "LLAMA_SERVICE",
    "KOKORO_SERVICE",
    "OUTBOUND_AUDIO_PROCESSOR",
]
TSP_SERVICE = "TEST_SIP_PROVIDER"

LOG_PAGE_LIMIT = 1000


def fetch_json(url, data=None):
    req = urllib.request.Request(url)
    if data is not None:
        req.add_header("Content-Type", "application/json")
        req.data = json.dumps(data).encode()
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        try:
            return json.loads(e.read())
        except Exception:
            return {"error": f"HTTP {e.code}: {e.reason}"}
    except Exception as e:
        return {"error": str(e)}


def post_json(url, data):
    return fetch_json(url, data=data)


def get_services():
    return fetch_json(f"{FRONTEND}/api/services")


def start_service(name):
    return post_json(f"{FRONTEND}/api/services/start", {"service": name})


def get_all_logs():
    """Fetch all available logs via pagination (backend caps each page at 1000)."""
    all_logs = []
    offset = 0
    while True:
        data = fetch_json(f"{FRONTEND}/api/logs?limit={LOG_PAGE_LIMIT}&offset={offset}")
        if "error" in data:
            print(f"    WARNING: log fetch error at offset {offset}: {data['error']}",
                  file=sys.stderr)
            break
        page = data.get("logs", [])
        if not page:
            break
        all_logs.extend(page)
        if len(page) < LOG_PAGE_LIMIT:
            break
        offset += LOG_PAGE_LIMIT
    return all_logs


def start_all_services():
    print("  Starting all pipeline services...")
    svcs = get_services()
    running = {s["name"] for s in svcs.get("services", []) if s.get("running")}

    for name in PIPELINE_SERVICES + [TSP_SERVICE]:
        if name in running:
            print(f"    {name}: already running")
        else:
            r = start_service(name)
            if "error" in r:
                print(f"    {name}: FAILED to start — {r['error']}", file=sys.stderr)
            else:
                print(f"    {name}: started")


def wait_for_warmup(seconds=10):
    print(f"  Waiting {seconds}s for services to warm up...")
    time.sleep(seconds)


def get_registered_users():
    r = fetch_json(f"{SIP_PROVIDER}/users")
    if "error" in r:
        return []
    return [u["username"] for u in r.get("users", [])]


def connect_testline(username):
    print(f"  Connecting testline for user '{username}'...")
    calls = fetch_json(f"{SIP_PROVIDER}/calls")
    if calls.get("calls"):
        call = calls["calls"][0]
        legs = call.get("legs", [])
        if isinstance(legs, list) and legs:
            print(f"    Call already active with {len(legs)} leg(s) — reusing")
            return legs[0].get("user", username)
        print(f"    Call already active — reusing")
        return username

    r = post_json(f"{SIP_PROVIDER}/conference", {"users": [username]})
    if "error" in r:
        raise RuntimeError(f"Failed to start conference: {r['error']}")
    print(f"    Conference started: {r}")
    time.sleep(2)
    return username


def wait_for_registration(timeout=60):
    print(f"  Waiting for a SIP user to register (max {timeout}s)...")
    deadline = time.time() + timeout
    while time.time() < deadline:
        users = get_registered_users()
        if users:
            print(f"    Registered: {users}")
            return users[0]
        time.sleep(2)
    raise RuntimeError("No SIP user registered with test_sip_provider within timeout")


def enable_wav_recording(output_dir):
    print(f"  Enabling WAV recording -> {output_dir}")
    r = post_json(f"{SIP_PROVIDER}/wav_recording", {"enabled": True, "dir": output_dir})
    if "error" in r:
        print(f"    WARNING: test_sip_provider wav_recording: {r['error']}", file=sys.stderr)
    else:
        print(f"    test_sip_provider: {r}")

    r = post_json(f"{FRONTEND}/api/oap/wav_recording", {"enabled": True, "dir": output_dir})
    if "error" in r:
        print(f"    WARNING: OAP wav_recording: {r['error']}", file=sys.stderr)
    else:
        print(f"    OAP: {r}")


def disable_wav_recording():
    post_json(f"{SIP_PROVIDER}/wav_recording", {"enabled": False, "dir": ""})
    post_json(f"{FRONTEND}/api/oap/wav_recording", {"enabled": False, "dir": ""})


def inject_sample(sample_file, leg_username):
    print(f"  Injecting '{sample_file}' into leg '{leg_username}'...")
    r = post_json(f"{SIP_PROVIDER}/inject", {
        "file": sample_file,
        "leg": leg_username,
        "no_silence": True,
    })
    if "error" in r:
        raise RuntimeError(f"Injection failed: {r['error']}")
    print(f"    Inject: {r}")
    return r


def wait_for_injection_complete(timeout=120):
    print("  Waiting for injection to complete...")
    deadline = time.time() + timeout
    while time.time() < deadline:
        status = fetch_json(f"{SIP_PROVIDER}/status")
        if "error" in status:
            time.sleep(1)
            continue
        if status.get("injecting") is None:
            print("    Injection complete")
            return
        time.sleep(1)
    print("  WARNING: injection did not complete within timeout", file=sys.stderr)


def wait_for_pipeline(extra_buffer=5):
    print(f"  Waiting {extra_buffer}s for Kokoro+OAP to finish...")
    time.sleep(extra_buffer)


def hangup():
    print("  Hanging up call...")
    r = post_json(f"{SIP_PROVIDER}/hangup", {})
    print(f"    Hangup: {r}")


def wait_for_wav_files(wav_dir, timeout=10):
    """Poll until OAP writes both _input.wav and _output.wav (written on CALL_END)."""
    print(f"  Waiting for WAV files to be written (max {timeout}s)...")
    deadline = time.time() + timeout
    while time.time() < deadline:
        input_wavs = glob.glob(os.path.join(wav_dir, "oap_call_*_input.wav"))
        output_wavs = glob.glob(os.path.join(wav_dir, "oap_call_*_output.wav"))
        if input_wavs and output_wavs:
            print(f"    Found {len(input_wavs)} OAP input WAV(s), {len(output_wavs)} OAP output WAV(s)")
            return
        time.sleep(1)
    present = (
        glob.glob(os.path.join(wav_dir, "oap_call_*_input.wav")) +
        glob.glob(os.path.join(wav_dir, "oap_call_*_output.wav"))
    )
    if present:
        print(f"  WARNING: only partial OAP WAV set found ({[os.path.basename(p) for p in present]})"
              " — CALL_END may not have fully propagated", file=sys.stderr)
    else:
        print("  WARNING: no OAP WAV files found after timeout — CALL_END may not have propagated",
              file=sys.stderr)


def collect_logs(run_dir, logs):
    """Write logs (oldest-first) and extract LLaMA responses."""
    os.makedirs(run_dir, exist_ok=True)

    log_file = os.path.join(run_dir, "pipeline.log")
    with open(log_file, "w", encoding="utf-8") as f:
        for entry in reversed(logs):
            ts = entry.get("timestamp", "")
            svc = entry.get("service", "")
            lvl = entry.get("level", "")
            msg = entry.get("message", "")
            f.write(f"[{ts}] [{svc}] [{lvl}] {msg}\n")
    print(f"    {len(logs)} log entries -> {log_file}")

    llama_file = os.path.join(run_dir, "llama_response.txt")
    llama_lines = [
        entry.get("message", "")
        for entry in reversed(logs)
        if entry.get("service", "").upper() in ("LLAMA_SERVICE", "LLAMA")
    ]
    if llama_lines:
        with open(llama_file, "w", encoding="utf-8") as f:
            f.write("\n".join(llama_lines) + "\n")
        print(f"    {len(llama_lines)} LLaMA entries -> {llama_file}")
    else:
        print("    No LLaMA log entries found")

    return log_file


def list_wav_files(wav_dir):
    """Return paths of all tsp/oap WAV files already present in wav_dir.

    OAP produces two files per call:
      oap_call_<id>_<ts>_input.wav  — 24kHz raw Kokoro output (pre-OAP)
      oap_call_<id>_<ts>_output.wav — 8kHz post-downsample signal to SIP client
    TSP produces one file per leg:
      tsp_call_<id>_<user>_<ts>.wav — 8kHz audio received back at the test provider
    """
    found = []
    for pattern in ["tsp_call_*.wav",
                    "oap_call_*_input.wav",
                    "oap_call_*_output.wav"]:
        found.extend(sorted(glob.glob(os.path.join(wav_dir, pattern))))
    for fpath in found:
        print(f"    Found: {os.path.basename(fpath)}")
    if not found:
        print("    WARNING: no WAV files found", file=sys.stderr)
    return found


def get_sample_file(testfiles_dir, iteration):
    samples = sorted(glob.glob(os.path.join(testfiles_dir, "sample_*.wav")))
    if not samples:
        raise RuntimeError(f"No sample_*.wav files found in {testfiles_dir}")
    return os.path.basename(samples[(iteration - 1) % len(samples)])


def main():
    parser = argparse.ArgumentParser(description="Stage 7 diagnostic collection")
    parser.add_argument("--output-dir", default="stage7_output", help="Base output directory")
    parser.add_argument("--iterations", type=int, default=10, help="Number of test runs")
    parser.add_argument("--testfiles-dir", default="Testfiles", help="Directory with sample WAV files")
    parser.add_argument("--no-start-services", action="store_true",
                        help="Skip starting services (assume already running)")
    parser.add_argument("--warmup", type=int, default=10, help="Warmup seconds (default 10)")
    args = parser.parse_args()

    output_dir = os.path.abspath(args.output_dir)
    testfiles_dir = os.path.abspath(args.testfiles_dir)

    print("=== Stage 7 Diagnostic Collection ===")
    print(f"Output directory : {output_dir}")
    print(f"Iterations       : {args.iterations}")
    print(f"Test files       : {testfiles_dir}")
    print()

    active_call = [False]

    def cleanup(signum=None, frame=None):
        print("\n[Cleanup] Disabling WAV recording and hanging up...", file=sys.stderr)
        disable_wav_recording()
        if active_call[0]:
            hangup()
        sys.exit(1)

    signal.signal(signal.SIGINT, cleanup)
    signal.signal(signal.SIGTERM, cleanup)

    if not args.no_start_services:
        print("[Setup] Starting services...")
        start_all_services()
        wait_for_warmup(args.warmup)
    else:
        print("[Setup] Skipping service start (--no-start-services)")

    print("[Setup] Waiting for SIP user registration...")
    try:
        leg_user = wait_for_registration(timeout=60)
    except RuntimeError as e:
        print(f"FATAL: {e}", file=sys.stderr)
        sys.exit(1)

    collected = []

    for run_n in range(1, args.iterations + 1):
        print(f"\n{'='*60}")
        print(f"Run {run_n}/{args.iterations}")
        print(f"{'='*60}")

        run_dir = os.path.join(output_dir, f"run_{run_n:02d}")
        os.makedirs(run_dir, exist_ok=True)

        try:
            sample_file = get_sample_file(testfiles_dir, run_n)
        except RuntimeError as e:
            print(f"  FATAL: {e}", file=sys.stderr)
            break

        print(f"  Sample: {sample_file}")

        try:
            active_user = connect_testline(leg_user)
            active_call[0] = True
        except RuntimeError as e:
            print(f"  SKIP run {run_n}: {e}", file=sys.stderr)
            continue

        enable_wav_recording(run_dir)

        try:
            inject_sample(sample_file, active_user)
        except RuntimeError as e:
            print(f"  SKIP run {run_n}: {e}", file=sys.stderr)
            disable_wav_recording()
            hangup()
            active_call[0] = False
            continue

        wait_for_injection_complete(timeout=120)
        wait_for_pipeline(extra_buffer=5)

        hangup()
        active_call[0] = False

        wait_for_wav_files(run_dir, timeout=10)

        print("  Collecting logs...")
        logs = get_all_logs()
        log_file = collect_logs(run_dir, logs)

        wav_files = list_wav_files(run_dir)
        disable_wav_recording()

        collected.append({
            "run": run_n,
            "sample": sample_file,
            "log_file": log_file,
            "wav_files": wav_files,
            "log_entries": len(logs),
        })

        print(f"  Run {run_n} complete: {len(wav_files)} WAV file(s), {len(logs)} log entries")

        if run_n < args.iterations:
            print("  Waiting 3s before next run...")
            time.sleep(3)

            users = get_registered_users()
            if users:
                leg_user = users[0]
            else:
                print("  Waiting for re-registration...")
                try:
                    leg_user = wait_for_registration(timeout=60)
                except RuntimeError as e:
                    print(f"  WARNING: {e}", file=sys.stderr)

    print(f"\n{'='*60}")
    print("SUMMARY")
    print(f"{'='*60}")
    print(f"Completed {len(collected)}/{args.iterations} runs")
    print()
    for c in collected:
        wavs = [os.path.basename(w) for w in c["wav_files"]]
        print(f"  Run {c['run']:02d}: {c['sample']} | {len(c['wav_files'])} WAV(s): {wavs} | "
              f"{c['log_entries']} log entries")
    print()
    print(f"Output written to: {output_dir}")


if __name__ == "__main__":
    main()
