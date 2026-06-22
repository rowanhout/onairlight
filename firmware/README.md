# On-Air Lamp - ESP32-firmware

Firmware voor de fysieke on-air lampen. Elke lamp is een ESP32 met bedraad
Ethernet (PoE) die als **WebSocket-client** verbindt met de centrale
FastAPI-server. De server bepaalt de aan/uit-stand; de ESP32 schakelt de lamp
en meldt zijn werkelijke status terug.

## Werking

- De ESP32 verbindt met de server op pad `/ws/device` en geeft zijn identiteit
  mee als query-parameters:
  `ws(s)://<host>:<poort>/ws/device?id=<id>&naam=<naam>&ruimte=<ruimte>`
  (de waarden worden URL-geencodeerd).
- **Protocol** (JSON-tekstberichten):
  - server -> esp: `{"type":"command","state":true|false}` -> lamp schakelen en
    daarna echte status terugsturen.
  - server -> esp: `{"type":"pong"}` -> antwoord op heartbeat, negeren.
  - esp -> server: `{"type":"state","state":true|false}` -> bij elke wijziging
    en direct na (re)connect.
  - esp -> server: `{"type":"ping"}` -> periodieke heartbeat (`HEARTBEAT_MS`).
- Vlak na connect stuurt de server zelf een `command` met de laatst bekende
  gewenste stand; die past de ESP32 toe.
- **Veilige default:** bij opstart staat de lamp UIT (GPIO als OUTPUT, LOW)
  *voordat* het netwerk wordt gestart. Een herstart zet de ON-AIR-lamp dus nooit
  per ongeluk aan.

## Hardware-bedrading

De D&R ON Air Light wordt continu gevoed met +12V / -12V. De ESP32 schakelt de
lamp **laag-zijdig** via een logic-level MOSFET (IRLZ44N) op de twee
**Control**-klemmen (het massacircuit van de LED-strip).

Stroomvoorziening van de 12V:

- **Olimex ESP32-POE-ISO:** het bord is PoE-gevoed. De 12V voor de lamp komt uit
  een 5V -> 12V boost-converter.
- **WT32-ETH01:** de 12V komt rechtstreeks uit de PoE-splitter.

MOSFET-bedrading (IRLZ44N), laag-zijdig op de twee Control-klemmen:

- **Source** -> massazijde van het Control-circuit (pos 3 / -12V).
- **Drain**  -> de andere Control-klem (pos 4 / -LED strip).
- **Gate**   -> GPIO van de ESP32 via een **220 Ohm** weerstand.
- **Pull-down:** **10 kOhm** van gate naar GND (houdt de MOSFET dicht zolang de
  GPIO nog niet als output is geinitialiseerd -> lamp blijft uit bij boot).

Active-HIGH: GPIO HIGH = lamp AAN (ON AIR), GPIO LOW = lamp UIT.

Standaard lamp-GPIO per bord:

| Bord                  | Lamp-GPIO |
|-----------------------|-----------|
| Olimex ESP32-POE-ISO  | GPIO 32   |
| WT32-ETH01            | GPIO 4    |

> De **+Led Strip / -Led Strip** klemmen zijn af-fabriek al bedraad. Niet
> aankomen; alleen de twee **Control**-klemmen gebruik je voor de MOSFET.

## Configureren

Kopieer het voorbeeldbestand en vul je gegevens in:

```sh
cp firmware/onair-esp32/config.h.example firmware/onair-esp32/config.h
```

Stel in `config.h` minimaal in:

- `BOARD` -> `BOARD_OLIMEX_POE_ISO` (standaard) of `BOARD_WT32_ETH01`.
- `SERVER_HOST`, `SERVER_PORT`, `USE_TLS` (zie ws vs wss hieronder).
- `LAMP_ID` (uniek!), `LAMP_NAAM`, `LAMP_RUIMTE`.
- Eventueel `LAMP_PIN` (`-1` = board-default) en `HEARTBEAT_MS` (15000).
- Optioneel `LAMP_HOSTNAME` (uitgecommentarieerd) voor een vaste netwerknaam.

## ws (LAN) vs wss (cloud)

- **ws (LAN, simpel en robuust):** server draait lokaal op je netwerk zonder
  TLS. Zet `USE_TLS = false`, `SERVER_PORT = 8080`. Aanbevolen op een vertrouwd
  LAN.
- **wss (cloud / Railway):** server draait achter HTTPS. Zet `USE_TLS = true`,
  `SERVER_PORT = 443`.
  **Let op:** bij wss wordt het server-certificaat **niet** gevalideerd
  (insecure mode van arduinoWebSockets >= 2.4.0). De verbinding is versleuteld,
  maar niet beschermd tegen man-in-the-middle.

## Flashen via PlatformIO

```sh
cd firmware/onair-esp32
pio run -e olimex -t upload        # Olimex ESP32-POE-ISO (standaard)
pio run -e wt32-eth01 -t upload    # WT32-ETH01
pio device monitor                 # seriele monitor (115200 baud)
```

## Flashen via Arduino IDE

1. Voeg in **Bestand -> Voorkeuren -> Aanvullende Board Manager-URL's** toe:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. Installeer via **Tools -> Board -> Boards Manager** het pakket
   **esp32** (van Espressif).
3. Kies het juiste bord onder **Tools -> Board**:
   - Olimex: **OLIMEX ESP32-PoE-ISO**
   - WT32-ETH01: **ESP32 Dev Module**
4. Installeer via **Tools -> Bibliotheken beheren** de libraries:
   - **arduinoWebSockets** (links2004) - versie >= 2.4.0
   - **ArduinoJson** (bblanchon) - versie 7.x
5. Kopieer `config.h.example` naar `config.h` en vul je instellingen in.
6. Open `onair-esp32.ino` en klik op **Uploaden**.

## Testen

1. Open de **seriele monitor** op **115200 baud**.
2. Je hoort te zien:
   - het toegekende **IP-adres** (`[eth] IP-adres: ...`),
   - `[ws] verbonden met server: ...`.
3. Daarna verschijnt de lamp in de web-app en is hij schakelbaar. Bij elk
   commando zie je in de monitor de lamp aan/uit gaan en de status teruggemeld
   worden.

Wil je eerst **zonder hardware** testen? Gebruik dan de gesimuleerde lamp
`tools/fake_device.py` in de repo-root:

```sh
pip install websockets
python tools/fake_device.py --id studio-1 --naam "Studio 1" --ruimte "Studio 1"
```
