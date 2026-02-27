---
description: VAD Service Summary
alwaysApply: true
---

# VAD Service

## Overview
The **VAD Service** (`vad-service.cpp`) performs Voice Activity Detection on the continuous audio stream from IAP and segments it into speech chunks for Whisper transcription.

## Internal Function
- **Energy-based VAD**: Uses 50ms analysis frames with adaptive noise floor tracking. Detects speech onset when energy exceeds `noise_floor * threshold_mult` (default 2.0x) with a minimum energy floor of 0.00005 to reject G.711 codec noise.
- **Micro-pause Detection**: Triggers speech-end after 400ms of silence (8 frames × 50ms), capturing word-boundary pauses instead of waiting for full sentence gaps. This keeps chunks at 1.5-8 seconds for fast Whisper inference.
- **Chunk Constraints**: Maximum 8s chunks (default, configurable via `--vad-max-chunk-ms`), minimum 0.5s (reject noise bursts). RMS energy filter (threshold 0.005) prevents near-silence hallucinations. Smart-split searches for energy dips near chunk boundaries when max length is reached.
- **Inactivity Flush**: If no new audio arrives for 1000ms while speech is active, flushes the buffer immediately.
- **Speech Signal Broadcasting**: Sends SPEECH_ACTIVE/SPEECH_IDLE management messages downstream to coordinate with TTS (e.g., stop playback when caller speaks).
- **Context Preservation**: Includes 6 frames (300ms) of pre-speech context audio for natural chunk boundaries, capturing initial consonants and articles.

## Inbound Connections
- **Inbound Audio Processor (TCP)**: Receives float32 PCM audio (16kHz) on ports 13115 (mgmt) and 13116 (data).

## Outbound Connections
- **Whisper Service (TCP)**: Sends segmented speech chunks (float32 PCM) to Whisper on ports 13120 (mgmt) and 13121 (data).

## Command-Line Parameters
- `--vad-window-ms <ms>`: VAD analysis frame size (default: 50ms, range: 10-500ms)
- `--vad-threshold <mult>`: Energy threshold multiplier over noise floor (default: 2.0, range: 0.5-10.0)
- `--vad-silence-ms <ms>`: Silence duration to trigger speech-end (default: 400ms)
- `--vad-max-chunk-ms <ms>`: Maximum speech chunk duration (default: 8000ms)
