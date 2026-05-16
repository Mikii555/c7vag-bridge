/**
 * wifi_server.c — FunkBridge WiFi subsystem  (korrigierte Version)
 *
 * Bugfixes gegenüber der alten Version:
 *   1. AP ist jetzt wirklich OFFEN (kein Passwort, WIFI_AUTH_OPEN)
 *   2. s_ws_fd wird bei Disconnect korrekt auf -1 zurückgesetzt
 *      (über httpd close-Callback + Fehlerbehandlung im Send-Task)
 *   3. JSON in api_status verwendet korrekte \" Escape-Sequenzen
 *   4. DNS-Socket hat SO_RCVTIMEO → sauberer Shutdown möglich
 *   5. wifi_server_stop() löscht den Ring-Buffer und räumt alles auf
 *   6. nvs_flash_init() wird in main.c erwartet (nicht hier doppelt)
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

/* ── Konfiguration ────────────────────────────────────────────────────────── */
#define WIFI_AP_SSID            "FunkBridge"
#define WIFI_AP_CHANNEL         6
#define WIFI_AP_MAX_CONN        4
#define WIFI_MDNS_HOSTNAME      "funkbridge"
#define WIFI_CONNECT_TIMEOUT_S  15
#define WS_RING_BUF_SIZE        (8 * 1024)
#define WS_MAX_FRAME_SIZE       (4096 + 8)

/* ── Zustand ──────────────────────────────────────────────────────────────── */
static httpd_handle_t   s_server     = NULL;
static int              s_ws_fd      = -1;
static RingbufHandle_t  s_tx_ringbuf = NULL;
static wifi_frame_cb_t  s_rx_cb      = NULL;
static volatile bool    s_running    = false;

/* ── NVS Helfer ───────────────────────────────────────────────────────────── */
#define NVS_NS   "funkbridge"
#define NVS_MODE "wifi_mode"
#define NVS_SSID "wifi_ssid"
#define NVS_PASS "wifi_pass"

funkbridge_wifi_mode_t wifi_get_mode(void) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return WIFI_MODE_AP;
    uint8_t mode = WIFI_MODE_AP;
    nvs_get_u8(h, NVS_MODE, &mode);
    nvs_close(h);
    return (funkbridge_wifi_mode_t)mode;
}

static void nvs_save_mode(funkbridge_wifi_mode_t mode) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, NVS_MODE, (uint8_t)mode);
    nvs_commit(h);
    nvs_close(h);
}

static void nvs_get_credentials(char *ssid, size_t ssid_len,
                                  char *pass, size_t pass_len) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
    nvs_get_str(h, NVS_SSID, ssid, &ssid_len);
    nvs_get_str(h, NVS_PASS, pass, &pass_len);
    nvs_close(h);
}

static void nvs_save_credentials(const char *ssid, const char *pass) {
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_SSID, ssid);
    nvs_set_str(h, NVS_PASS, pass);
    nvs_commit(h);
    nvs_close(h);
}

/* ── WebSocket Frame Push (aus ISO-TP Task) ──────────────────────────────── */
void wifi_server_push_frame(uint16_t tx_id, uint16_t rx_id,
                              const uint8_t *data, size_t len) {
    if (!s_running || s_ws_fd < 0 || !s_tx_ringbuf) return;
    if (len + 8 > WS_MAX_FRAME_SIZE) return;

    uint8_t buf[WS_MAX_FRAME_SIZE];
    buf[0] = 0xF1;
    buf[1] = 0x00;
    buf[2] = rx_id & 0xFF; buf[3] = rx_id >> 8;
    buf[4] = tx_id & 0xFF; buf[5] = tx_id >> 8;
    buf[6] = len   & 0xFF; buf[7] = len   >> 8;
    memcpy(buf + 8, data, len);

    xRingbufferSend(s_tx_ringbuf, buf, len + 8, 0);
}

void wifi_server_set_rx_callback(wifi_frame_cb_t cb) {
    s_rx_cb = cb;
}

/* ── httpd Socket-Close Callback (FIX: s_ws_fd zurücksetzen) ─────────────── */
static void ws_close_cb(httpd_handle_t hd, int sockfd) {
    if (sockfd == s_ws_fd) {
        ESP_LOGI(TAG, "WebSocket client disconnected (fd=%d)", sockfd);
        s_ws_fd = -1;
    }
}

/* ── WebSocket Handler ────────────────────────────────────────────────────── */
static esp_err_t ws_handler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        if (s_ws_fd >= 0) {
            ESP_LOGW(TAG, "WS: zweiter Client abgelehnt (fd=%d aktiv)", s_ws_fd);
            httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Only one WS client");
            return ESP_FAIL;
        }
        s_ws_fd = httpd_req_to_sockfd(req);
        ESP_LOGI(TAG, "WebSocket verbunden (fd=%d)", s_ws_fd);
        return ESP_OK;
    }

    httpd_ws_frame_t pkt = { .type = HTTPD_WS_TYPE_BINARY };
    uint8_t buf[WS_MAX_FRAME_SIZE];
    pkt.payload = buf;

    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, sizeof(buf));
    if (ret != ESP_OK) {
        /* FIX: bei Empfangsfehler fd freigeben */
        ESP_LOGW(TAG, "WS recv fehler (fd=%d), trenne", s_ws_fd);
        s_ws_fd = -1;
        return ret;
    }

    if (pkt.len >= 8 && buf[0] == 0xF1 && s_rx_cb) {
        uint16_t rx_id = buf[2] | (buf[3] << 8);
        uint16_t tx_id = buf[4] | (buf[5] << 8);
        uint16_t sz    = buf[6] | (buf[7] << 8);
        if (sz <= pkt.len - 8)
            s_rx_cb(tx_id, rx_id, buf + 8, sz);
    }
    return ESP_OK;
}

/* ── WebSocket Send Task ──────────────────────────────────────────────────── */
static void ws_send_task(void *arg) {
    while (s_running) {
        size_t item_size = 0;
        void *item = xRingbufferReceive(s_tx_ringbuf, &item_size,
                                        pdMS_TO_TICKS(50));
        if (item) {
            if (s_ws_fd >= 0 && s_server) {
                httpd_ws_frame_t pkt = {
                    .type    = HTTPD_WS_TYPE_BINARY,
                    .payload = (uint8_t *)item,
                    .len     = item_size,
                };
                esp_err_t ret = httpd_ws_send_frame_async(s_server,
                                                           s_ws_fd, &pkt);
                if (ret != ESP_OK) {
                    /* FIX: Send-Fehler = Client weg */
                    ESP_LOGW(TAG, "WS send fehler → fd zurückgesetzt");
                    s_ws_fd = -1;
                }
            }
            vRingbufferReturnItem(s_tx_ringbuf, item);
        }
    }
    vTaskDelete(NULL);
}

/* ── Captive Portal / API Handler ────────────────────────────────────────── */
static esp_err_t captive_redirect(httpd_req_t *req) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t android_204(httpd_req_t *req) {
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

/* POST /api/wifi — Zugangsdaten speichern und in STA-Mode wechseln */
static esp_err_t api_wifi_post(httpd_req_t *req) {
    char buf[256] = {0};
    int received = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (received <= 0) return ESP_FAIL;

    char ssid[64] = {0}, pass[64] = {0};
    char *p = strstr(buf, "ssid=");
    if (p) {
        p += 5;
        char *e = strchr(p, '&');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l < 64) { memcpy(ssid, p, l); ssid[l] = 0; }
    }
    p = strstr(buf, "password=");
    if (p) {
        p += 9;
        char *e = strchr(p, '&');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        if (l < 64) { memcpy(pass, p, l); pass[l] = 0; }
    }

    nvs_save_credentials(ssid, pass);
    nvs_save_mode(WIFI_MODE_STATION);
    httpd_resp_sendstr(req, "OK - rebooting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

/* GET /api/status — JSON Status  (FIX: korrekte \" Escapes) */
static esp_err_t api_status(httpd_req_t *req) {
    char json[256];
    snprintf(json, sizeof(json),
             "{\"mode\":%d,\"hostname\":\"funkbridge\",\"version\":\"%s\"}",
             (int)wifi_get_mode(), FUNKBRIDGE_VERSION);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    return ESP_OK;
}

/* SPIFFS Datei-Handler */
static esp_err_t spiffs_handler(httpd_req_t *req) {
    char path[64];
    const char *uri = req->uri;
    if (strcmp(uri, "/") == 0) uri = "/index.html";
    snprintf(path, sizeof(path), "/spiffs%s", uri);

    FILE *f = fopen(path, "r");
    if (!f) {
        ESP_LOGW(TAG, "Datei nicht gefunden: %s", path);
        httpd_resp_send_404(req);
        return ESP_OK;
    }

    if (strstr(path, ".html"))     httpd_resp_set_type(req, "text/html");
    else if (strstr(path, ".js"))  httpd_resp_set_type(req, "application/javascript");
    else if (strstr(path, ".css")) httpd_resp_set_type(req, "text/css");
    else if (strstr(path, ".ico")) httpd_resp_set_type(req, "image/x-icon");

    char chunk[512];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0)
        httpd_resp_send_chunk(req, chunk, n);
    httpd_resp_send_chunk(req, NULL, 0);
    fclose(f);
    return ESP_OK;
}

/* ── HTTP Server starten ──────────────────────────────────────────────────── */
static void start_http_server(bool ap_mode) {
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.uri_match_fn   = httpd_uri_match_wildcard;
    cfg.max_open_sockets = 5;
    /* FIX: Socket-Close Callback registrieren → s_ws_fd wird zurückgesetzt */
    cfg.close_fn       = ws_close_cb;

    if (httpd_start(&s_server, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "HTTP Server start fehlgeschlagen");
        return;
    }

    /* WebSocket */
    httpd_uri_t ws = {
        .uri = "/ws", .method = HTTP_GET,
        .handler = ws_handler, .is_websocket = true
    };
    httpd_register_uri_handler(s_server, &ws);

    /* API */
    httpd_uri_t st = { .uri = "/api/status", .method = HTTP_GET,  .handler = api_status };
    httpd_uri_t wp = { .uri = "/api/wifi",   .method = HTTP_POST, .handler = api_wifi_post };
    httpd_register_uri_handler(s_server, &st);
    httpd_register_uri_handler(s_server, &wp);

    if (ap_mode) {
        httpd_uri_t a204    = { .uri = "/generate_204",        .method = HTTP_GET, .handler = android_204 };
        httpd_uri_t hotspot = { .uri = "/hotspot-detect.html", .method = HTTP_GET, .handler = captive_redirect };
        httpd_uri_t ncsi    = { .uri = "/ncsi.txt",            .method = HTTP_GET, .handler = captive_redirect };
        httpd_register_uri_handler(s_server, &a204);
        httpd_register_uri_handler(s_server, &hotspot);
        httpd_register_uri_handler(s_server, &ncsi);
    }

    /* SPIFFS Wildcard — muss als letztes registriert werden */
    httpd_uri_t files = { .uri = "/*", .method = HTTP_GET, .handler = spiffs_handler };
    httpd_register_uri_handler(s_server, &files);

    ESP_LOGI(TAG, "HTTP Server gestartet (ap_mode=%d)", ap_mode);
}

/* ── mDNS ─────────────────────────────────────────────────────────────────── */
static void start_mdns(void) {
    mdns_init();
    mdns_hostname_set(WIFI_MDNS_HOSTNAME);
    mdns_instance_name_set("FunkBridge ISO-TP Bridge");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    ESP_LOGI(TAG, "mDNS: funkbridge.local");
}

/* ── DNS Captive Portal Task (FIX: Socket-Timeout für sauberen Shutdown) ─── */
static void dns_captive_task(void *arg) {
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket() fehlgeschlagen");
        vTaskDelete(NULL);
        return;
    }

    /* FIX: Timeout setzen → Task kann sauber beendet werden */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind() fehlgeschlagen");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS Captive Portal Task gestartet");

    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);

    while (s_running) {
        int n = recvfrom(sock, buf, sizeof(buf), 0,
                         (struct sockaddr *)&client, &clen);
        if (n < 12) continue; /* Timeout oder zu kurz → nächste Runde */

        /* Minimale DNS-Antwort: alle Anfragen → 192.168.4.1 */
        uint8_t resp[512];
        memcpy(resp, buf, n);
        resp[2] = 0x81; resp[3] = 0x80;  /* QR=1, RCODE=0 */
        resp[4] = 0x00; resp[5] = 0x01;  /* QDCOUNT = 1 */
        resp[6] = 0x00; resp[7] = 0x01;  /* ANCOUNT = 1 */
        resp[8] = 0x00; resp[9] = 0x00;
        resp[10]= 0x00; resp[11]= 0x00;

        /* Ende der Question Section finden */
        int qend = 12;
        while (qend < n && buf[qend]) {
            int label_len = buf[qend];
            if (qend + 1 + label_len >= n) break;
            qend += label_len + 1;
        }
        qend += 5; /* null-byte + QTYPE(2) + QCLASS(2) */
        if (qend > n) continue;

        /* Answer Record anhängen */
        int rlen = qend;
        resp[rlen++] = 0xC0; resp[rlen++] = 0x0C; /* Name-Pointer auf Question */
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;  /* TYPE A */
        resp[rlen++] = 0x00; resp[rlen++] = 0x01;  /* CLASS IN */
        resp[rlen++] = 0x00; resp[rlen++] = 0x00;
        resp[rlen++] = 0x00; resp[rlen++] = 0x3C;  /* TTL 60s */
        resp[rlen++] = 0x00; resp[rlen++] = 0x04;  /* RDLENGTH 4 */
        resp[rlen++] = 192;  resp[rlen++] = 168;
        resp[rlen++] = 4;    resp[rlen++] = 1;      /* 192.168.4.1 */

        sendto(sock, resp, rlen, 0,
               (struct sockaddr *)&client, clen);
    }

    close(sock);
    ESP_LOGI(TAG, "DNS Task beendet");
    vTaskDelete(NULL);
}

/* ── WiFi AP Mode  (FIX: OFFEN, kein Passwort) ───────────────────────────── */
static void start_ap_mode(void) {
    ESP_LOGI(TAG, "AP Mode: SSID=%s  (offen, kein Passwort)", WIFI_AP_SSID);

    esp_netif_t *ap_netif = esp_netif_create_default_wifi_ap();

    esp_netif_ip_info_t ip_info = {
        .ip      = { .addr = ESP_IP4TOADDR(192, 168, 4,   1) },
        .netmask = { .addr = ESP_IP4TOADDR(255, 255, 255, 0) },
        .gw      = { .addr = ESP_IP4TOADDR(192, 168, 4,   1) },
    };
    esp_netif_dhcps_stop(ap_netif);
    esp_netif_set_ip_info(ap_netif, &ip_info);
    esp_netif_dhcps_start(ap_netif);

    wifi_config_t cfg = {
        .ap = {
            .ssid           = WIFI_AP_SSID,
            .ssid_len       = strlen(WIFI_AP_SSID),
            .channel        = WIFI_AP_CHANNEL,
            .password       = "",         /* FIX: kein Passwort */
            .max_connection = WIFI_AP_MAX_CONN,
            .authmode       = WIFI_AUTH_OPEN,   /* FIX: offenes Netz */
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    start_mdns();
    start_http_server(true);
    xTaskCreate(dns_captive_task, "dns_captive", 4096, NULL, 2, NULL);

    ESP_LOGI(TAG, "AP bereit → http://192.168.4.1  oder  http://funkbridge.local");
}

/* ── WiFi Station Mode ────────────────────────────────────────────────────── */
static void start_station_mode(void) {
    char ssid[64] = {0}, pass[64] = {0};
    nvs_get_credentials(ssid, sizeof(ssid), pass, sizeof(pass));

    if (!ssid[0]) {
        ESP_LOGW(TAG, "Keine WLAN-Zugangsdaten → Fallback AP Mode");
        start_ap_mode();
        return;
    }

    ESP_LOGI(TAG, "Verbinde mit: %s", ssid);
    esp_netif_create_default_wifi_sta();

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, pass, sizeof(cfg.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_connect();

    int waited = 0;
    while (waited < WIFI_CONNECT_TIMEOUT_S * 10) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            ESP_LOGI(TAG, "Verbunden mit %s", ssid);
            start_mdns();
            start_http_server(false);
            ESP_LOGI(TAG, "Bereit: http://funkbridge.local");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        waited++;
    }

    ESP_LOGW(TAG, "Verbindungs-Timeout → Fallback AP Mode");
    esp_wifi_stop();
    start_ap_mode();
}

/* ── SPIFFS Mount ─────────────────────────────────────────────────────────── */
static void init_spiffs(void) {
    esp_vfs_spiffs_conf_t conf = {
        .base_path              = "/spiffs",
        .partition_label        = NULL,
        .max_files              = 8,
        .format_if_mount_failed = false,
    };
    esp_err_t ret = esp_vfs_spiffs_register(&conf);
    if (ret == ESP_OK) {
        size_t total = 0, used = 0;
        esp_spiffs_info(NULL, &total, &used);
        ESP_LOGI(TAG, "SPIFFS gemountet — %d/%d Bytes genutzt", used, total);
    } else {
        ESP_LOGE(TAG, "SPIFFS Mount FEHLGESCHLAGEN (%s) → "
                      "bitte 'idf.py -p PORT spiffs-flash' ausführen!",
                 esp_err_to_name(ret));
    }
}

/* ── Öffentliche Schnittstelle ────────────────────────────────────────────── */
void wifi_server_start(void) {
    s_running    = true;
    s_ws_fd      = -1;
    s_tx_ringbuf = xRingbufferCreate(WS_RING_BUF_SIZE, RINGBUF_TYPE_BYTEBUF);
    if (!s_tx_ringbuf) {
        ESP_LOGE(TAG, "Ring-Buffer Allokierung fehlgeschlagen!");
        return;
    }

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    init_spiffs();

    xTaskCreate(ws_send_task, "ws_send", 4096, NULL, 2, NULL);

    funkbridge_wifi_mode_t mode = wifi_get_mode();
    if (mode == WIFI_MODE_STATION)
        start_station_mode();
    else
        start_ap_mode();
}

void wifi_server_stop(void) {
    if (!s_running) return;
    s_running = false;

    ESP_LOGI(TAG, "WiFi Server wird gestoppt...");

    if (s_server) {
        httpd_stop(s_server);
        s_server = NULL;
    }

    esp_wifi_stop();
    esp_wifi_deinit();
    mdns_free();

    /* FIX: Ring-Buffer aufräumen */
    if (s_tx_ringbuf) {
        vRingbufferDelete(s_tx_ringbuf);
        s_tx_ringbuf = NULL;
    }

    s_ws_fd = -1;
    ESP_LOGI(TAG, "WiFi Subsystem gestoppt");
}