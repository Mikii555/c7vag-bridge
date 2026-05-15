#ifndef WIFI_SERVER_H
#define WIFI_SERVER_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* WiFi operating modes */
typedef enum {
    WIFI_MODE_DISABLED = 0,
    WIFI_MODE_AP       = 1,   /* Access Point + captive portal */
    WIFI_MODE_STATION  = 2,   /* Join existing network */
} funkbridge_wifi_mode_t;

/* Callback Typ für empfangene Daten */
typedef void (*wifi_frame_cb_t)(uint16_t tx_id, uint16_t rx_id, 
                                 const uint8_t *data, size_t len);

/* --- Core Functions --- */

// Startet das Subsystem (NVS muss vorher initialisiert sein!)
void wifi_server_start(void);

// Beendet Server und WiFi sauber (Wichtig für Sleep-Modus)
void wifi_server_stop(void);

// Liest den aktuellen Modus (AP/STA/OFF) aus dem NVS
funkbridge_wifi_mode_t wifi_get_mode(void);

// Setzt den Callback für eingehende WebSocket-Daten
void wifi_server_set_rx_callback(wifi_frame_cb_t cb);

// Pusht Daten vom ISO-TP/CAN-Bus in den WebSocket-Ringbuffer
void wifi_server_push_frame(uint16_t tx_id, uint16_t rx_id, 
                             const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* WIFI_SERVER_H */