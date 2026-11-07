# Changelog

## 1.1.0

- Added: wifi-hal contract 1.1 (`"provide": {"php-baremetal/wifi-hal": "1.1.0"}`). One listener hears the
  station, scan and access point events (`bm_wifi_hal_set_listener()`), always from the default event loop
  task, one at a time and in order.
- Added: `bm_wifi_hal_sta_connect_ex()`: a connect that waits for the address or returns at once, with its
  own number of automatic retries (`-1` retries forever). The outcome comes as an address or a give-up; a
  waiting connect that times out gives up with reason `OTHER`.
- Added: disconnect reasons mapped from the ESP-IDF codes (wrong password, network not found, connection
  lost, asked for, other), with the raw code kept. A password refusal (202, 15, 204; not `AUTH_EXPIRE`, 2)
  counts as wrong only until the connect has had an address; after that the same codes mean the connection
  was lost. Two refusals in a row give up at once with `WRONG_PASSWORD`, whatever the number of retries
  (`-1` included); a single refusal is retried like any other reason, and any other reason starts the count
  again. Once the station was associated, every end not asked for that is not a counted refusal is `LOST`
  (a hotspot switched off deauthenticates with code 3).
- Added: an optional log line (`ESP_LOGI`) with the default event loop task's stack high-water mark after the
  first `SCAN_DONE` and the first `STA_GOT_IP`, on when the build defines `BM_WIFI_HAL_LOG_EVT_STACK` to 1
  (off by default). Measured on the board: 1416 to 1592 bytes free of the 2304-byte stack.
- Added: the access point events follow the script's calls. Switching the station on while the access point
  is up (AP to APSTA) makes ESP-IDF stop and restart the access point internally; that raises no
  `AP_STOPPED`/`AP_STARTED` and keeps the client count. `AP_STOPPED` comes only after
  `bm_wifi_hal_ap_stop()`, and every `bm_wifi_hal_ap_start()` gives one `AP_STARTED`, also when it replaces
  the settings of an access point already up. Settings refused for an access point already up leave it running
  with the old ones (it used to be stopped).
- Added: `bm_wifi_hal_scan_start()` and `bm_wifi_hal_scan_results()`, a scan that does not block (up to 48
  results kept); `bm_wifi_hal_scan()` now runs on top of them.
- Added: `bm_wifi_hal_sta_ssid()` and `bm_wifi_hal_ap_active()`.
- Changed: `bm_wifi_hal_ap_clients()` counts the stations from the driver's join and leave events, so it
  matches the counts the events carry.
- Changed: losing the address while associated (`IP_EVENT_STA_LOST_IP`) now clears it, so
  `bm_wifi_hal_sta_connected()` returns false until a new address arrives.

## 1.0.2

- Fixed: a 32-byte SSID (and a 64-digit PSK) was cut by one byte, so the network could not be joined or the
  access point had the wrong name.
- Fixed: the automatic reconnects after a drop used what the first connect had left of its 10 retries. The
  count now starts again at every address the station gets, and stops for good after a disconnect or a
  connect timeout (an address that arrived late no longer turns the retries back on).
- Fixed: the driver wrote SSIDs and passwords, in clear, to the `nvs` partition. It now keeps them in RAM
  (`WIFI_STORAGE_RAM`). Entries written by earlier versions stay until the `nvs` partition is erased.

## 1.0.1

- Changed: the author's contact email.
- Docs: projects use `"sdk": "esp32@^2.0"`.

## 1.0.0

Initial release.
