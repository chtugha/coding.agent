---
description: Summary of the Database component
alwaysApply: true
---

# Database

## Overview
The **Database** (`database.cpp`, `database.h`) component provides persistent storage for the entire WhisperTalk system using SQLite3.

## Internal Function
- **Schema Management**: Defines and maintains tables for `callers`, `calls`, `sip_lines`, and system configuration.
- **Data Persistence**: Stores call logs, transcriptions, LLM responses, and service statuses.
- **State Management**: Tracks which calls are currently active and allows services to coordinate state across restarts.
- **Configuration Storage**: Holds system-wide settings like "system speed" and model paths.

## Inbound Connections
- **Service API (Direct)**: Accessible via direct C++ method calls by any service linked with `database.o`.

## Outbound Connections
- **SQLite3 File**: Writes data to `whisper_talk.db` on the local filesystem.
