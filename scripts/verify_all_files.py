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
import librosa
from collections import defaultdict

PROCESSED_DIR = "/Volumes/eHDD/moshi-rag-data/processed"
WHISPER_CLI = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           "whisper-cpp", "build", "bin", "whisper-cli")
WHISPER_MODEL = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                             "bin", "models", "ggml-large-v3-q5_0.bin")

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


def whisper_transcribe_to_json(wav_path, verify_json_path, timeout=600):
    fname = os.path.basename(wav_path)
    is_main = "_main" in fname

    audio, sr = sf.read(wav_path)
    if audio.ndim == 2 and audio.shape[1] == 2:
        active_channel = audio[:, 0] if is_main else audio[:, 1]
    else:
        active_channel = audio if audio.ndim == 1 else audio[:, 0]

    active_16k = librosa.resample(active_channel.astype(np.float32), orig_sr=sr, target_sr=16000)

    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False, dir="/tmp") as tmp:
        tmp_path = tmp.name
        sf.write(tmp_path, active_16k, 16000, subtype="PCM_16")

    output_base = verify_json_path.replace(".json", "")
    cmd = [
        WHISPER_CLI, "-m", WHISPER_MODEL, "-l", "de",
        "--beam-size", "5", "--best-of", "5",
        "-ojf", "-of", output_base,
        "-f", tmp_path
    ]
    try:
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, timeout=timeout)
        actual_json = output_base + ".json"
        if os.path.exists(actual_json):
            with open(actual_json, "r", encoding="utf-8") as f:
                data = json.load(f)
            return data
        return None
    except Exception as e:
        log(f"    WHISPER ERROR on {fname}: {e}")
        return None
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)


def extract_text_from_whisper_json(whisper_data):
    if not whisper_data or "transcription" not in whisper_data:
        return ""
    segments = whisper_data["transcription"]
    full_text = ""
    for seg in segments:
        text = seg.get("text", "").strip()
        full_text += " " + text
    return full_text.strip()


def check_wav_format(wav_path):
    try:
        info = sf.info(wav_path)
    except Exception as e:
        return "CORRUPT", f"Cannot read: {e}"

    if info.channels != 2:
        return "FAIL", f"Not stereo: channels={info.channels}"

    if info.samplerate != 48000:
        return "FAIL", f"Wrong sample rate: {info.samplerate} (expected 48000)"

    dur = info.duration
    if dur < 0.1:
        return "WARN", f"Very short: {dur:.3f}s"
    return "OK", f"dur={dur:.2f}s"


def check_wav_channels(wav_path):
    fname = os.path.basename(wav_path)
    try:
        audio, sr = sf.read(wav_path)
    except Exception as e:
        return "CORRUPT", f"Cannot read audio data: {e}"

    left = audio[:, 0]
    right = audio[:, 1]
    left_rms = np.sqrt(np.mean(left ** 2))
    right_rms = np.sqrt(np.mean(right ** 2))
    left_nonzero = np.count_nonzero(left)
    right_nonzero = np.count_nonzero(right)

    if "_main" in fname:
        if right_nonzero > 0:
            return "FAIL", f"_main file has non-zero right channel ({right_nonzero} samples)"
        if left_rms < 0.0001:
            return "FAIL", f"_main file has silent left channel (RMS={left_rms:.6f})"
    elif "_other" in fname:
        if left_nonzero > 0:
            return "FAIL", f"_other file has non-zero left channel ({left_nonzero} samples)"
        if right_rms < 0.0001:
            return "FAIL", f"_other file has silent right channel (RMS={right_rms:.6f})"
    else:
        return "WARN", f"Unknown suffix (not _main or _other)"

    return "OK", f"L_RMS={left_rms:.4f} R_RMS={right_rms:.4f}"


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


def log(msg=""):
    print(msg, flush=True)


def verify_dataset(proc_dir):
    log("=" * 80)
    log(f"VERIFICATION: {proc_dir}")
    log(f"Model: {WHISPER_MODEL}")
    log(f"Method: active-channel extraction -> 16kHz mono -> beam=5, best-of=5 -> full JSON")
    log("=" * 80)

    if not os.path.exists(proc_dir):
        log(f"  Directory does not exist, skipping.")
        return {}

    datasets = sorted([d for d in os.listdir(proc_dir)
                        if os.path.isdir(os.path.join(proc_dir, d))])
    log(f"Found datasets: {datasets}")

    summary = defaultdict(lambda: {
        "total": 0, "ok": 0, "fail": 0, "corrupt": 0, "warn": 0,
        "wer_samples": 0, "wer_sum": 0.0, "wer_pass": 0, "wer_fail": 0,
        "facts_count": 0, "total_duration": 0.0,
        "main_count": 0, "other_count": 0,
        "channel_checked": 0, "channel_fail": 0,
        "corrupt_files": [], "fail_files": [], "wer_fail_files": []
    })

    for ds in datasets:
        ds_dir = os.path.join(proc_dir, ds)
        wav_files = sorted(glob.glob(os.path.join(ds_dir, "*.wav")))
        log(f"\n{'=' * 60}")
        log(f"Dataset: {ds} ({len(wav_files)} WAV files)")
        log("=" * 60)

        s = summary[ds]
        s["total"] = len(wav_files)

        channel_check_interval = max(1, len(wav_files) // 100)

        for i, wav_path in enumerate(wav_files):
            json_path = wav_path.replace(".wav", ".json")
            verify_json_path = wav_path.replace(".wav", "_verify.json")
            fname = os.path.basename(wav_path)

            is_main = "_main" in fname
            is_other = "_other" in fname

            if is_main:
                s["main_count"] += 1
            elif is_other:
                s["other_count"] += 1

            status, msg = check_wav_format(wav_path)
            if status == "CORRUPT":
                s["corrupt"] += 1
                s["corrupt_files"].append((fname, msg))
                continue
            elif status == "FAIL":
                s["fail"] += 1
                s["fail_files"].append((fname, msg))
                continue
            elif status == "WARN":
                s["warn"] += 1

            dur_match = re.search(r"dur=([\d.]+)s", msg)
            dur = float(dur_match.group(1)) if dur_match else 0.0
            s["total_duration"] += dur

            if i % channel_check_interval == 0:
                ch_status, ch_msg = check_wav_channels(wav_path)
                s["channel_checked"] += 1
                if ch_status in ("CORRUPT", "FAIL"):
                    s["channel_fail"] += 1
                    s["fail"] += 1
                    s["fail_files"].append((fname, ch_msg))
                    continue

            if not os.path.exists(json_path):
                s["fail"] += 1
                s["fail_files"].append((fname, "Missing JSON"))
                continue

            j_status, j_msg, words, has_fact = check_json(json_path)
            if j_status in ("CORRUPT", "FAIL"):
                s["fail"] += 1
                s["fail_files"].append((fname, j_msg))
                continue

            if has_fact:
                s["facts_count"] += 1

            s["ok"] += 1

            if os.path.exists(verify_json_path):
                try:
                    with open(verify_json_path, "r", encoding="utf-8") as f:
                        whisper_data = json.load(f)
                except Exception:
                    whisper_data = None
            else:
                whisper_data = whisper_transcribe_to_json(wav_path, verify_json_path)

            if whisper_data and words:
                gt_text = extract_non_fact_text(words)
                gt_clean = re.sub(r"[^\w\s]", "", gt_text.lower()).strip()
                whisper_full_text = extract_text_from_whisper_json(whisper_data)
                wh_clean = re.sub(r"[^\w\s]", "", whisper_full_text.lower()).strip()
                if len(gt_clean.split()) >= 2:
                    wer = compute_wer(gt_clean, wh_clean)
                    s["wer_samples"] += 1
                    s["wer_sum"] += wer
                    if wer <= 0.5:
                        s["wer_pass"] += 1
                    else:
                        s["wer_fail"] += 1
                        s["wer_fail_files"].append((fname, f"WER={wer:.2%}", gt_clean[:80], wh_clean[:80]))

            if (i + 1) % 500 == 0 or (i + 1) == len(wav_files):
                curr_avg = s["wer_sum"] / s["wer_samples"] if s["wer_samples"] > 0 else 0
                log(f"    [{ds}] {i+1}/{len(wav_files)} files | "
                    f"OK={s['ok']} FAIL={s['fail']} | "
                    f"WER avg={curr_avg:.2%} ({s['wer_samples']} samples, {s['wer_pass']} pass, {s['wer_fail']} fail)")

        avg_wer = s["wer_sum"] / s["wer_samples"] if s["wer_samples"] > 0 else 0
        facts_pct = (s["facts_count"] / s["ok"] * 100) if s["ok"] > 0 else 0
        log(f"\n  {ds} SUMMARY:")
        log(f"    OK={s['ok']} FAIL={s['fail']} CORRUPT={s['corrupt']} WARN={s['warn']}")
        log(f"    Main={s['main_count']} Other={s['other_count']}")
        log(f"    Duration: {s['total_duration']/3600:.2f}h, Facts: {s['facts_count']} ({facts_pct:.1f}%)")
        log(f"    Channel checks: {s['channel_checked']} done, {s['channel_fail']} failed")
        log(f"    WER: avg={avg_wer:.2%} ({s['wer_pass']} pass, {s['wer_fail']} fail / {s['wer_samples']} samples)")

    log("\n" + "=" * 80)
    log("FINAL SUMMARY")
    log("=" * 80)

    total_ok, total_fail, total_corrupt, total_facts = 0, 0, 0, 0
    total_dur, total_wer_sum, total_wer_samples = 0.0, 0.0, 0
    total_main, total_other = 0, 0

    for ds, s in sorted(summary.items()):
        avg_wer = s["wer_sum"] / s["wer_samples"] if s["wer_samples"] > 0 else 0
        facts_pct = (s["facts_count"] / s["ok"] * 100) if s["ok"] > 0 else 0
        log(f"  {ds:20s}: {s['total']:6d} files | OK={s['ok']:5d} FAIL={s['fail']:3d} CORRUPT={s['corrupt']:3d} | "
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
    log(f"\n  {'TOTAL':20s}: {total_ok + total_fail + total_corrupt:6d} files | OK={total_ok:5d} FAIL={total_fail:3d} CORRUPT={total_corrupt:3d}")
    log(f"  Main files: {total_main}, Other files: {total_other}")
    log(f"  Total duration: {total_dur/3600:.2f} hours")
    log(f"  Total facts injections: {total_facts}")
    log(f"  Grand avg WER: {grand_wer:.2%} ({total_wer_samples} samples)")

    log("\n" + "=" * 80)
    log("CORRUPT FILES")
    log("=" * 80)
    any_issues = False
    for ds, s in sorted(summary.items()):
        for fname, reason in s["corrupt_files"]:
            log(f"  [{ds}] {fname}: {reason}")
            any_issues = True
    if not any_issues:
        log("  None")

    log("\n" + "=" * 80)
    log("FAILED FILES")
    log("=" * 80)
    any_issues = False
    for ds, s in sorted(summary.items()):
        for fname, reason in s["fail_files"][:30]:
            log(f"  [{ds}] {fname}: {reason}")
            any_issues = True
        if len(s["fail_files"]) > 30:
            log(f"  [{ds}] ... and {len(s['fail_files']) - 30} more")
    if not any_issues:
        log("  None")

    log("\n" + "=" * 80)
    log("HIGH WER FILES (WER > 50%)")
    log("=" * 80)
    any_issues = False
    for ds, s in sorted(summary.items()):
        for entry in s["wer_fail_files"][:20]:
            fname, wer_str, gt, hyp = entry
            log(f"  [{ds}] {fname}: {wer_str}")
            log(f"    GT:  {gt}")
            log(f"    HYP: {hyp}")
            any_issues = True
        if len(s["wer_fail_files"]) > 20:
            log(f"  [{ds}] ... and {len(s['wer_fail_files']) - 20} more high-WER files")
    if not any_issues:
        log("  None")

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
    log(f"\nDetailed report saved to: {report_path}")
    return report


def main():
    if len(sys.argv) > 1:
        proc_dir = sys.argv[1]
    else:
        proc_dir = PROCESSED_DIR
    verify_dataset(proc_dir)


if __name__ == "__main__":
    main()
