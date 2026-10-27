/*
   +----------------------------------------------------------------------+
   | php-baremetal: esp32-wifi-hal                                        |
   +----------------------------------------------------------------------+
   | Copyright (c) 2026 Gianfrancesco Aurecchia                           |
   +----------------------------------------------------------------------+
   | This source file is subject to the MIT license, that is bundled with |
   | this package in the file LICENSE.                                    |
   +----------------------------------------------------------------------+
   | Authors: Gianfrancesco Aurecchia <gianfri@php-baremetal.com>         |
   +----------------------------------------------------------------------+
*/

/*
 * ESP32 implementation of the WiFi HAL (bm_wifi_hal.h) over esp_wifi / esp_netif / esp_event. Holds the
 * whole lifecycle: one-time bring-up, the event-driven connection state (got-IP via an event group),
 * STA scan/join and SoftAP. The neutral binding (wifi.c) is unaware of all of it.
 *
 * Every esp_wifi_* / esp_netif_* / nvs_flash_* call runs on the executor task (internal-RAM stack): the
 * PHP task's stack is in PSRAM and the driver may touch flash. Waits (scan done, got IP) happen on the
 * caller's task. lwIP and the default event loop are brought up by the host before PHP starts.
 */
#include "bm_wifi_hal.h"
#include "executor.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STA_GOT_IP      BIT0
#define SCAN_DONE       BIT1
#define WIFI_ENQUEUE_MS 1000
#define SCAN_TIMEOUT_MS 15000

static const char *TAG = "php-wifi";

static bool s_inited  = false;
static bool s_started = false;
static bool s_sta_on  = false;
static bool s_ap_on   = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;
static EventGroupHandle_t s_events = NULL;
static portMUX_TYPE s_ip_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_sta_ip[16] = "";
/* Both guarded by s_ip_lock (written on the executor, read-modify-written on the event task). */
static int s_sta_retry  = 0;   /* auto-retries left for the connect in progress (0 = give up) */
static int s_sta_ignore = 0;   /* disconnect events caused by our own esp_wifi_disconnect(), to skip */

typedef struct {
    const char *ssid;
    const char *password;
    int channel;
    int max_conn;
    bm_wifi_ap_t *aps;
    int max;
    int count;
    int value;
    char *buf;
    size_t len;
} wifi_op_t;

static esp_err_t run(esp_err_t (*fn)(void *arg), wifi_op_t *op)
{
    return executor_run_sync(fn, op, WIFI_ENQUEUE_MS);
}

static void set_sta_ip(const char *ip)
{
    portENTER_CRITICAL(&s_ip_lock);
    strlcpy(s_sta_ip, ip, sizeof s_sta_ip);
    portEXIT_CRITICAL(&s_ip_lock);
}

/* Runs on the default event loop task (internal stack): never calls into PHP. */
static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_SCAN_DONE) {
        xEventGroupSetBits(s_events, SCAN_DONE);
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *) data;
        set_sta_ip("");
        xEventGroupClearBits(s_events, STA_GOT_IP);
        /* The first association often drops with a transient reason (4 assoc-expire, 200-series);
         * retry a few times before giving up rather than failing on the first hiccup. */
        bool ignore = false, retry = false;
        portENTER_CRITICAL(&s_ip_lock);
        if (s_sta_ignore > 0) {
            s_sta_ignore--;
            ignore = true;
        } else if (s_sta_retry > 0) {
            s_sta_retry--;
            retry = true;
        }
        portEXIT_CRITICAL(&s_ip_lock);
        if (ignore) {
            return;
        }
        if (retry) {
            ESP_LOGW(TAG, "sta disconnected, reason=%d -- retrying", d ? d->reason : -1);
            /* Deliberate exception to the executor rule: this is the default event-loop task (internal
             * stack, not PHP's PSRAM stack), and executor_run_sync from here would block the loop. */
            esp_wifi_connect();
        } else {
            ESP_LOGW(TAG, "sta disconnected, reason=%d", d ? d->reason : -1);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *) data;
        char ip[16];
        snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip_info.ip));
        set_sta_ip(ip);
        xEventGroupSetBits(s_events, STA_GOT_IP);
    }
}

static esp_err_t do_init(void *arg)
{
    (void) arg;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs init failed: %s", esp_err_to_name(err));
        return err;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "wifi init failed: %s", esp_err_to_name(err));
        return err;
    }
    if (s_events == NULL) {
        s_events = xEventGroupCreate();
    }
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL);
    }
    return err;
}

int bm_wifi_hal_init(void)
{
    if (s_inited) {
        return 0;
    }
    if (run(do_init, NULL) != ESP_OK) {
        return -1;
    }
    s_inited = true;
    return 0;
}

static wifi_mode_t desired_mode(void)
{
    if (s_sta_on && s_ap_on) return WIFI_MODE_APSTA;
    if (s_sta_on)            return WIFI_MODE_STA;
    if (s_ap_on)             return WIFI_MODE_AP;
    return WIFI_MODE_NULL;
}

/* Executor only. */
static esp_err_t apply_mode(void)
{
    wifi_mode_t m = desired_mode();
    if (m == WIFI_MODE_NULL) {
        if (s_started) {
            esp_wifi_stop();
            s_started = false;
        }
        return ESP_OK;
    }
    esp_err_t err = esp_wifi_set_mode(m);
    if (err == ESP_OK && !s_started) {
        err = esp_wifi_start();
        s_started = err == ESP_OK;
    }
    return err;
}

/* Executor only. */
static esp_err_t sta_up(void)
{
    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            return ESP_FAIL;
        }
    }
    bool was_on = s_sta_on;
    s_sta_on = true;
    esp_err_t err = apply_mode();
    if (err != ESP_OK) {
        s_sta_on = was_on;
    }
    return err;
}

static const char *auth_str(wifi_auth_mode_t a)
{
    switch (a) {
        case WIFI_AUTH_OPEN:            return "open";
        case WIFI_AUTH_WEP:             return "wep";
        case WIFI_AUTH_WPA_PSK:         return "wpa";
        case WIFI_AUTH_WPA2_PSK:        return "wpa2";
        case WIFI_AUTH_WPA_WPA2_PSK:    return "wpa/wpa2";
        case WIFI_AUTH_WPA2_ENTERPRISE: return "wpa2-ent";
        case WIFI_AUTH_WPA3_PSK:        return "wpa3";
        case WIFI_AUTH_WPA2_WPA3_PSK:   return "wpa2/wpa3";
        case WIFI_AUTH_WAPI_PSK:        return "wapi";
        default:                        return "unknown";
    }
}

static esp_err_t do_scan_start(void *arg)
{
    (void) arg;
    esp_err_t err = sta_up();
    if (err != ESP_OK) {
        return err;
    }
    xEventGroupClearBits(s_events, SCAN_DONE);
    wifi_scan_config_t sc = { 0 };
    return esp_wifi_scan_start(&sc, false);
}

static esp_err_t do_scan_abort(void *arg)
{
    (void) arg;
    esp_wifi_scan_stop();
    esp_wifi_clear_ap_list();
    return ESP_OK;
}

static esp_err_t do_scan_fetch(void *arg)
{
    wifi_op_t *op = arg;
    uint16_t n = 0;
    op->count = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        esp_wifi_clear_ap_list();
        return ESP_OK;
    }
    if (op->max > 0 && n > (uint16_t) op->max) {
        n = (uint16_t) op->max;   /* get_ap_records with this count frees the rest of the list */
    }
    wifi_ap_record_t *recs = malloc(n * sizeof(wifi_ap_record_t));
    if (recs == NULL) {
        esp_wifi_clear_ap_list();
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = esp_wifi_scan_get_ap_records(&n, recs);
    if (err == ESP_OK) {
        int count = (int) n > op->max ? op->max : (int) n;
        for (int i = 0; i < count; i++) {
            bm_wifi_ap_t *ap = &op->aps[i];
            strlcpy(ap->ssid, (const char *) recs[i].ssid, sizeof ap->ssid);
            memcpy(ap->bssid, recs[i].bssid, sizeof ap->bssid);
            ap->rssi = recs[i].rssi;
            ap->channel = recs[i].primary;
            strlcpy(ap->auth, auth_str(recs[i].authmode), sizeof ap->auth);
        }
        op->count = count;
    }
    free(recs);
    return err;
}

int bm_wifi_hal_scan(bm_wifi_ap_t *out, int max)
{
    if (bm_wifi_hal_init() != 0) {
        return -1;
    }
    if (run(do_scan_start, NULL) != ESP_OK) {
        return -1;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, SCAN_DONE, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(SCAN_TIMEOUT_MS));
    if ((bits & SCAN_DONE) == 0) {
        run(do_scan_abort, NULL);
        return -1;
    }
    wifi_op_t op = { .aps = out, .max = max };
    if (run(do_scan_fetch, &op) != ESP_OK) {
        return -1;
    }
    return op.count;
}

static esp_err_t do_sta_connect(void *arg)
{
    wifi_op_t *op = arg;
    wifi_config_t wc = { 0 };
    strlcpy((char *) wc.sta.ssid, op->ssid, sizeof wc.sta.ssid);
    /* Scan every channel and pick the strongest match -- more robust than the default fast scan,
     * which stops at the first hit and can miss WPA2/WPA3-transition APs. */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (op->password && op->password[0]) {
        strlcpy((char *) wc.sta.password, op->password, sizeof wc.sta.password);
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        /* Support WPA2/WPA3-transition APs (common on phone hotspots): PMF-capable and WPA3-SAE
         * with both hunt-and-peck and hash-to-element. Harmless for plain WPA2 networks. */
        wc.sta.pmf_cfg.capable = true;
        wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    } else {
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    esp_err_t err = sta_up();
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    }
    if (err != ESP_OK) {
        return err;
    }
    /* Drop a previous association first. Its STA_DISCONNECTED event arrives asynchronously on the
     * event task: count it so the handler skips it instead of burning a retry / double-connecting.
     * Only an associated STA is sure to emit that event, so only then is it counted. */
    wifi_ap_record_t cur;
    bool associated = esp_wifi_sta_get_ap_info(&cur) == ESP_OK;   /* takes a semaphore: outside the lock */
    portENTER_CRITICAL(&s_ip_lock);
    s_sta_retry = 0;
    s_sta_ignore = associated ? 1 : 0;
    portEXIT_CRITICAL(&s_ip_lock);
    if (associated) {
        esp_wifi_disconnect();
    }
    set_sta_ip("");
    xEventGroupClearBits(s_events, STA_GOT_IP);
    portENTER_CRITICAL(&s_ip_lock);
    s_sta_retry = 10;   /* auto-retry transient association drops during this connect */
    portEXIT_CRITICAL(&s_ip_lock);
    return esp_wifi_connect();
}

static esp_err_t do_sta_disconnect(void *arg)
{
    (void) arg;
    portENTER_CRITICAL(&s_ip_lock);
    s_sta_retry = 0;
    portEXIT_CRITICAL(&s_ip_lock);
    esp_wifi_disconnect();
    set_sta_ip("");
    return ESP_OK;
}

bool bm_wifi_hal_sta_connect(const char *ssid, const char *password, int timeout_ms)
{
    if (bm_wifi_hal_init() != 0) {
        return false;
    }
    wifi_op_t op = { .ssid = ssid, .password = password };
    if (run(do_sta_connect, &op) != ESP_OK) {
        return false;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, STA_GOT_IP, pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_ms));
    if ((bits & STA_GOT_IP) == 0) {
        /* Timed out: stop the background retries so we cannot connect after reporting failure. */
        run(do_sta_disconnect, NULL);
        return false;
    }
    return true;
}

void bm_wifi_hal_sta_disconnect(void)
{
    if (s_inited) {
        run(do_sta_disconnect, NULL);
    }
}

bool bm_wifi_hal_sta_connected(void)
{
    portENTER_CRITICAL(&s_ip_lock);
    bool connected = s_sta_ip[0] != '\0';
    portEXIT_CRITICAL(&s_ip_lock);
    return connected;
}

void bm_wifi_hal_sta_ip(char *out, size_t len)
{
    char ip[16];
    portENTER_CRITICAL(&s_ip_lock);
    memcpy(ip, s_sta_ip, sizeof ip);
    portEXIT_CRITICAL(&s_ip_lock);
    strlcpy(out, ip, len);
}

static esp_err_t do_sta_rssi(void *arg)
{
    wifi_op_t *op = arg;
    wifi_ap_record_t ap;
    esp_err_t err = esp_wifi_sta_get_ap_info(&ap);
    if (err == ESP_OK) {
        op->value = ap.rssi;
    }
    return err;
}

bool bm_wifi_hal_sta_rssi(int *out)
{
    wifi_op_t op = { 0 };
    /* Not connected: false without asking the driver, which logs an error when not associated. */
    if (!s_inited || !bm_wifi_hal_sta_connected() || run(do_sta_rssi, &op) != ESP_OK) {
        return false;
    }
    *out = op.value;
    return true;
}

static esp_err_t do_ap_start(void *arg)
{
    wifi_op_t *op = arg;
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            return ESP_FAIL;
        }
    }
    wifi_config_t wc = { 0 };
    strlcpy((char *) wc.ap.ssid, op->ssid, sizeof wc.ap.ssid);
    wc.ap.ssid_len = (uint8_t) strlen((const char *) wc.ap.ssid);
    wc.ap.channel = (uint8_t) op->channel;
    wc.ap.max_connection = (uint8_t) op->max_conn;
    if (op->password && strlen(op->password) >= 8) {
        strlcpy((char *) wc.ap.password, op->password, sizeof wc.ap.password);
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        /* WPA2 needs >= 8 chars; anything shorter (or none) opens the network. */
        wc.ap.authmode = WIFI_AUTH_OPEN;
    }

    s_ap_on = true;
    esp_err_t err = apply_mode();
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    }
    if (err != ESP_OK) {
        s_ap_on = false;
        apply_mode();
    }
    return err;
}

bool bm_wifi_hal_ap_start(const char *ssid, const char *password, int channel, int max_conn)
{
    if (bm_wifi_hal_init() != 0) {
        return false;
    }
    wifi_op_t op = { .ssid = ssid, .password = password, .channel = channel, .max_conn = max_conn };
    return run(do_ap_start, &op) == ESP_OK;
}

static esp_err_t do_ap_stop(void *arg)
{
    (void) arg;
    s_ap_on = false;
    return apply_mode();
}

void bm_wifi_hal_ap_stop(void)
{
    if (s_inited) {
        run(do_ap_stop, NULL);
    }
}

static esp_err_t do_ap_ip(void *arg)
{
    wifi_op_t *op = arg;
    esp_netif_ip_info_t info;
    esp_err_t err = esp_netif_get_ip_info(s_ap_netif, &info);
    if (err == ESP_OK) {
        snprintf(op->buf, op->len, IPSTR, IP2STR(&info.ip));
    }
    return err;
}

void bm_wifi_hal_ap_ip(char *out, size_t len)
{
    out[0] = '\0';
    if (s_ap_netif == NULL || !s_ap_on) {
        return;
    }
    char ip[16] = "";
    wifi_op_t op = { .buf = ip, .len = sizeof ip };
    if (run(do_ap_ip, &op) == ESP_OK) {
        strlcpy(out, ip, len);
    }
}

static esp_err_t do_ap_clients(void *arg)
{
    wifi_op_t *op = arg;
    wifi_sta_list_t list;
    esp_err_t err = esp_wifi_ap_get_sta_list(&list);
    if (err == ESP_OK) {
        op->value = list.num;
    }
    return err;
}

int bm_wifi_hal_ap_clients(void)
{
    wifi_op_t op = { 0 };
    if (!s_inited || run(do_ap_clients, &op) != ESP_OK) {
        return 0;
    }
    return op.value;
}
