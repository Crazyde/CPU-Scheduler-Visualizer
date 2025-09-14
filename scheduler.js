// scheduler.js
// Frontend: collects inputs, calls backend, renders (Horizontal Gantt + Process Table + Ready Queue)

const BACKEND_ENDPOINT = "/schedule";

// Gantt sizing
const PIXELS_PER_UNIT = 36;
const MIN_BLOCK_PX = 72; // slightly smaller blocks for a tighter Gantt

// Color palette mapping (unique colors per process)
const COLOR_CLASSES = [
  "c-0","c-1","c-2","c-3","c-4","c-5","c-6","c-7","c-8","c-9"
];
function colorClassFor(name) {
  const s = String(name || "");
  const code = s.charCodeAt(0) || 0;
  const idx = Math.abs(code + s.length * 7) % COLOR_CLASSES.length;
  return COLOR_CLASSES[idx];
}

// Helpers
function parseNumberList(str) {
  if (!str) return [];
  return str.split(',').map(s => s.trim()).filter(Boolean).map(s => {
    const n = Number(s);
    return Number.isFinite(n) ? n : null;
  });
}
function sum(arr) { return (arr || []).reduce((a, b) => a + (Number(b) || 0), 0); }

// Main
async function solve() {
  clearStatus();

  const algorithm = document.getElementById('algorithm').value;
  const procCountRaw = document.getElementById('proc-count')?.value;
  const processCount = Number(procCountRaw);
  let arrival   = parseNumberList(document.getElementById('arrival').value);
  let burst     = parseNumberList(document.getElementById('burst').value);
  const priorityRaw = document.getElementById('priority')?.value || '';
  let priority  = parseNumberList(priorityRaw);
  const priorityOrder = document.getElementById('priority-order')?.value || 'lower';
  const quantum   = Number(document.getElementById('quantum')?.value) || 2;
  const operation = document.getElementById('operation')?.value || 'TRACE';

  const priorityNeeded = (algorithm === 'priority' || algorithm === 'ppriority' || algorithm === 'aging');

  // Validation
  if (!algorithm) {
    showStatus("❌ Please choose a scheduling algorithm.", true);
    return;
  }
  if (arrival.length === 0 || burst.length === 0) {
    showStatus("❌ Arrival and Burst must be provided.", true);
    return;
  }
  if (arrival.length !== burst.length) {
    showStatus("❌ Arrival and Burst arrays must have same length.", true);
    return;
  }
  if (priorityNeeded && priority.length && priority.length !== arrival.length) {
    showStatus("❌ Priority array length must match arrival/burst.", true);
    return;
  }
  if (Number.isFinite(processCount) && processCount > 0) {
    if (arrival.length !== processCount || burst.length !== processCount) {
      showStatus(`❌ Please provide exactly ${processCount} arrivals and bursts.`, true);
      return;
    }
    if (priorityNeeded && priority.length && priority.length !== processCount) {
      showStatus(`❌ Provide ${processCount} priorities or leave blank.`, true);
      return;
    }
  }

  // Ensure all burst times are positive numbers (avoid NaN in backend stats)
  for (let i = 0; i < burst.length; i++) {
    const b = Number(burst[i]);
    if (!Number.isFinite(b) || b <= 0) {
      showStatus("❌ Each Burst must be a positive number (greater than 0).", true);
      return;
    }
  }

  const totalBurst = sum(burst);
  const lastInstantHint = totalBurst + Math.max(0, Math.min(...arrival));

  // Build backend input
  let inputStr = "trace\n";
  let algoStr  = "";
  switch (algorithm) {
    case 'fcfs': algoStr = "1"; break;
    case 'sjf':  algoStr = "3"; break;
    case 'srtf': algoStr = "4"; break;
    case 'rr':   algoStr = `2-${quantum}`; break;
    case 'priority':  algoStr = "9"; break;
    case 'ppriority': algoStr = "A"; break;
    case 'hrrn': algoStr = "7"; break;
    case 'mlfq': algoStr = "8"; break;
    case 'mlfqexp': algoStr = "M"; break;
    case 'aging':    algoStr = "L"; break;
    default: algoStr = "1";
  }
  inputStr += algoStr + "\n";
  inputStr += String(lastInstantHint) + "\n";
  inputStr += arrival.length + "\n";
  inputStr += (priorityOrder === 'higher' ? "higher\n" : "lower\n");

  for (let i = 0; i < arrival.length; i++) {
    const name = String.fromCharCode(65 + i);
    const arr  = arrival[i];
    const bur  = burst[i];
    const prio = (priority[i] != null) ? priority[i] : 0;
    if (priorityNeeded) {
      inputStr += `${name},${arr},${bur},${prio}\n`;
    } else {
      inputStr += `${name},${arr},${bur}\n`;
    }
  }

  showStatus("Sending request to backend...", false);
  try {
    const res = await fetch(BACKEND_ENDPOINT, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ input: inputStr })
    });
    if (!res.ok) {
      const text = await res.text();
      showStatus(`Backend error: ${res.status} ${res.statusText} - ${text}`, true);
      return;
    }

    const data = await res.json();
    const output = (data.output !== undefined) ? String(data.output).trim() : '';
    const stats = !output
      ? (data && typeof data === 'object' ? data : null)
      : (() => { try { return JSON.parse(output); } catch { return null; } })();

    if (!stats) {
      showStatus("⚠️ Unexpected backend response.", true);
      document.getElementById('output').textContent = JSON.stringify(data, null, 2);
      return;
    }

    renderStatsTable(stats, operation, { totalBurst });
    showStatus("✅ Result received", false);
  } catch (err) {
    showStatus("❌ Network or CORS error: " + (err.message || err), true);
    document.getElementById('output').innerText = '';
  }
}

// Helpers for rendering
function trimTrailingIdle(ganttArray) {
  if (!Array.isArray(ganttArray)) return [];
  let end = ganttArray.length - 1;
  while (end >= 0) {
    const v = ganttArray[end];
    if (v === -1 || (typeof v === 'string' && v.toLowerCase() === 'idle')) end--;
    else break;
  }
  return ganttArray.slice(0, end + 1);
}

function buildBlocksFromGantt(ganttArray) {
  const arr = trimTrailingIdle(ganttArray);
  const blocks = [];
  if (!Array.isArray(arr) || arr.length === 0) return blocks;
  let last = arr[0], start = 0;
  for (let i = 1; i < arr.length; i++) {
    if (arr[i] !== last) {
      blocks.push({ name: last, start, end: i });
      last = arr[i];
      start = i;
    }
  }
  blocks.push({ name: last, start, end: arr.length });
  return blocks;
}

function countIdleSinceZero(ganttArray) {
  const arr = trimTrailingIdle(ganttArray || []);
  let idle = 0;
  for (const v of arr) {
    if (v === -1) { idle++; continue; }
    if (typeof v === 'string' && v.toLowerCase() === 'idle') { idle++; }
  }
  return idle;
}

function renderHorizontalGantt(stats, meta = {}) {
  if (!stats.gantt || !Array.isArray(stats.gantt)) return '';
  const names = Array.isArray(stats.processes)
    ? stats.processes.map(p => p.name)
    : null;

  const blocks = buildBlocksFromGantt(stats.gantt);
  if (blocks.length === 0) return '';

  const completion = blocks[blocks.length - 1].end;
  const sumBurst = meta.totalBurst ?? null;
  const idleTime = countIdleSinceZero(stats.gantt);

  let html = '<div class="gantt horizontal-gantt"><div class="gantt-title">Gantt Chart</div>';
  html += '<div class="gantt-row">';
  for (const b of blocks) {
    const rawName = (typeof b.name === 'number' && names && names[b.name] != null)
      ? names[b.name]
      : b.name;
    const isIdle = (rawName === -1) || (String(rawName).toLowerCase() === 'idle');
    const displayName = isIdle ? 'I' : String(rawName);
    const duration = Math.max(1, b.end - b.start);
    const px = Math.max(duration * PIXELS_PER_UNIT, MIN_BLOCK_PX);
    const colorClass = isIdle ? '' : colorClassFor(displayName);
    // Show ranges as start-end using the right boundary 'end' so blocks read continuously
    // e.g., 0-2 followed by 2-3 (no visual off-by-one)
    const range = `${b.start}-${b.end}`;

    const narrow = px < 90;
    html += `<div class="gantt-box ${isIdle ? 'idle' : colorClass} ${narrow ? 'narrow' : ''}" style="width:${px}px">
      <div class="gantt-label" title="${displayName==='I'?'Idle':escapeHtml(displayName)}">${escapeHtml(displayName)}</div>
      <div class="gantt-time">${escapeHtml(range)}</div>
    </div>`;
  }
  html += '</div>';
  html += `<div class="gantt-meta">
      <span><strong>CT_last:</strong> ${completion}</span>
      ${sumBurst != null ? `<span><strong>Sum(BT):</strong> ${sumBurst}</span>` : ''}
      <span><strong>Idle time (gaps):</strong> ${idleTime}</span>
      <span class="hint">Total time = Sum(BT) + idle gaps. If Idle = 0, there are no gaps and CT_last = Sum(BT).</span>
    </div>`;
  html += '</div>';
  return html;
}

function renderReadyQueue(stats) {
  const ganttTrim = trimTrailingIdle(stats.gantt || []);
  const rq = Array.isArray(stats.readyQueues)
    ? stats.readyQueues.slice(0, ganttTrim.length)
    : [];

  if (ganttTrim.length === 0) return '';

  let html = '<div class="readyq">';
  html += '<div class="readyq-title">Ready Queue (per time unit)</div>';
  html += '<div class="readyq-strip">';
  for (let t = 0; t < ganttTrim.length; t++) {
    const running = ganttTrim[t];
    const isIdleRun = (running === -1) || (typeof running === 'string' && running.toLowerCase() === 'idle');
    const runningLabel = isIdleRun
      ? 'I'
      : ((Array.isArray(stats.processes) && typeof running === 'number' && stats.processes[running])
          ? stats.processes[running].name
          : String(running));

    const waiting = (rq[t] || []).filter(n => String(n) !== String(runningLabel));
    const items = waiting.map(n => {
      const label = (n === -1 || String(n).toLowerCase() === 'idle') ? 'I' : String(n);
      const cc = label === 'I' ? '' : colorClassFor(label);
      return `<span class="rq-badge ${cc}" title="Ready (waiting) at t=${t}">${escapeHtml(label)}</span>`;
    }).join('');

    html += `<div class="readyq-col" style="width:${PIXELS_PER_UNIT}px">
               <div class="rq-top">
                 <span class="rq-time">t=${t}</span>
                 <span class="rq-running" title="Running at t=${t}">${escapeHtml(runningLabel)}</span>
               </div>
               <div class="rq-items">${items || '<span class="rq-empty">—</span>'}</div>
             </div>`;
  }
  html += '</div></div>';
  return html;
}

function renderGanttScale(completion) {
  if (!Number.isFinite(completion) || completion <= 0) return '';
  let html = '<div class="gantt-scale"><div class="gantt-scale-row">';
  for (let t = 0; t <= completion; t++) {
    html += `<div class="tick" style="width:${PIXELS_PER_UNIT}px"><span class="tick-label">${t}</span></div>`;
  }
  html += '</div></div>';
  return html;
}

function renderStatsTable(stats, operation = "TRACE", meta = {}) {
  let html = '';
  if (operation === "TRACE") {
    html += renderHorizontalGantt(stats, meta);
    html += renderReadyQueue(stats);
  }
  html += '<h3>Process Table</h3>';
  html += '<table class="process-table"><thead><tr><th>Job</th><th>Arrival (AT)</th><th>Burst (BT)</th><th>Priority</th><th>Finish (CT)</th><th>Turnaround (TAT)</th><th>Waiting (WT)</th><th>Response (RT)</th></tr></thead><tbody>';
  if (Array.isArray(stats.processes)) {
    for (const p of stats.processes) {
      html += `<tr>
        <td>${escapeHtml(String(p.name ?? '-'))}</td>
        <td>${escapeHtml(String(p.arrival ?? '-'))}</td>
        <td>${escapeHtml(String(p.service ?? '-'))}</td>
        <td>${escapeHtml(String(p.priority ?? '-'))}</td>
        <td>${escapeHtml(String(p.finish ?? '-'))}</td>
        <td>${escapeHtml(String(p.tat ?? '-'))}</td>
        <td>${escapeHtml(String(p.wait ?? '-'))}</td>
        <td>${escapeHtml(String(p.resp ?? '-'))}</td>
      </tr>`;
    }
  } else {
    html += '<tr><td colspan="8">No process data available</td></tr>';
  }
  html += '</tbody></table>';
  if (stats.averages) {
    const a = stats.averages;
    html += `<div class='averages'><b>Average TAT:</b> ${formatNumber(a.tat)} &nbsp; | &nbsp; <b>Average WT:</b> ${formatNumber(a.wait)} &nbsp; | &nbsp; <b>Average RT:</b> ${formatNumber(a.resp)}</div>`;
  }
  html += `
    <div class="legend">
      <div class="legend-title">Abbreviations</div>
      <ul>
        <li><b>AT</b> — Arrival Time</li>
        <li><b>BT</b> — Burst Time</li>
        <li><b>CT</b> — Completion Time</li>
        <li><b>TAT</b> — Turnaround Time (= CT − AT)</li>
        <li><b>WT</b> — Waiting Time (= TAT − BT)</li>
        <li><b>RT</b> — Response Time (first start − AT)</li>
        <li><b>I</b> — Idle (no process running)</li>
      </ul>
      <div class="legend-note">In the Ready Queue, the top pill shows the <b>Running</b> process; the colored badges below are the processes waiting at that time.</div>
    </div>
  `;
  document.getElementById('output').innerHTML = html;
}

function formatNumber(v) {
  return (typeof v === 'number' && Number.isFinite(v)) ? v.toFixed(2) : '-';
}
function escapeHtml(str) {
  return String(str).replace(/[&<>"'`]/g, m => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;','`':'&#96;'})[m]);
}
function showStatus(msg, isError=false) {
  const el = document.getElementById('status'); if (!el) return;
  el.textContent = msg; el.style.color = isError ? '#fca5a5' : '#94a3b8';
}
function clearStatus() {
  const el = document.getElementById('status'); if (!el) return; el.textContent = '';
}

// Initial welcome content for the output pane
function renderWelcome() {
  const el = document.getElementById('output');
  if (!el) return;
  el.innerHTML = `
    <div class="welcome">
      <div class="welcome-icon">⚡</div>
      <div class="welcome-title">CPU Scheduling Visualizer</div>
      <div class="welcome-sub">Select <span class="wel-chip">algo</span> → set <span class="wel-chip">#proc</span> → enter <span class="wel-chip">AT/BT</span> <span class="wel-chip opt">/ PRI</span> → <b>Run</b>.</div>
    </div>
  `;
}
