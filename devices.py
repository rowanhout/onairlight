"""Device-registry voor on-air lampen (JSON-persistentie).

Gemodelleerd op het opslagpatroon uit ``auth.py``: laden/opslaan via ``Path`` +
``json``. Elke lamp is een ESP32 die zich met zijn ``id`` aanmeldt via de
device-WebSocket. De registry bewaart de configuratie (naam, ruimte, groep) en
de laatst bekende status (aan/uit, online/offline).
"""

import json
import os
import threading
from pathlib import Path
from typing import Optional

# Zelfde opslaglocatie als auth.py: standaard naast de code, in de cloud via
# ONAIR_DATA_DIR naar een persistent volume.
_DATA_DIR = Path(os.getenv("ONAIR_DATA_DIR") or Path(__file__).resolve().parent)
_DATA_DIR.mkdir(parents=True, exist_ok=True)
LAMPS_FILE = _DATA_DIR / "lamps.json"

# Eén lock zodat gelijktijdige WebSocket- en HTTP-handlers de JSON niet corrumperen.
_lock = threading.Lock()


def _load() -> dict:
    if not LAMPS_FILE.exists():
        return {}
    return json.loads(LAMPS_FILE.read_text())


def _save(lamps: dict) -> None:
    LAMPS_FILE.write_text(json.dumps(lamps, indent=2, ensure_ascii=False))


def _default(lamp_id: str) -> dict:
    return {
        "id": lamp_id,
        "naam": lamp_id,
        "ruimte": "",
        "groep": "",
        "state": False,     # False = uit, True = aan (on-air)
        "online": False,
        "last_seen": None,
    }


def all_lamps() -> list[dict]:
    """Alle lampen, gesorteerd op ruimte + naam."""
    with _lock:
        lamps = _load()
    return sorted(lamps.values(), key=lambda l: (l.get("ruimte", ""), l.get("naam", "")))


def get(lamp_id: str) -> Optional[dict]:
    with _lock:
        return _load().get(lamp_id)


def upsert(lamp_id: str, **velden) -> dict:
    """Maak of werk een lamp bij. Onbekende lampen worden automatisch aangemaakt
    met standaardwaarden (zo registreert een nieuwe ESP32 zich vanzelf)."""
    with _lock:
        lamps = _load()
        lamp = lamps.get(lamp_id) or _default(lamp_id)
        lamp.update(velden)
        lamp["id"] = lamp_id  # id blijft altijd de sleutel
        lamps[lamp_id] = lamp
        _save(lamps)
        return lamp


def update_meta(lamp_id: str, naam: Optional[str] = None, ruimte: Optional[str] = None,
                groep: Optional[str] = None) -> Optional[dict]:
    """Werk de weergavevelden (naam/ruimte/groep) van een bestaande lamp bij.
    De ``id`` blijft ongemoeid (die zit in de ESP32-config). Geeft ``None`` als
    de lamp niet bestaat; alleen meegegeven (niet-``None``) velden worden gezet."""
    with _lock:
        lamps = _load()
        lamp = lamps.get(lamp_id)
        if lamp is None:
            return None
        if naam is not None:
            lamp["naam"] = naam
        if ruimte is not None:
            lamp["ruimte"] = ruimte
        if groep is not None:
            lamp["groep"] = groep
        _save(lamps)
        return lamp


def set_state(lamp_id: str, state: bool) -> Optional[dict]:
    """Zet de gewenste aan/uit-status. Geeft de lamp terug, of None als onbekend."""
    with _lock:
        lamps = _load()
        lamp = lamps.get(lamp_id)
        if lamp is None:
            return None
        lamp["state"] = bool(state)
        _save(lamps)
        return lamp


def set_online(lamp_id: str, online: bool, last_seen: Optional[str] = None) -> Optional[dict]:
    with _lock:
        lamps = _load()
        lamp = lamps.get(lamp_id)
        if lamp is None:
            return None
        lamp["online"] = bool(online)
        if last_seen is not None:
            lamp["last_seen"] = last_seen
        _save(lamps)
        return lamp


def delete(lamp_id: str) -> bool:
    with _lock:
        lamps = _load()
        if lamp_id not in lamps:
            return False
        del lamps[lamp_id]
        _save(lamps)
        return True


def in_group(groep: str) -> list[dict]:
    """Alle lampen in een groep óf ruimte met deze naam (case-insensitive)."""
    groep_l = groep.lower()
    return [
        l for l in all_lamps()
        if l.get("groep", "").lower() == groep_l or l.get("ruimte", "").lower() == groep_l
    ]
