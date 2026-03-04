---
description: Kokoro TTS Service Summary
alwaysApply: true
---

# Kokoro TTS Service

## Overview
The **Kokoro TTS Service** (`kokoro-service.cpp`) is a high-fidelity Text-to-Speech engine optimized for Apple Silicon with CoreML ANE acceleration.

## Internal Function
- **Synthesis**: Uses the Kokoro model with PyTorch/CoreML to convert text into 24kHz float32 PCM audio.
- **Phonemization**: espeak-ng (libespeak-ng) converts text → IPA phonemes. Language auto-detected (German "de" / English "en-us"). Phoneme cache (10000 entries, LRU eviction) avoids repeated espeak-ng calls.
- **Vocab encoding**: KokoroVocab maps phoneme strings → int64 token IDs via greedy longest-match (up to 4 chars, UTF-8 aware) over vocab.json.
- **Two-stage model**: Duration model predicts phoneme durations + alignment tensors; Decoder model generates waveform.
- **Language Support**: Specifically optimized for German and English using espeak-ng for phonemization.
- **Streaming**: Streams synthesized audio chunks directly to the `Outbound Audio Processor`.
- **CoreML Split Decoder**: Uses CoreML Apple Neural Engine acceleration (MLComputeUnitsAll) for the decoder portion of the model when compiled with -DKOKORO_COREML. Provides ~2-4× speedup on M-series chips.
- **Output normalization**: Peaks clipped to 0.95 ceiling to prevent G.711 clipping. Only scales down, never amplifies.
- **SPEECH_ACTIVE handling**: Abandons synthesis and clears output buffer immediately when caller speech is detected.

## Inbound Connections
- **LLaMA Service (TCP)**: Receives response text via interconnect on ports 13140 (mgmt) and 13141 (data).

## Outbound Connections
- **Outbound Audio Processor (TCP)**: Streams float32 PCM audio to OAP on ports 13150 (mgmt) and 13151 (data).

## Command-Line Parameters
- `--model <path>`: Path to Kokoro TorchScript model (default: models/kokoro.pt)
- `--voice <path>`: Path to voice style embedding (default: models/voice.bin)
- `--vocab <path>`: Path to vocab.json (default: models/vocab.json)
- `--log-level <LEVEL>`: Initial log verbosity (ERROR/WARN/INFO/DEBUG/TRACE, default: INFO)

## Runtime Commands (cmd port 13142)
- `PING`: Health check (returns `PONG`)
- `STATUS`: Returns model path, upstream/downstream state, active call count
- `SET_LOG_LEVEL:<LEVEL>`: Change log verbosity at runtime without restart
