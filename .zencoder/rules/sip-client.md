---
description: Summary of the SIP Client program
alwaysApply: true
---

# SIP Client

## Overview
The **SIP Client** (`sip-client-main.cpp`) acts as the central RTP gateway for the WhisperTalk system. It manages SIP signaling, handles incoming calls, and coordinates the audio data flow between the telephony network and the internal processing services.

## Internal Function
- **SIP Signaling**: Implements a lightweight SIP stack to register with servers and handle INVITE/BYE requests.
- **Standalone Operation**: Fully decoupled from the system database. Accepts credentials via command-line arguments and maintains internal state for active calls.
- **ID Negotiation**: Communicates with the `InboundAudioProcessor` via a Unix Domain Socket to negotiate a unique `call_num_id` for each call session.
- **Resilient Forwarding**: Forwards RTP packets to the `InboundAudioProcessor` via UDP, prefixed with the 4-byte negotiated `call_num_id`. Automatically dumps streams if the processor is offline and resumes when it returns.
- **Multi-Instance Support**: Multiple SIP client instances can run concurrently, coordinating with the central audio processor to avoid ID collisions.

## Inbound Connections
- **SIP/SDP (Network)**: Receives signaling on port 5060 (UDP).
- **RTP (Network)**: Receives audio packets from the remote party on dynamically negotiated UDP ports.
- **RTP (Internal)**: Receives G.711 audio packets from `OutboundAudioProcessor` via UDP.

## Outbound Connections
- **RTP (Network)**: Sends audio packets to the remote party.
- **Inbound Audio Processor (UDP)**: Sends raw RTP packets to `InboundAudioProcessor` (port 9001), prefixed with a 4-byte big-endian `call_num_id`.
- **Inbound Audio Processor (Unix Socket)**: Connects to `/tmp/inbound-audio-processor.ctrl` for `call_num_id` negotiation.
