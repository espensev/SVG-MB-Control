# Write-Path Live-Validation Protocol & Disposition — 2026-06-18

Status: **method/protocol record (neutral).** This document defines how the four
shipped write-path safety features would be reproduced and detected on the live
controller, and records the **disposition** of live (`M`) evidence for each. It
draws no design conclusion. `AGENTS.md` §Feature Intake Gate applies: this is
characterization/observation method only — **no control-path code, no
`control.json` behavior change, no new schema.** `AGENTS.md` §Live Runtime Safety
governs execution: forcing a sidecar fault or corrupting a live sidecar and
observing fan writes is an explicit, operator-present live task.
**Companion to:** `docs/features/FEAT-0010..0013`, `docs/features/FEAT-0008-watchdog-hung-worker-recovery.md`
(the `M`-evidence recording model), `docs/TRACEABILITY.md`.

## 1. Why this exists, and what it is not

The write-path safety review (`FEAT-0010/0011/0012/0013`) closed on the
automated-test + review (`T`/`R`) bar. Every §10 requirement row for those four
specs verifies `T` or `T,R`; **none requires `M`** (manual runtime measurement).
Acceptance is therefore already met, and live (`M`) evidence is **supplementary
defense-in-depth**, not a closure gate.

- In scope: per-feature trigger → expected observable → pass criterion, the
  operator-observable surfaces, and an honest disposition (live-feasible,
  proxy-only, or `T`-only-closed) for each.
- Out of scope: any control-path code, any `control.json` change, any schema
  change, and the hardware thermal envelope (hardware-owned: SMU die-temp throttle
  + SuperIO/EC last-PWM fallback).

## 2. Operator-observable surfaces (the verdict signals)

All three are read-only and already shipped:

1. **NDJSON event log** — `release/runtime/logs/svg_mb_control_events.jsonl`.
2. **`control_runtime.json`** per-channel fields:
   `consecutive_sidecar_persist_failures`, `last_response_source`,
   `sensor_failed`, `circuit_breaker_open` (`src/runtime/runtime_status.cpp`).
3. **`--health` JSON** — `health_state` / `degraded_channel_count` /
   `sidecar_quarantined_present` (`src/runtime/runtime_health.cpp`).

Safety envelope for any live run: operator present; cool idle window; back up any
file before mutating it; bounded duration; the hardware backstops remain in
effect. Where a trigger cannot be
produced safely on the production path, use a **separate runtime-home** (not
`release\runtime`) with the simulated writer or injected inputs — the `FEAT-0008`
`NtSuspendProcess` proxy pattern.

## 3. Per-feature trigger → observe → pass, and disposition

### FEAT-0010 — sidecar persist fault must not veto the write
- **Disposition: live-feasible on the production path (low risk).** The fan is
  still commanded throughout; this is the safe-by-design path.
- Trigger: while the live worker runs, hold an exclusive handle on (or ACL-deny
  write to) `release/runtime/pending_writes.json` so the atomic write throws after
  its retries.
- Observe: `control_loop.sidecar_upsert_failed` in the event log;
  `consecutive_sidecar_persist_failures > 0` in `control_runtime.json`; `--health`
  `degraded`; **and the fan duty still tracks the computed setpoint**.
- Pass: write proceeds (duty tracks setpoint) + counter increments + health
  degrades + `circuit_breaker_open` stays false. Release the lock → counter resets.

### FEAT-0012 — corrupt sidecar is quarantined, startup proceeds
- **Disposition: live-feasible with an operator-gated controlled restart
  (moderate risk).** Fans hold last PWM across the brief restart; cool idle window,
  file backed up.
- Trigger: stop the worker; back up then overwrite
  `release/runtime/pending_writes.json` with malformed JSON; restart the worker.
- Observe: `reconcile.sidecar_quarantined` in the event log; a
  `pending_writes.json.corrupt` sibling preserving the original bytes;
  `sidecar_quarantined_present = true` with `health_state = degraded`; the control
  loop ticks (no relaunch-thrash).
- Pass: worker reaches the control loop (tick advances), corrupt bytes preserved
  under `.corrupt`, event emitted, health degraded.

### FEAT-0011 — half-open breaker probe (recovered-actuator self-heal)
- **Disposition: proxy-only, or `T`-only-closed — no safe production-path
  trigger.** Opening the breaker needs five real `ApplyDuty` failures, and while
  the breaker is open the channel parks at last duty (a real thermal risk on a live
  channel); there is no production fault-injection knob to fail `ApplyDuty`.
- Proxy route: run the controller against a separate runtime-home with the
  simulated writer to drive 5 failures → rising demand → probe (barely beyond the
  existing C++ test `TestRisingDemandProbesOpenBreaker`). Observe
  `control_loop.circuit_breaker_probe` at most once per 5 s, then the breaker
  closing on the recovered writer.
- Otherwise: accept `T`-only and record "not safely reproducible on the production
  cooling path".

### FEAT-0013 — source-aware CPU-dropout safe mode
- **Disposition: proxy-only, or `T`-only-closed — no safe production-path
  trigger.** The trigger needs the CPU composite to read NaN for three ticks while
  GPU stays available — a forced sensor fault on the live box. The *result* is
  fail-safe (fans → 100%, loud); the *trigger* is the risk.
- Proxy route: an injected-`TempInputs` harness or a separate runtime-home with a
  simulated CPU-read-NaN sequence (available → dropped ×3 → returned), mirroring
  `TestSourceAwareCpuDropoutTripsSafeMode`. Observe
  `last_response_source = source_aware_cpu_dropout_safe_mode`, `sensor_failed =
  true`, the `safety_override`-driven 100% duty, and recovery on CPU return.
- Otherwise: accept `T`-only and record the scope limit.

## 4. Live-run precondition

Live (`M`) runs on hardware are optional supplementary evidence, not closure gates
for these features. Run them only with an operator present, a cool idle window,
bounded duration, and a backup of any file the test mutates. Proxy validation
(FEAT-0011/0013) remains the preferred path when the production trigger would
create unnecessary thermal or operational risk.

## 5. Where `M`-evidence is recorded (when captured)

Follow the `FEAT-0008` §14 model exactly: append one `M` row to the owning spec's
§14 verification log with the live commit hash, local timestamp, the observed event
string(s), the observed `control_runtime.json` field value(s), and the observed
`--health` state; state the proxy scope limit (what the proxy does and does **not**
prove) where the proxy route is used; mirror the result in `docs/TRACEABILITY.md`
§3 in the same change (`AGENTS.md` §Change Checklist). A `TRACEABILITY` result of
`partial` is acceptable for FEAT-0011/0013 if the proxy route is taken and the
production-path `M` is recorded as not safely reproducible.

## 6. Disposition summary

| Feature | Live-M disposition | Closure status |
|---|---|---|
| FEAT-0010 | live-feasible on production path (low risk) | `T`/`R` met; `M` optional supplementary |
| FEAT-0012 | live-feasible, operator-gated restart (moderate risk) | `T`/`R` met; `M` optional supplementary |
| FEAT-0011 | proxy-only / `T`-only-closed (no safe production trigger) | `T`/`R` met; `M` not required |
| FEAT-0013 | proxy-only / `T`-only-closed (no safe production trigger) | `T`/`R` met; `M` not required |

This disposition closes the "live (M) validation" backlog item: it is decided
(supplementary, proxy-or-`T`-only for two of the four), not left as an open
obligation.
