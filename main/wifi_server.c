#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "driver/twai.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"

/* Project Includes */
#include "isotp.h"
#include "ble_server.h"
#include "isotp_link_containers.h"
#include "persist.h"
#include "constants.h"
#include "led.h"
#include "eeprom.h"
#include "uart.h"
#include "connection_handler.h"
#include "isotp_bridge.h"
#include "wifi_server.h"
#include "mcp2515.h"

static const char* MAIN_TAG = "Main";
SemaphoreHandle_t sync_task_sem;

void app_main(void)
{
    ESP_LOGI(MAIN_TAG, "Application starting on ESP32 (WROOM)...");

    // 1. NVS Initialisierung für WiFi/BLE Credentials
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Watchdog Timer (Klassische ESP32 IDF 4.x/5.x Kompatibilität)
#if CONFIG_ESP_TASK_WDT_EN
    #if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
        esp_task_wdt_config_t twdt_config = {
            .timeout_ms = WDT_TIMEOUT_S * 1000,
            .idle_core_mask = (1 << portNUM_PROCESSORS) - 1,
            .trigger_panic = true,
        };
        esp_task_wdt_reconfigure(&twdt_config);
    #else
        esp_task_wdt_init(WDT_TIMEOUT_S, true);
    #endif
#endif
    esp_task_wdt_add(NULL);

    // 3. System Initialisierung
    sync_task_sem = xSemaphoreCreateBinary();
    eeprom_init();
    ble_server_init();
    ch_init();
    led_init();
    uart_init();     // Hier sicherstellen, dass in uart.h GPIO 16/17 steht!
    twai_init();
    mcp2515_init();
    isotp_init();
    persist_init();

    // BLE Name laden
    char* gapName = eeprom_read_str(BLE_GAP_KEY);
    if (gapName) {
        ble_set_gap_name(gapName, false);
        free(gapName);
    }

#if SLEEP_MODE == 1
    while(1) {
#endif
        /* Transport-Wahl basierend auf deiner wifi_server.c */
        funkbridge_wifi_mode_t wifi_mode = wifi_get_mode();
        bool use_wifi = (wifi_mode != WIFI_MODE_DISABLED);

        if (use_wifi) {
            wifi_server_set_rx_callback(bridge_received_wifi);
            wifi_server_start();
            ESP_LOGI(MAIN_TAG, "WiFi Mode: %d started", (int)wifi_mode);
        } else {
            ble_server_callbacks callbacks = {
                .data_received             = bridge_received_ble,
                .notifications_subscribed   = bridge_connect,
                .notifications_unsubscribed = bridge_disconnect
            };
            ble_server_start(callbacks);
            ESP_LOGI(MAIN_TAG, "BLE Mode started");
        }

        /* Start der CAN/Bridge Tasks */
        twai_start_task();
        mcp2515_start_task();
        isotp_start_task();
        persist_start_task();
        uart_start_task();
        ch_start_task();

        /* Warten auf Sleep-Befehl */
        while (ch_take_sleep_sem() != pdTRUE) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(100));
        }

        /* Stoppen der Dienste */
        if (use_wifi) {
            // wifi_server_stop(); // Falls implementiert
        } else {
            ble_stop_advertising();
            while (ble_connected()) {
                vTaskDelay(pdMS_TO_TICKS(TIMEOUT_NORMAL));
                esp_task_wdt_reset();
            }
            ble_server_stop();
        }

        ch_stop_task();
        uart_stop_task();
        persist_stop_task();
        isotp_stop_task();
        mcp2515_stop_task();
        twai_stop_task();

        ESP_LOGI(MAIN_TAG, "Entering Sleep...");
        esp_sleep_enable_timer_wakeup(SLEEP_TIME * 1000000ULL);

#if SLEEP_MODE == 1
        esp_task_wdt_reset();
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_light_sleep_start();
        esp_task_wdt_reset();
    }
#endif

    esp_deep_sleep_start();
}