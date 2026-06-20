# Multi-machine profiles + restart-based profile switch — decision — 2026-06-20

Status: Current. Design decision record for
`docs/features/FEAT-0023-machine-profiles-and-restart-switch.md`
(namespace `REQ-MPROFILE-*`).

**Companion to:** `docs/features/FEAT-0023-machine-profiles-and-restart-switch.md`,
`docs/features/FEAT-0003-selectable-profile-hot-swap.md`,
`docs/fan-restart-restore-and-plant-model-measurement-2026-06-20.md`,
`docs/CONTROL_LOOP.md`, `docs/WRITE_ORCHESTRATION.md`,
`docs/MEASUREMENT_GATE.md`, `docs/RUNTIME_HOME.md`.

## Problem

The controller loads exactly one `--config` file, baked once into the worker
command line by the supervisor (`control_supervisor.cpp:553`, built from the
captured `config_source_path` at `:542`/`:732`), and re-read by each spawned
worker at startup (`app_main.cpp:82-93`, `:340-344`). Changing control behavior
means editing that file or restarting with a different `--config`. There is no
named-profile concept for the control loop (`--profile` exists only on the
`analyze report` subcommand), no machine-identity resolution anywhere in the
worker (`service_probe.cpp`, `env_util.cpp`, `runtime_util.cpp` read no host
name), and the per-machine `config/machines/*.cooling.policy.json` is pure
documentation that the worker never loads (it is consumed only by Python tests
and analysis scripts).

The controller is being deployed across several machines / machine types. Each
rig has a different fan inventory and different curves, and an operator wants to
select among behavior profiles (for example a quieter or a more aggressive
profile) on a running machine without hand-editing config.

## Decisions

### D-MPROFILE-1 — Switch by restarting the worker, not an in-process swap

A profile switch is delivered by restarting the control-loop worker into a
different active profile, reusing the existing supervised-restart machinery, not
by an in-process tick-boundary swap. The active law and config are fixed for a
worker's lifetime; you change them by cycling the worker.

Rationale: the supervisor already re-reads the config on every worker spawn, so a
restart naturally picks up a new profile. Restart-based switching removes the
entire in-process live-swap apparatus that `FEAT-0003` would otherwise need
(tick-boundary request intake, control-law dynamic-state transition semantics,
build-then-validate-then-swap on the live loop). It does **not** remove the
measurement-gate obligations for a live non-curve write — those are config-load
preconditions that apply at worker startup regardless (see D-MPROFILE-6).

### D-MPROFILE-2 — Accept the BIOS-auto gap; do not latch (no new fan watchdog)

The switch uses the existing graceful-restore shutdown path. During the
~1–2 s worker gap the fans revert to BIOS SmartFan auto (the worker restores the
captured baseline duty **and** mode register on graceful exit,
`control_loop.cpp:236-241` → `fan_sio.cpp:925-935`). This feature does **not**
add a no-restore duty-latch path and does **not** add a fan-safety watchdog.

Rationale and evidence: a graceful restart is not acoustically seamless under
load — measured 2026-06-20, a graceful `--stop` drove BIOS-auto RPM up to ~2.5×
the controller's setpoint on the rear exhaust and ~1.9× on a radiator exhaust
(`docs/fan-restart-restore-and-plant-model-measurement-2026-06-20.md`, Finding 1).
A true-seamless switch would require suppressing restore and latching the last
duty, which then needs an out-of-process fan-safety backstop because a latch plus
a failed relaunch leaves fans frozen with no controller and no BIOS curve
(`restore_on_exit=false`, `sio_fan_writer.cpp:94`). The maintainer chose to
**accept the gap**: the switch reverts to BIOS auto (a safe, BIOS-managed state)
during the gap and is near-silent only near idle. This is the simpler and the
**safer-failure** choice — a failed switch leaves fans on the BIOS curve, not
frozen — and removes the latch and watchdog work entirely. Making the switch
acoustically seamless is explicitly out of scope and would be a separate,
measurement-gated change.

### D-MPROFILE-3 — Two-axis profile model: machine-base + behavior-overlay

A profile is a composition of a machine-base and a behavior-overlay, resolved at
load into the existing `ControlConfig`/`ControlLoopConfig` shape:

- **Machine-base** carries the physical identity already described by
  `config/machines/*.cooling.policy.json` (board, case, fan models/diameters,
  channel roles/directions, intake/exhaust grouping, excluded AIO ch6). This
  schema (`svg_mb_control.machine_cooling_policy.v1`) is promoted from
  documentation to a loaded input.
- **Behavior-overlay** carries the tunable control behavior (the `control_loop`
  curves, boosts, cadence) and is shareable across machines by role/channel.

`config/control.release.json` remains the baked default composition and the
fallback. The raw policy JSON is not itself the profile: it has no `control_loop`
runtime fields and is never parsed by the worker today.

Rationale: only the control config has runtime authority. Behavior tuning is
currently duplicated across `control.release.json` and the policy JSON and pinned
equal by `tests/test_config_contracts.py`; a catalog must split the machine
substrate from the behavior so the two stop being hand-duplicated. The
composition keeps that pin.

### D-MPROFILE-4 — Identity resolution + precedence

When no explicit config or profile is given, the controller resolves a profile
from machine identity (host name via `GetComputerNameW`, plus an optional
`runtime_home/machine_id.txt` override), falling back to a built-in default when
identity is unreadable or not in the catalog. Precedence, highest first:
`--config <file>` → `--profile <name>` / `SVG_MB_PROFILE` → machine identity →
built-in default. Resolution slots into the existing config-path chain at
`app_main.cpp:69-94`, ahead of `LoadControlConfig`, so an explicit `--config`
always wins and the no-catalog path is unchanged.

### D-MPROFILE-5 — Supervisor-owned switch: request, validate, zero-backoff cycle, auto-revert

The switch is owned by the supervisor loop, not the tick runner (the breaker-reset
request is consumed by the worker; a switch must be consumed by the supervisor
because it is delivered by restarting the worker):

1. **Request:** a supervisor-consumed, take-once `profile.switch.request.json`
   (modeled on the breaker-reset Request/Take/Clear trio,
   `runtime_lifecycle.cpp:40-103`) names the target profile, written by an
   explicit operator subcommand parallel to `--reset-breakers`
   (`app_main.cpp:164-184`). An active-profile pointer the loop re-resolves each
   iteration replaces the captured-once `config_source_path`, so revert is atomic.
2. **Validate before activate:** the supervisor load-validates the candidate at
   the top of the loop; on failure it keeps the running worker, emits a rejection
   event, and clears the request without cycling.
3. **Graceful worker cycle (no crash backoff):** on a valid candidate, repoint the
   pointer and gracefully stop the current worker so its shutdown restore runs
   (fans revert to BIOS auto) — a worker-scoped cooperative stop, distinct from the
   global stop request that also ends the supervisor — escalating to
   force-terminate only if the worker does not stop within the stop timeout (reuse
   the FEAT-0008 `EscalateForceTerminate` hung-worker path). Then respawn on the new
   profile, skipping the crash backoff and not incrementing the crash
   `restart_count`, so the intentional cycle is distinguished from a crash. The
   no-backoff bound is the crash backoff only, not the graceful-stop wait. This
   keeps the gap on the graceful-restore path (D-MPROFILE-2): fans revert to BIOS
   auto, not a frozen latch.
4. **Auto-revert to last-known-good:** if the post-switch worker fails startup,
   repoint to the last profile that produced a worker which survived startup and
   respawn from it; reset `restart_count` once a worker survives startup; remove
   the current first-spawn early-return so an operator-switch failure self-heals
   instead of killing the supervisor.

**Known limit (recorded, not solved here):** supervisor-side validation is
parse-only (`control_config.cpp:119-129` does JSON parse + file-exists + field
rejection, no hardware bind); the hardware bind happens in the spawned worker.
A profile that parses cleanly but fails or hangs on hardware bind is not caught
by validation, and the auto-revert branch only fires for worker exits within the
~1500 ms startup window (`control_supervisor.cpp:576-579`); a profile that hangs
or exits slow bypasses it. Because D-MPROFILE-2 accepts the BIOS-auto gap, the
failure mode during such a window is "fans on BIOS auto" (safe), not "fans
frozen"; tightening this validation/revert window is tracked as an open decision
in the spec, not built in the first slice.

### D-MPROFILE-6 — The control-law / PID seam stays FEAT-0003, restart-selected, sequenced after

The per-channel control-law seam (`IChannelController` + a non-curve law such as
PID) is **not** part of this feature. It remains `FEAT-0003`, re-scoped to be
selected at startup from a profile's config under this restart-based switching
(law fixed per worker lifetime) rather than swapped in-process. Restart-selection
drops `FEAT-0003`'s live-swap parts (D5 dynamic-state reset, D7 tick-boundary
apply order) but keeps the full `docs/MEASUREMENT_GATE.md` posture for a live
non-curve write: shadow/dry-run first, characterization evidence plus a non-NaN
positive slew cap, both enforced as config-load preconditions (`FEAT-0003` D6).
The slew cap still defaults to NaN/off in code (`control_loop.h:29-31`,
`channel_evaluator.cpp:55-57`), so that precondition remains load-bearing.
`FEAT-0003` is sequenced **after** this feature ships and the profile catalog +
restart switch are validated on hardware.

## Consequences

- The supervisor gains a switch path (request intake, pointer re-resolution,
  graceful worker stop with a force-terminate-on-timeout fallback,
  no-crash-backoff respawn, auto-revert, `restart_count` reset). This is new
  supervisor code, not free reuse.
- The startup config-path chain gains profile/identity resolution; the no-catalog
  path and an explicit `--config` are unchanged.
- The machine policy schema becomes a loaded input; the
  `tests/test_config_contracts.py` pin between the resolved profile and its
  machine-base is preserved.
- Runtime status and the standard control-loop CSV gain an additive
  active-profile-name + resolution-source field; a switch emits applied / rejected
  / reverted events. These are observational and do not feed control.
- `FEAT-0003` is reopened from "not a net benefit / not scheduled" to a planned
  later phase, restart-selected; its measurement-gate posture is unchanged.

## Verification

- `.\scripts\Test-LocalCI.ps1`: profile composition/resolution tests (default
  profile reproduces current behavior; precedence; identity fallback); supervisor
  switch tests (validate-before-activate rejects a bad candidate and keeps the
  worker; zero-backoff cycle; auto-revert to last-known-good); status/CSV
  active-profile field + switch events; review that the fields/events are
  observational.
- Code review vs `docs/CONTROL_LOOP.md`, `docs/WRITE_ORCHESTRATION.md`,
  `docs/MEASUREMENT_GATE.md`, and this decision: switch owned by the supervisor;
  default profile preserves the shipped cadence/channel set/control identity; the
  switch uses the existing graceful restore (no latch).
- Runtime evidence (respecting `AGENTS.md` §Live Runtime Safety) that a switch
  cycles the worker into a new profile and that a bad profile auto-reverts to
  last-known-good.
