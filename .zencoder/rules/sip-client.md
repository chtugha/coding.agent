---
description: SIP Client Summary
alwaysApply: true
---

# SIP Client

## Overview
The **SIP Client** (`sip-client-main.cpp`) is a standalone C++ program that acts as the RTP gateway for the Prodigy system. It handles SIP registration, incoming calls (INVITE), and routes audio between the telephony network and the internal processors.

## Internal Function
- **SIP Signaling**: Implements SIP registration (with Digest MD5 authentication) and INVITE/BYE handling using raw UDP sockets. Sends `100 Trying` before `200 OK` on incoming INVITEs. NOTIFY/OPTIONS/CANCEL get `200 OK` responses to satisfy PBX keepalive probes.
- **Re-Registration**: `registration_loop` fires at `max(60s, granted_expires * 2/3)` (typically ~2400s for a 3600s Expires). The `Expires:` value is parsed from the REGISTER 200 OK and stored in `SipLine::granted_expires`. On each re-registration, preemptive auth is attempted if credentials are already known (avoids a 401 round-trip); if the stored nonce has expired the PBX issues a fresh 401 and the challenge handler automatically retries.
- **Local IP Auto-Detection**: Uses the connected UDP socket trick (`connect()` + `getsockname()`) to determine the local network IP that routes to each PBX. Each line stores its own `local_ip` for multi-PBX scenarios. This IP is used in Contact headers, Via headers, and SDP `c=` lines sent in 200 OK responses to INVITE.
- **Digest Authentication**: Handles `401 Unauthorized` / `407 Proxy Authentication Required` challenges by parsing `WWW-Authenticate`/`Proxy-Authenticate` headers, computing MD5 digest response (`HA1=MD5(user:realm:password)`, `HA2=MD5(method:uri)`, `response=MD5(HA1:nonce:HA2)`), and re-sending REGISTER with `Authorization` header.
- **SDP Media IP Parsing**: Parses the `c=IN IP4 <addr>` line from INVITE SDP to determine `remote_ip` for outgoing RTP. Falls back to the UDP sender address if no `c=` line is present (handles PBX media proxies correctly).
- **Unique SIP IDs**: Via branch tags, registration Call-IDs, and RTP SSRCs use `arc4random()` (OS-seeded CSPRNG) for guaranteed global uniqueness across process restarts, satisfying RFC 3261 §8.1.1.7.
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
- `--lines <n>`: Number of SIP lines to register at startup (**default: 0**; when 0, no positional `<user>` or `<server>` args are required and the client starts with no registered lines)
- `--log-level <LEVEL>`: Initial log verbosity (ERROR/WARN/INFO/DEBUG/TRACE, default: INFO)

## Runtime Commands (cmd port 13102)
- `ADD_LINE <user> <server_ip> <port> <password>`: Register a new SIP account dynamically (password last to allow spaces, "-" means empty, port 1-65535 defaults to 5060)
- `REMOVE_LINE <index>`: Remove a SIP line by index
- `LIST_LINES`: Returns `LINES <idx>:<user>:<registered|unregistered>:<server_ip>:<port>:<local_ip> ...`
- `GET_STATS`: Returns RTP counters for all active calls
- `PING`: Health check (returns `PONG`)
- `STATUS`: Returns registered lines, active call count, connection state
- `SET_LOG_LEVEL:<LEVEL>`: Change log verbosity at runtime without restart
