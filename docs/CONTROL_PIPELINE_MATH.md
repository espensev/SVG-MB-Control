# Control Pipeline — Mathematical Reference

Status: **current**, last verified 2026-05-28 against
`src/control/tick_runner.cpp` (per-tick orchestration),
`src/control/channel_evaluator.cpp` (curve, smoothing, boost composition,
rate limit, authority reassert),
`src/control/boost_stage.cpp` (per-stage boost integrator shared by
thermal_pressure, midband_pressure, gpu_airflow, and cpu_low_soak),
`src/control/low_band_integrator.cpp` (global signal, debt, per-channel
stage), `src/control/cadence_score.cpp` (slew score, cadence target,
generic rate-limit helper), `src/control/channel_write.cpp` (write
gates), `src/control/control_scheduler.cpp` (sliced wait), and
`src/policy/control_policy.cpp` (curve / blend helpers). Cross-cuts the
prose in `CONTROL_LOOP.md` with the actual numerical operators.

This document is normative for the **computation**, not for the lifecycle
(start / stop / supervision / persistence) — those remain in
`CONTROL_LOOP.md`. Section anchors call out the source line that implements
each equation so this can be diffed against future changes.

Keep this file in lock-step with source, shipped config, and runtime traces.
Any change to curve lookup, smoothing, boost composition, low-band behavior,
cadence scoring, CSV/status control fields, or channel response attribution
must update this reference and the cross-references in `CONTROL_LOOP.md`,
`RUNTIME_HOME.md`, and `RUNTIME_LOGGING_AND_EVALUATION.md`.

---

## 1. Notation

| Symbol | Meaning | Source |
|---|---|---|
| $k$ | tick index ($k = 0, 1, 2, \dots$) | `tick_count` |
| $t_k$ | steady-clock time of tick $k$ start | `tick_started_steady` |
| $\Delta t_k = t_k - t_{k-1}$ | inter-tick interval, ms | `loop_achieved_interval_ms` |
| $P$ | configured tick interval, ms | `poll_tick_ms` |
| $F$ | adaptive-cadence floor, ms ($F \le P$) | `poll_tick_floor_ms` |
| $E_k$ | effective tick interval, ms ($F \le E_k \le P$) | `loop_intended_interval_ms` |
| $c \in \mathcal{C}$ | channel index (configured channels) | `context.channels` |
| $T^{\mathrm{cpu}}_k$ | CPU Tctl/Tdie, °C, or undefined | `temp_inputs.cpu_c` |
| $T^{\mathrm{gpu}}_k$ | GPU control envelope, °C, or undefined | `temp_inputs.gpu_c` |
| $T^{(c)}_k$ | primary curve temperature for channel $c$ | `SelectPrimaryCurveInput(...)` |
| $u^{(c)}_k$ | issued duty setpoint, % | `last_issued_pct` |
| $r^{(c)}_k$ | raw curve demand, % | `last_raw_demand_pct` |
| $s^{(c)}_k$ | smoothed demand, % | `smoothed_demand_pct` |
| $B^{(c, \cdot)}_k$ | additive pressure boost terms, % | per-channel state |
| $\mathrm{S}_5(x)$ | quintic smootherstep, $x \in [0, 1]$ | `SmootherStep` |
| $\sigma(x; a, b)$ | smootherstep-scaled ramp ($a < b$) | `SmoothScale` |

Undefined sensor readings are tracked with `NaN`; "$x$ undefined" below
denotes `std::isnan(x) == true` or an explicit *not available* flag.

The quintic blend and clipped ramp are used pervasively:

$$
\mathrm{S}_5(x) = x^3 (6 x^2 - 15 x + 10), \qquad x \in [0, 1].
$$

$$
\sigma(x; a, b) =
\begin{cases}
0 & x < a \text{ or } x \text{ undefined} \\
\mathrm{S}_5\!\left(\dfrac{x - a}{b - a}\right) & a \le x \le b,\; b > a \\
1 & x > b.
\end{cases}
$$

$\mathrm{S}_5$ satisfies $\mathrm{S}_5(0) = 0$, $\mathrm{S}_5(1) = 1$,
$\mathrm{S}'_5(0) = \mathrm{S}'_5(1) = 0$, and
$\mathrm{S}''_5(0) = \mathrm{S}''_5(1) = 0$ — first and second derivatives
vanish at both endpoints, so any quantity gated by $\sigma$ enters and
leaves the band $[a, b]$ with continuous acceleration.

The implementation uses Horner form,
`t * t * t * ((6.0 * t - 15.0) * t + 10.0)`, which is
algebraically identical to the polynomial above and numerically preferred.

---

## 2. Per-tick pipeline

Each tick $k$ executes the following composition. Read top-to-bottom; arrows
denote data dependencies, not control flow.

```
                 telemetry (AMD, GPU, SIO/fan)
                              │
                              ▼
              ┌──── TempInputs (CPU, GPU envelope) ────┐
              │                                        │
              ▼                                        ▼
      low-band global state                  per-channel evaluation:
      (signal, debt, stages)                  T(c), src(c) ← primary input
              │                                r(c) ← curve(T(c))
              │                                r(c) ← max(r(c), curve_cpu(Tcpu))
              │                                s(c) ← EMA + bounded decay
              │                                B(c,thp)  ← integrator(T(c))
              │                                B(c,mid)  ← integrator(T(c))
              │                                B(c,gpu)  ← integrator(Tgpu)
              │                                B(c,soak) ← integrator(Tcpu, per-min)
              ▼                                          │
         B(c,lb_eff) = min(B(c,lb_stage),                │
                           low_band_residual_cap_pct)    │
              └──────────────────┬─────────────────────────┘
                                 ▼
              u_des(c) = clip( s(c) + Σ boosts , 0, 100 )
                                 │
                                 ▼
                u(c) = rate_limit( u_des(c), u_prev(c), Δt_write )
                                 │
                                 ▼
              gates: deadband, cooldown, policy, breaker
                                 │
                                 ▼
                       fan write or hold
                                 │
                                 ▼
              cadence: E_k = round( P - τ_k · (P - F) )
                       with τ_k = slew-derived transient
```

---

## 3. Temperature inputs

### 3.1 CPU temperature

Read from the AMD sensor whose label equals
`control_loop.cpu_temp_label`. If absent, $T^{\mathrm{cpu}}_k$ is undefined.

### 3.2 GPU control envelope

$$
T^{\mathrm{gpu}}_k = \max\bigl(T^{\mathrm{core}}_k,\; T^{\mathrm{memjn}}_k,\;
T^{\mathrm{hotspot}}_k \cdot \mathbb{1}[T^{\mathrm{hotspot}}_k > 0]\bigr).
$$

Implemented in `GpuControlEnvelopeC()`. `hotspot` is only mixed in when
the snapshot reports a strictly positive value. If `gpu.available` is
false, $T^{\mathrm{gpu}}_k$ is undefined.

### 3.3 Primary curve temperature

For channel $c$ with configured `temp_blend`:

$$
T^{(c)}_k =
\begin{cases}
T^{\mathrm{cpu}}_k & \text{`cpu_only`} \\
T^{\mathrm{gpu}}_k & \text{`gpu_only`} \\
\max(T^{\mathrm{cpu}}_k,\; T^{\mathrm{gpu}}_k) & \text{`max_cpu_gpu`} \\
T^{\mathrm{gpu}}_k & \text{`max_cpu_gpu_source_aware`, CPU below guard, GPU available} \\
T^{\mathrm{cpu}}_k & \text{`max_cpu_gpu_source_aware`, CPU below guard, GPU unavailable} \\
\max(T^{\mathrm{cpu}}_k,\; T^{\mathrm{gpu}}_k) & \text{`max_cpu_gpu_source_aware`, CPU at/above guard}
\end{cases}
$$

The source-aware guard is `source_aware_cpu_hot_guard_c`. If the guard is
undefined or CPU is unavailable, `max_cpu_gpu_source_aware` uses the GPU
envelope when available. If GPU telemetry is unavailable below the guard but
CPU telemetry is available, the primary curve falls back to CPU instead of
entering sensor-safe mode. At or above the guard, it deliberately reuses legacy
`max_cpu_gpu` raw-temperature selection to retain high-CPU support.

The policy helper `BlendTemps` exposes the simplified source-aware
temperature preference for simple callers and C++ smoke tests: GPU when
available, CPU fallback otherwise. The control loop itself uses
`SelectPrimaryCurveInput`, which returns both the temperature and a source
label: `cpu`, `gpu`, `cpu_fallback`, `cpu_guard`, `gpu_guard`, or
`unavailable`.

---

## 4. Curve evaluation

A curve is a piecewise function defined by sorted points
$\{(\tau_i, d_i)\}_{i=0}^{N-1}$ with $\tau_0 < \tau_1 < \dots < \tau_{N-1}$.

### 4.1 Linear shape

$$
g_{\mathrm{lin}}(T) =
\begin{cases}
d_0 & T \le \tau_0 \\
d_{i-1} + \dfrac{T - \tau_{i-1}}{\tau_i - \tau_{i-1}} (d_i - d_{i-1})
  & \tau_{i-1} < T \le \tau_i \\
d_{N-1} & T > \tau_{N-1}.
\end{cases}
$$

### 4.2 Smootherstep shape

Same partition; the segment-local parameter is reshaped by $\mathrm{S}_5$:

$$
g_{\mathrm{smooth}}(T) = d_{i-1}
  + \mathrm{S}_5\!\left(\dfrac{T - \tau_{i-1}}{\tau_i - \tau_{i-1}}\right)
    (d_i - d_{i-1}),
\quad \tau_{i-1} < T \le \tau_i.
$$

### 4.3 Floor and clip

$$
r^{(c)}_k = \mathrm{clip}\bigl( g_{\,\mathrm{shape}}(T^{(c)}_k),\;
\max(0, m^{(c)}),\; 100 \bigr),
$$

with $m^{(c)} =$ `min_duty_pct`.

### 4.4 CPU override (max of two)

If `cpu_override_curve` is non-empty and $T^{\mathrm{cpu}}_k$ is defined,
the override demand is $r^{(c, \mathrm{cpu})}_k =
\mathrm{clip}\bigl(g_{\,\mathrm{shape}}(T^{\mathrm{cpu}}_k),
\max(0, m^{(c)}), 100\bigr)$ and the post-override raw demand is

$$
r^{(c)}_k \leftarrow \max\bigl(r^{(c)}_k,\; r^{(c, \mathrm{cpu})}_k\bigr).
$$

When the override dominates, the channel's "observed temperature" used by
the integrators (§6) becomes $T^{\mathrm{cpu}}_k$ rather than
$T^{(c)}_k$.

---

## 5. Demand smoothing

Let $\Delta = r^{(c)}_k - s^{(c)}_{k-1}$ and
$\Delta\tau = \Delta t_k / 60\,000$ (in minutes). Configured per channel:
$\alpha_{\uparrow}$ (`demand_smoothing_rise_alpha`),
$\alpha_{\downarrow}$ (`demand_smoothing_fall_alpha`),
$\rho^{\downarrow}$ (`decay_latch_pct_per_min`),
$L$ (`decay_latch_above_pct`). The implementation clamps both $\alpha$
values to $[0, 1]$ at use, so out-of-range smoothing config silently
saturates rather than overshooting or inverting the EMA direction. The
*evaluation* interval $\Delta t_k$ used here is the channel's
`elapsed_since_last_evaluation_ms` (time since the channel's previous
`EvaluateChannel` call), not the write interval used by the rate limiter
in §8.3.

If $|\Delta| \le 10^{-4}$, $s^{(c)}_k = r^{(c)}_k$. Otherwise:

**Rising** ($\Delta > 0$):

$$
s^{(c)}_k =
\begin{cases}
r^{(c)}_k & \alpha_{\uparrow} \text{ undefined} \\
\mathrm{clip}\bigl(s^{(c)}_{k-1} + \alpha_{\uparrow}\,\Delta,\; 0,\; 100\bigr)
  & \text{otherwise.}
\end{cases}
$$

**Falling** ($\Delta < 0$): first apply EMA when $\alpha_{\downarrow}$ is
configured, $\tilde s = s^{(c)}_{k-1} + \alpha_{\downarrow}\,\Delta$
(otherwise $\tilde s = r^{(c)}_k$). Then, if a decay latch is configured
($\rho^{\downarrow} > 0$, $\Delta t_k > 0$) **and** either the latch
threshold is undefined or $s^{(c)}_{k-1} \ge L$ or $\tilde s \ge L$, cap
the drop per tick:

$$
s^{(c)}_k = \mathrm{clip}\bigl(
   \max(\tilde s,\; s^{(c)}_{k-1} - \rho^{\downarrow}\,\Delta\tau),\;
   0,\; 100\bigr).
$$

If the latch zone does not apply, $s^{(c)}_k = \mathrm{clip}(\tilde s, 0, 100)$.

Behavior summary: rise smoothing is a plain EMA; fall smoothing is an EMA
followed by a per-minute floor (the "latch") whenever the smoothed level
is anywhere near the configured high band.

---

## 6. Pressure-boost integrators

Four additive boost terms accumulate on top of $s^{(c)}_k$. Three share an
identical integrator with seconds-scale rates; the fourth (CPU low-soak)
uses a minutes-scale rate. All four are clamped to a configured maximum
and return to zero in their respective release conditions.

Implementation: a single `UpdateBoostStage` in `src/control/boost_stage.cpp`
covers all four, driven by `kBoostStageSpecs` which tags each stage with
its input source (`ObservedTemp` / `GpuEnvelope` / `CpuTemp`), rate unit
(`PerSec` / `PerMin`), and release semantics (`BelowStart` for the three
seconds-scale stages, `ExplicitRelease` for the CPU low-soak band).
Per-channel state is `ChannelState::boosts[BoostStage]` (configured side:
`ChannelControlConfig::boosts[BoostStage]`).

### 6.1 Unified seconds-scale integrator

Given a generic boost state $B$, observed temperature $T_{\mathrm{obs}}$,
band $[a, b]$, rise rate $\gamma^{\uparrow}$ (%/s), fall rate
$\gamma^{\downarrow}$ (%/s), and ceiling $B_{\max}$:

$$
B \leftarrow \mathrm{clip}(B, 0, B_{\max}).
$$

If $T_{\mathrm{obs}} \ge a$ **and** $B < B_{\max}$:

$$
B_k = B_{k-1} + \gamma^{\uparrow} \cdot \sigma_{\mathrm{loc}}(T_{\mathrm{obs}})
\cdot \dfrac{\Delta t_k}{1000},
\qquad
\sigma_{\mathrm{loc}}(T_{\mathrm{obs}}) = \begin{cases}
1 & b \le a \\
\mathrm{S}_5\!\left(\mathrm{clip}\bigl(\tfrac{T_{\mathrm{obs}} - a}{b - a}, 0, 1\bigr)\right) & b > a.
\end{cases}
$$

Otherwise, if $T_{\mathrm{obs}} < a$:

$$
B_k = B_{k-1} - \gamma^{\downarrow} \cdot \dfrac{\Delta t_k}{1000}.
$$

Finally $B_k \leftarrow \mathrm{clip}(B_k, 0, B_{\max})$.

The integrator is **disabled** (returns $0$) when any of the start, full,
rise, fall, or max parameters is undefined, when the rise or max parameter is
non-positive, or when the fall parameter is negative (`UpdateBoostStage`
guard clause). A zero fall rate is valid and means no decay. When
$T_{\mathrm{obs}}$ is undefined the existing boost is held and no integration
occurs (`BelowStart` release mode). For the `ExplicitRelease` stage (CPU
low-soak) an undefined input causes decay instead, matching the pre-table
behavior.

Anti-windup is implicit: integration is gated on $B < B_{\max}$ on the
rising path. Decay is unconditional on the falling path.

### 6.2 Three seconds-scale stages

| State | $T_{\mathrm{obs}}$ | $(a, b)$ |
|---|---|---|
| Thermal pressure $B^{(c,\mathrm{thp})}$ | $T^{(c)}_k$ (or $T^{\mathrm{cpu}}_k$ on CPU override) | $a$ = `thermal_pressure_start_c`, $b$ = `..._full_c` |
| Mid-band pressure $B^{(c,\mathrm{mid})}$ | same as above | $a$ = `midband_pressure_start_c`, $b$ = `..._full_c` |
| GPU airflow $B^{(c,\mathrm{gpu})}$ | $T^{\mathrm{gpu}}_k$ (or undefined → integrator holds) | $a$ = `gpu_airflow_start_c`, $b$ = `..._full_c` |

### 6.3 CPU low-soak (minutes-scale)

Same shape, but rates are %/min and $\Delta\tau$ is used. The release
condition is asymmetric and uses a separate `cpu_low_soak_release_c`
hysteresis temperature $r$:

$$
B^{(c,\mathrm{soak})}_k =
\begin{cases}
B^{(c,\mathrm{soak})}_{k-1} + \gamma^{\uparrow}\,
  \mathrm{S}_5\!\left(\mathrm{clip}\bigl(\tfrac{T^{\mathrm{cpu}}_k - a}{b - a}, 0, 1\bigr)\right) \Delta\tau
& T^{\mathrm{cpu}}_k \ge a \\[6pt]
B^{(c,\mathrm{soak})}_{k-1} - \gamma^{\downarrow}\,\Delta\tau
& T^{\mathrm{cpu}}_k \le r \text{ or undefined} \\[2pt]
B^{(c,\mathrm{soak})}_{k-1} & \text{(holds)}
\end{cases}
$$

then clipped to $[0, B_{\max}]$. The band $(r, a)$ is a hysteresis zone:
neither accrual nor decay occurs there.

---

## 7. Low-band global signal, debt, and per-channel stages

Low-band is the **second-priority** stage: it provides a slow integral
contribution that activates only after sustained mid/upper-low-band
temperatures and yields to the primary pressure stages.

### 7.1 Global scales and signal

With config $\theta = $ `context.loop.low_band`:

$$
\sigma^{\mathrm{cpu}}_k = \sigma\bigl(T^{\mathrm{cpu}}_k;\; \theta.\mathrm{cpu\_start\_c},\; \theta.\mathrm{cpu\_full\_c}\bigr),
\quad
\sigma^{\mathrm{gpu}}_k = \sigma\bigl(T^{\mathrm{gpu}}_k;\; \theta.\mathrm{gpu\_start\_c},\; \theta.\mathrm{gpu\_full\_c}\bigr).
$$

The instantaneous **signal** is

$$
S_k = \mathrm{clip}\bigl(
  \max(w_{\mathrm{cpu}}\,\sigma^{\mathrm{cpu}}_k,\;
       w_{\mathrm{gpu}}\,\sigma^{\mathrm{gpu}}_k),\;
  0,\; 1\bigr),
\quad w_{\mathrm{cpu, gpu}} \in [0, 1].
$$

### 7.2 Debt with primary-response veto

The **primary-response flag**

$$
\Pi_k = \exists\, c : \max\bigl(B^{(c,\mathrm{mid})}_{k-1},
B^{(c,\mathrm{gpu})}_{k-1}, B^{(c,\mathrm{thp})}_{k-1}\bigr) > 0.05
$$

is computed from the **previous tick's** boost values (the integrator
runs once per tick before per-channel evaluation; the one-tick lag is
deliberate). Debt $D_k \in [0, 1]$ evolves as:

$$
D_k =
\begin{cases}
\mathrm{clip}\bigl(D_{k-1} + \rho^{\uparrow} S_k \Delta\tau,\; 0, 1\bigr)
  & S_k > 10^{-4} \text{ and } \neg\Pi_k \\
\mathrm{clip}\bigl(D_{k-1} - \rho^{\downarrow} \Delta\tau,\; 0, 1\bigr)
  & R^{\mathrm{cpu}}_k \text{ and } R^{\mathrm{gpu}}_k \\
D_{k-1} & \text{otherwise (held).}
\end{cases}
$$

(`rise_per_min` and `fall_per_min` are the per-minute rates;
$\Delta\tau$ uses the achieved tick interval when finite, else $P/60\,000$.)
The release predicates treat missing sensors as released:
$R^{\mathrm{cpu}}_k \equiv (T^{\mathrm{cpu}}_k \text{ undefined}) \lor
T^{\mathrm{cpu}}_k \le \theta.\mathrm{cpu\_release\_c}$, and similarly for
GPU.

### 7.3 Per-channel stage activation

Each channel $c$ tracks an *eligible-time* counter $\theta^{(c)}_k$ and a
boolean *active* latch $A^{(c)}_k$. With threshold
$\theta^{(c)} = $ `low_band_debt_threshold`:

$$
\theta^{(c)}_k =
\begin{cases}
\theta^{(c)}_{k-1} + \Delta t_k & D_k \ge \theta^{(c)} \\
0 & D_k < \theta^{(c)}.
\end{cases}
$$

The active latch transitions:

- $A^{(c)}_k \leftarrow \text{true}$ iff
  $A^{(c)}_{k-1} = \text{false}$,
  $\theta^{(c)}_k \ge $ `low_band_hold_ms`,
  and at least `stage_spacing_ms` has elapsed globally since the previous
  stage activation across any channel.
- $A^{(c)}_k \leftarrow \text{false}$ iff $D_k \le 0.75 \cdot \theta^{(c)}$
  (deactivation has a hysteresis margin of 25 % of the threshold).
- Otherwise $A^{(c)}_k = A^{(c)}_{k-1}$.

When $A^{(c)}_k$ is true the target boost is

$$
\hat B^{(c,\mathrm{lb})}_k = B^{(c)}_{\max,\mathrm{lb}} \cdot
\sigma\bigl(D_k; \theta^{(c)}, 1\bigr).
$$

Else $\hat B^{(c,\mathrm{lb})}_k = 0$. The realized stage boost
$B^{(c,\mathrm{lb})}_k$ is rate-limited toward $\hat B$ using
`stage_rise_pct_per_min` / `stage_fall_pct_per_min` (see §8.1) and
clipped to $[0, B^{(c)}_{\max,\mathrm{lb}}]$ (or to $0$ when the channel
is not configured for low-band).

### 7.4 Effective low-band contribution (residual cap)

If `low_band_residual_cap_pct` $= \kappa$ is configured:

$$
B^{(c,\mathrm{lb,eff})}_k = \min\bigl(B^{(c,\mathrm{lb})}_k,\; \kappa\bigr).
$$

When $\kappa$ is undefined, $B^{(c,\mathrm{lb,eff})}_k = B^{(c,\mathrm{lb})}_k$
(pre-feature behavior).

---

## 8. Setpoint composition and rate limit

### 8.1 Rate-limit operator

The generic rate-limiter used by both the channel setpoint and the
low-band stage boost is:

$$
\mathrm{RL}(x_{\mathrm{des}}, x_{\mathrm{prev}}, \Delta t;\;
  \rho^{\uparrow}, \rho^{\downarrow}, \delta_{\max}) =
\begin{cases}
x_{\mathrm{des}} & |x_{\mathrm{des}} - x_{\mathrm{prev}}| \le \alpha \\
x_{\mathrm{prev}} + \mathrm{sgn}(x_{\mathrm{des}} - x_{\mathrm{prev}})\,\alpha
  & \text{otherwise,}
\end{cases}
$$

where $\rho = \rho^{\uparrow}$ if $x_{\mathrm{des}} > x_{\mathrm{prev}}$ else
$\rho^{\downarrow}$, $\alpha = \min(\rho \cdot \Delta t / 60\,000,\;
\delta_{\max})$ (the optional `max_setpoint_step_pct` cap is only applied
when finite and positive), and $x_{\mathrm{prev}}$ undefined returns
$x_{\mathrm{des}}$ unconditionally. Identity holds when $\rho$ is
non-positive or undefined (no rate limit applied).

### 8.2 Desired setpoint

$$
u^{(c),\mathrm{des}}_k = \mathrm{clip}\Bigl(
  s^{(c)}_k + B^{(c,\mathrm{thp})}_k + B^{(c,\mathrm{mid})}_k +
  B^{(c,\mathrm{gpu})}_k + B^{(c,\mathrm{soak})}_k +
  B^{(c,\mathrm{lb,eff})}_k,\; 0,\; 100\Bigr).
$$

### 8.3 Rate-limited setpoint

$$
u^{(c)}_k = \mathrm{RL}\bigl(u^{(c),\mathrm{des}}_k, u^{(c)}_{k-1}, \Delta t^{\mathrm{write}};\;
  \rho^{\uparrow}_c, \rho^{\downarrow}_c, \delta_{\max,c}\bigr),
$$

where $\Delta t^{\mathrm{write}}$ is the elapsed time since the **previous
successful write** for that channel (`elapsed_since_last_write_ms`), not
the tick interval.

### 8.4 Feedforward / correction decomposition

The CSV publishes both raw demand and the deviation introduced by
smoothing, integrators, and the rate limiter:

$$
\mathrm{feedforward}^{(c)}_k = r^{(c)}_k,
\qquad
\mathrm{correction}^{(c)}_k = u^{(c)}_k - r^{(c)}_k.
$$

---

## 9. Output gates

A computed $u^{(c)}_k$ is committed only if every gate passes:

1. **Deadband.** If $|u^{(c)}_k - u^{(c)}_{k-1}| < \delta^{(c)}$
   (`effective_deadband_pct`), suppress. Bypassed when an
   authority-reassert flag is raised.
2. **Authority reassert.** When `effective_hold_ms` $= 0$ and the
   observed fan mode/duty has drifted beyond
   $\max(\texttt{kAuthorityDutyTolerancePct}, \delta^{(c)})$ from the
   last issued value, set the reassert flag.
3. **Cooldown.** Reject writes for which
   `elapsed_since_last_write_ms` $< C^{(c)}_{\mathrm{eff}}$, where
   $C^{(c)}_{\mathrm{eff}} = \min(C^{(c)},
   \texttt{kAuthorityReassertCooldownMs} = 2000)$ if the reassert flag is
   set, else $C^{(c)}$ (`effective_cooldown_ms`). Bypassed on
   `first_write` only.
4. **Baseline captured.** Skip if no baseline duty/mode has been recorded
   yet for the channel.
5. **Policy.** Skip if `effective_write_allowed` is false in the runtime
   fan snapshot for the channel.
6. **Breaker.** Skip if the per-channel circuit breaker is open, **unless**
   this is a sensor-safe (safe-mode) command (`safety_override`): a
   thermal-safety write must reach the hardware even when the breaker is open
   (`docs/discovery-recovery-gap-audit-2026-06-04.md`, remediation 3). A normal
   command is still skipped while the breaker is open. This gates *whether* a
   computed setpoint is written; it does not change the setpoint value, so the
   control identity above is unaffected.

A successful write updates $u^{(c)}_{k-1} \leftarrow u^{(c)}_k$ and
records `last_write_time` $= t_k$.

A **failed** write increments `consecutive_write_failures`; once it
reaches `kMaxConsecutiveFailures` ($= 5$) the breaker opens. The open breaker
then gates future normal writes for that channel; a sensor-safe (safe-mode)
command bypasses it, and a successful bypassed write closes the breaker.
`--reset-breakers` clears open
breakers and failure counters through `circuit_breaker_reset.request.json`;
`--reset-breaker-channel <n>` narrows the reset to one channel. The next write
still passes through the same baseline, policy, cooldown, and fan-backend gates.

---

## 10. Adaptive cadence

The effective tick interval $E_k$ is computed at end-of-tick from a
unitless transient $\tau_k \in [0, 1]$ derived from temperature slew.

### 10.1 Slew score (Phase 2)

Let the achieved interval be $\Delta t_k$ (in ms). The per-sensor slew is

$$
\dot T^{\mathrm{cpu}}_k = \dfrac{|T^{\mathrm{cpu}}_k - T^{\mathrm{cpu}}_{k-1}|}{\Delta t_k / 1000},
\qquad
\dot T^{\mathrm{gpu}}_k = \dfrac{|T^{\mathrm{gpu}}_k - T^{\mathrm{gpu}}_{k-1}|}{\Delta t_k / 1000},
$$

defined only when both the current and previous sample for that sensor
exist. The peak slew is $\dot T_k = \max(\dot T^{\mathrm{cpu}}_k,
\dot T^{\mathrm{gpu}}_k)$ with each missing term taken as $0$. The
score is

$$
\tau^{\mathrm{slew}}_k = \sigma\bigl(\dot T_k;\;
\texttt{cadence\_slew\_start\_c\_per\_s},\;
\texttt{cadence\_slew\_full\_c\_per\_s}\bigr).
$$

Phase 2 sets $\tau_k = \tau^{\mathrm{slew}}_k$ (the setpoint-motion term
of the original design is deferred to Phase 2b).

### 10.2 Target and rate limit

The instantaneous target is

$$
\widehat E_k = \mathrm{round}\bigl(P - \tau_k \cdot (P - F)\bigr).
$$

The realized effective interval is rate-limited so it can tighten
instantly but only relaxes back upward at `cadence_relax_per_s`:

$$
E_k = \mathrm{clip}\Bigl(\mathrm{RL}\bigl(\widehat E_k, E_{k-1},
\Delta t_k;\; 60 \cdot \texttt{cadence\_relax\_per\_s},\; \infty,\; \infty\bigr),\;
F,\; P\Bigr).
$$

The downward (tightening) rate is effectively infinite, so $E_k$ snaps
down on a fresh transient; the upward rate is `cadence_relax_per_s`
per second. When $F = P$ (default, and the shipped profile), $E_k
\equiv P$ and the cadence path is byte-identical to the pre-feature
loop.

The CSV / status field `loop_slip_ms` is
$\Delta t_k - P$, i.e. the achieved interval minus the **base** poll
period $P$, not minus the effective $E_k$. Slip is therefore a measure
of overshoot against the configured base cadence and remains a valid
budget indicator even when $F < P$.

### 10.3 Stop-latency interaction

`WaitForNextControlTick` waits until $t_k + E_k$, but slices the wait
into at most 50 ms intervals and re-evaluates both the in-process stop
flag and the on-disk `stop.request.json` between slices. Maximum stop
latency is therefore $\min(50\,\mathrm{ms},\; E_k - (\text{now} - t_k))$.

---

## 11. Sensor-safe mode

If the primary curve input $T^{(c)}_k$ is undefined for
`kMaxConsecutiveSensorFailures` $= 3$ consecutive ticks, the channel
enters safe mode and overrides the curve evaluation with

$$
r^{(c)}_k = \texttt{kSafeModeFanDuty} = 100, \quad
\text{response\_source} = \text{`sensor\_safe\_mode'.}
$$

Smoothing, integrators, rate limit, and gates still apply to $u^{(c)}_k$
once safe mode is entered. Recovery occurs on the first tick with a
defined $T^{(c)}_k$; the integrators carry their pre-failure state.

---

## 12. Properties and invariants

1. **Bounded setpoint.** $u^{(c)}_k \in [0, 100]$ for all $c, k$, regardless
   of curve / integrator misconfiguration, by the clip in §8.2.
2. **Pre-feature equivalence.**
   - $F = P$ (default) ⟹ $E_k = P \forall k$. The cadence path is a no-op.
   - All `*_max_boost_pct` undefined / non-positive ⟹ every integrator
     returns $0$, $u^{(c),\mathrm{des}}_k = s^{(c)}_k$, and only EMA +
     rate-limit shape the setpoint.
   - `low_band.enabled = false` ⟹ $D_k$ never updates,
     $B^{(c,\mathrm{lb})}_k$ stays at its default $0$.
3. **Low-band yields to primary.** While $\Pi_k$ is true (any channel has
   any primary boost $> 0.05$%), $D_k$ does not accrue. Combined with
   $\kappa$ (residual cap), the low-band contribution to the final
   setpoint is bounded above by both $\kappa$ and the channel's
   `low_band_max_boost_pct`.
4. **Anti-windup.** Pressure integrators do not accrue once at ceiling
   (`boost < max_boost_pct` guard) and unconditionally decay below
   threshold. They cannot wind up beyond `*_max_boost_pct`.
5. **Smoothness of triggering bands.** $\sigma$ is $C^2$ at both
   endpoints; entering or leaving any band (`midband`, `gpu_airflow`,
   `thermal_pressure`, low-band scales, cadence slew) does not introduce
   a discontinuity in the contribution's first or second derivative.
6. **Rate-limit consistency.** Both $\mathrm{RL}$ users (setpoint and
   cadence) reduce to identity when the per-direction rate is undefined
   or non-positive; both honor a finite absolute step cap when one is
   provided.

---

## 13. Real-data validation

The equations above are necessary but not sufficient. Each run-backed tuning
pass should also validate them against the runtime CSV and status bundle.

### 13.1 Current validation state

2026-05-20 local check:

- Source/config comparison passed for the equations documented here, with the
  guard correction in §6.1.
- Shipped configs `config/control.example.json` and
  `config/control.release.json` currently assert
  `control_loop.poll_tick_ms=250`, `write_cooldown_ms=250`,
  `deadband_pct<=0.25`, and live channels `0,1,2,3,4,5`.
- `release\svg-mb-control.exe --status --json` reported `stopped` and no
  `release\runtime\control_runtime.json`; no current control-loop CSV was
  present in the checked runtime locations. This pass therefore did not make a
  trace-backed claim about current thermal response quality.

Do not promote a controller tuning conclusion from this document alone. If no
runtime CSV/status bundle is available, record that explicitly and treat the
math check as code/config validation only.

2026-05-24 Cinebench R23 response check:

- Source run:
  `release\runtime\logs\archive\svg_mb_control_control-loop_20260524_024240.csv`,
  producer `svg-mb-control` `0.1.0`, git hash `94a1d4c6a34c`, config SHA256
  `51a16ea6b673f6610bea59c28865b87458843188d4e17195a147c977718fbc78`.
- Reported workload score was Cinebench R23 `42236`; this was used only as
  evidence that a meaningful CPU heat load was present, not as the tuning
  objective.
- After filtering malformed rows and physically impossible telemetry, the
  CPU-heavy segment (`cpu_tctl_c >= 80`) held CPU/Tctl at p50 `87.625 C`,
  p90 `88.250 C`, p99 `88.976 C`, and max `89.250 C`. GPU memory stayed cool
  in that segment (p90 `48 C`, max `50 C`), so the run validates CPU response
  only, not combined CPU+GPU behavior.
- Fan response during the CPU-heavy segment showed channel `2` p90/max
  `94.40/95.11%`, channel `3` `90.40/91.11%`, and channel `4`
  `92.03/92.20%`, while radiator exhaust candidates channel `1` and channel
  `5` were much lower at `57.23/57.51%` and `59.71/60.09%`. The high-heat
  response therefore over-relied on intake/front lanes relative to the
  remaining exhaust headroom.
- The release config was adjusted to raise `thermal_pressure_max_boost_pct` to
  `20.0` on channels `1` and `5` and lower it to `14.0` on channel `4`, with
  steeper `cpu_override_curve` knees on `1`/`5` than on `4`. The next validation
  pass should compare the same CPU-heavy workload against this config and
  inspect CPU p90/max, ch1/ch5 RPM, ch4 setpoint, subjective noise, and any
  sidecar/logging errors.
- The run also exposed logging-quality issues: malformed CSV rows, invalid
  JSONL rows, and Windows error 5 sidecar/evidence write failures. Treat raw
  analyzer maxima from this run as unreliable unless filtered.

2026-05-26 low-load steady-state check (static-floor reference profile,
commit `10ceaec`):

- Source run: `release\runtime\logs\svg_mb_control_output.csv`
  (session start `2026-05-26T10:14:00`, manifest config
  `sha256=036cda22e65c7f06f64f865556cf18771c86caa715b34f00a7acca489c093f06`,
  producer git hash `b396b53a94a9`).
- Per-channel `last_setpoint_pct` matched the then-configured floors
  within the PWM quantization step
  (channels `0/1/2/3/4/5` at
  `15.5/22.0/60.15/56.15/31.0/20.0`%).
- `low_band_evidence.json` reported `activation_count = 0` and
  `max_debt ≈ 8.7e-4`; the low-band path stayed below the per-channel
  debt thresholds for the duration of the capture, so the static floor uplift
  kept the integrated signal below activation under the observed
  Tctl/Tdie p50 of `~46.8 C` and GPU core p50 `~28 C`.
- `control_runtime.json` reported no open circuit breakers, no
  consecutive sensor or write failures, and
  `loop_slip_ms ≤ ~1.1 ms` against a `poll_tick_ms = 250` budget.
- No logging-quality regressions reproduced from the 2026-05-24 run
  (no malformed CSV rows, no Windows error 5 sidecar failures
  observed in `svg_mb_control_events.jsonl`).

The 2026-05-26 capture validates the identities in §8.2 (clipped
additive composition), §6 (integrator hold at zero with no rising
input), and §7 (debt does not accrue when `signal ≈ 0`) against that
reference profile, and confirms `loop_slip_ms`/`loop_overrun` invariants
for §10. The later dynamic low/medium intake profile changes config curve
points but does not change the mathematical control identity in this file.

2026-05-26 source-aware blend counterfactual:

- Decision record:
  `docs\source-aware-blend-decision-2026-05-26.md`.
- Existing current-config CSVs with config SHA256
  `036cda22e65c7f06f64f865556cf18771c86caa715b34f00a7acca489c093f06`
  were recomputed offline against `config\control.release.json`.
- The recomputed current feedforward matched CSV feedforward exactly
  (p95/max absolute error `0.000/0.000`), then compared guarded
  `max_cpu_gpu_source_aware` alternatives.
- A `75 C` CPU guard on channels `0`, `2`, `3`, and `4` had the best
  risk-adjusted result: average current-config reduction
  `1.93` total duty-points/tick, warm-row reduction `5.07`, and
  CPU-hot/GPU-cool reduction `0.00`. Unguarded source-aware blending was
  rejected because a historical CPU-heavy trace would have removed
  `68.83` duty-points/tick in CPU-hot/GPU-cool rows.

### 13.2 Per-run checks to keep current

For every real-data pass, use the active runtime manifest/status to identify
the CSV, event log, current state, config, build, and notes. Then verify at
least these identities:

- `channelN_correction_pct = channelN_setpoint_pct -
  channelN_feedforward_pct` for rows with both numeric values.
- `channelN_low_band_effective_boost_pct <=
  channelN_low_band_stage_boost_pct`, and if
  `low_band_residual_cap_pct` is configured, the effective value is also
  `<= low_band_residual_cap_pct`.
- `loop_intended_interval_ms` is inside `[poll_tick_floor_ms, poll_tick_ms]`
  and `cadence_transient` is inside `[0, 1]`.
- `channelN_setpoint_pct` stays inside `[0, 100]`.
- `channelN_primary_temp_source` is present for controlled channels and
  matches the configured blend path (`cpu`, `gpu`, `cpu_fallback`,
  `cpu_guard`, `gpu_guard`, or `unavailable`).
- Response sources in CSV/status match the active nonzero terms:
  `thermal_pressure`, `midband_pressure`, `gpu_airflow`, `cpu_low_soak`, and
  `low_band_stage`.

Update this section whenever the CSV schema, status schema, or analyzer output
changes enough that these checks are incomplete.

---

## 14. Cross-references

| Section | Source |
|---|---|
| §2 (per-tick orchestration) | `tick_runner.cpp:RunControlTick` (calls `UpdateLowBandState` before the per-channel loop, then `EvaluateChannel` + `TryApplyChannelSetpoint` per channel, then `ComputeCadence`, then `WaitForNextControlTick`) |
| §3   | `channel_evaluator.cpp:SelectPrimaryCurveInput`, `control_policy.cpp:BlendTemps`, `channel_evaluator.cpp:GpuControlEnvelopeC` |
| §4   | `control_policy.cpp:LookupCurve`, `control_policy.cpp:SmootherStep` |
| §5   | `channel_evaluator.cpp:ApplyDemandSmoothing` |
| §6.1–6.2 | `boost_stage.cpp:UpdateBoostStage` (BelowStart specs in `kBoostStageSpecs`: ThermalPressure, MidbandPressure, GpuAirflow) |
| §6.3 | `boost_stage.cpp:UpdateBoostStage` (ExplicitRelease spec: CpuLowSoak) |
| §7   | `low_band_integrator.cpp:UpdateLowBandState`, `control_math.cpp:SmoothScale` |
| §8.1 | `channel_evaluator.cpp:RateLimitSetpoint`, `control_math.cpp:MoveTowardRateLimited` |
| §8.2–8.3 | `channel_evaluator.cpp:EvaluateChannel` (final composition) |
| §9   | `channel_write.cpp:TryApplyChannelSetpoint`, `channel_evaluator.cpp:FanNeedsAuthorityReassert`, `WriteCooldownForAuthorityReassert` |
| §10  | `cadence_score.cpp:ComputeCadence`, `control_scheduler.cpp:WaitForNextControlTick`, `tick_runner.cpp` (CSV `loop_slip_ms` derivation) |
| §11  | `channel_evaluator.cpp:EvaluateChannel` (sensor safe-mode branch) |
