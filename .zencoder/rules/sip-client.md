# SIP Client

## Overview
The **SIP Client** (`sip-client-main.cpp`) is a standalone C++ program that acts as the RTP gateway for the WhisperTalk system. It handles SIP registration, incoming calls (INVITE), and routes audio between the telephony network and the internal processors.

## Internal Function
- **SIP Signaling**: Implements basic SIP registration and INVITE/BYE handling using raw UDP sockets.
- **RTP Routing**: 
    - Receives RTP packets from the network and forwards them (with a 4-byte `call_id` prefix) to the `Inbound Audio Processor` via UDP.
    - Receives encoded G.711 frames from the `Outbound Audio Processor` via UDP and sends them as RTP packets to the remote caller.
- **Session Management**: Tracks active calls by `Call-ID` and assigns numeric `call_id` for internal routing.

## Inbound Connections
- **SIP (Network)**: UDP port 5060 (or as configured).
- **RTP (Network)**: Dynamically negotiated UDP ports.
- **Audio (Internal)**: UDP port 9002 (receives audio from `Outbound Audio Processor`).

## Outbound Connections
- **SIP (Network)**: UDP to SIP server.
- **RTP (Network)**: UDP to remote party.
- **Audio (Internal)**: UDP port 9001 (sends audio to `Inbound Audio Processor`).
- **Control (Unix Socket)**: Sends `ACTIVATE:call_id` to `/tmp/*.ctrl` to notify processors of new calls.
