#ifndef CONFIG_FUNKBRIDGE_WIFI_DEFAULT_MODE
#include <stdint.h>
#include <stddef.h>

void wifi_server_push_frame(uint16_t tx_id, uint16_t rx_id, const uint8_t *data, size_t len) {
    (void)tx_id; (void)rx_id; (void)data; (void)len;
}
void wifi_server_start(void) {}
void wifi_server_stop(void) {}
void wifi_server_set_rx_callback(void *cb) { (void)cb; }
#endif

#include "esp_wifi_types.h"
esp_err_t wifi_get_mode(wifi_mode_t *mode) {
    if(mode) *mode = WIFI_MODE_NULL;
    return 0;
}
