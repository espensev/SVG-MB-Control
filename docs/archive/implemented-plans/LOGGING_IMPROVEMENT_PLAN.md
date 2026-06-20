# Logging Improvement Plan

Status: completed implementation record, compacted 2026-05-29. Current
operator guidance lives in `docs\RUNTIME_LOGGING_AND_EVALUATION.md`; runtime
artifact details live in `docs\RUNTIME_HOME.md`.

## Goal

Make controller tuning data-driven without turning the runtime into a larger
service. Keep raw logs local by default and commit compact summaries,
manifests, or decision records only when they justify a tuning change.

## Implemented

- Native `svg-mb-control analyze ingest` imports runtime manifests, CSV
  archives, JSONL events, and plant-model captures into SQLite.
- Native `analyze ingest --csv <path> [--events <path>]` covers bare archived
  control-loop CSVs with no runtime manifest.
- Native `svg-mb-control analyze report` emits text or JSON summaries, compact
  Markdown decision records, and analysis manifests with source/output hashes.
- `scripts\analyze_control_run.py` is only a raw-CSV convenience wrapper around
  native ingest/report. It no longer implements independent analysis logic.
- Runtime manifests are controller-owned:
  `logs\svg_mb_control_manifest.json` and
  `logs\archive\svg_mb_control_<mode>_<timestamp>.manifest.json`.
- Runtime CSV prologues include build/config/runtime-policy identity fields
  without changing row shape.
- Runtime JSONL events include normalized `severity` and `error_code` fields,
  and SQLite ingest stores them in first-class event columns.
- `control_runtime.json` exposes per-channel sensor/write failure state and
  breaker state; `--reset-breakers` writes the explicit reset request consumed
  by the control loop.
- Foreground `evidence-log` carries deeper backend timing/change diagnostics
  outside the control-loop hot path.

## Current Evidence Path

Use native ingest/report for new tuning work:

```powershell
release\svg-mb-control.exe analyze ingest --runtime-home .\release\runtime --db .\release\runtime\svg_mb_control.db
release\svg-mb-control.exe analyze report --runtime-home .\release\runtime --db .\release\runtime\svg_mb_control.db --idle-seconds 300 --profile combined-load --notes "ambient and subjective noise notes" --out run-summary.txt --manifest-out run-manifest.json
```

For a single raw CSV not yet in a runtime home, either call native ingest/report
directly with `analyze ingest --csv`, or use:

```powershell
python .\scripts\analyze_control_run.py --csv path\to\run.csv --events path\to\events.jsonl
```

## Deferred

- Keep per-sensor-group timing out of the control-loop CSV unless measured
  evidence shows unexplained jitter or runtime cost. Use foreground
  `evidence-log` first.
- Improve experiment accounting only when the native decision record is not
  enough for a concrete tuning decision.
