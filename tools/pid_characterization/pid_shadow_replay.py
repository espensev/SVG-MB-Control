#!/usr/bin/env python3
"""Offline PID shadow-replay for FEAT-0003 REQ-PROFILE-07 characterization.

Replays the PID control law over a recorded control-loop CSV (real curve-driven
telemetry) and compares the PID trajectory against the curve baseline the worker
actually commanded -- the "shadow-log comparison" decision D6 requires before a
channel's pid.allow_live may be set. This is a non-actuating, zero-risk capture:
nothing touches the live controller.

Fidelity: the PidStep / LookupCurve / RateLimitSetpoint ports below are validated
against the exact C++ unit-test vectors (tests/cpp/pid_controller_tests.cpp and
the curve/rate behaviour) by selftest(), which MUST pass before any replay runs.
"""
from __future__ import annotations

import argparse
import csv
import json
import math
import os
from dataclasses import dataclass, field

NAN = float("nan")


# ---- faithful ports of the shipped C++ math --------------------------------

def smoothstep(t: float) -> float:
    # src/control/control_math.cpp: clamp(t,0,1); t*t*t*((6t-15)t+10)
    t = min(1.0, max(0.0, t))
    return t * t * t * ((6.0 * t - 15.0) * t + 10.0)


def lookup_curve(curve, temp_c: float, min_floor_pct: float, shape: str) -> float:
    # src/policy/control_policy.cpp LookupCurve (+ smootherstep)
    if not curve:
        return max(0.0, min_floor_pct)
    if temp_c <= curve[0][0]:
        raw = curve[0][1]
    elif temp_c >= curve[-1][0]:
        raw = curve[-1][1]
    else:
        raw = curve[-1][1]
        for i in range(1, len(curve)):
            lo_t, lo_d = curve[i - 1]
            hi_t, hi_d = curve[i]
            if temp_c <= hi_t:
                span = hi_t - lo_t
                if span <= 0.0:
                    raw = hi_d
                else:
                    frac = (temp_c - lo_t) / span
                    if shape == "smootherstep":
                        frac = smoothstep(frac)
                    raw = lo_d + frac * (hi_d - lo_d)
                break
    floor = max(0.0, min_floor_pct)
    return min(max(raw, floor), 100.0)


def rate_limit_setpoint(desired, last, elapsed_ms, rise, fall, max_step):
    # src/control/channel_evaluator.cpp RateLimitSetpoint (post review fix:
    # the step cap and the directional rate are INDEPENDENT bounds).
    if math.isnan(last):
        return desired
    delta = desired - last
    if abs(delta) <= 0.0001:
        return desired
    rate = rise if delta > 0.0 else fall
    has_rate = not math.isnan(rate) and rate > 0.0
    has_step = not math.isnan(max_step) and max_step > 0.0
    if not has_rate and not has_step:
        return desired
    max_allowed = math.inf
    if has_rate:
        max_allowed = rate * (elapsed_ms / 60000.0)
    if has_step:
        max_allowed = min(max_allowed, max_step)
    if abs(delta) <= max_allowed:
        return desired
    return last + (max_allowed if delta > 0.0 else -max_allowed)


@dataclass
class PidState:
    integral: float = 0.0
    prev_temp_c: float = NAN
    has_prev: bool = False


@dataclass
class PidTerms:
    error_c: float = NAN
    p_term: float = 0.0
    i_term: float = 0.0
    d_term: float = 0.0
    raw_setpoint_pct: float = NAN


def pid_step(kp, ki, kd, bias, temp, target, dt_s, out_min, out_max,
             integral_min, integral_max, state: PidState) -> PidTerms:
    # src/control/pid_controller.cpp PidStep (exact).
    terms = PidTerms()
    if not math.isfinite(temp) or not math.isfinite(target):
        state.has_prev = False
        terms.raw_setpoint_pct = NAN
        return terms
    error = temp - target
    terms.error_c = error
    terms.p_term = kp * error

    d_term = 0.0
    if state.has_prev and dt_s > 0.0 and kd != 0.0:
        d_term = kd * (temp - state.prev_temp_c) / dt_s
    terms.d_term = d_term

    integrating = state.has_prev and dt_s > 0.0 and ki != 0.0
    candidate = state.integral
    if integrating:
        candidate = state.integral + error * dt_s
        if not math.isnan(integral_min) and candidate < integral_min:
            candidate = integral_min
        if not math.isnan(integral_max) and candidate > integral_max:
            candidate = integral_max
    raw = bias + terms.p_term + ki * candidate + d_term

    commit = True
    if integrating:
        if raw > out_max and error > 0.0:
            commit = False
        elif raw < out_min and error < 0.0:
            commit = False
    if commit:
        state.integral = candidate
    else:
        raw = bias + terms.p_term + ki * state.integral + d_term
    terms.i_term = ki * state.integral
    terms.raw_setpoint_pct = raw

    state.prev_temp_c = temp
    state.has_prev = True
    return terms


# ---- fidelity self-test against the C++ vectors ----------------------------

def _approx(a, b, eps=1e-9):
    return abs(a - b) <= eps


def selftest() -> None:
    fails = []

    def ck(cond, msg):
        if not cond:
            fails.append(msg)

    # Pure-P
    s = PidState()
    t = pid_step(2, 0, 0, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, NAN, NAN, s)
    ck(_approx(t.error_c, 10) and _approx(t.p_term, 20) and _approx(t.raw_setpoint_pct, 20),
       "pure-P")
    # Derivative sign (rising temp raises duty)
    s = PidState()
    a = pid_step(0, 0, 1, 30.0, 55.0, 50.0, 1.0, 0.0, 100.0, NAN, NAN, s)
    b = pid_step(0, 0, 1, 30.0, 60.0, 50.0, 1.0, 0.0, 100.0, NAN, NAN, s)
    ck(_approx(a.d_term, 0) and _approx(a.raw_setpoint_pct, 30), "deriv first step")
    ck(_approx(b.d_term, 5) and _approx(b.raw_setpoint_pct, 35) and b.raw_setpoint_pct > a.raw_setpoint_pct,
       "deriv sign +Kd")
    # Derivative-on-measurement, no kick on target change
    s = PidState()
    pid_step(0, 0, 1, 0.0, 50.0, 50.0, 1.0, 0.0, 100.0, NAN, NAN, s)
    t = pid_step(0, 0, 1, 0.0, 50.0, 40.0, 1.0, 0.0, 100.0, NAN, NAN, s)
    ck(_approx(t.d_term, 0), "no derivative kick on target change")
    # Integral accumulates
    s = PidState()
    s1 = pid_step(0, 0.5, 0, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, NAN, NAN, s)
    s2 = pid_step(0, 0.5, 0, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, NAN, NAN, s)
    s3 = pid_step(0, 0.5, 0, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, NAN, NAN, s)
    ck(_approx(s1.i_term, 0) and _approx(s2.i_term, 5) and _approx(s3.i_term, 10), "integral accumulates")
    # Anti-windup freeze
    s = PidState()
    pid_step(0, 1, 0, 95.0, 60.0, 50.0, 1.0, 0.0, 100.0, NAN, NAN, s)
    w2 = pid_step(0, 1, 0, 95.0, 60.0, 50.0, 1.0, 0.0, 100.0, NAN, NAN, s)
    w3 = pid_step(0, 1, 0, 95.0, 60.0, 50.0, 1.0, 0.0, 100.0, NAN, NAN, s)
    ck(_approx(w2.i_term, 0) and _approx(w3.i_term, 0), "anti-windup freeze")
    # Integral clamp
    s = PidState()
    pid_step(0, 1, 0, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, NAN, 15.0, s)
    c2 = pid_step(0, 1, 0, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, NAN, 15.0, s)
    c3 = pid_step(0, 1, 0, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, NAN, 15.0, s)
    c4 = pid_step(0, 1, 0, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, NAN, 15.0, s)
    ck(_approx(c2.i_term, 10) and _approx(c3.i_term, 15) and _approx(c4.i_term, 15), "integral clamp")
    # NaN measurement does not poison
    s = PidState()
    pid_step(1, 1, 1, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, NAN, NAN, s)
    pid_step(1, 1, 1, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, NAN, NAN, s)
    pid_step(1, 1, 1, 0.0, NAN, 50.0, 1.0, 0.0, 100.0, NAN, NAN, s)
    ck(math.isfinite(s.integral), "NaN guard keeps integral finite")
    g = pid_step(1, 1, 1, 0.0, 60.0, 50.0, 1.0, 0.0, 100.0, NAN, NAN, s)
    ck(math.isfinite(g.raw_setpoint_pct), "PID recovers after NaN")
    # RateLimit step cap independent of rates
    ck(_approx(rate_limit_setpoint(90, 30, 1000, NAN, NAN, 5), 35), "rl step-cap rise")
    ck(_approx(rate_limit_setpoint(0, 30, 1000, NAN, NAN, 5), 25), "rl step-cap fall")
    ck(_approx(rate_limit_setpoint(90, 30, 1000, NAN, NAN, NAN), 90), "rl no limit")
    ck(_approx(rate_limit_setpoint(90, NAN, 1000, NAN, NAN, 5), 90), "rl first write")
    ck(_approx(rate_limit_setpoint(90, 30, 60000, 30, 30, 5), 35), "rl rate+step")
    ck(_approx(rate_limit_setpoint(90, 30, 60000, 6, 6, NAN), 36), "rl rate only")
    # Smoothstep endpoints
    ck(_approx(smoothstep(0), 0) and _approx(smoothstep(1), 1) and _approx(smoothstep(0.5), 0.5),
       "smoothstep endpoints/midpoint")

    if fails:
        raise AssertionError("PID port fidelity self-test FAILED: " + "; ".join(fails))


# ---- CSV replay ------------------------------------------------------------

def read_csv_rows(path):
    with open(path, newline="") as fh:
        lines = fh.read().splitlines()
    hdr_idx = next(i for i, ln in enumerate(lines)
                   if "channel0_observed_temp_c" in ln)
    reader = csv.DictReader(lines[hdr_idx:])
    return list(reader), reader.fieldnames


def _f(row, key):
    v = row.get(key, "")
    if v is None or v == "":
        return NAN
    try:
        return float(v)
    except ValueError:
        return NAN


@dataclass
class ChannelPid:
    channel: int
    curve: list
    curve_shape: str
    min_duty_pct: float
    rise: float
    fall: float
    max_step: float
    target_c: float
    kp: float
    ki: float
    kd: float
    feedforward: str = "curve"          # "curve" | "fixed"
    fixed_feedforward_pct: float = NAN
    integral_min: float = NAN
    integral_max: float = NAN


def replay_channel(rows, cfg: ChannelPid, out_csv_path):
    state = PidState()
    last_issued = NAN
    tcol = f"channel{cfg.channel}_observed_temp_c"
    scol = f"channel{cfg.channel}_setpoint_pct"
    out_min = min(max(cfg.min_duty_pct, 0.0), 100.0)
    n = 0
    div = []           # pid - curve baseline
    pid_vals = []
    curve_vals = []
    max_abs_div = 0.0
    sat_high = 0
    sat_low = 0
    with open(out_csv_path, "w", newline="") as out:
        w = csv.writer(out)
        w.writerow(["row", "wall_clock", "dt_s", "observed_temp_c",
                    "curve_setpoint_pct", "pid_setpoint_pct", "divergence_pct",
                    "pid_error_c", "pid_p_term", "pid_i_term", "pid_d_term",
                    "pid_raw_pct"])
        for i, r in enumerate(rows):
            temp = _f(r, tcol)
            curve_sp = _f(r, scol)
            dt = _f(r, "loop_achieved_interval_ms")
            dt_s = 0.0 if math.isnan(dt) else dt / 1000.0
            dt_s = min(max(dt_s, 0.0), 5.0)
            if math.isnan(temp):
                continue
            if cfg.feedforward == "fixed":
                bias = cfg.fixed_feedforward_pct
            else:
                bias = lookup_curve(cfg.curve, temp, cfg.min_duty_pct, cfg.curve_shape)
            terms = pid_step(cfg.kp, cfg.ki, cfg.kd, bias, temp, cfg.target_c, dt_s,
                             out_min, 100.0, cfg.integral_min, cfg.integral_max, state)
            sp = min(max(terms.raw_setpoint_pct, out_min), 100.0)
            sp = rate_limit_setpoint(sp, last_issued, 0.0 if math.isnan(dt) else dt,
                                     cfg.rise, cfg.fall, cfg.max_step)
            last_issued = sp
            if sp >= 99.999:
                sat_high += 1
            if sp <= out_min + 1e-6:
                sat_low += 1
            n += 1
            if not math.isnan(curve_sp):
                d = sp - curve_sp
                div.append(d)
                curve_vals.append(curve_sp)
                pid_vals.append(sp)
                max_abs_div = max(max_abs_div, abs(d))
            w.writerow([i, r.get("wall_clock", ""), f"{dt_s:.3f}", f"{temp:.3f}",
                        "" if math.isnan(curve_sp) else f"{curve_sp:.3f}",
                        f"{sp:.3f}",
                        "" if math.isnan(curve_sp) else f"{sp - curve_sp:.3f}",
                        f"{terms.error_c:.3f}", f"{terms.p_term:.3f}",
                        f"{terms.i_term:.3f}", f"{terms.d_term:.3f}",
                        f"{terms.raw_setpoint_pct:.3f}"])
    mean_div = sum(div) / len(div) if div else NAN
    rms_div = math.sqrt(sum(d * d for d in div) / len(div)) if div else NAN
    return {
        "channel": cfg.channel,
        "rows": n,
        "target_c": cfg.target_c,
        "gains": {"kp": cfg.kp, "ki": cfg.ki, "kd": cfg.kd},
        "feedforward": cfg.feedforward,
        "curve_setpoint_range": [min(curve_vals), max(curve_vals)] if curve_vals else None,
        "pid_setpoint_range": [min(pid_vals), max(pid_vals)] if pid_vals else None,
        "divergence_pct": {
            "mean": mean_div, "rms": rms_div, "max_abs": max_abs_div,
            "min": min(div) if div else NAN, "max": max(div) if div else NAN,
        },
        "saturation": {"at_100_pct_rows": sat_high, "at_min_duty_rows": sat_low,
                       "frac_high": sat_high / n if n else NAN,
                       "frac_low": sat_low / n if n else NAN},
        "artifact": os.path.basename(out_csv_path),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--csv", required=True, help="recorded control-loop CSV")
    ap.add_argument("--config", required=True, help="control.release.json (curves)")
    ap.add_argument("--gains", required=True,
                    help="JSON: {channel: {target_c,kp,ki,kd,feedforward,...}}")
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--max-rows", type=int, default=0, help="0 = all")
    args = ap.parse_args()

    selftest()
    print("[ok] PID port fidelity self-test passed (matches C++ vectors)")

    cfgroot = json.load(open(args.config))
    chan_cfg = {c["channel"]: c for c in cfgroot["control_loop"]["channels"]}
    gains = json.load(open(args.gains))
    rows, _ = read_csv_rows(args.csv)
    if args.max_rows:
        rows = rows[: args.max_rows]
    os.makedirs(args.outdir, exist_ok=True)

    summary = {"source_csv": os.path.basename(args.csv), "rows": len(rows),
               "channels": []}
    for ch_str, g in gains.items():
        ch = int(ch_str)
        cc = chan_cfg[ch]
        cfg = ChannelPid(
            channel=ch,
            curve=[(p["temp_c"], p["duty_pct"]) for p in cc.get("curve", [])],
            curve_shape=cc.get("curve_shape", "linear"),
            min_duty_pct=cc.get("min_duty_pct", 0.0),
            rise=cc.get("rise_rate_pct_per_min", NAN),
            fall=cc.get("fall_rate_pct_per_min", NAN),
            max_step=cc.get("max_setpoint_step_pct", NAN),
            target_c=g["target_c"], kp=g["kp"], ki=g["ki"], kd=g.get("kd", 0.0),
            feedforward=g.get("feedforward", "curve"),
            fixed_feedforward_pct=g.get("fixed_feedforward_pct", NAN),
            integral_min=g.get("integral_min", NAN),
            integral_max=g.get("integral_max", NAN),
        )
        out_csv = os.path.join(args.outdir, f"pid_shadow_ch{ch}.csv")
        res = replay_channel(rows, cfg, out_csv)
        summary["channels"].append(res)
        d = res["divergence_pct"]
        print(f"[ch{ch}] target={cfg.target_c} kp={cfg.kp} ki={cfg.ki} kd={cfg.kd} "
              f"ff={cfg.feedforward} | div mean={d['mean']:.2f} rms={d['rms']:.2f} "
              f"max|.|={d['max_abs']:.2f} | sat hi={res['saturation']['frac_high']*100:.1f}% "
              f"lo={res['saturation']['frac_low']*100:.1f}%")

    sumpath = os.path.join(args.outdir, "pid_shadow_summary.json")
    json.dump(summary, open(sumpath, "w"), indent=2)
    print(f"[ok] wrote {sumpath} + per-channel artifacts to {args.outdir}")


if __name__ == "__main__":
    main()
