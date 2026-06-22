// Afstandsbediening (kiosk-pagina): configureerbare knoppen, live status via WS.

const RC = window.__RC__ || { slug: "", knoppen: [] };
const TOKEN = window.__TOKEN__ || "";
const grid = document.getElementById("rc-grid");
const leeg = document.getElementById("rc-leeg");
const statusEl = document.getElementById("rc-status");

// device-id -> {state, online}
const states = {};

function escapeHtml(s) {
  return String(s == null ? "" : s).replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

// Een knop is "aan" als al zijn targets aan staan; "online" als minstens één
// target online is.
function knopAan(knop) {
  const t = knop.targets || [];
  return t.length > 0 && t.every((id) => states[id] && states[id].state);
}
function knopOnline(knop) {
  const t = knop.targets || [];
  return t.length === 0 || t.some((id) => states[id] && states[id].online);
}

function render() {
  const knoppen = RC.knoppen || [];
  if (leeg) leeg.hidden = knoppen.length > 0;
  grid.innerHTML = "";
  knoppen.forEach((knop, idx) => {
    const aan = knopAan(knop);
    const online = knopOnline(knop);
    const btn = document.createElement("button");
    btn.className = "rc-knop" + (aan ? " on" : "") + (online ? "" : " offline");
    btn.style.setProperty("--kleur", knop.kleur || "#e11d2a");
    btn.innerHTML =
      `<span class="rc-knop-label">${escapeHtml(knop.label)}</span>` +
      `<span class="rc-knop-status">${aan ? "ON AIR" : "uit"}</span>`;
    btn.addEventListener("click", () => doAction(idx, btn));
    grid.appendChild(btn);
  });
}

async function doAction(idx, btn) {
  btn.classList.add("bezig");
  try {
    await fetch(`/rc/${RC.slug}/action`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ token: TOKEN, knop: idx }),
    });
  } catch (e) {
    console.error("actie mislukt", e);
  }
  btn.classList.remove("bezig");
}

function applyLamp(l) {
  states[l.id] = { state: !!l.state, online: !!l.online };
  render();
}

function connect() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  const url = `${proto}://${location.host}/rc/${RC.slug}/ws?token=${encodeURIComponent(TOKEN)}`;
  const ws = new WebSocket(url);
  ws.onopen = () => { statusEl.textContent = "Online"; statusEl.className = "status-pill online"; };
  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.type === "snapshot") msg.lamps.forEach(applyLamp);
    else if (msg.type === "lamp") applyLamp(msg.lamp);
  };
  ws.onclose = () => {
    statusEl.textContent = "Offline";
    statusEl.className = "status-pill offline";
    setTimeout(connect, 1500);
  };
  ws.onerror = () => ws.close();
}

render();
connect();
