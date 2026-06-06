"use strict";

const CHANNEL_SETPOINT_RE = /^channel(\d+)_setpoint_pct$/;
const ISSUE_EVENTS = new Set([
  "control_loop.policy_refused",
  "control_loop.write_failed",
  "control_loop.sensor_failure",
  "control_loop.circuit_breaker_opened",
  "runtime.sidecar_warning",
]);
const LIVE_CSV_URL = "/api/live-tail.csv?bytes=8000000";
const LIVE_EVENTS_URL = "/api/events-tail.jsonl?bytes=2000000";
const LIVE_HEALTH_URL = "/api/health.json";
const HEALTH_POLL_MS = 5000;

const state = {
  rows: [],
  events: [],
  summary: null,
  history: [],
  compare: [],
  csvName: "",
  summaryName: "",
  focusChannel: "",
  live: {
    on: false,
    autoRefresh: true,
    loadedOnce: false,
    loading: false,
    generation: 0,
    csvSignature: null,
    eventsSignature: null,
    baseUrl: "../../release/runtime",
    intervalS: 2,
    cap: 2400,
    rows: [],
    prevTick: null,
    lastAppendAt: 0,
    status: "idle",
    reason: "not started",
    ageMs: null,
    lastRt: null,
    lastSt: null,
    lastErr: "",
    timer: null,
    csvBackup: null,
    csvNameBackup: "",
  },
};

function readFile(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result || ""));
    reader.onerror = () => reject(reader.error);
    reader.readAsText(file);
  });
}

async function fetchTextIfOk(url) {
  const separator = url.includes("?") ? "&" : "?";
  const response = await fetch(`${url}${separator}t=${Date.now()}`, { cache: "no-store" });
  if (!response.ok) {
    throw new Error(`${response.status} ${response.statusText}`);
  }
  return response.text();
}

// CSV row/quote parsing and the "#"-prologue skip mirror the native producer
// src/runtime/runtime_csv_archive.cpp; the prologue grammar is documented in
// docs/RUNTIME_LOGGING_AND_EVALUATION.md. Keep aligned with the native
// analyzer's ParseCsvLine in src/analyze/analyze_csv.cpp.
function parseCsv(text) {
  const content = text
    .split(/\r?\n/)
    .filter((line) => line.trim() && !line.startsWith("#"))
    .join("\n");
  if (!content.trim()) {
    return [];
  }

  const records = [];
  let row = [];
  let field = "";
  let quoted = false;

  for (let i = 0; i < content.length; i += 1) {
    const ch = content[i];
    if (quoted) {
      if (ch === '"' && content[i + 1] === '"') {
        field += '"';
        i += 1;
      } else if (ch === '"') {
        quoted = false;
      } else {
        field += ch;
      }
      continue;
    }

    if (ch === '"') {
      quoted = true;
    } else if (ch === ",") {
      row.push(field);
      field = "";
    } else if (ch === "\n") {
      row.push(field);
      records.push(row);
      row = [];
      field = "";
    } else if (ch !== "\r") {
      field += ch;
    }
  }
  row.push(field);
  records.push(row);

  const header = records.shift() || [];
  return records
    .filter((record) => record.some((value) => value !== ""))
    .map((record) => {
      const out = {};
      header.forEach((name, index) => {
        out[name] = record[index] ?? "";
      });
      return out;
    });
}

function parseJsonl(text) {
  return text
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean)
    .map((line) => {
      try {
        return JSON.parse(line);
      } catch {
        return { event_type: "invalid_json", detail: line };
      }
    });
}

function maybeNumber(value) {
  if (value === null || value === undefined || value === "") {
    return null;
  }
  const parsed = Number(value);
  return Number.isFinite(parsed) ? parsed : null;
}

function hasNumber(value) {
  return value !== null && value !== undefined && Number.isFinite(value);
}

function formatNumber(value, digits = 1, fallback = "No data") {
  if (value === null || value === undefined || !Number.isFinite(value)) {
    return fallback;
  }
  return value.toFixed(digits);
}

function formatInteger(value, fallback = "No data") {
  if (value === null || value === undefined || !Number.isFinite(value)) {
    return fallback;
  }
  return Math.round(value).toLocaleString();
}

function formatSeconds(value, fallback = "No time data") {
  if (value === null || value === undefined || !Number.isFinite(value)) {
    return fallback;
  }
  if (value >= 600) {
    return `${(value / 60).toFixed(1)} min`;
  }
  return `${value.toFixed(1)} s`;
}

function formatUnit(value, unit, digits = 1, fallback = "No data") {
  return hasNumber(value) ? `${formatNumber(value, digits)} ${unit}` : fallback;
}

function formatPct(value, digits = 1, fallback = "No setpoint") {
  return hasNumber(value) ? `${formatNumber(value, digits)}%` : fallback;
}

function formatSignedPct(value, digits = 1, fallback = "missing demand") {
  if (!hasNumber(value)) {
    return fallback;
  }
  const prefix = value >= 0 ? "+" : "";
  return `${prefix}${formatNumber(value, digits)}%`;
}

function percentile(values, pct) {
  if (!values.length) {
    return null;
  }
  return percentileSorted([...values].sort((a, b) => a - b), pct);
}

function percentileSorted(sorted, pct) {
  // Nearest-rank on an ascending-sorted array: pX is the value at index
  // round((X/100) * (n - 1)); p100 is the maximum. Math.floor(x + 0.5)
  // reproduces C++ std::llround (round half away from zero) for the
  // non-negative index, matching the native analyze report
  // (src/analyze/analyze_report_data.cpp).
  if (!sorted.length) {
    return null;
  }
  let index = Math.floor((sorted.length - 1) * (pct / 100) + 0.5);
  if (index >= sorted.length) {
    index = sorted.length - 1;
  }
  return sorted[index];
}

function stats(values) {
  const clean = [];
  let sum = 0;
  for (const value of values) {
    if (Number.isFinite(value)) {
      clean.push(value);
      sum += value;
    }
  }
  if (!clean.length) {
    return { count: 0, min: null, p50: null, p90: null, p95: null, p99: null, max: null, avg: null };
  }
  clean.sort((a, b) => a - b);
  return {
    count: clean.length,
    min: clean[0],
    p50: percentileSorted(clean, 50),
    p90: percentileSorted(clean, 90),
    p95: percentileSorted(clean, 95),
    p99: percentileSorted(clean, 99),
    max: clean[clean.length - 1],
    avg: sum / clean.length,
  };
}

function capLiveRows(rows) {
  const cap = Math.floor(Number(state.live.cap));
  if (!Number.isFinite(cap) || cap <= 0 || rows.length <= cap) {
    return rows;
  }
  return rows.slice(-cap);
}

function gpuEnvelope(row) {
  const explicit = maybeNumber(row.gpu_envelope_c);
  if (explicit !== null) {
    return explicit;
  }
  const values = [maybeNumber(row.gpu_core_c), maybeNumber(row.gpu_memjn_c)]
    .filter((value) => value !== null);
  const hotspot = maybeNumber(row.gpu_hotspot_c);
  if (hotspot !== null && hotspot > 0) {
    values.push(hotspot);
  }
  return values.length ? Math.max(...values) : null;
}

function rowIntervalSeconds(row) {
  const value = maybeNumber(row.loop_achieved_interval_ms);
  if (value === null || value < 0) {
    return null;
  }
  return value / 1000;
}

function elapsedSeconds(rows) {
  const intervals = rows.map(rowIntervalSeconds);
  if (intervals.some((value) => value !== null)) {
    const elapsed = [];
    let current = 0;
    intervals.forEach((interval) => {
      elapsed.push(current);
      if (interval !== null) {
        current += interval;
      }
    });
    return elapsed;
  }

  const wallTimes = rows.map((row) => {
    const raw = row.loop_started_wall_clock || row.wall_clock;
    const parsed = raw ? Date.parse(raw) : Number.NaN;
    return Number.isFinite(parsed) ? parsed : null;
  });
  const base = wallTimes.find((value) => value !== null);
  if (base === undefined) {
    return rows.map(() => null);
  }
  return wallTimes.map((value) => value === null ? null : (value - base) / 1000);
}

function detectChannels(rows) {
  const names = rows[0] ? Object.keys(rows[0]) : [];
  return names
    .map((name) => CHANNEL_SETPOINT_RE.exec(name))
    .filter(Boolean)
    .map((match) => Number(match[1]))
    .sort((a, b) => a - b);
}

function column(rows, name) {
  return rows.map((row) => maybeNumber(row[name])).filter((value) => value !== null);
}

// Boost component column names come from the CSV header producer
// src/runtime/runtime_csv_rows.cpp (kBoostStageSpecs); the low_band_* fallback
// is emitted only there. Keep in sync with that producer and native
// tick_channel_samples ingestion.
function channelResponseBoost(row, channel) {
  const names = [
    `channel${channel}_thermal_pressure_boost_pct`,
    `channel${channel}_midband_pressure_boost_pct`,
    `channel${channel}_gpu_airflow_boost_pct`,
    `channel${channel}_cpu_low_soak_boost_pct`,
  ];
  let total = 0;
  let seen = false;
  names.forEach((name) => {
    const value = maybeNumber(row[name]);
    if (value !== null) {
      total += value;
      seen = true;
    }
  });
  const effectiveLowBand = maybeNumber(row[`channel${channel}_low_band_effective_boost_pct`]);
  const rawLowBand = maybeNumber(row[`channel${channel}_low_band_stage_boost_pct`]);
  const lowBand = effectiveLowBand !== null ? effectiveLowBand : rawLowBand;
  if (lowBand !== null) {
    total += lowBand;
    seen = true;
  }
  return seen ? total : null;
}

function eventCounts(events) {
  const counts = new Map();
  events.forEach((event) => {
    const type = String(event.event_type || "unknown");
    counts.set(type, (counts.get(type) || 0) + 1);
  });
  return [...counts.entries()].sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]));
}

/* ----------------------------------------------------------------------------
 * Control-theory analysis core (pure, DOM-free, Node-testable)
 *
 * A fan controller is a disturbance regulator: the configured curve is a static
 * reference manifold, feedforward_pct is that curve at the live temperature
 * (instantaneous demand), and setpoint_pct is what the dynamic layer (EWMA
 * smoothing + decay-latch + response boosts + slew limit) delivered.
 * correction_pct = setpoint - feedforward is the whole dynamic signature.
 * ------------------------------------------------------------------------- */

function finitePairs(xs, ys) {
  const out = [];
  for (let i = 0; i < xs.length; i += 1) {
    const x = xs[i];
    const y = ys[i];
    if (Number.isFinite(x) && Number.isFinite(y)) {
      out.push([x, y]);
    }
  }
  return out;
}

function resampleToGrid(pairs, grid) {
  const out = new Array(grid.length).fill(null);
  if (!pairs.length) {
    return out;
  }
  let j = 0;
  for (let k = 0; k < grid.length; k += 1) {
    const gx = grid[k];
    if (gx <= pairs[0][0]) {
      out[k] = pairs[0][1];
      continue;
    }
    if (gx >= pairs[pairs.length - 1][0]) {
      out[k] = pairs[pairs.length - 1][1];
      continue;
    }
    while (j < pairs.length - 1 && pairs[j + 1][0] < gx) {
      j += 1;
    }
    const [x0, y0] = pairs[j];
    const [x1, y1] = pairs[Math.min(j + 1, pairs.length - 1)];
    out[k] = x1 === x0 ? y0 : y0 + (y1 - y0) * ((gx - x0) / (x1 - x0));
  }
  return out;
}

function mean(values) {
  let sum = 0;
  let n = 0;
  for (const v of values) {
    if (Number.isFinite(v)) {
      sum += v;
      n += 1;
    }
  }
  return n ? sum / n : null;
}

function robustSigma(values) {
  const clean = values.filter(Number.isFinite);
  if (clean.length < 2) {
    return 0;
  }
  const med = percentile(clean, 50);
  const dev = clean.map((v) => Math.abs(v - med));
  return 1.4826 * percentile(dev, 50);
}

// Subtract the least-squares line so spectra reflect oscillation, not drift/DC.
function detrend(values) {
  const n = values.length;
  let sx = 0;
  let sy = 0;
  let sxx = 0;
  let sxy = 0;
  let m = 0;
  for (let i = 0; i < n; i += 1) {
    if (!Number.isFinite(values[i])) {
      continue;
    }
    sx += i;
    sy += values[i];
    sxx += i * i;
    sxy += i * values[i];
    m += 1;
  }
  if (m < 2) {
    return values.map((v) => (Number.isFinite(v) ? 0 : 0));
  }
  const denom = m * sxx - sx * sx;
  const slope = denom !== 0 ? (m * sxy - sx * sy) / denom : 0;
  const intercept = (sy - slope * sx) / m;
  return values.map((v, i) => (Number.isFinite(v) ? v - (intercept + slope * i) : 0));
}

function crossCorrLag(a, b, dtSec, maxLagSec) {
  const n = Math.min(a.length, b.length);
  const ma = mean(a.slice(0, n));
  const mb = mean(b.slice(0, n));
  if (ma === null || mb === null) {
    return { lagSec: null, corr: null };
  }
  const da = new Array(n);
  const db = new Array(n);
  let va = 0;
  let vb = 0;
  for (let i = 0; i < n; i += 1) {
    da[i] = Number.isFinite(a[i]) ? a[i] - ma : 0;
    db[i] = Number.isFinite(b[i]) ? b[i] - mb : 0;
    va += da[i] * da[i];
    vb += db[i] * db[i];
  }
  if (va <= 0 || vb <= 0) {
    return { lagSec: null, corr: null };
  }
  const norm = Math.sqrt(va * vb);
  const maxLag = Math.max(1, Math.round(maxLagSec / dtSec));
  let best = { lag: 0, corr: -Infinity };
  for (let lag = -maxLag; lag <= maxLag; lag += 1) {
    let acc = 0;
    for (let i = 0; i < n; i += 1) {
      const j = i + lag; // positive lag => b follows a by `lag` samples
      if (j >= 0 && j < n) {
        acc += da[i] * db[j];
      }
    }
    const c = acc / norm;
    if (c > best.corr) {
      best = { lag, corr: c };
    }
  }
  return { lagSec: best.lag * dtSec, corr: best.corr };
}

function totalVariation(values) {
  let tv = 0;
  let prev = null;
  for (const v of values) {
    if (!Number.isFinite(v)) {
      continue;
    }
    if (prev !== null) {
      tv += Math.abs(v - prev);
    }
    prev = v;
  }
  return tv;
}

function reversals(values, eps) {
  let count = 0;
  let prev = null;
  let dir = 0;
  for (const v of values) {
    if (!Number.isFinite(v)) {
      continue;
    }
    if (prev !== null) {
      const d = v - prev;
      if (Math.abs(d) >= eps) {
        const nd = d > 0 ? 1 : -1;
        if (dir !== 0 && nd !== dir) {
          count += 1;
        }
        dir = nd;
      }
    }
    prev = v;
  }
  return count;
}

function hysteresisArea(points) {
  if (points.length < 3) {
    return 0;
  }
  let area = 0;
  for (let i = 0; i < points.length; i += 1) {
    const a = points[i];
    const b = points[(i + 1) % points.length];
    area += a.x * b.y - b.x * a.y;
  }
  return Math.abs(area) / 2;
}

function degSecondsOver(values, intervalsSec, limit) {
  if (!Number.isFinite(limit)) {
    return null;
  }
  let acc = 0;
  for (let i = 0; i < values.length; i += 1) {
    const v = values[i];
    const dt = intervalsSec[i];
    if (Number.isFinite(v) && Number.isFinite(dt) && v > limit) {
      acc += (v - limit) * dt;
    }
  }
  return acc;
}

function timeAtOrAbove(values, intervalsSec, limit) {
  if (!Number.isFinite(limit)) {
    return { rows: 0, seconds: 0, firstSec: null };
  }
  let rows = 0;
  let seconds = 0;
  let firstSec = null;
  let elapsed = 0;
  for (let i = 0; i < values.length; i += 1) {
    const v = values[i];
    const dt = Number.isFinite(intervalsSec[i]) ? intervalsSec[i] : 0;
    if (Number.isFinite(v) && v >= limit) {
      rows += 1;
      seconds += dt;
      if (firstSec === null) {
        firstSec = elapsed;
      }
    }
    elapsed += dt;
  }
  return { rows, seconds, firstSec };
}

// ---- Spectral analysis ----------------------------------------------------

function prevPow2(n) {
  let p = 1;
  while (p * 2 <= n) {
    p *= 2;
  }
  return p;
}

// In-place iterative radix-2 Cooley-Tukey FFT (re/im length must be pow2).
function fft(re, im) {
  const n = re.length;
  for (let i = 1, j = 0; i < n; i += 1) {
    let bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      [re[i], re[j]] = [re[j], re[i]];
      [im[i], im[j]] = [im[j], im[i]];
    }
  }
  for (let len = 2; len <= n; len <<= 1) {
    const ang = (-2 * Math.PI) / len;
    const wr = Math.cos(ang);
    const wi = Math.sin(ang);
    for (let i = 0; i < n; i += len) {
      let cr = 1;
      let ci = 0;
      for (let k = 0; k < len / 2; k += 1) {
        const ar = re[i + k];
        const ai = im[i + k];
        const br = re[i + k + len / 2] * cr - im[i + k + len / 2] * ci;
        const bi = re[i + k + len / 2] * ci + im[i + k + len / 2] * cr;
        re[i + k] = ar + br;
        im[i + k] = ai + bi;
        re[i + k + len / 2] = ar - br;
        im[i + k + len / 2] = ai - bi;
        const ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
}

function hannWindow(n) {
  const w = new Float64Array(n);
  for (let i = 0; i < n; i += 1) {
    w[i] = 0.5 - 0.5 * Math.cos((2 * Math.PI * i) / (n - 1));
  }
  return w;
}

// Welch PSD: detrended, Hann-windowed, 50%-overlap, averaged periodograms.
function welchPSD(signal, fs) {
  const x = detrend(signal.filter(Number.isFinite));
  const n = x.length;
  if (n < 16 || !Number.isFinite(fs) || fs <= 0) {
    return null;
  }
  const seg = Math.min(256, prevPow2(Math.max(16, Math.floor(n / 2))));
  if (seg < 16) {
    return null;
  }
  const hop = seg / 2;
  const w = hannWindow(seg);
  let winPow = 0;
  for (let i = 0; i < seg; i += 1) {
    winPow += w[i] * w[i];
  }
  const half = seg / 2;
  const acc = new Float64Array(half);
  let segments = 0;
  for (let start = 0; start + seg <= n; start += hop) {
    const re = new Float64Array(seg);
    const im = new Float64Array(seg);
    for (let i = 0; i < seg; i += 1) {
      re[i] = x[start + i] * w[i];
    }
    fft(re, im);
    for (let k = 1; k < half; k += 1) {
      acc[k] += re[k] * re[k] + im[k] * im[k];
    }
    segments += 1;
  }
  if (segments === 0) {
    return null;
  }
  const freqs = [];
  const psd = [];
  for (let k = 1; k < half; k += 1) {
    freqs.push((k * fs) / seg);
    psd.push((2 * acc[k]) / (segments * fs * winPow));
  }
  return { freqs, psd, seg, segments };
}

function dominantPeriod(psdResult, recordSeconds) {
  if (!psdResult) {
    return { periodSec: null, oscIndex: null };
  }
  const { freqs, psd } = psdResult;
  const maxPeriod = Math.max(4, 0.45 * recordSeconds);
  let bestK = -1;
  let bestV = -Infinity;
  for (let k = 0; k < psd.length; k += 1) {
    const period = 1 / freqs[k];
    if (period < 2 || period > maxPeriod) {
      continue;
    }
    if (psd[k] > bestV) {
      bestV = psd[k];
      bestK = k;
    }
  }
  if (bestK < 0) {
    return { periodSec: null, oscIndex: null };
  }
  const med = percentile(psd.filter((v) => v > 0), 50) || 1e-12;
  return { periodSec: 1 / freqs[bestK], oscIndex: bestV / med };
}

// STFT magnitude matrix (dB, robustly normalised to 0..1) for the spectrogram.
function spectrogram(signal, fs) {
  const x = detrend(signal.filter(Number.isFinite));
  const n = x.length;
  if (n < 32 || !Number.isFinite(fs) || fs <= 0) {
    return null;
  }
  const seg = Math.min(128, prevPow2(Math.max(16, Math.floor(n / 8))));
  if (seg < 16) {
    return null;
  }
  const hop = Math.max(1, Math.floor(seg / 4));
  const w = hannWindow(seg);
  const half = seg / 2;
  const times = [];
  const frames = [];
  let vmin = Infinity;
  let vmax = -Infinity;
  for (let start = 0; start + seg <= n; start += hop) {
    const re = new Float64Array(seg);
    const im = new Float64Array(seg);
    for (let i = 0; i < seg; i += 1) {
      re[i] = x[start + i] * w[i];
    }
    fft(re, im);
    const col = new Float64Array(half - 1);
    for (let k = 1; k < half; k += 1) {
      const p = re[k] * re[k] + im[k] * im[k];
      const db = 10 * Math.log10(p + 1e-12);
      col[k - 1] = db;
      if (db < vmin) vmin = db;
      if (db > vmax) vmax = db;
    }
    frames.push(col);
    times.push((start + seg / 2) / fs);
  }
  if (!frames.length) {
    return null;
  }
  const all = [];
  frames.forEach((c) => c.forEach((v) => all.push(v)));
  const lo = percentile(all, 5);
  const hi = percentile(all, 99);
  const periods = [];
  for (let k = 1; k < half; k += 1) {
    periods.push(seg / (k * fs));
  }
  return { times, periods, frames, lo, hi, vmin, vmax, seg };
}

function detectSteps(ff, grid, dtSec) {
  const win = Math.max(1, Math.round(4 / dtSec));
  const diffs = [];
  for (let i = 0; i + win < ff.length; i += 1) {
    if (Number.isFinite(ff[i]) && Number.isFinite(ff[i + win])) {
      diffs.push(ff[i + win] - ff[i]);
    }
  }
  const sigma = robustSigma(diffs);
  const thresh = Math.max(3, 3.5 * sigma);
  const candidates = [];
  for (let i = 0; i + win < ff.length; i += 1) {
    if (!Number.isFinite(ff[i]) || !Number.isFinite(ff[i + win])) {
      continue;
    }
    const d = ff[i + win] - ff[i];
    if (Math.abs(d) >= thresh) {
      candidates.push({ i, mag: d });
    }
  }
  const minSep = Math.max(1, Math.round(25 / dtSec));
  candidates.sort((a, b) => Math.abs(b.mag) - Math.abs(a.mag));
  const chosen = [];
  for (const c of candidates) {
    if (chosen.every((s) => Math.abs(s.i - c.i) >= minSep)) {
      chosen.push(c);
    }
    if (chosen.length >= 8) {
      break;
    }
  }
  return chosen
    .filter((s) => s.mag > 0)
    .map((s) => ({ index: s.i, atSec: grid[s.i], magFF: s.mag }))
    .sort((a, b) => a.atSec - b.atSec);
}

function stepMetrics(step, grids, dtSec) {
  const { ff, sp, temp, fan } = grids;
  const n = sp.length;
  const i0 = step.index;
  const pre = Math.max(1, Math.round(6 / dtSec));
  const post = Math.max(2, Math.round(60 / dtSec));
  const lo = Math.max(0, i0 - pre);
  const hi = Math.min(n - 1, i0 + post);

  const spBase = mean(sp.slice(Math.max(0, i0 - 3), i0 + 1));
  const spEnd = mean(sp.slice(Math.max(lo, hi - pre), hi + 1));
  const dSp = spEnd !== null && spBase !== null ? spEnd - spBase : null;

  let deadSec = null;
  if (spBase !== null && dSp !== null) {
    const trig = Math.max(0.5, 0.08 * Math.abs(dSp));
    for (let j = i0; j <= hi; j += 1) {
      if (Number.isFinite(sp[j]) && Math.abs(sp[j] - spBase) > trig) {
        deadSec = (j - i0) * dtSec;
        break;
      }
    }
  }

  let rise = null;
  if (spBase !== null && dSp !== null && Math.abs(dSp) > 1) {
    const lvl = (frac) => spBase + frac * dSp;
    let t10 = null;
    let t90 = null;
    for (let j = i0; j <= hi; j += 1) {
      const v = sp[j];
      if (!Number.isFinite(v)) {
        continue;
      }
      const reached = dSp > 0 ? v >= lvl(0.1) : v <= lvl(0.1);
      const reached90 = dSp > 0 ? v >= lvl(0.9) : v <= lvl(0.9);
      if (t10 === null && reached) {
        t10 = j;
      }
      if (t10 !== null && reached90) {
        t90 = j;
        break;
      }
    }
    if (t10 !== null && t90 !== null) {
      rise = (t90 - t10) * dtSec;
    }
  }

  const tempBase = Number.isFinite(temp[i0]) ? temp[i0] : mean(temp.slice(lo, i0 + 1));
  let tempPeak = null;
  for (let j = i0; j <= hi; j += 1) {
    if (Number.isFinite(temp[j]) && (tempPeak === null || temp[j] > tempPeak)) {
      tempPeak = temp[j];
    }
  }
  const tempExcursion = tempPeak !== null && tempBase !== null ? tempPeak - tempBase : null;

  let settleSec = null;
  const tempEnd = mean(temp.slice(Math.max(lo, hi - pre), hi + 1));
  if (tempEnd !== null && tempExcursion !== null && Math.abs(tempExcursion) > 0.5) {
    const band = Math.max(0.8, 0.1 * Math.abs(tempExcursion));
    let lastOut = i0;
    for (let j = i0; j <= hi; j += 1) {
      if (Number.isFinite(temp[j]) && Math.abs(temp[j] - tempEnd) > band) {
        lastOut = j;
      }
    }
    settleSec = (lastOut - i0) * dtSec;
  }

  const resid = Number.isFinite(ff[hi]) && Number.isFinite(sp[hi]) ? ff[hi] - sp[hi] : null;

  const rel = [];
  for (let j = lo; j <= hi; j += 1) {
    rel.push({
      x: (j - i0) * dtSec,
      sp: Number.isFinite(sp[j]) && spBase !== null ? sp[j] - spBase : null,
      fan: Number.isFinite(fan[j]) ? fan[j] : null,
    });
  }

  return { atSec: step.atSec, magFF: step.magFF, dSp, deadSec, rise, tempExcursion, settleSec, resid, rel };
}

function channelGrids(rows, elapsed, ch, grid) {
  const xs = elapsed;
  const pick = (name) => resampleToGrid(
    finitePairs(xs, rows.map((r) => maybeNumber(r[name]))),
    grid,
  );
  return {
    ff: pick(`channel${ch}_feedforward_pct`),
    sp: pick(`channel${ch}_setpoint_pct`),
    temp: pick(`channel${ch}_observed_temp_c`),
    boost: resampleToGrid(
      finitePairs(xs, rows.map((r) => channelResponseBoost(r, ch))),
      grid,
    ),
    fan: pick(`fan${ch}_rpm`),
  };
}

function decimate(arr, maxPoints) {
  if (arr.length <= maxPoints) {
    return arr;
  }
  const stride = Math.ceil(arr.length / maxPoints);
  const out = [];
  for (let i = 0; i < arr.length; i += stride) {
    out.push(arr[i]);
  }
  if (out[out.length - 1] !== arr[arr.length - 1]) {
    out.push(arr[arr.length - 1]);
  }
  return out;
}

function channelQuality(rows, elapsed, ch, grid, dtSec, durationMin) {
  const g = channelGrids(rows, elapsed, ch, grid);
  const ctrl = crossCorrLag(g.ff, g.sp, dtSec, 30);
  const fan = crossCorrLag(g.sp, g.fan, dtSec, 30);

  let sse = 0;
  let nn = 0;
  for (let i = 0; i < g.sp.length; i += 1) {
    if (Number.isFinite(g.sp[i]) && Number.isFinite(g.ff[i])) {
      const e = g.sp[i] - g.ff[i];
      sse += e * e;
      nn += 1;
    }
  }
  const trackRms = nn ? Math.sqrt(sse / nn) : null;

  const spNative = rows.map((r) => maybeNumber(r[`channel${ch}_setpoint_pct`]));
  const tv = totalVariation(spNative);
  const effortPerMin = durationMin && durationMin > 0 ? tv / durationMin : null;
  const revPerMin = durationMin && durationMin > 0 ? reversals(g.sp, 0.25) / durationMin : null;

  const td = decimate(g.temp, 4000);
  const sd = decimate(g.sp, 4000);
  const traj = [];
  for (let i = 0; i < td.length; i += 1) {
    if (Number.isFinite(td[i]) && Number.isFinite(sd[i])) {
      traj.push({ x: td[i], y: sd[i] });
    }
  }
  const hyst = hysteresisArea(traj);

  const psd = welchPSD(g.sp, dtSec > 0 ? 1 / dtSec : 1);
  const dom = dominantPeriod(psd, grid.length * dtSec);

  let boostOn = 0;
  let boostTot = 0;
  for (const r of rows) {
    const b = channelResponseBoost(r, ch);
    if (b !== null) {
      boostTot += 1;
      if (b > 0.01) {
        boostOn += 1;
      }
    }
  }
  const boostShare = boostTot ? boostOn / boostTot : null;

  return {
    channel: ch,
    ctrlLag: ctrl.lagSec,
    ctrlCorr: ctrl.corr,
    fanLag: fan.lagSec,
    fanCorr: fan.corr,
    trackRms,
    effortPerMin,
    revPerMin,
    hyst,
    domPeriod: dom.periodSec,
    oscIndex: dom.oscIndex,
    boostShare,
  };
}

function focusDetail(rows, elapsed, ch, grid, dtSec) {
  const g = channelGrids(rows, elapsed, ch, grid);

  const td = decimate(grid, 4000);
  const tempD = decimate(g.temp, 4000);
  const spD = decimate(g.sp, 4000);
  const t0 = grid.length ? grid[0] : 0;
  const tSpan = grid.length ? (grid[grid.length - 1] - t0) || 1 : 1;
  const traj = [];
  for (let i = 0; i < td.length; i += 1) {
    if (Number.isFinite(tempD[i]) && Number.isFinite(spD[i])) {
      traj.push({ x: tempD[i], y: spD[i], t: (td[i] - t0) / tSpan });
    }
  }

  const bins = new Map();
  for (let i = 0; i < g.temp.length; i += 1) {
    const t = g.temp[i];
    const f = g.ff[i];
    if (Number.isFinite(t) && Number.isFinite(f)) {
      const key = Math.round(t * 2) / 2;
      if (!bins.has(key)) {
        bins.set(key, []);
      }
      bins.get(key).push(f);
    }
  }
  const ref = [...bins.entries()]
    .map(([x, arr]) => ({ x, y: percentile(arr, 50) }))
    .sort((a, b) => a.x - b.x);

  const dec = [];
  for (let i = 0; i < grid.length; i += 1) {
    dec.push({ x: grid[i], ff: g.ff[i], sp: g.sp[i], boost: g.boost[i] });
  }
  const slopes = [];
  for (let i = 1; i < g.sp.length; i += 1) {
    if (Number.isFinite(g.sp[i]) && Number.isFinite(g.sp[i - 1])) {
      slopes.push(Math.abs(g.sp[i] - g.sp[i - 1]) / dtSec);
    }
  }
  const clipMag = slopes.length ? percentile(slopes, 95) : 0;
  const clip = new Array(grid.length).fill(false);
  for (let i = 1; i < grid.length; i += 1) {
    const ds = (g.sp[i] - g.sp[i - 1]) / dtSec;
    const desired = (Number.isFinite(g.ff[i]) ? g.ff[i] : 0) + (Number.isFinite(g.boost[i]) ? g.boost[i] : 0);
    const gap = desired - g.sp[i];
    if (
      Number.isFinite(ds) && clipMag > 0 &&
      Math.abs(ds) >= 0.85 * clipMag &&
      Math.abs(gap) > 1 &&
      Math.sign(gap) === Math.sign(ds)
    ) {
      clip[i] = true;
    }
  }

  const steps = detectSteps(g.ff, grid, dtSec).map((s) => stepMetrics(s, g, dtSec));

  const fs = dtSec > 0 ? 1 / dtSec : 1;
  const psd = welchPSD(g.sp, fs);
  const dom = dominantPeriod(psd, grid.length * dtSec);
  const spec = spectrogram(g.sp, fs);

  return {
    channel: ch,
    phase: { traj, ref, area: hysteresisArea(traj.map((p) => ({ x: p.x, y: p.y }))) },
    decomp: { points: decimate(dec, 3000), clip: decimate(clip, 3000) },
    steps,
    spectral: { psd, dom, spec, recordSec: grid.length * dtSec },
  };
}

// Lightweight per-run scalar bundle for cross-run Pareto comparison.
function runScalarBundle(rows, threshold, name) {
  const elapsed = elapsedSeconds(rows);
  const intervals = rows.map(rowIntervalSeconds);
  const duration = intervals.some((v) => v !== null)
    ? intervals.reduce((s, v) => s + (v || 0), 0)
    : (elapsed.length > 1 && Number.isFinite(elapsed[0]) && Number.isFinite(elapsed.at(-1)) ? elapsed.at(-1) - elapsed[0] : null);
  const durationMin = duration && duration > 0 ? duration / 60 : null;
  const channels = detectChannels(rows);

  let effortPerMin = 0;
  channels.forEach((ch) => {
    const tv = totalVariation(rows.map((r) => maybeNumber(r[`channel${ch}_setpoint_pct`])));
    if (durationMin) {
      effortPerMin += tv / durationMin;
    }
  });

  const gpu = rows.map(gpuEnvelope);
  const exceed = degSecondsOver(gpu, intervals.map((v) => v || 0), threshold);
  const gpuPeak = gpu.reduce((m, v) => (v !== null && (m === null || v > m) ? v : m), null);

  // Fan lag on the busiest channel only (keeps multi-file compare fast).
  let fanLag = null;
  if (channels.length && Number.isFinite(elapsed[0])) {
    let busy = channels[0];
    let spread = -1;
    channels.forEach((ch) => {
      const s = stats(column(rows, `channel${ch}_setpoint_pct`));
      const sp = s.count ? s.max - s.min : 0;
      if (sp > spread) {
        spread = sp;
        busy = ch;
      }
    });
    const span = (elapsed.filter(Number.isFinite).at(-1) || 0) - elapsed[0];
    if (span > 1) {
      const ng = Math.min(2048, Math.max(64, Math.floor(span)));
      const dt = span / ng;
      const grid = Array.from({ length: ng + 1 }, (_, k) => elapsed[0] + k * dt);
      const g = channelGrids(rows, elapsed, busy, grid);
      fanLag = crossCorrLag(g.sp, g.fan, dt, 30).lagSec;
    }
  }

  return {
    name,
    rows: rows.length,
    duration,
    channels: channels.length,
    effortPerMin: durationMin ? effortPerMin : null,
    exceed,
    gpuPeak,
    fanLag,
  };
}

// A run is on the frontier if no other run beats it on BOTH effort and exceed.
function paretoFrontier(points) {
  return points.map((p) => {
    if (!Number.isFinite(p.effortPerMin) || !Number.isFinite(p.exceed)) {
      return { ...p, frontier: false };
    }
    const dominated = points.some((q) =>
      q !== p &&
      Number.isFinite(q.effortPerMin) && Number.isFinite(q.exceed) &&
      q.effortPerMin <= p.effortPerMin && q.exceed <= p.exceed &&
      (q.effortPerMin < p.effortPerMin || q.exceed < p.exceed));
    return { ...p, frontier: !dominated };
  });
}

/* ---- Live snapshot adapter (pure) ---------------------------------------
 * Maps the controller's two atomically-written runtime JSONs onto the same
 * row shape parseCsv produces, so the entire analysis/render stack is reused
 * verbatim for the live feed. control_runtime.json carries the controller
 * state (per-channel demand/setpoint/boost + loop vitals); current_state.json
 * carries fan rpm/duty, CPU sensors and the GPU block.
 * ------------------------------------------------------------------------- */
function snapshotToRow(rt, st, intervalMs) {
  const row = {};
  const num = (v) => (typeof v === "number" && Number.isFinite(v) ? v : "");
  row.wall_clock = (rt && (rt.loop_finished_wall_clock || rt.loop_last_evaluation))
    || (st && st.snapshot_time) || "";
  row.loop_started_wall_clock = (rt && rt.loop_started_wall_clock) || row.wall_clock;
  row.mode = "live";
  row.loop_achieved_interval_ms = Number.isFinite(intervalMs) ? intervalMs : "";
  row.loop_intended_interval_ms = rt ? num(rt.loop_intended_interval_ms) : "";
  row.loop_work_duration_ms = rt ? num(rt.loop_work_duration_ms) : "";
  row.loop_slip_ms = rt ? num(rt.loop_slip_ms) : "";
  row.loop_tick_count = rt ? num(rt.loop_tick_count) : "";
  row.loop_overrun = rt && rt.loop_overrun ? "true" : "false";

  let tctl = null;
  let cmax = null;
  if (st && Array.isArray(st.amd_sensors)) {
    st.amd_sensors.forEach((s) => {
      const t = Number(s && s.temperature_c);
      if (Number.isFinite(t)) {
        cmax = cmax === null ? t : Math.max(cmax, t);
        const lab = String((s && s.label) || "").toLowerCase();
        if (tctl === null && (lab.includes("tctl") || lab.includes("tdie"))) {
          tctl = t;
        }
      }
    });
  }
  row.cpu_tctl_c = tctl !== null ? tctl : "";
  row.cpu_max_c = cmax !== null ? cmax : "";

  const g = st && st.gpu ? st.gpu : null;
  row.gpu_available = g && g.available ? "true" : "false";
  row.gpu_core_c = g ? num(g.core_c) : "";
  row.gpu_memjn_c = g ? num(g.memjn_c) : "";
  row.gpu_hotspot_c = g ? num(g.hotspot_c) : "";

  const fansByCh = {};
  if (st && Array.isArray(st.fans)) {
    st.fans.forEach((f) => {
      if (f && f.channel != null) {
        fansByCh[f.channel] = f;
      }
    });
  }
  if (rt && Array.isArray(rt.controlled_channels)) {
    rt.controlled_channels.forEach((c) => {
      const ch = c && c.channel;
      if (ch == null) {
        return;
      }
      row[`channel${ch}_observed_temp_c`] = num(c.last_observed_temp_c);
      row[`channel${ch}_feedforward_pct`] = num(c.last_raw_demand_pct);
      row[`channel${ch}_smoothed_pct`] = num(c.last_smoothed_demand_pct);
      row[`channel${ch}_setpoint_pct`] = num(c.last_setpoint_pct);
      row[`channel${ch}_thermal_pressure_boost_pct`] = num(c.last_thermal_pressure_boost_pct);
      row[`channel${ch}_midband_pressure_boost_pct`] = num(c.last_midband_pressure_boost_pct);
      row[`channel${ch}_gpu_airflow_boost_pct`] = num(c.last_gpu_airflow_boost_pct);
      row[`channel${ch}_cpu_low_soak_boost_pct`] = num(c.last_cpu_low_soak_boost_pct);
      row[`channel${ch}_low_band_stage_boost_pct`] = num(c.last_low_band_stage_boost_pct);
      row[`channel${ch}_low_band_effective_boost_pct`] = num(c.last_low_band_effective_boost_pct);
      row[`channel${ch}_total_writes`] = num(c.total_writes);
      row[`channel${ch}_sensor_failed`] = c.sensor_failed ? "true" : "false";
      row[`channel${ch}_circuit_breaker_open`] = c.circuit_breaker_open ? "true" : "false";
      const f = fansByCh[ch];
      row[`fan${ch}_rpm`] = f ? num(f.rpm) : "";
      row[`fan${ch}_duty_pct`] = f ? num(f.duty_percent) : "";
      row[`fan${ch}_tach_valid`] = f && f.tach_valid ? "true" : "false";
    });
  }
  return row;
}

// Classify the feed: LIVE only if the controller tick advanced and the
// snapshot is fresh; never present a frozen file as if it were live.
function liveStatus(prevTick, rt, ageMs, maxAgeMs) {
  const cap = Number.isFinite(maxAgeMs) ? maxAgeMs : 15000;
  if (!rt) {
    return { state: "offline", reason: "no runtime json", tick: prevTick };
  }
  const tick = Number(rt.loop_tick_count);
  if (!Number.isFinite(tick)) {
    return { state: "stale", reason: "no tick count", tick: prevTick };
  }
  if (prevTick != null && tick === prevTick) {
    return { state: "stale", reason: "controller not advancing", tick };
  }
  if (Number.isFinite(ageMs) && ageMs > cap) {
    return { state: "stale", reason: `snapshot ${Math.round(ageMs / 1000)} s old`, tick };
  }
  return { state: "live", reason: "ok", tick };
}

function pushCapped(arr, item, cap) {
  arr.push(item);
  if (arr.length > cap) {
    arr.splice(0, arr.length - cap);
  }
  return arr;
}

function computeModel(rows, events, opts) {
  const options = opts || {};
  const threshold = Number.isFinite(options.threshold) ? options.threshold : null;
  const cpuThreshold = Number.isFinite(options.cpuThreshold) ? options.cpuThreshold : null;
  const elapsed = elapsedSeconds(rows);
  const intervals = rows.map(rowIntervalSeconds);
  const duration = intervals.some((value) => value !== null)
    ? intervals.reduce((sum, value) => sum + (value || 0), 0)
    : (elapsed.length > 1 && elapsed[0] !== null && elapsed.at(-1) !== null ? elapsed.at(-1) - elapsed[0] : null);

  const gpuValues = rows.map(gpuEnvelope);
  const gpuClean = gpuValues.filter((value) => value !== null);
  let peakIndex = -1;
  let peakValue = null;
  gpuValues.forEach((value, index) => {
    if (value !== null && (peakValue === null || value > peakValue)) {
      peakValue = value;
      peakIndex = index;
    }
  });

  let aboveThresholdRows = 0;
  let aboveThresholdSeconds = 0;
  let timeToThreshold = null;
  if (threshold !== null) {
    gpuValues.forEach((value, index) => {
      if (value !== null && value >= threshold) {
        aboveThresholdRows += 1;
        aboveThresholdSeconds += intervals[index] || 0;
        if (timeToThreshold === null) {
          timeToThreshold = elapsed[index];
        }
      }
    });
  }

  const channels = detectChannels(rows);
  const channelSummaries = channels.map((channel) => {
    const setpoints = column(rows, `channel${channel}_setpoint_pct`);
    const boosts = rows
      .map((row) => channelResponseBoost(row, channel))
      .filter((value) => value !== null);
    const writes = column(rows, `channel${channel}_total_writes`);
    const writeDelta = writes.length
      ? Math.max(0, Math.max(...writes) - Math.min(...writes))
      : null;
    const setpointStats = stats(setpoints);
    const boostStats = stats(boosts);
    const writesPerMinute = writeDelta !== null && duration && duration > 0
      ? writeDelta / (duration / 60)
      : null;
    const peakRow = peakIndex >= 0 ? rows[peakIndex] : null;
    return {
      channel,
      setpointStats,
      boostStats,
      writes: writeDelta,
      writesPerMinute,
      setpointAtPeak: peakRow ? maybeNumber(peakRow[`channel${channel}_setpoint_pct`]) : null,
    };
  });

  const loopOverruns = rows.reduce((count, row) => {
    const raw = String(row.loop_overrun || "").toLowerCase();
    return count + (raw === "true" || raw === "1" ? 1 : 0);
  }, 0);

  const finiteElapsed = elapsed.filter(Number.isFinite);
  let grid = [];
  let dtSec = 1;
  if (finiteElapsed.length > 4 && duration && duration > 1) {
    const span = finiteElapsed[finiteElapsed.length - 1] - finiteElapsed[0];
    const nGrid = Math.min(5000, Math.max(64, Math.floor(span)));
    dtSec = span / nGrid;
    const base = finiteElapsed[0];
    grid = Array.from({ length: nGrid + 1 }, (_, k) => base + k * dtSec);
  }
  const durationMin = duration && duration > 0 ? duration / 60 : null;

  let quality = [];
  let focus = null;
  if (grid.length && channels.length) {
    quality = channels.map((ch) => channelQuality(rows, elapsed, ch, grid, dtSec, durationMin));

    let focusCh = Number.isFinite(options.focusChannel) ? options.focusChannel : null;
    if (focusCh === null || !channels.includes(focusCh)) {
      let bestCh = channels[0];
      let bestSpread = -1;
      channels.forEach((ch) => {
        const s = stats(column(rows, `channel${ch}_setpoint_pct`));
        const spread = s.count ? (s.max - s.min) : 0;
        if (spread > bestSpread) {
          bestSpread = spread;
          bestCh = ch;
        }
      });
      focusCh = bestCh;
    }
    focus = focusDetail(rows, elapsed, focusCh, grid, dtSec);
  }

  const exceedanceDegSec = degSecondsOver(gpuValues, intervals.map((v) => v || 0), threshold);
  const cpuValues = column(rows, "cpu_tctl_c");
  const cpuWatch = timeAtOrAbove(
    rows.map((row) => maybeNumber(row.cpu_tctl_c)),
    intervals.map((v) => v || 0),
    cpuThreshold,
  );

  return {
    rows,
    elapsed,
    intervals,
    duration,
    channels,
    channelSummaries,
    threshold,
    cpuThreshold,
    gpuValues,
    gpuStats: stats(gpuClean),
    cpuStats: stats(cpuValues),
    intervalStats: stats(column(rows, "loop_achieved_interval_ms")),
    workStats: stats(column(rows, "loop_work_duration_ms")),
    loopOverruns,
    peak: { index: peakIndex, value: peakValue, elapsed: peakIndex >= 0 ? elapsed[peakIndex] : null },
    thresholdSummary: { rows: aboveThresholdRows, seconds: aboveThresholdSeconds, timeToThreshold },
    eventCounts: eventCounts(events || []),
    grid,
    dtSec,
    quality,
    focus,
    exceedanceDegSec,
    cpuWatch,
  };
}

function buildModel(opts) {
  return computeModel(state.rows, state.events, opts);
}

/* ---- Rendering ----------------------------------------------------------- */

let els = null;
const TIME_CHART_IDS = ["temperatureChart", "channelChart", "decompChart"];

function readPalette() {
  if (typeof document === "undefined" || !window.getComputedStyle) {
    return {};
  }
  const cs = getComputedStyle(document.documentElement);
  const g = (n, fb) => (cs.getPropertyValue(n).trim() || fb);
  return {
    text: g("--text", "#161d29"),
    muted: g("--muted", "#5d6779"),
    faint: g("--faint", "#98a2b3"),
    grid: g("--grid", "rgba(0,0,0,0.07)"),
    chartBg: g("--chart-bg", "#fbfcfe"),
    line: g("--line", "#dde3ec"),
    cursor: g("--cursor", "#334155"),
    blue: g("--c-blue", "#2563c9"),
    teal: g("--c-teal", "#0f897d"),
    amber: g("--c-amber", "#b3711a"),
    violet: g("--c-violet", "#6d4fc4"),
    red: g("--c-red", "#ce3b3b"),
    green: g("--c-green", "#1f8a52"),
    gray: g("--c-gray", "#8b95a5"),
    pink: g("--c-pink", "#c2459a"),
    cyan: g("--c-cyan", "#1597ad"),
  };
}

function channelPalette(pal) {
  return [pal.blue, pal.teal, pal.amber, pal.violet, pal.red, pal.green, pal.pink, pal.cyan];
}

function timeColor(f) {
  // Perceptual cool->warm ramp (viridis-ish), readable on light and dark.
  const stops = [[68, 1, 84], [59, 82, 139], [33, 145, 140], [94, 201, 98], [253, 231, 37]];
  const x = Math.max(0, Math.min(0.9999, f)) * (stops.length - 1);
  const i = Math.floor(x);
  const u = x - i;
  const a = stops[i];
  const b = stops[i + 1];
  return `rgb(${Math.round(a[0] + (b[0] - a[0]) * u)}, ${Math.round(a[1] + (b[1] - a[1]) * u)}, ${Math.round(a[2] + (b[2] - a[2]) * u)})`;
}

function heatColor(t) {
  // Inferno-ish ramp for the spectrogram.
  const stops = [[2, 2, 12], [40, 11, 84], [101, 21, 110], [159, 42, 99], [212, 72, 66], [245, 125, 21], [250, 193, 39], [252, 255, 164]];
  const x = Math.max(0, Math.min(0.9999, t)) * (stops.length - 1);
  const i = Math.floor(x);
  const u = x - i;
  const a = stops[i];
  const b = stops[i + 1];
  return `rgb(${Math.round(a[0] + (b[0] - a[0]) * u)}, ${Math.round(a[1] + (b[1] - a[1]) * u)}, ${Math.round(a[2] + (b[2] - a[2]) * u)})`;
}

function renderMetric(label, value, detail = "") {
  return `
    <div class="metric">
      <div class="metric-label">${escapeHtml(label)}</div>
      <div class="metric-value">${escapeHtml(value)}</div>
      <div class="metric-detail">${escapeHtml(detail)}</div>
    </div>`;
}

function renderOverview(model) {
  if (!model.rows.length && !state.summary) {
    els.overviewMetrics.innerHTML = [
      renderMetric("Rows", "0", "CSV not loaded"),
      renderMetric("Duration", "No CSV", "Load a run CSV"),
      renderMetric("CPU max", "No CPU data", "CSV not loaded"),
      renderMetric("GPU envelope", "No GPU data", "CSV not loaded"),
    ].join("");
    return;
  }

  const summaryTemps = state.summary?.temperatures || {};
  const summaryGpu = summaryTemps.gpu_envelope_c || {};
  const summaryCpu = summaryTemps.cpu_tctl_c || {};
  const rows = model.rows.length || state.summary?.row_count || 0;
  const duration = model.duration ?? state.summary?.duration_seconds ?? null;
  const cpuMax = model.cpuStats.max ?? summaryCpu.max ?? null;
  const cpuP90 = model.cpuStats.p90 ?? summaryCpu.p90 ?? null;
  const gpuMax = model.gpuStats.max ?? summaryGpu.max ?? null;
  const gpuP90 = model.gpuStats.p90 ?? summaryGpu.p90 ?? null;
  const thresholdSeconds = model.thresholdSummary.seconds || state.summary?.gpu_response?.above_threshold_seconds || 0;
  const dom = model.focus?.spectral?.dom;
  const cpuWatchSeconds = model.cpuWatch?.seconds ?? 0;
  const cpuWatchRows = model.cpuWatch?.rows ?? 0;

  els.overviewMetrics.innerHTML = [
    renderMetric("Rows", formatInteger(rows), `${model.channels.length} channels`),
    renderMetric("Duration", formatSeconds(duration), state.csvName || state.summaryName || ""),
    renderMetric("CPU max", formatUnit(cpuMax, "C"), `p90 ${formatUnit(cpuP90, "C", 1, "missing")}`),
    renderMetric("CPU watch", formatSeconds(cpuWatchSeconds, "No time above watch"), `${formatUnit(model.cpuThreshold, "C")} · ${cpuWatchRows} rows`),
    renderMetric("GPU envelope", formatUnit(gpuMax, "C", 1, "No GPU fields"), `p90 ${formatUnit(gpuP90, "C", 1, "missing")}`),
    renderMetric("GPU peak time", formatSeconds(model.peak.elapsed ?? state.summary?.gpu_response?.peak?.elapsed_seconds), "from run start"),
    renderMetric("GPU over limit", formatSeconds(thresholdSeconds, "No time over limit"), formatUnit(model.threshold, "C")),
    renderMetric("GPU exceedance", model.exceedanceDegSec === null ? "No GPU limit" : formatInteger(model.exceedanceDegSec), "degC-s over limit"),
    renderMetric("Dominant period", dom && dom.periodSec ? `${formatNumber(dom.periodSec)} s` : "none", dom && dom.oscIndex ? `osc idx ${formatNumber(dom.oscIndex)}` : "focus ch"),
    renderMetric("Loop p95", formatUnit(model.intervalStats.p95, "ms", 1, "No timing"), `${model.loopOverruns} overruns`),
  ].join("");
}

function renderChannelTable(model) {
  if (!model.channelSummaries.length) {
    els.channelRows.innerHTML = '<tr><td colspan="8" class="empty">No channel data loaded.</td></tr>';
    return;
  }
  els.channelRows.innerHTML = model.channelSummaries.map((item) => `
    <tr>
      <td>${item.channel}</td>
      <td>${formatPct(item.setpointStats.p50)}</td>
      <td>${formatPct(item.setpointStats.p90)}</td>
      <td>${formatPct(item.setpointStats.max)}</td>
      <td>${formatPct(item.setpointAtPeak)}</td>
      <td>${formatPct(item.boostStats.max, 1, "no boost")}</td>
      <td>${formatInteger(item.writes)}</td>
      <td>${formatNumber(item.writesPerMinute, 1, "not enough time")}</td>
    </tr>
  `).join("");
}

function renderQuality(model) {
  if (!model.quality || !model.quality.length) {
    els.qualityRows.innerHTML = '<tr><td colspan="9" class="empty">Load a control-loop CSV with timing to compute response metrics.</td></tr>';
    els.qualityNote.textContent = "Per-channel response metrics";
    return;
  }
  const focusCh = model.focus ? model.focus.channel : null;
  els.qualityRows.innerHTML = model.quality.map((q) => {
    const lagTxt = (lag, corr) => lag === null ? "not detected" : `${lag.toFixed(1)} s${corr !== null && corr < 0.3 ? " ?" : ""}`;
    return `
    <tr${q.channel === focusCh ? ' class="focus-row"' : ""}>
      <td>${q.channel}${q.channel === focusCh ? " &#9673;" : ""}</td>
      <td>${lagTxt(q.ctrlLag, q.ctrlCorr)}</td>
      <td>${lagTxt(q.fanLag, q.fanCorr)}</td>
      <td>${formatPct(q.trackRms)}</td>
      <td>${formatUnit(q.effortPerMin, "%/min", 1, "not enough data")}</td>
      <td>${formatNumber(q.revPerMin, 1, "not enough data")}</td>
      <td>${formatInteger(q.hyst)}</td>
      <td>${q.domPeriod ? `${formatNumber(q.domPeriod)} s` : "none"}</td>
      <td>${q.boostShare === null ? "no boost data" : `${(q.boostShare * 100).toFixed(0)}%`}</td>
    </tr>`;
  }).join("");
  els.qualityNote.textContent = `${model.quality.length} channels | dt ${model.dtSec.toFixed(2)} s | "?" weak corr`;
}

function renderEvents(model) {
  if (!state.events.length) {
    els.eventSummary.innerHTML = '<div class="empty">No events loaded.</div>';
    els.eventNote.textContent = "Optional JSONL";
    return;
  }
  const issueCount = state.events.filter((event) => ISSUE_EVENTS.has(String(event.event_type || ""))).length;
  els.eventNote.textContent = `${state.events.length} events, ${issueCount} issue markers`;
  els.eventSummary.innerHTML = model.eventCounts.slice(0, 12).map(([type, count]) => `
    <div class="event-row">
      <span>${escapeHtml(type)}</span>
      <span class="event-count">${count}</span>
    </div>
  `).join("");
}

function summaryHistoryRow(summary, fallbackName) {
  const temps = summary.temperatures || {};
  const cpu = temps.cpu_tctl_c || {};
  const gpu = temps.gpu_envelope_c || {};
  const response = summary.gpu_response || {};
  const runLabel = summary.run_id || summary.profile || fallbackName || "summary";
  return `
    <tr>
      <td>${escapeHtml(String(runLabel))}</td>
      <td>${formatInteger(summary.row_count)}</td>
      <td>${formatSeconds(summary.duration_seconds)}</td>
      <td>${formatNumber(cpu.p90)} / ${formatNumber(cpu.max)}</td>
      <td>${formatNumber(gpu.p90)} / ${formatNumber(gpu.max)}</td>
      <td>${formatSeconds(response.above_threshold_seconds)}</td>
    </tr>`;
}

function renderHistory() {
  const summaries = [];
  if (state.summary) {
    summaries.push({ summary: state.summary, name: state.summaryName });
  }
  state.history.forEach((item) => summaries.push(item));
  if (!summaries.length) {
    els.historyRows.innerHTML = '<tr><td colspan="6" class="empty">No history summaries loaded.</td></tr>';
    els.historyNote.textContent = "Analyzer JSON summaries";
    return;
  }
  els.historyRows.innerHTML = summaries.map((item) => summaryHistoryRow(item.summary, item.name)).join("");
  els.historyNote.textContent = `${summaries.length} summaries`;
}

function seriesFromRows(rows, elapsed, fields) {
  return fields.map((field) => {
    const points = [];
    rows.forEach((row, index) => {
      const x = elapsed[index];
      const y = field.value(row);
      if (x !== null && y !== null) {
        points.push({ x, y });
      }
    });
    return { ...field, points };
  }).filter((field) => field.points.length);
}

function chartGeom(canvas, pal) {
  const ctx = canvas.getContext("2d");
  const rect = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  const width = Math.max(320, Math.floor(rect.width));
  const height = Math.max(220, Number(canvas.getAttribute("height")) || Math.floor(rect.height));
  canvas.width = Math.floor(width * dpr);
  canvas.height = Math.floor(height * dpr);
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = pal.chartBg || "#fbfcfe";
  ctx.fillRect(0, 0, width, height);
  return { ctx, width, height };
}

function emptyCanvas(canvas, pal, text) {
  const { ctx, width, height } = chartGeom(canvas, pal);
  const lines = String(text || "No data").split("\n").filter(Boolean);
  const box = {
    left: 18,
    top: 18,
    right: width - 18,
    bottom: Math.min(height - 18, 18 + 26 + lines.length * 20),
  };
  ctx.save();
  ctx.fillStyle = "rgba(127, 127, 127, 0.08)";
  ctx.strokeStyle = pal.line || "#dde3ec";
  ctx.setLineDash([5, 4]);
  ctx.lineWidth = 1;
  ctx.fillRect(box.left, box.top, box.right - box.left, box.bottom - box.top);
  ctx.strokeRect(box.left, box.top, box.right - box.left, box.bottom - box.top);
  ctx.setLineDash([]);
  ctx.fillStyle = pal.text || "#161d29";
  ctx.font = "600 14px Segoe UI, sans-serif";
  ctx.fillText(lines[0] || "No data", box.left + 14, box.top + 28);
  ctx.fillStyle = pal.muted || "#687385";
  ctx.font = "13px Segoe UI, sans-serif";
  for (let i = 1; i < lines.length; i += 1) {
    ctx.fillText(lines[i], box.left + 14, box.top + 28 + i * 20);
  }
  ctx.restore();
  canvas.__cursor = null;
}

function safeDraw(canvas, pal, label, drawFn) {
  try {
    drawFn();
  } catch (error) {
    const message = error && error.message ? error.message : String(error);
    emptyCanvas(canvas, pal, `${label} render failed\n${message}`);
  }
}

function axisLabels(ctx, plot, pal, xTitle, yTitle) {
  ctx.save();
  ctx.fillStyle = pal.muted;
  ctx.font = "11px Segoe UI, sans-serif";
  if (xTitle) {
    ctx.textAlign = "center";
    ctx.textBaseline = "bottom";
    ctx.fillText(xTitle, (plot.left + plot.right) / 2, plot.bottom + 30);
  }
  if (yTitle) {
    ctx.translate(14, (plot.top + plot.bottom) / 2);
    ctx.rotate(-Math.PI / 2);
    ctx.textAlign = "center";
    ctx.textBaseline = "top";
    ctx.fillText(yTitle, 0, 0);
  }
  ctx.restore();
}

function drawChart(canvas, series, options, pal) {
  const { ctx, width, height } = chartGeom(canvas, pal);
  if (!series.length) {
    ctx.fillStyle = pal.muted;
    ctx.font = "14px Segoe UI, sans-serif";
    ctx.fillText(options.emptyText || "No data", 18, 32);
    canvas.__cursor = null;
    return;
  }

  const allPoints = series.flatMap((item) => item.points);
  const maxX = Math.max(...allPoints.map((point) => point.x), 1);
  const values = allPoints.map((point) => point.y);
  let minY = Math.min(...values);
  let maxY = Math.max(...values);
  if (options.yMin !== undefined) {
    minY = Math.min(minY, options.yMin);
  }
  if (minY === maxY) {
    minY -= 1;
    maxY += 1;
  }
  const padY = (maxY - minY) * 0.08;
  minY -= padY;
  maxY += padY;

  const plot = { left: 58, right: width - 18, top: 40, bottom: height - 46 };
  const xScale = (x) => plot.left + (x / maxX) * (plot.right - plot.left);
  const yScale = (y) => plot.bottom - ((y - minY) / (maxY - minY)) * (plot.bottom - plot.top);

  ctx.strokeStyle = pal.grid;
  ctx.lineWidth = 1;
  ctx.fillStyle = pal.muted;
  ctx.font = "11px Segoe UI, sans-serif";
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (let i = 0; i <= 4; i += 1) {
    const t = i / 4;
    const y = plot.top + t * (plot.bottom - plot.top);
    ctx.beginPath();
    ctx.moveTo(plot.left, y);
    ctx.lineTo(plot.right, y);
    ctx.stroke();
    ctx.fillText(formatNumber(maxY - t * (maxY - minY), 0), plot.left - 8, y);
  }
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  for (let i = 0; i <= 4; i += 1) {
    const t = i / 4;
    const x = plot.left + t * (plot.right - plot.left);
    ctx.beginPath();
    ctx.moveTo(x, plot.bottom);
    ctx.lineTo(x, plot.bottom + 4);
    ctx.stroke();
    ctx.fillText(formatSeconds(maxX * t), x, plot.bottom + 8);
  }
  axisLabels(ctx, plot, pal, options.axisX, options.axisY);

  (options.hlines || []).forEach((h) => {
    if (!Number.isFinite(h.y) || h.y < minY || h.y > maxY) {
      return;
    }
    const y = yScale(h.y);
    ctx.strokeStyle = h.color;
    ctx.globalAlpha = 0.6;
    ctx.lineWidth = 1;
    ctx.setLineDash([6, 4]);
    ctx.beginPath();
    ctx.moveTo(plot.left, y);
    ctx.lineTo(plot.right, y);
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.globalAlpha = 1;
    ctx.fillStyle = h.color;
    ctx.font = "10px Segoe UI, sans-serif";
    ctx.textAlign = "right";
    ctx.textBaseline = "bottom";
    ctx.fillText(h.label, plot.right - 2, y - 1);
  });

  series.forEach((item) => {
    ctx.strokeStyle = item.color;
    ctx.lineWidth = item.width || 1.8;
    ctx.beginPath();
    item.points.forEach((point, index) => {
      const x = xScale(point.x);
      const y = yScale(point.y);
      index === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
    });
    ctx.stroke();
  });

  let legendX = plot.left;
  let legendY = 16;
  ctx.font = "12px Segoe UI, sans-serif";
  ctx.textAlign = "left";
  ctx.textBaseline = "middle";
  series.forEach((item) => {
    const labelWidth = ctx.measureText(item.label).width + 28;
    if (legendX + labelWidth > plot.right) {
      legendX = plot.left;
      legendY += 16;
    }
    ctx.fillStyle = item.color;
    ctx.fillRect(legendX, legendY - 4, 14, 3);
    ctx.fillStyle = pal.text;
    ctx.fillText(item.label, legendX + 20, legendY);
    legendX += labelWidth;
  });

  canvas.__cursor = {
    plot,
    xMin: 0,
    xMax: maxX,
    series: series.map((s) => ({ label: s.label, color: s.color, points: s.points })),
    xToPx: (x) => xScale(x),
    yToPx: (y) => yScale(y),
    pxToX: (px) => ((px - plot.left) / (plot.right - plot.left)) * maxX,
  };
}

function phaseEmptyReason(model) {
  if (!model.rows.length) {
    return "Waiting for live control CSV\nPick Control CSV manually if the live run is not packaged under release/runtime.";
  }
  if (!model.channels.length) {
    return "No channel columns found\nExpected channelN_setpoint_pct fields in the CSV.";
  }
  if (!model.grid.length) {
    return "Need loop timing\nExpected loop_achieved_interval_ms or wall-clock timing fields.";
  }
  if (!model.focus) {
    return "No focus channel selected\nChoose a channel with setpoint and observed-temp data.";
  }
  return `No phase samples for channel ${model.focus.channel}\nExpected channel${model.focus.channel}_observed_temp_c and channel${model.focus.channel}_setpoint_pct.`;
}

function drawPhasePortrait(canvas, model, pal) {
  if (!model.focus || !model.focus.phase.traj.length) {
    emptyCanvas(canvas, pal, phaseEmptyReason(model));
    return;
  }
  const { ctx, width, height } = chartGeom(canvas, pal);
  const { traj, ref, area } = model.focus.phase;

  const xs = traj.map((p) => p.x).concat(ref.map((p) => p.x));
  const ys = traj.map((p) => p.y).concat(ref.map((p) => p.y));
  let minX = Math.min(...xs);
  let maxX = Math.max(...xs);
  let minY = Math.min(...ys, 0);
  let maxY = Math.max(...ys, 100);
  const padX = (maxX - minX) * 0.06 || 1;
  minX -= padX;
  maxX += padX;
  const padY = (maxY - minY) * 0.06 || 1;
  minY -= padY;
  maxY += padY;

  const plot = { left: 56, right: width - 18, top: 38, bottom: height - 46 };
  const sx = (x) => plot.left + ((x - minX) / (maxX - minX)) * (plot.right - plot.left);
  const sy = (y) => plot.bottom - ((y - minY) / (maxY - minY)) * (plot.bottom - plot.top);

  ctx.strokeStyle = pal.grid;
  ctx.fillStyle = pal.muted;
  ctx.font = "11px Segoe UI, sans-serif";
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (let i = 0; i <= 4; i += 1) {
    const t = i / 4;
    const y = plot.top + t * (plot.bottom - plot.top);
    ctx.beginPath();
    ctx.moveTo(plot.left, y);
    ctx.lineTo(plot.right, y);
    ctx.stroke();
    ctx.fillText(`${formatNumber(maxY - t * (maxY - minY), 0)}%`, plot.left - 8, y);
  }
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  for (let i = 0; i <= 4; i += 1) {
    const t = i / 4;
    const x = plot.left + t * (plot.right - plot.left);
    ctx.beginPath();
    ctx.moveTo(x, plot.top);
    ctx.lineTo(x, plot.bottom);
    ctx.stroke();
    ctx.fillStyle = pal.muted;
    ctx.fillText(`${formatNumber(minX + t * (maxX - minX), 0)}`, x, plot.bottom + 8);
  }
  axisLabels(ctx, plot, pal, "observed temp (C)", "delivered setpoint (%)");

  if (ref.length > 1) {
    ctx.strokeStyle = pal.gray;
    ctx.setLineDash([5, 4]);
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ref.forEach((p, i) => (i === 0 ? ctx.moveTo(sx(p.x), sy(p.y)) : ctx.lineTo(sx(p.x), sy(p.y))));
    ctx.stroke();
    ctx.setLineDash([]);
  }

  ctx.lineWidth = 1.6;
  for (let i = 1; i < traj.length; i += 1) {
    ctx.strokeStyle = timeColor(traj[i].t);
    ctx.beginPath();
    ctx.moveTo(sx(traj[i - 1].x), sy(traj[i - 1].y));
    ctx.lineTo(sx(traj[i].x), sy(traj[i].y));
    ctx.stroke();
  }

  const last = traj[traj.length - 1];
  ctx.fillStyle = pal.text;
  ctx.beginPath();
  ctx.arc(sx(last.x), sy(last.y), 4.5, 0, Math.PI * 2);
  ctx.fill();

  ctx.textAlign = "left";
  ctx.textBaseline = "top";
  ctx.font = "12px Segoe UI, sans-serif";
  ctx.fillStyle = pal.text;
  ctx.fillText(`Ch ${model.focus.channel} · loop area ${formatInteger(area)} (C·%)`, plot.left, 14);
  ctx.fillStyle = pal.gray;
  ctx.textAlign = "right";
  ctx.fillText("- - reference curve · color = time", plot.right, 14);
  canvas.__cursor = null;
}

function drawDecomposition(canvas, model, pal) {
  if (!model.focus || !model.focus.decomp.points.length) {
    emptyCanvas(canvas, pal, "Load a CSV — needs feedforward + setpoint.");
    return;
  }
  const { ctx, width, height } = chartGeom(canvas, pal);
  const pts = model.focus.decomp.points;
  const clip = model.focus.decomp.clip;
  const x0 = pts[0].x;
  const xSpan = (pts[pts.length - 1].x - x0) || 1;

  let maxY = 0;
  pts.forEach((p) => {
    [p.ff, p.sp, (p.ff || 0) + (p.boost || 0)].forEach((v) => {
      if (Number.isFinite(v)) {
        maxY = Math.max(maxY, v);
      }
    });
  });
  maxY = Math.max(10, maxY * 1.12);

  const plot = { left: 52, right: width - 16, top: 42, bottom: height - 44 };
  const sx = (x) => plot.left + ((x - x0) / xSpan) * (plot.right - plot.left);
  const sy = (y) => plot.bottom - (y / maxY) * (plot.bottom - plot.top);

  ctx.strokeStyle = pal.grid;
  ctx.fillStyle = pal.muted;
  ctx.font = "11px Segoe UI, sans-serif";
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (let i = 0; i <= 4; i += 1) {
    const t = i / 4;
    const y = plot.top + t * (plot.bottom - plot.top);
    ctx.beginPath();
    ctx.moveTo(plot.left, y);
    ctx.lineTo(plot.right, y);
    ctx.stroke();
    ctx.fillText(`${formatNumber(maxY - t * maxY, 0)}%`, plot.left - 8, y);
  }
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  for (let i = 0; i <= 4; i += 1) {
    const t = i / 4;
    ctx.fillText(formatSeconds(t * xSpan), plot.left + t * (plot.right - plot.left), plot.bottom + 8);
  }
  axisLabels(ctx, plot, pal, "elapsed", "duty (%)");

  ctx.fillStyle = "rgba(206, 59, 59, 0.12)";
  for (let i = 1; i < pts.length; i += 1) {
    if (clip[i]) {
      ctx.fillRect(sx(pts[i - 1].x), plot.top, Math.max(1, sx(pts[i].x) - sx(pts[i - 1].x)), plot.bottom - plot.top);
    }
  }

  const drawBand = (predicate, color) => {
    ctx.fillStyle = color;
    ctx.beginPath();
    let open = false;
    for (let i = 0; i < pts.length; i += 1) {
      const p = pts[i];
      if (!Number.isFinite(p.ff) || !Number.isFinite(p.sp) || !predicate(p)) {
        if (open) {
          for (let j = i - 1; j >= 0 && Number.isFinite(pts[j].ff) && Number.isFinite(pts[j].sp) && predicate(pts[j]); j -= 1) {
            ctx.lineTo(sx(pts[j].x), sy(pts[j].ff));
          }
          ctx.closePath();
          ctx.fill();
          ctx.beginPath();
          open = false;
        }
        continue;
      }
      if (!open) {
        ctx.moveTo(sx(p.x), sy(p.ff));
        open = true;
      }
      ctx.lineTo(sx(p.x), sy(p.sp));
    }
    if (open) {
      for (let j = pts.length - 1; j >= 0 && Number.isFinite(pts[j].ff) && Number.isFinite(pts[j].sp) && predicate(pts[j]); j -= 1) {
        ctx.lineTo(sx(pts[j].x), sy(pts[j].ff));
      }
      ctx.closePath();
      ctx.fill();
    }
  };
  drawBand((p) => p.sp >= p.ff, "rgba(31, 138, 82, 0.20)");
  drawBand((p) => p.sp < p.ff, "rgba(206, 59, 59, 0.18)");

  const line = (key, color, w) => {
    ctx.strokeStyle = color;
    ctx.lineWidth = w;
    ctx.beginPath();
    let started = false;
    pts.forEach((p) => {
      const v = key(p);
      if (!Number.isFinite(v)) {
        started = false;
        return;
      }
      const x = sx(p.x);
      const y = sy(v);
      started ? ctx.lineTo(x, y) : (ctx.moveTo(x, y), (started = true));
    });
    ctx.stroke();
  };
  line((p) => p.ff, pal.gray, 1.5);
  line((p) => (p.ff || 0) + (p.boost || 0), pal.amber, 1);
  line((p) => p.sp, pal.blue, 2.4);

  const legend = [
    ["feedforward (curve)", pal.gray],
    ["+ boost", pal.amber],
    ["delivered setpoint", pal.blue],
    ["holding / lagging", pal.green],
    ["slew-clipped (est.)", pal.red],
  ];
  let lx = plot.left;
  ctx.font = "12px Segoe UI, sans-serif";
  ctx.textAlign = "left";
  ctx.textBaseline = "middle";
  legend.forEach(([label, color]) => {
    ctx.fillStyle = color;
    ctx.fillRect(lx, 16, 14, 3);
    ctx.fillStyle = pal.text;
    ctx.fillText(label, lx + 20, 18);
    lx += ctx.measureText(label).width + 42;
  });

  canvas.__cursor = {
    plot,
    xMin: x0,
    xMax: x0 + xSpan,
    series: [
      { label: "feedforward", color: pal.gray, points: pts.map((p) => ({ x: p.x, y: p.ff })) },
      { label: "setpoint", color: pal.blue, points: pts.map((p) => ({ x: p.x, y: p.sp })) },
      { label: "boost", color: pal.amber, points: pts.map((p) => ({ x: p.x, y: p.boost })) },
    ],
    xToPx: (x) => sx(x),
    yToPx: (y) => sy(y),
    pxToX: (px) => x0 + ((px - plot.left) / (plot.right - plot.left)) * xSpan,
  };
}

function drawStepGallery(canvas, model, pal) {
  const steps = model.focus ? model.focus.steps : [];
  if (!steps.length) {
    emptyCanvas(canvas, pal, "No load-step transients detected in this run.");
    return;
  }
  const { ctx, width, height } = chartGeom(canvas, pal);

  let minX = Infinity;
  let maxX = -Infinity;
  let minY = 0;
  let maxY = 0;
  steps.forEach((s) => s.rel.forEach((r) => {
    if (r.sp === null) {
      return;
    }
    minX = Math.min(minX, r.x);
    maxX = Math.max(maxX, r.x);
    minY = Math.min(minY, r.sp);
    maxY = Math.max(maxY, r.sp);
  }));
  if (!Number.isFinite(minX)) {
    emptyCanvas(canvas, pal, "No usable step windows.");
    return;
  }
  const padY = (maxY - minY) * 0.1 || 2;
  minY -= padY;
  maxY += padY;

  const plot = { left: 56, right: width - 16, top: 42, bottom: height - 42 };
  const sx = (x) => plot.left + ((x - minX) / (maxX - minX)) * (plot.right - plot.left);
  const sy = (y) => plot.bottom - ((y - minY) / (maxY - minY)) * (plot.bottom - plot.top);

  ctx.strokeStyle = pal.grid;
  ctx.fillStyle = pal.muted;
  ctx.font = "11px Segoe UI, sans-serif";
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (let i = 0; i <= 4; i += 1) {
    const t = i / 4;
    const y = plot.top + t * (plot.bottom - plot.top);
    ctx.beginPath();
    ctx.moveTo(plot.left, y);
    ctx.lineTo(plot.right, y);
    ctx.stroke();
    ctx.fillText(`${formatNumber(maxY - t * (maxY - minY), 0)}%`, plot.left - 8, y);
  }

  ctx.strokeStyle = pal.faint;
  ctx.setLineDash([4, 4]);
  ctx.beginPath();
  ctx.moveTo(sx(0), plot.top);
  ctx.lineTo(sx(0), plot.bottom);
  ctx.stroke();
  ctx.setLineDash([]);
  ctx.fillStyle = pal.muted;
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  for (let s = Math.ceil(minX / 10) * 10; s <= maxX; s += 10) {
    ctx.fillText(`${s}s`, sx(s), plot.bottom + 8);
  }
  axisLabels(ctx, plot, pal, "time relative to load step (s)", "Δ setpoint (%)");

  steps.forEach((s) => {
    ctx.strokeStyle = pal.blue;
    ctx.globalAlpha = 0.28;
    ctx.lineWidth = 1;
    ctx.beginPath();
    let started = false;
    s.rel.forEach((r) => {
      if (r.sp === null) {
        started = false;
        return;
      }
      const x = sx(r.x);
      const y = sy(r.sp);
      started ? ctx.lineTo(x, y) : (ctx.moveTo(x, y), (started = true));
    });
    ctx.stroke();
  });
  ctx.globalAlpha = 1;

  const maxLen = Math.max(...steps.map((s) => s.rel.length));
  const med = [];
  for (let k = 0; k < maxLen; k += 1) {
    const xsamp = [];
    const ysamp = [];
    steps.forEach((s) => {
      const r = s.rel[k];
      if (r && r.sp !== null) {
        xsamp.push(r.x);
        ysamp.push(r.sp);
      }
    });
    if (ysamp.length) {
      med.push({ x: percentile(xsamp, 50), y: percentile(ysamp, 50) });
    }
  }
  ctx.strokeStyle = pal.text;
  ctx.lineWidth = 2.6;
  ctx.beginPath();
  med.forEach((p, i) => (i === 0 ? ctx.moveTo(sx(p.x), sy(p.y)) : ctx.lineTo(sx(p.x), sy(p.y))));
  ctx.stroke();

  ctx.fillStyle = pal.text;
  ctx.font = "12px Segoe UI, sans-serif";
  ctx.textAlign = "left";
  ctx.textBaseline = "top";
  ctx.fillText(`Ch ${model.focus.channel} · ${steps.length} load steps · bold = median envelope`, plot.left, 14);
  canvas.__cursor = null;
}

function renderStepTable(model) {
  const steps = model.focus ? model.focus.steps : [];
  if (!steps.length) {
    els.stepRows.innerHTML = '<tr><td colspan="8" class="empty">No steps detected.</td></tr>';
    els.stepNote.textContent = "Load-step transients, time-zeroed";
    return;
  }
  els.stepRows.innerHTML = steps.map((s, i) => `
    <tr>
      <td>#${i + 1}</td>
      <td>${formatSeconds(s.atSec)}</td>
      <td>${formatSignedPct(s.magFF)}</td>
      <td>${s.deadSec === null ? "not detected" : formatSeconds(s.deadSec)}</td>
      <td>${s.rise === null ? "not settled" : formatSeconds(s.rise)}</td>
      <td>${s.tempExcursion === null ? "missing temp" : formatUnit(s.tempExcursion, "C")}</td>
      <td>${s.settleSec === null ? "not settled" : formatSeconds(s.settleSec)}</td>
      <td>${s.resid === null ? "missing demand" : formatPct(s.resid)}</td>
    </tr>
  `).join("");
  els.stepNote.textContent = `${steps.length} steps | Ch ${model.focus.channel}`;
}

function drawPsd(canvas, model, pal) {
  const sp = model.focus?.spectral;
  if (!sp || !sp.psd || !sp.psd.psd.length) {
    emptyCanvas(canvas, pal, "Need a longer run for spectral analysis.");
    return;
  }
  const { ctx, width, height } = chartGeom(canvas, pal);
  const { freqs, psd } = sp.psd;
  const periods = freqs.map((f) => 1 / f);
  const pMin = Math.max(2, periods[periods.length - 1]);
  const pMax = periods[0];
  const dbs = psd.map((v) => 10 * Math.log10(v + 1e-12));
  let dbMin = Math.min(...dbs);
  let dbMax = Math.max(...dbs);
  if (dbMin === dbMax) {
    dbMin -= 1;
    dbMax += 1;
  }
  const pad = (dbMax - dbMin) * 0.08;
  dbMin -= pad;
  dbMax += pad;

  const plot = { left: 56, right: width - 16, top: 38, bottom: height - 46 };
  const lpMin = Math.log10(pMin);
  const lpMax = Math.log10(pMax);
  const sx = (p) => plot.left + ((Math.log10(p) - lpMin) / (lpMax - lpMin)) * (plot.right - plot.left);
  const sy = (db) => plot.bottom - ((db - dbMin) / (dbMax - dbMin)) * (plot.bottom - plot.top);

  ctx.strokeStyle = pal.grid;
  ctx.fillStyle = pal.muted;
  ctx.font = "11px Segoe UI, sans-serif";
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (let i = 0; i <= 4; i += 1) {
    const t = i / 4;
    const y = plot.top + t * (plot.bottom - plot.top);
    ctx.beginPath();
    ctx.moveTo(plot.left, y);
    ctx.lineTo(plot.right, y);
    ctx.stroke();
    ctx.fillText(`${formatNumber(dbMax - t * (dbMax - dbMin), 0)}`, plot.left - 8, y);
  }
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  [2, 5, 10, 20, 30, 60, 120, 240, 480].forEach((p) => {
    if (p >= pMin && p <= pMax) {
      const x = sx(p);
      ctx.strokeStyle = pal.grid;
      ctx.beginPath();
      ctx.moveTo(x, plot.top);
      ctx.lineTo(x, plot.bottom);
      ctx.stroke();
      ctx.fillStyle = pal.muted;
      ctx.fillText(`${p}s`, x, plot.bottom + 8);
    }
  });
  axisLabels(ctx, plot, pal, "oscillation period (s, log)", "power (dB)");

  ctx.strokeStyle = pal.blue;
  ctx.lineWidth = 1.8;
  ctx.beginPath();
  for (let k = 0; k < psd.length; k += 1) {
    const x = sx(periods[k]);
    const y = sy(dbs[k]);
    k === 0 ? ctx.moveTo(x, y) : ctx.lineTo(x, y);
  }
  ctx.stroke();

  if (sp.dom && sp.dom.periodSec) {
    const x = sx(Math.max(pMin, Math.min(pMax, sp.dom.periodSec)));
    ctx.strokeStyle = pal.amber;
    ctx.setLineDash([5, 4]);
    ctx.lineWidth = 1.5;
    ctx.beginPath();
    ctx.moveTo(x, plot.top);
    ctx.lineTo(x, plot.bottom);
    ctx.stroke();
    ctx.setLineDash([]);
    ctx.fillStyle = pal.amber;
    ctx.textAlign = x > (plot.left + plot.right) / 2 ? "right" : "left";
    ctx.textBaseline = "top";
    ctx.fillText(` ${formatNumber(sp.dom.periodSec)} s `, x, plot.top + 2);
  }

  ctx.fillStyle = pal.text;
  ctx.font = "12px Segoe UI, sans-serif";
  ctx.textAlign = "left";
  ctx.textBaseline = "top";
  const idx = sp.dom && sp.dom.oscIndex ? `osc index ${formatNumber(sp.dom.oscIndex)} (peak/median)` : "no clear limit cycle";
  ctx.fillText(`Ch ${model.focus.channel} setpoint · ${idx}`, plot.left, 14);
  canvas.__cursor = null;
}

function drawSpectrogram(canvas, model, pal) {
  const spec = model.focus?.spectral?.spec;
  if (!spec || !spec.frames.length) {
    emptyCanvas(canvas, pal, "Need a longer run for the spectrogram.");
    return;
  }
  const { ctx, width, height } = chartGeom(canvas, pal);
  const { times, periods, frames, lo, hi } = spec;
  const plot = { left: 56, right: width - 16, top: 24, bottom: height - 46 };
  const nF = frames.length;
  const nK = periods.length;
  const cw = (plot.right - plot.left) / nF;
  const ch = (plot.bottom - plot.top) / nK;
  const span = hi - lo || 1;

  for (let f = 0; f < nF; f += 1) {
    const col = frames[f];
    const x = plot.left + f * cw;
    for (let k = 0; k < nK; k += 1) {
      const norm = Math.max(0, Math.min(1, (col[k] - lo) / span));
      ctx.fillStyle = heatColor(norm);
      // col[k] is FFT bin k+1: longest period at the bottom, shortest at the top.
      const y = plot.bottom - (k + 1) * ch;
      ctx.fillRect(x, y, Math.ceil(cw) + 1, Math.ceil(ch) + 1);
    }
  }

  ctx.fillStyle = pal.muted;
  ctx.font = "11px Segoe UI, sans-serif";
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  [2, 5, 10, 20, 30, 60, 120, 240].forEach((p) => {
    let bestK = -1;
    let bestD = Infinity;
    for (let k = 0; k < nK; k += 1) {
      const d = Math.abs(periods[k] - p);
      if (d < bestD) {
        bestD = d;
        bestK = k;
      }
    }
    if (bestK >= 0 && p >= periods[nK - 1] && p <= periods[0]) {
      const y = plot.bottom - (bestK + 0.5) * ch;
      ctx.fillText(`${p}s`, plot.left - 8, y);
    }
  });
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  const tMax = times[times.length - 1] || 1;
  for (let i = 0; i <= 4; i += 1) {
    const t = i / 4;
    ctx.fillText(formatSeconds(tMax * t), plot.left + t * (plot.right - plot.left), plot.bottom + 8);
  }
  axisLabels(ctx, plot, pal, "elapsed", "period (s)");
  canvas.__cursor = null;
}

function drawPareto(canvas, points, currentName, pal) {
  if (!points.length) {
    emptyCanvas(canvas, pal, 'Load runs via "Compare CSVs" to plot the effort/risk trade-off.');
    return;
  }
  const valid = points.filter((p) => Number.isFinite(p.effortPerMin) && Number.isFinite(p.exceed));
  if (!valid.length) {
    emptyCanvas(canvas, pal, "Runs need timing + temperature to place on the trade-off.");
    return;
  }
  const { ctx, width, height } = chartGeom(canvas, pal);
  let maxX = Math.max(...valid.map((p) => p.effortPerMin), 1);
  let maxY = Math.max(...valid.map((p) => p.exceed), 1);
  maxX *= 1.12;
  maxY = maxY * 1.12 || 1;

  const plot = { left: 62, right: width - 18, top: 40, bottom: height - 48 };
  const sx = (x) => plot.left + (x / maxX) * (plot.right - plot.left);
  const sy = (y) => plot.bottom - (y / maxY) * (plot.bottom - plot.top);

  ctx.strokeStyle = pal.grid;
  ctx.fillStyle = pal.muted;
  ctx.font = "11px Segoe UI, sans-serif";
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";
  for (let i = 0; i <= 4; i += 1) {
    const t = i / 4;
    const y = plot.top + t * (plot.bottom - plot.top);
    ctx.beginPath();
    ctx.moveTo(plot.left, y);
    ctx.lineTo(plot.right, y);
    ctx.stroke();
    ctx.fillText(formatInteger(maxY - t * maxY), plot.left - 8, y);
  }
  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  for (let i = 0; i <= 4; i += 1) {
    const t = i / 4;
    ctx.fillText(formatNumber(maxX * t, 0), plot.left + t * (plot.right - plot.left), plot.bottom + 8);
  }
  axisLabels(ctx, plot, pal, "control effort (%/min)  —  lower = quieter", "thermal exceedance (C·s)  —  lower = safer");

  ctx.fillStyle = pal.muted;
  ctx.font = "12px Segoe UI, sans-serif";
  ctx.textAlign = "left";
  ctx.textBaseline = "top";
  ctx.fillText("◣ better", plot.left + 4, plot.bottom - 18);

  const front = valid.filter((p) => p.frontier).sort((a, b) => a.effortPerMin - b.effortPerMin);
  if (front.length > 1) {
    ctx.strokeStyle = pal.green;
    ctx.lineWidth = 1.8;
    ctx.setLineDash([6, 4]);
    ctx.beginPath();
    front.forEach((p, i) => (i === 0 ? ctx.moveTo(sx(p.effortPerMin), sy(p.exceed)) : ctx.lineTo(sx(p.effortPerMin), sy(p.exceed))));
    ctx.stroke();
    ctx.setLineDash([]);
  }

  valid.forEach((p) => {
    const x = sx(p.effortPerMin);
    const y = sy(p.exceed);
    const isCurrent = p.name === currentName;
    ctx.fillStyle = p.frontier ? pal.green : pal.muted;
    ctx.beginPath();
    ctx.arc(x, y, isCurrent ? 7 : 5, 0, Math.PI * 2);
    ctx.fill();
    if (isCurrent) {
      ctx.strokeStyle = pal.blue;
      ctx.lineWidth = 2.5;
      ctx.beginPath();
      ctx.arc(x, y, 10, 0, Math.PI * 2);
      ctx.stroke();
    }
    ctx.fillStyle = pal.text;
    ctx.font = "11px Segoe UI, sans-serif";
    ctx.textAlign = "left";
    ctx.textBaseline = "middle";
    const label = p.name.length > 22 ? `${p.name.slice(0, 21)}…` : p.name;
    ctx.fillText(` ${label}`, x + 8, y);
  });
  canvas.__cursor = null;
}

function renderPareto(model, pal) {
  const points = [];
  if (model.rows.length && Number.isFinite(model.exceedanceDegSec)) {
    let effort = 0;
    let any = false;
    model.quality.forEach((q) => {
      if (Number.isFinite(q.effortPerMin)) {
        effort += q.effortPerMin;
        any = true;
      }
    });
    let fanLag = null;
    const lags = model.quality.map((q) => q.fanLag).filter(Number.isFinite);
    if (lags.length) {
      fanLag = lags.reduce((s, v) => s + v, 0) / lags.length;
    }
    points.push({
      name: state.csvName || "current run",
      rows: model.rows.length,
      effortPerMin: any ? effort : null,
      exceed: model.exceedanceDegSec,
      gpuPeak: model.gpuStats.max,
      fanLag,
    });
  }
  const currentName = points.length ? points[0].name : null;
  state.compare.forEach((c) => points.push(c.bundle));

  const ranked = paretoFrontier(points);
  drawPareto(els.paretoChart, ranked, currentName, pal);

  if (!ranked.length) {
    els.paretoRows.innerHTML = '<tr><td colspan="6" class="empty">Load one or more CSVs via "Compare CSVs".</td></tr>';
    els.paretoNote.textContent = 'Load "Compare CSVs" to plot runs';
    return;
  }
  const sorted = [...ranked].sort((a, b) => (b.frontier - a.frontier) || ((a.exceed ?? Infinity) - (b.exceed ?? Infinity)));
  els.paretoRows.innerHTML = sorted.map((p) => `
    <tr${p.name === currentName ? ' class="focus-row"' : ""}>
      <td>${escapeHtml(p.name)}${p.name === currentName ? " &#9673;" : ""}</td>
      <td>${formatNumber(p.effortPerMin, 1, "not enough data")}</td>
      <td>${p.exceed === null || p.exceed === undefined ? "no GPU limit" : formatInteger(p.exceed)}</td>
      <td>${formatUnit(p.gpuPeak, "C", 1, "missing GPU")}</td>
      <td>${p.fanLag === null || p.fanLag === undefined ? "not detected" : formatSeconds(p.fanLag)}</td>
      <td>${p.frontier ? "● frontier" : "—"}</td>
    </tr>
  `).join("");
  els.paretoNote.textContent = `${ranked.length} runs · ${ranked.filter((p) => p.frontier).length} on frontier`;
}

function populateChannelSelect(model) {
  const sel = els.focusChannel;
  const want = ["", ...model.channels.map(String)].join(",");
  if (sel.dataset.signature === want) {
    return;
  }
  sel.dataset.signature = want;
  const current = state.focusChannel;
  sel.innerHTML = '<option value="">auto</option>'
    + model.channels.map((c) => `<option value="${c}">Channel ${c}</option>`).join("");
  sel.value = current;
}

let lastPal = null;

function renderCharts(model, pal) {
  const tempSeries = seriesFromRows(model.rows, model.elapsed, [
    { key: "cpu_tctl_c", label: "CPU Tctl", color: pal.red, value: (row) => maybeNumber(row.cpu_tctl_c) },
    { key: "gpu_envelope_c", label: "GPU envelope", color: pal.violet, width: 3, value: gpuEnvelope },
    { key: "gpu_core_c", label: "GPU core", color: pal.teal, value: (row) => maybeNumber(row.gpu_core_c) },
    { key: "gpu_memjn_c", label: "GPU mem", color: pal.amber, value: (row) => maybeNumber(row.gpu_memjn_c) },
    { key: "gpu_hotspot_c", label: "GPU hotspot", color: pal.blue, value: (row) => maybeNumber(row.gpu_hotspot_c) },
  ]);
  safeDraw(els.temperatureChart, pal, "Temperature chart", () =>
    drawChart(els.temperatureChart, tempSeries, { emptyText: "Waiting for live CSV or choose Control CSV.", yMin: 0, axisX: "elapsed", axisY: "temperature (C)" }, pal));
  els.temperatureNote.textContent = model.rows.length
    ? `${model.rows.length} rows · peak GPU ${formatNumber(model.peak.value)} C`
    : "Waiting for live CSV";

  const cc = channelPalette(pal);
  const channelSeries = seriesFromRows(model.rows, model.elapsed, model.channels.map((channel, index) => ({
    key: `channel${channel}_setpoint_pct`,
    label: `Ch ${channel}`,
    color: cc[index % cc.length],
    value: (row) => maybeNumber(row[`channel${channel}_setpoint_pct`]),
  })));
  safeDraw(els.channelChart, pal, "Channel chart", () =>
    drawChart(els.channelChart, channelSeries, { emptyText: "Waiting for channel setpoint columns.", yMin: 0, axisX: "elapsed", axisY: "setpoint (%)" }, pal));
  els.channelNote.textContent = model.channels.length ? `${model.channels.length} active channels · hover to read values` : "No channel columns yet";

  safeDraw(els.phaseChart, pal, "Phase portrait", () => drawPhasePortrait(els.phaseChart, model, pal));
  safeDraw(els.decompChart, pal, "Reference chart", () => drawDecomposition(els.decompChart, model, pal));
  safeDraw(els.stepChart, pal, "Step response", () => drawStepGallery(els.stepChart, model, pal));
  safeDraw(els.psdChart, pal, "Spectrum", () => drawPsd(els.psdChart, model, pal));
  safeDraw(els.spectroChart, pal, "Spectrogram", () => drawSpectrogram(els.spectroChart, model, pal));
  try {
    renderPareto(model, pal);
  } catch (error) {
    const message = error && error.message ? error.message : String(error);
    emptyCanvas(els.paretoChart, pal, `Pareto render failed\n${message}`);
  }

  els.phaseNote.textContent = model.focus ? `Ch ${model.focus.channel} · temp → setpoint` : "No channel yet";
  els.decompNote.textContent = model.focus ? `Ch ${model.focus.channel} · feedforward / boost / dynamic` : "feedforward / boost / dynamic";
  const dom = model.focus?.spectral?.dom;
  els.psdNote.textContent = dom && dom.periodSec ? `dominant ≈ ${formatNumber(dom.periodSec)} s` : "no clear limit cycle";
  els.spectroNote.textContent = model.focus ? `Ch ${model.focus.channel} setpoint STFT` : "Setpoint STFT over the session";

  // Cache draw closures so the crosshair can repaint without recomputing.
  els.temperatureChart.__draw = () => drawChart(els.temperatureChart, tempSeries, { emptyText: "Waiting for live CSV or choose Control CSV.", yMin: 0, axisX: "elapsed", axisY: "temperature (C)" }, pal);
  els.channelChart.__draw = () => drawChart(els.channelChart, channelSeries, { emptyText: "Waiting for channel setpoint columns.", yMin: 0, axisX: "elapsed", axisY: "setpoint (%)" }, pal);
  els.decompChart.__draw = () => drawDecomposition(els.decompChart, model, pal);
}

function render() {
  const pal = readPalette();
  const model = buildModel({
    threshold: maybeNumber(els.gpuThreshold.value),
    cpuThreshold: maybeNumber(els.cpuThreshold.value),
    focusChannel: state.focusChannel === "" ? null : Number(state.focusChannel),
  });
  lastPal = pal;
  const loadedParts = [
    state.csvName ? `CSV: ${state.csvName}` : "",
    state.summaryName ? `JSON: ${state.summaryName}` : "",
    state.compare.length ? `compare: ${state.compare.length}` : "",
    state.events.length ? `events: ${state.events.length}` : "",
  ].filter(Boolean);
  els.runSubtitle.textContent = loadedParts.join(" | ") || "Load a control-loop CSV to inspect response over time.";
  const loaded = model.rows.length > 0;
  els.loadStatus.textContent = loaded ? "Run loaded" : (state.summary ? "Summary loaded" : "No run loaded");
  els.loadStatus.classList.toggle("is-loaded", loaded);
  populateChannelSelect(model);
  renderOverview(model);
  renderChannelTable(model);
  renderQuality(model);
  renderEvents(model);
  renderHistory();
  renderCharts(model, pal);
  renderStepTable(model);
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

/* ---- Crosshair (synced across the time-aligned charts) ------------------- */

function nearestPoint(points, x) {
  if (!points.length) {
    return null;
  }
  let lo = 0;
  let hi = points.length - 1;
  while (lo < hi) {
    const mid = (lo + hi) >> 1;
    if (points[mid].x < x) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  const a = points[Math.max(0, lo - 1)];
  const b = points[lo];
  if (!a) return b;
  if (!b) return a;
  return Math.abs(a.x - x) <= Math.abs(b.x - x) ? a : b;
}

function paintCursor(elapsed, hostCanvas, evt) {
  const tip = els.cursorTip;
  let tipRows = "";
  TIME_CHART_IDS.forEach((id) => {
    const c = els[id];
    if (!c || !c.__draw || !c.__cursor) {
      return;
    }
    c.__draw();
    const cur = c.__cursor;
    const ctx = c.getContext("2d");
    const x = Math.max(cur.plot.left, Math.min(cur.plot.right, cur.xToPx(elapsed)));
    ctx.save();
    ctx.strokeStyle = lastPal ? lastPal.cursor : "#334155";
    ctx.globalAlpha = 0.55;
    ctx.lineWidth = 1;
    ctx.setLineDash([4, 3]);
    ctx.beginPath();
    ctx.moveTo(x, cur.plot.top);
    ctx.lineTo(x, cur.plot.bottom);
    ctx.stroke();
    ctx.restore();
    if (c === hostCanvas) {
      const parts = [];
      ctx.save();
      cur.series.forEach((s) => {
        const pt = nearestPoint(s.points.filter((p) => Number.isFinite(p.y)), elapsed);
        if (!pt) {
          return;
        }
        const dotX = Math.max(cur.plot.left, Math.min(cur.plot.right, cur.xToPx(pt.x)));
        const dotY = cur.yToPx(pt.y);
        if (Number.isFinite(dotY)) {
          ctx.fillStyle = s.color;
          ctx.beginPath();
          ctx.arc(dotX, dotY, 3.4, 0, Math.PI * 2);
          ctx.fill();
        }
        parts.push(`<div class="tip-row"><span class="tip-key"><span class="swatch" style="background:${s.color}"></span>${escapeHtml(s.label)}</span><span class="tip-val">${formatNumber(pt.y)}</span></div>`);
      });
      ctx.restore();
      tipRows = parts.join("");
    }
  });
  if (evt && tipRows) {
    tip.innerHTML = `<div class="tip-time">t = ${formatSeconds(elapsed)}</div>${tipRows}`;
    const pad = 16;
    let left = evt.clientX + pad;
    let top = evt.clientY + pad;
    const rect = tip.getBoundingClientRect();
    if (left + rect.width > window.innerWidth - 8) {
      left = evt.clientX - rect.width - pad;
    }
    if (top + rect.height > window.innerHeight - 8) {
      top = evt.clientY - rect.height - pad;
    }
    tip.style.left = `${Math.max(8, left)}px`;
    tip.style.top = `${Math.max(8, top)}px`;
    tip.classList.add("show");
  }
}

function clearCursor() {
  els.cursorTip.classList.remove("show");
  TIME_CHART_IDS.forEach((id) => {
    const c = els[id];
    if (c && c.__draw) {
      c.__draw();
    }
  });
}

let cursorRaf = 0;
function attachCursor(canvas) {
  canvas.addEventListener("mousemove", (evt) => {
    if (!canvas.__cursor) {
      return;
    }
    const rect = canvas.getBoundingClientRect();
    const px = evt.clientX - rect.left;
    const cur = canvas.__cursor;
    const x = Math.max(cur.xMin, Math.min(cur.xMax, cur.pxToX(px)));
    if (cursorRaf) {
      cancelAnimationFrame(cursorRaf);
    }
    cursorRaf = requestAnimationFrame(() => paintCursor(x, canvas, evt));
  });
  canvas.addEventListener("mouseleave", () => {
    if (cursorRaf) {
      cancelAnimationFrame(cursorRaf);
      cursorRaf = 0;
    }
    clearCursor();
  });
}

/* ---- Theme --------------------------------------------------------------- */

function applyTheme(theme) {
  const dark = theme === "dark";
  document.documentElement.dataset.theme = dark ? "dark" : "light";
  if (els && els.themeLabel) {
    els.themeLabel.textContent = dark ? "Light" : "Dark";
    els.themeIcon.innerHTML = dark ? "&#9728;" : "&#9789;";
  }
  try {
    localStorage.setItem("svgmb-theme", dark ? "dark" : "light");
  } catch {
    /* ignore */
  }
}

/* ---- Runtime health (pure summary + render + poll) ---------------------- */

const HEALTH_CLASS = {
  healthy: "ok",
  degraded: "warn",
  stale: "warn",
  stopped: "muted",
  failed: "bad",
};

function formatAgeSeconds(value) {
  const seconds = Number(value);
  if (!Number.isFinite(seconds)) {
    return "";
  }
  if (seconds < 2) {
    return "now";
  }
  if (seconds < 60) {
    return `${Math.round(seconds)} s old`;
  }
  if (seconds < 3600) {
    return `${Math.round(seconds / 60)} min old`;
  }
  if (seconds < 86400) {
    return `${Math.round(seconds / 3600)} h old`;
  }
  return `${Math.round(seconds / 86400)} d old`;
}

function formatSizeBytes(value) {
  const bytes = Number(value);
  if (!Number.isFinite(bytes) || bytes < 0) {
    return "";
  }
  if (bytes < 1024) {
    return `${bytes} B`;
  }
  if (bytes < 1024 * 1024) {
    return `${(bytes / 1024).toFixed(1)} KB`;
  }
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

function describeHealthFile(meta) {
  if (!meta || typeof meta !== "object" || meta.exists !== true) {
    return "missing";
  }
  const parts = [];
  const age = formatAgeSeconds(meta.age_seconds);
  const size = formatSizeBytes(meta.size_bytes);
  if (age) {
    parts.push(age);
  }
  if (size) {
    parts.push(size);
  }
  return parts.length ? parts.join(", ") : "present";
}

function liveFileSignature(meta) {
  if (!meta || typeof meta !== "object" || meta.exists !== true) {
    return "";
  }
  return `${meta.modified_time || ""}|${meta.size_bytes ?? ""}`;
}

function liveMetadataSignature(payload) {
  const files = payload && payload.files && typeof payload.files === "object"
    ? payload.files
    : {};
  return {
    csv: liveFileSignature(files.live_csv),
    events: liveFileSignature(files.events),
  };
}

function commitLiveMetadataSignature(signature) {
  if (!signature || !signature.csv) {
    return;
  }
  state.live.csvSignature = signature.csv;
  state.live.eventsSignature = signature.events || "";
}

function liveMetadataChanged(signature) {
  if (!signature || !signature.csv || !state.live.autoRefresh || state.live.loading) {
    return false;
  }
  if (!state.live.csvSignature && !state.live.eventsSignature) {
    return !state.live.loadedOnce;
  }
  return (
    signature.csv !== state.live.csvSignature ||
    (signature.events || "") !== (state.live.eventsSignature || "")
  );
}

function disableLiveAutoRefresh() {
  state.live.autoRefresh = false;
  state.live.on = false;
  state.live.loading = false;
  state.live.generation += 1;
  state.live.csvSignature = null;
  state.live.eventsSignature = null;
}

function healthMetadataDetail(payload) {
  const detail = [];
  const push = (k, v) => {
    if (v !== null && v !== undefined && String(v) !== "") {
      detail.push({ k, v: String(v) });
    }
  };
  if (!payload || typeof payload !== "object") {
    return detail;
  }
  push("runtime home", payload.runtime_home);
  const files = payload.files && typeof payload.files === "object" ? payload.files : {};
  push("health file", describeHealthFile(files.control_health));
  push("runtime file", describeHealthFile(files.control_runtime));
  push("live CSV", describeHealthFile(files.live_csv));
  push("events", describeHealthFile(files.events));
  return detail;
}

function summarizeHealth(payload) {
  const detail = healthMetadataDetail(payload);
  if (!payload || payload.available === false) {
    return {
      state: "unknown",
      label: "unknown",
      cls: "muted",
      reason: "no runtime health files found",
      detail,
    };
  }
  const h = payload.health && typeof payload.health === "object" ? payload.health : {};
  const sup =
    payload.supervisor && typeof payload.supervisor === "object" ? payload.supervisor : {};
  const rt = payload.runtime && typeof payload.runtime === "object" ? payload.runtime : {};

  const rawState =
    typeof h.last_health_state === "string" && h.last_health_state
      ? h.last_health_state
      : "unknown";
  const push = (k, v) => {
    if (v !== null && v !== undefined && String(v) !== "") {
      detail.push({ k, v: String(v) });
    }
  };
  push("mode", rt.mode);
  push("runtime", rt.status_detail || rt.status);
  push("worker pid", rt.process_id);
  push("supervisor pid", sup.supervisor_pid);
  if (Number.isFinite(sup.worker_restart_count)) {
    push("restarts", sup.worker_restart_count);
  }
  if (sup.last_worker_exit_code !== null && sup.last_worker_exit_code !== undefined) {
    const at = sup.last_worker_exit_time ? ` @ ${sup.last_worker_exit_time}` : "";
    push("last worker exit", `${sup.last_worker_exit_code}${at}`);
  }
  push("last restore", rt.last_successful_restore_time);
  push("loop checked", rt.loop_last_evaluation);
  push("checked", h.last_health_time);
  const reason = typeof h.last_health_reason === "string" ? h.last_health_reason : "";

  return {
    state: rawState,
    label: rawState,
    cls: HEALTH_CLASS[rawState] || "muted",
    reason: reason || (payload.health ? "" : "runtime sidecar found; no persisted health check"),
    detail,
  };
}

function renderHealth(summary) {
  if (!els || !els.healthState) {
    return;
  }
  els.healthState.textContent = summary.label;
  els.healthState.className = `status-pill health-pill health-${summary.cls}`;
  els.healthReason.textContent = summary.reason || "";
  if (!summary.detail.length) {
    els.healthDetail.innerHTML =
      '<span class="health-empty">No runtime health data.</span>';
    return;
  }
  els.healthDetail.innerHTML = summary.detail
    .map(
      (d) =>
        `<span class="health-item"><b>${escapeHtml(d.k)}</b> ${escapeHtml(d.v)}</span>`,
    )
    .join("");
}

async function loadHealth() {
  if (
    !window.location ||
    (window.location.protocol !== "http:" && window.location.protocol !== "https:")
  ) {
    renderHealth(
      summarizeHealth({ available: false }),
    );
    return;
  }
  try {
    const text = await fetchTextIfOk(LIVE_HEALTH_URL);
    const payload = JSON.parse(text);
    renderHealth(summarizeHealth(payload));
    maybeRefreshLiveRun(payload);
  } catch {
    renderHealth({
      state: "unknown",
      label: "unavailable",
      cls: "muted",
      reason: "health endpoint unreachable",
      detail: [],
    });
  }
}

function maybeRefreshLiveRun(payload) {
  const signature = liveMetadataSignature(payload);
  if (!signature.csv || !state.live.autoRefresh) {
    return;
  }
  if (!state.live.csvSignature && !state.live.eventsSignature && state.live.loadedOnce) {
    commitLiveMetadataSignature(signature);
    return;
  }
  if (liveMetadataChanged(signature)) {
    loadLiveRun({
      preserveOnError: state.live.loadedOnce,
      signature,
      silent: state.live.loadedOnce,
    });
  }
}

async function loadLiveRun(options = {}) {
  if (!window.location || window.location.protocol !== "http:" && window.location.protocol !== "https:") {
    els.loadStatus.textContent = "Pick a CSV";
    els.runSubtitle.textContent = "Open through Start-EvalDashboard.ps1 for live auto-load.";
    return;
  }

  if (state.live.loading) {
    return;
  }

  const silent = options.silent === true;
  const preserveOnError = options.preserveOnError === true;
  const generation = state.live.generation;
  state.live.loading = true;
  if (!silent) {
    els.loadStatus.textContent = "Loading live";
    els.runSubtitle.textContent = "Loading recent live CSV rows...";
  }
  try {
    const csvText = await fetchTextIfOk(LIVE_CSV_URL);
    const rows = capLiveRows(parseCsv(csvText));
    if (!rows.length) {
      throw new Error("CSV has no rows yet");
    }
    if (generation !== state.live.generation || !state.live.autoRefresh) {
      return;
    }
    state.rows = rows;
    state.csvName = "live tail: release/runtime/logs/svg_mb_control_output.csv";
    state.live.on = true;
    state.live.loadedOnce = true;
    state.live.lastErr = "";
    commitLiveMetadataSignature(options.signature);

    try {
      state.events = parseJsonl(await fetchTextIfOk(LIVE_EVENTS_URL));
    } catch {
      state.events = [];
    }

    render();
  } catch (error) {
    if (generation !== state.live.generation || !state.live.autoRefresh) {
      return;
    }
    const message = error && error.message ? error.message : String(error);
    state.live.lastErr = message;
    if (!preserveOnError) {
      state.rows = [];
      state.live.on = false;
      els.loadStatus.textContent = "Pick a CSV";
      els.runSubtitle.textContent = `Live CSV unavailable: ${message}. Choose Control CSV manually.`;
      render();
    }
  } finally {
    if (generation === state.live.generation) {
      state.live.loading = false;
    }
  }
}

/* ---- Bootstrap (browser only) ------------------------------------------- */

if (typeof document !== "undefined") {
  els = {
    csvFile: document.getElementById("csvFile"),
    summaryFile: document.getElementById("summaryFile"),
    eventsFile: document.getElementById("eventsFile"),
    historyFiles: document.getElementById("historyFiles"),
    compareFiles: document.getElementById("compareFiles"),
    cpuThreshold: document.getElementById("cpuThreshold"),
    gpuThreshold: document.getElementById("gpuThreshold"),
    focusChannel: document.getElementById("focusChannel"),
    themeToggle: document.getElementById("themeToggle"),
    themeLabel: document.getElementById("themeLabel"),
    themeIcon: document.getElementById("themeIcon"),
    runSubtitle: document.getElementById("runSubtitle"),
    loadStatus: document.getElementById("loadStatus"),
    healthState: document.getElementById("healthState"),
    healthDetail: document.getElementById("healthDetail"),
    healthReason: document.getElementById("healthReason"),
    overviewMetrics: document.getElementById("overviewMetrics"),
    temperatureNote: document.getElementById("temperatureNote"),
    channelNote: document.getElementById("channelNote"),
    qualityNote: document.getElementById("qualityNote"),
    phaseNote: document.getElementById("phaseNote"),
    decompNote: document.getElementById("decompNote"),
    stepNote: document.getElementById("stepNote"),
    psdNote: document.getElementById("psdNote"),
    spectroNote: document.getElementById("spectroNote"),
    paretoNote: document.getElementById("paretoNote"),
    eventNote: document.getElementById("eventNote"),
    historyNote: document.getElementById("historyNote"),
    temperatureChart: document.getElementById("temperatureChart"),
    channelChart: document.getElementById("channelChart"),
    phaseChart: document.getElementById("phaseChart"),
    decompChart: document.getElementById("decompChart"),
    stepChart: document.getElementById("stepChart"),
    psdChart: document.getElementById("psdChart"),
    spectroChart: document.getElementById("spectroChart"),
    paretoChart: document.getElementById("paretoChart"),
    channelRows: document.getElementById("channelRows"),
    qualityRows: document.getElementById("qualityRows"),
    stepRows: document.getElementById("stepRows"),
    paretoRows: document.getElementById("paretoRows"),
    eventSummary: document.getElementById("eventSummary"),
    historyRows: document.getElementById("historyRows"),
    cursorTip: document.getElementById("cursorTip"),
  };

  let savedTheme = "light";
  try {
    savedTheme = localStorage.getItem("svgmb-theme") || "light";
  } catch {
    /* ignore */
  }
  applyTheme(savedTheme);

  els.csvFile.addEventListener("change", async (event) => {
    const file = event.target.files?.[0];
    if (!file) {
      return;
    }
    disableLiveAutoRefresh();
    state.rows = parseCsv(await readFile(file));
    state.csvName = file.name;
    render();
  });

  els.summaryFile.addEventListener("change", async (event) => {
    const file = event.target.files?.[0];
    if (!file) {
      return;
    }
    state.summary = JSON.parse(await readFile(file));
    state.summaryName = file.name;
    render();
  });

  els.eventsFile.addEventListener("change", async (event) => {
    const file = event.target.files?.[0];
    if (!file) {
      return;
    }
    disableLiveAutoRefresh();
    state.events = parseJsonl(await readFile(file));
    render();
  });

  els.historyFiles.addEventListener("change", async (event) => {
    const files = [...(event.target.files || [])];
    state.history = [];
    for (const file of files) {
      state.history.push({ name: file.name, summary: JSON.parse(await readFile(file)) });
    }
    render();
  });

  els.compareFiles.addEventListener("change", async (event) => {
    const files = [...(event.target.files || [])];
    state.compare = [];
    const threshold = maybeNumber(els.gpuThreshold.value);
    for (const file of files) {
      try {
        const rows = parseCsv(await readFile(file));
        if (rows.length) {
          state.compare.push({ name: file.name, bundle: runScalarBundle(rows, threshold, file.name) });
        }
      } catch {
        /* skip unreadable file */
      }
    }
    render();
  });

  els.cpuThreshold.addEventListener("input", render);
  els.gpuThreshold.addEventListener("input", render);
  els.focusChannel.addEventListener("change", () => {
    state.focusChannel = els.focusChannel.value;
    render();
  });
  els.themeToggle.addEventListener("click", () => {
    const next = document.documentElement.dataset.theme === "dark" ? "light" : "dark";
    applyTheme(next);
    render();
  });
  window.addEventListener("resize", () => {
    if (cursorRaf) {
      cancelAnimationFrame(cursorRaf);
      cursorRaf = 0;
    }
    render();
  });

  TIME_CHART_IDS.forEach((id) => attachCursor(els[id]));

  render();
  loadLiveRun();
  loadHealth();
  setInterval(loadHealth, HEALTH_POLL_MS);
}

if (typeof module !== "undefined" && module.exports) {
  module.exports = {
    parseCsv,
    parseJsonl,
    stats,
    percentile,
    capLiveRows,
    elapsedSeconds,
    detectChannels,
    finitePairs,
    resampleToGrid,
    detrend,
    crossCorrLag,
    totalVariation,
    reversals,
    hysteresisArea,
    degSecondsOver,
    fft,
    welchPSD,
    dominantPeriod,
    spectrogram,
    detectSteps,
    stepMetrics,
    channelGrids,
    channelQuality,
    focusDetail,
    runScalarBundle,
    paretoFrontier,
    computeModel,
    buildModel,
    summarizeHealth,
    formatAgeSeconds,
    liveMetadataSignature,
    liveMetadataChanged,
    state,
  };
}
