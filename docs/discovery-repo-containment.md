# Discovery — Repo Containment

**Goal:** Verify that `SVG-MB-Control` stands on its own, identify any submodule, script, generated-link, absolute-path, or sibling-repo dependency, and tighten the repo boundary where needed.
**Date:** 2026-04-16
**Status:** complete
**Recommended next:** none — containment fixes were applied directly; remaining work is to commit the vendored payload so the branch truly owns it in git history

---

## Questions

1. Are there any git submodules, symlinks, junctions, or generated links in the repo?
2. Does the top-level build or packaging logic resolve source trees from outside this repo?
3. Do runtime code and config files resolve assets from inside the repo, or do they depend on sibling-repo paths?
4. Do the vendored `third_party` trees remain self-contained inside this checkout?
5. What containment gaps still remain after the audit?

---

## Findings

### Q1: Are there any git submodules, symlinks, junctions, or generated links in the repo?

**Answer:** No. The repo has no `.gitmodules`, `git submodule status` returned no entries, and a recursive reparse-point scan returned no filesystem links.

**Evidence:**
- Command: `Test-Path .gitmodules` returned false.
- Command: `git submodule status` returned no output.
- Command: `Get-ChildItem -Recurse -Force | Where-Object { $_.Attributes -band [IO.FileAttributes]::ReparsePoint }` returned no entries.

**Implications:**
- The containment problem is not caused by submodules or filesystem indirection.
- Remaining work is limited to plain file content and version-control state.

### Q2: Does the top-level build or packaging logic resolve source trees from outside this repo?

**Answer:** After this change, no. The root build now pins both vendored libraries to `third_party/` paths under this repo instead of exposing cache-path overrides that could point at sibling checkouts.

**Evidence:**
- `CMakeLists.txt:8-20` pins `SVG_MB_SIO_SOURCE_DIR` to `third_party/SVG-MB-SIO` and adds it with `add_subdirectory(...)`.
- `CMakeLists.txt:38-55` pins `SVG_MB_CONTROL_GPU_TELEMETRY_SOURCE_DIR` to `third_party/nvapi-controller/telemetry`.
- `scripts/Build-Release.ps1:77-84` derives all build, dist, and release paths from `$RepoRoot`.
- `scripts/Build-Release.ps1:158-201` copies `AMDFamily17.bin` from repo-local `resources\pawnio` when present.

**Implications:**
- The default build path no longer has an escape hatch to sibling source trees.
- Packaging stays rooted under this checkout.

### Q3: Do runtime code and config files resolve assets from inside the repo, or do they depend on sibling-repo paths?

**Answer:** Runtime resolution is repo-contained. The config uses repo-relative runtime paths, and the AMD reader searches the executable tree, cwd, and packaged `resources\pawnio` locations inside this repo.

**Evidence:**
- `config/control.example.json:4` writes snapshots to `..\runtime\current_state.json`.
- `config/control.example.json:6` sets `runtime_home_path` to `..\runtime`.
- `config/control.release.json:4` writes snapshots to `runtime\current_state.json`.
- `src/amd_reader.cpp:183-210` resolves `AMDFamily17.bin` from environment overrides or from repo-local `resources`, `release/resources`, and `dist/resources`.
- `third_party/SVG-MB-SIO/src/fan_sio.cpp:164-166` resolves `LpcIO.bin` from repo-local `resources`, `release/resources`, and `dist/resources`.

**Implications:**
- The remaining relative paths are internal to this repo and package layout, not sibling-repo dependencies.
- Runtime asset lookup is compatible with both in-repo development and packaged release layouts.

### Q4: Do the vendored `third_party` trees remain self-contained inside this checkout?

**Answer:** Yes. Their internal `..` references stay inside the vendored subtree, and the stale README instructions that pointed back to other repos or machine-local paths were corrected in this turn.

**Evidence:**
- `third_party/nvapi-controller/telemetry/CMakeLists.txt:16-22` references `../src/...` files within the vendored `nvapi-controller` copy.
- `third_party/nvapi-controller/telemetry/CMakeLists.txt:63-69` includes `${CMAKE_CURRENT_SOURCE_DIR}/../src`, which stays inside `third_party/nvapi-controller`.
- `third_party/nvapi-controller/telemetry/include/gpu_telemetry/nvapi_loader.h:4` includes `../../../src/nvapi_loader.h` inside the same vendored tree.
- `third_party/SVG-MB-SIO/README.md:25-34` now documents direct builds against the vendored path inside this repo.
- `third_party/nvapi-controller/telemetry/README.md:7-10` and `third_party/nvapi-controller/telemetry/README.md:127-160` now describe vendored, repo-local build/install flows instead of an external monorepo and machine-specific install path.

**Implications:**
- Vendoring is structurally self-contained.
- Internal parent-directory references in vendored code are acceptable because they do not escape the vendored root.

### Q5: What containment gaps still remain after the audit?

**Answer:** The main remaining gap is version-control ownership: `resources/` and `third_party/` exist in the worktree but are currently untracked, so the branch is not yet self-contained in git history until those directories are committed.

**Evidence:**
- `git status --short` shows `?? resources/` and `?? third_party/`.
- Command: `git ls-files resources third_party` returned no tracked files under either path.
- `README.md:5-6` and `docs/REPO_ROLE.md:14-18` already describe these directories as repo-owned vendored content.

**Implications:**
- The code and docs now assume repo-local ownership, but the branch still needs those files committed to make that true for anyone else who checks it out.
- This is a repository-state issue, not a source-code wiring issue.

---

## Cross-Cutting Analysis

### Constraints
- The direct runtime depends on packaged PawnIO binaries under `resources\pawnio`; removing them would break AMD and SIO hardware access.
- Vendored `nvapi-controller` telemetry intentionally uses internal parent-directory references within its own subtree; flattening that would be a separate refactor, not a containment fix.
- The top-level release flow depends on Windows-local toolchain components such as Visual Studio and `VCPKG_ROOT`, but not on sibling repositories.

### Risks
| Risk | Likelihood | Impact | Notes |
|------|-----------|--------|-------|
| Vendored payload stays untracked in git | H | H | Other clones would lose the code/resources that the repo now expects locally. |
| Future edits reintroduce external CMake cache-path overrides | M | M | The top-level build was tightened here, but nothing yet enforces the rule automatically. |
| Vendored docs drift away from the actual in-repo build flow again | M | L | The READMEs were corrected, but they are still copied code and can drift independently. |

### Open Questions
All questions answered.

---

## Recommendation

Containment is now correct at the source and build-graph level. The next practical step is to commit `resources/` and `third_party/` so the branch truly owns the vendored payload it now references.
