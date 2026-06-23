# Discovery Targets — Master Value-Ranked List

**Date:** 2026-06-14
**Status:** historical discovery artifact (NOT authoritative). Candidates only; promote via
`docs/features/` before product-code changes (AGENTS.md Feature Intake Gate).
**Source:** two verified fan-outs (`wf_45e89d06-a60` pass 1, `wf_32fc8e34-cee` pass 2);
51 verified survivors merged into actionable clusters.
**Method:** static read+grep only; every item adversarially verified. See
`discovery-loop-plan-tighter-pass-2026-06-14.md` for full evidence per item.

**Value rubric:** `value = (impact × goal_weight × reachability × confidence) ÷ effort`.
impact high=3/med=2/low=1 · goal G1=1.0/G2=0.75/G3=0.5 · reachability live=1.0/latent-reachable=0.6/
needs-future-change=0.4/unreachable-today=0.2 · confidence high=1.0/med=0.8/low=0.6 · effort low=1/med=2/high=3.
Score is relative, for ordering only — not a precision metric.

---

## Tier 1 — Do first (highest value)

| # | Target | Pass | Goal/Sev | Effort | Reach | Score | Why this rank |
|---|---|---|---|---|---|---|---|
| 1 | **HR-2** — wedged-but-alive worker has no force-recovery; watchdog only does a cooperative `--restart` that times out | 1 | G1/high | med | live | **1.50** | Live mechanism, fans freeze at last duty; the one state the watchdog claims to own and the audit *misclassifies* as recovered. The binding hazard. |
| 2 | **W2-2** — duplicate channel number silently accepted → two state machines fight one fan + crash-recovery sidecar baseline corruption | 2 | G1/high | **low** | latent | **1.80** | Highest value/effort: a one-pass validator add. Operator-typo trigger keeps it latent, but blast radius (actuation + recovery corruption) is broad. |

## Tier 2 — High value (G1, verified, clear path)

| # | Target | Pass | Goal/Sev | Effort | Reach | Score | Why |
|---|---|---|---|---|---|---|---|
| 3 | **GPU-INIT-1** — NVML init is ctor-only, no retry/re-init → permanent silent GPU demotion, watchdog-blind | 2 | G1/med | med | latent | **0.60** | Same boot-race class that already fired via SIO (2026-06-11); NVML never got the hardening. Recurring, known-real failure class. |
| 4 | **EH-2/3/4/5** — discarded write/append returns make CSV-row, snapshot, and event-log failures silent (EH-4 hides the rest) | 1 | G1/med | med | live | **0.80** | Evidence-integrity (not direct thermal), but the most reachable (any disk/stream error) and a coherent one-decision fix. |
| 5 | **W6-2** — reconcile restore-fail refuses boot AND leaves orphaned fan at killed duty; supervisor gives up after one startup failure | 2 | G1/med | med | latent | **0.48** | Worst *outcome* (no thermal control at all), but compound trigger (leftover sidecar + restore fail + warm reboot). |
| 6 | **Curve-shape gap** (W2-1+AM-2) — `ValidateCurve` allows duty-non-monotonic / duplicate-temp / single-point curves → cooling inverts as part heats | 2 | G1/med | low | latent | **0.96** | Thermally dangerous per-event, cheap fix, but needs a maintainer contract decision first (reject-on-load vs documented). |
| 7 | **AM-1** — GPU thermal-read miss → envelope 0.0 with `gpu_available=true` → curve-minimum, no `sensor_failed` | 2 | G1/med | low | needs-future | **0.64** | Reachable analog of W1 (0.0 not NaN). Latent (deployed RTX reads nonzero) but proven; **scope with GPU-INIT-1** (shared `available=false` predicate) → near-zero marginal effort. |
| 8 | **W10-2** — sentinel self-heal only at next build → watchdog stays Disabled (triggers don't fire) until then | 2 | G1/med | med | latent | **0.48** | Adjacent to HR-2 (watchdog *absent* vs *can't-act*); compound precondition. |
| 9 | **HW-01+CONC-3+F1** — SampleCpuCycles affinity path weaker than its own validation probe (no confirm-spin, ignored restore, no RAII, untested) | 1 | G1/med | med | latent | **0.40** | Evidence-correctness for FEAT-0006; default-off/log-only/quarantined so not thermal. |
| 10 | **W9-1+W9-2** — both tasks `Interactive` logon → no pre-logon recovery; b03aa76 boot-trigger rationale is false (doc-drift masks it) | 2 | G1+G3/med | med | latent | **0.40** | Worst case behind unverifiable auto-logon state; W9-2 doc-drift is load-bearing. |
| 11 | **F3** — incident-proven SIO init-retry (5×250ms) + transient-read retry (3×75ms) have no automated test | 1 | G1/med | med | live | **0.80** | Shipped reliability code with zero coverage; test, not a defect. |
| 12 | **HR-1+HR-4** — watchdog liveness defeatable by PID-reuse (HR-1) and backward-clock/DST staleness clamp (HR-4) | 1 | G1/med(HR-4) | med | latent | **0.48** | HR-4's DST fall-back is a once-a-year guaranteed trigger; hardens HR-2's detection inputs. |

## Tier 3 — Medium value

| # | Target | Pass | Goal/Sev | Effort | Reach | Score | Why |
|---|---|---|---|---|---|---|---|
| 13 | **AP-2** — analyze report scans `tick_channel_samples` 4× with 2 non-PK sorts; missing covering index | 1 | G2/med | low | live | **0.75** | Top performance item; off the live control path (batch tool). Cheap (`CREATE INDEX`). |
| 14 | **W2-3** — channel object missing `"channel"` key silently dropped → whole fan removed from control | 2 | G1/med | low | latent | **0.96** | Bundle with W2-2; contradicts the loader's own fail-loud intent. |
| 15 | **W7-1** — `log_retain_days==0` (or `rotate_hours==0` in a long session) disables pruning, no guard → unbounded growth | 2 | G1/low | low | needs-future | **0.40** | Hand-edit/zero-config trigger; feeds the EH-2 silent-row-loss path. |
| 16 | **W7-3** — `RuntimeCsvLogger::Open()` failure silently disables session logging (evidence_log handles the same case) | 2 | G1/low | low | latent | **0.36** | Observability; thermal unaffected. Inconsistency with a sibling that does it right. |
| 17 | **W6-3** — calibrate / write-once take no runtime singleton → concurrent sidecar clobber with the live worker | 2 | G1/low | low | latent | **0.36** | Rare operator action; also affects kWriteOnce. |
| 18 | **HR-3** — read-path-only PawnIO/AMD outage leaves status `running` (never classified failed) | 1 | G1/low | med | latent | **0.18** | Belongs to FEAT-0004 (Draft); CpuOnly channels *do* degrade, only GPU-bearing track silently. |
| 19 | **HR-5** — supervisor `worker_restart_count` never resets → backoff pins at 32s + count conflation | 1 | G1/low | med | latent | **0.18** | Per-process scope; modest recovery slowdown + observability. |
| 20 | **F2** — hold-expiry `kRestoreFailed` (non-timeout) branch untested | 1 | G1/low | med | latent | **0.18** | Fan retains curve control (not stranded); needs a maintainer policy ruling. |
| 21 | **W10-1** — sentinel write-before-disable ordering (one-line reorder) | 2 | G1/low | low | latent | **0.36** | Trivial hardening within the build-release sentinel mechanism. |
| 22 | **CONC-1** — first `--stop` in the supervisor/worker startup window can be clobbered by clear-calls | 1 | G1/low | med | latent | **0.18** | Silent-failure framing refuted (surfaces as 15s timeout + exit 2, retryable). |
| 23 | **F5** — `pending_writes_unreadable→kFailed` early-return precedence untested | 1 | G1/low | low | latent | **0.36** | Correct today; regression-guard test (use malformed/dir, not absent file). |
| 24 | **W8-A** — no baseline-restore on a normal OS shutdown/reboot/logoff; no clean status/event either | 2 | G1/low | med | latent | **0.18** | Thermal covered by reconcile-on-boot; residual is an audit-trail/doc gap. |
| 25 | **AP-3** — `IngestEvents --force` does a per-run COUNT+UPDATE (N+1) | 1 | G2/low | low | live | **0.75** | Rare maintenance path; collapsible to one set-based UPDATE. |
| 26 | **W8-B** — console handler returns TRUE then races the fan restore on the main thread (CTRL_CLOSE) | 2 | G1/low | low | needs-future | **0.24** | Foreground dev-mode only; not the deployed (file-based stop) path. |

## Tier 4 — Low value (cleanup / doc / test-only / latent-only / already-covered)

| # | Target | Pass | Goal/Sev | Effort | Status | Why low |
|---|---|---|---|---|---|---|
| 27 | **DRIFT-2** — `CONTROL_LOOP.md` lists fields as required that the loader treats as per-channel optionals | 1 | G3/med | low | doc-only | Zero runtime impact; `must`=enforced convention violation. |
| 28 | **G3-01** — channel CSV column contract hand-replicated across 4 files, no cross-list assertion | 1 | G3/low | low | latent | Lists agree today; worst case analysis data loss. |
| 29 | **W3-3** — `duty_pct→duty_raw` conversion has no test (**conversion itself verified CORRECT**) | 2 | G3/low | low | test-gap | Regression surface only; bundle with the write-path pass. |
| 30 | **W5-1** — low-band integrator state machine has no C++ unit test | 2 | G3/low | med | test-gap | Coverage gap on a stateful path; nothing wrong today. |
| 31 | **W2-4** — curve point missing `temp_c`/`duty_pct` silently dropped → reshaped curve | 2 | G3/low | low | latent | Per-point typo trigger; fold into config-validation pass. |
| 32 | **G3-02** — three SHA-256-over-BCrypt impls with three hex encoders | 1 | G3/low | low | latent | Identical output today; extract `BytesToLowerHex`, keep streaming. |
| 33 | **HP-2** — `snapshot_age_ms` re-parses an ISO string the sampler just formatted | 1 | G2/low | low | partly-done | Historical control-optimization register was folded into `docs/control-latency-reduction-design-2026-06-18.md`; trivial vs hardware-read budget. |
| 34 | **DRIFT-1** — `CONTROL_PIPELINE_MATH §6.1` anti-windup guard the integrator doesn't have | 1 | G3/low | low | doc-only | Output byte-identical; already a PATH_NOTES "Idea (verify)". |
| 35 | **WAC-1** — FEAT-0005 core gap confirmed in code (write success = driver-error-only, no readback) | 2 | G1 | low | parked | Fully covered by parked FEAT-0005; **doc maintenance only**. Per-tick RPM/duty readback IS available → resolves the spec's open question. |
| 36 | **WAC-3** — shutdown restore-on-exit is itself an unconfirmed write | 2 | G1/G3/low | low | parked | Fold into FEAT-0005 confirmation scope; exits with fans at a temp-tracking duty. |
| 37 | **WAC-4** — FEAT-0005 §2 line anchors drifted | 2 | doc | low | doc-only | Refresh when FEAT-0005 is touched. |
| 38 | **HW-03** — Zen 5 (model 0x44) reads per-CCD Tdie via a Zen 4 register base | 1 | G1/low | low | telemetry | Control input isolated by exact label; pending live-validation item, not a defect. |
| 39 | **HW-02** — `ExecutePawnIo` bounds `input_count` not `output_count` | 1 | G1/low | low | unreachable | Internal linkage, all callers `output_count=1u`, fully guarded. "Nothing to discover." |
| 40 | **CONC-5** — PCI-mutex `WAIT_ABANDONED` adopted with no breadcrumb | 1 | G3/low | low | refuted | Weakest survivor; garbage-read mechanism refuted. **Recommend drop.** |

---

## Still-uncovered (after 2 passes; not yet findings — would need a 3rd pass or targeted tool)

`runtime_health.cpp` DST/backward-clock age-clamp masking (G1, once-a-year) · supervisor
restart-policy asymmetry (startup-give-up vs retry-forever) · cross-layer CSV column-contract drift
(use the **schema-validator** skill) · 01452dc trailing-header-skip parser test gap · analyze
ingest dedup/transaction correctness (G3).

## Reading the ranks

- **Effort-adjusted:** W2-2 (#2) and the curve-shape/W2-3 config items score high partly because
  they are **low-effort** — a single `control_loop_config.cpp` validator pass clears #2, #6, #14, #31.
- **Cluster, don't cherry-pick:** AM-1 (#7) + GPU-INIT-1 (#3) share one fix; the four config items
  share one pass; the FEAT-0005 WAC items are doc-only until the spec is un-parked.
- **Score ≠ urgency alone:** HR-2 (#1) has a lower raw score than W2-2 (#2) only because it's
  medium-effort; it is the overall #1 by *hazard* (live, the watchdog's claimed job).
