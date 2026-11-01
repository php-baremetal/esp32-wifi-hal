# Changelog

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
