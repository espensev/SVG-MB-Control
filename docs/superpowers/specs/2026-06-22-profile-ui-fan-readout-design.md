# Profile-Switch UI — Fan Readout (design)

**Date:** 2026-06-22
**Component:** `tools/profile_switch_ui` (local Rust operator helper)
**Status:** design, pending implementation plan

## Goal

Add a live per-channel fan readout to the local profile-switch UI so an operator
can see, at a glance, what each fan is doing while choosing and applying a
profile.

## Scope & non-goals

This is a display enhancement to the **non-shipped local dev helper** only. It is
outside the `AGENTS.md` Feature Intake Gate: no controller capability, runtime
protocol, schema/status/log field, CLI surface, or shipped-config behavior
changes. Reads only; no new write path.

Non-goals (explicitly out):

- No commanded-vs-actual duty divergence diagnostics (that is FEAT-0005 territory).
- No friendly fan names/labels — channels are shown as `ch0`..`ch5`.
- No live no-flicker JS panel and no configurable refresh interval (see Future).

## Data

The helper already resolves `runtime_home` from
`svg-mb-control.exe --health --json`. Two files under that home feed the readout:

- `current_state.json` → `fans[]`: `channel`, `duty_percent` (actual hardware
  readback), `rpm`, `tach_valid`.
- `control_runtime.json` → `controlled_channels[]`: `channel`,
  `last_observed_temp_c`, `last_primary_temp_source`, `controller_kind`.

Rows are the union of channels found, joined by `channel`, sorted ascending.

Field rules:

- **Duty %** — `fans[].duty_percent`, 1 decimal.
- **RPM** — `fans[].rpm` when `tach_valid` is true, else `—`.
- **Temp** — `controlled_channels[].last_observed_temp_c` with the source in
  parentheses, e.g. `42 °C (gpu)`.
- **Controller** — `controlled_channels[].controller_kind` (e.g. `curve_overlay`,
  `pid`).
- A channel present in only one source renders the available fields and `—` for
  the rest.

## Parsing

Add `serde` + `serde_json` to `Cargo.toml`. Define typed structs holding only the
fields above, with `#[serde(default)]` for resilience to missing keys. Replace
the existing hand-rolled `extract_json_string_field` / `parse_json_string` reads
(health `state`/`health`/`worker_pid`/`runtime_home`, `active_profile_name` /
`active_profile_source`, profile `_comment`) with `serde_json` deserialization,
and remove the hand-rolled string extractor entirely. This also retires the two
review nits about that parser (the `\uXXXX` handling and the naive first-key
match).

## UI

- A new `<section class="fans">` containing a table with columns
  **Fan · Duty % · RPM · Temp · Controller**, placed under the existing
  state grid (Health / Active profile / Source) and above the profile-switch
  table. Reuse the existing table CSS.
- Whole-page auto-refresh: add `<meta http-equiv="refresh" content="3">` to the
  page `<head>` so the page reloads roughly every 3 seconds.

Accepted trade-offs of whole-page refresh: each reload re-runs `--health` and
`--status` (two subprocess spawns per ~3 s — fine for a local helper), and a
just-shown "profile switch requested" notice clears on the next reload.

## Error handling

- If `current_state.json` or `control_runtime.json` is missing, unreadable, or
  fails to parse, the fan section renders a `fan data unavailable` message and the
  rest of the page (profiles, switch, status) still renders. No panic.
- Per-file: a parse failure degrades only that file's contribution (e.g. snapshot
  parse fails → duty/rpm columns show `—`, temp/controller still show if
  `control_runtime.json` parsed).

## Testing (`cargo test`)

- `fans[] + channels[]` joined → expected rows (all fields populated).
- `tach_valid=false` → RPM renders `—`.
- channel present in only one source → partial row with `—` fillers.
- missing / malformed JSON → `unavailable`, no panic.
- `cargo build` and `cargo clippy` clean.

## Future (not now)

- Live no-flicker fan panel via a read-only `GET /fans` JSON endpoint + small JS
  poll.
- Commanded-vs-actual duty divergence flag (non-actuating fan hint).
- Friendly fan names and a configurable refresh interval.
