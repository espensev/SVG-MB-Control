# Discovery - Dashboard Health Polling

**Goal:** Evaluate `tools/eval_dashboard/server.py::build_health_payload()` and the dashboard health/runtime polling path.
**Date:** 2026-05-18
**Status:** complete
**Recommended next:** Runtime-home alignment, freshness metadata, and metadata-gated live chart refresh have been implemented.

---

## Questions

1. What does `build_health_payload()` report, and where does it source health state?
2. How does the dashboard poll health compared with live runtime data?
3. Does the health path duplicate or conflict with normal runtime logging/polling?
4. What is the next actionable change?

---

## Findings

### Q1: What does `build_health_payload()` report, and where does it source health state?

**Answer:** It reads three JSON sidecars under `release/runtime`: `control_health.json`, `control_supervisor.json`, and `control_runtime.json`. The dashboard-side health state is not re-derived; it passes through the last persisted `--health` result from `control_health.json` and adds a small runtime/supervisor detail view.

**Evidence:**
- `tools/eval_dashboard/server.py:15` - `build_health_payload(repo_root)` starts the aggregation.
- `tools/eval_dashboard/server.py:22` - the base path is hardcoded to `<repo_root>/release/runtime`.
- `tools/eval_dashboard/server.py:30` - reads `control_health.json`.
- `tools/eval_dashboard/server.py:31` - reads `control_supervisor.json`.
- `tools/eval_dashboard/server.py:32` - reads `control_runtime.json`.
- `tools/eval_dashboard/server.py:34` - trims runtime details to a small field allowlist.
- `src/runtime_health.cpp:170` - persisted health is explicitly best-effort and written by the `--health` CLI path.
- `docs/RUNTIME_HOME.md:171` - documents `control_health.json` as the most recent health assessment.

**Implications:**
- The server is correctly keeping health evaluation in C++.
- The dashboard can show stale health if no watchdog or operator path periodically runs `--health`.
- The hardcoded `release/runtime` path is the main correctness limit when the active config uses a different runtime home.

### Q2: How does the dashboard poll health compared with live runtime data?

**Answer:** Health polls every 5 seconds. Live CSV and events are fetched once at startup from bounded tail endpoints. The charts can therefore show a startup snapshot while the health pill continues updating.

**Evidence:**
- `tools/eval_dashboard/dashboard.js:11` - live CSV endpoint is `/api/live-tail.csv?bytes=8000000`.
- `tools/eval_dashboard/dashboard.js:12` - live events endpoint is `/api/events-tail.jsonl?bytes=2000000`.
- `tools/eval_dashboard/dashboard.js:13` - health endpoint is `/api/health.json`.
- `tools/eval_dashboard/dashboard.js:14` - health poll interval is 5000 ms.
- `tools/eval_dashboard/dashboard.js:2676` - `loadHealth()` fetches and renders health.
- `tools/eval_dashboard/dashboard.js:2700` - `loadLiveRun()` fetches CSV/events.
- `tools/eval_dashboard/dashboard.js:2868` - `loadLiveRun()` runs once during bootstrap.
- `tools/eval_dashboard/dashboard.js:2870` - only health is scheduled with `setInterval`.

**Implications:**
- Health polling is cheap enough and separate from the heavier analysis path.
- A live operator view can be misleading: health may be current while chart data is old.
- Polling full CSV tails every 5 seconds would be wasteful; use file size/mtime gating or a slower opt-in refresh.

### Q3: Does the health path duplicate or conflict with normal runtime logging/polling?

**Answer:** It does not conflict with the runtime logging plane. Runtime logging remains CSV/JSONL plus sidecars; health is a summary of sidecars. The current issue is coordination and freshness, not duplicate logging.

**Evidence:**
- `docs/RUNTIME_LOGGING_AND_EVALUATION.md:58` - `control_runtime.json` is status, not per-tick data.
- `docs/RUNTIME_LOGGING_AND_EVALUATION.md:59` - CSV is the timing/response analysis source.
- `src/control_loop.cpp:626` - status writes are deliberately rate-limited to reduce disk I/O.
- `src/control_loop.cpp:628` - control-loop status writes every 10 ticks.
- `src/control_loop.cpp:640` - status sidecar is still updated by the control loop.
- `src/control_status_writer.cpp:84` - status sidecar publishes active CSV, manifest, and event log paths.
- `tools/eval_dashboard/server.py:143` - live CSV tail endpoint reads the fixed live mirror.
- `tools/eval_dashboard/server.py:156` - events tail endpoint reads the fixed event log.

**Implications:**
- Keep databases out of the live dashboard path for now.
- The dashboard should use the live sidecar metadata to locate logs and decide whether data changed.
- SQLite remains the right place for historical ingest, retention gates, and run summaries.

### Q4: What is the next actionable change?

**Answer:** Add runtime-home alignment, freshness metadata, and metadata-gated auto-refresh. The dashboard server should accept a runtime home, defaulting to `release/runtime`, and all health/tail endpoints should use it. The health payload should expose path and freshness metadata so the UI can tell "health current, data stale" from "health unavailable." The UI should use that same metadata as the low-cost trigger for refreshing live CSV/events data.

**Evidence:**
- `scripts/Start-EvalDashboard.ps1:13` - dashboard launch has no runtime-home option.
- `tools/eval_dashboard/server.py:22` - health path is hardcoded to `release/runtime`.
- `tools/eval_dashboard/server.py:144` - live CSV path is hardcoded to `release/runtime/logs/svg_mb_control_output.csv`.
- `tools/eval_dashboard/server.py:157` - event log path is hardcoded to `release/runtime/logs/svg_mb_control_events.jsonl`.
- `README.md:279` - docs describe the dashboard as loading only packaged `release/runtime` logs.

**Implications:**
- This is small, testable, and directly improves correctness for non-packaged/runtime-home experiments.
- It is a better next step than adding DB reads to the live dashboard.
- File metadata gating keeps refresh useful on low-end systems without repeatedly parsing unchanged CSV tails.

---

## Cross-Cutting Analysis

### Constraints

- `control_health.json` is the last persisted CLI health assessment, not an always-current health daemon output.
- `control_runtime.json` is intentionally rate-limited and cannot be treated as the per-tick source.
- The CSV/event live endpoints serve bounded tails, but parsing and re-rendering the full tail is still heavier than polling JSON health.
- The current dashboard assumes `release/runtime`; this is documented but not aligned with config-resolved runtime homes.

### Risks

| Risk | Likelihood | Impact | Notes |
|------|------------|--------|-------|
| Health pill updates while charts remain stale | Low | Medium | Metadata-gated refresh now reloads CSV/events when health metadata changes. |
| Dashboard reads the wrong runtime home | Low | Medium | Server and launcher now accept an explicit runtime home. |
| Polling CSV tails too often | Medium | Medium | Re-fetching and re-rendering 8 MB tails would waste CPU/browser time on low-end systems; metadata gating limits this. |
| Stale `control_health.json` looks authoritative | Low | Medium | UI exposes file age/freshness metadata. |

### Open Questions

All questions answered.

---

## Optimization Register

| Candidate | Type | Evidence | Risk | Confidence | Decision |
|-----------|------|----------|------|------------|----------|
| Add `--runtime-home` / `-RuntimeHome` to dashboard server and launcher | correctness | `server.py:22`, `server.py:144`, `server.py:157` | low | high | implemented |
| Add file metadata/freshness to `/api/health.json` | observability | `dashboard.js:2609`, `dashboard.js:2676` | low | high | implemented |
| Use `control_runtime.json` log paths when available for CSV/events | correctness | `control_status_writer.cpp:84` | medium | medium | suggest-next |
| Add live data refresh gated by mtime/size changes | hot-path | `dashboard.js:2868`, `dashboard.js:2870` | medium | medium | implemented |
| Read SQLite directly from dashboard | dependency | live path currently uses file sidecars only | medium | high | defer |

---

## Recommendation

Runtime-home/freshness pass implemented directly:

1. `tools/eval_dashboard/server.py` accepts `--runtime-home`, defaults to `release/runtime`, and uses it for health, CSV tail, and events tail endpoints.
2. `scripts/Start-EvalDashboard.ps1` accepts `-RuntimeHome` and passes the resolved path to the Python server.
3. `/api/health.json` includes `runtime_home`, `runtime_home_exists`, and per-file freshness metadata.
4. The health panel now shows file presence, age, and size for the health, runtime, live CSV, and events sidecars.
5. `tests/test_eval_dashboard.py` covers metadata and non-default runtime homes.

Metadata-gated live chart refresh has also been implemented on top of the health poll. The dashboard reuses `/api/health.json` file metadata as the cheap change detector, reloads CSV/events only when mtime or size changes, and disables live auto-refresh after manual CSV or events selection.
