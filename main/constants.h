#ifndef CONSTANTS_H
#define CONSTANTS_H

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

/* Globales Semaphor zum Synchronisieren von Task-Start/Stop */
extern SemaphoreHandle_t sync_task_sem;

/* ── Typen & Makros ───────────────────────────────────────────────────────── */
typedef int16_t                         bool16;
#define tMUTEX(x)                       xSemaphoreTake(x, portMAX_DELAY)
#define rMUTEX(x)                       xSemaphoreGive(x)

/* ── Hardware: AITRIP ESP32-DevKitC-32 (ESP-WROOM-32, CH340C, 30-pin) ────────
 *
 *  GPIO0  = Strapping (Boot-Modus) — nicht verwenden
 *  GPIO2  = WS2812 LED Data        — muss LOW beim Boot sein ✓
 *  GPIO4  = TWAI RX (TJA1051T)
 *  GPIO5  = TWAI TX (TJA1051T)     — Strapping, idles HIGH = sicher ✓
 *  GPIO6–11 = Flash-Speicher        — NIEMALS verwenden!
 *  GPIO13 = LED Enable (A0-Board)
 *  GPIO17 = MCP2515 CS
 *  GPIO18 = MCP2515 SCK
 *  GPIO19 = MCP2515 MISO
 *  GPIO21 = TJA1051T STB (SILENT)  — LOW = aktiv ✓
 *  GPIO23 = MCP2515 MOSI
 *  GPIO25 = UART1 TX               ← NEU: externe UART-Kommunikation
 *  GPIO26 = UART1 RX               ← NEU: externe UART-Kommunikation
 *  GPIO34 = MCP2515 INT            — Input-only, kein internen Pull-up!
 *                                     10kΩ nach 3.3V HARDWARE-PFLICHT
 * ─────────────────────────────────────────────────────────────────────────── */

/* ── MCP2515 (SPI, 100 kbps, OBD Pins 3/11) ──────────────────────────────── */
#define MCP2515_SCK_PIN                 18
#define MCP2515_MOSI_PIN                23
#define MCP2515_MISO_PIN                19
#define MCP2515_CS_PIN                  17
#define MCP2515_INT_PIN                 34  /* Input-only — 10kΩ zu 3.3V nötig! */
#define MCP2515_CLOCK_HZ                4000000
#define BLE_COMMAND_FLAG_CONV_CAN       0x10

/* ── Bridge-Einstellungen ─────────────────────────────────────────────────── */
#define BRG_SETTING_ISOTP_STMIN         1
#define BRG_SETTING_LED_COLOR           2
#define BRG_SETTING_PERSIST_DELAY       3
#define BRG_SETTING_PERSIST_Q_DELAY     4
#define BRG_SETTING_BLE_SEND_DELAY      5
#define BRG_SETTING_BLE_MULTI_DELAY     6
#define BRG_SETTING_PASSWORD            7
#define BRG_SETTING_GAP                 8
#define BRG_SETTING_RAW_SNIFF           9
#define BLE_RAW_SNIFF_ID                0xCAFE

/* ── Task-Konfiguration ───────────────────────────────────────────────────── */
#define TASK_STACK_SIZE                 3072

#define TWAI_TASK_PRIO                  3
#define ISOTP_TSK_PRIO                  2
#define MAIN_TSK_PRIO                   1
#define PERSIST_TSK_PRIO                0
#define HANDLER_TSK_PRIO                0
#define UART_TSK_PRIO                   1

/* ── GPIO-Belegung ────────────────────────────────────────────────────────── */
#define SILENT_GPIO_NUM                 21
#define LED_ENABLE_GPIO_NUM             13
#define LED_GPIO_NUM                    2
#define GPIO_OUTPUT_PIN_SEL(X)          ((1ULL << (X)))

/* ── Puffer-Größen ────────────────────────────────────────────────────────── */
#define ISOTP_QUEUE_SIZE                64
#define UART_QUEUE_SIZE                 96
#define ISOTP_BUFFER_SIZE               4096
#define ISOTP_BUFFER_SIZE_SMALL         512

/* ── Timeouts (ms) ────────────────────────────────────────────────────────── */
#define TIMEOUT_SHORT                   50
#define TIMEOUT_NORMAL                  100
#define TIMEOUT_LONG                    1000
#define TIMEOUT_CANCONNECTION           2
#define TIMEOUT_FIRSTBOOT               30
#define TIMEOUT_UARTCONNECTION          120
#define TIMEOUT_UARTPACKET              1

/* ── Sleep & Watchdog ─────────────────────────────────────────────────────── */
#define SLEEP_MODE                      0       /* 0 = Deep Sleep, 1 = Light Sleep */
#define SLEEP_TIME                      5
#define WDT_TIMEOUT_S                   5
#define US_TO_S                         1000000

/* ── Passwort / BLE ───────────────────────────────────────────────────────── */
/* #define PASSWORD_CHECK */   /* Deaktiviert — wir kontrollieren beide Seiten */
#define FUNKBRIDGE_VERSION              "1.0.0"
#define MAX_PASSWORD_LENGTH             64
#define PASSWORD_KEY                    "Password"
#define PASSWORD_DEFAULT                "BLE2"
#define BLE_GAP_KEY                     "GAP"

/* ── TWAI / CAN (500 kbps, TJA1051T) ─────────────────────────────────────── */
#define CAN_INTERNAL_BUFFER_SIZE        1024
#define CAN_TX_PORT                     5
#define CAN_RX_PORT                     4
#define CAN_CLK_IO                      TWAI_IO_UNUSED
#define CAN_BUS_IO                      TWAI_IO_UNUSED
#define CAN_MODE                        TWAI_MODE_NORMAL
#define CAN_ALERTS                      (TWAI_ALERT_ABOVE_ERR_WARN  | \
                                         TWAI_ALERT_ERR_PASS        | \
                                         TWAI_ALERT_BUS_OFF         | \
                                         TWAI_ALERT_BUS_RECOVERED)
#define CAN_FLAGS                       ESP_INTR_FLAG_LEVEL1
#define CAN_CLK_DIVIDER                 0
#define CAN_TIMING                      TWAI_TIMING_CONFIG_500KBITS()
#define CAN_FILTER                      TWAI_FILTER_CONFIG_ACCEPT_ALL()

/* ── UART1 (externe Kommunikation, 250 kbps) ──────────────────────────────
 *
 *  FIX: War UART_NUM_0 → Crash durch uart_driver_install() auf dem
 *       Console-UART (CH340C). Jetzt UART_NUM_1 auf freien GPIOs 25/26.
 *
 *  Pinbelegung:
 *    GPIO25 = TX  (zum externen Gerät)
 *    GPIO26 = RX  (vom externen Gerät)
 *
 *  Serial Monitor bleibt auf UART0 / 115200 Baud für Debug-Output.
 * ─────────────────────────────────────────────────────────────────────────── */
#define UART_PORT_NUM                   UART_NUM_1      /* FIX: war UART_NUM_0 */
#define UART_TXD                        GPIO_NUM_25     /* FIX: war UART_PIN_NO_CHANGE */
#define UART_RXD                        GPIO_NUM_26     /* FIX: war UART_PIN_NO_CHANGE */
#define UART_RTS                        UART_PIN_NO_CHANGE
#define UART_CTS                        UART_PIN_NO_CHANGE
#define UART_BAUD_RATE                  250000
#define UART_BUFFER_SIZE                8192
#define UART_INTERNAL_BUFFER_SIZE       2048
/* #define UART_ECHO */

/* ── Persist ──────────────────────────────────────────────────────────────── */
#define PERSIST_COUNT                   2
#define PERSIST_MAX_MESSAGE             64
#define PERSIST_DEFAULT_MESSAGE_DELAY   20
#define PERSIST_DEFAULT_QUEUE_DELAY     10

#endif /* CONSTANTS_H */
