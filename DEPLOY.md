# Deployen op Railway

Deze app draait prima op [Railway](https://railway.app). Let op de twee
belangrijke punten: **persistente opslag** (een volume) en het feit dat de
server dan **publiek op internet** staat (zie de waarschuwing onderaan).

> De app is van origine **LAN-only** bedoeld (lokale Raspberry Pi/mini-pc). Een
> cloud-deploy is handig voor demo's/testen of als je bewust alles cloud-based
> wilt. Voor een betrouwbare broadcast-omgeving blijft lokaal draaien de
> robuustste keuze (geen internet-afhankelijkheid, lagere latency).

## Stap voor stap

### 1. Project aanmaken vanuit de repo
1. Log in op Railway en kies **New Project → Deploy from GitHub repo**.
2. Selecteer `rowanhout/onairlight` (branch `main`).
3. Railway detecteert Python (via `requirements.txt`) en gebruikt het
   start-commando uit de `Procfile` automatisch. `$PORT` wordt door Railway
   gezet — daar hoef je niets voor te doen.

### 2. Persistent volume toevoegen (cruciaal!)
Zonder volume is de schijf *ephemeral*: bij elke redeploy zijn je gebruikers en
lampen weg.
1. Open de service → tab **Variables/Settings → Volumes → New Volume**.
2. Mount path: **`/data`**.

### 3. Environment-variabelen instellen
Service → **Variables → New Variable**, voeg toe:

| Variabele | Waarde | Toelichting |
|-----------|--------|-------------|
| `SECRET_KEY` | lange random string | `python -c "import secrets; print(secrets.token_hex(32))"` |
| `ONAIR_API_KEY` | je eigen sleutel | voor Companion / REST-API (`X-API-Key`) |
| `ONAIR_DATA_DIR` | `/data` | wijst opslag naar het volume uit stap 2 |
| `ONAIR_ADMIN_USER` | bv. `rowan` | eenmalige admin (zie stap 4) |
| `ONAIR_ADMIN_PASSWORD` | sterk wachtwoord | "" |
| `ONAIR_ADMIN_NAAM` | bv. `Rowan` | optioneel, weergavenaam |

### 4. Eerste admin aanmaken
Je hebt op Railway geen handige interactieve shell, dus `create_user.py` is
lastig. Daarom maakt de app bij het **opstarten** automatisch een admin aan op
basis van `ONAIR_ADMIN_USER` + `ONAIR_ADMIN_PASSWORD` (alleen als die gebruiker
nog niet bestaat). Na de eerste succesvolle login mag je die twee variabelen
weer **verwijderen**.

### 5. Deployen & openen
1. Klik **Deploy**.
2. Service → **Settings → Networking → Generate Domain** voor een publieke URL
   (`https://<jouw-app>.up.railway.app`).
3. Open de URL en log in met de admin uit stap 4.
4. (Optioneel) Zet onder **Settings → Healthcheck** het pad op `/login`.

### 6. Lampen / Companion laten verbinden
- **ESP32-lampen** verbinden via beveiligde WebSocket:
  `wss://<jouw-app>.up.railway.app/ws/device?id=<lamp-id>` (poort 443, TLS).
  De firmware moet dan `wss://` aankunnen — zie `firmware/`.
- **Companion / REST-API**: gebruik dezelfde URL met `X-API-Key: <ONAIR_API_KEY>`,
  bijvoorbeeld `https://<jouw-app>.up.railway.app/api/lamps/<id>/on`.

## Testen zonder hardware (tegen de cloud)
```bash
pip install websockets
python tools/fake_device.py --server wss://<jouw-app>.up.railway.app \
    --id studio-1 --naam "Studio 1" --ruimte "Studio 1"
```

## ⚠️ Let op: internet-exposure
Op Railway staat de control-server publiek op internet. Er is sessie-login +
API-key, maar:
- internet eruit = lampen niet schakelbaar;
- meer aanvalsoppervlak dan LAN-only + VPN.

Kies een sterk `SECRET_KEY` en `ONAIR_API_KEY`, en overweeg voor productie
alsnog een lokale server met VPN voor toegang van buiten.
