# Write-failure breaker half-open probe — Decision & Plan — 2026-06-17

**Status:** Current — decision settled 2026-06-17. Settles FEAT-0011 promotion
gate 3 and the §11 open decisions; authorizes the v1 implementation.
**Owns:** `docs/features/FEAT-0011-write-failure-breaker-rising-cooling-demand.md`
(`REQ-COOLWRITE-*`).
**Basis:** the static-verified gap at `src/control/channel_write.cpp:307` — an open
write-failure breaker suppresses every non-safety write, so a breaker opened by a
transient `ApplyDuty` failure burst cannot self-heal through a normal cooling write
once the actuator recovers; it clears only via a sensor-safe trip (sensor loss, not
high temp) or operator `--reset-breakers`. Corroborated by the 2026-06-17 external
review (Finding 5).

## 1. Context
The breaker correctly stops *futile* writes to a genuinely-failing actuator. But
the only auto-recovery is the breaker closing on a successful `safety_override`
write, which fires on **sensor loss**, not on high temperature. So a channel whose
actuator failed transiently (breaker opened) and then recovered, with a *working*
sensor, stays locked out and under-cooled until an operator resets it or the
SuperIO/EC ~95 °C hardware backstop intervenes.

## 2. Decisions

### D-COOLWRITE-1 — Bounded half-open probe, not a blanket bypass
While the breaker is open, suppress normal writes **except** a probe: when more
cooling is wanted and a backoff has elapsed, let one write through. A successful
probe closes the breaker; a failed one keeps it open.

*Rationale:* a blanket bypass (allow all rising-demand writes) would spam a
genuinely-failing actuator every tick, defeating the breaker's purpose. The
rate-limited probe recovers a *transient* failure without that cost. (The review
explicitly rejected a blanket bypass.)

### D-COOLWRITE-2 — Fixed 5 s backoff, hardcoded
At most one probe per `kBreakerProbeBackoff = 5 s` (`ChannelState`, hardcoded; no
config key in v1).

*Rationale:* 5 s bounds the under-cool-after-recovery window while costing ~1
futile write per 5 s against a still-failing actuator (negligible vs the ~4
writes/s the breaker is saving), and is comfortably inside fan/thermal time
constants. A per-channel config key is a later refinement.

### D-COOLWRITE-3 — "Cooling wanted" = setpoint above the last applied duty
A probe is eligible when the computed setpoint exceeds the last applied duty
(`last_issued_pct`), or on a first write. This generalizes the spec's "rising
demand" framing to also cover a hot-but-steady channel whose demand sits above the
last applied duty — both want their cooling restored.

### D-COOLWRITE-4 — Reuse the existing success/failure paths; safety unchanged
A successful probe runs the existing `NoteSuccessfulChannelWrite` (closes the
breaker, resets `consecutive_write_failures`, emits `circuit_breaker_closed`); a
failed probe runs the existing `HandleChannelWriteFailure` (breaker stays open).
The probe attempt emits an additive `control_loop.circuit_breaker_probe` event;
the outcome is the existing `circuit_breaker_closed` / `write_failed` event.
`safety_override` is unchanged — sensor-safe commands still bypass immediately,
above the probe logic.

## 3. Risks & mitigations
- **Persistently-failing actuator.** Probes keep failing; the channel stays
  under-cooled and falls to the EC ~95 °C hardware backstop. The probe heals
  *transient* failures only and is **not** a substitute for the backstop —
  accepted and recorded.
- **Probe spam.** Bounded by the 5 s backoff (`last_probe_time` is set before the
  write attempt, so a failed probe still waits the full backoff).
- **Normal-path / safety regression.** None: a breaker-closed channel and a
  `safety_override` command are unchanged; only the breaker-open non-safety path
  gains the bounded probe. No new published status field.

## 4. Rollback
Contained: revert the breaker-gate branch in `channel_write.cpp` and the two
additive `ChannelState` fields (`last_probe_time`, `kBreakerProbeBackoff`). No
schema/config migration.
