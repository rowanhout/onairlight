# On-Air Lamp

Bediening van **on-air lampen** voor een professionele video-broadcastomgeving:
een lamp buiten de opnameruimte die aangeeft dat er wordt opgenomen.

Te bedienen via:
- de **web-app / webremote** (telefoon, tablet, desktop),
- de **REST-API**,
- **Bitfocus Companion** (Generic HTTP-module — zie [`COMPANION.md`](COMPANION.md)).

Elke lamp is een **ESP32** die via Power-over-Ethernet (PoE) wordt gevoed en via
een WebSocket verbinding maakt met deze server. Zie [`firmware/`](firmware/) voor
de firmware en het hardware-advies.

## Architectuur

```
 [web-app]   [Companion]            clients op het LAN
     |  HTTP+WS  |  HTTP (X-API-Key)
     v           v
 +-----------------------------+
 |  server.py (FastAPI)        |   draait op een altijd-aan machine
 |  REST-API + WebSocket-hub   |   (Raspberry Pi / mini-pc)
 |  device-registry: lamps.json|
 +-----------------------------+
     ^  WebSocket (één per lamp)
     |
 [ESP32-PoE] ...  schakelt via GPIO+MOSFET de 12V-lamp
```

Elke ESP32 opent als **WebSocket-client** een verbinding naar de server. Daardoor
weet de server direct of een lamp online is en wat zijn status is, zonder vaste
IP-adressen. Browsers krijgen via een tweede WS-kanaal live updates.

## Installeren & draaien

```bash
pip install -r requirements.txt
cp .env.example .env          # en pas SECRET_KEY en ONAIR_API_KEY aan
python create_user.py rowan "Rowan" --admin
uvicorn server:app --host 0.0.0.0 --port 8080
```

> `SECRET_KEY` is **verplicht** (minimaal 16 tekens) — zonder geldige waarde
> stopt de server meteen bij het opstarten met een duidelijke foutmelding, in
> plaats van onveilig door te draaien met een voorspelbare standaardwaarde.
> Genereer er een met `openssl rand -hex 32` (of
> `python -c "import secrets; print(secrets.token_hex(32))"`).

Open daarna `http://<server-ip>:8080` en log in.

> Draai dit op een altijd-aan machine in hetzelfde lokale netwerk als de lampen
> (bijv. een Raspberry Pi of mini-pc). De opzet is **LAN-only**: stel de lampen
> en de server niet rechtstreeks aan het internet bloot. Voor toegang van buiten
> later: gebruik een VPN.

## In de cloud draaien (Railway)

Liever in de cloud i.p.v. op een lokale machine? Zie [`DEPLOY.md`](DEPLOY.md)
voor een stap-voor-stap Railway-handleiding (persistent volume, env-variabelen
en een automatische eerste-admin). Let op: dat zet de server publiek op
internet — voor een betrouwbare broadcast-omgeving blijft LAN-only de
robuustste keuze.

## Testen zonder hardware

In een tweede terminal:

```bash
pip install websockets
python tools/fake_device.py --id studio-1 --naam "Studio 1" --ruimte "Studio 1"
```

De gesimuleerde lamp verschijnt direct in de web-app. Tik op de tegel of gebruik
de API; het commando komt aan in de simulator en de tegel kleurt rood (ON AIR).

## REST-API

Alle `/api`-routes vereisen óf een ingelogde sessie (web-app) óf de header
`X-API-Key: <ONAIR_API_KEY>`.

| Methode | Pad                         | Actie                              |
|---------|-----------------------------|------------------------------------|
| GET     | `/api/lamps`                | lijst van lampen + status          |
| GET     | `/api/lamps/{id}/state`     | status van één lamp (voor feedback)|
| POST    | `/api/lamps/{id}/on`        | lamp aan (ON AIR)                  |
| POST    | `/api/lamps/{id}/off`       | lamp uit                           |
| POST    | `/api/lamps/{id}/toggle`    | omschakelen                        |
| POST    | `/api/groups/{groep}/on`    | hele groep/ruimte aan              |
| POST    | `/api/groups/{groep}/off`   | hele groep/ruimte uit              |

Voorbeeld:

```bash
curl -X POST -H "X-API-Key: $ONAIR_API_KEY" \
     http://localhost:8080/api/lamps/studio-1/on
```

## Bestanden

| Bestand            | Functie                                            |
|--------------------|----------------------------------------------------|
| `server.py`        | FastAPI-app: routes, REST-API, WebSockets          |
| `auth.py`          | sessie-/wachtwoordauthenticatie (bcrypt)           |
| `devices.py`       | device-registry (`lamps.json`)                     |
| `hub.py`           | WebSocket-hub (lampen + browsers)                  |
| `templates/`       | web-app (login + hoofdscherm)                      |
| `static/`          | CSS + JavaScript (live updates)                    |
| `tools/fake_device.py` | ESP32-simulator voor testen zonder hardware    |
| `firmware/`        | ESP32-firmware + hardware-advies                   |
| `COMPANION.md`     | Bitfocus Companion-integratie                      |
