# Rate-limiter elapsed-cap — decision (2026-06-25)

**Status:** Current (direction). The shipped cap value and the live before/after
evidence are settled by the FEAT-0025 validation gate.
**Owns the direction for:** `docs/features/FEAT-0025-rate-limit-elapsed-cap.md`
(`REQ-SLEWCAP-*`).
**Companion to:** `docs/CONTROL_PIPELINE_MATH.md` §8 (the identity it amends),
`docs/COOLING_STRATEGY.md`, `docs/response-evaluation-tuning-plan.md`,
`docs/MEASUREMENT_GATE.md`, `docs/intake-lead-grounding-2026-06-25.md` (the
forensic + replay evidence).

> Dated design-decision record per `docs/features/README.md` §3 promotion gate 3.

## 1. Problem (forensic, 2026-06-25)

Operator report: fan tightness "got worse in less than a day," before any config
change. A per-session forensic timeline keyed to the binary/config sha
(`docs/intake-lead-grounding-2026-06-25.md`) found:

- The control config is **byte-identical across all sessions**
  (`config_sha256 = 45a0a1c7`) — not a slew/curve edit.
- The regression is **loop timing**, landing at the `ba83aed` rebuild deployed in
  the prior 06-25 session: `loop_slip` 1.2 ms → up to 13 ms, `loop_work` p99
  ~50 ms → 70–191 ms, intervals spiking to 3.58 s. Worst during the energy/sweeper
  live-M captures (the off-thread sweeper's PawnIO handle contends with the control
  loop's AMD reads for the `Global\Access_PCI` mutex), partially recovering after.

**Mechanism (why a timing problem sounds like a slew problem):**
`RateLimitSetpoint` sizes each step by the rate budget
$\alpha = \min(\rho \cdot \Delta t^{\mathrm{write}} / 60000,\; \delta_{\max})$,
where $\Delta t^{\mathrm{write}}$ is elapsed-since-last-write
(`src/control/channel_evaluator.cpp:370`, `docs/CONTROL_PIPELINE_MATH.md` §8.1). At
a uniform 250 ms tick (the "tight" past) every step is ~0.375 %. When a tick slips,
$\Delta t^{\mathrm{write}}$ grows, the budget grows, and the setpoint **overshoots
the noisy curve target and then corrects back** — an audible direction-reversal
"hunting." Identical config, looser feel, from loop jitter alone.

## 2. Decision

### D-SLEWCAP-1 — Cap the elapsed used in the rate budget
Amend `RateLimitSetpoint` so the rate budget uses a **bounded** elapsed:

$$\alpha = \min\bigl(\rho \cdot \min(\Delta t^{\mathrm{write}},\, \Delta t_{\max}) / 60000,\; \delta_{\max}\bigr)$$

where $\Delta t_{\max}$ is a new config field `rate_limit_max_elapsed_ms`. When it is
unset / non-positive, the elapsed is uncapped and behavior is **identical to today**
(identity preserved, back-compatible).

### D-SLEWCAP-2 — Loop-level config, applied to both laws and the low-band RL
`rate_limit_max_elapsed_ms` is a single `control_loop`-level setting (loop slip is
systemic, not per-channel), threaded into every `RateLimitSetpoint` call. Because
the operator already shares one rate limiter across the curve law, the FEAT-0003 PID
law, and the low-band stage boost (`CONTROL_PIPELINE_MATH.md` §8.1), the cap applies
uniformly to all three — consistent by construction.

### D-SLEWCAP-3 — Shipped value ≈ nominal tick + margin (candidate 300 ms)
The shipped cap is set just above the nominal tick (`poll_tick_ms = 250`), candidate
**300 ms**. Consequence (the key safety property): at nominal cadence
$\Delta t^{\mathrm{write}} \le 300$ ms, so the cap is **inactive and the output is
byte-identical to the pre-feature path** — only ticks whose elapsed exceeds the cap
(slip, or deadband-accumulated gaps) are clipped. The fix touches the pathological
tail, not normal operation. The exact value is settled by the validation gate.

### D-SLEWCAP-4 — Cap, not fixed-per-tick step
A fixed per-tick step was rejected: it discards legitimate catch-up after a longer
gap. The cap allows bounded catch-up (up to $\Delta t_{\max}$) while removing the
unbounded-elapsed overshoot — the minimal change that fixes the mechanism.

## 3. Validated evidence (replay)

From `docs/intake-lead-grounding-2026-06-25.md` (replay v3, model validated to
0.10–0.12 % vs the logged setpoint, fed the **real** per-tick
`loop_achieved_interval_ms` on the highest-slip session):

| metric (loaded band) | shipped | cap @300 ms |
|---|---|---|
| ch2 direction reversals /1000 | 72 | ~0 |
| ch4 direction reversals /1000 | 58 | ~0 |
| ch4 per-write step std / max | 0.032 / 0.37 | 0.018 / 0.30 |
| up-response to a real rise (63 %) | 27.2 s | 27.2 s (unchanged) |

The cap eliminates the slip-induced overshoot/"hunting" and uniformizes steps, with
**zero cost to the up-response**. (`max_setpoint_step_pct` already bounds absolute
step size; the cap's specific win is removing the elapsed-driven overshoot-then-
reverse pattern under slip. Reversal→0 is on the worst-slip window; the live gate
confirms it across clean windows.)

## 3b. Live mechanism confirmation (CPU-stress, cap-off, 2026-06-25)

A live capture on the **current shipped** config (session `20260625_190530`, build
`b6770464`, `config_sha256 = c5b5cb21` — i.e. *after* the FEAT-0024 intake-lead
merge, not the `45a0a1c7` forensic baseline of §1) under operator-driven 100%-busy
y-cruncher bursts independently reconfirms the §1 mechanism on hardware. This is the
**cap-off baseline** observation — the cap is not yet implemented — and is *not* the
REQ-SLEWCAP-05 before/after gate:

- **The elapsed→step coupling is real and monotonic.** Max up-step per tick, binned
  by `loop_slip_ms`: ≤50 ms → 0.00 % p50 / 0.52 % max; 50–300 ms → 0.53 % p50;
  300–1000 ms → 0.60 % p50; **>1000 ms → 0.80 % p50 / 0.95 % p90 / 1.16 % max**.
  Larger slip authorizes a larger step, exactly as §1 predicts.
- **Slip recurs from a non-sweeper cause** — the §4 durability point, observed. The
  off-thread sweeper was **off** this session (`cpu_cycles_acquisition = disabled`,
  `cpu_pkg_energy_acquisition = quarantine`), yet plain CPU saturation drove
  `loop_achieved_interval_ms` p50 `250.9 → 1127 ms` (slip p50 `877 ms`, max `5.6 s`,
  cadence ≈ `3.99 → 0.89 Hz`) at `system_cpu_busy_pct ≥ 99 %`, via both read-path
  block (`loop_work` up to `4.5 s`) and scheduling starvation (wait up to `4.9 s`).
  Stopping the sweeper does not remove the fragility; CPU contention alone reproduces
  it.
- **Bounding the claim — no reversals in this workload.** Under the monotonic CPU
  rise the inflated steps did **not** reverse: 0 down-steps > deadband (0.25 %) on
  ch0–4, one 0.31 % on ch5. The `58–72 / 1000` reversals in §3 come from the
  GPU-confounded loaded band where the curve target oscillates; the step-inflation
  mechanism is general, but it becomes audible "hunting" only when the target is
  noisy. The cap remains the right fix (it removes the inflation at the source); the
  acoustic payoff is largest on oscillating-target workloads.

Source: live control-loop CSV from session `20260625_190530`, not committed
(`AGENTS.md` §Live Runtime Safety).

## 4. Why this over the operational alternatives

- **Reduce telemetry / stop the sweeper** (operational): the captures are already
  done and the sweeper is off, so the acute cause is past — but this does not prevent
  recurrence the next time any telemetry, capture, or external contention slips the
  loop. Not durable.
- **Revert the `ba83aed` binary** (operational): does not address the latent
  elapsed-budget fragility; the same slip from any future cause would re-loosen.
- **The elapsed-cap is durable:** it decouples fan smoothness from loop timing for
  all causes, permanently, and is near-identity at nominal cadence.

## 5. Scope boundary

- This is **independent of FEAT-0024** (intake-lead, merged PR #32, not deployed,
  disposition still open). FEAT-0025 fixes the steady-load tightness regression;
  FEAT-0024 is about up-response lead.
- The earlier **demand-hysteresis / delta-rise** idea is **shelved**: the validated
  data (`docs/intake-lead-grounding-2026-06-25.md`) showed steady-state was already
  steady (std 1.5–2.6 %, tick jitter 0.009 %/tick) and the real culprit was loop
  timing. See `[[control-is-feedforward-airflow-not-pid]]`.

## 6. Validation & rollout

- **T:** unit test of `RateLimitSetpoint` — capped elapsed clips the budget; unset =
  identity; nominal-cadence (elapsed ≤ cap) output byte-identical to uncapped.
- **M (live gate):** a before/after capture (cap off → on) showing the slip-induced
  reversal / step-irregularity drop and the up-response unchanged, analyzed per
  `docs/response-evaluation-tuning-plan.md`. Respect `AGENTS.md` §Live Runtime Safety.
- **Measurement gate:** not crossed — `poll_tick_ms`, `write_cooldown_ms`,
  `deadband_pct`, the channel set, and curves are unchanged; only the rate-budget
  elapsed is bounded.
