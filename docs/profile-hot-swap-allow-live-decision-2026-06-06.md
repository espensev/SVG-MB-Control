# Profile Hot-Swap — `pid.allow_live` Reconsideration Brief — 2026-06-06

**Project:** svg-mb-control
**Status:** Open — awaits maintainer selection (decision-support, not a decision)
**Companion to:** `docs/profile-hot-swap-decision-2026-06-03.md` (decision D6),
`docs/features/FEAT-0003-selectable-profile-hot-swap.md` (REQ-PROFILE-07),
`docs/MEASUREMENT_GATE.md`,
`docs/modular-profile-hotswap-discussion-2026-06-06.md` (measurement-gate section)
**Purpose:** lay out the two directions for the live-PID authorization posture so
the maintainer can choose, after a review surfaced that decision **D6** and
`docs/MEASUREMENT_GATE.md` can be read as inconsistent. PID =
proportional-integral-derivative. This brief changes no decision; D6 stands until
a direction below is selected.

> This is design-capture with no runtime effect. FEAT-0003 is `Draft`,
> not scheduled (see its Scope & intent), and none of the PID/`allow_live`
> machinery exists in code. Nothing here authorizes work.

## 1. What was decided, and what this reopens

Decision **D6** (`docs/profile-hot-swap-decision-2026-06-03.md:167-182`) selected:
shadow/dry-run is the default for a PID channel, and a per-channel
`pid.allow_live: true` opt-in authorizes live PID writes **immediately, without
first requiring the characterization evidence**, on the grounds that the crossing
is "explicit and recorded, not silent." On 2026-06-06 `REQ-PROFILE-07` was aligned
to D6 (a requirement derives from its decision —
`docs/features/README.md` §3 gate 4), which removed the *documentation*
contradiction between the requirement and the decision.

This brief reopens the *substantive* question that alignment did not settle:
**is D6's `allow_live`-without-evidence posture the right one?**

## 2. The tension (grounded)

1. **Evidence gate vs. consent gate.** `docs/MEASUREMENT_GATE.md` Exit Criteria
   are measurements ("a different controller strategy" clears the gate only once
   cadence/write-response data and "a compact summary records the data that
   justified the change" exist, `MEASUREMENT_GATE.md:72-85`). The discussion doc's
   measurement-gate section argues that `allow_live` provides *consent* (who moved
   the baseline and when) but not *evidence* (whether it was safe to move), so it
   does not clear the gate on the gate's own terms.
2. **Shadow evidence is necessary but not sufficient.** Shadow/dry-run logs what
   PID *computed* against a temperature trajectory some other controller was
   driving. It cannot exhibit the closed-loop failure modes (limit cycle,
   overshoot, integral windup) that only appear once PID's own output moves the
   temperature it reads — the same modes D6's D3b/D3c leans (derivative-on-
   measurement, anti-windup) exist to manage.
3. **The compensating safety floor is not guaranteed.** D6 names the slew cap as
   part of the "regardless" safety floor (`:179-180`), but the slew fields default
   to NaN/off: `rise_rate_pct_per_min`, `fall_rate_pct_per_min`, and
   `max_setpoint_step_pct` default to `quiet_NaN()` (`src/control/control_loop.h:29-31`),
   and `RateLimitSetpoint` returns the desired value unmodified when the rate is
   NaN (`src/control/channel_evaluator.cpp:55-57`). On a channel whose config omits
   these fields, an `allow_live` PID could step 0→100% in one tick, bounded only by
   the `[0,100]` clamp, sensor-safe mode, and the write-failure breaker.

**Current exposure (checked 2026-06-06):** the shipped `config/control.release.json`
sets all three slew fields for every live channel 0–5 (channel 6 blocked), so the
NaN-default path is **not active in the shipped config today**. The gap is a missing
*precondition* — a future PID/profile config could omit the fields — not a live
defect in the current build.

## 3. Options

### Option A — Accept D6 as written

`allow_live` authorizes live PID immediately; the audit event
(`control_loop.profile_applied`) is the control; shadow/dry-run stays the default
but is not mandatory before going live.

- **For:** matches the maintainer's recorded acceptance that live PID "runs off the
  characterized curve baseline before evidence exists" (`:181-182`); keeps the
  operator surface simple; design-capture only, so no live exposure now.
- **Against:** leaves the evidence-vs-consent reading of `MEASUREMENT_GATE.md`
  unresolved, and leaves the slew-cap precondition unenforced — an `allow_live`
  channel is only as safe as its config remembered to be.

### Option B — Tighten (two independent levers; pick either or both)

- **B1 — evidence-before-live.** Re-tighten so `allow_live` requires the
  `MEASUREMENT_GATE.md` characterization evidence (shadow-log comparison accepted
  as that evidence) before it authorizes a live write. This restores
  REQ-PROFILE-07's original evidence-first intent and would re-align the requirement
  in the other direction. Resolves tension #1/#2; costs an explicit evidence step
  per channel before live.
- **B2 — slew-cap precondition.** Make `allow_live: true` a parse-time error unless
  that channel sets a non-NaN, positive slew cap (`max_setpoint_step_pct` and/or the
  rate fields). Converts "trust the config" into "the one floor that bounds a
  mis-tuned PID is provably present before it can write." Resolves tension #3; cheap
  (one validation rule); independent of B1.

## 4. Recommendation

Adopt **B2 unconditionally** — it closes a verified gap for one validation rule and
has no downside — and keep **shadow/dry-run the default**. Treat **B1 as the
maintainer's read of `MEASUREMENT_GATE.md`**: if the gate is meant as an evidence
gate for a control-*law* change (the discussion doc's reading), choose B1 and accept
shadow-log comparison as the evidence; if the gate is meant only to prevent *silent*
baseline drift, Option A + B2 is sufficient. The cheapest defensible posture is
**A + B2**; the strictest is **B1 + B2**.

## 5. Consequence on REQ-PROFILE-07 (so the docs stay consistent)

- **A or B2-only:** REQ-PROFILE-07 stays as aligned 2026-06-06 (live PID via the
  recorded `allow_live` opt-in); B2 adds "and the channel must set a non-NaN slew
  cap."
- **B1:** REQ-PROFILE-07 is re-tightened to require evidence before `allow_live`,
  and D6 is updated to match; the 2026-06-06 alignment is reversed by intent, not by
  drift.

Whichever is chosen, update D6 in `docs/profile-hot-swap-decision-2026-06-03.md`
first (decision is the source of truth), then REQ-PROFILE-07 and
`docs/TRACEABILITY.md` in the same change, and close out the open item in
`docs/modular-profile-hotswap-plan-2026-06-06.md` §7-8.

## 6. Grounding

- D6 text: `docs/profile-hot-swap-decision-2026-06-03.md:167-182`.
- Gate Exit Criteria: `docs/MEASUREMENT_GATE.md:72-85`.
- Slew-cap NaN default: `src/control/control_loop.h:29-31`;
  `src/control/channel_evaluator.cpp:55-57`.
- Shipped slew config present for channels 0–5:
  `config/control.release.json` (rate fields per channel).
- Substantive argument in full: `docs/modular-profile-hotswap-discussion-2026-06-06.md`
  "measurement gate" section.
