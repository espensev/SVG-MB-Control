# PID Control Law — Mathematical Reference

Status: **current**, last verified 2026-06-21 against
`src/control/pid_controller.cpp` (`PidStep` + `PidController::Evaluate`),
`src/control/pid_controller.h`, and `tests/cpp/pid_controller_tests.cpp`.

This is the identity reference for the **PID control law** (FEAT-0003
`PidController`), the sibling to `CONTROL_PIPELINE_MATH.md`, which stays the
identity reference for the **curve/overlay law** (`CurveOverlayController`). Both
laws sit behind the per-channel `IChannelController` seam and share the same
primary-temperature selection, the `[min_duty, 100]` clamp, the safety slew cap,
and the law-agnostic write path; only the setpoint computation differs.

A live PID write crosses `docs/MEASUREMENT_GATE.md`. PID is shadow/dry-run by
default; the live path is gated by decision D6 (see §6). Keep this file in
lock-step with `pid_controller.cpp`, the FEAT-0003 spec, and
`docs/profile-hot-swap-decision-2026-06-03.md`.

---

## 1. Notation

| Symbol | Meaning | Source |
|---|---|---|
| $k$ | PID step index for a channel ($k = 0, 1, 2, \dots$) | per-channel eval |
| $T_k$ | primary control temperature, °C | `SelectPrimaryCurveInput(...).temp_c` |
| $\Delta\tau_k$ | clamped inter-evaluation interval, s | `timing.elapsed_since_last_evaluation_ms / 1000`, clamped to $[0, 5]$ |
| $\theta$ | per-channel target temperature, °C | `pid.target_c` |
| $e_k$ | control error, °C | $e_k = T_k - \theta$ |
| $K_p, K_i, K_d$ | gains ($\ge 0$) | `pid.kp / ki / kd` |
| $b_k$ | feed-forward bias, % | `pid.feedforward` (curve\|fixed) |
| $I_k$ | integral accumulator | `PidState::integral` |
| $p_k, i_k, d_k$ | per-term contributions, % | `PidTerms` |
| $\rho_k$ | raw PID setpoint, % | `PidTerms::raw_setpoint_pct` |
| $m^{(c)}$ | channel floor, % | `min_duty_pct` |
| $u_k$ | issued duty setpoint, % | `ChannelEvaluation::setpoint_pct` |

The error convention is **cooling-native**: $e_k = T_k - \theta$, so a channel
hotter than its target produces a positive error that drives **more** duty. All
gains are non-negative.

## 2. Primary temperature (shared with the curve law)

$T_k$ and its source label come from `SelectPrimaryCurveInput(temp_inputs,
config)` — the *same* selector the curve law uses (`temp_blend` = cpu_only /
gpu_only / max / source-aware, including the `source_aware_cpu_hot_guard_c`
guard). The PID law does not re-implement source selection.

If the primary source is unavailable, the law enters the shared **sensor-safe
mode**: $u_k = \texttt{kSafeModeFanDuty} = 100\%$, `safety_override = true`,
`response_source = "sensor_safe_mode"`, and the PID memory is frozen
($\texttt{has\_prev} \leftarrow \text{false}$). A live PID's safe command bypasses
the write-failure breaker via `safety_override`; a shadow PID still suppresses the
write.

## 3. Feed-forward bias (decision D3a)

$$
b_k =
\begin{cases}
\texttt{LookupCurve}(\text{curve}, T_k, m^{(c)}, \text{shape}) & \text{feedforward} = \text{curve} \\
\texttt{pid.fixed\_feedforward\_pct} & \text{feedforward} = \text{fixed}
\end{cases}
$$

Curve feed-forward makes the PID terms a correction on top of the existing
temperature→duty curve (which stays as shape/floor); fixed feed-forward makes a
standalone PID around a constant resting duty.

## 4. PID terms (`PidStep`)

**Proportional.** $p_k = K_p\, e_k$.

**Derivative — on measurement, positive sign (decision D3b, 2026-06-21 sign
reconciliation).**

$$
d_k =
\begin{cases}
K_d\, \dfrac{T_k - T_{k-1}}{\Delta\tau_k} & k \ge 1,\ \Delta\tau_k > 0,\ K_d \ne 0 \\
0 & \text{first step, } \Delta\tau_k \le 0, \text{ or } K_d = 0
\end{cases}
$$

The sign is **positive** because the error convention is $e = T - \theta$: a
rising temperature must *raise* duty (anticipatory cooling). Taking the derivative
on the measurement $T$ (not on $e$) means a live change of $\theta$ produces no
derivative kick. (The textbook $-K_d\,\dot T$ form applies to the opposite
convention $e = \theta - T$; see the decision doc D3b note.)

**Integral — clamp + conditional integration (decision D3c).** A tentative
accumulator is formed and bounded:

$$
I'_k = \operatorname{clamp}\!\big(I_{k-1} + e_k\,\Delta\tau_k,\; I_{\min},\; I_{\max}\big)
$$

(integration only for $k \ge 1$, $\Delta\tau_k > 0$, $K_i \ne 0$; a NaN bound = no
clamp on that side). The raw output is computed with $I'_k$; the step is then
**committed only if the output does not saturate against the bounds in the error's
direction**:

$$
I_k =
\begin{cases}
I_{k-1} & (\rho > 100 \wedge e_k > 0)\ \text{or}\ (\rho < m^{(c)} \wedge e_k < 0) \\
I'_k & \text{otherwise}
\end{cases}
\qquad i_k = K_i\, I_k
$$

Conditional integration freezes wind-up while saturated; the clamp is the
secondary hard bound.

**Raw setpoint.** $\rho_k = b_k + p_k + i_k + d_k$.

## 5. Output conditioning (shared safety floor)

The min-duty floor and ceiling, then the safety slew cap (the *same*
`RateLimitSetpoint` the curve law applies, decision D4):

$$
u^{*}_k = \operatorname{clamp}\!\big(\rho_k,\; \operatorname{clamp}(m^{(c)}, 0, 100),\; 100\big)
$$
$$
u_k = \texttt{RateLimitSetpoint}\big(u^{*}_k,\; \texttt{last\_issued\_pct},\; \texttt{elapsed\_since\_last\_write\_ms},\; \text{rise},\ \text{fall},\ \text{max\_step}\big)
$$

A mis-tuned PID therefore cannot step the duty faster than the configured slew
cap. The downstream write gates (deadband, cooldown, baseline, control-hold,
circuit breaker) are law-agnostic and apply identically.

## 6. Live / shadow gate (decision D6, REQ-PROFILE-07)

`PidController` is **shadow/dry-run** unless `PidLiveAuthorized(config)` holds,
which requires **all** of:

1. `pid.allow_live == true`, and
2. a positive non-NaN slew cap — `max_setpoint_step_pct > 0` **or** both
   `rise_rate_pct_per_min > 0` and `fall_rate_pct_per_min > 0`, and
3. a non-empty `pid.characterization_artifact` that **exists on disk**.

A shadow controller sets `ChannelEvaluation::write_suppressed`, so the write path
computes and logs the setpoint but never actuates. An unevidenced `allow_live` is
**downgraded** to shadow at construction (not a config-load failure); only a
malformed PID config (missing `target_c`, negative gain, fixed feed-forward
without `fixed_feedforward_pct`, `integral_min >= integral_max`) fails the load.
At startup the worker emits `control_loop.profile_applied` for a live PID (the
gate crossing) and `control_loop.pid_shadow` (with reason) for a shadow one.

## 7. Reported evidence (REQ-PROFILE-08)

Each tick, a PID channel records `controller_kind = "pid"`, `pid_error_c`
($e_k$), `pid_p_term`/`pid_i_term`/`pid_d_term` ($p_k, i_k, d_k$), and
`pid_setpoint_raw_pct` ($\rho_k$) on `ChannelState`, surfaced additively in the
control-loop CSV (`channel<N>_pid_*`) and JSON status. Curve-only fields
(`feedforward_pct`, `correction_pct`) blank for a PID channel because the PID law
leaves `last_raw_demand_pct` NaN; the `pid_*` fields are null/blank for a curve
channel.

## 8. dt robustness

$\Delta\tau_k$ is clamped to $[0, 5]$ s (`kPidMaxDtSeconds`) before the integral
and derivative terms, so a single multi-second control-loop stall (a documented
environmental hazard) cannot produce a giant integral jump or derivative spike
when PID drives the loop live.
