# Loop Rotation State

**Storage root:** `docs/`
**Last updated:** 2026-06-14

Tracks which seams the `/loop` discovery operator has swept, so repeated passes rotate to
fresh areas instead of re-reading covered ground. Not authoritative; see
`docs/discovery-loop-plan-tighter-pass-2026-06-14.md` for the ranked output.

## Goal frame (stable)

| Goal ID | Priority | Goal |
|---|---|---|
| G1 | P1 | Correctness / reliability |
| G2 | P2 | Cost / performance |
| G3 | P3 | Maintainability / architecture |

## Seams swept

| Date | Seam | Pass type | Result |
|---|---|---|---|
| 2026-06-14 | concurrency / locking | fan-out | swept |
| 2026-06-14 | hw-read / PawnIO / MSR | fan-out | swept |
| 2026-06-14 | health / recovery state machine | fan-out | swept — HR-2 top promote |
| 2026-06-14 | analyze pipeline perf | fan-out | swept — AP-2 lead |
| 2026-06-14 | control hot-path perf | fan-out | swept |
| 2026-06-14 | error-handling seams | fan-out | swept — EH cluster |
| 2026-06-14 | duplication / dead code | fan-out | swept |
| 2026-06-14 | test blind spots | fan-out | swept |
| 2026-06-14 | doc-vs-code drift | fan-out | swept |
| 2026-06-14 | write-actuation | fan-out | **finder crashed** — covered by hand (FEAT-0005 parked) |

## Wave 2 swept (pass 2, 2026-06-14, `wf_32fc8e34-cee`)

| Seam | Result |
|---|---|
| actuation-math (`control_policy.cpp`) | AM-1: 0.0-sentinel→curve-min reachable (NaN unreachable) |
| config-validation | **W2-2 dup-channel (G1/high)**; curve-shape; W2-3/W2-4 silent drops |
| units-scaling (`duty_pct→raw`) | conversion CORRECT; only W3-3 test gap |
| gpu-lifecycle | **GPU-INIT-1 no-retry demotion (G1)** |
| low-band-integrator | W5-1 (no unit test, G3) |
| calibration | W6-2 reconcile-fail refuse-boot; W6-3 no singleton |
| persistence-retention | W7-1 retain=0 no-prune; W7-3 CSV-open silent |
| shutdown-wiring | W8-A/W8-B (restore not run on OS shutdown) |
| install-scripts | W9-1/W9-2 no pre-logon recovery + false rationale |
| build-release | W10-1/W10-2 watchdog Disabled until next build |
| control-math | dt math broadly clean |
| write-actuation (re-run) | WAC-1/3/4 = FEAT-0005 confirmed (doc only) |

## Still not swept (pass-2 critic — low priority, diminishing returns)

- `runtime_health.cpp` AssessHealthState DST/backward-clock age-clamp masking (G1, once-a-year)
- supervisor restart-policy asymmetry (startup-give-up vs retry-forever)
- cross-layer CSV column-contract drift (use `schema-validator` skill, not a fan-out)
- 01452dc trailing-header-skip parser test gap; analyze ingest dedup/transaction correctness (G3)
- already-covered re-raises: PCI-mutex WAIT_ABANDONED (CONC-5), IsProcessActive exit-259 (unreachable)

## Status: broad fan-out COMPLETE — switch to execution

Two passes (102 agents, ~7M tokens, ~61 min) covered the live reliability surface. Recommend no
pass 3; execute the top clusters instead (see `loop-checkpoint-tighter-pass-targets.md`).

## Rotation rule reminders

- Avoid re-sweeping a 2026-06-14 seam unless code materially changed or a finding needs validation.
- Prior `docs/discovery-*.md` (control-math, gpu-response, polling-logging, recovery-gap, etc.)
  count as covered ground.
- Every 5th pass or after 3 no-delta passes: run a cross-cutting sentinel sweep.
