# CPU Energy Quarantine-Exit Evidence — session 4 — 2026-06-25

Auto-scored by `scripts/score_energy_session.py` from `release\runtime\experiments\energy-quarantine\sweeper-on\manifest.json`. One of >=3 independent sessions; promotion to `validated` stays a manual maintainer step (decision §Quarantine).

- Session CSV: `D:\Development\Thermals\SVG-MB\SVG-MB-Control\release\runtime\experiments\energy-quarantine\sweeper-on\session.csv` (5268 rows)
- git `ba83aed14a58`  config `45a0a1c732a0`  session_start `2026-06-25T05:32:27`
- Cycles captured: True   Rehearse: False
- Markers: enable=quarantine revert=quarantine

**Result: 5 PASS, 0 FAIL, 1 MANUAL/INCOMPLETE.**

| # | Criterion | Verdict | Detail |
|---|---|---|---|
| 1 | Counter continuity across >=1 wrap | **PASS** | negative deltas=0, ~1.82 wraps over 119476 J; idle blanks=0; load blanks=0/709 (loop-starvation artifact, blanks cleanly -> crit 5) |
| 2 | Plausible range + load tracking | **PASS** | idle=67.5 W, load=165.7 W, cooldown=62.9 W (ceiling 400) |
| 3 | +/-15% external SMU cross-check | **PASS** | RAPL steady=165.9 W vs SMU=165.0 W -> +0.5% (tol +/-15%); LHM/RAPL-derived (cross-check, load-fragile)=164.4 |
| 4 | Effective-frequency validity (cycles) | **MANUAL** | dAPERF/dMPERF idle=1.242, load=1.228; derived idle=5339 MHz, load=5278 MHz @ P0 4300 (no locked setpoint; supply --locked-mhz for the Option B cross-check) (cycle source: allcore) |
| 5 | Fault behavior (no false zero / clean) | **PASS** | quarantine rows=5268, false-zero deltas=0, session reached cooldown=True |
| 6 | No-disturbance vs disabled baseline | **PASS** | energy-on no-load p95 1.73->1.78 ms (d+0.05, baseline SD 0.46); overrun/h 0.0->0.0 (load phase excluded) |

## Phase watts (time-weighted, distinct windows)
- idle 67.5 W | load 165.7 W | cooldown 62.9 W | steady 165.9 W

Criterion 3 manual fallback: if no SMU steady-window sample was harvested, record a Ryzen Master / confirmed-SMU HWiNFO average over `2026-06-25T05:39:10..2026-06-25T05:48:10` and recompute the % delta by hand.

