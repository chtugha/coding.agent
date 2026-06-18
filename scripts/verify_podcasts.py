#!/usr/bin/env python3
import os
import sys
import glob
import json
import re
import subprocess
import tempfile
import numpy as np
import soundfile as sf
import librosa
from collections import defaultdict
from difflib import SequenceMatcher

PROCESSED_DIR = "/Volumes/eHDD/moshi-rag-data/processed"
PODCAST_DIR = os.path.join(PROCESSED_DIR, "podcast")

WHISPER_CLI = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           "whisper-cpp", "build", "bin", "whisper-cli")
WHISPER_MODEL = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                             "bin", "models", "ggml-large-v3-q5_0.bin")

VERIFY_SUFFIX = "_wverify.json"
REPORT_NAME = "podcast_verification_report.json"


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


def compute_containment(gt_words, verify_words):
    if not gt_words:
        return 1.0
    sm = SequenceMatcher(None, gt_words, verify_words, autojunk=False)
    matching = sum(block.size for block in sm.get_matching_blocks())
    return matching / len(gt_words)


def compute_lcs_ratio(gt_words, verify_words):
    if not gt_words:
        return 1.0 if not verify_words else 0.0
    m, n = len(gt_words), len(verify_words)
    if m > 500 or n > 500:
        sm = SequenceMatcher(None, gt_words, verify_words, autojunk=False)
        lcs_len = sum(block.size for block in sm.get_matching_blocks())
        return lcs_len / m
    prev = [0] * (n + 1)
    curr = [0] * (n + 1)
    for i in range(1, m + 1):
        for j in range(1, n + 1):
            if gt_words[i - 1] == verify_words[j - 1]:
                curr[j] = prev[j - 1] + 1
            else:
                curr[j] = max(prev[j], curr[j - 1])
        prev, curr = curr, [0] * (n + 1)
    return prev[n] / m


def classify_issue(gt_words, verify_words, containment, lcs_ratio, wer):
    if not verify_words:
        return "SILENT"
    if len(gt_words) <= 3 and len(verify_words) > len(gt_words) * 2:
        return "SHORT_CHUNK_OVERFLOW"
    if containment >= 0.8 and lcs_ratio >= 0.7:
        return "MINOR_VARIATION"
    if containment < 0.3 and lcs_ratio < 0.3:
        return "CONTENT_MISMATCH"
    if containment >= 0.5 and wer > 0.5:
        return "EXTRA_WORDS"
    if containment < 0.5 and lcs_ratio >= 0.4:
        return "PARTIAL_OVERLAP"
    return "SIGNIFICANT_DIVERGENCE"


def tokens_to_word_alignments(whisper_json, speaker):
    segments = whisper_json.get("transcription", [])
    alignments = []
    for seg in segments:
        tokens = seg.get("tokens", [])
        current_word = ""
        word_start_ms = None
        word_end_ms = None
        for tok in tokens:
            text = tok.get("text", "")
            if text.startswith("[_") and text.endswith("]"):
                continue
            from_ms = tok["offsets"]["from"]
            to_ms = tok["offsets"]["to"]
            if text.startswith(" ") or current_word == "":
                if current_word:
                    cleaned = re.sub(r"[^\w\däöüßÄÖÜ\s-]", "", current_word).strip()
                    if cleaned and word_start_ms is not None:
                        alignments.append([
                            cleaned,
                            [round(word_start_ms / 1000.0, 6), round(word_end_ms / 1000.0, 6)],
                            speaker
                        ])
                current_word = text.lstrip()
                word_start_ms = from_ms
                word_end_ms = to_ms
            else:
                current_word += text
                word_end_ms = to_ms
        if current_word:
            cleaned = re.sub(r"[^\w\däöüßÄÖÜ\s-]", "", current_word).strip()
            if cleaned and word_start_ms is not None:
                alignments.append([
                    cleaned,
                    [round(word_start_ms / 1000.0, 6), round(word_end_ms / 1000.0, 6)],
                    speaker
                ])
    return alignments


def whisper_transcribe_to_alignments(wav_path, speaker, timeout=120):
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

    with tempfile.TemporaryDirectory() as tmpdir:
        output_base = os.path.join(tmpdir, "whisper_out")
        cmd = [
            WHISPER_CLI, "-m", WHISPER_MODEL, "-l", "de",
            "--beam-size", "1", "--best-of", "1",
            "-ojf", "-of", output_base,
            "-f", tmp_path
        ]
        try:
            subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           text=True, timeout=timeout)
            actual_json = output_base + ".json"
            if os.path.exists(actual_json):
                with open(actual_json, "r", encoding="utf-8") as f:
                    whisper_data = json.load(f)
                return tokens_to_word_alignments(whisper_data, speaker)
            return None
        except Exception as e:
            log(f"    WHISPER ERROR on {fname}: {e}")
            return None
        finally:
            if os.path.exists(tmp_path):
                os.unlink(tmp_path)


def extract_gt_text(alignments):
    in_fact = False
    words = []
    prev_word = ""
    for entry in alignments:
        w = entry[0]
        if w == "[Injected" and not in_fact:
            in_fact = True
            prev_word = w
            continue
        if in_fact:
            if w == "reference]" and prev_word.lower() == "injected":
                in_fact = False
            prev_word = w
            continue
        words.append(w)
        prev_word = w
    return " ".join(words)


def clean_for_wer(text):
    return re.sub(r"[^\w\s]", "", text.lower()).strip()


def log(msg=""):
    print(msg, flush=True)


def verify_podcasts(podcast_dir, force=False):
    log("=" * 80)
    log("PODCAST VERIFICATION (word-level whisper alignment method)")
    log(f"Model: {WHISPER_MODEL}")
    log(f"Method: active-channel -> 16kHz mono -> beam=5, best-of=5 -> token-level timestamps -> word merge")
    log(f"Metrics: WER + LCS ratio + containment score + issue classification")
    log("=" * 80)

    if not os.path.exists(podcast_dir):
        log(f"  Podcast directory does not exist: {podcast_dir}")
        return {}

    wav_files = sorted(glob.glob(os.path.join(podcast_dir, "*.wav")))
    log(f"Found {len(wav_files)} podcast WAV files")

    stats = {
        "total": len(wav_files),
        "processed": 0, "skipped": 0, "errors": 0,
        "wer_samples": 0, "wer_sum": 0.0, "wer_pass": 0, "wer_fail": 0,
        "containment_sum": 0.0, "lcs_sum": 0.0,
        "main_count": 0, "other_count": 0,
        "total_duration": 0.0,
        "wer_fail_files": [], "error_files": [],
        "issue_counts": defaultdict(int),
        "wer_distribution": {"0-10": 0, "10-20": 0, "20-30": 0, "30-50": 0, "50-75": 0, "75-100": 0, "100+": 0},
        "per_episode": defaultdict(lambda: {
            "wer_sum": 0.0, "wer_count": 0, "files": 0,
            "containment_sum": 0.0, "lcs_sum": 0.0,
            "issues": defaultdict(int)
        })
    }

    for i, wav_path in enumerate(wav_files):
        fname = os.path.basename(wav_path)
        json_path = wav_path.replace(".wav", ".json")
        verify_path = wav_path.replace(".wav", VERIFY_SUFFIX)

        is_main = "_main" in fname
        is_other = "_other" in fname
        speaker = "SPEAKER_MAIN" if is_main else "SPEAKER_OTHER"

        if is_main:
            stats["main_count"] += 1
        elif is_other:
            stats["other_count"] += 1

        ep_match = re.match(r"(ep\d+)_", fname)
        ep_label = ep_match.group(1) if ep_match else "unknown"

        try:
            info = sf.info(wav_path)
            dur = info.duration
            stats["total_duration"] += dur
        except Exception:
            dur = 0.0

        if not os.path.exists(json_path):
            stats["errors"] += 1
            stats["error_files"].append((fname, "Missing transcript JSON"))
            continue

        try:
            with open(json_path, "r", encoding="utf-8") as f:
                gt_data = json.load(f)
        except Exception as e:
            stats["errors"] += 1
            stats["error_files"].append((fname, f"Cannot parse transcript JSON: {e}"))
            continue

        gt_alignments = gt_data.get("alignments", [])
        if not gt_alignments:
            stats["errors"] += 1
            stats["error_files"].append((fname, "Empty alignments in transcript"))
            continue

        if os.path.exists(verify_path) and not force:
            try:
                with open(verify_path, "r", encoding="utf-8") as f:
                    verify_data = json.load(f)
                verify_alignments = verify_data.get("alignments", [])
            except Exception:
                verify_alignments = None
        else:
            verify_alignments = None

        if verify_alignments is None:
            verify_alignments = whisper_transcribe_to_alignments(wav_path, speaker)
            if verify_alignments is not None:
                verify_data = {"alignments": verify_alignments}
                try:
                    with open(verify_path, "w", encoding="utf-8") as f:
                        json.dump(verify_data, f, ensure_ascii=False)
                except Exception as e:
                    log(f"    WARN: Could not write verify file {verify_path}: {e}")

        if verify_alignments is None:
            stats["errors"] += 1
            stats["error_files"].append((fname, "Whisper transcription failed"))
            continue

        stats["processed"] += 1

        gt_text = extract_gt_text(gt_alignments)
        gt_clean = clean_for_wer(gt_text)
        gt_words = gt_clean.split()

        verify_text = " ".join([a[0] for a in verify_alignments])
        verify_clean = clean_for_wer(verify_text)
        verify_words = verify_clean.split()

        if len(gt_words) < 2:
            continue

        wer = compute_wer(gt_clean, verify_clean)
        containment = compute_containment(gt_words, verify_words)
        lcs_ratio = compute_lcs_ratio(gt_words, verify_words)
        issue = classify_issue(gt_words, verify_words, containment, lcs_ratio, wer)

        stats["wer_samples"] += 1
        stats["wer_sum"] += wer
        stats["containment_sum"] += containment
        stats["lcs_sum"] += lcs_ratio
        stats["issue_counts"][issue] += 1

        ep = stats["per_episode"][ep_label]
        ep["wer_sum"] += wer
        ep["wer_count"] += 1
        ep["files"] += 1
        ep["containment_sum"] += containment
        ep["lcs_sum"] += lcs_ratio
        ep["issues"][issue] += 1

        wer_pct = wer * 100
        if wer_pct <= 10:
            stats["wer_distribution"]["0-10"] += 1
        elif wer_pct <= 20:
            stats["wer_distribution"]["10-20"] += 1
        elif wer_pct <= 30:
            stats["wer_distribution"]["20-30"] += 1
        elif wer_pct <= 50:
            stats["wer_distribution"]["30-50"] += 1
        elif wer_pct <= 75:
            stats["wer_distribution"]["50-75"] += 1
        elif wer_pct <= 100:
            stats["wer_distribution"]["75-100"] += 1
        else:
            stats["wer_distribution"]["100+"] += 1

        if wer <= 0.5:
            stats["wer_pass"] += 1
        else:
            stats["wer_fail"] += 1
            stats["wer_fail_files"].append({
                "file": fname,
                "wer": round(wer, 4),
                "containment": round(containment, 4),
                "lcs_ratio": round(lcs_ratio, 4),
                "issue": issue,
                "gt_text": gt_clean[:120],
                "verify_text": verify_clean[:120],
                "gt_words": len(gt_words),
                "verify_words": len(verify_words),
                "duration": round(dur, 2)
            })

        if (i + 1) % 50 == 0 or (i + 1) == len(wav_files):
            curr_avg_wer = stats["wer_sum"] / stats["wer_samples"] if stats["wer_samples"] > 0 else 0
            curr_avg_cont = stats["containment_sum"] / stats["wer_samples"] if stats["wer_samples"] > 0 else 0
            curr_avg_lcs = stats["lcs_sum"] / stats["wer_samples"] if stats["wer_samples"] > 0 else 0
            log(f"  [{i+1}/{len(wav_files)}] processed={stats['processed']} errors={stats['errors']} | "
                f"WER={curr_avg_wer:.2%} contain={curr_avg_cont:.2%} LCS={curr_avg_lcs:.2%} "
                f"(pass={stats['wer_pass']} fail={stats['wer_fail']})")

    avg_wer = stats["wer_sum"] / stats["wer_samples"] if stats["wer_samples"] > 0 else 0
    avg_contain = stats["containment_sum"] / stats["wer_samples"] if stats["wer_samples"] > 0 else 0
    avg_lcs = stats["lcs_sum"] / stats["wer_samples"] if stats["wer_samples"] > 0 else 0

    log(f"\n{'=' * 80}")
    log("PODCAST VERIFICATION SUMMARY")
    log("=" * 80)
    log(f"  Total files: {stats['total']}")
    log(f"  Processed: {stats['processed']}")
    log(f"  Errors: {stats['errors']}")
    log(f"  Main: {stats['main_count']}  Other: {stats['other_count']}")
    log(f"  Duration: {stats['total_duration']/3600:.2f}h")
    log(f"  WER avg: {avg_wer:.2%}")
    log(f"  Containment avg: {avg_contain:.2%}  (fraction of GT words found in verify, in order)")
    log(f"  LCS ratio avg: {avg_lcs:.2%}  (longest common subsequence / GT length)")
    log(f"  WER pass (<=50%): {stats['wer_pass']}")
    log(f"  WER fail (>50%):  {stats['wer_fail']}")
    log(f"  WER samples: {stats['wer_samples']}")

    log(f"\n  Issue Classification:")
    for issue, count in sorted(stats["issue_counts"].items(), key=lambda x: -x[1]):
        pct = count / stats["wer_samples"] * 100 if stats["wer_samples"] > 0 else 0
        log(f"    {issue:30s}: {count:5d} ({pct:5.1f}%)")

    log(f"\n  WER Distribution:")
    for bucket, count in stats["wer_distribution"].items():
        pct = count / stats["wer_samples"] * 100 if stats["wer_samples"] > 0 else 0
        bar = "#" * int(pct / 2)
        log(f"    {bucket:>8s}%: {count:5d} ({pct:5.1f}%) {bar}")

    log(f"\n  Per-Episode (worst 20 by WER):")
    ep_wers = []
    for ep_name, ep_stats in stats["per_episode"].items():
        if ep_stats["wer_count"] > 0:
            ep_avg_wer = ep_stats["wer_sum"] / ep_stats["wer_count"]
            ep_avg_cont = ep_stats["containment_sum"] / ep_stats["wer_count"]
            ep_avg_lcs = ep_stats["lcs_sum"] / ep_stats["wer_count"]
            top_issue = max(ep_stats["issues"].items(), key=lambda x: x[1])[0] if ep_stats["issues"] else "NONE"
            ep_wers.append((ep_name, ep_avg_wer, ep_avg_cont, ep_avg_lcs, ep_stats["files"], ep_stats["wer_count"], top_issue, dict(ep_stats["issues"])))
    ep_wers.sort(key=lambda x: -x[1])
    for ep_name, ep_avg_wer, ep_avg_cont, ep_avg_lcs, ep_files, ep_count, top_issue, issues in ep_wers[:20]:
        log(f"    {ep_name:>8s}: WER={ep_avg_wer:.2%} contain={ep_avg_cont:.2%} LCS={ep_avg_lcs:.2%} "
            f"({ep_count} samples) top_issue={top_issue}")

    if stats["wer_fail_files"]:
        log(f"\n  High WER files (WER > 50%, showing first 30):")
        sorted_fails = sorted(stats["wer_fail_files"], key=lambda x: -x["wer"])
        for entry in sorted_fails[:30]:
            log(f"    {entry['file']}: WER={entry['wer']:.2%} contain={entry['containment']:.2%} "
                f"LCS={entry['lcs_ratio']:.2%} [{entry['issue']}]")
            log(f"      GT ({entry['gt_words']}w):  {entry['gt_text']}")
            log(f"      VER({entry['verify_words']}w): {entry['verify_text']}")

    content_mismatch_by_ep = defaultdict(int)
    for entry in stats["wer_fail_files"]:
        if entry["issue"] == "CONTENT_MISMATCH":
            m = re.match(r"(ep\d+)", entry["file"])
            if m:
                content_mismatch_by_ep[m.group(1)] += 1
    if content_mismatch_by_ep:
        log(f"\n  CONTENT_MISMATCH count by episode (audio-transcript misalignment):")
        for ep_name, count in sorted(content_mismatch_by_ep.items(), key=lambda x: -x[1])[:20]:
            log(f"    {ep_name}: {count} files")

    if stats["error_files"]:
        log(f"\n  Error files (first 20):")
        for fname, reason in stats["error_files"][:20]:
            log(f"    {fname}: {reason}")

    report = {
        "method": "word-level whisper alignment with multi-metric analysis",
        "model": WHISPER_MODEL,
        "total": stats["total"],
        "processed": stats["processed"],
        "errors": stats["errors"],
        "main_count": stats["main_count"],
        "other_count": stats["other_count"],
        "duration_hours": round(stats["total_duration"] / 3600, 2),
        "avg_wer": round(avg_wer, 4),
        "avg_containment": round(avg_contain, 4),
        "avg_lcs_ratio": round(avg_lcs, 4),
        "wer_samples": stats["wer_samples"],
        "wer_pass": stats["wer_pass"],
        "wer_fail": stats["wer_fail"],
        "issue_counts": dict(stats["issue_counts"]),
        "wer_distribution": stats["wer_distribution"],
        "per_episode": {
            ep_name: {
                "avg_wer": round(s["wer_sum"] / s["wer_count"], 4) if s["wer_count"] > 0 else 0,
                "avg_containment": round(s["containment_sum"] / s["wer_count"], 4) if s["wer_count"] > 0 else 0,
                "avg_lcs_ratio": round(s["lcs_sum"] / s["wer_count"], 4) if s["wer_count"] > 0 else 0,
                "wer_count": s["wer_count"],
                "files": s["files"],
                "issues": dict(s["issues"])
            }
            for ep_name, s in stats["per_episode"].items()
        },
        "wer_fail_files": stats["wer_fail_files"][:200],
        "error_files": stats["error_files"][:50]
    }

    report_path = os.path.join(podcast_dir, REPORT_NAME)
    with open(report_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    log(f"\nReport saved to: {report_path}")

    return report


def main():
    import random
    force = "--force" in sys.argv
    podcast_dir = PODCAST_DIR
    sample_per_ep = 0
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--force":
            pass
        elif args[i] == "--sample" and i + 1 < len(args):
            i += 1
            sample_per_ep = int(args[i])
        elif os.path.isdir(args[i]):
            podcast_dir = args[i]
        i += 1
    verify_podcasts(podcast_dir, force=force)


if __name__ == "__main__":
    main()
