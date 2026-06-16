# CPU Control-Loop — Live FEAT-0008 Sweep Findings — 2026-06-16

Status: **measurement record (neutral).** Records a bounded starvation sweep run
against the **live, FEAT-0008-enabled** controller immediately after deploying the
force-terminate feature, plus the deterministic suspend used to bank the
REQ-WATCHDOG-01 (M) recovery evidence. It draws no Layer-0 sizing decision; Phase 2
and the maintainer own that. `AGENTS.md` §Feature Intake Gate held: the controller
ran at its shipped `BelowNormal` priority (no Layer-0 fix applied), and the only
added component was the default-OFF, read-only synthetic load generator. Companion
to `docs/cpu-loop-stall-reproduction-protocol-2026-06-15.md` (the method),
`docs/cpu-loop-stall-reproduction-findings-2026-06-16.md` (the first pass), and
`docs/features/FEAT-0008-watchdog-hung-worker-recovery.md` (the feature).

Scope:

- In scope: the deterministic suspend recovery measurement (FEAT-0008 mechanism),
  the per-cell starvation results, the watchdog-restart-under-load observation, and
  the thermal bound.
- Out of scope: any Layer-0 threshold/priority decision; the AVX-512 power-matched
  escalation (protocol §5, **not** run); a multi-run probability campaign.

## 1. Setup

- **Machine:** 32 logical CPUs. **Controller:** deployed FEAT-0008 build
  (`sourceCommit e5bafdb`, `build-release.ps1` 2026-06-16), shipped `BelowNormal`
  priority, `poll_tick_ms = 250` (nominal 4 ticks/s). Watchdog scheduled task
  enabled (`PT1M` cadence, `PT2M` time limit), `stale_after_ms = 10000`.
- **Instrument:** `cpu-synth-load.exe` rebuilt from the current `tools/cpu_synth_load.cpp`
  (the shipped-in-`release\runtime\experiments` binary predates the
  `--priority/--pin/--oversubscribe` knobs); AVX2 FMA saturator, default-OFF,
  read-only (no MSR/PCI/fan writes).
- **Detector (CAVEAT — not the canonical one):** this pass used an *ad-hoc*
  orchestrator (Appendix A), not the repo's
  `release/runtime/experiments/loop-stall/Measure-StallCell.ps1`. It sampled
  `control_runtime.json` `loop_tick_count` / `status` / `cpu_tctl_c` (from the live
  CSV) every ~3 s and flagged a "stall" on two consecutive zero tick-deltas
  (~6 s frozen), then killed the load to remove heat. This is **cruder** than the
  canonical cadence/write-gap detector and its sampler runs at normal priority, so
  it can itself be starved; treat its "stall" labels as *degraded-cadence*
  indicators, and use the `events.jsonl` supervisor lifecycle as the authoritative
  signal.
- **Authoritative signal:** `release/runtime/logs/svg_mb_control_events.jsonl`.
- **Safety envelope:** bounded 45 s cells, 20 s cooldown, `cpu_tctl_c ≥ 85 °C`
  abort, stall-detection kills the load immediately, SMU 95 °C hardware throttle as
  the backstop, FEAT-0008 force-terminate live to recover a genuine hard freeze.

## 2. Method

Two measurements against the live loop:

1. **Deterministic recovery (FEAT-0008 mechanism, REQ-WATCHDOG-01 M):** suspend the
   live worker with `NtSuspendProcess` (a guaranteed hung-worker proxy that cannot
   poll the stop sentinel), wait past the 10 s staleness, trigger the production
   watchdog task, and observe the recovery from `events.jsonl`.
2. **Bounded starvation sweep (characterization, n = 1 per cell):** escalate AVX2
   load while the controller stays at `BelowNormal`.

| Cell | `cpu-synth-load` args |
|---|---|
| Baseline | `--threads 32 --priority normal --seconds 45` |
| A2 | `--threads 32 --priority high --seconds 45` |
| B | `--threads 32 --priority high --pin --seconds 45` |
| C2 | `--oversubscribe 4 --priority high --pin --seconds 45` |

## 3. Results

### 3.1 Deterministic recovery (REQ-WATCHDOG-01 M) — recovered

Suspended live worker pid 44984 (06:12:39) → watchdog `--restart` → 15 s graceful
stop timed out → **force-terminated** and relaunched as pid 36348 (06:13:09).
Authoritative event (06:13:06):

```
supervisor.worker_force_terminated  "force-terminated hung worker pid=44984 after graceful stop timed out (stop_result=2)"
```

Exactly **one** `worker_force_terminated`, **zero** `supervisor_force_terminated`
(the supervisor self-exited per D1), **zero** `force_terminate_failed`. Old pid
confirmed gone; loop resumed ticking.

### 3.2 Starvation sweep (n = 1 per cell)

| Cell | peak `cpu_tctl_c` | loop outcome | restart? |
|---|---|---|---|
| Baseline (normal) | 75.6 °C | cadence degraded to intermittent (Δtick → 0 for ~6 s), then recovered, **same pid** | no |
| A2 (high) | 76.4 °C | watchdog **graceful** restart (worker honored the stop) | yes — pid 36348 → 37536 @ 06:24:22 |
| B (high+pin) | 75.6 °C | cadence degraded, recovered, **same pid** | no |
| C2 (high+pin+os×4) | 75.9 °C | watchdog **graceful** restart | yes — pid 37536 → 44676 @ 06:26:51 |

Both restarts were graceful and watchdog-initiated, confirmed by the supervisor
lifecycle (not a crash-loop — `restart_count=0` with a fresh supervisor):

```
06:24:22  worker_exited   pid=36348 exit_code=0 stop_requested=true
06:24:22  supervisor.shutdown      (old supervisor exits)
06:24:22  supervisor.start         (new supervisor — the --restart relaunch)
06:24:22  worker_started  pid=37536 restart_count=0
```

(identical shape at 06:26:51 → pid 44676). The watchdog is the only `--restart`
source during the sweep. **Zero** force-terminations occurred across all cells.

## 4. Findings

1. **REQ-WATCHDOG-04 negative gate, confirmed live.** Under real AVX2 starvation
   the worker degraded but always honored the 15 s graceful stop; the watchdog
   recovered it **without** force-terminate (0 force-kills, 0 `force_terminate_failed`).
   Force-terminate is correctly gated to the hard-freeze (`stop_result==2`) case.
2. **Watchdog/staleness-vs-load over-sensitivity (the operationally notable one).**
   A `BelowNormal` worker under sustained legitimate load degrades enough to exceed
   the 10 s `stale_after_ms`, so the watchdog `--restart`s a *degraded-but-alive*
   worker — and the relaunched worker meets the same load (two restarts in
   ~2.5 min here). Each restart briefly drops fan-control authority and re-runs
   startup reconcile. This is recovery of a worker that was *slow*, not *broken*,
   and it directly reinforces the deferred **L0-B priority-elevation** lever
   (raising the worker above `BelowNormal` would keep its status fresh under load).
3. **AVX2 did not reproduce a hard freeze — but at n = 1.** No cell produced a
   worker that missed the graceful stop. Consistent with the protocol's premise
   that the hard-freeze is *probabilistic* and likely needs the AVX-512
   power-matched escalation (§5). A single bounded run per cell **cannot** estimate
   the stall rate; this is not evidence the hard freeze "cannot" occur.
4. **Thermal bound.** Sweep-wide peak `cpu_tctl_c` 76.4 °C (cell A2; ~18.6 °C below
   the 95 °C SMU throttle); no abort triggered. AVX2 on this liquid loop, even with
   fans held by a degraded controller, stayed well inside the envelope for bounded
   45 s cells.

## 5. Caveats

- **n = 1 per cell** — characterization, not a probability estimate.
- **Non-canonical detector** (Appendix A) — cruder than `Measure-StallCell.ps1`;
  its sampler can be starved; "stall" labels are degraded-cadence indicators, the
  `events.jsonl` lifecycle is authoritative.
- **Mechanism vs premise** — §3.1 proves FEAT-0008 *recovers* a hung worker
  (mechanism, via an `NtSuspendProcess` proxy); it does **not** prove a natural
  hard freeze occurs. That premise question is open (item 3 + protocol §5).

## 6. References

- `docs/cpu-loop-stall-reproduction-protocol-2026-06-15.md` — method, cells, §5 escalation.
- `docs/cpu-loop-stall-reproduction-findings-2026-06-16.md` — first reproduction pass.
- `docs/features/FEAT-0008-watchdog-hung-worker-recovery.md` — the feature (§14 verification log).
- `docs/cpu-loop-survival-layer0-plan-2026-06-16.md` — Layer-0 plan (L0-B priority lever).

## Appendix A — Sweep orchestrator + detector thresholds (for audit)

Ad-hoc PowerShell orchestrator (run from `%TEMP%`, not a shipped tool). Per cell:
record pre `tick`/`cpu_tctl_c`; launch `cpu-synth-load.exe` with the cell args
(self-terminating at `--seconds 45`); sample `control_runtime.json` +
`cpu_tctl_c` (from `log_csv_path`) every 3 s; **abort** the whole sweep if
`cpu_tctl_c ≥ 85.0`; flag **stall** and kill the load on two consecutive zero
tick-deltas (~6 s of frozen `loop_tick_count`); after the load ends, watch up to
60 s for a pid change / events; 20 s cooldown between cells. Recovery/attribution
read from `events.jsonl` (`supervisor.worker_exited` / `supervisor.shutdown` /
`supervisor.start` / `supervisor.worker_started` / `supervisor.worker_force_terminated`).
Detector limitation: a normal-priority sampler under a `high`-priority pinned load
can be starved, inflating apparent zero-deltas — hence `events.jsonl` is treated as
the authoritative signal and "stall" labels as degraded-cadence indicators.

## Appendix B — Restart attribution re-verified against `events.jsonl` ground truth (2026-06-16)

A later independent eval flagged the §3.1/§3.2 restart attributions for direct
verification against the authoritative log rather than the prose summary. Re-pulled
from `release/runtime/logs/svg_mb_control_events.jsonl` (the supervisor lifecycle
events from the M-evidence onward). The **`exit_code` field is the decisive
force-vs-graceful discriminator** the original tables did not surface:
`TerminateProcess(process, 1u)` makes a force-killed worker exit with `exit_code=1`,
while a worker that honors the stop sentinel exits `exit_code=0`.

| `worker_exited` (time) | pid → relaunch | `exit_code` | `worker_force_terminated`? | verdict |
|---|---|---|---|---|
| 06:13:06 | 44984 → 36348 | **1** | **yes** — `… stop_result=2` | force-terminated (the deterministic suspend, REQ-WATCHDOG-01 M) |
| 06:24:22 | 36348 → 37536 | **0** | none | graceful watchdog `--restart` (cell A2) |
| 06:26:51 | 37536 → 44676 | **0** | none | graceful watchdog `--restart` (cell C2) |

Whole-log tallies (text-mode scan, treating the 686 k-line jsonl as text):

- Exactly **one** `supervisor.worker_force_terminated` in the entire log — the
  06:13:06 suspend (pid 44984, `stop_result=2`, `exit_code=1`). No other.
- **Zero** `supervisor.supervisor_force_terminated`; **zero**
  `supervisor.force_terminate_failed`.
- Each restart shows the full graceful shape (`worker_exited stop_requested=true`
  → `supervisor.shutdown` → `supervisor.start` → `worker_started restart_count=0`),
  and the two sweep restarts carry `exit_code=0` — confirming §3.2's "graceful,
  watchdog-initiated, not a crash-loop" claim from primary data, not inference.
- Cell ordering corroborates load-attribution: only two restarts followed the
  M-evidence relaunch, at 06:24:22 and 06:26:51 — bracketing cell B (`high+pin`),
  which did **not** restart (same pid), exactly as §3.2 reports for Baseline/B.
- Current live worker pid **44676** has run with **no further restart** since
  06:26:52 (loop stable since the sweep).

Conclusion: §3.1 (force-terminate M-evidence) and §3.2 (graceful watchdog restarts
under load) are **verified against the authoritative log**. The eval's
attribution flag is closed.

## Appendix C — Aggressive-cell n>1 repeat (protocol §5 precondition close) (2026-06-16)

§3.2 ran each cell at n=1, so §4 item 3 / §5 could not estimate the stall rate or
close the protocol §5 escalation precondition ("the most aggressive cell must fail
*across repeats*"). This addendum runs the single most aggressive cell —
`--oversubscribe 4 --priority high --pin --seconds 45` (128 threads, 4× oversubscribed,
HIGH priority, pinned) — **5 times** against the same live `BelowNormal`
FEAT-0008 controller, operator-present.

Orchestrator: `release/runtime/experiments/loop-stall/Run-AggressiveRepeat.ps1`
(untracked); 85 °C software abort, 25 s pre-cell cooldown gate (start each cell
≤ 70 °C), 55 s post-cell watch for a late watchdog restart, `events.jsonl`
byte-offset delta as the authoritative verdict. Raw:
`aggressive-repeat-results-20260616.json`.

| Cell | preTemp °C | maxTemp °C | verdict | force-term | restart | `sidecar_upsert_failed` |
|---|---|---|---|---|---|---|
| 1 | 59.1 | 75.8 | SURVIVE | 0 | 0 | 0 |
| 2 | 44.0 | 75.9 | SURVIVE | 0 | 0 | 0 |
| 3 | 44.0 | 75.9 | SURVIVE | 0 | 0 | 0 |
| 4 | 54.3 | 76.1 | SURVIVE | 0 | 0 | 0 |
| 5 | 66.0 | 75.8 | SURVIVE | 0 | 0 | 0 |

**Result: 5/5 SURVIVE; 0 force-terminations; 0 graceful restarts; 0 sidecar errors;
peak 76.1 °C.** Worker pid 44676 was unchanged across the entire ~11 min sweep.
Independently cross-checked against `events.jsonl` for the window
12:20:30-12:31:30: the only events were **1,940 `control_loop.write_applied`**
(tick 83646→85454, ~2.7/s avg including cooldown periods) — no `supervisor.*`
lifecycle event, no `force_terminate`, no `sidecar_upsert_failed`. The controller
actuated fans continuously throughout.

Findings:

1. **Protocol §5 precondition met; the close rests on mechanism, corroborated by
   n=6 (not proven by it).** The aggressive cell ran n=5 (this) + n=1 (§3.2 cell C2)
   = **n=6 total** with zero force-terminations and zero natural hard freezes
   (`stop_result==2`). The *reason* the scheduling axis cannot produce the FEAT-0008
   trigger is mechanistic (`docs/cpu-0609-freeze-classification-2026-06-16.md` §2):
   the stop-sentinel poll is timer-bound (`steady_clock`, throttle-immune) and
   Windows balance-set-manager boost caps a *runnable* `BelowNormal` thread's
   starvation at a few seconds, well inside the 15 s deadline. The n=6 is
   **consistent with** that, not a rate bound — 0/6 bounds the per-cell hard-freeze
   rate only loosely (rule of three, ~50 % upper 95 % CI). So this is "the mechanism
   predicts no hard freeze and six live cells agree," not "six runs prove it cannot
   occur."
2. **Stronger than §3.2 — with a measurement caveat.** Here the worker did not even
   go stale enough for a *graceful* watchdog restart (0/5; directly evidenced by zero
   staleness restarts) — at peak ~76 °C the `BelowNormal` worker stayed scheduled and
   status-fresh, consistent with the balance-set boost above. The §3.2 graceful
   restarts are the stochastic tail of the staleness-recycle, not the hard-freeze
   class. **Caveat:** this orchestrator recorded lifecycle + temp, not per-cell
   cadence, so `SURVIVE` means "no restart / force-terminate / sidecar-error," not a
   measured degradation depth; cell C2 restarting under *identical* args in §3.2 is
   the evidence the load *can* bite — this n=5 was the quieter tail.
3. **AVX-512 escalation superseded, not pending.** The mechanism/asymmetry analysis
   in `docs/cpu-0609-freeze-classification-2026-06-16.md` §2 establishes that a
   thermal power-virus cannot produce the worker-specific 15 s stop-miss and that
   the 06-09 whole-system class is outside FEAT-0008's recovery envelope; so §5's
   AVX-512 escalation is the wrong instrument, and this n=5 scheduling repeat is the
   correct close. The natural-hard-freeze premise is reframed **on evidence**: not
   reproducible by load on this system; FEAT-0008 is validated defense-in-depth for
   the deterministic-deschedule / kernel-block class (the `NtSuspendProcess` proxy
   M-evidence, §3.1).

Thermal: peak 76.1 °C (~18.9 °C below the 95 °C SMU throttle); no abort across 5
cells. AVX2 on this liquid loop with a degraded `BelowNormal` controller stays well
inside the envelope for bounded 45 s cells at n=5.
