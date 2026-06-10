# CPU Energy Quarantine-Exit Evidence — session 1 — 2026-06-10

Auto-scored by `scripts/score_energy_session.py` from `D:\Development\Thermals\SVG-MB\SVG-MB-Control\release\runtime\experiments\energy-quarantine\session1\manifest.json`. One of >=3 sessions over >=7 days; promotion to `validated` stays a manual maintainer step (decision §Quarantine).

- Session CSV: `D:\Development\Thermals\SVG-MB\SVG-MB-Control\release\runtime\experiments\energy-quarantine\session1\session.csv` (3164 rows)
- git `c4b6986f45a3`  config `b51e542a6138`  session_start `2026-06-10T16:31:14`
- Cycles captured: True   Rehearse: False
- Markers: enable=quarantine revert=disabled

**Result: 5 PASS, 0 FAIL, 1 MANUAL/INCOMPLETE.**

| # | Criterion | Verdict | Detail |
|---|---|---|---|
| 1 | Counter continuity across >=1 wrap | **PASS** | negative deltas=0, ~1.73 wraps over 113341 J; idle blanks=0; load blanks=73/319 (loop-starvation artifact, blanks cleanly -> crit 5) |
| 2 | Plausible range + load tracking | **PASS** | idle=73.7 W, load=190.6 W, cooldown=62.3 W (ceiling 400) |
| 3 | +/-15% external SMU cross-check | **PASS** | RAPL steady=190.7 W vs SMU=190.2 W -> +0.2% (tol +/-15%); LHM/RAPL-derived (cross-check, load-fragile)=191.0 |
| 4 | Effective-frequency validity (cycles) | **MANUAL** | dAPERF/dMPERF idle=1.252, load=1.192 (x base freq = effective; analyzer v10 derivation not built) |
| 5 | Fault behavior (no false zero / clean) | **PASS** | quarantine rows=3164, false-zero deltas=0, session reached cooldown=True |
| 6 | No-disturbance vs disabled baseline | **PASS** | energy-on no-load p95 1.73->1.71 ms (d-0.02, baseline SD 0.70); overrun/h 6.0->6.0 (load phase excluded) |

## Phase watts (time-weighted, distinct windows)
- idle 73.7 W | load 190.6 W | cooldown 62.3 W | steady 190.7 W

Criterion 3 manual fallback: if no SMU steady-window sample was harvested, record a Ryzen Master / confirmed-SMU HWiNFO average over `2026-06-10T16:38:16..2026-06-10T16:47:16` and recompute the % delta by hand.

