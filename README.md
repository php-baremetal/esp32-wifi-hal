# esp32-wifi-hal

The ESP32 side of [ext-wifi](package:php-baremetal/ext-wifi): the whole Wi-Fi lifecycle (radio init, scan,
station, access point, connection state) over `esp_wifi`, `esp_netif` and `esp_event`. It implements
`bm_wifi_hal.h` from ext-wifi, and runs every driver call on the executor task of
[esp32-executor](package:php-baremetal/esp32-executor).

| Key | Value |
| --- | --- |
| Type | `bm-hal` |
| Platform | `esp32` (`extra.baremetal.platform`) |
| Provides | `php-baremetal/wifi-hal` |
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
PHP task, on an event group. One deliberate exception: the automatic reconnect after a dropped association
calls `esp_wifi_connect()` from the default event loop task, which also has an internal-RAM stack.

**Init.** The first call runs `nvs_flash_init()` (erasing and re-initialising the `nvs` partition when it
reports no free pages or a newer format), then `esp_wifi_init()` with `WIFI_INIT_CONFIG_DEFAULT()`, and
registers handlers for `WIFI_EVENT` and `IP_EVENT_STA_GOT_IP`. lwIP and the default event loop are already
started by the SDK host. The station and access point interfaces are created on first use and kept.

**Mode.** The mode follows the roles in use: station, access point, both (`APSTA`), or none, in which case the
radio is stopped.

**Scan.** All channels, the default active scan; 15 s timeout, after which the scan is aborted and its list
cleared. Results are capped at the caller's maximum (48 from ext-wifi), and the driver's list is always freed.

**Connect.** `WIFI_ALL_CHANNEL_SCAN` with `WIFI_CONNECT_AP_BY_SIGNAL` (the strongest access point with the
SSID). With a password: threshold `WIFI_AUTH_WPA2_PSK`, PMF capable, WPA3 SAE with both hunt-and-peck and
hash-to-element (`WPA3_SAE_PWE_BOTH`); without: open. A current association is dropped first, and its
disconnect event is skipped rather than counted. Each connect allows 10 automatic retries of a dropped
association (`sta disconnected, reason=.. -- retrying` on the console). On timeout the attempt is stopped so
the board cannot connect after reporting failure.

**Access point.** SSID and password as given; WPA2-PSK when the password has 8 or more characters, otherwise
open. Channel and maximum stations are passed to the driver unchecked.

**State.** The station IP is kept under a spinlock, updated by the got-IP and disconnected events, so
`wifi_connected()` and `wifi_ip()` never call the driver. `wifi_rssi()` asks the driver only while connected.

## License

MIT.
