"""WebSocket-hub: verbindt de ESP32-lampen en de browser-clients.

Twee soorten verbindingen:
- **device-sockets**: één per ESP32. De ESP32 meldt zich aan met zijn ``id``,
  stuurt status/heartbeats en ontvangt aan/uit-commando's.
- **client-sockets**: de browsers/web-app. Krijgen een live broadcast bij elke
  statuswijziging zodat alle schermen direct meebewegen.
"""

import asyncio
from typing import Dict, Optional

from fastapi import WebSocket
from starlette.websockets import WebSocketState


class ConnectionManager:
    def __init__(self) -> None:
        # lamp_id -> device WebSocket (de ESP32)
        self._devices: Dict[str, WebSocket] = {}
        # browser-client -> optioneel filter (set van lamp-id's). None = alles.
        # Afstandsbedieningen krijgen een filter zodat een token-pagina alleen
        # updates van de toegewezen devices ziet (geen lek van andere lampen).
        self._clients: Dict[WebSocket, Optional[set]] = {}
        self._lock = asyncio.Lock()

    # -- Browser-clients ----------------------------------------------------

    async def connect_client(self, ws: WebSocket, only_ids: Optional[set] = None) -> None:
        await ws.accept()
        async with self._lock:
            self._clients[ws] = only_ids

    async def disconnect_client(self, ws: WebSocket) -> None:
        async with self._lock:
            self._clients.pop(ws, None)

    async def broadcast(self, message: dict) -> None:
        """Stuur een bericht naar alle browser-clients. Clients met een filter
        krijgen alleen ``lamp``-updates van hun eigen devices. Dode sockets
        worden opgeruimd."""
        async with self._lock:
            clients = list(self._clients.items())
        lamp_id = None
        if message.get("type") == "lamp":
            lamp_id = message.get("lamp", {}).get("id")
        dood = []
        for ws, only_ids in clients:
            if only_ids is not None and lamp_id is not None and lamp_id not in only_ids:
                continue
            try:
                await ws.send_json(message)
            except Exception:
                dood.append(ws)
        if dood:
            async with self._lock:
                for ws in dood:
                    self._clients.pop(ws, None)

    # -- Device-sockets (ESP32's) ------------------------------------------

    async def register_device(self, lamp_id: str, ws: WebSocket) -> bool:
        """Accepteer een nieuwe device-verbinding voor ``lamp_id``, tenzij er
        al een levende (nog open) socket voor dit id is geregistreerd.

        Zonder deze check kan iedereen die het id van een bestaande lamp weet
        (er is geen authenticatie op ``/ws/device``) zich ermee aanmelden en zo
        de live verbinding overnemen: commando's gaan dan naar de aanvaller
        i.p.v. naar de echte ESP32, terwijl de lamp in de UI gewoon
        'online' lijkt. Door de overname te weigeren als de bestaande socket
        nog daadwerkelijk open is, sluiten we die hijack af.

        Een dode/gesloten oude socket (bv. na een netwerkblip of stroomstoring
        van de ESP32) wordt gewoon vervangen — dat is een normale reconnect en
        blijft dus mogelijk.

        Geeft ``True`` als de verbinding is geaccepteerd, ``False`` als hij is
        geweigerd. Bij ``False`` heeft deze aanroep de socket niet geaccepteerd;
        de aanroeper moet hem sluiten (bv. met code 1008)."""
        async with self._lock:
            bestaande = self._devices.get(lamp_id)
            if bestaande is not None and self._is_alive(bestaande):
                return False
            await ws.accept()
            self._devices[lamp_id] = ws
            return True

    @staticmethod
    def _is_alive(ws: WebSocket) -> bool:
        """True als een eerder geregistreerde device-socket nog daadwerkelijk
        open is (en niet alleen nog in de dict staat, bv. omdat de disconnect
        nog niet is verwerkt)."""
        return (
            ws.client_state == WebSocketState.CONNECTED
            and ws.application_state == WebSocketState.CONNECTED
        )

    async def unregister_device(self, lamp_id: str) -> None:
        async with self._lock:
            if self._devices.get(lamp_id) is not None:
                del self._devices[lamp_id]

    def is_online(self, lamp_id: str) -> bool:
        return lamp_id in self._devices

    async def send_command(self, lamp_id: str, state: bool) -> bool:
        """Stuur een aan/uit-commando naar één ESP32. Geeft False als de lamp
        offline is (geen device-socket)."""
        ws = self._devices.get(lamp_id)
        if ws is None:
            return False
        try:
            await ws.send_json({"type": "command", "state": bool(state)})
            return True
        except Exception:
            await self.unregister_device(lamp_id)
            return False


# Eén gedeelde instantie voor de hele app.
manager = ConnectionManager()
