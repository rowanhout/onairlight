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

> Voor de technicus: een volledige stap-voor-stap montage- en
> bedradingshandleiding staat in [`MONTAGE.md`](MONTAGE.md) (opzet met
> WT32-ETH01 + PoE-splitter).

De D&R ON Air Light wordt continu gevoed met +12V / -12V. De ESP32 schakelt de
lamp **laag-zijdig** via een logic-level MOSFET (IRLZ44N) op de twee
**Control**-klemmen (het massacircuit van de LED-strip).

Stroomvoorziening van de 12V:

- **Olimex ESP32-POE2 (aanbevolen):** het bord haalt 802.3at-PoE (25W) uit de
  UTP-kabel en levert zelf **12V @ 2A**. De lamp (12V / ~0,45A) hangt dus
  rechtstreeks aan de 12V-uitgang van het bord. **Geen PoE-splitter en geen
  buck/boost-converter nodig** -- een UTP-kabel de lamp in en klaar.
- **Olimex ESP32-POE-ISO:** het bord is PoE-gevoed maar levert extern maar ~1W;
  te weinig voor de lamp. De 12V moet dan van een aparte bron komen.
- **WT32-ETH01:** de 12V komt rechtstreeks uit een PoE-splitter.

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
| Olimex ESP32-POE2     | GPIO 4    |
| Olimex ESP32-POE-ISO  | GPIO 32   |
| WT32-ETH01            | GPIO 4    |

> Neem je lamp-GPIO nooit uit de Ethernet-pinnen.

### ⚠ Zet de switchpoort op 10 Mbps full duplex

Op 100 Mbit haalt dit bord het niet: het RMII-signaal is niet schoon genoeg,
en frames waarin de hardware-checksumcontrole een fout ziet worden geruisloos
weggegooid. Kleine pakketjes overleven dat (DNS, NTP, het opzetten van een
TCP-verbinding), volle frames niet.

Het gevolg is bijzonder misleidend: de lamp krijgt netjes een IP-adres, kan
namen opzoeken en verbindingen opzetten — maar alles wat een paar volle frames
nodig heeft komt nooit aan. De TLS-handshake is daarvan het eerste slachtoffer,
want daarin stuurt de server zijn certificaatketen van enkele kilobytes. Je ziet
dan `start_ssl_client: -1` en gaat certificaten, poorten en firewalls
onderzoeken, terwijl het probleem in de fysieke laag zit.

Symptomen die allemaal dezelfde oorzaak hebben:

- `start_ssl_client: -1` of een TLS-handshake die niets terugkrijgt
- een HTTP-download die op **0 bytes** blijft staan terwijl het verzoek
  aantoonbaar wél bij de server aankwam
- kleine antwoorden (een 301 van de router) komen wél door
- af en toe een TCP-verbinding die ruim een seconde duurt (hertransmissie)

Diagnose in één meting: laat het bord een pagina ophalen van een machine op
hetzelfde subnet. Komt die niet binnen terwijl de server het verzoek wél zag,
dan is dit het.

Zet `#define VEREIS_10MBIT 1` in `config.h` zodat de firmware luid waarschuwt
als de link toch op 100 Mbit staat. Voor deze toepassing kost 10 Mbit niets: de
lamp stuurt een paar JSON-berichtjes per uur.

### Specifiek voor de Olimex ESP32-POE2

Dit bord gebruikt een **WROVER**-module. Die claimt GPIO16/17 voor PSRAM,
waardoor Olimex de Ethernet-klok naar **GPIO0** heeft verplaatst (op de oudere
POE/POE-ISO is dat GPIO17). De firmware doet dat automatisch; dit is alleen ter
info.

Bezet op de POE2: GPIO 0 (Ethernet-klok), 12 (PHY power), 16/17 (PSRAM),
18/19/21-23/25-27 (Ethernet), 1 (serieel), 2/14/15 (microSD).
Vrij en geschikt als uitgang: **GPIO 4** (standaard), 13, 33, 5.
**GPIO 20 bestaat niet op dit bord.**

> De **+Led Strip / -Led Strip** klemmen zijn af-fabriek al bedraad. Niet
> aankomen; alleen de twee **Control**-klemmen gebruik je voor de MOSFET.

## Configureren

Kopieer het voorbeeldbestand en vul je gegevens in:

```sh
cp firmware/onair-esp32/config.h.example firmware/onair-esp32/config.h
```

Stel in `config.h` minimaal in:

- `BOARD` -> `BOARD_OLIMEX_POE2` (standaard), `BOARD_OLIMEX_POE_ISO` of
  `BOARD_WT32_ETH01`.
- `SERVER_HOST`, `SERVER_PORT`, `USE_TLS` (zie "Hoe de lamp met de server
  verbindt" hieronder).
- `LAMP_ID` (uniek!), `LAMP_NAAM`, `LAMP_RUIMTE`.
- Eventueel `LAMP_PIN` (`-1` = board-default) en `HEARTBEAT_MS` (15000).
- Optioneel `LAMP_HOSTNAME` (uitgecommentarieerd) voor een vaste netwerknaam.

## Hoe de lamp met de server verbindt

Er zijn drie manieren; kies er één in `config.h`. Je kunt er ook **twee**
opgeven: een primaire en een reserve.

### Aanbevolen opzet: lokaal primair, cloud als reserve

Een on-air lamp die alleen werkt als het internet het doet, kan tijdens een
uitzending uitvallen — en op een netwerk dat je niet zelf beheert, kan hij
helemaal nooit gaan werken. Zet daarom de server op het eigen netwerk voorop:

```c
#define SERVER_HOST "192.168.1.50"    // server op het eigen LAN
#define SERVER_PORT 8080
#define USE_TLS     false

#define SERVER2_HOST "web-production-a09cb.up.railway.app"   // reserve
#define SERVER2_PORT 443
#define SERVER2_TLS  true
```

Lukt de primaire 30 seconden niet, dan probeert de lamp de reserve, en zo door.
De lokale route heeft geen internet, geen DNS, geen certificaten en geen
kloppende klok nodig — er is dus vrijwel niets dat kapot kan.

### 1. Cloud via HTTPS (wss)

```c
#define SERVER_HOST "reclamp.madera.video"
#define SERVER_PORT 443
#define USE_TLS     true
```

Het servercertificaat wordt **gevalideerd** tegen de root-certificaten in
`certs.h` (Let's Encrypt ISRG Root X1/X2 + DigiCert Global Root G2). Omdat
validatie een kloppende klok vereist, haalt de firmware eerst de tijd op: NTP,
dan NTP via de router, dan de `Date`-header van een HTTP-antwoord, en als laatste
de bouwdatum van de firmware.

> Waarom `beginSslWithCA()` en niet `beginSSL()`? Afhankelijk van de versie van
> arduinoWebSockets zet `beginSSL()` de "insecure" modus niet, waarna de
> handshake faalt met `start_ssl_client: -1`. De CA-variant werkt op elke
> versie én is veiliger.
>
> Faalt de handshake met `start_ssl_client: -1`, ga dan **niet** meteen
> certificaten of firewalls onderzoeken. Controleer eerst of de switchpoort op
> **10 Mbps full duplex** staat — zie de waarschuwing hierboven. Dat is in de
> praktijk vrijwel altijd de oorzaak: de certificaatketen is het eerste dat een
> paar volle frames nodig heeft, en die komen op 100 Mbit niet aan.

### 2. Lokaal op het LAN

```c
#define SERVER_HOST "192.168.1.50"
#define SERVER_PORT 8080
#define USE_TLS     false
```

## Aansluiten om te flashen

- **Olimex ESP32-POE2:** heeft een **USB-C-poort met USB-serial aan boord**.
  Kabel in de PC, klaar -- je hebt géén losse USB-TTL-adapter nodig en je hoeft
  geen knop in te drukken. Verschijnt er geen poort, installeer dan de driver
  van de USB-serial chip (CH340 of CP210x).
  *Flash het bord bij voorkeur via USB zonder dat de PoE-kabel erin zit.*
- **Olimex ESP32-POE-ISO:** idem, via de USB-poort op het bord.
- **WT32-ETH01:** heeft géén USB. Gebruik een USB-naar-TTL-adapter (3,3V),
  verbind TX/RX gekruist + GND + 5V, en trek **IO0 naar GND** tijdens het
  opstarten om in de bootloader te komen.

## Flashen via PlatformIO

```sh
cd firmware/onair-esp32
pio run -e poe2 -t upload          # Olimex ESP32-POE2 (standaard)
pio run -e olimex -t upload        # Olimex ESP32-POE-ISO
pio run -e wt32-eth01 -t upload    # WT32-ETH01
pio device monitor                 # seriele monitor (115200 baud)
```

## Verbinding uitzoeken

Bij elke start doet de firmware een korte zelftest en print die het resultaat
per stap, zodat je ziet wáár het strandt in plaats van alleen "verbroken":

```
[test] DNS  : altaria.proxy.rlwy.net -> 66.33.22.11  (24 ms)
[test] TCP  : poort 35261 open (41 ms)
[test] HTTP : HTTP/1.1 200 OK
```

- **DNS mislukt** -> je DHCP-server levert geen bruikbare DNS mee; zet
  `DNS_FALLBACK` in `config.h`.
- **TCP onbereikbaar** -> het netwerk of een firewall laat die poort niet door.
- **HTTP geen antwoord** -> de poort staat open, maar er luistert iets anders
  dan de app (bijvoorbeeld een filter-appliance).
- **Alles OK maar de WebSocket verbindt niet** -> bouw met de debug-omgeving;
  de library print dan zelf wat ze verstuurt en terugkrijgt:

```sh
pio run -e poe2-debug -t upload && pio device monitor
```

## Flashen via Arduino IDE

1. Voeg in **Bestand -> Voorkeuren -> Aanvullende Board Manager-URL's** toe:
   `https://espressif.github.io/arduino-esp32/package_esp32_index.json`
2. Installeer via **Tools -> Board -> Boards Manager** het pakket
   **esp32** (van Espressif).
3. Kies het juiste bord onder **Tools -> Board -> esp32**:
   - ESP32-POE2: **OLIMEX ESP32-POE2** (staat die er niet in jouw core-versie?
     kies dan **OLIMEX ESP32-PoE** -- zelfde ESP32, werkt prima)
   - ESP32-POE-ISO: **OLIMEX ESP32-PoE-ISO**
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
