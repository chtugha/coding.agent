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

## Pipeline role (post 2026-04)
Kokoro is **not** a pipeline node. It is a dock client of the generic TTS stage (`tts-service`). It opens a TCP connection to `127.0.0.1:13143`, sends a one-line JSON HELLO (`{"name":"kokoro","sample_rate":24000,"channels":1,"format":"f32le"}`), and receives `OK\n`. Thereafter the dock forwards LLaMA text to Kokoro and Kokoro's audio frames back to OAP. On a `CUSTOM SHUTDOWN` mgmt frame from the dock (issued when a different engine wins the slot) Kokoro joins its synthesis workers and exits via `std::_Exit(0)`.

## Inbound / Outbound Connections
- **TTS dock (TCP, loopback)**: single socket on port 13143. Tag-prefixed frames carry LLaMA text in and PCM audio out. Mgmt signals (CALL_END, SPEECH_ACTIVE, SPEECH_IDLE, CUSTOM SHUTDOWN) arrive on the same socket.

## Command-Line Parameters
- `--voice <name>`: Voice (`df_eva`, `dm_bernd`)
- `--log-level <LEVEL>`: Initial log verbosity (ERROR/WARN/INFO/DEBUG/TRACE, default: INFO)

## Runtime Commands (cmd port 13144)
- `PING`: Health check (returns `PONG`)
- `STATUS`: Returns model path, upstream/downstream state, active call count, current speed
- `SET_LOG_LEVEL:<LEVEL>`: Change log verbosity at runtime without restart
- `SET_SPEED:<val>`: Change TTS speed multiplier (0.5–2.0, default 1.0). Higher = faster/lighter voice.
- `GET_SPEED`: Returns current speed as `SPEED:<val>`
