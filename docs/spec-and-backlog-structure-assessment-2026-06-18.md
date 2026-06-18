# Spec, Plan, and Backlog Structure — Assessment (2026-06-18)

**Status:** Snapshot (not a maintained contract). **Reviewed:** 2026-06-18
against `origin/main` at `0ee14ea`. **Authorizes nothing.** `git log`,
`docs/features/` specs, `docs/TRACEABILITY.md`, and `AGENTS.md` stay
authoritative. This is a point-in-time review of how specs/plans/roadmaps/issues
are structured and a labeled recommendation for the backlog entry-point; per
`AGENTS.md` (Feature Intake Gate) it does not authorize product-code work.

**Update later on 2026-06-18:** the disk-growth retention lane called out below
was promoted to in-repo Accepted specs: `FEAT-0015` (event JSONL retention) and
`FEAT-0016` (analyze DB retention). This snapshot's original observation is kept
as historical context; current status lives in `docs/features/README.md`,
`docs/TRACEABILITY.md`, and `docs/next_steps.md`.

## 1. Scope and method

Static read (no code changed, no runtime interaction) of the governance and
planning surfaces: `AGENTS.md`, `docs/features/README.md`,
`docs/features/_FEATURE_TEMPLATE.md`, the `FEAT-0001..0014` specs,
`docs/TRACEABILITY.md`, `docs/PATH_NOTES.md`, `docs/next_steps.md`,
`tests/test_feature_specs.py`, the `docs/` file census, and GitHub issues/PRs via
`gh`. The decision-queue lines in §4 are paraphrased from each spec's
open-decision section where one exists (FEAT-0014 §11; FEAT-0009 §12 measurement
gate; FEAT-0004 §13 gate 3). At this snapshot point, FEAT-0015/0016 had no
in-repo spec yet; they were promoted later on 2026-06-18.

`docs/` census at `0ee14ea` (includes this assessment): 78 `.md` at the `docs/`
root (plus 8 under `docs/archive/`), of which 12 `discovery-*`, 8 `*plan*`,
14 `*-decision-*`, 9 `*-evidence/validation/results*`, and 23 `cpu-*` topic
files.

A first read was taken on a feature branch behind `main` and showed
FEAT-0011/0012/0013 as held-Draft; they are in fact `Implemented` on `main`
(PRs #11/#12/#13, merged 2026-06-17), and the write-path review was then closed
by PR #14 (`0ee14ea`). The numbers below are taken from `main` after fast-forward
to `0ee14ea`. That a second sync was needed mid-review is itself evidence for §5:
the current queue is reconstructable only by reading `main`'s registry plus
`git log`, not from any one standing document.

## 2. What is working (recommend: no change)

- **Feature-spec governance** (`docs/features/README.md`): one `FEAT-NNNN` spec
  per capability; the `Reserved → Draft → Accepted → Implemented → Done`
  lifecycle (§2); one `REQ-<AREA>-*` namespace per feature (§4–§5); promotion
  gates (§3) requiring a dated decision record before `REQ-*` IDs are assigned.
- **Machine enforcement** (`tests/test_feature_specs.py`): fails the Python lane
  when registry rows, `REQ-*` coverage (spec §6/§10/§14), traceability rows,
  accepted/implemented gate state, or implemented verification-log results
  drift. `docs/TRACEABILITY.md` is the central `REQ-*`→verification map.
- **Canonical contract** (`AGENTS.md`): the Feature Intake Gate forces
  spec-before-build; the Change Checklist keeps the matching docs updated in the
  same change as code.
- **Separated record types**: specs = contract; `PATH_NOTES.md` = journal
  (states it is "not a system of record"); `*-decision-*.md` = dated choices;
  `discovery-*.md` = historical context unless marked current in `AGENTS.md`
  §Navigation.

Assessment: the machinery is sound and CI-enforced. A prior multi-agent review
(recorded in `PATH_NOTES.md` 2026-06-06) already found the only excess was in
speculative specs (FEAT-0005/0006 demoted to `Reserved`), not the machinery — so
the recommendation in §5 adds **no** new system.

## 3. The gap — no single aggregated priority entry-point

"What to do next" is distributed across the surfaces below. `docs/next_steps.md`
is a maintained backlog and is the closest thing to an entry-point, but it is
prose/topic-organized, omits FEAT-0009 and the disk-growth retention, and is
titled as a review artifact rather than the feature-pipeline entry-point. No
single surface gives a crisp priority + decisions-owed view.

| Surface | Holds | Limitation |
|---|---|---|
| `docs/next_steps.md` | Maintained topical backlog | At this snapshot, current through the 2026-06-17 write-path closeout, but prose/topic-organized and silent on FEAT-0009 and the disk-growth retention (issue #4 / PR #9), so it was not a complete priority view. FEAT-0015/0016 were added later on 2026-06-18. |
| `docs/features/README.md` registry | Feature pipeline + Status | Authoritative per-feature status, but had no priority/sequence or aggregated decision view (the §5 fix adds one). |
| `docs/PATH_NOTES.md` "Ideas / backlog" | Unscheduled ideas | One of several idea homes. |
| `docs/discovery-loop-targets-value-ranked-2026-06-14.md` | 40 ranked discovery candidates | Dated snapshot; discovery-scoped, not feature-scoped. |
| held-Draft specs (FEAT-0009, 0014) | Each spec's open decisions | The decisions owed were not visible in aggregate (see §4). |
| `docs/STRUCTURE_AND_STABILITY.md` §Migration Order ("Remaining polish"); `CONTROL_SIMPLIFICATION_TARGETS.md`; `LOGGING_IMPROVEMENT_PLAN.md`; `SCRIPT_STACK_REVIEW.md` | Topic backlogs | By-design per-topic. |
| 8× `docs/*-plan-*.md` | Multi-gate forward roadmaps | By-design per-topic. |
| GitHub issue #4 + draft PR #9 | Disk-growth retention | At this snapshot, issue/PR were the only surfaces. Later on 2026-06-18 this moved into `docs/features/FEAT-0015-*`, `FEAT-0016-*`, and traceability. |

This is **not** an over-supply of documents — the per-topic plans and topic
backlogs are a deliberate design. The gap is the absence of a single top-level
*entry-point* that names what is active, the order, and what is blocked on a
maintainer decision.

## 4. Decision queue (the specs that should follow)

After the 2026-06-17 write-path wave, the held-on-a-decision set is small. The
calls owed, paraphrased from each spec's open-decision section where one exists:

| Spec | Decision or gate owed |
|---|---|
| **FEAT-0014** reconcile/restore blocked-channel guard (§11) | Where the guard lives (Control-layer pre-check only vs also mirror `channel_blocked` into the vendored restore functions); whether a skipped blocked-channel entry is cleared or retained; whether `--write-once`'s exit-5 refusal is sufficient. Real gap, not reachable by the shipped single-profile config. |
| **FEAT-0009** controller priority elevation (§12) | Run the A/B contention experiment to justify promotion (the default is already `inherit`). §11 also holds two open choices (`above_normal` vs `high_timecritical`; hot-reloadable vs startup-only). |
| **FEAT-0004** hardware-access health signal (§13 gate 3) | Write and mark current the §9 design-decision record before it is buildable. |
| **FEAT-0015 / FEAT-0016** disk-growth retention | Snapshot state: no in-repo spec; ties to GitHub issue #4 and draft PR #9. Later on 2026-06-18, both were promoted to Accepted specs on `main`-bound docs. |

Held design-capture (a `Draft`, not on a decision): FEAT-0003 (selectable
control-law profile) is recorded in `docs/features/README.md` §2 as
not-a-net-benefit and not scheduled.

Recently resolved (context, see `git log` / `docs/next_steps.md`): the write-path
safety review closed 2026-06-17 — FEAT-0010/0011/0012/0013 are `Implemented` and
merged. Decision records: `docs/source-aware-cpu-dropout-decision-2026-06-17.md`,
`docs/corrupt-sidecar-quarantine-decision-2026-06-17.md`,
`docs/breaker-probe-decision-2026-06-17.md`.

## 5. Recommendation — thin priority index (selected and implemented)

Maintainer decision (2026-06-18): add a **thin current-priority index** to one
existing surface rather than consolidating into a new doc or `next_steps.md`.

Implemented 2026-06-18: a `## Current priority` block at the top of
`docs/features/README.md` carrying (a) the active feature work, (b) the
aggregated decisions/gates-owed queue from §4, and (c) links to — not copies of —
the per-topic backlogs in §3. It does not duplicate the per-topic plans, does not
add a new tracking system, and does not change the spec lifecycle or the
enforcement test (`tests/test_feature_specs.py` parses only registry rows whose
first cell is a `[FEAT-NNNN](...)` link, so the index block is invisible to it).

Alternatives considered and not selected: (1) consolidate everything into
`next_steps.md` as the single authoritative backlog (more upkeep; one home);
(2) leave the backlog distributed and rely on per-session re-derivation (status
quo; the cost is the branch-lag reconstruction problem in §1).

## 6. Already-settled — do not re-litigate

`docs/next_steps.md` records the maintainer's standing position that a `docs/`
subfolder reorg and archiving closed plans are "low priority… skip unless the
flat layout causes friction." This assessment does not re-recommend a reorg; the
§5 index is additive and leaves the flat layout in place.

## 7. What this authorizes

Nothing. Promoting any FEAT in §4 requires its decision/gate cleared first, then
explicit build authorization per `AGENTS.md`. Keeping the §5 index current is
maintenance, not new work: refresh the `## Current priority` block when a
feature's status or the decision queue changes.
