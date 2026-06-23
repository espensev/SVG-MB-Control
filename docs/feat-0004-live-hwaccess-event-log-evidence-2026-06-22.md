# FEAT-0004 live hardware-access event-log evidence - 2026-06-22

## Summary

Pass. An isolated live `read-loop` runtime slice emitted the FEAT-0004
hardware-access transition event and published the matching runtime status
fields.

The raw runtime capture is retained locally under the ignored runtime directory:

`runtime\feat-0004-hwaccess-live-m-20260622-021427`

## Runtime slice

- Date/time: 2026-06-22 02:14:27 local time.
- Repo HEAD: `162fbeb6ce87683cab402a4f381780f8dae76c8c`.
- Executable: `build\x64-release\svg-mb-control.exe`.
- Version banner: `svg-mb-control 0.1.0 (162fbeb6ce87)`.
- Config: `runtime\feat-0004-hwaccess-live-m-20260622-021427\control.read-loop.json`.
- Runtime home: `runtime\feat-0004-hwaccess-live-m-20260622-021427\runtime-home`.
- Mode: `read-loop`.
- Stop path: `build\x64-release\svg-mb-control.exe --stop --config runtime\feat-0004-hwaccess-live-m-20260622-021427\control.read-loop.json`, exit code `0`.
- Process exit code: `0`.

The runtime was pre-seeded with `control_runtime.json` reporting
`hwaccess_state=unavailable`, `hwaccess_read_state=unavailable`, and
`hwaccess_write_state=unavailable` so the startup transition would exercise the
restored-event path. The read loop ran one successful poll, then exited through
the documented cooperative stop request.

Safety boundary: this was a foreground `read-loop` proof against an isolated
runtime home. It did not publish `release\`, restart installed scheduled tasks,
reset breakers, manage PawnIO, or write fan duty. The read-loop sample path uses
`AmdReader::Sample`, `GpuReader::Sample`, and `FanWriter::ReadAllChannels` before
publishing runtime JSON/CSV/events.

## Observed event

Event file:

`runtime\feat-0004-hwaccess-live-m-20260622-021427\runtime-home\logs\svg_mb_control_events.jsonl`

Observed event:

```json
{
  "event_time": "2026-06-22T02:14:28",
  "event_type": "read_loop.hwaccess_restored",
  "mode": "read-loop",
  "schema": "svg_mb_control.event.v1",
  "severity": "info",
  "success": true,
  "detail": "hardware access state=available read=available write=available read_detail=AMD/SMN reader initialized write_detail=svg_mb_sio"
}
```

## Final status

Final `control_runtime.json` summary:

```text
status=shutdown
status_detail=stop requested
successful_polls=1
skipped_polls=0
hwaccess_state=available
hwaccess_read_state=available
hwaccess_write_state=available
hwaccess_read_detail=AMD/SMN reader initialized
hwaccess_write_detail=svg_mb_sio
```

## Validation

- `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` passed on 2026-06-22 after
  rebuilding `build\x64-release\svg-mb-control.exe` from commit
  `162fbeb6ce87683cab402a4f381780f8dae76c8c`.
- The live read-loop evidence above closes FEAT-0004 `REQ-HWHEALTH-04` manual
  runtime/event-log evidence (`M`).
