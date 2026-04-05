# Project: svg-mb-control

## Build

The repo now provides a repo-local release entrypoint:

```powershell
.\build-release.ps1
```

It performs a clean `x64-release` configure/build, stages
`svg-mb-control.exe` plus the packaged `control.json`, runs
`python -m unittest discover tests -v` unless `-SkipTests` is supplied, then
publishes `release\` and a timestamped archive.

Manual CMake remains valid for incremental local work:

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
```

## Documentation standard

Every statement in README, contract, reference, and policy docs must meet these
rules:

- **No vague adjectives.** Do not write "fast", "detailed", "high-rate", "meaningful", or "deterministic" without a number, a field name, or a testable definition. If you cannot point to a specific code path, CLI flag, or output field that backs the claim, remove the claim.
- **No undefined terms.** Every technical term used in a doc must either be obvious from context, defined inline, or reference a specific code artifact.
- **No unverifiable claims.** If a claim cannot be verified by reading the code or running the tool, it does not belong.
- **No casual tone.** Docs are professional and detached. State what the tool does.
- **"Should" vs "must" vs "is".** If the code enforces a rule, say "must" or "is." If it is advisory, say "should" and mark it as advisory.
- **Challenge every sentence.** When writing or reviewing docs, ask: "Can I point to the exact line of code, flag name, or test that makes this true?" If not, rewrite or remove.
- **No forward-looking claims.** Do not document behavior that is not implemented.

## Code conventions

- C++20, MSVC on Windows x64.
- Keep the Bench boundary external. Do not include or link Bench internals.
- The read loop, write orchestration, and control loop all remain
  subprocess-only.
- Bounded writes go through the existing `set-fixed-duty` and
  `restore-auto` bridge commands.
- The subprocess adapter owns process launch, stdout/stderr capture, timeout, and exit-code handling.
