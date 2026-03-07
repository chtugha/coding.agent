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
- **Output normalization**: Always normalizes to a 0.90 peak ceiling. Scales both up and down — signals quieter than peak 0.90 are amplified to use the full G.711 dynamic range. Signals with peak < 0.03 (silence) are left untouched to avoid boosting noise.
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
- `STATUS`: Returns model path, upstream/downstream state, active call count, current speed
- `SET_LOG_LEVEL:<LEVEL>`: Change log verbosity at runtime without restart
- `SET_SPEED:<val>`: Change TTS speed multiplier (0.5–2.0, default 1.0). Higher = faster/lighter voice.
- `GET_SPEED`: Returns current speed as `SPEED:<val>`
