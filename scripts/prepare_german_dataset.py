import os
import re
import sys
import xml.etree.ElementTree as ET
import glob
import json
import traceback
import soundfile as sf
import numpy as np
import torch
import torchaudio

RAW_DIR = "/Volumes/eHDD/moshi-rag-data/datasets"
PROCESSED_DIR = "/Volumes/eHDD/moshi-rag-data/processed"
PROCESSED_DIR2 = "/Volumes/eHDD/moshi-rag-data/processed2"
PROCESSED_DIR3 = "/Volumes/eHDD/moshi-rag-data/processed3"

for _d in [PROCESSED_DIR, PROCESSED_DIR2, PROCESSED_DIR3]:
    os.makedirs(_d, exist_ok=True)

TARGET_SR = 24000
MAX_PODCAST_SEGMENT_SEC = 180

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

        spk0_words = []
        spk1_words = []
        for tier in root.findall(".//tier"):
            speaker = tier.attrib.get("speaker")
            category = tier.attrib.get("category", "")
            if "norm" in category:
                for event in tier.findall("event"):
                    start_id = event.attrib.get("start")
                    end_id = event.attrib.get("end")
                    text = (event.text or "").strip()
                    if not text or start_id not in tli_to_time or end_id not in tli_to_time:
                        continue
                    start_t = tli_to_time[start_id]
                    end_t = tli_to_time[end_id]
                    if speaker == "SPK0":
                        spk0_words.append((text, start_t, end_t))
                    elif speaker == "SPK1":
                        spk1_words.append((text, start_t, end_t))
        spk0_words.sort(key=lambda x: x[1])
        spk1_words.sort(key=lambda x: x[1])
        return spk0_words, spk1_words
    except Exception:
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
        return []


def parse_cha(cha_path):
    spk0_words = []
    spk1_words = []
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
        return [], []


WHISPER_CLI = os.path.join(os.path.dirname(os.path.dirname(__file__)), "whisper-cpp", "build", "bin", "whisper-cli")
WHISPER_MODEL = os.path.join(os.path.dirname(os.path.dirname(__file__)), "bin", "models", "ggml-large-v3-turbo-q5_0.bin")


def detect_podcast_offset(audio, sr, first_segment_text):
    import subprocess
    import tempfile

    probe_dur = min(180, audio.shape[1] / sr)
    probe_samples = int(probe_dur * sr)
    mono = np.mean(audio[:, :probe_samples], axis=0).astype(np.float32)

    if sr != 16000:
        t = torch.tensor(mono).unsqueeze(0)
        resampler = torchaudio.transforms.Resample(sr, 16000)
        mono_16k = resampler(t).numpy()[0]
    else:
        mono_16k = mono

    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        tmp_path = tmp.name
        sf.write(tmp_path, mono_16k, 16000)

    try:
        cmd = [WHISPER_CLI, "-m", WHISPER_MODEL, "-l", "de", "-f", tmp_path]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=120)
        lines = res.stdout.strip().split("\n")
    except Exception:
        return 0.0
    finally:
        os.unlink(tmp_path)

    ref_words = set(re.sub(r"[^\w\s]", "", first_segment_text.lower()).split())
    if len(ref_words) < 3:
        return 0.0

    for line in lines:
        m = re.match(r"\[(\d{2}):(\d{2}):(\d{2})\.(\d+) --> (\d{2}):(\d{2}):(\d{2})\.(\d+)\]\s*(.*)", line)
        if not m:
            continue
        seg_start = int(m.group(1)) * 3600 + int(m.group(2)) * 60 + int(m.group(3)) + int(m.group(4)) / 1000.0
        seg_text = m.group(9).strip()
        seg_words = set(re.sub(r"[^\w\s]", "", seg_text.lower()).split())
        if len(ref_words) > 0 and len(seg_words) > 0:
            overlap = len(ref_words & seg_words) / len(ref_words)
            if overlap >= 0.5:
                return max(0.0, seg_start)

    return 0.0


def parse_podcast_json(json_path):
    spk0_words = []
    spk1_words = []
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
                if not speaker:
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
        return [], []


def build_gating_mask(words, num_samples, sr):
    mask = np.zeros(num_samples, dtype=np.float32)
    for _, start, end in words:
        s_idx = max(0, int((start - 0.2) * sr))
        e_idx = min(num_samples, int((end + 0.2) * sr))
        if s_idx < e_idx:
            mask[s_idx:e_idx] = 1.0
    return mask


def _make_fact_injection(inject_ref_prob):
    if inject_ref_prob > 0.0 and np.random.random() < inject_ref_prob:
        fact = np.random.choice(FACTS)
        ref_words = f"[Injected reference] {fact} [End of injected reference]".split()
        ref_dur = 0.05
        fact_aligns = []
        for r_idx, rw in enumerate(ref_words):
            fact_aligns.append([rw, [r_idx * ref_dur, (r_idx + 1) * ref_dur], "SPEAKER_MAIN"])
        shift = len(ref_words) * ref_dur
        return fact_aligns, shift
    return None, 0.0


def _write_segment(seg_audio, seg_aligns, sr, output_dir, dataset_name, fname):
    os.makedirs(output_dir, exist_ok=True)
    wav_path = os.path.join(output_dir, f"{fname}.wav")
    json_path = os.path.join(output_dir, f"{fname}.json")

    if sr != TARGET_SR:
        tensor = torch.from_numpy(seg_audio).to(torch.float32)
        resampler = torchaudio.transforms.Resample(sr, TARGET_SR)
        seg_audio = resampler(tensor).numpy()

    sf.write(wav_path, seg_audio.T, TARGET_SR)
    with open(json_path, "w", encoding="utf-8") as jf:
        json.dump({"alignments": seg_aligns}, jf, ensure_ascii=False)

    final_dur = seg_audio.shape[1] / float(TARGET_SR)
    return {"path": f"{dataset_name}/{fname}.wav", "duration": final_dur}


def process_full_dialogue(audio, sr, spk0_words, spk1_words, dataset_name, file_id,
                          output_dir, output_dir2, output_dir3,
                          mono_source=False, inject_ref_prob=0.0,
                          max_segment_sec=None, trim_start_sec=0.0):
    num_channels, total_samples = audio.shape

    if trim_start_sec > 0:
        trim_samples = int(trim_start_sec * sr)
        if trim_samples >= total_samples:
            return [], [], []
        audio = audio[:, trim_samples:]
        total_samples = audio.shape[1]

    duration = total_samples / float(sr)
    if duration < 1.0:
        return [], [], []

    if mono_source:
        mono = np.mean(audio, axis=0).astype(np.float32)
    else:
        mono = None

    mask0 = build_gating_mask(spk0_words, total_samples, sr)
    mask1 = build_gating_mask(spk1_words, total_samples, sr)

    stereo = np.zeros((2, total_samples), dtype=np.float32)
    if mono_source:
        stereo[0] = mono * mask0
        stereo[1] = mono * mask1
    else:
        ch0 = audio[0] if num_channels >= 1 else audio[0]
        ch1 = audio[1] if num_channels >= 2 else audio[0]
        stereo[0] = ch0 * mask0
        stereo[1] = ch1 * mask1

    aligns_spk0 = []
    for word, start, end in spk0_words:
        if end > 0 and start < duration:
            aligns_spk0.append((word, start, end))

    aligns_spk1 = []
    for word, start, end in spk1_words:
        if end > 0 and start < duration:
            aligns_spk1.append((word, start, end))

    if max_segment_sec and duration > max_segment_sec * 1.2:
        segments = []
        seg_start = 0.0
        seg_idx = 0
        while seg_start < duration:
            seg_end = min(seg_start + max_segment_sec, duration)
            segments.append((seg_start, seg_end, seg_idx))
            seg_start = seg_end
            seg_idx += 1
    else:
        segments = [(0.0, duration, 0)]

    entries1, entries2, entries3 = [], [], []

    for seg_start, seg_end, seg_idx in segments:
        s_idx = int(seg_start * sr)
        e_idx = min(int(seg_end * sr), total_samples)
        seg_audio = stereo[:, s_idx:e_idx]
        seg_dur = seg_audio.shape[1] / float(sr)

        if seg_dur < 1.0:
            continue

        if len(segments) > 1:
            fname = f"{file_id}_seg{seg_idx:03d}"
        else:
            fname = file_id

        seg_a0 = []
        for w, ws, we in aligns_spk0:
            if we > seg_start and ws < seg_end:
                adj_s = max(0.0, ws - seg_start)
                adj_e = min(seg_dur, we - seg_start)
                if adj_e > adj_s:
                    seg_a0.append((w, adj_s, adj_e))

        seg_a1 = []
        for w, ws, we in aligns_spk1:
            if we > seg_start and ws < seg_end:
                adj_s = max(0.0, ws - seg_start)
                adj_e = min(seg_dur, we - seg_start)
                if adj_e > adj_s:
                    seg_a1.append((w, adj_s, adj_e))

        if not seg_a0 and not seg_a1:
            continue

        fact_aligns, shift = _make_fact_injection(inject_ref_prob)

        if shift > 0:
            silence_samples = int(shift * sr)
            seg_audio_inj = np.concatenate([np.zeros((2, silence_samples), dtype=np.float32), seg_audio], axis=1)
        else:
            seg_audio_inj = seg_audio

        # --- Variant 1: processed/ — left=spk0 (MAIN), right=spk1 ---
        v1_aligns = []
        if fact_aligns:
            v1_aligns.extend(fact_aligns)
        for w, s, e in seg_a0:
            v1_aligns.append([w, [s + shift, e + shift], "SPEAKER_MAIN"])
        e1 = _write_segment(seg_audio_inj, v1_aligns, sr, output_dir, dataset_name, fname)
        entries1.append(e1)

        # --- Variant 2: processed2/ — left=spk1 (MAIN), right=spk0 (swapped channels) ---
        swapped_audio = np.stack([seg_audio_inj[1], seg_audio_inj[0]])
        v2_aligns = []
        if fact_aligns:
            v2_aligns.extend(fact_aligns)
        for w, s, e in seg_a1:
            v2_aligns.append([w, [s + shift, e + shift], "SPEAKER_MAIN"])
        e2 = _write_segment(swapped_audio, v2_aligns, sr, output_dir2, dataset_name, fname)
        entries2.append(e2)

        # --- Variant 3: processed3/ — both speakers transcribed ---
        v3_aligns = []
        if fact_aligns:
            v3_aligns.extend(fact_aligns)
        both = [(w, s, e, "SPEAKER_MAIN") for w, s, e in seg_a0] + \
               [(w, s, e, "SPEAKER_OTHER") for w, s, e in seg_a1]
        both.sort(key=lambda x: x[1])
        for w, s, e, spk in both:
            v3_aligns.append([w, [s + shift, e + shift], spk])
        e3 = _write_segment(seg_audio_inj, v3_aligns, sr, output_dir3, dataset_name, fname)
        entries3.append(e3)

    return entries1, entries2, entries3


def ensure_disfluency_dataset():
    disfluency_dir = os.path.join(PROCESSED_DIR, "disfluency")
    if os.path.exists(disfluency_dir) and len(glob.glob(os.path.join(disfluency_dir, "*.wav"))) > 0:
        print("Disfluency dataset already prepared locally.")
        return

    print("Downloading and preparing nyrahealth/disfluency_speech_german dataset locally...")
    os.makedirs(disfluency_dir, exist_ok=True)
    import io
    from datasets import load_dataset, Audio
    try:
        ds = load_dataset("nyrahealth/disfluency_speech_german")
        ds = ds.cast_column("audio", Audio(decode=False))
        test_split = ds["test"]
        print(f"Downloaded disfluency dataset. Found {len(test_split)} samples.")
    except Exception as e:
        print("Failed to download nyrahealth/disfluency_speech_german:", e)
        return

    for idx, sample in enumerate(test_split):
        audio_data = sample["audio"]
        audio_bytes = audio_data["bytes"]
        array, orig_sr = sf.read(io.BytesIO(audio_bytes))

        tensor = torch.tensor(array, dtype=torch.float32).unsqueeze(0)
        if orig_sr != TARGET_SR:
            resampler = torchaudio.transforms.Resample(orig_sr, TARGET_SR)
            tensor = resampler(tensor)

        stereo_tensor = torch.zeros((2, tensor.shape[1]), dtype=torch.float32)
        stereo_tensor[0, :] = tensor[0, :]

        wav_filename = f"nyra_{idx:04d}.wav"
        wav_path = os.path.join(disfluency_dir, wav_filename)

        torchaudio.save(wav_path, stereo_tensor, TARGET_SR)

        duration = stereo_tensor.shape[1] / float(TARGET_SR)

        transcript = sample.get("verbatim_transcript") or sample.get("intended_transcript") or ""
        words = transcript.strip().split()
        alignments = []
        if words:
            word_dur = duration / len(words)
            for w_idx, word in enumerate(words):
                alignments.append([
                    word,
                    [w_idx * word_dur, (w_idx + 1) * word_dur],
                    "SPEAKER_MAIN"
                ])

        json_filename = f"nyra_{idx:04d}.json"
        json_path = os.path.join(disfluency_dir, json_filename)
        with open(json_path, "w", encoding="utf-8") as f:
            json.dump({"alignments": alignments}, f, ensure_ascii=False)

    print(f"Disfluency dataset prepared: {len(test_split)} samples.")


WHISPER_CLI = os.path.join(os.path.dirname(os.path.dirname(__file__)), "whisper-cpp", "build", "bin", "whisper-cli")
WHISPER_MODEL = os.path.join(os.path.dirname(os.path.dirname(__file__)), "bin", "models", "ggml-large-v3-turbo-q5_0.bin")


def transcribe_right_channel(audio, sr):
    import tempfile
    import subprocess
    right = audio[:, 1]
    if np.sqrt(np.mean(right ** 2)) < 0.001:
        return []

    if sr != 16000:
        t = torch.tensor(right, dtype=torch.float32).unsqueeze(0)
        resampler = torchaudio.transforms.Resample(sr, 16000)
        right_16k = resampler(t).numpy()[0]
    else:
        right_16k = right

    with tempfile.NamedTemporaryFile(suffix=".wav", delete=False) as tmp:
        tmp_path = tmp.name
        sf.write(tmp_path, right_16k, 16000)

    try:
        cmd = [WHISPER_CLI, "-m", WHISPER_MODEL, "-l", "de", "-ml", "1", "-otxt", "-f", tmp_path]
        res = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=120)
        lines = res.stdout.strip().split("\n")
        words = []
        for line in lines:
            m = re.match(r"\[(\d{2}):(\d{2}):(\d{2})\.(\d{3})\s*-->\s*(\d{2}):(\d{2}):(\d{2})\.(\d{3})\]\s*(.*)", line)
            if m:
                sh, sm, ss, sms = int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(4))
                eh, em, es, ems = int(m.group(5)), int(m.group(6)), int(m.group(7)), int(m.group(8))
                start_t = sh * 3600 + sm * 60 + ss + sms / 1000.0
                end_t = eh * 3600 + em * 60 + es + ems / 1000.0
                text = m.group(9).strip()
                if not text:
                    continue
                seg_words = text.split()
                if seg_words:
                    seg_dur = (end_t - start_t) / len(seg_words)
                    for wi, w in enumerate(seg_words):
                        ws = start_t + wi * seg_dur
                        we = ws + seg_dur
                        words.append((w, ws, we))
        return words
    except Exception:
        return []
    finally:
        os.unlink(tmp_path)


def parse_medical_file(wav_path, json_path):
    audio, sr = sf.read(wav_path)
    with open(json_path, "r", encoding="utf-8") as f:
        data = json.load(f)
    aligns = data.get("alignments", [])

    spk0_words = []
    for entry in aligns:
        word = entry[0]
        start_t, end_t = entry[1]
        spk0_words.append((word, start_t, end_t))

    spk1_words = transcribe_right_channel(audio, sr)

    return audio, sr, spk0_words, spk1_words


def main():
    ensure_disfluency_dataset()
    manifest_entries1 = []
    manifest_entries2 = []
    manifest_entries3 = []

    print("\n" + "=" * 60)
    print("Processing BeMaTac dataset...")
    print("=" * 60)
    bematac_dir1 = os.path.join(PROCESSED_DIR, "bematac")
    bematac_dir2 = os.path.join(PROCESSED_DIR2, "bematac")
    bematac_dir3 = os.path.join(PROCESSED_DIR3, "bematac")
    bematac_files = []
    for l_dir in ["l1_exmaralda_2", "l2_exmaralda_2"]:
        exb_dir = os.path.join(RAW_DIR, "BeMaTac", l_dir)
        wav_dir = os.path.join(RAW_DIR, "BeMaTac", l_dir.replace("exmaralda", "wav"))
        if os.path.exists(exb_dir):
            for exb_path in glob.glob(os.path.join(exb_dir, "*.exb")):
                file_id = os.path.splitext(os.path.basename(exb_path))[0]
                wav_path = os.path.join(wav_dir, f"{file_id}.wav")
                if os.path.exists(wav_path):
                    bematac_files.append((exb_path, wav_path, file_id))

    bematac_files = sorted(bematac_files)
    bematac_count = 0
    bematac_errors = 0
    for i, (exb_p, wav_p, fid) in enumerate(bematac_files):
        spk0, spk1 = parse_exb(exb_p)
        try:
            audio, sr = sf.read(wav_p)
            audio = audio.T
            if audio.ndim == 1:
                audio = np.stack([audio, audio])
            e1, e2, e3 = process_full_dialogue(audio, sr, spk0, spk1, "bematac", fid,
                                                bematac_dir1, bematac_dir2, bematac_dir3,
                                                inject_ref_prob=0.01)
            manifest_entries1.extend(e1)
            manifest_entries2.extend(e2)
            manifest_entries3.extend(e3)
            bematac_count += len(e1)
        except Exception as e:
            log_error("bematac", fid, str(e))
            bematac_errors += 1
        if (i + 1) % 10 == 0 or i == len(bematac_files) - 1:
            print(f"  BeMaTac: {i + 1}/{len(bematac_files)} files, {bematac_count} output files, {bematac_errors} errors")
    print(f"BeMaTac complete: {bematac_count} files from {len(bematac_files)} sources ({bematac_errors} errors)")

    print("\n" + "=" * 60)
    print("Processing German Conversational Speech Corpus dataset...")
    print("=" * 60)
    gcsc_dir1 = os.path.join(PROCESSED_DIR, "gcsc")
    gcsc_dir2 = os.path.join(PROCESSED_DIR2, "gcsc")
    gcsc_dir3 = os.path.join(PROCESSED_DIR3, "gcsc")
    gcsc_txt_dir = os.path.join(RAW_DIR, "German_Conversational_Speech_Corpus", "TXT")
    gcsc_wav_dir = os.path.join(RAW_DIR, "German_Conversational_Speech_Corpus", "WAV")
    gcsc_count = 0
    gcsc_errors = 0

    if os.path.exists(gcsc_txt_dir):
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

        paired_items = sorted(list(paired.items()))
        for pi, (core, spk_list) in enumerate(paired_items):
            if len(spk_list) == 2:
                spk_list.sort()
                spk0_id, txt0 = spk_list[0]
                spk1_id, txt1 = spk_list[1]

                wav0 = os.path.join(gcsc_wav_dir, f"{core}{spk0_id:04d}.wav")
                wav1 = os.path.join(gcsc_wav_dir, f"{core}{spk1_id:04d}.wav")

                if os.path.exists(wav0) and os.path.exists(wav1):
                    fid = f"{core}{spk0_id}_{spk1_id}"
                    try:
                        spk0_w = parse_gcsc_txt(txt0)
                        spk1_w = parse_gcsc_txt(txt1)

                        a0, sr0 = sf.read(wav0)
                        a1, sr1 = sf.read(wav1)

                        min_len = min(len(a0), len(a1))
                        audio = np.stack([a0[:min_len], a1[:min_len]])

                        e1, e2, e3 = process_full_dialogue(audio, sr0, spk0_w, spk1_w, "gcsc", fid,
                                                            gcsc_dir1, gcsc_dir2, gcsc_dir3,
                                                            inject_ref_prob=0.01)
                        manifest_entries1.extend(e1)
                        manifest_entries2.extend(e2)
                        manifest_entries3.extend(e3)
                        gcsc_count += len(e1)
                    except Exception as e:
                        log_error("gcsc", fid, str(e))
                        gcsc_errors += 1
            if (pi + 1) % 20 == 0 or pi == len(paired_items) - 1:
                print(f"  GCSC: {pi + 1}/{len(paired_items)} pairs, {gcsc_count} output files, {gcsc_errors} errors")
    print(f"GCSC complete: {gcsc_count} files ({gcsc_errors} errors)")

    print("\n" + "=" * 60)
    print("Processing CallFriend dataset...")
    print("=" * 60)
    cf_dir1 = os.path.join(PROCESSED_DIR, "callfriend")
    cf_dir2 = os.path.join(PROCESSED_DIR2, "callfriend")
    cf_dir3 = os.path.join(PROCESSED_DIR3, "callfriend")
    cf_trans_dir = os.path.join(RAW_DIR, "German.CallFriend.Corpus", "CallFriendTranscript")
    cf_wav_dir = os.path.join(RAW_DIR, "German.CallFriend.Corpus", "CallFriendWav")
    cf_count = 0
    cf_errors = 0
    if os.path.exists(cf_trans_dir):
        cha_files = sorted(glob.glob(os.path.join(cf_trans_dir, "*.cha")))
        for ci, cha_p in enumerate(cha_files):
            fid = os.path.splitext(os.path.basename(cha_p))[0]
            wav_p = os.path.join(cf_wav_dir, f"{fid}.wav")
            if os.path.exists(wav_p):
                try:
                    spk0, spk1 = parse_cha(cha_p)
                    audio, sr = sf.read(wav_p)
                    audio = audio.T
                    if audio.ndim == 1:
                        audio = np.stack([audio, audio])
                    e1, e2, e3 = process_full_dialogue(audio, sr, spk0, spk1, "callfriend", fid,
                                                        cf_dir1, cf_dir2, cf_dir3,
                                                        inject_ref_prob=0.01)
                    manifest_entries1.extend(e1)
                    manifest_entries2.extend(e2)
                    manifest_entries3.extend(e3)
                    cf_count += len(e1)
                except Exception as e:
                    log_error("callfriend", fid, str(e))
                    cf_errors += 1
            if (ci + 1) % 10 == 0 or ci == len(cha_files) - 1:
                print(f"  CallFriend: {ci + 1}/{len(cha_files)} files, {cf_count} output files, {cf_errors} errors")
    print(f"CallFriend complete: {cf_count} files ({cf_errors} errors)")

    print("\n" + "=" * 60)
    print("Processing CallHome dataset...")
    print("=" * 60)
    ch_dir1 = os.path.join(PROCESSED_DIR, "callhome")
    ch_dir2 = os.path.join(PROCESSED_DIR2, "callhome")
    ch_dir3 = os.path.join(PROCESSED_DIR3, "callhome")
    ch_trans_dir = os.path.join(RAW_DIR, "German.CallHome.Corpus", "CallHomeTranscript")
    ch_wav_dir = os.path.join(RAW_DIR, "German.CallHome.Corpus", "CallHomeWav")
    ch_count = 0
    ch_errors = 0
    if os.path.exists(ch_trans_dir):
        cha_files = sorted(glob.glob(os.path.join(ch_trans_dir, "*.cha")))
        for ci, cha_p in enumerate(cha_files):
            fid = os.path.splitext(os.path.basename(cha_p))[0]
            wav_p = os.path.join(ch_wav_dir, f"{fid}.wav")
            if os.path.exists(wav_p):
                try:
                    spk0, spk1 = parse_cha(cha_p)
                    audio, sr = sf.read(wav_p)
                    audio = audio.T
                    if audio.ndim == 1:
                        audio = np.stack([audio, audio])
                    e1, e2, e3 = process_full_dialogue(audio, sr, spk0, spk1, "callhome", fid,
                                                        ch_dir1, ch_dir2, ch_dir3,
                                                        inject_ref_prob=0.01)
                    manifest_entries1.extend(e1)
                    manifest_entries2.extend(e2)
                    manifest_entries3.extend(e3)
                    ch_count += len(e1)
                except Exception as e:
                    log_error("callhome", fid, str(e))
                    ch_errors += 1
            if (ci + 1) % 10 == 0 or ci == len(cha_files) - 1:
                print(f"  CallHome: {ci + 1}/{len(cha_files)} files, {ch_count} output files, {ch_errors} errors")
    print(f"CallHome complete: {ch_count} files ({ch_errors} errors)")

    print("\n" + "=" * 60)
    print("Processing Gemischtes Hack Podcast dataset (episodes 150-300)...")
    print("=" * 60)
    podcast_dir1 = os.path.join(PROCESSED_DIR, "podcast")
    podcast_dir2 = os.path.join(PROCESSED_DIR2, "podcast")
    podcast_dir3 = os.path.join(PROCESSED_DIR3, "podcast")
    podcast_trans_dir = os.path.join(RAW_DIR, "Gemischtes.Hack.Podcast", "transcripts")
    podcast_audio_dir = os.path.join(RAW_DIR, "Gemischtes.Hack.Podcast")
    podcast_count = 0
    podcast_errors = 0

    if os.path.exists(podcast_trans_dir):
        ep_jsons = sorted(glob.glob(os.path.join(podcast_trans_dir, "episode_*.json")))
        mp3_files = sorted(glob.glob(os.path.join(podcast_audio_dir, "*.mp3")))

        mp3_by_ep = {}
        for mp3 in mp3_files:
            m_mp3 = re.search(r"^#(\d+)\s", os.path.basename(mp3))
            if m_mp3:
                mp3_by_ep[int(m_mp3.group(1))] = mp3

        matched_episodes = []
        for json_p in ep_jsons:
            m = re.search(r"episode_(\d+)_", os.path.basename(json_p))
            if m:
                ep_num = int(m.group(1))
                if 150 <= ep_num <= 300 and ep_num in mp3_by_ep:
                    matched_episodes.append((ep_num, json_p, mp3_by_ep[ep_num]))

        matched_episodes.sort()
        print(f"  Found {len(matched_episodes)} matched episodes (150-300)")

        for ei, (ep_num, json_p, mp3_path) in enumerate(matched_episodes):
            try:
                spk0, spk1 = parse_podcast_json(json_p)
                if not spk0 and not spk1:
                    log_error("podcast", f"ep{ep_num}", "No speaker words found in transcript")
                    podcast_errors += 1
                    continue

                audio, sr = torchaudio.load(mp3_path)
                audio = audio.numpy()
                if audio.ndim == 1:
                    audio = np.stack([audio, audio])

                with open(json_p, "r", encoding="utf-8") as jf:
                    jdata = json.load(jf)
                first_segs = [s for s in jdata.get("segments", []) if s.get("text", "").strip()]
                first_text = first_segs[0]["text"].strip() if first_segs else ""

                offset = detect_podcast_offset(audio, int(sr), first_text)
                if offset > 0:
                    print(f"    ep{ep_num}: detected ad offset = {offset:.1f}s, cutting intro and adjusting timestamps")

                trim_start = offset

                e1, e2, e3 = process_full_dialogue(audio, int(sr), spk0, spk1, "podcast", f"ep{ep_num}",
                                                    podcast_dir1, podcast_dir2, podcast_dir3,
                                                    mono_source=True, inject_ref_prob=0.01,
                                                    max_segment_sec=MAX_PODCAST_SEGMENT_SEC,
                                                    trim_start_sec=trim_start)
                manifest_entries1.extend(e1)
                manifest_entries2.extend(e2)
                manifest_entries3.extend(e3)
                podcast_count += len(e1)
            except Exception as e:
                log_error("podcast", f"ep{ep_num}", str(e))
                podcast_errors += 1
            if (ei + 1) % 10 == 0 or ei == len(matched_episodes) - 1:
                print(f"  Podcast: {ei + 1}/{len(matched_episodes)} episodes, {podcast_count} output files, {podcast_errors} errors")
    print(f"Podcast complete: {podcast_count} files ({podcast_errors} errors)")

    print("\n" + "=" * 60)
    print("Processing Medical dataset...")
    print("=" * 60)
    med_raw_dir = os.path.join(RAW_DIR, "medical", "stereo")
    med_dir1 = os.path.join(PROCESSED_DIR, "medical")
    med_dir2 = os.path.join(PROCESSED_DIR2, "medical")
    med_dir3 = os.path.join(PROCESSED_DIR3, "medical")
    med_count = 0
    med_errors = 0

    if os.path.exists(med_raw_dir):
        med_wavs = sorted(glob.glob(os.path.join(med_raw_dir, "*.wav")))
        print(f"  Found {len(med_wavs)} medical WAV files")
        print(f"  Transcribing right channels with whisper-cli (this may take a while)...")
        for mi, mw in enumerate(med_wavs):
            fid = os.path.splitext(os.path.basename(mw))[0]
            mj = mw.replace(".wav", ".json")
            if not os.path.exists(mj):
                log_error("medical", fid, "Missing JSON")
                med_errors += 1
                continue
            try:
                audio, sr, spk0, spk1 = parse_medical_file(mw, mj)
                audio_t = audio.T
                if audio_t.ndim == 1:
                    audio_t = np.stack([audio_t, audio_t])
                e1, e2, e3 = process_full_dialogue(audio_t, sr, spk0, spk1, "medical", fid,
                                                    med_dir1, med_dir2, med_dir3,
                                                    inject_ref_prob=0.01)
                manifest_entries1.extend(e1)
                manifest_entries2.extend(e2)
                manifest_entries3.extend(e3)
                med_count += len(e1)
            except Exception as e:
                log_error("medical", fid, str(e))
                med_errors += 1
            if (mi + 1) % 50 == 0 or mi == len(med_wavs) - 1:
                print(f"  Medical: {mi + 1}/{len(med_wavs)} files, {med_count} output files, {med_errors} errors")
    else:
        print(f"  WARNING: Medical raw data not found at {med_raw_dir}")
    print(f"Medical complete: {med_count} files ({med_errors} errors)")

    print("\n" + "=" * 60)
    print("Combining all datasets and exporting final train.jsonl and valid.jsonl manifests...")
    print("=" * 60)

    disfluency_wavs = sorted(glob.glob(os.path.join(PROCESSED_DIR, "disfluency", "*.wav")))
    disfluency_entries = []
    for dw in disfluency_wavs:
        try:
            info = sf.info(dw)
            disfluency_entries.append({
                "path": f"disfluency/{os.path.basename(dw)}",
                "duration": info.duration
            })
        except Exception as e:
            log_error("disfluency", os.path.basename(dw), str(e))

    for variant_idx, (m_entries, proc_dir, label) in enumerate([
        (manifest_entries1, PROCESSED_DIR, "processed"),
        (manifest_entries2, PROCESSED_DIR2, "processed2"),
        (manifest_entries3, PROCESSED_DIR3, "processed3"),
    ]):
        np.random.seed(42)
        shuffled = list(m_entries)
        np.random.shuffle(shuffled)
        split_idx = int(len(shuffled) * 0.95)
        train_entries = shuffled[:split_idx]
        valid_entries = shuffled[split_idx:] + disfluency_entries

        print(f"\n[{label}] Training samples: {len(train_entries)}")
        print(f"[{label}] Validation samples: {len(valid_entries)} (incl. {len(disfluency_entries)} disfluency)")

        with open(os.path.join(proc_dir, "train.jsonl"), "w", encoding="utf-8") as f:
            for entry in train_entries:
                f.write(json.dumps(entry) + "\n")

        with open(os.path.join(proc_dir, "valid.jsonl"), "w", encoding="utf-8") as f:
            for entry in valid_entries:
                f.write(json.dumps(entry) + "\n")

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

    print("\nDataset preparation completed successfully!")


if __name__ == "__main__":
    main()
