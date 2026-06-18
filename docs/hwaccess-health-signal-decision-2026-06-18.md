# Decision: Hardware-access (PawnIO) health signal — observability scope

**Project:** svg-mb-control
**Status:** Current
**Owns:** FEAT-0004 §9 (`REQ-HWHEALTH-*`) — promotion gate 3.
**Basis:** the 2026-06-04 recovery-gap audit
(`docs/discovery-recovery-gap-audit-2026-06-04.md`, Q1–Q3 and Q6 remediation 2)
and FEAT-0004 §2 (the named code/contract gap with file:line evidence).

## 1. Context

When the PawnIO kernel interface is absent, both hardware paths fail to
initialize and the controller reports the generic terminal status `failed` /
`direct-read-failed`, which `AssessHealthState` maps to `kFailed` → exit code `3`
(`src/runtime/runtime_health.cpp:111-116, 219-232`). The watchdog restarts only
on exit code `2` and returns `3` unchanged (`src/platform/task_runner.cpp:195-205`),
so the condition is never acted on — and a restart could not help, because the
controller does not and (per `AGENTS.md` §Repo Boundary) must not load the driver.
The failure class most able to take the controller offline with no self-recovery
is, at the health surface, indistinguishable from any other terminal failure.

This decision settles FEAT-0004 §9: whether the new signal changes the
exit-code/watchdog contract or stays additive, whether read and write paths each
get their own field, and whether anything acts on the signal.

## 2. Decisions

- **D-HWHEALTH-1 — additive status + event only; exit-code mapping and watchdog
  restart contract unchanged.** A process restart cannot create an absent driver,
  so re-routing this condition to the restart-on-`2` path would induce flapping.
  The value is making the condition *nameable and observable*, not changing
  recovery. Any change to the exit-code semantics is a separate decision.
- **D-HWHEALTH-2 — separate read-path (AMD/SMN) and write-path (SIO/LPC)
  availability fields, tri-state (available / unavailable / unknown).** The audit
  confirmed the two paths open PawnIO independently and can fail independently.
- **D-HWHEALTH-3 — no false-positive availability.** The signal reports
  unknown/unavailable, not healthy, until an observed successful PawnIO open
  (mirrors the no-false-zero rule in FEAT-0002). The existing `failed` /
  `direct-read-failed` values keep their current meaning; the new signal is an
  additive overlay, not a rename.
- **D-HWHEALTH-4 — acting on the signal is out of scope for FEAT-0004.** The
  signal is surfaced (status field + transition event + `--diagnose`/`--health`)
  for an operator or an external supervisor to subscribe to. Whether the watchdog
  or an external recovery agent should act on it, and with what action, is a
  separate future decision.
- **D-HWHEALTH-5 — recovery-gap remediation 2 is a distinct feature, not folded
  in.** The audit's remediation 2 (a degraded/failed channel — exit code `1` — never
  reaches an operator; the watchdog reports health `1` as its own success,
  `docs/discovery-recovery-gap-audit-2026-06-04.md` Q6) is an
  escalation/alerting concern, separate from FEAT-0004's read-only observability.
  It is **not** folded into FEAT-0004 (doing so would re-open this spec's
  deliberately observability-only scope and its promotion gates). It is recorded
  as a future FEAT candidate; a FEAT id / `REQ-*` namespace is **not** reserved
  here, to avoid colliding with the in-flight FEAT-0015/0016 (PR #9) and
  FEAT-0017/0018/0019 intake work.

## 3. Risks & mitigations

- **Misread as a recovery feature.** Mitigation: the spec and §5 state explicitly
  that the controller must not load/start/restart the driver (REQ-HWHEALTH-05);
  the signal is observability only.
- **Schema drift.** Mitigation: fields are additive and tri-state; absence means
  unknown; `docs/RUNTIME_HOME.md` is updated at implementation (REQ-HWHEALTH-03).

## 4. Rollback

The feature is additive status fields + events with no control or exit-code
change, so removal is dropping the fields/events and reverting the
`docs/RUNTIME_HOME.md` delta; no runtime-home file or control behavior depends on
them.
