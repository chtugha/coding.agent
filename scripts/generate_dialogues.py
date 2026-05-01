#!/usr/bin/env python3
# generate_dialogues.py — Step 1 of German Moshi fine-tune pipeline.
#
# Generates 500 German phone dialogue scripts via GPT-4o-mini:
#   300 × Hausarztpraxis (medical / GP office)
#   200 × Kfz-Werkstatt  (mechanics workshop)
#
# Output: scripts/dialogues.json
#
# Requirements: pip install openai
# Env:          OPENAI_API_KEY

import json
import os
import random
import time
from pathlib import Path

from openai import OpenAI

client = OpenAI()

OUTPUT_PATH = Path(__file__).parent / "dialogues.json"

# Each entry: (scenario_key, target_count, description_for_LLM)
SCENARIOS = {
    "medical": [
        ("new_appointment",       45, "Ein Patient ruft zum ersten Mal an, um einen Termin beim Hausarzt zu vereinbaren. Er kennt die Praxis noch nicht."),
        ("followup_appointment",  40, "Ein Patient ruft an, um einen Kontrolltermin nach einer Behandlung, einer Erkrankung oder einem Krankenhausaufenthalt zu vereinbaren."),
        ("prescription_renewal",  35, "Ein Patient ruft an, um ein Wiederholungsrezept für ein bekanntes Dauermedikament (z.B. Blutdruckmittel, Schilddrüse, Diabetes) zu erhalten."),
        ("lab_result",            30, "Ein Patient ruft an, um nach seinen Laborergebnissen (Blutbild, Cholesterin, TSH, etc.) zu fragen. Manchmal gibt es Befunde, manchmal ist noch nichts da."),
        ("sick_note",             30, "Ein Patient ruft an, um eine Arbeitsunfähigkeitsbescheinigung zu beantragen oder zu verlängern. Manchmal ist es kompliziert."),
        ("referral",              25, "Ein Patient braucht eine Überweisung zu einem Facharzt (Orthopäde, Kardiologe, Dermatologe, etc.). Die Rezeption muss Details klären."),
        ("cancellation",          25, "Ein Patient muss einen Termin absagen oder verschieben. Manchmal ist es dringend, manchmal nicht."),
        ("symptom_triage",        35, "Ein Patient ruft mit akuten Symptomen an (Fieber, Brustschmerzen, Atemnot, starke Schmerzen). Die Rezeption muss die Dringlichkeit einschätzen und reagieren."),
        ("medication_question",   20, "Ein Patient hat Fragen zu einem verschriebenen Medikament: Dosierung, Nebenwirkungen, Wechselwirkungen, oder ob er es nüchtern nehmen soll."),
        ("insurance_query",       15, "Ein Patient fragt wegen seiner Krankenversicherung: Ist der Arzt Kassenarzt? Was kostet privat? Braucht er einen Überweisungsschein?"),
    ],
    "mechanics": [
        ("tyre_change",    50, "Ein Kunde ruft an, um einen Termin für den saisonalen Reifenwechsel zu vereinbaren (Winterreifen auf Sommerreifen oder umgekehrt). Manchmal hat er eigene Räder, manchmal soll die Werkstatt Reifen besorgen."),
        ("oil_change",     35, "Ein Kunde ruft wegen Ölwechsel oder kleiner/großer Inspektion an. Manchmal läuft eine Serviceleuchte, manchmal ist es ein Routinetermin."),
        ("mot_appointment",30, "Ein Kunde braucht einen Termin für die Hauptuntersuchung (HU/TÜV) oder die Abgasuntersuchung (AU). Manchmal ist der TÜV schon abgelaufen."),
        ("noise_diagnosis",30, "Ein Kunde beschreibt ein komisches Geräusch oder Verhalten seines Autos und möchte wissen, was los sein könnte und was ein Termin kostet."),
        ("brake_repair",   20, "Ein Kunde meldet Bremsgeräusche oder schlechtes Bremsverhalten. Die Werkstatt klärt ob Inspektion oder sofortige Reparatur nötig ist."),
        ("cost_estimate",  20, "Ein Kunde fragt nach einem Kostenvoranschlag für eine bestimmte Reparatur. Die Werkstatt erklärt was sie brauchen um einen KVA zu erstellen."),
        ("logistics",      15, "Ein Kunde klärt praktische Fragen: Wann kann er das Auto abgeben? Gibt es einen Hol- und Bringservice? Wann ist es fertig? Kann er einen Ersatzwagen haben?"),
    ],
}

SYSTEM_PROMPT = """Du erstellst realistische deutsche Telefongespräche als Trainingsdaten für eine KI.

ROLLEN:
- MOSHI = die KI-Assistentin, die den Anruf entgegennimmt (Rezeptionistin der Arztpraxis ODER Mitarbeiterin der Werkstatt)
- USER  = der anrufende Patient oder Kunde

WICHTIGE REGELN:
1. MOSHI beginnt IMMER das Gespräch mit einer Begrüßung und dem Namen der Einrichtung.
   Beispiele: "Praxis Dr. Hoffmann, guten Tag!" / "Autohaus Becker, Werkstatt, was kann ich für Sie tun?"
2. Natürliches gesprochenes Deutsch — keine Schriftsprache, keine Bullets, keine Aufzählungen.
3. Realistische Gesprächselemente einbauen: kurze Pausen ("Einen Moment bitte..."),
   Verständnisfragen ("Wie war der Name nochmal?"), Bestätigungen ("Ich trage das gleich ein.").
4. Verschiedene Praxisnamen / Werkstattnamen / Patientennamen / Fahrzeugtypen verwenden.
5. Jedes Gespräch muss einzigartig sein — andere Namen, andere Details, andere Formulierungen.
6. Ausgabe NUR als JSON-Array im vorgegebenen Format. KEIN Text davor oder danach.

JSON-FORMAT (strikt einhalten):
[
  {
    "id": "...",
    "domain": "medical" oder "mechanics",
    "scenario": "...",
    "length": "short" oder "medium" oder "long",
    "turns": [
      {"speaker": "moshi", "text": "Praxis Dr. Hoffmann, guten Tag!"},
      {"speaker": "user",  "text": "Guten Tag, ich würde gerne einen Termin..."},
      ...
    ]
  }
]"""


def make_user_prompt(domain: str, scenario_desc: str, length_desc: str, ids: list[str]) -> str:
    domain_label = "Hausarztpraxis" if domain == "medical" else "Kfz-Werkstatt"
    return (
        f"Erstelle {len(ids)} verschiedene Telefongespräche.\n\n"
        f"EINRICHTUNG: {domain_label}\n"
        f"SZENARIO: {scenario_desc}\n"
        f"GESPRÄCHSLÄNGE: {length_desc}\n"
        f"IDs für die Gespräche (in dieser Reihenfolge): {', '.join(ids)}\n\n"
        "Stelle sicher, dass jedes Gespräch völlig anders ist — "
        "unterschiedliche Namen, Situationen, Formulierungen und Details."
    )


LENGTH_SPECS = {
    "short":  "4–6 Sprechrunden gesamt (ca. 35–45 Sekunden). Knappe, zielgerichtete Kommunikation. Kein Smalltalk.",
    "medium": "8–12 Sprechrunden gesamt (ca. 80–100 Sekunden). Normales Gespräch mit Rückfragen und Bestätigungen.",
    "long":   "14–20 Sprechrunden gesamt (ca. 130–160 Sekunden). Ausführlich, mehrere Anliegen oder komplizierte Situation.",
}


def length_plan(total: int) -> list[str]:
    n_short  = round(total * 0.30)
    n_long   = round(total * 0.20)
    n_medium = total - n_short - n_long
    plan = (["short"] * n_short) + (["medium"] * n_medium) + (["long"] * n_long)
    random.shuffle(plan)
    return plan


def _validate_dialogue(d: dict) -> bool:
    if not isinstance(d, dict):
        return False
    turns = d.get("turns")
    if not isinstance(turns, list) or len(turns) == 0:
        return False
    for t in turns:
        if not isinstance(t, dict):
            return False
        if "speaker" not in t or "text" not in t:
            return False
        if t["speaker"] not in ("moshi", "user"):
            return False
        if not isinstance(t["text"], str) or len(t["text"].strip()) == 0:
            return False
    return True


def call_api(domain: str, scenario_key: str, scenario_desc: str, length: str, ids: list[str]) -> list[dict]:
    prompt = make_user_prompt(domain, scenario_desc, LENGTH_SPECS[length], ids)
    for attempt in range(4):
        try:
            resp = client.chat.completions.create(
                model="gpt-4o-mini",
                messages=[
                    {"role": "system", "content": SYSTEM_PROMPT},
                    {"role": "user",   "content": prompt},
                ],
                temperature=0.92,
                max_tokens=16384,
            )
            raw = resp.choices[0].message.content.strip()
            start = raw.find("[")
            end   = raw.rfind("]") + 1
            if start == -1 or end == 0:
                raise ValueError("No JSON array found in response")
            dialogues = json.loads(raw[start:end])
            valid = []
            for i, d in enumerate(dialogues):
                eid = ids[i] if i < len(ids) else f"{domain[:3]}_{i:04d}"
                d["id"]       = eid
                d["domain"]   = domain
                d["scenario"] = scenario_key
                d["length"]   = length
                if _validate_dialogue(d):
                    valid.append(d)
                else:
                    print(f"      ⚠ invalid dialogue {eid} — skipped")
            return valid
        except Exception as exc:
            wait = 2 ** attempt
            print(f"      ⚠ attempt {attempt+1} failed ({exc}) — retry in {wait}s")
            time.sleep(wait)
    print("      ✗ all attempts failed, skipping batch")
    return []


def main():
    random.seed(42)
    all_dialogues: list[dict] = []
    counter = {"medical": 0, "mechanics": 0}

    for domain, scenarios in SCENARIOS.items():
        prefix = "med" if domain == "medical" else "mec"
        print(f"\n{'═' * 56}")
        print(f"  {domain.upper()} dialogues")
        print(f"{'═' * 56}")

        for scenario_key, target, scenario_desc in scenarios:
            lengths = length_plan(target)
            print(f"\n  ▸ {scenario_key}  ({target} total)")

            batch_size = 5
            idx = 0
            while idx < target:
                batch_lengths = lengths[idx : idx + batch_size]
                groups: dict[str, list[str]] = {"short": [], "medium": [], "long": []}
                for offset, length in enumerate(batch_lengths):
                    global_id = counter[domain] + idx + offset
                    groups[length].append(f"{prefix}_{global_id:04d}")

                for length, ids in groups.items():
                    if not ids:
                        continue
                    print(f"    {length:6s} ×{len(ids)}  ", end="", flush=True)
                    batch = call_api(domain, scenario_key, scenario_desc, length, ids)
                    all_dialogues.extend(batch)
                    print(f"→ {len(batch)} ok")
                    time.sleep(0.4)

                idx += batch_size

            counter[domain] += target

    random.shuffle(all_dialogues)

    OUTPUT_PATH.write_text(
        json.dumps(all_dialogues, ensure_ascii=False, indent=2),
        encoding="utf-8",
    )

    med = sum(1 for d in all_dialogues if d.get("domain") == "medical")
    mec = sum(1 for d in all_dialogues if d.get("domain") == "mechanics")
    turns = sum(len(d.get("turns", [])) for d in all_dialogues)
    chars = sum(len(t["text"]) for d in all_dialogues for t in d.get("turns", []))

    print(f"\n{'═' * 56}")
    print(f"  ✓ {len(all_dialogues)} dialogues written → {OUTPUT_PATH}")
    print(f"     medical:   {med}")
    print(f"     mechanics: {mec}")
    print(f"     turns:     {turns}")
    print(f"     chars:     {chars:,}  (≈ ${chars * 2 / 1_000_000 * 15:.2f} TTS cost at $15/1M)")
    print(f"{'═' * 56}")


if __name__ == "__main__":
    main()
