# SVG-MB-Control

Role: long-lived product-runtime repo for the SVG motherboard stack.

## Owns

- the product runtime executable
- product-owned runtime config and runtime home
- subprocess consumption of the frozen Bench bridge
- later service-lifetime control behavior

## Does Not Own

- Bench proof probes or campaign wrappers
- Bench artifact-contract ownership
- the low-level reusable SIO backend
- reference-observer workflows
- the reserved LibreHardwareMonitor tree

## Current Phase 0 Rule

Phase 0 is read-only and subprocess-only.

This repo currently consumes:

- `read-snapshot`

from the frozen Bench bridge contract at:

- `D:\Development\Thermals\SVG-MB\SVG-MB-Bench\docs\BRIDGE_CONTRACT.md`

Phase 0 must not:

- include Bench headers
- link Bench sources
- consume Bench internals as if they were a library boundary
- own the low-level SIO transport boundary from `SVG-MB-SIO`
