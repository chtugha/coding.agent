# tomedo-crawl — RAG Sidecar Service

## Overview

`tomedo-crawl` is a standalone C++17 binary that adds **Retrieval-Augmented Generation (RAG)** to the Prodigy telephony pipeline.  When a call arrives, the LLaMA service queries tomedo-crawl to fetch the matching patient's medical context (diagnoses, medications, appointments, phone number).  LLaMA then prepends this context to the system prompt so the AI assistant can greet the caller by name and give medically-informed responses.

The service is a **sidecar** — it is not wired into the audio pipeline graph.  All communication with other services is via its own HTTP REST API on port `13181`.

---

## Architecture

```
                         ┌──────────────────────────────────────┐
   [Tomedo EMR server]   │          tomedo-crawl (port 13181)   │
   192.168.10.9:8443     │                                       │
         │               │  ┌──────────┐   ┌─────────────────┐  │
   mTLS HTTPS ──────────►│  │ Crawler  │──►│  VectorStore    │  │
                         │  └──────────┘   │  SQLite + HNSW  │  │
   [Ollama / embeddings] │        │        └────────┬────────┘  │
   127.0.0.1:11434       │        │ phones          │            │
         │               │  ┌─────▼──────┐   /query (ANN)       │
   HTTP POST ◄───────────│  │ PhoneIndex │                       │
                         │  │  SQLite    │                       │
                         │  └─────▲──────┘                      │
                         │        │ /caller                      │
                         └────────┼─────────────────────────────┘
                                  │ HTTP (loopback)
                         ┌────────┴──────────────┐
                         │  sip-client  (INVITE)  │  → /caller POST
                         │  llama-service          │  → /caller GET, /query GET
                         └───────────────────────┘
```

### Key design decisions

| Concern | Solution |
|---|---|
| HTTP server | `mongoose.h` (shared with `frontend.cpp`) |
| Tomedo API client | POSIX sockets + OpenSSL mutual TLS |
| Vector storage | `hnswlib` (header-only HNSW, in-memory) + SQLite (persistent BLOB) |
| Embedding | HTTP POST to Ollama `/api/embeddings` |
| Phone lookup | Local SQLite `phone_index` table (no server-side search endpoint) |
| Configuration | Encrypted SQLite `config` table (no INI file) |
| Security | SQLCipher-encrypted database, mTLS to Tomedo, loopback-only HTTP listener |
| Ollama management | Spawns/monitors `ollama serve` as a child process |

---

## Port Map

| Port | Purpose |
|------|---------|
| **13180** | Interconnect mgmt (reserved; not currently used by pipeline) |
| **13181** | HTTP REST API (sip-client, llama-service, frontend) |
| **13182** | Interconnect cmd (reserved) |

---

## HTTP API Reference

All responses are `application/json`.  The listener binds to `127.0.0.1` (loopback only) by default.

### `GET /health`

Returns service status including Ollama state and indexed document count.

```json
{
  "status": "ok",
  "indexed_docs": 42500,
  "index_usage_pct": 8,
  "ollama_installed": true,
  "ollama_running": true,
  "last_crawl": 1744567200
}
```

| Field | Type | Description |
|-------|------|-------------|
| `indexed_docs` | int | Number of text chunks in the vector store |
| `index_usage_pct` | int | Percentage of HNSW capacity used (max 500 000 by default) |
| `last_crawl` | int/null | Unix timestamp of the last completed crawl, or null |

---

### `POST /caller`

Called by sip-client when an inbound INVITE is received.  Triggers an asynchronous phone lookup.

**Request body:**
```json
{"call_id": 42, "phone_number": "07383-942735"}
```

**Response:** `202 Accepted`

The lookup runs in the background (`ResolveQueue`).  The caller status transitions from `pending` → `found`/`not_found`/`error` within ~100 ms (local SQLite query).

---

### `GET /caller/{call_id}`

Poll the identity lookup result for a call.

**Response:**
```json
{
  "call_id": 42,
  "status": "found",
  "name": "Kunsch",
  "vorname": "Lothar",
  "patient_id": 776
}
```

| `status` | Meaning |
|----------|---------|
| `pending` | Lookup in progress |
| `found` | Patient identified; `name`, `vorname`, `patient_id` are populated |
| `not_found` | No matching phone number in the index |
| `error` | Lookup failed (queue overflow or internal error) |

---

### `DELETE /caller/{call_id}`

Removes the caller record.  Called by sip-client on call hangup.  Returns `204 No Content`.

---

### `GET /query` or `POST /query`

Semantic search against the vector store.  The request text is embedded via Ollama and the top-K nearest chunks are returned.

**GET parameters** (or POST JSON body):

| Parameter | Default | Description |
|-----------|---------|-------------|
| `text` | required | Query string to embed and search |
| `top_k` | `3` | Number of results to return |
| `patient_id` | `-1` | Optional patient filter (post-filter after ANN) |

**Response:**
```json
{
  "results": [
    {
      "text": "Patient: Lothar Kunsch (ID 776), geb. 15.09.1954\nDiagnosen: Hypertonie...",
      "source": "patient/776",
      "patient_id": 776,
      "score": 0.142
    }
  ]
}
```

`score` is the L2 distance from the query vector (lower = more similar).

Returns `503` if Ollama is unreachable (embedding failed).

---

### `POST /crawl/trigger`

Requests an immediate crawl.  The crawl thread picks up the flag within 1 second.  Returns `202`.

---

### `POST /vectors/wipe`

Deletes all chunks from SQLite and wipes the hnswlib index.  Returns `200`.  A full crawl is required afterwards to repopulate.

---

### `GET /ollama/status`

Reports whether Ollama is installed and whether `ollama serve` is currently running, plus the active embedding model.

**Response:**
```json
{"installed": true, "running": true, "model": "nomic-embed-text:latest"}
```

---

### `POST /ollama/install`

Triggers a background install of the Ollama runtime via the platform installer (Homebrew on macOS).  Returns `202 Accepted`; progress is logged via the standard log forwarder.

---

### `POST /ollama/start` / `POST /ollama/stop`

Start or stop the `ollama serve` process managed by tomedo-crawl.

---

### `GET /config`

Returns all configuration keys and their current values from the encrypted SQLite `config` table.

---

### `POST /config`

Write one or more configuration keys.  Changes take effect on the next service restart (except `crawl_interval_sec` and `ollama_*` which are read dynamically where possible).

**Request body:**
```json
{
  "tomedo_host": "192.168.10.9",
  "tomedo_port": "8443",
  "ollama_model": "nomic-embed-text"
}
```

---

## Configuration Reference

All configuration is stored in the encrypted SQLite database (`tomedo-crawl.db` by default) in the `config` table.  The frontend writes to this table via `/api/rag/config`.

| Key | Default | Description |
|-----|---------|-------------|
| `tomedo_host` | `192.168.10.9` | Tomedo server hostname or IP |
| `tomedo_port` | `8443` | Tomedo HTTPS port |
| `tomedo_db` | `tomedo_live` | Tomedo database name (path prefix) |
| `tomedo_cert_pem` | `/etc/tomedo-crawl/client.pem` | Path to the mTLS client certificate PEM (cert + key) |
| `crawl_interval_sec` | `3600` | Seconds between automatic crawls (frontend converts daily-time to seconds) |
| `ollama_url` | `http://127.0.0.1:11434` | Base URL for Ollama HTTP API |
| `ollama_model` | `embeddinggemma:300m` | Embedding model name (must be pulled before first crawl) |
| `api_host` | `127.0.0.1` | Interface for the HTTP server to bind on |
| `api_port` | `13181` | HTTP server port |
| `log_port` | `22022` | Frontend UDP log port |
| `hnsw_max_elements` | `500000` | Maximum vectors the HNSW index can hold |

---

## Database Schema

The SQLite database (`tomedo-crawl.db`) is encrypted with SQLCipher.  The key is derived from the platform identity via `db_key.h` (Apple Keychain on macOS).

### `config` table

```sql
CREATE TABLE config (
    key   TEXT PRIMARY KEY NOT NULL,
    value TEXT NOT NULL
);
```

### `chunks` table (vector store)

```sql
CREATE TABLE chunks (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    source     TEXT    NOT NULL,       -- e.g. "patient/776"
    patient_id INTEGER,
    text       TEXT    NOT NULL,       -- the raw text chunk
    embedding  BLOB    NOT NULL,       -- raw float32 array, little-endian
    updated_at INTEGER NOT NULL        -- Unix timestamp
);
CREATE INDEX idx_patient ON chunks(patient_id);
CREATE UNIQUE INDEX idx_source_patient ON chunks(source, patient_id);
```

### `phone_index` table

```sql
CREATE TABLE phone_index (
    id         INTEGER PRIMARY KEY AUTOINCREMENT,
    phone      TEXT    NOT NULL,       -- digit-only normalised number
    patient_id INTEGER NOT NULL,
    name       TEXT,
    vorname    TEXT
);
CREATE INDEX idx_phone_digits ON phone_index(phone);
CREATE UNIQUE INDEX idx_phone_patient ON phone_index(phone, patient_id);
```

---

## Tomedo API Integration

### Authentication

Tomedo uses **mutual TLS** (client certificate).  The macOS Tomedo client installs a self-signed RSA-4096 certificate pair in the user's Keychain on first server connection.

Export procedure (one-time, run on the Mac where the Tomedo client is installed):

```bash
# Export identity from Keychain as PKCS#12
security export \
  -k ~/Library/Keychains/login.keychain-db \
  -t identities -f pkcs12 -P "" \
  -o /tmp/tomedo_client.p12

# Convert to PEM (cert + private key in one file, no password)
openssl pkcs12 -legacy \
  -in /tmp/tomedo_client.p12 -nodes -passin pass:"" \
  -out /etc/tomedo-crawl/client.pem

chmod 600 /etc/tomedo-crawl/client.pem
```

Upload the PEM via the frontend (Services → TOMEDO_CRAWL → Client Certificate field) or set `tomedo_cert_pem` in the config table.

### Confirmed Endpoints (probed 2026-04-11)

| Method | Path | Description |
|--------|------|-------------|
| GET | `/tomedo_live/serverstatus` | Server health check |
| GET | `/tomedo_live/patient?flach=true` | Flat patient list (~15 k records, no phone data) |
| GET | `/tomedo_live/patient/{id}` | Full patient record including phone numbers |
| GET | `/tomedo_live/patient/{id}/patientenDetailsRelationen?...` | Diagnoses, Kartei, Behandlungsfälle |
| GET | `/tomedo_live/patient/{id}/patientenDetailsRelationen/medikamentenPlan` | Medications |
| GET | `/tomedo_live/patient/{id}/termine?flach=true` | Appointments |

> **Note:** `GET /patient/searchByAttributes?query=...&telefonNummern=true` was confirmed to return an empty dict — server-side phone search does **not** work.  Phone lookup is done entirely from the local `phone_index` table.

### Phone Number Fields (per-patient record)

| JSON path | Meaning |
|-----------|---------|
| `patientenDetails.kontaktdaten.telefon` | Main phone (may contain `\n`-separated entries) |
| `patientenDetails.kontaktdaten.telefon2` | Secondary phone |
| `patientenDetails.kontaktdaten.handyNummer` | Mobile |
| `patientenDetails.kontaktdaten.telefon3` | Tertiary phone |
| `patientenDetails.kontaktdaten.weitereTelefonummern[]` | Additional numbers |

---

## Crawl Pipeline

```
enumerate_patients()        ← GET /patient?flach=true (one call, ~15k records)
    │
    ▼  for each patient (batches of 100, 10ms sleep between batches)
fetch_patient_context_full()
    ├─ GET /patient/{id}                          (phones + contact data)
    ├─ GET /patient/{id}/patientenDetailsRelationen (diagnoses)
    ├─ GET /patient/{id}/...medikamentenPlan       (medications)
    └─ GET /patient/{id}/termine?flach=true        (appointments)
    │
    ▼
compose natural-language document
    │
    ├─► phone_index_upsert()   (phone digits → SQLite phone_index)
    │
    ▼
chunk_text()     (512-token windows, 64-token overlap, sentence-boundary splits)
    │
    ▼  for each chunk
embed_text()     (POST http://127.0.0.1:11434/api/embeddings)
    │
    ▼
VectorStore::upsert()  (SQLite BLOB + hnswlib addPoint)
```

### Incremental Crawl

After a clean first crawl, subsequent crawls only re-fetch patients whose `zuletztAufgerufen` (last-accessed timestamp) is newer than `since_ts` (the epoch-ms timestamp when the previous crawl started).  The `since_ts` cursor is only advanced when:
- The crawl completed without HTTP errors (`skipped == 0`)
- The crawl was not interrupted by SIGTERM

This prevents patients from being silently skipped after a partial crawl.

---

## Embedding Model

The default embedding model is `embeddinggemma:300m`.  To use `nomic-embed-text` (768-dim, higher quality):

1. Pull the model: `ollama pull nomic-embed-text`
2. Update the config: set `ollama_model = nomic-embed-text` via the frontend
3. Click **Wipe Vectors** in the frontend (or `POST /vectors/wipe`) to clear the old embeddings
4. Trigger a fresh crawl

> **Important:** Changing the embedding model **requires wiping the vector store** because the dimensions change.  tomedo-crawl detects a model change at startup and automatically wipes the store before the first crawl.

---

## Ollama Management

tomedo-crawl owns the lifecycle of the local `ollama serve` process:

1. **Startup check:** verifies `ollama` is installed (`which ollama`).  If not, sends a notification alert to the frontend dashboard.
2. **Auto-start:** if Ollama is installed but not running, calls `spawn_ollama_serve_detached()` to start it before the first crawl.
3. **Model availability:** after Ollama is running, checks if the configured embedding model is available.  If not, triggers `ollama pull` automatically.
4. **Monitoring:** the `g_ollama_running` atomic is polled periodically and reflected in `/health`.

Ollama configuration keys stored in the `config` table:

| Key | Description |
|-----|-------------|
| `ollama_url` | Ollama base URL (default `http://127.0.0.1:11434`) |
| `ollama_model` | Active embedding model |

---

## Security

### Database Encryption

The SQLite database is encrypted with **SQLCipher** (AES-256-CBC).  The encryption key is derived from the platform identity:
- **macOS:** hardware UUID read from `IOPlatformUUID` via IOKit, concatenated with a fixed salt and SHA-256 hashed.  This ties the database to the specific machine.

The database is **not readable** by other processes or on other machines without the same platform UUID.

### Tomedo Communication

All Tomedo API calls use mutual TLS (client certificate + server CA verification is disabled because Tomedo uses a self-signed server cert).  No plaintext credentials are transmitted.

### RAG API

The REST API binds to `127.0.0.1` (loopback) only.  No external access is possible without explicit network routing changes.  No authentication is required on the loopback interface — the binding is the security boundary.

### Inter-service Communication

All inter-service HTTP calls (llama-service → tomedo-crawl, sip-client → tomedo-crawl) use TLS when `prodigy_tls::ensure_certs()` provides a valid certificate pair (shared with the rest of Prodigy via `tls_cert.h`).

---

## Building

`tomedo-crawl` is built as part of the normal Prodigy build:

```bash
cd build && ninja -j$(sysctl -n hw.ncpu)
```

The binary is placed in `bin/tomedo-crawl`.

### Dependencies

| Dependency | Source | Notes |
|-----------|--------|-------|
| `mongoose.h` / `mongoose.c` | vendored | HTTP server + TLS client |
| `sqlite3.h` / `sqlite3.c` | vendored | Persistent storage |
| `sqlcipher/` | vendored | SQLite encryption extension |
| `hnswlib.h` | `third_party/hnswlib/hnswlib.h` (v0.9.0) | HNSW ANN index |
| `openssl/` | system via Homebrew or `third_party/openssl/` | TLS for Tomedo mTLS |
| `db_key.h` | project | Platform-derived database key |
| `tls_cert.h` | project | Shared TLS cert management |
| `interconnect.h` | project | LogForwarder + ServiceType |
| POSIX threads | system | `std::thread` |

---

## Running

```bash
# With default database path (tomedo-crawl.db in CWD)
bin/tomedo-crawl

# With explicit database path
bin/tomedo-crawl /var/lib/tomedo-crawl/data.db
```

### Command-Line Arguments

| Position | Description |
|----------|-------------|
| `argv[1]` | Path to the SQLite database file (optional; default: `tomedo-crawl.db`) |

Additional runtime flags (appended as extra arguments by the frontend's "Service Arguments" builder):

| Flag | Description |
|------|-------------|
| `--verbose` | Enable DEBUG log level |
| `--skip-initial-crawl` | Do not run a crawl immediately at startup |
| `--phone-only` | Only update the phone index (skip embedding) |
| `--no-embed` | Index phone numbers but do not embed text chunks |
| `--top-k N` | Default top-K for /query (overrides config default of 3) |
| `--chunk-size N` | Chunk size in estimated tokens (default 512) |
| `--overlap N` | Chunk overlap in estimated tokens (default 64) |
| `--workers N` | Number of embedding worker threads (default 4) |

---

## Frontend Integration

The frontend manages tomedo-crawl as a regular service (start/stop/restart) and exposes a dedicated configuration panel in the Services page.

### API Endpoints (frontend → tomedo-crawl)

| Endpoint | Description |
|----------|-------------|
| `GET /api/rag/health` | Proxy for GET 13181/health |
| `GET /api/rag/config` | Read all config keys |
| `POST /api/rag/config` | Write config keys (written to tomedo-crawl.db) |
| `POST /api/rag/cert_upload` | Upload PEM certificate file |
| `POST /api/rag/trigger_crawl` | Forward to POST 13181/crawl/trigger |
| `GET /api/rag/ollama/models` | Forward to GET 13181/ollama/models |
| `POST /api/rag/ollama/start` | Forward to POST 13181/ollama/start |
| `POST /api/rag/ollama/stop` | Forward to POST 13181/ollama/stop |
| `POST /api/rag/ollama/pull` | Forward pull request |
| `POST /api/rag/wipe_vectors` | Wipe vector store |

### Dashboard

tomedo-crawl and Ollama appear as separate nodes in the pipeline visualization:
- **RAG** node (purple border) — reflects `/health` status
- **Ollama** node (orange border) — reflects `ollama_running` from `/health`

---

## llama-service Integration

llama-service queries tomedo-crawl before each LLM inference call:

1. **Caller identification** (`rag_get_caller(call_id)`): `GET /caller/{call_id}` → parses `name`, `patient_id`.
2. **RAG context** (`rag_query(text, top_k, patient_id)`): `GET /query?text=...&top_k=N&patient_id=N` → concatenates result text snippets.
3. **Dynamic system prompt**: the base system prompt is extended with:
   ```
   <greeting_hint>: "Der Anrufer ist Max Mustermann (Patient ID 4711)."
   Kontextinformation aus Praxissystem:
   <rag_context>
   ```

All tomedo-crawl calls use a **150 ms timeout** and are fire-and-forget — if the service is unavailable the LLM proceeds with the base system prompt without patient context.  This ensures calls complete normally even when tomedo-crawl is stopped.

---

## sip-client Integration

sip-client notifies tomedo-crawl when a call arrives or ends:

- **Call start** (`notify_tomedo_crawl`): non-blocking TCP connect (50 ms timeout) → `POST /caller` with `call_id` and phone number extracted from the SIP `From:` header URI.
- **Call end** (`handle_call_end`): fire-and-forget `DELETE /caller/{call_id}`.

Failures are logged at DEBUG level and never delay call setup.

---

## Troubleshooting

### "embedding_unavailable" in /query response
Ollama is not reachable.  Check:
```bash
curl http://127.0.0.1:11434/api/tags
```
Use the frontend (Start button in Ollama Subservice section) or run `ollama serve` manually.

### indexed_docs stays at 0 after crawl
- Check that the Tomedo certificate is uploaded and the host/port are correct.
- Check logs for "enumerate_patients: HTTP 401" or TLS errors.
- Verify `ollama pull <model>` has completed for the configured model.

### "VectorStore: dim mismatch" in logs
The embedding model was changed without wiping the vector store.  Click **Wipe Vectors** in the frontend, then trigger a fresh crawl.

### Phone lookup always returns not_found
The phone_index is populated during the full crawl.  Trigger a crawl and wait for it to complete (`indexed_docs > 0` in /health).
