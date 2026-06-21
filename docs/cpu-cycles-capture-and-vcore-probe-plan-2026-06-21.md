# Plan: post-change cycles capture + Vcore SVI probe (2026-06-21)

Two enabling steps for the CPU config fingerprint in
`docs/idle-cpu-temp-trend-and-enrichment-2026-06-20.md` ("CPU-side telemetry"):

- **A (#2):** capture a session with effective-frequency (APERF/MPERF) logging on,
  after the new BIOS + -25 all-core Curve Optimizer change, so MHz-per-watt exists
  and the fingerprint can see the undervolt.
- **B (#3):** a throwaway read-only probe to discover whether live Vcore/VSoC is
  reachable on this Zen5 part, so voltage can become a fingerprint dimension.

Neither changes control behavior. The capture briefly restarts the worker and is
read-only telemetry; the probe never links the control loop. Both need the
operator to run them (live-runtime + elevated), per `AGENTS.md` Live Runtime Safety.

## A. Capture a post-change cycles-on session (#2)

The existing `scripts/Capture-EnergySession.ps1` already does exactly this: it
snapshots a disabled baseline, enables the read-only RAPL energy **and** cycle
path (cycles are on unless `-EnergyOnly`), restarts the worker tree so the env
propagates, verifies the `quarantine` marker, drives idle -> synthetic load ->
cooldown while harvesting the HWiNFO (SMU) + LibreHardwareMonitor (RAPL) reference
sensors, and **always reverts to disabled** in a `finally` block. No script change
is required for a one-shot capture.

### Prerequisites

- Build the synthetic load generator once (off by default):
  `cmake -DSVG_MB_CONTROL_BUILD_SYNTH_LOAD=ON build/x64-release && cmake --build build/x64-release --target cpu_synth_load`
  (the capture auto-discovers `cpu-synth-load.exe`, or pass `-SynthLoadExe`).
- P0 (base) frequency for the analyzer eff-MHz derivation: **4300 MHz** for the
  9950X3D (MPERF_RO counts at base; effective MHz = ratio x P0).
- Run elevated, at idle (the enable/revert each restart the worker once).
- Confirm the -25 CO / BIOS change is the current live state (it is, as of
  2026-06-20).

### Steps

1. **Dry run (no-touch):** `./scripts/Capture-EnergySession.ps1 -Rehearse`
   — exercises the profile + harvest with no enable and no restart.
2. **Real capture (idle -> load -> cooldown):**
   `./scripts/Capture-EnergySession.ps1 -SessionLabel cycles-postco-2026-06-21`
   (defaults: 300 s idle, 720 s load, 300 s cooldown; 720 s load crosses a 32-bit
   energy wrap). The load band is what exposes the V/F operating point, so the
   load segment is required — an idle-only capture cannot show MHz/W.
3. The script reverts energy+cycles to `disabled` and restarts the worker at the
   end (even on error). It emits a manifest JSON for `score_energy_session.py`.

### Verification checklist (on the new session CSV under `release/runtime/logs/`)

- `cpu_cycles_acquisition` = `quarantine` during the enabled window (never
  auto-`validated`; the analyzer does not branch on it).
- `cpu_aperf_delta` / `cpu_mperf_delta` populate on the ~1 s cadence windows
  (intervening 250 ms ticks mirror the last window; the analyzer de-dups on
  `cpu_cycles_sample_id`).
- `cpu_pkg_energy_delta_uj` populates (energy on too).

### Hot-path gate (cycles adds a per-tick cost energy did not)

Energy logging passed gate-6 (per-tick NVML/RAPL read did not move the 250 ms
baseline). Cycles is different: `SampleCpuCycles` pins the control thread to
logical CPU 0 for the paired MSR reads once per ~1 s window. That affinity pin is
on the control thread, so before trusting a cycles-on session — and **before** any
consideration of leaving cycles on persistently — confirm it does not move the
loop budget:

- Compare `loop_work_duration_ms` p50/p99/max and `loop_overrun` count between the
  disabled baseline segment (the script captures one first) and the cycles-on
  segment, at matched GPU-idle state.
- Pass = no rise in overruns and no material p99/max shift attributable to the
  ~1 s cycle window. If it does move the baseline, the all-core/off-control-thread
  redesign (below) is required before cycles is usable beyond a one-shot capture.

### First-look analysis (does the data show the -25?)

- Effective frequency: native report `svg-mb-control analyze report --p0-mhz 4300`
  (the v10 cycles block: cycle-weighted sum(dAPERF)/sum(dMPERF) x P0, per-window
  p50/p90/max). Package watts + theta per band: `scripts/analyze_cpu_temp_power.py`.
- First look = eff-MHz and package watts within this one session's idle vs load
  bands; sanity vs the pre-change sessions 1-3 (idle ratio ~1.23, load ~1.19).
- The actual undervolt signal is **eff-MHz binned by (system_cpu_busy_pct, package
  watts)** — a -25 CO raises MHz at the same watts (or lowers watts at the same
  MHz). Computing that cross-regime step is the `cpu_config_fingerprint.py` module
  (separate, larger item in the enrichment doc); this capture produces its input.

### Scope caveat + follow-on

The cycle read pins **core 0 only**, so the ratio is one core's V/F point, not the
package, on an asymmetric dual-CCD part (core 0 likely enumerates onto the cooler
V-cache CCD1). For detecting the -25 a single-core eff-MHz-at-watts step is still
usable; a true **package** eff-MHz and an uncontaminated cycles/Joule need a new
all-core aggregation (sum dAPERF/dMPERF across one logical core per physical),
ideally on a dedicated off-control-thread sweep so affinity hopping never touches
the 250 ms loop. That is a separate build item, not part of this capture.

### Persistent cycles-on (future, optional)

`scripts/Set-EnergyLoggingProfile.ps1` is the steady-state profile flip and
currently forces `SVG_MB_CONTROL_CPU_CYCLES_MODE='disabled'` in both directions.
Leaving cycles on continuously (so the fingerprint runs without a manual capture)
would add a `-WithCycles` switch there **and** requires the hot-path gate above to
pass on a sustained run plus a recorded decision (mirroring D-PWRLOG-1 for energy).
Not needed for the one-shot capture; do it only if continuous fingerprinting is
wanted.

## B. Vcore SVI discovery probe (#3 — written, compiles)

`tools/cpu_vcore_probe.cpp` (+ CMake target `cpu_vcore_probe`, off by default).
Built and linked clean against the configured toolchain on 2026-06-21.

### What it does

Live Vcore is the most direct undervolt fingerprint, but the address/decode for
Zen5 (SVI3) is unknown. The transport is **not** the blocker: the shipped bin's
`ioctl_read_smn` accepts an arbitrary 32-bit SMN address (it is not allow-listed,
unlike the MSR path), and already serves the Tctl (0x00059800) and per-CCD Tdie
(0x00059B08) reads. The probe:

1. Opens PawnIO, loads the AMDFamily17 bin (reusing `pawnio_binary.h`).
2. Sanity-reads Tctl and CCD0 via SMN to prove the transport (decode reused from
   `amd_decode.h`).
3. Sweeps `0x0005A000..0x0005A024` (the SVI2-era telemetry neighborhood) and prints
   each raw 32-bit value + a candidate voltage decode (VID = bits[23:16],
   V = 1.55 - 0.00625*VID), at idle and during an in-process busy spin.
4. Prints a manual three-part verdict — it asserts nothing.

It is read-only (`ioctl_read_smn` only, never writes), default-OFF
(`SVG_MB_CONTROL_BUILD_VCORE_PROBE`), not built by Test-LocalCI / Build-Release /
CTest, and never shipped in `release\`.

### Build and run (elevated, controller monitor-only)

```
cmake -DSVG_MB_CONTROL_BUILD_VCORE_PROBE=ON build/x64-release
cmake --build build/x64-release --target cpu_vcore_probe
# run elevated; for the load-tracking signal, run cpu-synth-load in another window
build/x64-release/cpu-vcore-probe.exe
```

### Promotion gate (do NOT trust a value until all three pass)

Compare the decoded candidate against HWiNFO64 "CPU Core Voltage (SVI2 TFN)":

1. matches HWiNFO within tolerance at **idle and under an all-core load**;
2. **rises** from idle -> busy (a register that does not track core load is the
   wrong field);
3. reads **measurably below** the pre(-25 CO) regime at matched load (the whole
   point — a CO undervolt lowers Vcore at a given operating point).

A second near-constant ~1.0-1.2 V register that does not track core load is the
SoC plane (VSoC). Only after all three pass: promote the confirmed address+decode
into `src/hardware/amd_decode.h` (a `DecodeSviVoltage`) and an opt-in read in
`amd_reader.cpp` behind a default-off env (mirroring the energy/cycles gates), add
a CSV column + ingest, and it becomes an `idle_vcore_mv` fingerprint dimension. The
SVI candidate addresses/decode are SVI2-era and **unverified** for SVI3 — the
probe exists precisely because they may be wrong; never promote on plausibility.

### Out of scope

Per-core VID and the PPT/TDC/EDC power limits are **not** reachable by this cheap
direct-SMN read — they live in the SMU PM table behind a write-poll-read mailbox
protocol with a version-specific struct layout. That is a separate, larger
feasibility item (a PM-table reader), not this probe.

## C. CPU config fingerprint module (the consumer)

`scripts/cpu_config_fingerprint.py` (skeleton, written + runs today). It turns the
capture (A) and probe (B) outputs into auto-detected config regimes with **no
operator annotation**:

- **Config-pure dimensions** drive segmentation: `idle_power_floor_w`,
  `power_at_busy_w` per busy band, `eff_mhz_per_w_by_band` (needs A), `ccd_balance_c`
  (CCD2-CCD1 from the existing `amd_sensor_summary` text), `idle_voltage_core_mv`
  (needs B). **Cooling-output scalars** (`tctl_idle_floor_c`, `theta_c_per_w`) are
  reported but never segmentation inputs, so a repaste/ambient shift cannot mint a
  false regime.
- **Segmentation:** exact pre-cuts on `git_hash` / `config_sha256` change, then a
  stdlib median/MAD step detector (step > 4*MAD, persisting >= 2 runs) on the
  config-pure dims. Acquisition transitions (the 06-18 energy-on flip) are
  pre-registered as non-config boundaries so they never read as a config regime.
- **Output:** versioned `svg_mb_control.cpu_fingerprint.v1` JSON: regimes labeled
  `regime_1..N` (never an operator string) each with a `session_summary`-style
  rollup, trigger reason, and `config-attributable` vs output-only tag.

Verified run over the current 67 archived runs: it produced 9 regimes; the step
detector fired on `idle_power_floor_w` and `ccd_balance_c` (not just git
boundaries), and `ccd_balance_c` tracks the idle floor down across regimes
(16.9 -> 10.3 -> 7.9 C). Capture-dependent dims (`eff_mhz_per_w`,
`idle_voltage_core_mv`) degrade to null with explicit `notes`, populated once A/B
land.

## D. Merge alignment with the new NVG + SVG logging

Verified against NVG `docs/ANALYZER_RUNTIME_OBSERVABILITY_SPECS.md`,
`src/csv_logger.h`, and the `thermal_data.db` schema, and SVG `control_csv.py` /
`analyze_*.py`. The fingerprint + the proposed CPU columns are built to converge
when the two projects merge:

| Convention (both projects) | How this work mirrors it |
|---|---|
| Analyzer/reporting class is preferred over controller-behavior changes; it is never a control input | The fingerprint is offline analyzer-only; the workload/regime tag and per-CCD/voltage columns are telemetry, never fed to control |
| Additive, versioned schema names are compatibility contracts (NVG `nvg_smooth.*.v1`, SVG `svg_mb_control.log.v1`); never rename, add keys only | Output is `svg_mb_control.cpu_fingerprint.v1`; consumers ignore unknown keys; no existing schema renamed |
| Schema/summary/retention changes are NOT bundled with controller/config work; each behavior-changing unit is its own commit | The fingerprint adds **no** schema change (reads existing columns + the `amd_sensor_summary` text); per-CCD/voltage column promotion would be its own additive, ingest-versioned commit |
| A `session_summary` rollup table; segment labels via `load_marks` / `# campaign=` | The per-regime rollup mirrors `session_summary`; a "regime" is the discovered (not hand-typed) analogue of a `load_marks`/campaign segment |
| snake_case units `_c/_w/_mw/_mhz/_mv/_pct/_ms`; NVG already logs `voltage_core_mv` | Promoted CPU columns mirror NVG names: Vcore -> `cpu_voltage_core_mv`, dies -> `cpu_ccd1_tdie_c`/`cpu_ccd2_tdie_c`, eff-freq -> `cpu_eff_freq_mhz` (NVG `clock_gfx_mhz`) |
| Provenance metadata block (version/source/mode/gpu_name/session_start; SVG adds git_hash/config_sha256) | Every fingerprint carries `git_hash`, `config_sha256`, `session_start` from the CSV meta block |

Disposition (using NVG's legend): the fingerprint module and the per-CCD column
parse are `implement-now` (analyzer-only, additive); the Vcore column is
`evidence-gated` / `operator-action` (the probe must confirm the SVI3 address
first); persistent cycles-on is `operator-decision-then-implement` (needs the
hot-path gate + a recorded decision). Nothing here renames a schema, bundles a
control change, or touches retention.

## Status

- #3 probe: written and compiles (`cpu-vcore-probe.exe`). Awaiting an elevated run
  + HWiNFO64 comparison to find/confirm the SVI3 address (or conclude it is not a
  direct SMN read).
- #2 capture: runbook ready on existing tooling; awaiting an operator-run capture
  (elevated, at idle) + the hot-path gate check.
- Fingerprint module: `scripts/cpu_config_fingerprint.py` skeleton written + runs
  (9 regimes over 67 runs; step detector fires on config-pure dims). Aligned to the
  NVG + SVG merge conventions (section D). Capture/probe fill its null dims.
- Per-CCD Tdie: **implemented** (the one capture-free column). Shared
  `control_csv.parse_ccd_temps` + per-CCD columns and a `CCD2-CCD1 C` band column
  in `analyze_cpu_temp_power.py`; both the analyzer and the fingerprint consume the
  shared parser. TDD'd (`tests/test_cpu_temp_power.py`): 6/6 plus 43/43 analyzer/
  `control_csv` consumer tests green; verified on live data (CCD2 ~17 C above CCD1
  at load). No schema change; backfills all history.
- Nothing is committed or shipped; probe + persistent-cycles are throwaway/opt-in
  until results justify promotion; the per-CCD parse and fingerprint are additive
  analyzer-only.
