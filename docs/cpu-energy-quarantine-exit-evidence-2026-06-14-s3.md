# CPU Energy Quarantine-Exit Evidence — session 3 — 2026-06-14

Auto-scored by `scripts/score_energy_session.py` from `D:\Development\Thermals\SVG-MB\SVG-MB-Control\release\runtime\experiments\energy-quarantine\session3_20260614_023314\manifest.json`. One of >=3 independent sessions; promotion to `validated` stays a manual maintainer step (decision §Quarantine).

- Session CSV: `D:\Development\Thermals\SVG-MB\SVG-MB-Control\release\runtime\experiments\energy-quarantine\session3_20260614_023314\session.csv` (5384 rows)
- git `01452dc42d69`  config `b51e542a6138`  session_start `2026-06-14T02:33:18`
- Cycles captured: True   Rehearse: False
- Markers: enable=quarantine revert=disabled

**Result: 5 PASS, 0 FAIL, 1 MANUAL/INCOMPLETE.**

| # | Criterion | Verdict | Detail |
|---|---|---|---|
| 1 | Counter continuity across >=1 wrap | **PASS** | negative deltas=0, ~2.10 wraps over 137379 J; idle blanks=0; load blanks=0/718 (loop-starvation artifact, blanks cleanly -> crit 5) |
| 2 | Plausible range + load tracking | **PASS** | idle=62.1 W, load=190.3 W, cooldown=67.1 W (ceiling 400) |
| 3 | +/-15% external SMU cross-check | **PASS** | RAPL steady=190.5 W vs SMU=190.3 W -> +0.1% (tol +/-15%); LHM/RAPL-derived (cross-check, load-fragile)=190.4 |
| 4 | Effective-frequency validity (cycles) | **MANUAL** | dAPERF/dMPERF idle=1.231, load=1.193 (x base freq = effective; analyze report --p0-mhz <base> derives it; promotion stays manual) |
| 5 | Fault behavior (no false zero / clean) | **PASS** | quarantine rows=5384, false-zero deltas=0, session reached cooldown=True |
| 6 | No-disturbance vs disabled baseline | **PASS** | energy-on no-load p95 1.63->1.58 ms (d-0.05, baseline SD 0.44); overrun/h 0.9->0.0 (load phase excluded) |

## Phase watts (time-weighted, distinct windows)
- idle 62.1 W | load 190.3 W | cooldown 67.1 W | steady 190.5 W

Criterion 3 manual fallback: if no SMU steady-window sample was harvested, record a Ryzen Master / confirmed-SMU HWiNFO average over `2026-06-14T02:40:20..2026-06-14T02:49:20` and recompute the % delta by hand.

