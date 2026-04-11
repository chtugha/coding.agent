# PRD: Database Startup Management

## Background

The Prodigy frontend (`frontend.cpp`) uses SQLite (`frontend.db`) for all persistent state: logs, service configs, test results, settings, SIP lines, models, and benchmarks. Currently, `init_database()` is called unconditionally in the `FrontendServer` constructor — it opens (or creates) the database file and runs `CREATE TABLE IF NOT EXISTS` plus a hardcoded migration list. There is no user-facing choice about whether to reuse an existing database or start fresh, and no mechanism to detect or handle schema drift beyond the existing `ALTER TABLE` migrations.

## Requirements

### R1: Startup Database Prompt (CLI)

On startup, before constructing `FrontendServer`, the `main()` function must detect whether `frontend.db` already exists in the project root.

- **If no database exists**: Print a message ("No existing database found. Creating new database...") and proceed to create a fresh database (current behavior).
- **If a database exists**: Prompt the user via stdin:
  ```
  Existing database found: frontend.db
  [R] Reuse existing database
  [N] Create new database (existing database will be backed up)
  Choice [R/N]:
  ```
  - **R (Reuse)**: Proceed to schema validation (R2).
  - **N (New)**: Rename the existing file to `frontend.db.bak.<timestamp>` (ISO 8601, e.g. `frontend.db.bak.20260411T051200`), then create a fresh database.

### R2: Schema Validation on Reuse

When the user chooses to reuse an existing database, the system must validate that the schema matches the current expected design before proceeding.

- **Validation approach**: Compare the set of tables and their columns (name + type) against the canonical schema defined in `init_database()`.
  - For each expected table: verify it exists and has all expected columns.
  - Detect missing tables and missing columns.
- **If schema is current**: Print "Database schema is up to date." and proceed normally.
- **If schema is outdated** (missing tables or columns):
  - Print a summary of what is missing (e.g., "Missing table: tts_validation_tests", "Missing column: model_benchmark_runs.german_pct").
  - Automatically apply migrations: create missing tables and add missing columns via `ALTER TABLE ADD COLUMN`.
  - Print "Database schema updated successfully." and proceed normally.
- **If migration fails**: Print the error, abort startup.

### R3: Command-Line Flag for Non-Interactive Mode

Support a `--db` flag for scripted/non-interactive usage:
- `--db new` — always create a fresh database (backup existing if present).
- `--db reuse` — always reuse existing database (fail if none exists).
- If `--db` is provided, skip the interactive prompt entirely.

### R4: Preserve Existing Behavior

- The `CREATE TABLE IF NOT EXISTS` statements in `init_database()` remain the canonical schema definition.
- The existing `ALTER TABLE` migration list continues to run after schema creation (for forward compatibility with older databases).
- Seed data (`INSERT OR IGNORE INTO service_config ...`) continues to run on every startup.
- No changes to the web UI or HTTP API are required for this feature.

## Assumptions

- The frontend is always run from a terminal with stdin available (except when `--db` flag is used).
- Backup files (`frontend.db.bak.*`) are not automatically cleaned up; this is acceptable for a local developer tool.
- Schema validation only checks table/column presence, not column types or constraints (SQLite is loosely typed and `ALTER TABLE` cannot change types anyway).
- The `sqlite_sequence` and `sqlite_master` internal tables are excluded from validation.

## Out of Scope

- Web UI for database management (already exists via `/api/db/schema` and `/api/db/query`).
- Automatic data migration (e.g., transforming row data between schema versions).
- Database encryption or authentication.
