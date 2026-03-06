#!/usr/bin/env python3
"""Stage 7 Diagnostic Collection Script.

Automates 10 test runs that each:
  1. Start all services (if not already running)
  2. Wait 10s for warmup
  3. Connect 1 testline to test_sip_provider
  4. Enable WAV recording in test_sip_provider and OAP
  5. Inject a sample file into the testline
  6. Wait for pipeline completion
  7. Collect logs
  8. Hang up (triggers WAV file write)
  9. Copy WAV files to run output directory

Usage:
    python3 tests/run_stage7.py [--output-dir stage7_output] [--iterations 10]
                                [--testfiles-dir Testfiles] [--no-start-services]
"""
import argparse
import glob
import json
import os
import shutil
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


def fetch_json(url, data=None, method=None):
    req = urllib.request.Request(url)
    if data is not None:
        body = json.dumps(data).encode()
        req.add_header("Content-Type", "application/json")
        req.data = body
    if method:
        req.method = method
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
    r = post_json(f"{FRONTEND}/api/services/start", {"service": name})
    return r


def stop_service(name):
    r = post_json(f"{FRONTEND}/api/services/stop", {"service": name})
    return r


def get_logs(limit=2000):
    return fetch_json(f"{FRONTEND}/api/logs?limit={limit}")


def start_all_services():
    print("  Starting all pipeline services...")
    svcs = get_services()
    running = {s["name"] for s in svcs.get("services", []) if s.get("running")}

    for name in PIPELINE_SERVICES:
        if name in running:
            print(f"    {name}: already running")
        else:
            r = start_service(name)
            if "error" in r:
                print(f"    {name}: FAILED to start — {r['error']}", file=sys.stderr)
            else:
                print(f"    {name}: started")

    if TSP_SERVICE in running:
        print(f"    {TSP_SERVICE}: already running")
    else:
        r = start_service(TSP_SERVICE)
        if "error" in r:
            print(f"    {TSP_SERVICE}: FAILED to start — {r['error']}", file=sys.stderr)
        else:
            print(f"    {TSP_SERVICE}: started")


def wait_for_warmup(seconds=10):
    print(f"  Waiting {seconds}s for services to warm up...")
    time.sleep(seconds)


def get_registered_users():
    r = fetch_json(f"{SIP_PROVIDER}/users")
    return [u["username"] for u in r.get("users", [])]


def connect_testline(username):
    print(f"  Connecting testline for user '{username}'...")
    calls = fetch_json(f"{SIP_PROVIDER}/calls")
    if calls.get("calls"):
        call = calls["calls"][0]
        legs = call.get("legs", [])
        if isinstance(legs, list) and len(legs) >= 1:
            print(f"    Call already active with {len(legs)} leg(s) — reusing")
            return legs[0].get("user", username) if legs else username
        leg_count = legs if isinstance(legs, int) else 1
        print(f"    Call already active ({leg_count} leg(s)) — reusing")
        return username

    r = post_json(f"{SIP_PROVIDER}/conference", {"users": [username]})
    if "error" in r:
        raise RuntimeError(f"Failed to start conference: {r['error']}")
    print(f"    Conference started: {r}")
    time.sleep(2)
    return username


def wait_for_registration(timeout=60):
    print(f"  Waiting for a SIP user to register with test_sip_provider (max {timeout}s)...")
    deadline = time.time() + timeout
    while time.time() < deadline:
        users = get_registered_users()
        if users:
            print(f"    Registered: {users}")
            return users[0]
        time.sleep(2)
    raise RuntimeError("No SIP user registered with test_sip_provider within timeout")


def enable_wav_recording(output_dir):
    print(f"  Enabling WAV recording in test_sip_provider -> {output_dir}")
    r = post_json(f"{SIP_PROVIDER}/wav_recording", {"enabled": True, "dir": output_dir})
    if "error" in r:
        print(f"    WARNING: test_sip_provider wav_recording failed: {r['error']}", file=sys.stderr)
    else:
        print(f"    test_sip_provider: {r}")

    print(f"  Enabling WAV recording in OAP -> {output_dir}")
    r = post_json(f"{FRONTEND}/api/oap/wav_recording", {"enabled": True, "dir": output_dir})
    if "error" in r:
        print(f"    WARNING: OAP wav_recording failed: {r['error']}", file=sys.stderr)
    else:
        print(f"    OAP: {r}")


def disable_wav_recording():
    post_json(f"{SIP_PROVIDER}/wav_recording", {"enabled": False, "dir": ""})
    post_json(f"{FRONTEND}/api/oap/wav_recording", {"enabled": False, "dir": ""})


def inject_sample(sample_file, leg_username):
    print(f"  Injecting sample '{sample_file}' into leg '{leg_username}'...")
    r = post_json(f"{SIP_PROVIDER}/inject", {
        "file": sample_file,
        "leg": leg_username,
        "no_silence": True,
    })
    if "error" in r:
        raise RuntimeError(f"Injection failed: {r['error']}")
    print(f"    Inject response: {r}")
    return r


def wait_for_injection_complete(timeout=120):
    print("  Waiting for injection to complete...")
    deadline = time.time() + timeout
    while time.time() < deadline:
        status = fetch_json(f"{SIP_PROVIDER}/status")
        if status.get("injecting") is None:
            print("    Injection complete (injecting=null)")
            return
        time.sleep(1)
    print("  WARNING: injection did not complete within timeout", file=sys.stderr)


def wait_for_pipeline(extra_buffer=5):
    print(f"  Waiting {extra_buffer}s for Kokoro+OAP to finish...")
    time.sleep(extra_buffer)


def collect_logs(log_file, limit=2000):
    print(f"  Collecting logs -> {log_file}")
    data = get_logs(limit=limit)
    logs = data.get("logs", [])
    os.makedirs(os.path.dirname(log_file), exist_ok=True)
    with open(log_file, "w", encoding="utf-8") as f:
        for entry in logs:
            ts = entry.get("timestamp", "")
            svc = entry.get("service", "")
            lvl = entry.get("level", "")
            msg = entry.get("message", "")
            f.write(f"[{ts}] [{svc}] [{lvl}] {msg}\n")
    print(f"    {len(logs)} log entries saved")
    return logs


def hangup():
    print("  Hanging up call...")
    r = post_json(f"{SIP_PROVIDER}/hangup", {})
    print(f"    Hangup: {r}")
    time.sleep(1)


def copy_wav_files(src_dir, dst_dir):
    os.makedirs(dst_dir, exist_ok=True)
    copied = []
    for pattern in ["tsp_call_*.wav", "oap_call_*.wav"]:
        for fpath in glob.glob(os.path.join(src_dir, pattern)):
            dst = os.path.join(dst_dir, os.path.basename(fpath))
            shutil.copy2(fpath, dst)
            copied.append(dst)
            print(f"    Copied: {os.path.basename(fpath)}")
    if not copied:
        print("    WARNING: no WAV files found to copy", file=sys.stderr)
    return copied


def get_sample_files(testfiles_dir, iteration):
    samples = sorted(glob.glob(os.path.join(testfiles_dir, "sample_*.wav")))
    if not samples:
        raise RuntimeError(f"No sample_*.wav files found in {testfiles_dir}")
    idx = (iteration - 1) % len(samples)
    return os.path.basename(samples[idx])


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

    print(f"=== Stage 7 Diagnostic Collection ===")
    print(f"Output directory : {output_dir}")
    print(f"Iterations       : {args.iterations}")
    print(f"Test files       : {testfiles_dir}")
    print()

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
        wav_staging_dir = os.path.join(run_dir, "wav_staging")
        os.makedirs(wav_staging_dir, exist_ok=True)

        sample_file = get_sample_files(testfiles_dir, run_n)
        print(f"  Sample: {sample_file}")

        try:
            active_user = connect_testline(leg_user)
        except RuntimeError as e:
            print(f"  SKIP run {run_n}: {e}", file=sys.stderr)
            continue

        enable_wav_recording(wav_staging_dir)

        try:
            inject_sample(sample_file, active_user)
        except RuntimeError as e:
            print(f"  SKIP run {run_n}: {e}", file=sys.stderr)
            disable_wav_recording()
            continue

        wait_for_injection_complete(timeout=120)
        wait_for_pipeline(extra_buffer=5)

        log_file = os.path.join(run_dir, "pipeline.log")
        logs = collect_logs(log_file)

        hangup()
        time.sleep(2)

        wav_files = copy_wav_files(wav_staging_dir, run_dir)
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
            if not users:
                print("  Waiting for re-registration...")
                try:
                    leg_user = wait_for_registration(timeout=60)
                except RuntimeError as e:
                    print(f"  WARNING: {e}", file=sys.stderr)
            else:
                leg_user = users[0]

    print(f"\n{'='*60}")
    print("SUMMARY")
    print(f"{'='*60}")
    print(f"Completed {len(collected)}/{args.iterations} runs")
    print()
    for c in collected:
        wavs = [os.path.basename(w) for w in c["wav_files"]]
        print(f"  Run {c['run']:02d}: {c['sample']} | {len(c['wav_files'])} WAV(s): {wavs}")
    print()
    print(f"Output written to: {output_dir}")


if __name__ == "__main__":
    main()
