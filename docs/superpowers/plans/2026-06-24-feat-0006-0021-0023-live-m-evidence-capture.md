# Live-M Evidence Capture Plan — FEAT-0006 / FEAT-0021 / FEAT-0023

> **For the operator + assisting agent:** These are **operator-run live-hardware
> capture runbooks**, not code tasks. Each restarts the live worker on real
> cooling hardware, so each requires explicit live-runtime authorization
> (`AGENTS.md` §Live Runtime Safety) and a cool idle window with the operator
> present. Steps use checkbox (`- [ ]`) syntax for tracking. The analysis /
> scoring / doc-writing steps can be run by the agent; the deploy / enable /
> restart / load steps are operator actions.

**Goal:** Close the three open live-measurement (`M`) items —
FEAT-0006 §12 off-thread-sweeper loop-timing gate (+ the `quarantine → validated`
marker decision), FEAT-0021 REQ-GPUCTX-04 live cadence check, and FEAT-0023
REQ-MPROFILE-10 deployed-default-profile check — using the existing capture
harness and scoring tooling, then record the evidence in each spec's §14 and
`docs/TRACEABILITY.md`.

**Approach:** Reuse what already exists. `scripts/Capture-EnergySession.ps1` is
the ready-made FEAT-0006 harness (snapshots a baseline CSV, enables the read-only
energy+cycle path, restarts the worker tree so the new env propagates, drives
idle→synthetic-load→cooldown, snapshots the enabled CSV, and **always reverts to
disabled** in a `finally` block). `scripts/score_loop_timing_gate.py` scores the
§12 gate. FEAT-0021 and FEAT-0023 are confirmation checks against the live
control-loop CSV — no new product code. The only fork needing a maintainer
decision is **how FEAT-0023 deploys the composed default profile** (Runbook 3).

**Tooling / inputs (the "tech stack"):**
- Harness: `scripts/Capture-EnergySession.ps1` (self-elevating; `-Rehearse` for a no-touch dry run).
- Scorers: `scripts/score_loop_timing_gate.py` (§12 sweeper gate), `scripts/score_energy_session.py` (per-session criteria incl. cycle criterion-4).
- Analyzer: `release\svg-mb-control.exe analyze ingest|report`.
- Load: `cpu-synth-load.exe` (auto-built by the harness; CMake target `cpu_synth_load`, `-DSVG_MB_CONTROL_BUILD_SYNTH_LOAD=ON`). No in-repo GPU load tool — GPU load is operator-supplied.
- CSV helpers for ad-hoc summaries: `scripts/control_csv.py` (`parse_control_csv`, `column`, `to_float`).

## Global Constraints

Every step below is implicitly bound by these. Values are copied verbatim from
the repo.

- **Live Runtime Safety** (`AGENTS.md`): no fan/MSR/CPU **writes**; the energy +
  cycle path is read-only telemetry. Restarts cause a brief control gap (fans
  hold/return to BIOS SmartFan auto). Run at idle, operator present, authorized.
- **Repo root:** `D:\Development\Thermals\SVG-MB\SVG-MB-Control`
- **Live control-loop CSV:** `release\runtime\logs\svg_mb_control_output.csv`
- **Scheduled tasks** (path `\SVG-MB Control\`): worker `SVG-MB Control`
  (`svg-mb-control-task-runner.exe --start --config "…\release\control.json"`),
  `SVG-MB Control Watchdog` (`--watchdog-run --config …`), `SVG-MB Energy Safety
  Revert` (currently **Disabled**), `SVG-MB Energy Session 2`/`3` (Disabled).
- **Current live state (D-PWRLOG-1 steady state):**
  `SVG_MB_CONTROL_RAPL_ENERGY_MODE=enabled`,
  `SVG_MB_CONTROL_CPU_CYCLES_MODE=disabled`, Safety Revert **disabled**. CPU
  package power + GPU board power + GPU context already log live; the
  **all-core off-thread sweeper has never run live** (cycles disabled).
- **Live build provenance:** `release/build-info.json` → `builtUtc
  2026-06-23T06:24:56Z`, `workingTreeDirty: false`, `testsPassed: true`. This is
  **after** the FEAT-0006 all-core-sweeper merge (PRs #25/#26, 2026-06-21) and
  FEAT-0003 Done (2026-06-22), so **no new build is required for FEAT-0006 or
  FEAT-0021**. (Confirm the binary contains the sweeper at capture time — see
  Runbook 2 §B.) Any FEAT-0023 Option B build must start from a clean tree
  (build-provenance dirty-stamp lesson).
- **Shipped profile (the envelope):** `250 ms` control tick, `250 ms` write
  cooldown, 6 live channels. Do not move these.
- **Acquisition marker is governance-only:** the worker only ever writes
  `disabled` / `unavailable` / `quarantine`; it **never** auto-writes
  `validated` (`amd_reader.cpp`). `quarantine → validated` is a recorded
  maintainer decision, never a code or CSV edit.
- **Evidence doc naming:** `docs/<topic>-<YYYY-MM-DD>.md` (today = `2026-06-24`).
- **P0 base frequency for `--p0-mhz`:** `4300` (Ryzen 9 9950X3D base; used in the
  s1 cycle evidence).

---

## Shared procedure: the worker-restart / env-propagation mechanic

All three runbooks (and the harness) restart the worker **tree** so a new
persistent env or task argument takes effect (env is frozen at process
creation). The canonical sequence (from `Set-EnergyLoggingProfile.ps1` /
`Capture-EnergySession.ps1`, elevated):

1. `Stop-ScheduledTask` the **watchdog first** (so it cannot respawn the worker mid-swap), then the worker task.
2. Kill any lingering `svg-mb-control.exe` whose command line points at `release\` (15 s deadline loop).
3. `Start-ScheduledTask` the worker, then the watchdog.

`Capture-EnergySession.ps1` does this internally; for FEAT-0023 the operator
applies it after editing task arguments. **Expected-warning note for our current
state:** the harness's post-run revert waits for the marker to return to
`disabled`, but because energy is intentionally left `enabled` live, the marker
stays `quarantine` and the harness prints "marker did not return to 'disabled' —
CHECK". That warning is **expected and benign here** — the real post-run invariant
is energy `enabled`/`quarantine` + cycles `disabled` (the harness restores the
pre-run env values it captured, which are exactly that).

---

## Runbook 1 — FEAT-0021 (REQ-GPUCTX-04): live cadence / process-resource check

> **STATUS: DONE 2026-06-25 — `pass (with finding)`.** Ran read-only against the
> live + archived control-loop CSVs (no restart needed), adversarially verified by
> a 5-skeptic workflow. Evidence: `docs/feat-0021-live-cadence-evidence-2026-06-25.md`;
> FEAT-0021 §14 / `docs/TRACEABILITY.md` / `docs/features/README.md` updated;
> `test_feature_specs` 5/5 green. Non-blocking follow-up (off-thread the context
> read; longer clean LIVE window) tracked in `docs/next_steps.md`.

**What it must show (spec §6/§10/§14):** the added periodic GPU workload-context
read (cached, ≤ once per 1000 ms) keeps loop timing and process-resource metrics
**inside the shipped 250 ms envelope**. FEAT-0021 is already live (in the 06-23
build), so this is a confirmation against a representative window, not an
investigation. `T`/`R` already pass; only `M` is open.

**Artifacts:**
- Create: `docs/feat-0021-live-cadence-evidence-2026-06-24.md`
- Read: `release\runtime\logs\svg_mb_control_output.csv` (live) and/or a window
  copied from `release\runtime\logs\archive\`.
- Update at close: FEAT-0021 §14 REQ-GPUCTX-04 row (`partial → pass`),
  `docs/TRACEABILITY.md` REQ-GPUCTX-04, `docs/features/README.md` "Recently
  implemented" FEAT-0021 note (drop "live M pending").

**Acceptance — primary (absolute within-envelope), measured over a
representative recent live window (≥ ~30 min of normal use, ideally including a
GPU-busy stretch):**

- [ ] **Step 1: Confirm FEAT-0021 context is actually being captured.** In the live CSV, `gpu_context_acquisition == bounded_context_sample` on data rows, and `gpu_context_sample_age_ms` ramps toward ~1000 ms then resets (proves the 1000 ms cache cadence whose cost we are bounding). If it reads `disabled`/`unavailable`, the context read is not active — stop and investigate before claiming an envelope result.

- [ ] **Step 2: Summarize the timing/resource columns over the window.** Throwaway snippet (agent may run), using the in-repo CSV helper:

```python
# scratch: FEAT-0021 envelope summary — run from repo root
import sys; sys.path.insert(0, "scripts")
from control_csv import parse_control_csv, column, to_float
import statistics as st
csv = r"release\runtime\logs\svg_mb_control_output.csv"
_meta, h, rows = parse_control_csv(csv)
def col(name): return [to_float(v) for v in column(h, rows, name) if to_float(v) is not None]
ach   = col("loop_achieved_interval_ms")
slip  = col("loop_slip_ms")
work  = col("loop_work_duration_ms")
cpu   = col("process_cpu_pct")
overr = [str(v).strip().lower() for v in column(h, rows, "loop_overrun")]
def pct(xs, q): xs=sorted(xs); k=int(round((len(xs)-1)*q/100)); return xs[k] if xs else None
print("n rows           :", len(rows))
print("achieved ms p50/p99:", pct(ach,50), pct(ach,99))
print("slip ms p50/p99/max:", pct(slip,50), pct(slip,99), max(slip) if slip else None)
print("work ms p50/p99    :", pct(work,50), pct(work,99))
print("overrun true frac  :", round(sum(v=='true' for v in overr)/max(1,len(overr)),5))
print("process_cpu_pct p50/p99:", pct(cpu,50), pct(cpu,99))
```

- [ ] **Step 3: Read health over the window.** `release\svg-mb-control.exe --health --json` → `health_state` is `healthy` (not `degraded`/`stale`/`failed`); also confirm no health regression in the window via `control_health.json`.

- [ ] **Step 4: Apply the pass thresholds (within-envelope):**
  - `loop_intended_interval_ms == 250` (sanity), `loop_achieved_interval_ms` p50 ≈ 250 and p99 with no sustained drift above 250.
  - `loop_slip_ms` p99 bounded to low tens of ms **excluding** environmental spikes (> 100 ms ticks are pre-existing/environmental per the FEAT-0020 gate-6 finding; a handful is acceptable).
  - `loop_overrun == true` fraction ≪ 1% excluding environmental spikes.
  - `process_cpu_pct` low (single digits) and not elevated versus pre-FEAT-0021 norms.
  - `health_state == healthy` throughout.
  - **Verdict:** PASS if all hold; if not, FEAT-0021 stays `partial` and the cause is recorded.

- [ ] **Step 5 (optional strengthening — do NOT block on it):** before/after `loop_work_duration_ms` using the §12 tooling, baseline = a pre-2026-06-20 archive CSV (energy+GPU-power on, no FEAT-0021 context read), candidate = the current window:

```
python scripts\score_loop_timing_gate.py --baseline <pre-0021-archive>.csv --candidate <current-window>.csv
```
  Read the idle/load `p99_bulk` deltas as context (no material increase = the context read is cost-neutral). Skip if no clean comparable baseline exists — the absolute check in Step 4 is what REQ-GPUCTX-04 requires.

- [ ] **Step 6: Write `docs/feat-0021-live-cadence-evidence-2026-06-24.md`** with the window bounds, the Step 2/3 numbers, the verdict, and (if run) the Step 5 deltas. Then update FEAT-0021 §14 / TRACEABILITY / README.

> **Note:** GPU load is operator-supplied (no in-repo GPU tool). A window that
> includes a GPU-busy stretch makes the evidence stronger (context fields vary,
> GPU power non-idle), but the timing envelope is the pass criterion regardless.

---

## Runbook 2 — FEAT-0006 (§12): off-thread-sweeper loop-timing gate + marker decision

**What it must show (spec §12/§14):** enabling the all-core **off-thread
sweeper** does **not** move the shipped 250 ms control-loop profile —
`loop_work_duration_ms` p99-of-bulk unchanged within tolerance, bucketed by GPU
load, overruns/max as context only. This is the FEAT-0020 gate-6 analog for the
new sweeper thread and is the precondition before any enabled-live all-core
evidence leaves `quarantine`. **The sweeper has never run live**, so these are
net-new captures (the 06-10/12/14 energy sessions predate it).

**Scope notes:**
- Enabling cycles (`SVG_MB_CONTROL_CPU_CYCLES_MODE=enabled`) turns on **both** the per-core APERF/MPERF read and the all-core off-thread sweeper; there is no separate sweeper-only flag. The gate therefore bounds their **combined** per-tick cost vs cycles-OFF — conservative, and the sweeper is the new element §12 targets.
- Energy stays `enabled` in **all** captures (it is the live steady state), so the only variable across baseline/candidate is the cycle path. Use each run's `session.csv` (the controlled idle→load→cooldown window), **not** `baseline_disabled.csv` (that pre-enable snapshot is for `score_energy_session.py` criterion-6).
- The §12 tolerance is **provisional** until calibrated off-vs-off; the script header records ~0.03 ms p99-bulk idle drift on 2026-06-21 as a sanity reference.

**Artifacts:**
- Create: `docs/feat-0006-loop-timing-gate-evidence-2026-06-24.md`
- Produces (per run): `release\runtime\experiments\energy-quarantine\<stamp>\{baseline_disabled.csv, session.csv, reference_sensors.csv, manifest.json}`
- Update at close: FEAT-0006 §14 REQ-CPUEFF-01 row + the §12 narrative, `docs/TRACEABILITY.md` REQ-CPUEFF-01, `docs/features/README.md` FEAT-0006 line + `docs/next_steps.md` "FEAT-0006 downstream work".

- [ ] **Step 0: Pre-flight (operator).** Authorized, cool idle window. Start HWiNFO64 (Scribe) if you want the SMU reference for `score_energy_session.py` criterion-3 (optional for the §12 gate). Confirm `SVG_MB_CONTROL_CPU_CYCLES_MODE` is currently `disabled` so the OFF captures are true sweeper-OFF:
```
[Environment]::GetEnvironmentVariable('SVG_MB_CONTROL_CPU_CYCLES_MODE','User')   # expect: disabled (or empty)
```

- [ ] **Step 1: Dry-run the harness** (no live touch) to confirm wiring:
```
.\scripts\Capture-EnergySession.ps1 -Rehearse -SessionLabel sweeper-gate-rehearse
```

- [ ] **Step 2: Capture OFF-baseline run A (sweeper OFF).** `-EnergyOnly` enables energy but not cycles, so the sweeper stays off:
```
.\scripts\Capture-EnergySession.ps1 -EnergyOnly -LoadThreads 28 -SessionLabel sweeper-off-A
```
  (Defaults: idle 300 s → load 720 s → cooldown 300 s. `-LoadThreads 28` matches the s2/s3 sessions — near-all-core load while leaving the control thread schedulable.) Record the `<stamp>` OutDir and its `session.csv` path.

- [ ] **Step 3: Capture OFF-baseline run B (sweeper OFF)** — identical command, for off-vs-off calibration:
```
.\scripts\Capture-EnergySession.ps1 -EnergyOnly -LoadThreads 28 -SessionLabel sweeper-off-B
```

- [ ] **Step 4: Capture the ON candidate (sweeper ON).** Default mode enables energy **and** cycles (per-core + all-core sweeper):
```
.\scripts\Capture-EnergySession.ps1 -LoadThreads 28 -SessionLabel sweeper-on
```

- [ ] **Step 5: Confirm the sweeper actually ran in the ON capture (pre-gate gate).** In the ON `session.csv`, `cpu_cycles_allcore_cores` > 0 on most rows, `cpu_aperf_delta_allcore`/`cpu_mperf_delta_allcore`/`cpu_cycles_window_ms_allcore` populated, and `cpu_cycles_acquisition == quarantine`. If the all-core columns are blank / `cores == 0` everywhere, the live binary did not run the sweeper (wrong build or cycles not enabled) — **stop and resolve before scoring** (a "PASS" on a candidate where the sweeper never ran is meaningless).

- [ ] **Step 6: Calibrate the tolerance off-vs-off.** Run the gate baseline-vs-baseline and read the natural per-bucket `p99_bulk` drift:
```
python scripts\score_loop_timing_gate.py --baseline <off-A>\session.csv --candidate <off-B>\session.csv
```
  Set `--rel-tol` / `--abs-tol-ms` to cover the measured idle+load drift with margin (sanity-check against the ~0.03 ms idle reference; if the off-vs-off drift is far larger, investigate the captures before trusting any verdict).

- [ ] **Step 7: Score the real gate (OFF vs ON) with the calibrated tolerance:**
```
python scripts\score_loop_timing_gate.py --baseline <off-A>\session.csv --candidate <sweeper-on>\session.csv --rel-tol <cal> --abs-tol-ms <cal>
```
  **PASS criteria:** verdict `PASS (provisional)` (exit 0); the `idle` and `load` buckets read `ok` (candidate `p99_bulk` ≤ allowed). Overruns `base->cand` not materially increased and `max` are **context only** — a single environmental stall must not be read as a failure (the script enforces this). If a bucket reads `MOVED`, the sweeper perturbs that bucket → record the numbers, do **not** promote, and feed back to the sweeper design.

- [ ] **Step 8: Cycle criterion-4 (effective-frequency validity).** Score the ON session and derive effective frequency:
```
python scripts\score_energy_session.py --manifest <sweeper-on>\manifest.json --session-num <N>
release\svg-mb-control.exe analyze ingest --runtime-home .\release\runtime --db .\release\runtime\svg_mb_control.db
release\svg-mb-control.exe analyze report --runtime-home .\release\runtime --db .\release\runtime\svg_mb_control.db --p0-mhz 4300 --json --out feat0006-allcore-report.json
```
  Confirm the all-core `package` effective-MHz block is plausible (ratio and effective MHz in a sane band for the workload; contributing-core max/min present). Criterion-4 has been the standing `MANUAL` item.

- [ ] **Step 9: Marker decision (governance — maintainer, not automated).** If Step 7 PASSES and Step 8 is satisfactory, **recommend** recording the cycle path `quarantine → validated` as a dated governance decision. This is a **doc** action only (FEAT-0006 §14 + `docs/TRACEABILITY.md` REQ-CPUEFF-01 + a decision note); the logged marker stays `quarantine` in data — `validated` is the evaluation outcome, never a code/CSV flip. Do **not** auto-promote.

- [ ] **Step 10: Confirm the box returned to steady state.** After the harness's `finally` revert: energy `enabled` (marker `quarantine`), cycles `disabled`, watchdog running, `health_state == healthy`. (The "did not return to 'disabled'" warning is expected — see Shared procedure.)

- [ ] **Step 11: Write `docs/feat-0006-loop-timing-gate-evidence-2026-06-24.md`** (calibration drift, the OFF-vs-ON per-bucket table + verdict, the sweeper-confirm counts, criterion-4 effective-MHz, and the marker recommendation). Update FEAT-0006 §14 / TRACEABILITY / README / next_steps.

> **Out of scope (do not fold in):** REQ-CPUEFF-08, the CPU-setting label, is a
> separate **unimplemented code** item, not gated by this capture. Note it as
> still deferred; do not attempt it here.

---

## Runbook 3 — FEAT-0023 (REQ-MPROFILE-10): deployed default profile reproduces baseline

**What it must show (spec §6/§10/§12/§14):** a profile-**resolved** default
(`snd-desk-composed`, proven byte-identical to `control.release.json` by
`profile_composition_tests`) reproduces the shipped 250 ms cadence, 250 ms write
cooldown, 6-channel set, and control-computation identity **on hardware**.
`T`/`R` pass; only the on-hardware `M` is open.

### ⚠ DECISION REQUIRED before executing this runbook — deploy mechanism

The live worker/watchdog tasks bake `--config "…\release\control.json"`, and
startup precedence is `--config` > `--profile`/`SVG_MB_PROFILE` (spec §5,
"`--config` always wins"). **Setting `SVG_MB_PROFILE` is therefore ruled out** —
the supervised worker would keep running the shipped config and `active_profile_name`
would read `control`, silently false-passing while measuring the wrong thing.
Choose one (this is a maintainer call; it changes the steps below and has
different permanence/risk):

- **Option A — temporary task reconfig (recommended for the M).** Edit the
  `SVG-MB Control` and `SVG-MB Control Watchdog` task arguments from
  `--config "…\control.json"` to `--profile snd-desk-composed` for the
  measurement window, then revert. Reversible, no build, isolates the resolution
  path. First verify `--profile` flows through the task-runner → supervisor →
  worker (Step 2).
- **Option B — wire the catalog into Build-Release/installer.** Deploy
  `config/profiles/` + `config/overlays/` into `release\` and launch via
  `--profile` (or machine-identity). Permanent; is itself a config/deploy change
  that must go through the Feature Intake Gate (`AGENTS.md`) on a clean tree.
  This is the productization the spec says this M "gates" — usually done **after**
  Option A confirms the M.

`SVG_MB_PROFILE` env: **rejected** (config wins → false pass). Manual foreground
`--profile` instance: **rejected** (a second process writing fans alongside the
live worker is a Live-Runtime-Safety conflict).

**Artifacts:**
- Create: `docs/feat-0023-live-default-profile-evidence-2026-06-24.md`
- Update at close: FEAT-0023 §14 REQ-MPROFILE-10 row (`partial → pass`),
  `docs/TRACEABILITY.md` REQ-MPROFILE-10, `docs/features/README.md` FEAT-0023
  note (drop "on-hardware live M deferred").

- [ ] **Step 1: Resolution pre-check (no restart).** Confirm the composed profile resolves to the shipped config:
```
release\svg-mb-control.exe --show-config --profile snd-desk-composed
```
  Expect: resolved name `snd-desk-composed`, a resolution source, and a config path; the reported `ControlLoopConfig` matches `control.release.json` (cadence 250, cooldown 250, 6 channels, curves/boosts). (`profile_composition_tests` already proves byte-identity; this confirms it on the live box.)

- [ ] **Step 2 (Option A): Verify `--profile` flows end-to-end, then deploy.** Edit both task arguments (worker `--start --profile snd-desk-composed`, watchdog `--watchdog-run --profile snd-desk-composed`), apply the Shared restart sequence, then **hard gate:** read `active_profile_name` from the live control-loop CSV / `control_runtime.json`.
  - **PASS gate:** `active_profile_name == snd-desk-composed`.
  - **FAIL/STOP:** if it reads `control` or `control.release`, `--profile` did not flow through (task-runner did not forward it) — revert immediately and resolve the wiring before any measurement. Do not proceed.

- [ ] **Step 3: Capture a live window under the composed default** (representative conditions; ≥ ~30 min; a CPU/GPU-busy stretch strengthens identity evidence). Confirm against the shipped baseline:
  - `loop_intended_interval_ms == 250`, `loop_achieved_interval_ms` p50 ≈ 250 (cadence unchanged).
  - Write cooldown unchanged (no extra writes / same setpoint-update cadence as a `control.release.json` reference window).
  - Same 6 channels active with the same `channelN_response_source` values as the release reference.
  - Control identity: setpoints for matched inputs equal the release reference (byte-identical config guarantees this — confirm on hardware, not just in test).
  - `active_profile_name == snd-desk-composed`, `active_profile_source` populated, `health_state == healthy` throughout.
  - **Verdict:** PASS if all hold.

- [ ] **Step 4 (optional, explicitly Live-Runtime-Safety-gated): exercise the live switch once.** At idle, operator present, validate the headline switch end-to-end:
```
release\svg-mb-control.exe --set-profile snd-desk-composed
```
  Observe: `profile.switch.request.json` taken by the supervisor → `supervisor.profile_switch_signaled` → graceful worker cycle (fans → captured BIOS auto during the ~1–2 s gap; **note RPM rises under load — the switch is not acoustically seamless, by design**) → `supervisor.profile_applied`, `active_profile_name` flips. This documents REQ-MPROFILE-06/08 behavior on hardware (REQ-10 itself only needs Steps 1–3).

- [ ] **Step 5: Roll back.** Restore the task arguments to `--config "…\control.json"` (Option A) or restore the shipped installer (Option B); apply the Shared restart; confirm `active_profile_name == control` and `health_state == healthy`.

- [ ] **Step 6: Write `docs/feat-0023-live-default-profile-evidence-2026-06-24.md`** (deploy mechanism used, the Step 1 resolution output, the Step 2 gate result, the Step 3 cadence/channel/identity comparison, any Step 4 switch observations, rollback confirmation). Update FEAT-0023 §14 / TRACEABILITY / README.

---

## Sequencing & time budget

Advisor-endorsed order (cheapest/safest first; don't block the first two on the
FEAT-0023 deploy decision):

1. **FEAT-0021 (Runbook 1)** — analysis of an existing/representative live window; ~minutes once a good window exists. No restart needed for the primary check.
2. **FEAT-0006 (Runbook 2)** — longest: 3 captures × (~22 min profile + restart/verify overhead) ≈ 75–90 min of attended capture, plus scoring. Needs all-core load.
3. **FEAT-0023 (Runbook 3)** — gated on the deploy-mechanism decision; two worker restarts (deploy + rollback).

## Division of labour

- **Operator (authorized, live):** all deploy / enable / `Capture-EnergySession` / task-reconfig / load / restart / switch steps; the `quarantine → validated` governance decision.
- **Agent (assist):** the dry-run review, the Step-2/Step-6 analysis snippets, running the scorers and `analyze report`, drafting the three evidence docs, and the §14 / TRACEABILITY / README / next_steps edits once numbers exist.

## Self-review (against the specs)

- **Spec coverage:** Runbook 1 → REQ-GPUCTX-04 (§10 `T,R,M`). Runbook 2 → FEAT-0006 §12 sweeper gate + REQ-CPUEFF-01 cycle promotion + criterion-4 + the marker decision. Runbook 3 → REQ-MPROFILE-10 (§10 `T,R,M`); Step 4 also touches REQ-MPROFILE-06/08. ✔
- **No false zeros / false passes:** the sweeper-ran pre-gate (R2 §5) and the `active_profile_name` hard gate (R3 §2) are the two checks that stop a meaningless "pass." ✔
- **Marker scope:** governance-only, no auto-promote (R2 §9). REQ-CPUEFF-08 explicitly out of scope. ✔
- **Live-runtime safety:** every restart flagged; energy/cycle path read-only; harness `finally` reverts; FEAT-0023 switch (R3 §4) explicitly idle/operator-gated. ✔
- **No product code** unless FEAT-0023 Option B is chosen (then: Feature Intake Gate, clean tree). ✔
