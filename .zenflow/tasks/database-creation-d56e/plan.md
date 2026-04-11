# Full SDD workflow

## Configuration
- **Artifacts Path**: {@artifacts_path} → `.zenflow/tasks/{task_id}`

---

## Agent Instructions

---

## Workflow Steps

### [x] Step: Requirements
<!-- chat-id: ba811fac-a681-4350-83ac-e0e47bc42b51 -->

Create a Product Requirements Document (PRD) based on the feature description.

1. Review existing codebase to understand current architecture and patterns
2. Analyze the feature definition and identify unclear aspects
3. Ask the user for clarifications on aspects that significantly impact scope or user experience
4. Make reasonable decisions for minor details based on context and conventions
5. If user can't clarify, make a decision, state the assumption, and continue

Save the PRD to `{@artifacts_path}/requirements.md`.

### [x] Step: Technical Specification
<!-- chat-id: 1ae51c6c-54f4-4587-bffc-e31948a5930e -->

Create a technical specification based on the PRD in `{@artifacts_path}/requirements.md`.

1. Review existing codebase architecture and identify reusable components
2. Define the implementation approach

Save to `{@artifacts_path}/spec.md` with:
- Technical context (language, dependencies)
- Implementation approach referencing existing code patterns
- Source code structure changes
- Data model / API / interface changes
- Delivery phases (incremental, testable milestones)
- Verification approach using project lint/test commands

### [x] Step: Planning
<!-- chat-id: cf6308d3-2dd2-490a-b420-fabbb76db43c -->

Replaced the Implementation step with concrete tasks below.

### [ ] Step: Add utility functions and --db flag parsing

Add the following free functions to `database.h` (before the `FrontendServer` inline methods):
- `database_exists(const std::string& db_path) -> bool` — `stat()` check for file existence
- `backup_database(const std::string& db_path) -> bool` — rename to `frontend.db.bak.<YYYYMMDDTHHmmss>`, return false on failure
- `prompt_database_action(const std::string& db_path) -> char` — interactive stdin prompt, prints the menu from the PRD, reads a character, converts to uppercase, accepts `R` or `N`, loops on invalid input

Modify `main()` in `frontend.cpp`:
- After existing `--port` parsing, add `--db` flag parsing (`new` or `reuse`). Store in a local variable (e.g., `std::string db_mode`).
- After resolving `project_root` and before constructing `FrontendServer`:
  - Compute `db_path = project_root + "/frontend.db"`
  - If `--db new`: call `backup_database()` if DB exists; if backup fails, print error and `return 1`; then proceed
  - If `--db reuse`: fail with error if DB doesn't exist, then proceed
  - If no `--db` flag:
    - DB doesn't exist: print "No existing database found. Creating new database..." and proceed
    - DB exists: call `prompt_database_action()`. If `N`, call `backup_database()` (abort on failure). If `R`, proceed.
- Construct `FrontendServer` as before (line 6812)

**Verification**: `cd build && ninja -j$(sysctl -n hw.ncpu)` — must compile clean

### [ ] Step: Add schema validation and diagnostic reporting

In `frontend.cpp`, add `bool validate_schema();` declaration to the `FrontendServer` class body (near `init_database()` at line 540).

In `database.h`, add the implementation (follows the same pattern as `init_database()` — declared in class, inline-defined in `database.h`):
- Define a static canonical schema structure: an array of `{table_name, {col1, col2, ...}}` structs/pairs that mirrors the 13 `CREATE TABLE` statements in `init_database()`. Exclude `sqlite_sequence` and `sqlite_master`. Note: this array must be updated whenever the schema in `init_database()` changes.
- `inline bool FrontendServer::validate_schema()` — uses `db_` member directly:
  - For each canonical table: run `PRAGMA table_info(<table>)` and collect existing column names
  - Compare against expected columns
  - Print diagnostic lines for missing tables ("Missing table: X — created") and missing columns ("Missing column: X.Y — added")
  - If nothing is missing, print "Database schema is up to date."
  - If things were missing, print "Database schema updated successfully." (since `init_database()` already ran and fixed them)
  - Return false only if a critical table is still absent after init (query `sqlite_master` to confirm)

Modify `main()` in `frontend.cpp`:
- After `FrontendServer server(port, project_root);` (line 6812), if the user chose to reuse (either via prompt or `--db reuse`), call `server.validate_schema()`
  - If validation returns false, print error and `return 1`

**Verification**: `cd build && ninja -j$(sysctl -n hw.ncpu)` — must compile clean. Manual test: rename a column in an existing `frontend.db` to simulate old schema, run frontend with `--db reuse`, verify diagnostic output.
