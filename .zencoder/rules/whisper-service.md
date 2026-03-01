---
description: Whisper Service Summary
alwaysApply: true
---

# Whisper Service

## Overview
The **Whisper Service** (`whisper-service.cpp`) performs Automatic Speech Recognition (ASR) on pre-segmented speech chunks received from the VAD Service.

## Internal Function
- **Inference**: Uses `whisper.cpp` optimized for Apple Silicon via **CoreML/Metal**, providing near-real-time transcription. Receives complete speech segments (typically 1-8 seconds) — no VAD logic is performed here.
- **Decoding**: GREEDY strategy (not beam search). On short 2-8s segments, greedy is ~3-5x faster than beam_size=5 with negligible accuracy difference. Temperature fallback handles errors (temp_inc=0.2).
- **Telephony-Optimized Params**: `no_speech_thold=0.9` (prevents early decoder stop on G.711 audio), `entropy_thold=2.8` (tolerant of telephony uncertainty). No `initial_prompt` — prompts cause hallucination artifacts on codec-degraded audio.
- **No Normalization**: Audio is passed directly to Whisper without peak normalization, matching whisper-cli default behavior for optimal transcription accuracy.
- **Hallucination Filter (runtime-toggleable, default OFF)**: Detects and filters common Whisper hallucination patterns (e.g., "Untertitel", "Copyright", "Musik") using exact-match comparison. Also detects repetitive text patterns. Includes trailing hallucination stripping that iteratively removes known suffixes (e.g., "Untertitelung des ZDF", "Hier geht's") using word-boundary-aware matching. Toggled at runtime via `HALLUCINATION_FILTER:ON/OFF` commands on cmd port 13122, or via the frontend Whisper Configuration checkbox.
- **RMS Energy Check**: Rejects chunks with RMS < 0.005 as near-silence to prevent hallucinations.
- **Packet Buffering**: If LLaMA is disconnected, buffers up to 64 transcription packets and drains them when reconnected.

## Inbound Connections
- **VAD Service (TCP)**: Receives pre-segmented float32 PCM audio chunks on ports 13120 (mgmt) and 13121 (data).

## Outbound Connections
- **LLaMA Service (TCP)**: Sends transcribed text to the `LLaMA Service` on ports 13130 (mgmt) and 13131 (data).

## Command-Line Parameters
- `--language <lang>` / `-l <lang>`: Whisper language code (default: "de")
- `--model <path>` / `-m <path>`: Path to Whisper GGML model file (default: models/ggml-large-v3-turbo-q5_0.bin)

## Runtime Commands (cmd port 13122)
- `HALLUCINATION_FILTER:ON` / `HALLUCINATION_FILTER:OFF`: Enable/disable hallucination filtering at runtime (default: OFF)
- `HALLUCINATION_FILTER:STATUS`: Query current filter state (returns `HALLUCINATION_FILTER:ON` or `HALLUCINATION_FILTER:OFF`)
- `STATUS`: Returns model name, upstream/downstream connection state, and hallucination filter state
- `PING`: Health check (returns `PONG`)
