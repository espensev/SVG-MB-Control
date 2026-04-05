# SVG-MB-Control Repo Role

Role: long-lived product-runtime repo for the SVG motherboard stack.

## Owns

- the product runtime executable
- product-owned runtime config and runtime home
- subprocess consumption of the frozen Bench bridge
- read-loop and control-loop process lifetimes
- bounded write orchestration through the Bench bridge
- optional in-process GPU telemetry through linked `gpu_telemetry`

## Does Not Own

- Bench proof probes or campaign wrappers
- Bench artifact-contract ownership
- the low-level reusable SIO backend
- reference-observer workflows
- the reserved LibreHardwareMonitor tree

## Boundary Rule

This repo may consume these Bench commands through subprocess launch only:

- `logger-service`
- `read-snapshot`
- `set-fixed-duty`
- `restore-auto`

Frozen contract source:

- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\BRIDGE_CONTRACT.md`

This repo must not:

- include Bench headers
- link Bench sources
- consume Bench internals as if they were a library boundary
- own the low-level SIO transport boundary from `SVG-MB-SIO`

GPU input is allowed through the `gpu_telemetry` package produced by
`D:\Development\Thermals\Nvida-fancontrol-unofficial\nvapi-controller`, but
Control remains the owner of the runtime process and control policy.
