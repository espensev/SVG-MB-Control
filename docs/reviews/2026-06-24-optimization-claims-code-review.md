# Optimization Claims vs Code Review - 2026-06-24

Static review of optimization claims against the retained standalone source.
No live runtime interaction was performed. SQ-control now owns operation;
this review does not authorize starting the legacy runtime.

## Current findings

Source verification resolves the original correctness/hardening items:

- CPUID vendor/brand buffers use `int cpu_info[4]` and `std::memcpy`, preserving
  vendor EBX/EDX/ECX order and 16-byte brand blocks.
- AMD/GPU/simulated-fan/environment helpers use strict numeric parsing from
  `src/platform/numeric_parse.h`; analyzer CLI and calibration check consumed
  character counts. The old trailing-junk findings no longer describe those paths.
- PawnIO helpers and fan read/write result types now carry `[[nodiscard]]`;
  `tests/cpp/numeric_parse_tests.cpp` covers strict numeric parsing.

The [remediation plan](../superpowers/plans/2026-06-24-optimization-remediation.md)
retains the implementation packages and acceptance criteria.

## Performance boundaries

- AMD `TryParseDoubleEnv` is a simulated per-tick path only when
  `SVG_MB_CONTROL_SIM_DIRECT_AMD_MODE=enabled` without a fixed sequence.
  `ParseDoubleSequence` runs during construction. Their cleanup is not evidence
  of deployed PawnIO sampling gains.
- Real AMD sampling reuses `sample_buffer_`, retains vector capacity, and takes
  one `Global\Access_PCI` lock around the Tctl/CCD sequence. Stable CPU/transport
  strings and sensor labels still copy. Keep owned snapshots unless profiling
  justifies a different contract.
- Direct runtime snapshots and SIO bulk reads reserve/reuse vectors. Additional
  `reserve`/`emplace_back` work is a small optimization, not a demonstrated
  control-latency fix.
- Offline analyzer CSV ingest is the stronger large-input candidate:
  `analyze_csv.cpp` already has a fully consumed `from_chars` double fast path
  and checked `std::stod` fallback, but field materialization and row/header
  lookup warrant measurement against representative archived CSVs before claims.
- `available()` accessors can be considered for `noexcept`; allocating
  `init_warning()` returns a string and must not be marked unconditionally
  non-throwing.

## Unresolved work

1. Add isolated driver/fallback seams for PawnIO IOCTL outcomes, SIO
   status/retry translation, CPUID decoding, and tick parsing.
   `pawnio_binary_tests.cpp` currently covers hashing/spec/path resolution and
   status strings, not simulated `LoadPawnIoBinary` device results.
2. Record an analyzer ingest baseline and matching after-change measurements
   before optimizing field indexes/allocations. Choose simulation parsing,
   offline ingest, or live snapshot copying deliberately; they affect different
   execution paths.
3. If needed, add opt-in static-analysis/sanitizer/LTO lanes separately from
   the default release/local-CI gate. CMake/MSVC Release already implies
   optimization such as `/O2`; absence of explicit `/GL`/`/LTCG` is not an
   unoptimized-build defect. Tool support and build/link cost require validation.

## Safety and validation

PawnIO not-found/access-denied and SIO unavailable/timeout status translation
already exist. The application assumes PawnIO is installed, records warnings,
continues without AMD telemetry when unavailable, and does not install,
start, or restart the driver. Remediation is external; unit tests must keep
real driver/hardware interaction out.

For separately scoped source work, use
`.\scripts\Test-LocalCI.ps1 -KeepBuildDir`, focused parser/status tests, and
representative offline measurements. Preserve accepted/rejected numeric-token
contracts and analyzer outputs; do not replace parsers solely for a claimed
hot-path benefit.
