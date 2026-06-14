# CPU Energy Quarantine-Exit Evidence — session 2 — 2026-06-12

Auto-scored by `scripts/score_energy_session.py` from `D:\Development\Thermals\SVG-MB\SVG-MB-Control\release\runtime\experiments\energy-quarantine\session2_20260612_183956\manifest.json`. One of >=3 independent sessions; promotion to `validated` stays a manual maintainer step (decision §Quarantine).

- Session CSV: `D:\Development\Thermals\SVG-MB\SVG-MB-Control\release\runtime\experiments\energy-quarantine\session2_20260612_183956\session.csv` (5384 rows)
- git `01452dc42d69`  config `b51e542a6138`  session_start `2026-06-12T18:39:59`
- Cycles captured: True   Rehearse: False
- Markers: enable=quarantine revert=disabled

**Result: 5 PASS, 0 FAIL, 1 MANUAL/INCOMPLETE.**

| # | Criterion | Verdict | Detail |
|---|---|---|---|
| 1 | Counter continuity across >=1 wrap | **PASS** | negative deltas=0, ~2.03 wraps over 133306 J; idle blanks=0; load blanks=0/719 (loop-starvation artifact, blanks cleanly -> crit 5) |
| 2 | Plausible range + load tracking | **PASS** | idle=68.5 W, load=184.6 W, cooldown=46.7 W (ceiling 400) |
| 3 | +/-15% external SMU cross-check | **PASS** | RAPL steady=184.7 W vs SMU=184.6 W -> +0.1% (tol +/-15%); LHM/RAPL-derived (cross-check, load-fragile)=184.9 |
| 4 | Effective-frequency validity (cycles) | **MANUAL** | dAPERF/dMPERF idle=1.228, load=1.198 (x base freq = effective; analyze report --p0-mhz <base> derives it; promotion stays manual) |
| 5 | Fault behavior (no false zero / clean) | **PASS** | quarantine rows=5384, false-zero deltas=0, session reached cooldown=True |
| 6 | No-disturbance vs disabled baseline | **PASS** | energy-on no-load p95 1.77->1.77 ms (d-0.00, baseline SD 0.45); overrun/h 0.0->0.0 (load phase excluded) |

## Phase watts (time-weighted, distinct windows)
- idle 68.5 W | load 184.6 W | cooldown 46.7 W | steady 184.7 W

Criterion 3 manual fallback: if no SMU steady-window sample was harvested, record a Ryzen Master / confirmed-SMU HWiNFO average over `2026-06-12T18:47:01..2026-06-12T18:56:02` and recompute the % delta by hand.

