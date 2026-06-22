"""On-Air Lamp — centrale FastAPI-server.

Bedient on-air lampen via de web-app/webremote, de REST-API (o.a. voor Bitfocus
Companion) en houdt live WebSocket-verbindingen aan met de ESP32-lampen én de
browsers. Draait op een altijd-aan machine in het lokale netwerk.

Opzet (SessionMiddleware/StaticFiles/Jinja2) en de auth-redirect-helpers zijn
gemodelleerd op de bestaande houtprivate-app.
"""

import os
from datetime import datetime, timezone

from dotenv import load_dotenv
from fastapi import (Depends, FastAPI, Form, Header, Request, WebSocket,
                     WebSocketDisconnect)
from fastapi.responses import HTMLResponse, JSONResponse, RedirectResponse
from fastapi.staticfiles import StaticFiles
from fastapi.templating import Jinja2Templates
from starlette.middleware.sessions import SessionMiddleware

load_dotenv()

import auth
import devices
from hub import manager

API_KEY = os.getenv("ONAIR_API_KEY", "")

app = FastAPI(title="On-Air Lamp")
app.add_middleware(
    SessionMiddleware,
    secret_key=os.getenv("SECRET_KEY", "verander-mij-in-productie"),
)
app.mount("/static", StaticFiles(directory="static"), name="static")
templates = Jinja2Templates(directory="templates")


def _now() -> str:
    return datetime.now(timezone.utc).isoformat()


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
            "lamps": devices.all_lamps(),
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

    await manager.register_device(lamp_id, ws)
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
