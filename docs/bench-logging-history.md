# Bench / Logging History

**Status:** historical summary, created 2026-06-09.

This file consolidates the older Bench-vs-Control logging discovery notes that
used to live as five separate flat docs:

- `docs/archive/discovery-bench-cpp-priority.md`
- `docs/archive/discovery-bench-logger-gap.md`
- `docs/archive/discovery-control-bench-logging.md`
- `docs/archive/discovery-current-vs-earlier.md`
- `docs/archive/discovery-logging-parity.md`

Use this summary for the durable conclusions. Open the archived originals only
when the dated evidence trail matters. The current operator and implementation
contracts are `docs/RUNTIME_LOGGING_AND_EVALUATION.md`,
`docs/RUNTIME_HOME.md`, `README.md`, and the source/tests.

## Durable Conclusions

Control is the shipping runtime. It must stay standalone and must not depend on
Bench, NVG, or other sibling repos at runtime. The useful lesson from Bench and
NVG was the artifact shape, not a dependency path:

- keep live state/recovery JSON separate from historical telemetry;
- write append-only CSV archives plus stable latest-file mirrors;
- record structured JSONL events for write, restore, failure, and lifecycle
  context;
- publish manifests with row/event accounting and artifact identity;
- keep rich characterization evidence outside the control hot path unless
  measured control-loop evidence proves it belongs there.

That direction has since shipped in Control. Normal validation now uses the
Control-owned runtime CSV, event log, manifests, native analyzer, and compact
decision records. Foreground `evidence-log` provides the wider read-only GPU,
SIO voltage/temperature, fan raw-byte, and backend-timing evidence that older
notes expected from Bench.

## What Changed Since The Original Notes

The original April and May discovery notes predated most of the current runtime
logging system. Several old findings are intentionally obsolete:

- Control no longer has only `current_state.json`, `control_runtime.json`, and
  `pending_writes.json`; it also owns rotated CSV archives, latest CSV mirrors,
  JSONL events, manifests, analyzer ingest/report/prune, and health/supervisor
  sidecars.
- The old "missing rich evidence" gap is closed for the foreground evidence
  plane: Control now captures richer GPU, SIO voltage/temperature, fan raw-byte,
  change-flag, and per-backend timing fields outside the controller hot path.
- The repo no longer needs Bench as a required external logger. Bench remains a
  characterization reference and source of design ideas only.
- Broad Python reduction was never a Control runtime blocker. Control uses
  Python for tests and thin offline helper flow, not for runtime behavior.

## Preserved Historical Facts

The old documents still preserve a few useful facts:

- the repo-count census from 2026-04-16, showing Control was already mostly C++
  while Bench still had a large helper/orchestration surface;
- the Bench export/no-import boundary: Control can port concepts, but runtime
  behavior must not call into a sibling repo;
- the two-plane model: a narrow controller plane for deterministic control and
  recovery, plus a richer read-only evidence plane for characterization;
- the warning not to copy Bench's full heavy logger stack into Control's active
  loop.

## Current Guidance

For runtime logging, tuning, and analyzer workflow, start with
`docs/RUNTIME_LOGGING_AND_EVALUATION.md`. For sidecar fields, status behavior,
log paths, manifests, and retention, use `docs/RUNTIME_HOME.md`. For control
math and CSV/status identities, use `docs/CONTROL_PIPELINE_MATH.md`.

Do not reopen any archived Bench/logging recommendation as implementation work
without checking the current source, tests, and feature-intake rules first.
