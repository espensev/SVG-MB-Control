# CPU Energy Quarantine-Exit Validation - 2026-06-14

Status: evidence complete for package-energy validation; marker promotion remains
a manual maintainer decision.

## Calendar-span decision

The earlier fixed `>= 7 day` quarantine span was removed on 2026-06-14. It was
an unsupported policy margin, not a measured requirement. The retained gate is
`>= 3 independent capture sessions` plus the six session-level Evaluation
criteria from `docs/cpu-work-energy-acquisition-decision-2026-06-07.md`.

## Evidence set

| Session | Date | Evidence note | Result | Energy-specific outcome |
|---|---|---|---|---|
| 1 | 2026-06-10 | `docs/cpu-energy-quarantine-exit-evidence-2026-06-10-s1.md` | 5 PASS / 0 FAIL / 1 MANUAL | PASS |
| 2 | 2026-06-12 | `docs/cpu-energy-quarantine-exit-evidence-2026-06-12-s2.md` | 5 PASS / 0 FAIL / 1 MANUAL | PASS |
| 3 | 2026-06-14 | `docs/cpu-energy-quarantine-exit-evidence-2026-06-14-s3.md` | 5 PASS / 0 FAIL / 1 MANUAL | PASS |

The MANUAL item in all three notes is criterion 4, effective-frequency validity
for the APERF/MPERF cycle path. It does not block the package-energy validation
decision, but it does keep `cpu_cycles_acquisition` promotion separate.

## Runtime closeout

- Session 3 was started manually through the existing scheduled-task wrapper on
  2026-06-14 at 02:33:13 local time.
- The capture reverted successfully: the run log recorded `marker = disabled`
  after the revert.
- The live CSV tail after completion showed `cpu_pkg_energy_acquisition=disabled`
  and `cpu_cycles_acquisition=disabled`.
- The future `SVG-MB Energy Session 3` trigger for 2026-06-18 was disabled after
  the successful manual run to avoid a redundant fourth load session.

No raw runtime CSV captures are added by this note.
