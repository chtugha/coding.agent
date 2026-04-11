// ============================================================
// tomedo-crawl  —  RAG sidecar for Prodigy
// ============================================================
//
// TOMEDO API DISCOVERY (probed 2026-04-11 against live server)
// ============================================================
//
// BASE URL:    https://192.168.10.9:8443/tomedo_live/
// AUTH:        Mutual TLS — macOS Keychain identity "tomedoClientCert"
//              (self-signed RSA-4096; the Tomedo macOS client installs
//               this certificate pair automatically on first server
//               connection).  For this service we export it once:
//                 security export -k ~/Library/Keychains/login.keychain-db \
//                   -t identities -f pkcs12 -P "" \
//                   -o /etc/tomedo-crawl/client.p12
//              then convert to PEM for POSIX TLS use (OpenSSL):
//                 openssl pkcs12 -legacy -in client.p12 -nodes \
//                   -passin pass:"" -out /etc/tomedo-crawl/client.pem
//              The PEM file contains both the certificate and private key.
//
// PATIENT LIST (flat, no phone data):
//   GET /patient?flach=true
//   → JSON array, 15 421 records (as of discovery date)
//   Each record: { "ident": N, "nachname": "...", "vorname": "...",
//                  "titel": null, "geburtsDatum": <epoch_ms>,
//                  "ort": "...", ... }
//   Phone numbers are NOT included in the flat list.
//
// PATIENT FULL RECORD (includes phone fields):
//   GET /patient/{id}
//   Phone fields inside patientenDetails.kontaktdaten:
//     telefon         — main phone (may contain \n-separated entries)
//     telefon2        — secondary phone
//     handyNummer     — mobile
//     telefon3        — tertiary phone
//     weitereTelefonummern[] — additional numbers
//   Example: { "telefon": "07383-942735", "handyNummer": null, ... }
//
// PHONE-BASED CALLER LOOKUP STRATEGY:
//   The Tomedo API has no server-side phone-search endpoint.
//   GET /patient/searchByAttributes?query={term}&telefonNummern=true
//   searches by name only (telefonNummern=true affects display, not
//   the match). Confirmed: querying "656" or "089" returns {}.
//   → Solution: During background crawl, fetch GET /patient/{id} for
//     every patient and store phone→ident mapping in SQLite.
//     Incoming-call lookups query this local table (O(1) by index).
//
// PATIENT DETAILS WITH CLINICAL RELATIONS:
//   GET /patient/{id}/patientenDetailsRelationen
//       ?limitScheine=true&limitKartei=50&limitFormulare=10
//       &limitVerordnungen=50&limitMedikamentenPlan=50
//       &limitZeiterfassungen=true&limitBehandlungsfaelle=true
//   → JSON object with arrays:
//     diagnosen[]        — { freitext: "human-readable ICD text",
//                            typ: "G"(gesichert)|"V"(Verdacht)|"Z",
//                            icdKatalogEintrag.ident, datum }
//     medikamentenPlan[] — fetched separately (see below)
//     karteiEintraege[]  — chart/notes entries
//     behandlungsfaelle[]
//
// MEDICATIONS:
//   GET /patient/{id}/patientenDetailsRelationen/medikamentenPlan
//   → JSON array, each entry:
//     { nameBeiVerordnung: "TENSOFLUX 2,5 mg/5 mg Tabletten 100 ST N3",
//       dosierungFrueh: "1", dosierungMittag: null,
//       dosierungAbend: null, dosierungNacht: null,
//       wirkstaerkeBeiVerordnung: "2.5 mg / 5 mg",
//       darreichungBeiVerordnung: "TABL", ... }
//
// APPOINTMENTS:
//   GET /patient/{id}/termine?flach=true
//   → JSON array, each entry:
//     { ident, beginn: <epoch_ms>, ende: <epoch_ms>, info: "TDP", ... }
//
// VISITS (Besuch):
//   GET /besuch/{patient_id}/besucheForPatient
//   → JSON array with visit records including embedded patient data.
//
// FUTURE APPOINTMENTS:
//   GET /patient/{id}/futureTermine
//   → JSON array (may be empty if none scheduled)
//
// SERVER STATUS (health check):
//   GET /serverstatus
//   → { "status": "OK", "softwareVersion": "2026-03-30 15:45",
//       "revision": 121155207, ... }
//
// LLM SERVICE (Tomedo-hosted Gemini/Mistral — available but NOT used
// by this service; we use local Ollama instead):
//   POST /{db}/llmservice/{userIdent}/v1/chat/completions
//   OpenAI-compatible API; models: gemini-2.5-flash, mistral-medium-latest
//
// BRIEFKOMMANDO RESOLUTION:
//   No server-side Briefkommando endpoint was found. Briefkommando
//   ($[d:ICD]$, $[&p.name]$, etc.) is a client-side template expansion
//   feature. Patient context documents are composed directly from the
//   JSON fields above (no server-side resolution needed).
//
// RAG CONTEXT DOCUMENT FORMAT (produced per patient):
//   Patient: {vorname} {nachname} (ID {ident}), geb. {geburtsDatum}
//   Diagnosen: {diagnosen[].freitext} ({typ})  [max 20 active]
//   Medikamente: {name} {wirkstaerke} {dosierungFrueh}-{mittag}-{abend}
//   Nächster Termin: {beginn_formatted} ({info})
//   Telefon: {telefon}
//
// PAGINATION:
//   The flat patient list returns all records in one call (~15k entries,
//   ~3 MB JSON). No server-side pagination parameter was found; the
//   response is complete in a single request.
//   For the background crawl we process patients in batches of 100,
//   sleeping 10ms between batches to avoid hammering the server.
//
// ============================================================
// Implementation begins in Step 2 (skeleton) through Step 6 (full).
// ============================================================
