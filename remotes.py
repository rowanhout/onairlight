"""Registry van afstandsbedieningen (remotes), met JSON-persistentie.

Een afstandsbediening is een aparte, deelbare webpagina (kiosk/wandtablet) met
configureerbare knoppen. Elke knop stuurt één of meer devices aan met een actie
(on/off/toggle). De pagina is bereikbaar via een geheime token-link, zonder
login. Opslagpatroon gelijk aan ``devices.py`` (Path + json + lock).
"""

import json
import os
import re
import secrets
import threading
from pathlib import Path
from typing import Optional

_DATA_DIR = Path(os.getenv("ONAIR_DATA_DIR") or Path(__file__).resolve().parent)
_DATA_DIR.mkdir(parents=True, exist_ok=True)
REMOTES_FILE = _DATA_DIR / "remotes.json"

_lock = threading.Lock()


def _load() -> dict:
    if not REMOTES_FILE.exists():
        return {}
    return json.loads(REMOTES_FILE.read_text())


def _save(remotes: dict) -> None:
    REMOTES_FILE.write_text(json.dumps(remotes, indent=2, ensure_ascii=False))


def _slugify(naam: str) -> str:
    slug = re.sub(r"[^a-z0-9]+", "-", naam.lower()).strip("-")
    return slug or "rc"


def _new_token() -> str:
    return secrets.token_urlsafe(16)


def all_remotes() -> list[dict]:
    with _lock:
        remotes = _load()
    return sorted(remotes.values(), key=lambda r: r.get("naam", ""))


def get(slug: str) -> Optional[dict]:
    with _lock:
        return _load().get(slug)


def create(naam: str) -> dict:
    """Maak een nieuwe afstandsbediening met een unieke slug en een verse token."""
    with _lock:
        remotes = _load()
        base = _slugify(naam)
        slug = base
        i = 2
        while slug in remotes:
            slug = f"{base}-{i}"
            i += 1
        remote = {
            "slug": slug,
            "naam": naam,
            "token": _new_token(),
            "kolommen": 2,
            "knoppen": [],
        }
        remotes[slug] = remote
        _save(remotes)
        return remote


def update(slug: str, naam: Optional[str] = None, kolommen: Optional[int] = None,
           knoppen: Optional[list] = None) -> Optional[dict]:
    with _lock:
        remotes = _load()
        remote = remotes.get(slug)
        if remote is None:
            return None
        if naam is not None:
            remote["naam"] = naam
        if kolommen is not None:
            remote["kolommen"] = max(1, min(6, int(kolommen)))
        if knoppen is not None:
            remote["knoppen"] = knoppen
        _save(remotes)
        return remote


def regenerate_token(slug: str) -> Optional[dict]:
    with _lock:
        remotes = _load()
        remote = remotes.get(slug)
        if remote is None:
            return None
        remote["token"] = _new_token()
        _save(remotes)
        return remote


def delete(slug: str) -> bool:
    with _lock:
        remotes = _load()
        if slug not in remotes:
            return False
        del remotes[slug]
        _save(remotes)
        return True
