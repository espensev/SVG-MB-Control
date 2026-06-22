# Decision: the runtime event JSONL gets a retention bound (rotation + severity-aware persistence)

**Project:** svg-mb-control
**Status:** Current (Accepted 2026-06-18)
**Owning feature:** `docs/features/FEAT-0015-event-log-retention.md`
(`REQ-EVENTRET-*`)
**Companion to:** `AGENTS.md`, `docs/RUNTIME_HOME.md`,
`docs/RUNTIME_LOGGING_AND_EVALUATION.md`, `docs/MEASUREMENT_GATE.md`,
`docs/discovery-runtime-disk-growth-2026-06-14.md`

## Context

`logs/svg_mb_control_events.jsonl` is append-only with no rotation, age cap, size
cap, or severity filter. `AppendRuntimeEvent` (`src/runtime/runtime_event_log.cpp:210`)
resolves the path, serializes one NDJSON line, and writes it in append mode; no
other code path prunes it. The `log_rotate_hours` / `log_retain_days` keys drive
`RuntimeCsvLogger` for the **CSV archive only** (`src/runtime/evidence_log.cpp:78-87`).
On the 2026-06-14 snapshot (`docs/discovery-runtime-disk-growth-2026-06-14.md`,
Finding 1) the file was 212 MB / 586,825 lines, of which **98.8% (579,565 lines)
were `control_loop.write_applied`** — an `info`-severity per-write record emitted at
`src/control/channel_write.cpp:384`. The same investigation observed a NUL byte
mid-file, indicating at least one partial/torn write.

This is the issue [#4](https://github.com/espensev/SVG-MB-Control/issues/4) Finding 1
design gap, not an unset config value.

## Options considered

- **A — size/age rotation only.** Rotate the active event JSONL to an archived
  file on a size/age bound and delete archives past a retention window, mirroring
  the CSV path. Bounds the file but keeps persisting every `write_applied`, so the
  rotation churns and the per-write noise stays in the freshest window.
- **B — severity-based persistence only.** Stop persisting routine `info`
  `write_applied` (sample or drop), always keep `warning`/`error`/`critical` and
  lifecycle events. Cuts ~98.8% of volume but, without rotation, a long-lived
  process can still grow the surviving stream without an absolute cap.
- **A+B — rotation plus severity-aware reduction.** Rotate by size/age **and**
  reduce routine `write_applied` persistence, always keeping non-`info` and
  lifecycle events.

## Decision

**Adopt A+B.** Reasons:

- The two levers attack different failure modes: severity reduction removes the
  98.8% that dominates the file, and rotation guarantees a hard upper bound even
  for an event mix the severity rule does not anticipate. Either alone leaves a
  residual unbounded path (A churns volume; B has no absolute cap).
- No telemetry fidelity is lost: the per-write record already exists at full
  fidelity in the CSV evidence path, so dropping/sampling `write_applied` from the
  **event** log does not lose data — it removes a duplicate.
- Diagnostics are preserved: every `warning`/`error`/`critical` (`InferSeverity`,
  `runtime_event_log.cpp:156`) and lifecycle/transition event stays persisted
  within the retention window (`REQ-EVENTRET-03`).

**Bound (implementation defaults, revisable):** rotate the event JSONL on the same
`log_rotate_hours` / `log_retain_days` window already used for the CSV archive
(one retention story for the operator), and persist `control_loop.write_applied`
at a reduced rate rather than per write. Absent any new/changed config key,
behavior stays the current append (`REQ-EVENTRET-04`), so no existing runtime home
becomes invalid.

**Durability:** each persisted event stays a single atomic append (the current
`FILE_APPEND_DATA` single-`WriteFile`, `runtime_event_log.cpp:299-321`); rotation
must rename/close the active file without splitting a line, using the
`FILE_SHARE_DELETE` handling the reader already anticipates
(`runtime_event_log.cpp:28-40`). The mid-file NUL cause is confirmed or explicitly
bounded at implementation (`REQ-EVENTRET-02`); the single-write append is the
expected mitigation.

## Scope and gate

- **Scope:** event-JSONL management only. The CSV archive retention (within policy)
  and the analyze-DB retention (separate `FEAT-0016`) are out of scope.
- **Measurement gate:** not crossed (`docs/MEASUREMENT_GATE.md`). Logging-side only;
  cadence, channels, and input strategy are unchanged, no control-identity term
  moves, and the control-thread append must stay non-blocking.
- **Schema:** the `svg_mb_control.event.v1` payload is unchanged; rotation adds
  archived-file naming and at most an additive config key. Doc updates at
  implementation: `docs/RUNTIME_HOME.md` and `docs/RUNTIME_LOGGING_AND_EVALUATION.md`.
- **Implementation/verification** are authorized by this decision but are **staged
  for a Windows-host session**, because this repo's build is Windows-only
  (`CMAKE_RC_COMPILER`) and the `Test-LocalCI` C++/Python lanes must verify the
  rotation/atomicity/severity behavior before the spec's §14 log is filled.
