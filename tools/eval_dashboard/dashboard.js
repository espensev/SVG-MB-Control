"use strict";

const CHANNEL_SETPOINT_RE = /^channel(\d+)_setpoint_pct$/;
const ISSUE_EVENTS = new Set([
  "control_loop.policy_refused",
  "control_loop.write_failed",
  "control_loop.sensor_failure",
  "control_loop.circuit_breaker_opened",
  "runtime.sidecar_warning",
]);

const state = {
  rows: [],
  events: [],
  summary: null,
  history: [],
  csvName: "",
  summaryName: "",
};

const els = {
  csvFile: document.getElementById("csvFile"),
  summaryFile: document.getElementById("summaryFile"),
  eventsFile: document.getElementById("eventsFile"),
  historyFiles: document.getElementById("historyFiles"),
  gpuThreshold: document.getElementById("gpuThreshold"),
  runSubtitle: document.getElementById("runSubtitle"),
  loadStatus: document.getElementById("loadStatus"),
  overviewMetrics: document.getElementById("overviewMetrics"),
  temperatureNote: document.getElementById("temperatureNote"),
  channelNote: document.getElementById("channelNote"),
  eventNote: document.getElementById("eventNote"),
  historyNote: document.getElementById("historyNote"),
  temperatureChart: document.getElementById("temperatureChart"),
  channelChart: document.getElementById("channelChart"),
  channelRows: document.getElementById("channelRows"),
  eventSummary: document.getElementById("eventSummary"),
  historyRows: document.getElementById("historyRows"),
};

function readFile(file) {
  return new Promise((resolve, reject) => {
    const reader = new FileReader();
    reader.onload = () => resolve(String(reader.result || ""));
    reader.onerror = () => reject(reader.error);
    reader.readAsText(file);
  });
}

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

function formatNumber(value, digits = 1) {
  if (value === null || value === undefined || !Number.isFinite(value)) {
    return "n/a";
  }
  return value.toFixed(digits);
}

function formatInteger(value) {
  if (value === null || value === undefined || !Number.isFinite(value)) {
    return "n/a";
  }
  return Math.round(value).toLocaleString();
}

function formatSeconds(value) {
  if (value === null || value === undefined || !Number.isFinite(value)) {
    return "n/a";
  }
  if (value >= 600) {
    return `${(value / 60).toFixed(1)} min`;
  }
  return `${value.toFixed(1)} s`;
}

function percentile(values, pct) {
  if (!values.length) {
    return null;
  }
  const sorted = [...values].sort((a, b) => a - b);
  if (sorted.length === 1) {
    return sorted[0];
  }
  const position = (sorted.length - 1) * (pct / 100);
  const lower = Math.floor(position);
  const upper = Math.ceil(position);
  if (lower === upper) {
    return sorted[lower];
  }
  const fraction = position - lower;
  return sorted[lower] + (sorted[upper] - sorted[lower]) * fraction;
}

function stats(values) {
  const clean = values.filter((value) => Number.isFinite(value));
  if (!clean.length) {
    return { count: 0, min: null, p50: null, p90: null, p95: null, p99: null, max: null, avg: null };
  }
  return {
    count: clean.length,
    min: Math.min(...clean),
    p50: percentile(clean, 50),
    p90: percentile(clean, 90),
    p95: percentile(clean, 95),
    p99: percentile(clean, 99),
    max: Math.max(...clean),
    avg: clean.reduce((sum, value) => sum + value, 0) / clean.length,
  };
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

function eventCounts(events) {
  const counts = new Map();
  events.forEach((event) => {
    const type = String(event.event_type || "unknown");
    counts.set(type, (counts.get(type) || 0) + 1);
  });
  return [...counts.entries()].sort((a, b) => b[1] - a[1] || a[0].localeCompare(b[0]));
}

function buildModel() {
  const rows = state.rows;
  const threshold = maybeNumber(els.gpuThreshold.value);
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
    const boosts = column(rows, `channel${channel}_thermal_pressure_boost_pct`);
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

  return {
    rows,
    elapsed,
    intervals,
    duration,
    channels,
    channelSummaries,
    threshold,
    gpuValues,
    gpuStats: stats(gpuClean),
    cpuStats: stats(column(rows, "cpu_tctl_c")),
    intervalStats: stats(column(rows, "loop_achieved_interval_ms")),
    workStats: stats(column(rows, "loop_work_duration_ms")),
    loopOverruns,
    peak: {
      index: peakIndex,
      value: peakValue,
      elapsed: peakIndex >= 0 ? elapsed[peakIndex] : null,
    },
    thresholdSummary: {
      rows: aboveThresholdRows,
      seconds: aboveThresholdSeconds,
      timeToThreshold,
    },
    eventCounts: eventCounts(state.events),
  };
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
      renderMetric("Duration", "n/a", "CSV not loaded"),
      renderMetric("CPU max", "n/a", "CSV not loaded"),
      renderMetric("GPU envelope", "n/a", "CSV not loaded"),
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

  els.overviewMetrics.innerHTML = [
    renderMetric("Rows", formatInteger(rows), `${model.channels.length} channels`),
    renderMetric("Duration", formatSeconds(duration), state.csvName || state.summaryName || ""),
    renderMetric("CPU max", `${formatNumber(cpuMax)} C`, `p90 ${formatNumber(cpuP90)} C`),
    renderMetric("GPU envelope", `${formatNumber(gpuMax)} C`, `p90 ${formatNumber(gpuP90)} C`),
    renderMetric("GPU peak time", formatSeconds(model.peak.elapsed ?? state.summary?.gpu_response?.peak?.elapsed_seconds), "from run start"),
    renderMetric("GPU threshold", formatSeconds(thresholdSeconds), `${formatNumber(model.threshold)} C`),
    renderMetric("Loop p95", `${formatNumber(model.intervalStats.p95)} ms`, `${model.loopOverruns} overruns`),
    renderMetric("Loop work p95", `${formatNumber(model.workStats.p95)} ms`, "controller cost"),
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
      <td>${formatNumber(item.setpointStats.p50)}%</td>
      <td>${formatNumber(item.setpointStats.p90)}%</td>
      <td>${formatNumber(item.setpointStats.max)}%</td>
      <td>${formatNumber(item.setpointAtPeak)}%</td>
      <td>${formatNumber(item.boostStats.max)}%</td>
      <td>${formatInteger(item.writes)}</td>
      <td>${formatNumber(item.writesPerMinute)}</td>
    </tr>
  `).join("");
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

  els.historyRows.innerHTML = summaries
    .map((item) => summaryHistoryRow(item.summary, item.name))
    .join("");
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

function drawChart(canvas, series, options) {
  const ctx = canvas.getContext("2d");
  const rect = canvas.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  const width = Math.max(320, Math.floor(rect.width));
  const height = Math.max(220, Number(canvas.getAttribute("height")) || Math.floor(rect.height));
  canvas.width = Math.floor(width * dpr);
  canvas.height = Math.floor(height * dpr);
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.clearRect(0, 0, width, height);
  ctx.fillStyle = "#fbfcfd";
  ctx.fillRect(0, 0, width, height);

  if (!series.length) {
    ctx.fillStyle = "#687385";
    ctx.font = "14px Segoe UI, sans-serif";
    ctx.fillText(options.emptyText || "No data", 18, 32);
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

  const plot = { left: 54, right: width - 18, top: 42, bottom: height - 34 };
  const xScale = (x) => plot.left + (x / maxX) * (plot.right - plot.left);
  const yScale = (y) => plot.bottom - ((y - minY) / (maxY - minY)) * (plot.bottom - plot.top);

  ctx.strokeStyle = "#d8dee8";
  ctx.lineWidth = 1;
  ctx.fillStyle = "#687385";
  ctx.font = "11px Segoe UI, sans-serif";
  ctx.textAlign = "right";
  ctx.textBaseline = "middle";

  for (let i = 0; i <= 4; i += 1) {
    const t = i / 4;
    const y = plot.top + t * (plot.bottom - plot.top);
    const value = maxY - t * (maxY - minY);
    ctx.beginPath();
    ctx.moveTo(plot.left, y);
    ctx.lineTo(plot.right, y);
    ctx.stroke();
    ctx.fillText(formatNumber(value, 0), plot.left - 8, y);
  }

  ctx.textAlign = "center";
  ctx.textBaseline = "top";
  for (let i = 0; i <= 4; i += 1) {
    const t = i / 4;
    const x = plot.left + t * (plot.right - plot.left);
    const value = maxX * t;
    ctx.beginPath();
    ctx.moveTo(x, plot.bottom);
    ctx.lineTo(x, plot.bottom + 4);
    ctx.stroke();
    ctx.fillText(formatSeconds(value), x, plot.bottom + 8);
  }

  series.forEach((item) => {
    ctx.strokeStyle = item.color;
    ctx.lineWidth = item.width || 2;
    ctx.beginPath();
    item.points.forEach((point, index) => {
      const x = xScale(point.x);
      const y = yScale(point.y);
      if (index === 0) {
        ctx.moveTo(x, y);
      } else {
        ctx.lineTo(x, y);
      }
    });
    ctx.stroke();
  });

  let legendX = plot.left;
  let legendY = 14;
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
    ctx.fillStyle = "#18202c";
    ctx.fillText(item.label, legendX + 20, legendY);
    legendX += labelWidth;
  });
}

function renderCharts(model) {
  const tempSeries = seriesFromRows(model.rows, model.elapsed, [
    { key: "cpu_tctl_c", label: "CPU Tctl", color: "#c83f3f", value: (row) => maybeNumber(row.cpu_tctl_c) },
    { key: "gpu_envelope_c", label: "GPU envelope", color: "#7251b5", width: 3, value: gpuEnvelope },
    { key: "gpu_core_c", label: "GPU core", color: "#137b75", value: (row) => maybeNumber(row.gpu_core_c) },
    { key: "gpu_memjn_c", label: "GPU mem", color: "#a66512", value: (row) => maybeNumber(row.gpu_memjn_c) },
    { key: "gpu_hotspot_c", label: "GPU hotspot", color: "#2864c9", value: (row) => maybeNumber(row.gpu_hotspot_c) },
  ]);
  drawChart(els.temperatureChart, tempSeries, { emptyText: "Load a control-loop CSV.", yMin: 0 });
  els.temperatureNote.textContent = model.rows.length
    ? `${model.rows.length} rows, peak GPU ${formatNumber(model.peak.value)} C`
    : "CSV required";

  const channelColors = ["#2864c9", "#137b75", "#a66512", "#7251b5", "#c83f3f", "#277447", "#56616f", "#8a4b20"];
  const channelSeries = seriesFromRows(model.rows, model.elapsed, model.channels.map((channel, index) => ({
    key: `channel${channel}_setpoint_pct`,
    label: `Ch ${channel}`,
    color: channelColors[index % channelColors.length],
    value: (row) => maybeNumber(row[`channel${channel}_setpoint_pct`]),
  })));
  drawChart(els.channelChart, channelSeries, { emptyText: "Load a control-loop CSV.", yMin: 0 });
  els.channelNote.textContent = model.channels.length
    ? `${model.channels.length} active channel columns`
    : "CSV required";
}

function render() {
  const model = buildModel();
  const loadedParts = [
    state.csvName ? `CSV: ${state.csvName}` : "",
    state.summaryName ? `JSON: ${state.summaryName}` : "",
    state.events.length ? `events: ${state.events.length}` : "",
  ].filter(Boolean);
  els.runSubtitle.textContent = loadedParts.join(" | ") || "Load a control-loop CSV to inspect response over time.";
  els.loadStatus.textContent = model.rows.length ? "Run loaded" : (state.summary ? "Summary loaded" : "No run loaded");
  renderOverview(model);
  renderChannelTable(model);
  renderEvents(model);
  renderHistory();
  renderCharts(model);
}

function escapeHtml(value) {
  return String(value)
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll('"', "&quot;")
    .replaceAll("'", "&#39;");
}

els.csvFile.addEventListener("change", async (event) => {
  const file = event.target.files?.[0];
  if (!file) {
    return;
  }
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
  state.events = parseJsonl(await readFile(file));
  render();
});

els.historyFiles.addEventListener("change", async (event) => {
  const files = [...(event.target.files || [])];
  state.history = [];
  for (const file of files) {
    state.history.push({
      name: file.name,
      summary: JSON.parse(await readFile(file)),
    });
  }
  render();
});

els.gpuThreshold.addEventListener("input", render);
window.addEventListener("resize", render);

render();
