# Modular Profile Hot-Swap — Deepening Plan (design-capture) — 2026-06-06

**Project:** svg-mb-control
**Status:** design-capture only — deepens `docs/features/FEAT-0003-selectable-profile-hot-swap.md`
**Version:** 0.1   **Updated:** 2026-06-06
**Companion to:** `docs/features/FEAT-0003-selectable-profile-hot-swap.md`,
`docs/profile-hot-swap-decision-2026-06-03.md`,
`docs/features/FEAT-0001-hot-swap-write-policy.md`,
`docs/CONTROL_LOOP.md`, `docs/CONTROL_PIPELINE_MATH.md`,
`docs/WRITE_ORCHESTRATION.md`, `docs/RUNTIME_HOME.md`,
`docs/MEASUREMENT_GATE.md`, `docs/STRUCTURE_AND_STABILITY.md`, `AGENTS.md`
**Purpose:** carry the "change the entire per-channel control function at runtime"
idea one level deeper than FEAT-0003 — specifically the **modular** angle: a
control-law *registry* and a *named-profile* surface — and record the grounded
current state and the open maintainer questions the existing D1–D7 decisions do
not settle. This adds clearly-labeled **proposals** only; it does not authorize
work and does not change FEAT-0003's status.

## 0. Status guard (read first)

FEAT-0003 carries a recorded maintainer decision. This document does **not**
rescind, weaken, or supersede it. Quoted (with one marked elision) from
`docs/features/FEAT-0003-selectable-profile-hot-swap.md:16-23`:

> **Scope & intent (2026-06-03).** This spec is design-capture, not scheduled
> work. Per the maintainer, the feature is not believed to be a net benefit and
> is not planned. PID is recorded as the worked **example of the range** of
> control laws the per-channel seam must be able to host … Any implementation
> would be a demonstration.

The same status is recorded in `docs/profile-hot-swap-decision-2026-06-03.md:3-8`
and in the 2026-06-03 **Idea** row of `docs/PATH_NOTES.md` ("FEAT-0003 is
design-capture … not scheduled work"; PATH_NOTES is a frequently-appended
journal, so it is cited by dated entry rather than line number). Accordingly:

- Everything in §3, §4, and §6 below is a **proposal** or a **design-capture of a
  demonstration's structure**, not authorized work and not a backlog to start.
- The directional decisions **D1–D7** in `docs/profile-hot-swap-decision-2026-06-03.md`
  remain the architecture of record. This document **extends** two seams those
  decisions left open (the law-dispatch mechanism behind D1; the profile object
  and startup surface around D7); it does not re-decide D1–D7.
- Per `CLAUDE.md` doctrine, current behavior is stated with `is`/`was` and a
  file:line; proposed behavior is stated with `would`/`could` and labeled.

This document also corrects file:line references in the two 2026-06-03 docs that
have since drifted (§8); those corrections are accuracy-only and change no
decision.

## 1. What we have today (look first)

The per-channel control "function" is the single free function **`EvaluateChannel`**
(`src/control/channel_evaluator.cpp:440-474`). It is a pure input→output
orchestration over file-private helpers with no I/O, which is why D1 frames
wrapping it as a controller as a *move, not a rewrite*
(`docs/profile-hot-swap-decision-2026-06-03.md:72-80`).

**Inputs / output.** `EvaluateChannel` takes a mutable `ChannelState&` (per-channel
config plus every dynamic-state field), `const ControlLoopConfig&`, a
`const TempInputs&`, a `const RuntimeSnapshotIndex&`, and a steady-clock `now`
(`channel_evaluator.h:56-60`). It returns a `ChannelEvaluation` by value
(`channel_evaluator.h:30-47`) — the law-agnostic output contract: `has_setpoint`,
`setpoint_pct`, `observed_temp_c`, `response_source`, the authority-reassert
flag/detail, the sensor-event fields, and `safety_override` (set only on the
sensor-safe forced-100% command; the write path lets that one command bypass the
write-failure circuit breaker — `channel_evaluator.h:41-46`).

**Stages, in tick order** (all in `channel_evaluator.cpp`):

| Stage | What it does | Location |
|---|---|---|
| Input select + primary curve + sensor-failure latch | `SelectPrimaryCurveInput` switches on `config.temp_blend` (incl. the source-aware CPU-hot guard); at 3 consecutive failures forces 100% (`sensor_safe_mode`, `safety_override=true`) | `:198-237`, `:243-283`, sensor-safe `:248-264` |
| CPU override curve | strict max-wins overlay; cannot lower demand | `ApplyCpuOverride`, `:287-301` |
| Demand smoothing | asymmetric rise/fall EMA + decay-latch floor | `ApplyDemandSmoothing`, `:72-117` |
| Four boost overlays | thermal-pressure, midband-pressure, gpu-airflow, cpu-low-soak leaky integrators | `UpdateDemandAndBoosts`, `:307-337` |
| Low-band cap + final sum + clamp | caps the low-band residual, sums the base + four boosts + residual, clamps to `[0,100]` | `ComputeFinalSetpoint`, `:344-364` |
| Rate limiter | bounds the move from `last_issued_pct` by per-minute rise/fall + `max_setpoint_step_pct` | `RateLimitSetpoint`, `:37-70` |
| Authority reassert | flags drift between last-issued and observed fan state (continuous-hold mode only) | `DetectAuthorityReassert`, `:378-391` |

The boost-sum operand order is intentionally fixed for bit-identical output:
`std::clamp(smoothed_base + ThermalPressure + MidbandPressure + GpuAirflow +
CpuLowSoak + low_band_contrib, 0.0, 100.0)` at `:353-364`, with a load-bearing
comment citing `CONTROL_PIPELINE_MATH.md §5`.

**Where state lives.** `ChannelState` (`src/control/control_runtime_context.h:17-84`)
holds the channel `config` *and* the curve law's dynamic state in one struct:
`last_raw_demand_pct` (`:31`), `smoothed_demand_pct` (`:32`), `boosts[]` (`:35`),
and the `low_band_*` accumulator block (`:53-69`). The context constructor copies
each `loop.channels[i]` into `ChannelState.config`
(`control_runtime_context.cpp:17-21`). One `std::vector<ChannelState>` holds one
entry per channel. `EvaluateChannel` mutates the curve-law fields plus reporting
fields; it does **not** touch the write/breaker/baseline fields (`write_active`,
`hold_deadline`, `circuit_breaker_open`, `baseline_*`), which belong to the write
path (`channel_write.cpp`).

**Single call site.** The tick body calls the law at exactly one place:
`tick_runner.cpp:241-243`. There is no interface and no dispatch, so the law's
*numbers* can change via config but its *kind* cannot change at all — at startup
or at runtime. Profile selection is restart-only: the supervisor bakes one
`--config <path>` into the worker command line (`control_supervisor.cpp:259-270`,
key at `:267-268`) and re-bakes the same path on every restart; the control
worker's CLI parser (`src/app/app_args.cpp`, `--config` at `:129`) has no
`--profile` flag — the only `--profile` in the repo is the offline analyzer's
report filter (`src/analyze/analyze_cli.cpp:294`) — and there is no `controller` /
`control_law` discriminator key anywhere in config parsing.

This is the gap FEAT-0003 §2 names: "the control law is hard-coded" with a single
call site and no seam.

## 2. What "change the entire function" means against this code

The unit that would swap is the whole per-channel law — the entire
`EvaluateChannel` computation — behind the `IChannelController` seam D1 defines
(`docs/profile-hot-swap-decision-2026-06-03.md:51-69`). The seam would sit at a
single call site over an already law-agnostic output contract, because:

- `ChannelEvaluation` (`channel_evaluator.h:30-47`) is already a law-agnostic
  output contract — a second law would fill in the same struct, so the tick body
  and every downstream consumer (write gate, status, CSV) would be unchanged.
- The single call site (`tick_runner.cpp:241`) is the only place a virtual call
  would replace a direct call.

Two facts make this more than swapping a function pointer, and they are the reason
the "modular" framing needs the two deepenings in §3–§4 rather than just D1:

1. **Config and dynamic state are welded** (`control_runtime_context.h:17-84`).
   A genuinely different law (e.g. a PID integral/derivative accumulator) has no
   field to live in, and there is no defined seam for which `ChannelState` fields
   carry over vs reset. D2 already chose "controller owns its state, slim
   `ChannelState` to law-agnostic fields"
   (`docs/profile-hot-swap-decision-2026-06-03.md:86-107`) — so "change the
   function" requires a state-ownership refactor first, not an additive drop-in.
2. **Output conditioning is inline in the curve law, not separated.** Sensor-safe
   (`:248-264`), EMA smoothing + decay-latch (`:72-117`), and the rate limiter
   (`:37-70`) all sit inside `channel_evaluator.cpp`. D4's **firm** law-agnostic
   list is the clamp, sensor-safe, deadband, cooldown, breaker, and write gate
   (`docs/profile-hot-swap-decision-2026-06-03.md:141-145`). Two further
   dispositions are recorded there only as **open leans** (`:147-153`): the EMA
   smoothing + decay latch lean curve-law-specific, and the rise/fall/step rate
   limiter leans shared as a safety slew clamp. Either way that split is **not
   drawn in code today**. So hosting a second law would first require extracting
   the shared conditioning out of the curve function.

The consequence (captured as a proposal in §6): the real first slice of "change
the entire function" would be a **seam-extraction refactor of the existing curve
law** that produces bit-identical output, not a parallel second module added
beside it.

## 3. Proposal A — how the law kind is selected (deepens D1)

D1 fixes the abstraction (`IChannelController` with `Evaluate` / `Reset` /
`Kind`) but does not settle **how a per-channel `controller` discriminator maps to
a controller instance**. The config loader has no discriminator dispatch today:
`LoadChannelConfig` (`control_loop_config.cpp:407-472`) is a flat, monomorphic
parse that reads every field unconditionally. The only string-keyed discriminator
precedent is the throw-on-unknown enum parse — `ParseCurveShape` (called at
`control_loop_config.cpp:423-426`) and `ParseTempBlend` (`:462-465`). A separate
precedent for **table-driven extensibility** (adding a behavior by adding a table
row rather than editing several functions) is the boost-stage spec table iterated
by enum index in validation (`kBoostStageSpecs`, `control_loop_config.cpp:258-260`);
it is a fixed array indexed by enum, not a string-keyed selector, so it models the
registry's *extend-by-row* shape, not its string dispatch.

Two shapes are available; both are net-new because no law dispatch exists yet.

**Option A1 — hard-named enum + switch.** A `controller` string would parse to a
`ControllerKind` enum throw-on-unknown (mirroring `ParseCurveShape`), and a
`switch` at construction would build the matching controller. Adding a third law
would touch the enum, the construction switch, and one per-law config branch.

- Touches three named sites and matches the `ParseCurveShape` parse precedent
  exactly.
- Covers the FEAT-0003 demonstration, which names exactly two kinds
  (`curve_overlay`, `pid`).

**Option A2 — registry / factory table.** A registration table would map a
`controller` string → `{ config-parser, factory }`. Adding a law would register one
table row plus its config parser; the loader, the call site, and the other
controllers would be untouched.

- This is the literal "modular" reading: a new control function would be added as a
  registered table row rather than through edits in the enum, switch, and parse
  branch.
- Cost: one indirection layer (the table plus a registration unit) that earns its
  keep only once the number of laws exceeds the two-law demonstration, or
  out-of-file registration is wanted.

**Recommendation (proposal).** Define the seam so the *choice between A1 and A2
stays internal and reversible*: keep the `controller` key and the controller
interface fixed, and treat the dispatch (switch vs table) as a private detail behind
a single `MakeChannelController(const ChannelControlConfig&)` factory. Start a
demonstration with A1 (three named sites, matches the parse precedent); move to A2
only if a third law or external registration is wanted. Because the call site
(`tick_runner.cpp:241`), `ChannelEvaluation`, and the controllers would not depend
on which dispatch is used, switching A1→A2 later would touch one factory function.
This keeps "modular" available without paying for the indirection before there is a
second law to justify it.

This recommendation does not change D1; it fills the dispatch gap D1 left open.

## 4. Proposal B — what a "profile" is, and the whole-profile swap (deepens D7)

D7 fixes the apply order for a profile change (validate the candidate via
`LoadControlLoopConfig` → channel-set delta → build + reset + swap at the tick
boundary; `docs/profile-hot-swap-decision-2026-06-03.md:184-204`). It does **not**
pin what a *profile* is as an operator object, nor the startup selection surface.
FEAT-0003 §7/§8 float both a `runtime/requests/profile.json` request and an
optional `--profile <name>` selector but leave the form open.

**"Whole-profile swap" would rebuild the entire per-channel controller set at one
tick boundary.** "Change the entire function" generalizes from one channel to all
of them: a profile would name a control law plus parameters *per channel*, and
applying it would rebuild every channel's controller and swap `context.loop` +
each `context.channels[i].config` + each controller together, between ticks, so no
tick runs a half-applied profile. To stay atomic under a mid-build failure, the new
controllers would be built into a staging vector and swapped in only after all
construct successfully (an open implementation constraint: D7 validates the config
first, but controller construction can still throw). This would reuse the
single-threaded, taken-once-per-tick intake the breaker reset already uses (§5).

Three forms a "profile" could take (proposals):

- **B-form 1 — inline full config.** The request would carry a full `control_loop`
  config body; the swap would validate it with `LoadControlLoopConfig` and apply
  it. It carries the full config body, so the operator hand-authors or generates
  it per change; no on-disk resolver is needed.
- **B-form 2 — named on-disk profile (recommended).** Profiles would live as named
  config files (for example `config/profiles/<name>.json`); the request would be a
  thin `{ "profile": "<name>" }`, and an optional startup `--profile <name>` would
  resolve the same name at boot. This matches the existing `--config` precedent (a
  path baked once, `control_supervisor.cpp:259-270`) and makes the named file the
  durable source of record, with the request reduced to the profile name.
- **B-form 3 — delta/overlay.** The request would carry only the fields that differ
  from the running config. It carries the fewest fields, but would need a
  merge-and-revalidate layer that does not exist and would blur "what is the active
  profile" — not recommended for a demonstration.

**Recommendation (proposal).** Use B-form 2 as the durable surface (named on-disk
profiles + `--profile` at startup, the same name in the request file), and allow
B-form 1 (inline body) as an escape hatch for ad-hoc changes. Both would validate
the candidate through the same `LoadControlLoopConfig` path D7 already names, so
there would be one validation gate regardless of form. Persisting a *live* change
back to a profile file stays out of scope (FEAT-0003 §3 non-goals; in-memory until
restart).

**Restart-revert risk (proposal).** Because the supervisor re-bakes the startup
`--config` path on every worker restart (`control_supervisor.cpp:259-270`) and a
live profile change would be in-memory only, an unattended supervisor auto-restart
(for example crash recovery) would silently revert an active hot-swapped profile to
the baked `--config`, with no operator signal. A demonstration would need to either
accept and document that revert-on-restart behavior, or have the swap rewrite the
baked `--profile`/`--config` selection and emit a startup event naming the
active-vs-baked profile.

This recommendation does not change D7's apply order; it names the profile object
and the startup surface D7 assumed.

## 5. Reusable hot-swap machinery (implemented vs spec)

A whole-profile swap reuses the breaker-reset request-intake pattern, which **is
implemented today**, and the FEAT-0001 build-then-swap choreography, which **is
specified but not yet implemented**.

**Implemented — the request-intake template** (the breaker reset both specs copy):

- Request file: `circuit_breaker_reset.request.json` at the runtime-home root,
  resolved by `RuntimeBreakerResetRequestPath` (`runtime_lifecycle.cpp:20-23`).
- Take-once-then-clear: `TakeRuntimeBreakerResetRequest`
  (`runtime_lifecycle.cpp:55-103`) parses then unconditionally clears the file at
  `:101` — a parse failure also deletes the file and is surfaced, not retried
  (single-shot).
- Tick consumption: `ProcessCircuitBreakerResetRequest(...)` is called exactly
  once at `tick_runner.cpp:195`, after `SampleDirectRuntimeSnapshot` and before
  the per-channel loop. The tick is straight-line single-threaded, so a request
  read at `:195` cannot interleave with an in-flight write.
- A profile intake would mirror this exactly: a profile request consumed once at
  the same point, before the per-channel loop rebuilds controllers. One caveat the
  template carries: the breaker-reset request is written atomically
  (`TryWriteJsonFileAtomic`, `runtime_lifecycle.cpp:51`), but a hand-dropped
  B-form-2 profile request has no such guarantee — combined with the
  take-once-delete-on-parse-error behavior, a torn write would be consumed and
  silently lost. A demonstration would either specify an atomic operator-write path
  or accept torn-write-consumed-and-lost (open question §7-5).

**Spec-only — FEAT-0001 build-then-swap** (Accepted, authorized, not yet built;
`docs/features/FEAT-0001-hot-swap-write-policy.md:4,85,235-252`). Its restore /
capture choreography is what a profile swap reuses for a **channel-set change**
(REQ-PROFILE-09): for dropped channels, restore baseline and clear `write_active`
via the same path as `HandleExpiredHoldRestore` (`channel_write.cpp:186-257`).
Because FEAT-0001 is not implemented, REQ-PROFILE-09 already says channel-set
changes are out of scope until it ships, and requires a swap to reject a candidate
whose channel set differs (`docs/features/FEAT-0003-selectable-profile-hot-swap.md:198`).

**Primitives that exist vs are missing** (for the §6 ordering):

| Primitive | Status | Location |
|---|---|---|
| `CreateFanWriter(runtime_policy)` factory | implemented | `fan_writer.h:135` |
| `HandleExpiredHoldRestore` → restore + clear `write_active` | implemented | `channel_write.cpp:186-257` |
| breaker-reset request-intake template | implemented | `runtime_lifecycle.cpp:55-103`, `tick_runner.cpp:195` |
| `IChannelController` seam / `MakeChannelController` factory | missing | — |
| per-channel `controller` discriminator + per-law parse | missing | — |
| profile request file + `TakeRuntimeProfileRequest` | missing | — |
| FEAT-0001 write-policy swap (needed for channel-set changes) | spec-only | — |

## 6. Dependency order for a demonstration (design-capture; not a schedule)

Per FEAT-0003, "any implementation would be a demonstration." This section records
the **dependency order** such a demonstration would follow, so the ordering is not
re-derived later. It is design-capture, **not** authorized work and **not** a
commitment; each step would still need the maintainer to lift the "not scheduled"
status. The order is driven by the two facts in §2 (state welding; inline
conditioning), so the refactor precedes any second law.

1. **Step 1 would extract shared output conditioning from the curve law.** Pull
   the law-agnostic conditioning out of `channel_evaluator.cpp` into a shared
   output stage applied after any law produces a setpoint — D4's firm list (clamp,
   sensor-safe latch and `safety_override`, deadband, cooldown, breaker, write
   gate, `decision:141-145`), plus — if D4's open leans (`decision:147-153`) are
   confirmed — the rate limiter as a safety slew clamp, while the EMA smoothing +
   decay latch stay curve-law-specific. Gate: the existing CTest/pytest lanes stay
   green and the curve output is bit-identical — the load-bearing boost-sum order
   (`:353-364`) locked by an output-equivalence test (REQ-PROFILE-01). Note:
   `EvaluateChannel` has direct test call sites (`tests/cpp/channel_write_tests.cpp`)
   that the seam change in step 2 would have to update, and the bit-identity test
   would call through the new seam.
2. **Step 2 would introduce `IChannelController` and `CurveOverlayController`.**
   Wrap the remaining curve law as one implementation behind the seam; replace the
   direct call at `tick_runner.cpp:241` with the virtual call; move the curve-law
   dynamic state off `ChannelState` into the controller and slim `ChannelState` to
   the law-agnostic fields (D2). Gate: output-equivalence test still bit-identical.
3. **Step 3 would add the `controller` discriminator + `MakeChannelController`
   factory** (Proposal A, starting A1). Absent key = `curve_overlay`, so every
   existing config stays valid unedited (REQ-PROFILE-03).
4. **Step 4 would add the profile request + intake**, mirroring the breaker reset
   at `tick_runner.cpp:195`, validating via `LoadControlLoopConfig`, applying D7's
   build-then-reset-then-swap on a **fixed channel set** (channel-set changes
   deferred to FEAT-0001, REQ-PROFILE-09). Emit
   `control_loop.profile_applied` / `_rejected` / `_invalid`.
5. **Step 5 would add `PidController` as the second, deliberately-different law**
   (P/PI/PD/PID by gain selection, D3), available first in the non-writing
   shadow/dry-run path so a *writing* channel does not switch to an uncharacterized
   law and move the measurement-gate baseline silently. Live PID would sit behind
   the explicit per-channel `pid.allow_live` opt-in (D6, `decision:169-182`).
   Resolved 2026-06-06: the earlier divergence between D6 and REQ-PROFILE-07 over
   whether characterization evidence is required *before* live PID is closed —
   REQ-PROFILE-07 was aligned to D6 (the requirement derives from the decision per
   `docs/features/README.md` §3 gate 4), so live PID is gated by the explicit,
   recorded `pid.allow_live` opt-in, not an evidence-first block. The residual open
   item (what compact evidence a recorded `allow_live` crossing should still
   gather) is §7-8.
6. **Step 6 would (optionally) promote dispatch A1 → A2** (registry table) only if
   a third law or external registration is wanted.

Verification for every step would map to `.\scripts\Test-LocalCI.ps1` and the
contract reviews already listed in FEAT-0003 §10; doc updates would follow
`AGENTS.md` §Change Checklist (notably `CONTROL_PIPELINE_MATH.md` scoped to the
curve law plus a sibling PID identity reference, REQ-PROFILE-10).

## 7. Open questions for the maintainer (not settled by D1–D7)

These surfaced from grounding and are not decided by the existing decision record.

1. **Dispatch shape (Proposal A).** A1 hard-named switch (matches
   `ParseCurveShape`) vs A2 registry table (matches `kBoostStageSpecs`)? The
   recommendation in §3 is "A1 now, reversible to A2," but the maintainer owns the
   call.
2. **Profile object (Proposal B).** Inline full config (B-form 1), named on-disk
   profile (B-form 2, recommended), or delta/overlay (B-form 3)?
3. **Channel-set changes in slice 1.** Confirm slice 1 is law/param swap on a
   **fixed** channel set, deferring add/remove/reorder until FEAT-0001 ships
   (REQ-PROFILE-09).
4. **Request-file convention (corrected toward flat 2026-06-06).** Every
   implemented request file is flat at the runtime-home root
   (`circuit_breaker_reset.request.json`, `stop.request.json`;
   `runtime_lifecycle.cpp:17,22`); no `runtime/requests/` directory exists today.
   FEAT-0003 §7 previously gave a `runtime/requests/profile.json` example; it is
   corrected to the flat-root `profile.request.json` to match that precedent and
   its own "modeled on the breaker-reset request file" claim. Introducing a
   `runtime/requests/` subdir later remains an option, but has no precedent — open
   only as a deliberate future choice, not the default. (FEAT-0001 §7/§8 carried
   the same `runtime/requests/write_policy.json` example; it was corrected to the
   flat-root `write_policy.request.json` in the same 2026-06-06 change, so both
   specs now match the flat convention.)
5. **Single-shot intake ergonomics.** The breaker-reset take deletes the request
   file even on parse error (`runtime_lifecycle.cpp:101`). A mirrored profile
   intake would be single-shot — a malformed profile request is consumed and
   rejected once, not retried. Is that the wanted operator ergonomics?
6. **Seam-extraction-first ordering.** Confirm the first slice is the
   conditioning-extraction refactor of the existing curve law (§6 step 1) with a
   bit-identity gate, **before** any second law — i.e. the modular seam is not an
   additive drop-in.
7. **`last_raw_demand_pct` ownership (D2 gap).** D2's field-bucketing did not name
   `last_raw_demand_pct` (`control_runtime_context.h:31`, set at
   `channel_evaluator.cpp:457`). It is a curve-law EMA input, so it likely moves
   into `CurveOverlayController`; confirm when slimming `ChannelState`.
8. **Measurement-gate evidence (requirement-vs-decision divergence resolved
   2026-06-06; substantive question open).** The *documentation* divergence is
   closed: REQ-PROFILE-07 was aligned to D6 (the requirement derives from the
   decision), so the requirement no longer contradicts the decision's
   `pid.allow_live` design. Whether D6's `allow_live` actually *satisfies*
   `docs/MEASUREMENT_GATE.md` is a separate, unsettled question — the maintainer's
   call on D6 itself. Two defensible readings exist: (a) the gate's "What This
   Blocks" list is cadence, write cooldown, blocked-channel membership, and
   higher-rate-cadence strategy, so a same-cadence PID on an already-live channel
   is a recorded crossing rather than a blocked one; (b) the gate's Exit Criteria
   are measurements ("a different controller strategy" clears the gate only once
   evidence exists), so `allow_live` is *consent, not evidence* and does not clear
   the gate on its own terms. The discussion doc's "measurement gate" section
   argues (b) in detail and flags a concrete code gap: the slew-cap floor D4
   relies on (`rise/fall_rate_pct_per_min`, `max_setpoint_step_pct`) defaults to
   NaN/off (`control_loop.h:29-31`; `RateLimitSetpoint` returns the value
   unmodified on NaN, `channel_evaluator.cpp:55-57`), so an `allow_live` PID is not
   guaranteed a slew bound. Open for the maintainer: accept D6 as written, or
   tighten D6 (and then REQ-PROFILE-07) to require evidence and/or a non-NaN slew
   cap before `allow_live`. The accept-vs-tighten options are written up in
   `docs/profile-hot-swap-allow-live-decision-2026-06-06.md`.

## 8. Reference-drift correction (accuracy-only)

Grounding re-verified every file:line reference cited in the two 2026-06-03 docs
against current source on branch `analyze-native-superset`. The
`channel_evaluator.{cpp,h}` references drifted ~5 lines later than their
2026-06-03 form, driven by recovery-gap remediation 3 (commit `33f8d24`/`68a6fc4`),
which added the `safety_override` field and comment to `ChannelEvaluation` and the
sensor-safe block. Every reference outside that file is exact. The corrections
applied to `docs/features/FEAT-0003-selectable-profile-hot-swap.md` and
`docs/profile-hot-swap-decision-2026-06-03.md` in the same change as this document:

| Symbol | Cited (2026-06-03) | Corrected (current) |
|---|---|---|
| `EvaluateChannel` | `channel_evaluator.cpp:435-469` | `channel_evaluator.cpp:440-474` |
| `EvaluatePrimarySetpoint` | `channel_evaluator.cpp:243-278` | `channel_evaluator.cpp:243-283` |
| `duty = curve + overlays` composition | `channel_evaluator.cpp:273-367` | `channel_evaluator.cpp:453-467` (the four helper calls in `EvaluateChannel`) |
| boost-sum operand order | `channel_evaluator.cpp:348-359` | `channel_evaluator.cpp:353-364` |
| `ChannelEvaluation` result type | `channel_evaluator.h:30-41` | `channel_evaluator.h:30-47` |
| sensor-safe mode | `channel_evaluator.cpp:248-259` | `channel_evaluator.cpp:248-264` |

Two further imprecisions are recorded but left as-is to avoid rewording the
2026-06-03 prose: `RestoreSavedState` is a `FanWriter` method (declared in
`fan_writer.h`, defined `sio_fan_writer.cpp:262-278`) that `HandleExpiredHoldRestore`
*calls* at `channel_write.cpp:205`, not a declaration at `channel_write.h:38-48`
(that range is `HandleExpiredHoldRestore` only); and the cited
`channel_write.cpp:204-221` is an interior slice of `HandleExpiredHoldRestore`,
whose full span is `:186-257`.

## 9. Doc-threading map

How this document and a future demonstration would relate to existing docs.

| Doc | Relationship |
|---|---|
| `docs/features/FEAT-0003-selectable-profile-hot-swap.md` | This deepens it; it stays the spec of record. Status unchanged (Draft, design-capture). |
| `docs/profile-hot-swap-decision-2026-06-03.md` | D1–D7 stay the architecture of record. §3/§4 here fill the dispatch and profile-object gaps those decisions left open; they do not re-decide D1–D7. |
| `docs/features/README.md` | Process authority. Advancing FEAT-0003 would require promoting it `Draft → Accepted` through the §3 gates; nothing here is normative until then. |
| `docs/TRACEABILITY.md` | The central `REQ-*`→verification map. This document introduces no new `REQ-*` (it references existing `REQ-PROFILE-*`), so it changes nothing there; a demonstration would populate the `REQ-PROFILE-*` rows as it builds. |
| `docs/PATH_NOTES.md` | A dated 2026-06-06 **Added** entry records this document; the FEAT-0003 **Idea**/backlog rows stay "design-capture only." Cited by dated entry, not line number — PATH_NOTES is frequently appended. |
| `docs/STRUCTURE_AND_STABILITY.md`, `docs/CODE_MAP.md` | Would be updated at implementation to list the controller seam and its files; a controller TU would live in `src/control/`, not `src/policy/`. |
| `docs/CONTROL_PIPELINE_MATH.md` | Would be scoped to the curve law (`CurveOverlayController`) with values preserved; a sibling PID identity doc added (REQ-PROFILE-10). |
| `docs/MEASUREMENT_GATE.md`, `docs/RUNTIME_HOME.md`, `docs/CONTROL_LOOP.md`, `docs/WRITE_ORCHESTRATION.md`, `README.md` | Would be updated at implementation per `AGENTS.md` §Change Checklist (live-write gating; request/status/event schema; operator workflow). |
