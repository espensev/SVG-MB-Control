# Project: svg-mb-control

## Build

Preferred release entrypoint for agents:

```powershell
.\build-release.ps1
```

Use the repo's documented build and workflow entrypoints before inventing a new
bootstrap path. Do not search for `vcvars.bat`, hand-roll a Visual Studio
environment, or replace the repo workflow with ad hoc raw `cmake`, `ninja`, or
`msbuild` commands unless the user explicitly asks for low-level build
debugging.
It performs a clean `x64-release` configure/build, stages the packaged release,
runs `python -m unittest discover tests -v` unless `-SkipTests` is supplied,
then publishes `release\` and the archive bundle.

Manual CMake remains valid only for explicit incremental local work or when the
task is to debug the lower-level CMake path itself:

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
