# Project: svg-mb-control

## Build

Phase 0 uses the manual CMake path:

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
```

There is no `Build-Release.ps1` in Phase 0.

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
- Phase 1 extends Phase 0 with a persistent read-loop supervisor. It remains
  subprocess-only and read-only.
- Phase 2 adds bounded write orchestration through the existing
  `set-fixed-duty` and `restore-auto` bridge commands. The write path is
  separate from the read loop.
- The subprocess adapter owns process launch, stdout/stderr capture, timeout, and exit-code handling.
