const svg = document.getElementById('graph');
const subtitle = document.getElementById('subtitle');
const statusEl = document.getElementById('status');
const summaryEl = document.getElementById('summary');
const topicsEl = document.getElementById('topics');
const diagnosticsEl = document.getElementById('diagnostics');

let selected = null;

function layout(snapshot) {
  const width = svg.clientWidth || 900;
  const height = svg.clientHeight || 600;
  const groups = {
    router: [],
    peer: [],
    client: [],
    transport: [],
    publisher: [],
    subscriber: [],
    topic: [],
    other: [],
  };
  for (const node of snapshot.nodes) {
    if (groups[node.kind]) groups[node.kind].push(node);
    else groups.other.push(node);
  }
  const columns = [
    ['router', 0.12],
    ['peer', 0.25],
    ['client', 0.34],
    ['transport', 0.44],
    ['publisher', 0.55],
    ['topic', 0.70],
    ['subscriber', 0.86],
    ['other', 0.50],
  ];
  const positions = new Map();
  for (const [kind, xFrac] of columns) {
    const nodes = groups[kind] || [];
    const step = height / (nodes.length + 1);
    nodes.forEach((node, index) => {
      positions.set(node.id, {
        x: Math.max(70, Math.min(width - 140, width * xFrac)),
        y: Math.max(48, Math.min(height - 48, step * (index + 1))),
      });
    });
  }
  return positions;
}

function draw(snapshot) {
  const width = svg.clientWidth || 900;
  const height = svg.clientHeight || 600;
  const positions = layout(snapshot);
  svg.setAttribute('viewBox', `0 0 ${width} ${height}`);
  svg.innerHTML = `
    <defs>
      <marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="6" markerHeight="6" orient="auto-start-reverse">
        <path d="M 0 0 L 10 5 L 0 10 z" fill="#7b8490"></path>
      </marker>
    </defs>
  `;

  for (const link of snapshot.links) {
    const a = positions.get(link.source);
    const b = positions.get(link.target);
    if (!a || !b) continue;
    const line = document.createElementNS('http://www.w3.org/2000/svg', 'line');
    line.setAttribute('x1', a.x);
    line.setAttribute('y1', a.y);
    line.setAttribute('x2', b.x);
    line.setAttribute('y2', b.y);
    line.setAttribute('class', `link ${link.kind}`);
    svg.appendChild(line);
  }

  for (const node of snapshot.nodes) {
    const pos = positions.get(node.id);
    if (!pos) continue;
    const g = document.createElementNS('http://www.w3.org/2000/svg', 'g');
    g.setAttribute('class', `node ${node.kind}${node.stale ? ' stale' : ''}`);
    g.setAttribute('transform', `translate(${pos.x}, ${pos.y})`);
    g.style.cursor = 'pointer';
    g.onclick = () => {
      selected = node.id;
      renderSidebar(snapshot);
    };

    const circle = document.createElementNS('http://www.w3.org/2000/svg', 'circle');
    circle.setAttribute('r', node.kind === 'topic' ? 17 : 14);
    g.appendChild(circle);

    const text = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    text.setAttribute('x', 24);
    text.setAttribute('y', -2);
    text.textContent = compact(node.label, 32);
    g.appendChild(text);

    const detail = document.createElementNS('http://www.w3.org/2000/svg', 'text');
    detail.setAttribute('class', 'detail');
    detail.setAttribute('x', 24);
    detail.setAttribute('y', 13);
    detail.textContent = node.kind === 'topic'
      ? `${node.messages} msg, ${node.rate_hz.toFixed(1)} Hz`
      : compact(node.detail, 34);
    g.appendChild(detail);

    svg.appendChild(g);
  }
}

function renderSidebar(snapshot) {
  subtitle.textContent = `${snapshot.observed_keyexpr} via ${snapshot.connect}`;
  const totalMessages = snapshot.topics.reduce((sum, topic) => sum + topic.messages, 0);
  const totalBytes = snapshot.topics.reduce((sum, topic) => sum + topic.bytes, 0);
  summaryEl.innerHTML = `
    <div><span class="pill">${snapshot.nodes.length} nodes</span><span class="pill">${snapshot.links.length} links</span></div>
    <div><span class="pill">${snapshot.topics.length} topics</span><span class="pill">${totalMessages} messages</span><span class="pill">${formatBytes(totalBytes)}</span></div>
    <div><span class="pill">${snapshot.admin_entries} admin records</span><span class="pill">uptime ${(snapshot.uptime_ms / 1000).toFixed(1)}s</span></div>
  `;

  const rows = snapshot.topics
    .slice()
    .sort((a, b) => b.messages - a.messages || a.topic.localeCompare(b.topic))
    .map(topic => `
      <tr>
        <td><code>${escapeHtml(topic.topic)}</code><br><span class="muted">${escapeHtml(topic.root_type || 'unknown')}</span></td>
        <td>${topic.messages}</td>
        <td>${topic.rate_hz_5s.toFixed(1)}</td>
        <td>${formatBytes(topic.bytes)}</td>
      </tr>
    `).join('');
  topicsEl.innerHTML = `
    <table>
      <thead><tr><th>Topic</th><th>Msg</th><th>Hz</th><th>Bytes</th></tr></thead>
      <tbody>${rows || '<tr><td colspan="4" class="warn">No traffic observed yet.</td></tr>'}</tbody>
    </table>
  `;

  const selectedNode = snapshot.nodes.find(node => node.id === selected);
  diagnosticsEl.innerHTML = `
    ${snapshot.last_error ? `<div class="error">${escapeHtml(snapshot.last_error)}</div>` : ''}
    ${snapshot.admin_error ? `<div class="warn">${escapeHtml(snapshot.admin_error)}</div>` : ''}
    ${selectedNode ? `
      <h2>Selected</h2>
      <div><code>${escapeHtml(selectedNode.label)}</code></div>
      <div>${escapeHtml(selectedNode.kind)}: ${escapeHtml(selectedNode.detail)}</div>
      <div>${selectedNode.messages} messages, ${formatBytes(selectedNode.bytes)}</div>
    ` : '<div class="muted">Click a node for details.</div>'}
  `;
}

async function refresh() {
  try {
    const response = await fetch('/api/graph', { cache: 'no-store' });
    const snapshot = await response.json();
    statusEl.textContent = 'live';
    statusEl.className = '';
    draw(snapshot);
    renderSidebar(snapshot);
  } catch (error) {
    statusEl.textContent = `disconnected: ${error}`;
    statusEl.className = 'error';
  }
}

function compact(text, limit) {
  text = String(text || '');
  return text.length <= limit ? text : `${text.slice(0, limit - 1)}…`;
}

function formatBytes(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KiB`;
  return `${(bytes / 1024 / 1024).toFixed(1)} MiB`;
}

function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>"']/g, ch => ({
    '&': '&amp;',
    '<': '&lt;',
    '>': '&gt;',
    '"': '&quot;',
    "'": '&#39;',
  }[ch]));
}

window.addEventListener('resize', refresh);
setInterval(refresh, 1000);
refresh();
