// ============================================================
// tomedo-crawl  —  RAG sidecar for Prodigy
// ============================================================
//
// TOMEDO API DISCOVERY  (probed live 2026-04-11, server 192.168.10.9)
// ============================================================
//
// LIVE PROBE EVIDENCE (redacted PII — field shapes confirmed):
//
//   $ curl -sk --cert client.pem --key client.pem \
//       https://192.168.10.9:8443/tomedo_live/serverstatus
//   → {"status":"OK","softwareVersion":"2026-03-30 15:45","revision":121155207,...}
//
//   $ curl -sk ... https://192.168.10.9:8443/tomedo_live/patient?flach=true \
//       | python3 -c "import json,sys; d=json.load(sys.stdin); print(len(d))"
//   → 15421
//
//   $ curl -sk ... https://192.168.10.9:8443/tomedo_live/patient?flach=true \
//       | python3 -c "import json,sys; print(json.dumps(json.load(sys.stdin)[0],indent=2))"
//   → {
//       "ident": 1,
//       "nachname": "Arnold",        ← confirmed field name
//       "vorname": "Herbert",        ← confirmed field name
//       "titel": null,
//       "geburtsDatum": -530802000000,  ← epoch ms (negative = before 1970)
//       "ort": "Pfronstetten",
//       "zuletztAufgerufen": 1775631991660,
//       "patientenDetails": {"ident": 1},  ← flat list has NO phone data
//       "nachname_phonetic": "_ernolc_",
//       "vorname_phonetic": "_erferc_",
//       "revision": null,
//       "geburtsname": null
//     }
//
//   $ curl -sk ... https://192.168.10.9:8443/tomedo_live/patient/776
//   → {
//       "nachname": "Kunsch",
//       "vorname": "Lothar",
//       "patientenDetails": {
//         "kontaktdaten": {
//           "telefon":      "07383-942735",  ← confirmed field name
//           "telefon2":     null,
//           "handyNummer":  null,             ← confirmed field name
//           "telefon3":     null,
//           "weitereTelefonummern": [],
//           "fax": null,
//           ...
//         },
//         ...
//       }
//     }
//
//   PHONE SEARCH — CONFIRMED DOES NOT WORK BY PHONE DIGITS:
//   $ curl -sk ... ".../patient/searchByAttributes?query=942735&telefonNummern=true"
//   → {}     ← empty dict, not an array — name-only search confirmed
//
//   $ curl -sk ... ".../patient/1403/patientenDetailsRelationen/medikamentenPlan"
//   → 12 entries, e.g.:
//     { "nameBeiVerordnung": "AMLODIPIN/Valsartan/HCT Heumann 10/160/12,5mg FTA 98 ST",
//       "dosierungFrueh": null, "dosierungMittag": null, "dosierungAbend": null,
//       "wirkstaerkeBeiVerordnung": "10 mg / 160 mg / 12,5 mg",
//       "darreichungBeiVerordnung": "FITBL", ... }
//
//   $ curl -sk ... ".../patient/3892/patientenDetailsRelationen?limitScheine=true&limitMedikamentenPlan=50"
//   → diagnosen: 101 entries, e.g.:
//     { "freitext": "Sinusitis",   ← human-readable ICD description confirmed
//       "typ": null,               ← some entries have null typ, use freitext as primary
//       "icdKatalogEintrag": {"ident": 12345}, ... }
//     other entries: {"freitext": "lokal allergische Reaktion auf Wespenstich", "typ": "G"}
//
// ============================================================
//
// BASE URL:    https://192.168.10.9:8443/tomedo_live/
// AUTH:        Mutual TLS — macOS Keychain identity "tomedoClientCert"
//              (self-signed RSA-4096; the Tomedo macOS client installs
//               this certificate pair automatically on first server
//               connection). Export once:
//                 security export -k ~/Library/Keychains/login.keychain-db \
//                   -t identities -f pkcs12 -P "" \
//                   -o /tmp/tomedo_client.p12
//                 openssl pkcs12 -legacy -in /tmp/tomedo_client.p12 -nodes \
//                   -passin pass:"" -out /etc/tomedo-crawl/client.pem
//              PEM contains both cert and private key (no password).
//              Use OpenSSL: SSL_CTX_use_certificate_file +
//                           SSL_CTX_use_PrivateKey_file.
//              NO HTTP Authorization header required.
//
// PATIENT LIST (flat, no phone data):
//   GET /patient?flach=true
//   → JSON array, 15 421 records (confirmed 2026-04-11)
//   Fields: ident, nachname, vorname, titel, geburtsDatum (epoch ms),
//           ort, zuletztAufgerufen — phone NOT included.
//
// PATIENT FULL RECORD (includes phone fields):
//   GET /patient/{id}
//   Phone fields inside patientenDetails.kontaktdaten (confirmed names):
//     telefon         — main phone (may contain \n-separated entries)
//     telefon2        — secondary phone
//     handyNummer     — mobile
//     telefon3        — tertiary phone
//     weitereTelefonummern[] — additional numbers
//
// PHONE-BASED CALLER LOOKUP:
//   No server-side phone-search endpoint exists (confirmed).
//   searchByAttributes?query={digits}&telefonNummern=true → {} (empty).
//   → Build local phone_index SQLite table during background crawl.
//   → Lookup at call time: query local SQLite by phone digits (LIKE match).
//
// PATIENT DETAILS WITH CLINICAL RELATIONS:
//   GET /patient/{id}/patientenDetailsRelationen
//       ?limitScheine=true&limitKartei=50&limitFormulare=10
//       &limitVerordnungen=50&limitMedikamentenPlan=50
//       &limitZeiterfassungen=true&limitBehandlungsfaelle=true
//   → JSON object; key arrays:
//     diagnosen[]  — { freitext: "human-readable text", typ: "G"|"V"|null,
//                      icdKatalogEintrag.ident }
//     (other arrays: karteiEintraege, behandlungsfaelle, ...)
//
// MEDICATIONS (separate endpoint — not in patientenDetailsRelationen body):
//   GET /patient/{id}/patientenDetailsRelationen/medikamentenPlan
//   → JSON array, e.g. 12 entries:
//     { nameBeiVerordnung, wirkstaerkeBeiVerordnung,
//       darreichungBeiVerordnung,
//       dosierungFrueh, dosierungMittag, dosierungAbend, dosierungNacht }
//
// APPOINTMENTS:
//   GET /patient/{id}/termine?flach=true
//   → JSON array: { ident, beginn: epoch_ms, ende: epoch_ms, info }
//
// VISITS (Besuch):
//   GET /besuch/{patient_id}/besucheForPatient
//
// SERVER HEALTH:
//   GET /serverstatus → { status: "OK", softwareVersion, revision }
//
// NO BRIEFKOMMANDO API:
//   Briefkommando ($[d:...]$, $[&p.name]$, etc.) is client-side only.
//   Context documents are composed directly from the JSON fields above.
//
// NO STATISTICS SQL API:
//   GET /statistik/ → "RESTEASY003210: Could not find resource"
//   Custom SQL queries are not supported via REST.
//
// RAG CONTEXT DOCUMENT FORMAT (produced per patient):
//   Patient: {vorname} {nachname} (ID {ident}), geb. {geburtsDatum}
//   Diagnosen: {diagnosen[].freitext} [max 20 with non-null freitext]
//   Medikamente: {nameBeiVerordnung} {dosierungFrueh}-{mittag}-{abend}
//   Nächster Termin: {beginn_formatted} ({info})
//   Telefon: {telefon}
//
// PAGINATION:
//   Flat patient list returns all ~15k records in one HTTP response.
//   No server-side pagination parameter found.
//   Background crawl processes patients in batches of 100 with 10ms
//   sleep between batches to avoid hammering the server.
//
// ============================================================
// Implementation begins in Step 2 (skeleton) through Step 6 (full).
// ============================================================
