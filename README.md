# esp32-wifi-hal

The ESP32 side of [ext-wifi](package:php-baremetal/ext-wifi): the whole Wi-Fi lifecycle (radio init, scan,
station, access point, connection state) over `esp_wifi`, `esp_netif` and `esp_event`. It implements
`bm_wifi_hal.h` from ext-wifi (contract 1.1), and runs every driver call made for PHP on the executor task
of [esp32-executor](package:php-baremetal/esp32-executor).

| Key | Value |
| --- | --- |
| Type | `bm-hal` |
| Platform | `esp32` (`extra.baremetal.platform`) |
| Provides | `php-baremetal/wifi-hal` 1.1.0 |
| Requires | `php-baremetal/esp32-executor` |
| ESP-IDF components | `esp_wifi`, `esp_netif`, `esp_event`, `nvs_flash` |

## Install

Projects do not require it. A project requires `php-baremetal/ext-wifi`, and conductor adds
`php-baremetal/esp32-wifi-hal` (and with it `esp32-executor`) because the project's SDK
(`"sdk": "esp32@^2.0"`) has the platform `esp32`:

```bash
composer require php-baremetal/ext-wifi
```

The SDK accepts ext-wifi only on a board with a radio: the ESP32-S3 boards, and the Waveshare ESP32-P4-WIFI6,
where the same `esp_wifi` calls reach an ESP32-C6 companion over ESP-Hosted (build-tested only).

## Behaviour on ESP32

**The executor rule.** The PHP task's stack is in PSRAM, and the Wi-Fi driver may touch the flash (NVS, PHY
calibration). So every `esp_wifi_*`, `esp_netif_*` and `nvs_flash_*` call is wrapped in
`executor_run_sync()` and runs on the executor task (core 0, internal-RAM stack); the PHP task waits. If the
executor queue stays full for 1000 ms the call fails. Waiting for a scan or for the IP address happens on the
PHP task, on an event group. Deliberate exceptions, all on the default event loop task (which also has an
internal-RAM stack): the automatic reconnect after a dropped association calls `esp_wifi_connect()`, and the
end of a scan reads the driver's list there.

**Init.** The first call runs `nvs_flash_init()` (erasing and re-initialising the `nvs` partition when it
reports no free pages or a newer format), then `esp_wifi_init()` with `WIFI_INIT_CONFIG_DEFAULT()`, and
registers handlers for `WIFI_EVENT`, `IP_EVENT_STA_GOT_IP` and `IP_EVENT_STA_LOST_IP`. lwIP and the default
event loop are already started by the SDK host. The station and access point interfaces are created on first
use and kept.

**Mode.** The mode follows the roles in use: station, access point, both (`APSTA`), or none, in which case the
radio is stopped.

**Scan.** All channels, the default active scan. `bm_wifi_hal_scan_start()` starts it and returns (false
when a scan is already running or the driver refuses, for example while the station is joining). When the
driver reports the end, the list (up to 48 access points) is copied to a buffer, the driver's list is freed,
and `SCAN_DONE` goes to the listener; `bm_wifi_hal_scan_results()` copies from that buffer, -1 after a failed
scan. The blocking `bm_wifi_hal_scan()` is the same plus a wait of up to 15 s, after which the scan is
aborted (still one `SCAN_DONE`) and the call returns -1.

**Connect.** `WIFI_ALL_CHANNEL_SCAN` with `WIFI_CONNECT_AP_BY_SIGNAL` (the strongest access point with the
SSID). With a password: threshold `WIFI_AUTH_WPA2_PSK`, PMF capable, WPA3 SAE with both hunt-and-peck and
hash-to-element (`WPA3_SAE_PWE_BOTH`); without: open. An attempt in progress (associating, associated, or
between retries) is cancelled first, before the new configuration is set. Each connect, disconnect and timeout
starts a new attempt generation and posts a marker to the event loop. Station events the old attempt raised
before the cancel are queued ahead of the marker, so they are recognised and dropped: they never count as a
retry or a give-up of the new attempt. An old association the listener had heard ends with exactly one
`STA_DISCONNECTED` (`BY_USER`). `bm_wifi_hal_sta_connect_ex()` takes the number of automatic retries of a failed or
dropped association (`sta disconnected, reason=.. -- retrying` on the console): `-1` retries forever, and the
count starts again at every address. Two password refusals in a row before the first address give up at once. `bm_wifi_hal_sta_connect()` is the waiting form with 10 retries. With
`wait` false the call returns once the attempt has started. With `wait` true and no address within the
timeout the attempt is stopped, so the board cannot connect after reporting failure, and `STA_GAVE_UP`
(reason `OTHER`, code 0) is delivered before the call returns false. A waiting connect that succeeds returns
true after the listener has heard `STA_GOT_IP`.

**Access point.** SSID and password as given; WPA2-PSK when the password has 8 or more characters, otherwise
open. Channel and maximum stations are passed to the driver unchecked.

**State.** The station IP and the SSID asked for are kept under a spinlock, updated by the events, so
`wifi_connected()`, `wifi_ip()` and `wifi_ssid()` never call the driver. The SSID stays after a give-up,
until the next connect or disconnect. `wifi_rssi()` asks the driver only while connected. The access point's
station count comes from its join and leave events.

## Events

`bm_wifi_hal_set_listener()` installs one listener. Every call to it is made from the ESP-IDF default event
loop task, one event at a time, in the order the events happen, never from an interrupt and never with a lock
held. What the PHP side causes (a waiting connect that timed out, an aborted scan) is posted to the same loop,
so it is delivered from that task too.

The listener and the end of a scan run on that task's stack (`CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE`,
2304 bytes in the SDK). Measured on the board: 1416 to 1592 bytes still free after the first scan and the first
IP address. To measure again, build with `BM_WIFI_HAL_LOG_EVT_STACK` defined to 1 (it is 0 by default): the HAL
then logs the high-water mark once after the first `SCAN_DONE` and once after the first `STA_GOT_IP`
(`event task stack high-water mark after the first ...: N bytes`).

| Event | When | Fields |
| --- | --- | --- |
| `STA_CONNECTED` | `WIFI_EVENT_STA_CONNECTED` for the current attempt | none |
| `STA_DISCONNECTED` | `WIFI_EVENT_STA_DISCONNECTED`, only if `STA_CONNECTED` was delivered | `reason`, `code`, `retrying` |
| `STA_GOT_IP` | `IP_EVENT_STA_GOT_IP` for the current attempt | `ip` |
| `STA_LOST_IP` | `IP_EVENT_STA_LOST_IP` while associated | none |
| `STA_GAVE_UP` | no retry left, a retry that could not start, or a waiting connect that timed out | `reason`, `code` |
| `SCAN_DONE` | once per started scan, success or not | none |
| `AP_STARTED` | `WIFI_EVENT_AP_START` after `bm_wifi_hal_ap_start()`, or that call on an access point already up | none |
| `AP_STOPPED` | `WIFI_EVENT_AP_STOP` after `bm_wifi_hal_ap_stop()` | `clients` before the stop |
| `AP_CLIENT_JOINED` / `AP_CLIENT_LEFT` | `WIFI_EVENT_AP_STACONNECTED` / `WIFI_EVENT_AP_STADISCONNECTED` | `mac`, `clients` after the change |

With no retry left, a station that was associated gets `STA_DISCONNECTED` (`retrying` false), then
`STA_GAVE_UP` with the same reason. Two password refusals in a row (only counted before the connect's first
address) give up the same way at once with `WRONG_PASSWORD`, whatever the number of retries, `-1` included.
A single refusal is retried like any other reason, because a weak link or a busy access point can raise one
for a correct password (with no retry left it gives up with `WRONG_PASSWORD`); any other reason starts the
count again. Every other reason, `NOT_FOUND` included, uses the retries. `bm_wifi_hal_sta_disconnect()`, and a connect that cancels one in
progress, give `STA_DISCONNECTED` with reason `BY_USER` only when the station was associated, and never
`STA_GAVE_UP`.

Reasons (`code` is the raw ESP-IDF `wifi_err_reason_t`):

| Reason | ESP-IDF codes |
| --- | --- |
| `WRONG_PASSWORD` | `WIFI_REASON_AUTH_FAIL` (202), `WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT` (15), `WIFI_REASON_HANDSHAKE_TIMEOUT` (204), while the connect has not had an address yet |
| `NOT_FOUND` | before the association: `WIFI_REASON_NO_AP_FOUND` (201), `WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY` (210), `WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD` (211), `WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD` (212) |
| `LOST` | once `STA_CONNECTED` was delivered: every code not asked for that is not a refusal counted as `WRONG_PASSWORD`, `WIFI_REASON_AUTH_LEAVE` (3) of a switched-off access point included; before the association `WIFI_REASON_BEACON_TIMEOUT` (200), and after the connect's first address also `WIFI_REASON_CONNECTION_FAIL` (205), `WIFI_REASON_AUTH_FAIL` (202), `WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT` (15), `WIFI_REASON_HANDSHAKE_TIMEOUT` (204) and `WIFI_REASON_AUTH_EXPIRE` (2) |
| `BY_USER` | a disconnect asked for: `bm_wifi_hal_sta_disconnect()` or a new connect |
| `OTHER` | every other code before the association (`WIFI_REASON_AUTH_EXPIRE` (2) included); a waiting connect that timed out (code 0) |

## License

MIT.
