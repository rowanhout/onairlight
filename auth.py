"""Sessie-/wachtwoordauthenticatie voor de On-Air web-app.

Gemodelleerd op het bewezen patroon uit de bestaande ``auth.py`` in de
repo-root (bcrypt + JSON-opslag), maar met een eigen gebruikersbestand zodat
dit project los staat van de Montage Planning Dashboard.
"""

import json
import os
from pathlib import Path
from typing import Optional

import bcrypt
from fastapi import Request

# Opslaglocatie. Standaard naast de code (lokaal draaien); in de cloud zet je
# ONAIR_DATA_DIR naar een persistent volume (bijv. /data op Railway) zodat de
# gebruikers niet verdwijnen bij een redeploy.
_DATA_DIR = Path(os.getenv("ONAIR_DATA_DIR") or Path(__file__).resolve().parent)
_DATA_DIR.mkdir(parents=True, exist_ok=True)
USERS_FILE = _DATA_DIR / "users.json"


def _load_users() -> dict:
    if not USERS_FILE.exists():
        return {}
    return json.loads(USERS_FILE.read_text())


def _save_users(users: dict) -> None:
    USERS_FILE.write_text(json.dumps(users, indent=2, ensure_ascii=False))


def hash_password(password: str) -> str:
    return bcrypt.hashpw(password.encode(), bcrypt.gensalt()).decode()


def verify_password(plain: str, hashed: str) -> bool:
    return bcrypt.checkpw(plain.encode(), hashed.encode())


def login(username: str, password: str) -> bool:
    users = _load_users()
    user = users.get(username)
    if not user:
        return False
    if not user.get("actief", True):
        return False
    return verify_password(password, user["wachtwoord"])


def create_user(username: str, naam: str, password: str, admin: bool = False,
                actief: bool = True) -> bool:
    """Maak een nieuwe gebruiker aan. False als de gebruikersnaam al bestaat."""
    users = _load_users()
    if username in users:
        return False
    users[username] = {
        "naam": naam,
        "wachtwoord": hash_password(password),
        "actief": actief,
        "admin": admin,
    }
    _save_users(users)
    return True


def get_user(username: str) -> Optional[dict]:
    return _load_users().get(username)


def is_admin(username: str) -> bool:
    user = get_user(username)
    return bool(user and user.get("admin", False))


def get_display_naam(username: str) -> str:
    user = get_user(username)
    if user:
        return user.get("naam") or username
    return username


def get_session_user(request: Request) -> Optional[str]:
    return request.session.get("user")
