# Session handoff — 2026-06-30 (pre-audit)

Handoff of an agent session's work on `svg-mb-control`, written to seed the
upcoming codebase audit. Grounded in `git log`, the PRs named below, and the
verification passes recorded here. This is a journal/handoff, not a contract;
`docs/features/`, `docs/TRACEABILITY.md`, and `git log` stay authoritative.

## 1. What this session shipped / opened

| Thread | State | PR |
|---|---|---|
| Issue #4 disk growth → FEAT-0015 (event-JSONL retention) + FEAT-0016 (analyze-DB run-purge) specs + decision records | **Merged** to `main`; maintainer then **implemented** both (`Implemented`, Test-LocalCI green) | #29 (merged) |
| FEAT-0025 AMD-GPU telemetry intake (renumbered from a FEAT-0024 collision; merged `main` in) | Draft, reconciled, mergeable | #28 (the other session's, reconciled here) |
| Windows CI workflow (`.github/workflows/ci-windows.yml`) | **On `main`** (reconciled with the maintainer's parallel add) | via #29 |
| **Tctl/Tdie false-cold guard** — a zero/garbage SMN read no longer poses as a valid 0 °C CPU input | Draft; `amd_decode_tests` **PASS** on Windows CI; adversarially reviewed SAFE | #33 |
| **Overlay re-sync** — `release.behavior.json` → FEAT-0024-retuned `control.release.json` (greens `profile_composition_tests`, which is red on `main`) | Draft; in CI | #34 |

**Cascade to clean:** `main` is currently **red** on `svg_mb_control_profile_composition_tests` (stale overlay). PR #34 fixes it. Merge order: **#34 → green `main` → re-run + merge #33 and #28.**

## 2. Pre-live verification pass (sensor flow · control math · config)

Run against `main` (`fab37d7`) ahead of a planned on-hardware evidence run. The
engine is **correct and safe to run**; the issues are operational + one decision,
not engine-correctness bugs.

**P0 — before the live run:**
1. **Scrub `SVG_MB_CONTROL_SIM_*` from the deploy shell**; confirm `transport_path` = the real PawnIO bin on tick 1. The AMD/GPU readers honor sim env vars live with no production guard — a stale var silently fakes the whole capture.
2. **The shipped intake curves are the Draft, un-validated FEAT-0024 retune** (verified in `control.release.json`). A live run *is* its Pass-3 — label the evidence as such, or revert intake lanes for a clean baseline.
3. **Launch on `--config control.release.json`**, not `--profile snd-desk-composed` (composition was out of sync; PR #34 fixes that).

**P1 — known / hardening:**
4. **CPU false-cold** — fixed by PR #33 (`DecodeTctl` plausibility gate).
5. **AMD-Radeon target = CPU-only run** — `gpu_airflow` (the only GPU-fed boost) is inert with no NVIDIA GPU; everything falls back to CPU `Tctl/Tdie` safely, but the run is narrow vs the GPU-bearing target. FEAT-0025 (#28) is the structural fix.

**P2 — cleanup (no output impact):** safe-mode 100% is still subject to the write cooldown (optionally let `safety_override` short-circuit it like `authority_reassert`); `CONTROL_PIPELINE_MATH.md §6.1` describes anti-windup as a `B<B_max` gate but the code is integrate-then-clamp (`boost_stage.cpp:102-109`) — cosmetic doc drift.

**Verified clean:** control math matches the docs behaviorally and PID is disabled in shipped config; sensors read hardware directly (no cross-process staleness), GPU-absence degrades to `cpu_fallback`, FEAT-0013 dropout implemented, 3-miss safe-mode trips + bypasses the breaker; config curves monotonic, floors in range, ch6 (pump) blocked, write policy sane.

## 3. Terminal/evidence-stage plan (the "fast" path)

Get the engine as ready as it can be, then let real runtime be the test:
1. **Calibrate:** fan sweeps only (lands in `plant_model_*`).
2. **Run:** normal-time real run = the main test.
3. **DB runbook:** confirm **no stale pre-v13 `svg_mb_control.db`** (ingest refuses on schema mismatch; current `kSchemaVersion = 13`) → `analyze ingest` → `analyze prune --db-retain-days N` (bounded; FEAT-0016) → `analyze report --decision-record-out auto` (the evidence artifact).

## 4. Open / parked for the audit

- **FEAT-0025 (#28)** — promote D-AMDGPU-1 (Proposed→Current), then implement the vendored ADLX backend (needs Windows + AMD hardware). The structural fix so the run isn't CPU-only.
- **Held-Draft gates:** FEAT-0014 (reconcile/restore guard), FEAT-0009 (priority elevation A/B), FEAT-0017 (response retune), FEAT-0018 (cadence floor — crosses the measurement gate). See `docs/features/README.md` §"Current priority".
- **Doc/config cleanup:** the `§6.1` anti-windup wording; the safe-mode cooldown short-circuit.
- **Cross-repo:** `mmvg` + `svg` work — deferred.

## 5. Pointers for the audit

- Feature state: `docs/features/README.md` §"Current priority" + the §5 registry; `docs/TRACEABILITY.md`.
- Control identity: `docs/CONTROL_PIPELINE_MATH.md` (one known cosmetic drift, §6.1).
- Sensor path: `src/hardware/amd_reader.cpp` / `amd_decode.h`, `src/hardware/gpu_reader.cpp`, `src/control/channel_evaluator.cpp` (`GpuControlEnvelopeC`, `SelectPrimaryCurveInput`).
- Config: `config/control.release.json` (shipped) + `config/overlays/release.behavior.json` + `config/machines/snd-desk.cooling.policy.json`; `docs/COOLING_STRATEGY.md`.
- CI: `.github/workflows/ci-windows.yml` (runs `Test-LocalCI` on `windows-latest`).
