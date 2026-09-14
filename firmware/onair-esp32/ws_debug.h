// ============================================================================
// ws_debug.h  --  logging van arduinoWebSockets aanzetten
// ============================================================================
//
// arduinoWebSockets logt via de macro DEBUG_WEBSOCKETS(...). Die moet al
// gedefinieerd zijn wanneer de library zelf gecompileerd wordt -- en de
// library is een eigen vertaaleenheid, dus een #define in onair-esp32.ino
// bereikt haar niet. Daarom dwingt de debug-omgeving in platformio.ini dit
// bestand met -include aan het begin van elk bronbestand af.
//
// Gebruik:
//     pio run -e poe2-debug -t upload && pio device monitor
//
// De normale omgeving (poe2) blijft stil.
// ----------------------------------------------------------------------------

#pragma once

// Alleen in C++: sommige libraries bevatten ook .c-bestanden, en Arduino.h is
// C++. Zonder deze bewaking breekt de build op precies die bestanden.
#ifdef __cplusplus
  #include <Arduino.h>
  #ifndef DEBUG_WEBSOCKETS
    #define DEBUG_WEBSOCKETS(...) Serial.printf(__VA_ARGS__)
  #endif
#endif
