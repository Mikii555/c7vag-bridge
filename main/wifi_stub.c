#ifndef CONFIG_FUNKBRIDGE_WIFI_DEFAULT_MODE
#include <stdint.h>
#include <stddef.h>

// Stub for BLE-only build
void wifi_server_push_frame(uint16_t tx_id, uint16_t rx_id, const uint8_t *data, size_t len) {
    (void)tx_id;
    (void)rx_id;
    (void)data;
    (void)len;
}
#endif
