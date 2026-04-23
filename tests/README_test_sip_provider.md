# Test SIP Provider (B2BUA)

A standalone Back-to-Back User Agent for end-to-end testing of the Prodigy pipeline. Two SIP clients connect to this provider, which bridges them so their full pipelines can have a conversation with each other.

## Architecture

```
Pipeline A                                          Pipeline B
┌──────────┐    RTP     ┌──────────────────┐    RTP     ┌──────────┐
│SIP Client├───────────►│  Test SIP Prov.  ├───────────►│SIP Client│
│  (alice) │◄───────────┤  (B2BUA + RTP    │◄───────────┤  (bob)   │
└────┬─────┘    relay   │   relay)         │    relay   └────┬─────┘
     │                  └──────────────────┘                 │
     ▼                                                       ▼
   IAP → VAD → Whisper → LLaMA → TTS → OAP      IAP → VAD → Whisper → LLaMA → TTS → OAP
   (TTS = tts-service dock + docked Kokoro or NeuTTS engine)
```

### SIP Flow

```
Client A                  Provider                  Client B
   │                         │                         │
   │──── REGISTER ──────────►│                         │
   │◄──── 200 OK ───────────│                         │
   │                         │◄──── REGISTER ──────────│
   │                         │──── 200 OK ────────────►│
   │                         │                         │
   │◄──── INVITE (SDP) ─────│──── INVITE (SDP) ──────►│
   │──── 200 OK (SDP) ──────►│◄──── 200 OK (SDP) ─────│
   │◄──── ACK ───────────────│──── ACK ───────────────►│
   │                         │                         │
   │◄════ RTP bidirectional relay via UDP sockets ════►│
   │                         │                         │
   │──── BYE ───────────────►│──── BYE ───────────────►│
   │◄──── 200 OK ───────────│◄──── 200 OK ────────────│
```

## Files

| File | Description |
|------|-------------|
| `test_sip_provider.cpp` | Standalone B2BUA binary (680 lines). Not a GTest — runs as a long-lived process. |
| `test_sip_provider_unit.cpp` | GTest unit tests (249 lines, 25 tests) for parsing and encoding functions. |

## Usage

### Basic End-to-End Test

```bash
# Terminal 1: Start the provider
./bin/test_sip_provider --port 5060 --duration 60 --inject

# Terminal 2: Start pipeline A (all 6 services for alice)
./bin/sip-client alice 127.0.0.1 5060

# Terminal 3: Start pipeline B (all 6 services for bob)
./bin/sip-client bob 127.0.0.1 5060
```

The provider waits for both clients to REGISTER, then sends INVITE to each. When both answer with 200 OK, bidirectional RTP relay begins.

### CLI Options

| Option | Default | Description |
|--------|---------|-------------|
| `-p, --port PORT` | `5060` | SIP listen port (UDP) |
| `-d, --duration SECS` | `30` | Call duration before auto-hangup |
| `-i, --inject` | off | Inject a 3-second 400Hz G.711 mu-law test tone into Leg A to kick-start the pipeline without requiring a real microphone |
| `-b, --ip ADDR` | `127.0.0.1` | Local IP address used in SIP/SDP headers |
| `-h, --help` | | Show help |

### With Audio Injection

The `--inject` flag generates a 400Hz sine wave encoded as G.711 mu-law (PCMU, payload type 0) and sends it to Leg A's RTP port after a 500ms delay. This simulates inbound speech so the pipeline processes audio without needing a real audio source. The tone is 3 seconds long (150 RTP packets at 20ms ptime).

## Unit Tests

Run with:

```bash
./bin/test_sip_provider_unit
```

### Test Suites

| Suite | Tests | What It Covers |
|-------|-------|----------------|
| `UlawEncodingTest` | 6 | G.711 mu-law: silence (0xFF), positive/negative range, sign symmetry, clipping, sine wave variation |
| `SipHeaderTest` | 6 | SIP header extraction: basic, leading spaces, missing header, end-of-message, LF-only line endings, multiple headers |
| `SdpParsingTest` | 8 | SDP media port and connection IP parsing: valid ports, multi-codec, missing lines, invalid ports (0, >65535), end-of-line |
| `RtpPacketTest` | 2 | RTP header field layout (version, seq, timestamp, SSRC in network byte order), payload size (12 + 160 = 172) |
| `SipUsernameExtraction` | 3 | Username parsing from SIP URI: basic, with tag parameter, numbered lines |

## Output

The provider prints a results summary when the call ends:

```
=========================================
   Test SIP Provider — Call Results
=========================================
  Duration:     60s
  Leg A:        alice (RTP 127.0.0.1:10001)
  Leg B:        bob (RTP 127.0.0.1:10002)
-----------------------------------------
  A -> B:       2500 packets  (430 KB)
  B -> A:       2400 packets  (413 KB)
-----------------------------------------
  Result:       PASS (bidirectional audio)
=========================================
```

| Result | Meaning |
|--------|---------|
| **PASS** | RTP packets flowed in both directions (A->B and B->A) |
| **PARTIAL** | RTP packets flowed in only one direction |
| **FAIL** | No RTP packets were relayed |

## Limitations

- Single call only (one pair of clients at a time)
- No SIP authentication or digest challenge
- Minimal SDP parsing (extracts `m=audio` port and `c=IN IP4` address only)
- No codec negotiation (assumes PCMU/8000)
- No SRTP or TLS (plaintext UDP only)
- Test-only tool, not a production SIP server
