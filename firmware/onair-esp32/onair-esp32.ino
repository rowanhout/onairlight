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

// Bestaande config.h-bestanden (van vóór de server-side sleutelcheck op
// /ws/device) hebben ONAIR_API_KEY nog niet gedefinieerd — val dan terug op
// leeg, wat overeenkomt met een server zonder ONAIR_API_KEY (geen sleutel
// vereist).
#ifndef ONAIR_API_KEY
#define ONAIR_API_KEY ""
#endif

#include <ETH.h>
#include <WiFi.h>               // levert WiFi.onEvent + ARDUINO_EVENT_ETH_* events
#include <WebSocketsClient.h>   // links2004/arduinoWebSockets  (>= 2.4.0)
#include <ArduinoJson.h>        // bblanchon/ArduinoJson v7

// ===========================================================================
// Board-afhankelijke Ethernet (RMII / LAN8720) configuratie
// ===========================================================================
// We leiden alle PHY-instellingen af uit BOARD (zie config.h).
#if BOARD == BOARD_OLIMEX_POE_ISO
  #define ETH_PHY_ADDR    0
  #define ETH_PHY_POWER   12
  #define ETH_PHY_MDC     23
  #define ETH_PHY_MDIO    18
  #define ETH_CLK_MODE    ETH_CLOCK_GPIO17_OUT
  #define BOARD_LAMP_PIN  32
  #define BOARD_NAAM      "Olimex ESP32-POE-ISO"
#elif BOARD == BOARD_WT32_ETH01
  #define ETH_PHY_ADDR    1
  #define ETH_PHY_POWER   16
  #define ETH_PHY_MDC     23
  #define ETH_PHY_MDIO    18
  #define ETH_CLK_MODE    ETH_CLOCK_GPIO0_IN
  #define BOARD_LAMP_PIN  4
  #define BOARD_NAAM      "WT32-ETH01"
#else
  #error "Onbekend BOARD in config.h (kies BOARD_OLIMEX_POE_ISO of BOARD_WT32_ETH01)"
#endif

// De PHY is altijd een LAN8720 voor de ondersteunde borden.
#define ETH_PHY_TYPE ETH_PHY_LAN8720

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
      Serial.printf("[eth] snelheid: %d Mbps, %s\n",
                    ETH.linkSpeed(), ETH.fullDuplex() ? "full duplex" : "half duplex");
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
  // Vereist door de server zodra ONAIR_API_KEY daar is ingesteld, zodat niet
  // elke client op het netwerk een lamp-id kan registreren/kapen.
  String apiKey = ONAIR_API_KEY;
  if (apiKey.length() > 0) {
    pad += "&key=" + urlEncode(apiKey);
  }

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
  ETH.begin(ETH_PHY_TYPE, ETH_PHY_ADDR, ETH_PHY_MDC, ETH_PHY_MDIO,
            ETH_PHY_POWER, ETH_CLK_MODE);
#else
  // Arduino-ESP32 core 2.x signatuur
  ETH.begin(ETH_PHY_ADDR, ETH_PHY_POWER, ETH_PHY_MDC, ETH_PHY_MDIO,
            ETH_PHY_TYPE, ETH_CLK_MODE);
#endif

  // --- WebSocket-client starten ---
  startWebSocket();
}

void loop() {
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
