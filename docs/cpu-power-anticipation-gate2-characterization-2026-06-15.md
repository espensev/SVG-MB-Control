# CPU Power-Anticipation — Gate 2 Characterization Measurements — 2026-06-15

Status: **measurement record (neutral).** This document records the inputs,
methods, and numeric outputs of the Gate 2(a) characterization for the proposed
power-anticipation boost. It draws no go/no-go conclusion. Interpretation against
the plan's decision criteria is owned by
`docs/cpu-power-feedforward-plan-2026-06-10.md` §5 (Gates) and §6 (Evaluation),
and by the maintainer; nothing here authorizes or rejects work.

Scope:

- In scope: data provenance, the two measurement methods and their parameters,
  the per-session numeric results with machine-readable dataset files, and the
  measurement-resolution limits plus the capture campaign that would lift them
  (Appendix C).
- Out of scope: any judgment of whether the measured values meet or fail a gate,
  any comparison verdict between signals, and any recommendation.

## 1. Source data

Three energy-enabled capture sessions from the package-energy quarantine set.
Each session has a five-minute idle phase, a twelve-minute synthetic-load phase,
and a five-minute cooldown phase, recorded at the shipped 250 ms control tick.
The raw inputs are each session's `session.csv` and `manifest.json`.

| Session | `session_start` | `git_hash` | `config_sha256` (12) | rows | mean s/row | `load_threads` | source dir |
|---|---|---|---|---|---|---|---|
| 1 | 2026-06-10T16:31:14 | `c4b6986f45a3` | `b51e542a6138` | 3164 | 0.427 | 0 (all-core) | `release/runtime/experiments/energy-quarantine/session1` |
| 2 | 2026-06-12T18:39:59 | `01452dc42d69` | `b51e542a6138` | 5384 | 0.251 | 28 | `release/runtime/experiments/energy-quarantine/session2_20260612_183956` |
| 3 | 2026-06-14T02:33:18 | `01452dc42d69` | `b51e542a6138` | 5384 | 0.251 | 28 | `release/runtime/experiments/energy-quarantine/session3_20260614_023314` |

Phase boundaries (seconds relative to `manifest.phases.load_start`):

| Session | idle_start | load_start | steady_start | steady_end | cooldown_start |
|---|---|---|---|---|---|
| 1 | −300 | 0 | +120 | +660 | +720 |
| 2 | −300 | 0 | +120 | +661 | +721 |
| 3 | −300 | 0 | +120 | +660 | +720 |

Energy-window coverage (distinct `cpu_power_sample_id` windows; a window is
"blanked" when its derived watts is undefined; acquisition marker per row):

| Session | distinct windows | blanked windows | acquisition rows |
|---|---|---|---|
| 1 | 981 | 36 | `quarantine`: 3164 |
| 2 | 1345 | 0 | `quarantine`: 5384 |
| 3 | 1345 | 0 | `quarantine`: 5384 |

## 2. Signal definitions

- **Package watts** `W` is derived per row as
  `W = cpu_pkg_energy_delta_uj / cpu_power_window_ms / 1000`. The producer
  mirrors one ~1 s energy window across the 250 ms ticks of that window, so `W`
  repeats across the ticks of a window; a row with a missing or non-positive
  window, a missing or negative delta, or `cpu_power_sample_id` in `{"", "0"}`
  yields an undefined `W`.
- **`system_cpu_busy_pct`** is the FEAT-0002 whole-system busy percentage logged
  each tick.
- **`cpu_tctl_c`** is the AMD Tctl die temperature logged each tick.

## 3. Method A — lag-scanned level correlation

Tool: `scripts/analyze_power_lead.py` (unchanged; `--max-lag-s 30`). For an
ordered driver/response pair, the method computes the Pearson correlation of the
driver against the response shifted by each non-negative lag from 0 to 30 s, and
reports the lag with the highest correlation (`lead_s`), that correlation (`r`),
and the correlation at lag 0 (`r_at_zero`). It is computed for four pairs:
package watts and `system_cpu_busy_pct`, each against `cpu_tctl_c` and against a
±1 s central-difference smoothed `dTctl/dt`.

| Session | watts→Tctl | watts→dTctl/dt | busy→Tctl | busy→dTctl/dt |
|---|---|---|---|---|
| 1 | lead 1.28 s, r 0.951 (r@0 0.949) | lead 0.00 s, r 0.057 | lead 0.85 s, r 0.923 (r@0 0.922) | lead 0.00 s, r 0.043 |
| 2 | lead 0.50 s, r 0.992 (r@0 0.992) | lead 0.00 s, r 0.039 | lead 0.50 s, r 0.983 (r@0 0.983) | lead 0.00 s, r 0.031 |
| 3 | lead 0.50 s, r 0.995 (r@0 0.995) | lead 0.00 s, r 0.040 | lead 0.50 s, r 0.995 (r@0 0.995) | lead 0.00 s, r 0.030 |

Full precision: `datasets/lag_scan_session{1,2,3}.json`. The lag resolution equals
one row (Session 1 ≈ 0.43 s; Sessions 2–3 ≈ 0.25 s). See Appendix A for the
property that motivated adding Method B.

## 4. Method B — event-onset timing (the suggested approach)

Tool: `scripts/analyze_power_onset.py`. The method measures, for each of package
watts, `system_cpu_busy_pct`, and `cpu_tctl_c`, the time relative to
`manifest.phases.load_start` at which the signal first crosses a fixed fraction
of its own idle→steady amplitude, then differences those crossing times.

Definitions (default parameters in Appendix B):

1. **idle level** = the 10th percentile of the signal over the pre-load window
   `[load_start − 300 s, load_start − 10 s]`. A low quantile is used so that
   background activity present on a live machine during the nominal idle phase
   does not raise the idle estimate.
2. **steady level** = the median of the signal over the manifest steady window
   `[steady_start, steady_end]`.
3. **threshold** = `idle + 0.5 × (steady − idle)`. Using each signal's own
   amplitude makes the crossing time independent of the signal's absolute
   baseline and units.
4. **crossing time** = the time of the first row, within the onset window
   `[load_start − 60 s, load_start + 150 s]`, at which the signal stays strictly
   above its threshold for at least 2 s of consecutive rows.

Levels and thresholds (W for watts, % for busy, °C for Tctl):

| Session | watts idle→steady (thr) | busy idle→steady (thr) | Tctl idle→steady (thr) |
|---|---|---|---|
| 1 | 60.5 → 190.4 (125.4) | 7.1 → 100.0 (53.5) | 60.0 → 82.4 (71.2) |
| 2 | 59.7 → 184.5 (122.1) | 7.5 → 92.1 (49.8) | 58.3 → 81.1 (69.7) |
| 3 | 53.2 → 190.0 (121.6) | 7.0 → 96.7 (51.8) | 62.4 → 82.9 (72.6) |

Crossing times and inter-signal offsets (seconds vs `load_start`):

| Session | t_watts | t_busy | t_Tctl | watts−busy | Tctl−watts | Tctl−busy |
|---|---|---|---|---|---|---|
| 1 | +1.0 | +1.0 | +1.0 | 0.0 | 0.0 | 0.0 |
| 2 | +2.0 | +2.0 | +1.0 | 0.0 | −1.0 | −1.0 |
| 3 | +1.0 | +1.0 | +0.0 | 0.0 | −1.0 | −1.0 |

Tctl reference levels in the same sessions (named control thresholds from
`release/control.json`: `midband_pressure_start_c` = 64 °C; lowest
`thermal_pressure_start_c` = 84 °C). Crossing times are over the full session,
relative to `load_start`:

| Session | Tctl max (°C) | first Tctl > 64 °C | first Tctl > 84 °C |
|---|---|---|---|
| 1 | 84.6 | −284 s | +406 s |
| 2 | 82.4 | −302 s | not reached |
| 3 | 84.4 | −302 s | +345 s |

Full precision: `datasets/onset_session{1,2,3}-*.json` and
`datasets/event_onset_summary.csv`.

## 5. Datasets

All files are under
`release/runtime/experiments/energy-quarantine/gate2-characterization-2026-06-15/`:

| File | Contents |
|---|---|
| `datasets/trace_session{1,2,3*}.csv` | Tidy per-row signal extract (one row per logged tick): `t_rel_load_start_s`, `wall_clock`, `cpu_pkg_watts`, `system_cpu_busy_pct`, `cpu_tctl_c`, `cpu_power_sample_id`. The six columns the analysis reads, lifted out of the 100+-column `session.csv`, sufficient to plot the curves and recompute either method independently. |
| `datasets/lag_scan_session{1,2,3}.json` | Method A output per session (leads, correlations, watts distributions, window coverage, session metadata). |
| `datasets/onset_session{1,2,3}-*.json` | Method B output per session (levels, thresholds, crossing times, offsets, Tctl reference crossings, parameters). |
| `datasets/event_onset_summary.csv` | Method B, one row per session, 24 columns (Appendix B). |
| `README.md` | Dataset index and regeneration commands. |

Watts distributions binned by `system_cpu_busy_pct < 5 %` (idle) vs `≥ 5 %`
(load), from Method A:

| Session | idle n | idle p50 | idle p95 | load n | load p50 | load p95 | load max |
|---|---|---|---|---|---|---|---|
| 1 | 130 | 53.9 W | 59.7 W | 2957 | 70.4 W | 192.1 W | 200.2 W |
| 2 | 815 | 45.3 W | 49.7 W | 4565 | 183.8 W | 187.6 W | 195.2 W |
| 3 | 0 | n/a | n/a | 5380 | 187.8 W | 193.8 W | 198.2 W |

(The Method A idle bin uses a `busy < 5 %` filter; the Method B idle level uses a
pre-load-window quantile. They are different estimators and are not expected to
match. Session 3 has no `busy < 5 %` rows, so its Method A idle bin is empty.)

### Regeneration

```
# from repo root; read-only, stdlib Python only
OUT=release/runtime/experiments/energy-quarantine/gate2-characterization-2026-06-15/datasets
for n in 1 2 3; do
  python scripts/analyze_power_lead.py --csv <session$n>/session.csv \
    --max-lag-s 30 --format json --out $OUT/lag_scan_session$n.json
done
python scripts/analyze_power_onset.py \
  --session-dir <session1> --session-dir <session2> --session-dir <session3> \
  --json-dir $OUT --csv-out $OUT/event_onset_summary.csv --trace-dir $OUT
```

## Appendix A — property motivating Method B

For two signals that both undergo a single step change at the same time, the
Pearson correlation between their levels, as a function of relative lag, is
maximized at or near zero lag, and the location of that maximum is insensitive
to any sub-step onset offset between the two signals. A correlation against a
smoothed derivative is non-monotonic when the driver is elevated both at the
rising edge and during the steady plateau. Method B measures threshold-crossing
times directly and does not depend on the lag of maximum correlation. Both
methods are reported here; this appendix states why a crossing-time measure was
added, not which value any gate requires.

## Appendix B — parameters

Method A (`analyze_power_lead.py`): `--max-lag-s 30`, `--idle-busy-pct 5`,
`--deriv-half-window-s 1`.

Method B (`analyze_power_onset.py`) defaults used here: `span_fraction 0.5`,
`idle_quantile 0.10`, `pre_load_window_s 300`, `onset_window_pre_s 60`,
`onset_window_post_s 150`, `sustain_s 2.0`. The per-session `sustain_rows` is
`max(3, round(sustain_s / mean_s_per_row))` = 5 for Session 1 and 8 for
Sessions 2–3.

`event_onset_summary.csv` columns: `session_label`, `session_start`, `git_hash`,
`load_threads`, `rows`, `tick_seconds`, `watts_idle`, `watts_steady`,
`watts_threshold`, `busy_idle`, `busy_steady`, `busy_threshold`, `tctl_idle`,
`tctl_steady`, `tctl_threshold`, `onset_watts_s`, `onset_busy_s`, `onset_tctl_s`,
`watts_minus_busy_s`, `tctl_minus_watts_s`, `tctl_minus_busy_s`, `tctl_max_c`,
`tctl_cross_64c_s`, `tctl_cross_84c_s`.

## Appendix C — measurement-resolution limits and the campaign that would resolve the deferred onset-lead question

This appendix records what the two methods can and cannot resolve from the
present captures, the measured context around that, and the capture campaign
that would lift those limits. It states limits and measured values only; the
go/no-go remains owned by the plan §5/§6. All values are recomputed from the
`datasets/trace_session*.csv` extracts.

### C.1 Onset-lead resolution

- Energy windows are ~1 s and are mirrored across the 250 ms ticks of each
  window (§2), so derived watts changes at about one quarter of the row rate.
  The onset estimator's resolution is one row (Session 1 ≈ 0.43 s,
  Sessions 2–3 ≈ 0.25 s).
- The load stimulus is a single maximal step (§1), giving one onset event per
  session; both watts and `system_cpu_busy_pct` cross their thresholds within
  one window of `load_start`.
- Consequence: the watts-vs-busy onset offset is resolvable to ±1 row at this
  cadence (measured 0 s, §4). A power→Tctl lead shorter than the energy-window
  period is not resolvable from these captures. The supported statement is
  "watts-vs-busy onset offset is 0 ± 1 tick; a sub-window power lead is
  unresolvable at this cadence," not "no lead exists."

### C.2 The pre-load ("idle") phase is not quiescent

Per-session pre-load (`t_rel_load_start_s` < 0) distributions:

| Session | busy p50/p95 (%) | Tctl p50 (°C) / rows >64 °C | watts p95/max (W) | `busy<5%` floor p50/p95 (W) |
|---|---|---|---|---|
| 1 | 10.7 / 20.6 | 62.9 / 39 % | 95.9 / 107.9 | 53.9 / 59.7 (§5) |
| 2 | 9.7 / 15.3 | 59.8 / 4 % | 83.2 / 94.0 | 45.3 / 49.7 (§5) |
| 3 | 9.3 / 15.8 | 63.4 / 22 % | 77.6 / 121.1 | none (0 rows) |

- The operative idle watts ceiling for any `start_w` siting is the pre-load
  excursion p95/max (94–121 W), not the `busy<5%` quiet floor (45–60 W): the
  quiet-floor filter excludes the background activity that a `start_w` would need to
  clear. The load plateau is ~184–190 W (§4).
- The `midband_pressure` term (start 64 °C) sees a session-dependent pre-load
  active fraction (Session 1 39 %, Session 3 22 %, Session 2 4 %).

### C.3 Session heterogeneity, coverage, and what the methods do not measure

- Session 1 differs from Sessions 2–3 in binary (`c4b6986f45a3` vs
  `01452dc42d69`), load (all-core vs 28-thread), tick (0.43 vs 0.25 s/row), and
  blank rate (2.4 % vs 0.1 % undefined-watts rows). Sessions 2–3 are the
  matched-binary, matched-load pair; the watts-vs-busy onset offset is 0 s in
  all three (§4), i.e. invariant across those differences.
- Load-phase `system_cpu_busy_pct` is occupancy-dependent: all-core (Session 1)
  p50 100 %; 28-thread (Sessions 2–3) p50 92–97 %, not pinned at 100 %.
- Session 3 has no `busy<5%` rows, so the `busy<5%` idle-floor distribution
  rests on Sessions 1–2.
- Relative-amplitude fidelity between watts and `system_cpu_busy_pct` under
  busy saturation (full occupancy with variable per-core intensity, e.g. AVX
  vs integer) is a steady/high-load property and is not measured by either
  method here, which compare onset crossing-time order on a single step. Plan
  §1 bounds the anticipation term to transients, so that amplitude property
  pertains to a separate mechanism rather than to the §4 boost.

### C.4 Capture campaign that would resolve the deferred onset-lead question

Resolving a sub-window power-vs-busy lead would require:

- **(a) Un-mirrored / sub-second energy sampling**, so watts is not ~4× coarser
  than the tick. This is a producer-side change: the present ~1 s window read
  outside the `Global\Access_PCI` mutex is a disturbance-mitigation design
  (`docs/cpu-work-energy-acquisition-decision-2026-06-07.md`), so faster
  sampling re-opens criterion 6 (no-disturbance) and the Gate 1 always-on
  decision.
- **(b) A ramped or staircase load** rather than a single step, so the signals
  separate in time.
- **(c) Load at full core occupancy with variable per-core intensity** (e.g.
  AVX vs integer) — the regime where `system_cpu_busy_pct` saturates near
  100 % while watts continues to track frequency and residency.
- **(d) ≥ 3 independent sessions on one binary with a finer onset estimator**
  (sub-tick interpolation, multiple load events per session); resolution is
  bounded by (a).
