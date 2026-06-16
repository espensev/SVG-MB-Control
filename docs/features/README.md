# svg-mb-control — Feature specs

**Project:** svg-mb-control
**Status:** Accepted   **Version:** 0.4   **Updated:** 2026-06-16
**Companion to:** `AGENTS.md`, `docs/TRACEABILITY.md`,
`docs/FEATURE_VERIFICATION_CHECKLIST.md`, `docs/STRUCTURE_AND_STABILITY.md`,
`docs/MEASUREMENT_GATE.md`
**Purpose:** define the *spec-before-build* system — each new feature gets its
own detailed spec **before** it is implemented, so the implementation can be
checked against the written spec afterward.

## 1. What this is

This folder holds one **feature spec** per planned feature, written from
[`_FEATURE_TEMPLATE.md`](_FEATURE_TEMPLATE.md). A feature spec is the durable
contract for a feature: what problem it solves (from observed runtime evidence or
a named code/contract gap), the exact behavior, its requirements (`REQ-*` IDs),
config/schema deltas, the design decision(s) it depends on, and — critically —
its **acceptance criteria and a verification log** so that after the feature is
built you can confirm it against what was promised.

This does not replace the existing repo contracts; it threads them together.
`AGENTS.md` stays the canonical agent contract (`CLAUDE.md`); a feature spec is
where a single new feature is scoped against those contracts before code lands.

| Surface | Role | Relationship to a feature spec |
|---|---|---|
| `AGENTS.md` | Canonical build / boundary / safety / change-checklist contract. | The rules every feature spec is scoped against. |
| `docs/STRUCTURE_AND_STABILITY.md` | Source layout and responsibility boundaries. | Says *where* a feature's code lives. |
| `docs/CONTROL_LOOP.md`, `READ_LOOP.md`, `WRITE_ORCHESTRATION.md`, `RUNTIME_HOME.md`, `CONTROL_PIPELINE_MATH.md` | Mode and runtime behavior contracts. | The behavior sections a spec references instead of restating. |
| `docs/MEASUREMENT_GATE.md` | What is blocked until characterized (cadence, channels, mixed-input strategy). | A spec must declare whether it crosses the gate and what evidence it needs. |
| `docs/<topic>-decision-YYYY-MM-DD.md` | One dated decision per direction-setting choice. | A feature earns its decision record before implementation; the spec links it. |
| `docs/TRACEABILITY.md` | Central `REQ-*` to verification map. | Every feature requirement gets one audit row here when introduced or changed. |
| `docs/FEATURE_VERIFICATION_CHECKLIST.md` | Practical implementation and handoff checklist. | The checklist used while building and verifying an accepted feature. |
| `.\scripts\Test-LocalCI.ps1` / `.\build-release.ps1` | CI-style validation and release gates. | Where a spec's `REQ-*` requirements are verified. |
| `tests/test_feature_specs.py` | Machine check for feature-spec consistency. | Fails the Python lane if registry, requirement, traceability, gate, or verification-log state drifts. |

> A `Reserved` or `Draft` spec is planning, not a backlog to start, and not
> permission to write code. Nothing in a feature spec is normative until its
> promotion gates (§3) pass and implementation is authorized.

## 2. Lifecycle of a feature spec

```text
Reserved  ->  Draft  ->  Accepted  ->  Implemented  ->  Done
```

1. **Reserved** — listed in the registry below with a `REQ-*` namespace, but the
   spec body is not yet written (or is a stub).
2. **Draft** — the spec is being written in detail (this is "document it before
   we add it"). The promotion gates (§3) are worked through here.
3. **Accepted** — the spec is agreed and may become buildable work when
   implementation is explicitly authorized; its required design decision
   record(s) exist and are current; `REQ-*` IDs are assigned; verification is
   mapped to real checks (`Test-LocalCI`, build-release, contract review, or
   runtime evidence) and to `docs/TRACEABILITY.md`.
4. **Implemented** — code lands; the relevant docs are updated per `AGENTS.md`
   §Change Checklist. The spec's **verification log** is filled in: each `REQ-*`
   is checked against the running controller ("the check-against-it-later step").
5. **Done** — all acceptance criteria pass; the spec is the historical record of
   what shipped.

A spec may sit at **Draft with all promotion gates (§3) checked** when it is
deliberate design-capture that is not being promoted — for example `FEAT-0003`,
recorded as not-a-net-benefit and not scheduled. That differs from **Accepted**:
Accepted means agreed and authorizable (still needing explicit build
authorization), whereas a held Draft is complete design that is intentionally
not advancing at all.

## 3. Promotion gates

Before a feature becomes buildable work, its spec must clear all gates in the
template's §13 checklist. In short:

1. Problem statement sourced from observed runtime evidence or a named
   code/contract gap — not speculation.
2. Identify which repo invariant it stresses: Repo Boundary and Live Runtime
   Safety (`AGENTS.md`), the Measurement Gate baseline (`MEASUREMENT_GATE.md`),
   control-computation identity (`CONTROL_PIPELINE_MATH.md`), and runtime-home
   schema stability (`RUNTIME_HOME.md`).
3. Write or update the design decision record(s) **before** implementation
   starts (dated `docs/<topic>-decision-YYYY-MM-DD.md`).
4. Assign concrete `REQ-*` IDs only after the decision chooses a direction.
5. Map every requirement to a real verification check in the feature spec and
   in `docs/TRACEABILITY.md` in the same change that introduces it.
6. Confirm it does not violate Live Runtime Safety or Repo Boundary and does not
   silently move the Measurement Gate baseline.
7. Doctrine check (`CLAUDE.md`): grounded claims, correct `must`/`should`/`is`
   usage, no undefined terms or unqualified vague adjectives.

## 4. Naming

- One file per feature: `FEAT-NNNN-<slug>.md` (e.g. `FEAT-0001-<slug>.md`).
- `NNNN` is a zero-padded, never-reused sequence number.
- H1 is the ID: `# FEAT-NNNN: <Feature name>`.
- Each feature owns exactly one `REQ-<AREA>-*` namespace, reserved in §5 below.
  This registry is the namespace authority for the repo.

## 5. Feature registry

Reserve a row (FEAT id + `REQ-*` namespace) when a feature is first proposed, so
it has a home before it is built. Write the spec body when implementation
approaches. None below is normative until its promotion gates pass. A `Reserved`
row carries no linked spec body in the enforced set; a body written then deferred
is parked under [`_parked/`](_parked/) and the row rejoins the enforced set
(`tests/test_feature_specs.py`) only when promoted back to `Draft`.

| FEAT | Feature | `REQ-*` namespace | Status |
|---|---|---|---|
| [FEAT-0001](FEAT-0001-hot-swap-write-policy.md) | Hot-swap runtime write policy | `REQ-WRITEPOLICY-*` | Accepted |
| [FEAT-0002](FEAT-0002-cpu-settings-evidence-logger.md) | CPU settings evidence logger | `REQ-CPUSETTINGS-*` | Implemented (source/test load layer; label deferred) |
| [FEAT-0003](FEAT-0003-selectable-profile-hot-swap.md) | Selectable control-law profile with hot-swap | `REQ-PROFILE-*` | Draft |
| [FEAT-0004](FEAT-0004-hardware-access-health-signal.md) | Hardware-access dependency health signal (PawnIO availability) | `REQ-HWHEALTH-*` | Draft |
| FEAT-0005 | Write actuation confirmation (non-actuating-write detection) | `REQ-ACTCONFIRM-*` | Reserved (body parked in `_parked/`) |
| [FEAT-0006](FEAT-0006-cpu-work-energy-efficiency-evidence.md) | CPU work & energy efficiency evidence (work-per-Joule) | `REQ-CPUEFF-*` | Accepted (energy logger + analyzer avg-power landed; cycle APERF/MPERF logger landed 2026-06-09 default-off; analyzer effective-frequency derivation landed 2026-06-10, analyze schema v10; energy quarantine-exit evidence complete across 3 independent sessions; marker promotion remains manual) |
| FEAT-0007 | RAM temperature telemetry (per-DIMM, via existing Super I/O read) | `REQ-RAMTEMP-*` | Reserved (body parked in `_parked/`) |
| [FEAT-0008](FEAT-0008-watchdog-hung-worker-recovery.md) | Watchdog hung-worker recovery (force-kill escalation) | `REQ-WATCHDOG-*` | Implemented (v1 complete: force-terminate escalation + `--restart` wiring landed; C++ unit + Python suspend-based integration tests pass; live deterministic suspend recovery evidence passed; natural load hard-freeze premise closed as not reproducible by load on this system; post-v1 options remain in FEAT-0008 §11) |
