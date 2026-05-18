"use strict";

/*
 * Node self-test for the control-theory analysis core in dashboard.js.
 * Run:  node selftest.js  [path-to-control-loop.csv]
 *
 * The renderers are DOM-only and guarded out under Node; this exercises the
 * pure math (resampling, cross-correlation lag, total variation, reversals,
 * hysteresis area, step detection/metrics) with synthetic signals plus a real
 * CSV smoke test.
 */

const fs = require("fs");
const path = require("path");
const D = require("./dashboard.js");

let passed = 0;
let failed = 0;

function ok(name, cond, detail) {
  if (cond) {
    passed += 1;
    console.log(`  PASS  ${name}`);
  } else {
    failed += 1;
    console.log(`  FAIL  ${name}${detail ? `  -> ${detail}` : ""}`);
  }
}

function near(a, b, tol) {
  return a !== null && b !== null && Number.isFinite(a) && Math.abs(a - b) <= tol;
}

function valueOrReason(value, formatter, reason) {
  return value === null || value === undefined ? reason : formatter(value);
}

console.log("\n[1] Primitive math");

ok("totalVariation([0,1,0,1]) == 3", D.totalVariation([0, 1, 0, 1]) === 3,
  String(D.totalVariation([0, 1, 0, 1])));

ok("reversals([0,1,0,1], 0.5) == 2", D.reversals([0, 1, 0, 1], 0.5) === 2,
  String(D.reversals([0, 1, 0, 1], 0.5)));

ok("reversals ignores sub-deadband jitter",
  D.reversals([0, 0.1, 0, 0.1, 0, 5, 0, 5], 0.5) === 2,
  String(D.reversals([0, 0.1, 0, 0.1, 0, 5, 0, 5], 0.5)));

const rg = D.resampleToGrid([[0, 0], [10, 10]], [0, 2.5, 5, 10, 12]);
ok("resampleToGrid linear interp + end clamp",
  rg[0] === 0 && rg[1] === 2.5 && rg[2] === 5 && rg[3] === 10 && rg[4] === 10,
  JSON.stringify(rg));

const square = [{ x: 0, y: 0 }, { x: 1, y: 0 }, { x: 1, y: 1 }, { x: 0, y: 1 }];
ok("hysteresisArea(unit square) == 1", D.hysteresisArea(square) === 1,
  String(D.hysteresisArea(square)));

ok("degSecondsOver basic",
  D.degSecondsOver([70, 80, 90], [1, 1, 1], 75) === (5 + 15),
  String(D.degSecondsOver([70, 80, 90], [1, 1, 1], 75)));

// Cross-correlation: b is a delayed by 5 samples => b follows a by +5.
const N = 400;
const a = Array.from({ length: N }, (_, i) => Math.sin(i / 9) + 0.3 * Math.sin(i / 3.1));
const L = 5;
const b = Array.from({ length: N }, (_, i) => a[Math.max(0, i - L)]);
const cc = D.crossCorrLag(a, b, 1, 20);
ok("crossCorrLag detects +5 sample follower lag",
  near(cc.lagSec, L, 1) && cc.corr > 0.8,
  `lagSec=${cc.lagSec} corr=${cc.corr && cc.corr.toFixed(3)}`);

// Step detection: flat 20%, jump to 60% at index 60.
const dt = 1;
const grid = Array.from({ length: 200 }, (_, i) => i * dt);
const ff = grid.map((_, i) => (i < 60 ? 20 : 60) + (i % 2 ? 0.05 : -0.05));
const steps = D.detectSteps(ff, grid, dt);
ok("detectSteps finds the one rising step near t=60",
  steps.length === 1 && Math.abs(steps[0].atSec - 60) <= 6,
  JSON.stringify(steps.map((s) => ({ at: s.atSec, mag: s.magFF.toFixed(1) }))));

if (steps.length === 1) {
  const sp = grid.map((_, i) => {
    if (i <= 60) return 20;
    return Math.min(60, 20 + (i - 60) * 4); // 10 s ramp to target
  });
  const temp = grid.map((_, i) => (i < 60 ? 50 : 50 + Math.min(20, (i - 60) * 0.8)));
  const m = D.stepMetrics(steps[0], { ff, sp, temp, fan: grid.map(() => 0) }, dt);
  // A windowed detector flags the step start a few samples before the actual
  // setpoint move, so dead time legitimately includes that lead (<= window+pre).
  ok("stepMetrics: dead time bounded, rise ~8s, excursion > 5C",
    m.deadSec !== null && m.deadSec <= 8 && near(m.rise, 8, 4) && m.tempExcursion > 5,
    `dead=${m.deadSec} rise=${m.rise} exc=${m.tempExcursion}`);
}

console.log("\n[2] Real CSV smoke test");

const csvArg = process.argv[2]
  || path.join(__dirname, "..", "..", "release", "runtime", "logs", "svg_mb_control_output.csv");

if (!fs.existsSync(csvArg)) {
  console.log(`  SKIP  no CSV at ${csvArg}`);
} else {
  const raw = fs.readFileSync(csvArg, "utf8");
  const lines = raw.split(/\r?\n/);
  // Keep comment/header lines + a representative slice of data rows.
  const head = lines.slice(0, 4);
  const body = lines.slice(4, 4 + 18000);
  const t0 = Date.now();
  const rows = D.parseCsv(head.concat(body).join("\n"));
  D.state.rows = rows;
  D.state.events = [];
  const model = D.buildModel({ threshold: 70, focusChannel: null });
  const ms = Date.now() - t0;

  ok("parsed rows", rows.length > 1000, `${rows.length} rows`);
  ok("channels detected", model.channels.length >= 1, JSON.stringify(model.channels));
  ok("uniform grid built", model.grid.length > 10 && Number.isFinite(model.dtSec),
    `grid=${model.grid.length} dt=${model.dtSec && model.dtSec.toFixed(3)}s`);
  ok("quality row per channel", model.quality.length === model.channels.length,
    `${model.quality.length} vs ${model.channels.length}`);
  ok("focus detail present",
    !!model.focus && model.focus.phase.traj.length > 0 && model.focus.phase.ref.length > 0
      && model.focus.decomp.points.length > 0,
    model.focus ? `traj=${model.focus.phase.traj.length} ref=${model.focus.phase.ref.length} dec=${model.focus.decomp.points.length}` : "no focus");
  const q = model.quality.every((x) =>
    (x.ctrlLag === null || Number.isFinite(x.ctrlLag)) &&
    (x.trackRms === null || Number.isFinite(x.trackRms)) &&
    (x.hyst === null || Number.isFinite(x.hyst)));
  ok("quality metrics finite-or-null (no NaN leaks)", q);
  ok("buildModel under 4 s on 18k rows", ms < 4000, `${ms} ms`);

  console.log(`\n  Focus channel: ${model.focus && model.focus.channel}`);
  console.log("  ch | ctrlLag | fanLag | trackRMS | effort/min | rev/min | hystArea | boost%");
  model.quality.forEach((x) => {
    console.log(
      `  ${String(x.channel).padStart(2)} | `
      + `${valueOrReason(x.ctrlLag, (v) => v.toFixed(1) + "s", "not detected").padStart(12)} | `
      + `${valueOrReason(x.fanLag, (v) => v.toFixed(1) + "s", "not detected").padStart(12)} | `
      + `${valueOrReason(x.trackRms, (v) => v.toFixed(2), "missing").padStart(8)} | `
      + `${valueOrReason(x.effortPerMin, (v) => v.toFixed(1), "short run").padStart(10)} | `
      + `${valueOrReason(x.revPerMin, (v) => v.toFixed(1), "short").padStart(7)} | `
      + `${valueOrReason(x.hyst, (v) => String(Math.round(v)), "missing").padStart(8)} | `
      + `${valueOrReason(x.boostShare, (v) => (v * 100).toFixed(0) + "%", "no boost data")}`,
    );
  });
  const st = model.focus ? model.focus.steps : [];
  console.log(`\n  Disturbance steps detected (focus ch): ${st.length}`);
  st.forEach((s, i) => console.log(
    `   #${i + 1} at ${s.atSec.toFixed(0)}s  dFF=+${s.magFF.toFixed(1)}%  `
    + `dead=${valueOrReason(s.deadSec, (v) => v.toFixed(1) + "s", "not detected")}  `
    + `rise=${valueOrReason(s.rise, (v) => v.toFixed(1) + "s", "not settled")}  `
    + `tempPeak=${valueOrReason(s.tempExcursion, (v) => v.toFixed(1) + "C", "missing temp")}  `
    + `settle=${valueOrReason(s.settleSec, (v) => v.toFixed(0) + "s", "not settled")}`,
  ));
}

console.log("\n[3] Spectral + Pareto");

const ramp = Array.from({ length: 50 }, (_, i) => 3 + 2 * i);
const dtr = D.detrend(ramp);
ok("detrend removes a linear trend", Math.max(...dtr.map(Math.abs)) < 1e-6,
  String(Math.max(...dtr.map(Math.abs))));

const M = 64;
const k0 = 8;
const fre = new Float64Array(M);
const fim = new Float64Array(M);
for (let i = 0; i < M; i += 1) {
  fre[i] = Math.cos((2 * Math.PI * k0 * i) / M);
}
D.fft(fre, fim);
let pk = -1;
let pv = -1;
for (let k = 1; k <= M / 2; k += 1) {
  const mag = fre[k] * fre[k] + fim[k] * fim[k];
  if (mag > pv) {
    pv = mag;
    pk = k;
  }
}
ok("fft puts a pure cosine's energy in the right bin", pk === k0, `peak bin ${pk}, expected ${k0}`);

const P = 10;
const NS = 1024;
const sig = Array.from({ length: NS }, (_, i) => 2 * Math.sin((2 * Math.PI * i) / P) + 0.02 * Math.sin(i));
const psd = D.welchPSD(sig, 1);
const dom = D.dominantPeriod(psd, NS);
ok("welchPSD + dominantPeriod recover a 10 s oscillation",
  psd !== null && near(dom.periodSec, 10, 1.5) && dom.oscIndex > 5,
  `period=${dom.periodSec && dom.periodSec.toFixed(2)} oscIdx=${dom.oscIndex && dom.oscIndex.toFixed(1)}`);

const spec = D.spectrogram(sig, 1);
ok("spectrogram returns a time x period matrix",
  !!spec && spec.frames.length > 1 && spec.periods.length > 1,
  spec ? `frames=${spec.frames.length} bins=${spec.periods.length}` : "null");

const pts = [
  { name: "A", effortPerMin: 1, exceed: 10 },
  { name: "B", effortPerMin: 2, exceed: 2 },
  { name: "C", effortPerMin: 3, exceed: 20 },
];
const fr = D.paretoFrontier(pts).filter((p) => p.frontier).map((p) => p.name).sort();
ok("paretoFrontier keeps only non-dominated runs",
  JSON.stringify(fr) === JSON.stringify(["A", "B"]), JSON.stringify(fr));

if (D.state.rows.length) {
  const m2 = D.buildModel({ threshold: 70, focusChannel: null });
  ok("focus.spectral present on the real run",
    !!(m2.focus && m2.focus.spectral && m2.focus.spectral.psd && m2.focus.spectral.spec),
    m2.focus ? `psd=${!!m2.focus.spectral.psd} spec=${!!m2.focus.spectral.spec}` : "no focus");
  const b = D.runScalarBundle(D.state.rows, 70, "real");
  ok("runScalarBundle yields finite effort + exceedance",
    Number.isFinite(b.effortPerMin) && Number.isFinite(b.exceed),
    `effort=${b.effortPerMin && b.effortPerMin.toFixed(1)} exceed=${b.exceed && b.exceed.toFixed(0)} fanLag=${b.fanLag}`);
}

console.log("\n[4] Runtime health summary");

ok("summarizeHealth(null) -> unknown/muted",
  (() => { const s = D.summarizeHealth(null); return s.state === "unknown" && s.cls === "muted"; })(),
  JSON.stringify(D.summarizeHealth(null)));

ok("summarizeHealth({available:false}) -> unknown",
  D.summarizeHealth({ available: false }).state === "unknown");

const healthy = D.summarizeHealth({
  available: true,
  health: { last_health_state: "healthy", last_health_reason: "fresh", last_health_time: "2026-05-18T15:00:00" },
  supervisor: { supervisor_pid: 111, worker_restart_count: 0, last_worker_exit_code: 0, last_worker_exit_time: "2026-05-18T14:59:00" },
  runtime: { mode: "control-loop", process_id: 222, last_successful_restore_time: "2026-05-18T14:58:00" },
});
ok("healthy -> ok class + reason passthrough",
  healthy.state === "healthy" && healthy.cls === "ok" && healthy.reason === "fresh",
  JSON.stringify(healthy));
ok("healthy detail carries mode/pids and exit code 0",
  healthy.detail.some((d) => d.k === "mode" && d.v === "control-loop")
  && healthy.detail.some((d) => d.k === "supervisor pid" && d.v === "111")
  && healthy.detail.some((d) => d.k === "last worker exit" && d.v.startsWith("0")),
  JSON.stringify(healthy.detail));

ok("degraded -> warn", D.summarizeHealth({ available: true, health: { last_health_state: "degraded" } }).cls === "warn");
ok("stale -> warn", D.summarizeHealth({ available: true, health: { last_health_state: "stale" } }).cls === "warn");
ok("stopped -> muted", D.summarizeHealth({ available: true, health: { last_health_state: "stopped" } }).cls === "muted");
ok("failed -> bad", D.summarizeHealth({ available: true, health: { last_health_state: "failed" } }).cls === "bad");
ok("unknown state string -> muted, label preserved",
  (() => { const s = D.summarizeHealth({ available: true, health: { last_health_state: "weird" } }); return s.cls === "muted" && s.label === "weird"; })());

console.log(`\n${failed === 0 ? "ALL GREEN" : "FAILURES"}: ${passed} passed, ${failed} failed\n`);
process.exit(failed === 0 ? 0 : 1);
