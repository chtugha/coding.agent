import os
import re
import xml.etree.ElementTree as ET
import glob
import json
import soundfile as sf
import numpy as np
import torch
import torchaudio

RAW_DIR = "/Volumes/eHDD/moshi-rag-data/datasets"
PROCESSED_DIR = "/Volumes/eHDD/moshi-rag-data/processed"

os.makedirs(PROCESSED_DIR, exist_ok=True)

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
        with open(cha_path, "r", encoding="utf-8") as f:
            for line in f:
                if line.startswith("*PAR0:"):
                    speaker = "PAR0"
                elif line.startswith("*PAR1:"):
                    speaker = "PAR1"
                elif line.startswith("*A:"):
                    speaker = "PAR0"
                elif line.startswith("*B:"):
                    speaker = "PAR1"
                else:
                    continue
                m = re.search(r"\x15(\d+)_(\d+)\x15", line)
                if not m:
                    continue
                start_t = float(m.group(1)) / 1000.0
                end_t = float(m.group(2)) / 1000.0
                text_part = re.sub(r"\x15.*?\x15", "", line)
                text_part = re.sub(r"\*PAR\d+:\s*", "", text_part)
                text_part = re.sub(r"\*[AB]:\s*", "", text_part)
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
                        if speaker == "PAR0":
                            spk0_words.append((w_clean, w_start, w_end))
                        else:
                            spk1_words.append((w_clean, w_start, w_end))
        spk0_words.sort(key=lambda x: x[1])
        spk1_words.sort(key=lambda x: x[1])
        return spk0_words, spk1_words
    except Exception:
        return [], []

def parse_podcast_json(json_path):
    spk0_words = []
    spk1_words = []
    try:
        with open(json_path, "r", encoding="utf-8") as f:
            data = json.load(f)
            segments = data.get("segments", [])
            for seg in segments:
                start_t = float(seg["start"])
                end_t = float(seg["end"])
                text = seg.get("text", "").strip()
                speaker = seg.get("speaker", "")
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
                        if "Speaker A" in speaker or "A" == speaker:
                            spk0_words.append((w_clean, w_start, w_end))
                        elif "Speaker B" in speaker or "B" == speaker:
                            spk1_words.append((w_clean, w_start, w_end))
        spk0_words.sort(key=lambda x: x[1])
        spk1_words.sort(key=lambda x: x[1])
        return spk0_words, spk1_words
    except Exception:
        return [], []

def build_gating_mask(words, num_samples, sr, duration_sec):
    mask = np.zeros(num_samples, dtype=bool)
    for _, start, end in words:
        s_idx = max(0, int((start - 0.2) * sr))
        e_idx = min(num_samples, int((end + 0.2) * sr))
        if s_idx < e_idx:
            mask[s_idx:e_idx] = True
    return mask

def process_and_chunk_duplex(audio, sr, spk0_words, spk1_words, dataset_name, file_id, inject_ref_prob=0.0):
    num_channels, total_samples = audio.shape
    duration = total_samples / float(sr)
    
    # Merge and sort all words from both speakers to identify speaker turns (changes)
    all_events = []
    for w, s, e in spk0_words:
        all_events.append((s, e, 0, w))
    for w, s, e in spk1_words:
        all_events.append((s, e, 1, w))
    all_events.sort(key=lambda x: x[0])
    
    if not all_events:
        return []
        
    turns = []
    current_speaker = all_events[0][2]
    current_words = [all_events[0]]
    
    for ev in all_events[1:]:
        s, e, speaker, w = ev
        if speaker == current_speaker:
            current_words.append(ev)
        else:
            turns.append((current_speaker, current_words))
            current_speaker = speaker
            current_words = [ev]
    if current_words:
        turns.append((current_speaker, current_words))
        
    samples_created = []
    
    for t_idx, (speaker, turn_words) in enumerate(turns):
        turn_start = turn_words[0][0]
        turn_end = turn_words[-1][1]
        
        # Add 0.2s padding before and after the speaker turn to ensure phonetic completeness
        pad_start = max(0.0, turn_start - 0.2)
        pad_end = min(duration, turn_end + 0.2)
        
        c_dur = pad_end - pad_start
        if c_dur < 0.5:
            continue
            
        c_start_idx = int(pad_start * sr)
        c_end_idx = int(pad_end * sr)
        
        # Determine whether to inject reference text for this turn (with probability inject_ref_prob)
        should_inject = (inject_ref_prob > 0.0 and np.random.random() < inject_ref_prob)
        
        aligns = []
        if should_inject:
            fact = np.random.choice(FACTS)
            ref_words = f"[Injected reference] {fact} [End of injected reference]".split()
            ref_dur = 0.05
            for r_idx, rw in enumerate(ref_words):
                aligns.append([rw, [r_idx * ref_dur, (r_idx + 1) * ref_dur], "SPEAKER_MAIN"])
            shift = len(ref_words) * ref_dur
        else:
            shift = 0.0
            
        for s_t, e_t, _, w in turn_words:
            # Shift timestamps so they are relative to pad_start plus shift
            aligns.append([w, [s_t - pad_start + shift, e_t - pad_start + shift], "SPEAKER_MAIN"])
            
        # Get original speaker's source channel from the input audio
        src_chan = speaker % 2
        if src_chan >= audio.shape[0]:
            src_chan = 0 # fallback if mono
            
        source_audio = audio[src_chan, c_start_idx:c_end_idx].copy()
        
        if shift > 0:
            silence_samples = int(shift * sr)
            target_slice = np.zeros((2, len(source_audio) + silence_samples), dtype=np.float32)
            if speaker % 2 == 0:
                target_slice[1, silence_samples:] = source_audio # Left muted, Right active
            else:
                target_slice[0, silence_samples:] = source_audio # Right muted, Left active
        else:
            target_slice = np.zeros((2, len(source_audio)), dtype=np.float32)
            if speaker % 2 == 0:
                target_slice[1, :] = source_audio # Left muted, Right active
            else:
                target_slice[0, :] = source_audio # Right muted, Left active
                
        # Resample slice to 24000
        if sr != 24000:
            tensor = torch.from_numpy(target_slice).to(torch.float32)
            resampler = torchaudio.transforms.Resample(sr, 24000)
            resampled = resampler(tensor).numpy()
        else:
            resampled = target_slice.astype(np.float32)
            
        target_dir = os.path.join(PROCESSED_DIR, dataset_name)
        os.makedirs(target_dir, exist_ok=True)
        
        # Generate file name based on speaker: spkA for even, spkB for odd
        spk_suffix = "spkA" if (speaker % 2 == 0) else "spkB"
        wav_name = f"{file_id}_t{t_idx}_{spk_suffix}.wav"
        wav_path = os.path.join(target_dir, wav_name)
        
        sf.write(wav_path, resampled.T, 24000)
        
        json_path = wav_path.replace(".wav", ".json")
        with open(json_path, "w", encoding="utf-8") as jf:
            json.dump({"alignments": aligns}, jf, ensure_ascii=False)
            
        samples_created.append({
            "path": f"/data/{dataset_name}/{wav_name}",
            "duration": resampled.shape[1] / 24000.0
        })
        
    return samples_created

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
        
        # Resample to 24000Hz (Mimi standard)
        tensor = torch.tensor(array, dtype=torch.float32).unsqueeze(0) # [1, T]
        if orig_sr != 24000:
            resampler = torchaudio.transforms.Resample(orig_sr, 24000)
            tensor = resampler(tensor)
        
        # Convert mono [1, T] to stereo [2, T]
        # Left channel (0) is muted, Right channel (1) is active (Speaker A)
        stereo_tensor = torch.zeros((2, tensor.shape[1]), dtype=torch.float32)
        stereo_tensor[1, :] = tensor[0, :]
        
        wav_filename = f"nyra_{idx:04d}.wav"
        wav_path = os.path.join(disfluency_dir, wav_filename)
        
        # Save as 24kHz WAV
        torchaudio.save(wav_path, stereo_tensor, 24000)
        
        # Calculate duration
        duration = stereo_tensor.shape[1] / 24000.0
        
        # Parse transcript & create alignments
        transcript = sample["verbatim_transcript"] or sample["intended_transcript"] or ""
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

def main():
    ensure_disfluency_dataset()
    manifest_entries = []
    
    # 1. BeMaTac Dataset
    print("Processing BeMaTac dataset...")
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
    for exb_p, wav_p, fid in bematac_files:
        spk0, spk1 = parse_exb(exb_p)
        try:
            audio, sr = sf.read(wav_p)
            audio = audio.T
            # Force stereo
            if audio.ndim == 1:
                audio = np.stack([audio, audio])
            chunks = process_and_chunk_duplex(audio, sr, spk0, spk1, "bematac", fid)
            manifest_entries.extend(chunks)
        except Exception:
            continue
            
    # 2. German Conversational Speech Corpus
    print("Processing German Conversational Speech Corpus dataset...")
    gcsc_txt_dir = os.path.join(RAW_DIR, "German_Conversational_Speech_Corpus", "TXT")
    gcsc_wav_dir = os.path.join(RAW_DIR, "German_Conversational_Speech_Corpus", "WAV")
    
    if os.path.exists(gcsc_txt_dir):
        # find matching pairs
        txt_files = glob.glob(os.path.join(gcsc_txt_dir, "*.txt"))
        paired = {}
        for tf in txt_files:
            base = os.path.basename(tf)
            m = re.match(r"(A\d+_S\d+_\d+_G\d+)", base)
            if m:
                prefix = m.group(1)
                # find sibling (e.g. if G0203, find G0204)
                core = prefix[:-4]
                spk_id = int(prefix[-4:])
                if core not in paired:
                    paired[core] = []
                paired[core].append((spk_id, tf))
                
        paired_items = sorted(list(paired.items()))
        for core, spk_list in paired_items:
            if len(spk_list) == 2:
                spk_list.sort()
                spk0_id, txt0 = spk_list[0]
                spk1_id, txt1 = spk_list[1]
                
                wav0 = os.path.join(gcsc_wav_dir, f"{core}{spk0_id:04d}.wav")
                wav1 = os.path.join(gcsc_wav_dir, f"{core}{spk1_id:04d}.wav")
                
                if os.path.exists(wav0) and os.path.exists(wav1):
                    try:
                        spk0_w = parse_gcsc_txt(txt0)
                        spk1_w = parse_gcsc_txt(txt1)
                        
                        a0, sr0 = sf.read(wav0)
                        a1, sr1 = sf.read(wav1)
                        
                        min_len = min(len(a0), len(a1))
                        audio = np.stack([a0[:min_len], a1[:min_len]])
                        
                        chunks = process_and_chunk_duplex(audio, sr0, spk0_w, spk1_w, "gcsc", f"{core}{spk0_id}_{spk1_id}")
                        manifest_entries.extend(chunks)
                    except Exception:
                        continue
                        
    # 3. CallFriend Dataset
    print("Processing CallFriend dataset...")
    cf_trans_dir = os.path.join(RAW_DIR, "German.CallFriend.Corpus", "CallFriendTranscript")
    cf_wav_dir = os.path.join(RAW_DIR, "German.CallFriend.Corpus", "CallFriendWav")
    if os.path.exists(cf_trans_dir):
        cha_files = glob.glob(os.path.join(cf_trans_dir, "*.cha"))
        for cha_p in cha_files:
            fid = os.path.splitext(os.path.basename(cha_p))[0]
            wav_p = os.path.join(cf_wav_dir, f"{fid}.wav")
            if os.path.exists(wav_p):
                try:
                    spk0, spk1 = parse_cha(cha_p)
                    audio, sr = sf.read(wav_p)
                    audio = audio.T
                    chunks = process_and_chunk_duplex(audio, sr, spk0, spk1, "callfriend", fid)
                    manifest_entries.extend(chunks)
                except Exception:
                    continue
                    
    # 4. CallHome Dataset
    print("Processing CallHome dataset...")
    ch_trans_dir = os.path.join(RAW_DIR, "German.CallHome.Corpus", "CallHomeTranscript")
    ch_wav_dir = os.path.join(RAW_DIR, "German.CallHome.Corpus", "CallHomeWav")
    if os.path.exists(ch_trans_dir):
        cha_files = glob.glob(os.path.join(ch_trans_dir, "*.cha"))
        for cha_p in cha_files:
            fid = os.path.splitext(os.path.basename(cha_p))[0]
            wav_p = os.path.join(ch_wav_dir, f"{fid}.wav")
            if os.path.exists(wav_p):
                try:
                    spk0, spk1 = parse_cha(cha_p)
                    audio, sr = sf.read(wav_p)
                    audio = audio.T
                    chunks = process_and_chunk_duplex(audio, sr, spk0, spk1, "callhome", fid)
                    manifest_entries.extend(chunks)
                except Exception:
                    continue
                    
    # 5. Gemischtes Hack Podcast
    print("Processing Gemischtes Hack Podcast dataset (episodes 150-300)...")
    podcast_trans_dir = os.path.join(RAW_DIR, "Gemischtes.Hack.Podcast", "transcripts")
    podcast_audio_dir = os.path.join(RAW_DIR, "Gemischtes.Hack.Podcast")
    
    if os.path.exists(podcast_trans_dir):
        ep_jsons = sorted(glob.glob(os.path.join(podcast_trans_dir, "episode_*.json")))
        mp3_files = sorted(glob.glob(os.path.join(podcast_audio_dir, "*.mp3")))
        
        for json_p in ep_jsons:
            m = re.search(r"episode_(\d+)_", os.path.basename(json_p))
            if m:
                ep_num = int(m.group(1))
                if 150 <= ep_num <= 300:
                    matched_mp3 = None
                    for mp3 in mp3_files:
                        m_mp3 = re.search(r"^#(\d+)\s", os.path.basename(mp3))
                        if m_mp3 and int(m_mp3.group(1)) == ep_num:
                            matched_mp3 = mp3
                            break
                    if matched_mp3:
                        try:
                            spk0, spk1 = parse_podcast_json(json_p)
                            audio, sr = torchaudio.load(matched_mp3)
                            audio = audio.numpy()
                            if audio.ndim == 1:
                                audio = np.stack([audio, audio])
                                
                            chunks = process_and_chunk_duplex(audio, sr, spk0, spk1, "podcast", f"ep{ep_num}")
                            manifest_entries.extend(chunks)
                        except Exception:
                            continue
                            
    # Build train and valid manifests
    print("Combining all datasets and exporting final train.jsonl and valid.jsonl manifests...")
    # Add pre-processed disfluency samples to manifest
    disfluency_wavs = glob.glob(os.path.join(PROCESSED_DIR, "disfluency", "*.wav"))
    disfluency_entries = []
    for dw in disfluency_wavs:
        try:
            info = sf.info(dw)
            disfluency_entries.append({
                "path": f"/data/disfluency/{os.path.basename(dw)}",
                "duration": info.duration
            })
        except Exception:
            continue
            
    # Add pre-processed medical samples to manifest
    medical_wavs = glob.glob(os.path.join(PROCESSED_DIR, "stereo", "*.wav"))
    med_train = []
    med_valid = []
    for idx, mw in enumerate(sorted(medical_wavs)):
        try:
            info = sf.info(mw)
            item = {
                "path": f"/data/stereo/{os.path.basename(mw)}",
                "duration": info.duration
            }
            if idx < 450:
                med_train.append(item)
            else:
                med_valid.append(item)
        except Exception:
            continue
            
    # Manifest distribution: spontaneous conversations split (95% train, 5% valid)
    np.random.seed(42)
    np.random.shuffle(manifest_entries)
    split_idx = int(len(manifest_entries) * 0.95)
    spont_train = manifest_entries[:split_idx]
    spont_valid = manifest_entries[split_idx:]
    
    final_train = med_train + spont_train
    final_valid = med_valid + spont_valid + disfluency_entries
    
    print(f"Final training samples: {len(final_train)} (Medical: {len(med_train)}, Spontaneous: {len(spont_train)})")
    print(f"Final validation samples: {len(final_valid)} (Medical: {len(med_valid)}, Spontaneous: {len(spont_valid)}, Disfluency: {len(disfluency_entries)})")
    
    # Write manifests
    with open(os.path.join(PROCESSED_DIR, "train.jsonl"), "w", encoding="utf-8") as f:
        for entry in final_train:
            f.write(json.dumps(entry) + "\n")
            
    with open(os.path.join(PROCESSED_DIR, "valid.jsonl"), "w", encoding="utf-8") as f:
        for entry in final_valid:
            f.write(json.dumps(entry) + "\n")
            
    print("Dataset preparation completed successfully!")

if __name__ == "__main__":
    main()
