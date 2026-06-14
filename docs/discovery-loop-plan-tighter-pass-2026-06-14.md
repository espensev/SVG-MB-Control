# Loop Plan Feed — Targets For A Tighter Pass

**Theme:** discovery pass — find code seams worth a tighter follow-up investigation
**Date:** 2026-06-14
**Status:** historical discovery artifact (NOT current/authoritative). Ideas here are
candidates, not authorized work. Promote via `docs/features/` before any product-code change
(per `AGENTS.md` Feature Intake Gate).
**Storage root:** `docs/`
**Paths searched:** `docs/` (no project `memory/`; 9 pre-existing `discovery-*.md` treated as prior coverage)
**Method:** 10-seam parallel fan-out (Workflow `wf_45e89d06-a60`, 48 agents) → per-finding
adversarial verification against the code → dedup/rank synthesis → completeness critic.
36 findings raised, **26 survived verification**, 10 dropped as misreads/already-mitigated.
**Recommended next:** `/discover` per target below. `/planner` is NOT yet appropriate for any
target — none is authorized by an Accepted + build-authorized FEAT.

---

## Method Caveats (read before trusting any single item)

- **Static read+grep only.** Every claim is by inspection, not execution. The missing-index
  claim (AP-2) was never confirmed via `EXPLAIN QUERY PLAN`; no degrade branch was exercised
  through the existing sim seams (`SIM_RESTORE_MODE=fail`, `RAPL_ENERGY_MODE`,
  `CPU_CYCLES_MODE`); test-blind-spot items are grep-of-`tests/`, not measured branch coverage.
- **One finder died** (`find:write-actuation`, socket error). That seam is FEAT-0005 territory
  (already investigated this session — see "Known parked target" below), so the gap is covered
  by hand, not lost.
- **The completeness critic found 11 unscanned areas** (see "Wave 2"). Some are higher-stakes
  than the ranked survivors; treat Wave 2 as co-equal input, not an afterthought.

---

## Goal Frame

| Goal ID | Priority | Goal | Success Signal |
|---|---|---|---|
| G1 | P1 | Correctness / reliability — silent failure, races, lost errors, unsafe degrade, thermal-safety | A named, code-evidenced gap with a bounded remediation shape |
| G2 | P2 | Cost / performance — repeated scans, N+1, per-tick waste | Measurable redundant work on a hot or batch path |
| G3 | P3 | Maintainability — duplication, dead code, doc-vs-code drift, test blind spots | Drift/duplication that is a future-regression vector |

---

## Top recommendation (do these first)

1. **HR-2 — wedged-but-alive worker has no force-recovery** (G1, high, **verified**). The one
   state the watchdog claims to own (`stale`/health=2) has no force-kill fallback; the existing
   recovery-gap audit *misclassifies* it as "Recovered". Highest-ranked verified finding; the
   load-bearing evidence is a reproducible grep (`TerminateProcess|taskkill|Stop-Process` = none
   in `src`) plus the audit's own misclassification — well above the noise floor.
2. **EH-2/3/4/5 — silent write/append failures** (G1, **verified**, evidence-integrity).
   Discarded `bool` returns make CSV-row, snapshot-publish, and event-log write failures silent;
   an event-log append failure hides every other error report. One coherent design fix.
3. **A second fan-out over the 11 unscanned Wave-2 seams** (below) — the core actuation math
   (`control_policy.cpp`), config validation, and the `duty_pct→duty_raw` conversion were *never
   read*. These are **completeness-critic leads, NOT verified to survivor depth** — treat them as
   things to confirm, not confirmed gaps. (W1 has since been partly run down — see its corrected
   row; it is defense-in-depth, not the thermal-safety headline it first looked like.)

---

## Pass 2 results (verified Wave-2 fan-out — Workflow `wf_32fc8e34-cee`)

12 seams, 40 findings, **25 survived verification**, 15 dropped, 54 agents, ~38 min. This pass
ran the Wave-2 leads *through* the adversarial verify layer (pass-1 Wave-2 was unverified critic
output). Two corrections to pass-1 calls:
- **W3 (duty_pct→duty_raw scaling) is CORRECT** — verify confirmed `lround(clamp(duty,0,100)*255/100)`
  (`fan_sio.cpp:830`); only a **test gap** survives (W3-3). My pass-1 "off-by-scale?" worry was unfounded.
- **W1 (LookupCurve floor on bad temp) IS reachable** — not via NaN (unreachable, as corrected) but
  via the **0.0 sentinel** (AM-1): a GPU thermal-read miss leaves `core_c/memjn_c=0.0` with
  `gpu_available=true`, so `LookupCurve(0.0)` returns curve-minimum with no `sensor_failed` signal.
  Latent today (deployed RTX 5090 reads nonzero) but the mechanism is proven end-to-end.

**Combined top targets across both passes (the comparison you asked for):**

| Rank | Target | Pass | Goal | vs others |
|---|---|---|---|---|
| 1 | **HR-2** wedged-worker has no force-recovery | 1 | G1/high | Live mechanism; the binding hazard. Overall #1. |
| 2 | **W2-2** duplicate channel number silently accepted → two state machines fight one fan + sidecar baseline corruption | 2 | G1/high | **WITH HR-2.** More latent (config-typo trigger) but broader blast radius (actuation + crash-recovery corruption). #1 *new* find. Effort: **low**. |
| 3 | **EH-2/3/4/5** silent write/append failures | 1 | G1/med | Evidence-integrity. |
| 4 | **GPU-INIT-1** NVML init ctor-only, no retry/re-init → permanent silent GPU demotion | 2 | G1/med | WITH EH; same boot-race class as the 2026-06-11 SIO incident, but NVML never got the retry hardening. |
| 5 | **Merged curve-shape gap** (W2-1+AM-2) no duty-monotonicity check → cooling-inverted curve loads silently | 2 | G1/med | Config-validator completeness; hand-edit trigger. |
| 6 | **AM-1** GPU 0.0-as-cold → curve-minimum, no failure signal | 2 | G1/med | Reachable analog of W1; **scope with GPU-INIT-1** (shared `available=false` predicate). |
| 7 | **W6-2** reconcile restore-fail refuses boot AND leaves orphaned fan at killed duty | 2 | G1/med | Fail-closed → no thermal control; supervisor gives up after one startup failure. |
| 8 | **W10-2** sentinel self-heal only at next build → watchdog Disabled (triggers don't fire) until then | 2 | G1/med | Adjacent to HR-2: watchdog *absent* vs watchdog *can't-act*. |
| 9 | **W9-1+W9-2** both tasks `Interactive` logon → no pre-logon recovery; b03aa76 boot-trigger rationale is false | 2 | G1+G3 | Doc-drift (W9-2) masks the gap. |

Pass-2 Later tier (fold into the clusters above): W3-3 (conversion test), W5-1 (low-band no unit
test), W7-1 (`retain_days==0` disables pruning), W7-3 (CSV-open failure silent), W2-3/W2-4 (silent
channel/point drops), W8-A/W8-B (no baseline-restore on OS shutdown), W6-3 (calibrate/write-once no
singleton → concurrent sidecar clobber), WAC-1/WAC-3 (FEAT-0005 confirmed — **doc maintenance only**,
and per-tick RPM/duty readback IS available, which resolves FEAT-0005's blocking open question).

**Pass-2 sequencing (clusters):**
- **Cluster A — config validation** (one `/loop run` over `control_loop_config.cpp`): W2-2 leads,
  then merged curve-shape, W2-3, W2-4. Curve-shape needs a `/discover` first (reject-on-load vs
  documented contract is a maintainer call).
- **Cluster B — GPU reader** (scope together): GPU-INIT-1 (re-init) + AM-1 (positive-temp gate)
  share one `available=false` predicate.
- **Cluster C — watchdog/boot-resilience**: W9-1/W9-2 (principal + false rationale) + W10-1 (trivial
  sentinel-write-order reorder) + W10-2 (boot-independent re-enable).
- **Cluster D — FEAT-0005**: WAC-1/WAC-3 → doc maintenance only; bundle W3-3 (write-path test).

**Still uncovered after 2 passes (pass-2 critic):** `runtime_health.cpp` AssessHealthState
backward-clock/**DST fall-back** masking (a once-a-year guaranteed HR-2∩HR-4 trigger); supervisor
restart-policy asymmetry (give-up-once on startup-crash vs retry-forever after) — pairs with W6-2;
cross-layer CSV column-contract drift (use the **schema-validator** skill); the 01452dc
trailing-header-skip parser change is untested (same commit as F3); PCI-mutex `WAIT_ABANDONED`
(= pass-1 CONC-5, low); `IsProcessActive` exit-259 (checked — unreachable today); analyze ingest
dedup/transaction correctness (offline G3).

---

## Prioritized Queue

### Now

| Target | Goal | Evidence | Why now | Next skill |
|---|---|---|---|---|
| **HR-2** wedged-but-alive worker (stale/health=2) cannot be force-recovered | G1 / high | `task_runner.cpp:201-203` (health==2 → cooperative `--restart` only); `control_supervisor.cpp:399-423` (RequestStopAndWait times out at 15s, returns 2, launches no replacement); `app_main.cpp:190-197` (early-return, `start_requested` never set); `control_loop.cpp:174-182`+`tick_runner.cpp:418-419` (stop seen only between ticks — a tick wedged in `RunControlTick` never sees it); `control_supervisor.cpp:584-586` (supervisor blocks in `WaitForSingleObject` on the wedged handle); grep `TerminateProcess\|taskkill\|Stop-Process` across `src` = none | Highest-ranked verified finding; `discovery-recovery-gap-audit-2026-06-04.md:241` misclassifies this exact state as "Recovered (the only covered case)". Fix is bounded + Repo-Boundary-safe (force-terminate our own `last_worker_pid`/`supervisor_pid`). | `/discover health/recovery` — should watchdog+supervisor escalate to a bounded force-terminate of the recorded PID after a cooperative stop times out? |
| **EH-2/3/4/5** discarded write/append returns → silent CSV-row, snapshot-publish, event-log failures | G1 / medium | EH-2: `tick_runner.cpp:368-377`, `read_loop.cpp:286-302` discard `WriteRow()`; `runtime_csv_archive.cpp:463-483` returns false without flipping `is_open()`. EH-3: `tick_runner.cpp:206-218` discards `WriteRuntimeSnapshotFile` AND advances `last_snapshot_write_time` regardless (suppresses ~1s retry). EH-4: `runtime_event_log.cpp:210-336` returns false; ~70 discard sites (not `[[nodiscard]]`), and those events are the *only* report of write/restore/breaker errors. EH-5: `write_orchestrator.cpp:200-209` cerr-only vs sibling structured events. `evidence_log.cpp:272-326` already shows the intended capture-and-emit pattern. | Four G1 findings on one mechanism with an internal dependency (EH-4 hides EH-2/EH-3). One coherent design decision, not four patches. | `/discover error-handling` — design a rate-limited write-failure surface + a last-resort sink / sticky "event log unwritable" status flag; make `last_snapshot_write_time` advance only on success. |

### Next

| Target | Goal | Evidence | Why next | Next skill |
|---|---|---|---|---|
| **HW-01 + CONC-3 + F1** SampleCpuCycles affinity-pin path | G1 / medium | `amd_reader.cpp:686-696` pins core 0 then reads MPERF/APERF with **no `GetCurrentProcessorNumber` confirm-spin** (the validating probe `cpu_cycle_counter_probe.cpp:80-87` *does* spin-confirm); `cpu_cycles.h:79-87` guard only blanks ratio∉(0,8] so an in-band cross-core delta passes; `:695` restore return ignored; pin is manual-mirror not RAII (unlike `PciMutexLock` `:196-223`); sim reader short-circuits before the call (`amd_reader.cpp:833-842`) so degrade branches are untested; **design drift**: decision doc §5 (`cpu-work-energy-acquisition-decision-2026-06-07.md`) says use a dedicated short-lived sampler thread, never repin the control thread — shipped code repins the control thread that owns `AmdReader`. | Shipped enabled read is strictly weaker than the probe that "validated" it. Bounded (default-off, log-only, quarantined) → evidence-correctness not thermal-safety, hence Next. | `/discover hardware-read` — add confirm-spin (or move to the documented sampler thread); check/log restore; decide single-group assertion vs `SetThreadGroupAffinity`; add a clock+MSR seam for degrade-branch coverage; wrap pin in an `AffinityScope` RAII. |
| **AP-2** report scans `tick_channel_samples` 4× per run, 2 sorts not matching the PK | G2 / medium | `analyze_report.cpp:80,103,105,133` (four consumers); `analyze_report_queries.cpp:412-413` & `565-567` `ORDER BY channel, tick_count` (not PK-satisfiable); `:788-790`/`:847-849` two more; PK is `(run_id, tick_count, channel)` (`analyze_channel_sample_columns.cpp:152`) with no secondary index in `analyze_db.cpp`. | Highest-ranked G2; concrete fix (`CREATE INDEX (run_id, channel, tick_count)` + possible single shared load). Off the control hot path (batch tool). | `/discover analyze-perf` — `EXPLAIN QUERY PLAN` the `:412/:565` selects on a v10 DB to confirm a temp-B-tree sort; evaluate the covering index; verify one shared channel-sample load can serve all four projections. |
| **F3** SIO init-retry (5×250ms) + transient-read retry (3×75ms) — the boot-resilience fix — has no test | G1 / medium | `sio_fan_writer.cpp:104-128` init loop + inline incident note `:97-102`; `:41-55` `RetryTransientSioOperation` + `:35-39` `IsTransientSioStatus`; grep `tests/` for these symbols = 0; incident commits `01452dc`/`8f82b1b`. | Incident-proven, live-deployed logic rests entirely on manual validation. Splits into a near-test-ready predicate (lift from anon namespace) + a constructor seam (the real cost). | `/discover testability` — can `MbSioController` be injected and the retry helpers lifted out of the anonymous namespace, so a fake returning N transient failures then ok can drive the loop? |
| **HR-1 + HR-4** weak watchdog liveness signals (PID-reuse + wall-clock staleness) | G1 / medium(HR-4)/low(HR-1) | HR-1: `runtime_util.cpp:25-38` `IsProcessActive` trusts any STILL_ACTIVE PID (no identity check); `runtime_health.cpp:99-100,125-129` depend on it; authoritative `ProbeRuntimeSingletonHeld` (`runtime_singleton.cpp:189-211`) used by launcher only; worker holds a `kWorker` mutex (`app_main.cpp:248-250`). HR-4: `runtime_health.cpp:144-149` age clamps negative→0 so a future-dated `last_update` never trips `stale_after_ms`; control-loop status emits no `stale` boolean so the age path is the sole signal; `test_runtime_health.py` tests only a *past* timestamp. | Both inputs to the restart decision can be defeated; HR-4's backward-clock-step suppresses restart end-to-end at boot — exactly when the watchdog is most needed. Folds into the HR-2 pass (same state machine). | `/discover health/recovery` — wire `ProbeRuntimeSingletonHeld(kWorker)` as a cross-check; add a monotonic/steady freshness basis (or treat future-dated `last_update` as suspect); add a future-timestamp regression test. |

### Later (fold into the passes above; do not sweep separately)

| Target | Goal | One-line | Why held |
|---|---|---|---|
| AP-3 | G2 | `IngestEvents --force` does per-run COUNT+UPDATE (N+1), collapsible to one set-based UPDATE (`analyze_ingest.cpp:257-290`) | Rare maintenance path, index-assisted, scales with run count not events |
| DRIFT-2 | G3 | `CONTROL_LOOP.md` lists `curve_shape`/`rise_rate`/`fall_rate` as required `control_loop` fields; loader treats them as per-channel optionals and shipped config omits them (`control_loop_config.cpp:423-431,520,524`) | Zero runtime impact; doc-trust + `must`=enforced convention violation |
| DRIFT-1 | G3 | `CONTROL_PIPELINE_MATH.md §6.1`/inv#4 describe a `B<B_max` anti-windup guard the integrator lacks (terminal clamp instead, `boost_stage.cpp:89,102-109`) | Output proven byte-identical; already in `PATH_NOTES.md` as "Idea (verify)" |
| HR-5 | G1 | Supervisor `worker_restart_count` never resets after sustained health; backoff pins at 32s (`control_supervisor.cpp:548,642,647`) | Per-process scope (reboot zeroes it); modest recovery slowdown, observability-only |
| F2 | G1 | Hold-expiry `kRestoreFailed` (non-timeout) branch untested (`channel_write.cpp:242-256`); `SIM_RESTORE_MODE=fail` exists | Fan retains active curve control (not stranded); needs a maintainer policy ruling |
| HP-2 | G2 | `snapshot_age_ms` re-parses an ISO string the sampler just formatted (`runtime_csv_rows.cpp:424-427`) | Already "partly done" in `discovery-control-optimization-options.md`; trivial vs hardware-read budget |
| F5 | G1 | `pending_writes_unreadable→kFailed` early-return precedence untested (`runtime_health.cpp:42-47,105-109`) | Correct today; regression-guard only (test must use malformed/dir, not absent file) |
| HR-3 | G1 | Read-path-only PawnIO/AMD outage leaves status `running` (`control_loop.cpp:86,129-133`) | sharpened-known; belongs to **FEAT-0004** (Draft); CpuOnly channels *do* degrade, only GPU-bearing ones silently track GPU |
| CONC-1 | G1 | Operator `--stop` during startup window can be clobbered by clear-calls (`control_supervisor.cpp:463,710`; `control_loop.cpp:63`) | Silent-failure framing **refuted** — surfaces as 15s timeout + exit 2, operator-retryable |
| G3-01 | G3 | Channel CSV column contract hand-replicated across 4 files, no cross-list assertion (`runtime_csv_rows.cpp:309-396` vs `analyze_channel_sample_columns.cpp:13-48`); renaming `channel{N}_observed_temp_c` silently drops all channel rows (`analyze_csv.cpp:344-348`) | Lists agree today; worst case is analysis data loss, never actuation |
| G3-02 | G3 | Three SHA-256-over-BCrypt impls with three hex encoders (`file_hash.cpp`, `pawnio_binary.cpp`, `runtime_singleton.cpp`) | Identical output today; extract a narrow `BytesToLowerHex`, preserve 64KB streaming |
| CONC-5 | G3 | `WAIT_ABANDONED` adopted as clean acquisition with no breadcrumb (`amd_reader.cpp:202-204`) | **Weakest survivor** — garbage-read mechanism refuted; recommend drop |
| HW-02 | G1 | `ExecutePawnIo` bounds `input_count` not `output_count` (`amd_reader.cpp:231-253`) | Latent only — internal linkage, all callers `output_count=1u`, fully guarded; nothing to discover |
| HW-03 | G1 | 9950X3D (Zen 5, model 0x44) reads per-CCD Tdie via a Zen 4 register base (`amd_decode.h:60-65`) | Telemetry-only (control input isolated by exact label); already a pending live-validation item |

---

## Wave 2 — areas the sweep NEVER scanned (completeness critic)

These are not yet verified to survivor depth (they are **completeness-critic outputs — static, not
run through the adversarial verify layer**), but several carry concrete `file:line` evidence and
rank above some Later items. **Recommend a second fan-out targeting these.** W1 below has been
partly run down by hand (post-review) and corrected; W2–W11 remain unverified leads.

| # | Uncovered area | Stakes | Concrete hook |
|---|---|---|---|
| W1 | **→ NOW VERIFIED via pass 2 as AM-1 (reachable through 0.0, not NaN).** See Pass 2 results above and the AM-1 row in the Next queue. **`control_policy.cpp` `LookupCurve` NaN handling — corrected.** A NaN `temp_c` returns the **floor, not safe-mode 100%** (`control_policy.cpp:72-100`: NaN fails every comparison → `raw=0.0` → clamp→floor; empty-curve also returns floor). **But** the original "silently drives min duty" headline is largely refuted: the **CPU path is NaN-gated** (`tick_runner.cpp:183` sets `cpu_available` only on `!isnan` → `sensor_failed` → safe-mode 100%), so CPU NaN cannot reach the curve. The **GPU path is asymmetric** — `tick_runner.cpp:189-193` sets `gpu_available=true` with no `isnan` check, and `GpuControlEnvelopeC` (`channel_evaluator.cpp:402-407`) = `max(core_c,memjn_c)` is NaN only if **both** are NaN — yet those fields **default to 0.0** at every layer (`gpu_reader.cpp:425-426,450-453`; `runtime_snapshot.cpp:117-119` deserialize with `value(...,0.0)`), so a NaN envelope is **not a demonstrated reachable state**. | **G1 defense-in-depth** (downgraded from "highest") | Add a NaN guard in `LookupCurve` + a symmetric `isnan` guard on the GPU envelope (`tick_runner.cpp:189-193`) to match the CPU path; add a NaN-temp `LookupCurve` unit test. |
| W2 | **Curve/config validation gaps.** `ValidateControlLoopConfig` never checks for **duplicate channel numbers**; `ValidateCurve` never enforces **duty monotonicity** — a curve whose duty *decreases* as temp rises loads and runs (reduces cooling as the part heats). | G1/G3 | `control_loop_config.cpp:498-500` (no dup check), `:71-91` (no monotonicity), `:225-228` (sorts temp asc only) |
| W3 | **`duty_pct → duty_raw` scaling never inspected.** Control math is `duty_pct` (0–100 double); hardware takes `duty_raw` (uint8). Rounding/scale/saturation of the conversion (0–100→0–255 vs 0–100, and whether 100% maps to true hardware max) is unverified — a classic off-by-scale on every commanded duty. | G1 correctness | `sio_fan_writer.cpp:283` `ApplyChannelDuty(..., duty_raw, ...)`; conversion likely in `channel_write.cpp` or `third_party/SVG-MB-SIO` |
| W4 | **GPU NVML init has no boot-retry.** `GpuReader::available()` is sticky — NVML init runs once in the constructor and is never retried; a boot-race NVML failure (same class as the SIO boot-resilience incident) permanently demotes every GPU-bearing channel to CPU-only for the process lifetime. Unlike `SioFanWriter` (5×250ms init retry), `GpuReader` has none. | G1 reliability | `gpu_reader.cpp:394-404` (one-shot init), `:412-415` (sticky false), `:446-449` (no reconnect) |
| W5 | **`low_band_integrator.cpp` UpdateLowBandState — unread.** Per-tick stateful integrator equal in mutation surface to `boost_stage` (which *was* covered): one-tick-lag primary-response assumption, `elapsed_ms==0`→`poll_tick_ms` dt fallback under a stalled clock, undocumented 0.75 de-activation hysteresis, unbounded accumulators. | G1/G2 | `low_band_integrator.cpp:85-87,107-122,152` |
| W6 | **Calibration orphan recovery untested.** If the process is hard-killed between `ApplyDuty` and `RestoreSavedState` mid-calibration, the `pending_writes` sidecar is the only recovery — whether the reconcile pass actually re-applies `baseline_duty_raw` is unsurfaced. | G1 actuation | `calibration.cpp:398-411,447-453` |
| W7 | **CSV archive has no total-byte cap.** Pruning is age-only and runs only at chunk rotation; `rotate_hours_==0` disables rotation → pruning never runs → single chunk grows unbounded → feeds the EH-2 silent-row-loss path. Latent (shipped config sets rotate=4h/retain=7d). | G1 latent | `runtime_csv_archive.cpp:266,402-448,451-453` |
| W8 | **Shutdown signal wiring.** `app_signals.cpp` sets `g_stop_signaled` on CTRL_C/CLOSE/SHUTDOWN but grep shows no consumers in the loops — unclear how it reaches the (file-based) stop check, and whether fans get a baseline restore inside the OS-granted `CTRL_SHUTDOWN_EVENT` window before force-termination. | G1 graceful-shutdown | `app_signals.cpp:16-25` |
| W9 | **Scheduled-task trigger asymmetry.** Worker got a boot trigger (commit `b03aa76`) while the watchdog is logon-triggered (`Install-...WatchdogScheduledTask.ps1:90-105`) — possible boot-to-logon window where a wedged worker is unwatched. | G1 install/recovery | compare worker vs watchdog trigger sets |
| W10 | **`build-release.ps1` publish integrity** not re-examined for residual interruption windows after the `8f82b1b` hardening (is the exe swap rename-based, is the sentinel restore in a finally/trap). | G1 deployment | the `0x80070002` incident class |
| W11 | **`control_math.cpp` `MoveTowardRateLimited` + `cadence_score.cpp`** dt-derived rate math under zero/negative elapsed time — feeds both the boost and low-band integrators. | G2/G3 | `control_math.cpp:20` |

---

## Known parked target (the finder that crashed)

- **FEAT-0005 — write-actuation confirmation ("quiet-and-hot" stall detection).** RPM tach is
  read (`fan_writer.h:30-43`, `FanChannelState.tach_valid/rpm`) but used only for low-band
  evidence/calibration — **never as a write-confirm**. A write is treated successful on no driver
  error (`channel_write.cpp:337-344`); the breaker counts only driver-level failures (`:44-46`).
  A fan stalled at high commanded duty has no detector. Spec is **Reserved/parked** since
  2026-06-06 (`docs/features/_parked/FEAT-0005-write-actuation-confirmation.md`). This is the
  highest-value *reliability* item, but it is a maintainer governance decision (un-park), not a
  discovery gap. Distinct fault class from HR-2 (stalled fan vs wedged worker process).

---

## Sequencing Notes

- **Health/recovery is one subsystem touched by 5 findings** (HR-2, HR-1, HR-4, HR-5, CONC-1).
  Do HR-2 first (binding hazard; its discovery re-touches `AssessHealthState`/`task_runner`/
  `control_supervisor`), then fold HR-1+HR-4 (detection-signal hardening) and HR-5/CONC-1 into
  the *same* pass — avoid three separate sweeps of `control_supervisor.cpp`.
- **EH-4 is the meta-dependency** — decide the last-resort sink / sticky flag *within* the EH pass,
  not after, since EH-4's failure hides EH-2/EH-3.
- **The SampleCpuCycles cluster** (HW-01+CONC-3+F1) is all within `amd_reader.cpp ~686-735` —
  one pass covering code-fix + test-seam + RAII together.
- **DRIFT-1/DRIFT-2/G3-01** are doc/test-fixture coherence — batch into one pass.
- **`next_skill` is `/discover` for every routable target** — none is authorized by an
  Accepted+build-authorized FEAT (FEAT-0004 Draft, FEAT-0005 parked, FEAT-0006 cycles
  Accepted-but-not-build-authorized).

---

## Open Questions (maintainer must decide)

- **HR-2:** Is a bounded `TerminateProcess` on our own recorded `last_worker_pid`/`supervisor_pid`
  (after cooperative stop times out) acceptable under the Repo Boundary? (Boundary forbids loading
  PawnIO, not killing our own processes — confirm this reading.)
- **W1/W2:** Is a NaN-temp→floor (vs safe-mode 100%) the intended degrade? Is a non-monotonic
  curve ever legitimate (warn vs reject)?
- **EH-4:** Which last-resort sink for event-log append failure — stderr (only captured at
  startup-exit today) or a sticky status flag?
- **F2:** Is "continue silently" the intended policy for a non-timeout restore failure at hold expiry?
- **HR-3 / FEAT-0004:** Advance FEAT-0004 Draft→Accepted so HR-3's evidence has a home?
- **FEAT-0005:** Un-park for the quiet-and-hot stall detector?
- **FEAT-0006 cycles:** Is the confirm-spin/sampler-thread fix build-authorized, or does it stay
  in energy-only-now scope? (Accepted ≠ build-authorized.)

---

## Missing Modalities (what a stronger next sweep should add)

Dynamic verification (run `EXPLAIN QUERY PLAN`; exercise degrade branches via the sim env hooks);
measured branch coverage (not grep-of-`tests/`); git-churn/blame risk ranking; a mechanical
`[[nodiscard]]`/`-Wunused-result` discarded-return audit; thread-sanitizer on the
supervisor/worker/watchdog interaction; fault injection (disk-full, read-only `runtime_home`,
mid-write power loss) on the actuation/persistence boundary.
