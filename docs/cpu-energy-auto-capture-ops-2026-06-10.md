# CPU-Energy Quarantine Auto-Capture — Operating Assumptions & Run Flow — 2026-06-10

How the unattended capture sessions registered by
`scripts/Schedule-EnergySessions.ps1` are expected to run, and what user
behavior they assume. Automates the manual procedure in
`docs/cpu-energy-quarantine-exit-capture-runbook-2026-06-10.md`; the Evaluation
criteria are normative in
`docs/cpu-work-energy-acquisition-decision-2026-06-07.md` §Evaluation.

Current state (verified 2026-06-14): sessions 1, 2, and 3 have run and each
scored 5 PASS / 0 FAIL / 1 MANUAL. The fixed calendar-span gate was removed on
2026-06-14; quarantine exit now requires **>= 3 independent sessions** plus
maintainer review. The future Session 3 task trigger was disabled after the
manual 2026-06-14 run to avoid an unnecessary fourth load session.

## Assumptions about user behavior

1. **Logged in at 04:00.** The session tasks run with LogonType Interactive (so
   the harvester can read HWiNFO's shared memory in the user session). User
   `Sev` must be **logged in** at the scheduled time. A **locked screen is
   fine.** If logged out, the task does not run then; `StartWhenAvailable` runs
   it at the next logon.
2. **Machine on or asleep — not fully off.** `WakeToRun` wakes the PC from
   sleep. If the PC is powered off at 04:00, the session runs when it is next
   on + logged in.
3. **Idle at 04:00.** The idle (5 min) and cooldown (5 min) phases assume no
   competing heavy CPU work, so the idle power floor (criterion 2) and the
   no-disturbance comparison (criterion 6) are clean. The 12-min synthetic load
   phase deliberately uses **28 of 32 threads** — heavy enough to cross a 32-bit
   energy wrap, with headroom so it does not starve the 250 ms control loop
   (session 1 showed 32/32 starves it: p95 slip ~2.5 s).
4. **HWiNFO running** (it has `Autorun=1`) for the SMU criterion-3 reference. If
   it is down the wrapper launches Scribe; if neither is available the session
   still runs and scores criteria 1/2/5/6, with criterion 3 left MANUAL.
5. **No manual interference** during a window: do not hand-edit
   `SVG_MB_CONTROL_RAPL_ENERGY_MODE` / `_CPU_CYCLES_MODE` or stop the worker
   task while a session is mid-run.

## Run flow (one session, ~22 min, fully unattended)

`Invoke-EnergySessionCapture.ps1` → `Capture-EnergySession.ps1`:

1. Ensure HWiNFO is up (launch Scribe if needed).
2. Snapshot a fresh **disabled** baseline CSV (criterion-6 reference).
3. Set energy + cycle env = `enabled`; **restart the worker tree**; verify the
   live marker reads `quarantine` (aborts + reverts if the env did not
   propagate).
4. Start the sensor harvester (HWiNFO **SMU** + LHM, 1 s cadence).
5. **idle 5 min → synthetic load 12 min (28 threads) → cooldown 5 min**, marking
   a steady sub-window for the SMU cross-check.
6. `finally`: set env = `disabled`, restart the worker tree, verify marker =
   `disabled`. **Runs even on error**, so energy is never left enabled.
7. Auto-score the 6 criteria → write
   `docs/cpu-energy-quarantine-exit-evidence-<date>-s<N>.md`.

## Restart / failure resilience

- **Tasks persist across reboots** (Task Scheduler). A missed window runs at the
  next availability; a transient failure retries (RestartCount 2 / 10 min).
- **Boot/logon safety-revert** (`Reset-EnergyToDisabled.ps1`, triggers
  AtStartup + AtLogon): forces the energy/cycle env to `disabled` after any
  restart, and restarts the worker if it came back still in `quarantine`. So a
  session interrupted by a reboot/crash/power-loss **cannot leave energy
  enabled** (decision §Disturbance mitigation).
- The worker task auto-starts **AtLogon** and HWiNFO via `Autorun`, so after a
  reboot the controller and the SMU reference return on their own.
- If a session fails entirely, re-run manually (elevated):
  `pwsh -File scripts\Invoke-EnergySessionCapture.ps1 -SessionNum <N> -LoadThreads 28`

## Promotion (manual, never automatic)

After **>= 3 independent sessions pass**, the maintainer reviews the evidence
notes and flips `cpu_pkg_energy_acquisition` → `validated` (and
`cpu_cycles_acquisition` only if criterion 4 is confirmed), then reconciles
FEAT-0006 §14 / `docs/TRACEABILITY.md` (decision §Quarantine).
