#!/usr/bin/env python3
"""Full pipeline WER (Word Error Rate) test script.

Usage::

    python3 tests/run_pipeline_test.py --engine <ENGINE> [--model <MODEL_NAME>] [--testfiles <DIR>]

    # Legacy positional syntax still supported:
    python3 tests/run_pipeline_test.py <MODEL_NAME> [TESTFILES_DIR]

Args:
    --engine ENGINE   Engine to test: kokoro, neutts, or moshi.
                      For classic engines (kokoro/neutts) the script ensures the
                      correct TTS engine is docked before running.
                      For moshi the script uses the Moshi pipeline instead of classic.
                      (default: whichever engine is currently docked)
    --model NAME      Label for this test run (e.g. "ggml-large-v3-turbo-q5_0").
                      Used in the output filename: /tmp/pipeline_results_<NAME>.json
    --testfiles DIR   Directory containing sample_NN.wav + sample_NN.txt pairs
                      (default: "Testfiles")

Prerequisites:
    - Frontend server must be at http://127.0.0.1:8080 (log API + service
      controls: /api/services/start, /api/services/stop, /api/tts/status).
    - TestSipProvider must be at http://127.0.0.1:22011 (audio injection API).
    - The "alice" SIP account must be registered with the provider.
    - Classic engines (kokoro/neutts): SIP, IAP, VAD, Whisper, LLaMA,
      TTS_SERVICE stage, OAP must be running.
    - Moshi engine: SIP, IAP, MOSHI_SERVICE, OAP must be running.

Test flow for each sample (sample_NN.wav):
    Classic (kokoro/neutts):
        1. Inject WAV audio via SipProvider into alice's RTP stream.
        2. Audio flows: SIP→IAP→VAD→Whisper→LLaMA→TTS(engine)→OAP→SIP.
        3. Poll logs for Whisper transcription entries ("Transcription (Xms): <text>").
        4. Collect all chunks until idle, compare against ground truth.
    Moshi:
        1. Inject WAV audio via SipProvider into alice's RTP stream.
        2. Audio flows: SIP→IAP→MOSHI_SERVICE→OAP→SIP.
        3. Poll logs for Moshi reference text entries ("Moshi transcription: <text>").
        4. Collect all chunks until idle, compare against ground truth.

WER computation:
    Similarity is computed as character-level Levenshtein distance normalized by the
    length of the longer string::

        similarity = (1 - dist/max_len) * 100

    Pre-processing (normalize()):
        - Lowercase, strip punctuation (.,;:!?-—"'«»„"")
        - German number words → digits (e.g. "zwanzig" → "20"), compound numbers handled
          via a hardcoded lookup table for common test cases.
        - Collapse whitespace.

    Thresholds:
        - PASS: similarity >= 99.5%  (essentially perfect transcription)
        - WARN: similarity >= 90%    (acceptable; triggers manual review / tuning)
        - FAIL: similarity < 90%     (below acceptable threshold; requires investigation)
        - TIMEOUT: no transcription received within audio_dur + 20s

Results:
    - Per-sample: status (PASS/WARN/FAIL/TIMEOUT), similarity%, inference ms,
      chunk count, ground truth, and full concatenated transcription.
    - Summary: PASS/WARN/FAIL counts, avg inference time.
    - Saved to: /tmp/pipeline_results_<ENGINE>_<MODEL_NAME>.json
    - Also readable via GET /api/test_results in the frontend UI.
"""
import argparse, json, sys, time, re, urllib.request, os

FRONTEND = "http://127.0.0.1:8080"
SIP_PROVIDER = "http://127.0.0.1:22011"

VALID_ENGINES = ("kokoro", "neutts", "moshi")
ENGINE_SERVICE_MAP = {
    "kokoro": "KOKORO_ENGINE",
    "neutts": "NEUTTS_ENGINE",
    "moshi": "MOSHI_SERVICE",
}

def parse_args():
    if len(sys.argv) > 1 and not sys.argv[1].startswith("-"):
        model = sys.argv[1]
        testfiles = sys.argv[2] if len(sys.argv) > 2 else "Testfiles"
        engine = os.environ.get("WER_ENGINE", "")
        return engine, model, testfiles
    parser = argparse.ArgumentParser(description="Pipeline WER test")
    parser.add_argument("--engine", choices=VALID_ENGINES, default="",
                        help="Engine to test: kokoro, neutts, or moshi")
    parser.add_argument("--model", default="test",
                        help="Label for this test run")
    parser.add_argument("--testfiles", default="Testfiles",
                        help="Directory containing sample_NN.wav + sample_NN.txt")
    args = parser.parse_args()
    return args.engine, args.model, args.testfiles

ENGINE, MODEL_NAME, TESTFILES_DIR = parse_args()

def fetch_json(url, data=None):
    req = urllib.request.Request(url)
    if data:
        req.add_header("Content-Type", "application/json")
        data = json.dumps(data).encode()
    try:
        with urllib.request.urlopen(req, data, timeout=10) as r:
            return json.loads(r.read())
    except Exception as e:
        return {"error": str(e)}

GERMAN_NUMBERS = {
    'null': '0', 'eins': '1', 'zwei': '2', 'drei': '3', 'vier': '4',
    'fünf': '5', 'sechs': '6', 'sieben': '7', 'acht': '8', 'neun': '9',
    'zehn': '10', 'elf': '11', 'zwölf': '12', 'dreizehn': '13', 'vierzehn': '14',
    'fünfzehn': '15', 'sechzehn': '16', 'siebzehn': '17', 'achtzehn': '18', 'neunzehn': '19',
    'zwanzig': '20', 'dreißig': '30', 'vierzig': '40', 'fünfzig': '50',
    'sechzig': '60', 'siebzig': '70', 'achtzig': '80', 'neunzig': '90',
    'hundert': '100', 'tausend': '1000', 'million': '1000000', 'millionen': '1000000',
}

def german_number_to_digits(text):
    compounds = [
        (r'zweitausenddreiundzwanzig', '2023'),
        (r'zweitausendsiebenundachtzig', '2087'),
        (r'zweitausendneunzehn', '2019'),
        (r'zweitausendfünfzehn', '2015'),
        (r'vierhundertfünfundneunzig', '495'),
        (r'dreihundertfünfzig', '350'),
        (r'siebenundsechzig', '67'),
        (r'neunundzwanzig', '29'),
        (r'neunundvierzig', '49'),
    ]
    for pattern, replacement in compounds:
        text = re.sub(pattern, replacement, text, flags=re.IGNORECASE)
    return text

def normalize(s):
    s = s.lower().strip()
    s = german_number_to_digits(s)
    s = re.sub(r'[.,;:!?\-\u2014\"\'«»\u201e\u201c\u201d]', '', s)
    s = re.sub(r'\s+', ' ', s).strip()
    return s

def levenshtein(a, b):
    if len(a) < len(b): return levenshtein(b, a)
    if len(b) == 0: return len(a)
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a):
        curr = [i + 1]
        for j, cb in enumerate(b):
            curr.append(min(prev[j+1]+1, curr[j]+1, prev[j]+(ca!=cb)))
        prev = curr
    return prev[-1]

def similarity(a, b):
    na, nb = normalize(a), normalize(b)
    dist = levenshtein(na, nb)
    mx = max(len(na), len(nb))
    return (1 - dist/mx) * 100 if mx > 0 else 100

def tts_status():
    """Return the name of the currently docked TTS engine, or None."""
    data = fetch_json(f"{FRONTEND}/api/tts/status")
    if "error" in data:
        return None
    return data.get("engine")

def wait_for_tts_engine(expected, timeout_s=60):
    """Poll /api/tts/status until `engine == expected` or timeout."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        cur = tts_status()
        if cur == expected:
            return True
        time.sleep(1)
    return False

def wait_for_tts_engine_gone(previous, timeout_s=30):
    """Wait until /api/tts/status reports a different engine (or None)."""
    deadline = time.time() + timeout_s
    while time.time() < deadline:
        cur = tts_status()
        if cur != previous:
            return True
        time.sleep(0.5)
    return False

def start_service(name):
    r = fetch_json(f"{FRONTEND}/api/services/start", {"name": name})
    return "error" not in r

def stop_service(name):
    r = fetch_json(f"{FRONTEND}/api/services/stop", {"name": name})
    return "error" not in r

def scenario_switch_engine_mid_call():
    """Spec §5.4 scenario: switch engine mid-call.

    The TTS dock uses last-connect-wins: starting a second engine while the
    first is active triggers a HELLO-first swap. The SIP leg must NOT drop
    and the dock's /api/tts/status must flip to the new engine.
    """
    print("=== Scenario: switch engine mid-call ===")
    start = tts_status()
    if start is None:
        print("  SKIP: no engine currently docked; nothing to switch from.")
        return True
    target = "neutts" if start == "kokoro" else "kokoro"
    target_service = "NEUTTS_ENGINE" if target == "neutts" else "KOKORO_ENGINE"
    print(f"  Current engine: {start}  →  target: {target} ({target_service})")

    if not start_service(target_service):
        print(f"  FAIL: could not start {target_service}")
        return False

    if not wait_for_tts_engine(target, timeout_s=90):
        print(f"  FAIL: dock did not swap to {target} within 90s (status={tts_status()})")
        return False

    # SIP leg must still be active
    calls = fetch_json(f"{SIP_PROVIDER}/calls").get("calls", [])
    if not calls:
        print("  FAIL: SIP leg was dropped during swap")
        return False

    print(f"  PASS: dock swapped {start} → {target}, SIP leg intact")
    return True

def scenario_kill_restart_engine():
    """Spec §5.4 scenario: SIGKILL the active engine, restart it, re-dock.

    The dock must go to NONE after the kill, and back to the engine name
    after the restart; audio must resume without restarting LLaMA / OAP /
    TTS stage.
    """
    print("=== Scenario: kill + restart engine ===")
    cur = tts_status()
    if cur is None:
        print("  SKIP: no engine docked.")
        return True
    svc = "NEUTTS_ENGINE" if cur == "neutts" else "KOKORO_ENGINE"
    print(f"  Killing active engine: {cur} ({svc})")
    if not stop_service(svc):
        print(f"  FAIL: could not stop {svc}")
        return False

    # Dock must drop the slot (STATUS → NONE)
    deadline = time.time() + 15
    while time.time() < deadline:
        if tts_status() is None:
            break
        time.sleep(0.5)
    else:
        print(f"  FAIL: dock did not clear slot after {svc} stopped (status={tts_status()})")
        return False

    print(f"  Dock cleared. Restarting {svc}...")
    if not start_service(svc):
        print(f"  FAIL: could not restart {svc}")
        return False

    if not wait_for_tts_engine(cur, timeout_s=90):
        print(f"  FAIL: dock did not re-dock {cur} within 90s (status={tts_status()})")
        return False

    print(f"  PASS: {cur} re-docked without restarting TTS stage / LLaMA / OAP")
    return True

def get_latest_transcription():
    data = fetch_json(f"{FRONTEND}/api/logs?limit=500")
    for log in data.get("logs", []):
        if "Transcription (" in log.get("message", ""):
            m = re.search(r'Transcription \((\d+)ms\):\s*(.*)', log["message"])
            if m:
                return int(m.group(1)), m.group(2).strip()
    return None, None

def is_moshi_engine():
    return ENGINE == "moshi"

def ensure_engine():
    if not ENGINE:
        cur = tts_status()
        if cur:
            print(f"No engine specified, using currently docked engine: {cur}")
        else:
            print("No engine specified and no engine docked. Running with whatever is available.")
        return

    if is_moshi_engine():
        svc_name = ENGINE_SERVICE_MAP["moshi"]
        services = fetch_json(f"{FRONTEND}/api/services")
        moshi_running = False
        for svc in services.get("services", []):
            if svc.get("name") == svc_name and svc.get("pid", 0) > 0:
                moshi_running = True
                break
        if not moshi_running:
            print(f"Starting {svc_name}...")
            if not start_service(svc_name):
                print(f"ERROR: could not start {svc_name}")
                sys.exit(1)
            print(f"  Waiting for Moshi backend to initialize (30s)...")
            time.sleep(30)
        else:
            print(f"Moshi service already running.")
        return

    svc_name = ENGINE_SERVICE_MAP[ENGINE]
    cur = tts_status()
    if cur == ENGINE:
        print(f"Engine '{ENGINE}' already docked.")
        return

    if cur and cur != ENGINE:
        other_svc = ENGINE_SERVICE_MAP.get(cur)
        if other_svc:
            print(f"Stopping current engine '{cur}' ({other_svc})...")
            stop_service(other_svc)
            if not wait_for_tts_engine_gone(cur, timeout_s=30):
                print(f"WARNING: dock did not release {cur} within 30s, continuing anyway")
            time.sleep(2)

    print(f"Starting engine '{ENGINE}' ({svc_name})...")
    if not start_service(svc_name):
        print(f"ERROR: could not start {svc_name}")
        sys.exit(1)
    print(f"  Waiting for '{ENGINE}' to dock (up to 120s)...")
    if not wait_for_tts_engine(ENGINE, timeout_s=120):
        print(f"ERROR: '{ENGINE}' did not dock within 120s (status={tts_status()})")
        sys.exit(1)
    print(f"  Engine '{ENGINE}' docked successfully.")

def collect_transcription_log_pattern():
    if is_moshi_engine():
        return "Moshi transcription: "
    return "Transcription ("

def parse_transcription_log(message):
    if is_moshi_engine():
        m = re.search(r'Moshi transcription:\s*(.*)', message)
        if m:
            return 0, m.group(1).strip()
        return None, None
    m = re.search(r'Transcription \((\d+)ms\):\s*(.*)', message)
    if m:
        return int(m.group(1)), m.group(2).strip()
    return None, None

engine_label = ENGINE if ENGINE else "auto"
print(f"=== Pipeline WER Test: engine={engine_label} model={MODEL_NAME} ===")
print(f"Time: {time.strftime('%Y-%m-%d %H:%M:%S')}")
print()

ensure_engine()
print()

SIP_LEG = "alice"

results = []
last_ts = None

status = fetch_json(f"{SIP_PROVIDER}/calls")
if not status.get("calls") or not status["calls"]:
    print("No active call. Checking if alice is registered...")
    for attempt in range(30):
        users_data = fetch_json(f"{SIP_PROVIDER}/users")
        usernames = [u["username"] for u in users_data.get("users", [])]
        if SIP_LEG in usernames:
            print(f"  {SIP_LEG} is registered. Starting single-leg call...")
            r = fetch_json(f"{SIP_PROVIDER}/conference", {"users": [SIP_LEG]})
            if "error" not in r:
                print(f"  Call started: {r}")
                time.sleep(3)
            else:
                print(f"  ERROR starting call: {r}")
                sys.exit(1)
            break
        print(f"  Attempt {attempt+1}/30: {SIP_LEG} not yet registered. Waiting 2s...")
        time.sleep(2)
    else:
        print(f"ERROR: {SIP_LEG} never registered with SIP provider. Is sip-client running?")
        sys.exit(1)
else:
    print(f"Call already active ({len(status['calls'][0].get('legs',0))} legs)")
    call = status["calls"][0]
    if isinstance(call.get("legs"), list) and len(call["legs"]) >= 2:
        print("  2-leg conference detected — hanging up and restarting as single-leg to avoid feedback loop...")
        fetch_json(f"{SIP_PROVIDER}/hangup", {})
        time.sleep(2)
        r = fetch_json(f"{SIP_PROVIDER}/conference", {"users": [SIP_LEG]})
        if "error" in r:
            print(f"  ERROR restarting single-leg call: {r}")
            sys.exit(1)
        print(f"  Single-leg call started: {r}")
        time.sleep(3)

log_pattern = collect_transcription_log_pattern()

data = fetch_json(f"{FRONTEND}/api/logs?limit=500")
baseline_msgs = set()
for log in data.get("logs", []):
    if log_pattern in log.get("message", ""):
        baseline_msgs.add(log["timestamp"] + log["message"])

for i in range(1, 21):
    sample = f"sample_{i:02d}"
    gt_file = os.path.join(TESTFILES_DIR, f"{sample}.txt")
    
    if not os.path.exists(gt_file):
        print(f"SKIP {sample}: no ground truth")
        continue
    
    with open(gt_file) as f:
        ground_truth = f.read().strip()
    
    result = fetch_json(f"{SIP_PROVIDER}/inject", {"file": f"{sample}.wav", "leg": SIP_LEG})
    if "error" in result:
        print(f"FAIL {sample}: injection error: {result['error']}")
        results.append((sample, "FAIL", 0, 0, ground_truth, f"injection error: {result['error']}"))
        continue
    
    wav_file = os.path.join(TESTFILES_DIR, f"{sample}.wav")
    audio_dur_s = 10.0
    try:
        import wave
        with wave.open(wav_file) as wf:
            audio_dur_s = wf.getnframes() / wf.getframerate()
    except Exception:
        pass
    
    all_transcriptions = []
    inference_ms_total = []
    max_wait = int(audio_dur_s + 20)
    last_chunk_time = None
    idle_timeout = 6.0
    
    poll_start = time.time()
    while time.time() - poll_start < max_wait:
        time.sleep(2)
        data = fetch_json(f"{FRONTEND}/api/logs?limit=500")
        found_new = False
        for log in reversed(data.get("logs", [])):
            key = log["timestamp"] + log["message"]
            if key in baseline_msgs:
                continue
            if log_pattern in log.get("message", ""):
                ms, txt = parse_transcription_log(log["message"])
                if txt is not None:
                    baseline_msgs.add(key)
                    all_transcriptions.append((ms, txt))
                    inference_ms_total.append(ms)
                    last_chunk_time = time.time()
                    found_new = True
        if not found_new and last_chunk_time is not None:
            if time.time() - last_chunk_time >= idle_timeout:
                break
    
    if not all_transcriptions:
        print(f"TIMEOUT {sample}: no transcription after {max_wait}s")
        results.append((sample, "TIMEOUT", 0, 0, ground_truth, ""))
        time.sleep(5)
        continue
    
    full_transcription = " ".join(t for _, t in all_transcriptions)
    avg_ms = int(sum(inference_ms_total) / len(inference_ms_total)) if inference_ms_total else 0
    
    if len(all_transcriptions) > 1:
        print(f"  [{len(all_transcriptions)} chunks: {' | '.join(t[:30] for _,t in all_transcriptions)}]")
    
    sim = similarity(ground_truth, full_transcription)
    if sim >= 99.5:
        verdict = "PASS"
    elif sim >= 90:
        verdict = "WARN"
    else:
        verdict = "FAIL"
    
    gt_short = ground_truth[:60]
    tr_short = full_transcription[:60]
    print(f"{verdict} {sample}: {sim:.1f}% ({avg_ms}ms avg, {len(all_transcriptions)} chunk(s))")
    print(f"  GT:  {gt_short}")
    print(f"  GOT: {tr_short}")
    print()
    
    results.append((sample, verdict, sim, avg_ms, ground_truth, full_transcription))
    
    time.sleep(3)

# Summary
print()
print("=" * 60)
print(f"SUMMARY: engine={engine_label} model={MODEL_NAME}")
print("=" * 60)
pass_count = sum(1 for r in results if r[1] == "PASS")
warn_count = sum(1 for r in results if r[1] == "WARN")
fail_count = sum(1 for r in results if r[1] in ("FAIL", "TIMEOUT"))
times = [r[3] for r in results if r[3] > 0]
avg_time = sum(times) / len(times) if times else 0

print(f"PASS: {pass_count}, WARN: {warn_count}, FAIL: {fail_count}")
print(f"Avg inference: {avg_time:.0f}ms")
print()
for r in results:
    print(f"  {r[1]:7s} {r[0]}: {r[2]:.1f}% ({r[3]}ms)")

# Save results as JSON
safe_name = MODEL_NAME.replace(' ', '_').replace('+', '_')
outfile = f"/tmp/pipeline_results_{engine_label}_{safe_name}.json"
with open(outfile, 'w') as f:
    json.dump({
        "engine": engine_label,
        "model": MODEL_NAME,
        "results": [{"sample": r[0], "status": r[1], "similarity": r[2], 
                     "inference_ms": r[3], "ground_truth": r[4], "transcription": r[5]} for r in results],
        "summary": {"pass": pass_count, "warn": warn_count, "fail": fail_count, "avg_ms": avg_time}
    }, f, indent=2, ensure_ascii=False)
print(f"\nResults saved to {outfile}")

# Optional TTS dock scenarios (spec §5.4). Enable with RUN_TTS_SCENARIOS=1
if os.environ.get("RUN_TTS_SCENARIOS") == "1":
    print()
    print("=" * 60)
    print("TTS dock scenarios")
    print("=" * 60)
    ok1 = scenario_switch_engine_mid_call()
    ok2 = scenario_kill_restart_engine()
    if not (ok1 and ok2):
        sys.exit(2)
