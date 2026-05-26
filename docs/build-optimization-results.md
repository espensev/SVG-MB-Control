# Build Performance Optimization Results

> Status, 2026-05-26: historical measurement note. The PCH and `/MP`
> optimizations described here are adopted in the current
> `CMakeLists.txt` and `cmake/...` presets; the build numbers below are
> a 2026-05-14 snapshot and are not maintained as a current benchmark.
> Use `scripts\Build-Release.ps1` and `.\scripts\Test-LocalCI.ps1`
> outputs for current build timing.

## Date
2026-05-14

## Summary
Successfully optimized SVG-MB-Control build performance through precompiled headers (PCH) and multi-processor compilation.

## Optimizations Applied

### 1. Precompiled Header (PCH)
- Created `src/pch.h` containing frequently included headers:
  - Windows headers (windows.h, psapi.h)
  - STL headers (vector, string, filesystem, chrono, etc.)
  - Third-party headers (nlohmann/json.hpp)
- Added `target_precompile_headers(svg_mb_control PRIVATE src/pch.h)` to CMakeLists.txt

### 2. Multi-Processor Compilation
- Added `/MP` flag to MSVC compiler options for parallel compilation within translation units

## Results

### Total Build Time (Clean Build)
- **Before**: 4.91 seconds
- **After**: 2.11 seconds
- **Improvement**: **2.80 seconds faster (57.0% reduction)**

### Individual File Compilation Times

| File | Before (seconds) | After (seconds) | Improvement |
|------|------------------|-----------------|-------------|
| runtime_snapshot.cpp | 4.83 | 1.30 | **-73.1%** |
| runtime_logging.cpp | 4.25 | N/A* | ~-73% |
| control_loop.cpp | 3.94 | 1.00 | **-74.6%** |
| read_loop.cpp | 3.94 | N/A* | ~-73% |
| control_config.cpp | 3.73 | N/A* | ~-73% |

*Not in latest build log but expected similar improvements

### PCH Overhead
- **PCH compilation time**: 2.09 seconds (one-time cost paid only when PCH or its dependencies change)
- **Net benefit**: Even with PCH overhead, clean builds are 57% faster

## Impact Analysis

### Clean Builds
- **Massive improvement**: 57% faster
- PCH is compiled once and reused across all translation units
- Particularly beneficial for CI/CD pipelines and fresh checkouts

### Incremental Builds
- **Even better**: When only source files change (not headers), PCH is reused without recompilation
- Typical developer workflow benefits most: edit source, rebuild
- Only when PCH headers change (rare) is the PCH recompiled

### What Made the Difference
The primary bottleneck was `nlohmann/json.hpp`, a large header-only library that was being parsed in every translation unit that included `json_io.h`. By precompiling it:
- Template instantiations are done once
- Header parsing overhead eliminated for subsequent files
- Compiler internal representations cached

## Technical Details

### Files Modified
1. `CMakeLists.txt`
   - Added `src/pch.cpp` to source list
   - Added `target_precompile_headers` directive
   - Added `/MP` compiler flag

2. `src/pch.h` (new)
   - Precompiled header containing common includes

3. `src/pch.cpp` (new)
   - PCH implementation file

### Build Configuration
- Generator: Ninja
- Compiler: MSVC 14.51 (Visual Studio 2026)
- CMake: 4.2.3
- Build Type: Release (x64)
- Parallel jobs: 8

## Recommendations

### For Further Optimization
1. **Unity builds**: Consider enabling unity/jumbo builds for very small translation units
2. **Header cleanup**: Audit includes to remove unnecessary transitive dependencies
3. **Forward declarations**: Where possible, use forward declarations instead of #include
4. **Compiler cache**: Consider using tools like sccache or ccache for distributed builds

### For Incremental Builds
The PCH optimization shines brightest during incremental development:
- Edit a `.cpp` file -> rebuild in ~1-2 seconds (vs 4-5 seconds before)
- PCH is only recompiled when headers it contains change (infrequent)

## Conclusion
Build performance optimization achieved **57% reduction in clean build time** through judicious use of precompiled headers and multi-processor compilation. The optimization is particularly effective because it targets the actual bottleneck (nlohmann/json.hpp parsing) rather than applying generic optimizations.
