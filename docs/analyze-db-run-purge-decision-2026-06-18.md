# Decision: the analyze SQLite DB gets an age-based run-purge plus post-purge reclaim

**Project:** svg-mb-control
**Status:** Current (Accepted 2026-06-18)
**Owning feature:** `docs/features/FEAT-0016-analyze-db-run-purge.md`
(`REQ-DBRETAIN-*`)
**Companion to:** `AGENTS.md`, `docs/RUNTIME_HOME.md`, `README.md`,
`docs/MEASUREMENT_GATE.md`, `docs/discovery-runtime-disk-growth-2026-06-14.md`

## Context

`analyze prune` (`src/analyze/analyze_prune.cpp`, `RunAnalyzePrune`) deletes
**archive CSV bundles** older than `retain_days` and already ingested; it never
deletes DB rows. The DB is the designed long-term sink, yet nothing bounds it. The
only `DELETE FROM runs` statements are idempotent re-ingest deletes keyed by
manifest path / `session_start`+`mode` (`src/analyze/analyze_ingest_db.cpp:67,84`),
not age/size retention, and there is no `VACUUM` anywhere in `src/`.

Finding 2 of `docs/discovery-runtime-disk-growth-2026-06-14.md` (resolved
2026-06-14, read-only `sqlite3`) established the 4.9 GB is **genuine ingested
telemetry, not bloat**: `PRAGMA freelist_count` = 0 (VACUUM alone reclaims
nothing), 208/208 distinct `runs` (no duplicate ingest), file dominated by two
23,004,429-row `tick_*` tables at ~1.57 KB/tick (~500 MB/day). `runs(id)` is the
cascade parent for the dependent `tick_*` / `events` rows
(`src/analyze/analyze_db.cpp:41,108,113,147,175`). Issue
[#4](https://github.com/espensev/SVG-MB-Control/issues/4) Finding 2.

The 2026-06-18 live check after FEAT-0020 and validation found
`release/runtime/svg_mb_control.db` at 8,377,511,936 bytes (7.80 GiB). Because
the DB is a derived analyzer artifact, it was deleted as an immediate safe
reclaim, freeing that space without touching raw CSV/event files or live
sidecars. `release/runtime` was 3.439 GiB after the reclaim; the structural
retention gap remains until this feature is implemented.

## Options considered

- **Bound:** age window (`session_start < cutoff`) vs. total-size cap vs.
  run-count cap.
- **Location:** extend `analyze prune` with a DB-retention flag vs. a new
  `analyze db-prune` / `analyze vacuum` subcommand.
- **Reclaim:** unconditional `VACUUM` vs. VACUUM only after a purge that deleted
  rows vs. no reclaim.

## Decision

**Adopt: an age-based run-purge inside `analyze prune`, cascade-deleting old
`runs`, followed by a one-time `VACUUM` only when rows were deleted.** Reasons:

- **Age window**, exposed as `--db-retain-days`, mirrors the existing
  `analyze prune --retain-days` archive semantics — one mental model for the
  operator, and the natural unit for telemetry retention. A size cap is recorded as
  a follow-on knob in Scope and gate, not v1, because age is the dimension the
  existing CSV retention already uses.
- **Extend `analyze prune`** rather than add a subcommand: the prune path already
  holds the resolved DB path and the dry-run/`--apply` convention
  (`src/analyze/analyze_cli.cpp:41`, `PruneOptions`), so a DB-retention flag reuses
  that surface. The purge runs with `PRAGMA foreign_keys = ON` so `ON DELETE
  CASCADE` from `runs(id)` removes the dependent `tick_*` / `events` rows rather
  than orphaning them (`REQ-DBRETAIN-02`).
- **VACUUM only after a delete:** Finding 2 measured freelist = 0 on an un-purged
  DB, so an unconditional VACUUM is wasted I/O over a multi-GB file. Run the
  one-time reclaim only when the purge deleted runs (`REQ-DBRETAIN-03`).
- **Idempotent ingest preserved:** the purge touches only out-of-window `runs`; the
  by-manifest-path / by-session de-duplication (`IsManifestPathInDb` /
  `IsSessionInDb`) stays correct for retained runs, and the operation keeps the
  prune dry-run default with explicit `--apply` (`REQ-DBRETAIN-04`).

**Fold in W7-1 while here.** The discovery backlog's W7-1 — `log_retain_days == 0`
/ `log_rotate_hours == 0` silently disabling CSV pruning/rotation
(`src/runtime/runtime_csv_archive.cpp:403,452`) — is the same disk-growth family. A
zero `--db-retain-days` must be treated as "retention disabled" explicitly (logged),
not as a silent no-op, so the DB path does not reproduce the W7-1 trap.

## Scope and gate

- **Scope:** analyze-DB retention only. The CSV-bundle prune behavior and the
  per-tick ingest fidelity are unchanged (`REQ-DBRETAIN-05`).
- **Open (follow-on, not v1):** a total-size or run-count cap in addition to the age
  window, and whether the bound becomes a scheduled-maintenance config key rather
  than a CLI flag.
- **Measurement gate:** not crossed (`docs/MEASUREMENT_GATE.md`). Offline analyze
  CLI over a gitignored artifact; no live action, no schema/`schema_version`
  change (current analyze schema remains v11), no control-identity movement.
- **Implementation/verification** are authorized by this decision but remain
  pending product-code work. The purge/cascade/reclaim tests
  (`tests/test_analyze_ingest.py` sibling) must run under `Test-LocalCI` before
  the spec's §14 log is filled.
