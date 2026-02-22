#!/usr/bin/env python3
import json, sys, time, re, urllib.request, os

FRONTEND = "http://127.0.0.1:8080"
SIP_PROVIDER = "http://127.0.0.1:22011"
TESTFILES_DIR = sys.argv[2] if len(sys.argv) > 2 else "Testfiles"
MODEL_NAME = sys.argv[1] if len(sys.argv) > 1 else "test"

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

def get_latest_transcription():
    data = fetch_json(f"{FRONTEND}/api/logs")
    for log in data.get("logs", []):
        if "Transcription (" in log.get("message", ""):
            m = re.search(r'Transcription \((\d+)ms\):\s*(.*)', log["message"])
            if m:
                return int(m.group(1)), m.group(2).strip()
    return None, None

print(f"=== Pipeline Test: {MODEL_NAME} ===")
print(f"Time: {time.strftime('%Y-%m-%d %H:%M:%S')}")
print()

results = []
last_ts = None

# Get current latest transcription timestamp to use as baseline
data = fetch_json(f"{FRONTEND}/api/logs")
baseline_msgs = set()
for log in data.get("logs", []):
    if "Transcription (" in log.get("message", ""):
        baseline_msgs.add(log["timestamp"] + log["message"])

for i in range(1, 21):
    sample = f"sample_{i:02d}"
    gt_file = os.path.join(TESTFILES_DIR, f"{sample}.txt")
    
    if not os.path.exists(gt_file):
        print(f"SKIP {sample}: no ground truth")
        continue
    
    with open(gt_file) as f:
        ground_truth = f.read().strip()
    
    # Inject
    result = fetch_json(f"{SIP_PROVIDER}/inject", {"file": f"{sample}.wav", "leg": "a"})
    if "error" in result:
        print(f"FAIL {sample}: injection error: {result['error']}")
        results.append((sample, "FAIL", 0, 0, ground_truth, f"injection error: {result['error']}"))
        continue
    
    # Wait for transcription (audio duration + VAD flush + inference)
    transcription = None
    whisper_ms = 0
    
    for attempt in range(40):  # up to 80s
        time.sleep(2)
        data = fetch_json(f"{FRONTEND}/api/logs")
        for log in data.get("logs", []):
            key = log["timestamp"] + log["message"]
            if key in baseline_msgs:
                continue
            if "Transcription (" in log.get("message", ""):
                m = re.search(r'Transcription \((\d+)ms\):\s*(.*)', log["message"])
                if m:
                    whisper_ms = int(m.group(1))
                    transcription = m.group(2).strip()
                    baseline_msgs.add(key)
                    break
        if transcription is not None:
            break
    
    if transcription is None:
        print(f"TIMEOUT {sample}: no transcription after 80s")
        results.append((sample, "TIMEOUT", 0, 0, ground_truth, ""))
        time.sleep(5)
        continue
    
    sim = similarity(ground_truth, transcription)
    if sim >= 99.5:
        status = "PASS"
    elif sim >= 90:
        status = "WARN"
    else:
        status = "FAIL"
    
    gt_short = ground_truth[:60]
    tr_short = transcription[:60]
    print(f"{status} {sample}: {sim:.1f}% ({whisper_ms}ms)")
    print(f"  GT:  {gt_short}")
    print(f"  GOT: {tr_short}")
    print()
    
    results.append((sample, status, sim, whisper_ms, ground_truth, transcription))
    
    # Wait for silence flush before next injection
    time.sleep(5)

# Summary
print()
print("=" * 60)
print(f"SUMMARY: {MODEL_NAME}")
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
outfile = f"/tmp/pipeline_results_{MODEL_NAME.replace(' ', '_').replace('+', '_')}.json"
with open(outfile, 'w') as f:
    json.dump({
        "model": MODEL_NAME,
        "results": [{"sample": r[0], "status": r[1], "similarity": r[2], 
                     "inference_ms": r[3], "ground_truth": r[4], "transcription": r[5]} for r in results],
        "summary": {"pass": pass_count, "warn": warn_count, "fail": fail_count, "avg_ms": avg_time}
    }, f, indent=2, ensure_ascii=False)
print(f"\nResults saved to {outfile}")
