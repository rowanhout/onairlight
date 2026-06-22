// On-Air web-app: tikken op een tegel toggelt de lamp; live updates via WebSocket.

const grid = document.getElementById("lampgrid");
const empty = document.getElementById("empty");

// -- Tegel toggelen ----------------------------------------------------
grid.addEventListener("click", async (e) => {
  const tile = e.target.closest(".tile");
  if (!tile) return;
  const id = tile.dataset.id;
  // Optimistische UI; de WS-broadcast bevestigt de echte status.
  try {
    await fetch(`/api/lamps/${encodeURIComponent(id)}/toggle`, { method: "POST" });
  } catch (err) {
    console.error("toggle mislukt", err);
  }
});

// -- Tegel renderen / bijwerken ---------------------------------------
function applyLamp(lamp) {
  let tile = grid.querySelector(`.tile[data-id="${CSS.escape(lamp.id)}"]`);
  if (!tile) {
    tile = document.createElement("button");
    tile.className = "tile";
    tile.dataset.id = lamp.id;
    tile.innerHTML =
      '<span class="tile-naam"></span>' +
      '<span class="tile-ruimte"></span>' +
      '<span class="tile-status"></span>' +
      '<span class="dot" title="verbinding"></span>';
    grid.appendChild(tile);
  }
  tile.querySelector(".tile-naam").textContent = lamp.naam || lamp.id;
  const ruimte = tile.querySelector(".tile-ruimte");
  ruimte.textContent = lamp.ruimte || "";
  ruimte.hidden = !lamp.ruimte;
  tile.querySelector(".tile-status").textContent = lamp.state ? "ON AIR" : "uit";
  tile.classList.toggle("on", !!lamp.state);
  tile.classList.toggle("off", !lamp.state);
  tile.classList.toggle("offline", !lamp.online);
  if (empty) empty.hidden = grid.children.length > 0;
}

// -- WebSocket met automatisch herverbinden ---------------------------
function connect() {
  const proto = location.protocol === "https:" ? "wss" : "ws";
  const ws = new WebSocket(`${proto}://${location.host}/ws`);

  ws.onmessage = (ev) => {
    const msg = JSON.parse(ev.data);
    if (msg.type === "snapshot") {
      msg.lamps.forEach(applyLamp);
    } else if (msg.type === "lamp") {
      applyLamp(msg.lamp);
    }
  };
  ws.onclose = () => setTimeout(connect, 1500); // herverbind
  ws.onerror = () => ws.close();
}

connect();
