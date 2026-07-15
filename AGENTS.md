# ⚠️ RETIRED — SVG-MB-Control is no longer in active duty

Active development and operation of motherboard telemetry and fan control have
moved to **SQ-control**, which now owns this responsibility. This repo's runtime
is expected to be **off** by design — that is intentional, not a fault. The
agent contract below applies only when intentionally working on this archived
repo.

---

## Build Workflow

- Use `.\build-release.ps1` or `scripts\Build-Release.ps1` as the default build entrypoint for this repo.
- Use `.\scripts\Test-LocalCI.ps1 -KeepBuildDir` for local CI-style validation
  when the task does not need to publish `release\` or create an archive.
- Follow repo-documented runtime, validation, and test workflows before inventing new helper scripts or ad hoc command sequences.
- Do not search for `vcvars.bat`, hand-roll Visual Studio environment setup, or replace the repo workflow with raw `cmake`, `ninja`, or `msbuild` commands unless the user explicitly asks for low-level build debugging.
- If the documented build or workflow path fails, diagnose or fix that path instead of silently switching to a different one.
- Manual CMake commands are only for explicit incremental local work or
  low-level CMake/build debugging.

## Repo Boundary

- Keep the repo standalone. Runtime behavior must not depend on sibling repos.
- Do not reintroduce external bridge code paths or subprocess adapters.

## Navigation For Agents

- Use `README.md` for the implemented product scope, release build, run,
  analyze, config, runtime-home, and test workflows.
- Use `docs\STRUCTURE_AND_STABILITY.md` for current source layout,
  responsibility boundaries, and remaining structural polish.
- Use `docs\BUILD_TARGETS_AND_DEPENDENCIES.md` for the executables, runtime
  scheduled-task processes, and vendored dependencies.
- Use `docs\PATH_NOTES.md` for the dated log of completed/fixed/added work and
  the unscheduled ideas backlog. It is a curated journal, not a system of
  record; `git log`, `docs\features\`, and `docs\STRUCTURE_AND_STABILITY.md`
  stay authoritative.
- Use `docs\features\README.md`, the matching `docs\features\FEAT-*.md`, and
  `docs\TRACEABILITY.md` for new feature intake, requirement IDs, promotion
  status, and requirement-to-verification mapping. Use
  `docs\FEATURE_VERIFICATION_CHECKLIST.md` while implementing or verifying a
  feature. Start from the `## Current priority` index at the top of
  `docs\features\README.md` for the active/next work and the decisions-owed queue.
- Use `docs\CONTROL_LOOP.md`, `docs\READ_LOOP.md`, and
  `docs\WRITE_ORCHESTRATION.md` for mode-specific runtime behavior.
- Use `docs\RUNTIME_HOME.md` for runtime sidecars, status fields, health
  behavior, logs, manifests, and archive retention.
- Use `docs\RUNTIME_LOGGING_AND_EVALUATION.md` for controller tuning,
  runtime-evidence collection, analyzer workflow, and current logging gaps.
- Use `docs\CONTROL_PIPELINE_MATH.md` as the maintained numerical reference
  for control-computation identity.
- Use `docs\COOLING_STRATEGY.md` for the strategy, fan inventory, floor
  philosophy, and fan-relationship rules behind the shipped curves. The
  machine-readable companion is
  `config\machines\snd-desk.cooling.policy.json`.
- Treat `docs\discovery-*.md`, `docs\archive\*.md`, and other older
  recommendation files as historical context unless they explicitly say they
  are current and agree with README, current docs, source, and tests.
  `docs\COOLING_STRATEGY.md`,
  `docs\response-evaluation-tuning-plan.md`, and
  `docs\NORMAL_RUNTIME_AIRFLOW_PROFILE.md` are maintained as current.

## Feature Intake Gate

- Before product-code work starts for a new capability, control behavior,
  runtime schema/status/log/manifest field, CLI/operator surface, live-runtime
  workflow, or shipped config behavior, identify the owning `docs\features\FEAT-*`
  spec and confirm it is implementation-authorized.
- If no accepted, implementation-authorized spec exists, land or update the spec
  first. The spec must define the `REQ-*` IDs, required decision record(s),
  acceptance criteria, and entries in `docs\TRACEABILITY.md`.
- Before handoff for feature work, use
  `docs\FEATURE_VERIFICATION_CHECKLIST.md` and keep the owning spec's
  verification log aligned with `docs\TRACEABILITY.md`.
- `Reserved` and `Draft` specs are planning records, not implementation
  permission. `Accepted` specs are buildable only when the spec text or the
  maintainer explicitly authorizes implementation.
- Defect fixes, behavior-preserving refactors, and docs-only corrections do not
  require a feature spec, but behavior changes found during those tasks must
  either map to an existing spec or promote a new one before the behavior is
  expanded.
- Ideas in `docs\PATH_NOTES.md` or discovery/recommendation docs are not
  authorized work until promoted through `docs\features\`.

## Change Checklist

- For docs-only changes, read back the edited docs and check `git diff`.
- For C++ behavior changes, run `.\scripts\Test-LocalCI.ps1 -KeepBuildDir`.
- For release/package changes, run `.\build-release.ps1` or
  `scripts\Build-Release.ps1` with the narrowest safe options for the task.
- For control-computation changes, update `docs\CONTROL_PIPELINE_MATH.md`.
- For runtime sidecar, health, log, manifest, or archive schema changes, update
  `docs\RUNTIME_HOME.md` and the relevant runtime docs.
- For tuning or runtime-evidence workflow changes, update
  `docs\RUNTIME_LOGGING_AND_EVALUATION.md`.
- For CLI or operator workflow changes, update `README.md` and the relevant
  mode-specific doc.
- For feature-spec or `REQ-*` changes, update `docs\features\README.md`, the
  owning feature spec, and `docs\TRACEABILITY.md` in the same change.

## Live Runtime Safety

- Do not start, stop, restart, install scheduled tasks, reset breakers, or write
  fan duty unless the task explicitly requires live runtime interaction.
- Prefer no-publish validation for code review and implementation work that
  does not need to update `release\`.
- Do not commit raw runtime CSV captures by default; commit compact summaries or
  decision records when runtime evidence needs to be preserved.

## Documentation Maintenance

- Keep `docs\CONTROL_PIPELINE_MATH.md` updated with control-computation changes
  and with real-data validation notes when runtime CSV/status evidence changes
  the maintained control identities.
- In README, contract, reference, and policy docs, make claims verifiable from
  code, CLI behavior, config, tests, or runtime evidence. Avoid forward-looking
  statements unless the behavior already exists in this repo.
