# FEAT-0007: RAM temperature telemetry

> **Reserved / parked 2026-06-06.** Proposed feature ("wish") captured before
> implementation; this full body is preserved under `docs/features/_parked/`
> pending a feasibility confirmation and a design decision. The active registry
> row is in `docs/features/README.md` §5. While parked, this body is not part of
> the enforced feature set (`tests/test_feature_specs.py`) and its
> `REQ-RAMTEMP-*` rows are not mirrored in `docs/TRACEABILITY.md`. This body
> preserves a candidate promotion path; promotion to `Draft` and buildability
> still require the feasibility confirmation and design decision named below.

**Project:** svg-mb-control
**Status:** Reserved (parked; body preserved)   **Version:** 0.1   **Updated:** 2026-06-07
**Namespace:** `REQ-RAMTEMP-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`, `docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`, `docs/READ_LOOP.md`, `docs/RUNTIME_HOME.md`, `docs/RUNTIME_LOGGING_AND_EVALUATION.md`, `docs/MEASUREMENT_GATE.md`
**Purpose:** surface per-DIMM memory (RAM) temperature telemetry — already
reachable through the existing Super I/O backend — into the read/control
snapshot, runtime status, control CSV, and analyze layers, as additive
read-only telemetry.

## 1. Summary

The controller reads AMD CPU temperatures (`src/hardware/amd_reader.{h,cpp}`)
and optional NVIDIA GPU temperatures (`src/hardware/gpu_reader.{h,cpp}`), but it
exposes no DRAM/DIMM (RAM) temperature in the read/control snapshot,
`current_state.json`, runtime status, the control CSV, or the analyze schema.
This feature proposes surfacing per-DIMM temperature as additive, optional,
read-only telemetry. The read path already exists: the vendored Super I/O
backend (`third_party/SVG-MB-SIO`) is a Nuvoton **NCT6701D** controller that
enumerates 23 temperature sources, four of which are DIMM sources
(`Agent0 DIMM0`, `Agent0 DIMM1`, `Agent1 DIMM0`, `Agent1 DIMM1`), and
`evidence-log` mode already calls that read path. The proposed work is to lift
the DIMM-labeled sources out of the evidence-only path into the normal
read/control telemetry surfaces, after confirming the sources carry valid data
on the target board. Whether DIMM temperature also becomes a *control input* is
explicitly out of scope for this slice and is gated separately
(`docs/MEASUREMENT_GATE.md` + a design decision).

## 2. Problem & motivation  *(promotion gate 1)*

Named code/contract gap, confirmed by a code read on 2026-06-06:

1. **No RAM temperature anywhere in the primary telemetry contract.** The
   read/control snapshot and status carry CPU (`amd_reader`) and optional GPU
   (`gpu_reader`) temperatures only; there is no memory-temperature field in
   `current_state.json`, the control CSV row schema
   (`src/runtime/runtime_csv_rows.{h,cpp}`), the analyze ingest schema
   (`src/analyze/analyze_ingest_db.cpp`; schema version `kSchemaVersion = 9` in
   `src/analyze/analyze_db.h`), or status.
2. **The data is already read, but only on the evidence path.** The Super I/O
   backend exposes `read_sio_temperatures` /
   `MbSioController::read_sio_temperatures`
   (`third_party/SVG-MB-SIO/include/svg_mb_sio/svg_mb_sio.h:133`,
   `third_party/SVG-MB-SIO/src/svg_mb_sio.cpp:176`), returning
   `MbSioTemperatureSnapshot { temperature_c, raw, half_raw, valid, label }` for
   all 23 sources. The DIMM source labels and their HWM bank registers are
   declared in `third_party/SVG-MB-SIO/src/fan_sio.cpp:85-88`
   (`Agent0 DIMM0` 0x405, `Agent0 DIMM1` 0x406, `Agent1 DIMM0` 0x407,
   `Agent1 DIMM1` 0x408). `evidence-log` already reads and logs every SIO
   temperature source via `fan_writer->ReadSioTemperatures()` →
   `ConvertTemperatures(...)` → the evidence record's `sio_temperatures`
   (`src/runtime/evidence_log.cpp:212-304`). So the DIMM sources are read by
   existing code; they are simply not promoted into the normal telemetry
   surfaces.
3. **Operator/analysis blind spot.** DIMM thermal headroom is not visible to the
   operator (`--status`), the eval dashboard, or `analyze report`, so RAM
   thermals cannot be correlated with airflow or used as future control
   evidence.

Supporting context (not a normative claim): these source labels match the
Nuvoton temperature-source enumeration that third-party monitors (for example
HWiNFO / LibreHardwareMonitor) read for DIMM temperature, which is consistent
with the same data being reachable here through the same chip. Whether the
specific board populates these sources is the open feasibility question in §11.

## 3. Goals & non-goals

**Goals**
- Surface per-DIMM temperature (the `Agent0/1 DIMM0/1` sources) as additive,
  optional, read-only fields in the read/control snapshot, `current_state.json`,
  runtime status, the control CSV, and the analyze schema/report.
- Reuse the existing SVG-MB-SIO temperature read path; add no new hardware
  transport.
- Report only sources flagged `valid`; report absent/zero/invalid DIMM sources
  as unavailable, never as `0 C`.

**Non-goals**
- No DIMM temperature as a control input in this slice (no curve, blend, boost,
  or safety override keyed on RAM temperature). That is deferred and separately
  gated.
- No new SMBus host-controller driver, no direct DIMM TSOD/SPD-hub I2C/I3C
  access, and no change to `third_party/SVG-MB-SIO` public API beyond what the
  existing read already provides.
- No UI work (out of scope per `docs/MEASUREMENT_GATE.md`); the eval dashboard
  may render fields that already exist in the CSV.
- No fan write, start/stop, or policy change.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | Reuses the already-vendored `third_party/SVG-MB-SIO` read path; adds no new external transport or tool dependency. |
| No fan write / start / stop / breaker reset outside an explicit live task | `AGENTS.md` §Live Runtime Safety | Read-only telemetry; the read path issues no write and touches no policy. |
| Shipped 250 ms cadence / channel set is the measured baseline | `docs/MEASUREMENT_GATE.md` | Telemetry-only; does not change cadence, channels, or control strategy. A DIMM read added to the hot path must fit the existing tick budget or be sampled on the evidence/wider tier, which is an open decision (§11). |
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | No control-math change in this slice (DIMM is not a control input here). |
| Runtime sidecar / status / manifest / CSV schema stays backward-compatible | `docs/RUNTIME_HOME.md`, `docs/RUNTIME_LOGGING_AND_EVALUATION.md` | New fields are additive and optional; old runtime-home files, archives, and configs stay valid; analyze ingest tolerates archives without the new columns. |

## 5. Behavior specification

Proposed behavior (none of this is implemented yet):

- The read/control path obtains DIMM temperatures from the existing SIO
  temperature read (`MbSioController::read_sio_temperatures` via the fan-writer
  wrapper used by `evidence-log`), filtered to the DIMM source labels declared
  in `fan_sio.cpp` (`Agent0/1 DIMM0/1`).
- A DIMM source is surfaced only when its `MbSioTemperatureSnapshot.valid` is
  true. An invalid or unpopulated source surfaces as unavailable
  (field absent / null), never as `0 C`, mirroring how other optional
  telemetry (GPU) is represented when absent.
- Sampling tier is an open decision (§11): either fold the DIMM read into the
  normal read/control tier (it shares the SIO transport already touched for fan
  reads) or keep it on the wider evidence/`evidence-log` tier to protect the
  250 ms control hot-path budget.
- Source files the behavior would live in or near: `src/hardware/fan_writer.h`
  / `sio_fan_writer.cpp` (read wrapper already present),
  `src/runtime/read_loop`/control snapshot assembly,
  `src/runtime/runtime_csv_rows.{h,cpp}` (CSV columns),
  `src/analyze/analyze_ingest_db.cpp` (schema), and the status/JSON IO layer.
- Failure behavior: if the SIO temperature read fails or returns no valid DIMM
  source, the controller behaves exactly as today (no RAM fields), with no new
  failure path on the control loop.

## 6. Requirements  *(promotion gate 4 — assign IDs only after the design decision picks a direction)*

> IDs are drafted for the parked body; they become normative when this spec is
> promoted to `Draft`/`Accepted` and mirrored into `docs/TRACEABILITY.md`.

| ID | Requirement |
|---|---|
| REQ-RAMTEMP-01 | RAM temperature is read through the existing SVG-MB-SIO temperature read path; no new hardware transport, driver, or `third_party/SVG-MB-SIO` public-API addition is introduced. |
| REQ-RAMTEMP-02 | Only DIMM sources whose `MbSioTemperatureSnapshot.valid` is true are surfaced; an invalid, absent, or zero DIMM source is reported as unavailable, never as `0 C`. |
| REQ-RAMTEMP-03 | Per-DIMM temperature is surfaced into the read/control snapshot, `current_state.json`, and runtime status as additive, optional fields that keep existing runtime-home files and consumers valid. |
| REQ-RAMTEMP-04 | Per-DIMM temperature is added to the control CSV row schema and analyze ingest schema additively; archives and CSVs written before the change still ingest and report. |
| REQ-RAMTEMP-05 | The RAM-temperature read is read-only: it issues no fan write and alters no runtime policy or breaker state. |
| REQ-RAMTEMP-06 | RAM temperature is telemetry-only in this slice; it is not used as a control input (no curve, blend, boost, or safety override keyed on it) until a separate decision and `docs/MEASUREMENT_GATE.md` evidence authorize it. |
| REQ-RAMTEMP-07 | Empirical validity of the target board's DIMM sources is confirmed from existing `evidence-log` output (the `valid` flag and a plausible non-zero reading) before any control-input use is considered. |

## 7. Data / schema deltas

- New/changed fields (proposed, additive, optional): per-DIMM
  `temperature_c` keyed by stable DIMM source label (`Agent0 DIMM0`,
  `Agent0 DIMM1`, `Agent1 DIMM0`, `Agent1 DIMM1`) plus a validity/availability
  marker, in the read/control snapshot, `current_state.json`, status JSON, and
  the control CSV row.
- Config impact (`config/control.*.json`, `config/machines/*.json`): none
  required for telemetry; an optional enable/label-mapping key may be considered
  at implementation.
- Schema/version impact: additive CSV columns
  (`docs/RUNTIME_LOGGING_AND_EVALUATION.md`) and an analyze ingest schema bump
  (`src/analyze/analyze_ingest_db.cpp`; `kSchemaVersion` in
  `src/analyze/analyze_db.h`, currently `9`) at
  implementation, with old-archive compatibility preserved
  (`docs/RUNTIME_HOME.md`).

## 8. CLI / config / operator surface deltas

- `--status` and `--status --json` would gain optional per-DIMM temperature
  fields when valid sources are present.
- `analyze report` would gain RAM idle/load percentiles analogous to
  `cpu_tctl_c` once the CSV columns exist.
- README and the relevant mode docs (`READ_LOOP.md`, `RUNTIME_HOME.md`,
  `RUNTIME_LOGGING_AND_EVALUATION.md`) updated at implementation per
  `AGENTS.md` §Change Checklist.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| `docs/ram-temperature-telemetry-decision-YYYY-MM-DD.md` (proposed) | Sampling tier (control hot-path vs evidence/wider tier); DIMM-source label-to-slot mapping and stability; field naming/availability representation; CSV/schema layout. | Proposed (not written) |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

> Mirrored into `docs/TRACEABILITY.md` only when this spec is promoted out of
> `_parked/`.

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-RAMTEMP-01 | R | Review the read path reuses `read_sio_temperatures`; no `third_party/SVG-MB-SIO` public-API or transport addition. |
| REQ-RAMTEMP-02 | T, R | Test that invalid/zero DIMM sources surface as unavailable, not `0 C`; review against `MbSioTemperatureSnapshot.valid` handling. |
| REQ-RAMTEMP-03 | T, R | Snapshot/status field test; review `RUNTIME_HOME.md` additive-schema compatibility. |
| REQ-RAMTEMP-04 | T | Analyze ingest compatibility test with archives missing the new columns; CSV header/round-trip test. |
| REQ-RAMTEMP-05 | R | Review confirms no write/policy/breaker side effect on the read path. |
| REQ-RAMTEMP-06 | R | Review confirms no control-math/curve/blend/safety dependency on RAM temperature in this slice; cross-check `CONTROL_PIPELINE_MATH.md`. |
| REQ-RAMTEMP-07 | M | `evidence-log` output on the target board shows the DIMM sources `valid` with plausible readings before any control use. |

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| Do the target board's `Agent0/1 DIMM0/1` sources carry valid, non-zero data? | Implementation (telemetry slice) | Unknown; confirm from existing `evidence-log` output (no code needed). |
| Sampling tier: control hot-path vs evidence/wider tier | Implementation | Lean evidence/wider tier first to protect the 250 ms budget; promote to control tier only with cadence evidence. |
| DIMM-source-label → physical-slot mapping and its stability | Implementation | Use the chip source labels verbatim; do not assert slot identity without evidence. |
| Whether RAM temperature ever becomes a control input | A separate decision + `MEASUREMENT_GATE.md` evidence | Out of scope here; telemetry-only. |

## 12. Measurement gate & dependencies

- **Measurement gate:** the telemetry slice does not cross
  `docs/MEASUREMENT_GATE.md` (no cadence, channel, or mixed-input strategy
  change). Any future use of RAM temperature as a control input does cross it
  and requires characterization evidence first.
- **Depends on:** the already-vendored `third_party/SVG-MB-SIO` temperature read
  path; no dependency on other `FEAT-*`.
- **Build/test impact:** new CSV/schema-compatibility tests, an analyze schema
  bump with old-archive coverage, and doc updates per the standard checklist; no
  `CONTROL_PIPELINE_MATH.md` change for the telemetry slice.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [ ] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2). *(Code/contract gap documented; runtime validity of the DIMM sources on the target board still to be confirmed from `evidence-log`.)*
- [ ] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [ ] 3. Required design decision record(s) written and marked current (§9). *(Proposed, not yet written.)*
- [ ] 4. Concrete `REQ-*` IDs assigned from the reserved namespace (§6). *(Drafted; normative on promotion.)*
- [ ] 5. Verification mapped to real checks and mirrored in `docs/TRACEABILITY.md` (§10). *(Mapped in-spec; not mirrored while parked.)*
- [ ] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline.
- [ ] 7. Doctrine check: claims grounded; `must`/`should`/`is` per `CLAUDE.md`; no undefined terms or unqualified vague adjectives.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-RAMTEMP-01 | | | |
| REQ-RAMTEMP-02 | | | |
| REQ-RAMTEMP-03 | | | |
| REQ-RAMTEMP-04 | | | |
| REQ-RAMTEMP-05 | | | |
| REQ-RAMTEMP-06 | | | |
| REQ-RAMTEMP-07 | | | |

**Spec vs. implementation deltas:** <record at implementation>
