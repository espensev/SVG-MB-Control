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

## Promotion decision support (package-energy marker)

Promotion of `cpu_pkg_energy_acquisition` is a per-criterion judgment, not a
session count or a calendar span. The table records what a PASS on each
Evaluation criterion (`docs/cpu-work-energy-acquisition-decision-2026-06-07.md`
§Evaluation) establishes, and its status across sessions 1-3.

| Criterion | What a PASS establishes | Status |
|---|---|---|
| 1 Counter continuity across >=1 wrap | 32-bit modular subtraction and Delta-t handling stay correct over the full session, including idle | PASS x3 (~2 wraps/session, 0 negative deltas) |
| 2 Plausible range + load tracking | the idle floor and load tracking are physical, not a value pinned constant regardless of load | PASS x3 |
| 3 +/-15% external SMU cross-check | the ESU encoding and the delta-J/delta-t derivation are correct (reference taken from a path independent of the RAPL energy MSR) | PASS x3 (+0.1% s2/s3 at the load plateau) |
| 5 Fault behavior (no false zero) | an absent or failed read blanks cleanly with no crash and no false zero | PASS x3 |
| 6 No-disturbance vs disabled baseline | the read path does not regress the 250 ms control loop | PASS x3 (loop p95 within baseline SD) |

Criterion 4 is the APERF/MPERF effective-frequency check. It gates
`cpu_cycles_acquisition`, not the package-energy marker, and stays MANUAL; it is
addressed in `docs/cpu-cycles-effective-freq-validation-plan-2026-06-14.md`.

### Why no further package-energy cross-check session is planned

A single steady-window cross-check (criterion 3) is sufficient for the
package-energy marker; additional operating points do not add information.

- The transforms the pipeline applies on top of the SMU/RAPL power model are the
  ESU scale (a multiplicative constant), the 32-bit modular subtraction, the
  delta-J/delta-t division, and per-`cpu_power_sample_id` de-duplication. A wrong
  ESU scale is multiplicative, so it deviates by the same percentage at every
  load; the +0.1% agreement at the load plateau already bounds it. Modular wrap is
  exercised across the whole session by criterion 1; the delta-t division and the
  de-duplication are load-independent.
- The SMU package-power reference and the RAPL energy MSR both derive from the
  same on-die SMU power model. The cross-check therefore validates the encoding
  and derivation (its stated purpose under criterion 3), but it cannot detect a
  systematic bias of that shared model versus physical truth. An idle or mid-load
  SMU cross-check reproduces the same agreement rather than revealing a new error
  class.
- The only measurement that would add information is an external reference
  independent of the SMU model -- wall power minus rest-of-system, or an EPS12V
  current shunt -- compared once over a steady window. For a read-only, log-only
  signal that gates no control path (decision doc §Scope), this absolute-accuracy
  check is optional and is not required for promotion.

### Conclusion

The package-energy quarantine-exit criteria (1, 2, 3, 5, 6) hold across three
independent sessions with worker-restart cycles and idle/load/cooldown coverage.
No further capture session changes that evidence; the only stronger evidence is
the optional external-meter check above.

Maintainer decision (2026-06-14): the evidence is accepted and
`cpu_pkg_energy_acquisition` promotion is authorized. The runtime producer
currently stamps `quarantine` at capture time (`src/hardware/amd_reader.cpp`
`SamplePackageEnergy`); emitting `validated` for future captures is a producer
code change gated by an explicit, non-automatic promotion mechanism (decision doc
§Quarantine: "Promotion is never automatic"). Choosing that mechanism and
implementing it is the remaining step; the analyzer does not branch on the marker
(it is a provenance stamp), so no analyzer change is required. Archived session
1-3 data keeps its `quarantine` capture stamp. The cycle marker is evaluated
separately (`docs/cpu-cycles-effective-freq-validation-plan-2026-06-14.md`) and
is not yet promotable.

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
