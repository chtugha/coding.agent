---
description: SIP Client Summary
alwaysApply: true
---

# SIP Client

## Overview
The **SIP Client** (`sip-client-main.cpp`) is a standalone C++ program that acts as the RTP gateway for the WhisperTalk system. It handles SIP registration, incoming calls (INVITE), and routes audio between the telephony network and the internal processors.

## Internal Function
- **SIP Signaling**: Implements basic SIP registration and INVITE/BYE handling using raw UDP sockets.
- **RTP Routing**: 
    - Receives RTP packets from the network and forwards them to the `Inbound Audio Processor` via interconnect TCP.
    - Receives encoded G.711 frames from the `Outbound Audio Processor` via interconnect TCP and sends them as RTP packets to the remote caller.
- **Session Management**: Tracks active calls by `Call-ID` and assigns numeric `call_id` for internal routing.
- **RTP Packet Tracking**: Atomic counters for RX/TX packets, bytes, forwarded and discarded counts per call session.
- **Command Port**: Listens on port 13102 for frontend commands (ADD_LINE, GET_STATS).

## Inbound Connections
- **SIP (Network)**: UDP port 5060 (or as configured).
- **RTP (Network)**: Dynamically negotiated UDP ports.
- **OAP (TCP)**: Receives G.711 audio from Outbound Audio Processor via interconnect on ports 13100 (mgmt) and 13101 (data).

## Outbound Connections
- **SIP (Network)**: UDP to SIP server.
- **RTP (Network)**: UDP to remote party.
- **IAP (TCP)**: Sends RTP packets to Inbound Audio Processor on ports 13110 (mgmt) and 13111 (data).

## Command-Line Parameters
- `--user <user>`: SIP username for initial line
- `--server <ip>`: SIP server IP address
- `--port <port>`: SIP server port (default: 5060)
- `--password <pass>`: SIP password for Digest auth
- `--lines <n>`: Number of SIP lines to register
- `--log-level <LEVEL>`: Initial log verbosity (ERROR/WARN/INFO/DEBUG/TRACE, default: INFO)

## Runtime Commands (cmd port 13102)
- `ADD_LINE:<user>:<server>:<port>:<password>`: Register a new SIP account dynamically
- `GET_STATS`: Returns JSON stats (RTP counters) for all active calls
- `PING`: Health check (returns `PONG`)
- `STATUS`: Returns registered lines, active call count, connection state
- `SET_LOG_LEVEL:<LEVEL>`: Change log verbosity at runtime without restart
