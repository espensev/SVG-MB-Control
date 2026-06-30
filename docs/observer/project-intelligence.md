# Project Intelligence - SVG-MB-Control

**Last updated:** 2026-06-26T07:26:28+02:00
**Purpose:** durable repo-local notes for future work selection. This is a
passive observation surface, not a plan and not a replacement for
`docs\features\README.md`, `docs\RUNTIME_HOME.md`, or
`docs\RUNTIME_LOGGING_AND_EVALUATION.md`.

---

## Current Intent

`SVG-MB-Control` remains the standalone runtime repo for motherboard/chassis
telemetry, fan control, runtime state, recovery, logging, and analyzer evidence.
It should inform SQ-Control and ThermalHQ planning through documented semantics,
small fixtures, and decision records, not through runtime coupling or subprocess
bridges.

The useful cross-repo evidence surfaces are:

- `current_state.json` fan tach, raw duty/mode, and effective write-policy state,
- `control_runtime.json` health, timing, log-path, active-profile, and channel
  response state,
- control-loop CSV response, power, load, GPU context, and per-channel
  attribution fields,
- runtime manifests and event JSONL,
- `analyze ingest` / `analyze report` outputs and decision records.

Any new runtime field, schema, CLI/operator surface, export artifact, or
behavior change must go through the normal feature-intake gate before product
code work starts.

Related files:

- `README.md`
- `docs\discovery-sq-thermalhq-control-alignment.md`
- `docs\RUNTIME_HOME.md`
- `docs\RUNTIME_LOGGING_AND_EVALUATION.md`
- `docs\features\README.md`
- `D:\Development\Thermals\SQ-control\docs\observer\project-intelligence.md`
- `D:\Development\Thermals\ThermalHQ\SQ_CONTROL_NEEDS_BRIDGE_2026-06-26.md`

---

## Working Rules For Future Agents

- Keep this repo standalone. Do not add SQ-control, ThermalHQ, NVG, or other
  sibling repo runtime dependencies.
- Treat cross-repo alignment as docs, fixtures, and field-map work unless a
  feature spec explicitly authorizes a behavior/schema/API change.
- Keep power and workload fields labeled as evidence-only; they are not control
  inputs.
- Do not commit raw runtime CSV captures by default. Commit compact summaries,
  fixtures, or decision records.
- Respect live-runtime safety: do not start, stop, restart, change scheduled
  tasks, reset breakers, or write fan duty unless the task explicitly requires
  live interaction.

---

## Observation Log

Append-oriented records live in `data\observations.jsonl`.
