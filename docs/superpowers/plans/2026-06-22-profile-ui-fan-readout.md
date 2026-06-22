# Profile-Switch UI Fan Readout Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a live per-channel fan readout (duty % · RPM · temp · controller kind) to the local profile-switch UI, auto-refreshing every ~3 s.

**Architecture:** Introduce a `runtime` module in the Rust helper that uses `serde_json` to parse the controller's `--health --json`, `control_runtime.json`, and `current_state.json`, and joins the snapshot's hardware readback (`fans[]`) with the controller's per-channel state (`controlled_channels[]`) into `FanRow`s. The HTTP/render layer in `main.rs` reads the files, calls the module, renders a Fans table, and adds a whole-page meta-refresh. The hand-rolled JSON string extractor is retired.

**Tech Stack:** Rust 2021, `serde` + `serde_json`, std-only HTTP (existing). No controller/runtime changes.

## Global Constraints

- Component is `tools/profile_switch_ui` only — a **non-shipped local operator helper**. No controller, runtime-protocol, schema/status/log field, CLI surface, or shipped-config change (outside the `AGENTS.md` Feature Intake Gate).
- Reads only. The existing `POST /switch` same-origin guard and `--set-profile` path are untouched.
- Rust edition `2021`. New deps: `serde = { version = "1", features = ["derive"] }`, `serde_json = "1"`.
- All runtime JSON parsing tolerates missing/extra keys (`#[serde(default)]`) and never panics on malformed input — degrade to "unavailable".
- Channels are shown as `ch0`..`ch5` (no friendly names). Duty shown is the **actual hardware readback** (`fans[].duty_percent`).
- Run `cargo build`, `cargo test`, `cargo clippy` from `tools/profile_switch_ui`; all must be clean.

---

### Task 1: `runtime` module — serde structs + fan-row join

**Files:**
- Modify: `tools/profile_switch_ui/Cargo.toml` (add deps)
- Create: `tools/profile_switch_ui/src/runtime.rs`
- Modify: `tools/profile_switch_ui/src/main.rs` (add `mod runtime;` near the top, after the `use` block)

**Interfaces:**
- Produces:
  - `runtime::HealthInfo { health_state: Option<String>, status: Option<String>, process_id: Option<i64>, runtime_home: Option<String> }`
  - `runtime::RuntimeJson { active_profile_name: Option<String>, requested_profile_name: Option<String>, active_profile_source: Option<String>, controlled_channels: Vec<...> }`
  - `runtime::FanRow { channel: u32, duty_percent: Option<f64>, rpm: Option<i64>, tach_valid: bool, observed_temp_c: Option<f64>, temp_source: Option<String>, controller_kind: Option<String> }`
  - `runtime::parse_health(&str) -> HealthInfo`
  - `runtime::parse_runtime(&str) -> RuntimeJson`
  - `runtime::parse_profile_comment(&str) -> Option<String>`
  - `runtime::build_fan_rows(snapshot_json: Option<&str>, runtime_json: Option<&str>) -> Vec<FanRow>`

- [ ] **Step 1: Add dependencies to `Cargo.toml`**

Replace the `[dependencies]` section (currently empty) in `tools/profile_switch_ui/Cargo.toml`:

```toml
[dependencies]
serde = { version = "1", features = ["derive"] }
serde_json = "1"
```

- [ ] **Step 2: Write the failing test (create `src/runtime.rs` with the API + tests)**

Create `tools/profile_switch_ui/src/runtime.rs`:

```rust
use serde::Deserialize;
use std::collections::BTreeMap;

#[derive(Debug, Default, Deserialize)]
pub struct HealthInfo {
    #[serde(default)]
    pub health_state: Option<String>,
    #[serde(default)]
    pub status: Option<String>,
    #[serde(default)]
    pub process_id: Option<i64>,
    #[serde(default)]
    pub runtime_home: Option<String>,
}

#[derive(Debug, Default, Deserialize)]
pub struct RuntimeJson {
    #[serde(default)]
    pub active_profile_name: Option<String>,
    #[serde(default)]
    pub requested_profile_name: Option<String>,
    #[serde(default)]
    pub active_profile_source: Option<String>,
    #[serde(default)]
    pub controlled_channels: Vec<ChannelEntry>,
}

#[derive(Debug, Default, Deserialize)]
pub struct ChannelEntry {
    #[serde(default)]
    pub channel: Option<u32>,
    #[serde(default)]
    pub controller_kind: Option<String>,
    #[serde(default)]
    pub last_observed_temp_c: Option<f64>,
    #[serde(default)]
    pub last_primary_temp_source: Option<String>,
}

#[derive(Debug, Default, Deserialize)]
struct SnapshotJson {
    #[serde(default)]
    fans: Vec<FanEntry>,
}

#[derive(Debug, Default, Deserialize)]
struct FanEntry {
    #[serde(default)]
    channel: Option<u32>,
    #[serde(default)]
    duty_percent: Option<f64>,
    #[serde(default)]
    rpm: Option<i64>,
    #[serde(default)]
    tach_valid: bool,
}

#[derive(Debug, Default, Deserialize)]
struct ProfileMeta {
    #[serde(default, rename = "_comment")]
    comment: Option<String>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct FanRow {
    pub channel: u32,
    pub duty_percent: Option<f64>,
    pub rpm: Option<i64>,
    pub tach_valid: bool,
    pub observed_temp_c: Option<f64>,
    pub temp_source: Option<String>,
    pub controller_kind: Option<String>,
}

impl FanRow {
    fn empty(channel: u32) -> Self {
        FanRow {
            channel,
            duty_percent: None,
            rpm: None,
            tach_valid: false,
            observed_temp_c: None,
            temp_source: None,
            controller_kind: None,
        }
    }
}

pub fn parse_health(json: &str) -> HealthInfo {
    serde_json::from_str(json).unwrap_or_default()
}

pub fn parse_runtime(json: &str) -> RuntimeJson {
    serde_json::from_str(json).unwrap_or_default()
}

pub fn parse_profile_comment(json: &str) -> Option<String> {
    serde_json::from_str::<ProfileMeta>(json)
        .ok()
        .and_then(|m| m.comment)
}

/// Join the hardware snapshot (`fans[]`) with the controller's per-channel
/// state (`controlled_channels[]`) into one row per channel, sorted ascending.
/// Either input may be absent; malformed JSON degrades to no contribution.
pub fn build_fan_rows(snapshot_json: Option<&str>, runtime_json: Option<&str>) -> Vec<FanRow> {
    let mut rows: BTreeMap<u32, FanRow> = BTreeMap::new();

    if let Some(text) = snapshot_json {
        let snap: SnapshotJson = serde_json::from_str(text).unwrap_or_default();
        for fan in snap.fans {
            if let Some(ch) = fan.channel {
                let row = rows.entry(ch).or_insert_with(|| FanRow::empty(ch));
                row.duty_percent = fan.duty_percent;
                row.rpm = fan.rpm;
                row.tach_valid = fan.tach_valid;
            }
        }
    }

    if let Some(text) = runtime_json {
        let rt: RuntimeJson = serde_json::from_str(text).unwrap_or_default();
        for ch_entry in rt.controlled_channels {
            if let Some(ch) = ch_entry.channel {
                let row = rows.entry(ch).or_insert_with(|| FanRow::empty(ch));
                row.observed_temp_c = ch_entry.last_observed_temp_c;
                row.temp_source = ch_entry.last_primary_temp_source;
                row.controller_kind = ch_entry.controller_kind;
            }
        }
    }

    rows.into_values().collect()
}

#[cfg(test)]
mod tests {
    use super::*;

    const SNAPSHOT: &str = r#"{
        "fans": [
            {"channel": 0, "duty_percent": 15.6, "rpm": 600, "tach_valid": true},
            {"channel": 1, "duty_percent": 22.0, "rpm": 0, "tach_valid": false}
        ]
    }"#;

    const RUNTIME: &str = r#"{
        "active_profile_name": "control",
        "active_profile_source": "explicit_config",
        "controlled_channels": [
            {"channel": 0, "controller_kind": "curve_overlay", "last_observed_temp_c": 42.0, "last_primary_temp_source": "gpu"},
            {"channel": 2, "controller_kind": "pid", "last_observed_temp_c": 50.0, "last_primary_temp_source": "cpu"}
        ]
    }"#;

    #[test]
    fn joins_snapshot_and_runtime_by_channel() {
        let rows = build_fan_rows(Some(SNAPSHOT), Some(RUNTIME));
        // channels 0, 1 (snapshot only), 2 (runtime only) -> 3 rows, sorted
        assert_eq!(rows.iter().map(|r| r.channel).collect::<Vec<_>>(), vec![0, 1, 2]);

        let ch0 = &rows[0];
        assert_eq!(ch0.duty_percent, Some(15.6));
        assert_eq!(ch0.rpm, Some(600));
        assert!(ch0.tach_valid);
        assert_eq!(ch0.observed_temp_c, Some(42.0));
        assert_eq!(ch0.temp_source.as_deref(), Some("gpu"));
        assert_eq!(ch0.controller_kind.as_deref(), Some("curve_overlay"));
    }

    #[test]
    fn channel_present_in_one_source_only_has_partial_row() {
        let rows = build_fan_rows(Some(SNAPSHOT), Some(RUNTIME));
        // ch1: snapshot only -> no temp/controller
        let ch1 = rows.iter().find(|r| r.channel == 1).unwrap();
        assert_eq!(ch1.duty_percent, Some(22.0));
        assert!(!ch1.tach_valid);
        assert_eq!(ch1.observed_temp_c, None);
        assert_eq!(ch1.controller_kind, None);
        // ch2: runtime only -> no duty/rpm
        let ch2 = rows.iter().find(|r| r.channel == 2).unwrap();
        assert_eq!(ch2.duty_percent, None);
        assert_eq!(ch2.rpm, None);
        assert_eq!(ch2.controller_kind.as_deref(), Some("pid"));
    }

    #[test]
    fn missing_or_malformed_inputs_do_not_panic() {
        assert!(build_fan_rows(None, None).is_empty());
        assert!(build_fan_rows(Some("not json"), Some("{")).is_empty());
        // runtime present, snapshot garbage -> rows from runtime only
        let rows = build_fan_rows(Some("garbage"), Some(RUNTIME));
        assert_eq!(rows.len(), 2);
        assert_eq!(rows[0].duty_percent, None);
    }

    #[test]
    fn parses_health_and_profile_comment() {
        let h = parse_health(r#"{"health_state":"healthy","process_id":23940,"runtime_home":"D:\\rt"}"#);
        assert_eq!(h.health_state.as_deref(), Some("healthy"));
        assert_eq!(h.process_id, Some(23940));
        assert_eq!(h.runtime_home.as_deref(), Some("D:\\rt"));
        assert_eq!(
            parse_profile_comment(r#"{"_comment":"quiet  desk"}"#).as_deref(),
            Some("quiet  desk")
        );
        assert_eq!(parse_profile_comment("{}"), None);
    }
}
```

- [ ] **Step 3: Wire the module into `main.rs`**

In `tools/profile_switch_ui/src/main.rs`, immediately after the final `use` line (the block ends with `use std::time::Duration;`), add:

```rust
mod runtime;
```

- [ ] **Step 4: Run the tests to verify they pass**

Run (from `tools/profile_switch_ui`):
```
cargo test runtime:: -- --nocapture
```
Expected: PASS — `joins_snapshot_and_runtime_by_channel`, `channel_present_in_one_source_only_has_partial_row`, `missing_or_malformed_inputs_do_not_panic`, `parses_health_and_profile_comment` all ok. (First run downloads `serde`/`serde_json`.)

- [ ] **Step 5: Commit**

```bash
git add tools/profile_switch_ui/Cargo.toml tools/profile_switch_ui/Cargo.lock tools/profile_switch_ui/src/runtime.rs tools/profile_switch_ui/src/main.rs
git commit -m "feat(profile-ui): add serde runtime module with fan-row join"
```

---

### Task 2: Migrate existing JSON reads to serde; remove the hand-rolled extractor

**Files:**
- Modify: `tools/profile_switch_ui/src/main.rs` (`read_runtime_state`, `read_active_profile_from_runtime`, `read_profile_comment`; delete `extract_json_string_field`, `parse_json_string`, and their two tests)

**Interfaces:**
- Consumes: `runtime::parse_health`, `runtime::parse_runtime`, `runtime::parse_profile_comment` (Task 1).

- [ ] **Step 1: Replace `read_profile_comment`**

Replace the whole `read_profile_comment` function:

```rust
fn read_profile_comment(path: &Path) -> Option<String> {
    fs::read_to_string(path)
        .ok()
        .and_then(|text| extract_json_string_field(&text, "_comment"))
        .map(|comment| compact_whitespace(&comment))
}
```

with:

```rust
fn read_profile_comment(path: &Path) -> Option<String> {
    fs::read_to_string(path)
        .ok()
        .and_then(|text| runtime::parse_profile_comment(&text))
        .map(|comment| compact_whitespace(&comment))
}
```

- [ ] **Step 2: Replace the health summary block in `read_runtime_state`**

Replace this block (the `let health_summary = if health.ok { ... }` assignment, which uses `extract_json_string_field`):

```rust
    let health_summary = if health.ok {
        let state = extract_json_string_field(&health.stdout, "state")
            .or_else(|| extract_json_string_field(&health.stdout, "health"));
        let worker = extract_json_string_field(&health.stdout, "worker_pid");
        match (state, worker) {
            (Some(state), Some(worker)) => format!("{state}; worker PID {worker}"),
            (Some(state), None) => state,
            _ => "Health command succeeded".to_string(),
        }
    } else {
        format!(
            "Health command failed (exit {:?}): {}{}",
            health.code,
            health.stdout.trim(),
            health.stderr.trim()
        )
    };
```

with (note `health_info` is parsed once and reused in the next step):

```rust
    let health_info = runtime::parse_health(&health.stdout);
    let health_summary = if health.ok {
        let state = health_info
            .health_state
            .clone()
            .or_else(|| health_info.status.clone());
        match (state, health_info.process_id) {
            (Some(state), Some(pid)) => format!("{state}; worker PID {pid}"),
            (Some(state), None) => state,
            _ => "Health command succeeded".to_string(),
        }
    } else {
        format!(
            "Health command failed (exit {:?}): {}{}",
            health.code,
            health.stdout.trim(),
            health.stderr.trim()
        )
    };
```

- [ ] **Step 3: Replace the `read_active_profile_from_runtime` call to use the parsed runtime_home**

In `read_runtime_state`, the active-profile line currently is:

```rust
    let (active_profile, active_source) = read_active_profile_from_runtime(&health.stdout);
```

Replace `read_active_profile_from_runtime` entirely. Its current body:

```rust
fn read_active_profile_from_runtime(health_json: &str) -> (Option<String>, Option<String>) {
    let Some(runtime_home) = extract_json_string_field(health_json, "runtime_home") else {
        return (None, None);
    };
    let runtime_home = PathBuf::from(runtime_home);
    let candidates = [
        runtime_home.join("control_runtime.json"),
        runtime_home.join("control_supervisor.json"),
    ];

    for candidate in candidates {
        let Ok(text) = fs::read_to_string(candidate) else {
            continue;
        };
        let name = extract_json_string_field(&text, "active_profile_name")
            .or_else(|| extract_json_string_field(&text, "requested_profile_name"));
        let source = extract_json_string_field(&text, "active_profile_source");
        if name.is_some() || source.is_some() {
            return (name, source);
        }
    }

    (None, None)
}
```

Replace with (takes the already-parsed `runtime_home`):

```rust
fn read_active_profile_from_runtime(runtime_home: Option<&str>) -> (Option<String>, Option<String>) {
    let Some(runtime_home) = runtime_home else {
        return (None, None);
    };
    let runtime_home = PathBuf::from(runtime_home);
    let candidates = [
        runtime_home.join("control_runtime.json"),
        runtime_home.join("control_supervisor.json"),
    ];

    for candidate in candidates {
        let Ok(text) = fs::read_to_string(candidate) else {
            continue;
        };
        let rt = runtime::parse_runtime(&text);
        let name = rt.active_profile_name.or(rt.requested_profile_name);
        let source = rt.active_profile_source;
        if name.is_some() || source.is_some() {
            return (name, source);
        }
    }

    (None, None)
}
```

And update its call site in `read_runtime_state`:

```rust
    let (active_profile, active_source) =
        read_active_profile_from_runtime(health_info.runtime_home.as_deref());
```

- [ ] **Step 4: Delete the hand-rolled extractor and its tests**

Delete the entire `extract_json_string_field` function and the entire `parse_json_string` function from `main.rs`. Then delete these two tests from the `#[cfg(test)] mod tests` block: `extracts_simple_json_string_field` and `decodes_unicode_escape_in_json_string`.

- [ ] **Step 5: Run build + tests to verify**

Run (from `tools/profile_switch_ui`):
```
cargo build && cargo test
```
Expected: PASS — compiles with no reference to `extract_json_string_field`/`parse_json_string`; remaining `main.rs` tests (`decodes_form_profile_value`, `rejects_path_like_profile_names`, `rejects_reserved_and_dash_profile_names`, `same_origin_guard_blocks_cross_origin_switch`) and the `runtime::tests` pass. If the compiler reports an unused import or dead code, remove it.

- [ ] **Step 6: Commit**

```bash
git add tools/profile_switch_ui/src/main.rs
git commit -m "refactor(profile-ui): parse runtime JSON with serde; drop hand-rolled extractor"
```

---

### Task 3: Render the fan table + whole-page auto-refresh

**Files:**
- Modify: `tools/profile_switch_ui/src/main.rs` (`RuntimeState` struct, `read_runtime_state`, `render_page`, `page_css`)

**Interfaces:**
- Consumes: `runtime::build_fan_rows`, `runtime::FanRow` (Task 1); `health_info.runtime_home` (Task 2).

- [ ] **Step 1: Add `fans` to `RuntimeState`**

In the `RuntimeState` struct definition, add a field:

```rust
#[derive(Debug)]
struct RuntimeState {
    health_summary: String,
    active_profile: Option<String>,
    active_source: Option<String>,
    status_text: String,
    fans: Vec<runtime::FanRow>,
}
```

Update the two places that construct `RuntimeState`:
- In `real_main` (the error path inside the `for incoming` loop), add `fans: Vec::new(),` to the `RuntimeState { ... }` literal.
- In `read_runtime_state`, add `fans` (built in the next step) to the returned `RuntimeState { ... }`.

- [ ] **Step 2: Build the fan rows in `read_runtime_state`**

In `read_runtime_state`, after `health_info` is available and before constructing the returned `RuntimeState`, add:

```rust
    let fans = if let Some(home) = health_info.runtime_home.as_deref() {
        let home = PathBuf::from(home);
        let snapshot = fs::read_to_string(home.join("current_state.json")).ok();
        let control = fs::read_to_string(home.join("control_runtime.json")).ok();
        runtime::build_fan_rows(snapshot.as_deref(), control.as_deref())
    } else {
        Vec::new()
    };
```

Then include `fans,` in the returned `RuntimeState { ... }`.

- [ ] **Step 3: Render the fan table in `render_page`**

In `render_page`, before the `format!("<!doctype html> ...")` call, build the rows string:

```rust
    let fan_rows = if state.fans.is_empty() {
        "<tr><td colspan=\"5\" class=\"empty\">Fan data unavailable.</td></tr>".to_string()
    } else {
        let mut out = String::new();
        for fan in &state.fans {
            let duty = match fan.duty_percent {
                Some(v) => format!("{v:.1}%"),
                None => "&mdash;".to_string(),
            };
            let rpm = match (fan.tach_valid, fan.rpm) {
                (true, Some(v)) => v.to_string(),
                _ => "&mdash;".to_string(),
            };
            let temp = match (fan.observed_temp_c, fan.temp_source.as_deref()) {
                (Some(t), Some(src)) => format!("{t:.0} &deg;C ({})", html_escape(src)),
                (Some(t), None) => format!("{t:.0} &deg;C"),
                _ => "&mdash;".to_string(),
            };
            let controller = match fan.controller_kind.as_deref() {
                Some(k) => html_escape(k),
                None => "&mdash;".to_string(),
            };
            out.push_str(&format!(
                "<tr><td class=\"name\">ch{}</td><td>{}</td><td>{}</td><td>{}</td><td>{}</td></tr>",
                fan.channel, duty, rpm, temp, controller
            ));
        }
        out
    };
```

Then add a Fans `<section>` to the HTML. Insert it into the `format!` template immediately after the `</section>` that closes `state-grid` and before the `<p class="consequence">` line, using a new positional argument:

```html
         <section class=\"table-wrap fans\">\
         <table>\
         <thead><tr><th>Fan</th><th>Duty %</th><th>RPM</th><th>Temp</th><th>Controller</th></tr></thead>\
         <tbody>{fan_rows}</tbody>\
         </table>\
         </section>\
```

(`{fan_rows}` is a captured identifier, so no positional-arg renumbering is needed — it interpolates the `fan_rows` variable directly.)

- [ ] **Step 4: Add the whole-page meta-refresh**

In the same `format!` template, inside `<head>`, immediately after `<meta name=\"viewport\" ...>`, add:

```html
         <meta http-equiv=\"refresh\" content=\"3\">\
```

- [ ] **Step 5: Add minimal CSS for the fans section spacing**

In `page_css`, append this rule to the returned stylesheet string (before the closing `"#`):

```css
.fans { margin: 12px 0 16px; }
```

- [ ] **Step 6: Build, lint, run tests**

Run (from `tools/profile_switch_ui`):
```
cargo build && cargo clippy && cargo test
```
Expected: all clean; no warnings from clippy; all tests pass.

- [ ] **Step 7: Commit**

```bash
git add tools/profile_switch_ui/src/main.rs
git commit -m "feat(profile-ui): show per-channel fan duty/RPM/temp with 3s auto-refresh"
```

---

### Task 4: Live verification

**Files:** none (verification only).

- [ ] **Step 1: Launch the UI against the running controller**

Run (from repo root):
```
./scripts/Start-ProfileSwitchUi.ps1 -NoOpen
```
Then open `http://127.0.0.1:8766/` in a browser (or `curl http://127.0.0.1:8766/`).

- [ ] **Step 2: Confirm the fan table renders with live data**

Expected: a Fans table with rows `ch0`..`ch5`, each showing a duty `%`, an RPM number (or `—` if a tach is invalid), a temp like `42 °C (gpu)`, and a controller (`curve_overlay`). The page reloads itself about every 3 s and the duty/RPM values track the controller. Confirm the existing profile-switch table and Apply buttons still work, and the health line now reads like `healthy; worker PID <pid>`.

- [ ] **Step 3: Confirm graceful degradation**

Run the UI pointed at a path with no runtime (e.g. `cargo run --manifest-path tools/profile_switch_ui/Cargo.toml -- --repo D:\\does-not-exist --exe release\\svg-mb-control.exe`) and confirm the page still renders with `Fan data unavailable.` instead of crashing. Stop it with Ctrl-C.

- [ ] **Step 4: Stop the helper**

Close the `Start-ProfileSwitchUi.ps1` process (Ctrl-C in its window). No commit.

---

## Self-Review

**Spec coverage:**
- Per-channel duty % + RPM + temp + controller → Task 1 (`FanRow`/`build_fan_rows`) + Task 3 (render). ✓
- Data from `current_state.json` `fans[]` + `control_runtime.json` `controlled_channels[]`, joined by channel → Task 1. ✓
- RPM `—` when `!tach_valid` → Task 3 Step 3. ✓
- Whole-page ~3 s auto-refresh → Task 3 Step 4. ✓
- serde_json parsing; retire hand-rolled extractor → Task 1 (deps/module) + Task 2 (migration/deletion). ✓
- Graceful "unavailable" on missing/malformed → Task 1 (`unwrap_or_default`), Task 3 Step 3 (empty rows), Task 4 Step 3 (verified). ✓
- Tests: join, tach_valid=false, single-source, malformed → Task 1 Step 2. ✓
- No controller/schema/CLI/shipped-config change; reads only → Global Constraints; only `tools/profile_switch_ui` touched. ✓

**Placeholder scan:** No TBD/TODO; every code step shows complete code. ✓

**Type consistency:** `build_fan_rows(Option<&str>, Option<&str>) -> Vec<FanRow>` defined in Task 1 and consumed with the same signature in Task 3 Step 2. `FanRow` fields (`duty_percent`, `rpm`, `tach_valid`, `observed_temp_c`, `temp_source`, `controller_kind`, `channel`) used identically in Task 3 Step 3. `runtime::parse_health/parse_runtime/parse_profile_comment` defined in Task 1 and consumed in Task 2. `HealthInfo.runtime_home` (Task 1) consumed in Task 2 Step 3 and Task 3 Step 2. ✓
