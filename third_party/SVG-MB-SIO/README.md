# SVG-MB-SIO

Static C++ library for motherboard Super I/O access on the NCT6701D family.
It owns the reusable backend boundary for fan control, SIO voltage reads, SIO
temperature reads, restore operations, and raw register access.

This library is intentionally SIO-only:

- no AMD SMN transport
- no embedded verifier or logging workflow
- no runtime policy or process-lifetime ownership

Higher-level control policy and runtime orchestration belong in the integrating
application.

## Prerequisites

- Windows 10/11 x64
- Visual Studio 2022 with the C++ desktop workload
- `VCPKG_ROOT` must point to a vcpkg checkout
- administrator privileges for live hardware access
- PawnIO driver for LPC/SIO access
- `LpcIO.bin` available from packaged `resources\pawnio\LpcIO.bin` or from
  `SVG_MB_PAWNIO_LPC_BIN`

## Build

`SVG-MB-Control` normally builds this library in-tree through the top-level
`CMakeLists.txt`. To build the vendored copy directly from the repo root:

```powershell
cmake -S third_party/SVG-MB-SIO -B build/svg_mb_sio -G Ninja
cmake --build build/svg_mb_sio --config Release
```

The resulting build tree emits `svg_mb_sio.lib`.

## Usage

```cpp
#include <svg_mb_sio/svg_mb_sio.h>

MbSioController ctrl;
MbSioWritePolicy policy;
policy.writes_enabled = true;
policy.blocked_channels = {6};

std::string warning;
if (!ctrl.init(policy, warning)) {
    return;
}

auto dev = ctrl.discover();

std::vector<MbFanSnapshot> fans;
ctrl.read_fans(dev, fans);

std::vector<MbSioTemperatureSnapshot> temps;
ctrl.read_sio_temperatures(dev, temps);

ctrl.set_fan_duty(dev, 0, 50.0);
ctrl.restore_all_fans(dev);
ctrl.shutdown();
```

## Hardware Scope

- `NCT6701D`: 7 fan channels, 16 voltage ADCs, 23 SIO temperature sources
- Transport: PawnIO-backed `LpcIO.bin` for LPC / Super I/O access
- Concurrency: coordinated with the OS-global ISA bus mutex so external
  monitors can observe the board side-by-side
