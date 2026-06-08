import os
import sys
import glob
import json
import subprocess
import re
import tempfile
import numpy as np
import soundfile as sf
from collections import defaultdict

PROCESSED_DIRS = [
    "/Volumes/eHDD/moshi-rag-data/processed",
    "/Volumes/eHDD/moshi-rag-data/processed2",
    "/Volumes/eHDD/moshi-rag-data/processed3",
]
WHISPER_CLI = os.path.join(os.path.dirname(os.path.dirname(__file__)), "whisper-cpp", "build", "bin", "whisper-cli")
WHISPER_MODEL = os.path.join(os.path.dirname(os.path.dirname(__file__)), "bin", "models", "ggml-large-v3-turbo-q5_0.bin")

FACTS_KEYWORDS = [
    "schwarzschild", "quantenverschränkung", "hawking", "gravitationswellen",
    "superposition", "schrödinger", "dunkle materie", "zentrifugalkraft",
    "doppelspaltexperiment", "heisenberg", "unschärferelation", "raumzeit",
    "injected reference", "end of injected reference"
]


def compute_wer(reference, hypothesis):
    ref_words = reference.lower().split()
    hyp_words = hypothesis.lower().split()
    if not ref_words:
        return 0.0 if not hyp_words else 1.0

    d = np.zeros((len(ref_words) + 1, len(hyp_words) + 1), dtype=int)
    for i in range(len(ref_words) + 1):
        d[i][0] = i
    for j in range(len(hyp_words) + 1):
        d[0][j] = j

    for i in range(1, len(ref_words) + 1):
        for j in range(1, len(hyp_words) + 1):
            if ref_words[i - 1] == hyp_words[j - 1]:
                d[i][j] = d[i - 1][j - 1]
            else:
                d[i][j] = 1 + min(d[i - 1][j], d[i][j - 1], d[i - 1][j - 1])

    return d[len(ref_words)][len(hyp_words)] / len(ref_words)


def run_whisper_on_channel(wav_path, channel=0, start_sec=0.0, duration_sec=30.0, timeout=90):
    try:
        audio, sr = sf.read(wav_path)
    except Exception:
        return ""

    if audio.ndim == 2:
        ch_audio = audio[:, channel]
    else:
        ch_audio = audio

    start_sample = int(start_sec * sr)
    end_sample = min(int((start_sec + duration_sec) * sr), len(ch_audio))
    if start_sample >= end_sample:
        return ""
    segment = ch_audio[start_sample:end_sample]

    if sr != 16000:
        num_samples = int(len(segment) * 16000 / sr)
        from scipy.signal import resample as scipy_resample
        segment = scipy_resample(segment, num_samples).astype(np.float32)

    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        tmp_path = tmp.name
        sf.write(tmp_path, segment, 16000)

    try:
        cmd = [WHISPER_CLI, "-m", WHISPER_MODEL, "-l", "de", "--no-timestamps", "-f", tmp_path]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout)
        lines = res.stdout.strip().split("\n")
        clean = [l.strip() for l in lines if l.strip() and not l.startswith("[")]
        return " ".join(clean).strip()
    except Exception:
        return ""
    finally:
        os.unlink(tmp_path)


def check_stereo_format(wav_path):
    try:
        audio, sr = sf.read(wav_path)
    except Exception as e:
        return "CORRUPT", f"Cannot read: {e}"

    if audio.ndim != 2 or audio.shape[1] != 2:
        return "FAIL", f"Not stereo: shape={audio.shape}"

    if sr != 24000:
        return "FAIL", f"Wrong sample rate: {sr} (expected 24000)"

    left_rms = np.sqrt(np.mean(audio[:, 0] ** 2))
    right_rms = np.sqrt(np.mean(audio[:, 1] ** 2))

    if left_rms < 0.0001 and right_rms < 0.0001:
        return "FAIL", "Both channels silent"

    left_nonzero = np.count_nonzero(audio[:, 0])
    right_nonzero = np.count_nonzero(audio[:, 1])
    total = audio.shape[0]

    left_active_pct = left_nonzero / total * 100
    right_active_pct = right_nonzero / total * 100

    dur = audio.shape[0] / sr
    if dur < 0.5:
        return "WARN", f"Very short: {dur:.2f}s"

    return "OK", f"dur={dur:.2f}s L_RMS={left_rms:.4f}({left_active_pct:.0f}%) R_RMS={right_rms:.4f}({right_active_pct:.0f}%)"


def check_json_integrity(json_path):
    try:
        with open(json_path, "r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception as e:
        return "CORRUPT", f"Cannot parse JSON: {e}", [], False

    aligns = data.get("alignments", [])
    if not aligns:
        return "FAIL", "Empty alignments", [], False

    words = []
    has_fact = False
    prev_end = -1
    timing_ok = True
    for item in aligns:
        if not isinstance(item, list) or len(item) < 3:
            return "FAIL", f"Malformed alignment entry: {item}", [], False
        word = item[0]
        ts = item[1]
        if not isinstance(ts, list) or len(ts) < 2:
            return "FAIL", f"Malformed timestamp: {item}", [], False
        if ts[0] < 0 or ts[1] < ts[0]:
            timing_ok = False
        words.append(word)
        wl = word.lower()
        for kw in FACTS_KEYWORDS:
            if kw in wl:
                has_fact = True

    return "OK", f"{len(words)} words, timing={'OK' if timing_ok else 'BAD'}, facts={'YES' if has_fact else 'NO'}", words, has_fact


def verify_processed_dir(PROCESSED_DIR):
    print("=" * 80)
    print(f"COMPREHENSIVE DATASET VERIFICATION: {PROCESSED_DIR}")
    print("=" * 80)

    if not os.path.exists(PROCESSED_DIR):
        print(f"  Directory does not exist, skipping.")
        return {}

    datasets = sorted([d for d in os.listdir(PROCESSED_DIR)
                        if os.path.isdir(os.path.join(PROCESSED_DIR, d))])
    print(f"Found datasets: {datasets}")

    summary = defaultdict(lambda: {
        "total": 0, "ok": 0, "fail": 0, "corrupt": 0, "warn": 0,
        "wer_samples": 0, "wer_sum": 0.0, "wer_pass": 0, "wer_fail": 0,
        "facts_count": 0, "total_duration": 0.0,
        "corrupt_files": [], "fail_files": [], "wer_fail_files": []
    })

    for ds in datasets:
        ds_dir = os.path.join(PROCESSED_DIR, ds)
        wav_files = sorted(glob.glob(os.path.join(ds_dir, "*.wav")))
        json_files = sorted(glob.glob(os.path.join(ds_dir, "*.json")))

        print(f"\n{'=' * 60}")
        print(f"Dataset: {ds} ({len(wav_files)} WAV, {len(json_files)} JSON)")
        print("=" * 60)

        s = summary[ds]
        s["total"] = len(wav_files)

        wer_interval = max(1, len(wav_files) // 20)

        for i, wav_path in enumerate(wav_files):
            json_path = wav_path.replace(".wav", ".json")

            status, msg = check_stereo_format(wav_path)
            if status == "CORRUPT":
                s["corrupt"] += 1
                s["corrupt_files"].append((os.path.basename(wav_path), msg))
                continue
            elif status == "FAIL":
                s["fail"] += 1
                s["fail_files"].append((os.path.basename(wav_path), msg))
                continue
            elif status == "WARN":
                s["warn"] += 1

            dur_match = re.search(r"dur=([\d.]+)s", msg)
            if dur_match:
                s["total_duration"] += float(dur_match.group(1))

            if os.path.exists(json_path):
                j_status, j_msg, words, has_fact = check_json_integrity(json_path)
                if j_status == "CORRUPT":
                    s["corrupt"] += 1
                    s["corrupt_files"].append((os.path.basename(json_path), j_msg))
                    continue
                elif j_status == "FAIL":
                    s["fail"] += 1
                    s["fail_files"].append((os.path.basename(json_path), j_msg))
                    continue

                if has_fact:
                    s["facts_count"] += 1
            else:
                s["fail"] += 1
                s["fail_files"].append((os.path.basename(wav_path), "Missing JSON"))
                continue

            s["ok"] += 1

            if i % wer_interval == 0 and words:
                gt_words = [w for w in words if w not in ["[Injected", "reference]", "[End", "of", "injected"]]
                fact_word_set = set()
                for kw in FACTS_KEYWORDS:
                    for w in kw.split():
                        fact_word_set.add(w.lower())

                clean_words = []
                in_fact = False
                for w in words:
                    if w == "[Injected":
                        in_fact = True
                        continue
                    if w == "reference]" and in_fact:
                        continue
                    if in_fact and w == "[End":
                        continue
                    if in_fact and w == "of":
                        continue
                    if in_fact and w == "injected":
                        continue
                    if in_fact and w == "reference]":
                        in_fact = False
                        continue
                    if not in_fact:
                        clean_words.append(w)

                gt_text = " ".join(clean_words)
                gt_text = re.sub(r"\s+", " ", gt_text).strip()

                if len(gt_text.split()) < 2:
                    continue

                with open(json_path) as jf:
                    jdata = json.load(jf)
                aligns = jdata.get("alignments", [])
                non_fact_aligns = []
                skip = False
                for a in aligns:
                    if a[0] == "[Injected":
                        skip = True
                        continue
                    if skip and a[0] == "reference]" and a[2] == "SPEAKER_MAIN":
                        if any(kw in " ".join([x[0].lower() for x in aligns]) for kw in FACTS_KEYWORDS[:3]):
                            continue
                        skip = False
                        continue
                    if not skip:
                        non_fact_aligns.append(a)

                if non_fact_aligns:
                    mid_idx = len(non_fact_aligns) // 2
                    wer_start = max(0.0, non_fact_aligns[max(0, mid_idx - 5)][1][0])
                    wer_end = non_fact_aligns[min(len(non_fact_aligns) - 1, mid_idx + 30)][1][1]
                    wer_dur = min(30.0, wer_end - wer_start)

                    wer_gt_words = []
                    for a in non_fact_aligns:
                        if a[1][1] > wer_start and a[1][0] < wer_start + wer_dur:
                            wer_gt_words.append(a[0])
                    wer_gt = " ".join(wer_gt_words)
                else:
                    wer_start = 0.0
                    wer_dur = 30.0
                    wer_gt = gt_text[:200]

                whisper_text = run_whisper_on_channel(wav_path, channel=0,
                                                      start_sec=wer_start, duration_sec=wer_dur)
                if not whisper_text:
                    continue

                gt_clean = re.sub(r"[^\w\s]", "", wer_gt.lower())
                wh_clean = re.sub(r"[^\w\s]", "", whisper_text.lower())
                wer = compute_wer(gt_clean, wh_clean)

                s["wer_samples"] += 1
                s["wer_sum"] += wer

                if wer <= 0.5:
                    s["wer_pass"] += 1
                else:
                    s["wer_fail"] += 1
                    s["wer_fail_files"].append((os.path.basename(wav_path), f"WER={wer:.2%}", wer_gt[:80], whisper_text[:80]))

                if (i + 1) % (wer_interval * 5) == 0:
                    avg_wer = s["wer_sum"] / s["wer_samples"] if s["wer_samples"] > 0 else 0
                    print(f"  [{i+1}/{len(wav_files)}] WER samples={s['wer_samples']}, avg_WER={avg_wer:.2%}")

        avg_wer = s["wer_sum"] / s["wer_samples"] if s["wer_samples"] > 0 else 0
        print(f"  {ds}: OK={s['ok']} FAIL={s['fail']} CORRUPT={s['corrupt']} WARN={s['warn']}")
        print(f"  Duration: {s['total_duration']/3600:.2f}h, Facts: {s['facts_count']}")
        print(f"  WER: avg={avg_wer:.2%} ({s['wer_pass']} pass, {s['wer_fail']} fail out of {s['wer_samples']} samples)")

    print("\n" + "=" * 80)
    print("FINAL SUMMARY")
    print("=" * 80)

    total_ok = 0
    total_fail = 0
    total_corrupt = 0
    total_facts = 0
    total_dur = 0.0
    total_wer_samples = 0
    total_wer_sum = 0.0

    for ds, s in sorted(summary.items()):
        avg_wer = s["wer_sum"] / s["wer_samples"] if s["wer_samples"] > 0 else 0
        facts_pct = (s["facts_count"] / s["ok"] * 100) if s["ok"] > 0 else 0
        print(f"  {ds:20s}: {s['total']:6d} files | OK={s['ok']:5d} FAIL={s['fail']:3d} CORRUPT={s['corrupt']:3d} | "
              f"dur={s['total_duration']/3600:.1f}h | facts={s['facts_count']:4d} ({facts_pct:.1f}%) | "
              f"WER={avg_wer:.2%} ({s['wer_samples']} samples)")
        total_ok += s["ok"]
        total_fail += s["fail"]
        total_corrupt += s["corrupt"]
        total_facts += s["facts_count"]
        total_dur += s["total_duration"]
        total_wer_samples += s["wer_samples"]
        total_wer_sum += s["wer_sum"]

    grand_wer = total_wer_sum / total_wer_samples if total_wer_samples > 0 else 0
    print(f"\n  {'TOTAL':20s}: {total_ok + total_fail + total_corrupt:6d} files | OK={total_ok:5d} FAIL={total_fail:3d} CORRUPT={total_corrupt:3d}")
    print(f"  Total duration: {total_dur/3600:.2f} hours")
    print(f"  Total facts injections: {total_facts}")
    print(f"  Grand avg WER: {grand_wer:.2%} ({total_wer_samples} samples)")

    print("\n" + "=" * 80)
    print("CORRUPT FILES")
    print("=" * 80)
    any_issues = False
    for ds, s in sorted(summary.items()):
        for fname, reason in s["corrupt_files"]:
            print(f"  [{ds}] {fname}: {reason}")
            any_issues = True
    if not any_issues:
        print("  None")

    print("\n" + "=" * 80)
    print("FAILED FILES")
    print("=" * 80)
    any_issues = False
    for ds, s in sorted(summary.items()):
        for fname, reason in s["fail_files"][:20]:
            print(f"  [{ds}] {fname}: {reason}")
            any_issues = True
        if len(s["fail_files"]) > 20:
            print(f"  [{ds}] ... and {len(s['fail_files']) - 20} more")
    if not any_issues:
        print("  None")

    print("\n" + "=" * 80)
    print("HIGH WER FILES (WER > 50%)")
    print("=" * 80)
    any_issues = False
    for ds, s in sorted(summary.items()):
        for entry in s["wer_fail_files"][:10]:
            fname, wer_str, gt, hyp = entry
            print(f"  [{ds}] {fname}: {wer_str}")
            print(f"    GT:  {gt}")
            print(f"    HYP: {hyp}")
            any_issues = True
        if len(s["wer_fail_files"]) > 10:
            print(f"  [{ds}] ... and {len(s['wer_fail_files']) - 10} more high-WER files")
    if not any_issues:
        print("  None")

    report_path = os.path.join(PROCESSED_DIR, "verification_report.json")
    report = {}
    for ds, s in summary.items():
        avg_wer = s["wer_sum"] / s["wer_samples"] if s["wer_samples"] > 0 else 0
        report[ds] = {
            "total": s["total"],
            "ok": s["ok"],
            "fail": s["fail"],
            "corrupt": s["corrupt"],
            "warn": s["warn"],
            "duration_hours": round(s["total_duration"] / 3600, 2),
            "facts_count": s["facts_count"],
            "facts_pct": round((s["facts_count"] / s["ok"] * 100) if s["ok"] > 0 else 0, 2),
            "avg_wer": round(avg_wer, 4),
            "wer_samples": s["wer_samples"],
            "wer_pass": s["wer_pass"],
            "wer_fail": s["wer_fail"],
            "corrupt_files": s["corrupt_files"][:50],
            "fail_files": s["fail_files"][:50],
            "wer_fail_files": [(f, w) for f, w, _, _ in s["wer_fail_files"][:50]]
        }
    with open(report_path, "w") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    print(f"\nDetailed report saved to: {report_path}")
    return report


def main():
    import sys as _sys
    if len(_sys.argv) > 1:
        dirs_to_check = _sys.argv[1:]
    else:
        dirs_to_check = PROCESSED_DIRS

    all_reports = {}
    for pdir in dirs_to_check:
        report = verify_processed_dir(pdir)
        all_reports[pdir] = report
        print()

    print("=" * 80)
    print("ALL VARIANTS VERIFICATION COMPLETE")
    print("=" * 80)
    for pdir, report in all_reports.items():
        if not report:
            print(f"  {pdir}: SKIPPED (not found)")
            continue
        total_ok = sum(r.get("ok", 0) for r in report.values())
        total_files = sum(r.get("total", 0) for r in report.values())
        total_dur = sum(r.get("duration_hours", 0) for r in report.values())
        print(f"  {pdir}: {total_ok}/{total_files} OK, {total_dur:.1f}h")


if __name__ == "__main__":
    main()
