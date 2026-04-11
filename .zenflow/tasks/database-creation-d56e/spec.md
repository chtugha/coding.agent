# Technical Specification: Database Startup Management

## Technical Context

- **Language**: C++17
- **Build system**: CMake + Ninja
- **Database**: SQLite3 (linked directly, `sqlite3.h` / `sqlite3.c`)
- **HTTP framework**: Mongoose (embedded)
- **Key files**:
  - `frontend.cpp` — `main()`, `FrontendServer` class declaration, constructor
  - `database.h` — `init_database()`, `handle_db_schema()`, `handle_db_query()`, `handle_db_write_mode()`
- **Existing arg parsing**: `main()` already handles `--port <N>` via simple `argc`/`argv` checks (no library)
- **Database path**: `project_root + "/frontend.db"`, stored as `db_path_` member

## Current Architecture

1. `main()` resolves the project root, then constructs `FrontendServer(port, project_root)`.
2. The `FrontendServer` constructor unconditionally calls `init_database()`, which:
   - Opens/creates `frontend.db`
   - Executes `CREATE TABLE IF NOT EXISTS` for 13 tables (the canonical schema)
   - Runs a hardcoded `migrations[]` array of `ALTER TABLE ADD COLUMN` / `DROP COLUMN` statements (errors silently ignored)
   - Inserts seed data (`INSERT OR IGNORE INTO service_config ...`)
   - Calls `rotate_logs()`
3. No user prompt, no schema validation, no `--db` flag exists today.

## Implementation Approach

### Principle: Minimal Invasive Changes

All new logic lives in two places:
1. **`main()`** in `frontend.cpp` — pre-construction database decision (prompt / `--db` flag / backup)
2. **`database.h`** — new static/free functions for schema validation and migration

The `FrontendServer` class and `init_database()` remain largely unchanged. The constructor already calls `init_database()` which uses `CREATE TABLE IF NOT EXISTS`, so it naturally handles both fresh and existing databases.

### New Functions

#### 1. `database_exists(const std::string& db_path) -> bool`
Simple `stat()` check. Placed in `database.h` as a free function (or static in `frontend.cpp`).

#### 2. `backup_database(const std::string& db_path) -> bool`
Renames `frontend.db` to `frontend.db.bak.<timestamp>` using `std::rename()`. Timestamp format: `YYYYMMDDTHHmmss` (ISO 8601 basic). Returns false on rename failure.

#### 3. `prompt_database_action(const std::string& db_path) -> char`
Prints the interactive prompt to stdout, reads a single character from stdin. Returns `'R'` or `'N'`. Loops on invalid input.

#### 4. `validate_and_migrate_schema(sqlite3* db) -> bool`
Called after `init_database()` when reusing an existing database. This function:
- Queries `PRAGMA table_info(<table>)` for each canonical table
- Compares against expected columns (extracted from the schema string in `init_database()`)
- Reports missing tables and missing columns
- Missing tables: already handled by `CREATE TABLE IF NOT EXISTS` in `init_database()`
- Missing columns: already handled by `migrations[]` in `init_database()`
- This function is primarily a **diagnostic reporter** — it prints what was missing/added after `init_database()` has already run
- Returns false only if critical tables are entirely absent after init

**Key insight**: `init_database()` already handles creation of missing tables (`CREATE TABLE IF NOT EXISTS`) and missing columns (the `migrations[]` array). The new `validate_and_migrate_schema()` function's job is to:
1. Open the DB read-only first and check what's missing
2. Print a diagnostic summary
3. Confirm that after `init_database()` runs, everything is present
4. Fail if something couldn't be fixed

### Modified Code Flow

```
main():
  1. Parse --port and --db flags from argv
  2. Resolve project_root (existing logic)
  3. Check if frontend.db exists
  4. If --db flag provided:
     - "new": backup if exists, proceed (init_database creates fresh)
     - "reuse": fail if not exists, proceed to construct server
  5. If no --db flag:
     - If DB doesn't exist: print message, proceed
     - If DB exists: prompt user [R/N]
       - R: proceed to construct server
       - N: backup, proceed
  6. Construct FrontendServer (calls init_database as before)
  7. If reusing: call validate_and_migrate_schema() after construction
     - This verifies init_database() handled everything
     - Prints diagnostic output
     - Aborts on failure
```

## Source Code Structure Changes

### `frontend.cpp`

- **`main()`**: Add `--db` argument parsing after existing `--port` parsing. Add database existence check, interactive prompt, and backup logic before `FrontendServer` construction. Add post-construction schema validation call when reusing.

### `database.h`

- Add `database_exists()` free function
- Add `backup_database()` free function  
- Add `validate_and_migrate_schema()` as a `FrontendServer` public method (needs access to `db_` member)
- Add `prompt_database_action()` free function

### Schema Definition Table (for validation)

The canonical schema is already defined as the `schema` string literal in `init_database()`. For validation, we need a structured representation. Approach: define a static array of `{table_name, {column_name, ...}}` structs in `database.h` that mirrors the `CREATE TABLE` statements. This is maintained alongside the schema string — a single source isn't practical without a parser, but the two are in the same file so drift is unlikely.

## Data Model / API / Interface Changes

- **No database schema changes** — this feature is about managing the existing schema.
- **No HTTP API changes** — the prompt is CLI-only.
- **New CLI interface**: `--db new|reuse` flag added to the frontend binary.
- **New stdout output**: Database status messages and optional interactive prompt.

## Delivery Phases

### Phase 1: Core Database Prompt and Backup
- Add `database_exists()` and `backup_database()` to `database.h`
- Add `prompt_database_action()` to `frontend.cpp` (or `database.h`)
- Modify `main()` to check DB existence, prompt user, perform backup if needed
- Add `--db` flag parsing

### Phase 2: Schema Validation and Diagnostic Reporting
- Add `validate_and_migrate_schema()` method to `FrontendServer`
- Define canonical schema table/column list for validation
- Call validation after `init_database()` when reusing
- Print diagnostic summary (missing tables/columns found and fixed)

## Verification Approach

- **Build**: `cd build && ninja -j$(sysctl -n hw.ncpu)` — must compile without errors or warnings
- **Manual test scenarios**:
  1. No `frontend.db` exists → should print "Creating new database" and proceed
  2. `frontend.db` exists, user chooses R → should validate schema, print status, proceed
  3. `frontend.db` exists, user chooses N → should backup to `frontend.db.bak.<ts>`, create fresh
  4. `--db new` → non-interactive fresh creation with backup
  5. `--db reuse` with no DB → should fail with error message
  6. Old schema DB (missing columns) reused → should print what was missing, apply migrations, succeed
