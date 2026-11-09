# Sendspin BT Bridge (ESP32-S31)

Firmware port of [trudenboy/sendspin-bt-bridge](https://github.com/trudenboy/sendspin-bt-bridge)
to the **ESP32-S31**: the board appears in Music Assistant as a Sendspin player and plays to one
Bluetooth A2DP speaker. It re-implements the original's behaviour on ESP-IDF; it is not a
line-by-line translation. `README.md` is the user documentation and feature mapping.

This repository is **public**. Never put private information in tracked files: personal paths,
LAN IP addresses, hostnames, credentials or personal details. Personal, per-developer
instructions belong in a git-ignored `CLAUDE.local.md`.

## Key commands

- `pio run`: build. `pio run -t upload`: flash over USB. `pio device monitor`: serial log at 115200.
- `uv run --python 3.11 --with ziglang python tests/run_bt_encoder_host.py`: host encoder tests.
- `uv run --python 3.11 --with ziglang python tests/run_transport_diag_host.py`: diagnostics counter tests.
- `python -m unittest discover -s tests -p test_ldac_overlay.py`: Bluedroid overlay checks.

Host tests do not exercise the radio. Codec, reconnect and sync changes need a hardware check.

## Quick reference

- **Target:** ESP32-S31, not S3 (the S3 has no Bluetooth Classic, so no A2DP). PSRAM is required
  for Sendspin. Default board: ESP32-S31-Function-CoreBoard-1 (16 MB flash, 16 MB octal PSRAM).
- **Toolchain:** only the community pioarduino platform supports the S31. It is pinned in
  `platformio.ini` (`61.04.00-RC1`, ESP-IDF 6.1.0) and needs pioarduino core 6.2.0.
  `framework = espidf`; there is no Arduino.
- **Pinned dependencies:** exact versions in `src/idf_component.yml`, and `dependencies.lock` is
  committed. Change versions deliberately and rebuild.
- **Sendspin:** sendspin-cpp 0.9.5 with the player, controller and metadata roles. Its API is
  main-loop-only except `notify_audio_played` and `send_command`.
- **Bluetooth:** Bluedroid A2DP source to one speaker. Codec preference is LDAC, then AAC-LC, then
  SBC, through ESP-IDF's external-codec path (`src/bt_encoder.*`). All Bluedroid callbacks are
  copied onto a queue and handled on the `bt_app` task. Public `BtManager` methods are async.
- **LDAC transport overlay:** `cmake/ldac_transport.cmake` runs `scripts/prepare_ldac_transport.py`
  to generate patched copies of five Bluedroid sources in the build directory. Never edit the
  shared ESP-IDF install. Anchors fail closed, so an ESP-IDF or platform upgrade starts here.
- **PCM format:** chosen once at boot (44.1/48 kHz, 16/24-bit), advertised identically to Sendspin
  and Bluetooth. There is no resampler on the device.
- **Radio:** Wi-Fi and Bluetooth share one 2.4 GHz radio. Wi-Fi modem sleep must stay on
  (ESP-IDF requires it while Bluetooth is enabled). Coexistence limits the top LDAC bitrate.
- **Behaviour kept from the original:**
  - reconnect backoff `min(interval·2^max(0,attempt−3), 300 s)`
  - the four idle modes
  - 125 ms starting static delay, replaced once by the speaker's AVDTP delay report
  - "require pairing" off by default
- **Storage:** settings in NVS namespace `bridge` (`src/app_config.*`). The sendspin-cpp identity
  and pairings are in namespace `sendspin`.
- **Flash budget:** two 3 MB OTA slots (`partitions.csv`); the image is about 81% of one slot.
- **Licences:** the project is MIT and keeps the original's notice. `components/libldac` is
  Apache 2.0 from AOSP; keep its LICENSE and NOTICE. `espressif/esp_audio_codec` may only be
  used with Espressif chips.

## Conventions

- C++ on ESP-IDF with `-Wall -Wextra`. Keep the build free of warnings.
- Credit upstream work. When code is adapted from another project, link it in the README credits
  and keep its licence.
