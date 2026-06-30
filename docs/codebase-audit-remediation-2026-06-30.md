# svg-mb-control audit remediation report (2026-06-30)

Companion to `docs/codebase-audit-2026-06-30.md`. That document records **what**
the audit found; this one records **why** each defect exists (root cause), the
**exact remediation** proposed, a **test plan**, the **risk** of the change, and a
**sequenced plan**. This is analysis only — **no engine code is changed by this
report**. Every claim is grounded in source at `main` @ `dc54a60`.

## How to read this

Each finding below has a fixed shape:
- **Why it exists** — the design decision or omission that produced the defect, not just the symptom.
- **Trigger** — the precise runtime/operator condition that makes it bite.
- **Blast radius** — what actually goes wrong on real hardware.
- **Remediation** — the specific code change proposed, with the seam it lands on.
- **Test** — how a Windows-CI test would prove the fix without hardware.
- **Risk** — what the fix could break, and why the proposed shape is conservative.

Severity and file anchors match the audit table. Findings are grouped by the
order I recommend fixing them, not by audit number.

---

## Tier 1 — thermal-safety gating (the four `high`s)

These four are the reason the engine is "safe on the shipped config in steady
state" but **not yet fail-safe against sensor degradation or a crash**. They
should land before any on-hardware evidence run that intends to exercise a
degraded-sensor or fault path.

### R1 — GPU false-cold 0 °C drives fans to minimum (audit #1, high, `src/hardware/gpu_reader.cpp:636`)

**Why it exists.** The GPU intake path trusts a single boolean (`available`) as
both "the reader is present" *and* "this sample's temperature is real." The
underlying `GpuProbe::sample_thermal_fast` returns `true` for any valid GPU
*index* — it answers "is there a GPU here," not "did the thermal read succeed."
When the undocumented-NVAPI read does not populate the fields,
`core_c/memjn_c/hotspot_c` keep their `GpuSnapshot` default of `0.0`, and
`GpuReader::Sample()` forwards that as `available=true, temp=0`. The AMD CPU path
learned this lesson in PR #33 (the `DecodeTctl`/`DecodeCcdTemp` validity gate);
the GPU path never got the equivalent gate.

**Trigger.** A `GpuOnly` (or source-aware CPU-dropped) channel cooling a hot GPU,
plus a transient thermal-read miss: backend not yet ready, discovery mask momentarily
`0`, or `thermals_.read()` failing for one tick.

**Blast radius.** The curve is evaluated at 0 °C → minimum duty on a hot GPU.
Because `available` stayed `true`, **no sensor-miss is counted**, so the 3-miss
sensor-safe trip never fires. This is a silent false-cold on the live control
input — the exact class the CPU path is now guarded against.

**Remediation.** Add a GPU-side validity gate, mirroring the CPU fix so the two
sensor paths are symmetric:
- In `GpuReader::Sample()`, treat a thermal sample whose `core_c` and `memjn_c`
  (and `hotspot_c` when the part reports it) are all `<= ` a small floor (e.g.
  `1.0 °C`) as **not available** — set `available=false` so the channel sees the
  GPU input as ABSENT and the existing sensor-miss / safe-mode machinery runs.
- Preferred deeper fix: have `GpuProbe`/`GpuSensorReader` distinguish "read
  succeeded" from "valid index" and propagate per-field validity, so a future
  legitimately-near-0 °C reading (cold boot) is not misclassified. The floor-gate
  is the minimal safe stopgap; the per-field validity is the correct structural fix.

**Test.** A unit test on the `GpuReader::Sample()` mapping (or a thin seam around
it) that feeds an all-zero `GpuSnapshot` with the probe returning `true` and
asserts `available==false`; and a positive case (e.g. 45 °C) asserting
`available==true`. No hardware needed — the snapshot is a plain struct.

**Risk.** Low. The only behavioral change is reclassifying an all-≈0 sample as
absent. The one edge case to respect is a genuinely cold GPU at idle; a `1 °C`
floor is well below any powered-GPU idle temperature, and an unpowered GPU has no
thermal demand, so degrading it to ABSENT (→ CPU fallback / safe-mode) is the
conservative direction.

### R2 — Safe-mode 100 % is rate-limited and cooldown-gated (audit #2, high, `src/control/channel_evaluator.cpp:166`)

**Why it exists.** `safety_override` was introduced for exactly one job —
bypassing the write-failure circuit breaker (`channel_write.cpp:316`) — and was
never threaded through the *setpoint-shaping* stages. The safe-mode setpoint
(`kSafeModeFanDuty = 100`) is produced at the top of the pipeline
(`EvaluatePrimarySetpoint`, line 166) and then flows through the same
`ApplyDemandSmoothing` → `RateLimitSetpoint` → deadband → write-cooldown path as
a normal demand. So the *intent* ("go to 100 % now, the sensor is blind") is
correct, but the *delivery* is throttled by the very limiters meant for smooth
normal operation.

**Trigger.** A channel running at a low/idle duty (e.g. 30 %) loses its sensor for
3 ticks on a hot part.

**Blast radius.** With shipped `control.release.json` values
(`max_setpoint_step_pct=0.6`, `rise_rate_pct_per_min=75`), the rise is capped to
~0.31 %/tick, so the fan takes **tens of seconds** to actually reach 100 % while
the blind part heats — and the event log already records "entering safe mode
(100 % duty)" (`channel_write.cpp:164`) that the actuator has not achieved, so the
evidence overstates protection.

**Remediation.** Make `safety_override` short-circuit the shaping stages, not just
the breaker:
- When `evaluation.safety_override` is set, bypass `ApplyDemandSmoothing` and
  `RateLimitSetpoint` — drive the final setpoint directly to `kSafeModeFanDuty`.
- In `TryApplyChannelSetpoint`, bypass the deadband and write-cooldown gates for a
  safety-override write so it is issued immediately rather than deferred.
- This mirrors the existing precedent where `authority_reassert` already
  short-circuits the cooldown (noted in the prior session's P2 list), so the
  pattern is established in the codebase.

**Test.** A `channel_write`/`channel_evaluator` unit test: drive a channel to a low
setpoint, force the 3-miss trip, and assert the *applied* setpoint reaches
`kSafeModeFanDuty` on the **same tick** (not after N ticks of rate-limited creep),
and that the write is not deferred by cooldown. The existing safe-mode tests give
the harness.

**Risk.** Low-to-medium. The change widens what `safety_override` bypasses, so the
audit fix must be careful that `safety_override` is *only* ever set on the genuine
safety trip (verified: it is set at `channel_evaluator.cpp:172` on the 3-miss path).
Bypassing the deadband for a safety write can cause one extra write per trip — an
acceptable cost for an immediate safe-mode response.

### R3 — Overlay base-channel coverage not enforced (audit #3, high, `src/control/profile_composition.cpp:94`)

**Why it exists.** `ComposeConfigRoot` validates the overlay in one direction only:
it iterates the overlay's channels and throws if an overlay channel is *not* a
base-controlled channel (`min_duty.find(number) == end`). The reverse invariant —
"every base-controlled fan must appear in the overlay" — was never written,
because the shipped overlay happens to list all of channels 0–5, so the gap is
invisible in the only configuration that ships.

**Trigger.** An operator edit: deleting a controlled fan's block (e.g. channel 4,
`front_radiator_intake_noctua`, base floor 24 %) from `release.behavior.json`.

**Blast radius.** The config still composes and loads with no diagnostic. That fan
produces no `ChannelControlConfig`, is never written by the control loop, and is
never floored — a silent under-cooling regression. It is config-data, not a runtime
fault, but the failure mode (a fan you think is controlled is not) is severe and
gives zero signal.

**Remediation.** After the injection loop in `ComposeConfigRoot`, assert the
overlay's channel-number set is a **superset** of `ControlledChannelMinDuty`'s keys;
throw a precise error — `"machine base controls channel N but the behavior overlay
does not list it"` — for any missing base-controlled channel. This closes the
FEAT-0023 composition invariant symmetrically with the existing forward check.

**Test.** A `profile_composition` unit test: compose a machine base that controls
channel N against an overlay that omits channel N, and assert it throws with the
channel number named. A positive test (full coverage composes cleanly) already
exists as `TestComposeReproducesReleaseControlLoop`.

**Risk.** Very low. It is a new validation that only rejects configs that are
already silently broken; it cannot change the composed output for any valid config
(the shipped overlay covers 0–5, so the gate is a no-op on `main`). The only care
needed: respect the `excluded` direction (channel 6 / pump) so an intentionally
excluded fan is not required in the overlay — the proposed check keys off
`ControlledChannelMinDuty`, which already excludes `direction == "excluded"`.

### R4 — Non-idempotent event ingest reopens Issue #4 (audit #4 & #5, high, `src/analyze/analyze_ingest.cpp:272`, `analyze_ingest_db.cpp:301`)

**Why it exists.** Two coupled omissions in the incremental ingest path:
1. **No idempotency on events.** `IngestEvents()` always re-parses the full
   `svg_mb_control_events.jsonl` and inserts every line; the `DELETE FROM events`
   is guarded by `options.force` only, and the events schema has no UNIQUE
   constraint. The manifest path *does* skip already-seen runs (`++runs_skipped;
   continue`), but the events path does not — so the two halves disagree about what
   "already ingested" means.
2. **Attribution depends on the run window.** Because the seen run is skipped,
   `run_windows` is empty, so `InsertEventsAttributed` writes every duplicate with
   `run_id = NULL`. FEAT-0016 prune deletes by `runs.id` via `ON DELETE CASCADE`
   and `CountOrphanRows` filters `WHERE e.run_id IS NOT NULL`, so NULL-`run_id` rows
   are structurally unreachable by retention.

The regression test `test_ingest_is_idempotent_without_force` asserts every table
count *except* `events`, so it gave false confidence that the path was idempotent.

**Trigger.** The normal operational loop: periodic `analyze ingest` (no `--force`)
over the same runtime home. Each run appends another full copy of the events file.

**Blast radius.** After N incremental ingests the `events` table holds
`events_per_file × N` rows, and `analyze prune --db-retain-days 7` reports
`db_orphan_rows=0` while thousands of unprunable duplicate events accumulate
forever — the exact unbounded-disk-growth mode Issue #4 / FEAT-0015 / FEAT-0016
were meant to close, partially reopened on the DB side.

**Remediation.** Two changes, both required:
- **Make event ingest idempotent on the non-force path.** Options, in order of
  preference: (a) skip the events phase entirely when `run_windows` is empty (the
  manifest for those runs was already skipped, so their events are already in the
  DB); or (b) add a dedup key `UNIQUE(run_id, event_time, event_type, detail)` with
  `INSERT OR IGNORE`; or (c) clear-and-replace events for the affected runs only.
  Option (a) is the smallest and matches the manifest path's existing skip logic.
- **Add a retention path for unattributed events.** `DELETE FROM events WHERE
  run_id IS NULL AND event_time < cutoff` in the prune step, so any NULL-`run_id`
  rows that already exist (or arrive from a non-attributable source) are bounded by
  FEAT-0016 rather than immortal.
- **Fix the test's false confidence.** Extend `test_ingest_is_idempotent_without_force`
  to assert the `events` count is stable across a second non-force ingest.

**Test.** Python lane (`tests/test_analyze_ingest.py`): ingest a fixture home
twice without `--force`, assert `events` row count is identical after the second
ingest and that no row has `run_id IS NULL`; and a prune test asserting a seeded
NULL-`run_id` row older than the cutoff is deleted.

**Risk.** Medium — this is the only Tier-1 fix that touches SQL and the retention
contract. The clear-and-replace and dedup-key variants need care around the
`runs.event_count_ingested` writeback (the adversarial verifier already refuted a
*separate* claim that `--force` re-ingest inflates that counter, so the `--force`
path is sound and must stay that way). Option (a) — skip-when-empty — is the
lowest-risk because it changes nothing on the `--force` or first-ingest paths.

---

## Tier 2 — reliability (the two `medium`s)

### R5 — Crash-latch backoff leaves fans frozen up to 32 s (audit #6, medium, `src/control/control_supervisor.cpp:821`)

**Why it exists.** Two design choices compose into a gap: the control-loop shutdown
restore (which reverts fans to BIOS auto) runs only on a *clean* loop exit, and
`sio_fan_writer.cpp:94` sets `restore_on_exit=false`, so a crash leaves the fan
latched in manual mode at its last commanded duty. The supervisor then backs off
`min(60, 1<<min(restart_count,5))` = 2,4,8,16,32,32… s before respawning, and
nothing out-of-process drives a safe duty during that window. The project's own
decision record (`docs/fan-restart-restore-and-plant-model-measurement-2026-06-20.md:45-49`)
already acknowledges this latched-no-fallback state.

**Trigger.** A worker that passed startup crashes at runtime (`exit_code != 0`, not
stop-requested), especially recurringly.

**Blast radius.** During each backoff no worker samples temperature or drives fans.
A recurring fault plus a load spike inside a ≤32 s window lets the part heat with
the fan stuck at a possibly-low idle duty.

**Remediation.** Bound the uncontrolled window with a failsafe. Two complementary
options:
- On a crash exit, have the supervisor (or a tiny standalone failsafe that does not
  depend on the crashed worker's state) drive blocked/controlled channels to a safe
  high duty, or hand the fans back to BIOS auto, *before* sleeping.
- And/or cap the backoff much lower (e.g. ≤4 s) so the uncontrolled window is small
  even without a failsafe write.

**Test.** A `control_supervisor` unit test asserting the backoff cap, plus a test
that a simulated crash exit path issues the safe-duty/BIOS-handoff write before the
sleep. The watchdog/restart harness (FEAT-0008) already exercises the supervisor.

**Risk.** Medium — touches the restart state machine, which is safety-relevant in
its own right. The safe-duty-on-crash write must itself be robust to the same
transport that the worker was using; handing to BIOS auto is the more robust
fallback because it does not require a successful manual write.

### R6 — Spec-gate cannot detect duplicate FEAT registry rows (audit #7, medium, `tests/test_feature_specs.py:93`)

**Why it exists.** `_registry_rows()` builds a `dict` keyed by FEAT id, so two rows
sharing an id silently collapse to the last one. The only consistency assertion is
a key-set comparison (`sorted(registry) == sorted(specs)`), which a duplicate does
not perturb. This is precisely the collision class that produced the FEAT-0024 /
FEAT-0025 clash PR #28 had to renumber by hand — the governance gate that is
supposed to catch it is blind to it.

**Trigger.** A maintainer copies an existing FEAT id (two `[FEAT-00NN]` rows in the
README §5 registry).

**Blast radius.** The Python lane stays green while the first row's
namespace/status/path go unchecked — a governance hole, not a runtime fault, but it
defeats the spec-before-build contract the repo relies on.

**Remediation.** In `_registry_rows`, collect ids into a list before dedup and
assert `len(ids) == len(set(ids))`; or count raw `[FEAT-` rows and assert equality
with `len(registry)`. One-line-class change in the test harness.

**Test.** The change *is* a test change; add a self-test fixture with a duplicated
id and assert the gate now fails.

**Risk.** Negligible. Pure test-harness hardening; cannot affect engine behavior.

---

## Tier 3 — hardening (`low`)

These are durability/robustness gaps with narrow triggers. Worth doing, but they do
not gate an evidence run.

- **R7 — `DecodeTctl` no lower bound (audit #8, `amd_decode.h:37`).** Add a
  physically-grounded lower bound to the `out_valid` predicate (and `DecodeCcdTemp`)
  so an offset-corrected sub-ambient reading (e.g. `temp_field=8` with the `0x80000`
  flag → −48 °C) is dropped as implausible. Cheap, pure-function, fully unit-testable
  (extend `amd_decode_tests.cpp`). The upper-bound and zero-field gates from PR #33
  already exist; this just completes the interval.
- **R8 — Event-log JSONL not fsync'd (audit #9, `runtime_event_log.cpp:365`).**
  Either `FlushFileBuffers` after `WriteFile` (per-event durability cost), or
  document and enforce that JSONL consumers tolerate a torn final line. Decide
  durability-vs-throughput explicitly rather than by omission.
- **R9 — CSV archive/mirror not fsync'd (audit #10, `runtime_csv_archive.cpp:309`).**
  Periodically `FlushFileBuffers` the archive handle at each manifest-update
  boundary, or stop asserting an exact `rows_written` in the synced manifest so the
  durable manifest count cannot exceed what is durably present in the CSV.
- **R10 — Local-time watchdog timestamps (audit #11, `runtime_health.cpp:184`).**
  Persist runtime timestamps in UTC (`Z`) or carry a monotonic heartbeat counter;
  compute age from a monotonic source; never clamp a negative age to "fresh" — treat
  an out-of-order timestamp as stale. Fixes the DST-fall-back / backward-clock window
  where a hung worker reads healthy.
- **R11 — Build hard-requires vcpkg (audit #12, `.github/workflows/ci-windows.yml:40`).**
  Make vcpkg optional: set the toolchain file/triplet only when `VCPKG_ROOT`
  resolves, resolve cmake/ninja/ctest from PATH otherwise, and drop/downgrade the
  throwing "Configure vcpkg" gate since no vcpkg package is consumed.

---

## Recommended sequencing

1. **R1, R2, R3** first — they are small, well-isolated, fully unit-testable without
   hardware, and they close the three thermal-safety gaps that make the engine
   not-yet-fail-safe. R3 in particular is near-zero-risk.
2. **R4** next — slightly larger (SQL + retention + test), choose the skip-when-empty
   variant to keep `--force` untouched; pair with the events-count test fix so the
   regression cannot silently return.
3. **R5, R6** — R6 is a one-line-class test fix (do it opportunistically); R5 touches
   the restart state machine and deserves its own focused PR.
4. **R7–R11** — batch the `low`s; R7 and R11 are trivial, R8/R9/R10 are small
   durability changes that can share one PR.

**PR shape recommendation:** R1/R2/R3 as one "fail-safe sensor + safe-mode" PR (they
share the safety theme and the `channel_*`/`gpu_reader` surface), R4 on its own
(analyze/DB surface, different reviewer focus), R5 on its own, and the `low`s
batched. All are Windows-CI-testable; none need AMD/NVIDIA hardware to prove the
fix logic (only the eventual on-hardware evidence run does).

---

## Explicitly out of scope of this report

No engine code, config, schema, or test was modified. Implementation is deferred
pending your go-ahead on scope and PR shape. When you approve, the natural first
step is the R1/R2/R3 safety PR.
