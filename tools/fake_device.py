"""Simuleer een ESP32-lamp zonder hardware, om de server end-to-end te testen.

Maakt verbinding met de device-WebSocket, registreert zich, beantwoordt
commando's en stuurt periodiek een heartbeat. Print elke statuswijziging.

Gebruik:
    pip install websockets
    python tools/fake_device.py --id studio-1 --naam "Studio 1" --ruimte "Studio 1"
"""

import argparse
import asyncio
import json
from urllib.parse import urlencode

import websockets


async def run(args) -> None:
    params = {"id": args.id}
    if args.naam:
        params["naam"] = args.naam
    if args.ruimte:
        params["ruimte"] = args.ruimte
    url = f"{args.server.rstrip('/')}/ws/device?{urlencode(params)}"

    state = False
    async with websockets.connect(url) as ws:
        print(f"[verbonden] {url}")

        async def heartbeat():
            while True:
                await asyncio.sleep(args.heartbeat)
                await ws.send(json.dumps({"type": "ping"}))

        hb = asyncio.create_task(heartbeat())
        try:
            async for raw in ws:
                msg = json.loads(raw)
                if msg.get("type") == "command":
                    state = bool(msg.get("state"))
                    print(f"[commando] lamp -> {'AAN (ON AIR)' if state else 'UIT'}")
                    # Bevestig de werkelijke status terug naar de server.
                    await ws.send(json.dumps({"type": "state", "state": state}))
                elif msg.get("type") == "pong":
                    pass
        finally:
            hb.cancel()


def main() -> None:
    p = argparse.ArgumentParser(description="Gesimuleerde on-air lamp")
    p.add_argument("--server", default="ws://localhost:8080", help="server-WS-URL")
    p.add_argument("--id", default="studio-1")
    p.add_argument("--naam", default="Teststudio")
    p.add_argument("--ruimte", default="Studio 1")
    p.add_argument("--heartbeat", type=float, default=15.0)
    asyncio.run(run(p.parse_args()))


if __name__ == "__main__":
    main()
