# CPU peak-temperature excursion investigation — 2026-06-09 04:42

- **Authored:** 2026-06-15
- **Event:** 2026-06-09 ~04:42 (local)
- **Status:** Investigation note (read-only analysis of archived logs; no code or
  config changed)
- **Scope:** Answer three questions about the hottest CPU record in the live
  archives — *when* it happened, *under what load*, and *whether the CPU was
  throttling* — and *validate* the `amd_sensor_summary` field the answer rests on.

---

## 1. TL;DR

- **When:** 2026-06-09 **04:42:42**, in
  `release/runtime/logs/archive/svg_mb_control_control-loop_20260609_023240.csv`.
  A second, smaller touch occurred at **04:58:47**, ~16 min later.
- **Load:** External workload (the controller's own `process_cpu_pct` was
  0.156% at the peak). It was **bursty/intermittent**, not a sustained soak:
  over the ~03:42–04:58 envelope median CPU busy was **11.6%**, punctuated by
  bursts to 100%. Package power and effective clock were **not logged** that
  night (`cpu_pkg_energy_acquisition=disabled`; the APERF/MPERF cycle columns
  did not exist yet).
- **Throttling:** **Yes, briefly and protectively.** On the hard bursts the die
  rode ~95 °C (the AMD Granite Ridge `Tjmax`) = thermally-limited boost. For a
  fraction of a second at 04:42:42 the control temperature (`Tctl`) reached
  **107.1 °C**, ~12 °C over `Tjmax` = hard thermal-limiting (the CPU's own
  thermal management cutting clocks). No `THERMTRIP`/shutdown occurred. The
  *magnitude* of clock loss is **unquantifiable for 06-09** (no clock telemetry
  that day).
- **The excursion to ~107 °C is real, not a sensor glitch** — established by the
  smoothed `Tctl`, which is slew-limited (§7) yet climbed a monotonic 7-sample
  ramp to 107.1 °C; a slew-limited signal can only *reach* 107.1 °C if the raw die
  was ≥107 °C. The raw CCD2 reading of 108.875 °C is its instantaneous value at
  that instant — directionally consistent, but CCD2 is itself glitching in this
  window (§6), so the **reliable figure is ~107 °C** (`Tctl`), not the exact
  108.875 °C (CCD2).
- **`amd_sensor_summary` validates as trustworthy** at the decode and transport
  level. See §5. The only residual read-quality nit is narrow (§6) and does not
  change the conclusion.

> Two earlier verbal claims made during this investigation are **retracted**:
> (a) that the excursion was a CCD2 sensor glitch with a real peak of only
> ~95 °C — the ~107 °C excursion is real (95 °C was the burst baseline), though
> the *exact* 108.875 °C is CCD2's own reading and is not independently trustworthy;
> (b) that a 45 °C drop in 250 ms is "thermodynamically impossible." Both are
> wrong; the corrected, evidence-backed positions are in §4, §5, and §7.

---

## 2. When & where

| Field | Value |
| --- | --- |
| `wall_clock` | `2026-06-09T04:42:42` |
| File | `…/logs/archive/svg_mb_control_control-loop_20260609_023240.csv` |
| `cpu_max_c` | `108.875` (this is the CCD2 `Tdie` value) |
| `cpu_tctl_c` | `107.000` |
| CCD breakdown at peak | `Tctl/Tdie=107.000 \| CCD1 (Tdie)=92.750 \| CCD2 (Tdie)=108.875` |
| `system_cpu_busy_pct` | `100.0` |
| `process_cpu_pct` (controller) | `0.156` |
| Fan duty at peak (ch0..ch6) | 39 / 47 / 81 / 77 / 55 / 47 / 96 % |

The `Tctl` ramp into the peak is smooth and monotonic across ~8 samples (~2 s),
which is why the excursion is read as real rather than a single bad sample:

```
97.875 → 103.375 → 104.875 → 105.750 → 106.375 → 107.000 → 107.125
```

Excursion counts in that file (46 883 rows):

| Threshold | Samples | Span |
| --- | --- | --- |
| `cpu_max_c` ≥ 95 °C | 925 | 03:42:25 .. 04:58:48 |
| ≥ 100 °C | 24 | 04:42:40 .. 04:58:48 |
| ≥ 105 °C | 12 | 04:42:41 .. 04:58:47 |
| ≥ 107 °C | 9 | — |
| ≥ 108 °C | 1 | 04:42:42 |

The hot record is the worst point of a brief cluster, not a sustained state.

---

## 3. Load characterization

Windowed statistics from the peak file (`cpu_max_c` / `cpu_tctl_c` /
`system_cpu_busy_pct`):

| Window | `cpu_max` p50 | `cpu_max` p90 | `cpu_max` max | busy p50 | frac busy≥95% |
| --- | --- | --- | --- | --- | --- |
| ±30 s around peak | 95.12 | 98.75 | 108.88 | 100.0 | 1.00 |
| 04:40–04:50 | 83.12 | 95.00 | 108.88 | 100.0 | 0.93 |
| 03:42–04:58 envelope | 64.25 | 93.00 | 108.88 | 11.6 | 0.30 |

Reading: an otherwise idle machine (envelope median busy 11.6 %) ran repeated
heavy bursts overnight; on the bursts the die rode 93–95 °C with brief
overshoots. No process identity is in the telemetry (only the controller's own
process is sampled), so the workload is unnamed; the signature (overnight,
intermittent, single-CCD-skewed bursts — see the 16 °C CCD1/CCD2 split at the
peak) is consistent with a scheduled/background job rather than interactive use.

For contrast, the later **instrumented** sustained-100 %-load sessions (06-10,
06-14) topped out at **~86 °C** `cpu_max` while holding **~5.1 GHz**
(APERF/MPERF ratio ≈ 1.19) at ~190 W package — comfortably below `Tjmax`. Those
sessions never entered the 06-09 regime, so they cannot be used to bound the
06-09 clock loss; they only show that under steady, thermally-even load the
cooling solution is power-limited, not temperature-limited.

---

## 4. Throttling assessment

| Phase | Temperature evidence | CPU behavior |
| --- | --- | --- |
| Burst baseline | die ~93–95 °C | Thermally-limited boost — shedding some clock to hold `Tjmax` (95 °C). Normal Precision Boost. |
| 04:42:42 spike | `Tctl` 97.9 → **107.1 °C** (monotonic, ~2 s) | ~12 °C over `Tjmax` → hard thermal-limiting; the CPU's thermal management was actively cutting clock/voltage. |
| Recovery | `Tctl` slews back at ≤ 6.5 °C/s (§7) | Excursion receded over seconds; no shutdown. |

- `Tjmax` for AMD "Granite Ridge" (Ryzen 9000, Family 1Ah) is **95 °C**; the AMD
  thermal-management loop acts on `Tctl`, so a `Tctl` of 107 °C means it was
  protecting the part hard for that window.
- **No `THERMTRIP`/shutdown** (`THERMTRIP` is ~115 °C+; the system ran straight
  through and temperatures were normal before and after).
- **Clock-loss magnitude is unquantifiable for 06-09** — no APERF/MPERF that
  day. A repeat under the current build would capture it (cycle logging via
  `SVG_MB_CONTROL_CPU_CYCLES_MODE` exists now).
- **Rarity:** across **2 240 369** logged samples the die exceeded 105 °C exactly
  **~10 times**, all on this one night, in the two clusters above.

---

## 5. Validation of `amd_sensor_summary` (the source field)

The analysis treats `amd_sensor_summary` (and the derived `cpu_tctl_c` /
`cpu_max_c`) as ground truth, so the decode and read path were audited.

### 5.1 Decode math — correct and unit-tested
Source: `src/hardware/amd_decode.h`; tests: `tests/cpp/amd_decode_tests.cpp`
(10 cases).

- **Tctl/Tdie:** `temp = (raw >> 21) * 0.125 °C`, minus a fixed 49 °C when bit 19
  (`0x80000`) is set. **No upper clamp.** Therefore `Tctl = 107` is a faithful
  decode of the raw register, not a saturation ceiling. Hand-check:
  `0x41000000 >> 21 = 520 → 520 × 0.125 = 65 °C` (matches the test).
- **Per-CCD Tdie:** `temp = (raw & 0xFFF) * 0.125 − 305 °C`, with a validity gate
  `raw_12bit > 0 && temp < 125 °C`. Hand-check: `2840 × 0.125 − 305 = 50 °C`
  (matches the test). `107.000` and `108.875` decode from **different** raw codes
  (3296 vs 3311), so the decode is linear and faithful — a repeated 107.000 means
  the raw register repeatedly returned code 3296, an upstream effect, not a
  decode artifact.
- Field corroboration: across 2.24 M samples the decoded `Tctl` and CCD values
  track each other and load, and sit on the expected 0.125 °C grid.

### 5.2 Read transport — coherent and uncached
Source: `src/hardware/amd_reader.cpp`, `AmdReader::Sample()`.

- Each tick acquires the system-wide `Global\Access_PCI` mutex **once** and reads
  `Tctl` (SMN `0x59800`) then every CCD (`0x59B08 + 4·i`) under that single lock —
  an **interleave-free snapshot**. Consequence: when one row shows `Tctl`=107 next
  to `CCD≈62`, those registers **genuinely disagreed at read time** (SMU register
  update-skew), not a software interleave.
- Every read is a fresh `ioctl_read_smn` (`ReadSmnLocked`); there is **no
  software caching**, so a repeated identical value originates in the
  SMU/SMN register, not the reader.
- The per-CCD validity gate only rejects `≥ 125 °C`; a misread that decodes to
  60–124 °C (e.g. 108.875, or the 62 °C neighbor) passes through **unfiltered**,
  which is how the few anomalous CCD samples reach the log.
- `PciMutexLock` treats `WAIT_ABANDONED` as acquired, so a co-resident SMN reader
  (e.g. HWiNFO / Ryzen Master) dying mid-read can disturb a sample — a known
  source of transient AMD SMN reads.
- Model→CCD-base mapping (`SelectCcdLayout`) resolves this dual-CCD part to base
  `0x59B08`; the resulting per-CCD values are physically sane, confirming the
  base is correct for this CPU.
- `cpu_max_c` is the **max over all decoded sensors** (`FindMaxCpuTemperature`,
  `src/runtime/runtime_csv_rows.cpp`), and `cpu_tctl_c` is the `Tctl/Tdie`
  sample. So `cpu_max_c` follows whichever sensor is hottest — here the glitching
  CCD2 — while `cpu_tctl_c` always reports the slew-limited composite. That is why
  the headline `cpu_max_c` reads 108.875 °C (CCD2) while the reliable `cpu_tctl_c`
  reads 107.000 °C at the same instant.

### 5.3 Verdict
The decode is correct and tested; the read path is coherent and uncached; values
are faithful in aggregate. The ~107 °C excursion is established by the smoothed,
slew-limited `Tctl` **alone** (§7): `Tctl` never moves faster than ~2.4 °C/tick
across 2.24 M samples, so a monotonic ramp to 107.1 °C requires the raw die to
have reached ~107 °C. CCD2's 108.875 °C is consistent in direction but is **not**
independent corroboration — CCD2 is itself glitching in this window (§6). The
reliable figure is **~107 °C** (`Tctl`); CCD2's exact value is treated cautiously.

---

## 6. Residual read anomaly (low impact)

Within the 04:42 cluster CCD2 returned the **identical** raw code (107.000) on
**5 consecutive ticks** while `Tctl` was ramping. A live junction sensor jitters
±0.125–0.25 °C tick-to-tick (seen everywhere else), so 5 identical reads is
atypical and consistent with a brief register **non-refresh/stale latch** on
that CCD. One row (04:42:43) is internally skewed (`Tctl`=107.1 in the same
coherent snapshot as both CCDs ≈ 62). These bound the **same** ~107 °C that the
`Tctl` ramp independently establishes, so they do not change the conclusion. The
04:58 cluster shows the identical signature (CCD2 hot, CCD1 ~92 °C, CCD2 latched
at 107.000), which reinforces a CCD2 read quirk — but `Tctl` ramped in both
clusters, so both excursions are real regardless. The quirk is plausibly
aggravated by PCI-mutex contention with another monitoring tool (§5.2).

---

## 7. Cooldown physics — "can the silicon drop temps that fast?"

**Yes.** Measured per-tick (~250 ms) slew from the logs:

| Signal | Steepest 1-tick drop (peak file) | Steepest 1-tick drop (clean 06-14) | Ticks with ≥10 °C swing (clean 06-14) |
| --- | --- | --- | --- |
| `Tctl` (smoothed control temp) | −2.38 °C | −0.88 °C | **0** |
| CCD1 (raw `Tdie`) | −35.50 °C | −31.38 °C | 79 |
| CCD2 (raw `Tdie`) | −45.00 °C | −30.25 °C | **565** |

- The **raw per-CCD `Tdie`** is a near-massless junction sensor tracking the
  active hotspot. When a boost burst ends or work migrates off a CCD, that CCD's
  reading collapses **30 °C in a single 250 ms tick — hundreds of times in a
  normal session** (565 such ticks on 06-14 alone). So fast per-CCD swings are
  real AMD behavior, not sensor failure. The 45 °C peak drop is ~1.5× the routine
  clean-session maximum.
- The **`Tctl` composite is slew-limited** (steepest real drop −2.4 °C/tick;
  **zero** ≥10 °C ticks ever). It cannot and does not jump 45 °C in 250 ms; it is
  the correct signal for "sustained" temperature and it shows the excursion
  peaking ~107 °C then receding at ≤ ~6.5 °C/s. That a slew-limited signal
  *reached* 107.1 °C is itself proof the raw die was ≥107 °C (§5.3) — which is why
  the excursion claim rests on `Tctl`, not on CCD2's exact value.

Practical takeaway: read `cpu_tctl_c` for sustained-temperature and control
decisions; treat single-tick `cpu_max_c`/per-CCD extremes as instantaneous
hotspot transients, not sustained die temperature.

---

## 8. Reproduction

- Files: `release/runtime/logs/archive/svg_mb_control_control-loop_*.csv` (the
  live control-loop archives). Peak file:
  `svg_mb_control_control-loop_20260609_023240.csv`.
- Columns used: `wall_clock`, `cpu_tctl_c`, `cpu_max_c`, `amd_sensor_summary`,
  `system_cpu_busy_pct`, `process_cpu_pct`, `cpu_pkg_energy_acquisition`,
  `cpu_aperf_delta`/`cpu_mperf_delta` (absent on 06-09), `fanN_duty_pct`.
- Method: global-max scan for `cpu_max_c`; windowed percentiles around the peak;
  per-tick first-difference of `cpu_tctl_c` and the CCD values parsed from
  `amd_sensor_summary`; cross-file count of CCD2 `≥105 °C`.
- Decode/transport audited in `src/hardware/amd_decode.h`,
  `src/hardware/amd_reader.cpp`, `tests/cpp/amd_decode_tests.cpp`.

### Caveats
- 06-09 predates package-power and APERF/MPERF logging; clock loss at the spike
  is not measurable from that night's data.
- `Tjmax` (95 °C) and `THERMTRIP` (~115 °C) are AMD platform specifications, not
  values encoded in this repo.
