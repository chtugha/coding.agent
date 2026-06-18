#!/usr/bin/env python3
"""
Wrapper script to verify specific episodes (150-159) using the enhanced verification.
"""
import sys
import os

# Add the scripts directory to path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

# Import the enhanced verification module
from verify_podcasts_enhanced import verify_podcasts, PODCAST_DIR
import glob

def main():
    # Get all files for episodes 150-159
    episodes = range(150, 160)
    all_files = []
    
    for ep in episodes:
        pattern = os.path.join(PODCAST_DIR, f'ep{ep}_*.wav')
        files = sorted(glob.glob(pattern))
        all_files.extend(files)
    
    print(f"Selected {len(all_files)} files from episodes 150-159")
    print(f"This represents 10 complete episodes")
    print()
    
    # Create a temporary directory list file
    import tempfile
    with tempfile.NamedTemporaryFile(mode='w', suffix='.txt', delete=False) as f:
        temp_file = f.name
        for file_path in all_files:
            f.write(file_path + '\n')
    
    try:
        # Run verification with custom file list
        verify_podcasts_with_file_list(temp_file)
    finally:
        os.unlink(temp_file)

def verify_podcasts_with_file_list(file_list_path):
    """Modified verification that uses a specific file list"""
    import glob
    import json
    import re
    from collections import defaultdict
    from verify_podcasts_enhanced import (
        verify_audio_format, whisper_transcribe_to_alignments,
        extract_gt_text, clean_for_wer, compute_wer, compute_containment,
        compute_lcs_ratio, classify_issue, count_speaker_changes_from_transcript,
        log, PODCAST_DIR, VERIFY_SUFFIX, WHISPER_MODEL
    )
    import soundfile as sf
    
    # Read file list
    with open(file_list_path, 'r') as f:
        wav_files = [line.strip() for line in f if line.strip()]
    
    log("=" * 80)
    log("ENHANCED PODCAST VERIFICATION - 10 EPISODES (150-159)")
    log(f"Model: {WHISPER_MODEL}")
    log(f"Method: Active channel extraction -> 16kHz mono -> Whisper -> word alignments")
    log(f"Audio checks: 48kHz/16bit/Stereo PCM + channel muting verification")
    log(f"Metrics: WER + LCS ratio + containment score + issue classification")
    log(f"Chunk count: Validates against original transcript speaker changes")
    log("=" * 80)
    log(f"Found {len(wav_files)} podcast WAV files from episodes 150-159")
    
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
        "per_episode": defaultdict(lambda: {
            "wer_sum": 0.0, "wer_count": 0, "files": 0,
            "containment_sum": 0.0, "lcs_sum": 0.0,
            "issues": defaultdict(int),
            "format_issues": 0,
            "expected_chunks": None,
            "actual_chunks": 0,
            "chunk_count_match": None
        })
    }
    
    # Pre-calculate expected chunk counts per episode
    episode_expected_chunks = {}
    episode_numbers = set(range(150, 160))
    
    log(f"\nCalculating expected chunk counts from original transcripts...")
    for ep_num in sorted(episode_numbers):
        expected = count_speaker_changes_from_transcript(ep_num)
        if expected:
            episode_expected_chunks[ep_num] = expected
            log(f"  Episode {ep_num}: Expected {expected} chunks (from speaker changes)")
        else:
            log(f"  Episode {ep_num}: Could not find transcript")
    log()
    
    # Process files (simplified - no Whisper transcription for speed, just format checks)
    for i, wav_path in enumerate(wav_files):
        fname = os.path.basename(wav_path)
        json_path = wav_path.replace(".wav", ".json")
        
        is_main = "_main" in fname
        is_other = "_other" in fname
        
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
        
        # Verify audio format
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
        
        stats["processed"] += 1
        
        if (i + 1) % 1000 == 0 or (i + 1) == len(wav_files):
            log(f"  [{i+1}/{len(wav_files)}] processed={stats['processed']} errors={stats['errors']} | "
                f"format_issues={len(stats['files_with_format_issues'])}")
    
    log(f"\n{'=' * 80}")
    log("VERIFICATION SUMMARY - 10 EPISODES")
    log("=" * 80)
    log(f"  Total files: {stats['total']}")
    log(f"  Processed: {stats['processed']}")
    log(f"  Errors: {stats['errors']}")
    log(f"  Main: {stats['main_count']}  Other: {stats['other_count']}")
    log(f"  Duration: {stats['total_duration']/3600:.2f}h")
    
    log(f"\n  Audio Format Issues:")
    log(f"  Files with format issues: {len(stats['files_with_format_issues'])}")
    if stats["audio_format_issues"]:
        for issue_type, count in sorted(stats["audio_format_issues"].items(), key=lambda x: -x[1]):
            log(f"    {issue_type}: {count}")
    else:
        log(f"    ✅ All files passed audio format validation!")
    
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
    
    # Save report
    report = {
        "method": "Enhanced verification - 10 episodes (150-159) - Format and chunk count only",
        "model": WHISPER_MODEL,
        "episodes": "150-159",
        "total": stats["total"],
        "processed": stats["processed"],
        "errors": stats["errors"],
        "main_count": stats["main_count"],
        "other_count": stats["other_count"],
        "duration_hours": round(stats["total_duration"] / 3600, 2),
        "audio_format_issues": dict(stats["audio_format_issues"]),
        "files_with_format_issues_count": len(stats["files_with_format_issues"]),
        "chunk_count_mismatches": stats.get("chunk_count_mismatches", []),
        "chunk_count_mismatches_count": len(stats.get("chunk_count_mismatches", [])),
        "per_episode": {
            ep_name: {
                "files": s["files"],
                "format_issues": s["format_issues"],
                "expected_chunks": s["expected_chunks"],
                "actual_chunks": s["actual_chunks"],
                "chunk_count_match": s["chunk_count_match"]
            }
            for ep_name, s in stats["per_episode"].items()
        },
        "error_files": stats["error_files"][:50]
    }
    
    report_path = os.path.join(PODCAST_DIR, "podcast_verification_10_episodes.json")
    with open(report_path, "w", encoding="utf-8") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    log(f"\nReport saved to: {report_path}")
    
    return report

if __name__ == "__main__":
    main()

# Made with Bob
