# FEAT-0024 Intake-Lead Fan Response Under Load — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the intake lanes (channels `2`/`3`/`4`) supply airflow ahead of the exhausts when a thermal load rises — engage earlier, ramp faster, and hold — via a config-only, rise-asymmetric retune, with idle and the exhaust lanes unchanged.

**Architecture:** Config-value-only change to the per-channel intake fields consumed by `src/control/channel_evaluator.cpp` (rate limiter + demand smoothing) and `src/control/boost_stage.cpp` (`gpu_airflow` integrator). No `src/` behavior change and no control-computation identity change. A new config-contract test pins the intake-lead invariants; the magnitudes are validated by a live Pass-1 (idle) + Pass-3 (combined load) capture before the live deploy.

**Tech Stack:** JSON config (`config/control.release.json`, `release/control.json`), Python `unittest` contract tests (`tests/test_config_contracts.py`), the in-tree analyzer (`svg-mb-control analyze`), PowerShell capture scripts.

## Global Constraints

- The owning spec is `docs/features/FEAT-0024-intake-lead-under-load.md` (`REQ-INLEAD-01..06`); the direction record is `docs/intake-lead-response-decision-2026-06-25.md`. Every task below maps to a REQ.
- **Intake lanes only:** edit channels `2`, `3`, `4`. Channels `0`, `1`, `5` must stay byte-unchanged (REQ-INLEAD-05).
- **Idle untouched:** no curve, `cpu_override`, or soft-floor knot at or below `72 C`, and no `min_duty_pct`, changes (REQ-INLEAD-04). `gpu_airflow_start_c` stays in `[55.0, 64.0]`.
- **Rise-asymmetric:** never raise `fall_rate_pct_per_min`, `demand_smoothing_fall_alpha`, or `decay_latch_pct_per_min` on any lane (REQ-INLEAD-05).
- **No measurement-gate move:** do not change `poll_tick_ms`, `write_cooldown_ms`, `deadband_pct`, the channel set, or `temp_blend`.
- **Radiator authority:** the channel `4` `cpu_override` knots at or above `90 C` stay byte-unchanged; only `72-86 C` is steepened (REQ-INLEAD-03). Channels `2`/`3` `cpu_override` are unchanged.
- `max_setpoint_step_pct` must stay `< 1.0` (existing contract `test_shipped_control_loop_configs_use_smooth_step_cadence`).
- `config/control.release.json` and `release/control.json` must stay byte-identical for the edited lanes.
- Do not run, restart, or deploy to the live controller except in Task 5, which is an explicit live-runtime task the operator authorizes.

## Candidate magnitudes (settled by Pass-3, Task 4)

Current → candidate, intake lanes only. Source: `docs/intake-lead-response-decision-2026-06-25.md` §3.

| Field | ch2 | ch3 | ch4 |
|---|---|---|---|
| `rise_rate_pct_per_min` | 90.0 → **125.0** | 90.0 → **125.0** | 60.0 → **120.0** |
| `max_setpoint_step_pct` | 0.7 → **0.95** | 0.7 → **0.95** | 0.6 → **0.95** |
| `gpu_airflow_start_c` | 62.0 → **58.0** | 62.0 → **58.0** | 64.0 → **58.0** |
| `gpu_airflow_max_boost_pct` | 8.0 → **12.0** | 8.0 → **12.0** | 5.0 → **10.0** |
| `demand_smoothing_rise_alpha` | 0.018 (unchanged) | 0.018 (unchanged) | 0.008 → **0.014** |
| `cpu_override_curve` | unchanged | unchanged | `82 C: 42→`**`50`**, `86 C: 46→`**`53`** (others unchanged) |

Effective rise ceiling `min(rise/60, step*1000/250)`: ch2/ch3 `1.5 → 2.083 %/s`, ch4 `1.0 → 2.0 %/s`; exhausts stay `1.25 %/s`.

---

## File Structure

- `tests/test_config_contracts.py` — add one test method `test_feat0024_intake_lanes_lead_under_load` (Task 1). Pins the intake-lead invariants on `config/control.release.json` and the deployed-mirror sync.
- `config/control.release.json` — intake-lane value edits (Task 2).
- `release/control.json` — identical intake-lane value edits (Task 2). This is the deployed file; editing it does not affect the running controller until it is restarted (Task 5).
- `docs/response-evaluation-tuning-plan.md`, `docs/COOLING_STRATEGY.md`, `config/machines/snd-desk.cooling.policy.json`, `docs/CONTROL_PIPELINE_MATH.md` — lockstep doc/prose updates (Task 3).
- Live-capture evidence file `docs/feat-0024-intake-lead-evidence-2026-06-25.md` (Task 4) and the spec/traceability verification-log fill (Task 4/5).

---

## Task 1: Config-contract test for the intake-lead invariants (TDD red)

Covers REQ-INLEAD-01, -02, -03, -05 as automated `T` checks. (REQ-INLEAD-04 idle-unchanged is already enforced by the existing `test_release_intake_low_end_curves_follow_machine_policy`; this test adds the ch4 `<= 72 C` / `>= 90 C` pins.)

**Files:**
- Modify: `tests/test_config_contracts.py` (add one method to `ConfigContractTests`)

**Interfaces:**
- Consumes: `_read_json`, `REPO_ROOT`, `unittest` (already imported via `from tests.helpers import *`).
- Produces: nothing other tasks import.

- [ ] **Step 1: Write the failing test**

Add this method inside `class ConfigContractTests` in `tests/test_config_contracts.py` (e.g. after `test_release_intake_low_end_curves_follow_machine_policy`):

```python
    def test_feat0024_intake_lanes_lead_under_load(self) -> None:
        # FEAT-0024 (REQ-INLEAD-*): intake lanes 2/3/4 lead the exhausts under
        # load — config-only surge-and-hold, rise-asymmetric, idle unchanged.
        release = _read_json(REPO_ROOT / "config" / "control.release.json")
        deployed = _read_json(REPO_ROOT / "release" / "control.json")
        self.assertIsNotNone(release)
        self.assertIsNotNone(deployed)
        rel = {c["channel"]: c for c in release["control_loop"]["channels"]}
        dep = {c["channel"]: c for c in deployed["control_loop"]["channels"]}
        cooldown = release["control_loop"]["write_cooldown_ms"]
        intakes = (2, 3, 4)
        exhausts = (0, 1, 5)
        shipped_rise = {2: 90.0, 3: 90.0, 4: 60.0}

        def ceiling(ch: dict) -> float:
            return min(
                ch["rise_rate_pct_per_min"] / 60.0,
                ch["max_setpoint_step_pct"] * 1000.0 / cooldown,
            )

        # Deployed mirror stays in sync with the source for the edited lanes.
        for cid in intakes:
            for key in (
                "rise_rate_pct_per_min",
                "max_setpoint_step_pct",
                "gpu_airflow_start_c",
                "gpu_airflow_max_boost_pct",
                "demand_smoothing_rise_alpha",
                "cpu_override_curve",
            ):
                self.assertEqual(
                    rel[cid][key],
                    dep[cid][key],
                    msg=f"release/control.json channel {cid} {key} out of sync",
                )

        # REQ-INLEAD-01: intakes ramp faster than every exhaust; ch4's rise
        # *raise* is at least as large as the 200mm intakes' raise.
        fastest_exhaust = max(ceiling(rel[c]) for c in exhausts)
        for cid in intakes:
            self.assertGreater(
                ceiling(rel[cid]),
                fastest_exhaust,
                msg=f"intake {cid} must ramp faster than the exhausts",
            )
            self.assertGreater(rel[cid]["rise_rate_pct_per_min"], shipped_rise[cid])
            self.assertGreater(rel[cid]["max_setpoint_step_pct"], 0.0)
            self.assertLess(rel[cid]["max_setpoint_step_pct"], 1.0)
        self.assertGreaterEqual(
            rel[4]["rise_rate_pct_per_min"] - shipped_rise[4],
            min(
                rel[2]["rise_rate_pct_per_min"] - shipped_rise[2],
                rel[3]["rise_rate_pct_per_min"] - shipped_rise[3],
            ),
            msg="ch4 (slowest fan) must get a rise raise >= the 200mm intakes",
        )

        # REQ-INLEAD-02: intakes engage and surge on a GPU climb ahead of exhausts.
        latest_intake_onset = max(rel[c]["gpu_airflow_start_c"] for c in intakes)
        earliest_exhaust_onset = min(rel[c]["gpu_airflow_start_c"] for c in exhausts)
        self.assertLess(
            latest_intake_onset,
            earliest_exhaust_onset,
            msg="intakes must begin gpu_airflow before the exhausts",
        )
        smallest_intake_ceiling = min(
            rel[c]["gpu_airflow_max_boost_pct"] for c in intakes
        )
        largest_exhaust_ceiling = max(
            rel[c]["gpu_airflow_max_boost_pct"] for c in exhausts
        )
        self.assertGreaterEqual(
            smallest_intake_ceiling,
            largest_exhaust_ceiling,
            msg="intake gpu_airflow ceilings must be >= the exhausts",
        )
        for cid in intakes:  # stays above true idle (existing [55,64] band holds)
            self.assertGreaterEqual(rel[cid]["gpu_airflow_start_c"], 55.0)

        # REQ-INLEAD-03: ch4 cpu_override steepened only in 72-86 C; <=72 and
        # >=90 unchanged; monotonic. ch2/ch3 cpu_override unchanged.
        ch4 = {p["temp_c"]: p["duty_pct"] for p in rel[4]["cpu_override_curve"]}
        self.assertEqual(ch4[35], 24.0)
        self.assertEqual(ch4[50], 27.0)
        self.assertEqual(ch4[62], 31.0)
        self.assertEqual(ch4[72], 38.0)
        self.assertEqual(ch4[90], 54.0)
        self.assertEqual(ch4[95], 70.0)
        self.assertGreater(ch4[82], 42.0)
        self.assertGreater(ch4[86], 46.0)
        ch4_duties = [p["duty_pct"] for p in rel[4]["cpu_override_curve"]]
        self.assertEqual(
            ch4_duties, sorted(ch4_duties), msg="ch4 cpu_override must stay monotonic"
        )
        self.assertEqual(
            [p["duty_pct"] for p in rel[2]["cpu_override_curve"]],
            [42.0, 46.0, 54.0, 64.0, 64.0, 66.0, 74.0, 86.0],
            msg="ch2 cpu_override must be unchanged",
        )
        self.assertEqual(
            [p["duty_pct"] for p in rel[3]["cpu_override_curve"]],
            [38.0, 42.0, 50.0, 60.0, 60.0, 62.0, 70.0, 82.0],
            msg="ch3 cpu_override must be unchanged",
        )

        # REQ-INLEAD-05: rise-asymmetric (fall not raised) + exhausts unchanged.
        shipped_fall = {
            2: (45.0, 0.006, 120.0),
            3: (45.0, 0.006, 120.0),
            4: (25.0, 0.003, 90.0),
        }
        for cid, (fr, fa, dl) in shipped_fall.items():
            self.assertLessEqual(rel[cid]["fall_rate_pct_per_min"], fr)
            self.assertLessEqual(rel[cid]["demand_smoothing_fall_alpha"], fa)
            self.assertLessEqual(rel[cid]["decay_latch_pct_per_min"], dl)
        shipped_exhaust = {
            0: (75.0, 0.6, 64.0, 4.0),
            1: (75.0, 0.8, 64.0, 5.0),
            5: (75.0, 0.8, 64.0, 5.0),
        }
        for cid, (rr, ms, gs, gm) in shipped_exhaust.items():
            self.assertEqual(rel[cid]["rise_rate_pct_per_min"], rr)
            self.assertEqual(rel[cid]["max_setpoint_step_pct"], ms)
            self.assertEqual(rel[cid]["gpu_airflow_start_c"], gs)
            self.assertEqual(rel[cid]["gpu_airflow_max_boost_pct"], gm)
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `python -m pytest tests/test_config_contracts.py::ConfigContractTests::test_feat0024_intake_lanes_lead_under_load -v`
Expected: FAIL — the shipped intake `rise_rate_pct_per_min` is still `90/90/60` and `gpu_airflow_start_c` is `62/62/64`, so `assertGreater(ceiling(intake), fastest_exhaust)` and the onset assertion fail.

- [ ] **Step 3: (no implementation in this task — config edits are Task 2)**

This task delivers only the failing test. Do not edit config here.

- [ ] **Step 4: Commit the failing test**

```bash
git add tests/test_config_contracts.py
git commit -m "test(feat-0024): pin intake-lead invariants (REQ-INLEAD-01/02/03/05)"
```

---

## Task 2: Apply the intake-lane config deltas (TDD green)

Covers REQ-INLEAD-01, -02, -03, -04, -05. Channels appear in `config/control.release.json` / `release/control.json` in the order `[0, 4, 3, 2, 5, 1]`; edit inside the channel-2, channel-3, channel-4 objects only.

**Files:**
- Modify: `config/control.release.json` (channels 2, 3, 4)
- Modify: `release/control.json` (channels 2, 3, 4 — identical edits)

**Interfaces:**
- Consumes: the candidate-magnitudes table above.
- Produces: the config the Task 1 test asserts.

- [ ] **Step 1: Edit channel 2** (in BOTH files, inside the `"channel": 2` object)

- `"rise_rate_pct_per_min": 90.0` → `125.0`
- `"max_setpoint_step_pct": 0.7` → `0.95`
- `"gpu_airflow_start_c": 62.0` → `58.0`
- `"gpu_airflow_max_boost_pct": 8.0` → `12.0`
- `cpu_override_curve`: **unchanged**

- [ ] **Step 2: Edit channel 3** (in BOTH files, inside the `"channel": 3` object)

- `"rise_rate_pct_per_min": 90.0` → `125.0`
- `"max_setpoint_step_pct": 0.7` → `0.95`
- `"gpu_airflow_start_c": 62.0` → `58.0`
- `"gpu_airflow_max_boost_pct": 8.0` → `12.0`
- `cpu_override_curve`: **unchanged**

- [ ] **Step 3: Edit channel 4** (in BOTH files, inside the `"channel": 4` object)

- `"rise_rate_pct_per_min": 60.0` → `120.0`
- `"max_setpoint_step_pct": 0.6` → `0.95`
- `"gpu_airflow_start_c": 64.0` → `58.0`
- `"gpu_airflow_max_boost_pct": 5.0` → `10.0`
- `"demand_smoothing_rise_alpha": 0.008` → `0.014`
- In `cpu_override_curve`, the `82 C` point `{"temp_c": 82, "duty_pct": 42}` → `"duty_pct": 50`, and the `86 C` point `{"temp_c": 86, "duty_pct": 46}` → `"duty_pct": 53`. Leave the `35/50/62/72/90/95` points unchanged.

- [ ] **Step 4: Verify the two files are identical**

Run: `python -c "import json,pathlib; a=json.loads(pathlib.Path('config/control.release.json').read_text()); b=json.loads(pathlib.Path('release/control.json').read_text()); print('IDENTICAL' if a==b else 'MISMATCH')"`
Expected: `IDENTICAL`

- [ ] **Step 5: Run the Task 1 test to verify it passes**

Run: `python -m pytest tests/test_config_contracts.py::ConfigContractTests::test_feat0024_intake_lanes_lead_under_load -v`
Expected: PASS

- [ ] **Step 6: Run the full config-contract + cooling-policy + spec suites**

Run: `python -m pytest tests/test_config_contracts.py tests/test_machine_cooling_policy.py tests/test_feature_specs.py -q`
Expected: PASS (the existing `>= 4%` front spacing, soft-floor, no-mirror/stagger, idle-low-end, and `max_setpoint_step_pct < 1.0` contracts stay green because no idle knot, `min_duty`, exhaust lane, or `< 1.0` step changed).

- [ ] **Step 7: Run the full local CI (no publish)**

Run: `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`
Expected: green (CTest + pytest). No `src/` change, so the C++ build is unaffected; this confirms nothing else regressed.

- [ ] **Step 8: Commit**

```bash
git add config/control.release.json release/control.json
git commit -m "feat(feat-0024): intake lanes lead under load (config surge-and-hold)"
```

---

## Task 3: Lockstep documentation updates

Covers the `AGENTS.md` §Change Checklist obligations for a control-behavior/curve-value change. No REQ verification here; this keeps the maintained docs consistent with the shipped config.

**Files:**
- Modify: `docs/response-evaluation-tuning-plan.md` (record the FEAT-0024 retune iteration + that Pass-1/Pass-3 evidence is owed)
- Modify: `docs/COOLING_STRATEGY.md` (intake `response_intent` prose: intakes lead under load)
- Modify: `config/machines/snd-desk.cooling.policy.json` (the `2`/`3`/`4` `response_intent` prose only — NOT the frozen `reference_static_low_load_rpm` / `release_min_duty_pct` / `soft_floor_curve` fields)
- Modify: `docs/CONTROL_PIPELINE_MATH.md` (§13 real-data validation note placeholder, filled after Pass-3)

- [ ] **Step 1: Add the tuning-plan iteration record**

In `docs/response-evaluation-tuning-plan.md`, under "Recent Validation Evidence" or a new "FEAT-0024 intake-lead retune" subsection, record: the intake-lane deltas (cite the candidate table), that the change is config-only/rise-asymmetric/idle-unchanged, and that Pass-1 (idle unchanged) + Pass-3 (intake-lead margin) are owed before adoption. Do not assert results yet.

- [ ] **Step 2: Update the intake response-intent prose**

In `docs/COOLING_STRATEGY.md` (Fan Inventory "Response intent" column / Fan-Relationship Rules) and the matching `config/machines/snd-desk.cooling.policy.json` `fans[].response_intent` prose for channels `2`/`3`/`4`, add one clause each that the lane leads the exhausts under load (earlier `gpu_airflow` onset; faster rate; ch4 the steeper `cpu_override` mid-band). Change prose only; leave every numeric policy field (reference RPM, min-duty, soft-floor) untouched.

- [ ] **Step 3: Add the §13 validation-note placeholder**

In `docs/CONTROL_PIPELINE_MATH.md` §13, add a dated line noting the FEAT-0024 intake-lane coefficient change (config-only; identity unchanged) with "validation: pending Pass-3" — to be completed in Task 4.

- [ ] **Step 4: Verify docs read back and the spec gate still passes**

Run: `python -m pytest tests/test_feature_specs.py tests/test_machine_cooling_policy.py -q`
Expected: PASS (prose-only policy edits do not change the asserted numeric fields).

- [ ] **Step 5: Commit**

```bash
git add docs/response-evaluation-tuning-plan.md docs/COOLING_STRATEGY.md config/machines/snd-desk.cooling.policy.json docs/CONTROL_PIPELINE_MATH.md
git commit -m "docs(feat-0024): record intake-lead retune + intake response-intent prose"
```

---

## Task 4: Live validation — Pass-1 idle + Pass-3 combined load (REQ-INLEAD-06, M)

This is an explicit live-runtime task (`AGENTS.md` §Live Runtime Safety). It captures the before/after evidence that settles the candidate magnitudes and confirms the intake-lead margin and unchanged idle. **Do not change `release/control.json` live yet** — Pass-1/Pass-3 run the candidate config under the operator's deploy in Task 5, OR a pre-deploy comparison capture is taken first; sequence with the operator.

**Files:**
- Create: `docs/feat-0024-intake-lead-evidence-2026-06-25.md` (capture summary)

- [ ] **Step 1: Capture the pre-change baseline** — with the current shipped config live, capture a Pass-1 idle hold (10 min) and a Pass-3 combined CPU+GPU load run (per `docs/response-evaluation-tuning-plan.md` Pass 1 / Pass 3 procedures). Record per-channel first-duty-increase time and ramp time, CPU Tctl and GPU memory p50/p90/max, and idle per-channel `last_setpoint_pct` / RPM.

- [ ] **Step 2: Deploy the candidate config and re-capture** — after Task 5 deploys the candidate `release/control.json`, repeat Pass-1 and Pass-3.

- [ ] **Step 3: Analyze** — run `svg-mb-control analyze ingest` + `svg-mb-control analyze report` on both runs. Confirm: (a) intake lanes `2`/`3`/`4` reach first-duty-increase and complete their ramp *before* the exhaust lanes on the Pass-3 climb; (b) CPU Tctl p90 `< 88 C` / max `< 92 C` and GPU memory p90 `< 70 C` / max `< 74 C` (acceptance band); (c) no `control_loop.authority_reasserted` events after startup; (d) Pass-1 idle per-channel setpoint/RPM unchanged vs the pre-change baseline (idle untouched).

- [ ] **Step 4: Record evidence** — write `docs/feat-0024-intake-lead-evidence-2026-06-25.md` with the before/after metrics and the verdict per REQ-INLEAD-06. If the intake-lead margin or the acceptance band is not met, tune the candidate magnitudes within the band (Task 2 values) and re-capture before adoption.

- [ ] **Step 5: Fill the verification log** — set FEAT-0024 §14 and `docs/TRACEABILITY.md` §3 results for REQ-INLEAD-01..06 (T from Tasks 1-2; M from this task; R by review), and promote the spec status from `Draft` to `Accepted`/`Implemented` per `docs/features/README.md` lifecycle once all gates pass. Commit.

---

## Task 5: Live deploy with rollback (explicit live-runtime task)

Operator-authorized. Deploys the validated config and verifies, with a clean rollback path.

- [ ] **Step 1: Snapshot rollback copy** — back up the current live `release/control.json` (e.g. `release/control.json.pre-feat0024`).
- [ ] **Step 2: Deploy** — put the candidate `release/control.json` in place (already edited in Task 2) and restart the controller via the documented path (the control loop reads config at start; FEAT-0023/0003 switch by restart).
- [ ] **Step 3: Verify** — confirm `control_runtime.json` is healthy (6 channels, no open breakers, no sensor/write failures, `loop_slip_ms` within budget), then run the Task 4 Pass-1/Pass-3 re-capture.
- [ ] **Step 4: Rollback if needed** — if the intake-lead margin is wrong, idle changed, or any stop condition fires (`docs/response-evaluation-tuning-plan.md` Stop Conditions), restore the snapshot and restart.
- [ ] **Step 5: Record the deploy** — note the deployed git hash / config sha256 and the verify result in the evidence doc.

---

## Self-Review

**Spec coverage:** REQ-INLEAD-01 → Task 1/2 (rate raise + ch4-not-laggard); REQ-INLEAD-02 → Task 1/2 (GPU onset + ceiling lead); REQ-INLEAD-03 → Task 1/2 (ch4 `cpu_override` 72-86 steepen, `<=72`/`>=90` pinned, ch2/3 unchanged); REQ-INLEAD-04 → existing low-end test + Task 1 ch4 `<=72` pins + Task 2 leaves idle knots/`min_duty` untouched; REQ-INLEAD-05 → Task 1/2 (fall not raised, exhausts frozen); REQ-INLEAD-06 → Task 4 (Pass-1/Pass-3 M). Docs obligations → Task 3. Deploy → Task 5. No gap.

**Placeholder scan:** the only deferred content is the Task 4/5 live metrics (inherently captured at run time) and the §13 "validation: pending" note (completed in Task 4) — both explicit, not vague.

**Type/value consistency:** the candidate table, the Task 2 edits, and the Task 1 test assertions use the same numbers (rise 125/125/120; step 0.95; gpu start 58, max 12/12/10; ch4 alpha 0.014; ch4 `cpu_override` 82→50/86→53; exhausts 75/0.6-0.8/64/4-5; fall 45/45/25, 0.006/0.006/0.003, 120/120/90). Effective-ceiling formula matches FEAT-0017 and the spec.
