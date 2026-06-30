# Session handoff — 2026-06-30 (post-audit)

Handoff of the agent session that drove the PR merge cascade to a clean `main`,
ran the codebase audit, and wrote the audit + remediation reports. Successor to
`docs/session-handoff-2026-06-30.md` (the *pre*-audit handoff). This is a
journal/handoff, not a contract — `docs/features/`, `docs/TRACEABILITY.md`, and
`git log` stay authoritative. Grounded in `git log`, the PRs named below, and the
two report docs this session produced.

## 0. TL;DR for the next session

- **`main` is clean** at `dc54a60` (four PRs merged this session: #34, #33, #35, #28).
- **The audit is done.** 12 confirmed findings (4 `high`), written up in
  `docs/codebase-audit-2026-06-30.md`, with a deep remediation plan in
  `docs/codebase-audit-remediation-2026-06-30.md`.
- **No engine fixes have been made.** Implementation is deferred by explicit user
  instruction ("detailed report only, no fixing yet"). The reports are the
  deliverable so far.
- **Open PR #36** (draft, branch `claude/great-darwin-pqzn1s`) carries both report
  docs. It is docs-only.
- **Next action when the user says go:** the R1/R2/R3 "fail-safe sensor + safe-mode"
  PR — the three small, hardware-free, thermal-safety fixes.

## 1. What this session did

### 1.1 Merge cascade → clean `main`
`main` was red on `svg_mb_control_profile_composition_tests` because the FEAT-0024
intake-lead retune updated `control.release.json` but not the
`release.behavior.json` overlay, breaking the FEAT-0023 "composed reproduces
release" invariant on *every* PR. Fixed and merged in order:

| PR | Change | Merged as |
|---|---|---|
| #34 | overlay re-sync (greens `profile_composition_tests`) — the keystone | `0fb3061` |
| #33 | Tctl/Tdie false-cold validity gate (`DecodeTctl` `out_valid`) | `06db78a` |
| #35 | pre-audit session handoff doc | `123d7e7` |
| #28 | FEAT-0025 AMD-GPU telemetry intake spec (renumbered from a FEAT-0024 collision) | `dc54a60` |

All four passed Windows CI green on the merge ref before merging. `main` HEAD is
now `dc54a60`. The cascade was sequenced #34 first (it unblocked the inherited
test failure on the other three).

### 1.2 Codebase audit
Ran a multi-agent audit (Workflow `wf_1561a408-b76`, 26 agents) against `dc54a60`:
seven subsystem reviewers (control pipeline · AMD/GPU sensor path · config/overlay
composition · analyze-DB/retention · runtime write-path · app/lifecycle · CI/build),
each producing structured findings, then an **independent adversarial verifier per
finding** instructed to refute it. **18 raised → 12 confirmed, 6 refuted, 0
uncertain.** Report: `docs/codebase-audit-2026-06-30.md` (`2eb89d0`).

The verifier earned its keep: it downgraded the GPU finding `critical`→`high`, and
refuted (a) a claim that `--force` re-ingest inflates `event_count_ingested` and
(b) a worker-force-terminate "fans stay frozen" claim, both as mechanically
inconsistent with the code. Do not re-raise those two — they were checked and are
sound.

### 1.3 Remediation report
Per-finding root-cause / trigger / blast-radius / remediation / test / risk, plus
sequencing and PR shape. Report: `docs/codebase-audit-remediation-2026-06-30.md`
(`5cbe1fb`). **Analysis only — no engine code, config, schema, or test changed.**

## 2. The 12 confirmed findings (implementation backlog)

Severity and anchors are from the audit; `Rn` ids are from the remediation report.
None are fixed. None require hardware to *fix or test* (only the eventual
on-hardware evidence run needs hardware).

| R | Sev | Subsystem | Anchor | One-line |
|---|---|---|---|---|
| R1 | high | sensor-path | `src/hardware/gpu_reader.cpp:636` | GPU thermal-read miss → valid 0 °C → fans to min, no sensor-miss counted |
| R2 | high | control | `src/control/channel_evaluator.cpp:166` | Safe-mode 100 % is rate-limited/cooldown-gated; `safety_override` only bypasses the breaker |
| R3 | high | config | `src/control/profile_composition.cpp:94` | No base→overlay coverage check; a dropped controlled fan loads silently uncontrolled |
| R4 | high | analyze-db | `analyze_ingest.cpp:272`, `analyze_ingest_db.cpp:301` | Non-`--force` event ingest not idempotent; dupes land `run_id=NULL`, immune to FEAT-0016 prune |
| R5 | med | lifecycle | `src/control/control_supervisor.cpp:821` | Crash backoff latches fans up to 32 s with no failsafe write |
| R6 | med | spec-gate | `tests/test_feature_specs.py:93` | Registry dict-keyed by id → blind to duplicate FEAT rows (the #28 collision class) |
| R7 | low | sensor-path | `src/hardware/amd_decode.h:37` | `DecodeTctl` gate has no lower bound; offset path can pass sub-ambient as valid |
| R8 | low | write-path | `src/runtime/runtime_event_log.cpp:365` | Event JSONL append not fsync'd; torn/lost final line on power loss |
| R9 | low | write-path | `src/runtime/runtime_csv_archive.cpp:309` | CSV rows flushed to OS buffer only; manifest count can exceed durable rows |
| R10 | low | lifecycle | `src/runtime/runtime_health.cpp:184` | Watchdog age uses local-time seconds; DST/backward-clock makes a hung worker read fresh |
| R11 | low | ci-build | `.github/workflows/ci-windows.yml:40` | vcpkg hard-required though no vcpkg dependency is consumed |

**Recommended order & PR shape (from the remediation report):**
1. R1+R2+R3 as one "fail-safe sensor + safe-mode" PR (shared safety theme,
   `channel_*`/`gpu_reader` surface, all near-zero-to-low risk, unit-testable).
2. R4 on its own (analyze/DB surface) — prefer the *skip-events-when-`run_windows`-
   is-empty* variant so the `--force` and first-ingest paths are untouched; pair
   with extending `test_ingest_is_idempotent_without_force` to assert the `events`
   count is stable (it currently checks every table *except* events — that is why
   the regression hid).
3. R6 opportunistically (one-line-class test fix); R5 on its own (restart state
   machine — safety-relevant).
4. R7–R11 batched (R7/R11 trivial; R8/R9/R10 small durability changes).

## 3. Verified-clean (do not re-audit without new evidence)

From the audit's "what was verified clean" section — these were traced and found
sound, so a future pass should not re-litigate them without a concrete new lead:
- Control pipeline math (smoothing/rate-limit/deadband/breaker) for normal
  (non-safety-override) operation; `safety_override` correctly bypasses the
  write-failure breaker where consulted.
- AMD core decode: `DecodeTctl`/`DecodeCcdTemp` reject zero-field and ≥125 °C reads
  (PR #33); only the lower bound (R7) is missing.
- Config composition forward check + shipped overlay coverage (channels 0–5
  controlled, 6 excluded/null floor); FEAT-0023 reproduction test pins composed
  output.
- analyze-db prune cascade, manifest-skip on seen runs, `--force` full clear,
  start-event→run attribution — all correct outside the non-idempotent events path.
- Atomic JSON writers (`json_io.cpp WriteJsonFileAtomic`) flush before rename — real
  crash durability; only the append-only JSONL/CSV streams lack it (R8/R9).
- Supervisor startup-gate, clean-shutdown restore, stop handling, FEAT-0008
  watchdog escalation — correct for the non-crash, monotonic-clock case.
- Spec-gate registry↔spec↔traceability key-set + per-field checks; vendored build
  graph self-contained.

## 4. Open / in-flight state

- **PR #36** (draft, `claude/great-darwin-pqzn1s` → `main`): the two report docs +
  this handoff. Docs-only. CI was `in_progress` at last check; expected green.
- **Hourly watch on #36:** `CronCreate` job `7b523aac` (fires at :23) re-checks
  #36's CI / mergeability / review comments and stays silent unless actionable; it
  self-deletes once #36 is merged or closed. **Caveat:** the job is *session-only*
  — it dies when this session ends and auto-expires after 7 days. A successor
  session must re-arm its own watch (or rely on webhooks, which deliver CI
  failures, review comments, and the merge/close event but **not** CI success or
  merge-conflict transitions).
- **Branch contract:** develop on `claude/great-darwin-pqzn1s`. If PR #36 is
  already merged when follow-up work starts, restart the branch from latest `main`
  (`git fetch origin main && git checkout -B claude/great-darwin-pqzn1s
  origin/main`) — a merged PR is finished; do not stack new commits on merged
  history.

## 5. Parked (carried over from the pre-audit handoff, still open)

- **FEAT-0025 (#28, merged spec):** promote D-AMDGPU-1 Proposed→Current, capture
  the §12 measurement-gate evidence, then implement the vendored ADLX backend
  (needs Windows + AMD hardware). The structural fix so a run isn't CPU-only.
- **Held-Draft gates:** FEAT-0014, FEAT-0009, FEAT-0017, FEAT-0018 — see
  `docs/features/README.md` §"Current priority".
- **Doc drift:** `CONTROL_PIPELINE_MATH.md §6.1` describes anti-windup as a `B<B_max`
  gate but the code is integrate-then-clamp (`boost_stage.cpp:102-109`) — cosmetic;
  the audit confirmed no behavioral bug there.
- **Terminal/evidence-stage plan** (from the pre-audit handoff §3): fan-sweep
  calibrate → normal-time real run = the main test → ingest/prune/report; pre-check
  no stale pre-v13 `svg_mb_control.db`. The R1–R4 fixes should land before a run
  that intends to exercise degraded-sensor or fault paths.
- **Cross-repo:** `mmvg` + `svg` — deferred.
- **Stale branch:** `wip/watchdog-exe-stash` — unreviewed; left as-is.

## 6. Pointers

- **This session's deliverables:** `docs/codebase-audit-2026-06-30.md` (findings),
  `docs/codebase-audit-remediation-2026-06-30.md` (root cause + fix design + tests).
- **Feature state:** `docs/features/README.md` §"Current priority" + §5 registry;
  `docs/TRACEABILITY.md`.
- **Control identity:** `docs/CONTROL_PIPELINE_MATH.md` (one cosmetic §6.1 drift).
- **Sensor path:** `src/hardware/amd_reader.cpp` / `amd_decode.h`,
  `src/hardware/gpu_reader.cpp`, `src/control/channel_evaluator.cpp`.
- **Config:** `config/control.release.json` + `config/overlays/release.behavior.json`
  + `config/machines/snd-desk.cooling.policy.json`; `docs/COOLING_STRATEGY.md`.
- **CI:** `.github/workflows/ci-windows.yml` (runs `Test-LocalCI` on
  `windows-latest`). This Linux container cannot build the Windows toolchain — only
  the cross-platform Python lane (`test_feature_specs`) runs locally; the C++ build
  + 26-test CTest run only in CI.
