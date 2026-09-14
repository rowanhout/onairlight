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

#include <ETH.h>
#include <WiFi.h>               // levert WiFi.onEvent + ARDUINO_EVENT_ETH_* events
#include <WebSocketsClient.h>   // links2004/arduinoWebSockets  (>= 2.4.0)
#include <ArduinoJson.h>        // bblanchon/ArduinoJson v7

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
// DNS_FALLBACK in config.h forceren we er zelf een, zodat het opzoeken van de
// servernaam toch lukt.
#include <lwip/dns.h>
void zetDnsFallback() {
  ip_addr_t dnsserver;
  if (ipaddr_aton(DNS_FALLBACK, &dnsserver)) {
    dns_setserver(0, &dnsserver);
    Serial.printf("[eth] DNS-fallback ingesteld op %s\n", DNS_FALLBACK);
  } else {
    Serial.printf("[eth] ongeldige DNS_FALLBACK: %s\n", DNS_FALLBACK);
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
// WebSocket-client starten
// ===========================================================================
void startWebSocket() {
  // Bouw het pad inclusief URL-geencodeerde query-parameters.
  String pad = "/ws/device";
  pad += "?id=" + urlEncode(LAMP_ID);
  pad += "&naam=" + urlEncode(LAMP_NAAM);
  pad += "&ruimte=" + urlEncode(LAMP_RUIMTE);

  Serial.printf("[ws] verbinden met %s://%s:%d%s\n",
                USE_TLS ? "wss" : "ws", SERVER_HOST, SERVER_PORT, pad.c_str());

#if USE_TLS
  // wss:// - versleuteld, maar certificaat wordt NIET gevalideerd (insecure).
  // Vereist arduinoWebSockets >= 2.4.0.
  ws.beginSSL(SERVER_HOST, SERVER_PORT, pad.c_str());
#else
  // ws:// - onversleuteld, simpel en robuust op een vertrouwd LAN.
  ws.begin(SERVER_HOST, SERVER_PORT, pad.c_str());
#endif

  ws.onEvent(wsEvent);
  ws.setReconnectInterval(3000);   // elke 3s opnieuw proberen bij verbreken
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

  // De WebSocket-client starten we pas zodra Ethernet een IP heeft (zie loop).
  // Eerder starten heeft geen zin: het opzoeken van de servernaam mislukt dan
  // en je krijgt een stroom "DNS Failed"-meldingen.
  Serial.println("[eth] wachten op netwerk (DHCP)...");
}

void loop() {
  // Start de WebSocket-client zodra het netwerk klaar is (eenmalig).
  if (ethVerbonden && !wsGestart) {
    wsGestart = true;
    startWebSocket();
  }

  ws.loop();

  // Periodieke heartbeat versturen.
  unsigned long nu = millis();
  if (nu - laatsteHeartbeat >= HEARTBEAT_MS) {
    laatsteHeartbeat = nu;
    if (ws.isConnected()) {
      sendPing();
    }
  }
}
