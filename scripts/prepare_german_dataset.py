#!/usr/bin/env python3
import os
import re
import sys
import xml.etree.ElementTree as ET
import glob
import json
import traceback
import soundfile as sf
import numpy as np
import librosa
from scipy.ndimage import gaussian_filter1d
from scipy.signal import correlate, resample

RAW_DIR = "/Volumes/eHDD/moshi-rag-data/datasets"
PROCESSED_DIR = "/Volumes/eHDD/moshi-rag-data/processed"
CACHE_PATH = "/Volumes/eHDD/moshi-rag-data/datasets/podcast_alignment_cache.json"
TARGET_SR = 48000
FACT_INJECT_PROB = 0.01
MIN_DUR_FOR_FACTS = 60.0
MIN_CHUNK_DUR = 0.1


FACTS = [
    "Der Schwarzschild-Radius beschreibt die Grenze, ab der keine Information dem Schwarzen Loch entkommen kann.",
    "Quantenverschränkung bewirkt, dass zwei Teilchen über beliebige Distanzen instantan korreliert bleiben.",
    "Die Hawking-Strahlung resultiert aus Quantenvakuumfluktuationen nahe des Ereignishorizonts.",
    "Gravitationswellen stauchen und strecken die Raumzeit bei asymmetrischen Sternkollisionen.",
    "Superposition bedeutet, dass sich ein Quantensystem gleichzeitig in mehreren Zuständen befindet.",
    "Die Schrödinger-Gleichung beschreibt die zeitliche Entwicklung einer quantenmechanischen Wellenfunktion.",
    "Dunkle Materie wechselwirkt scheinbar nur über die Gravitation mit normaler Materie.",
    "Ein stabiler Orbit erfordert die exakte Balance zwischen Zentrifugalkraft und Gravitationsanziehung.",
    "Der Spin eines Elektrons kann unter Beobachtung in einen eindeutigen Zustand kollabieren.",
    "Im Doppelspaltexperiment interferiert ein einzelnes Teilchen physikalisch mit sich selbst.",
    "Heisenbergs Unschärferelation verbietet die gleichzeitige Messung von Ort und Impuls.",
    "Die Krümmung der Raumzeit bestimmt die Bewegung von massereichen Himmelskörpern."
]

WHISPER_CLI = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                           "whisper-cpp", "build", "bin", "whisper-cli")
WHISPER_MODEL = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                             "bin", "models", "ggml-large-v3-turbo-q5_0.bin")

error_log = []


def log_error(dataset, file_id, msg):
    entry = f"[{dataset}] {file_id}: {msg}"
    error_log.append(entry)
    print(f"  ERROR: {entry}", file=sys.stderr)


def parse_exb(exb_path):
    try:
        tree = ET.parse(exb_path)
        root = tree.getroot()
        tli_to_time = {}
        for tli in root.findall(".//common-timeline/tli"):
            tli_id = tli.attrib.get("id")
            if tli_id and "time" in tli.attrib:
                tli_to_time[tli_id] = float(tli.attrib["time"])
        spk0_words, spk1_words = [], []
        for tier in root.findall(".//tier"):
            speaker = tier.attrib.get("speaker")
            category = tier.attrib.get("category", "")
            if "norm" not in category:
                continue
            for event in tier.findall("event"):
                start_id = event.attrib.get("start")
                end_id = event.attrib.get("end")
                text = (event.text or "").strip()
                if not text or start_id not in tli_to_time or end_id not in tli_to_time:
                    continue
                start_t = tli_to_time[start_id]
                end_t = tli_to_time[end_id]
                if end_t <= start_t:
                    continue
                if speaker == "SPK0":
                    spk0_words.append((text, start_t, end_t))
                elif speaker == "SPK1":
                    spk1_words.append((text, start_t, end_t))
        spk0_words.sort(key=lambda x: x[1])
        spk1_words.sort(key=lambda x: x[1])
        return spk0_words, spk1_words
    except Exception:
        traceback.print_exc()
        return [], []


def parse_gcsc_txt(txt_path):
    words = []
    try:
        with open(txt_path, "r", encoding="utf-8") as f:
            for line in f:
                parts = line.strip().split("\t")
                if len(parts) < 4:
                    continue
                time_str, text = parts[0], parts[3]
                m = re.match(r"\[([\d\.]+),\s*([\d\.]+)\]", time_str)
                if not m:
                    continue
                start_t = float(m.group(1))
                end_t = float(m.group(2))
                if text.startswith("[") and text.endswith("]"):
                    continue
                raw_words = text.strip().split()
                if not raw_words:
                    continue
                seg_dur = end_t - start_t
                if seg_dur <= 0:
                    continue
                word_dur = seg_dur / len(raw_words)
                for idx, w in enumerate(raw_words):
                    w_start = start_t + idx * word_dur
                    w_end = start_t + (idx + 1) * word_dur
                    words.append((w, w_start, w_end))
        words.sort(key=lambda x: x[1])
        return words
    except Exception:
        traceback.print_exc()
        return []


def parse_cha(cha_path):
    spk0_words, spk1_words = [], []
    try:
        speaker_map = {}
        with open(cha_path, "r", encoding="utf-8") as f:
            for line in f:
                m_spk = re.match(r"\*(\w+):", line)
                if not m_spk:
                    continue
                tag = m_spk.group(1)
                if tag not in speaker_map:
                    speaker_map[tag] = len(speaker_map)
                spk_idx = speaker_map[tag]
                m = re.search(r"\x15(\d+)_(\d+)\x15", line)
                if not m:
                    continue
                start_t = float(m.group(1)) / 1000.0
                end_t = float(m.group(2)) / 1000.0
                text_part = re.sub(r"\x15.*?\x15", "", line)
                text_part = re.sub(r"\*\w+:\s*", "", text_part)
                text_part = re.sub(r"<|>", "", text_part)
                text_part = re.sub(r"\[.*?\]", "", text_part)
                text_part = text_part.strip()
                raw_words = text_part.split()
                if not raw_words:
                    continue
                seg_dur = end_t - start_t
                if seg_dur <= 0:
                    continue
                word_dur = seg_dur / len(raw_words)
                for idx, w in enumerate(raw_words):
                    w_start = start_t + idx * word_dur
                    w_end = start_t + (idx + 1) * word_dur
                    w_clean = re.sub(r"[^\w\däöüßÄÖÜ\s-]", "", w)
                    if w_clean:
                        if spk_idx % 2 == 0:
                            spk0_words.append((w_clean, w_start, w_end))
                        else:
                            spk1_words.append((w_clean, w_start, w_end))
        spk0_words.sort(key=lambda x: x[1])
        spk1_words.sort(key=lambda x: x[1])
        return spk0_words, spk1_words
    except Exception:
        traceback.print_exc()
        return [], []


def parse_podcast_json(json_path):
    spk0_words, spk1_words = [], []
    try:
        with open(json_path, "r", encoding="utf-8") as f:
            data = json.load(f)
        segments = data.get("segments", [])
        speaker_map = {}
        for seg in segments:
            start_t = float(seg["start"])
            end_t = float(seg["end"])
            text = seg.get("text", "").strip()
            speaker = seg.get("speaker", "")
            if not speaker or not text:
                continue
            if speaker not in speaker_map:
                speaker_map[speaker] = len(speaker_map)
            raw_words = text.split()
            if not raw_words:
                continue
            seg_dur = end_t - start_t
            if seg_dur <= 0:
                continue
            word_dur = seg_dur / len(raw_words)
            for idx, w in enumerate(raw_words):
                w_start = start_t + idx * word_dur
                w_end = start_t + (idx + 1) * word_dur
                w_clean = re.sub(r"[^\w\däöüßÄÖÜ\s-]", "", w)
                if w_clean:
                    if speaker_map[speaker] == 0:
                        spk0_words.append((w_clean, w_start, w_end))
                    else:
                        spk1_words.append((w_clean, w_start, w_end))
        spk0_words.sort(key=lambda x: x[1])
        spk1_words.sort(key=lambda x: x[1])
        return spk0_words, spk1_words
    except Exception:
        traceback.print_exc()
        return [], []


def parse_podcast_turns(json_path):
    try:
        with open(json_path, "r", encoding="utf-8") as f:
            data = json.load(f)
        segments = data.get("segments", [])
        speaker_map = {}
        turns = []
        for seg in segments:
            start_t = float(seg["start"])
            end_t = float(seg["end"])
            text = seg.get("text", "").strip()
            speaker = seg.get("speaker", "")
            if not speaker or not text:
                continue
            if speaker not in speaker_map:
                speaker_map[speaker] = len(speaker_map)
            spk_label = "SPEAKER_MAIN" if speaker_map[speaker] == 0 else "SPEAKER_OTHER"
            
            # Use original word-level timestamps from JSON if available
            words = []
            word_list = seg.get("words", [])
            if word_list:
                # Original JSON has word-level timestamps - use them directly
                for word_info in word_list:
                    word_text = word_info.get("word", "").strip()
                    w_start = float(word_info.get("start", start_t))
                    w_end = float(word_info.get("end", start_t))
                    w_clean = re.sub(r"[^\w\däöüßÄÖÜ\s-]", "", word_text)
                    if w_clean and w_end > w_start:
                        words.append((w_clean, w_start, w_end))
            else:
                # Fallback: if no word-level timestamps, use segment-level only
                # (This should not happen for podcast data)
                raw_words = text.split()
                if raw_words:
                    seg_dur = end_t - start_t
                    if seg_dur > 0:
                        word_dur = seg_dur / len(raw_words)
                        for idx, w in enumerate(raw_words):
                            w_start = start_t + idx * word_dur
                            w_end = start_t + (idx + 1) * word_dur
                            w_clean = re.sub(r"[^\w\däöüßÄÖÜ\s-]", "", w)
                            if w_clean:
                                words.append((w_clean, w_start, w_end))
            
            if not words:
                continue
            if turns and turns[-1][0] == spk_label:
                prev = turns[-1]
                turns[-1] = (prev[0], prev[1], end_t, prev[3] + words)
            else:
                turns.append((spk_label, start_t, end_t, words))
        return turns
    except Exception:
        traceback.print_exc()
        return []


def parse_medical_json(json_path):
    spk0_words, spk1_words = [], []
    try:
        with open(json_path, "r", encoding="utf-8") as f:
            data = json.load(f)
        for entry in data.get("alignments", []):
            word = entry[0]
            start_t, end_t = entry[1]
            speaker = entry[2]
            if speaker == "SPEAKER_MAIN":
                spk0_words.append((word, start_t, end_t))
            elif speaker == "SPEAKER_OTHER":
                spk1_words.append((word, start_t, end_t))
        spk0_words.sort(key=lambda x: x[1])
        spk1_words.sort(key=lambda x: x[1])
        return spk0_words, spk1_words
    except Exception:
        traceback.print_exc()
        return [], []


def _whisper_transcribe_audio(audio_mono_16k, timeout=600):
    import subprocess
    import tempfile
    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        tmp_path = tmp.name
        sf.write(tmp_path, audio_mono_16k, 16000)
    try:
        cmd = [WHISPER_CLI, "-m", WHISPER_MODEL, "-l", "de", "-f", tmp_path]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                             text=True, timeout=timeout)
        lines = res.stdout.strip().split("\n")
    except Exception:
        return []
    finally:
        os.unlink(tmp_path)
    segments = []
    for line in lines:
        m = re.match(r"\[(\d{2}):(\d{2}):(\d{2})\.(\d+) --> (\d{2}):(\d{2}):(\d{2})\.(\d+)\]\s*(.*)", line)
        if not m:
            continue
        seg_start = int(m.group(1))*3600 + int(m.group(2))*60 + int(m.group(3)) + int(m.group(4))/1000.0
        seg_end = int(m.group(5))*3600 + int(m.group(6))*60 + int(m.group(7)) + int(m.group(8))/1000.0
        seg_text = m.group(9).strip()
        if seg_text:
            segments.append((seg_start, seg_end, seg_text))
    return segments


def _segments_to_words(segments):
    words = []
    for seg_start, seg_end, text in segments:
        ws = re.sub(r"[^\w\s]", "", text.lower()).split()
        if not ws:
            continue
        dur = max(seg_end - seg_start, 0.01)
        for i, w in enumerate(ws):
            t = seg_start + dur * i / len(ws)
            words.append((w, t))
    return words


def generate_transcript_waveform(transcript_segments, total_duration, sr=48000):
    """
    Generate a simulated waveform from transcript timestamps.
    Speech regions = 1.0, silence = 0.0
    """
    samples = int(total_duration * sr)
    waveform = np.zeros(samples, dtype=np.float32)
    
    for seg in transcript_segments:
        start_sample = int(seg['start'] * sr)
        end_sample = int(seg['end'] * sr)
        end_sample = min(end_sample, samples)
        if end_sample > start_sample:
            waveform[start_sample:end_sample] = 1.0
    
    # Smooth with gaussian filter to simulate energy envelope
    waveform = gaussian_filter1d(waveform, sigma=sr*0.05)  # 50ms smoothing
    
    return waveform


def extract_audio_envelope(audio, sr, window_size=0.1):
    """
    Extract energy envelope from audio waveform.
    Increased window size for faster processing.
    """
    window_samples = int(window_size * sr)
    hop = window_samples  # Use full window as hop for speed
    
    energy = []
    for i in range(0, len(audio) - window_samples, hop):
        window = audio[i:i+window_samples]
        rms = np.sqrt(np.mean(window**2))
        energy.append(rms)
    
    energy = np.array(energy)
    
    # Normalize
    if energy.max() > 0:
        energy = energy / energy.max()
    
    return energy


def find_nearest_silence(audio, sr, target_time, search_backward=True, window=2.0, threshold=0.01):
    """
    Find the nearest silence region to target_time.
    Searches within ±window seconds.
    """
    target_sample = int(target_time * sr)
    search_samples = int(window * sr)
    
    if search_backward:
        start = max(0, target_sample - search_samples)
        end = target_sample
        search_range = range(end, start, -int(0.01 * sr))
    else:
        start = target_sample
        end = min(len(audio), target_sample + search_samples)
        search_range = range(start, end, int(0.01 * sr))
    
    silence_window = int(0.05 * sr)
    
    for i in search_range:
        if i + silence_window > len(audio):
            continue
        window_audio = audio[i:i+silence_window]
        rms = np.sqrt(np.mean(window_audio**2))
        
        if rms < threshold:
            return i / sr
    
    return target_time


def detect_ad_breaks_from_correlation(audio_env, transcript_env, offset, sr, hop):
    """
    Detect ad breaks by finding regions where audio has energy but transcript doesn't.
    """
    offset_samples = int(offset * sr / hop)
    
    # Align envelopes
    if offset_samples >= 0:
        audio_aligned = audio_env[offset_samples:]
        transcript_aligned = transcript_env[:len(audio_aligned)]
    else:
        transcript_aligned = transcript_env[-offset_samples:]
        audio_aligned = audio_env[:len(transcript_aligned)]
    
    min_len = min(len(audio_aligned), len(transcript_aligned))
    audio_aligned = audio_aligned[:min_len]
    transcript_aligned = transcript_aligned[:min_len]
    
    # Detect mismatches
    threshold = 0.3
    mismatch = (audio_aligned > threshold) & (transcript_aligned < threshold)
    
    # Find contiguous regions
    ad_breaks = []
    in_ad = False
    ad_start = 0
    
    for i, is_mismatch in enumerate(mismatch):
        if is_mismatch and not in_ad:
            ad_start = i
            in_ad = True
        elif not is_mismatch and in_ad:
            if i - ad_start > 20:
                start_time = (ad_start + offset_samples) * hop / sr
                end_time = (i + offset_samples) * hop / sr
                ad_breaks.append((start_time, end_time))
            in_ad = False
    
    return ad_breaks


def find_alignment_with_cross_correlation(audio, sr, transcript_segments):
    """
    Find precise alignment between audio and transcript using cross-correlation.
    Returns: (offset_seconds, correlation_score, ad_breaks)
    """
    print(f"      [DEBUG] Starting cross-correlation alignment...")
    total_dur = len(audio) / sr
    print(f"      [DEBUG] Audio duration: {total_dur:.1f}s, {len(audio)} samples")
    
    # Downsample audio for faster processing (10Hz is sufficient for alignment)
    target_rate = 10  # Hz
    downsample_factor = int(sr / target_rate)
    audio_downsampled = audio[::downsample_factor]
    sr_down = sr / downsample_factor
    print(f"      [DEBUG] Downsampled audio to {sr_down:.1f}Hz: {len(audio_downsampled)} samples")
    
    # Generate transcript waveform at lower rate
    print(f"      [DEBUG] Generating transcript waveform from {len(transcript_segments)} segments...")
    transcript_wf = generate_transcript_waveform(transcript_segments, total_dur, int(sr_down))
    print(f"      [DEBUG] Transcript waveform: {len(transcript_wf)} samples")
    
    # Extract audio envelope
    print(f"      [DEBUG] Extracting audio envelope...")
    audio_envelope = extract_audio_envelope(audio_downsampled, int(sr_down))
    print(f"      [DEBUG] Audio envelope: {len(audio_envelope)} points")
    
    # Downsample transcript waveform to match envelope
    print(f"      [DEBUG] Resampling transcript waveform...")
    transcript_envelope = resample(transcript_wf, len(audio_envelope))
    print(f"      [DEBUG] Transcript envelope: {len(transcript_envelope)} points")
    
    # Cross-correlate to find best alignment
    print(f"      [DEBUG] Computing cross-correlation...")
    correlation = correlate(audio_envelope, transcript_envelope, mode='valid')
    print(f"      [DEBUG] Cross-correlation complete: {len(correlation)} points")
    
    # Find peak correlation
    best_offset_samples = np.argmax(correlation)
    audio_norm = np.linalg.norm(np.asarray(audio_envelope))
    transcript_norm = np.linalg.norm(np.asarray(transcript_envelope))
    best_correlation = float(correlation[best_offset_samples]) / (audio_norm * transcript_norm) if (audio_norm * transcript_norm) > 0 else 0.0
    
    # Convert to seconds (account for downsampling and envelope extraction)
    hop = int(0.1 * sr_down)  # Window size used in envelope extraction
    offset_seconds = best_offset_samples * hop / sr_down
    
    # Detect ad breaks
    print(f"      [DEBUG] Detecting ad breaks...")
    ad_breaks = detect_ad_breaks_from_correlation(
        audio_envelope,
        transcript_envelope,
        offset_seconds,
        int(sr_down),
        hop
    )
    print(f"      [DEBUG] Found {len(ad_breaks)} ad breaks")
    
    return offset_seconds, best_correlation, ad_breaks


def clean_podcast_ads_waveform_based(mono_audio, sr, transcript_json_path, ep_label=""):
    """
    Clean podcast ads using waveform pattern matching.
    This is more precise than text-based matching.
    """
    # Check cache first
    if os.path.exists(CACHE_PATH):
        try:
            with open(CACHE_PATH, "r", encoding="utf-8") as f:
                cache = json.load(f)
            if ep_label in cache:
                entry = cache[ep_label]
                initial_offset = entry.get("offset", 0.0)
                ad_breaks = entry.get("ad_breaks", [])
                print(f"    {ep_label}: loaded from cache (offset={initial_offset:.1f}s, {len(ad_breaks)} ads)")
                
                content_start = max(0.0, initial_offset)
                total_dur = len(mono_audio) / float(sr)
                ad_regions = [(s, e) for s, e in sorted(ad_breaks)]
                regions = _build_clean_regions(content_start, ad_regions, total_dur)
                if not regions:
                    return mono_audio[int(content_start * sr):], initial_offset, ad_regions
                chunks = []
                for rs, re_ in regions:
                    s1 = int(rs * sr)
                    s2 = min(int(re_ * sr), len(mono_audio))
                    if s2 > s1:
                        chunks.append(mono_audio[s1:s2])
                if chunks:
                    cleaned = np.concatenate(chunks)
                else:
                    cleaned = mono_audio[int(content_start * sr):]
                print(f"    {ep_label}: cleaned {total_dur:.0f}s -> {len(cleaned)/sr:.0f}s")
                return cleaned, initial_offset, ad_regions
        except Exception as ce:
            print(f"    {ep_label}: error loading cache: {ce}")
    
    # Load transcript
    with open(transcript_json_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    
    segments = data.get("segments", [])
    if not segments:
        return mono_audio, 0.0, []
    
    print(f"    {ep_label}: analyzing waveform patterns...")
    print(f"    {ep_label}: audio length={len(mono_audio)/sr:.1f}s, segments={len(segments)}")
    
    # Find alignment using cross-correlation
    print(f"    {ep_label}: starting cross-correlation (may take 30-60s for long episodes)...")
    offset, correlation, ad_breaks = find_alignment_with_cross_correlation(
        mono_audio, sr, segments
    )
    
    print(f"    {ep_label}: correlation={correlation:.3f}, offset={offset:.1f}s, {len(ad_breaks)} ads detected")
    
    # Refine ad break boundaries to silence regions
    ad_breaks_refined = []
    for ad_start, ad_end in ad_breaks:
        refined_start = find_nearest_silence(mono_audio, sr, ad_start, search_backward=True)
        refined_end = find_nearest_silence(mono_audio, sr, ad_end, search_backward=False)
        
        if refined_end > refined_start:
            ad_breaks_refined.append((refined_start, refined_end))
            print(f"    {ep_label}: ad break {refined_start:.1f}s - {refined_end:.1f}s")
    
    # Build clean regions
    content_start = max(0.0, offset)
    total_dur = len(mono_audio) / float(sr)
    regions = _build_clean_regions(content_start, ad_breaks_refined, total_dur)
    
    # Extract clean audio
    if not regions:
        return mono_audio[int(content_start * sr):], offset, ad_breaks_refined
    
    chunks = []
    for rs, re in regions:
        s1 = int(rs * sr)
        s2 = min(int(re * sr), len(mono_audio))
        if s2 > s1:
            chunks.append(mono_audio[s1:s2])
    
    if chunks:
        cleaned = np.concatenate(chunks)
    else:
        cleaned = mono_audio[int(content_start * sr):]
    
    print(f"    {ep_label}: cleaned {total_dur:.0f}s -> {len(cleaned)/sr:.0f}s")
    
    # Cache results
    if ad_breaks_refined:
        try:
            if os.path.exists(CACHE_PATH):
                with open(CACHE_PATH, "r", encoding="utf-8") as f:
                    cache = json.load(f)
            else:
                cache = {}
            cache[ep_label] = {"offset": offset, "ad_breaks": ad_breaks_refined}
            with open(CACHE_PATH, "w", encoding="utf-8") as f:
                json.dump(cache, f, ensure_ascii=False, indent=2)
        except Exception:
            pass
    
    return cleaned, offset, ad_breaks_refined



def _build_clean_regions(content_start, ad_breaks, total_dur):
    regions = []
    cursor = content_start
    for ad_s, ad_e in ad_breaks:
        if cursor < ad_s:
            regions.append((cursor, ad_s))
        cursor = ad_e
    if cursor < total_dur:
        regions.append((cursor, total_dur))
    return regions


def clean_podcast_ads(mono_audio, sr, transcript_json_path, ep_label=""):
    if os.path.exists(CACHE_PATH):
        try:
            with open(CACHE_PATH, "r", encoding="utf-8") as f:
                cache = json.load(f)
            if ep_label in cache:
                entry = cache[ep_label]
                initial_offset = entry.get("offset", 0.0)
                ad_breaks = entry.get("ad_breaks", [])
                print(f"    {ep_label}: loaded from cache (offset={initial_offset:.1f}s, {len(ad_breaks)} ads)")

                content_start = max(0.0, initial_offset)
                total_dur = len(mono_audio) / float(sr)
                ad_regions = [(s, e) for s, e in sorted(ad_breaks)]
                regions = _build_clean_regions(content_start, ad_regions, total_dur)
                if not regions:
                    return mono_audio[int(content_start * sr):], initial_offset, ad_regions
                chunks = []
                for rs, re_ in regions:
                    s1 = int(rs * sr)
                    s2 = min(int(re_ * sr), len(mono_audio))
                    if s2 > s1:
                        chunks.append(mono_audio[s1:s2])
                if chunks:
                    cleaned = np.concatenate(chunks)
                else:
                    cleaned = mono_audio[int(content_start * sr):]
                print(f"    {ep_label}: cleaned {total_dur:.0f}s -> {len(cleaned)/sr:.0f}s "
                      f"(removed {len(ad_regions)} ads, offset={initial_offset:.1f}s)")
                return cleaned, initial_offset, ad_regions
        except Exception as ce:
            print(f"    {ep_label}: error loading cache: {ce}")

    from difflib import SequenceMatcher
    with open(transcript_json_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    segments = [s for s in data.get("segments", []) if s.get("text", "").strip()]
    if not segments:
        return mono_audio, 0.0, []
    t_segs = [(float(s["start"]), float(s["end"]), s["text"].strip()) for s in segments]
    t_words = _segments_to_words(t_segs)
    if len(t_words) < 10:
        return mono_audio, 0.0, []
    if sr != 16000:
        mono_16k = librosa.resample(mono_audio.astype(np.float32), orig_sr=sr, target_sr=16000)
    else:
        mono_16k = mono_audio.astype(np.float32)
    print(f"    {ep_label}: whisper-transcribing full audio ({len(mono_audio)/sr:.0f}s)...")
    w_segs = _whisper_transcribe_audio(mono_16k, timeout=900)
    if not w_segs:
        print(f"    {ep_label}: whisper failed, returning original audio")
        return mono_audio, 0.0, []
    w_words = _segments_to_words(w_segs)
    if not w_words:
        return mono_audio, 0.0, []
    t_seq = [w for w, _ in t_words]
    w_seq = [w for w, _ in w_words]
    sm = SequenceMatcher(None, t_seq, w_seq, autojunk=False)
    matching_blocks = sm.get_matching_blocks()
    anchors = []
    for t_start_idx, w_start_idx, size in matching_blocks:
        if size < 3:
            continue
        mid = size // 2
        t_time = t_words[t_start_idx + mid][1]
        w_time = w_words[w_start_idx + mid][1]
        anchors.append((t_time, w_time))
    if len(anchors) < 2:
        print(f"    {ep_label}: too few anchor points ({len(anchors)}), returning original audio")
        return mono_audio, 0.0, []
    offsets = [(t, a - t) for t, a in anchors]
    initial_offset = offsets[0][1]
    ad_breaks = []
    for i in range(1, len(offsets)):
        offset_jump = offsets[i][1] - offsets[i - 1][1]
        if offset_jump > 10:
            t_gap = anchors[i][0] - anchors[i - 1][0]
            ad_audio_start = anchors[i - 1][1] + t_gap
            ad_audio_end = anchors[i][1]
            if ad_audio_end > ad_audio_start:
                ad_breaks.append((ad_audio_start, ad_audio_end))
                print(f"    {ep_label}: mid-roll ad at audio {ad_audio_start:.1f}s-{ad_audio_end:.1f}s")
    content_start = max(0.0, initial_offset)
    total_dur = len(mono_audio) / float(sr)
    regions = _build_clean_regions(content_start, ad_breaks, total_dur)
    if not regions:
        return mono_audio[int(content_start * sr):], initial_offset, ad_breaks
    chunks = []
    for rs, re_ in regions:
        s1 = int(rs * sr)
        s2 = min(int(re_ * sr), len(mono_audio))
        if s2 > s1:
            chunks.append(mono_audio[s1:s2])
    if chunks:
        cleaned = np.concatenate(chunks)
    else:
        cleaned = mono_audio[int(content_start * sr):]
    print(f"    {ep_label}: cleaned {total_dur:.0f}s -> {len(cleaned)/sr:.0f}s "
          f"(removed {len(ad_breaks)} ads, offset={initial_offset:.1f}s)")

    if ad_breaks:
        try:
            if os.path.exists(CACHE_PATH):
                with open(CACHE_PATH, "r", encoding="utf-8") as f:
                    cache = json.load(f)
            else:
                cache = {}
            cache[ep_label] = {"offset": initial_offset, "ad_breaks": ad_breaks}
            with open(CACHE_PATH, "w", encoding="utf-8") as f:
                json.dump(cache, f, ensure_ascii=False, indent=2)
        except Exception:
            pass

    return cleaned, initial_offset, ad_breaks


def resample_to_target(audio, sr):
    if sr == TARGET_SR:
        return audio.astype(np.float32)
    return librosa.resample(audio.astype(np.float32), orig_sr=sr, target_sr=TARGET_SR)


def compute_speaker_turns(spk0_words, spk1_words):
    # Group words of Main and Other into continuous phrases (gap < 1.5s)
    def group_speaker(words, speaker_name):
        if not words:
            return []
        sorted_w = sorted(words, key=lambda x: x[1])
        phrases = []
        curr_words = [sorted_w[0]]
        for w in sorted_w[1:]:
            prev_end = curr_words[-1][2]
            start = w[1]
            if start - prev_end < 1.5:
                curr_words.append(w)
            else:
                phrases.append({
                    "speaker": speaker_name,
                    "start": curr_words[0][1],
                    "end": max(x[2] for x in curr_words),
                    "words": curr_words
                })
                curr_words = [w]
        phrases.append({
            "speaker": speaker_name,
            "start": curr_words[0][1],
            "end": max(x[2] for x in curr_words),
            "words": curr_words
        })
        return phrases

    utts0 = group_speaker(spk0_words, "SPEAKER_MAIN")
    utts1 = group_speaker(spk1_words, "SPEAKER_OTHER")
    
    # Combine and sort all phrases by start time
    all_utts = sorted(utts0 + utts1, key=lambda x: x["start"])
    if not all_utts:
        return []

    resolved = []
    for curr in all_utts:
        if not resolved:
            resolved.append(curr)
            continue
        
        prev = resolved[-1]
        if prev["speaker"] == curr["speaker"]:
            # Same speaker: merge them!
            prev["end"] = max(prev["end"], curr["end"])
            prev["words"].extend(curr["words"])
            prev["words"].sort(key=lambda x: x[1])
        else:
            # Different speaker: check for overlap
            if curr["start"] < prev["end"]:
                # They overlap!
                if curr["end"] < prev["end"]:
                    # Nested overlap! Slice prev into two parts
                    p1_words = [w for w in prev["words"] if w[1] < curr["start"]]
                    p2_words = [w for w in prev["words"] if w[1] >= curr["end"]]
                    
                    original_prev_end = prev["end"]
                    
                    # Update prev to part 1
                    if p1_words:
                        prev["words"] = p1_words
                        prev["end"] = curr["start"]
                        resolved.append(curr)
                    else:
                        # Part 1 is empty, so resolved[-1] can just be replaced by curr
                        resolved[-1] = curr
                        
                    # Add part 2
                    if p2_words:
                        part2_turn = {
                            "speaker": prev["speaker"],
                            "start": curr["end"],
                            "end": original_prev_end,
                            "words": p2_words
                        }
                        resolved.append(part2_turn)
                else:
                    # Partial overlap: split at midpoint of overlap
                    mid = (curr["start"] + prev["end"]) / 2.0
                    prev["end"] = mid
                    curr["start"] = mid
                    prev["words"] = [w for w in prev["words"] if w[1] < mid]
                    curr["words"] = [w for w in curr["words"] if w[1] >= mid]
                    
                    # Only append if both have words left
                    if not prev["words"]:
                        resolved[-1] = curr
                    else:
                        resolved.append(curr)
            else:
                # No overlap
                resolved.append(curr)
                
    # Convert back to the expected list of tuples: (speaker, start, end, words)
    turns = []
    for r in resolved:
        if r["words"]: # only keep if there are words left
            turns.append((r["speaker"], r["start"], r["end"], r["words"]))
    return turns


def write_chunk(stereo, sr, aligns, output_dir, fname):
    os.makedirs(output_dir, exist_ok=True)
    wav_path = os.path.join(output_dir, f"{fname}.wav")
    json_path = os.path.join(output_dir, f"{fname}.json")
    sf.write(wav_path, stereo.T, sr, subtype="PCM_16")
    with open(json_path, "w", encoding="utf-8") as f:
        json.dump({"alignments": aligns}, f, ensure_ascii=False)


def maybe_inject_facts(stereo, sr, aligns, chunk_dur, speaker):
    if speaker != "SPEAKER_MAIN":
        return stereo, aligns
    if chunk_dur < MIN_DUR_FOR_FACTS:
        return stereo, aligns
    if np.random.random() >= FACT_INJECT_PROB:
        return stereo, aligns
    fact = np.random.choice(FACTS)
    ref_words = f"[Injected reference] {fact} [End of injected reference]".split()
    ref_dur = 0.05
    fact_aligns = []
    for r_idx, rw in enumerate(ref_words):
        fact_aligns.append([rw, [r_idx * ref_dur, (r_idx + 1) * ref_dur], "SPEAKER_MAIN"])
    shift = len(ref_words) * ref_dur
    silence = np.zeros((2, int(shift * sr)), dtype=np.float32)
    stereo = np.concatenate([silence, stereo], axis=1)
    shifted = [[w, [s + shift, e + shift], spk] for w, [s, e], spk in aligns]
    return stereo, fact_aligns + shifted


def process_dialogue(mono_raw, sr, spk0_words, spk1_words, dataset_name, file_id, output_dir, precomputed_turns=None):
    mono = resample_to_target(mono_raw, sr)
    sr = TARGET_SR
    total_samples = len(mono)
    total_dur = total_samples / float(sr)
    if total_dur < 0.5:
        return 0
    turns = precomputed_turns if precomputed_turns is not None else compute_speaker_turns(spk0_words, spk1_words)
    if not turns:
        return 0
    split_points = [0.0]
    for i in range(len(turns) - 1):
        prev_end = turns[i][2]
        next_start = turns[i + 1][1]
        midpoint = (prev_end + next_start) / 2.0
        midpoint = max(split_points[-1], min(midpoint, total_dur))
        split_points.append(midpoint)
    split_points.append(total_dur)
    chunk_count = 0
    for i, (speaker, _ts, _te, turn_words) in enumerate(turns):
        chunk_start = split_points[i]
        chunk_end = split_points[i + 1]
        s_sample = int(chunk_start * sr)
        e_sample = min(int(chunk_end * sr), total_samples)
        if e_sample <= s_sample:
            continue
        chunk_mono = mono[s_sample:e_sample]
        chunk_dur = len(chunk_mono) / float(sr)
        if chunk_dur < MIN_CHUNK_DUR:
            continue
        stereo = np.stack([chunk_mono.copy(), chunk_mono.copy()])
        if speaker == "SPEAKER_MAIN":
            suffix = "main"
            stereo[1] = 0.0
        else:
            suffix = "other"
            stereo[0] = 0.0
        chunk_aligns = []
        for word, ws, we in turn_words:
            adj_s = max(0.0, ws - chunk_start)
            adj_e = min(chunk_dur, we - chunk_start)
            if adj_e > adj_s:
                chunk_aligns.append([word, [round(adj_s, 6), round(adj_e, 6)], speaker])
        if not chunk_aligns:
            continue
        stereo, chunk_aligns = maybe_inject_facts(stereo, sr, chunk_aligns, chunk_dur, speaker)
        fname = f"{file_id}_{chunk_count:04d}_{suffix}"
        write_chunk(stereo, sr, chunk_aligns, output_dir, fname)
        chunk_count += 1
    return chunk_count


def process_single_speaker(mono_raw, sr, words, dataset_name, file_id, output_dir):
    mono = resample_to_target(mono_raw, sr)
    sr = TARGET_SR
    total_dur = len(mono) / float(sr)
    if total_dur < 0.5 or not words:
        return 0
    stereo = np.stack([mono.copy(), np.zeros_like(mono)])
    aligns = []
    for word, ws, we in words:
        if we > 0 and ws < total_dur:
            aligns.append([word, [round(ws, 6), round(min(we, total_dur), 6)], "SPEAKER_MAIN"])
    if not aligns:
        return 0
    stereo, aligns = maybe_inject_facts(stereo, sr, aligns, total_dur, "SPEAKER_MAIN")
    fname = f"{file_id}_main"
    write_chunk(stereo, sr, aligns, output_dir, fname)
    return 1


def load_wav_as_mono(wav_path):
    audio, sr = sf.read(wav_path)
    if audio.ndim == 2:
        mono = np.mean(audio, axis=1).astype(np.float32)
    else:
        mono = audio.astype(np.float32)
    return mono, sr


def process_bematac():
    print("\n" + "=" * 60)
    print("Processing BeMaTac dataset...")
    print("=" * 60)
    output_dir = os.path.join(PROCESSED_DIR, "bematac")
    files = []
    for l_dir in ["l1_exmaralda_2", "l2_exmaralda_2"]:
        exb_dir = os.path.join(RAW_DIR, "BeMaTac", l_dir)
        wav_dir = os.path.join(RAW_DIR, "BeMaTac", l_dir.replace("exmaralda", "wav"))
        if not os.path.exists(exb_dir):
            continue
        for exb_path in glob.glob(os.path.join(exb_dir, "*.exb")):
            file_id = os.path.splitext(os.path.basename(exb_path))[0]
            wav_path = os.path.join(wav_dir, f"{file_id}.wav")
            if os.path.exists(wav_path):
                files.append((exb_path, wav_path, file_id))
    files.sort()
    total_chunks, total_errors = 0, 0
    for i, (exb_p, wav_p, fid) in enumerate(files):
        try:
            spk0, spk1 = parse_exb(exb_p)
            mono, sr = load_wav_as_mono(wav_p)
            n = process_dialogue(mono, sr, spk0, spk1, "bematac", fid, output_dir)
            total_chunks += n
        except Exception as e:
            log_error("bematac", fid, str(e))
            total_errors += 1
        if (i + 1) % 5 == 0 or i == len(files) - 1:
            print(f"  BeMaTac: {i+1}/{len(files)} files, {total_chunks} chunks, {total_errors} errors")
    print(f"BeMaTac complete: {total_chunks} chunks from {len(files)} files ({total_errors} errors)")


def process_gcsc():
    print("\n" + "=" * 60)
    print("Processing German Conversational Speech Corpus...")
    print("=" * 60)
    output_dir = os.path.join(PROCESSED_DIR, "gcsc")
    gcsc_txt_dir = os.path.join(RAW_DIR, "German_Conversational_Speech_Corpus", "TXT")
    gcsc_wav_dir = os.path.join(RAW_DIR, "German_Conversational_Speech_Corpus", "WAV")
    if not os.path.exists(gcsc_txt_dir):
        print("  GCSC TXT directory not found, skipping")
        return
    txt_files = glob.glob(os.path.join(gcsc_txt_dir, "*.txt"))
    paired = {}
    for tf in txt_files:
        base = os.path.basename(tf)
        m = re.match(r"(A\d+_S\d+_\d+_G\d+)", base)
        if m:
            prefix = m.group(1)
            core = prefix[:-4]
            spk_id = int(prefix[-4:])
            if core not in paired:
                paired[core] = []
            paired[core].append((spk_id, tf))
    paired_items = sorted(paired.items())
    total_chunks, total_errors = 0, 0
    for pi, (core, spk_list) in enumerate(paired_items):
        if len(spk_list) != 2:
            continue
        spk_list.sort()
        spk0_id, txt0 = spk_list[0]
        spk1_id, txt1 = spk_list[1]
        wav0 = os.path.join(gcsc_wav_dir, f"{core}{spk0_id:04d}.wav")
        wav1 = os.path.join(gcsc_wav_dir, f"{core}{spk1_id:04d}.wav")
        if not (os.path.exists(wav0) and os.path.exists(wav1)):
            continue
        fid = f"{core}{spk0_id}_{spk1_id}"
        try:
            spk0_w = parse_gcsc_txt(txt0)
            spk1_w = parse_gcsc_txt(txt1)
            a0, sr0 = sf.read(wav0)
            a1, sr1 = sf.read(wav1)
            min_len = min(len(a0), len(a1))
            mono = ((a0[:min_len] + a1[:min_len]) / 2.0).astype(np.float32)
            n = process_dialogue(mono, sr0, spk0_w, spk1_w, "gcsc", fid, output_dir)
            total_chunks += n
        except Exception as e:
            log_error("gcsc", fid, str(e))
            total_errors += 1
        if (pi + 1) % 5 == 0 or pi == len(paired_items) - 1:
            print(f"  GCSC: {pi+1}/{len(paired_items)} pairs, {total_chunks} chunks, {total_errors} errors")
    print(f"GCSC complete: {total_chunks} chunks ({total_errors} errors)")


def process_callfriend():
    print("\n" + "=" * 60)
    print("Processing CallFriend dataset...")
    print("=" * 60)
    output_dir = os.path.join(PROCESSED_DIR, "callfriend")
    trans_dir = os.path.join(RAW_DIR, "German.CallFriend.Corpus", "CallFriendTranscript")
    wav_dir = os.path.join(RAW_DIR, "German.CallFriend.Corpus", "CallFriendWav")
    if not os.path.exists(trans_dir):
        print("  CallFriend transcript directory not found, skipping")
        return
    cha_files = sorted(glob.glob(os.path.join(trans_dir, "*.cha")))
    total_chunks, total_errors = 0, 0
    for ci, cha_p in enumerate(cha_files):
        fid = os.path.splitext(os.path.basename(cha_p))[0]
        wav_p = os.path.join(wav_dir, f"{fid}.wav")
        if not os.path.exists(wav_p):
            continue
        try:
            spk0, spk1 = parse_cha(cha_p)
            mono, sr = load_wav_as_mono(wav_p)
            n = process_dialogue(mono, sr, spk0, spk1, "callfriend", fid, output_dir)
            total_chunks += n
        except Exception as e:
            log_error("callfriend", fid, str(e))
            total_errors += 1
        if (ci + 1) % 10 == 0 or ci == len(cha_files) - 1:
            print(f"  CallFriend: {ci+1}/{len(cha_files)} files, {total_chunks} chunks, {total_errors} errors")
    print(f"CallFriend complete: {total_chunks} chunks ({total_errors} errors)")


def process_callhome():
    print("\n" + "=" * 60)
    print("Processing CallHome dataset...")
    print("=" * 60)
    output_dir = os.path.join(PROCESSED_DIR, "callhome")
    trans_dir = os.path.join(RAW_DIR, "German.CallHome.Corpus", "CallHomeTranscript")
    wav_dir = os.path.join(RAW_DIR, "German.CallHome.Corpus", "CallHomeWav")
    if not os.path.exists(trans_dir):
        print("  CallHome transcript directory not found, skipping")
        return
    cha_files = sorted(glob.glob(os.path.join(trans_dir, "*.cha")))
    total_chunks, total_errors = 0, 0
    for ci, cha_p in enumerate(cha_files):
        fid = os.path.splitext(os.path.basename(cha_p))[0]
        wav_p = os.path.join(wav_dir, f"{fid}.wav")
        if not os.path.exists(wav_p):
            continue
        try:
            spk0, spk1 = parse_cha(cha_p)
            mono, sr = load_wav_as_mono(wav_p)
            n = process_dialogue(mono, sr, spk0, spk1, "callhome", fid, output_dir)
            total_chunks += n
        except Exception as e:
            log_error("callhome", fid, str(e))
            total_errors += 1
        if (ci + 1) % 10 == 0 or ci == len(cha_files) - 1:
            print(f"  CallHome: {ci+1}/{len(cha_files)} files, {total_chunks} chunks, {total_errors} errors")
    print(f"CallHome complete: {total_chunks} chunks ({total_errors} errors)")



def process_podcast():
    print("\n" + "=" * 60)
    print("Processing Gemischtes Hack Podcast (episodes 150-300)...")
    print("=" * 60)
    output_dir = os.path.join(PROCESSED_DIR, "podcast")
    trans_dir = os.path.join(RAW_DIR, "Gemischtes.Hack.Podcast", "transcripts")
    audio_dir = os.path.join(RAW_DIR, "Gemischtes.Hack.Podcast")
    if not os.path.exists(trans_dir):
        print("  Podcast transcript directory not found, skipping")
        return
    ep_jsons = sorted(glob.glob(os.path.join(trans_dir, "episode_*.json")))
    mp3_files = sorted(glob.glob(os.path.join(audio_dir, "*.mp3")))
    mp3_by_ep = {}
    for mp3 in mp3_files:
        m_mp3 = re.search(r"^#(\d+)\s", os.path.basename(mp3))
        if m_mp3:
            mp3_by_ep[int(m_mp3.group(1))] = mp3
    matched = []
    for json_p in ep_jsons:
        m = re.search(r"episode_(\d+)_", os.path.basename(json_p))
        if m:
            ep_num = int(m.group(1))
            if 150 <= ep_num <= 300 and ep_num in mp3_by_ep:
                matched.append((ep_num, json_p, mp3_by_ep[ep_num]))
    matched.sort()
    print(f"  Found {len(matched)} matched episodes (150-300)")
    total_chunks, total_errors = 0, 0
    for ei, (ep_num, json_p, mp3_path) in enumerate(matched):
        try:
            print(f"\n  Processing episode {ep_num} ({ei+1}/{len(matched)})...")
            print(f"    Parsing transcript: {json_p}")
            turns = parse_podcast_turns(json_p)
            if not turns:
                log_error("podcast", f"ep{ep_num}", "No speaker turns in transcript")
                total_errors += 1
                continue
            print(f"    Found {len(turns)} speaker turns")
            
            print(f"    Loading audio: {mp3_path}")
            mono, sr = librosa.load(mp3_path, sr=TARGET_SR, mono=True)
            mono = mono.astype(np.float32)
            print(f"    Audio loaded: {len(mono)/sr:.1f}s at {sr}Hz")
            
            print(f"    Cleaning ads with waveform alignment...")
            cleaned, offset, ad_breaks = clean_podcast_ads_waveform_based(mono, int(sr), json_p, ep_label=f"ep{ep_num}")
            print(f"    Cleaned audio: {len(cleaned)/sr:.1f}s (removed {len(mono)/sr - len(cleaned)/sr:.1f}s)")
            
            print(f"    Creating chunks...")
            n = process_dialogue(cleaned, int(sr), None, None, "podcast", f"ep{ep_num}", output_dir, precomputed_turns=turns)
            print(f"    Created {n} chunks")
            total_chunks += n
        except Exception as e:
            log_error("podcast", f"ep{ep_num}", str(e))
            total_errors += 1
        if (ei + 1) % 5 == 0 or ei == len(matched) - 1:
            print(f"  Podcast: {ei+1}/{len(matched)} episodes, {total_chunks} chunks, {total_errors} errors")
    print(f"Podcast complete: {total_chunks} chunks ({total_errors} errors)")


def process_medical():
    print("\n" + "=" * 60)
    print("Processing Medical dataset...")
    print("=" * 60)
    output_dir = os.path.join(PROCESSED_DIR, "medical")
    med_dir = os.path.join(RAW_DIR, "medical", "stereo")
    if not os.path.exists(med_dir):
        print(f"  Medical directory not found at {med_dir}, skipping")
        return
    med_wavs = sorted(glob.glob(os.path.join(med_dir, "*.wav")))
    print(f"  Found {len(med_wavs)} medical WAV files")
    total_chunks, total_errors = 0, 0
    for mi, mw in enumerate(med_wavs):
        fid = os.path.splitext(os.path.basename(mw))[0]
        mj = mw.replace(".wav", ".json")
        if not os.path.exists(mj):
            log_error("medical", fid, "Missing JSON")
            total_errors += 1
            continue
        try:
            spk0, spk1 = parse_medical_json(mj)
            mono, sr = load_wav_as_mono(mw)
            n = process_dialogue(mono, sr, spk0, spk1, "medical", fid, output_dir)
            total_chunks += n
        except Exception as e:
            log_error("medical", fid, str(e))
            total_errors += 1
        if (mi + 1) % 50 == 0 or mi == len(med_wavs) - 1:
            print(f"  Medical: {mi+1}/{len(med_wavs)} files, {total_chunks} chunks, {total_errors} errors")
    print(f"Medical complete: {total_chunks} chunks ({total_errors} errors)")


def process_nyrahealth():
    print("\n" + "=" * 60)
    print("Processing Nyrahealth Disfluency dataset...")
    print("=" * 60)
    output_dir = os.path.join(PROCESSED_DIR, "nyrahealth")
    nyra_dir = os.path.join(RAW_DIR, "nyrahealth")
    if not os.path.exists(nyra_dir):
        print(f"  Nyrahealth directory not found at {nyra_dir}, skipping")
        return
    wav_files = sorted(glob.glob(os.path.join(nyra_dir, "*.wav")))
    print(f"  Found {len(wav_files)} nyrahealth WAV files")
    total_chunks, total_errors = 0, 0
    for ni, wav_p in enumerate(wav_files):
        fid = os.path.splitext(os.path.basename(wav_p))[0]
        json_p = wav_p.replace(".wav", ".json")
        if not os.path.exists(json_p):
            log_error("nyrahealth", fid, "Missing JSON")
            total_errors += 1
            continue
        try:
            with open(json_p, "r", encoding="utf-8") as f:
                data = json.load(f)
            transcript = data.get("intended_transcript", data.get("verbatim_transcript", ""))
            mono, sr = load_wav_as_mono(wav_p)
            duration = len(mono) / float(sr)
            words_list = transcript.strip().split()
            if not words_list or duration <= 0:
                continue
            word_dur = duration / len(words_list)
            words = [(w, i * word_dur, (i + 1) * word_dur) for i, w in enumerate(words_list)]
            n = process_single_speaker(mono, sr, words, "nyrahealth", fid, output_dir)
            total_chunks += n
        except Exception as e:
            log_error("nyrahealth", fid, str(e))
            total_errors += 1
        if (ni + 1) % 50 == 0 or ni == len(wav_files) - 1:
            print(f"  Nyrahealth: {ni+1}/{len(wav_files)} files, {total_chunks} chunks, {total_errors} errors")
    print(f"Nyrahealth complete: {total_chunks} chunks ({total_errors} errors)")


def process_mozilla():
    print("\n" + "=" * 60)
    print("Processing Mozilla German Spontaneous Speech...")
    print("=" * 60)
    output_dir = os.path.join(PROCESSED_DIR, "mozilla")
    mozilla_base = os.path.join(RAW_DIR, "Moziila.German.Spontaneous", "sps-corpus-3.0-2026-03-09-de")
    tsv_path = os.path.join(mozilla_base, "ss-corpus-de.tsv")
    audio_dir = os.path.join(mozilla_base, "audios")
    if not os.path.exists(tsv_path):
        print(f"  Mozilla TSV not found at {tsv_path}, skipping")
        return
    entries = []
    with open(tsv_path, "r", encoding="utf-8") as f:
        header = f.readline()
        for line in f:
            parts = line.strip().split("\t")
            if len(parts) < 7:
                continue
            audio_file = parts[2]
            duration_ms = float(parts[3]) if parts[3] else 0
            transcription = parts[6] if len(parts) > 6 else ""
            if transcription and audio_file:
                entries.append((audio_file, transcription, duration_ms))
    print(f"  Found {len(entries)} entries with transcriptions")
    total_chunks, total_errors = 0, 0
    for mi, (audio_file, transcription, duration_ms) in enumerate(entries):
        fid = os.path.splitext(audio_file)[0]
        audio_path = os.path.join(audio_dir, audio_file)
        if not os.path.exists(audio_path):
            log_error("mozilla", fid, "Audio file not found")
            total_errors += 1
            continue
        try:
            mono, sr = librosa.load(audio_path, sr=None, mono=True)
            mono = mono.astype(np.float32)
            duration = len(mono) / float(sr)
            words_list = transcription.strip().split()
            if not words_list or duration <= 0:
                continue
            word_dur = duration / len(words_list)
            words = [(w, i * word_dur, (i + 1) * word_dur) for i, w in enumerate(words_list)]
            n = process_single_speaker(mono, sr, words, "mozilla", fid, output_dir)
            total_chunks += n
        except Exception as e:
            log_error("mozilla", fid, str(e))
            total_errors += 1
        if (mi + 1) % 50 == 0 or mi == len(entries) - 1:
            print(f"  Mozilla: {mi+1}/{len(entries)} files, {total_chunks} chunks, {total_errors} errors")
    print(f"Mozilla complete: {total_chunks} chunks ({total_errors} errors)")


def main():
    import sys
    np.random.seed(42)
    os.makedirs(PROCESSED_DIR, exist_ok=True)

    dataset_args = [arg.lower() for arg in sys.argv[1:]]
    run_all = len(dataset_args) == 0

    if run_all or "bematac" in dataset_args:
        process_bematac()
    if run_all or "gcsc" in dataset_args:
        process_gcsc()
    if run_all or "callfriend" in dataset_args:
        process_callfriend()
    if run_all or "callhome" in dataset_args:
        process_callhome()
    if run_all or "podcast" in dataset_args:
        process_podcast()
    if run_all or "medical" in dataset_args:
        process_medical()
    if run_all or "nyrahealth" in dataset_args:
        process_nyrahealth()
    if run_all or "mozilla" in dataset_args:
        process_mozilla()

    if error_log:
        print(f"\n{'=' * 60}")
        print(f"ERRORS SUMMARY ({len(error_log)} total):")
        print("=" * 60)
        for err in error_log:
            print(f"  {err}")
        err_path = os.path.join(PROCESSED_DIR, "errors.log")
        with open(err_path, "w") as f:
            for err in error_log:
                f.write(err + "\n")
        print(f"Errors saved to {err_path}")

    print("\nDataset preparation completed!")


if __name__ == "__main__":
    main()
