# FEAT-NNNN: <Feature name>

**Project:** svg-mb-control
**Status:** Template   **Version:** 0.1   **Updated:** YYYY-MM-DD
**Namespace:** `REQ-<AREA>-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
the relevant mode doc (`CONTROL_LOOP.md` / `READ_LOOP.md` /
`WRITE_ORCHESTRATION.md`)
**Purpose:** one-line statement of the feature.

> Copy this file to `FEAT-NNNN-<slug>.md`, fill every section, and add the
> registry row in `README.md`. On copy, change **Status:** from `Template` to
> `Reserved` (or `Draft` once you begin writing in detail).
> Delete these quote-block instructions as you go. Keep claims grounded:
> per `CLAUDE.md`, every statement of current behavior must be verifiable from
> code, CLI behavior, config, tests, or runtime evidence, and forward-looking
> claims must be clearly labeled as proposed, not described as if implemented.
> Use `must` only for enforced rules, `should` only for advisory rules, and
> `is` only for current implemented behavior.

## 1. Summary

<One short paragraph: what the feature is and the operator- or controller-visible
outcome, in plain language. No vague adjectives without a field name, number, or
testable definition (`CLAUDE.md`).>

## 2. Problem & motivation  *(promotion gate 1)*

<The concrete problem, sourced from observed runtime evidence (CSV/status/event
logs) or a code/contract gap — not speculation. Why the current shipped behavior
is insufficient. Cite the evidence: archive CSV path, status field, event type,
or the contract section that is silent or wrong today.>

## 3. Goals & non-goals

**Goals**
- <what this feature will do>

**Non-goals**
- <explicitly out of scope, to prevent creep>

## 4. Stressed invariants  *(promotion gate 2)*

Which repo invariants this feature touches, and how it stays inside them. Every
feature must hold all of them; list the ones it actively stresses.

| Invariant | Source | How this feature stays inside it |
|---|---|---|
| <e.g. Repo stays standalone; no sibling-repo / bridge dependency> | `AGENTS.md` §Repo Boundary | <how> |
| <e.g. No fan write / start / stop / breaker reset outside an explicit live task> | `AGENTS.md` §Live Runtime Safety | <how> |
| <e.g. Shipped 250 ms cadence / channel set is the measured baseline> | `docs/MEASUREMENT_GATE.md` | <how, or which new measurement is required> |
| <e.g. Control-computation identity stays documented and validated> | `docs/CONTROL_PIPELINE_MATH.md` | <how> |
| <e.g. Runtime sidecar / status / manifest schema stays backward-compatible> | `docs/RUNTIME_HOME.md` | <how> |

## 5. Behavior specification

<The normative behavior, written so it is buildable and testable. Describe
states, triggers, defaults, edge cases, and failure behavior. Reference the
relevant contract sections (`CONTROL_LOOP.md`, `READ_LOOP.md`,
`WRITE_ORCHESTRATION.md`, `RUNTIME_HOME.md`, `CONTROL_PIPELINE_MATH.md`) instead
of restating them. Name the source files/functions the behavior lives in or near
(e.g. `src/control/tick_runner.cpp`).>

## 6. Requirements  *(promotion gate 4 — assign IDs only after the design decision picks a direction)*

| ID | Requirement |
|---|---|
| REQ-<AREA>-01 | <testable requirement> |
| REQ-<AREA>-02 | <testable requirement> |

IDs come from this feature's `REQ-<AREA>-*` namespace, reserved in the registry in
`README.md`. Keep them stable once published.

## 7. Data / schema deltas

<Prose description of any config, runtime sidecar, status, CSV-row, manifest, or
event-schema changes. Per `AGENTS.md` §Documentation Maintenance, the actual
schema edit and any version/schema bump happen at implementation and require
updating `docs/RUNTIME_HOME.md` (sidecar/status/manifest/archive) and/or
`docs/RUNTIME_LOGGING_AND_EVALUATION.md` (CSV columns / evidence). State
backward-compatibility: no existing runtime-home file, archive, or config may
become invalid.>

- New/changed fields: <field: type, default, optional?>
- Config impact (`config/control.*.json`, `config/machines/*.json`): <none | which keys>
- Schema/version impact: <none | bump needed at implementation, in which doc>

## 8. CLI / config / operator surface deltas

<New or changed CLI subcommands, flags, config keys, runtime-home request files
(e.g. breaker reset), status fields, or operator workflow. UI work is out of
scope for this repo (`docs/MEASUREMENT_GATE.md`). Update `README.md` and the
relevant mode-specific doc when the surface changes (`AGENTS.md` §Change
Checklist).>

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

This repo records decisions as dated files in `docs/` (pattern:
`<topic>-decision-YYYY-MM-DD.md` or `<topic>-design-YYYY-MM-DD.md`, e.g.
`docs/source-aware-blend-decision-2026-05-26.md`). A feature must not be
implemented until each direction-setting decision it depends on is written and
marked current.

| Decision doc | Decision it must settle | Status |
|---|---|---|
| `docs/<topic>-decision-YYYY-MM-DD.md` | <the direction this feature commits to> | <Proposed / Current> |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

What "done" means, and where each requirement is verified. Map every requirement
to a concrete check this repo actually runs. Add or update the matching
requirement row in `docs/TRACEABILITY.md` in the same change.

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-<AREA>-01 | T | `.\scripts\Test-LocalCI.ps1` (`tests/...`) |
| REQ-<AREA>-02 | R, M | code review vs `<contract> §<x>`; runtime CSV/status evidence |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| <question> | <implementation / measurement> | <lean / default> |

## 12. Measurement gate & dependencies

- **Measurement gate:** <does this cross a `docs/MEASUREMENT_GATE.md` boundary
  (faster cadence, more live channels, broader mixed-input strategy)? If so, name
  the characterization evidence required before implementation proceeds.>
- **Depends on:** <other `FEAT-*` or existing shipped capabilities>.
- **Build/test impact:** <new tests, new config fixtures, CONTROL_PIPELINE_MATH
  update, or none beyond the standard checklist>.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [ ] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2).
- [ ] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [ ] 3. Required design decision record(s) written and marked current (§9).
- [ ] 4. Concrete `REQ-*` IDs assigned from the reserved namespace (§6).
- [ ] 5. Verification mapped to real checks — `Test-LocalCI`, build-release, contract review, or runtime evidence (§10), and mirrored in `docs/TRACEABILITY.md`.
- [ ] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline.
- [ ] 7. Doctrine check: claims are grounded; `must`/`should`/`is` used per `CLAUDE.md`; no undefined terms or unqualified vague adjectives.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

The point of writing this spec in advance: after implementation, confirm each
requirement against the running controller and the cited contract. Date each
check; link the test run, build, commit, or runtime-evidence file. Keep
`docs/TRACEABILITY.md` aligned with the final result.

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-<AREA>-01 | | | |
| REQ-<AREA>-02 | | | |

**Spec vs. implementation deltas:** <record anything built differently from this
spec, and why. If behavior changed, update §5/§6, refresh the cited contract docs
per `AGENTS.md` §Change Checklist, and bump **Updated**.>
