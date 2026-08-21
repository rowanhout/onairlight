"""On-Air Lamp — centrale FastAPI-server.

Bedient on-air lampen via de web-app/webremote, de REST-API (o.a. voor Bitfocus
Companion) en houdt live WebSocket-verbindingen aan met de ESP32-lampen én de
browsers. Draait op een altijd-aan machine in het lokale netwerk.

Opzet (SessionMiddleware/StaticFiles/Jinja2) en de auth-redirect-helpers zijn
gemodelleerd op de bestaande houtprivate-app.
"""

import os
from datetime import datetime, timezone

import sentry_sdk
from dotenv import load_dotenv
from fastapi import (Depends, FastAPI, Form, Header, Request, WebSocket,
                     WebSocketDisconnect)
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from starlette.middleware.sessions import SessionMiddleware

load_dotenv()

sentry_sdk.init(
    dsn=os.getenv("SENTRY_DSN"),
    environment=os.getenv("SENTRY_ENVIRONMENT", "production"),
    traces_sample_rate=0.1,
)

import auth
import devices
import remotes
from hub import manager

API_KEY = os.getenv("ONAIR_API_KEY", "")

# SECRET_KEY ondertekent de sessiecookie. Zonder een eigen, geheime waarde kan
# iedereen met itsdangerous zelf een geldige sessiecookie fabriceren (bv.
# {"user": "rowan"}) en zo volledig als admin inloggen. Er is dus bewust GEEN
# werkende default: ontbreekt of is de waarde te kort, dan stopt de server
# meteen bij het opstarten in plaats van stilletjes onveilig door te draaien.
SECRET_KEY = os.getenv("SECRET_KEY", "")
if len(SECRET_KEY) < 16:
    raise SystemExit(
        "SECRET_KEY ontbreekt of is te kort (minimaal 16 tekens vereist).\n"
        "Genereer een lange, willekeurige waarde en zet die als omgevingsvariabele, bijv.:\n"
        "  openssl rand -hex 32\n"
        "of: python -c \"import secrets; print(secrets.token_hex(32))\""
    )

app = FastAPI(title="On-Air Lamp")
app.add_middleware(
    SessionMiddleware,
    secret_key=SECRET_KEY,
)
app.mount("/static", StaticFiles(directory="static"), name="static")
templates = Jinja2Templates(directory="templates")


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


@app.on_event("startup")
async def _seed_admin() -> None:
    """Maak bij de eerste start optioneel een admin aan op basis van env-vars.
    Handig voor cloud-deploys (Railway) zonder interactieve shell: zet
    ONAIR_ADMIN_USER en ONAIR_ADMIN_PASSWORD en de gebruiker wordt eenmalig
    aangemaakt als hij nog niet bestaat."""
    gebruiker = os.getenv("ONAIR_ADMIN_USER")
    wachtwoord = os.getenv("ONAIR_ADMIN_PASSWORD")
    if gebruiker and wachtwoord and auth.get_user(gebruiker) is None:
        auth.create_user(
            gebruiker,
            os.getenv("ONAIR_ADMIN_NAAM", gebruiker),
            wachtwoord,
            admin=True,
        )


# ---------------------------------------------------------------------------
# Auth
# ---------------------------------------------------------------------------

class _LoginRedirect(Exception):
    pass


@app.exception_handler(_LoginRedirect)
async def _login_redirect_handler(request: Request, exc: _LoginRedirect):
    return RedirectResponse(url="/login", status_code=302)


def require_login(request: Request) -> str:
    """Web-UI-routes: vereist een geldige sessie."""
    user = auth.get_session_user(request)
    if not user:
        raise _LoginRedirect()
    return user


def require_api(request: Request, x_api_key: str | None = Header(default=None)) -> str:
    """API-routes: geldig met óf een ingelogde sessie (web-app) óf een correcte
    ``X-API-Key`` (Companion / scripts)."""
    if API_KEY and x_api_key == API_KEY:
        return "api-key"
    user = auth.get_session_user(request)
    if user:
        return user
    raise _Unauthorized()


def require_admin(request: Request) -> str:
    """Beheer-API: vereist een ingelogde admin-sessie (geen API-key)."""
    user = auth.get_session_user(request)
    if not user or not auth.is_admin(user):
        raise _Unauthorized()
    return user


class _Unauthorized(Exception):
    pass


@app.exception_handler(_Unauthorized)
async def _unauthorized_handler(request: Request, exc: _Unauthorized):
    return JSONResponse({"error": "unauthorized"}, status_code=401)


# ---------------------------------------------------------------------------
# Web-app / webremote
# ---------------------------------------------------------------------------

@app.get("/login", response_class=HTMLResponse)
async def login_form(request: Request):
    return templates.TemplateResponse("login.html", {"request": request, "error": None})


@app.post("/login")
async def login_submit(request: Request, username: str = Form(...), password: str = Form(...)):
    if auth.login(username, password):
        request.session["user"] = username
        return RedirectResponse(url="/", status_code=302)
    return templates.TemplateResponse(
        "login.html",
        {"request": request, "error": "Onjuiste gebruikersnaam of wachtwoord."},
        status_code=401,
    )


@app.get("/logout")
async def logout(request: Request):
    request.session.clear()
    return RedirectResponse(url="/login", status_code=302)


@app.get("/", response_class=HTMLResponse)
async def index(request: Request, user: str = Depends(require_login)):
    return templates.TemplateResponse(
        "index.html",
        {
            "request": request,
            "user": user,
            "naam": auth.get_display_naam(user),
            "is_admin": auth.is_admin(user),
            "lamps": devices.all_lamps(),
        },
    )


@app.get("/beheer", response_class=HTMLResponse)
async def beheer(request: Request, user: str = Depends(require_login)):
    """Beheerpagina (admin-only): devices hernoemen + afstandsbedieningen."""
    if not auth.is_admin(user):
        return RedirectResponse(url="/", status_code=302)
    return templates.TemplateResponse(
        "beheer.html",
        {
            "request": request,
            "naam": auth.get_display_naam(user),
            "lamps": [_public(l) for l in devices.all_lamps()],
            "remotes": [_remote_public(r) for r in remotes.all_remotes()],
        },
    )


# ---------------------------------------------------------------------------
# REST-API (web-app + Companion)
# ---------------------------------------------------------------------------

def _public(lamp: dict) -> dict:
    """Lamp + live online-status uit de hub."""
    return {**lamp, "online": manager.is_online(lamp["id"])}


async def _apply_state(lamp_id: str, state: bool) -> dict | None:
    """Zet de gewenste status op, stuur naar de ESP32 en broadcast naar de UI."""
    lamp = devices.set_state(lamp_id, state)
    if lamp is None:
        return None
    await manager.send_command(lamp_id, state)
    payload = _public(lamp)
    await manager.broadcast({"type": "lamp", "lamp": payload})
    return payload


@app.get("/api/lamps")
async def api_lamps(_: str = Depends(require_api)):
    return [_public(l) for l in devices.all_lamps()]


@app.get("/api/lamps/{lamp_id}/state")
async def api_lamp_state(lamp_id: str, _: str = Depends(require_api)):
    lamp = devices.get(lamp_id)
    if lamp is None:
        return JSONResponse({"error": "onbekende lamp"}, status_code=404)
    return _public(lamp)


@app.post("/api/lamps/{lamp_id}/on")
async def api_lamp_on(lamp_id: str, _: str = Depends(require_api)):
    lamp = await _apply_state(lamp_id, True)
    if lamp is None:
        return JSONResponse({"error": "onbekende lamp"}, status_code=404)
    return lamp


@app.post("/api/lamps/{lamp_id}/off")
async def api_lamp_off(lamp_id: str, _: str = Depends(require_api)):
    lamp = await _apply_state(lamp_id, False)
    if lamp is None:
        return JSONResponse({"error": "onbekende lamp"}, status_code=404)
    return lamp


@app.post("/api/lamps/{lamp_id}/toggle")
async def api_lamp_toggle(lamp_id: str, _: str = Depends(require_api)):
    lamp = devices.get(lamp_id)
    if lamp is None:
        return JSONResponse({"error": "onbekende lamp"}, status_code=404)
    return await _apply_state(lamp_id, not lamp["state"])


@app.post("/api/groups/{groep}/on")
async def api_group_on(groep: str, _: str = Depends(require_api)):
    lampen = devices.in_group(groep)
    if not lampen:
        return JSONResponse({"error": "lege of onbekende groep"}, status_code=404)
    return [await _apply_state(l["id"], True) for l in lampen]


@app.post("/api/groups/{groep}/off")
async def api_group_off(groep: str, _: str = Depends(require_api)):
    lampen = devices.in_group(groep)
    if not lampen:
        return JSONResponse({"error": "lege of onbekende groep"}, status_code=404)
    return [await _apply_state(l["id"], False) for l in lampen]


@app.post("/api/lamps/{lamp_id}/rename")
async def api_lamp_rename(lamp_id: str, request: Request, _: str = Depends(require_admin)):
    """Hernoem een device: naam/ruimte/groep. De id blijft ongemoeid."""
    data = await request.json()
    lamp = devices.update_meta(
        lamp_id,
        naam=data.get("naam"),
        ruimte=data.get("ruimte"),
        groep=data.get("groep"),
    )
    if lamp is None:
        return JSONResponse({"error": "onbekende lamp"}, status_code=404)
    payload = _public(lamp)
    await manager.broadcast({"type": "lamp", "lamp": payload})
    return payload


# ---------------------------------------------------------------------------
# Afstandsbedieningen (beheer + kiosk-pagina)
# ---------------------------------------------------------------------------

def _remote_public(r: dict) -> dict:
    return {
        "slug": r["slug"],
        "naam": r.get("naam", r["slug"]),
        "token": r.get("token", ""),
        "kolommen": r.get("kolommen", 2),
        "knoppen": r.get("knoppen", []),
        "url": f"/rc/{r['slug']}?token={r.get('token', '')}",
    }


def _sanitize_buttons(knoppen) -> list:
    """Maak de van de client ontvangen knoppen veilig en consistent."""
    out = []
    if not isinstance(knoppen, list):
        return out
    for k in knoppen:
        if not isinstance(k, dict):
            continue
        actie = k.get("actie")
        if actie not in ("on", "off", "toggle"):
            actie = "toggle"
        targets = [str(t) for t in k.get("targets", []) if isinstance(t, str)]
        out.append({
            "label": str(k.get("label", "")),
            "kleur": str(k.get("kleur", "#e11d2a")),
            "actie": actie,
            "targets": targets,
        })
    return out


def _check_remote_token(slug: str, token: str | None) -> dict | None:
    r = remotes.get(slug)
    if r is None or not token or token != r.get("token"):
        return None
    return r


@app.get("/api/remotes")
async def api_remotes(_: str = Depends(require_admin)):
    return [_remote_public(r) for r in remotes.all_remotes()]


@app.post("/api/remotes")
async def api_remote_create(request: Request, _: str = Depends(require_admin)):
    data = await request.json()
    naam = (data.get("naam") or "").strip()
    if not naam:
        return JSONResponse({"error": "naam verplicht"}, status_code=400)
    return _remote_public(remotes.create(naam))


@app.get("/api/remotes/{slug}")
async def api_remote_get(slug: str, _: str = Depends(require_admin)):
    r = remotes.get(slug)
    if r is None:
        return JSONResponse({"error": "onbekende afstandsbediening"}, status_code=404)
    return _remote_public(r)


@app.put("/api/remotes/{slug}")
async def api_remote_update(slug: str, request: Request, _: str = Depends(require_admin)):
    data = await request.json()
    r = remotes.update(
        slug,
        naam=(data.get("naam") or "").strip() or None,
        kolommen=data.get("kolommen"),
        knoppen=_sanitize_buttons(data.get("knoppen")),
    )
    if r is None:
        return JSONResponse({"error": "onbekende afstandsbediening"}, status_code=404)
    return _remote_public(r)


@app.delete("/api/remotes/{slug}")
async def api_remote_delete(slug: str, _: str = Depends(require_admin)):
    if not remotes.delete(slug):
        return JSONResponse({"error": "onbekende afstandsbediening"}, status_code=404)
    return {"ok": True}


@app.post("/api/remotes/{slug}/token")
async def api_remote_token(slug: str, _: str = Depends(require_admin)):
    r = remotes.regenerate_token(slug)
    if r is None:
        return JSONResponse({"error": "onbekende afstandsbediening"}, status_code=404)
    return _remote_public(r)


@app.get("/rc/{slug}", response_class=HTMLResponse)
async def remote_page(slug: str, request: Request, token: str = ""):
    """Kiosk-/wandtablet-pagina, bereikbaar via geheime token-link (geen login)."""
    r = _check_remote_token(slug, token)
    if r is None:
        return HTMLResponse("Geen toegang — controleer de link/token.", status_code=403)
    return templates.TemplateResponse(
        "remote.html",
        {"request": request, "remote": _remote_public(r), "token": token},
    )


@app.post("/rc/{slug}/action")
async def remote_action(slug: str, request: Request):
    """Schakel een knop op een afstandsbediening (token-geverifieerd, geen login)."""
    data = await request.json()
    r = _check_remote_token(slug, data.get("token"))
    if r is None:
        return JSONResponse({"error": "geen toegang"}, status_code=403)
    idx = data.get("knop")
    knoppen = r.get("knoppen", [])
    if not isinstance(idx, int) or idx < 0 or idx >= len(knoppen):
        return JSONResponse({"error": "onbekende knop"}, status_code=404)
    knop = knoppen[idx]
    targets = knop.get("targets", [])
    actie = knop.get("actie", "toggle")
    if actie == "on":
        desired = True
    elif actie == "off":
        desired = False
    else:  # toggle: uit als alles al aan staat, anders alles aan
        states = [(devices.get(t) or {}).get("state", False) for t in targets]
        desired = not (len(states) > 0 and all(states))
    resultaat = [await _apply_state(t, desired) for t in targets]
    return {"knop": idx, "state": desired, "lamps": [l for l in resultaat if l]}


# ---------------------------------------------------------------------------
# WebSockets
# ---------------------------------------------------------------------------

@app.websocket("/ws")
async def ws_client(ws: WebSocket):
    """Browser-clients: ontvangen live statusupdates."""
    await manager.connect_client(ws)
    try:
        # Stuur direct de huidige stand zodat een nieuw scherm meteen klopt.
        await ws.send_json({
            "type": "snapshot",
            "lamps": [_public(l) for l in devices.all_lamps()],
        })
        while True:
            # We verwachten geen berichten van de browser; dit houdt de
            # verbinding open en detecteert disconnects.
            await ws.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        await manager.disconnect_client(ws)


@app.websocket("/ws/device")
async def ws_device(ws: WebSocket):
    """ESP32-lampen. Verbinden met ``?id=<lamp-id>`` (optioneel ``naam``,
    ``ruimte``). De server registreert ze, synct de gewenste status en
    verwerkt status/heartbeats."""
    lamp_id = ws.query_params.get("id")
    if not lamp_id:
        await ws.close(code=1008)  # policy violation: id verplicht
        return

    naam = ws.query_params.get("naam")
    ruimte = ws.query_params.get("ruimte")

    if not await manager.register_device(lamp_id, ws):
        # Er is al een levende device-socket voor dit id: weiger de overname
        # i.p.v. de bestaande (mogelijk echte) ESP32 stilletjes te vervangen.
        await ws.close(code=1008)  # policy violation: id al in gebruik
        return

    velden = {"online": True, "last_seen": _now()}
    if naam:
        velden["naam"] = naam
    if ruimte:
        velden["ruimte"] = ruimte
    lamp = devices.upsert(lamp_id, **velden)

    # Sync: stuur de laatst bekende gewenste status naar de ESP32.
    await manager.send_command(lamp_id, lamp["state"])
    await manager.broadcast({"type": "lamp", "lamp": _public(lamp)})

    try:
        while True:
            msg = await ws.receive_json()
            mtype = msg.get("type")
            if mtype == "state":
                # ESP32 meldt de werkelijke relais-/lampstatus terug.
                lamp = devices.upsert(
                    lamp_id, state=bool(msg.get("state")), last_seen=_now()
                )
                await manager.broadcast({"type": "lamp", "lamp": _public(lamp)})
            elif mtype == "ping":
                devices.set_online(lamp_id, True, _now())
                await ws.send_json({"type": "pong"})
    except WebSocketDisconnect:
        pass
    finally:
        await manager.unregister_device(lamp_id)
        lamp = devices.set_online(lamp_id, False, _now())
        if lamp is not None:
            await manager.broadcast({"type": "lamp", "lamp": _public(lamp)})


@app.websocket("/rc/{slug}/ws")
async def ws_remote(ws: WebSocket, slug: str):
    """Live updates voor een afstandsbediening: alleen de toegewezen devices."""
    token = ws.query_params.get("token")
    r = remotes.get(slug)
    if r is None or not token or token != r.get("token"):
        await ws.close(code=1008)
        return
    target_ids = set()
    for knop in r.get("knoppen", []):
        target_ids.update(knop.get("targets", []))
    await manager.connect_client(ws, only_ids=target_ids)
    try:
        await ws.send_json({
            "type": "snapshot",
            "lamps": [_public(l) for l in devices.all_lamps() if l["id"] in target_ids],
        })
        while True:
            await ws.receive_text()
    except WebSocketDisconnect:
        pass
    finally:
        await manager.disconnect_client(ws)
