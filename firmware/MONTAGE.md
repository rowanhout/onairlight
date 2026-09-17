# Montagehandleiding — On-Air Lamp (WT32-ETH01 + PoE-splitter)

Stap-voor-stap-instructie voor de technicus om één on-air lamp samen te stellen,
te bedraden en te testen. Eén UTP-kabel de lamp in; die levert via een
PoE-splitter zowel **data** (voor de ESP32) als **12V-voeding** (voor de lamp).

> **Werkwijze:** lees eerst de hele handleiding door. Werk **spanningsloos** en
> controleer elke voedingsspanning met een **multimeter vóórdat** je gevoelige
> elektronica aansluit. Een verkeerd ingestelde spanning maakt de ESP32 stuk.

---

## 1. Onderdelen (per lamp)

| # | Onderdeel | Specificatie |
|---|-----------|--------------|
| 1 | D&R ON Air Light | 12V DC, ~0,45 A (5,4 W), met schroefklemmenblok |
| 2 | WT32-ETH01 | ESP32 met bedraad Ethernet (LAN8720) |
| 3 | PoE-splitter | **802.3at**, **12V-uitgang**, met RJ45-datadoorvoer |
| 4 | Step-down (buck) | **12V → 5V**, ≥ 1 A |
| 5 | MOSFET | IRLZ44N (logic-level, TO-220) |
| 6 | Weerstand | 220 Ω (gate) |
| 7 | Weerstand | 10 kΩ (pull-down) |
| 8 | Perfboard + montagedraad, krimpkous | klein |
| 9 | Kort RJ45-patchkabeltje | splitter → WT32-ETH01 |
| 10 | UTP-kabel (Cat5e/Cat6) | naar de PoE-switch/injector |

Aan de andere kant van de kabel: een **PoE-switch of PoE-injector (802.3at)**.

Voor het flashen (eenmalig, per board): een **USB-naar-TTL-adapter (3,3V logic)**.

---

## 2. Stap A — Firmware flashen (doe dit eerst, op de werkbank)

De WT32-ETH01 heeft geen USB-poort; flashen gaat via een USB-naar-TTL-adapter.

1. Verbind de USB-TTL-adapter (op **3,3V**) met de WT32-ETH01:
   - `5V`  → `5V`  (of `3V3` → `3V3`)
   - `GND` → `GND`
   - adapter `TX` → WT32 `RX0`
   - adapter `RX` → WT32 `TX0`
2. Verbind **IO0 → GND** om in de bootloader te starten. Power-cyclen (of EN-reset).
3. Bereid `config.h` voor:
   ```sh
   cp firmware/onair-esp32/config.h.example firmware/onair-esp32/config.h
   ```
   Zet in `config.h`:
   - `BOARD` → `BOARD_WT32_ETH01`
   - `SERVER_HOST` / `SERVER_PORT` / `USE_TLS` (LAN: `ws`/8080, cloud: `wss`/443)
   - `LAMP_ID` (uniek per lamp!), `LAMP_NAAM`, `LAMP_RUIMTE`
   - `LAMP_PIN` mag op `-1` blijven → board-default **GPIO 4**
4. Upload (PlatformIO: `pio run -e wt32-eth01 -t upload`, of Arduino IDE met board
   **ESP32 Dev Module**). Zie `firmware/README.md` voor details.
5. Na uploaden: **IO0 los van GND** halen en resetten. Controleer in de seriële
   monitor (115200 baud) dat het board opstart.

> Flash de firmware vóór inbouw — dan weet je zeker dat het board werkt en hoef
> je later niet meer bij de USB-pinnen.

---

## 3. Stap B — MOSFET-printje bouwen

Bouw op een klein stukje perfboard de schakeltrap. IRLZ44N (TO-220), pinnen met
de bedrukte kant naar je toe: **1 = Gate, 2 = Drain, 3 = Source**.

```
   WT32 GPIO4 ──[ 220 Ω ]──┬── Gate (pin 1)
                           │
                      [ 10 kΩ ]
                           │
   WT32 GND ───────────────┴── Source (pin 3)  ── naar massazijde Control

                              Drain (pin 2)     ── naar andere Control-klem
```

- **220 Ω** tussen GPIO4 en de gate.
- **10 kΩ** tussen gate en GND (pull-down → lamp blijft **uit** bij opstart,
  óók voordat de software de pin instelt).
- **Source** gaat naar de massazijde (zie Stap D), **Drain** naar de andere
  Control-klem.

---

## 4. Stap C — Voeding voorbereiden en controleren (multimeter!)

1. Zet de **PoE-splitter** op **12V** uitgang (indien instelbaar) en sluit hem
   testmatig aan op de PoE-switch/injector. **Meet de uitgang: het moet 12V zijn**
   en let op de **polariteit** (+ en −). Noteer welke ader + is.
2. Sluit de **buck-converter** aan op de 12V (in) en **stel hem af op exact 5,0V**
   aan de uitgang. **Meet de uitgang met de multimeter vóórdat** je de
   WT32-ETH01 aansluit. Een buck die per ongeluk 12V doorgeeft, sloopt het board.

> Doe deze stap altijd, ook als de buck "voorafgesteld" lijkt. Controleer 5V.

---

## 5. Stap D — Bedrading in de lamp

De D&R-lamp heeft een schroefklemmenblok. De **+Led Strip / −Led Strip**-klemmen
zijn af-fabriek al bedraad — **niet aankomen**. Je gebruikt alleen de
voedingsklemmen en de twee **Control**-klemmen.

1. **Lampvoeding:** splitter **12V+** → klem **+12V DC**; splitter **12V−** →
   klem **−12V DC**.
2. **ESP32-voeding:** tak de 12V af naar de buck-ingang; buck-uitgang **5V** →
   WT32 `5V`, buck **GND** → WT32 `GND`.
3. **Data:** splitter-datapoort (RJ45) → met kort patchkabeltje → RJ45 van de
   WT32-ETH01.
4. **Schakelen (MOSFET op de Control-klemmen):**
   - MOSFET **Source** → de **massazijde** van het Control-circuit (de klem die
     met −12V is doorverbonden, pos 3).
   - MOSFET **Drain** → de **andere Control-klem** (pos 4, de −LED-zijde).
   - **Gate** → via 220 Ω naar WT32 **GPIO4**, met 10 kΩ pull-down naar GND.
5. **Gemeenschappelijke massa (belangrijk):** WT32 `GND`, buck-GND, MOSFET-source
   en de lamp-`−12V` moeten allemaal op dezelfde massa zitten. Bij deze bedrading
   gebeurt dat vanzelf (alles hangt aan −12V), maar controleer het.

Resultaat: **GPIO4 HIGH = Control gesloten = lamp AAN (ON AIR)**; LOW = uit.

---

## 6. Stap E — Test op de werkbank (vóór inbouw afmonteren)

1. Sluit aan: PoE-switch/injector → UTP → splitter → (12V naar lamp + buck,
   data naar WT32).
2. Open de **seriële monitor** (115200 baud). Je hoort te zien:
   - `[eth] IP-adres: …`
   - `[ws] verbonden met server: …`
3. Controleer in de web-app dat de lamp **online** verschijnt (juiste naam/ruimte).
4. Schakel via de web-app of een afstandsbediening: de lamp moet **direct** aan/uit
   gaan en in de monitor zie je het commando + de teruggemelde status.
5. Trek de stekker → de lamp gaat uit. Sluit weer aan → de lamp blijft **uit** tot
   de server hem aanstuurt (veilige default).

---

## 7. Stap F — Inbouwen en installeren

1. Monteer het MOSFET-printje, de buck en de WT32-ETH01 netjes in de
   lamp­behuizing; isoleer blanke contacten (krimpkous) en zet onderdelen vast.
2. Voer de **ene UTP-kabel** de behuizing in via een trekontlasting.
3. Sluit het deksel; controleer dat niets klemt of kortsluit.
4. Op locatie: UTP naar de **PoE-switch/injector (802.3at)**.
5. **Zet de switchpoort op 10 Mbps full duplex** — zie de waarschuwing hieronder.
6. Eindcontrole: lamp verschijnt online in de web-app en schakelt correct.

> ### ⚠ De switchpoort moet op 10 Mbps full duplex
>
> Niet op Auto. Op 100 Mbit is het Ethernet-signaal van dit bord niet schoon
> genoeg: frames met een bitfout worden door de hardware geruisloos weggegooid.
> Kleine pakketjes komen wel door, dus de lamp krijgt netjes een IP-adres en
> alles lijkt in orde — maar alles wat een volle frame nodig heeft (de
> beveiligde verbinding met de server) komt nooit aan. **Zonder enige
> foutmelding, nergens.**
>
> De lamp doet het dan simpelweg niet, en er is niets dat verklaart waarom.
> Deze ene instelling heeft ons dagen gekost om te vinden.
>
> In UniFi: **Ports** → de betreffende poort → **Link Speed** → `10 Mbps FDX`.
> Bij andere merken heet dit meestal *Port Speed* of *Duplex/Speed*.
>
> Verhuist de lamp ooit naar een andere poort, dan moet die instelling mee.
> Zet `#define VEREIS_10MBIT 1` in `config.h`, dan waarschuwt de firmware er
> luid over in de seriële monitor zodra de link tóch op 100 Mbit staat.

---

## 8. Checklist

- [ ] Firmware geflasht, `LAMP_ID` uniek, `BOARD = BOARD_WT32_ETH01`.
- [ ] Splitter-uitgang gemeten = 12V, polariteit bekend.
- [ ] Buck-uitgang gemeten = 5,0V **vóór** aansluiten WT32-ETH01.
- [ ] +Led Strip / −Led Strip niet aangeraakt.
- [ ] MOSFET: Source = massazijde Control, Drain = andere Control-klem.
- [ ] 220 Ω gate-weerstand + 10 kΩ pull-down aanwezig.
- [ ] Gemeenschappelijke massa gecontroleerd.
- [ ] Lamp uit bij opstart; schakelt via web-app/afstandsbediening.
- [ ] **Switchpoort staat op 10 Mbps full duplex** (niet op Auto).

## 9. Veelvoorkomende fouten

- **Lamp krijgt wel een IP-adres maar verbindt nooit met de server:** vrijwel
  altijd de linksnelheid. Zet de switchpoort op **10 Mbps full duplex**. Dit
  ziet er uit als een certificaat-, poort- of firewallprobleem en is dat niet;
  begin hier voordat je iets anders onderzoekt.
- **Lamp doet niets:** Control-klemmen verwisseld of MOSFET Source/Drain
  omgedraaid; of geen gemeenschappelijke massa.
- **WT32-ETH01 start niet / rookt:** buck stond niet op 5V — altijd meten.
- **Lamp altijd aan:** Control-klemmen permanent doorverbonden, of GPIO/pull-down
  fout (gate "zweeft").
- **Geen netwerk:** datakabel splitter→WT32 niet goed, of splitter niet op
  802.3at-poort.
