# FEAT-0016: Analyze SQLite DB has a retention bound (age/size run-purge + reclaim)

**Project:** svg-mb-control
**Status:** Accepted (2026-06-18)   **Version:** 0.2   **Updated:** 2026-06-18
**Namespace:** `REQ-DBRETAIN-*`
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/RUNTIME_HOME.md`, `docs/discovery-runtime-disk-growth-2026-06-14.md`,
`docs/features/FEAT-0015-event-log-retention.md`
**Purpose:** investigate, and propose a maintainer-decidable direction for, the
analyze SQLite database (`svg_mb_control.db`), which is the designed long-term
telemetry sink and the prune authority but has no bound of its own — no age/size
purge of old runs and no space reclaim.

## 1. Summary

`analyze prune` (`src/analyze/analyze_prune.cpp`, `RunAnalyzePrune`) deletes
**archive CSV bundles** that are older than `retain_days` and already ingested
into the DB; it never deletes rows from the DB. The DB is the intended retention
sink — per `docs/RUNTIME_HOME.md`, `prune` only removes bundles that the DB has
absorbed — yet nothing bounds the DB itself. The only `DELETE FROM runs`
statements in the codebase are idempotent-reingest deletes keyed by manifest path
or by `session_start`+`mode` (`src/analyze/analyze_ingest_db.cpp:67,84`), not an
age- or size-based retention purge. There is no `VACUUM` anywhere in `src/`.

On the snapshot in `docs/discovery-runtime-disk-growth-2026-06-14.md` the DB was
4.9 GB. The Finding 2 investigation (resolved 2026-06-14, read-only `sqlite3`)
established the size is **genuine ingested telemetry, not bloat**:
`PRAGMA freelist_count` = 0 (so VACUUM alone reclaims nothing), 208/208 distinct
`runs` (no duplicate ingest), with the file dominated by two 23,004,429-row
`tick_fan_samples` / `tick_channel_samples` tables at ~1.57 KB/tick (~500 MB/day
of continuous running). The remedy is therefore an age/size-based purge of old
`runs` — `ON DELETE CASCADE` from `runs(id)` drops the dependent `tick_*` and
`events` rows (`src/analyze/analyze_db.cpp:41,108,113,147,175`) — **followed by**
a one-time `VACUUM` to shrink the file once deletes have created free pages.

This spec structures that gap and proposes one direction (an age/size run-purge
plus a reclaim step). **It does not authorize code and does not assert a bound is
chosen**; the retention window, where the purge lives, and the reclaim trigger are
left as a maintainer decision (§11).

## 2. Problem & motivation  *(promotion gate 1)*

Sourced from observed runtime evidence (`docs/discovery-runtime-disk-growth-2026-06-14.md`,
Finding 2 and its resolved open question; tracking issue
[espensev/SVG-MB-Control#4](https://github.com/espensev/SVG-MB-Control/issues/4))
and a static-verified code gap.

1. **The retention sink has no bound of its own.** `RunAnalyzePrune`
   (`analyze_prune.cpp:273-379`) scans archive manifests, skips bundles that are
   `running`, within retention, or not ingested, and deletes only the eligible
   **CSV+manifest bundle** files. It opens the DB read-only through `IngestIndex`
   purely to confirm ingestion; it issues no `DELETE` and no `VACUUM` against the
   DB. So pruning frees CSV archive space while the DB — where the same telemetry
   now lives — keeps growing unbounded.

2. **No age/size run-purge exists.** The `DELETE FROM runs WHERE manifest_path = ?`
   and `DELETE FROM runs WHERE session_start = ? AND mode = ?` statements
   (`analyze_ingest_db.cpp:67-72,84-92`) are called only to replace a run before
   re-ingesting it (idempotent ingest). Neither is age- or size-bounded; there is
   no `WHERE session_start < <cutoff>` retention path and no run-count cap.

3. **No space reclaim exists, and it is needed only after deletes.** There is no
   `VACUUM` in `src/`. Finding 2 measured `PRAGMA freelist_count` = 0 on the
   un-purged DB, so VACUUM today reclaims nothing — the file is fully live pages.
   Reclaim becomes meaningful **after** a run-purge frees pages; a one-time
   `VACUUM` then shrinks the file. VACUUM is not an every-ingest operation.

4. **Cascade is the lever, if foreign keys are enforced.** `runs(id)` is the
   cascade parent for `tick_samples`, `tick_fan_samples`, `tick_channel_samples`,
   `plant_model_*`, and the ingested `events`
   (`analyze_db.cpp:41,108,113,147,175`; `analyze_channel_sample_columns.cpp:155`),
   so deleting old `runs` rows drops the dependent ~15-rows-per-tick detail. SQLite
   honors `ON DELETE CASCADE` only when `PRAGMA foreign_keys = ON` for the
   connection performing the delete; the purge must guarantee this so dependent
   rows are removed rather than orphaned.

## 3. Goals & non-goals

**Goals**
- Give the analyze DB an enforced upper bound: an age/size-based purge of old
  `runs` so the file does not grow without limit across ingested sessions.
- Reclaim disk after a purge: a one-time `VACUUM` (or equivalent) once the purge
  has created free pages, so the file shrinks rather than only stopping growth.
- Keep ingestion idempotent and correct: a purge of old runs must not corrupt the
  by-manifest-path / by-session de-duplication that `analyze ingest` relies on.

**Non-goals**
- No change to what `analyze ingest` captures per tick (the full-fidelity
  `tick_*` rows), the analyze schema columns, or `schema_version` beyond what a
  retention bound requires.
- No change to the existing `analyze prune` archive-bundle behavior; this feature
  adds DB-side retention, it does not alter CSV bundle pruning.
- No change to the runtime event JSONL retention; that is the separate FEAT-0015
  (`REQ-EVENTRET-*`), cross-referenced in §12.
- No change to live runtime control behavior — this is an offline `analyze`-side
  maintenance operation on a gitignored runtime artifact.

## 4. Stressed invariants  *(promotion gate 2)*

| Invariant | Source | How this proposal stays inside it |
|---|---|---|
| Runtime sidecar / status / manifest / archive schema stays backward-compatible | `docs/RUNTIME_HOME.md` | The analyze schema columns and `schema_version` are unchanged; the purge deletes rows by age/size and relies on existing `ON DELETE CASCADE`. No runtime-home file or archive becomes invalid. |
| No fan write / start / stop / breaker reset outside an explicit live task | `AGENTS.md` §Live Runtime Safety | `analyze` is an offline CLI over the runtime DB; the purge issues no live action and touches no control path. |
| Repo stays standalone; no sibling-repo / bridge dependency | `AGENTS.md` §Repo Boundary | The change is confined to `src/analyze/*` over the vendored `third_party/sqlite3`; no external dependency. |
| Shipped 250 ms cadence / channel set / input strategy is the measured baseline | `docs/MEASUREMENT_GATE.md` | Offline analyze-side retention only; cadence, channels, and input strategy are unchanged, so the gate baseline does not move. |
| Control-computation identity stays documented and validated | `docs/CONTROL_PIPELINE_MATH.md` | No control term changes; this operates on stored telemetry after the fact. |

## 5. Behavior specification

Behavior is **proposed (not yet implemented)** and is one direction for the
maintainer to accept, refine, or reject. It lives in `src/analyze/` near the
prune path (`analyze_prune.cpp`) and the run-delete helpers
(`analyze_ingest_db.cpp`).

- **Age/size run-purge.** An `analyze` operation deletes `runs` rows that fall
  outside the accepted bound — older than a retention window (`session_start <
  cutoff`), and/or beyond a total-size or run-count cap, keeping the most recent.
  The exact bound is an open decision (§11).
- **Cascade-driven detail removal.** With `PRAGMA foreign_keys = ON`, deleting a
  `runs` row removes its dependent `tick_samples`, `tick_fan_samples`,
  `tick_channel_samples`, and ingested `events` via `ON DELETE CASCADE`
  (`analyze_db.cpp:41,108,113,147,175`). The purge must verify foreign-key
  enforcement so no detail rows are orphaned.
- **Reclaim after purge.** After a purge that deleted rows, a one-time `VACUUM`
  (or `PRAGMA incremental_vacuum` if auto-vacuum is enabled) reclaims the freed
  pages and shrinks the file. Reclaim runs only when the purge actually deleted
  runs (Finding 2: freelist is 0 on an un-purged DB, so an unconditional VACUUM is
  wasted work).
- **Idempotent ingest preserved.** Purging old runs must not break the
  by-manifest-path / by-session de-duplication (`IsManifestPathInDb`,
  `IsSessionInDb`, `analyze_ingest_db.cpp:60-92`): a purged run whose archive
  bundle still exists may be re-ingestable, which is acceptable; a retained run
  must continue to de-duplicate correctly.
- **Dry-run parity.** The purge follows the existing prune convention of a
  dry-run default and an explicit `--apply`, reporting selected vs. deleted rows
  and reclaimed bytes (mirroring the `RunAnalyzePrune` summary line at
  `analyze_prune.cpp:365-377`).

## 6. Requirements  *(promotion gate 4)*

| ID | Requirement |
|---|---|
| REQ-DBRETAIN-01 | The analyze DB must gain an age/size-based purge of old `runs` (a bounded `DELETE FROM runs WHERE ...`), so the DB does not grow without limit across ingested sessions. The accepted bound (age window, size cap, and/or run-count cap, and which runs are retained) is recorded in §11 and the design decision before implementation. |
| REQ-DBRETAIN-02 | The purge must remove dependent detail rows, not orphan them: it must run with `PRAGMA foreign_keys = ON` so `ON DELETE CASCADE` from `runs(id)` drops the dependent `tick_samples` / `tick_fan_samples` / `tick_channel_samples` / ingested `events`, and a post-purge check must confirm no detail rows reference a deleted run. |
| REQ-DBRETAIN-03 | After a purge that deleted runs, the operation must reclaim freed space via a one-time `VACUUM` (or incremental vacuum), and it must not run an unconditional VACUUM when nothing was deleted (Finding 2: freelist is 0 on an un-purged DB). |
| REQ-DBRETAIN-04 | The purge must preserve idempotent ingest: the by-manifest-path and by-session de-duplication (`IsManifestPathInDb` / `IsSessionInDb`) must remain correct for retained runs, and the operation must default to dry-run with an explicit `--apply`, reporting selected/deleted runs and reclaimed bytes. |
| REQ-DBRETAIN-05 | The change must be confined to offline `analyze`-side retention: the analyze schema columns and `schema_version`, the per-tick ingest fidelity, and the existing `analyze prune` archive-bundle behavior are unchanged; `docs/RUNTIME_HOME.md` and `README.md` (analyze workflow) are updated at implementation. |

## 7. Data / schema deltas

- New/changed fields: none. The analyze schema (`runs`, `tick_samples`,
  `tick_fan_samples`, `tick_channel_samples`, `events`, `plant_model_*`) and
  `schema_version` (10) are unchanged; the purge uses existing columns
  (`session_start`, `runs(id)`) and existing `ON DELETE CASCADE`.
- Config impact (`config/control.*.json`, `config/machines/*.json`): none unless
  the bound is made config-driven rather than a CLI flag; recorded as an open
  decision (§11).
- Schema/version impact: none. Update `docs/RUNTIME_HOME.md` (the DB now has a
  documented retention bound, narrowing the current "no DB-side retention/VACUUM"
  framing) and `README.md` (analyze maintenance workflow) at implementation.

## 8. CLI / config / operator surface deltas

- Proposed operator surface (one of, open decision §11): extend `analyze prune`
  with a DB-retention flag (e.g. `--db-retain-days` / `--db-max-runs`), or add a
  sibling subcommand (e.g. `analyze db-prune` / `analyze vacuum`). Either reuses
  the existing `analyze` flag conventions — `--db <path>`, `--dry-run|--apply`,
  `--quiet` (`src/analyze/analyze_cli.cpp:41`) and the `PruneOptions` shape
  (`src/analyze/analyze_prune.h`). UI is out of scope (`docs/MEASUREMENT_GATE.md`).
- Doc updates at implementation are `docs/RUNTIME_HOME.md` and `README.md` per
  `AGENTS.md` §Change Checklist.

## 9. Design decision record(s)  *(promotion gate 3 — write before implementation)*

| Decision doc | Decision it must settle | Status |
|---|---|---|
| `docs/analyze-db-run-purge-decision-2026-06-18.md` | Retention bound, location, and reclaim: **accepted** — age-based `--db-retain-days` purge inside `analyze prune`, cascade-delete old `runs` under `foreign_keys=ON`, one-time `VACUUM` only when rows were deleted; zero retain disables explicitly (W7-1 guard); size/run-count cap deferred. | Accepted 2026-06-18 (current) |

## 10. Acceptance criteria & verification mapping  *(promotion gate 5)*

| Requirement | Verify (T/B/M/R) | Where |
|---|---|---|
| REQ-DBRETAIN-01 | T, R | `.\scripts\Test-LocalCI.ps1` (`tests/test_analyze_ingest.py` sibling): ingest several runs spanning the bound, run the purge, assert out-of-bound runs are deleted and in-bound runs retained; review vs the design decision recording the bound. |
| REQ-DBRETAIN-02 | T | Test asserts that after purging a run, no `tick_samples` / `tick_fan_samples` / `tick_channel_samples` / `events` rows reference the deleted `run_id` (cascade fired under `foreign_keys = ON`). |
| REQ-DBRETAIN-03 | T | Test asserts page/file-size reclaim after a purge that deleted runs (`page_count` drops), and that no VACUUM runs when nothing was deleted. |
| REQ-DBRETAIN-04 | T, R | Test asserts retained runs still de-duplicate on re-ingest (`IsManifestPathInDb` / `IsSessionInDb`), and that dry-run reports without deleting while `--apply` deletes; review vs the `analyze prune` dry-run convention. |
| REQ-DBRETAIN-05 | R | Review vs `docs/RUNTIME_HOME.md` / `docs/MEASUREMENT_GATE.md`: analyze schema/`schema_version`, per-tick fidelity, and existing CSV-bundle prune unchanged; offline-only; docs updated. |

Verify legend:
- **T** = automated test (`.\scripts\Test-LocalCI.ps1`, C++ smoke / pytest under `tests/`).
- **B** = build/release gate (`.\build-release.ps1` / `scripts\Build-Release.ps1`).
- **M** = manual runtime measurement (runtime CSV / status / event-log evidence; respects `AGENTS.md` §Live Runtime Safety).
- **R** = code review against the cited contract doc.

## 11. Open decisions

| Decision | Needed before | Current default |
|---|---|---|
| Whether to add DB-side retention at all, or accept the current unbounded sink. | promotion | Hold; the current behavior is shipped behavior until a maintainer judges the unbounded DB material (Finding 2 confirms it is real telemetry, ~500 MB/day, not bloat). |
| The bound: age window (`session_start < cutoff`), total-size cap, run-count cap, or a combination, and which runs are retained. | implementation | Undecided. An age window mirrors `analyze prune --retain-days`, but the maintainer may prefer a size or run-count cap given ~500 MB/day. |
| Where the purge lives: extend `analyze prune` vs. a new `analyze db-prune` / `analyze vacuum` subcommand. | implementation | Undecided. Extending `analyze prune` reuses the bundle-retention window; a separate subcommand keeps DB reclaim explicit. |
| Whether VACUUM runs automatically after a purge or as a separate explicit step. | implementation | Lean: automatic only when runs were deleted (REQ-DBRETAIN-03); never unconditional. |
| Whether the bound is CLI-flag or config-driven. | implementation | Lean: CLI flag initially (no runtime config surface), revisited if scheduled maintenance wants a config key. |

## 12. Measurement gate & dependencies

- **Measurement gate:** not crossed. This is offline analyze-side retention on a
  gitignored runtime artifact; it does not change cadence, live channels, or
  mixed-input strategy and adds no control term, so no characterization evidence
  is required before a decision (`docs/MEASUREMENT_GATE.md`).
- **Depends on:** the analyze DB layer (`src/analyze/analyze_db.cpp` schema +
  `ON DELETE CASCADE`, `src/analyze/analyze_ingest_db.cpp` run helpers) and the
  prune path (`src/analyze/analyze_prune.{h,cpp}`, `src/analyze/analyze_cli.cpp`).
  Independent of FEAT-0015 (event JSONL retention); the two together close the two
  unbounded artifacts in issue #4 but touch different code and can land separately.
- **Build/test impact:** new tests alongside `tests/test_analyze_ingest.py`
  (purge-by-bound, cascade completeness, reclaim-after-delete, idempotent-ingest
  preservation, dry-run/apply); doc updates to `docs/RUNTIME_HOME.md` and
  `README.md` per `AGENTS.md` §Change Checklist. No `docs/CONTROL_PIPELINE_MATH.md`
  change.

## 13. Promotion-gate checklist  *(all must pass before this is buildable work)*

- [x] 1. Problem statement sourced from observed runtime evidence or a named code/contract gap (§2 — runtime-observed in `docs/discovery-runtime-disk-growth-2026-06-14.md` Finding 2 with read-only DB inspection, and static-verified at `analyze_prune.cpp` / `analyze_ingest_db.cpp`).
- [x] 2. Stressed invariant(s) identified, including Repo Boundary, Live Runtime Safety, and Measurement Gate where they apply (§4).
- [x] 3. Required design decision record(s) written and marked current (§9 — `docs/analyze-db-run-purge-decision-2026-06-18.md`, Accepted 2026-06-18).
- [x] 4. Concrete `REQ-DBRETAIN-*` IDs assigned from the reserved namespace (§6).
- [x] 5. Verification mapped to real checks — `Test-LocalCI`, contract review (§10), to be mirrored in `docs/TRACEABILITY.md` on acceptance.
- [x] 6. Confirmed it does not violate `AGENTS.md` §Live Runtime Safety or §Repo Boundary, and does not silently move the `MEASUREMENT_GATE.md` baseline (offline analyze-side; schema/identity unchanged).
- [x] 7. Doctrine check: current behavior claims grounded with file:line; proposed behavior labeled as proposed; `must`/`should`/`is` used per `CLAUDE.md`; no undefined terms or unqualified vague adjectives.

> Promoted to Accepted 2026-06-18: the maintainer accepted the age-based run-purge
> inside `analyze prune` + post-purge VACUUM direction (§9 decision record). All
> seven promotion gates pass; the spec is buildable. Implementation and
> verification are staged for a Windows-host session (this repo's build is
> Windows-only), after which §14 is filled.

## 14. Verification log  *(fill in after the feature is built — "check against the spec later")*

Not started — Accepted 2026-06-18, not yet implemented. Each row is filled after
implementation, which is staged for a Windows-host session where `Test-LocalCI`
can build and run the tests (this repo's build is Windows-only).

| Requirement | Result (pass/fail) | Evidence (test run / commit / CSV / note) | Checked (date) |
|---|---|---|---|
| REQ-DBRETAIN-01 | | | |
| REQ-DBRETAIN-02 | | | |
| REQ-DBRETAIN-03 | | | |
| REQ-DBRETAIN-04 | | | |
| REQ-DBRETAIN-05 | | | |

**Spec vs. implementation deltas:** none yet (not implemented).
