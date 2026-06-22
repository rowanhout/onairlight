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

    async def register_device(self, lamp_id: str, ws: WebSocket) -> None:
        await ws.accept()
        async with self._lock:
            self._devices[lamp_id] = ws

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
