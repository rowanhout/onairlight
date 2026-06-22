// Beheerpagina: devices hernoemen + afstandsbedieningen samenstellen.

const devices = window.__DEVICES__ || [];
let remotes = window.__REMOTES__ || [];

const devicesEl = document.getElementById("devices");
const remotesEl = document.getElementById("remotes");

function escapeHtml(s) {
  return String(s == null ? "" : s).replace(/[&<>"']/g, (c) =>
    ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&#39;" }[c]));
}

function flash(btn, ok) {
  const t = btn.dataset.label || btn.textContent;
  btn.dataset.label = t;
  btn.textContent = ok ? "✓ Opgeslagen" : "✗ Fout";
  setTimeout(() => { btn.textContent = t; }, 1200);
}

// -- Devices hernoemen -------------------------------------------------
function renderDevices() {
  devicesEl.innerHTML = "";
  if (!devices.length) {
    devicesEl.innerHTML = '<p class="hint">Nog geen devices aangemeld.</p>';
    return;
  }
  devices.forEach((d) => {
    const row = document.createElement("div");
    row.className = "device-row";
    row.innerHTML =
      `<span class="mono" title="${escapeHtml(d.id)}">${escapeHtml(d.id)}</span>` +
      `<input class="d-naam" value="${escapeHtml(d.naam)}" placeholder="naam">` +
      `<input class="d-ruimte" value="${escapeHtml(d.ruimte)}" placeholder="ruimte">` +
      `<input class="d-groep" value="${escapeHtml(d.groep)}" placeholder="groep">` +
      `<button class="btn">Opslaan</button>`;
    row.querySelector("button").addEventListener("click", async (e) => {
      const body = {
        naam: row.querySelector(".d-naam").value,
        ruimte: row.querySelector(".d-ruimte").value,
        groep: row.querySelector(".d-groep").value,
      };
      const res = await fetch(`/api/lamps/${encodeURIComponent(d.id)}/rename`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify(body),
      });
      if (res.ok) Object.assign(d, body);
      flash(e.target, res.ok);
    });
    devicesEl.appendChild(row);
  });
}

// -- Afstandsbedieningen ----------------------------------------------
function renderRemotes() {
  remotesEl.innerHTML = "";
  if (!remotes.length) {
    remotesEl.innerHTML = '<p class="hint">Nog geen afstandsbedieningen. Maak er hierboven één aan.</p>';
    return;
  }
  remotes.forEach((rc) => remotesEl.appendChild(renderRemoteCard(rc)));
}

function renderRemoteCard(rc) {
  rc.knoppen = rc.knoppen || [];
  const card = document.createElement("div");
  card.className = "rc-card";
  const fullUrl = location.origin + rc.url;
  card.innerHTML =
    `<div class="rc-head">` +
      `<input class="rc-naam" value="${escapeHtml(rc.naam)}">` +
      `<span class="rc-actions">` +
        `<a class="btn" href="${escapeHtml(rc.url)}" target="_blank" rel="noopener">Openen</a>` +
        `<button class="btn btn-copy">Kopieer link</button>` +
        `<button class="btn btn-token">Nieuw token</button>` +
        `<button class="btn btn-del">Verwijder</button>` +
      `</span>` +
    `</div>` +
    `<div class="rc-link mono">${escapeHtml(fullUrl)}</div>` +
    `<label class="rc-kol">Kolommen <input type="number" class="rc-kolommen" min="1" max="6" value="${rc.kolommen || 2}"></label>` +
    `<div class="rc-knoppen"></div>` +
    `<div class="rc-card-acties"><button class="btn btn-add">+ Knop</button>` +
    `<button class="btn btn-primair btn-save">Opslaan</button></div>`;

  const knoppenEl = card.querySelector(".rc-knoppen");

  function renderKnoppen() {
    knoppenEl.innerHTML = "";
    rc.knoppen.forEach((k, ki) => {
      const krow = document.createElement("div");
      krow.className = "knop-row";
      const targetsHtml = devices.map((d) =>
        `<label class="chk"><input type="checkbox" value="${escapeHtml(d.id)}" ${(k.targets || []).includes(d.id) ? "checked" : ""}> ${escapeHtml(d.naam || d.id)}</label>`
      ).join("");
      krow.innerHTML =
        `<input class="k-label" value="${escapeHtml(k.label)}" placeholder="label">` +
        `<input class="k-kleur" type="color" value="${k.kleur || "#e11d2a"}">` +
        `<select class="k-actie">` +
          `<option value="toggle">toggle</option>` +
          `<option value="on">aan</option>` +
          `<option value="off">uit</option>` +
        `</select>` +
        `<div class="k-targets">${targetsHtml || '<span class="hint">geen devices</span>'}</div>` +
        `<button class="btn btn-kdel" title="knop verwijderen">×</button>`;
      krow.querySelector(".k-actie").value = k.actie || "toggle";
      krow.querySelector(".btn-kdel").addEventListener("click", () => {
        rc.knoppen.splice(ki, 1);
        renderKnoppen();
      });
      knoppenEl.appendChild(krow);
    });
  }
  renderKnoppen();

  card.querySelector(".btn-add").addEventListener("click", () => {
    rc.knoppen.push({ label: "", kleur: "#e11d2a", actie: "toggle", targets: [] });
    renderKnoppen();
  });

  card.querySelector(".btn-copy").addEventListener("click", (e) => {
    navigator.clipboard?.writeText(fullUrl);
    flash(e.target, true);
  });

  card.querySelector(".btn-token").addEventListener("click", async () => {
    if (!confirm("Nieuw token aanmaken? De oude deellink werkt daarna niet meer.")) return;
    const res = await fetch(`/api/remotes/${rc.slug}/token`, { method: "POST" });
    if (res.ok) { Object.assign(rc, await res.json()); renderRemotes(); }
  });

  card.querySelector(".btn-del").addEventListener("click", async () => {
    if (!confirm(`Afstandsbediening "${rc.naam}" verwijderen?`)) return;
    const res = await fetch(`/api/remotes/${rc.slug}`, { method: "DELETE" });
    if (res.ok) { remotes = remotes.filter((x) => x.slug !== rc.slug); renderRemotes(); }
  });

  card.querySelector(".btn-save").addEventListener("click", async (e) => {
    const knoppen = [...knoppenEl.querySelectorAll(".knop-row")].map((row) => ({
      label: row.querySelector(".k-label").value,
      kleur: row.querySelector(".k-kleur").value,
      actie: row.querySelector(".k-actie").value,
      targets: [...row.querySelectorAll(".k-targets input:checked")].map((c) => c.value),
    }));
    const body = {
      naam: card.querySelector(".rc-naam").value,
      kolommen: parseInt(card.querySelector(".rc-kolommen").value, 10) || 2,
      knoppen,
    };
    const res = await fetch(`/api/remotes/${rc.slug}`, {
      method: "PUT",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
    if (res.ok) Object.assign(rc, await res.json());
    flash(e.target, res.ok);
  });

  return card;
}

document.getElementById("rc-nieuw").addEventListener("click", async () => {
  const input = document.getElementById("rc-naam");
  const naam = input.value.trim();
  if (!naam) return;
  const res = await fetch("/api/remotes", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify({ naam }),
  });
  if (res.ok) { remotes.push(await res.json()); input.value = ""; renderRemotes(); }
});

renderDevices();
renderRemotes();
