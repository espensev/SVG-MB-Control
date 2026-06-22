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
