/**
 * wifi_server.c — FunkBridge WiFi subsystem (Corrected Version)
 */

#include "wifi_server.h"
#include "constants.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "mdns.h"
#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_log.h"
#include "esp_spiffs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/ringbuf.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "wifi_srv";

/* ── Configuration ────────────────────────────────────────────────────────── */
#define WIFI_AP_SSID            "FunkBridge"
#define WIFI_AP_PASS            "FunkBridge1"
#define WIFI_AP_IP              "192.168.4.1"
#define WIFI_AP_CHANNEL         6
#define WIFI_AP_MAX_CONN        4
#define WIFI_MDNS_HOSTNAME      "funkbridge"
#define WIFI_CONNECT_TIMEOUT_S  15
#define WS_RING_BUF_SIZE        (8 * 1024)
#define WS_MAX_FRAME_SIZE       (4096 + 8)

/* ── State ────────────────────────────────────────────────────────────────── */
static httpd_handle_t   s_server       = NULL;
static int              s_ws_fd        = -1;
static RingbufHandle_t  s_tx_ringbuf   = NULL;
static wifi_frame_cb_t  s_rx_cb        = NULL;
static bool              s_running      = false;

/* ── NVS helpers ──────────────────────────────────────────────────────────── */
#define NVS_NS   "funkbridge"
#define NVS_MODE "wifi_mode"
#define NVS_SSID "wifi_ssid"
#define NVS_PASS "wifi_pass"

funkbridge_wifi_mode_t wifi_get_mode(void) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err != ESP_OK) {
        return WIFI_MODE_AP;
    }
    uint8_t mode = WIFI_MODE_AP;
    nvs_get_u8(h, NVS_MODE, &mode);
    nvs_close(h);
    return (funkbridge_wifi_mode_t)mode;
}

static void nvs_save_mode(funkbridge_wifi_mode_t mode) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_MODE, (uint8_t)mode);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void nvs_get_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_str(h, NVS_SSID, ssid, &ssid_len);
        nvs_get_str(h, NVS_PASS, pass, &pass_len);
        nvs_close(h);
    }
}

static void nvs_save_credentials(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, NVS_SSID, ssid);
        nvs_set_str(h, NVS_PASS, pass);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ── WebSocket frame push ────────────────────────────────────────────────── */
void wifi_server_push_frame(uint16_t tx_id, uint16_t rx_id, const uint8_t *data, size_t len) {
    if (!s_running || s_ws_fd < 0 || s_tx_ringbuf == NULL) return;

    uint8_t buf[WS_MAX_FRAME_SIZE];
    if (len + 8 > sizeof(buf)) return;
    
    buf[0] = 0xF1; 
    buf[1] = 0x00;
    buf[2] = rx_id & 0xFF; buf[3] = rx_id >> 8;
    buf[4] = tx_id & 0xFF; buf[5] = tx_id >> 8;
    buf[6] = len & 0xFF;   buf[7] = len >> 8;
    memcpy(buf + 8, data, len);

    if (xRingbufferSend(s_tx_ringbuf, buf, len + 8, 0) != pdTRUE) {
        // Buffer full - oldest data is handled by ringbuffer type
    }
}

void wifi_server_set_rx_callback(wifi_frame_cb_t cb) {
    s_rx_cb = cb;
}

/* ── WebSocket handler ────────────────────────────────────────────────────── */
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        if (s_ws_fd >= 0) {
            return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Only one client");
        }
        s_ws_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WS Connected: %d", s_ws_fd);
        return ESP_OK;
    }

    httpd_ws_frame_t pkt = { .type = HTTPD_WS_TYPE_BINARY };
    uint8_t buf[WS_MAX_FRAME_SIZE];
    pkt.payload = buf;

    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, sizeof(buf));
    if (ret != ESP_OK) return ret;

    if (pkt.len >= 8 && buf[0] == 0xF1 && s_rx_cb) {
        uint16_t rx_id = buf[2] | (buf[3] << 8);
        uint16_t tx_id = buf[4] | (buf[5] << 8);
        uint16_t sz    = buf[6] | (buf[7] << 8);
        if (sz <= pkt.len - 8) s_rx_cb(tx_id, rx_id, buf + 8, sz);
    }
    return ESP_OK;
}

/* ── WebSocket send task ──────────────────────────────────────────────────── */
static void ws_send_task(void *arg) {
    while (s_running) {
        size_t item_size = 0;
        void *item = xRingbufferReceive(s_tx_ringbuf, &item_size, pdMS_TO_TICKS(100));
        if (item) {
            if (s_ws_fd >= 0 && s_server) {
                httpd_ws_frame_t pkt = {
                    .type    = HTTPD_WS_TYPE_BINARY,
                    .payload = (uint8_t *)item,
                    .len     = item_size,
                };
                httpd_ws_send_frame_async(s_server, s_ws_fd, &pkt);
            }
            vRingbufferReturnItem(s_tx_ringbuf, item);
        }
    }
    vTaskDelete(NULL);
}

/* ── Handlers ─────────────────────────────────────────────────────────────── */
static esp_err_t api_status(httpd_req_t *req) {
    char json[128];
    snprintf(json, sizeof(json), "{\"mode\":%d,\"version\":\"%s\"}", (int)wifi_get_mode(), FUNKBRIDGE_VERSION);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static esp_err_t spiffs_handler(httpd_req_t *req) {
    char path[128];
    const char *uri = req->uri;
    if (strcmp(uri, "/") == 0) uri = "/index.html";
    snprintf(path, sizeof(path), "/spiffs%s", uri);

    FILE *f = fopen(path, "r");
    if (!f) { httpd_resp_send_404(req); return ESP_OK; }

    if (strstr(path, ".html")) httpd_resp_set_type(req, "text/html");
    else if (strstr(path, ".js")) httpd_resp_set_type(req, "application/javascript");
    
    char chunk[512];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        httpd_resp_send_chunk(req, chunk, n);
    }
    httpd_resp_send_chunk(req, NULL, 0);
    fclose(f);
    return ESP_OK;
}

/* ── DNS Server ───────────────────────────────────────────────────────────── */
static void dns_captive_task(void *arg) {
    struct sockaddr_in addr = { .sin_family=AF_INET, .sin_port=htons(53), .sin_addr.s_addr=INADDR_ANY };
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    bind(sock, (struct sockaddr *)&addr, sizeof(addr));
    
    uint8_t buf[512];
    while (s_running) {
        struct sockaddr_in client;
        socklen_t clen = sizeof(client);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr *)&client, &clen);
        if (n < 12) continue;
        
        buf[2] |= 0x80; buf[3] = 0x80; // Response
        buf[7] = 1; // 1 Answer
        // Simplified DNS pointer response to 192.168.4.1
        int rlen = n;
        uint8_t ans[] = { 0xc0, 0x0c, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00, 0x00, 0x3c, 0x00, 0x04, 192, 168, 4, 1 };
        memcpy(buf + rlen, ans, sizeof(ans));
        sendto(sock, buf, rlen + sizeof(ans), 0, (struct sockaddr *)&client, clen);
    }
    close(sock);
    vTaskDelete(NULL);
}

/* ── WiFi Start ───────────────────────────────────────────────────────────── */
void wifi_server_start(void) {
    ESP_LOGI(TAG, "Init WiFi Subsystem...");
    s_running = true;

    // 1. Ringbuffer init
    s_tx_ringbuf = xRingbufferCreate(WS_RING_BUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (s_tx_ringbuf == NULL) {
        ESP_LOGE(TAG, "Failed to create Ringbuffer! Memory full?");
        return;
    }

    // 2. Netif & Events (only if not already done)
    esp_netif_init();
    
    // 3. SPIFFS
    esp_vfs_spiffs_conf_t conf = { .base_path="/spiffs", .max_files=5, .format_if_mount_failed=false };
    esp_vfs_spiffs_register(&conf);

    // 4. Tasks
    xTaskCreate(ws_send_task, "ws_send", 4096, NULL, 5, NULL);

    // 5. WiFi Mode Selection
    funkbridge_wifi_mode_t mode = wifi_get_mode();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    if (mode == WIFI_MODE_STATION) {
        char ssid[64]={0}, pass[64]={0};
        nvs_get_credentials(ssid, sizeof(ssid), pass, sizeof(pass));
        esp_netif_create_default_wifi_sta();
        wifi_config_t sta_cfg = {0};
        strncpy((char*)sta_cfg.sta.ssid, ssid, 32);
        strncpy((char*)sta_cfg.sta.password, pass, 64);
        esp_wifi_set_mode(WIFI_MODE_STA);
        esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    } else {
        esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();
        esp_netif_ip_info_t ip;
        ip.ip.addr = ESP_IP4TOADDR(192,168,4,1);
        ip.gw.addr = ESP_IP4TOADDR(192,168,4,1);
        ip.netmask.addr = ESP_IP4TOADDR(255,255,255,0);
        esp_netif_dhcps_stop(ap_netif);
        esp_netif_set_ip_info(ap_netif, &ip);
        esp_netif_dhcps_start(ap_netif);

        wifi_config_t ap_cfg = {
            .ap = { .ssid=WIFI_AP_SSID, .password=WIFI_AP_PASS, .channel=WIFI_AP_CHANNEL, 
                    .max_connection=4, .authmode=WIFI_AUTH_WPA2_PSK }
        };
        esp_wifi_set_mode(WIFI_MODE_AP);
        esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        xTaskCreate(dns_captive_task, "dns_cap", 3072, NULL, 3, NULL);
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    
    // HTTP Server
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;
    if (httpd_start(&s_server, &http_cfg) == ESP_OK) {
        httpd_uri_t ws_uri = { .uri="/ws", .method=HTTP_GET, .handler=ws_handler, .is_websocket=true };
        httpd_register_uri_handler(s_server, &ws_uri);
        httpd_uri_t stat_uri = { .uri="/api/status", .method=HTTP_GET, .handler=api_status };
        httpd_register_uri_handler(s_server, &stat_uri);
        httpd_uri_t fallback = { .uri="/*", .method=HTTP_GET, .handler=spiffs_handler };
        httpd_register_uri_handler(s_server, &fallback);
    }
}