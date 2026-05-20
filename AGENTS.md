## Build Workflow

- Use `.\build-release.ps1` or `scripts\Build-Release.ps1` as the default build entrypoint for this repo.
- Follow repo-documented runtime, validation, and test workflows before inventing new helper scripts or ad hoc command sequences.
- Do not search for `vcvars.bat`, hand-roll Visual Studio environment setup, or replace the repo workflow with raw `cmake`, `ninja`, or `msbuild` commands unless the user explicitly asks for low-level build debugging.
- If the documented build or workflow path fails, diagnose or fix that path instead of silently switching to a different one.

## Repo Boundary

- Keep the repo standalone. Runtime behavior must not depend on sibling repos.
- Do not reintroduce external bridge code paths or subprocess adapters.

## Documentation Maintenance

- Keep `docs\CONTROL_PIPELINE_MATH.md` updated with control-computation changes
  and with real-data validation notes when runtime CSV/status evidence changes
  the maintained control identities.
