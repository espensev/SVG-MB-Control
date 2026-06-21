# PID Shadow Characterization - 2026-06-21

Status: **current evidence record** for FEAT-0003 `REQ-PROFILE-07`.

This note records an offline PID shadow replay over a real curve-driven runtime
archive. It is a characterization artifact for the `pid.allow_live` gate, not a
live PID write authorization by itself. The replay did not start, stop, restart,
or write fans.

## Source

- Runtime CSV:
  `release/runtime/logs/archive/svg_mb_control_control-loop_20260621_101752.csv`
- Runtime manifest:
  `release/runtime/logs/archive/svg_mb_control_control-loop_20260621_101752.manifest.json`
- Session window: `2026-06-21T10:17:52` to `2026-06-21T14:17:53`
- Rows: `57,211`
- Packaged producer: `svg-mb-control` `0952e3d5352b`
- Config hash from manifest:
  `45a0a1c732a04f9aa3933d059b7a095b24d03b5034f0fbf541b717eb2f7557af`

The replay tool is `tools/pid_characterization/pid_shadow_replay.py`. It ports
the shipped `PidStep`, curve lookup, and setpoint rate limiter, then runs a
fidelity self-test against the C++ unit-test vectors before replaying the CSV.

Example command:

```powershell
python .\tools\pid_characterization\pid_shadow_replay.py `
  --csv .\release\runtime\logs\archive\svg_mb_control_control-loop_20260621_101752.csv `
  --config .\config\control.release.json `
  --gains .\tools\pid_characterization\gains_v2_gentle.json `
  --outdir .\tools\pid_characterization\run_101752_v2
```

The generated per-channel replay CSVs are local scratch artifacts. Keep compact
summaries in docs; do not commit the raw replay CSVs by default.

## Replay Results

`divergence` is `PID shadow setpoint - curve baseline setpoint`, in fan-duty
percentage points. `low-floor` is the share of replay rows pinned at the channel
minimum duty.

| Run | Scope | Gains / target | Result |
|---|---|---|---|
| `run_101752_v1` | channels 0,1,2,3,4,5 | `Kp=2.0`, `Ki=0.05`, `Kd=0.0`; targets 72 C for 0/1/5 and 75 C for 2/3/4 | Reject for live. RMS divergence ranged `8.46` to `17.49`; max absolute divergence ranged `29.83` to `38.32`; low-floor share was `86.0%` to `97.8%`. |
| `run_101752_v2` | channels 0,1,2,3,4,5 | `Kp=0.4`, `Ki=0.02`, `Kd=0.0`; same targets | Better but still not an all-channel live candidate. RMS divergence ranged `6.33` to `11.00`; max absolute divergence stayed near `25.09` to `26.25`; several channels remained biased below the curve baseline. |
| `run_101752_v3a` | channel 0 only | `Kp=0.3`, `Ki=0.01`, `Kd=0.0`, target 72 C | Channel-0-only exploratory candidate. Mean divergence `-2.59`, RMS `5.90`, max absolute `26.25`, low-floor `83.4%`. Still floor-biased. |
| `run_101752_v3b` | channel 0 only | `Kp=0.3`, `Ki=0.01`, `Kd=0.0`, target 68 C | Closest channel-0 replay in this pass. Mean divergence `-1.80`, RMS `4.27`, max absolute `26.25`, low-floor `81.1%`; PID range `15.5` to `39.2` versus curve range `15.5` to `46.54`. |

## Decision

This pass **does not support an all-channel live PID opt-in**. The all-channel
runs are materially below the curve baseline during meaningful portions of the
archive.

The only plausible next live experiment, if explicitly authorized later, is a
single-channel channel-0 pass using the `run_101752_v3b` shape or a newer
channel-0-only replay. That live pass must still meet all existing gate
conditions:

- `pid.allow_live: true` only on the selected channel,
- a positive non-NaN slew cap,
- `pid.characterization_artifact` pointing to an existing persisted artifact
  such as this note or a copied summary file,
- operator-present live-runtime authorization,
- a short before/after runtime capture proving the live PID did not move the
  shipped safety envelope.

Do not change the shipped `curve_overlay` profile or the default controller kind
from this evidence alone.
