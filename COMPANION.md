# Bitfocus Companion-integratie

De on-air lamp is te bedienen vanuit Companion **zonder eigen module**, via de
ingebouwde **Generic HTTP**-module. De REST-API is daar specifiek op ontworpen.

> Vervang in de voorbeelden `SERVER` door het IP/poort van de server
> (bijv. `192.168.1.50:8080`), `LAMP` door het lamp-id (bijv. `studio-1`) en
> `SLEUTEL` door je `ONAIR_API_KEY` uit `.env`.

## 1. Connection toevoegen

1. **Connections → Add connection → "Generic: HTTP Requests"**.
2. Geen vaste base-URL nodig; we zetten de volledige URL per knop.

## 2. Knop "ON AIR aan"

- **Press → actions → HTTP: POST**
  - URL: `http://SERVER/api/lamps/LAMP/on`
  - Headers: voeg toe `X-API-Key` met waarde `SLEUTEL`

## 3. Knop "uit"

- Zelfde, maar URL: `http://SERVER/api/lamps/LAMP/off`

## 4. Eén knop die toggelt

- POST naar `http://SERVER/api/lamps/LAMP/toggle`

## 5. Hele ruimte/groep tegelijk

- Aan:  `http://SERVER/api/groups/GROEP/on`
- Uit:  `http://SERVER/api/groups/GROEP/off`

`GROEP` matcht op het veld **groep** óf **ruimte** van de lampen, zodat je
bijvoorbeeld alle lampen van "Studio 1" in één keer schakelt.

## 6. Feedback: knop rood als de lamp aan staat

Companion kan periodiek de status pollen en zo de knopkleur sturen.

1. **Connection → Variables / HTTP polling**: doe elke ~1 s een
   **GET** `http://SERVER/api/lamps/LAMP/state` met header `X-API-Key: SLEUTEL`.
   De respons is JSON met o.a. `"state": true/false` en `"online": true/false`.
2. Maak op de knop een **feedback** op basis van de variabele `state`:
   - `state == true`  → achtergrond **rood** (ON AIR),
   - `state == false` → achtergrond donker/uit,
   - `online == false` → bijv. een waarschuwingskleur (lamp niet bereikbaar).

Zo zie je op de Stream Deck / Companion-knop in één oogopslag of de opname-
indicatie daadwerkelijk brandt.
