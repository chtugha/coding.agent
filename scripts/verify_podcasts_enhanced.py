#!/usr/bin/env python3
"""
Enhanced podcast verification script with complete audio format validation.
Adds missing checks for:
- Audio format (48kHz, 16-bit, Stereo PCM)
- Channel muting verification (double mono)
- Improved German text handling in WER calculation
"""
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
TRANSCRIPT_DIR = "/Volumes/eHDD/moshi-rag-data/datasets/Gemischtes.Hack.Podcast/Gemischtes.Hack.Podcast.Transcript/transcripts"

WHISPER_CLI = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           "whisper-cpp", "build", "bin", "whisper-cli")
WHISPER_MODEL = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                             "bin", "models", "ggml-large-v3-q5_0.bin")

VERIFY_SUFFIX = "_wverify.json"
REPORT_NAME = "podcast_verification_report_enhanced.json"


def count_speaker_changes_from_transcript(episode_number):
    """
    Count speaker changes from the original episode transcript.
    Returns expected chunk count (speaker_changes + 1) or None if transcript not found.
    """
    # Find transcript file for this episode
    transcript_pattern = os.path.join(TRANSCRIPT_DIR, f"episode_{episode_number:03d}_*.json")
    transcript_files = glob.glob(transcript_pattern)
    
    if not transcript_files:
        return None
    
    transcript_path = transcript_files[0]
    
    try:
        with open(transcript_path, 'r', encoding='utf-8') as f:
            data = json.load(f)
        
        segments = data.get('segments', [])
        if not segments:
            return None
        
        speaker_changes = 0
        prev_speaker = None
        
        for seg in segments:
            curr_speaker = seg.get('speaker', '')
            if prev_speaker and curr_speaker != prev_speaker:
                speaker_changes += 1
            prev_speaker = curr_speaker
        
        # Expected chunks = speaker changes + 1 (for the first speaker's chunk)
        return speaker_changes + 1
    
    except Exception as e:
        log(f"    ERROR reading transcript {transcript_path}: {e}")
        return None


def verify_audio_format(wav_path):
    """Verify audio is 48kHz, 16-bit, Stereo PCM with proper channel muting."""
    issues = []
    
    try:
        info = sf.info(wav_path)
        
        # Check sample rate
        if info.samplerate != 48000:
            issues.append(f"Sample rate is {info.samplerate}Hz, expected 48000Hz")
        
        # Check channels
        if info.channels != 2:
            issues.append(f"Channels: {info.channels}, expected 2 (stereo)")
        
        # Check bit depth and format
        if info.subtype != 'PCM_16':
            issues.append(f"Format is {info.subtype}, expected PCM_16")
        
        # Check channel muting
        fname = os.path.basename(wav_path)
        is_main = "_main" in fname
        is_other = "_other" in fname
        
        if is_main or is_other:
            audio, sr = sf.read(wav_path)
            
            if audio.ndim != 2 or audio.shape[1] != 2:
                issues.append("Audio is not stereo despite having 2 channels in metadata")
            else:
                left_channel = audio[:, 0]
                right_channel = audio[:, 1]
                
                # Check if channels are truly muted (all zeros or near-zero)
                # Use a small threshold to account for floating point precision
                left_silent = np.allclose(left_channel, 0, atol=1e-6)
                right_silent = np.allclose(right_channel, 0, atol=1e-6)
                
                # Calculate RMS to detect very quiet but non-zero channels
                left_rms = np.sqrt(np.mean(left_channel**2))
                right_rms = np.sqrt(np.mean(right_channel**2))
                
                if is_main:
                    # For _main files: left should have audio, right should be silent
                    if right_rms > 1e-4:
                        issues.append(f"Right channel should be muted for _main files (RMS: {right_rms:.6f})")
                    if left_rms < 1e-4:
                        issues.append(f"Left channel is silent but should have audio for _main files (RMS: {left_rms:.6f})")
                elif is_other:
                    # For _other files: right should have audio, left should be silent
                    if left_rms > 1e-4:
                        issues.append(f"Left channel should be muted for _other files (RMS: {left_rms:.6f})")
                    if right_rms < 1e-4:
                        issues.append(f"Right channel is silent but should have audio for _other files (RMS: {right_rms:.6f})")
                
                # Verify double mono: active channels should be identical
                if is_main and not right_silent:
                    # Both channels have audio - check if they're identical
                    if not np.allclose(left_channel, right_channel, atol=1e-6):
                        issues.append("Channels are not identical (not double mono)")
                elif is_other and not left_silent:
                    # Both channels have audio - check if they're identical
                    if not np.allclose(left_channel, right_channel, atol=1e-6):
                        issues.append("Channels are not identical (not double mono)")
        
    except Exception as e:
        issues.append(f"Error reading audio file: {e}")
    
    return issues


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
                    if cleaned and word_start_ms is not None and word_end_ms is not None:
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
            if cleaned and word_start_ms is not None and word_end_ms is not None:
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
    """Extract ground truth text, skipping injected FACTS."""
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


def check_facts_injection(alignments, duration_seconds):
    """
    Check if FACTS are properly injected according to rules:
    - FACTS should only be in chunks > 60 seconds
    - FACTS should be marked with [Injected reference] ... [End of injected reference]
    
    Returns: (has_facts: bool, is_valid: bool, issue: str or None)
    """
    MIN_DURATION_FOR_FACTS = 60.0
    
    # Check if chunk has FACTS
    has_facts = False
    in_fact = False
    prev_word = ""
    
    for entry in alignments:
        w = entry[0]
        if w == "[Injected" and not in_fact:
            has_facts = True
            in_fact = True
            prev_word = w
            continue
        if in_fact:
            if w == "reference]" and prev_word.lower() == "injected":
                in_fact = False
            prev_word = w
            continue
        prev_word = w
    
    # Validate FACTS injection rules
    if has_facts and duration_seconds < MIN_DURATION_FOR_FACTS:
        return True, False, f"FACTS in short chunk ({duration_seconds:.1f}s < 60s)"
    elif not has_facts and duration_seconds >= MIN_DURATION_FOR_FACTS:
        # This is OK - FACTS injection is probabilistic (1% chance)
        return False, True, None
    elif has_facts and duration_seconds >= MIN_DURATION_FOR_FACTS:
        return True, True, None
    else:
        return False, True, None


def clean_for_wer(text):
    """Clean text for WER calculation, preserving German characters."""
    # Remove punctuation but keep German umlauts and ß
    return re.sub(r"[^\w\säöüßÄÖÜ]", "", text.lower()).strip()


def log(msg=""):
    print(msg, flush=True)


def verify_podcasts(podcast_dir, force=False, max_files=None):
    log("=" * 80)
    log("ENHANCED PODCAST VERIFICATION")
    log(f"Model: {WHISPER_MODEL}")
    log(f"Method: Active channel extraction -> 16kHz mono -> Whisper -> word alignments")
    log(f"Audio checks: 48kHz/16bit/Stereo PCM + channel muting verification")
    log(f"Metrics: WER + LCS ratio + containment score + issue classification")
    log("=" * 80)

    if not os.path.exists(podcast_dir):
        log(f"  Podcast directory does not exist: {podcast_dir}")
        return {}

    wav_files = sorted(glob.glob(os.path.join(podcast_dir, "*.wav")))
    
    if max_files:
        wav_files = wav_files[:max_files]
    
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
        "audio_format_issues": defaultdict(int),
        "files_with_format_issues": [],
        "chunk_count_mismatches": [],
        "facts_validation": {
            "total_checked": 0,
            "chunks_with_facts": 0,
            "chunks_over_60s": 0,
            "invalid_facts_injection": [],
            "facts_in_short_chunks": 0
        },
        "per_episode": defaultdict(lambda: {
            "wer_sum": 0.0, "wer_count": 0, "files": 0,
            "containment_sum": 0.0, "lcs_sum": 0.0,
            "issues": defaultdict(int),
            "format_issues": 0,
            "expected_chunks": None,
            "actual_chunks": 0,
            "chunk_count_match": None,
            "facts_checked": 0,
            "facts_found": 0,
            "facts_invalid": 0
        })
    }
    
    # Pre-calculate expected chunk counts per episode
    episode_expected_chunks = {}
    episode_numbers = set()
    for wav_path in wav_files:
        fname = os.path.basename(wav_path)
        ep_match = re.match(r"ep(\d+)_", fname)
        if ep_match:
            episode_numbers.add(int(ep_match.group(1)))
    
    log(f"\nCalculating expected chunk counts from original transcripts...")
    for ep_num in sorted(episode_numbers):
        expected = count_speaker_changes_from_transcript(ep_num)
        if expected:
            episode_expected_chunks[ep_num] = expected
            log(f"  Episode {ep_num}: Expected {expected} chunks (from speaker changes)")
        else:
            log(f"  Episode {ep_num}: Could not find transcript")
    log()

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

        ep_match = re.match(r"ep(\d+)_", fname)
        ep_label = f"ep{ep_match.group(1)}" if ep_match else "unknown"
        ep_num = int(ep_match.group(1)) if ep_match else None
        
        # Track actual chunk count per episode
        if ep_num:
            stats["per_episode"][ep_label]["actual_chunks"] += 1
            if stats["per_episode"][ep_label]["expected_chunks"] is None:
                stats["per_episode"][ep_label]["expected_chunks"] = episode_expected_chunks.get(ep_num)

        # ENHANCED: Verify audio format
        format_issues = verify_audio_format(wav_path)
        if format_issues:
            for issue in format_issues:
                stats["audio_format_issues"][issue] += 1
            stats["files_with_format_issues"].append({
                "file": fname,
                "issues": format_issues
            })
            stats["per_episode"][ep_label]["format_issues"] += 1

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
                # Write ONLY alignments to _wverify.json (to match chunk transcript format)
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
        
        # ENHANCED: Validate FACTS injection
        has_facts, facts_valid, facts_issue = check_facts_injection(gt_alignments, dur)
        stats["facts_validation"]["total_checked"] += 1
        if dur >= 60.0:
            stats["facts_validation"]["chunks_over_60s"] += 1
        if has_facts:
            stats["facts_validation"]["chunks_with_facts"] += 1
            stats["per_episode"][ep_label]["facts_found"] += 1
        if not facts_valid:
            stats["facts_validation"]["facts_in_short_chunks"] += 1
            stats["facts_validation"]["invalid_facts_injection"].append({
                "file": fname,
                "duration": round(dur, 2),
                "issue": facts_issue
            })
            stats["per_episode"][ep_label]["facts_invalid"] += 1
        stats["per_episode"][ep_label]["facts_checked"] += 1

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

        # Write WER analysis data to separate _analysis.json file
        analysis_path = wav_path.replace(".wav", "_analysis.json")
        analysis_data = {
            "file": fname,
            "wer": round(wer, 4),
            "containment": round(containment, 4),
            "lcs_ratio": round(lcs_ratio, 4),
            "issue": issue,
            "gt_text": gt_clean,
            "verify_text": verify_clean,
            "gt_word_count": len(gt_words),
            "verify_word_count": len(verify_words),
            "duration": round(dur, 2),
            "speaker": speaker,
            "episode": ep_label
        }
        try:
            with open(analysis_path, "w", encoding="utf-8") as f:
                json.dump(analysis_data, f, ensure_ascii=False, indent=2)
        except Exception as e:
            log(f"    WARN: Could not write analysis file {analysis_path}: {e}")

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
                "duration": round(dur, 2),
                "format_issues": format_issues
            })

        if (i + 1) % 10 == 0 or (i + 1) == len(wav_files):
            curr_avg_wer = stats["wer_sum"] / stats["wer_samples"] if stats["wer_samples"] > 0 else 0
            curr_avg_cont = stats["containment_sum"] / stats["wer_samples"] if stats["wer_samples"] > 0 else 0
            curr_avg_lcs = stats["lcs_sum"] / stats["wer_samples"] if stats["wer_samples"] > 0 else 0
            log(f"  [{i+1}/{len(wav_files)}] processed={stats['processed']} errors={stats['errors']} | "
                f"WER={curr_avg_wer:.2%} contain={curr_avg_cont:.2%} LCS={curr_avg_lcs:.2%} "
                f"(pass={stats['wer_pass']} fail={stats['wer_fail']}) | "
                f"format_issues={len(stats['files_with_format_issues'])}")

    avg_wer = stats["wer_sum"] / stats["wer_samples"] if stats["wer_samples"] > 0 else 0
    avg_contain = stats["containment_sum"] / stats["wer_samples"] if stats["wer_samples"] > 0 else 0
    avg_lcs = stats["lcs_sum"] / stats["wer_samples"] if stats["wer_samples"] > 0 else 0

    log(f"\n{'=' * 80}")
    log("ENHANCED PODCAST VERIFICATION SUMMARY")
    log("=" * 80)
    log(f"  Total files: {stats['total']}")
    log(f"  Processed: {stats['processed']}")
    log(f"  Errors: {stats['errors']}")
    log(f"  Main: {stats['main_count']}  Other: {stats['other_count']}")
    log(f"  Duration: {stats['total_duration']/3600:.2f}h")
    log(f"  WER avg: {avg_wer:.2%}")
    log(f"  Containment avg: {avg_contain:.2%}")
    log(f"  LCS ratio avg: {avg_lcs:.2%}")
    log(f"  WER pass (<=50%): {stats['wer_pass']}")
    log(f"  WER fail (>50%):  {stats['wer_fail']}")
    log(f"  WER samples: {stats['wer_samples']}")

    log(f"\n  Audio Format Issues:")
    log(f"  Files with format issues: {len(stats['files_with_format_issues'])}")
    if stats["audio_format_issues"]:
        for issue_type, count in sorted(stats["audio_format_issues"].items(), key=lambda x: -x[1]):
            log(f"    {issue_type}: {count}")
    else:
        log(f"    ✅ All files passed audio format validation!")

    log(f"\n  Issue Classification:")
    for issue, count in sorted(stats["issue_counts"].items(), key=lambda x: -x[1]):
        pct = count / stats["wer_samples"] * 100 if stats["wer_samples"] > 0 else 0
        log(f"    {issue:30s}: {count:5d} ({pct:5.1f}%)")

    log(f"\n  WER Distribution:")
    for bucket, count in stats["wer_distribution"].items():
        pct = count / stats["wer_samples"] * 100 if stats["wer_samples"] > 0 else 0
        bar = "#" * int(pct / 2)
        log(f"    {bucket:>8s}%: {count:5d} ({pct:5.1f}%) {bar}")
    
    # Check chunk counts per episode
    log(f"\n  Chunk Count Verification (Expected vs Actual):")
    chunk_mismatches = []
    for ep_label in sorted(stats["per_episode"].keys()):
        ep_stats = stats["per_episode"][ep_label]
        expected = ep_stats["expected_chunks"]
        actual = ep_stats["actual_chunks"]
        
        if expected is not None:
            match = expected == actual
            ep_stats["chunk_count_match"] = match
            status = "✅ MATCH" if match else "❌ MISMATCH"
            log(f"    {ep_label}: Expected {expected}, Actual {actual} - {status}")
            
            if not match:
                chunk_mismatches.append({
                    "episode": ep_label,
                    "expected": expected,
                    "actual": actual,
                    "difference": actual - expected
                })
        else:
            log(f"    {ep_label}: Expected ?, Actual {actual} - ⚠️ NO TRANSCRIPT")
    
    if chunk_mismatches:
        log(f"\n  ⚠️ WARNING: {len(chunk_mismatches)} episode(s) with chunk count mismatches!")
        log(f"  This may indicate issues with the preparation script's speaker change detection.")
        stats["chunk_count_mismatches"] = chunk_mismatches
    else:
        log(f"\n  ✅ All episodes have matching chunk counts!")
    
    # FACTS Injection Validation
    log(f"\n  FACTS Injection Validation:")
    facts_val = stats["facts_validation"]
    log(f"  Total chunks checked: {facts_val['total_checked']}")
    log(f"  Chunks over 60s: {facts_val['chunks_over_60s']}")
    log(f"  Chunks with FACTS: {facts_val['chunks_with_facts']}")
    
    if facts_val['chunks_over_60s'] > 0:
        facts_rate = (facts_val['chunks_with_facts'] / facts_val['chunks_over_60s']) * 100
        log(f"  FACTS injection rate: {facts_rate:.2f}% (expected ~1% due to probabilistic injection)")
    
    if facts_val['facts_in_short_chunks'] > 0:
        log(f"  ⚠️ WARNING: {facts_val['facts_in_short_chunks']} chunks with FACTS in short chunks (<60s)!")
        log(f"  Invalid FACTS injections (first 10):")
        for entry in facts_val['invalid_facts_injection'][:10]:
            log(f"    {entry['file']}: {entry['duration']}s - {entry['issue']}")
    else:
        log(f"  ✅ All FACTS properly injected (only in chunks >60s)")

    if stats["files_with_format_issues"]:
        log(f"\n  Files with Audio Format Issues (first 20):")
        for entry in stats["files_with_format_issues"][:20]:
            log(f"    {entry['file']}:")
            for issue in entry['issues']:
                log(f"      - {issue}")

    if stats["wer_fail_files"]:
        log(f"\n  High WER files (WER > 50%, showing first 20):")
        sorted_fails = sorted(stats["wer_fail_files"], key=lambda x: -x["wer"])
        for entry in sorted_fails[:20]:
            log(f"    {entry['file']}: WER={entry['wer']:.2%} contain={entry['containment']:.2%} "
                f"LCS={entry['lcs_ratio']:.2%} [{entry['issue']}]")
            if entry.get('format_issues'):
                log(f"      Format issues: {', '.join(entry['format_issues'])}")

    if stats["error_files"]:
        log(f"\n  Error files (first 20):")
        for fname, reason in stats["error_files"][:20]:
            log(f"    {fname}: {reason}")

    report = {
        "method": "Enhanced word-level whisper alignment with audio format validation",
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
        "audio_format_issues": dict(stats["audio_format_issues"]),
        "files_with_format_issues_count": len(stats["files_with_format_issues"]),
        "files_with_format_issues": stats["files_with_format_issues"][:100],
        "chunk_count_mismatches": stats.get("chunk_count_mismatches", []),
        "chunk_count_mismatches_count": len(stats.get("chunk_count_mismatches", [])),
        "facts_validation": {
            "total_checked": stats["facts_validation"]["total_checked"],
            "chunks_over_60s": stats["facts_validation"]["chunks_over_60s"],
            "chunks_with_facts": stats["facts_validation"]["chunks_with_facts"],
            "facts_in_short_chunks": stats["facts_validation"]["facts_in_short_chunks"],
            "facts_injection_rate": round(
                (stats["facts_validation"]["chunks_with_facts"] / stats["facts_validation"]["chunks_over_60s"] * 100)
                if stats["facts_validation"]["chunks_over_60s"] > 0 else 0, 2
            ),
            "invalid_facts_injection": stats["facts_validation"]["invalid_facts_injection"][:50]
        },
        "per_episode": {
            ep_name: {
                "avg_wer": round(s["wer_sum"] / s["wer_count"], 4) if s["wer_count"] > 0 else 0,
                "avg_containment": round(s["containment_sum"] / s["wer_count"], 4) if s["wer_count"] > 0 else 0,
                "avg_lcs_ratio": round(s["lcs_sum"] / s["wer_count"], 4) if s["wer_count"] > 0 else 0,
                "wer_count": s["wer_count"],
                "files": s["files"],
                "format_issues": s["format_issues"],
                "expected_chunks": s["expected_chunks"],
                "actual_chunks": s["actual_chunks"],
                "chunk_count_match": s["chunk_count_match"],
                "facts_checked": s["facts_checked"],
                "facts_found": s["facts_found"],
                "facts_invalid": s["facts_invalid"],
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
    force = "--force" in sys.argv
    podcast_dir = PODCAST_DIR
    max_files = None
    
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--force":
            pass
        elif args[i] == "--max-files" and i + 1 < len(args):
            i += 1
            max_files = int(args[i])
        elif os.path.isdir(args[i]):
            podcast_dir = args[i]
        i += 1
    
    verify_podcasts(podcast_dir, force=force, max_files=max_files)


if __name__ == "__main__":
    main()

# Made with Bob
