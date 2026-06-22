"""Hulpscript om een gebruiker voor de web-app aan te maken.

Gebruik:
    python create_user.py <gebruikersnaam> "<Volledige naam>" [--admin]

Het wachtwoord wordt interactief gevraagd (niet zichtbaar in shell-historie).
"""

import getpass
import sys

import auth


def main() -> int:
    args = [a for a in sys.argv[1:] if a != "--admin"]
    admin = "--admin" in sys.argv
    if len(args) < 2:
        print(__doc__)
        return 1

    username, naam = args[0], args[1]
    wachtwoord = getpass.getpass("Wachtwoord: ")
    if wachtwoord != getpass.getpass("Herhaal wachtwoord: "):
        print("Wachtwoorden komen niet overeen.")
        return 1

    if auth.create_user(username, naam, wachtwoord, admin=admin):
        print(f"Gebruiker '{username}' aangemaakt{' (admin)' if admin else ''}.")
        return 0
    print(f"Gebruiker '{username}' bestaat al.")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
