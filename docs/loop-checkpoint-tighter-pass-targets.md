# Loop Checkpoint — Tighter-Pass Targets

**Theme:** discovery pass — find code seams worth a tighter follow-up investigation
**Date:** 2026-06-14
**Storage root:** `docs/`
**Paths searched:** `docs/` (no project `memory/`; 9 pre-existing `discovery-*.md` = prior coverage)
**Passes run:** 2 verified fan-outs (`wf_45e89d06-a60` pass 1, `wf_32fc8e34-cee` pass 2)
**Last area:** pass-2 Wave-2 seams (actuation-math, config-validation, units-scaling, gpu-lifecycle,
low-band-integrator, calibration, persistence-retention, shutdown-wiring, install-scripts,
build-release, control-math, write-actuation re-run)
**Next area:** EXECUTION, not more discovery — see "Recommended Next". Residual discovery seams are
listed under "Still uncovered" (mostly G3-offline / defense-in-depth / already-known).
**Resume instruction:** `/loop run config-validation` (Cluster A, cheapest high-value) OR
`/discover health/recovery` for HR-2

---

## Goal Frame

| Goal ID | Priority | Goal | Success Signal |
|---|---|---|---|
| G1 | P1 | Correctness / reliability | Named code-evidenced gap + bounded remediation |
| G2 | P2 | Cost / performance | Measurable redundant work on hot/batch path |
| G3 | P3 | Maintainability | Drift/duplication = future-regression vector |

---

## Coverage Counts

| Area | Pass | Result |
|---|---|---|
| concurrency, hw-read, health-recovery, analyze-perf, control-perf, error-seams, dup-deadcode, test-blindspots, doc-drift | 1 | swept — 26 survivors (HR-2 top) |
| write-actuation | 1 crashed / 2 re-run | WAC-1/3/4 = FEAT-0005 confirmed (doc maintenance only) |
| actuation-math, config-validation, units-scaling, gpu-lifecycle, low-band-integrator, calibration, persistence-retention, shutdown-wiring, install-scripts, build-release, control-math | 2 | swept — 25 survivors (W2-2 top new) |

Two passes: 22 distinct seams, 76 raw findings, **51 verified survivors**, 25 dropped by verify.

---

## Budget Counters

| | Pass 1 | Pass 2 | Total |
|---|---|---|---|
| Agents | 48 | 54 | 102 |
| Subagent tokens | ~3.37M | ~3.68M | ~7.05M |
| Wall time | ~23 min | ~38 min | ~61 min |
| Tool uses | 741 | 1053 | ~1794 |
| Raw → survived | 36→26 | 40→25 | 76→51 |

- Method: static read+grep only (no execution / no coverage instrumentation).
- Verify layer corrected 2 would-be-wrong calls: W1 (NaN unreachable → reachable via 0.0=AM-1) and
  W3 (duty→raw scaling confirmed CORRECT, dropped to a test gap).

---

## Promoted Candidates (combined, ranked)

Verification: **verified** = survived adversarial verify.

| Rank | Candidate | Pass | Goal | Impact | Effort | Verification | Next Skill |
|---|---|---|---|---|---|---|---|
| 1 | HR-2 wedged-worker force-recovery | 1 | G1 | high | medium | verified (promote) | /discover health/recovery |
| 2 | W2-2 duplicate channel → fighting state machines + sidecar corruption | 2 | G1 | high | low | verified (promote) | /loop run config-validation |
| 3 | EH-2/3/4/5 silent write/append failures | 1 | G1 | medium | medium | verified (promote) | /discover error-handling |
| 4 | GPU-INIT-1 NVML no-retry permanent demotion | 2 | G1 | medium | medium | verified (promote) | /loop run gpu-lifecycle |
| 5 | Merged curve-shape gap (W2-1+AM-2) | 2 | G1 | medium | low | verified | /discover curve-validation |
| 6 | AM-1 GPU 0.0-as-cold (scope w/ GPU-INIT-1) | 2 | G1 | medium | low | verified (sharpen) | /loop run gpu-lifecycle |
| 7 | W6-2 reconcile-fail refuse-boot + orphaned fan | 2 | G1 | medium | medium | verified | /discover reconcile-policy |
| 8 | W10-2 watchdog Disabled until next build | 2 | G1 | medium | medium | verified | /discover watchdog-reenable |
| 9 | W9-1+W9-2 no pre-logon recovery + false rationale | 2 | G1+G3 | medium | medium | verified | /discover prelogon-recovery |
| — | HW-01+CONC-3+F1, AP-2, F3, HR-1+HR-4 | 1 | G1/G2 | med | med | verified | see plan-feed |
| — | W3-3, W5-1, W7-1/3, W2-3/4, W8-A/B, W6-3, WAC-* | 2 | mixed | low | low | verified (Later) | bundle into clusters |

---

## Still uncovered (after 2 passes — pass-2 critic; diminishing returns)

- `runtime_health.cpp` AssessHealthState backward-clock / **DST fall-back** age-clamp masking
  (once-a-year guaranteed HR-2∩HR-4 trigger) — `/discover health-staleness clock-robustness`
- supervisor restart-policy asymmetry (give-up-once startup vs retry-forever after) — pairs W6-2
- cross-layer CSV column-contract drift → **schema-validator** skill (not agents)
- 01452dc trailing-header-skip parser untested (same commit as F3)
- PCI-mutex `WAIT_ABANDONED` (= pass-1 CONC-5, low); `IsProcessActive` exit-259 (unreachable today)
- analyze ingest dedup/transaction correctness (offline G3)

---

## Open Questions (maintainer must decide)

- HR-2: bounded `TerminateProcess` on our own PID acceptable under Repo Boundary?
- Curve-shape contract (W2-1/AM-2): reject-on-load duty-inversion vs documented monotonic contract?
- W6-2: reconcile-fail refuse-boot vs start-loop-and-retry? supervisor give-up-after-one intended?
- W10-2 / W9: boot-independent watchdog re-enable design; is auto-logon on SND-DESK? SYSTEM/S4U principal viable?
- FEAT-0005: un-park (gate 3) now that per-tick RPM/duty readback is confirmed available?
- W7-1: `log_retain_days/rotate_hours==0` — document-and-throw (like csv_flush) or support with a byte cap?

---

## Recommended Next

**Stop broad fan-out — switch to execution.** Two passes covered the live reliability surface;
the residual is mostly G3-offline / defense-in-depth / already-known. Highest-value next moves:
1. **Cluster A — `/loop run config-validation`** (W2-2 duplicate-channel is low-effort/high-value).
2. **Cluster B — `/loop run gpu-lifecycle`** (GPU-INIT-1 + AM-1, one `available=false` predicate).
3. **HR-2 — `/discover health/recovery`** (fold in the critic's DST-masking + supervisor asymmetry).
4. Cheap targeted follow-ups: **schema-validator** skill for CSV cross-layer drift; 01452dc test gap.
- `/planner` is NOT eligible for any item (no Accepted+build-authorized FEAT).

Source plan feed: `docs/discovery-loop-plan-tighter-pass-2026-06-14.md`.
