# FEAT-0021 live cadence evidence — 2026-06-25

**Project:** svg-mb-control
**Status:** Results record (live `M` evidence for FEAT-0021 / REQ-GPUCTX-04)
**Companion:** `docs/features/FEAT-0021-standard-control-loop-gpu-workload-context-logging.md`,
`docs/features/FEAT-0020-standard-control-loop-power-logging.md`,
`docs/feat-0020-live-flip-validation-results-2026-06-18.md`

This records the live-runtime measurement (`M`) evidence for REQ-GPUCTX-04: does
the once-per-1000 ms GPU workload-context refresh keep loop-timing and
process-resource metrics inside the measurement envelope? Only derived numbers
are recorded here; raw runtime CSV captures are not committed.

## 1. Provenance

- **Live build:** clean-tree release built 2026-06-23; `active_profile_name=control`,
  `gpu_context_acquisition=nvml` on every analyzed row (no `<blank>`, no
  `unavailable`), so the FEAT-0021 context slice is live and NVML-sourced.
- **Windows analyzed:**
  - **PRIMARY** — `archive/svg_mb_control_control-loop_20260624_180903.csv`, 57363
    rows (~4 h), frozen archive. This window carries the verdict.
  - **LIVE** — `svg_mb_control_output.csv`, 20848 rows at analysis time, still being
    appended (it grew during the run); its tail counts are a lower-bound snapshot
    and are used as corroboration, not as the headline.
  - **BASELINE** — `archive/svg_mb_control_control-loop_20260619_200546.csv`, 52828
    rows, pre-FEAT-0021 (`has_feat0021_context=false`; no `gpu_context_*` columns).
- Tick period is 250 ms (`loop_intended_interval_ms=250` on 100 % of rows in all
  three windows).

## 2. Method

`loop_work_duration_ms` is the cost-sensitive column (the fixed-start sleep masks
sub-ms cost in `loop_slip_ms`). To isolate the context read from steady per-tick
work, rows are split by `gpu_context_sample_age_ms` band:

- **refresh band (`age < 250` ms):** includes the tick where the context cache is
  refreshed (`age == 0`, exactly 20 % of ticks at the 1000 ms refresh / 250 ms tick
  ratio). The wide GPU read fires on the `age == 0` tick.
- **cached bands (`250–500`, `500–750`, `750–1000` ms):** ticks that reuse the
  cached context sample and perform no wide read.

The refresh-minus-cached median delta isolates the FEAT-0021 read cost. The cached
median is compared against the pre-FEAT-0021 BASELINE median to confirm per-tick
work is otherwise unchanged. `loop_work_duration_ms` is bucketed by `gpu_power_mw`
to test load dependence. The governing cadence metrics are
`loop_achieved_interval_ms`, `loop_slip_ms`, `loop_overrun`, and `process_cpu_pct`,
because `loop_work_duration_ms` and achieved cadence diverge (PRIMARY has a
work=2296 ms tick with slip 1.6 ms — a stall in measured work that did not breach
the achieved interval).

## 3. Results

### 3.1 Cadence stays inside the envelope (REQ-GPUCTX-04 named metrics)

The four metrics §10 names — achieved interval, slip, overrun, process CPU% —
hold against the pre-FEAT-0021 baseline.

| Metric | BASELINE 06-19 | PRIMARY 06-24 | LIVE |
|---|---|---|---|
| `loop_achieved_interval_ms` p99 | 251.92 | 251.97 | 252.00 |
| `loop_achieved_interval_ms` max | 253.05 | 2872.46 | 3486.78 |
| `loop_slip_ms` p99 | 1.92 | 1.97 | 2.00 |
| `loop_slip_ms` p99 (bulk) | 1.92 | 1.965 | 1.99 |
| `loop_overrun` true fraction | 0.0 | 7e-05 | 8.63e-04 |
| `process_cpu_pct` p99 | 0.146 | 0.156 | 0.156 |

Achieved-interval p99 sits at 252 ms in both post-FEAT-0021 windows — within 0.1 ms
of the baseline 251.92 ms. Slip p99 holds at ~2 ms. The worst achieved interval
(2872 ms PRIMARY / 3487 ms LIVE) is well under the ~10 s `control_runtime.json`
staleness threshold that triggers a watchdog recycle, so no recycle occurred. The
overrun fraction is nonzero where the baseline was zero, but rare and single-tick
(see §3.6). `process_cpu_pct` p99 is unchanged at 0.156 %.

### 3.2 The context-read cost is attributable and bounded

The refresh-minus-cached median delta is 41.1 ms in both independent windows:

| Band | PRIMARY p50 | PRIMARY p99 (bulk) | LIVE p50 | LIVE p99 (bulk) |
|---|---|---|---|---|
| refresh (`age < 250`) | 42.71 | 61.40 | 42.94 | 62.93 |
| cached `250–500` | 1.615 | 20.55 | 1.679 | 21.04 |
| cached `500–750` | 1.615 | 20.41 | 1.689 | 21.63 |
| cached `750–1000` | 1.605 | 20.47 | 1.689 | 22.19 |

- refresh p50 − cached p50 = 42.71 − 1.615 = **41.10 ms** (PRIMARY);
  42.94 − 1.679 = **41.26 ms** (LIVE).
- The cached-band median (1.6 ms) matches the pre-FEAT-0021 BASELINE
  `loop_work_duration_ms` p50 of **1.582 ms** to three significant figures, so
  per-tick work outside the refresh tick is unchanged.

The whole ~41 ms increment appears only on the once-per-~1000 ms refresh tick. Its
own cost caps at refresh p99-bulk 61.40 ms (PRIMARY) / 62.93 ms (LIVE) — ~25 % of
the 250 ms budget at the bulk p99. Because cached ticks cost 1.6 ms and refresh
ticks cost +41 ms, the read runs in-line on the control thread.

The read is load-independent. `loop_work_duration_ms` by `gpu_power_mw` bucket on
PRIMARY: idle (<150 W, n=57208) p99 54.73 ms; mid (n=155) p99 55.44 ms; no rows
landed in the load (≥350 W) bucket in PRIMARY or LIVE. A separate same-window
GPU-load capture (602 W peak, 99 % util) showed refresh-tick work p99 53.88 ms /
max 56.12 ms under load versus 64.81 ms / 284.50 ms at idle — load is lower at
every percentile, so GPU-idle is the conservative case for the context read, not an
understated one (§3.7).

### 3.3 Process CPU is unchanged

`process_cpu_pct` p99 is 0.156 % (PRIMARY and LIVE) versus 0.146 % (BASELINE);
max 0.196 % / 0.233 % versus 0.487 %. There are zero `process_cpu_pct` rows above
100 ms-equivalent and the metric does not regress.

### 3.4 Working set is a startup-transient plateau, not a leak

`process_working_set_bytes` p50 is 5.23 MB (PRIMARY) / 5.17 MB (LIVE) versus
3.52 MB (BASELINE); max 37.34 MB / 37.37 MB versus 5.75 MB. Read in row order, the
~37 MB peak is a process-startup transient confined to the first ~2 % of rows;
after warm-up the working set settles to ~4–6 MB and the last decile is lower than
the first (non-monotonic). The leak-relevant committed metric,
`process_private_bytes`, is flat at ~24 MB across all deciles in PRIMARY, LIVE, and
BASELINE (~0.5 MB drift), so there is no monotonic growth. The ~6 MB higher steady
plateau is a co-change effect (the 06-24 build carries features beyond FEAT-0021)
and is not attributable to FEAT-0021's cached context read alone; none of it is
growth.

### 3.5 Health stayed healthy across the window

Three independent sources agree the controller was healthy throughout the
FEAT-0021-active window:

- Live `--health --json`: `health_state=healthy`, `exit_code=0`,
  `degraded_channel_count=0`, `event_log_failure_active=false`,
  `sidecar_quarantined_present=false`, `worker_restart_count=0`.
- `control_health.json`: `last_health_state=healthy`, `last_health_exit_code=0`.
- Runtime event JSONL over 2026-06-24 18:00..22:10: 50 in-window events, all
  `severity=info` (supervisor.start → `worker_started` restart_count=0 →
  control_loop.start; zero worker exits, csv_write_failures, safe-mode, stale,
  quarantine, or degraded events). The only error/warning events in the 685-event
  corpus are two `worker_exited(exit_code=1)`+restart pairs dated 2026-06-23,
  outside the window.

### 3.6 Multi-second stalls: environmental magnitude, refresh-tick concentration

PRIMARY has 4 `loop_work_duration_ms > 250` ticks (max 2296.5 ms); LIVE has 18–21
(max 3484.1 ms); BASELINE had 0 (max 148.4 ms). Disposition:

- **The largest stalls land on cached ticks**, where no wide read ran. In LIVE the
  3484 ms, 3100 ms, and 2666 ms stalls occur at `age` 472/923/676 ms (cached bands,
  see §3.2 cached max columns: 3484.1 / 3100.2 / 2666.4 ms). The cached `250–500`
  band on PRIMARY hits 315.4 ms — a no-read tick over 250 ms. These are the
  pre-existing Layer-0 environmental descheduling class FEAT-0020 gate-6 documented
  (multi-second `loop_work` spikes present with no NVML call). The read cannot cause
  them.
- **Stall rate scales with the environment, not the read.** Absolute rate rises
  from 0.07/1000 (PRIMARY) toward ~0.9/1000 (LIVE), and the cached-tick rate scales
  the same way — environment sets how often stalls happen.
- **A residual is attributable to the in-line read.** Stalls concentrate on the
  `age == 0` refresh tick beyond chance: PRIMARY 3 of 4 (binomial P(X≥3|n=4,p=0.20)
  = 0.027, underpowered at n=4); LIVE 15 of 21 (expected 4.2; P(X≥15|n=21,p=0.20)
  < 0.0001). The refresh-vs-cached overrun rate is ~3–4× in both windows. The steady
  41 ms read lowers headroom to the 250 ms cliff by ~41 ms, so an environmental
  jitter event tips a refresh tick over more often. This does not make the read
  unbounded (its own cost caps at ~61 ms p99-bulk); it is the in-line read
  interacting with environmental jitter. The off-thread context read (the FEAT-0006
  all-core sweeper precedent) is the known mitigation if this residual is to be
  removed.

### 3.7 Representativeness and limitations

- A genuine busy-GPU window exists (602 W peak, 99 % util) and the context read is
  cheaper under load (§3.2), so the idle-dominated PRIMARY window does not mask a
  higher busy-GPU read cost — GPU-idle is the conservative case.
- Neither window captures a combined high-load CPU+GPU stress; CPU contention could
  deschedule the loop independently of the read. That divergent-load session is the
  missing test.
- LIVE was still being appended during analysis; its tail counts (refresh overrun
  fraction 13× PRIMARY, p99.9 364 ms vs 68 ms) are a lower-bound snapshot of a
  window that included clustered environmental bursts (overruns at adjacent indices
  with multi-second slip). PRIMARY (frozen, overrun 7e-05, refresh p99-bulk
  61.4 ms) carries the verdict; LIVE corroborates the read-cost and concentration
  direction.

## 4. Verdict

**REQ-GPUCTX-04: PASS-with-finding.** On the four metrics §10 names — achieved
interval, slip, overrun, process CPU% — the once-per-1000 ms GPU context refresh
stays inside the envelope: achieved-interval p99 252 ms (within 0.1 ms of the
251.92 ms baseline), slip p99 ~2 ms, overrun fraction 7e-05 (PRIMARY), process CPU%
p99 unchanged at 0.156 %. The context read cost is attributable and bounded (~41 ms
p50 / ~61 ms p99-bulk on the refresh tick once per ~1 s, versus 1.6 ms on cached
ticks and 1.582 ms on the pre-FEAT-0021 baseline — within the 250 ms budget).
Working set is a startup-transient plateau (committed `process_private_bytes` flat
~24 MB), and health stayed `healthy` with zero restarts across the window.

**Finding (does not breach the envelope; monitored follow-up):** the refresh tick's
overrun rate is ~3–4× the cached rate in both windows and multi-second stalls
concentrate on the `age == 0` refresh tick beyond chance (LIVE P < 0.0001). The
largest stalls land on cached (no-read) ticks and reproduce the pre-existing
FEAT-0020 environmental class, but the in-line 41 ms read lowers headroom to the
250 ms cliff, so it is the read interacting with environmental jitter. This does not
reopen the verdict: the named-metric envelope holds, so REQ-GPUCTX-04 closes as
`pass`. The finding is logged as a non-blocking follow-up in `docs/next_steps.md` —
(a) move the context read off-thread (the FEAT-0006 all-core-sweeper precedent) to
remove the refresh-tick residual, and (b) capture a longer clean LIVE window to
bound the multi-second tail. Neither is required for this M; both would tighten it.

## 5. Reproduction

Derived with a throwaway analyzer that splits `loop_work_duration_ms` by
`gpu_context_sample_age_ms` band and by `gpu_power_mw` bucket, plus the
cadence/resource percentiles in this record, over the three CSVs in §1. The verdict
was cross-checked by five independent adversarial passes: refresh-tick cost
attribution, multi-second-stall attribution (binomial test of `age == 0`
concentration), working-set leak test (`process_private_bytes` trend), health/event
continuity (`--health`, `control_health.json`, event JSONL), and window
representativeness (idle-vs-load refresh-tick cost). Re-derive at any future gate;
LIVE counts move as the file is appended.
