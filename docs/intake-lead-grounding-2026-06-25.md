# Intake-lead / steady-under-load — empirical grounding (2026-06-25)

## REGRESSION FOUND (2026-06-25): loop-timing, not a slew-config change

Operator report: fan tightness "got worse in less than a day," **before** the
FEAT-0024 commit. Per-session forensic timeline (binned by binary/config sha):

| session_start | git | config_sha256 | loop_slip ms | loop_work p99 ms | overrun % |
|---|---|---|---|---|---|
| 2026-06-21T15:34 | c17c42d | 45a0a1c7 | 0.99 | 56 | 0.00 |
| 2026-06-22T12:27 | 913dda3 | 45a0a1c7 | 1.18 | 50 | 0.02 |
| 2026-06-25T04:34 | **ba83aed** | 45a0a1c7 | **7.59** | **191** | **0.71** |
| 2026-06-25T08:25 | ba83aed | 45a0a1c7 | 4.19 | 70 | 0.16 |

- **`config_sha256 = 45a0a1c7` is unchanged across every session** — this is NOT a
  slew/curve config edit. The control config is byte-identical.
- **The periodic ~40 ms load is BASELINE, not new.** The good 06-22 session
  (`913dda3`) also has **20.0 % of ticks >40 ms**, the same 326 CSV columns, and
  energy/power/context columns populated — same as 06-25's 20.7 %. So the added
  telemetry/logging is NOT the regression; it was already present and fit in budget.
- **The regression is the TAIL + SLIP, and it tracks the captures.** Same spike
  *count*, but bigger spikes and far more slip, worst during the 04–05am
  energy/sweeper captures, then partially recovering:

  | 06-25 session | loop_slip ms | loop_work p99 ms | note |
  |---|---|---|---|
  | 04:34 | 7.59 | 191 | sweeper + energy capture |
  | 05:00 | 12.88 | 178 | sweeper + energy capture |
  | 07:20 | 2.19 | 68 | captures done |
  | 08:25 | 4.19 | 70 | current-ish |

  vs the 06-22 good state slip 1.18 / p99 50. This fingerprint (same load, worse
  tail, worst under the off-thread sweeper) = **resource contention**, not more work
  — almost certainly the **off-thread sweeper's own PawnIO handle contending for the
  `Global\\Access_PCI` mutex** (100 ms timeout) with the control loop's AMD reads,
  inflating per-tick work and slip.
- **Mechanism (the slew link):** `RateLimitSetpoint` budgets each step by
  `rate × elapsed_since_last_write / 60000`. At a uniform 250 ms tick (good state)
  every step is ~0.375 %. When a tick slips, that write's budget grows
  proportionally → a larger, **irregular** step. Identical config, looser feel — from
  loop jitter, not curve values.
- **Current residual:** the sweeper is off now, so the worst is past, but 08:25 still
  shows slip ~4 ms / p99 ~70 ms vs the 06-22 good 1.2 ms / 50 ms — modestly elevated.
  Likely the `ba83aed` binary hot path being a touch heavier than `913dda3`, and/or
  lingering box load.

## Two fix directions (look back before inventing)

1. **Restore the 06-22 good loop timing.** Sweeper/cycles are already off (D-PWRLOG-1
   steady state). Re-measure a clean window now; if slip is back near ~1 ms the
   looseness was capture-transient and self-heals. If still elevated, compare the
   `ba83aed` vs `913dda3` hot-path cost (the binary, not the config).
2. **Harden the rate limiter against loop jitter (the durable fix, "a form of slew
   limiting that was good").** Cap the `elapsed_since_last_write` used in the rate
   budget so a slipped tick cannot produce an oversized/overshooting step. Small,
   testable `RateLimitSetpoint` change. Decouples fan smoothness from loop timing.

### Replay v3 — elapsed-cap PROTOTYPE (validated, 2026-06-25)

Fed the **real per-tick `loop_achieved_interval_ms`** into the rate-budget elapsed,
on the highest-slip session (`…_050033.csv`: interval mean 262.9 ms, p99 404 ms,
**max 3580 ms**). Model validation (real-elapsed replay vs logged setpoint):
**mean|err| 0.10 % (ch2) / 0.12 % (ch4)**. Fix = cap elapsed at 300 ms.

| metric (loaded band) | shipped | capped @300 ms |
|---|---|---|
| ch2 direction reversals /1000 | 72 | ~0 |
| ch4 direction reversals /1000 | 58 | ~0 |
| ch4 per-write step std / max | 0.032 / 0.37 | 0.018 / 0.30 |
| ch2 setpoint jitter/tick | 0.0108 | 0.0083 |
| up-response to a real rise (63 %) | 27.2 s | 27.2 s (unchanged) |

**Result:** capping elapsed eliminates the slip-induced overshoot/"hunting"
(reversals → ~0) and uniformizes the steps, with **zero cost to the up-response**.
`max_setpoint_step_pct` already bounds absolute step size, so the cap's benefit is
specifically removing the elapsed-driven *overshoot-then-reverse* pattern under slip.
(Reversal→0 is on this high-slip window; verify across more windows + a clean live
capture before adoption.) This is the durable fix; it needs a FEAT spec +
`CONTROL_PIPELINE_MATH.md` §8 update + a contract/replay test + a live gate, since it
changes the control-computation identity.

---


Working analysis feeding `docs/intake-lead-response-decision-2026-06-25.md` and
`docs/features/FEAT-0024-intake-lead-under-load.md`. Read-only; uses the archived
control-loop CSVs. **Preliminary** — the candidate-magnitude numbers below are NOT
yet adoption-grade (see the model-fidelity caveat).

## Settings under comparison

- **Shipped (live):** ch2 `rise_alpha=0.018 fall_alpha=0.006 rise_rate=90 fall_rate=45 max_step=0.7 decay_latch=60/120 midband_max=6`;
  ch4 `0.008/0.003/60/25/0.6/34/90 midband_max=10`. Cadence 250 ms / cooldown 250 ms / deadband 0.25.
- **FEAT-0024 (merged PR #32, NOT deployed):** intake `rise_rate` ch2/3 90→125, ch4 60→120; `max_step`→0.95;
  `gpu_airflow_start_c`→58, `max_boost` 12/12/10; ch4 `rise_alpha` 0.008→**0.014**; ch4 `cpu_override` 82:42→50, 86:46→53.
  Per the 2026-06-25 control-philosophy feedback the ch4 `rise_alpha` raise is to be **reversed** (it opposes "long EMA").

## Findings — model-free (straight from logged columns; trustworthy)

Window: `…_20260625_082546.csv`, sustained 74–80 °C, 5444 ticks (~23 min), Tctl mean **76.5 °C std 0.97**.

- **Midband is saturated** at sustained high load (ch2 mean 5.7/6.0, ch4 9.4/10.0) → it is **not** the wander source.
- The **raw curve demand (`channelN_feedforward_pct`) tracks the ±1 °C Tctl wander** and is the largest per-tick mover (|Δ| 0.19–0.25 %/tick).
- The **final setpoint tick-to-tick motion is already small: ~0.02–0.03 %/tick** → per-tick steadiness is governed by the **rate limiter**, not the EMA.

## Findings — faithful replay (VALIDATED, trustworthy)

Replay = exact `ApplyDemandSmoothing` (rise/fall EMA + decay latch) + midband boost +
`RateLimitSetpoint` (elapsed-since-write budget) + 0.25 deadband, seeded from the logged
setpoint. `feedforward_pct = last_raw_demand_pct` = pre-smoothing raw curve demand
(`control_status_writer.cpp:21`). Validated on a **608 s steady run (Tctl 73–78 °C)**:
shipped-replay vs logged setpoint **mean|err| = 0.13 % (ch2) / 0.21 % (ch4)**; on the
rise segments 0.1 %. The model is faithful.

**Steady-state setpoint std / jitter on the steady run (lower = steadier):**

| candidate | ch2 std | ch2 jit | ch4 std | ch4 jit |
|---|---|---|---|---|
| shipped | 1.57 | 0.009 | 2.61 | 0.009 |
| deadband 0.5 | 1.60 | 0.007 | 2.63 | 0.009 |
| demand-hyst 2 % | 1.61 | 0.011 | 2.59 | 0.010 |
| demand-hyst 4 % | **1.35** | 0.009 | 2.67 | 0.010 |
| hyst4 + half rate-limit | 1.38 | 0.009 | 2.77 | 0.010 |

**Up-response to a real +19 % raw-demand rise (ticks to 63 % of step; val err 0.1 %):**

| candidate | ch2 | ch4 |
|---|---|---|
| shipped | 39.5 s | 41.0 s |
| demand-hyst 4 % | 41.8 s | 42.0 s |
| hyst4 + upward delta-gate | **37.2 s** | **37.2 s** |

## Conclusions (validated)

1. **At genuinely steady load the fans are already steady** — setpoint std 1.5–2.6 %,
   tick jitter ~0.009 %/tick. The audible "variance" is the fans **legitimately
   tracking real load swings** (Tctl moving 73–78 °C over minutes), plus louder absolute
   RPM under load, NOT control noise. There is little steady-state noise to remove.
2. **Blanket long EMA is rejected** — it does not reduce tick jitter (rate-limiter-bound)
   and badly delays the up-response. Consistent with `[[control-is-feedforward-airflow-not-pid]]`.
3. **demand-side hysteresis ≈4 %** trims ch2 steady std 1.57→1.35 (~14 %); **no help on
   ch4** (2.61→2.67). Even the best steadiness lever is modest. (std under-measures
   "hunting"/direction-reversals — hysteresis may help the ear more than std shows; not
   yet measured.)
4. **The valuable change is the up-response, not steady-state.** Shipped takes ~40 s to
   63 % of a +19 % rise (rate-limiter-dominated) — this is the "caught flat at 80+"
   gap. An **upward delta-rise gate** speeds it to ~37 s **and coexists with hysteresis**
   (steady hold + faster up). FEAT-0024's `rise_rate` raise attacks the same 40 s and is
   the endorsed part; its ch4 `rise_alpha` raise is reversed (opposes long-EMA intent).

## Recommended direction (validated)

- Keep FEAT-0024's intake **`rise_rate`/`max_step` raise** (faster up; the 40 s gap is
  the real problem). Reverse the ch4 `rise_alpha` raise.
- Add a small **upward delta-rise gate** (new curve-law term: on a fast raw-demand rise,
  bypass smoothing/hysteresis) — the only fast path, upward only.
- Add a modest **demand-side hysteresis (~4 %)** on the 200 mm intakes for steady-hold;
  skip it on ch4 (no benefit). New, small curve-law term.
- These are new mechanisms (config + small C++), so they reshape FEAT-0024 into a
  steady-hold + delta-rise feature rather than the pure config retune. Gains at true
  steady-state are modest; the up-response gain is the justification.
