# Campaign: complete the remaining sensible items (2026-06-18)

**Status:** planning note (execution roadmap). This doc is not a feature spec and does
not itself cross the Feature Intake Gate; each phase below that touches product code or a
feature spec carries its own gate.
**Companion to:** `AGENTS.md`, `docs/features/README.md`, `docs/TRACEABILITY.md`,
`docs/next_steps.md`.
**Basis:** two read-only triage sweeps on 2026-06-18 (the four closed-review residuals; the
full backlog = `next_steps.md` + the 14-row feature registry + open decision-debt in
project memory), reconciled with an advisor pass.

## Scope chosen by the maintainer (2026-06-18)

Three forks, answered:

1. **`analyze-native-superset` branch** — *review + merge the whole branch* into `main`.
2. **Campaign depth** — *author the FEAT-0004 / FEAT-0005 safety specs AND authorize the
   FEAT-0001 build* (the most forward option).
3. **Promote-to-Done** — *promote all five* Implemented specs (FEAT-0008 / 0010 / 0011 /
   0012 / 0013) to `Done` on the automated-test + review (T/R) bar.

## How this campaign is governed

- Nothing crosses the Feature Intake Gate without its own `docs/features/FEAT-*` spec, a
  dated decision record, `REQ-*` IDs, and `docs/TRACEABILITY.md` rows in the same change
  (`AGENTS.md` Feature Intake Gate / Change Checklist).
- `tests/test_feature_specs.py` must report 5/5 after every governance edit.
- The Measurement Gate still blocks any control-tick cadence, sub-profile floor, or
  live-channel default change without fresh `analyze`-workflow runtime evidence. **No phase
  in this campaign changes a cadence, floor, or channel default.**
- Each phase lands via the branch → PR → rebase-merge → cleanup pattern used through the
  write-path review; `main` stays releasable between phases.
- Product-code phases (Phase 4) follow TDD: a failing test precedes the implementation.

## Phases (ordered)

Ordering rule: zero-risk closeouts first, then verify-and-close governance, then the
branch-drift reconciliation (it gates the FEAT-0006 doc truth), then spec authoring, then
the one build. Each phase is independently shippable.

### Phase 0 — Safe closeouts (no decision, no risk)

| Item | Artifact | Gate / done-when |
|---|---|---|
| Linux-only test-skip | one-line `@unittest.skipUnless(shutil.which("powershell") or shutil.which("pwsh"), …)` on `tests/test_eval_dashboard.py:193` `test_dashboard_server_help` (`shutil`/`unittest` already star-imported) | Python lane still runs the test on Windows; skips when no PowerShell host on PATH |
| Live-M **disposition** (see §4) | new `docs/write-path-live-validation-protocol-2026-06-18.md` (gate-neutral method record; mirrors `docs/cpu-loop-stall-reproduction-protocol-2026-06-15.md`) | decides the live-M item rather than leaving it open; authorizes no code/config/schema change |
| FEAT-0014 keep-held confirmation | one-line revisit-trigger clarification in `FEAT-0014` §11/§13 (promote when a config introduces `blocked_channels` other than `[6]`, or a second restore caller appears) | no promotion; `test_feature_specs` stays 5/5 |

Not in Phase 0 (moved to Phase 2): de-staling the `next_steps.md` FEAT-0006 bullets — the
analyzer work they reference lives on the superset branch, not `main`, so correcting them
before the merge would document a state `main` does not have.

### Phase 1 — Verify-and-close: promote five Implemented specs → Done

Promote `FEAT-0008`, `FEAT-0010`, `FEAT-0011`, `FEAT-0012`, `FEAT-0013` from `Implemented`
to `Done`. This is verify-and-sign-off, not new code: confirm every owning `REQ-*` row is
`pass` in `TRACEABILITY.md`, each spec §14 verification log is filled, each decision record
is current, then flip the registry status cell + spec header in one change.

- Acceptance bar: T/R, already met (the write-path review closed on this bar). Live-M is
  recorded as supplementary per §4 — not a Done blocker.
- Gate: `test_feature_specs.py` 5/5 (registry / REQ / traceability / verification-log
  consistency); `Test-LocalCI.ps1` green.

### Phase 2 — Reconcile the `analyze-native-superset` branch (37 ahead / 0 behind)

The branch holds the entire first-party energy + APERF/MPERF cycles + power-feedforward
telemetry campaign (analyzer schema v10 derivations, Gate-2 characterization, the Option-C
cycles-per-Joule scope decision, the energy quarantine-exit closeout) plus a fresh
`docs/discovery-runtime-disk-growth-2026-06-14.md` finding. It is `0` behind `main`. The
runtime energy/cycle code slices already reached `main` in an earlier merge; the analyzer
derivations and governance docs did not, so `main`'s `FEAT-0006` v0.6 header is ahead of
`main`'s actual analyzer code.

Steps:

1. Review the 37-commit diff; confirm the slices remain default-off telemetry that gate no
   control path and introduce no cadence/floor/channel default change (Measurement Gate).
2. Triage-read `discovery-runtime-disk-growth-2026-06-14.md` and reconcile it with open
   **PR #9** (FEAT-0015/0016 runtime disk-growth retention intake specs) so the disk-growth
   story has one home, not two. Unbounded runtime log/sidecar growth is the one operational
   risk flagged (it could eventually break the sidecar write the write-path review just
   protected); confirm or discount it from the doc.
3. PR → merge the branch into `main` (rebase or merge per diff size); `test_feature_specs`
   5/5, `Test-LocalCI.ps1` green on the combined tree.
4. **Then** de-stale `next_steps.md`: the avg-watts analyzer report, the APERF/MPERF cycle
   logger, and the effective-frequency derivation are landed-on-`main` after the merge —
   flip those bullets from "open" to done; keep the genuinely-open forward-gates (energy
   `quarantine → validated` marker, criterion-4 locked-freq capture, cycles-per-Joule join
   scope).

### Phase 3 — Author the remaining fail-direction safety specs (Intake-Gate work, no code)

The largest remaining fail-direction hole after the write-path review: a fan physically
stalled while the controller believes its write succeeded ("quiet-and-hot") has no detector
today. Author specs only; build authorization is a separate later step.

1. **FEAT-0004** (`REQ-HWHEALTH-*`, Draft) — finish promoting the PawnIO-availability health
   signal: write the §9 dated decision record (which health field(s); read-path vs
   write-path distinction; additive `RUNTIME_HOME` schema), clear promotion gate 3, then
   Draft → Accepted. Observability-only, lower risk; it also names the one failure class a
   process restart cannot fix. Sequenced first because it unblocks FEAT-0005.
2. **FEAT-0005** (`REQ-ACTCONFIRM-*`, Reserved/parked) — promote the parked body back to
   Draft → Accepted: write-actuation confirmation / non-actuating-write detection (the
   actuation-truth signal; needs an RPM-vs-duty expectation model). Sequenced behind
   FEAT-0004.
3. **Recovery-gap rem-2** (operator escalation/paging for a degraded/failed channel the
   watchdog currently reports as healthy `1`) — decide whether it folds into FEAT-0004 or
   earns a new FEAT row; capture in the relevant decision record.

Per-item gate: dated decision record + `REQ-*` IDs assigned + acceptance criteria +
`TRACEABILITY.md` rows in the same change; `test_feature_specs` 5/5; status reaches
`Accepted`. **Accepted ≠ build-authorized.**

### Phase 4 — Build FEAT-0001 (hot-swap runtime write policy) — TDD

`FEAT-0001` (`REQ-WRITEPOLICY-*`) is the one `Accepted` spec with all seven promotion gates
cleared and no open decision/evidence blocker; build is authorized by this campaign's
fork-2 answer. It lets `writes_enabled` / `blocked_channels` change live, and it is the
precursor that `FEAT-0014`'s restore-path policy consultation depends on.

- TDD: failing C++ test first for each `REQ-WRITEPOLICY-*` slice, watched RED → GREEN.
- Honor the spec's safety gates: a build-then-swap failure path must retain the prior
  writer/policy (REQ-WRITEPOLICY-03); any live channel-unblock stays behind the Measurement
  Gate (REQ-WRITEPOLICY-07, R+M).
- Fill the §14 verification log; flip `Accepted → Implemented`; `TRACEABILITY` rows to
  `pass`; `test_feature_specs` 5/5; `Test-LocalCI.ps1` green.
- This is the heaviest item (L). It is sequenced last and checkpointed with the maintainer
  before landing.

## Dependency spine

- `FEAT-0004` → `FEAT-0005` (FEAT-0005 is sequenced behind the hardware-health signal).
- `FEAT-0001` → `FEAT-0014` (restore must consult the runtime write policy FEAT-0001 owns)
  and underpins the held `FEAT-0003`.
- Phase 2 (superset merge) → the `next_steps.md` FEAT-0006 de-stale (truth depends on the
  merge landing first).
- Phase 1 promotions are independent of all other phases.

## §4. Live-M disposition (decided here, not left open)

Acceptance for `FEAT-0010/0011/0012/0013` is already met: every §10 requirement row is
`T` or `T,R`; **none requires `M`** (manual runtime measurement). Live-M is therefore
supplementary defense-in-depth, not a closure gate. Disposition per feature:

- **FEAT-0010** (sidecar persist fault must not veto the write) — **live-reproducible on
  the production path safely**: hold an exclusive handle on `pending_writes.json` so the
  atomic write throws; observe `control_loop.sidecar_upsert_failed`, the persist-failure
  counter increment, degraded health, **and the fan duty still tracking setpoint**. Low risk
  (the fan is still commanded).
- **FEAT-0012** (corrupt sidecar quarantine) — **live-reproducible with an operator-gated
  controlled restart**: back up then corrupt `pending_writes.json`, restart, observe
  quarantine to `.corrupt`, `reconcile.sidecar_quarantined`, degraded health, and the loop
  ticking (no relaunch-thrash). Moderate risk; cool idle window, file backed up.
- **FEAT-0011** (half-open breaker probe) and **FEAT-0013** (source-aware CPU-dropout safe
  mode) — **no safe production-path trigger**: FEAT-0011 needs five real `ApplyDuty`
  failures then rising demand (the channel parks at last duty meanwhile); FEAT-0013 needs a
  forced CPU-sensor NaN for three ticks. Both are recorded as **proxy-validated** (separate
  runtime-home with the simulated writer / injected inputs — barely beyond the existing
  C++ tests) **or T-only-closed**, mirroring how FEAT-0008 §14 rejected the AVX-512
  escalation as the wrong instrument.
- **Precondition for any live run:** the box is currently unstable (6–9 whole-system halts
  in 11 days; see Parked §C). Live-M on hardware is deferred until the platform is
  stabilized; proxy validation has no such dependency.

When/if M evidence is captured, it appends one `M` row to each owning spec's §14 plus a
`TRACEABILITY.md` mirror (a later docs-only change), following the `FEAT-0008` §14 model.

## §C. Explicitly parked / out-of-band (NOT in this campaign)

Listed so nothing is silently dropped; each carries its revisit trigger.

- **Hardware/BIOS instability** (`system-halt-incident` memory) — 6–9 whole-system halts in
  11 days across five unrelated subsystems; the controller is a confirmed victim. Operator
  work only (EXPO → 4800, disable iGPU, MemTest86, off the beta BIOS; change one thing for
  days). **Out-of-band; it gates every load-sensitive experiment below.**
- **FEAT-0009 §12 priority-contention A/B experiment** — a real repo task, but its results
  are confounded by the platform instability, and the dominant Layer-0 stall mechanism was
  already addressed by the merged write-path fixes. Parked behind hardware stabilization.
- **Criterion-4 locked-frequency capture** and **divergent-load feedforward capture** —
  operator hardware steps; also gated behind a stable box.
- **FEAT-0008 post-v1 options** (configurable grace period, `--stop` escalation, pre-first-
  write PID-corroboration fallback, image-path canonicalization) — all non-blocking per
  FEAT-0008 §11; current fail directions are safe. Revisit on observed need.
- **FEAT-0003** (selectable control-law profile) — recorded not-a-net-benefit; held until a
  multi-profile need exists.
- **FEAT-0007** (per-DIMM RAM temperature) — Reserved/parked; needs DIMM-source-validity
  evidence + a sampling/schema decision first.
- **Power-feedforward as a control input** — Gate-2 NO-GO (0.0 s onset lead over the free
  `system_cpu_busy_pct`); reopen only on a divergent-load session. Energy stays evidence
  telemetry.
- **Cycles-per-Joule package join** — blocked on the core-0-cycles vs whole-package-energy
  scope mismatch; awaiting the Layer-2 decision (options doc recommends Option C).
- **Include-ownership** (module-qualified `#include` paths) — team decision; do nothing
  until decided.
- **Low-pri housekeeping** (archive closed records; split the 17 flat Python tests) — both
  churn cross-references / risk shared `helpers.py` imports for no functional gain; deferred
  per `next_steps.md`.
- **Known scoped write-path residuals** — legacy `MaxCpuGpu` CPU-dropout masking (latent;
  no shipped channel uses it), safe mode is a rate-limited ramp not an instant jump,
  FEAT-0012 health stays degraded until `.corrupt` is removed. Recorded, not defects; no
  action.

## Checkpoints

- After Phase 0 + Phase 1: report; these are low-risk and can land back-to-back.
- Before Phase 2 merge: confirm the superset diff review summary with the maintainer (the
  PR #9 reconciliation is a judgment call).
- Before Phase 4 landing: checkpoint the FEAT-0001 implementation with the maintainer
  (heaviest, real-code change).
