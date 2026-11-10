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
 * ESP32 implementation of the WiFi HAL (bm_wifi_hal.h, contract 1.2) over esp_wifi / esp_netif / esp_event.
 * Holds the whole lifecycle: one-time bring-up, the event-driven connection state (got-IP via an event group),
 * STA scan/join, SoftAP, and the listener. The neutral binding (wifi.c) is unaware of all of it.
 *
 * Every esp_wifi_* / esp_netif_* / nvs_flash_* call made for PHP runs on the executor task (internal-RAM
 * stack): the PHP task's stack is in PSRAM and the driver may touch flash. Waits (scan done, got IP, gave up)
 * happen on the caller's task. lwIP and the default event loop are brought up by the host before PHP starts.
 *
 * Events: wifi_evt() runs on the default event loop task and is the only caller of the listener, so the
 * listener hears every event from one task, in order, never with a lock held. What the executor has to say
 * (the start of a new attempt, an aborted scan) it posts to the same loop (BM_HAL_BASE) rather than calling
 * the listener itself.
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
#include "freertos/task.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STA_GOT_IP      BIT0
#define SCAN_DONE       BIT1
#define STA_GAVE_UP     BIT2
#define WIFI_ENQUEUE_MS 1000
#define SCAN_TIMEOUT_MS 15000
#define SCAN_MAX        48    /* scan results kept for scan_results() (ext-wifi asks for 48) */
#define STA_RETRIES     10    /* bm_wifi_hal_sta_connect(): the 1.0 budget */
#define POST_MS         100
#define PW_REFUSALS     2     /* consecutive password refusals, before the first IP, that give up WRONG_PASSWORD */

/* Board measurement: log the default event-loop task's stack high-water mark once after the first SCAN_DONE
 * and once after the first STA_GOT_IP (bytes left, ESP-IDF counts the stack in bytes). Off by default; build
 * with -DBM_WIFI_HAL_LOG_EVT_STACK=1 to measure. */
#ifndef BM_WIFI_HAL_LOG_EVT_STACK
#define BM_WIFI_HAL_LOG_EVT_STACK 0
#endif

/* Our own events on the default loop, so the listener is still called from the loop task only. */
static const char BM_HAL_BASE[] = "bm_wifi_hal";
enum { HAL_EV_FENCE = 1, HAL_EV_SCAN_ABORTED, HAL_EV_AP_RECONFIGURED };
enum { OP_CONNECT = 0, OP_DISCONNECT, OP_TIMEOUT };   /* what began an attempt generation */
typedef struct { uint32_t gen; int op; } hal_fence_t;

static const char *TAG = "php-wifi";

static bool s_inited  = false;
static bool s_started = false;
static bool s_sta_on  = false;
static bool s_ap_on   = false;   /* written on the executor; read anywhere with __atomic_load_n */
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif  = NULL;
static EventGroupHandle_t s_events = NULL;
static void (*s_listener)(const bm_wifi_hal_event_t *ev) = NULL;

/* Guarded by s_ip_lock (written on the executor, read-modify-written on the event task). */
static portMUX_TYPE s_ip_lock = portMUX_INITIALIZER_UNLOCKED;
static char s_sta_ip[16] = "";
static char s_sta_ssid[33] = "";   /* asked for by the last connect; kept after a give-up */
static bool s_sta_want  = false;   /* PHP wants the station connected: off after a disconnect, a give-up, a timeout */
static int s_sta_retry  = 0;       /* auto-retries left for the connect in progress (0 = give up, -1 = forever) */
static int s_sta_retry_max = 0;    /* the connect's `retries`: the budget refilled at every IP */
static bool s_sta_assoc = false;   /* the listener heard STA_CONNECTED and no STA_DISCONNECTED since */
static bool s_sta_had_ip = false;  /* an IP since the last connect (for the reason mapping) */
static int s_sta_refusals = 0;     /* consecutive password refusals before the first IP (see PW_REFUSALS) */
/*
 * Attempt generations and the fence (contract rule (a), and "a cancel never gives up").
 *
 * Every sta_connect_ex(), sta_disconnect() and wait-true timeout first bumps s_gen under s_ip_lock (and clears
 * s_sta_want), then cancels the old attempt with esp_wifi_disconnect() when one was in progress -- before
 * esp_wifi_set_config() -- and then posts HAL_EV_FENCE{gen} to the default loop, before the new attempt's
 * esp_wifi_connect(). The driver posts its events to the same loop, so every station event the old attempt
 * raised up to the cancel is queued ahead of the fence, and every event of the new attempt after it.
 *
 * - While s_fence_gen != s_gen (a fence still in the queue) station events are stale: STA_CONNECTED,
 *   STA_GOT_IP and STA_LOST_IP are dropped; STA_DISCONNECTED only closes the association the listener had
 *   heard (BY_USER, or the mapped reason for a timeout), and never retries or gives up.
 * - The fence closes the old association if the listener still thinks it is up (the same STA_DISCONNECTED),
 *   and for a timeout sends STA_GAVE_UP(OTHER, 0). s_sta_assoc is cleared by whichever sends the
 *   STA_DISCONNECTED, so an old association yields exactly one.
 * - The one old event that can still come after the fence is the ASSOC_LEAVE (8) of our own
 *   esp_wifi_disconnect(). s_sta_ignore counts those: an op only ever adds to it, the loop swallows one
 *   ASSOC_LEAVE per count, and the new attempt's first STA_CONNECTED (the driver has left the old network by
 *   then) resets it.
 * - A fence that cannot be posted sets s_fence_gen itself: the filtering is lost for that op, the new
 *   attempt's events are not.
 * - STA_GAVE_UP stamps s_gave_up_gen, so a wait-true connect only takes a give-up of its own generation.
 */
static uint32_t s_gen = 0;          /* the current attempt generation */
static uint32_t s_fence_gen = 0;    /* the newest fence the loop has seen */
static int s_gen_op = OP_CONNECT;   /* what began s_gen */
static uint32_t s_gave_up_gen = 0;  /* the generation of the last STA_GAVE_UP */
static int s_sta_ignore = 0;        /* ASSOC_LEAVE disconnects of our own cancels that may still come */
static int s_ap_clients = 0;        /* stations on the SoftAP, from the AP_STA(DIS)CONNECTED events */
static int s_ap_stop_clients = -1;  /* s_ap_clients when ap_stop() was asked, for AP_STOPPED */
/*
 * The access point as the script sees it. A mode change the station causes (AP -> APSTA when a scan or a
 * connect switches the station on, and back) makes ESP-IDF raise WIFI_EVENT_AP_STOP and then AP_START while
 * the access point stays up for the script. So each driver AP event is decided, under s_ip_lock, from the
 * state the script asked for (s_ap_on, set by ap_start(), cleared by ap_stop()) and from s_ap_reported
 * (AP_STARTED delivered and no AP_STOPPED since), never from which call raised it:
 * - AP_STOP: AP_STOPPED only when the access point is no longer wanted and was reported; while it is still
 *   wanted it is the first half of a mode change, and the client count is kept.
 * - AP_START: ignored while the access point is not wanted (a stop came after it, and the stop's own event
 *   settles the state); wanted and not reported, AP_STARTED; wanted and reported, the restart of a mode
 *   change, which resyncs the client count instead.
 * - ap_start() while the script still has it reported (already up, or stopped so recently that the
 *   AP_STOPPED has not been delivered) posts HAL_EV_AP_RECONFIGURED, delivered as AP_STARTED while the access
 *   point is wanted and reported: so every ap_start() gives one AP_STARTED.
 * E.g. a scan, then ap_stop() at once: the mode change's AP_STOP already finds the access point unwanted and
 * delivers the one AP_STOPPED; its AP_START and the stop's AP_STOP are then ignored.
 */
static bool s_ap_reported = false;  /* AP_STARTED delivered, and no AP_STOPPED since */

/* Guarded by s_scan_lock: the last finished scan. */
static portMUX_TYPE s_scan_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_scan_running = false;
static uint32_t s_scan_gen = 0;    /* bumped by every started scan: an abort only ends its own */
static int s_scan_count = -1;      /* -1: none yet, or the last scan failed */
static bm_wifi_ap_t s_scan_buf[SCAN_MAX];

typedef struct {
    const char *ssid;
    const char *password;
    int channel;
    int max_conn;
    int value;
    int result;
    uint32_t gen;
    char *buf;
    size_t len;
} wifi_op_t;

static esp_err_t run(esp_err_t (*fn)(void *arg), wifi_op_t *op)
{
    return executor_run_sync(fn, op, WIFI_ENQUEUE_MS);
}

/* ---- events (default event loop task) ---- */

static void emit(const bm_wifi_hal_event_t *ev)
{
    void (*cb)(const bm_wifi_hal_event_t *) = __atomic_load_n(&s_listener, __ATOMIC_ACQUIRE);
    if (cb != NULL) {
        cb(ev);
    }
}

static void emit_sta(bm_wifi_ev_type_t type, bm_wifi_reason_t reason, int code, bool retrying)
{
    bm_wifi_hal_event_t ev = { .type = type, .reason = reason, .code = code, .retrying = retrying };
    emit(&ev);
}

static void emit_ap(bm_wifi_ev_type_t type, const uint8_t *mac, int clients)
{
    bm_wifi_hal_event_t ev = { .type = type, .clients = clients };
    if (mac != NULL) {
        memcpy(ev.mac, mac, sizeof ev.mac);
    }
    emit(&ev);
}

void bm_wifi_hal_set_listener(void (*cb)(const bm_wifi_hal_event_t *ev))
{
    __atomic_store_n(&s_listener, cb, __ATOMIC_RELEASE);
}

/* A password refusal: the codes ESP-IDF reports when the AP turns the key down. A weak link or a busy AP can
 * raise them for a correct password too, so they count only before the attempt's first IP, and only
 * PW_REFUSALS in a row give up (see on_sta_disconnected). AUTH_EXPIRE (2) is the AP not answering: not one. */
static bool is_refusal(uint8_t r)
{
    return r == WIFI_REASON_AUTH_FAIL || r == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT
        || r == WIFI_REASON_HANDSHAKE_TIMEOUT;
}

/* The ESP-IDF disconnect reason (esp_wifi_types_generic.h) as a neutral one. had_ip: an IP was obtained
 * since the connect began; assoc: the listener heard STA_CONNECTED for this association; by_user: the
 * disconnect was asked for. */
static bm_wifi_reason_t map_reason(uint8_t r, bool had_ip, bool assoc, bool by_user)
{
    if (by_user) {
        return BM_WIFI_REASON_BY_USER;
    }
    /* A refusal before this attempt's first IP is a (possibly) wrong password: an IP proves the password,
     * so a later authentication or handshake failure is a link that went away. */
    if (is_refusal(r) && !had_ip) {
        return BM_WIFI_REASON_WRONG_PASSWORD;
    }
    /* Once associated, any other unrequested end is a connection that was up and went away (a hotspot
     * switched off deauthenticates first, with 3 and similar codes). */
    if (assoc) {
        return BM_WIFI_REASON_LOST;
    }
    switch (r) {
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_AUTH_EXPIRE:
            return had_ip ? BM_WIFI_REASON_LOST : BM_WIFI_REASON_OTHER;
        case WIFI_REASON_NO_AP_FOUND:
        case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
        case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
        case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
            return BM_WIFI_REASON_NOT_FOUND;
        case WIFI_REASON_BEACON_TIMEOUT:
            return BM_WIFI_REASON_LOST;
        case WIFI_REASON_CONNECTION_FAIL:
            return had_ip ? BM_WIFI_REASON_LOST : BM_WIFI_REASON_OTHER;
        default:
            return BM_WIFI_REASON_OTHER;
    }
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

/* Under s_ip_lock: a fence is still queued, so a station event now belongs to a cancelled attempt. */
static bool stale_locked(void)
{
    return s_fence_gen != s_gen;
}

/* The reason an old association closes with: BY_USER for a disconnect or a cancelling connect. */
static bm_wifi_reason_t old_reason_locked(uint8_t code)
{
    return s_gen_op == OP_TIMEOUT ? map_reason(code, s_sta_had_ip, false, false) : BM_WIFI_REASON_BY_USER;
}

static void on_sta_connected(void)
{
    portENTER_CRITICAL(&s_ip_lock);
    bool deliver = !stale_locked() && s_sta_want;
    if (deliver) {
        s_sta_assoc = true;
        s_sta_ignore = 0;   /* the driver has left the old network before joining this one */
    }
    portEXIT_CRITICAL(&s_ip_lock);
    if (deliver) {
        emit_sta(BM_WIFI_EV_STA_CONNECTED, BM_WIFI_REASON_OTHER, 0, false);
    }
}

static void on_sta_disconnected(const wifi_event_sta_disconnected_t *d)
{
    enum { D_OLD, D_STRAY, D_RETRY, D_GIVE_UP } act;
    uint8_t code = d ? d->reason : 0;
    portENTER_CRITICAL(&s_ip_lock);
    uint32_t gen = s_gen;
    bool was_assoc = s_sta_assoc;
    bool had_ip = s_sta_had_ip;
    bm_wifi_reason_t old_reason = old_reason_locked(code);
    bool own_leave = code == WIFI_REASON_ASSOC_LEAVE && s_sta_ignore > 0;
    if (stale_locked() || own_leave) {
        if (own_leave) {
            s_sta_ignore--;
        }
        act = D_OLD;   /* the cancelled attempt's: closes what the listener heard, nothing more */
    } else if (!s_sta_want) {
        act = D_STRAY;
    } else {
        /* PW_REFUSALS password refusals in a row before the first IP give up at once (WRONG_PASSWORD),
         * whatever `retries` is, -1 included. A single one is retried like any other reason, and any other
         * reason starts the count again. */
        s_sta_refusals = is_refusal(code) && !had_ip ? s_sta_refusals + 1 : 0;
        bool wrong_password = s_sta_refusals >= PW_REFUSALS;
        if (s_sta_retry != 0 && !wrong_password) {
            /* The first association often drops with a transient reason (4 assoc-expire, 200-series):
             * retry rather than failing on the first hiccup. -1 retries forever. */
            if (s_sta_retry > 0) {
                s_sta_retry--;
            }
            act = D_RETRY;
        } else {
            s_sta_want = false;
            s_gave_up_gen = gen;
            act = D_GIVE_UP;
        }
    }
    s_sta_assoc = false;
    s_sta_ip[0] = '\0';
    portEXIT_CRITICAL(&s_ip_lock);
    xEventGroupClearBits(s_events, STA_GOT_IP);

    bm_wifi_reason_t reason = map_reason(code, had_ip, was_assoc, false);
    if (act == D_RETRY) {
        ESP_LOGW(TAG, "sta disconnected, reason=%d -- retrying", code);
        /* Deliberate exception to the executor rule: this is the default event-loop task (internal
         * stack, not PHP's PSRAM stack), and executor_run_sync from here would block the loop. */
        esp_err_t err = esp_wifi_connect();
        /* A connect or a stop may have run between the decision and the call: this drop is then the old
         * attempt's (retrying false). After a stop (an op other than a connect, which cancels by itself) the
         * rejoin we just started is cancelled again, and that leave is counted like the op's own. */
        portENTER_CRITICAL(&s_ip_lock);
        bool superseded = s_gen != gen;
        bool stopped = superseded && s_gen_op != OP_CONNECT && err == ESP_OK;
        if (stopped) {
            s_sta_ignore++;
        }
        if (err != ESP_OK && !superseded) {
            s_sta_want = false;
            s_sta_retry = 0;
            s_gave_up_gen = gen;
        }
        portEXIT_CRITICAL(&s_ip_lock);
        if (stopped) {
            esp_wifi_disconnect();
        }
        if (superseded) {
            act = D_STRAY;
        } else if (err != ESP_OK) {
            act = D_GIVE_UP;
        }
    } else if (act != D_OLD) {
        ESP_LOGW(TAG, "sta disconnected, reason=%d", code);
    }

    switch (act) {
        case D_OLD:
            if (was_assoc) {
                emit_sta(BM_WIFI_EV_STA_DISCONNECTED, old_reason, code, false);
            }
            break;
        case D_STRAY:
            if (was_assoc) {
                emit_sta(BM_WIFI_EV_STA_DISCONNECTED, reason, code, false);
            }
            break;
        case D_RETRY:
            if (was_assoc) {
                emit_sta(BM_WIFI_EV_STA_DISCONNECTED, reason, code, true);
            }
            break;
        case D_GIVE_UP:
            if (was_assoc) {
                emit_sta(BM_WIFI_EV_STA_DISCONNECTED, reason, code, false);
            }
            emit_sta(BM_WIFI_EV_STA_GAVE_UP, reason, code, false);
            xEventGroupSetBits(s_events, STA_GAVE_UP);
            break;
    }
}

static void on_sta_got_ip(const ip_event_got_ip_t *e)
{
    char ip[16];
    snprintf(ip, sizeof ip, IPSTR, IP2STR(&e->ip_info.ip));
    /* Connected: refill the budget, so a later drop is retried as many times as the first join.
     * Only for the current attempt's association: an IP queued before a cancel, a stop or a timeout (or
     * before this attempt's STA_CONNECTED) is dropped, or retries would start again. */
    portENTER_CRITICAL(&s_ip_lock);
    bool deliver = !stale_locked() && s_sta_want && s_sta_assoc;
    if (deliver) {
        s_sta_retry = s_sta_retry_max;
        s_sta_had_ip = true;
        s_sta_refusals = 0;
        strlcpy(s_sta_ip, ip, sizeof s_sta_ip);
    }
    portEXIT_CRITICAL(&s_ip_lock);
    if (deliver) {
        bm_wifi_hal_event_t ev = { .type = BM_WIFI_EV_STA_GOT_IP };
        ev.ip[0] = esp_ip4_addr1(&e->ip_info.ip);
        ev.ip[1] = esp_ip4_addr2(&e->ip_info.ip);
        ev.ip[2] = esp_ip4_addr3(&e->ip_info.ip);
        ev.ip[3] = esp_ip4_addr4(&e->ip_info.ip);
        emit(&ev);
        /* After the listener: a wait-true connect returns true only once STA_GOT_IP has been heard. */
        xEventGroupSetBits(s_events, STA_GOT_IP);
    }
}

static void on_sta_lost_ip(void)
{
    portENTER_CRITICAL(&s_ip_lock);
    bool deliver = !stale_locked() && s_sta_assoc;   /* only while associated: not after a disconnect */
    s_sta_ip[0] = '\0';
    portEXIT_CRITICAL(&s_ip_lock);
    xEventGroupClearBits(s_events, STA_GOT_IP);
    if (deliver) {
        emit_sta(BM_WIFI_EV_STA_LOST_IP, BM_WIFI_REASON_OTHER, 0, false);
    }
}

/* The fence of an op (see s_gen): station events after it belong to the attempt it began. */
static void on_fence(const hal_fence_t *f)
{
    portENTER_CRITICAL(&s_ip_lock);
    if ((int32_t) (f->gen - s_fence_gen) > 0) {
        s_fence_gen = f->gen;
    }
    bool current = f->gen == s_gen;   /* a newer op's fence is still queued: it closes what is left */
    bool was_assoc = current && s_sta_assoc;
    bool give_up = current && f->op == OP_TIMEOUT;
    bm_wifi_reason_t reason = old_reason_locked(WIFI_REASON_ASSOC_LEAVE);
    if (current) {
        s_sta_assoc = false;
    }
    if (give_up) {
        s_gave_up_gen = f->gen;
    }
    portEXIT_CRITICAL(&s_ip_lock);
    if (was_assoc) {
        emit_sta(BM_WIFI_EV_STA_DISCONNECTED, reason, WIFI_REASON_ASSOC_LEAVE, false);
    }
    if (give_up) {
        emit_sta(BM_WIFI_EV_STA_GAVE_UP, BM_WIFI_REASON_OTHER, 0, false);
        xEventGroupSetBits(s_events, STA_GAVE_UP);
    }
}

/* Ends the scan we started, if one is running (and, for an abort, still the aborted one): with the driver's
 * list (fetch) or failed (-1). */
static void scan_finish(bool fetch, const uint32_t *aborted_gen)
{
    portENTER_CRITICAL(&s_scan_lock);
    bool running = s_scan_running && (aborted_gen == NULL || *aborted_gen == s_scan_gen);
    portEXIT_CRITICAL(&s_scan_lock);
    if (!running) {
        return;   /* not a scan we started, already ended, or an abort of an older scan */
    }
    wifi_ap_record_t *recs = NULL;
    uint16_t n = 0;
    int count = -1;
    if (fetch) {
        /* Same exception as the reconnect: driver calls from the event-loop task (internal stack). */
        esp_wifi_scan_get_ap_num(&n);
        if (n > SCAN_MAX) {
            n = SCAN_MAX;   /* get_ap_records with this count frees the rest of the list */
        }
        if (n == 0) {
            esp_wifi_clear_ap_list();
            count = 0;
        } else if ((recs = malloc(n * sizeof(wifi_ap_record_t))) == NULL) {
            esp_wifi_clear_ap_list();
        } else if (esp_wifi_scan_get_ap_records(&n, recs) == ESP_OK) {
            count = n > SCAN_MAX ? SCAN_MAX : (int) n;
        }
    }
    portENTER_CRITICAL(&s_scan_lock);
    for (int i = 0; i < count; i++) {
        bm_wifi_ap_t *ap = &s_scan_buf[i];
        strlcpy(ap->ssid, (const char *) recs[i].ssid, sizeof ap->ssid);
        memcpy(ap->bssid, recs[i].bssid, sizeof ap->bssid);
        ap->rssi = recs[i].rssi;
        ap->channel = recs[i].primary;
        strlcpy(ap->auth, auth_str(recs[i].authmode), sizeof ap->auth);
    }
    s_scan_count = count;
    s_scan_running = false;   /* before SCAN_DONE: a scan_start() made in reaction to it may begin */
    portEXIT_CRITICAL(&s_scan_lock);
    free(recs);
    bm_wifi_hal_event_t ev = { .type = BM_WIFI_EV_SCAN_DONE };
    emit(&ev);
    xEventGroupSetBits(s_events, SCAN_DONE);
}

/* WIFI_EVENT_AP_START (driver), or HAL_EV_AP_RECONFIGURED (ap_start() on an access point already up). */
static void on_ap_start(bool reconfigured)
{
    portENTER_CRITICAL(&s_ip_lock);
    bool wanted = __atomic_load_n(&s_ap_on, __ATOMIC_ACQUIRE);
    bool deliver = wanted && (reconfigured ? s_ap_reported : !s_ap_reported);
    bool restart = wanted && !reconfigured && s_ap_reported;   /* the second half of a mode change */
    if (!reconfigured && deliver) {
        s_ap_reported = true;
        s_ap_clients = 0;
        s_ap_stop_clients = -1;
    }
    portEXIT_CRITICAL(&s_ip_lock);
    if (restart) {
        /* Same exception as the reconnect: a driver call from the loop task. Stations the restart dropped
         * without an AP_STADISCONNECTED would otherwise stay counted. */
        wifi_sta_list_t list;
        if (esp_wifi_ap_get_sta_list(&list) == ESP_OK) {
            portENTER_CRITICAL(&s_ip_lock);
            s_ap_clients = list.num;
            portEXIT_CRITICAL(&s_ip_lock);
        }
    }
    if (deliver) {
        emit_ap(BM_WIFI_EV_AP_STARTED, NULL, 0);
    }
}

static void on_ap_stop(void)
{
    portENTER_CRITICAL(&s_ip_lock);
    /* Still wanted: the first half of a mode change, the access point stays up. */
    bool deliver = !__atomic_load_n(&s_ap_on, __ATOMIC_ACQUIRE) && s_ap_reported;
    int clients = s_ap_stop_clients >= 0 ? s_ap_stop_clients : s_ap_clients;
    if (deliver) {
        s_ap_reported = false;
        s_ap_stop_clients = -1;
        s_ap_clients = 0;
    }
    portEXIT_CRITICAL(&s_ip_lock);
    if (deliver) {
        emit_ap(BM_WIFI_EV_AP_STOPPED, NULL, clients);
    }
}

static void on_ap_client(bool joined, const uint8_t *mac)
{
    portENTER_CRITICAL(&s_ip_lock);
    if (joined) {
        s_ap_clients++;
    } else if (s_ap_clients > 0) {
        s_ap_clients--;
    }
    int clients = s_ap_clients;
    portEXIT_CRITICAL(&s_ip_lock);
    emit_ap(joined ? BM_WIFI_EV_AP_CLIENT_JOINED : BM_WIFI_EV_AP_CLIENT_LEFT, mac, clients);
}

#if BM_WIFI_HAL_LOG_EVT_STACK
/* Loop task only: logs, once per `*done`, how much of this task's stack was never used. */
static void log_evt_stack(bool *done, const char *after)
{
    if (!*done) {
        *done = true;
        ESP_LOGI(TAG, "event task stack high-water mark after the first %s: %u bytes", after,
                 (unsigned) uxTaskGetStackHighWaterMark(NULL));
    }
}
static bool s_hwm_scan = false;
static bool s_hwm_ip = false;
#endif

/* Runs on the default event loop task (internal stack): never calls into PHP. Every listener call is made
 * from here, after the branch has left its critical section. */
static void wifi_evt(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void) arg;
    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_SCAN_DONE: {
                const wifi_event_sta_scan_done_t *s = data;
                scan_finish(s != NULL && s->status == 0, NULL);
#if BM_WIFI_HAL_LOG_EVT_STACK
                log_evt_stack(&s_hwm_scan, "SCAN_DONE");
#endif
                break;
            }
            case WIFI_EVENT_STA_CONNECTED:
                on_sta_connected();
                break;
            case WIFI_EVENT_STA_DISCONNECTED:
                on_sta_disconnected(data);
                break;
            case WIFI_EVENT_AP_START:
                on_ap_start(false);
                break;
            case WIFI_EVENT_AP_STOP:
                on_ap_stop();
                break;
            case WIFI_EVENT_AP_STACONNECTED: {
                const wifi_event_ap_staconnected_t *c = data;
                on_ap_client(true, c ? c->mac : NULL);
                break;
            }
            case WIFI_EVENT_AP_STADISCONNECTED: {
                const wifi_event_ap_stadisconnected_t *c = data;
                on_ap_client(false, c ? c->mac : NULL);
                break;
            }
            default:
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        on_sta_got_ip(data);
#if BM_WIFI_HAL_LOG_EVT_STACK
        log_evt_stack(&s_hwm_ip, "STA_GOT_IP");
#endif
    } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
        on_sta_lost_ip();
    } else if (base == BM_HAL_BASE && id == HAL_EV_FENCE && data != NULL) {
        on_fence(data);
    } else if (base == BM_HAL_BASE && id == HAL_EV_AP_RECONFIGURED) {
        on_ap_start(true);
    } else if (base == BM_HAL_BASE && id == HAL_EV_SCAN_ABORTED && data != NULL) {
        scan_finish(false, data);
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
    /* Keep the SSID and password in RAM: by default the driver also writes them, in clear, to NVS. */
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (err != ESP_OK) {
        return err;
    }
    if (s_events == NULL) {
        s_events = xEventGroupCreate();
    }
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    /* WIFI_EVENT (any id) brings SCAN_DONE, STA_CONNECTED/DISCONNECTED, AP_START/STOP and
     * AP_STACONNECTED/STADISCONNECTED; the rest of the ids are ignored. */
    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL);
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_evt, NULL, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, wifi_evt, NULL, NULL);
    }
    if (err == ESP_OK) {
        err = esp_event_handler_instance_register(BM_HAL_BASE, ESP_EVENT_ANY_ID, wifi_evt, NULL, NULL);
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
    bool ap = __atomic_load_n(&s_ap_on, __ATOMIC_ACQUIRE);
    if (s_sta_on && ap) return WIFI_MODE_APSTA;
    if (s_sta_on)       return WIFI_MODE_STA;
    if (ap)             return WIFI_MODE_AP;
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

/* ---- scan ---- */

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
    wifi_op_t *op = arg;
    esp_wifi_scan_stop();
    esp_wifi_clear_ap_list();
    /* SCAN_DONE (failed) from the loop task, unless the driver's own SCAN_DONE ends the scan first; the
     * generation keeps a late abort from ending the next scan. */
    return esp_event_post(BM_HAL_BASE, HAL_EV_SCAN_ABORTED, &op->gen, sizeof op->gen, pdMS_TO_TICKS(POST_MS));
}

static bool scan_begin(uint32_t *gen)
{
    if (bm_wifi_hal_init() != 0) {
        return false;
    }
    portENTER_CRITICAL(&s_scan_lock);
    bool busy = s_scan_running;
    if (!busy) {
        s_scan_running = true;
        *gen = ++s_scan_gen;
    }
    portEXIT_CRITICAL(&s_scan_lock);
    if (busy) {
        return false;
    }
    if (run(do_scan_start, NULL) != ESP_OK) {
        portENTER_CRITICAL(&s_scan_lock);
        s_scan_running = false;
        portEXIT_CRITICAL(&s_scan_lock);
        return false;
    }
    return true;
}

bool bm_wifi_hal_scan_start(void)
{
    uint32_t gen;
    return scan_begin(&gen);
}

int bm_wifi_hal_scan_results(bm_wifi_ap_t *out, int max)
{
    portENTER_CRITICAL(&s_scan_lock);
    int n = s_scan_count;
    if (n > 0 && n > max) {
        n = max < 0 ? 0 : max;
    }
    if (n > 0) {
        memcpy(out, s_scan_buf, (size_t) n * sizeof *out);
    }
    portEXIT_CRITICAL(&s_scan_lock);
    return n;
}

int bm_wifi_hal_scan(bm_wifi_ap_t *out, int max)
{
    uint32_t gen;
    if (!scan_begin(&gen)) {
        return -1;
    }
    EventBits_t bits = xEventGroupWaitBits(s_events, SCAN_DONE, pdTRUE, pdFALSE,
                                           pdMS_TO_TICKS(SCAN_TIMEOUT_MS));
    if ((bits & SCAN_DONE) == 0) {
        wifi_op_t op = { .gen = gen };
        if (run(do_scan_abort, &op) == ESP_OK) {
            xEventGroupWaitBits(s_events, SCAN_DONE, pdTRUE, pdFALSE, pdMS_TO_TICKS(WIFI_ENQUEUE_MS));
        }
        return -1;
    }
    return bm_wifi_hal_scan_results(out, max);
}

/* ---- station ---- */

/* Executor: posts the fence of generation `gen` (see s_gen). Without it nothing would ever end the stale
 * window, so a failed post ends it here (the old attempt's queued events are then not filtered). */
static bool post_fence(uint32_t gen, int op)
{
    hal_fence_t f = { .gen = gen, .op = op };
    if (esp_event_post(BM_HAL_BASE, HAL_EV_FENCE, &f, sizeof f, pdMS_TO_TICKS(POST_MS)) == ESP_OK) {
        return true;
    }
    portENTER_CRITICAL(&s_ip_lock);
    if ((int32_t) (gen - s_fence_gen) > 0) {
        s_fence_gen = gen;
    }
    portEXIT_CRITICAL(&s_ip_lock);
    return false;
}

/* Executor: begins generation `op` (connect, disconnect, timeout) and cancels the attempt in progress, if
 * any, before anything else touches the driver. Returns the new generation. */
static uint32_t sta_cancel(int op)
{
    portENTER_CRITICAL(&s_ip_lock);
    uint32_t gen = ++s_gen;
    s_gen_op = op;
    bool in_progress = s_sta_want;   /* associating, associated, or between retries */
    s_sta_want = false;
    s_sta_retry = 0;
    s_sta_ip[0] = '\0';
    if (op == OP_DISCONNECT) {
        s_sta_ssid[0] = '\0';
    }
    if (in_progress) {
        s_sta_ignore++;   /* its ASSOC_LEAVE may come after the fence; never lowered by an op */
    }
    portEXIT_CRITICAL(&s_ip_lock);
    if (in_progress) {
        esp_wifi_disconnect();
    }
    xEventGroupClearBits(s_events, STA_GOT_IP);
    return gen;
}

static esp_err_t do_sta_connect(void *arg)
{
    wifi_op_t *op = arg;
    wifi_config_t wc = { 0 };
    /* The fields are not NUL-terminated at full length: a 32-byte SSID or a 64-digit PSK fills them. */
    memcpy(wc.sta.ssid, op->ssid, strnlen(op->ssid, sizeof wc.sta.ssid));
    /* Scan every channel and pick the strongest match -- more robust than the default fast scan,
     * which stops at the first hit and can miss WPA2/WPA3-transition APs. */
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    if (op->password && op->password[0]) {
        memcpy(wc.sta.password, op->password, strnlen(op->password, sizeof wc.sta.password));
        wc.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        /* Support WPA2/WPA3-transition APs (common on phone hotspots): PMF-capable and WPA3-SAE
         * with both hunt-and-peck and hash-to-element. Harmless for plain WPA2 networks. */
        wc.sta.pmf_cfg.capable = true;
        wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    } else {
        wc.sta.threshold.authmode = WIFI_AUTH_OPEN;
    }

    /* Cancel first (before set_config), then the fence, then the new attempt: see s_gen. */
    uint32_t gen = sta_cancel(OP_CONNECT);
    op->gen = gen;
    xEventGroupClearBits(s_events, STA_GAVE_UP);
    post_fence(gen, OP_CONNECT);

    esp_err_t err = sta_up();
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    }
    if (err != ESP_OK) {
        return err;
    }
    portENTER_CRITICAL(&s_ip_lock);
    bool current = s_gen == gen;
    if (current) {
        memset(s_sta_ssid, 0, sizeof s_sta_ssid);
        memcpy(s_sta_ssid, op->ssid, strnlen(op->ssid, sizeof s_sta_ssid - 1));
        s_sta_retry_max = op->value;
        s_sta_retry = op->value;   /* auto-retry failed / dropped associations during this connect */
        s_sta_had_ip = false;
        s_sta_refusals = 0;
        s_sta_want = true;
    }
    portEXIT_CRITICAL(&s_ip_lock);
    if (!current) {
        return ESP_FAIL;
    }
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        portENTER_CRITICAL(&s_ip_lock);
        if (s_gen == gen) {
            s_sta_want = false;
            s_sta_retry = 0;
        }
        portEXIT_CRITICAL(&s_ip_lock);
    }
    return err;
}

enum { STOP_USER = 0, STOP_TIMEOUT };
enum { STOPPED = 0, STOP_GAVE_UP_PENDING, STOP_ALREADY_GAVE_UP, STOP_GOT_IP, STOP_NO_EVENT };

/* Executor: sta_disconnect() (STOP_USER) or a wait-true connect that timed out (STOP_TIMEOUT). Stops the
 * background retries so the board cannot connect after reporting failure. For a timeout the fence sends
 * STA_GAVE_UP(OTHER, 0), after the old association's STA_DISCONNECTED. */
static esp_err_t do_sta_stop(void *arg)
{
    wifi_op_t *op = arg;
    bool timeout = op->value == STOP_TIMEOUT;
    if (timeout) {
        portENTER_CRITICAL(&s_ip_lock);
        bool got_ip = s_sta_ip[0] != '\0';
        bool gave_up = !s_sta_want;
        op->gen = s_gen;
        portEXIT_CRITICAL(&s_ip_lock);
        if (got_ip) {
            op->result = STOP_GOT_IP;   /* the IP won the race: the caller waits for its STA_GOT_IP */
            return ESP_OK;
        }
        if (gave_up) {
            op->result = STOP_ALREADY_GAVE_UP;   /* retries ran out first: its STA_GAVE_UP is on the way */
            return ESP_OK;
        }
    }
    uint32_t gen = sta_cancel(timeout ? OP_TIMEOUT : OP_DISCONNECT);
    op->gen = gen;
    bool fenced = post_fence(gen, timeout ? OP_TIMEOUT : OP_DISCONNECT);
    op->result = !fenced ? STOP_NO_EVENT : timeout ? STOP_GAVE_UP_PENDING : STOPPED;
    return ESP_OK;
}

/* Waits up to `ticks` for one of `bits`; a STA_GAVE_UP counts only when it is of generation `gen`. */
static bool wait_sta(EventBits_t bits, uint32_t gen, TickType_t ticks, EventBits_t *got)
{
    TickType_t start = xTaskGetTickCount();
    for (;;) {
        TickType_t spent = xTaskGetTickCount() - start;
        if (spent >= ticks) {
            return false;
        }
        EventBits_t b = xEventGroupWaitBits(s_events, bits, pdFALSE, pdFALSE, ticks - spent) & bits;
        if (b & STA_GOT_IP) {
            *got = STA_GOT_IP;
            return true;
        }
        if (b & STA_GAVE_UP) {
            portENTER_CRITICAL(&s_ip_lock);
            bool mine = s_gave_up_gen == gen;
            portEXIT_CRITICAL(&s_ip_lock);
            if (mine) {
                *got = STA_GAVE_UP;
                return true;
            }
            xEventGroupClearBits(s_events, STA_GAVE_UP);   /* an older attempt's, set after our clear */
            portENTER_CRITICAL(&s_ip_lock);
            mine = s_gave_up_gen == gen;   /* ours may have been stamped (and its bit cleared) meanwhile */
            portEXIT_CRITICAL(&s_ip_lock);
            if (mine) {
                *got = STA_GAVE_UP;
                return true;
            }
            continue;
        }
        if (b == 0) {
            return false;
        }
    }
}

bool bm_wifi_hal_sta_connect_ex(const char *ssid, const char *password, int timeout_ms, bool wait, int retries)
{
    if (bm_wifi_hal_init() != 0) {
        return false;
    }
    wifi_op_t op = { .ssid = ssid, .password = password, .value = retries < 0 ? -1 : retries };
    if (run(do_sta_connect, &op) != ESP_OK) {
        return false;
    }
    if (!wait) {
        return true;
    }
    EventBits_t got = 0;
    if (wait_sta(STA_GOT_IP | STA_GAVE_UP, op.gen, pdMS_TO_TICKS(timeout_ms < 0 ? 0 : timeout_ms), &got)) {
        return got == STA_GOT_IP;
    }
    wifi_op_t stop = { .value = STOP_TIMEOUT };
    if (run(do_sta_stop, &stop) != ESP_OK) {
        return false;
    }
    if (stop.result == STOP_GOT_IP) {
        wait_sta(STA_GOT_IP, stop.gen, pdMS_TO_TICKS(WIFI_ENQUEUE_MS), &got);
        return true;
    }
    if (stop.result != STOP_NO_EVENT) {
        /* The listener hears STA_GAVE_UP before this returns (bounded: the loop task sets the bit after it). */
        wait_sta(STA_GAVE_UP, stop.gen, pdMS_TO_TICKS(WIFI_ENQUEUE_MS), &got);
    }
    return false;
}

bool bm_wifi_hal_sta_connect(const char *ssid, const char *password, int timeout_ms)
{
    return bm_wifi_hal_sta_connect_ex(ssid, password, timeout_ms, true, STA_RETRIES);
}

/* The station's power save last applied (-1: none yet, the driver's default). Executor only. */
static int s_ps = -1;

static esp_err_t do_sta_set_ps(void *arg)
{
    wifi_op_t *op = arg;
    if (op->value == s_ps) {
        return ESP_OK;
    }
    wifi_ps_type_t t = op->value == BM_WIFI_PS_NONE ? WIFI_PS_NONE
                     : op->value == BM_WIFI_PS_MAX  ? WIFI_PS_MAX_MODEM : WIFI_PS_MIN_MODEM;
    esp_err_t err = esp_wifi_set_ps(t);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "power save %d refused: %s", op->value, esp_err_to_name(err));
        return err;
    }
    s_ps = op->value;
    return ESP_OK;
}

bool bm_wifi_hal_sta_set_power_save(bm_wifi_ps_t ps)
{
    if (ps != BM_WIFI_PS_NONE && ps != BM_WIFI_PS_MIN && ps != BM_WIFI_PS_MAX) {
        return false;
    }
    if (bm_wifi_hal_init() != 0) {
        return false;
    }
    wifi_op_t op = { .value = (int) ps };
    return run(do_sta_set_ps, &op) == ESP_OK;
}

void bm_wifi_hal_sta_disconnect(void)
{
    if (s_inited) {
        wifi_op_t op = { .value = STOP_USER };
        run(do_sta_stop, &op);
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

void bm_wifi_hal_sta_ssid(char *out, size_t len)
{
    char ssid[33];
    portENTER_CRITICAL(&s_ip_lock);
    memcpy(ssid, s_sta_ssid, sizeof ssid);
    portEXIT_CRITICAL(&s_ip_lock);
    ssid[sizeof ssid - 1] = '\0';
    strlcpy(out, ssid, len);
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

/* ---- SoftAP ---- */

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
    wc.ap.ssid_len = (uint8_t) strnlen(op->ssid, sizeof wc.ap.ssid);
    memcpy(wc.ap.ssid, op->ssid, wc.ap.ssid_len);
    wc.ap.channel = (uint8_t) op->channel;
    wc.ap.max_connection = (uint8_t) op->max_conn;
    if (op->password && strlen(op->password) >= 8) {
        memcpy(wc.ap.password, op->password, strnlen(op->password, sizeof wc.ap.password));
        wc.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        /* WPA2 needs >= 8 chars; anything shorter (or none) opens the network. */
        wc.ap.authmode = WIFI_AUTH_OPEN;
    }

    /* Wanted from here on; read together with s_ap_reported so the loop's decisions see one order. */
    portENTER_CRITICAL(&s_ip_lock);
    bool was_on = __atomic_load_n(&s_ap_on, __ATOMIC_ACQUIRE);
    __atomic_store_n(&s_ap_on, true, __ATOMIC_RELEASE);
    bool reported = s_ap_reported;
    portEXIT_CRITICAL(&s_ip_lock);
    esp_err_t err = apply_mode();
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &wc);
    }
    if (err != ESP_OK) {
        /* A refused change of a running access point leaves it up with its old settings (no event). */
        __atomic_store_n(&s_ap_on, was_on, __ATOMIC_RELEASE);
        if (!was_on) {
            apply_mode();
        }
        return err;
    }
    if (was_on || reported) {
        /* The script has it as up: no driver AP_START will be delivered for this call, so say it ourselves. */
        if (esp_event_post(BM_HAL_BASE, HAL_EV_AP_RECONFIGURED, NULL, 0, pdMS_TO_TICKS(POST_MS)) != ESP_OK) {
            ESP_LOGW(TAG, "ap started, but its event could not be queued");
        }
    }
    return ESP_OK;
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
    portENTER_CRITICAL(&s_ip_lock);
    if (__atomic_load_n(&s_ap_on, __ATOMIC_ACQUIRE)) {
        /* AP_STOPPED reports the stations connected before the stop, even if the driver drops them first. */
        s_ap_stop_clients = s_ap_clients;
    }
    __atomic_store_n(&s_ap_on, false, __ATOMIC_RELEASE);   /* not wanted: its AP_STOP is a real stop */
    portEXIT_CRITICAL(&s_ip_lock);
    return apply_mode();
}

void bm_wifi_hal_ap_stop(void)
{
    if (s_inited) {
        run(do_ap_stop, NULL);
    }
}

bool bm_wifi_hal_ap_active(void)
{
    return __atomic_load_n(&s_ap_on, __ATOMIC_ACQUIRE);
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
    if (s_ap_netif == NULL || !bm_wifi_hal_ap_active()) {
        return;
    }
    char ip[16] = "";
    wifi_op_t op = { .buf = ip, .len = sizeof ip };
    if (run(do_ap_ip, &op) == ESP_OK) {
        strlcpy(out, ip, len);
    }
}

/* The stations on the SoftAP, as counted from the driver's events (the count the events carry). */
int bm_wifi_hal_ap_clients(void)
{
    if (!s_inited || !bm_wifi_hal_ap_active()) {
        return 0;
    }
    portENTER_CRITICAL(&s_ip_lock);
    int n = s_ap_clients;
    portEXIT_CRITICAL(&s_ip_lock);
    return n;
}
