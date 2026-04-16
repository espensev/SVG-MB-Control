# Project: svg-mb-control

## Build

Preferred release entrypoint:

```powershell
.\build-release.ps1
```

It performs a clean `x64-release` configure/build, stages the packaged release,
runs `python -m unittest discover tests -v` unless `-SkipTests` is supplied,
then publishes `release\` and the archive bundle.

Manual CMake remains valid for incremental local work:

```powershell
cmake --preset x64-release
cmake --build --preset x64-release
```

## Documentation Standard

Every statement in README, contract, reference, and policy docs must meet these
rules:

- Do not use vague adjectives without a field name, number, or testable
  definition.
- Do not use undefined technical terms.
- Do not make claims that cannot be verified from the code, CLI, config, or
  tests.
- Use professional tone. State what the tool does.
- Use `must` only for enforced rules, `should` only for advisory rules, and
  `is` only for current implemented behavior.
- Remove forward-looking claims unless the behavior already exists in this repo.

## Code Conventions

- C++20, MSVC, Windows x64.
- Keep the repo standalone. Runtime behavior must not depend on sibling repos.
- Direct fan reads, writes, restore, `write-once`, and `control-loop` live in
  Control through `SVG-MB-SIO`.
- `one-shot` and `read-loop` are direct in-process paths.
- Do not reintroduce external bridge code paths or subprocess adapters.
- Do not move steady-state control-loop sampling behind another executable.
- Hermetic tests use simulation environment hooks, not extra helper binaries.
