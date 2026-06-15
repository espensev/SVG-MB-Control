# CPU Transient Power-Anticipation ("Spike Catcher") — Plan Scaffold — 2026-06-15

Status: **proposal / planning scaffold only.** Not authorized work
(`AGENTS.md` §Feature Intake Gate): no code, config, log-schema, or behavior
change is permitted from this document, including the producer-cadence change and
the shadow CSV columns below — those are product-code/schema work that requires a
new implementation-authorized FEAT first (§9). The owning v1 telemetry decision
(`docs/cpu-work-energy-acquisition-decision-2026-06-07.md` §Scope) holds: logged
energy "must not feed any control path." This document scaffolds a *transient*
(slope-keyed) sibling of the parked level-keyed power-anticipation boost up to,
not through, its FEAT intake. It is a companion to
`docs/cpu-power-feedforward-plan-2026-06-10.md`; that plan's §5 Gates and §6
Evaluation own the decision criteria, and this document inherits them.

This revision (2026-06-15) incorporates a four-lens adversarial review (math/
physics, governance, producer-cost, completeness). The substantive corrections it
folds in are flagged inline as **[rev]**.

The parked level variant is denoted there $B^{(c,\mathrm{pwr})}$. To avoid
collision, the term proposed here is the **transient power-anticipation boost**
$B^{\mathrm{spk}}_k$ (slope-keyed, edge-not-level).

---

## 1. Need (why the Gate 2 no-go does not close this — and what it leaves open)

The Gate 2 characterization (`docs/cpu-power-anticipation-gate2-characterization-2026-06-15.md`)
found, across three sessions, `watts − busy` onset offset = **0.0 s** and
`Tctl − watts` within ±1 tick of zero. Read against §6 of the companion plan,
that parks the level-keyed power variant *for control*: `system_cpu_busy_pct` is
validated, always-on, and captures the entire measured lead on those steps.

But every Gate 2 session was a single **idle → full** synthetic step
(`cpu-synth-load.exe`, 28/all threads) — the one load case where `busy%` already
wins, and not the case this feature is for. The target is the opposite regime:

- **Mid → spike from a busy-saturated baseline.** A y-cruncher VT3 / AVX-512
  burst entered while the machine is already at ~99 % busy surges package power
  (e.g. 180 W → 230 W) at **constant** busy%. `system_cpu_busy_pct` is pinned at
  100 % and does not move — it is **blind** to exactly the events we would want
  to react to. So the level no-go (power does not beat `busy%` on idle→full steps)
  says nothing about this stimulus, which the campaign never captured.

**[rev] The honest framing: `busy%` is the wrong comparator here; `dTctl/dt` is
the one that binds.** `busy%` is blind at pinned occupancy, but `dTctl/dt` is
not — and it is free and validated. The relevant question is therefore
**power-slope vs `dTctl/dt`**, not power vs `busy%`. And the Gate 2 record sets a
hard prior on that contest:

- Gate 2 measured `Tctl` reaching its 50 %-span within ~1 tick of watts on the
  step, because `Tctl` is the on-die sensor — the thermal mass between die power
  and the `Tctl` junction is small, so the die→`Tctl` time constant
  $\tau_{\mathrm{die}}$ is sub-second (~0.25–0.5 s on this part).
- Package power leads `dTctl/dt` by exactly $\tau_{\mathrm{die}}$. That is the
  entire structural head start this feature can exploit. It is **sub-second**,
  and §5.2 shows the smoothing needed to make a slope usable can consume most of
  it.

So this document is written with an **a-priori expectation that the margin is
thin** and may not clear fan-side response latency — the same physics prior
(small die→`Tctl` lag) that made the level variant's margin vs `busy%` zero. The
§8 go/no-go is set up to falsify or confirm that thin-margin expectation on the
correct stimulus; the honest base rate is that it parks too (§11).

The hard constraint, stated up front: **catching the spike earlier must not
perturb the mid/low ~99 % steady response.** Steady state is already converged by
the temperature-keyed law (companion §1: anticipation "cannot improve steady
state"). A term keyed on the *level* of power would fire continuously at
sustained 99 % load and add duty on top of the converged law — the §6
duty-pumping failure mode. The whole design below exists to get edge response
without that.

## 2. Why package energy stays in regardless (fair thermal evaluation)

Independent of any control use, **package watts is the denominator for any fair
thermal comparison.** Two control tunes that both peak at 84 °C are not
equivalent if one was shedding 180 W and the other 230 W — without W, °C is
measured against an unknown load. Energy telemetry therefore enables, as
*evaluation* signal: °C/W (thermal-resistance-style) metrics across tunes;
"temperature at matched W" comparisons; and detection of the failure where a
"cooler" result is just lower load, not better control.

**[rev] This value is already delivered by the shipped ~1 s mirrored watts**
(analyzer schema v9 time-weighted `ComputePackagePower`); it does **not** depend
on the un-mirrored producer change proposed below. The fair-eval case is strictly
orthogonal to control use and to the §5.1 cadence change: the Gate 2 control
no-go does not touch it, and the quarantine-exit energy work
(`cpu_pkg_energy_acquisition` validated 2026-06-14) has a permanent home as
evidence/eval telemetry. The validation in §8 reports W alongside every earliness
number so the spike sessions are compared at equal heat input.

## 3. The idea — an edge-keyed transient, not a level boost

$B^{\mathrm{spk}}$ is driven by the **rate of change** of package power, not its
level. The defining property: at any plateau — *any* absolute power, including
sustained 99 % load — the slope is ≈ 0, so the term is identically zero. Output
is produced only by a **rising** power edge. A machine at steady 99 % has no
edges, so it gets no boost; a y-cruncher burst is a steep rising edge, so it gets
a fast kick that washes out as the plateau is reached and the temperature-keyed
law takes over.

This is the formal answer to the §1 constraint: steady-state safety is *by
construction*, not by tuning (§6). It is also falsifiable (§6, §8).

**[rev] Scope bound vs the companion's reopen criterion.** The parked plan's
Outcome (2026-06-15) named the saturated-busy regime as "a steady/high-load
amplitude property [that] would seed a separate power-keyed *proportional*
feature." This term is **not** that feature: by §6 a *sustained* AVX load gets
$B^{\mathrm{spk}} \to 0$ after the entry edge. $B^{\mathrm{spk}}$ addresses only
the **brief transient** at burst onset; sustained high-power-at-pinned-busy
remains the amplitude case the companion deferred. The two scaffolds do not claim
the same regime.

## 4. Signal choice

The signal must be (a) sensitive to a surge at pinned busy, (b) faster than the
event (<1 s), (c) cheap enough in validation and access cost.

| Signal | Visible at pinned 99 % busy? | Latency | Access cost | Verdict |
|---|---|---|---|---|
| `system_cpu_busy_pct` | **No** — saturated at 100 % | fast | none (validated) | Out for this purpose; this is *why* the validated signal can't deliver it |
| RAPL pkg watts, **as shipped** (~1 s window mirrored across ticks) | yes | ~1 s, 4× coarser than ticks | none beyond current 2-MSR allow-list | Too coarse for a <1 s edge; `dW/dt` is quantization-of-time garbage. The Gate 2 limiting artifact |
| RAPL pkg watts, **un-mirrored** (poll accumulator at the 250 ms tick) | yes | ~250 ms | **gated producer change** (§5.1/§9): re-opens FEAT-0006 criterion 6 + Gate 1; same 2 MSRs | Cheapest path that resolves the edge — but not free; behind Gate 3 |
| CPU current / EDC via SVI3 | yes | earliest electrical signature of AVX-heavy bursts | **new telemetry path** beyond the RAPL allow-list → HVCI/blocklist exposure, fresh validation | Physically the *right* signal; highest uplift, highest cost. Deferred until §8 shows the un-mirrored-power path is insufficient |
| `dTctl/dt` (already logged) | yes (within ~$\tau_{\mathrm{die}}$) | downstream of the heat by $\tau_{\mathrm{die}}$ (sub-second) | none (validated) | **The primary comparator** — free, validated, and only ~$\tau_{\mathrm{die}}$ behind power |

**[rev] Decision: the primary go/no-go is power-slope vs `dTctl/dt`, not vs
`busy%`.** Prototype on un-mirrored RAPL only to *measure* whether power's
$\tau_{\mathrm{die}}$ head start survives smoothing and beats `dTctl/dt` by enough
to justify the producer change (§5.2, §8). Per the companion §6 "a cheaper
alternative exists and must be compared" discipline, ported from `busy%` to
`dTctl/dt`. The earlier "downstream / latest" dismissal of `dTctl/dt` was wrong:
Gate 2 shows the heat reaches `Tctl` almost immediately, so `dTctl/dt` is the bar
to beat, not a fallback.

## 5. Math (function contract — to be pinned by tests before FEAT intake, in the style of `src/control/power_anticipation.h`)

All times use the **measured** tick interval $\Delta t_k$, not an assumed 250 ms.

### 5.1 Un-mirrored watts input

Poll accumulator `0xC001029B` each control tick; timestamp the read.

$$W_k = \frac{\Delta E_{\mu J,k}}{\Delta t_{\mathrm{ms},k}}\cdot\frac{1}{1000}\ \ [\mathrm{W}],\quad \Delta E_{\mu J,k}=(E_k - E_{k-1})\ \text{with 32-bit wrap add}$$

`NaN` (not zero) on a missing/negative-after-wrap delta, a non-positive
$\Delta t$, or a blank `cpu_power_sample_id`.

**Noise reality (quantization is a non-issue).** At ~200 W / 250 ms,
$\Delta E \approx 5\times10^{7}\ \mu J$ against a 15.26 µJ ESU, so quantization is
~$3\times10^{-7}$ relative. The prior "un-mirroring makes W 4× noisier" claim was
wrong and is retracted.

**[rev] What the per-tick error actually is.** Because the accumulator is a
running energy *integral*, $W_k=\Delta E/\Delta t$ with a correctly measured
$\Delta t$ is unbiased under tick jitter — a long tick captures proportionally
more $\Delta E$, so the integral self-corrects. Timestamping does not "remove
jitter"; it is simply the correct denominator. The residual error is (a)
read-latency skew between the `steady_clock` timestamp and the `rdmsr` (energy
accrues over a slightly different interval than the measured $\Delta t$), and (b)
within-window load *variance* of instantaneous W when load is non-constant — and
(b) is exactly what §5.2 smooths, at the cost of the §5.2 lag. The jitter problem
and the smoothing-lag problem are the same problem.

**[rev] Fork, not in-place replace (decision needed at intake).** The prototype
reads the accumulator into a **new shadow column alongside the untouched,
validated ~1 s producer** — it does **not** mutate the validated
`cpu_power_sample_id` window contract or the analyzer's per-window GROUP-BY /
time-weight aggregation. An in-place cadence change (mirrored ~1 s → 250 ms) would
redefine "window" ~4× faster and silently invalidate the data stream that
`cpu_pkg_energy_acquisition` was flipped to `validated` on 2026-06-14, and the
analyzer consumer; it is out of scope here. Note the fork still incurs the
criterion-6 disturbance cost (§9) — it is the read *frequency*, not the column,
that disturbs the loop.

**[rev] Re-derive the stall guard for the cadence.** The shipped
`kEnergyDtCapMs = 3000` was sized as ~3× the 1 s window; at a 250 ms cadence that
is ~12 ticks loose, so a stalled read would pass the guard and emit one sample
spanning many missed ticks — a garbage slope exactly where §5.6 wants NaN-decay.
The cap is re-derived to ~600–750 ms ($k\approx 2$–$3\times$ tick) so a stall
blanks to NaN rather than aliasing.

### 5.2 Smoothing, slope, and the lead budget

$$\tilde W_k = \alpha_W W_k + (1-\alpha_W)\tilde W_{k-1},\quad \alpha_W = 1-e^{-\Delta t_k/\tau_W}$$
$$s_k = \frac{\tilde W_k - \tilde W_{k-1}}{\Delta t_k}\ [\mathrm{W/s}],\quad \tilde s_k = \alpha_s s_k + (1-\alpha_s)\tilde s_{k-1}$$

Differentiating amplifies noise, hence the EMA on $\tilde W$ before and optionally
on $s$ after.

**[rev] Lead budget — the design must clear this before any producer change is
justified.** The benefit un-mirroring buys is the recovered mirror staleness; the
smoothing chain spends it back as group delay. A first-order EMA has group delay
≈ $\tau$ at the edge. Define the net realizable lead over the as-shipped path:

$$L_{\mathrm{net}} \approx L_{\mathrm{mirror}} - \tau_W - \tau_s - L_{\mathrm{slew}}$$

with $L_{\mathrm{mirror}} \le \sim 0.75\text{–}1.0\ \mathrm{s}$ (one ~1 s window
age), $\tau_W \approx 0.5\ \mathrm{s}$ at the proposed constant, $\tau_s$ the
optional slope-EMA delay, and $L_{\mathrm{slew}}$ the ticks for the §5.5
integrator to reach a useful boost. At $\tau_W=0.5\ \mathrm{s}$ with a second EMA,
$L_{\mathrm{net}}$ can approach **zero** — the benefit and the noise-control
mechanism are in direct conflict. **The proposed constants must be shown to give
$L_{\mathrm{net}} > 0$ with a margin over fan-side latency, from §8 captures,
before the §9 producer change is authorized.**

**[rev] The comparison is fair to `dTctl/dt`, which is also smoothed.** A
`dTctl/dt` pre-trigger needs its own EMA (lag $\tau_C$). So the real quantity is
the *differential*:
$L_{\mathrm{net}}(\text{power}) - L_{\mathrm{net}}(\mathrm{dTctl/dt}) \approx
\tau_{\mathrm{die}} - (\tau_W - \tau_C)$. Power's only structural advantage is
$\tau_{\mathrm{die}}$ (§1, sub-second); if the two signals are smoothed
comparably it survives, but it is sub-second to begin with. This differential —
not an absolute power lead — is the §8 headline number.

### 5.3 Slope band → drive (smootherstep, $S(x)=6x^5-15x^4+10x^3$)

$$u_k = \mathrm{clamp}_{[0,1]}\!\left(\frac{\tilde s_k - s_{\text{start}}}{s_{\text{full}}-s_{\text{start}}}\right),\qquad r_k = S(u_k)$$

$s_{\text{start}}$ sits above the steady-state slope-noise floor; $s_{\text{full}}$
is a "real surge" slope. **Both come from §8 captures, not invented** — and both
depend on the burst *rise time*, not the step height (§8): a 60 W rise over
0.25 s is 240 W/s, over 2 s is 30 W/s, implying entirely different bands.
Smootherstep, not a hard ramp, against duty pumping near the band edge.

### 5.4 Level safety gate

$$g_k = \mathbb{1}[\tilde W_k \ge W_{\text{floor}}],\qquad d_k = g_k\,r_k\,B_{\max}$$

**[rev] $W_{\text{floor}}$ clears the *excursion* band, not the quiet floor.**
Gate 2 Appendix C.2 measured the operative idle ceiling on a live machine as the
pre-load excursion p95/max of **94–121 W**, not the `busy<5%` quiet floor of
45–60 W. $W_{\text{floor}}$ is pinned above 94–121 W from §8 captures, or an
ordinary background spike clears it and defeats the gate. Note this gate is partly
redundant with slope-keying (§3 already suppresses non-sustained idle micro-rises);
its incremental value over slope-keying alone is to be justified, not assumed.

### 5.5 Integrator — **[rev] a deliberate divergence, not "the §6.1 shape"**

$$B^{\mathrm{spk}}_k = B^{\mathrm{spk}}_{k-1} + \mathrm{clamp}\!\big(d_k - B^{\mathrm{spk}}_{k-1},\ -\text{fall}\cdot\Delta t_k,\ +\text{rise}\cdot\Delta t_k\big)$$

This is a **slew-rate-limited target-tracker** (it slews $B$ toward a target level
$d_k$), **not** the accumulator the shipped `UpdatePowerAnticipation` / §6.1
`UpdateBoostStage` implements (which integrates `boost += rise·scale·Δt` while
in-band, with no target and ramps to `max_boost` on sustained input). The
divergence is intentional: a transient slope *pulse* should yield a bounded boost
*pulse*, not time-integrate to max. Consequence: §5.5 needs **its own pinned unit
tests**; it is not covered by the existing `power_anticipation_tests.cpp`, and §9
"mirroring the companion" applies to rollout discipline, not to this control law.
`rise` large (fast attack); `fall` moderate (washes out without chatter),
consistent with the fan-coast-on-descent behavior.

### 5.6 Fail-safe (and the producer-skip interaction)

`NaN` input, a stale producer (age-out exceeded), or a wrap-blank forces
$d_k = 0$, so $B^{\mathrm{spk}}$ decays at `fall` to zero — it never holds on a
fault. Identical posture to `UpdatePowerAnticipation`'s decay-on-unavailable.

**[rev] Reconcile with the producer budget-skip.** The energy producer's
disturbance-mitigation §6 "budget skip" blanks a window rather than risk a tick
overrun, and skips become more frequent *under load* — exactly the regime where
the spike must fire. A skip blanks → NaN → decay, so the disturbance-avoidance
mechanism can suppress the boost at the moment of the surge. §8 robustness must
measure the budget-skip rate during the headline sessions and confirm the surge
edge is not routinely blanked; if it is, that is a no-go for the synchronous
un-mirrored read and an argument for a dedicated sampler thread (§9).

### 5.7 Composition with the CPU-heat boost stack (anti-double-count + handoff)

**[rev] Compose against the *active* stack, not only `thermal_pressure`.** Gate 2
shows `Tctl` plateaus ~81 °C and reaches the 84–86 °C `thermal_pressure_start_c`
late or never; the term actually engaged in the warm/saturated regime is
**`midband_pressure` (64 °C)**, already active before load in all three sessions
(and `cpu_low_soak` at 72 °C may also be engaged). Composing $B^{\mathrm{spk}}$
only against $B^{\mathrm{thp}}$ targets a mostly-inactive sibling. The combined
CPU-anticipation contribution is one of (spec decision):

- **C1 — winner-take-all (safe default):**
  $C^{\mathrm{cpu}}_k = \max\!\big(B^{\mathrm{mid}}_k,\ B^{\mathrm{soak}}_k,\ B^{\mathrm{thp}}_k,\ B^{\mathrm{spk}}_k\big)$.
  Joint ceiling = the larger single cap; the terms never sum.
- **C2 — attenuated-additive (smooth handoff):**
  $C^{\mathrm{cpu}}_k = B^{\mathrm{temp}}_k + B^{\mathrm{spk}}_k\,(1-\rho^{\mathrm{temp}}_k)$,
  where $B^{\mathrm{temp}}$ is the resolved temperature-keyed CPU-heat boost and
  $\rho^{\mathrm{temp}}_k$ its engaged fraction over the **full** stack (not just
  thp). As the temperature law engages, $B^{\mathrm{spk}}$ fades out.

**[rev] Marginal-duty analysis is mandatory.** Under C1, $B^{\mathrm{spk}}$ adds
duty only when it *exceeds* the already-engaged `midband` boost at the warm
baseline. That marginal headroom — not the raw $B_{\max}$ — is the real benefit,
and it must be quantified against the §C.2 warm-baseline data, because if midband
already commands $\ge B^{\mathrm{spk}}$, the transient term contributes nothing.
There is also a coverage risk: if the spike sustains *below* 84 °C, $B^{\mathrm{spk}}$
washes out (slope→0 at the new plateau) with no `thermal_pressure` term ramping to
replace it — a transient bump with no sustained handoff. §8 verifies the wash-out
coincides with a temperature term carrying the demand at the realistic ~81 °C
plateau. Lands in `correction_pct`; the `feedforward_pct` identity stays untouched.

### 5.8 Candidate channels

Same as the companion §4: CPU-heat movers — radiator exhausts `1`/`5`, rear
exhaust `0`, radiator intake `4`. Per-channel; absent parameters = stage disabled
(§6.1 guard-clause semantics). Selection is a §8-data spec decision.

## 6. Steady-state safety property (the claim that protects mid/low)

At any plateau where $\tilde W$ is constant (any absolute level, including 99 %
load): $s \to 0 \Rightarrow u \le 0 \Rightarrow r = 0 \Rightarrow d = 0
\Rightarrow B^{\mathrm{spk}} \to 0$. The term is **identically zero at steady
state regardless of absolute power.** Producing output requires a rising $W$ edge;
mid/low 99 % steady is a plateau, so $B^{\mathrm{spk}} = 0$ there. Testable, not
asserted: in shadow mode (§7) the boost over a sustained-load plateau must read
≈ 0 after the entry transient (§8 case 2 is the falsification test).

## 7. Shadow instrumentation (Phase A — log only, after Gate 3; NOT added into §8.2)

**[rev] These four columns are a log-schema change and are gated behind Gate 3**
(§9), not "Phase A now." Mirroring the energy path's own rollout: compute
everything, add nothing to duty, log additive CSV columns for replay.

- `cpu_pkg_watts_unmirrored` — §5.1 tick-cadence W, a **control-prototype input
  only**. (The §2 fair-eval denominator is served by the existing validated ~1 s
  mirrored watts and does not need this column.)
- `cpu_dwatts_dt_wps` — §5.2 smoothed slope $\tilde s$ (W/s).
- `cpu_dtctl_dt_cps` — smoothed `dTctl/dt` (°C/s), for the §4 primary comparison.
- `channelN_power_spike_boost_shadow_pct` — §5.5 computed $B^{\mathrm{spk}}$ per
  candidate channel (the replay target).

Default-off gate, same as the energy producer.

## 8. Validation with data

The point of this campaign is to capture the **load cases Gate 2 missed.** Each
case uses ≥ 3 independent sessions (repeatability bar) on the **same binary**
(Gate 2's session 1 differed in git hash and load config — avoid repeating that).

**[rev] Load tooling — first-party generator drafted 2026-06-15
(`tools/cpu_synth_spike_load.cpp`, OFF-by-default `SVG_MB_CONTROL_BUILD_SYNTH_LOAD`,
`/W4 /permissive- /arch:AVX2`, compiles clean).** The shipped
`tools/cpu_synth_load.cpp` is a fixed all-core *saturator* (idle→full only) and
cannot produce this stimulus. The new sibling holds **constant occupancy** with a
persistent worker pool that never sleeps (so `busy%` stays pinned), flipping every
worker via an atomic phase flag between a **memory-latency floor** (per-thread
DRAM-resident pointer-chase — busy pinned, watts low) and an **AVX2 FMA burst**
(watts surge), and timestamps every transition to stdout and an optional
`--marker-file` CSV (`event,iso,unix_ms`) — the **ground-truth spike-onset zero**
the earliness metric (§8 Replay) needs. `--bursts 0` is the §8 case-2
sustained-steady control session; `--bursts K` is case 1. It is first-party (no
`AGENTS.md` repo-boundary issue) and read-only (FP math + private-buffer loads; no
MSR, no fan writes). Two items remain before Phase B can run: **(i)** wiring it
into `Capture-EnergySession.ps1` (which currently invokes the saturator), and
**(ii)** the burst is AVX2 (a reproducible proxy); a real **y-cruncher VT3**
(AVX-512) cross-check gives a larger, more representative surge and remains a
complementary capture.

**Capture set:**

1. **Mid → spike (the headline case).** Sustained moderate load holding a fixed
   *lower-power* plateau at ~99 % busy, then injected y-cruncher VT3 bursts that
   surge package power ~40–60 W at constant busy, each emitting the injection
   marker. *Measure:* the §5.2 differential lead of `power_spike_boost_shadow`
   rise vs the `dtctl_dt` pre-trigger (primary), and vs `thermal_pressure` onset;
   anchor "earliness" to the injection marker; report `cpu_pkg_watts_unmirrored`
   alongside for fairness; record the producer budget-skip rate (§5.6).
2. **Sustained-steady control (the no-disturbance proof).** Long flat 99 % load,
   no bursts. *Exit:* `..._shadow_pct` p95 over the plateau < ε. The session that
   proves mid/low is untouched — the §1/§6 constraint made into a measurement.
3. **Idle (floor proof).** Zero firings; `..._shadow_pct` = 0 across idle.
4. *(optional)* **Idle → full step.** Reproduce the Gate 2 baseline; confirm the
   new term agrees with `busy%` there. Sanity, not the point.

**[rev] A-priori bar (set before capture).** From Gate 2: $\tau_{\mathrm{die}}$ is
~1 tick, the steady plateau is ~81 °C (midband engaged, `thermal_pressure` not),
and the operative idle watts ceiling is 94–121 W. The go/no-go bar is therefore
pre-registered: $B^{\mathrm{spk}}$'s lead must exceed **both** fan-side latency
(rate limiter + spin-up) **and** the free `dTctl/dt` pre-trigger by a useful
margin. The base-rate expectation (§1) is that it does not.

**Replay (Phase B).** Run the §5 pure function over the captured CSVs (the
un-mirrored W and the raw accumulator must be in the capture). Fix as quantitative
exit criteria in the eventual FEAT spec:

- **Earliness (vs the free alternative — PRIMARY).** Median differential lead of
  $B^{\mathrm{spk}}$ over the `dTctl/dt` pre-trigger, anchored to the injection
  marker. **If `dTctl/dt` captures most of the lead, do that and skip the RAPL
  change entirely.**
- **Earliness (vs fan-side latency).** The lead must clear the rate-limiter +
  spin-up budget by a useful margin, else **stop** (companion §5).
- **No disturbance.** `..._shadow_pct` p95 over steady plateaus < ε; zero idle
  firings; peak magnitude ≤ $B_{\max}$; producer criterion-6 metrics within
  baseline (§9).
- **Robustness / chatter.** Behavior under un-mirrored-W jitter; wrap/blank/skip →
  decay, no false kick; boost on/off transitions per minute bounded.

**[rev] Statistical power, separate from the repeatability bar.** "≥ 3 sessions"
is the acquisition-trust/repeatability threshold (binary + load-config), *not* a
justification for estimating a median lead. The headline metric is a distribution,
so the spec fixes a per-session **burst count** (N injected bursts) and a target
confidence interval on the median, independently of the 3-session bar.

**Tooling.** New `scripts/analyze_power_spike.py` (or an `analyze_power_onset.py`
extension): from `session.csv` + the raw accumulator + the injection marker,
compute un-mirrored W, $\tilde s$, and `dTctl/dt`; replay the shadow
$B^{\mathrm{spk}}$; report differential-lead distributions, steady-plateau
magnitude, and the budget-skip rate. Read-only, stdlib only, like the existing
analyzers.

## 9. Gates / phases — **[rev] producer + schema work is behind Gate 3**

Pre-FEAT. **Nothing in this document authorizes runtime, schema, or config
changes.** The companion plan places all producer/shadow work *after* Gate 3
authorization; this plan matches that ordering.

- **Now — paper design only.** This scaffold. No code, no MSR-cadence change, no
  CSV columns.
- **Gate 1 — always-on decision (inherited).** A control input cannot depend on a
  default-off env var (companion §5 Gate 1). Going live forces the un-mirrored
  read **always-on at ~4× the validated MSR-read rate** — a strictly harder
  disturbance posture than the level variant's contemplated 1 s always-on read.
  The criterion-6 evidence for that must be gathered *at the un-mirrored cadence*;
  it is **not** inherited from the 1 s closure.
- **Gate 3 — FEAT intake.** New FEAT number (FEAT-0006 cannot be expanded — its
  decision record excludes control use; and per Gate 2 Appendix C.4(a) a cadence
  change re-opens FEAT-0006 criterion 6, so any acquisition amendment is routed
  through the new FEAT or an explicit FEAT-0006 acquisition amendment with its own
  no-disturbance re-validation). The spec fixes $s_{\text{start}}$,
  $s_{\text{full}}$, $B_{\max}$, rates, $W_{\text{floor}}$, the re-derived dt-cap,
  the C1/C2 composition rule over the full CPU-heat stack, and the fork-vs-replace
  decision, from §8 evidence, plus `docs/TRACEABILITY.md` entries.
- **After Gate 3 — un-mirror producer change (§5.1).** Poll `0xC001029B` at the
  tick (or a dedicated short-lived sampler thread to keep the read off the control
  hot path), same 2-MSR allow-list (no new MSR, no HVCI/blocklist exposure),
  re-derived wrap/stall guard, NaN-not-zero. **This reverses the energy decision's
  disturbance-mitigation rule #2 (resource-window, not tick cadence) and
  invalidates the 2026-06-14 criterion-6 closure**, so it requires a fresh
  criterion-6 gate against a named disabled baseline *under the spike load*:
  `loop_slip_ms` p95/max bounded, zero new `loop_overrun`, no SMN-read-failure
  increase, no steady-state `authority_reasserted` increase.
- **After Gate 3 — Phase A shadow logging (§7)** → **Phase B replay (§8)** → fix
  exit criteria → run the go/no-go → phased live enable (shadow → per-channel
  low-cap → multi-session promotion), mirroring the companion §7.

## 10. Open questions for the maintainer

1. **Load tooling (§8):** the first-party constant-occupancy generator now exists
   (`tools/cpu_synth_spike_load.cpp`, emits the spike marker). Remaining: wire it
   into `Capture-EnergySession.ps1`, and decide whether to also cross-check against
   a real y-cruncher VT3 (AVX-512) run for a larger, representative surge.
2. **Composition (§5.7):** C1 winner-take-all or C2 attenuated-additive, and over
   which stack? (Must include `midband_pressure`, the active term — not only
   `thermal_pressure`.)
3. **Producer change (§5.1/§9):** fork (shadow column alongside the validated 1 s
   producer) vs in-place cadence change; synchronous control-thread read vs
   dedicated sampler thread; and acceptance of the criterion-6 re-open at the
   un-mirrored cadence.
4. **A-priori bar (§8):** confirm the pre-registered go/no-go (must beat both
   fan-side latency *and* free `dTctl/dt`), given the thin-margin base rate.
5. **Band/cap numbers (§5.3/§5.4):** all $s_{\text{start}}$/$s_{\text{full}}$/
   $W_{\text{floor}}$/$B_{\max}$/rates are §8-derived, not invented.

## 11. What this document is not

Not a FEAT spec, not a decision record, not implementation permission. The
go/no-go in §8 is real, and the §1 base rate is honest: the exploitable head start
is the sub-second die→`Tctl` time constant, the smoothing needed to use it can
consume most of it, and the comparator is the free `dTctl/dt`, not blind `busy%`.
If the un-mirrored power-slope lead does not beat both the fan-side latency and the
free `dTctl/dt` pre-trigger by a useful margin on the captured spike sessions —
the likely outcome — **this variant parks too**, and `dTctl/dt` or the status quo
carries the transient response instead.
