# WhisperTalk Frontend User Guide

## Overview

The WhisperTalk Frontend is a web-based control plane for the WhisperTalk real-time speech-to-speech telephony system. It runs locally at `http://localhost:8080` and provides:

- A **Dashboard** with real-time pipeline visualization and system health
- **Service management** for all 7 pipeline microservices
- **Test infrastructure** for running, monitoring, and analyzing tests
- **Configuration** for models, database, and credentials
- **Live log streaming** from all services

## Starting the Frontend

From the project root:

```bash
bin/frontend
```

Or from the `bin/` directory:

```bash
cd bin && ./frontend
```

The frontend resolves the project root automatically via symlink resolution and `chdir()` logic. It will bind to `http://127.0.0.1:8080` (localhost only).

### Custom Port

```bash
bin/frontend --port 9090
```

### Troubleshooting Startup

| Error | Cause | Fix |
|-------|-------|-----|
| `Fatal: database is read-only: /path/frontend.db` | `frontend.db` has wrong permissions or ownership | Check `ls -la frontend.db` — file must be owned by the user running `bin/frontend`. Fix with `chown $(whoami) frontend.db && chmod 644 frontend.db`, or delete it (will be recreated) |
| `Fatal: bin/frontend not found in project root` | CWD is wrong; frontend can't find itself | Run from project root: `bin/frontend` |
| `FATAL: Log port 22022 is already in use` | Another frontend instance is running | Kill the other instance |

## Navigation

The sidebar is organized into four sections:

### Dashboard (Home)

The default landing page. Shows:

- **Pipeline Overview**: A horizontal node chain (SIP > IAP > VAD > ASR > LLM > TTS > OAP) with live status dots. Green = online, grey = offline, red = error.
- **Health Badge**: "Healthy" (all services up), "Degraded" (some down), or "Offline" (most/all down).
- **Metric Cards**: Services Online, Running Tests, Tests Passed (with fail count), and Uptime.
- **Activity Feed**: Last 10 log entries from all services.
- **Quick Actions**: Start All Services, Stop All Services, Restart Failed.

Data refreshes every 3 seconds while the dashboard is visible.

### Pipeline Section

**Services**: View and manage all pipeline services. Each service card shows status, PID, uptime. Actions: Start, Stop, Restart. Click a service for detailed config (arguments, log level).

**Live Logs**: Real-time log stream via Server-Sent Events. Filter by service using the dropdown. Logs auto-scroll with the latest entries.

### Testing Section

**Test Runner**: Lists available test binaries (test_sanity, test_interconnect, etc.). Click a test to configure arguments and run it. Live output streams to the log view. History of past runs shown below.

**Test Results**: Aggregated view across all test types with:
- Summary metric cards (Total Tests, Pass Rate, Average Latency)
- Chart.js trend chart with filters (test type, date range)
- Sortable results table with status badges

**Beta Tests**: Organized into three tabs:
- **Component Tests**: SIP RTP Routing, IAP Codec Quality, Whisper Accuracy, LLaMA Quality, Kokoro TTS Quality
- **Pipeline Tests**: Shut-Up Mechanism, Full Pipeline Round-Trip, Pipeline Resilience, Stress Tests
- **Tools**: Test Audio Files browser, Audio Injection, SIP Lines Management

Each test panel is collapsible (click the header to expand/collapse).

### Configuration Section

**Models**: Browse registered models, add new ones, run benchmarks, search/download from HuggingFace.

**Database**: Execute SQL queries against the SQLite database. Read-only by default; toggle write mode for INSERT/UPDATE/DELETE. View schema.

**Credentials**: Manage SIP line credentials and API tokens.

## Theme Switching

Click the palette icon at the bottom of the sidebar to choose a theme:

- **Default**: Light Apple-inspired design
- **Dark**: Dark mode with adjusted surfaces and shadows
- **Slate**: Bootstrap Slate theme
- **Flatly**: Bootstrap Flatly theme
- **Cyborg**: Bootstrap Cyborg theme

The selected theme persists in the SQLite `settings` table.

## Test Results Page

### Filtering

Use the filter bar above the results table:
- **Test Type**: Filter by service_test, whisper_accuracy, model_benchmark, iap_quality
- **Date Range**: From/To date inputs to narrow the time window

### Chart

The trend chart shows test results over time. Hover for details. The chart uses Chart.js with zoom support (scroll to zoom, drag to pan).

### Table

Sortable columns: Type, Service, Status, Timestamp. Status badges are color-coded: green (pass), red (fail), yellow (warn).

## Beta Tests Page

### Running a Component Test

1. Navigate to **Beta Tests** > **Component Tests** tab
2. Click a test panel header to expand it
3. Configure parameters if needed (e.g., select audio file, choose model)
4. Click **Run Test**
5. Results appear in-panel with metrics and status

### Running a Pipeline Test

1. Navigate to **Beta Tests** > **Pipeline Tests** tab
2. Select a test (e.g., Full Pipeline Round-Trip)
3. Ensure required services are running (check Dashboard)
4. Click **Run Test**
5. Monitor progress via the live status display

### Tools Tab

- **Test Audio Files**: Browse WAV+TXT pairs in `Testfiles/`. Scan for new files.
- **Audio Injection**: Inject audio into active SIP calls via test_sip_provider.
- **SIP Lines**: Add/remove SIP accounts in a grid layout (up to 20 lines).

## Verifying Services Are Running

1. Check the **Dashboard** pipeline visualization — green dots indicate running services
2. Or go to **Services** page — each card shows "Running" or "Stopped"
3. The status bar at the bottom of the sidebar shows "N services" count
