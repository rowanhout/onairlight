// ============================================================================
// onair-esp32.ino  --  Firmware voor de On-Air Lamp (ESP32 + bedraad Ethernet)
// ============================================================================
//
// Werking in het kort:
//  - De ESP32 heeft bedraad Ethernet (PoE, LAN8720) en verbindt als WebSocket-
//    CLIENT met de centrale FastAPI-server op pad /ws/device.
//  - De server stuurt aan/uit-commando's; de ESP32 schakelt de lamp en stuurt
//    daarna zijn werkelijke status terug.
//  - Periodiek stuurt de ESP32 een heartbeat (ping) zodat de server hem als
//    online ziet.
//
// Protocol (JSON-tekstberichten):
//   server -> esp:  {"type":"command","state":true|false}
//                   {"type":"pong"}                          (negeren)
//   esp -> server:  {"type":"state","state":true|false}      (bij wijziging + na connect)
//                   {"type":"ping"}                           (heartbeat)
//
// VEILIGE DEFAULT: bij opstart staat de lamp UIT, en wel VOORDAT het netwerk
// wordt gestart, zodat een herstart nooit per ongeluk de ON-AIR-lamp aanzet.
// ----------------------------------------------------------------------------

#include "config.h"
#include "certs.h"              // root-certificaten voor wss://

#include <ETH.h>
#include <WiFi.h>               // levert WiFi.onEvent + ARDUINO_EVENT_ETH_* events
#include <WiFiClientSecure.h>   // voor de TLS-meting in de zelftest

// Wil je de interne logging van de WebSocket-library zien? Bouw dan met de
// omgeving poe2-debug (zie platformio.ini). Een #define hier werkt niet: de
// library is een eigen vertaaleenheid en ziet macro's uit deze sketch niet.
#include <WebSocketsClient.h>   // links2004/arduinoWebSockets  (>= 2.4.0)
#include <ArduinoJson.h>        // bblanchon/ArduinoJson v7
#include <time.h>

// ===========================================================================
// Board-afhankelijke Ethernet (RMII) configuratie
// ===========================================================================
// Alle PHY-instellingen worden afgeleid uit BOARD (zie config.h). Elke waarde
// is te overrulen door hem in config.h zelf te #definen -- handig als een
// board-revisie bijvoorbeeld een andere klok-modus gebruikt.
//
// We gebruiken bewust OA_-namen: ETH.h definieert zelf al macro's met namen
// als ETH_PHY_ADDR, en die willen we niet herdefinieren.
// Waarden volgens de officiele Olimex-voorbeeldcode en user manual voor de
// ESP32-POE2 (SOFTWARE/ARDUINO/LAN8720-POE2 en SOFTWARE/ESPHOME).
// LET OP: de POE2 gebruikt een WROVER-module; die claimt GPIO16/17 voor PSRAM,
// daarom loopt de Ethernet-klok hier over GPIO0 (en niet GPIO17 zoals op de
// oudere POE/POE-ISO).
#if BOARD == BOARD_OLIMEX_POE2
  #define BOARD_NAAM "Olimex ESP32-POE2"
  #define OA_DEF_PHY_ADDR   0
  #define OA_DEF_PHY_POWER  12
  #define OA_DEF_PHY_MDC    23
  #define OA_DEF_PHY_MDIO   18
  #define OA_DEF_CLK_MODE   ETH_CLOCK_GPIO0_OUT
  #define OA_DEF_LAMP_PIN   4
#elif BOARD == BOARD_OLIMEX_POE_ISO
  #define BOARD_NAAM "Olimex ESP32-POE-ISO"
  #define OA_DEF_PHY_ADDR   0
  #define OA_DEF_PHY_POWER  12
  #define OA_DEF_PHY_MDC    23
  #define OA_DEF_PHY_MDIO   18
  #define OA_DEF_CLK_MODE   ETH_CLOCK_GPIO17_OUT
  #define OA_DEF_LAMP_PIN   32
#elif BOARD == BOARD_WT32_ETH01
  #define BOARD_NAAM "WT32-ETH01"
  #define OA_DEF_PHY_ADDR   1
  #define OA_DEF_PHY_POWER  16
  #define OA_DEF_PHY_MDC    23
  #define OA_DEF_PHY_MDIO   18
  #define OA_DEF_CLK_MODE   ETH_CLOCK_GPIO0_IN
  #define OA_DEF_LAMP_PIN   4
#else
  #error "Onbekend BOARD in config.h (kies BOARD_OLIMEX_POE2, BOARD_OLIMEX_POE_ISO of BOARD_WT32_ETH01)"
#endif

// Board-defaults toepassen, tenzij config.h ze al heeft gezet.
#ifndef OA_PHY_ADDR
  #define OA_PHY_ADDR OA_DEF_PHY_ADDR
#endif
#ifndef OA_PHY_POWER
  #define OA_PHY_POWER OA_DEF_PHY_POWER
#endif
#ifndef OA_PHY_MDC
  #define OA_PHY_MDC OA_DEF_PHY_MDC
#endif
#ifndef OA_PHY_MDIO
  #define OA_PHY_MDIO OA_DEF_PHY_MDIO
#endif
#ifndef OA_CLK_MODE
  #define OA_CLK_MODE OA_DEF_CLK_MODE
#endif
#ifndef BOARD_LAMP_PIN
  #define BOARD_LAMP_PIN OA_DEF_LAMP_PIN
#endif

// De PHY is een LAN8710/LAN8720 op alle ondersteunde borden (register-
// compatibel; de LAN8720-driver werkt voor beide).
#ifndef OA_PHY_TYPE
  #define OA_PHY_TYPE ETH_PHY_LAN8720
#endif

// Standaard NTP-servers, zodat een oudere config.h zonder deze instellingen
// gewoon blijft werken.
#ifndef NTP_SERVER_1
  #define NTP_SERVER_1 "pool.ntp.org"
#endif
#ifndef NTP_SERVER_2
  #define NTP_SERVER_2 "time.cloudflare.com"
#endif

// Effectieve lamp-GPIO: gebruik LAMP_PIN uit config.h als die >= 0 is,
// anders de standaard-GPIO van het gekozen bord.
#if LAMP_PIN >= 0
  #define ACTIVE_LAMP_PIN LAMP_PIN
#else
  #define ACTIVE_LAMP_PIN BOARD_LAMP_PIN
#endif

// ===========================================================================
// Globale toestand
// ===========================================================================
WebSocketsClient ws;

static bool lampAan = false;        // huidige werkelijke lampstatus
static bool ethVerbonden = false;   // heeft Ethernet een IP-adres?
static bool wsGestart = false;      // is de WebSocket-client al gestart?
static unsigned long laatsteHeartbeat = 0;

// ===========================================================================
// Helpers
// ===========================================================================

// Eenvoudige URL-encoder voor de query-waarden (id/naam/ruimte). Houdt
// alfanumeriek en -_.~ ongemoeid, codeert al het andere als %XX.
String urlEncode(const String &waarde) {
  String uit;
  const char *hex = "0123456789ABCDEF";
  for (size_t i = 0; i < waarde.length(); i++) {
    char c = waarde.charAt(i);
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
        (c >= 'a' && c <= 'z') || c == '-' || c == '_' || c == '.' || c == '~') {
      uit += c;
    } else if (c == ' ') {
      uit += "%20";
    } else {
      uit += '%';
      uit += hex[(c >> 4) & 0x0F];
      uit += hex[c & 0x0F];
    }
  }
  return uit;
}

#ifdef DNS_FALLBACK
// Sommige netwerken geven via DHCP geen (bruikbare) DNS-server mee. Met
// DNS_FALLBACK in config.h zetten we er zelf een achter.
//
// Let op de volgorde: de server uit DHCP blijft nummer 0, de fallback wordt
// nummer 1. lwIP loopt ze in volgorde af, dus het netwerk zelf houdt voorrang
// en we wijken alleen uit als dat niet antwoordt. Andersom -- de fallback op
// nummer 0 -- gooit de DNS van het netwerk weg, en op netwerken die uitgaand
// DNS-verkeer blokkeren (dezelfde die vaak ook NTP dichtzetten) blijft er dan
// niets over dat wel werkt.
#include <lwip/dns.h>
void zetDnsFallback() {
  ip_addr_t dnsserver;
  if (!ipaddr_aton(DNS_FALLBACK, &dnsserver)) {
    Serial.printf("[eth] ongeldige DNS_FALLBACK: %s\n", DNS_FALLBACK);
    return;
  }

  const ip_addr_t * uitDhcp = dns_getserver(0);
  bool dhcpBruikbaar = (uitDhcp != nullptr) && !ip_addr_isany(uitDhcp);

  dns_setserver(dhcpBruikbaar ? 1 : 0, &dnsserver);
  if (dhcpBruikbaar) {
    Serial.printf("[eth] DNS: %s (netwerk) met %s als reserve\n",
                  ipaddr_ntoa(uitDhcp), DNS_FALLBACK);
  } else {
    Serial.printf("[eth] DNS: netwerk levert er geen, dus %s gebruikt\n",
                  DNS_FALLBACK);
  }
}
#endif

// Stel de lamp-GPIO in op de gewenste stand (active-HIGH).
void setLamp(bool aan) {
  lampAan = aan;
  digitalWrite(ACTIVE_LAMP_PIN, aan ? HIGH : LOW);
  Serial.printf("[lamp] -> %s\n", aan ? "AAN (ON AIR)" : "UIT");
}

// Stuur de werkelijke status naar de server: {"type":"state","state":...}
void sendState() {
  JsonDocument doc;
  doc["type"] = "state";
  doc["state"] = lampAan;
  String payload;
  serializeJson(doc, payload);
  ws.sendTXT(payload);
  Serial.printf("[ws] status verstuurd: %s\n", payload.c_str());
}

// Stuur een heartbeat: {"type":"ping"}
void sendPing() {
  ws.sendTXT("{\"type\":\"ping\"}");
}

// ===========================================================================
// Inkomende WebSocket-berichten verwerken
// ===========================================================================
void verwerkBericht(const char *payload, size_t lengte) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, lengte);
  if (err) {
    Serial.printf("[ws] kon JSON niet parsen: %s\n", err.c_str());
    return;
  }

  const char *type = doc["type"] | "";

  if (strcmp(type, "command") == 0) {
    // Server vraagt om een aan/uit-stand; toepassen en bevestigen.
    bool gewenst = doc["state"] | false;
    Serial.printf("[ws] commando ontvangen: %s\n", gewenst ? "AAN" : "UIT");
    setLamp(gewenst);
    sendState();   // werkelijke status terugmelden
  } else if (strcmp(type, "pong") == 0) {
    // Antwoord op onze heartbeat -> niets te doen.
  } else {
    Serial.printf("[ws] onbekend berichttype genegeerd: '%s'\n", type);
  }
}

// ===========================================================================
// WebSocket-events
// ===========================================================================
void wsEvent(WStype_t type, uint8_t *payload, size_t lengte) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("[ws] verbonden met server: %s\n", (char *)payload);
      // Meld direct na (re)connect onze huidige status. De server stuurt zelf
      // vlak hierna een command met de laatst gewenste stand; die passen we toe.
      sendState();
      break;

    case WStype_TEXT:
      verwerkBericht((const char *)payload, lengte);
      break;

    case WStype_DISCONNECTED:
      Serial.println("[ws] verbinding verbroken, opnieuw proberen...");
      break;

    case WStype_ERROR:
      Serial.println("[ws] WebSocket-fout");
      break;

    default:
      break;
  }
}

// ===========================================================================
// Ethernet-events (volg connectiviteit en print het IP)
// ===========================================================================
void ethEvent(WiFiEvent_t event) {
  switch (event) {
    case ARDUINO_EVENT_ETH_START:
      Serial.println("[eth] gestart");
#ifdef LAMP_HOSTNAME
      ETH.setHostname(LAMP_HOSTNAME);
#endif
      break;

    case ARDUINO_EVENT_ETH_CONNECTED:
      Serial.println("[eth] kabel verbonden (link up)");
      break;

    case ARDUINO_EVENT_ETH_GOT_IP:
      ethVerbonden = true;
      Serial.print("[eth] IP-adres: ");
      Serial.println(ETH.localIP());
      Serial.print("[eth] gateway  : ");
      Serial.println(ETH.gatewayIP());
      Serial.print("[eth] DNS      : ");
      Serial.println(ETH.dnsIP());
      Serial.printf("[eth] snelheid: %d Mbps, %s\n",
                    ETH.linkSpeed(), ETH.fullDuplex() ? "full duplex" : "half duplex");
#ifdef DNS_FALLBACK
      zetDnsFallback();
#endif
      break;

    case ARDUINO_EVENT_ETH_DISCONNECTED:
      ethVerbonden = false;
      Serial.println("[eth] kabel losgekoppeld (link down)");
      break;

    case ARDUINO_EVENT_ETH_STOP:
      ethVerbonden = false;
      Serial.println("[eth] gestopt");
      break;

    default:
      break;
  }
}

// ===========================================================================
// Tijd ophalen via NTP
// ===========================================================================
// Een gewone webserver op een standaardpoort, als ijkpunt. SERVER_HOST kan naar
// een kale TCP-proxy wijzen, en daar draait geen webserver -- niet bruikbaar om
// de tijd op te halen of om te meten of het bord uberhaupt naar buiten mag.
#ifndef CONTROLE_HOST
  #define CONTROLE_HOST "reclamp.madera.video"
#endif
#ifndef CONTROLE_PORT
  #define CONTROLE_PORT 443
#endif

// Een TLS-certificaat heeft een geldigheidsperiode. Zonder kloppende klok denkt
// de ESP32 dat het 1970 is en keurt hij elk certificaat af. Daarom halen we de
// tijd op voordat we verbinden.
//
// Dit blok wordt altijd meegecompileerd, ook bij USE_TLS = false: de zelftest
// gebruikt het om te kunnen meten of TLS op dit netwerk werkt. Bij ws:// wordt
// het tijdens normaal gebruik simpelweg niet aangeroepen.
//
// Alles na deze datum beschouwen we als een echte klok (en niet 1970).
static const time_t TIJD_DREMPEL = 1700000000;

static bool tijdIsGeldig() {
  return time(nullptr) > TIJD_DREMPEL;
}

static void printTijd() {
  time_t nu = time(nullptr);
  struct tm t;
  gmtime_r(&nu, &t);
  Serial.printf("[tijd] %04d-%02d-%02d %02d:%02d:%02d UTC\n",
                t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                t.tm_hour, t.tm_min, t.tm_sec);
}

// Reservemethode: haal de tijd uit de "Date:"-header van een gewoon HTTP-
// antwoord. Handig op netwerken waar NTP (UDP-poort 123) geblokkeerd is;
// poort 80 staat vrijwel altijd open. We vragen het aan onze eigen server,
// dus er is geen extra dienst nodig.
static bool zetTijdUitDatumRegel(const String & datum) {
  // Voorbeeld: "Sun, 14 Sep 2026 19:25:00 GMT"
  struct tm t = {};
  if (strptime(datum.c_str(), "%a, %d %b %Y %H:%M:%S", &t) == nullptr) {
    Serial.printf("[tijd] kon datum niet lezen: %s\n", datum.c_str());
    return false;
  }
  time_t epoch = mktime(&t);            // TZ staat op UTC, dus dit klopt
  struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  return tijdIsGeldig();
}

static bool tijdViaHttpDate() {
  WiFiClient client;
  // LET OP: WiFiClient::setTimeout() rekent op de ESP32 in SECONDEN.
  client.setTimeout(5);

  // Bewust CONTROLE_HOST en niet SERVER_HOST: wijst SERVER_HOST naar een kale
  // TCP-proxy, dan draait daar geen webserver op poort 80 en is deze stap bij
  // voorbaat kansloos.
  Serial.printf("[tijd] verbinden met %s:80 ...\n", CONTROLE_HOST);
  if (!client.connect(CONTROLE_HOST, 80, 5000)) {   // connect-timeout wel in ms
    Serial.println("[tijd] HTTP-verbinding mislukt (poort 80 dicht?)");
    return false;
  }
  client.print(String("HEAD / HTTP/1.1\r\nHost: ") + CONTROLE_HOST +
               "\r\nConnection: close\r\n\r\n");

  // Zelf de tijd bewaken in plaats van op de stream-timeout vertrouwen.
  String regel;
  bool gelukt = false;
  unsigned long start = millis();
  while (millis() - start < 8000) {
    while (client.available()) {
      char c = (char)client.read();
      if (c == '\r') continue;
      if (c != '\n') {
        if (regel.length() < 160) regel += c;
        continue;
      }
      if (regel.length() == 0) {        // lege regel = einde van de headers
        client.stop();
        return gelukt;
      }
      if (regel.startsWith("Date:") || regel.startsWith("date:")) {
        String datum = regel.substring(5);
        datum.trim();
        gelukt = zetTijdUitDatumRegel(datum);
        client.stop();
        return gelukt;
      }
      regel = "";
    }
    if (!client.connected() && !client.available()) break;
    delay(10);
  }
  client.stop();
  Serial.println("[tijd] geen Date-header ontvangen");
  return gelukt;
}

static bool probeerNtp(const char * server1, const char * server2, uint32_t maxWachtMs) {
  configTime(0, 0, server1, server2);
  unsigned long start = millis();
  while (!tijdIsGeldig() && millis() - start < maxWachtMs) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  return tijdIsGeldig();
}

// Laatste redmiddel: de bouwtijd van deze firmware. Niet exact, maar ruim
// nauwkeurig genoeg om de geldigheidsperiode van een certificaat te toetsen.
static bool tijdUitBuild() {
  char stempel[40];
  snprintf(stempel, sizeof(stempel), "%s %s", __DATE__, __TIME__);
  struct tm t = {};
  if (strptime(stempel, "%b %d %Y %H:%M:%S", &t) == nullptr) return false;
  time_t epoch = mktime(&t);
  struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);
  return tijdIsGeldig();
}

void synchroniseerTijd() {
  // Werk in UTC, zodat mktime() hierboven geen tijdzone-correctie toepast.
  setenv("TZ", "UTC0", 1);
  tzset();

  // 1) NTP via de ingestelde servers (internet).
  Serial.print("[tijd] NTP ophalen");
  if (probeerNtp(NTP_SERVER_1, NTP_SERVER_2, 8000)) {
    printTijd();
    return;
  }

  // 2) NTP via de router zelf: dat verkeer blijft binnen het eigen netwerk en
  //    wordt dus niet door een internetfilter tegengehouden.
  static char gateway[20] = {0};
  ETH.gatewayIP().toString().toCharArray(gateway, sizeof(gateway));
  if (strlen(gateway) > 0 && strcmp(gateway, "0.0.0.0") != 0) {
    Serial.printf("[tijd] NTP via de router (%s)", gateway);
    if (probeerNtp(gateway, nullptr, 5000)) {
      printTijd();
      return;
    }
  }

  // 3) Tijd uit de Date-header van een gewoon HTTP-antwoord.
  Serial.println("[tijd] NTP mislukt -- nu via HTTP");
  if (tijdViaHttpDate()) {
    printTijd();
    return;
  }

  // 4) Niets werkte: val terug op de bouwdatum van deze firmware.
  Serial.println("[tijd] HTTP mislukt -- val terug op de bouwdatum van de firmware");
  if (tijdUitBuild()) {
    printTijd();
    Serial.println("[tijd] (bij benadering, maar genoeg om het certificaat te toetsen)");
    return;
  }

  Serial.println("[tijd] LET OP: geen tijd gevonden; certificaatvalidatie faalt.");
}

// ===========================================================================
// Netwerk-zelftest
// ===========================================================================
// Bij verbindingsproblemen meldt de WebSocket-library alleen "verbinding
// verbroken" en verzwijgt ze de oorzaak. Deze test meet de keten zelf door.
//
// Harde les uit het debuggen hiervan: een meting die twee dingen tegelijk
// varieert bewijst niets, en een instrument met een verborgen timeout meet
// zichzelf in plaats van het netwerk. Beide fouten hebben hier uren gekost,
// dus elke meting hieronder varieert precies een ding en zet zijn eigen
// timeout expliciet.

// --- losse metingen, elk met precies EEN uitkomst -------------------------
//
// De vorige versie van deze zelftest vergeleek in een keer een andere host EN
// een andere poort, en trok daar een conclusie uit over poorten. Dat kan niet.
// Elke meting hieronder varieert daarom precies een ding.

// Naam opzoeken, apart getimed -- anders telt de DNS-tijd stilzwijgend mee in
// de verbindingstijd en lijkt een snelle server traag.
static bool meetDns(const char * host, IPAddress & uit) {
  unsigned long t0 = millis();
  bool ok = WiFi.hostByName(host, uit);
  if (ok) {
    Serial.printf("[test] dns  %-38s -> %-15s %4lu ms\n",
                  host, uit.toString().c_str(), millis() - t0);
  } else {
    Serial.printf("[test] dns  %-38s -> MISLUKT       %4lu ms\n",
                  host, millis() - t0);
  }
  return ok;
}

// Kale TCP-verbinding naar een IP-adres. Altijd per IP, nooit per naam, zodat
// DNS hier geen rol meer speelt.
static bool meetTcp(const char * wat, IPAddress ip, uint16_t poort) {
  WiFiClient c;
  unsigned long t0 = millis();
  bool ok = c.connect(ip, poort, 6000);
  unsigned long duur = millis() - t0;
  c.stop();
  Serial.printf("[test] tcp  %-22s %-15s :%-5u %-12s %4lu ms\n",
                wat, ip.toString().c_str(), poort,
                ok ? "open" : "GEEN ANTWOORD", duur);
  return ok;
}

// TLS-handshake, met een EIGEN handshake-timeout. Zonder setHandshakeTimeout()
// staat die in deze core hard op 120 seconden; de timeout die je aan connect()
// meegeeft dekt alleen de TCP-verbinding. Daar zijn we twee avonden in gelopen.
static bool meetTls(const char * host, uint16_t poort, bool metControle) {
  WiFiClientSecure c;
  c.setHandshakeTimeout(10);
  if (metControle) {
    c.setCACert(ONAIR_ROOT_CAS);
  } else {
    c.setInsecure();
  }
  unsigned long t0 = millis();
  bool ok = c.connect(host, poort, 6000);
  unsigned long duur = millis() - t0;
  char fout[128] = { 0 };
  if (!ok) c.lastError(fout, sizeof(fout));
  c.stop();
  Serial.printf("[test] tls  %-22s %-38s %-12s %5lu ms %s\n",
                metControle ? "met certificaat" : "zonder certificaat",
                host, ok ? "OK" : "MISLUKT", duur, fout);
  return ok;
}

// --- de zelftest ----------------------------------------------------------
//
// Doel: in een oogopslag zien welke van de drie lagen faalt (naam, poort,
// versleuteling) en of dat aan de bestemming of aan het netwerk ligt. Daarom
// altijd meerdere bestemmingen naast elkaar: een meting zonder vergelijking
// zegt niets.
static void netwerkZelftest() {
  Serial.println("[test] ================ netwerk-zelftest ================");

  IPAddress doel, ctrl, dns1;
  bool heeftDoel = meetDns(SERVER_HOST, doel);
  bool heeftCtrl = meetDns(CONTROLE_HOST, ctrl);
  dns1.fromString("1.1.1.1");

  Serial.println("[test] --- kale TCP-verbindingen ---");
  if (heeftDoel) {
    meetTcp("doel", doel, SERVER_PORT);
    // Zelfde IP, andere poort: onderscheidt "deze poort dicht" van
    // "deze bestemming onbereikbaar".
    if (SERVER_PORT != 443) meetTcp("doel, poort 443", doel, 443);
    if (SERVER_PORT != 80)  meetTcp("doel, poort 80", doel, 80);
  }
  if (heeftCtrl) {
    // Andere bestemming, poort 443: onderscheidt "deze bestemming" van
    // "alle bestemmingen".
    meetTcp("controlehost", ctrl, 443);
  }
  // Een derde partij die niets met ons te maken heeft, op een standaardpoort.
  meetTcp("1.1.1.1", dns1, 443);
  // En een hoge poort naar diezelfde derde partij: als hoge poorten echt
  // geblokkeerd zijn, faalt deze terwijl 443 hierboven lukte.
  meetTcp("1.1.1.1, hoge poort", dns1, 8443);

  Serial.println("[test] --- TLS-handshakes (10 s per poging) ---");
  // Zonder certificaatcontrole eerst: die meet puur of de handshake zelf
  // doorkomt. Lukt dat wel en de variant met certificaat niet, dan is het een
  // certificaatprobleem. Lukt geen van beide, dan komt de handshake niet door.
  meetTls(SERVER_HOST, 443, false);
  meetTls(SERVER_HOST, 443, true);
  // Een bestemming buiten ons eigen domein, om te zien of het aan onze server
  // ligt of aan alle TLS-verkeer op dit netwerk.
  meetTls("one.one.one.one", 443, false);

  Serial.println("[test] ================ einde zelftest =================");
  Serial.println("[test] lees dit zo: faalt ALLES bij tls maar lukt tcp overal,");
  Serial.println("[test] dan onderschept iets op dit netwerk versleuteld verkeer.");
}

// ===========================================================================
// WebSocket-client starten, met terugvalserver
// ===========================================================================
// Een on-air lamp die alleen werkt als het internet het doet, is een on-air
// lamp die tijdens een uitzending kan uitvallen. Daarom kent de firmware twee
// bestemmingen: een primaire (meestal een server op het eigen LAN, die geen
// internet, DNS of certificaten nodig heeft) en een reserve (de cloud). Lukt de
// ene FAILOVER_MS lang niet, dan gaat hij naar de andere, en zo door.
//
// Is er maar een server geconfigureerd, dan blijft het gedrag precies zoals het
// was: eindeloos opnieuw proberen op die ene.

struct Bestemming {
  const char * host;
  uint16_t     poort;
  bool         tls;
};

static const Bestemming BESTEMMINGEN[] = {
  { SERVER_HOST, SERVER_PORT, USE_TLS },
#ifdef SERVER2_HOST
  { SERVER2_HOST, SERVER2_PORT, SERVER2_TLS },
#endif
};
static const uint8_t AANTAL_BESTEMMINGEN =
    sizeof(BESTEMMINGEN) / sizeof(BESTEMMINGEN[0]);

// Hoe lang we een bestemming de kans geven voordat we de andere proberen.
#ifndef FAILOVER_MS
  #define FAILOVER_MS 30000
#endif

static uint8_t       actieveBestemming = 0;
static unsigned long pogingGestart     = 0;

void startWebSocket() {
  const Bestemming & b = BESTEMMINGEN[actieveBestemming];

  // Bouw het pad inclusief URL-geencodeerde query-parameters.
  String pad = "/ws/device";
  pad += "?id=" + urlEncode(LAMP_ID);
  pad += "&naam=" + urlEncode(LAMP_NAAM);
  pad += "&ruimte=" + urlEncode(LAMP_RUIMTE);

  Serial.printf("[ws] verbinden met %s://%s:%u%s\n",
                b.tls ? "wss" : "ws", b.host, b.poort, pad.c_str());

  if (b.tls) {
    // wss:// - versleuteld EN het servercertificaat wordt gevalideerd tegen de
    // roots in certs.h. We gebruiken bewust beginSslWithCA() en niet beginSSL():
    // afhankelijk van de libraryversie zet beginSSL() de "insecure" modus niet,
    // waardoor de handshake faalt met "start_ssl_client: -1".
    ws.beginSslWithCA(b.host, b.poort, pad.c_str(), ONAIR_ROOT_CAS);
  } else {
    // ws:// - onversleuteld, simpel en robuust op een vertrouwd LAN.
    ws.begin(b.host, b.poort, pad.c_str());
  }

  ws.onEvent(wsEvent);
  ws.setReconnectInterval(3000);   // elke 3s opnieuw proberen bij verbreken
  pogingGestart = millis();
}

// Blijft de huidige bestemming te lang stil, stap dan over op de volgende.
static void bewaakVerbinding() {
  if (AANTAL_BESTEMMINGEN < 2) return;   // niets om naar uit te wijken
  if (ws.isConnected()) {
    pogingGestart = millis();            // verbonden: de klok loopt niet
    return;
  }
  if (millis() - pogingGestart < FAILOVER_MS) return;

  actieveBestemming = (actieveBestemming + 1) % AANTAL_BESTEMMINGEN;
  Serial.printf("[ws] %lu s geen verbinding -- nu bestemming %u van %u\n",
                (unsigned long)(FAILOVER_MS / 1000),
                actieveBestemming + 1, AANTAL_BESTEMMINGEN);
  ws.disconnect();
  startWebSocket();
}

// ===========================================================================
// setup / loop
// ===========================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("====================================");
  Serial.println(" On-Air Lamp firmware");
  Serial.printf("  bord    : %s\n", BOARD_NAAM);
  Serial.printf("  lamp-pin: GPIO %d (active-HIGH)\n", ACTIVE_LAMP_PIN);
  Serial.printf("  lamp-id : %s\n", LAMP_ID);
  Serial.println("====================================");

  // --- VEILIGE DEFAULT: lamp UIT voordat het netwerk start ---
  pinMode(ACTIVE_LAMP_PIN, OUTPUT);
  digitalWrite(ACTIVE_LAMP_PIN, LOW);
  lampAan = false;

  // --- Ethernet starten ---
  WiFi.onEvent(ethEvent);

#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // Arduino-ESP32 core 3.x signatuur
  ETH.begin(OA_PHY_TYPE, OA_PHY_ADDR, OA_PHY_MDC, OA_PHY_MDIO,
            OA_PHY_POWER, OA_CLK_MODE);
#else
  // Arduino-ESP32 core 2.x signatuur
  ETH.begin(OA_PHY_ADDR, OA_PHY_POWER, OA_PHY_MDC, OA_PHY_MDIO,
            OA_PHY_TYPE, OA_CLK_MODE);
#endif

#ifdef STATIC_IP
  // Vast IP-adres in plaats van DHCP.
  {
    IPAddress ip, gw, sn, dns;
    ip.fromString(STATIC_IP);
    gw.fromString(STATIC_GATEWAY);
    sn.fromString(STATIC_SUBNET);
    dns.fromString(STATIC_DNS);
    if (ETH.config(ip, gw, sn, dns)) {
      Serial.printf("[eth] vast IP ingesteld: %s (gw %s, dns %s)\n",
                    STATIC_IP, STATIC_GATEWAY, STATIC_DNS);
    } else {
      Serial.println("[eth] LET OP: vast IP instellen is mislukt");
    }
  }
#endif

  // De WebSocket-client starten we pas zodra Ethernet een IP heeft (zie loop).
  // Eerder starten heeft geen zin: het opzoeken van de servernaam mislukt dan
  // en je krijgt een stroom "DNS Failed"-meldingen.
  Serial.println("[eth] wachten op netwerk (DHCP)...");
}

void loop() {
#ifdef STATIC_IP
  // Bij een vast IP komt het GOT_IP-event niet altijd; zodra de link er is en
  // we een geldig adres hebben, beschouwen we het netwerk als klaar.
  if (!ethVerbonden && ETH.linkUp() && ETH.localIP() != IPAddress(0, 0, 0, 0)) {
    ethVerbonden = true;
    Serial.print("[eth] netwerk klaar (vast IP): ");
    Serial.println(ETH.localIP());
    Serial.print("[eth] DNS      : ");
    Serial.println(ETH.dnsIP());
#ifdef DNS_FALLBACK
    zetDnsFallback();
#endif
  }
#endif

  // Start de WebSocket-client zodra het netwerk klaar is (eenmalig).
  //
  // De zelftest draait hierNA, niet ervoor. Diagnose mag de functie nooit
  // ophouden: de metingen kosten bij elkaar tientallen seconden, en een lamp
  // die daarop staat te wachten is een lamp die uit staat terwijl de studio
  // denkt dat hij aan is.
  if (ethVerbonden && !wsGestart) {
    wsGestart = true;
    // Alleen tijd ophalen als minstens een van de bestemmingen TLS gebruikt.
    // Bij een lamp die op een lokale server draait is dat niet zo, en dan
    // hoeft hij ook niet op een NTP-timeout te wachten.
    bool tlsNodig = false;
    for (uint8_t i = 0; i < AANTAL_BESTEMMINGEN; i++) {
      if (BESTEMMINGEN[i].tls) tlsNodig = true;
    }
    if (tlsNodig) synchroniseerTijd();
    startWebSocket();
#ifdef NET_TEST
    netwerkZelftest();
#endif
  }

  ws.loop();
  if (wsGestart) bewaakVerbinding();

  // Periodieke heartbeat versturen.
  unsigned long nu = millis();
  if (nu - laatsteHeartbeat >= HEARTBEAT_MS) {
    laatsteHeartbeat = nu;
    if (ws.isConnected()) {
      sendPing();
    }
  }
}
