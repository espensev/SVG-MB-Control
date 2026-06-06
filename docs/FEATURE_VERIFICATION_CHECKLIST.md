# svg-mb-control - Feature verification checklist

**Project:** svg-mb-control
**Status:** Accepted   **Version:** 0.1   **Updated:** 2026-06-06
**Companion to:** `AGENTS.md`, `docs/features/README.md`, `docs/TRACEABILITY.md`
**Purpose:** practical checklist for taking a feature from spec to verified
implementation without bypassing the specs-before-build gate.

## 1. Before implementation

- Identify the owning `docs/features/FEAT-*.md` spec.
- Confirm the feature is implementation-authorized. `Reserved` and `Draft`
  specs are planning only. `Accepted` specs are buildable only when the spec text
  or maintainer explicitly authorizes implementation.
- Confirm every promotion gate in the feature spec is closed, or stop and land
  the missing decision/evidence/spec update first.
- Confirm every `REQ-*` in the feature spec appears in
  `docs/TRACEABILITY.md` with a concrete verification home.
- If the feature changes control math, runtime schemas, CLI/operator behavior,
  runtime-home artifacts, or tuning workflow, list the contract docs that must
  be updated in the same implementation change.
- If the feature requires manual/runtime evidence, state the exact safe command,
  runtime home, CSV/status/event artifact, and no-live-write assumptions before
  the run starts.

## 2. During implementation

- Keep the change scoped to the owning feature's goals and non-goals.
- Preserve Repo Boundary: no sibling repo, bridge subprocess, or third-party
  sensor dependency unless the accepted spec explicitly permits it.
- Preserve Live Runtime Safety: do not start/stop/restart, reset breakers, or
  write fan duty unless the task explicitly requires live runtime interaction.
- Keep schema changes additive unless the feature spec and contract docs call
  out a versioned breaking change.
- Add or update automated tests for every `REQ-*` mapped to **T** in
  `docs/TRACEABILITY.md`.
- Update contract docs in the same change as behavior, CLI, runtime-home,
  analyzer, control-math, or tuning workflow changes.

## 3. Before handoff

- Fill the owning feature spec's verification log for every implemented
  requirement.
- Update `docs/TRACEABILITY.md` result values to match the verification log.
- Record any spec-vs-implementation delta in the owning feature spec before
  calling the work done.
- Run the required validation:
  - docs-only: read back edited docs and check `git diff`;
  - feature/spec traceability: `python -m unittest tests.test_feature_specs -v`;
  - C++ behavior: `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`;
  - release/package behavior: `.\build-release.ps1` or
    `scripts\Build-Release.ps1` with the narrowest safe options.
- Preserve runtime evidence as compact summaries or decision records by default;
  do not commit raw runtime CSV captures unless explicitly needed.
