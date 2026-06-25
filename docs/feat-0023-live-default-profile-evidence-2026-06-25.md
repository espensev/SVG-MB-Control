# FEAT-0023 REQ-MPROFILE-10 — deployed default profile reproduces baseline on hardware (2026-06-25)

**Verdict: PASS.** A profile-resolved default (`snd-desk-composed`) deployed to the
live worker reproduces the shipped 250 ms control cadence, 250 ms write cooldown,
6-channel set, and **control-computation identity** on hardware. The resolved
control config is byte-identical to `release\control.json` (89-line `--show-config`
diff, complementing `profile_composition_tests`), and the live worker ran it
healthy with `active_profile_name = snd-desk-composed`.

`T`/`R` already passed; this closes the on-hardware `M` for REQ-MPROFILE-10.

## Deploy mechanism — Option A2 (task `--config` repoint, reversible, no build)

The plan's literal `--profile` task-arg form is invalid: `svg-mb-control-task-runner.exe`
parses only `--config` (`ParseConfigPath`) and drops `--profile`, so setting the task
args to `--profile snd-desk-composed` would fall back to the default config and false-pass.
Option A2 instead repoints the worker + watchdog task `--config` to the composed profile
JSON for the measurement window, then reverts. The supervisor re-spawns the worker with
`--config <resolved>` and sets `active_profile_name` = the config-file stem, so the hard
gate reads `snd-desk-composed`.

- **Live binary:** SHA `65972F12…` (built 2026-06-23, clean) — no build required.
- **Caveat (accepted, reverted):** under the composed profile the worker resolves
  `Runtime home` to `config\runtime` (not `release\runtime`), so for the measurement
  window the live deploy depended on the dev tree and runtime state was written there.
  Reverted at rollback; the transient `config\runtime` was removed.

## Step 1 — resolution pre-check (read-only)

`release\svg-mb-control.exe --show-config --profile snd-desk-composed` resolves to
`config\profiles\snd-desk-composed.json` (source `profile_flag`), reporting the shipped
control config: control-loop tick **250 ms**, write cooldown **250 ms**, deadband 0.25 %,
6 channels (ch0 `curve_overlay` / `max_cpu_gpu_source_aware`, …). Matches the shipped
cadence/channels before any live action.

## Step 2 — deploy + hard gate

Both `SVG-MB Control` and `SVG-MB Control Watchdog` task `--config` were repointed to
`config\profiles\snd-desk-composed.json` (worker keeps `--start`, watchdog `--watchdog-run`),
exact prior args saved for revert, then the worker tree was restarted (watchdog stopped
first, `release\` worker procs drained, worker then watchdog restarted).

**Hard gate PASS:** the live worker came up with `active_profile_name = snd-desk-composed`
(source `explicit_config`), the `config\runtime\control_runtime.json` status file fresh
(< 8 s), a `svg-mb-control.exe` confirmed running with `snd-desk-composed` in its command
line, and `--health --config <composed>` reporting `healthy` (pid, 0 degraded channels,
tick `poll_tick_ms=250 cooldown=250`, runtime_home `config\runtime`). The deploy did not
fall through to `control` — the failure mode that would have false-passed. (The procedure
auto-reverts on a failed gate; it was not triggered.)

## Step 3 — composed-default window vs release reference

A ~5.5-min representative window (box idle; both this window and the release reference
are idle, energy-enabled — apples-to-apples). Confirmed against a recent `control.json`
reference window (`release\runtime\logs\svg_mb_control_output.csv`, the pre-deploy window):

| property | composed (`snd-desk-composed`) | release ref (`control.json`) |
|---|---|---|
| `loop_intended_interval_ms` | 250 | 250 |
| achieved interval p50 / p99 | 250.9 / 252.0 ms | 250.9 / 252.0 ms |
| `loop_slip_ms` p99 | 2.0 ms | 2.0 ms |
| `loop_overrun` fraction | 0.0 | 0.0 |
| active channels | 0–5 | 0–5 |
| `channelN_primary_temp_source` | `gpu` (all) | `gpu` (all) |
| `policy_writes_enabled` | `true` | `true` |
| `active_profile_name` | **snd-desk-composed** | control |

- **Control identity (definitive):** `--show-config` for the composed profile vs
  `--show-config --config release\control.json` are **byte-identical across all 89
  control-relevant lines** (cadence, channels, curves, boosts, blend modes, low-band,
  health/safety), only the config-path / profile-source / runtime-home lines differing
  (expected). Confirmed via **both** entry points — `--profile snd-desk-composed`
  *and* the exact deployed `--config config/profiles/snd-desk-composed.json` form
  (they converge through the same `ComposeConfigRoot`; the supervisor's normal spawn
  is `--config <resolved>`, so the `--config` diff covers the path actually run). This
  is the on-hardware confirmation of the byte-identity that `profile_composition_tests`
  proves in test.
- **Note on `channelN_response_source`:** this is a *dynamic, input-dependent* label
  (which response path fired a given tick), not a static config property. The composed
  window observed GPU temps up to 66.1 °C vs the release window's 68.4 °C, so the
  higher-temp paths (`+midband_pressure`, `primary_curve+midband_pressure`) fired more
  in the release window. The composed window's `response_source` set is a clean *subset*
  of the release vocabulary — same config, narrower observed-temp range — not a config
  difference (the 89-line config diff is identical).
- Health `healthy` throughout the window; 0 degraded channels.

## Step 4 — live switch exercise: SKIPPED (optional)

Not exercised this run. REQ-MPROFILE-10 needs only Steps 1–3, and the live
switch-by-restart behavior (graceful worker cycle, fans → BIOS auto during the ~1–2 s gap,
RPM rises under load — "restart = seamless" is FALSE under load) is already characterized
in the FEAT-0023 design measurement (`docs/multiprofile-restart-switch-decision-2026-06-20.md`,
`docs/fan-restart-restore-and-plant-model-measurement-2026-06-20.md`). Skipping also
minimizes time on the dev-tree runtime path.

## Step 5 — rollback

Both task `--config` args restored exactly to `release\control.json` (worker `--start`,
watchdog `--watchdog-run`), worker tree restarted. Confirmed: `active_profile_name = control`,
`health_state = healthy`, 0 degraded, runtime_home back to `release\runtime`, both task args
verified restored. Transient `config\runtime` removed; tree clean. Env steady state intact
(energy `enabled`, cycles `disabled`).

## Honest limitations

- **Window length.** ~5.5 min idle, shorter than the plan's "≥ ~30 min." Justified: the
  control config is byte-identical (`--show-config` diff + `profile_composition_tests`), so
  the M confirms the resolution/deploy path on hardware rather than re-measuring control
  behavior; a shorter window also minimizes time on the dev-tree runtime path. No CPU/GPU-busy
  stretch was included (the box was idle); a busy window would exercise more `response_source`
  paths but cannot change the byte-identical config.
- **Option A2 is a measurement deploy, not productization.** It depends on the dev tree and
  relocates runtime state for the window (reverted). Option B (productize `config/profiles`
  into `release\` via Build-Release/installer) is the permanent "deployed default" and remains
  separate future work (Feature Intake Gate + clean tree), usually done after this M confirms.
