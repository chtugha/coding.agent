#!/usr/bin/env python3
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
from concurrent.futures import ThreadPoolExecutor

PROCESSED_DIR = "/Volumes/eHDD/moshi-rag-data/processed"
WHISPER_CLI = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           "whisper-cpp", "build", "bin", "whisper-cli")
WHISPER_MODEL = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                             "bin", "models", "ggml-large-v3-turbo-q5_0.bin")

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


def whisper_transcribe(audio_segment, sr, timeout=120):
    if sr != 16000:
        import librosa
        audio_segment = librosa.resample(audio_segment.astype(np.float32), orig_sr=sr, target_sr=16000)
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        tmp_path = tmp.name
        sf.write(tmp_path, audio_segment, 16000)
    try:
        cmd = [WHISPER_CLI, "-m", WHISPER_MODEL, "-l", "de", "--no-timestamps", "-f", tmp_path]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, timeout=timeout)
        lines = res.stdout.strip().split("\n")
        clean = [l.strip() for l in lines if l.strip() and not l.startswith("[")]
        return " ".join(clean).strip()
    except Exception:
        return ""
    finally:
        os.unlink(tmp_path)


def check_wav_format(wav_path, read_audio=False):
    try:
        info = sf.info(wav_path)
    except Exception as e:
        return "CORRUPT", f"Cannot read: {e}", None, None

    if info.channels != 2:
        return "FAIL", f"Not stereo: channels={info.channels}", None, None

    if info.samplerate != 48000:
        return "FAIL", f"Wrong sample rate: {info.samplerate} (expected 48000)", None, None

    dur = info.duration

    if read_audio:
        try:
            audio, sr = sf.read(wav_path)
        except Exception as e:
            return "CORRUPT", f"Cannot read audio data: {e}", None, None

        left = audio[:, 0]
        right = audio[:, 1]
        left_rms = np.sqrt(np.mean(left ** 2))
        right_rms = np.sqrt(np.mean(right ** 2))
        left_nonzero = np.count_nonzero(left)
        right_nonzero = np.count_nonzero(right)

        fname = os.path.basename(wav_path)
        if "_main" in fname:
            if right_nonzero > 0:
                return "FAIL", f"_main file has non-zero right channel ({right_nonzero} samples)", audio, sr
            if left_rms < 0.0001:
                return "FAIL", f"_main file has silent left channel (RMS={left_rms:.6f})", audio, sr
        elif "_other" in fname:
            if left_nonzero > 0:
                return "FAIL", f"_other file has non-zero left channel ({left_nonzero} samples)", audio, sr
            if right_rms < 0.0001:
                return "FAIL", f"_other file has silent right channel (RMS={right_rms:.6f})", audio, sr
        else:
            return "WARN", f"Unknown suffix (not _main or _other)", audio, sr

        if dur < 0.1:
            return "WARN", f"Very short: {dur:.3f}s", audio, sr

        return "OK", f"dur={dur:.2f}s L_RMS={left_rms:.4f} R_RMS={right_rms:.4f}", audio, sr
    else:
        if dur < 0.1:
            return "WARN", f"Very short: {dur:.3f}s", None, info.samplerate
        return "OK", f"dur={dur:.2f}s", None, info.samplerate


def check_json(json_path):
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
    timing_ok = True
    speakers = set()
    for item in aligns:
        if not isinstance(item, list) or len(item) < 3:
            return "FAIL", f"Malformed entry: {item}", [], False
        word = item[0]
        ts = item[1]
        speaker = item[2]
        if not isinstance(ts, list) or len(ts) < 2:
            return "FAIL", f"Malformed timestamp: {item}", [], False
        if ts[0] < 0 or ts[1] < ts[0]:
            timing_ok = False
        words.append(word)
        speakers.add(speaker)
        wl = word.lower()
        for kw in FACTS_KEYWORDS:
            if kw in wl:
                has_fact = True

    fname = os.path.basename(json_path)
    if "_main" in fname:
        if "SPEAKER_OTHER" in speakers and "SPEAKER_MAIN" not in speakers:
            return "FAIL", f"_main JSON has wrong speaker: {speakers}", words, has_fact
    elif "_other" in fname:
        if "SPEAKER_MAIN" in speakers and "SPEAKER_OTHER" not in speakers:
            return "FAIL", f"_other JSON has wrong speaker: {speakers}", words, has_fact

    return "OK", f"{len(words)} words, timing={'OK' if timing_ok else 'BAD'}, facts={'YES' if has_fact else 'NO'}, speakers={speakers}", words, has_fact


def extract_non_fact_text(words):
    clean = []
    in_fact = False
    prev_word = ""
    for w in words:
        if w == "[Injected" and not in_fact:
            in_fact = True
            prev_word = w
            continue
        if in_fact:
            if w == "reference]" and prev_word == "injected":
                in_fact = False
                prev_word = w
                continue
            prev_word = w
            continue
        clean.append(w)
        prev_word = w
    return " ".join(clean)


def verify_single_file(args):
    wav_path, i, wer_interval, full_check_interval = args
    json_path = wav_path.replace(".wav", ".json")
    fname = os.path.basename(wav_path)

    is_main = "_main" in fname
    is_other = "_other" in fname

    should_read_audio = (i % full_check_interval == 0) or (i % wer_interval == 0)
    status, msg, audio, sr = check_wav_format(wav_path, read_audio=should_read_audio)

    res = {
        "fname": fname,
        "is_main": is_main,
        "is_other": is_other,
        "status": status,
        "msg": msg,
        "duration": 0.0,
        "json_status": "OK",
        "json_msg": "",
        "has_fact": False,
        "wer": None,
        "gt_text": "",
        "whisper_text": ""
    }

    if status in ("CORRUPT", "FAIL"):
        return res

    dur_match = re.search(r"dur=([\d.]+)s", msg)
    if dur_match:
        res["duration"] = float(dur_match.group(1))

    if not os.path.exists(json_path):
        res["json_status"] = "FAIL"
        res["json_msg"] = "Missing JSON"
        return res

    j_status, j_msg, words, has_fact = check_json(json_path)
    res["json_status"] = j_status
    res["json_msg"] = j_msg
    res["has_fact"] = has_fact

    if j_status in ("CORRUPT", "FAIL"):
        return res

    if i % wer_interval == 0 and words and audio is not None:
        gt_text = extract_non_fact_text(words)
        gt_clean = re.sub(r"[^\w\s]", "", gt_text.lower()).strip()
        if len(gt_clean.split()) >= 2:
            if is_main:
                channel = audio[:, 0]
            elif is_other:
                channel = audio[:, 1]
            else:
                channel = np.mean(audio, axis=1)

            max_dur = min(30.0, len(channel) / sr)
            segment = channel[:int(max_dur * sr)]

            whisper_text = whisper_transcribe(segment, sr)
            if whisper_text:
                wh_clean = re.sub(r"[^\w\s]", "", whisper_text.lower()).strip()
                wer = compute_wer(gt_clean, wh_clean)
                res["wer"] = wer
                res["gt_text"] = gt_clean
                res["whisper_text"] = wh_clean

    return res


def verify_dataset(proc_dir):
    print("=" * 80)
    print(f"VERIFICATION: {proc_dir}")
    print("=" * 80)

    if not os.path.exists(proc_dir):
        print(f"  Directory does not exist, skipping.")
        return {}

    datasets = sorted([d for d in os.listdir(proc_dir)
                        if os.path.isdir(os.path.join(proc_dir, d))])
    print(f"Found datasets: {datasets}")

    summary = defaultdict(lambda: {
        "total": 0, "ok": 0, "fail": 0, "corrupt": 0, "warn": 0,
        "wer_samples": 0, "wer_sum": 0.0, "wer_pass": 0, "wer_fail": 0,
        "facts_count": 0, "total_duration": 0.0,
        "main_count": 0, "other_count": 0,
        "corrupt_files": [], "fail_files": [], "wer_fail_files": []
    })

    for ds in datasets:
        ds_dir = os.path.join(proc_dir, ds)
        wav_files = sorted(glob.glob(os.path.join(ds_dir, "*.wav")))
        print(f"\n{'=' * 60}")
        print(f"Dataset: {ds} ({len(wav_files)} WAV files)")
        print("=" * 60)

        s = summary[ds]
        s["total"] = len(wav_files)

        wer_interval = max(1, len(wav_files) // 20)
        full_check_interval = max(1, len(wav_files) // 100)

        tasks_args = [(wav_path, i, wer_interval, full_check_interval) for i, wav_path in enumerate(wav_files)]

        print(f"  Scheduling {len(wav_files)} files with ThreadPoolExecutor...")
        results = []
        with ThreadPoolExecutor(max_workers=16) as executor:
            total_tasks = len(tasks_args)
            chunk_size = 5000
            for idx in range(0, total_tasks, chunk_size):
                chunk = tasks_args[idx : idx + chunk_size]
                chunk_results = list(executor.map(verify_single_file, chunk))
                results.extend(chunk_results)
                print(f"    Processed {len(results)}/{total_tasks} files...")

        for res in results:
            fname = res["fname"]

            if res["is_main"]:
                s["main_count"] += 1
            elif res["is_other"]:
                s["other_count"] += 1

            if res["status"] == "CORRUPT":
                s["corrupt"] += 1
                s["corrupt_files"].append((fname, res["msg"]))
                continue
            elif res["status"] == "FAIL":
                s["fail"] += 1
                s["fail_files"].append((fname, res["msg"]))
                continue
            elif res["status"] == "WARN":
                s["warn"] += 1

            s["total_duration"] += res["duration"]

            if res["json_status"] in ("CORRUPT", "FAIL"):
                s["fail"] += 1
                s["fail_files"].append((fname, res["json_msg"]))
                continue

            if res["has_fact"]:
                s["facts_count"] += 1

            s["ok"] += 1

            if res["wer"] is not None:
                wer = res["wer"]
                s["wer_samples"] += 1
                s["wer_sum"] += wer

                if wer <= 0.5:
                    s["wer_pass"] += 1
                else:
                    s["wer_fail"] += 1
                    s["wer_fail_files"].append((fname, f"WER={wer:.2%}", res["gt_text"][:80], res["whisper_text"][:80]))

        avg_wer = s["wer_sum"] / s["wer_samples"] if s["wer_samples"] > 0 else 0
        facts_pct = (s["facts_count"] / s["ok"] * 100) if s["ok"] > 0 else 0
        print(f"  {ds}: OK={s['ok']} FAIL={s['fail']} CORRUPT={s['corrupt']} WARN={s['warn']}")
        print(f"  Main={s['main_count']} Other={s['other_count']}")
        print(f"  Duration: {s['total_duration']/3600:.2f}h, Facts: {s['facts_count']} ({facts_pct:.1f}%)")
        print(f"  WER: avg={avg_wer:.2%} ({s['wer_pass']} pass, {s['wer_fail']} fail / {s['wer_samples']} samples)")

    print("\n" + "=" * 80)
    print("FINAL SUMMARY")
    print("=" * 80)

    total_ok, total_fail, total_corrupt, total_facts = 0, 0, 0, 0
    total_dur, total_wer_sum, total_wer_samples = 0.0, 0.0, 0
    total_main, total_other = 0, 0

    for ds, s in sorted(summary.items()):
        avg_wer = s["wer_sum"] / s["wer_samples"] if s["wer_samples"] > 0 else 0
        facts_pct = (s["facts_count"] / s["ok"] * 100) if s["ok"] > 0 else 0
        print(f"  {ds:20s}: {s['total']:6d} files | OK={s['ok']:5d} FAIL={s['fail']:3d} CORRUPT={s['corrupt']:3d} | "
              f"main={s['main_count']:5d} other={s['other_count']:5d} | "
              f"dur={s['total_duration']/3600:.1f}h | facts={s['facts_count']:4d} ({facts_pct:.1f}%) | "
              f"WER={avg_wer:.2%} ({s['wer_samples']} samples)")
        total_ok += s["ok"]
        total_fail += s["fail"]
        total_corrupt += s["corrupt"]
        total_facts += s["facts_count"]
        total_dur += s["total_duration"]
        total_wer_samples += s["wer_samples"]
        total_wer_sum += s["wer_sum"]
        total_main += s["main_count"]
        total_other += s["other_count"]

    grand_wer = total_wer_sum / total_wer_samples if total_wer_samples > 0 else 0
    print(f"\n  {'TOTAL':20s}: {total_ok + total_fail + total_corrupt:6d} files | OK={total_ok:5d} FAIL={total_fail:3d} CORRUPT={total_corrupt:3d}")
    print(f"  Main files: {total_main}, Other files: {total_other}")
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
        for fname, reason in s["fail_files"][:30]:
            print(f"  [{ds}] {fname}: {reason}")
            any_issues = True
        if len(s["fail_files"]) > 30:
            print(f"  [{ds}] ... and {len(s['fail_files']) - 30} more")
    if not any_issues:
        print("  None")

    print("\n" + "=" * 80)
    print("HIGH WER FILES (WER > 50%)")
    print("=" * 80)
    any_issues = False
    for ds, s in sorted(summary.items()):
        for entry in s["wer_fail_files"][:15]:
            fname, wer_str, gt, hyp = entry
            print(f"  [{ds}] {fname}: {wer_str}")
            print(f"    GT:  {gt}")
            print(f"    HYP: {hyp}")
            any_issues = True
        if len(s["wer_fail_files"]) > 15:
            print(f"  [{ds}] ... and {len(s['wer_fail_files']) - 15} more high-WER files")
    if not any_issues:
        print("  None")

    report_path = os.path.join(proc_dir, "verification_report.json")
    report = {}
    for ds, s in summary.items():
        avg_wer = s["wer_sum"] / s["wer_samples"] if s["wer_samples"] > 0 else 0
        report[ds] = {
            "total": s["total"],
            "ok": s["ok"],
            "fail": s["fail"],
            "corrupt": s["corrupt"],
            "warn": s["warn"],
            "main_count": s["main_count"],
            "other_count": s["other_count"],
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
    if len(sys.argv) > 1:
        proc_dir = sys.argv[1]
    else:
        proc_dir = PROCESSED_DIR
    verify_dataset(proc_dir)


if __name__ == "__main__":
    main()
