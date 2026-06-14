# Discovery: Runtime Disk Growth (2026-06-14)

Status: discovery finding, not promoted work. This document records observed
runtime-home disk usage on one machine at one point in time. It is not a
contract and does not authorize implementation. Tracking issue:
[espensev/SVG-MB-Control#4](https://github.com/espensev/SVG-MB-Control/issues/4).

## Context

Snapshot taken 2026-06-14 ~19:14 on MAINDESK while a `control-loop` session was
live (`current_state.json` health `healthy`, manifest `last_update`
2026-06-14T19:13). Branch `analyze-native-superset`, producer git
`57279e80973a`. All measured paths are under `release/runtime/`, which is
gitignored; no runtime data is committed.

## Measured footprint

| Artifact | Size | Bound state |
|---|---|---|
| `runtime/` total | 8.2 GB | — |
| `svg_mb_control.db` | 4.9 GB | unbounded — no age/size purge of old runs (confirmed real data, not bloat; see Finding 2) |
| `logs/archive/` (rotated CSV chunks) | 3.0 GB | within configured policy |
| `logs/svg_mb_control_events.jsonl` | 212 MB | unbounded — append-only, no retention |

Live config (`release/control.json`): `log_rotate_hours=4`,
`log_retain_days=7`, `csv_flush_interval_rows=4`.

## Finding 1 — event JSONL has no retention mechanism

`logs/svg_mb_control_events.jsonl` is 586,825 lines spanning 2026-05-21 to
2026-06-14. It is append-only across all sessions. 579,565 lines (98.8%) are
`control_loop.write_applied`.

`docs/RUNTIME_HOME.md` documents `log_rotate_hours` and `log_retain_days` for the
archive CSV chunks only. The event JSONL is documented as "append-only" with no
rotation or retention. This is a design gap, not an unset config value.

Robustness note: a NUL byte is present mid-file (`grep` falls back to binary mode
unless forced with `-a`), which indicates at least one partial or torn write.
Event-log append durability should be checked alongside retention.

Failure-event counts present in the same log are recorded here as observed facts
only; their root cause is out of scope for this disk-growth finding:
`control_loop.sidecar_upsert_failed` 1,397,
`control_loop.low_band_evidence_write_failed` 146,
`control_loop.write_failed` 13, `control_loop.init_failed` 5,
`reconcile.sidecar_cleanup_failed` 4, `control_loop.sidecar_load_failed` 1.

## Finding 2 — analyze SQLite DB has no size bound

`svg_mb_control.db` is 4.9 GB and was last written 2026-06-12 00:53, so the
current control-loop session is not writing it. It is produced by
`src/analyze/analyze_db.cpp`, where the `analyze` CLI ingests runtime CSV/JSONL
into SQLite.

This DB is the designed long-term store and the prune authority: per
`docs/RUNTIME_HOME.md`, `analyze prune` only deletes archive CSV bundles that
have already been ingested into it. Nothing bounds the DB itself — there is no
documented size cap, retention window, or VACUUM. The correct framing is that the
intended retention sink has no bound of its own. This area is the subject of the
active `analyze-native-superset` branch.

Resolved 2026-06-14 (internals inspected with the Python `sqlite3` module,
read-only). The 4.9 GB is expected accumulation, not bloat:

- `PRAGMA freelist_count` is 0 — zero free pages. The DB is fully packed and
  VACUUM would reclaim nothing. The size is not dead space from un-VACUUMed
  deletes. `PRAGMA page_count` 1,260,209 × 4,096-byte pages equals the file size,
  confirming all pages are live.
- `runs` holds 208 rows with 208 distinct `session_start` values — no duplicate
  ingestion. Idempotent ingest is correct.
- The size is genuine per-tick telemetry across 208 ingested control-loop
  sessions (2026-05-21 onward), 3,286,347 ticks. Each tick fans out to one
  `tick_samples` row, 7 `tick_fan_samples` rows (hardware fan state), and 7
  `tick_channel_samples` rows (per-channel control state) — 15 rows per tick —
  plus 900,917 ingested `events`. Exact row counts: `tick_fan_samples`
  23,004,429; `tick_channel_samples` 23,004,429; `tick_samples` 3,286,347;
  `events` 900,917; `runs` 208. `tick_fan_samples` and `tick_channel_samples`
  store different column sets and are not redundant.
- Effective cost is ~1.57 KB per tick, so ~500 MB per day of continuous
  control-loop running.

The growth is structural: full-fidelity per-tick ingest with no age- or
size-based purge of old runs. VACUUM is not the remedy. An age/size-based run
purge (`DELETE FROM runs ...`; `ON DELETE CASCADE` removes the dependent tick
rows) followed by a one-time VACUUM is. Per-table byte split was not measured
exactly because the `dbstat` virtual table is not compiled into this Python
SQLite build; the exact row counts above establish that the two 23 M-row tables
dominate the file.

## Not a problem

`logs/archive/` at 3.0 GB is within policy. The oldest chunk is dated
2026-06-07, the snapshot date is 2026-06-14, and `log_retain_days=7`, so the
archive holds exactly the configured 7-day window. Runtime pruning is working as
configured. If 3 GB per 7 days at the current CSV column width is undesired, that
is a tuning choice, not a defect.

## Suggested next steps (gated by the Feature Intake Gate in AGENTS.md)

1. Event JSONL: decide a retention model — size/age rotation like the CSV path,
   and/or severity-based persistence so `control_loop.write_applied` is not
   persisted per write. Likely needs a `docs/features/FEAT-*` spec.
2. Analyze DB: internals inspected (see Finding 2 — confirmed real data, not
   bloat). Decide an age/size-based purge of old `runs` (`ON DELETE CASCADE`
   removes dependent tick rows), followed by a one-time VACUUM. Likely needs a
   `docs/features/FEAT-*` spec.
3. Event-log durability: confirm the cause of the mid-file partial/torn write.

## Reproduction

```powershell
# Footprint
du -sh release/runtime release/runtime/svg_mb_control.db `
  release/runtime/logs/archive release/runtime/logs/svg_mb_control_events.jsonl

# Event distribution (force text mode; the file contains a NUL byte)
grep -aoE '"event_type":"[^"]*"' release/runtime/logs/svg_mb_control_events.jsonl `
  | sort | uniq -c | sort -rn

# Retention config
grep -nE 'log_retain_days|log_rotate_hours' release/control.json
```

DB internals (read-only; settles bloat-vs-expected):

```python
import sqlite3
con = sqlite3.connect(
    r"file:release/runtime/svg_mb_control.db?mode=ro", uri=True)
print("freelist", con.execute("PRAGMA freelist_count").fetchone()[0])  # 0 = no VACUUM gain
print("pages",    con.execute("PRAGMA page_count").fetchone()[0])
for t in ("runs", "tick_samples", "tick_fan_samples",
          "tick_channel_samples", "events"):
    print(t, con.execute(f'SELECT COUNT(*) FROM "{t}"').fetchone()[0])
print("distinct runs",
      con.execute("SELECT COUNT(DISTINCT session_start) FROM runs").fetchone()[0])
```
