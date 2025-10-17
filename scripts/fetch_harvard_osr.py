#!/usr/bin/env python3
import os
import re
import sys
import time
import html
from urllib.parse import urljoin
from urllib.request import urlopen, Request

OSR_URL = "https://www.voiptroubleshooter.com/open_speech/american.html"
HARVARD_URL = "https://www.cs.columbia.edu/~hgs/audio/harvard.html"
ROOT = "/Users/whisper/Documents/augment-projects/clean-repo"
DATA_DIR = os.path.join(ROOT, "tests", "data", "harvard")
WAV_DIR = os.path.join(DATA_DIR, "wav")
REF_TSV = os.path.join(DATA_DIR, "harvard_references.tsv")

os.makedirs(WAV_DIR, exist_ok=True)


def fetch(url: str) -> str:
    req = Request(url, headers={"User-Agent": "Mozilla/5.0 (AugmentAgent)"})
    with urlopen(req, timeout=30) as r:
        return r.read().decode("utf-8", errors="ignore")


def download(url: str, dst_path: str):
    if os.path.exists(dst_path) and os.path.getsize(dst_path) > 0:
        print(f"exists: {os.path.basename(dst_path)}")
        return
    req = Request(url, headers={"User-Agent": "Mozilla/5.0 (AugmentAgent)"})
    with urlopen(req, timeout=60) as r:
        data = r.read()
    with open(dst_path, "wb") as f:
        f.write(data)
    print(f"downloaded: {os.path.basename(dst_path)} ({len(data)} bytes)")


def parse_osr_wavs(html_text: str):
    # Find links to OSR_us_000_XXXX_8k.wav and capture XXXX
    links = [m for m in re.finditer(r'href=["\']([^"\']*OSR_us_000_(\d{4})_8k\.wav)["\']', html_text, flags=re.I)]
    seen = set()
    out = []
    for m in links:
        href = m.group(1)
        num = m.group(2)
        if num in seen:
            continue
        seen.add(num)
        out.append((href, num, m.start(), m.end()))
    out.sort(key=lambda x: int(x[1]))
    return out


def parse_harvard_sentences(html_text: str):
    # Extract <li> items in order as sentences; there should be ~720
    def strip_tags(s: str) -> str:
        s = re.sub(r"<script[\s\S]*?</script>", " ", s, flags=re.I)
        s = re.sub(r"<style[\s\S]*?</style>", " ", s, flags=re.I)
        s = re.sub(r"<[^>]+>", " ", s)
        s = html.unescape(s)
        s = re.sub(r"\s+", " ", s)
        return s.strip()
    lis = re.findall(r"<li[^>]*>([\s\S]*?)</li>", html_text, flags=re.I)
    sentences = []
    for li in lis:
        t = strip_tags(li).strip()
        if len(t.split()) >= 3:
            sentences.append(t)
    if len(sentences) >= 720:
        sentences = sentences[:720]
    return sentences


def main():
    print("Fetching OSR index...")
    osr_html = fetch(OSR_URL)
    wav_entries = parse_osr_wavs(osr_html)
    if not wav_entries:
        print("ERROR: no OSR wav links found", file=sys.stderr)
        sys.exit(2)
    print(f"Found {len(wav_entries)} wav entries")

    # Download WAVs
    base = OSR_URL
    for href, num, *_ in wav_entries:
        url = urljoin(base, href)
        fname = f"OSR_us_000_{num}_8k.wav"
        dst = os.path.join(WAV_DIR, fname)
        try:
            download(url, dst)
        except Exception as ex:
            print(f"WARN: failed {fname}: {ex}")
            time.sleep(0.25)

    # Build mapping TSV for files we have based on Harvard list numbers
    print("Fetching Harvard sentences page...")
    harv_html = fetch(HARVARD_URL)
    sentences = parse_harvard_sentences(harv_html)
    if not sentences or len(sentences) < 10:
        print("ERROR: insufficient Harvard sentences parsed", file=sys.stderr)
        sys.exit(3)
    print(f"Parsed {len(sentences)} sentences from Harvard page")

    # Format: filename\tindex\tsentence
    lines = ["# filename\tindex\tsentence"]
    count = 0
    for _, num, *_ in wav_entries:
        N = int(num)
        # Empirical mapping: OSR 0010 -> Harvard List 1, so offset by -9
        L = N - 9
        fname = f"OSR_us_000_{num}_8k.wav"
        if L <= 0 or (L - 1) * 10 >= len(sentences):
            lines.append(f"{fname}\t1\t")
            continue
        start = (L - 1) * 10
        for i in range(10):
            s = sentences[start + i]
            lines.append(f"{fname}\t{i+1}\t{s}")
            count += 1

    with open(REF_TSV, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print(f"Wrote mapping: {REF_TSV} ({count} sentence rows)")

    print("Done.")


if __name__ == "__main__":
    main()

